#include "wldpch.h"
#include "World/Terrain/TerrainRenderer.h"
#include "World/Renderer/Renderer.h"
#include "World/Renderer/Renderer3D.h"
#include "World/Renderer/RenderSettings.h"
#include "World/Renderer/ShaderUtils.h"
#include "World/Renderer/Texture/TextureLibrary.h"
#include "World/Core/StringPool.h"
#include "World/Scene/Components.h"
#include "World/Utils/Paths.h"

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
			Rhi::Handle<Rhi::Texture> DefaultNormalTextures[4];

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
			for (uint32_t i = 0; i < 4; ++i)
			{
				matDesc.Bindings.push_back({ 8 + i, Rhi::DescriptorType::CombinedImageSampler,
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

		// 3. 创建 4 层工业级 128x128 程序化微噪波纹理 (草地、泥土、岩石、沙地)
		constexpr uint32_t kNoiseTexDim = 128;
		std::vector<uint8_t> noiseBuffer(kNoiseTexDim * kNoiseTexDim * 4);

		for (uint32_t i = 0; i < 4; ++i)
		{
			if (!s_State.DefaultTextures[i])
			{
				for (uint32_t y = 0; y < kNoiseTexDim; ++y)
				{
					for (uint32_t x = 0; x < kNoiseTexDim; ++x)
					{
						const float fx = static_cast<float>(x);
						const float fy = static_cast<float>(y);
						const float n1 = std::sin(fx * 0.15f) * std::cos(fy * 0.15f);
						const float n2 = std::sin(fx * 0.35f + fy * 0.25f) * 0.5f;
						const float n3 = std::cos(fx * 0.05f + fy * 0.45f) * 0.3f;
						const float variation = (n1 + n2 + n3) * 22.0f;

						uint8_t r = 0, g = 0, b = 0;
						if (i == 0) // 草地
						{
							r = static_cast<uint8_t>(std::clamp(55.0f + variation * 0.8f, 0.0f, 255.0f));
							g = static_cast<uint8_t>(std::clamp(135.0f + variation * 1.2f, 0.0f, 255.0f));
							b = static_cast<uint8_t>(std::clamp(42.0f + variation * 0.5f, 0.0f, 255.0f));
						}
						else if (i == 1) // 泥土
						{
							r = static_cast<uint8_t>(std::clamp(88.0f + variation * 1.0f, 0.0f, 255.0f));
							g = static_cast<uint8_t>(std::clamp(62.0f + variation * 0.8f, 0.0f, 255.0f));
							b = static_cast<uint8_t>(std::clamp(46.0f + variation * 0.6f, 0.0f, 255.0f));
						}
						else if (i == 2) // 岩石
						{
							const float strata = std::sin(fx * 0.4f + fy * 0.8f) * 25.0f;
							r = static_cast<uint8_t>(std::clamp(115.0f + variation + strata, 0.0f, 255.0f));
							g = static_cast<uint8_t>(std::clamp(120.0f + variation + strata, 0.0f, 255.0f));
							b = static_cast<uint8_t>(std::clamp(125.0f + variation + strata, 0.0f, 255.0f));
						}
						else // 沙地
						{
							const float ripple = std::sin(fx * 0.3f + fy * 0.1f) * 15.0f;
							r = static_cast<uint8_t>(std::clamp(215.0f + variation * 0.6f + ripple, 0.0f, 255.0f));
							g = static_cast<uint8_t>(std::clamp(195.0f + variation * 0.5f + ripple, 0.0f, 255.0f));
							b = static_cast<uint8_t>(std::clamp(145.0f + variation * 0.4f + ripple, 0.0f, 255.0f));
						}

						const size_t idx = (y * kNoiseTexDim + x) * 4;
						noiseBuffer[idx + 0] = r;
						noiseBuffer[idx + 1] = g;
						noiseBuffer[idx + 2] = b;
						noiseBuffer[idx + 3] = 0xFF;
					}
				}

				Rhi::TextureDesc texDesc;
				texDesc.Type = Rhi::TextureType::Texture2D;
				texDesc.Format = Rhi::Format::R8G8B8A8_UNORM;
				texDesc.Extent = { kNoiseTexDim, kNoiseTexDim, 1 };
				texDesc.Usage = Rhi::TextureUsageSampled;
				texDesc.DebugName = "Terrain.DefaultNoiseTexture" + std::to_string(i);
				s_State.DefaultTextures[i] = device->CreateTexture(texDesc);
				if (s_State.DefaultTextures[i])
					s_State.DefaultTextures[i]->SetData(noiseBuffer.data(), noiseBuffer.size());
			}
		
		// 3b. 默认平坦法线贴图 (128, 128, 255)
		std::vector<uint8_t> defaultFlatNormal(16 * 16 * 4);
		for (size_t px = 0; px < 16 * 16; ++px)
		{
			defaultFlatNormal[px * 4 + 0] = 128;
			defaultFlatNormal[px * 4 + 1] = 128;
			defaultFlatNormal[px * 4 + 2] = 255;
			defaultFlatNormal[px * 4 + 3] = 255;
		}
		for (uint32_t i = 0; i < 4; ++i)
		{
			if (!s_State.DefaultNormalTextures[i])
			{
				Rhi::TextureDesc normDesc;
				normDesc.Type = Rhi::TextureType::Texture2D;
				normDesc.Format = Rhi::Format::R8G8B8A8_UNORM;
				normDesc.Extent = { 16, 16, 1 };
				normDesc.Usage = Rhi::TextureUsageSampled;
				normDesc.DebugName = "Terrain.DefaultNormal" + std::to_string(i);
				s_State.DefaultNormalTextures[i] = device->CreateTexture(normDesc);
				if (s_State.DefaultNormalTextures[i])
					s_State.DefaultNormalTextures[i]->SetData(defaultFlatNormal.data(), defaultFlatNormal.size());
			}
		}}

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
					for (uint32_t i = 0; i < 4; ++i)
					{
						Rhi::DescriptorWrite write;
						write.Binding = 8 + i;
						write.Type = Rhi::DescriptorType::CombinedImageSampler;
						write.Texture = s_State.DefaultNormalTextures[i];
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
		{
			s_State.DefaultTextures[i] = nullptr;
			s_State.DefaultNormalTextures[i] = nullptr;
		}
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
		if (s_State.ChunkCache.empty())
			return;

		std::vector<ChunkGpuMesh> stale;
		stale.reserve(s_State.ChunkCache.size());
		for (auto& [key, mesh] : s_State.ChunkCache)
		{
			stale.push_back(std::move(mesh));
		}
		s_State.ChunkCache.clear();
		s_State.LastDataPtr = nullptr;

		Renderer::QueueRelease([stale = std::move(stale)]() mutable
		{
			stale.clear();
		});
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

				std::filesystem::path resolvedPath = path;
				if (!std::filesystem::exists(resolvedPath))
				{
					resolvedPath = World::Paths::AssetRoot() / path;
					if (!std::filesystem::exists(resolvedPath))
						resolvedPath = World::Paths::ProjectFile(path);
				}

				auto data = std::make_shared<TerrainData>(256, 256, cellSize, heightScale);
				if (data->LoadFromFile(resolvedPath.string()))
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
			s_State.DefaultData->GenerateProceduralLandscape(2026);
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
		uniforms.CameraPosition = glm::vec4(cameraPosition, 1.0f);

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
				{
					const char* const defaultRelPaths[4] = {
						"textures/terrain/grass_albedo.png",
						"textures/terrain/dirt_albedo.png",
						"textures/terrain/rock_albedo.png",
						"textures/terrain/sand_albedo.png"
					};
					tex = TextureLibrary::Get().Get(defaultRelPaths[i], /*srgb*/ true);
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
			
			for (uint32_t i = 0; i < 4; ++i)
			{
				const TerrainLayer& layer = data.GetLayer(i);
				Rhi::Handle<Rhi::Texture> normTex = nullptr;
				if (layer.NormalTexture.HasPath())
				{
					const std::string& path = StringPool::Get().PathOf(layer.NormalTexture.Path);
					if (!path.empty())
						normTex = TextureLibrary::Get().Get(path, /*srgb*/ false);
				}
				if (!normTex)
				{
					const char* const defaultNormalPaths[4] = {
						"textures/terrain/grass_normal.png",
						"textures/terrain/dirt_normal.png",
						"textures/terrain/rock_normal.png",
						"textures/terrain/sand_normal.png"
					};
					normTex = TextureLibrary::Get().Get(defaultNormalPaths[i], /*srgb*/ false);
				}
				if (!normTex)
					normTex = s_State.DefaultNormalTextures[i];

				Rhi::DescriptorWrite write;
				write.Binding = 8 + i;
				write.Type = Rhi::DescriptorType::CombinedImageSampler;
				write.Texture = normTex;
				write.Sampler = s_State.Sampler;
				writes.push_back(write);
			}
s_State.MaterialDescriptorSets[slot]->Update(writes);
		}

		// 3. 绑定管线与描述符集
		cmd->BindPipeline(s_State.Pipeline);

		auto globalSet = s_State.CurrentGlobalSet;
		if (globalSet)
			cmd->BindDescriptorSet(globalSet, 0);

		if (s_State.ObjectDescriptorSets[slot])
			cmd->BindDescriptorSet(s_State.ObjectDescriptorSets[slot], 1);

		if (s_State.MaterialDescriptorSets[slot])
			cmd->BindDescriptorSet(s_State.MaterialDescriptorSets[slot], 2);

		// 4. Chunk CDLOD 分块生成与提交
		if (s_State.LastDataPtr != &data || data.IsDirty())
		{
			ClearCache();
			s_State.LastDataPtr = &data;
			const_cast<TerrainData&>(data).ClearDirty();
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

				const uint32_t chunkKey = (targetLod << 28) | ((cx & 0x3FFFu) << 14) | (cz & 0x3FFFu);
				auto& chunkMesh = s_State.ChunkCache[chunkKey];
				if (!chunkMesh.VertexBuffer || !chunkMesh.IndexBuffer || chunkMesh.Lod != targetLod)
				{
					if (chunkMesh.VertexBuffer || chunkMesh.IndexBuffer)
					{
						auto oldVB = std::move(chunkMesh.VertexBuffer);
						auto oldIB = std::move(chunkMesh.IndexBuffer);
						Renderer::QueueRelease([oldVB = std::move(oldVB), oldIB = std::move(oldIB)]() mutable
						{
							oldVB = nullptr;
							oldIB = nullptr;
						});
					}

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

	void TerrainRenderer::RenderTerrainShadow(const TerrainData& data, const TerrainRenderSettings& settings, uint32_t cascadeIndex)
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
		uniforms.TerrainParams = glm::vec4(settings.ReceiveShadow ? 1.0f : 0.0f, static_cast<float>(settings.EntityId), static_cast<float>(cascadeIndex), 0.0f);
		uniforms.CameraPosition = glm::vec4(0.0f, 0.0f, 0.0f, 1.0f);

		if (s_State.UniformBuffers[slot])
			s_State.UniformBuffers[slot]->SetData(&uniforms, sizeof(uniforms));

		cmd->BindPipeline(s_State.ShadowPipeline);

		auto globalSet = s_State.CurrentGlobalSet;
		if (globalSet)
			cmd->BindDescriptorSet(globalSet, 0);

		if (s_State.ObjectDescriptorSets[slot])
			cmd->BindDescriptorSet(s_State.ObjectDescriptorSets[slot], 1);

		const uint32_t chunkResolution = std::max(16u, settings.ChunkResolution);
		const uint32_t numChunksX = data.GetChunkCountX(chunkResolution);
		const uint32_t numChunksZ = data.GetChunkCountZ(chunkResolution);

		for (uint32_t cz = 0; cz < numChunksZ; ++cz)
		{
			for (uint32_t cx = 0; cx < numChunksX; ++cx)
			{
				for (uint32_t lod = 0; lod < 4; ++lod)
				{
					const uint32_t chunkKey = (lod << 28) | ((cx & 0x3FFFu) << 14) | (cz & 0x3FFFu);
					auto it = s_State.ChunkCache.find(chunkKey);
					if (it != s_State.ChunkCache.end() && it->second.VertexBuffer && it->second.IndexBuffer)
					{
						cmd->BindVertexBuffer(0, it->second.VertexBuffer, 0);
						cmd->BindIndexBuffer(it->second.IndexBuffer, 0, Rhi::IndexType::UInt32);
						cmd->DrawIndexed(it->second.IndexCount, 1, 0, 0, 0);
						break;
					}
				}
			}
		}
	}
}
