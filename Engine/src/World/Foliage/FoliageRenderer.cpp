#include "wldpch.h"
#include "World/Foliage/FoliageRenderer.h"
#include "World/Renderer/Renderer.h"
#include "World/Renderer/Renderer3D.h"
#include "World/Renderer/RenderSettings.h"
#include "World/Renderer/ShaderUtils.h"
#include "World/Utils/Paths.h"
#include "World/Renderer/Texture/TextureLibrary.h"
#include "World/Core/StringPool.h"
#include "World/Core/Log.h"

#include <glm/gtc/matrix_transform.hpp>
#include <unordered_map>

namespace World
{
	namespace
	{
		struct FoliageVertex
		{
			glm::vec3 Position;
			glm::vec3 Normal;
			glm::vec2 TexCoord;
		};

		struct FoliageRenderState
		{
			bool Initialized { false };
			Rhi::Handle<Rhi::RenderPass> MainRenderPass;
			Rhi::Handle<Rhi::Pipeline> Pipeline;
			Rhi::Handle<Rhi::DescriptorSetLayout> UniformLayout;
			Rhi::Handle<Rhi::DescriptorSetLayout> TextureLayout;
			Rhi::Handle<Rhi::DescriptorSet> UniformSet;
			Rhi::Handle<Rhi::DescriptorSet> TextureSet;
			Rhi::Handle<Rhi::Buffer> UniformBuffer;
			Rhi::Handle<Rhi::Texture> DefaultAlbedo;
			Rhi::Handle<Rhi::Sampler> Sampler;

			Rhi::Handle<Rhi::Buffer> DefaultMeshVB;
			Rhi::Handle<Rhi::Buffer> DefaultMeshIB;
			uint32_t DefaultMeshIndexCount { 0 };

			Rhi::Handle<Rhi::Buffer> InstanceBuffer;
			uint32_t InstanceCapacity { 0 };

			Rhi::Handle<Rhi::CommandBuffer> CurrentCmdBuffer;
			Rhi::Handle<Rhi::DescriptorSet> CurrentGlobalSet;

			std::unordered_map<std::string, std::shared_ptr<FoliageData>> FoliageCache;
		};

		static FoliageRenderState s_State;

		static Rhi::Handle<Rhi::RenderPass> CreateCompatibleMainRenderPass()
		{
			const Rhi::SampleCount sceneSamples = static_cast<Rhi::SampleCount>(RenderSettings::Msaa());

			Rhi::RenderPassDesc passDesc;
			Rhi::RenderPassAttachment color;
			color.Format = Rhi::Format::R8G8B8A8_UNORM;
			color.Samples = sceneSamples;
			color.Load = Rhi::LoadOp::Load;
			color.Store = Rhi::StoreOp::Store;
			color.InitialLayout = Rhi::AttachmentLayout::ColorAttachment;
			color.FinalLayout = Rhi::AttachmentLayout::ColorAttachment;

			Rhi::RenderPassAttachment entityId;
			entityId.Format = Rhi::Format::R32_SINT;
			entityId.Samples = sceneSamples;
			entityId.Load = Rhi::LoadOp::Load;
			entityId.Store = Rhi::StoreOp::Store;
			entityId.InitialLayout = Rhi::AttachmentLayout::ColorAttachment;
			entityId.FinalLayout = Rhi::AttachmentLayout::ColorAttachment;

			Rhi::RenderPassAttachment depth;
			depth.Format = Rhi::Format::D24_UNORM_S8_UINT;
			depth.Samples = sceneSamples;
			depth.Load = Rhi::LoadOp::Load;
			depth.Store = Rhi::StoreOp::Store;
			depth.InitialLayout = Rhi::AttachmentLayout::DepthStencilAttachment;
			depth.FinalLayout = Rhi::AttachmentLayout::DepthStencilAttachment;

			passDesc.Attachments = { color, entityId, depth };
			Rhi::SubpassDesc subpass;
			subpass.ColorAttachments = {
				{ 0, Rhi::AttachmentLayout::ColorAttachment },
				{ 1, Rhi::AttachmentLayout::ColorAttachment }
			};
			subpass.DepthStencilAttachment = { 2, Rhi::AttachmentLayout::DepthStencilAttachment };
			passDesc.Subpasses = { subpass };
			passDesc.DebugName = "Foliage.CompatiblePass";
			return Renderer::GetDevice()->CreateRenderPass(passDesc);
		}
	}

