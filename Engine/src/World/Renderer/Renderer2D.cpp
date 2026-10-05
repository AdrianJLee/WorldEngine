#include "wldpch.h"
#include "World/Renderer/Renderer2D.h"

#include "World/Math/Math.h"
#include "World/Renderer/Renderer.h"
#include "World/Renderer/RenderSettings.h"
#include "World/Renderer/ShaderUtils.h"

#include <glm/gtc/matrix_transform.hpp>

#include <array>
#include <filesystem>
#include <string>
#include <utility>

namespace World
{
	Rhi::Handle<Rhi::CommandBuffer> Renderer2D::s_CurrentCommandBuffer = nullptr;

	namespace
	{
		constexpr uint32_t MaxQuads = 20000;
		constexpr uint32_t MaxCircles = 20000;
		constexpr uint32_t MaxLines = 10000;
		constexpr uint32_t MaxTextureSlots = 32;

		struct QuadVertex
		{
			glm::vec3 Position;
			glm::vec4 Color;
			glm::vec2 TexCoord;
			float TexIndex = 0.0f;
			float TilingFactor = 1.0f;
			int EntityID = -1;
		};

		struct CircleVertex
		{
			glm::vec3 WorldPosition;
			glm::vec3 LocalPosition;
			glm::vec4 Color;
			float Thickness = 1.0f;
			float Fade = 0.005f;
			int EntityID = -1;
		};

		struct LineVertex
		{
			glm::vec3 Position;
			glm::vec4 Color;
			int EntityID = -1;
		};

		// Renderer2D 管线的渲染通道(与 SceneRenderer 目标结构相同)。
		Rhi::Handle<Rhi::RenderPass> s_RenderPass;

		Rhi::Handle<Rhi::Shader> CreateRendererShader(const char* hlsl, const char* debugName)
		{
			Rhi::ShaderDesc desc;
			desc.DebugName = debugName;
			desc.Stages.push_back(ShaderCompiler::CompileStage(Rhi::ShaderStage::Vertex, hlsl, "VSMain", "vs_6_0"));
			desc.Stages.push_back(ShaderCompiler::CompileStage(Rhi::ShaderStage::Fragment, hlsl, "PSMain", "ps_6_0"));
			return Renderer::GetDevice()->CreateShader(desc);
		}

		// HOTR-P1-T3:热重载用的非断言版本(失败 = 空句柄 + 可读原因;空 stage 不进 CreateShader)。
		Rhi::Handle<Rhi::Shader> TryCreateRendererShader(const char* hlsl, const char* debugName,
			std::string* error)
		{
			Rhi::ShaderDesc desc;
			desc.DebugName = debugName;
			Rhi::ShaderStageSource vertexStage;
			Rhi::ShaderStageSource pixelStage;
			if (!ShaderCompiler::TryCompileStage(Rhi::ShaderStage::Vertex, hlsl, "VSMain", "vs_6_0",
				vertexStage, error))
				return nullptr;
			if (!ShaderCompiler::TryCompileStage(Rhi::ShaderStage::Fragment, hlsl, "PSMain", "ps_6_0",
				pixelStage, error))
				return nullptr;
			desc.Stages.push_back(std::move(vertexStage));
			desc.Stages.push_back(std::move(pixelStage));
			Rhi::Handle<Rhi::Shader> shader = Renderer::GetDevice()->CreateShader(desc);
			if (!shader && error)
				*error = std::string("CreateShader failed for ") + hlsl;
			return shader;
		}

		Rhi::PipelineDesc MakePipeline(const Rhi::Handle<Rhi::Shader>& shader,
			const std::vector<Rhi::VertexAttribute>& attributes, uint32_t stride,
			Rhi::PrimitiveTopology topology,
			const Rhi::Handle<Rhi::DescriptorSetLayout>& textureLayout,
			float lineWidth = 1.0f)
		{
			Rhi::PipelineDesc desc;
			desc.Shader = shader;
			desc.DescriptorSetLayouts = { Renderer::GetGlobalDescriptorSetLayout(), textureLayout };
			desc.RenderPass = s_RenderPass;
			desc.Topology = topology;
			desc.LineWidth = lineWidth;
			// P4-4b:管线采样数跟随 rendering.msaa —— 必须与该管线实际绘制的渲染通道
			// (SceneRenderer 的场景通道)逐项一致,否则 Vulkan 的
			// VUID-VkGraphicsPipelineCreateInfo-subpass-00757(rasterizationSamples 必须
			// 等于子通道采样数)会在建管线/绘制时失败。GL 后端不看这个字段(由 FBO 驱动)。
			desc.Samples = static_cast<Rhi::SampleCount>(RenderSettings::Msaa());
			desc.VertexBindings.push_back({ 0, stride, false });
			desc.VertexAttributes = attributes;
			// 2D 批次不做背面剔除:quad/circle 都是单面精灵,负缩放(镜像精灵)会翻转绕序,
			// 而且 Vulkan 的屏幕绕序与 GL 相反 —— 用默认的 Front=CCW + Cull=Back 会让
			// **整块 2D 内容在 Vulkan 下被剔除**(2026-09-16 W10-3 双后端基线在 2DTest 发现:
			// GL 三个实体、Vulkan 一个都没有)。
			desc.Cull = Rhi::CullMode::None;
			desc.Blends.push_back({
				true,
				Rhi::BlendFactor::SrcAlpha, Rhi::BlendFactor::OneMinusSrcAlpha, Rhi::BlendOp::Add,
				Rhi::BlendFactor::SrcAlpha, Rhi::BlendFactor::OneMinusSrcAlpha, Rhi::BlendOp::Add,
				0xF });
			desc.DebugName = shader->GetDesc().DebugName;
			return desc;
		}

