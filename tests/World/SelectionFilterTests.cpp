#include "World/Scene/SelectionFilter.h"
#include "World/Scene/Scene.h"
#include "World/Scene/Entity.h"
#include "World/Scene/Components.h"
#include "World/Core/Log.h"
#include "World/Core/WorldContext.h"

#include <cassert>
#include <iostream>

using namespace World;

static void TestMaskOperations()
{
	SelectionFilter filter;
	assert(filter.GetSelectableMask() == Layer_All);
	assert(filter.GetVisibleMask() == Layer_All);
	assert(filter.GetLockedMask() == 0);

	// Selectable mask
	filter.SetLayerSelectable(Layer_UI, false);
	assert(!filter.IsLayerSelectable(Layer_UI));
	assert(filter.IsLayerSelectable(Layer_Actor));

	// Visible mask
	filter.SetLayerVisible(Layer_Background, false);
	assert(!filter.IsLayerVisible(Layer_Background));
	assert(filter.IsLayerVisible(Layer_Decor));

	// Locked mask
	filter.SetLayerLocked(Layer_Collider, true);
	assert(filter.IsLayerLocked(Layer_Collider));
	assert(!filter.IsLayerLocked(Layer_Trigger));

	// Reset
	filter.Reset();
	assert(filter.GetSelectableMask() == Layer_All);
	assert(filter.GetVisibleMask() == Layer_All);
	assert(filter.GetLockedMask() == 0);

	std::cout << "[PASS] TestMaskOperations\n";
}

static void TestEntityClassification()
{
	WorldContext context;
	Scene scene(context);

	// Tag-based UI
	Entity uiEntity = Entity::CreateEntity(&scene, "UI_HealthBar");
	assert(SelectionFilter::ClassifyEntityLayer(uiEntity, scene) == Layer_UI);

	// Tag-based Actor
	Entity actorEntity = Entity::CreateEntity(&scene, "Player_Knight");
	assert(SelectionFilter::ClassifyEntityLayer(actorEntity, scene) == Layer_Actor);

	// Component-based Light
	Entity lightEntity = Entity::CreateEntity(&scene, "Torch");
	lightEntity.AddComponent<PointLightComponent>();
	assert(SelectionFilter::ClassifyEntityLayer(lightEntity, scene) == Layer_Light);

	// Component-based Trigger
	Entity triggerEntity = Entity::CreateEntity(&scene, "DoorSensor");
	auto& boxSensor = triggerEntity.AddComponent<BoxCollider2DComponent>();
	boxSensor.IsSensor = true;
	assert(SelectionFilter::ClassifyEntityLayer(triggerEntity, scene) == Layer_Trigger);

	// Component-based Collider
	Entity wallEntity = Entity::CreateEntity(&scene, "StoneWall");
	auto& boxWall = wallEntity.AddComponent<BoxCollider2DComponent>();
	boxWall.IsSensor = false;
	assert(SelectionFilter::ClassifyEntityLayer(wallEntity, scene) == Layer_Collider);

	// Component-based Background
	Entity bgEntity = Entity::CreateEntity(&scene, "DistantMountains");
	bgEntity.AddComponent<ParallaxLayerComponent>();
	assert(SelectionFilter::ClassifyEntityLayer(bgEntity, scene) == Layer_Background);

	// Component-based Decor
	Entity decorEntity = Entity::CreateEntity(&scene, "GrassTilemap");
	decorEntity.AddComponent<TilemapComponent>();
	assert(SelectionFilter::ClassifyEntityLayer(decorEntity, scene) == Layer_Decor);

	std::cout << "[PASS] TestEntityClassification\n";
}

static void TestCanSelectEntity()
{
	WorldContext context;
	Scene scene(context);
	SelectionFilter filter;

	Entity player = Entity::CreateEntity(&scene, "Player");
	assert(filter.CanSelectEntity(player, scene));

	// 1. Lock actor layer -> cannot select
	filter.SetLayerLocked(Layer_Actor, true);
	assert(!filter.CanSelectEntity(player, scene));
	filter.SetLayerLocked(Layer_Actor, false);
	assert(filter.CanSelectEntity(player, scene));

	// 2. Hide actor layer -> cannot select
	filter.SetLayerVisible(Layer_Actor, false);
	assert(!filter.CanSelectEntity(player, scene));
	filter.SetLayerVisible(Layer_Actor, true);
	assert(filter.CanSelectEntity(player, scene));

	// 3. Make actor layer unselectable -> cannot select
	filter.SetLayerSelectable(Layer_Actor, false);
	assert(!filter.CanSelectEntity(player, scene));
	filter.SetLayerSelectable(Layer_Actor, true);
	assert(filter.CanSelectEntity(player, scene));

	std::cout << "[PASS] TestCanSelectEntity\n";
}

