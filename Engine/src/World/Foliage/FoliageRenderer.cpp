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
			// 帧槽位环形化缓冲与描述符集（消除 CPU 写入与 GPU 读取在飞帧间的数据竞态/闪烁）
			Rhi::Handle<Rhi::DescriptorSet> UniformSets[Renderer::FramesInFlight];
			Rhi::Handle<Rhi::DescriptorSet> TextureSets[Renderer::FramesInFlight];
			Rhi::Handle<Rhi::Texture> BoundTextures[Renderer::FramesInFlight];
			Rhi::Handle<Rhi::Buffer> UniformBuffers[Renderer::FramesInFlight];
			Rhi::Handle<Rhi::Buffer> InstanceBuffers[Renderer::FramesInFlight];
			uint32_t InstanceCapacities[Renderer::FramesInFlight] = {};

			Rhi::Handle<Rhi::Texture> DefaultAlbedo;
			Rhi::Handle<Rhi::Sampler> Sampler;

			Rhi::Handle<Rhi::Buffer> DefaultMeshVB;
			Rhi::Handle<Rhi::Buffer> DefaultMeshIB;
			uint32_t DefaultMeshIndexCount { 0 };

			Rhi::Handle<Rhi::Buffer> RockMeshVB;
			Rhi::Handle<Rhi::Buffer> RockMeshIB;
			uint32_t RockMeshIndexCount { 0 };

			Rhi::Handle<Rhi::Buffer> BushMeshVB;
			Rhi::Handle<Rhi::Buffer> BushMeshIB;
			uint32_t BushMeshIndexCount { 0 };

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

			for (uint32_t slot = 0; slot < Renderer::FramesInFlight; ++slot)
			{
				Rhi::BufferDesc ubDesc;
				ubDesc.Size = sizeof(FoliageShaderUniforms);
				ubDesc.Usage = Rhi::BufferUsageUniform;
				ubDesc.Memory = Rhi::MemoryHint::HostVisible;
				ubDesc.DebugName = "Foliage.UniformBuffer";
				s_State.UniformBuffers[slot] = device->CreateBuffer(ubDesc);

				if (s_State.UniformLayout && s_State.UniformBuffers[slot])
				{
					s_State.UniformSets[slot] = device->CreateDescriptorSet(s_State.UniformLayout);
					if (s_State.UniformSets[slot])
					{
						Rhi::DescriptorWrite w;
						w.Binding = 1;
						w.Type = Rhi::DescriptorType::UniformBuffer;
						w.Buffer = s_State.UniformBuffers[slot];
						w.BufferOffset = 0;
						w.BufferRange = sizeof(FoliageShaderUniforms);
						s_State.UniformSets[slot]->Update({ w });
					}
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
			sDesc.MipmapMode = Rhi::SamplerMipmapMode::Linear;
			sDesc.AddressU = Rhi::SamplerAddressMode::ClampToEdge;
			sDesc.AddressV = Rhi::SamplerAddressMode::ClampToEdge;
			sDesc.AddressW = Rhi::SamplerAddressMode::ClampToEdge;
			sDesc.MaxAnisotropy = 4.0f;
			sDesc.DebugName = "Foliage.Sampler";
			s_State.Sampler = device->CreateSampler(sDesc);

			Rhi::Handle<Rhi::Texture> tex = TextureLibrary::Get().Get("textures/foliage/grass_tuft.png", /*srgb*/ true);
			if (tex)
			{
				s_State.DefaultAlbedo = tex;
			}
			else
			{
				constexpr uint32_t kFoliageTexDim = 512;
				std::vector<uint8_t> foliageBuffer(kFoliageTexDim * kFoliageTexDim * 4, 0);

				for (int b = 0; b < 32; ++b)
				{
					const float baseX = 256.0f + (b - 15.5f) * 12.0f;
					const float bend = (b - 15.5f) * 8.5f;
					for (uint32_t y = 10; y < kFoliageTexDim - 5; ++y)
					{
						const float t = 1.0f - static_cast<float>(y) / static_cast<float>(kFoliageTexDim);
						const float cx = baseX + bend * (t * t);
						const float halfW = std::max(1.5f, (1.0f - std::pow(t, 0.72f)) * 12.0f);

						for (int dx = -static_cast<int>(halfW); dx <= static_cast<int>(halfW); ++dx)
						{
							const int px = static_cast<int>(cx) + dx;
							if (px >= 0 && px < static_cast<int>(kFoliageTexDim))
							{
								const size_t idx = (y * kFoliageTexDim + px) * 4;
								const float u = static_cast<float>(dx) / halfW;
								const float edgeFade = std::min(1.0f, (halfW - std::abs(static_cast<float>(dx))) * 1.5f);
								const float sideLit = 0.9f + 0.18f * u;
								foliageBuffer[idx + 0] = static_cast<uint8_t>(std::clamp((30.0f + t * 110.0f) * sideLit, 0.0f, 255.0f));
								foliageBuffer[idx + 1] = static_cast<uint8_t>(std::clamp((65.0f + t * 135.0f) * sideLit, 0.0f, 255.0f));
								foliageBuffer[idx + 2] = static_cast<uint8_t>(std::clamp((22.0f + t * 35.0f) * sideLit, 0.0f, 255.0f));
								foliageBuffer[idx + 3] = static_cast<uint8_t>(255.0f * edgeFade);
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
				for (uint32_t slot = 0; slot < Renderer::FramesInFlight; ++slot)
				{
					s_State.TextureSets[slot] = device->CreateDescriptorSet(s_State.TextureLayout);
					if (s_State.TextureSets[slot])
					{
						Rhi::DescriptorWrite w;
						w.Binding = 4;
						w.Type = Rhi::DescriptorType::CombinedImageSampler;
						w.Texture = s_State.DefaultAlbedo;
						w.Sampler = s_State.Sampler;
						s_State.TextureSets[slot]->Update({ w });
					}
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

		// 3b. 构造真实 3D 碎石几何网格 (Pebble Boulder: 14 vertices, 24 triangles)
		{
			FoliageVertex rockVerts[14] = {
				{ { -0.35f, 0.00f, -0.25f }, { -0.4f, -0.6f, -0.3f }, { 0.2f, 0.2f } },
				{ {  0.00f, 0.00f, -0.42f }, {  0.0f, -0.7f, -0.5f }, { 0.5f, 0.1f } },
				{ {  0.38f, 0.00f, -0.20f }, {  0.5f, -0.6f, -0.3f }, { 0.8f, 0.2f } },
				{ {  0.35f, 0.00f,  0.25f }, {  0.5f, -0.6f,  0.4f }, { 0.8f, 0.8f } },
				{ { -0.05f, 0.00f,  0.40f }, { -0.1f, -0.7f,  0.5f }, { 0.5f, 0.9f } },
				{ { -0.38f, 0.00f,  0.18f }, { -0.5f, -0.6f,  0.3f }, { 0.2f, 0.8f } },

				{ { -0.48f, 0.20f, -0.30f }, { -0.8f,  0.3f, -0.4f }, { 0.1f, 0.3f } },
				{ {  0.05f, 0.24f, -0.52f }, {  0.1f,  0.4f, -0.8f }, { 0.5f, 0.0f } },
				{ {  0.50f, 0.22f, -0.22f }, {  0.8f,  0.3f, -0.3f }, { 0.9f, 0.3f } },
				{ {  0.45f, 0.18f,  0.30f }, {  0.8f,  0.2f,  0.5f }, { 0.9f, 0.7f } },
				{ { -0.08f, 0.22f,  0.48f }, { -0.1f,  0.3f,  0.8f }, { 0.5f, 1.0f } },
				{ { -0.50f, 0.20f,  0.22f }, { -0.8f,  0.3f,  0.3f }, { 0.1f, 0.7f } },

				{ { -0.12f, 0.38f, -0.08f }, { -0.2f,  0.9f, -0.1f }, { 0.4f, 0.45f } },
				{ {  0.14f, 0.36f,  0.10f }, {  0.2f,  0.9f,  0.2f }, { 0.6f, 0.55f } },
			};
			for (int vi = 0; vi < 14; ++vi) {
				rockVerts[vi].Normal = glm::normalize(rockVerts[vi].Normal);
			}
			const uint32_t rockIndices[72] = {
				0, 1, 7,   0, 7, 6,
				1, 2, 8,   1, 8, 7,
				2, 3, 9,   2, 9, 8,
				3, 4, 10,  3, 10, 9,
				4, 5, 11,  4, 11, 10,
				5, 0, 6,   5, 6, 11,
				6, 7, 12,  7, 8, 12,  8, 13, 12,
				8, 9, 13,  9, 10, 13, 10, 11, 13,
				11, 6, 12, 11, 12, 13,
				12, 13, 7, 13, 12, 10
			};
			Rhi::BufferDesc rVbDesc;
			rVbDesc.Size = sizeof(rockVerts);
			rVbDesc.Usage = Rhi::BufferUsageVertex;
			rVbDesc.Memory = Rhi::MemoryHint::DeviceLocal;
			rVbDesc.InitialData = rockVerts;
			rVbDesc.DebugName = "Foliage.RockMeshVB";
			s_State.RockMeshVB = device->CreateBuffer(rVbDesc);

			Rhi::BufferDesc rIbDesc;
			rIbDesc.Size = sizeof(rockIndices);
			rIbDesc.Usage = Rhi::BufferUsageIndex;
			rIbDesc.Memory = Rhi::MemoryHint::DeviceLocal;
			rIbDesc.InitialData = rockIndices;
			rIbDesc.DebugName = "Foliage.RockMeshIB";
			s_State.RockMeshIB = device->CreateBuffer(rIbDesc);
			s_State.RockMeshIndexCount = 72;
		}

		// 3c. 构造真实半球立体灌木叶簇网格 (Hemispherical Bush Dome: 5 quads, 20 vertices, 30 indices)
		{
			FoliageVertex bushVerts[20];
			uint32_t bushIndices[30];
			constexpr float kPi = 3.1415926535f;

			for (int q = 0; q < 4; ++q)
			{
				const float a = static_cast<float>(q) * (kPi * 0.5f) + 0.39f;
				const float cosA = std::cos(a);
				const float sinA = std::sin(a);
				const glm::vec3 outward(cosA, 0.35f, sinA);

				const int bv = q * 4;
				const glm::vec3 centerBottom = glm::vec3(cosA * 0.25f, 0.05f, sinA * 0.25f);
				const glm::vec3 rightDir(-sinA * 0.65f, 0.0f, cosA * 0.65f);
				const glm::vec3 upDir(cosA * 0.45f, 0.95f, sinA * 0.45f);

				bushVerts[bv + 0] = { centerBottom - rightDir, glm::normalize(outward + glm::vec3(0, 0.5f, 0)), { 0.0f, 1.0f } };
				bushVerts[bv + 1] = { centerBottom + rightDir, glm::normalize(outward + glm::vec3(0, 0.5f, 0)), { 1.0f, 1.0f } };
				bushVerts[bv + 2] = { centerBottom + rightDir + upDir, glm::normalize(outward + glm::vec3(0, 0.8f, 0)), { 1.0f, 0.0f } };
				bushVerts[bv + 3] = { centerBottom - rightDir + upDir, glm::normalize(outward + glm::vec3(0, 0.8f, 0)), { 0.0f, 0.0f } };

				const int bi = q * 6;
				bushIndices[bi + 0] = bv + 0;
				bushIndices[bi + 1] = bv + 1;
				bushIndices[bi + 2] = bv + 2;
				bushIndices[bi + 3] = bv + 0;
				bushIndices[bi + 4] = bv + 2;
				bushIndices[bi + 5] = bv + 3;
			}
			bushVerts[16] = { { -0.6f, 1.05f, -0.6f }, { 0.0f, 1.0f, 0.0f }, { 0.0f, 0.0f } };
			bushVerts[17] = { {  0.6f, 1.05f, -0.6f }, { 0.0f, 1.0f, 0.0f }, { 1.0f, 0.0f } };
			bushVerts[18] = { {  0.6f, 1.05f,  0.6f }, { 0.0f, 1.0f, 0.0f }, { 1.0f, 1.0f } };
			bushVerts[19] = { { -0.6f, 1.05f,  0.6f }, { 0.0f, 1.0f, 0.0f }, { 0.0f, 1.0f } };

			bushIndices[24] = 16; bushIndices[25] = 17; bushIndices[26] = 18;
			bushIndices[27] = 16; bushIndices[28] = 18; bushIndices[29] = 19;

			Rhi::BufferDesc bVbDesc;
			bVbDesc.Size = sizeof(bushVerts);
			bVbDesc.Usage = Rhi::BufferUsageVertex;
			bVbDesc.Memory = Rhi::MemoryHint::DeviceLocal;
			bVbDesc.InitialData = bushVerts;
			bVbDesc.DebugName = "Foliage.BushMeshVB";
			s_State.BushMeshVB = device->CreateBuffer(bVbDesc);

			Rhi::BufferDesc bIbDesc;
			bIbDesc.Size = sizeof(bushIndices);
			bIbDesc.Usage = Rhi::BufferUsageIndex;
			bIbDesc.Memory = Rhi::MemoryHint::DeviceLocal;
			bIbDesc.InitialData = bushIndices;
			bIbDesc.DebugName = "Foliage.BushMeshIB";
			s_State.BushMeshIB = device->CreateBuffer(bIbDesc);
			s_State.BushMeshIndexCount = 30;
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
				pDesc.AlphaToCoverage = true;
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
		for (uint32_t slot = 0; slot < Renderer::FramesInFlight; ++slot)
		{
			s_State.InstanceBuffers[slot] = nullptr;
			s_State.InstanceCapacities[slot] = 0;
			s_State.UniformBuffers[slot] = nullptr;
			s_State.UniformSets[slot] = nullptr;
			s_State.TextureSets[slot] = nullptr;
			s_State.BoundTextures[slot] = nullptr;
		}
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

		const uint32_t slot = Renderer::FrameSlot();
		const uint32_t neededSize = static_cast<uint32_t>(visibleInstances.size() * sizeof(FoliageInstanceData));
		if (!s_State.InstanceBuffers[slot] || s_State.InstanceCapacities[slot] < neededSize)
		{
			if (s_State.InstanceBuffers[slot])
			{
				auto oldBuf = std::move(s_State.InstanceBuffers[slot]);
				Renderer::QueueRelease([oldBuf = std::move(oldBuf)]() mutable {});
			}
			Rhi::BufferDesc bDesc;
			bDesc.Size = std::max(neededSize, 65536u);
			bDesc.Usage = Rhi::BufferUsageVertex;
			bDesc.Memory = Rhi::MemoryHint::HostVisible;
			bDesc.DebugName = "Foliage.InstanceBuffer";
			s_State.InstanceBuffers[slot] = device->CreateBuffer(bDesc);
			s_State.InstanceCapacities[slot] = bDesc.Size;
		}

		if (s_State.InstanceBuffers[slot])
			s_State.InstanceBuffers[slot]->SetData(visibleInstances.data(), neededSize);

		FoliageShaderUniforms uniforms;
		uniforms.WindParams = glm::vec4(comp.WindFrequency, comp.WindStrength, time, 1.0f);
		uniforms.WindDirection = glm::vec4(1.0f, 0.0f, 0.3f, 0.0f);
		uniforms.CullParams = glm::vec4(comp.MaxCullDistance, comp.FadeRange, 0.0f, 0.0f);
		if (s_State.UniformBuffers[slot])
			s_State.UniformBuffers[slot]->SetData(&uniforms, sizeof(uniforms));

		// 按物种分组排序，实现每种植物绑定专属纹理与材质进行批次渲染
		struct TypeBatch {
			uint32_t TypeIndex;
			std::vector<FoliageInstanceData> Instances;
		};
		std::vector<TypeBatch> batches;
		std::unordered_map<uint32_t, size_t> typeToBatch;
		for (const auto& inst : visibleInstances)
		{
			const uint32_t ti = static_cast<uint32_t>(inst.Params.w);
			auto it = typeToBatch.find(ti);
			if (it == typeToBatch.end())
			{
				typeToBatch[ti] = batches.size();
				batches.push_back({ ti, { inst } });
			}
			else
			{
				batches[it->second].Instances.push_back(inst);
			}
		}

		std::vector<FoliageInstanceData> sortedInstances;
		sortedInstances.reserve(visibleInstances.size());
		struct BatchDrawInfo {
			uint32_t TypeIndex;
			uint32_t StartInstance;
			uint32_t InstanceCount;
		};
		std::vector<BatchDrawInfo> drawInfos;
		for (const auto& b : batches)
		{
			BatchDrawInfo info;
			info.TypeIndex = b.TypeIndex;
			info.StartInstance = static_cast<uint32_t>(sortedInstances.size());
			info.InstanceCount = static_cast<uint32_t>(b.Instances.size());
			drawInfos.push_back(info);
			sortedInstances.insert(sortedInstances.end(), b.Instances.begin(), b.Instances.end());
		}

		if (s_State.InstanceBuffers[slot])
			s_State.InstanceBuffers[slot]->SetData(sortedInstances.data(), neededSize);

		cmd->BindPipeline(s_State.Pipeline);
		if (s_State.CurrentGlobalSet)
			cmd->BindDescriptorSet(s_State.CurrentGlobalSet, 0);
		if (s_State.UniformSets[slot])
			cmd->BindDescriptorSet(s_State.UniformSets[slot], 1);

		cmd->BindVertexBuffer(0, s_State.DefaultMeshVB, 0);
		cmd->BindVertexBuffer(1, s_State.InstanceBuffers[slot], 0);
		cmd->BindIndexBuffer(s_State.DefaultMeshIB, 0, Rhi::IndexType::UInt32);

		static std::unordered_map<Rhi::Texture*, Rhi::Handle<Rhi::DescriptorSet>> s_TypeTextureSets[Renderer::FramesInFlight];

		for (const auto& drawInfo : drawInfos)
		{
			Rhi::Handle<Rhi::Texture> currentTex = nullptr;
			if (drawInfo.TypeIndex < data.GetTypeCount() && data.GetType(drawInfo.TypeIndex).MaterialAsset.Path.IsValid())
			{
				const std::string matPath = std::string(StringPool::Get().PathOf(data.GetType(drawInfo.TypeIndex).MaterialAsset.Path));
				if (!matPath.empty())
					currentTex = TextureLibrary::Get().Get(matPath, /*srgb*/ true);
			}
			if (!currentTex)
			{
				if (drawInfo.TypeIndex == 1)
					currentTex = TextureLibrary::Get().Get("textures/foliage/wild_flower.png", true);
				else if (drawInfo.TypeIndex == 2)
					currentTex = TextureLibrary::Get().Get("textures/foliage/bush_leaves.png", true);
				else if (drawInfo.TypeIndex == 3)
					currentTex = TextureLibrary::Get().Get("textures/terrain/rock_albedo.png", true);
				else
					currentTex = TextureLibrary::Get().Get("textures/foliage/grass_tuft.png", true);
			}
			if (!currentTex)
				currentTex = s_State.DefaultAlbedo;

			Rhi::Handle<Rhi::DescriptorSet> targetSet = nullptr;
			auto it = s_TypeTextureSets[slot].find(currentTex.get());
			if (it != s_TypeTextureSets[slot].end() && it->second)
			{
				targetSet = it->second;
			}
			else if (s_State.TextureLayout)
			{
				targetSet = device->CreateDescriptorSet(s_State.TextureLayout);
				if (targetSet)
				{
					Rhi::DescriptorWrite w;
					w.Binding = 4;
					w.Type = Rhi::DescriptorType::CombinedImageSampler;
					w.Texture = currentTex;
					w.Sampler = s_State.Sampler;
					targetSet->Update({ w });
					s_TypeTextureSets[slot][currentTex.get()] = targetSet;
				}
			}
			if (!targetSet)
				targetSet = s_State.TextureSets[slot];

			if (targetSet)
				cmd->BindDescriptorSet(targetSet, 2);

			// 按物种几何类型精确绑定专属网格
			const FoliageType& curType = (drawInfo.TypeIndex < data.GetTypeCount()) ? data.GetType(drawInfo.TypeIndex) : data.GetType(0);
			const std::string mPath = curType.MeshAsset.Path.IsValid() ? std::string(StringPool::Get().PathOf(curType.MeshAsset.Path)) : "";
			const bool isRock = (drawInfo.TypeIndex == 3 || mPath.find("scatter_pebble") != std::string::npos || curType.Name.find("碎石") != std::string::npos || curType.Name.find("Rock") != std::string::npos);
			const bool isBush = (drawInfo.TypeIndex == 2 || mPath.find("bush_cluster") != std::string::npos || curType.Name.find("灌木") != std::string::npos || curType.Name.find("Shrub") != std::string::npos);

			if (isRock && s_State.RockMeshVB && s_State.RockMeshIB)
			{
				cmd->BindVertexBuffer(0, s_State.RockMeshVB, 0);
				cmd->BindIndexBuffer(s_State.RockMeshIB, 0, Rhi::IndexType::UInt32);
				cmd->DrawIndexed(s_State.RockMeshIndexCount, drawInfo.InstanceCount, 0, 0, drawInfo.StartInstance);
			}
			else if (isBush && s_State.BushMeshVB && s_State.BushMeshIB)
			{
				cmd->BindVertexBuffer(0, s_State.BushMeshVB, 0);
				cmd->BindIndexBuffer(s_State.BushMeshIB, 0, Rhi::IndexType::UInt32);
				cmd->DrawIndexed(s_State.BushMeshIndexCount, drawInfo.InstanceCount, 0, 0, drawInfo.StartInstance);
			}
			else
			{
				cmd->BindVertexBuffer(0, s_State.DefaultMeshVB, 0);
				cmd->BindIndexBuffer(s_State.DefaultMeshIB, 0, Rhi::IndexType::UInt32);
				cmd->DrawIndexed(s_State.DefaultMeshIndexCount, drawInfo.InstanceCount, 0, 0, drawInfo.StartInstance);
			}
		}
	}
}