		// HOTR-P1-T3:批次顶点布局/拓扑/线宽的**唯一事实源** —— Init(建管线 + 建缓冲)与
		// ReloadShaders(只重建管线)共用,保证重建出的管线与启动期逐项一致。
		// 着色器路径同样只有这一处(Init / Reload 用同名源)。
		constexpr const char* kQuadShaderPath = "assets/shaders/Renderer2D_Quad.slang";
		constexpr const char* kCircleShaderPath = "assets/shaders/Renderer2D_Circle.slang";
		constexpr const char* kLineShaderPath = "assets/shaders/Renderer2D_Line.slang";

		struct BatchSpec
		{
			std::vector<Rhi::VertexAttribute> Attributes;
			Rhi::PrimitiveTopology Topology = Rhi::PrimitiveTopology::TriangleList;
			float LineWidth = 1.0f;
		};

		const BatchSpec& QuadBatchSpec()
		{
			static const BatchSpec spec {
				{
					{ 0, 0, Rhi::Format::R32G32B32_SFLOAT, 0 },
					{ 1, 0, Rhi::Format::R32G32B32A32_SFLOAT, 12 },
					{ 2, 0, Rhi::Format::R32G32_SFLOAT, 28 },
					{ 3, 0, Rhi::Format::R32_SFLOAT, 36 },
					{ 4, 0, Rhi::Format::R32_SFLOAT, 40 },
					{ 5, 0, Rhi::Format::R32_SINT, 44 },
				}, Rhi::PrimitiveTopology::TriangleList, 1.0f };
			return spec;
		}

		const BatchSpec& CircleBatchSpec()
		{
			static const BatchSpec spec {
				{
					{ 0, 0, Rhi::Format::R32G32B32_SFLOAT, 0 },
					{ 1, 0, Rhi::Format::R32G32B32_SFLOAT, 12 },
					{ 2, 0, Rhi::Format::R32G32B32A32_SFLOAT, 24 },
					{ 3, 0, Rhi::Format::R32_SFLOAT, 40 },
					{ 4, 0, Rhi::Format::R32_SFLOAT, 44 },
					{ 5, 0, Rhi::Format::R32_SINT, 48 },
				}, Rhi::PrimitiveTopology::TriangleList, 1.0f };
			return spec;
		}

		const BatchSpec& LineBatchSpec()
		{
			static const BatchSpec spec {
				{
					{ 0, 0, Rhi::Format::R32G32B32_SFLOAT, 0 },
					{ 1, 0, Rhi::Format::R32G32B32A32_SFLOAT, 12 },
					{ 2, 0, Rhi::Format::R32_SINT, 28 },
				}, Rhi::PrimitiveTopology::LineList, 2.0f };
			return spec;
		}

		template <typename Vertex>
		struct Batch
		{
			Rhi::Handle<Rhi::Pipeline> Pipeline;
			Rhi::Handle<Rhi::Buffer> VertexBuffer;
			Rhi::Handle<Rhi::Buffer> IndexBuffer;
			Vertex* Base = nullptr;
			Vertex* Ptr = nullptr;
			uint32_t IndexCount = 0;
		};

		struct Renderer2DData
		{
			Batch<QuadVertex> Quads;
			Batch<CircleVertex> Circles;
			Batch<LineVertex> Lines;

			Rhi::Handle<Rhi::Sampler> Sampler;
			Rhi::Handle<Rhi::DescriptorSet> TextureDescriptorSet;
			Rhi::Handle<Rhi::DescriptorSetLayout> TextureLayout;
			std::array<Rhi::Handle<Rhi::Texture>, MaxTextureSlots> Textures;
			// 已绑定的 GPU 纹理句柄(用于去重:同一张贴图在本帧只占一个槽位)。
			std::array<Rhi::Handle<Rhi::Texture>, MaxTextureSlots> SourceTextures;
			uint32_t TextureSlotIndex = 1;

