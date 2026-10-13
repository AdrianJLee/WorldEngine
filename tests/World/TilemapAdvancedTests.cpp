#include "World/Renderer/Tilemap.h"
#include "World/Renderer/TilemapGridUtils.h"
#include "World/Renderer/RuleTile.h"
#include "World/Core/Log.h"

#include <iostream>
#include <cassert>
#include <cmath>

using namespace World;

static void TestTilemapGridTopologies()
{
	const glm::vec2 cellSize { 2.0f, 1.0f };
	const uint32_t mapW = 16;
	const uint32_t mapH = 12;

	// 1. Orthogonal roundtrip
	{
		for (int y = 0; y < static_cast<int>(mapH); ++y)
		{
			for (int x = 0; x < static_cast<int>(mapW); ++x)
			{
				glm::vec2 local = TilemapGridUtils::GridToLocal(x, y, TilemapGridType::Orthogonal, cellSize, mapW, mapH);
				glm::ivec2 grid = TilemapGridUtils::LocalToGrid(local, TilemapGridType::Orthogonal, cellSize, mapW, mapH);
				assert(grid.x == x && grid.y == y);
			}
		}
		auto poly = TilemapGridUtils::GetCellPolygon(4, 5, TilemapGridType::Orthogonal, cellSize, mapW, mapH);
		assert(poly.size() == 4);
	}

	// 2. Isometric 2.5D roundtrip
	{
		for (int y = 0; y < static_cast<int>(mapH); ++y)
		{
			for (int x = 0; x < static_cast<int>(mapW); ++x)
			{
				glm::vec2 local = TilemapGridUtils::GridToLocal(x, y, TilemapGridType::Isometric, cellSize, mapW, mapH);
				glm::ivec2 grid = TilemapGridUtils::LocalToGrid(local, TilemapGridType::Isometric, cellSize, mapW, mapH);
				assert(grid.x == x && grid.y == y);
			}
		}
		auto poly = TilemapGridUtils::GetCellPolygon(4, 5, TilemapGridType::Isometric, cellSize, mapW, mapH);
		assert(poly.size() == 4);
		assert(TilemapGridUtils::ComputeIsometricDepthKey(0, 0) < TilemapGridUtils::ComputeIsometricDepthKey(1, 1));
	}

	// 3. Hexagonal Pointy & Flat
	{
		auto polyP = TilemapGridUtils::GetCellPolygon(2, 3, TilemapGridType::HexagonalPointy, cellSize, mapW, mapH);
		assert(polyP.size() == 6);
		auto polyF = TilemapGridUtils::GetCellPolygon(2, 3, TilemapGridType::HexagonalFlat, cellSize, mapW, mapH);
		assert(polyF.size() == 6);
	}

	std::cout << "[PASS] TestTilemapGridTopologies\n";
}

static void TestRuleTileAutoTiling()
{
	const uint16_t baseTileId = 10;
	RuleTile ruleTile = RuleTile::CreateStandard47Template("GrassAutoRule", "assets/textures/grass.watlas", baseTileId);
	assert(ruleTile.GetName() == "GrassAutoRule");
	assert(ruleTile.GetDefaultTileId() == baseTileId);
	assert(!ruleTile.GetRules().empty());

	Tilemap tm("TestMap", 8, 8);

	// 在 (2,2) 到 (4,4) 绘制 3x3 实心方块
	tm.FillRect(2, 2, 4, 4, 1, true);

	// 在 (0,0) 放置一个孤立地块
	tm.SetTile(0, 0, 1);
	tm.SetSolid(0, 0, true);

	// 应用自动拼接规则
	tm.ApplyRuleTile(ruleTile);

	// 验证 47-Tile 规则拼接结果:
	// 中心: (3, 3) 必须为 baseTileId (全实心中心)
	assert(tm.GetTile(3, 3) == baseTileId);

	// 外部四角:
	assert(tm.GetTile(2, 2) == baseTileId + 1); // 左上外角
	assert(tm.GetTile(4, 2) == baseTileId + 2); // 右上外角
	assert(tm.GetTile(2, 4) == baseTileId + 3); // 左下外角
	assert(tm.GetTile(4, 4) == baseTileId + 4); // 右下外角

	// 四个边缘:
	assert(tm.GetTile(3, 2) == baseTileId + 5); // 上边缘
	assert(tm.GetTile(3, 4) == baseTileId + 6); // 下边缘
	assert(tm.GetTile(2, 3) == baseTileId + 7); // 左边缘
	assert(tm.GetTile(4, 3) == baseTileId + 8); // 右边缘

	// 孤立地块 (0, 0):
	assert(tm.GetTile(0, 0) == baseTileId + 15);

	std::cout << "[PASS] TestRuleTileAutoTiling\n";
}

static void TestRuleTileYamlRoundtrip()
{
	RuleTile src = RuleTile::CreateStandard47Template("DungeonWallRule", "assets/textures/dungeon.watlas", 20);
	const std::string yaml = src.SerializeYaml();
	assert(!yaml.empty());
	assert(yaml.find("DungeonWallRule") != std::string::npos);
	assert(yaml.find("DefaultTileId: 20") != std::string::npos);

	RuleTile dst;
	assert(dst.DeserializeYaml(yaml));
	assert(dst.GetName() == "DungeonWallRule");
	assert(dst.GetAtlasPath() == "assets/textures/dungeon.watlas");
	assert(dst.GetDefaultTileId() == 20);
	assert(dst.GetRules().size() == src.GetRules().size());

	std::cout << "[PASS] TestRuleTileYamlRoundtrip\n";
}

int main()
{
	Log::Init();
	std::cout << "Running WorldTilemapAdvancedTests...\n";

	TestTilemapGridTopologies();
	TestRuleTileAutoTiling();
	TestRuleTileYamlRoundtrip();

	std::cout << "All WorldTilemapAdvancedTests passed successfully.\n";
	return 0;
}