	void FoliageRenderer::Initialize()
	{
		if (s_State.Initialized)
			return;

		auto device = Renderer::GetDevice();
		if (!device)
			return;

		// 1. 常量缓冲与布局
		{
			Rhi::DescriptorSetLayoutDesc uDesc;
			uDesc.Bindings.push_back({ 1, Rhi::DescriptorType::UniformBuffer,
				Rhi::ShaderStageFlag(Rhi::ShaderStage::Vertex) | Rhi::ShaderStageFlag(Rhi::ShaderStage::Fragment), 1 });
			uDesc.DebugName = "Foliage.UniformLayout";
			s_State.UniformLayout = device->CreateDescriptorSetLayout(uDesc);

			Rhi::BufferDesc ubDesc;
			ubDesc.Size = sizeof(FoliageShaderUniforms);
			ubDesc.Usage = Rhi::BufferUsageUniform;
			ubDesc.Memory = Rhi::MemoryHint::HostVisible;
			ubDesc.DebugName = "Foliage.UniformBuffer";
			s_State.UniformBuffer = device->CreateBuffer(ubDesc);

			if (s_State.UniformLayout && s_State.UniformBuffer)
			{
				s_State.UniformSet = device->CreateDescriptorSet(s_State.UniformLayout);
				if (s_State.UniformSet)
				{
					Rhi::DescriptorWrite w;
					w.Binding = 1;
					w.Type = Rhi::DescriptorType::UniformBuffer;
					w.Buffer = s_State.UniformBuffer;
					w.BufferOffset = 0;
					w.BufferRange = sizeof(FoliageShaderUniforms);
					s_State.UniformSet->Update({ w });
				}
			}
		}

		// 2. 贴图采样器与默认绿色草地贴图
		{
			Rhi::DescriptorSetLayoutDesc tDesc;
			tDesc.Bindings.push_back({ 4, Rhi::DescriptorType::CombinedImageSampler,
				Rhi::ShaderStageFlag(Rhi::ShaderStage::Fragment), 1 });
			tDesc.DebugName = "Foliage.TextureLayout";
			s_State.TextureLayout = device->CreateDescriptorSetLayout(tDesc);

			Rhi::SamplerDesc sDesc;
			sDesc.MinFilter = Rhi::Filter::Linear;
			sDesc.MagFilter = Rhi::Filter::Linear;
			sDesc.AddressU = Rhi::SamplerAddressMode::Repeat;
			sDesc.AddressV = Rhi::SamplerAddressMode::Repeat;
			sDesc.AddressW = Rhi::SamplerAddressMode::Repeat;
			sDesc.DebugName = "Foliage.Sampler";
			s_State.Sampler = device->CreateSampler(sDesc);

			Rhi::Handle<Rhi::Texture> tex = TextureLibrary::Get().Get("textures/foliage/grass_tuft.png", /*srgb*/ true);
			if (tex)
			{
				s_State.DefaultAlbedo = tex;
			}
			else
			{
				constexpr uint32_t kFoliageTexDim = 128;
				std::vector<uint8_t> foliageBuffer(kFoliageTexDim * kFoliageTexDim * 4, 0);

				for (int b = 0; b < 12; ++b)
				{
					const float baseX = 64.0f + (b - 6.0f) * 6.0f;
					const float bend = (b - 5.5f) * 4.5f;
					for (uint32_t y = 10; y < kFoliageTexDim - 5; ++y)
					{
						const float t = 1.0f - static_cast<float>(y) / static_cast<float>(kFoliageTexDim);
						const float cx = baseX + bend * (t * t);
						const float halfW = std::max(1.0f, (1.0f - t * 0.9f) * 4.0f);

						for (int dx = -static_cast<int>(halfW); dx <= static_cast<int>(halfW); ++dx)
						{
							const int px = static_cast<int>(cx) + dx;
							if (px >= 0 && px < static_cast<int>(kFoliageTexDim))
							{
								const size_t idx = (y * kFoliageTexDim + px) * 4;
								foliageBuffer[idx + 0] = static_cast<uint8_t>(35.0f + t * 90.0f);
								foliageBuffer[idx + 1] = static_cast<uint8_t>(75.0f + t * 135.0f);
								foliageBuffer[idx + 2] = static_cast<uint8_t>(25.0f + t * 35.0f);
								foliageBuffer[idx + 3] = 0xFF;
							}
						}
					}
				}

				Rhi::TextureDesc texDesc;
				texDesc.Type = Rhi::TextureType::Texture2D;
				texDesc.Format = Rhi::Format::R8G8B8A8_UNORM;
				texDesc.Extent = { kFoliageTexDim, kFoliageTexDim, 1 };
				texDesc.Usage = Rhi::TextureUsageSampled;
				texDesc.DebugName = "Foliage.DefaultAlbedo";
				s_State.DefaultAlbedo = device->CreateTexture(texDesc);
				if (s_State.DefaultAlbedo)
					s_State.DefaultAlbedo->SetData(foliageBuffer.data(), foliageBuffer.size());
			}

			if (s_State.TextureLayout && s_State.DefaultAlbedo)
			{
				s_State.TextureSet = device->CreateDescriptorSet(s_State.TextureLayout);
				if (s_State.TextureSet)
				{
					Rhi::DescriptorWrite w;
					w.Binding = 4;
					w.Type = Rhi::DescriptorType::CombinedImageSampler;
					w.Texture = s_State.DefaultAlbedo;
					w.Sampler = s_State.Sampler;
					s_State.TextureSet->Update({ w });
				}
			}
		}

		// 3. 工业级 3 面交叉自然弯曲草丛模型 (3 quads, 12 vertices, 18 indices)
		{
			constexpr float kPi = 3.1415926535f;
			FoliageVertex vertices[12];
			uint32_t indices[18];

			for (int q = 0; q < 3; ++q)
			{
				const float angle = static_cast<float>(q) * (kPi / 3.0f);
				const float cosA = std::cos(angle);
				const float sinA = std::sin(angle);
				const glm::vec3 dir(cosA * 0.5f, 0.0f, sinA * 0.5f);
				const glm::vec3 normal = glm::normalize(glm::vec3(-sinA * 0.4f, 0.85f, cosA * 0.4f));

				const int baseV = q * 4;
				vertices[baseV + 0] = { -dir, normal, { 0.0f, 1.0f } };
				vertices[baseV + 1] = {  dir, normal, { 1.0f, 1.0f } };
				vertices[baseV + 2] = {  dir + glm::vec3(0.0f, 1.4f, 0.0f), normal, { 1.0f, 0.0f } };
				vertices[baseV + 3] = { -dir + glm::vec3(0.0f, 1.4f, 0.0f), normal, { 0.0f, 0.0f } };

				const int baseI = q * 6;
				indices[baseI + 0] = baseV + 0;
				indices[baseI + 1] = baseV + 1;
				indices[baseI + 2] = baseV + 2;
				indices[baseI + 3] = baseV + 0;
				indices[baseI + 4] = baseV + 2;
				indices[baseI + 5] = baseV + 3;
			}

			Rhi::BufferDesc vbDesc;
			vbDesc.Size = sizeof(vertices);
			vbDesc.Usage = Rhi::BufferUsageVertex;
			vbDesc.Memory = Rhi::MemoryHint::DeviceLocal;
			vbDesc.InitialData = vertices;
			vbDesc.DebugName = "Foliage.DefaultMeshVB";
			s_State.DefaultMeshVB = device->CreateBuffer(vbDesc);

			Rhi::BufferDesc ibDesc;
			ibDesc.Size = sizeof(indices);
			ibDesc.Usage = Rhi::BufferUsageIndex;
			ibDesc.Memory = Rhi::MemoryHint::DeviceLocal;
			ibDesc.InitialData = indices;
			ibDesc.DebugName = "Foliage.DefaultMeshIB";
			s_State.DefaultMeshIB = device->CreateBuffer(ibDesc);
			s_State.DefaultMeshIndexCount = 18;
		}

		// 4. 着色器与渲染管线
		s_State.MainRenderPass = CreateCompatibleMainRenderPass();
		if (s_State.MainRenderPass)
		{
			Rhi::ShaderDesc sDesc;
			sDesc.DebugName = "Foliage-Shader";
			sDesc.Stages.push_back(ShaderCompiler::CompileStage(Rhi::ShaderStage::Vertex,
				"assets/shaders/Foliage.slang", "VSMain", "vs_6_0"));
			sDesc.Stages.push_back(ShaderCompiler::CompileStage(Rhi::ShaderStage::Fragment,
				"assets/shaders/Foliage.slang", "PSMain", "ps_6_0"));
			auto shader = device->CreateShader(sDesc);

			if (shader)
			{
				Rhi::PipelineDesc pDesc;
				pDesc.Shader = shader;
				pDesc.RenderPass = s_State.MainRenderPass;
				pDesc.DescriptorSetLayouts = { Renderer::GetGlobalDescriptorSetLayout(),
					s_State.UniformLayout, s_State.TextureLayout };

				Rhi::VertexBinding b0;
				b0.Binding = 0;
				b0.Stride = sizeof(FoliageVertex);
				b0.PerInstance = false;

				Rhi::VertexBinding b1;
				b1.Binding = 1;
				b1.Stride = sizeof(FoliageInstanceData);
				b1.PerInstance = true;

				pDesc.VertexBindings = { b0, b1 };

				Rhi::VertexAttribute aPos { 0, 0, Rhi::Format::R32G32B32_SFLOAT, static_cast<uint32_t>(offsetof(FoliageVertex, Position)) };
				Rhi::VertexAttribute aNorm { 1, 0, Rhi::Format::R32G32B32_SFLOAT, static_cast<uint32_t>(offsetof(FoliageVertex, Normal)) };
				Rhi::VertexAttribute aUV { 2, 0, Rhi::Format::R32G32_SFLOAT, static_cast<uint32_t>(offsetof(FoliageVertex, TexCoord)) };

				Rhi::VertexAttribute aX0 { 4, 1, Rhi::Format::R32G32B32A32_SFLOAT, 0 };
				Rhi::VertexAttribute aX1 { 5, 1, Rhi::Format::R32G32B32A32_SFLOAT, 16 };
				Rhi::VertexAttribute aX2 { 6, 1, Rhi::Format::R32G32B32A32_SFLOAT, 32 };
				Rhi::VertexAttribute aX3 { 7, 1, Rhi::Format::R32G32B32A32_SFLOAT, 48 };
				Rhi::VertexAttribute aParams { 8, 1, Rhi::Format::R32G32B32A32_SFLOAT, static_cast<uint32_t>(offsetof(FoliageInstanceData, Params)) };

				pDesc.VertexAttributes = { aPos, aNorm, aUV, aX0, aX1, aX2, aX3, aParams };
				pDesc.Topology = Rhi::PrimitiveTopology::TriangleList;
				pDesc.Cull = Rhi::CullMode::None;
				pDesc.DepthStencil.DepthTest = true;
				pDesc.DepthStencil.DepthWrite = true;
				pDesc.DepthStencil.DepthCompare = Rhi::CompareOp::LessOrEqual;
				pDesc.Samples = static_cast<Rhi::SampleCount>(RenderSettings::Msaa());
				pDesc.DebugName = "Foliage.MainPipeline";
				s_State.Pipeline = device->CreatePipeline(pDesc);
			}
		}

		s_State.Initialized = true;
	}

