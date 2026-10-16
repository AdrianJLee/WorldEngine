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

int main()
{
	Log::Init();
	std::cout << "Running WorldSelectionFilterTests...\n";

	TestMaskOperations();
	TestEntityClassification();
	TestCanSelectEntity();

	std::cout << "All WorldSelectionFilterTests passed successfully.\n";
	return 0;
}