			glm::vec4 VertexPositions[4] =
			{
				{ -0.5f, -0.5f, 0.0f, 1.0f },
				{ 0.5f, -0.5f, 0.0f, 1.0f },
				{ 0.5f, 0.5f, 0.0f, 1.0f },
				{ -0.5f, 0.5f, 0.0f, 1.0f }
			};

			Renderer2D::Statistics Stats;
		};

		Renderer2DData s_Data;

		template <typename Vertex, uint32_t MaxIndices>
		void CreateBatchPipeline(Batch<Vertex>& batch, const Rhi::Handle<Rhi::Shader>& shader,
			const BatchSpec& spec, const Rhi::Handle<Rhi::DescriptorSetLayout>& textureLayout)
		{
			batch.Pipeline = Renderer::GetDevice()->CreatePipeline(
				MakePipeline(shader, spec.Attributes, sizeof(Vertex), spec.Topology, textureLayout, spec.LineWidth));
		}

		template <typename Vertex, uint32_t MaxIndices>
		void CreateBatchBuffers(Batch<Vertex>& batch, const BatchSpec& spec, uint32_t maxVertices)
		{
			Rhi::BufferDesc vertexDesc;
			vertexDesc.Size = static_cast<uint64_t>(maxVertices) * sizeof(Vertex);
			vertexDesc.Usage = Rhi::BufferUsageVertex;
			vertexDesc.Memory = Rhi::MemoryHint::HostVisible;
			batch.VertexBuffer = Renderer::GetDevice()->CreateBuffer(vertexDesc);
			batch.Base = new Vertex[maxVertices];
			batch.Ptr = batch.Base;

			std::vector<uint32_t> indices(MaxIndices);
			uint32_t offset = 0;
			if (spec.Topology == Rhi::PrimitiveTopology::LineList)
			{
				for (uint32_t i = 0; i < MaxIndices; i += 2)
				{
					indices[i + 0] = offset + 0;
					indices[i + 1] = offset + 1;
					offset += 2;
				}
			}
			else
			{
				for (uint32_t i = 0; i < MaxIndices; i += 6)
				{
					indices[i + 0] = offset + 0;
					indices[i + 1] = offset + 1;
					indices[i + 2] = offset + 2;
					indices[i + 3] = offset + 2;
					indices[i + 4] = offset + 3;
					indices[i + 5] = offset + 0;
					offset += 4;
				}
			}
			Rhi::BufferDesc indexDesc;
			indexDesc.Size = MaxIndices * sizeof(uint32_t);
			indexDesc.Usage = Rhi::BufferUsageIndex;
			indexDesc.InitialData = indices.data();
			batch.IndexBuffer = Renderer::GetDevice()->CreateBuffer(indexDesc);
		}

		template <typename Vertex, uint32_t MaxIndices>
		void CreateBatch(Batch<Vertex>& batch, const Rhi::Handle<Rhi::Shader>& shader,
			const BatchSpec& spec, const Rhi::Handle<Rhi::DescriptorSetLayout>& textureLayout,
			uint32_t maxVertices)
		{
			CreateBatchPipeline<Vertex, MaxIndices>(batch, shader, spec, textureLayout);
			CreateBatchBuffers<Vertex, MaxIndices>(batch, spec, maxVertices);
		}

		template <typename Vertex>
		void ResetBatch(Batch<Vertex>& batch)
		{
			batch.IndexCount = 0;
			batch.Ptr = batch.Base;
		}
	}

