#include "wldpch.h"
#include "World/Terrain/TerrainRenderer.h"
#include "World/Renderer/Renderer.h"
#include "World/Renderer/Renderer3D.h"
#include "World/Renderer/RenderSettings.h"
#include "World/Renderer/ShaderUtils.h"
#include "World/Renderer/Texture/TextureLibrary.h"
#include "World/Core/StringPool.h"
#include "World/Scene/Components.h"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>

namespace World
{
	namespace
	{
		struct ChunkGpuMesh
		{
			uint32_t Lod = 999;
			uint32_t IndexCount = 0;
			Rhi::Handle<Rhi::Buffer> VertexBuffer;
			Rhi::Handle<Rhi::Buffer> IndexBuffer;
		};

		struct TerrainRenderState
		{
			bool Initialized = false;
			Rhi::Handle<Rhi::RenderPass> MainRenderPass;
			Rhi::Handle<Rhi::DescriptorSetLayout> ObjectLayout;
			Rhi::Handle<Rhi::DescriptorSetLayout> MaterialLayout;
			Rhi::Handle<Rhi::Pipeline> Pipeline;
			Rhi::Handle<Rhi::Pipeline> ShadowPipeline;

			Rhi::Handle<Rhi::Sampler> Sampler;
			Rhi::Handle<Rhi::Texture> DefaultTextures[4];

			// 帧槽位 UniformBuffer 与描述符集
			Rhi::Handle<Rhi::Buffer> UniformBuffers[Renderer::FramesInFlight];
			Rhi::Handle<Rhi::DescriptorSet> ObjectDescriptorSets[Renderer::FramesInFlight];
			Rhi::Handle<Rhi::DescriptorSet> MaterialDescriptorSets[Renderer::FramesInFlight];

			Rhi::Handle<Rhi::CommandBuffer> CurrentCommandBuffer;
			Rhi::Handle<Rhi::DescriptorSet> CurrentGlobalSet;
			glm::mat4 CurrentViewProjection { 1.0f };

			std::unordered_map<uint32_t, ChunkGpuMesh> ChunkCache;
			const TerrainData* LastDataPtr = nullptr;

			std::unordered_map<std::string, std::shared_ptr<TerrainData>> DataCache;
			std::shared_ptr<TerrainData> DefaultData;
		};

		static TerrainRenderState s_State;

		static Rhi::Handle<Rhi::RenderPass> CreateCompatibleMainRenderPass()
		{
			const Rhi::SampleCount sceneSamples = static_cast<Rhi::SampleCount>(RenderSettings::Msaa());
			const bool multisampled = sceneSamples != Rhi::SampleCount::Count1;

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

			if (multisampled)
			{
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
			passDesc.DebugName = "Terrain.CompatibleRenderPass";
			return Renderer::GetDevice()->CreateRenderPass(passDesc);
		}
	}

