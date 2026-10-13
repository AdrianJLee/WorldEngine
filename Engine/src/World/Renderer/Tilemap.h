#pragma once

#include "World/Core/Export.h"
#include "World/Core/StringPool.h"
#include "World/Renderer/TilemapGridUtils.h"

#include <glm/glm.hpp>
#include <cstdint>
#include <string>
#include <vector>
#include <memory>

namespace World
{
	class RuleTile;
	struct WLD_API MergedColliderRect
	{
		float X { 0.0f };      // Bottom-left X in grid cell units
		float Y { 0.0f };      // Bottom-left Y in grid cell units
		float Width { 1.0f };  // Width in grid cell units
		float Height { 1.0f }; // Height in grid cell units
	};

	class WLD_API Tilemap
	{
	public:
		static constexpr uint32_t kChunkSize = 16;

		Tilemap() = default;
		Tilemap(std::string name, uint32_t width, uint32_t height, std::string atlasPath = "");

		const std::string& GetName() const noexcept { return m_Name; }
		void SetName(std::string name) { m_Name = std::move(name); }

		const std::string& GetAtlasPath() const noexcept { return m_AtlasPath; }
		void SetAtlasPath(std::string path) { m_AtlasPath = std::move(path); }

		uint32_t GetWidth() const noexcept { return m_Width; }
		uint32_t GetHeight() const noexcept { return m_Height; }
		void Resize(uint32_t width, uint32_t height);

		const glm::vec2& GetCellSize() const noexcept { return m_CellSize; }
		void SetCellSize(const glm::vec2& size) noexcept { m_CellSize = size; }

		TilemapGridType GetGridType() const noexcept { return m_GridType; }
		void SetGridType(TilemapGridType type) noexcept { m_GridType = type; }

		// 规则瓦片自动拼接 (对整图或指定单格及 8 邻域应用规则匹配)
		void ApplyRuleTile(const RuleTile& ruleTile);
		void ApplyRuleTileAt(int x, int y, const RuleTile& ruleTile);

		// tileId: 0 = empty, 1..N = SubTexture index (0..N-1) + 1
		void SetTile(uint32_t x, uint32_t y, uint16_t tileId);
		uint16_t GetTile(uint32_t x, uint32_t y) const noexcept;

		void SetSolid(uint32_t x, uint32_t y, bool solid);
		bool IsSolid(uint32_t x, uint32_t y) const noexcept;

		void Clear();
		void FillRect(uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1, uint16_t tileId, bool setSolid = false);
		void FloodFill(uint32_t startX, uint32_t startY, uint16_t newTileId, bool setSolid = false);

		uint32_t CountPaintedTiles() const noexcept;
		uint32_t CountSolidTiles() const noexcept;

		// Synthesizes contiguous solid cells into maximal merged rectangles to eliminate ghost collisions
		std::vector<MergedColliderRect> ComputeMergedColliderRects() const;

		const std::vector<uint16_t>& GetRawTiles() const noexcept { return m_Tiles; }
		const std::vector<uint8_t>& GetRawCollisionMask() const noexcept { return m_CollisionMask; }

		std::string SerializeYaml() const;
		bool DeserializeYaml(const std::string& yaml);

		static std::shared_ptr<Tilemap> LoadFromFile(const std::string& filepath);
		static std::shared_ptr<Tilemap> GetOrLoad(const std::string& filepath);
		static void ClearCache();
		bool SaveToFile(const std::string& filepath) const;

	private:
		std::string m_Name { "NewTilemap" };
		std::string m_AtlasPath;
		uint32_t m_Width { 16 };
		uint32_t m_Height { 16 };
		glm::vec2 m_CellSize { 1.0f, 1.0f };
		TilemapGridType m_GridType { TilemapGridType::Orthogonal };
		std::vector<uint16_t> m_Tiles;        // size = m_Width * m_Height
		std::vector<uint8_t> m_CollisionMask; // size = m_Width * m_Height (0 = passable, 1 = solid)
	};
}