	void Renderer2D::Init()
	{
		WLD_PROFILE_FUNCTION();

		// P4-4b:渲染通道结构与渲染器管线的采样数都来自 rendering.msaa。Renderer2D::Init
		// 是 Renderer::Init 里最早的渲染器初始化(Renderer3D::Init 的清单装载在它之后),
		// 这里先按工作目录装载一次,保证 2D 管线与随后创建的 3D 管线/场景通道拿到同一个
		// 生效采样数(否则 msaa>1 时 2D 管线在场景通道里绘制会被判为不兼容)。
		RenderSettings::LoadFromProject(std::filesystem::current_path());

		// 纹理描述符(binding 1 = 贴图槽;GL 侧单元号 = binding)。
		Rhi::DescriptorSetLayoutDesc textureLayout;
		textureLayout.Bindings.push_back({ 1, Rhi::DescriptorType::CombinedImageSampler,
			Rhi::ShaderStageFlag(Rhi::ShaderStage::Fragment), MaxTextureSlots });
		s_Data.TextureLayout =
			Renderer::GetDevice()->CreateDescriptorSetLayout(textureLayout);
		s_Data.TextureDescriptorSet =
			Renderer::GetDevice()->CreateDescriptorSet(
				s_Data.TextureLayout);

		Rhi::SamplerDesc samplerDesc;
		samplerDesc.MinFilter = Rhi::Filter::Linear;
		samplerDesc.MagFilter = Rhi::Filter::Linear;
		samplerDesc.AddressU = Rhi::SamplerAddressMode::Repeat;
		samplerDesc.AddressV = Rhi::SamplerAddressMode::Repeat;
		samplerDesc.AddressW = Rhi::SamplerAddressMode::Repeat;
		s_Data.Sampler = Renderer::GetDevice()->CreateSampler(samplerDesc);

		// Renderer2D 管线渲染进 SceneRenderer 目标;创建结构相同的渲染通道,
		// Vulkan 只要求管线与帧缓冲使用的 pass 兼容,无需同一实例。
		// P4-4b:结构必须跟随 rendering.msaa,与 SceneRenderer 的场景通道逐项一致
		// (msaa==1 时就是原来的三附录;>1 时是"3 个多采样附件 + 2 个单采样 resolve 目标"。
		// Vulkan 的渲染通道兼容性比较颜色/深度/resolve 引用的格式与采样数,少一项都不兼容)。
		const Rhi::SampleCount sceneSamples = static_cast<Rhi::SampleCount>(RenderSettings::Msaa());
		const bool multisampled = sceneSamples != Rhi::SampleCount::Count1;
		Rhi::RenderPassDesc passDesc;
		Rhi::RenderPassAttachment color;
		color.Format = Rhi::Format::R8G8B8A8_UNORM;
		color.Samples = sceneSamples;
		color.Load = Rhi::LoadOp::Clear;
		color.Store = Rhi::StoreOp::Store;
		color.InitialLayout = Rhi::AttachmentLayout::ColorAttachment;
		color.FinalLayout = Rhi::AttachmentLayout::ColorAttachment;
		Rhi::RenderPassAttachment entityId;
		entityId.Format = Rhi::Format::R32_SINT;
		entityId.Samples = sceneSamples;
		entityId.Load = Rhi::LoadOp::Clear;
		entityId.Store = Rhi::StoreOp::Store;
		entityId.InitialLayout = Rhi::AttachmentLayout::ColorAttachment;
		entityId.FinalLayout = Rhi::AttachmentLayout::ColorAttachment;
		Rhi::RenderPassAttachment depth;
		depth.Format = Rhi::Format::D24_UNORM_S8_UINT;
		depth.Samples = sceneSamples;
		depth.Load = Rhi::LoadOp::Clear;
		depth.Store = Rhi::StoreOp::Store;
		depth.InitialLayout = Rhi::AttachmentLayout::DepthStencilAttachment;
		depth.FinalLayout = Rhi::AttachmentLayout::DepthStencilAttachment;
		passDesc.Attachments = { color, entityId, depth };
		Rhi::SubpassDesc subpass;
		subpass.ColorAttachments = {
			{ 0, Rhi::AttachmentLayout::ColorAttachment },
			{ 1, Rhi::AttachmentLayout::ColorAttachment },
		};
		subpass.DepthStencilAttachment = { 2, Rhi::AttachmentLayout::DepthStencilAttachment };
		if (multisampled)
		{
			// 3 = 单采样颜色 resolve / 4 = 单采样实体 id resolve;本通道只用于建管线,
			// 不挂帧缓冲,但 resolve 引用必须与场景通道相同才能保持兼容。
			Rhi::RenderPassAttachment colorResolve;
			colorResolve.Format = Rhi::Format::R8G8B8A8_UNORM;
			colorResolve.Samples = Rhi::SampleCount::Count1;
			colorResolve.Load = Rhi::LoadOp::DontCare;
			colorResolve.Store = Rhi::StoreOp::Store;
			colorResolve.InitialLayout = Rhi::AttachmentLayout::ColorAttachment;
			colorResolve.FinalLayout = Rhi::AttachmentLayout::ColorAttachment;
			Rhi::RenderPassAttachment entityResolve;
			entityResolve.Format = Rhi::Format::R32_SINT;
			entityResolve.Samples = Rhi::SampleCount::Count1;
			entityResolve.Load = Rhi::LoadOp::DontCare;
			entityResolve.Store = Rhi::StoreOp::Store;
			entityResolve.InitialLayout = Rhi::AttachmentLayout::ColorAttachment;
			entityResolve.FinalLayout = Rhi::AttachmentLayout::ColorAttachment;
			passDesc.Attachments.push_back(colorResolve);
			passDesc.Attachments.push_back(entityResolve);
			subpass.ResolveAttachments = { 3, 4 };
		}
		passDesc.Subpasses = { subpass };
		s_RenderPass = Renderer::GetDevice()->CreateRenderPass(passDesc);

		// 白色纹理(槽 0)。
		Rhi::TextureDesc whiteDesc;
		whiteDesc.Type = Rhi::TextureType::Texture2D;
		whiteDesc.Format = Rhi::Format::R8G8B8A8_UNORM;
		whiteDesc.Extent = { 1, 1, 1 };
		whiteDesc.Usage = Rhi::TextureUsageSampled;
		s_Data.Textures[0] = Renderer::GetDevice()->CreateTexture(whiteDesc);
		const uint8_t white[4] = { 255, 255, 255, 255 };
		s_Data.Textures[0]->SetData(white, 4);
		s_Data.SourceTextures[0] = nullptr;

		const auto quadShader = CreateRendererShader(kQuadShaderPath, "Renderer2D-Quad");
		CreateBatch<QuadVertex, MaxQuads * 6>(s_Data.Quads, quadShader, QuadBatchSpec(),
			s_Data.TextureLayout, MaxQuads * 4);

		const auto circleShader = CreateRendererShader(kCircleShaderPath, "Renderer2D-Circle");
		CreateBatch<CircleVertex, MaxCircles * 6>(s_Data.Circles, circleShader, CircleBatchSpec(),
			s_Data.TextureLayout, MaxCircles * 4);

		const auto lineShader = CreateRendererShader(kLineShaderPath, "Renderer2D-Line");
		CreateBatch<LineVertex, MaxLines * 2>(s_Data.Lines, lineShader, LineBatchSpec(),
			s_Data.TextureLayout, MaxLines * 2);
	}