	void TerrainRenderer::Init(const Rhi::Handle<Rhi::RenderPass>& mainRenderPass)
	{
		WLD_TRACE_FUNCTION();
		const auto& device = Renderer::GetDevice();
		if (!device)
			return;

		if (mainRenderPass)
			s_State.MainRenderPass = mainRenderPass;
		else if (!s_State.MainRenderPass)
			s_State.MainRenderPass = CreateCompatibleMainRenderPass();

		// 1. 创建描述符集布局
		if (!s_State.ObjectLayout)
		{
			Rhi::DescriptorSetLayoutDesc objDesc;
			objDesc.Bindings.push_back({ 1, Rhi::DescriptorType::UniformBuffer,
				Rhi::ShaderStageFlag(Rhi::ShaderStage::Vertex) | Rhi::ShaderStageFlag(Rhi::ShaderStage::Fragment), 1 });
			objDesc.DebugName = "Terrain.ObjectLayout";
			s_State.ObjectLayout = device->CreateDescriptorSetLayout(objDesc);
		}

		if (!s_State.MaterialLayout)
		{
			Rhi::DescriptorSetLayoutDesc matDesc;
			for (uint32_t i = 0; i < 4; ++i)
			{
				matDesc.Bindings.push_back({ 4 + i, Rhi::DescriptorType::CombinedImageSampler,
					Rhi::ShaderStageFlag(Rhi::ShaderStage::Fragment), 1 });
			}
			matDesc.DebugName = "Terrain.MaterialLayout";
			s_State.MaterialLayout = device->CreateDescriptorSetLayout(matDesc);
		}

		// 2. 创建材质采样器 (Repeat + Linear)
		if (!s_State.Sampler)
		{
			Rhi::SamplerDesc samplerDesc;
			samplerDesc.MinFilter = Rhi::Filter::Linear;
			samplerDesc.MagFilter = Rhi::Filter::Linear;
			samplerDesc.AddressU = Rhi::SamplerAddressMode::Repeat;
			samplerDesc.AddressV = Rhi::SamplerAddressMode::Repeat;
			samplerDesc.AddressW = Rhi::SamplerAddressMode::Repeat;
			samplerDesc.DebugName = "Terrain.Sampler";
			s_State.Sampler = device->CreateSampler(samplerDesc);
		}

		// 3. 创建 4 层程序化默认纯色纹理 (草地绿、泥土棕、岩石灰、沙地黄)
		const uint8_t defaultColors[4][4] = {
			{ 0x38, 0x8E, 0x3C, 0xFF }, // Layer 0: 草地绿色
			{ 0x5D, 0x40, 0x37, 0xFF }, // Layer 1: 泥土棕色
			{ 0x78, 0x90, 0x9C, 0xFF }, // Layer 2: 岩石青灰色
			{ 0xD7, 0xCC, 0x8C, 0xFF }  // Layer 3: 沙地浅黄色
		};
		for (uint32_t i = 0; i < 4; ++i)
		{
			if (!s_State.DefaultTextures[i])
			{
				Rhi::TextureDesc texDesc;
				texDesc.Type = Rhi::TextureType::Texture2D;
				texDesc.Format = Rhi::Format::R8G8B8A8_UNORM;
				texDesc.Extent = { 1, 1, 1 };
				texDesc.Usage = Rhi::TextureUsageSampled;
				texDesc.DebugName = "Terrain.DefaultTexture" + std::to_string(i);
				s_State.DefaultTextures[i] = device->CreateTexture(texDesc);
				if (s_State.DefaultTextures[i])
					s_State.DefaultTextures[i]->SetData(defaultColors[i], 4);
			}
		}

		// 4. 创建帧槽位 UniformBuffer 与描述符集
		for (uint32_t slot = 0; slot < Renderer::FramesInFlight; ++slot)
		{
			if (!s_State.UniformBuffers[slot])
			{
				Rhi::BufferDesc uboDesc;
				uboDesc.Size = sizeof(TerrainUniforms);
				uboDesc.Usage = Rhi::BufferUsageUniform;
				uboDesc.Memory = Rhi::MemoryHint::HostVisible;
				uboDesc.DebugName = "Terrain.UBO." + std::to_string(slot);
				s_State.UniformBuffers[slot] = device->CreateBuffer(uboDesc);
			}

			if (!s_State.ObjectDescriptorSets[slot] && s_State.ObjectLayout)
			{
				s_State.ObjectDescriptorSets[slot] = device->CreateDescriptorSet(s_State.ObjectLayout);
				if (s_State.ObjectDescriptorSets[slot] && s_State.UniformBuffers[slot])
				{
					Rhi::DescriptorWrite write;
					write.Binding = 1;
					write.Type = Rhi::DescriptorType::UniformBuffer;
					write.Buffer = s_State.UniformBuffers[slot];
					write.BufferOffset = 0;
					write.BufferRange = sizeof(TerrainUniforms);
					s_State.ObjectDescriptorSets[slot]->Update({ write });
				}
			}

			if (!s_State.MaterialDescriptorSets[slot] && s_State.MaterialLayout)
			{
				s_State.MaterialDescriptorSets[slot] = device->CreateDescriptorSet(s_State.MaterialLayout);
				if (s_State.MaterialDescriptorSets[slot])
				{
					std::vector<Rhi::DescriptorWrite> writes;
					for (uint32_t i = 0; i < 4; ++i)
					{
						Rhi::DescriptorWrite write;
						write.Binding = 4 + i;
						write.Type = Rhi::DescriptorType::CombinedImageSampler;
						write.Texture = s_State.DefaultTextures[i];
						write.Sampler = s_State.Sampler;
						writes.push_back(write);
					}
					s_State.MaterialDescriptorSets[slot]->Update(writes);
				}
			}
		}

		// 5. 创建管线
		EnsurePipeline(s_State.MainRenderPass);
		s_State.Initialized = true;
	}

