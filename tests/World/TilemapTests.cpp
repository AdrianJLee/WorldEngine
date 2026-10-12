#include "World/Renderer/Tilemap.h"
#include "World/Scene/Components.h"
#include "World/Core/Log.h"

#include <cassert>
#include <cmath>
#include <iostream>

using namespace World;

static void TestTilemapPaintingAndFloodFill()
{
	Tilemap tm("Level1_Ground", 16, 12, "assets/textures/tileset.watlas");
	assert(tm.GetName() == "Level1_Ground");
	assert(tm.GetAtlasPath() == "assets/textures/tileset.watlas");
	assert(tm.GetWidth() == 16);
	assert(tm.GetHeight() == 12);
	assert(tm.CountPaintedTiles() == 0);
	assert(tm.CountSolidTiles() == 0);

	// Set and get individual tile & solid mask
	tm.SetTile(3, 4, 7);
	tm.SetSolid(3, 4, true);
	assert(tm.GetTile(3, 4) == 7);
	assert(tm.IsSolid(3, 4));
	assert(tm.GetTile(99, 99) == 0);
	assert(!tm.IsSolid(99, 99));
	assert(tm.CountPaintedTiles() == 1);
	assert(tm.CountSolidTiles() == 1);

	// Clear and test FillRect (including reversed bounds x0 > x1, y0 > y1)
	tm.Clear();
	assert(tm.CountPaintedTiles() == 0);
	assert(tm.CountSolidTiles() == 0);

	// Create a hollow 5x5 border from (2,2) to (6,6)
	tm.FillRect(2, 2, 6, 2, 2, true);
	tm.FillRect(2, 6, 6, 6, 2, true);
	tm.FillRect(2, 2, 2, 6, 2, true);
	tm.FillRect(6, 6, 6, 2, 2, true); // reversed Y
	assert(tm.GetTile(2, 2) == 2);
	assert(tm.GetTile(6, 6) == 2);
	assert(tm.GetTile(4, 4) == 0); // interior is still empty

	// 4-connected FloodFill inside the 3x3 interior (3..5, 3..5)
	tm.FloodFill(4, 4, 5, false);
	for (uint32_t y = 3; y <= 5; ++y)
	{
		for (uint32_t x = 3; x <= 5; ++x)
		{
			assert(tm.GetTile(x, y) == 5);
			assert(!tm.IsSolid(x, y));
		}
	}
	// Outside the border must remain 0
	assert(tm.GetTile(1, 1) == 0);
	assert(tm.GetTile(7, 7) == 0);

	// Resize preservation test
	tm.Resize(20, 14);
	assert(tm.GetWidth() == 20);
	assert(tm.GetHeight() == 14);
	assert(tm.GetTile(4, 4) == 5);
	assert(tm.GetTile(2, 2) == 2);
	assert(tm.IsSolid(2, 2));

	std::cout << "[PASS] TestTilemapPaintingAndFloodFill\n";
}