	// HOTR-P1-T3:引擎内建 shader 热重载 —— **只**重建 shader + 管线:render pass / 纹理描述符
	// 布局与描述符集 / 采样器 / 白纹理 / 顶点索引缓冲 / CPU 顶点数组全部保留。
	// 任一 stage 编不出字节码或任一条管线建不出来 → 整个 owner 放弃(旧句柄一个都不动),
	// 返回 0 并把可读原因写进 error。成功时返回重建的管线数,旧管线走 QueueRelease 延迟释放
	// (Vulkan 在飞命令缓冲仍引用旧 VkPipeline)。
	uint32_t Renderer2D::ReloadShaders(std::string* error)
	{
		WLD_PROFILE_FUNCTION();
		if (!Renderer::GetDevice())
		{
			if (error) *error = "RHI device is not initialized";
			return 0;
		}

		// 1) 三个 shader 先编出来(非断言入口;空 stage 视为失败)。
		std::string reason;
		const Rhi::Handle<Rhi::Shader> quadShader =
			TryCreateRendererShader(kQuadShaderPath, "Renderer2D-Quad", &reason);
		const Rhi::Handle<Rhi::Shader> circleShader = quadShader
			? TryCreateRendererShader(kCircleShaderPath, "Renderer2D-Circle", &reason) : nullptr;
		const Rhi::Handle<Rhi::Shader> lineShader = circleShader
			? TryCreateRendererShader(kLineShaderPath, "Renderer2D-Line", &reason) : nullptr;
		if (!lineShader)
		{
			if (error) *error = reason.empty() ? std::string("shader compilation failed") : reason;
			return 0;
		}

		// 2) 三条新管线全部建好;任一条失败 = 整体失败,既有句柄不动。
		const BatchSpec& quadSpec = QuadBatchSpec();
		const BatchSpec& circleSpec = CircleBatchSpec();
		const BatchSpec& lineSpec = LineBatchSpec();
		const Rhi::Handle<Rhi::Pipeline> quadPipeline = Renderer::GetDevice()->CreatePipeline(
			MakePipeline(quadShader, quadSpec.Attributes, sizeof(QuadVertex),
				quadSpec.Topology, s_Data.TextureLayout, quadSpec.LineWidth));
		const Rhi::Handle<Rhi::Pipeline> circlePipeline = Renderer::GetDevice()->CreatePipeline(
			MakePipeline(circleShader, circleSpec.Attributes, sizeof(CircleVertex),
				circleSpec.Topology, s_Data.TextureLayout, circleSpec.LineWidth));
		const Rhi::Handle<Rhi::Pipeline> linePipeline = Renderer::GetDevice()->CreatePipeline(
			MakePipeline(lineShader, lineSpec.Attributes, sizeof(LineVertex),
				lineSpec.Topology, s_Data.TextureLayout, lineSpec.LineWidth));
		if (!quadPipeline || !circlePipeline || !linePipeline)
		{
			if (error) *error = "pipeline creation failed (Renderer2D)";
			return 0;
		}

		// 3) 提交:替换 + 旧管线延迟释放。
		const Rhi::Handle<Rhi::Pipeline> oldQuad = s_Data.Quads.Pipeline;
		const Rhi::Handle<Rhi::Pipeline> oldCircle = s_Data.Circles.Pipeline;
		const Rhi::Handle<Rhi::Pipeline> oldLine = s_Data.Lines.Pipeline;
		s_Data.Quads.Pipeline = quadPipeline;
		s_Data.Circles.Pipeline = circlePipeline;
		s_Data.Lines.Pipeline = linePipeline;
		Renderer::QueueRelease([oldQuad, oldCircle, oldLine]() {});
		return 3;
	}