	void TerrainRenderer::Shutdown()
	{
		WLD_TRACE_FUNCTION();
		ClearCache();
		s_State.Pipeline = nullptr;
		s_State.ShadowPipeline = nullptr;
		s_State.ObjectLayout = nullptr;
		s_State.MaterialLayout = nullptr;
		s_State.Sampler = nullptr;
		for (uint32_t i = 0; i < 4; ++i)
			s_State.DefaultTextures[i] = nullptr;
		for (uint32_t slot = 0; slot < Renderer::FramesInFlight; ++slot)
		{
			s_State.UniformBuffers[slot] = nullptr;
			s_State.ObjectDescriptorSets[slot] = nullptr;
			s_State.MaterialDescriptorSets[slot] = nullptr;
		}
		s_State.MainRenderPass = nullptr;
		s_State.CurrentCommandBuffer = nullptr;
		s_State.CurrentGlobalSet = nullptr;
		s_State.DataCache.clear();
		s_State.DefaultData = nullptr;
		s_State.Initialized = false;
	}

	void TerrainRenderer::EnsureInitialized()
	{
		if (!s_State.Initialized || !s_State.Pipeline)
			Init();
	}

	void TerrainRenderer::EnsurePipeline(const Rhi::Handle<Rhi::RenderPass>& renderPass)
	{
		const auto& device = Renderer::GetDevice();
		if (!device || !renderPass)
			return;

		if (s_State.Pipeline && s_State.MainRenderPass == renderPass)
			return;

		s_State.MainRenderPass = renderPass;

		// 顶点输入布局
		Rhi::VertexBinding binding;
		binding.Binding = 0;
		binding.Stride = sizeof(TerrainVertex);
		binding.PerInstance = false;

		Rhi::VertexAttribute attrPos;
		attrPos.Location = 0;
		attrPos.Binding = 0;
		attrPos.Format = Rhi::Format::R32G32B32_SFLOAT;
		attrPos.Offset = static_cast<uint32_t>(offsetof(TerrainVertex, Position));

		Rhi::VertexAttribute attrNorm;
		attrNorm.Location = 1;
		attrNorm.Binding = 0;
		attrNorm.Format = Rhi::Format::R32G32B32_SFLOAT;
		attrNorm.Offset = static_cast<uint32_t>(offsetof(TerrainVertex, Normal));

		Rhi::VertexAttribute attrUV;
		attrUV.Location = 2;
		attrUV.Binding = 0;
		attrUV.Format = Rhi::Format::R32G32_SFLOAT;
		attrUV.Offset = static_cast<uint32_t>(offsetof(TerrainVertex, TexCoord));

		Rhi::VertexAttribute attrSplat;
		attrSplat.Location = 3;
		attrSplat.Binding = 0;
		attrSplat.Format = Rhi::Format::R32G32B32A32_SFLOAT;
		attrSplat.Offset = static_cast<uint32_t>(offsetof(TerrainVertex, SplatWeights));

		Rhi::VertexAttribute attrHole;
		attrHole.Location = 4;
		attrHole.Binding = 0;
		attrHole.Format = Rhi::Format::R32_SFLOAT;
		attrHole.Offset = static_cast<uint32_t>(offsetof(TerrainVertex, Hole));

		const std::vector<Rhi::VertexAttribute> attributes = { attrPos, attrNorm, attrUV, attrSplat, attrHole };

		// 主渲染管线
		{
			Rhi::ShaderDesc shaderDesc;
			shaderDesc.DebugName = "Terrain-MainShader";
			shaderDesc.Stages.push_back(ShaderCompiler::CompileStage(Rhi::ShaderStage::Vertex,
				"assets/shaders/Terrain.slang", "VSMain", "vs_6_0"));
			shaderDesc.Stages.push_back(ShaderCompiler::CompileStage(Rhi::ShaderStage::Fragment,
				"assets/shaders/Terrain.slang", "PSMain", "ps_6_0"));
			auto shader = device->CreateShader(shaderDesc);

			if (shader)
			{
				Rhi::PipelineDesc pipeDesc;
				pipeDesc.Shader = shader;
				pipeDesc.RenderPass = renderPass;
				pipeDesc.DescriptorSetLayouts = { Renderer::GetGlobalDescriptorSetLayout(),
					s_State.ObjectLayout, s_State.MaterialLayout };
				pipeDesc.VertexBindings = { binding };
				pipeDesc.VertexAttributes = attributes;
				pipeDesc.Topology = Rhi::PrimitiveTopology::TriangleList;
				pipeDesc.Front = (Renderer::GetBackendName() == "vulkan")
					? Rhi::FrontFace::Clockwise : Rhi::FrontFace::CounterClockwise;
				pipeDesc.Cull = Rhi::CullMode::Back;
				pipeDesc.DepthStencil.DepthTest = true;
				pipeDesc.DepthStencil.DepthWrite = true;
				pipeDesc.DepthStencil.DepthCompare = Rhi::CompareOp::LessOrEqual;
				pipeDesc.Samples = static_cast<Rhi::SampleCount>(RenderSettings::Msaa());
				pipeDesc.DebugName = "Terrain.MainPipeline";
				s_State.Pipeline = device->CreatePipeline(pipeDesc);
			}
		}

		// 阴影渲染管线
		auto shadowPass = Renderer3D::GetShadowRenderPass();
		if (shadowPass)
		{
			Rhi::ShaderDesc shadowShaderDesc;
			shadowShaderDesc.DebugName = "Terrain-ShadowShader";
			shadowShaderDesc.Stages.push_back(ShaderCompiler::CompileStage(Rhi::ShaderStage::Vertex,
				"assets/shaders/Terrain.slang", "VSShadowMain", "vs_6_0"));
			shadowShaderDesc.Stages.push_back(ShaderCompiler::CompileStage(Rhi::ShaderStage::Fragment,
				"assets/shaders/Terrain.slang", "PSShadowMain", "ps_6_0"));
			auto shadowShader = device->CreateShader(shadowShaderDesc);

			if (shadowShader)
			{
				Rhi::PipelineDesc shadowPipeDesc;
				shadowPipeDesc.Shader = shadowShader;
				shadowPipeDesc.RenderPass = shadowPass;
				shadowPipeDesc.DescriptorSetLayouts = { Renderer::GetGlobalDescriptorSetLayout(), s_State.ObjectLayout };
				shadowPipeDesc.VertexBindings = { binding };
				shadowPipeDesc.VertexAttributes = attributes;
				shadowPipeDesc.Topology = Rhi::PrimitiveTopology::TriangleList;
				shadowPipeDesc.Cull = Rhi::CullMode::Front;
				shadowPipeDesc.DepthStencil.DepthTest = true;
				shadowPipeDesc.DepthStencil.DepthWrite = true;
				shadowPipeDesc.DepthStencil.DepthCompare = Rhi::CompareOp::LessOrEqual;
				shadowPipeDesc.Samples = Rhi::SampleCount::Count1;
				shadowPipeDesc.DebugName = "Terrain.ShadowPipeline";
				s_State.ShadowPipeline = device->CreatePipeline(shadowPipeDesc);
			}
		}
	}