	void FoliageRenderer::Shutdown()
	{
		ClearCache();
		s_State = FoliageRenderState {};
	}

	void FoliageRenderer::ClearCache()
	{
		s_State.FoliageCache.clear();
	}

	void FoliageRenderer::SetContext(const Rhi::Handle<Rhi::CommandBuffer>& cmdBuffer,
		const Rhi::Handle<Rhi::DescriptorSet>& globalSet)
	{
		s_State.CurrentCmdBuffer = cmdBuffer;
		s_State.CurrentGlobalSet = globalSet;
	}

	std::shared_ptr<FoliageData> FoliageRenderer::GetOrCreateFoliageData(const AssetRef& asset)
	{
		const std::string path = std::string(StringPool::Get().PathOf(asset.Path));
		if (path.empty())
		{
			static auto fallback = std::make_shared<FoliageData>();
			return fallback;
		}

		auto it = s_State.FoliageCache.find(path);
		if (it != s_State.FoliageCache.end())
			return it->second;

		std::filesystem::path resolvedPath = path;
		if (!std::filesystem::exists(resolvedPath))
		{
			resolvedPath = World::Paths::AssetRoot() / path;
			if (!std::filesystem::exists(resolvedPath))
				resolvedPath = World::Paths::ProjectFile(path);
		}

		auto foliage = std::make_shared<FoliageData>();
		if (std::filesystem::exists(resolvedPath))
		{
			foliage->LoadFromFile(resolvedPath.string());
		}
		s_State.FoliageCache[path] = foliage;
		return foliage;
	}