	void Renderer2D::Shutdown()
	{
		WLD_PROFILE_FUNCTION();
		s_CurrentCommandBuffer = nullptr;

		const auto release = [](auto& batch)
		{
			batch.Pipeline = nullptr;
			batch.VertexBuffer = nullptr;
			batch.IndexBuffer = nullptr;
			delete[] batch.Base;
			batch.Base = nullptr;
			batch.Ptr = nullptr;
			batch.IndexCount = 0;
		};
		release(s_Data.Quads);
		release(s_Data.Circles);
		release(s_Data.Lines);

		s_Data.Sampler = nullptr;
		s_RenderPass = nullptr;
		s_Data.TextureDescriptorSet = nullptr;
		s_Data.TextureLayout = nullptr;
		s_Data.Textures.fill(nullptr);
		s_Data.SourceTextures.fill(nullptr);
		s_Data.TextureSlotIndex = 1;
		s_Data.Stats = {};
	}

	void Renderer2D::BeginScene(const Camera&, const glm::mat4&, Rhi::Handle<Rhi::CommandBuffer> commandBuffer)
	{
		WLD_PROFILE_FUNCTION();
		s_CurrentCommandBuffer = commandBuffer;
	}

	void Renderer2D::EndScene()
	{
		WLD_PROFILE_FUNCTION();
		Flush();
		s_CurrentCommandBuffer = nullptr;
	}

	void Renderer2D::StartBatch()
	{
		s_Data.TextureSlotIndex = 1;
		ResetBatch(s_Data.Quads);
		ResetBatch(s_Data.Circles);
		ResetBatch(s_Data.Lines);
	}

	void Renderer2D::NextBatch()
	{
		Flush();
		StartBatch();
	}

	void Renderer2D::Flush()
	{
		if (!s_CurrentCommandBuffer)
			return;

		// 纹理描述符更新。
		std::vector<Rhi::DescriptorWrite> writes;
		writes.reserve(s_Data.TextureSlotIndex);
		for (uint32_t i = 0; i < s_Data.TextureSlotIndex; ++i)
		{
			Rhi::DescriptorWrite write;
			write.Binding = 1;
			write.ArrayIndex = i;
			write.Type = Rhi::DescriptorType::CombinedImageSampler;
			write.Texture = s_Data.Textures[i];
			write.Sampler = s_Data.Sampler;
			writes.push_back(write);
		}
		s_Data.TextureDescriptorSet->Update(writes);

		auto flushBatch = [&](auto& batch)
		{
			if (batch.Ptr == batch.Base)
				return;
			const uint64_t size = static_cast<uint64_t>(
				reinterpret_cast<uint8_t*>(batch.Ptr) - reinterpret_cast<uint8_t*>(batch.Base));
			// 映射写:批次上传发生在 render pass 内,Vulkan 不允许在那里录制
			// vkCmdUpdateBuffer/vkCmdCopyBuffer;缓冲按帧槽位环形化,上一轮 GPU 引用已由帧栅栏保证结束。
			batch.VertexBuffer->SetData(batch.Base, size, 0);
			s_CurrentCommandBuffer->BindPipeline(batch.Pipeline);
			// set 1 必须在绑定本管线之后立即绑定:同帧若先提交了 3D 对象(其 set 1 是对象 UBO 布局),
			// 提前绑定的纹理集与当前管线布局不兼容(VUID-vkCmdBindDescriptorSets-pDescriptorSets-00358)。
			s_CurrentCommandBuffer->BindDescriptorSet(s_Data.TextureDescriptorSet, 1);
			s_CurrentCommandBuffer->BindVertexBuffer(0, batch.VertexBuffer);
			s_CurrentCommandBuffer->BindIndexBuffer(batch.IndexBuffer);
			s_CurrentCommandBuffer->DrawIndexed(batch.IndexCount);
			s_Data.Stats.DrawCalls++;
		};

		flushBatch(s_Data.Lines);
		flushBatch(s_Data.Quads);
		flushBatch(s_Data.Circles);
	}

	void Renderer2D::DrawQuadCore(const glm::mat4& transform, const Rhi::Handle<Rhi::Texture>& texture,
		const glm::vec4& color, const glm::vec2* texCoords, float tilingFactor, int entityID)
	{
		WLD_PROFILE_FUNCTION();
		glm::vec3 transformedPositions[4];
		ComputeQuadPositions(transform, transformedPositions);
		DrawQuadPositions(transformedPositions, texture, color, texCoords, tilingFactor, entityID);
	}

	// 只做"每实体独立"的顶点变换:可在工作线程调用(只读 s_Data.VertexPositions)。
	//
	// 2D 的三处顶点变换(quad / circle / rect)**只有这一份实现**(标准 §6.5.1 P0/P1):
	// 之前 circle 与 rect 各写了一遍 glm 循环,和这里的批量核重复。
	// 统一是**纯重构**而非优化:批量核与 `glm::mat4 * glm::vec4` **逐位相同**
	// (由 `World.Simd` 钉住:2000 随机矩阵 × 4 角 × 3 分量,bit-differences=0),
	// 所以像素不可能变化。名字里的 `_SIMD_x4` 是历史命名:非 x86-64 目标下它就是标量回退。
	void Renderer2D::ComputeQuadPositions(const glm::mat4& transform, glm::vec3 outPositions[4])
	{
		Math::MultiplyMat4ByVec4_SIMD_x4(transform, s_Data.VertexPositions, outPositions);
	}

