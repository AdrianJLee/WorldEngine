#include "World/Renderer/TextureAtlas.h"
#include "World/Scene/Components.h"
#include "World/Core/Log.h"
#include "World/Core/StringPool.h"

#include <iostream>
#include <cassert>
#include <cmath>
#include <vector>
#include <algorithm>

using namespace World;

static void TestSubTextureUVMath()
{
	const uint32_t atlasW = 200;
	const uint32_t atlasH = 100;
	// Rect: x=20, y=10, w=40, h=30 (top-left pixel origin)
	const glm::vec4 rect { 20.0f, 10.0f, 40.0f, 30.0f };
	const glm::vec2 pivot { 0.5f, 0.0f };
	const glm::vec4 border { 4.0f, 4.0f, 5.0f, 5.0f };

	SubTexture2D sub("hero_run", rect, atlasW, atlasH, pivot, border);

	assert(sub.Name == "hero_run");
	assert(sub.Pivot.x == 0.5f && sub.Pivot.y == 0.0f);
	assert(sub.OriginalSize.x == 40.0f && sub.OriginalSize.y == 30.0f);

	// u0 = 20 / 200 = 0.10f
	// u1 = (20 + 40) / 200 = 0.30f
	// v0 = 10 / 100 = 0.10f (top)
	// v1 = (10 + 30) / 100 = 0.40f (bottom)
	assert(std::abs(sub.TexCoords[0].x - 0.10f) < 1e-5f);
	assert(std::abs(sub.TexCoords[0].y - 0.40f) < 1e-5f); // Bottom-Left
	assert(std::abs(sub.TexCoords[1].x - 0.30f) < 1e-5f);
	assert(std::abs(sub.TexCoords[1].y - 0.40f) < 1e-5f); // Bottom-Right
	assert(std::abs(sub.TexCoords[2].x - 0.30f) < 1e-5f);
	assert(std::abs(sub.TexCoords[2].y - 0.10f) < 1e-5f); // Top-Right
	assert(std::abs(sub.TexCoords[3].x - 0.10f) < 1e-5f);
	assert(std::abs(sub.TexCoords[3].y - 0.10f) < 1e-5f); // Top-Left

	std::cout << "[PASS] TestSubTextureUVMath\n";
}

static void TestAtlasSerializationRoundtrip()
{
	TextureAtlas atlas("assets/textures/characters/knight.png", 512, 512);
	atlas.AddSubTexture(SubTexture2D("idle_0", { 0.0f, 0.0f, 64.0f, 64.0f }, 512, 512, { 0.5f, 0.0f }));
	atlas.AddSubTexture(SubTexture2D("idle_1", { 64.0f, 0.0f, 64.0f, 64.0f }, 512, 512, { 0.5f, 0.0f }));
	atlas.AddSubTexture(SubTexture2D("ui_frame", { 128.0f, 0.0f, 100.0f, 80.0f }, 512, 512, { 0.5f, 0.5f }, { 10.0f, 10.0f, 10.0f, 10.0f }));

	std::string yaml = atlas.SerializeYaml();
	assert(!yaml.empty());
	assert(yaml.find("idle_0") != std::string::npos);
	assert(yaml.find("ui_frame") != std::string::npos);

	TextureAtlas loaded;
	bool ok = loaded.DeserializeYaml(yaml);
	assert(ok);
	assert(loaded.GetTexturePath() == "assets/textures/characters/knight.png");
	assert(loaded.GetWidth() == 512);
	assert(loaded.GetHeight() == 512);
	assert(loaded.GetSubTextureCount() == 3);

	const SubTexture2D* sub0 = loaded.FindSubTexture("idle_0");
	assert(sub0 != nullptr);
	assert(sub0->Name == "idle_0");
	assert(sub0->PixelRect.z == 64.0f && sub0->PixelRect.w == 64.0f);
	assert(sub0->Pivot.y == 0.0f);

	const SubTexture2D* subFrame = loaded.FindSubTexture("ui_frame");
	assert(subFrame != nullptr);
	assert(subFrame->Border.x == 10.0f);

	// Test lookup by NameId
	NameId nid = StringPool::Get().InternName("idle_1");
	const SubTexture2D* sub1 = loaded.FindSubTexture(nid);
	assert(sub1 != nullptr);
	assert(sub1->Name == "idle_1");

	assert(loaded.FindSubTexture("non_existent") == nullptr);

	std::cout << "[PASS] TestAtlasSerializationRoundtrip\n";
}

