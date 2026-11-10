#pragma once

#include "World/Core/Export.h"
#include "World/Core/AssetRef.h"
#include "World/RHI/Rhi.h"
#include "World/Renderer/Camera.h"
#include "World/Terrain/Terrain.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace World
{
	struct TerrainComponent;

	// std140 UBO 结构，与 Terrain.slang 中的 TerrainUniforms 逐字段对齐 (144B)
	struct TerrainUniforms
	{
		glm::mat4 Model { 1.0f };
		glm::vec4 LayerTiling01 { 10.0f, 10.0f, 10.0f, 10.0f };
		glm::vec4 LayerTiling23 { 10.0f, 10.0f, 10.0f, 10.0f };
		glm::vec4 LayerRoughnessMetallic { 0.8f, 0.8f, 0.9f, 0.6f };
		glm::vec4 SlopeParams { 0.707f, 0.15f, 2.0f, 1.0f };
		glm::vec4 TerrainParams { 1.0f, -1.0f, 0.0f, 0.0f }; // x=receiveShadow, y=entityId
		glm::vec4 CameraPosition { 0.0f, 0.0f, 0.0f, 1.0f };
	};
	static_assert(sizeof(TerrainUniforms) == 160, "TerrainUniforms must be 160 bytes std140");
	static_assert(offsetof(TerrainUniforms, Model) == 0, "Model at offset 0");
	static_assert(offsetof(TerrainUniforms, LayerTiling01) == 64, "LayerTiling01 at offset 64");
	static_assert(offsetof(TerrainUniforms, LayerTiling23) == 80, "LayerTiling23 at offset 80");
	static_assert(offsetof(TerrainUniforms, LayerRoughnessMetallic) == 96, "LayerRoughnessMetallic at offset 96");
	static_assert(offsetof(TerrainUniforms, SlopeParams) == 112, "SlopeParams at offset 112");
	static_assert(offsetof(TerrainUniforms, TerrainParams) == 128, "TerrainParams at offset 128");
	static_assert(offsetof(TerrainUniforms, CameraPosition) == 144, "CameraPosition at offset 144");

	struct TerrainRenderSettings
	{
		glm::mat4 Transform { 1.0f };
		uint32_t ChunkResolution { 64 };
		float LodDistanceRatio { 1.5f };
		bool CastShadow { true };
		bool ReceiveShadow { true };
		bool HoleCutoutEnabled { true };
		float SlopeThreshold { 0.707f };
		float SlopeBlendRange { 0.15f };
		int32_t EntityId { -1 };
	};

	// 封装地形 RHI 渲染管线与 Chunk CDLOD 网格提交
	class WLD_API TerrainRenderer
	{
	public:
		static void Init(const Rhi::Handle<Rhi::RenderPass>& mainRenderPass = nullptr);
		static void Shutdown();

		// 场景提交开始与结束
		static void BeginScene(const glm::mat4& viewProjection,
			const Rhi::Handle<Rhi::CommandBuffer>& commandBuffer,
			const Rhi::Handle<Rhi::DescriptorSet>& globalDescriptorSet = nullptr);
		static void EndScene();

		// 派工单核心绘制提交接口
		static void RenderTerrain(const TerrainData& data, const TerrainComponent& comp, const Camera& camera);
		static void RenderTerrain(const TerrainData& data, const TerrainRenderSettings& settings, const Camera& camera, const glm::mat4& cameraTransform);
		static void RenderTerrain(const TerrainData& data, const TerrainRenderSettings& settings, const glm::mat4& viewProjection, const glm::vec3& cameraPosition);

		// 阴影通道渲染
		static void RenderTerrainShadow(const TerrainData& data, const TerrainRenderSettings& settings, uint32_t cascadeIndex = 0);

		// 依据 AssetRef 或默认参数获取/加载 TerrainData
		static std::shared_ptr<TerrainData> GetOrCreateTerrainData(const AssetRef& asset, float cellSize = 1.0f, float heightScale = 1.0f);

		// 设置/更新当前渲染上下文 (由 SceneRenderer 每帧协调)
		static void SetContext(const Rhi::Handle<Rhi::CommandBuffer>& cmdBuffer,
			const Rhi::Handle<Rhi::DescriptorSet>& globalDescriptorSet);

		// 刷新/清理内部 Chunk GPU 缓冲与纹理缓存
		static void ClearCache();

		// 管线与布局访问
		static Rhi::Handle<Rhi::Pipeline> GetPipeline();
		static Rhi::Handle<Rhi::Pipeline> GetShadowPipeline();
		static Rhi::Handle<Rhi::DescriptorSetLayout> GetObjectLayout();
		static Rhi::Handle<Rhi::DescriptorSetLayout> GetMaterialLayout();

	private:
		static void EnsureInitialized();
		static void EnsurePipeline(const Rhi::Handle<Rhi::RenderPass>& renderPass);
	};
}