	void TerrainRenderer::BeginScene(const glm::mat4& viewProjection,
		const Rhi::Handle<Rhi::CommandBuffer>& commandBuffer,
		const Rhi::Handle<Rhi::DescriptorSet>& globalDescriptorSet)
	{
		EnsureInitialized();
		s_State.CurrentViewProjection = viewProjection;
		s_State.CurrentCommandBuffer = commandBuffer;
		s_State.CurrentGlobalSet = globalDescriptorSet;
	}

	void TerrainRenderer::EndScene()
	{
		s_State.CurrentCommandBuffer = nullptr;
		s_State.CurrentGlobalSet = nullptr;
	}

	void TerrainRenderer::SetContext(const Rhi::Handle<Rhi::CommandBuffer>& cmdBuffer,
		const Rhi::Handle<Rhi::DescriptorSet>& globalDescriptorSet)
	{
		s_State.CurrentCommandBuffer = cmdBuffer;
		s_State.CurrentGlobalSet = globalDescriptorSet;
	}

	void TerrainRenderer::ClearCache()
	{
		s_State.ChunkCache.clear();
		s_State.LastDataPtr = nullptr;
	}

	Rhi::Handle<Rhi::Pipeline> TerrainRenderer::GetPipeline()
	{
		EnsureInitialized();
		return s_State.Pipeline;
	}