	void Renderer2D::ComputeCirclePositions(const glm::mat4& transform, glm::vec3 outPositions[4])
	{
		// 与 quad 同一次顶点变换(圆的四个包围盒角),走同一份实现。
		ComputeQuadPositions(transform, outPositions);
	}
	// 批次状态机部分(纹理槽/批缓冲指针):必须在主线程按原顺序执行。
	void Renderer2D::DrawQuadPositions(const glm::vec3 positions[4], const Rhi::Handle<Rhi::Texture>& texture,
		const glm::vec4& color, const glm::vec2* texCoords, float tilingFactor, int entityID)
	{
		WLD_PROFILE_FUNCTION();
		if (s_Data.Quads.IndexCount >= MaxQuads * 6)
			NextBatch();

		constexpr glm::vec2 defaultTexCoords[] = { { 0, 0 }, { 1, 0 }, { 1, 1 }, { 0, 1 } };
		const glm::vec2* actualTexCoords = texCoords ? texCoords : defaultTexCoords;

		// 纹理已经是 GPU 句柄(唯一驻留在 TextureLibrary),这里只做"本帧是否有同槽"的去重。
		float textureIndex = 0.0f;
		if (texture)
		{
			for (uint32_t i = 1; i < s_Data.TextureSlotIndex; i++)
				if (s_Data.SourceTextures[i].get() == texture.get())
				{
					textureIndex = static_cast<float>(i);
					break;
				}
			if (textureIndex == 0.0f)
			{
				if (s_Data.TextureSlotIndex >= MaxTextureSlots)
					NextBatch();
				textureIndex = static_cast<float>(s_Data.TextureSlotIndex);
				s_Data.Textures[s_Data.TextureSlotIndex] = texture;
				s_Data.SourceTextures[s_Data.TextureSlotIndex] = texture;
				s_Data.TextureSlotIndex++;
			}
		}

		for (uint32_t i = 0; i < 4; i++)
		{
			s_Data.Quads.Ptr->Position = positions[i];
			s_Data.Quads.Ptr->Color = color;
			s_Data.Quads.Ptr->TexCoord = actualTexCoords[i];
			s_Data.Quads.Ptr->TexIndex = textureIndex;
			s_Data.Quads.Ptr->TilingFactor = tilingFactor;
			s_Data.Quads.Ptr->EntityID = entityID;
			s_Data.Quads.Ptr++;
		}
		s_Data.Quads.IndexCount += 6;
		s_Data.Stats.QuadCount++;
	}

	void Renderer2D::DrawCircleCore(const glm::mat4& transform, const glm::vec4& color,
		float thickness, float fade, int entityID)
	{
		WLD_PROFILE_FUNCTION();
		glm::vec3 positions[4];
		ComputeCirclePositions(transform, positions);
		DrawCirclePositions(positions, color, thickness, fade, entityID);
	}

	void Renderer2D::DrawCirclePositions(const glm::vec3 positions[4], const glm::vec4& color,
		float thickness, float fade, int entityID)
	{
		WLD_PROFILE_FUNCTION();
		if (s_Data.Circles.IndexCount >= MaxCircles * 6)
			NextBatch();
		for (uint32_t i = 0; i < 4; i++)
		{
			s_Data.Circles.Ptr->WorldPosition = positions[i];
			s_Data.Circles.Ptr->LocalPosition = s_Data.VertexPositions[i] * 2.0f;
			s_Data.Circles.Ptr->Color = color;
			s_Data.Circles.Ptr->Thickness = thickness;
			s_Data.Circles.Ptr->Fade = fade;
			s_Data.Circles.Ptr->EntityID = entityID;
			s_Data.Circles.Ptr++;
		}
		s_Data.Circles.IndexCount += 6;
		s_Data.Stats.CircleCount++;
	}

	void Renderer2D::DrawLineCore(const glm::vec3& p0, const glm::vec3& p1,
		const glm::vec4& color, int entityID)
	{
		WLD_PROFILE_FUNCTION();
		if (s_Data.Lines.IndexCount >= MaxLines * 2)
			NextBatch();
		s_Data.Lines.Ptr->Position = p0;
		s_Data.Lines.Ptr->Color = color;
		s_Data.Lines.Ptr->EntityID = entityID;
		s_Data.Lines.Ptr++;
		s_Data.Lines.Ptr->Position = p1;
		s_Data.Lines.Ptr->Color = color;
		s_Data.Lines.Ptr->EntityID = entityID;
		s_Data.Lines.Ptr++;
		s_Data.Lines.IndexCount += 2;
		s_Data.Stats.LineCount++;
	}