static void TestGreedyMergedColliderRects()
{
	// Case 1: 5 contiguous horizontal solid tiles -> 1 merged 5x1 rect
	{
		Tilemap tm("HorizontalPlatform", 16, 12);
		tm.FillRect(3, 2, 7, 2, 1, true);
		assert(tm.CountSolidTiles() == 5);

		auto rects = tm.ComputeMergedColliderRects();
		assert(rects.size() == 1);
		assert(std::abs(rects[0].X - 3.0f) < 1e-5f);
		assert(std::abs(rects[0].Y - 2.0f) < 1e-5f);
		assert(std::abs(rects[0].Width - 5.0f) < 1e-5f);
		assert(std::abs(rects[0].Height - 1.0f) < 1e-5f);
	}

	// Case 2: 3x3 solid block -> 1 merged 3x3 rect
	{
		Tilemap tm("SolidBlock3x3", 16, 12);
		tm.FillRect(4, 5, 6, 7, 1, true);
		assert(tm.CountSolidTiles() == 9);

		auto rects = tm.ComputeMergedColliderRects();
		assert(rects.size() == 1);
		assert(std::abs(rects[0].X - 4.0f) < 1e-5f);
		assert(std::abs(rects[0].Y - 5.0f) < 1e-5f);
		assert(std::abs(rects[0].Width - 3.0f) < 1e-5f);
		assert(std::abs(rects[0].Height - 3.0f) < 1e-5f);
	}

	// Case 3: L-shaped solid block -> 2 merged rectangles
	// Bottom bar: x=1..4, y=1 (4x1)
	// Vertical stem: x=1, y=2..3 (1x2)
	{
		Tilemap tm("LShape", 10, 10);
		tm.FillRect(1, 1, 4, 1, 1, true);
		tm.FillRect(1, 2, 1, 3, 1, true);
		assert(tm.CountSolidTiles() == 6);

		auto rects = tm.ComputeMergedColliderRects();
		assert(rects.size() == 2);
		assert(std::abs(rects[0].X - 1.0f) < 1e-5f && std::abs(rects[0].Y - 1.0f) < 1e-5f);
		assert(std::abs(rects[0].Width - 4.0f) < 1e-5f && std::abs(rects[0].Height - 1.0f) < 1e-5f);
		assert(std::abs(rects[1].X - 1.0f) < 1e-5f && std::abs(rects[1].Y - 2.0f) < 1e-5f);
		assert(std::abs(rects[1].Width - 1.0f) < 1e-5f && std::abs(rects[1].Height - 2.0f) < 1e-5f);
	}

	std::cout << "[PASS] TestGreedyMergedColliderRects\n";
}

static void TestTilemapYamlRoundtrip()
{
	Tilemap src("DungeonFloor", 8, 6, "assets/textures/dungeon.watlas");
	src.SetCellSize({ 1.5f, 1.5f });
	src.FillRect(0, 0, 7, 0, 3, true);
	src.SetTile(4, 3, 11);
	src.SetSolid(4, 3, false);

	const std::string yaml = src.SerializeYaml();
	assert(!yaml.empty());
	assert(yaml.find("DungeonFloor") != std::string::npos);
	assert(yaml.find("assets/textures/dungeon.watlas") != std::string::npos);

	Tilemap dst;
	const bool ok = dst.DeserializeYaml(yaml);
	assert(ok);
	assert(dst.GetName() == "DungeonFloor");
	assert(dst.GetAtlasPath() == "assets/textures/dungeon.watlas");
	assert(dst.GetWidth() == 8);
	assert(dst.GetHeight() == 6);
	assert(std::abs(dst.GetCellSize().x - 1.5f) < 1e-5f);
	assert(std::abs(dst.GetCellSize().y - 1.5f) < 1e-5f);
	assert(dst.CountPaintedTiles() == src.CountPaintedTiles());
	assert(dst.CountSolidTiles() == src.CountSolidTiles());

	for (uint32_t y = 0; y < 6; ++y)
	{
		for (uint32_t x = 0; x < 8; ++x)
		{
			assert(dst.GetTile(x, y) == src.GetTile(x, y));
			assert(dst.IsSolid(x, y) == src.IsSolid(x, y));
		}
	}

	static_assert(sizeof(TilemapComponent) == 56, "TilemapComponent size contract");
	static_assert(std::is_trivially_copyable_v<TilemapComponent>, "TilemapComponent must be trivially copyable");

	std::cout << "[PASS] TestTilemapYamlRoundtrip\n";
}

int main()
{
	Log::Init();
	std::cout << "Running WorldTilemapTests...\n";

	TestTilemapPaintingAndFloodFill();
	TestGreedyMergedColliderRects();
	TestTilemapYamlRoundtrip();

	std::cout << "All WorldTilemapTests passed successfully.\n";
	return 0;
}