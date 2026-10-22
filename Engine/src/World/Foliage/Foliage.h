#pragma once

#include "World/Core/AssetRef.h"
#include "World/Core/Export.h"
#include "World/Terrain/Terrain.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace World
{
	struct FoliageType
	{
		std::string Name { "Grass" };
		AssetRef MeshAsset;
		AssetRef MaterialAsset;
		float Density { 1.0f };               // 期望散布密度 (每平方米)
		glm::vec2 ScaleMinMax { 0.8f, 1.2f }; // 缩放随机区间
		float RandomYaw { 360.0f };           // 随机偏航角 (0..360 度)
		float PitchVariation { 5.0f };        // 随机倾斜角 (度)
		glm::vec2 SlopeLimit { 0.0f, 45.0f }; // 坡度限制 (最小度数, 最大度数)
		glm::vec2 HeightLimit { -1000.0f, 1000.0f }; // 高度限制
		float CullDistance { 150.0f };        // 独立视距裁剪
		float WindMultiplier { 1.0f };        // 风力乘数
		bool AlignToNormal { true };          // 是否对齐地表法线
		bool PaintingActive { true };         // 参与笔刷混刷开关
		uint32_t LayerMask { 0xFFFFFFFF };    // 地表材质图层掩码过滤：Bit 0=Layer0/Grass, Bit 1=Layer1/Dirt, Bit 2=Layer2/Rock, Bit 3=Layer3/Sand
	};

	struct FoliageInstanceData
	{
		glm::mat4 Transform { 1.0f };
		glm::vec4 Params { 0.0f }; // x = windPhase (0..2*pi), y = ditherOffset (0..1), z = scale, w = typeIndex
	};

	struct FoliageChunk
	{
		int32_t ChunkX { 0 };
		int32_t ChunkZ { 0 };
		glm::vec3 BoundsMin { 0.0f };
		glm::vec3 BoundsMax { 0.0f };
		std::vector<FoliageInstanceData> Instances;
		bool DirtyBuffer { true };
	};

	class WLD_API FoliageData
	{
	public:
		FoliageData();
		~FoliageData() = default;

		// 植被类型管理
		size_t GetTypeCount() const { return m_Types.size(); }
		FoliageType& GetType(size_t index) { return m_Types[index]; }
		const FoliageType& GetType(size_t index) const { return m_Types[index]; }
		size_t AddType(const FoliageType& type);
		void RemoveType(size_t index);

		// 实例查询与访问
		size_t GetTotalInstanceCount() const;
		size_t GetInstanceCountByType(size_t typeIndex) const;
		const std::vector<FoliageChunk>& GetChunks() const { return m_Chunks; }
		std::vector<FoliageChunk>& GetChunksMut() { return m_Chunks; }

		// 笔刷散布与擦除算法
		uint32_t PaintFoliage(const glm::vec3& center, float radius, float density, size_t typeIndex,
			const TerrainData* terrain = nullptr, const glm::mat4& terrainTransform = glm::mat4(1.0f));
		uint32_t PaintFoliageMulti(const glm::vec3& center, float radius, float brushDensity,
			const std::vector<size_t>& activeTypes,
			const TerrainData* terrain = nullptr, const glm::mat4& terrainTransform = glm::mat4(1.0f));
		uint32_t EraseFoliage(const glm::vec3& center, float radius, int32_t typeIndex = -1);

		// 地表对齐与重新贴地
		void RealignToTerrain(const TerrainData* terrain, const glm::mat4& terrainTransform = glm::mat4(1.0f));

		// 视锥与视距剔除
		void CollectVisibleInstances(const glm::vec3& cameraPos, const glm::mat4& viewProj,
			float maxDistance, float fadeRange,
			std::vector<FoliageInstanceData>& outInstances) const;

		// 持久化与文件 I/O
		bool SaveToFile(const std::string& path) const;
		bool LoadFromFile(const std::string& path);

		void Clear();

	private:
		FoliageChunk& GetOrCreateChunk(int32_t cx, int32_t cz);

		std::vector<FoliageType> m_Types;
		std::vector<FoliageChunk> m_Chunks;
		float m_ChunkSize { 64.0f };
	};
}