	void FoliageRenderer::RenderFoliage(const FoliageData& data, const FoliageComponent& comp,
		const glm::vec3& cameraPosition, const glm::mat4& viewProj, float time)
	{
		if (!s_State.Initialized)
			Initialize();

		if (!s_State.Pipeline || !s_State.DefaultMeshVB || s_State.DefaultMeshIndexCount == 0)
			return;

		auto device = Renderer::GetDevice();
		auto cmd = s_State.CurrentCmdBuffer;
		if (!device || !cmd)
			return;

		std::vector<FoliageInstanceData> visibleInstances;
		visibleInstances.reserve(1024);
		data.CollectVisibleInstances(cameraPosition, viewProj, comp.MaxCullDistance, comp.FadeRange, visibleInstances);

		if (visibleInstances.empty())
			return;

		const uint32_t neededSize = static_cast<uint32_t>(visibleInstances.size() * sizeof(FoliageInstanceData));
		if (!s_State.InstanceBuffer || s_State.InstanceCapacity < neededSize)
		{
			if (s_State.InstanceBuffer)
			{
				auto oldBuf = std::move(s_State.InstanceBuffer);
				Renderer::QueueRelease([oldBuf = std::move(oldBuf)]() mutable {});
			}
			Rhi::BufferDesc bDesc;
			bDesc.Size = std::max(neededSize, 65536u);
			bDesc.Usage = Rhi::BufferUsageVertex;
			bDesc.Memory = Rhi::MemoryHint::HostVisible;
			bDesc.DebugName = "Foliage.InstanceBuffer";
			s_State.InstanceBuffer = device->CreateBuffer(bDesc);
			s_State.InstanceCapacity = bDesc.Size;
		}

		if (s_State.InstanceBuffer)
			s_State.InstanceBuffer->SetData(visibleInstances.data(), neededSize);

		FoliageShaderUniforms uniforms;
		uniforms.WindParams = glm::vec4(comp.WindFrequency, comp.WindStrength, time, 1.0f);
		uniforms.WindDirection = glm::vec4(1.0f, 0.0f, 0.3f, 0.0f);
		uniforms.CullParams = glm::vec4(comp.MaxCullDistance, comp.FadeRange, 0.0f, 0.0f);
		if (s_State.UniformBuffer)
			s_State.UniformBuffer->SetData(&uniforms, sizeof(uniforms));

		Rhi::Handle<Rhi::Texture> currentTex = TextureLibrary::Get().Get("textures/foliage/grass_tuft.png", /*srgb*/ true);
		if (!currentTex)
			currentTex = s_State.DefaultAlbedo;
		if (currentTex && s_State.TextureSet)
		{
			Rhi::DescriptorWrite w;
			w.Binding = 4;
			w.Type = Rhi::DescriptorType::CombinedImageSampler;
			w.Texture = currentTex;
			w.Sampler = s_State.Sampler;
			s_State.TextureSet->Update({ w });
		}

		cmd->BindPipeline(s_State.Pipeline);
		if (s_State.CurrentGlobalSet)
			cmd->BindDescriptorSet(s_State.CurrentGlobalSet, 0);
		if (s_State.UniformSet)
			cmd->BindDescriptorSet(s_State.UniformSet, 1);
		if (s_State.TextureSet)
			cmd->BindDescriptorSet(s_State.TextureSet, 2);

		cmd->BindVertexBuffer(0, s_State.DefaultMeshVB, 0);
		cmd->BindVertexBuffer(1, s_State.InstanceBuffer, 0);
		cmd->BindIndexBuffer(s_State.DefaultMeshIB, 0, Rhi::IndexType::UInt32);

		cmd->DrawIndexed(s_State.DefaultMeshIndexCount, static_cast<uint32_t>(visibleInstances.size()), 0, 0, 0);
	}
}