static void TestPresetsAndMultiLayerInteraction()
{
	SelectionFilter filter;
	const uint32_t allLayers[] = {
		Layer_Actor, Layer_Light, Layer_Trigger, Layer_Collider,
		Layer_Decor, Layer_Background, Layer_UI
	};

	// Preset: Actors Only
	for (uint32_t mask : allLayers)
		filter.SetLayerSelectable(mask, mask == Layer_Actor);

	assert(filter.IsLayerSelectable(Layer_Actor));
	assert(!filter.IsLayerSelectable(Layer_Light));
	assert(!filter.IsLayerSelectable(Layer_Trigger));
	assert(!filter.IsLayerSelectable(Layer_Collider));
	assert(!filter.IsLayerSelectable(Layer_Decor));
	assert(!filter.IsLayerSelectable(Layer_Background));
	assert(!filter.IsLayerSelectable(Layer_UI));

	// Preset: Lock BG & Decor
	filter.Reset();
	filter.SetLayerLocked(Layer_Background, true);
	filter.SetLayerLocked(Layer_Decor, true);
	assert(filter.IsLayerLocked(Layer_Background));
	assert(filter.IsLayerLocked(Layer_Decor));
	assert(!filter.IsLayerLocked(Layer_Actor));

	// Reset
	filter.Reset();
	assert(filter.GetSelectableMask() == Layer_All);
	assert(filter.GetLockedMask() == 0);

	std::cout << "[PASS] TestPresetsAndMultiLayerInteraction\n";
}

static void TestViewportPickingFilterWorkflow()
{
	WorldContext context;
	Scene scene(context);
	SelectionFilter filter;

	Entity bg = Entity::CreateEntity(&scene, "Parallax_Sky");
	bg.AddComponent<ParallaxLayerComponent>();

	Entity tree = Entity::CreateEntity(&scene, "Forest_Tilemap");
	tree.AddComponent<TilemapComponent>();

	Entity player = Entity::CreateEntity(&scene, "Player_Hero");
	player.AddComponent<CharacterControllerComponent>();

	Entity torch = Entity::CreateEntity(&scene, "Cave_Torch");
	torch.AddComponent<PointLightComponent>();

	// Default: all entities can be selected
	assert(filter.CanSelectEntity(bg, scene));
	assert(filter.CanSelectEntity(tree, scene));
	assert(filter.CanSelectEntity(player, scene));
	assert(filter.CanSelectEntity(torch, scene));

	// 1. Lock BG & Decor -> bg and tree cannot be selected in viewport
	filter.SetLayerLocked(Layer_Background, true);
	filter.SetLayerLocked(Layer_Decor, true);
	assert(!filter.CanSelectEntity(bg, scene));
	assert(!filter.CanSelectEntity(tree, scene));
	assert(filter.CanSelectEntity(player, scene));
	assert(filter.CanSelectEntity(torch, scene));

	// 2. Set Actors Only -> only player can be selected
	const uint32_t allLayers[] = {
		Layer_Actor, Layer_Light, Layer_Trigger, Layer_Collider,
		Layer_Decor, Layer_Background, Layer_UI
	};
	for (uint32_t mask : allLayers)
		filter.SetLayerSelectable(mask, mask == Layer_Actor);

	assert(!filter.CanSelectEntity(bg, scene));
	assert(!filter.CanSelectEntity(tree, scene));
	assert(!filter.CanSelectEntity(torch, scene));
	assert(filter.CanSelectEntity(player, scene));

	std::cout << "[PASS] TestViewportPickingFilterWorkflow\n";
}

int main()
{
	Log::Init();
	std::cout << "Running WorldSelectionFilterTests...\n";

	TestMaskOperations();
	TestEntityClassification();
	TestCanSelectEntity();
	TestPresetsAndMultiLayerInteraction();
	TestViewportPickingFilterWorkflow();

	std::cout << "All WorldSelectionFilterTests passed successfully.\n";
	return 0;
}