	Rhi::Handle<Rhi::Pipeline> TerrainRenderer::GetShadowPipeline()
	{
		EnsureInitialized();
		return s_State.ShadowPipeline;
	}

	Rhi::Handle<Rhi::DescriptorSetLayout> TerrainRenderer::GetObjectLayout()
	{
		EnsureInitialized();
		return s_State.ObjectLayout;
	}

	Rhi::Handle<Rhi::DescriptorSetLayout> TerrainRenderer::GetMaterialLayout()
	{
		EnsureInitialized();
		return s_State.MaterialLayout;
	}

	std::shared_ptr<TerrainData> TerrainRenderer::GetOrCreateTerrainData(const AssetRef& asset, float cellSize, float heightScale)
	{
		if (asset.HasPath())
		{
			const std::string& path = StringPool::Get().PathOf(asset.Path);
			if (!path.empty())
			{
				auto it = s_State.DataCache.find(path);
				if (it != s_State.DataCache.end())
					return it->second;

				auto data = std::make_shared<TerrainData>(256, 256, cellSize, heightScale);
				if (data->LoadFromFile(path))
				{
					s_State.DataCache[path] = data;
					return data;
				}
				s_State.DataCache[path] = data;
				return data;
			}
		}

		if (!s_State.DefaultData)
		{
			s_State.DefaultData = std::make_shared<TerrainData>(256, 256, cellSize, heightScale);
			// 赋予平缓的波浪高低起伏，使默认视口所见即为商业级起伏丘陵
			for (uint32_t z = 0; z < 256; ++z)
			{
				for (uint32_t x = 0; x < 256; ++x)
				{
					const float fx = static_cast<float>(x) * 0.05f;
					const float fz = static_cast<float>(z) * 0.05f;
					const float h = std::sin(fx) * std::cos(fz) * 6.0f + std::sin(fx * 0.5f) * 10.0f;
					s_State.DefaultData->SetHeight(x, z, h);

					const float grassWeight = std::clamp(1.0f - (h * 0.05f), 0.1f, 1.0f);
					const float dirtWeight = std::clamp(h * 0.05f, 0.0f, 0.9f);
					s_State.DefaultData->SetSplatWeights(x, z, glm::vec4(grassWeight, dirtWeight, 0.0f, 0.0f));
				}
			}
		}
		return s_State.DefaultData;
	}

