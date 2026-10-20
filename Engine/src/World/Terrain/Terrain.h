#pragma once

#include "World/Core/AssetRef.h"
#include "World/Core/Export.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace World
{
	enum class TerrainBrushFalloff : uint8_t
	{
		Linear = 0,
		Smooth = 1,
		Sharp = 2,
		Constant = 3
	};

	enum class TerrainSculptMode : uint8_t
	{
		Raise = 0,
		Lower = 1,
		Smooth = 2,
		Flatten = 3,
		Erode = 4,
		Hole = 5,
		PaintLayer = 6
	};

	struct TerrainLayer
	{
		std::string Name;
		AssetRef AlbedoTexture;
		AssetRef NormalTexture;
		float Roughness { 0.8f };
		float Metallic { 0.0f };
		glm::vec2 Tiling { 10.0f, 10.0f };
	};

	struct TerrainRaycastHit
	{
		bool Hit { false };
		glm::vec3 Point { 0.0f };
		glm::vec3 Normal { 0.0f, 1.0f, 0.0f };
		float Distance { 0.0f };
		uint32_t SampleX { 0 };
		uint32_t SampleZ { 0 };
	};

	struct TerrainVertex
	{
		glm::vec3 Position { 0.0f };
		glm::vec3 Normal { 0.0f, 1.0f, 0.0f };
		glm::vec2 TexCoord { 0.0f };
		glm::vec4 SplatWeights { 1.0f, 0.0f, 0.0f, 0.0f }; // Layer 0..3
		float Hole { 0.0f }; // 0.0 = solid, 1.0 = hole cutout
	};

	struct TerrainChunk
	{
		uint32_t ChunkX { 0 };
		uint32_t ChunkZ { 0 };
		uint32_t CurrentLod { 0 };
		glm::vec3 BoundsMin { 0.0f };
		glm::vec3 BoundsMax { 0.0f };
		std::vector<TerrainVertex> Vertices;
		std::vector<uint32_t> Indices;
		bool DirtyMesh { true };
	};

	class WLD_API TerrainData
	{
	public:
		TerrainData();
		TerrainData(uint32_t width, uint32_t height, float cellSize = 1.0f, float heightScale = 1.0f);
		~TerrainData() = default;

		void Resize(uint32_t width, uint32_t height, float cellSize = 1.0f);

		uint32_t GetWidth() const { return m_Width; }
		uint32_t GetHeight() const { return m_Height; }
		float GetCellSize() const { return m_CellSize; }
		float GetHeightScale() const { return m_HeightScale; }
		void SetHeightScale(float scale) { m_HeightScale = scale; }

		float GetWorldWidth() const { return (m_Width > 1 ? static_cast<float>(m_Width - 1) * m_CellSize : 0.0f); }
		float GetWorldHeight() const { return (m_Height > 1 ? static_cast<float>(m_Height - 1) * m_CellSize : 0.0f); }

		// 标量高度访问
		float GetHeight(uint32_t x, uint32_t z) const;
		void SetHeight(uint32_t x, uint32_t z, float h);

		// 世界空间高精双线性插值高度与表面法线
		float SampleHeight(float worldX, float worldZ) const;
		glm::vec3 SampleNormal(float worldX, float worldZ) const;

		// Splatmap 权重访问 (4 通道 RGBA 0..255)
		glm::vec4 GetSplatWeights(uint32_t x, uint32_t z) const;
		void SetSplatWeights(uint32_t x, uint32_t z, const glm::vec4& weights);

		// 地洞遮罩访问 (0 = 通行, 1 = 镂空)
		bool IsHole(uint32_t x, uint32_t z) const;
		void SetHole(uint32_t x, uint32_t z, bool hole);

		// 材质图层定义 (最多 4 层)
		TerrainLayer& GetLayer(uint32_t index);
		const TerrainLayer& GetLayer(uint32_t index) const;

		// 空间射线投射
		bool Raycast(const glm::vec3& rayOrigin, const glm::vec3& rayDir, float maxDistance, TerrainRaycastHit& outHit) const;

		// 视口雕刻笔刷算法
		void SculptRaiseLower(float worldX, float worldZ, float radius, float deltaHeight, TerrainBrushFalloff falloff);
		void SculptSmooth(float worldX, float worldZ, float radius, float strength, TerrainBrushFalloff falloff);
		void SculptFlatten(float worldX, float worldZ, float radius, float targetHeight, float strength, TerrainBrushFalloff falloff);
		void SculptErode(float worldX, float worldZ, float radius, float strength, int iterations);
		void PaintLayer(float worldX, float worldZ, float radius, uint32_t layerIndex, float strength, TerrainBrushFalloff falloff);
		void PaintHole(float worldX, float worldZ, float radius, bool holeCutout);

		// 计算笔刷权重衰减
		static float EvaluateFalloff(float distance, float radius, TerrainBrushFalloff falloff);

		// 分块与 CDLOD 网格生成
		uint32_t GetChunkCountX(uint32_t chunkResolution = 64) const;
		uint32_t GetChunkCountZ(uint32_t chunkResolution = 64) const;
		void GenerateChunkMesh(uint32_t chunkX, uint32_t chunkZ, uint32_t chunkResolution, uint32_t lod, TerrainChunk& outChunk) const;

		// 脏区域管理
		void MarkDirty(float minWorldX, float minWorldZ, float maxWorldX, float maxWorldZ);
		bool IsDirty() const { return m_IsDirty; }
		void ClearDirty() { m_IsDirty = false; }

		// 持久化与导入导出
		bool SaveToFile(const std::string& path) const;
		bool LoadFromFile(const std::string& path);
		bool ImportHeightmapRaw16(const std::string& path, uint32_t width, uint32_t height, float minH, float maxH);
		bool ExportHeightmapRaw16(const std::string& path) const;

		const std::vector<float>& GetHeightBuffer() const { return m_Heights; }
		const std::vector<uint32_t>& GetSplatmapBuffer() const { return m_Splatmap; }
		const std::vector<uint8_t>& GetHoleBuffer() const { return m_HoleMask; }

	private:
		uint32_t m_Width { 256 };
		uint32_t m_Height { 256 };
		float m_CellSize { 1.0f };
		float m_HeightScale { 1.0f };
		bool m_IsDirty { true };

		std::vector<float> m_Heights;
		std::vector<uint32_t> m_Splatmap; // RGBA 4 channels packed as uint32_t (R=layer0, G=layer1, B=layer2, A=layer3)
		std::vector<uint8_t> m_HoleMask;  // 0 = solid, 1 = hole
		TerrainLayer m_Layers[4];
	};
}
