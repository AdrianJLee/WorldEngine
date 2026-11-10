#include "World/Level/Core/DeltaState.h"
#include "World/Scene/Components.h"
#include "World/Scene/Scene.h"
#include "World/Scene/Entity.h"
#include "World/Core/Log.h"
#include "World/Core/WorldContext.h"

#include <cassert>
#include <cmath>
#include <filesystem>
#include <iostream>

using namespace World;
using namespace World::Level;

static void TestDeltaStateSerializationAndReplay()
{
	DeltaStateRegistry delta;
	delta.MarkDestroyed(1001);
	delta.SetProperty(1002, "Transform.Location", "15.0,25.0,35.0");
	delta.SetProperty(1002, "Health.CurrentHealth", "45.0");

	assert(delta.IsDestroyed(1001));
	assert(delta.HasProperty(1002, "Transform.Location"));

	std::filesystem::path tempFile = std::filesystem::temp_directory_path() / "test_slice.wdelta";
	bool saved = delta.SaveToFile(tempFile.string());
	assert(saved);

	DeltaStateRegistry loaded;
	bool loadSuccess = loaded.LoadFromFile(tempFile.string());
	assert(loadSuccess);
	assert(loaded.IsDestroyed(1001));
	assert(loaded.GetProperty(1002, "Transform.Location") == "15.0,25.0,35.0");
	assert(loaded.GetProperty(1002, "Health.CurrentHealth") == "45.0");

	WorldContext context;
	Scene scene(context);

	Entity e1 = Entity::CreateEntity(&scene, "DestroyedMob", UUID(1001));

	Entity e2 = Entity::CreateEntity(&scene, "DamagedChest", UUID(1002));
	auto& tr2 = e2.AddComponent<TransformComponent>();
	tr2.Location = glm::vec3(0.0f);
	auto& h2 = e2.AddComponent<HealthComponent>();
	h2.CurrentHealth = 100.0f;

	DeltaStateRegistry::ApplyDeltaState(scene, loaded);

	assert(!e1.IsValid());

	assert(std::abs(tr2.Location.x - 15.0f) < 1e-4f);
	assert(std::abs(tr2.Location.y - 25.0f) < 1e-4f);
	assert(std::abs(tr2.Location.z - 35.0f) < 1e-4f);
	assert(std::abs(h2.CurrentHealth - 45.0f) < 1e-4f);

	std::filesystem::remove(tempFile);
	std::cout << "[PASS] TestDeltaStateSerializationAndReplay\n";
}

int main()
{
	Log::Init();
	std::cout << "Running WorldSpatialDeltaPersistenceTests...\n";

	TestDeltaStateSerializationAndReplay();

	std::cout << "All WorldSpatialDeltaPersistenceTests passed successfully.\n";
	return 0;
}