	void TerrainRenderer::RenderTerrain(const TerrainData& data, const TerrainComponent& comp, const Camera& camera)
	{
		TerrainRenderSettings settings;
		settings.ChunkResolution = comp.ChunkResolution;
		settings.LodDistanceRatio = comp.LodDistanceRatio;
		settings.ReceiveShadow = (comp.ReceiveShadow != 0);
		settings.HoleCutoutEnabled = (comp.HoleCutoutEnabled != 0);
		settings.CastShadow = (comp.CastShadow != 0);

		const glm::mat4 viewProj = camera.GetProjectionMatrix();
		RenderTerrain(data, settings, viewProj, glm::vec3(0.0f, 50.0f, 50.0f));
	}

	void TerrainRenderer::RenderTerrain(const TerrainData& data, const TerrainRenderSettings& settings,
		const Camera& camera, const glm::mat4& cameraTransform)
	{
		const glm::mat4 view = glm::inverse(cameraTransform);
		const glm::mat4 viewProj = camera.GetProjectionMatrix() * view;
		const glm::vec3 camPos = glm::vec3(cameraTransform[3]);
		RenderTerrain(data, settings, viewProj, camPos);
	}

	void TerrainRenderer::RenderTerrain(const TerrainData& data, const TerrainRenderSettings& settings,
		const glm::mat4& viewProjection, const glm::vec3& cameraPosition)
	{
		WLD_TRACE_FUNCTION();
		EnsureInitialized();

		const auto& device = Renderer::GetDevice();
		if (!device || !s_State.Pipeline)
			return;

		auto cmd = s_State.CurrentCommandBuffer;
		if (!cmd)
			return;

		const uint32_t slot = static_cast<uint32_t>(Renderer::FrameSlot());

		// 1. 上传 TerrainUniforms
		TerrainUniforms uniforms;
		uniforms.Model = settings.Transform;
		for (uint32_t i = 0; i < 4; ++i)
		{
			const TerrainLayer& layer = data.GetLayer(i);
			if (i == 0) { uniforms.LayerTiling01.x = layer.Tiling.x; uniforms.LayerTiling01.y = layer.Tiling.y; uniforms.LayerRoughnessMetallic.x = layer.Roughness; }
			else if (i == 1) { uniforms.LayerTiling01.z = layer.Tiling.x; uniforms.LayerTiling01.w = layer.Tiling.y; uniforms.LayerRoughnessMetallic.y = layer.Roughness; }
			else if (i == 2) { uniforms.LayerTiling23.x = layer.Tiling.x; uniforms.LayerTiling23.y = layer.Tiling.y; uniforms.LayerRoughnessMetallic.z = layer.Roughness; }
			else if (i == 3) { uniforms.LayerTiling23.z = layer.Tiling.x; uniforms.LayerTiling23.w = layer.Tiling.y; uniforms.LayerRoughnessMetallic.w = layer.Roughness; }
		}
		uniforms.SlopeParams = glm::vec4(settings.SlopeThreshold, settings.SlopeBlendRange, 2.0f, settings.HoleCutoutEnabled ? 1.0f : 0.0f);
		uniforms.TerrainParams = glm::vec4(settings.ReceiveShadow ? 1.0f : 0.0f, static_cast<float>(settings.EntityId), 0.0f, 0.0f);

		if (s_State.UniformBuffers[slot])
			s_State.UniformBuffers[slot]->SetData(&uniforms, sizeof(uniforms));

		// 2. 更新材质描述符集 (检查 Layer 纹理)
		if (s_State.MaterialDescriptorSets[slot])
		{
			std::vector<Rhi::DescriptorWrite> writes;
			for (uint32_t i = 0; i < 4; ++i)
			{
				const TerrainLayer& layer = data.GetLayer(i);
				Rhi::Handle<Rhi::Texture> tex = nullptr;
				if (layer.AlbedoTexture.HasPath())
				{
					const std::string& path = StringPool::Get().PathOf(layer.AlbedoTexture.Path);
					if (!path.empty())
						tex = TextureLibrary::Get().Get(path, /*srgb*/ true);
				}
				if (!tex)
					tex = s_State.DefaultTextures[i];

				Rhi::DescriptorWrite write;
				write.Binding = 4 + i;
				write.Type = Rhi::DescriptorType::CombinedImageSampler;
				write.Texture = tex;
				write.Sampler = s_State.Sampler;
				writes.push_back(write);
			}
			s_State.MaterialDescriptorSets[slot]->Update(writes);
		}

		// 3. 绑定管线与描述符集
		cmd->BindPipeline(s_State.Pipeline);

		auto globalSet = s_State.CurrentGlobalSet;
		if (globalSet)
			cmd->BindDescriptorSet(globalSet);

		if (s_State.ObjectDescriptorSets[slot])
			cmd->BindDescriptorSet(s_State.ObjectDescriptorSets[slot]);

		if (s_State.MaterialDescriptorSets[slot])
			cmd->BindDescriptorSet(s_State.MaterialDescriptorSets[slot]);

		// 4. Chunk CDLOD 分块生成与提交
		if (s_State.LastDataPtr != &data || data.IsDirty())
		{
			s_State.ChunkCache.clear();
			s_State.LastDataPtr = &data;
		}

		const uint32_t chunkResolution = std::max(16u, settings.ChunkResolution);
		const uint32_t numChunksX = data.GetChunkCountX(chunkResolution);
		const uint32_t numChunksZ = data.GetChunkCountZ(chunkResolution);

		const float worldChunkSizeX = (data.GetWidth() > 1 && numChunksX > 0)
			? (data.GetWorldWidth() / static_cast<float>(numChunksX)) : 64.0f;
		const float worldChunkSizeZ = (data.GetHeight() > 1 && numChunksZ > 0)
			? (data.GetWorldHeight() / static_cast<float>(numChunksZ)) : 64.0f;
		const float chunkDiagonal = std::sqrt(worldChunkSizeX * worldChunkSizeX + worldChunkSizeZ * worldChunkSizeZ);
		const float lodBaseDist = chunkDiagonal * std::max(0.5f, settings.LodDistanceRatio);

		for (uint32_t cz = 0; cz < numChunksZ; ++cz)
		{
			for (uint32_t cx = 0; cx < numChunksX; ++cx)
			{
				const uint32_t chunkKey = (cx << 16) | cz;

				// 计算 Chunk 世界空间中心
				const float minX = static_cast<float>(cx * (chunkResolution - 1)) * data.GetCellSize();
				const float minZ = static_cast<float>(cz * (chunkResolution - 1)) * data.GetCellSize();
				const glm::vec3 localCenter(minX + worldChunkSizeX * 0.5f, 0.0f, minZ + worldChunkSizeZ * 0.5f);
				const glm::vec3 worldCenter = glm::vec3(settings.Transform * glm::vec4(localCenter, 1.0f));

				const float distanceToCam = glm::distance(worldCenter, cameraPosition);

				// 动态 CDLOD 计算
				uint32_t targetLod = 0;
				if (distanceToCam > lodBaseDist * 4.0f)
					targetLod = 3;
				else if (distanceToCam > lodBaseDist * 2.0f)
					targetLod = 2;
				else if (distanceToCam > lodBaseDist)
					targetLod = 1;

				auto& chunkMesh = s_State.ChunkCache[chunkKey];
				if (!chunkMesh.VertexBuffer || !chunkMesh.IndexBuffer || chunkMesh.Lod != targetLod)
				{
					TerrainChunk chunkData;
					data.GenerateChunkMesh(cx, cz, chunkResolution, targetLod, chunkData);

					if (chunkData.Vertices.empty() || chunkData.Indices.empty())
						continue;

					chunkMesh.Lod = targetLod;
					chunkMesh.IndexCount = static_cast<uint32_t>(chunkData.Indices.size());

					// 顶点缓冲
					Rhi::BufferDesc vbDesc;
					vbDesc.Size = chunkData.Vertices.size() * sizeof(TerrainVertex);
					vbDesc.Usage = Rhi::BufferUsageVertex;
					vbDesc.Memory = Rhi::MemoryHint::DeviceLocal;
					vbDesc.InitialData = chunkData.Vertices.data();
					vbDesc.DebugName = "Terrain.ChunkVB." + std::to_string(cx) + "_" + std::to_string(cz);
					chunkMesh.VertexBuffer = device->CreateBuffer(vbDesc);

					// 索引缓冲
					Rhi::BufferDesc ibDesc;
					ibDesc.Size = chunkData.Indices.size() * sizeof(uint32_t);
					ibDesc.Usage = Rhi::BufferUsageIndex;
					ibDesc.Memory = Rhi::MemoryHint::DeviceLocal;
					ibDesc.InitialData = chunkData.Indices.data();
					ibDesc.DebugName = "Terrain.ChunkIB." + std::to_string(cx) + "_" + std::to_string(cz);
					chunkMesh.IndexBuffer = device->CreateBuffer(ibDesc);
				}

				if (chunkMesh.VertexBuffer && chunkMesh.IndexBuffer && chunkMesh.IndexCount > 0)
				{
					cmd->BindVertexBuffer(0, chunkMesh.VertexBuffer, 0);
					cmd->BindIndexBuffer(chunkMesh.IndexBuffer, 0, Rhi::IndexType::UInt32);
					cmd->DrawIndexed(chunkMesh.IndexCount, 1, 0, 0, 0);
				}
			}
		}
	}