	void Renderer2D::DrawRectCore(const glm::mat4& transform, const glm::vec4& color, int entityID)
	{
		WLD_PROFILE_FUNCTION();
		glm::vec3 vertices[4];
		ComputeQuadPositions(transform, vertices);   // 与 quad/circle 同一份顶点变换
		for (uint32_t i = 0; i < 4; i++)
			vertices[i].z = 0.0f;
		DrawLineCore(vertices[0], vertices[1], color, entityID);
		DrawLineCore(vertices[1], vertices[2], color, entityID);
		DrawLineCore(vertices[2], vertices[3], color, entityID);
		DrawLineCore(vertices[3], vertices[0], color, entityID);
	}

	void Renderer2D::DrawQuad(const glm::vec2& position, const glm::vec2& size, const glm::vec4& color)
	{
		DrawQuad(glm::vec3(position, 0.0f), size, color);
	}

	void Renderer2D::DrawQuad(const glm::vec3& position, const glm::vec2& size, const glm::vec4& color)
	{
		DrawQuadCore(glm::translate(glm::mat4(1.0f), position) * glm::scale(glm::mat4(1.0f), { size.x, size.y, 1.0f }),
			nullptr, color, nullptr, 1.0f, -1);
	}

	void Renderer2D::DrawQuad(const glm::vec2& position, const glm::vec2& size, const Rhi::Handle<Rhi::Texture>& texture, float tilingFactor)
	{
		DrawQuad(glm::vec3(position, 0.0f), size, texture, tilingFactor);
	}

	void Renderer2D::DrawQuad(const glm::vec3& position, const glm::vec2& size, const Rhi::Handle<Rhi::Texture>& texture, float tilingFactor)
	{
		DrawQuadCore(glm::translate(glm::mat4(1.0f), position) * glm::scale(glm::mat4(1.0f), { size.x, size.y, 1.0f }),
			texture, glm::vec4(1.0f), nullptr, tilingFactor, -1);
	}
	void Renderer2D::DrawQuad(const glm::mat4& transform, const Rhi::Handle<Rhi::Texture>& texture,
		float tilingFactor, const glm::vec4& tintColor)
	{
		DrawQuadCore(transform, texture, tintColor, nullptr, tilingFactor, -1);
	}

	void Renderer2D::DrawRotatedQuad(const glm::vec2& position, const glm::vec2& size, float rotation, const glm::vec4& color)
	{
		DrawRotatedQuad(glm::vec3(position, 0.0f), size, rotation, color);
	}

	void Renderer2D::DrawRotatedQuad(const glm::vec3& position, const glm::vec2& size, float rotation, const glm::vec4& color)
	{
		WLD_PROFILE_FUNCTION();
		glm::mat4 transform = glm::translate(glm::mat4(1.0f), position)
			* glm::rotate(glm::mat4(1.0f), rotation, { 0.0f, 0.0f, 1.0f })
			* glm::scale(glm::mat4(1.0f), { size.x, size.y, 1.0f });
		DrawQuadCore(transform, nullptr, color);
	}

	void Renderer2D::DrawRotatedQuad(const glm::vec2& position, const glm::vec2& size, float rotation,
		const Rhi::Handle<Rhi::Texture>& texture, float tilingFactor, const glm::vec4& tintColor)
	{
		DrawRotatedQuad(glm::vec3(position, 0.0f), size, rotation, texture, tilingFactor, tintColor);
	}

	void Renderer2D::DrawRotatedQuad(const glm::vec3& position, const glm::vec2& size, float rotation,
		const Rhi::Handle<Rhi::Texture>& texture, float tilingFactor, const glm::vec4& tintColor)
	{
		WLD_PROFILE_FUNCTION();
		glm::mat4 transform = glm::translate(glm::mat4(1.0f), position)
			* glm::rotate(glm::mat4(1.0f), rotation, { 0.0f, 0.0f, 1.0f })
			* glm::scale(glm::mat4(1.0f), { size.x, size.y, 1.0f });
		DrawQuadCore(transform, texture, tintColor, nullptr, tilingFactor, -1);
	}
	void Renderer2D::DrawRect(const glm::vec3& position, const glm::vec2& size, const glm::vec4& color, float rotation)
	{
		WLD_PROFILE_FUNCTION();
		glm::mat4 transform = glm::translate(glm::mat4(1.0f), position)
			* glm::rotate(glm::mat4(1.0f), rotation, { 0.0f, 0.0f, 1.0f })
			* glm::scale(glm::mat4(1.0f), { size.x, size.y, 1.0f });
		DrawRectCore(transform, color, -1);
	}

	Renderer2D::Statistics Renderer2D::GetStats()
	{
		return s_Data.Stats;
	}

	void Renderer2D::ResetStats()
	{
		memset(&s_Data.Stats, 0, sizeof(Statistics));
	}
}