static void Test2DSortingRules()
{
	struct RenderItem
	{
		std::string name;
		int32_t sortingLayer;
		int32_t orderInLayer;
		float depthKey;
		uint32_t id;
	};

	std::vector<RenderItem> items = {
		{ "foreground_tree", 10, 0, 0.0f, 1 },
		{ "bg_mountain", -10, 0, 0.0f, 2 },
		{ "player_front", 0, 0, -5.0f, 3 }, // lower Y -> deeper in Y-sort -> draws later
		{ "player_back", 0, 0, -10.0f, 4 }, // higher Y -> higher on screen -> draws earlier
		{ "item_on_ground", 0, 1, -10.0f, 5 } // order in layer = 1 -> draws on top of player_back
	};

	std::stable_sort(items.begin(), items.end(), [](const RenderItem& a, const RenderItem& b)
	{
		if (a.sortingLayer != b.sortingLayer)
			return a.sortingLayer < b.sortingLayer;
		if (a.orderInLayer != b.orderInLayer)
			return a.orderInLayer < b.orderInLayer;
		if (std::abs(a.depthKey - b.depthKey) > 1e-4f)
			return a.depthKey < b.depthKey;
		return a.id < b.id;
	});

	// Order must be:
	// 0: bg_mountain (sortingLayer = -10)
	// 1: player_back (layer 0, order 0, depthKey -10.0f)
	// 2: player_front (layer 0, order 0, depthKey -5.0f)
	// 3: item_on_ground (layer 0, order 1)
	// 4: foreground_tree (sortingLayer = 10)
	assert(items[0].name == "bg_mountain");
	assert(items[1].name == "player_back");
	assert(items[2].name == "player_front");
	assert(items[3].name == "item_on_ground");
	assert(items[4].name == "foreground_tree");

	std::cout << "[PASS] Test2DSortingRules\n";
}

static void TestColliderAndSpriteComponents()
{
	static_assert(sizeof(SpriteComponent) == 64, "SpriteComponent size contract");
	static_assert(sizeof(CapsuleCollider2DComponent) == 32, "CapsuleCollider2DComponent size contract");
	static_assert(std::is_trivially_copyable_v<SpriteComponent>, "SpriteComponent must be trivially copyable");
	static_assert(std::is_trivially_copyable_v<CapsuleCollider2DComponent>, "CapsuleCollider2DComponent must be trivially copyable");

	SpriteComponent sprite;
	assert(sprite.Pivot.x == 0.5f && sprite.Pivot.y == 0.5f);
	assert(!sprite.FlipX && !sprite.FlipY);
	assert(sprite.SortingLayer == 0 && sprite.OrderInLayer == 0);
	assert(!sprite.EnableYSort);

	CapsuleCollider2DComponent cap;
	assert(cap.Radius == 0.25f);
	assert(cap.Length == 0.5f);
	assert(cap.Direction == 0); // Vertical
	assert(cap.ShowCollider);
	assert(!cap.IsSensor);

	std::cout << "[PASS] TestColliderAndSpriteComponents\n";
}

int main()
{
	Log::Init();
	std::cout << "Running TextureAtlas and 2D System Tests...\n";

	TestSubTextureUVMath();
	TestAtlasSerializationRoundtrip();
	Test2DSortingRules();
	TestColliderAndSpriteComponents();

	std::cout << "All 2D system tests passed successfully.\n";
	return 0;
}