	void TerrainRenderer::RenderTerrainShadow(const TerrainData& data, const TerrainRenderSettings& settings)
	{
		WLD_TRACE_FUNCTION();
		EnsureInitialized();

		const auto& device = Renderer::GetDevice();
		if (!device || !s_State.ShadowPipeline)
			return;

		auto cmd = s_State.CurrentCommandBuffer;
		if (!cmd)
			return;

		const uint32_t slot = static_cast<uint32_t>(Renderer::FrameSlot());

		// 1. 上传 TerrainUniforms
		TerrainUniforms uniforms;
		uniforms.Model = settings.Transform;
		uniforms.SlopeParams = glm::vec4(settings.SlopeThreshold, settings.SlopeBlendRange, 2.0f, settings.HoleCutoutEnabled ? 1.0f : 0.0f);
		uniforms.TerrainParams = glm::vec4(settings.ReceiveShadow ? 1.0f : 0.0f, static_cast<float>(settings.EntityId), 0.0f, 0.0f);

		if (s_State.UniformBuffers[slot])
			s_State.UniformBuffers[slot]->SetData(&uniforms, sizeof(uniforms));

		cmd->BindPipeline(s_State.ShadowPipeline);

		auto globalSet = s_State.CurrentGlobalSet;
		if (globalSet)
			cmd->BindDescriptorSet(globalSet);

		if (s_State.ObjectDescriptorSets[slot])
			cmd->BindDescriptorSet(s_State.ObjectDescriptorSets[slot]);

		const uint32_t chunkResolution = std::max(16u, settings.ChunkResolution);
		const uint32_t numChunksX = data.GetChunkCountX(chunkResolution);
		const uint32_t numChunksZ = data.GetChunkCountZ(chunkResolution);

		for (uint32_t cz = 0; cz < numChunksZ; ++cz)
		{
			for (uint32_t cx = 0; cx < numChunksX; ++cx)
			{
				const uint32_t chunkKey = (cx << 16) | cz;
				auto it = s_State.ChunkCache.find(chunkKey);
				if (it != s_State.ChunkCache.end() && it->second.VertexBuffer && it->second.IndexBuffer)
				{
					cmd->BindVertexBuffer(0, it->second.VertexBuffer, 0);
					cmd->BindIndexBuffer(it->second.IndexBuffer, 0, Rhi::IndexType::UInt32);
					cmd->DrawIndexed(it->second.IndexCount, 1, 0, 0, 0);
				}
			}
		}
	}
}
