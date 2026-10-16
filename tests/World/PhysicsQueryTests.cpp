#include "World/Physics/PhysicsQuery.h"
#include "World/Scene/Components.h"
#include "World/Scene/Scene.h"
#include "World/Scene/Entity.h"
#include "World/Core/Log.h"
#include "World/Core/WorldContext.h"

#include <cassert>
#include <cmath>
#include <iostream>

using namespace World;
using namespace World::Physics;

static void TestRaycast3D()
{
	WorldContext context;
	Scene scene(context);

	// 1. BoxCollider3D
	Entity boxEntity = Entity::CreateEntity(&scene, "Box");
	auto& boxTr = boxEntity.AddComponent<TransformComponent>();
	boxTr.Location = glm::vec3(0.0f, 0.0f, 5.0f);
	auto& box = boxEntity.AddComponent<BoxCollider3DComponent>();
	box.HalfExtents = glm::vec3(1.0f, 1.0f, 1.0f);

	// Raycast along +Z towards box
	RaycastHit hit = PhysicsQuerySystem::Raycast(scene, glm::vec3(0.0f, 0.0f, 0.0f), glm::vec3(0.0f, 0.0f, 1.0f), 10.0f);
	assert(hit.Hit);
	assert(hit.Entity == (entt::entity)boxEntity);
	// Front face of box is at z = 4.0 (5.0 - 1.0)
	assert(std::abs(hit.Distance - 4.0f) < 1e-4f);
	assert(std::abs(hit.Point.z - 4.0f) < 1e-4f);
	assert(hit.Normal.z < -0.9f); // Facing back towards ray origin

	// 2. Raycast Miss
	RaycastHit miss = PhysicsQuerySystem::Raycast(scene, glm::vec3(0.0f, 0.0f, 0.0f), glm::vec3(1.0f, 0.0f, 0.0f), 10.0f);
	assert(!miss.Hit);

	// 3. SphereCollider3D
	Entity sphereEntity = Entity::CreateEntity(&scene, "Sphere");
	auto& sphereTr = sphereEntity.AddComponent<TransformComponent>();
	sphereTr.Location = glm::vec3(0.0f, 6.0f, 0.0f);
	auto& sphere = sphereEntity.AddComponent<SphereCollider3DComponent>();
	sphere.Radius = 1.0f;

	RaycastHit hitSphere = PhysicsQuerySystem::Raycast(scene, glm::vec3(0.0f, 0.0f, 0.0f), glm::vec3(0.0f, 1.0f, 0.0f), 10.0f);
	assert(hitSphere.Hit);
	assert(hitSphere.Entity == (entt::entity)sphereEntity);
	// Bottom of sphere is at y = 5.0 (6.0 - 1.0)
	assert(std::abs(hitSphere.Distance - 5.0f) < 1e-4f);
	assert(hitSphere.Normal.y < -0.9f);

	std::cout << "[PASS] TestRaycast3D\n";
}

static void TestRaycast2D()
{
	WorldContext context;
	Scene scene(context);

	Entity box2dEntity = Entity::CreateEntity(&scene, "Box2D");
	auto& tr = box2dEntity.AddComponent<TransformComponent>();
	tr.Location = glm::vec3(4.0f, 0.0f, 0.0f);
	auto& box2d = box2dEntity.AddComponent<BoxCollider2DComponent>();
	box2d.Size = glm::vec2(1.0f, 1.0f);

	RaycastHit hit = PhysicsQuerySystem::Raycast2D(scene, glm::vec2(0.0f, 0.0f), glm::vec2(1.0f, 0.0f), 10.0f);
	assert(hit.Hit);
	assert(hit.Entity == (entt::entity)box2dEntity);
	// Left face at x = 3.0 (4.0 - 1.0)
	assert(std::abs(hit.Distance - 3.0f) < 1e-4f);
	assert(hit.Normal.x < -0.9f);

	std::cout << "[PASS] TestRaycast2D\n";
}

static void TestSphereCast()
{
	WorldContext context;
	Scene scene(context);

	Entity boxEntity = Entity::CreateEntity(&scene, "TargetBox");
	auto& tr = boxEntity.AddComponent<TransformComponent>();
	tr.Location = glm::vec3(0.0f, 0.0f, 10.0f);
	auto& box = boxEntity.AddComponent<BoxCollider3DComponent>();
	box.HalfExtents = glm::vec3(1.0f, 1.0f, 1.0f);

	// SphereCast with radius 0.5f along +Z towards box
	// Box front face is at z = 9.0. Swept sphere front touches at z = 9.0, meaning sphere center is at z = 8.5!
	ShapeCastHit hit = PhysicsQuerySystem::SphereCast(scene, glm::vec3(0.0f, 0.0f, 0.0f), 0.5f, glm::vec3(0.0f, 0.0f, 1.0f), 20.0f);
	assert(hit.Hit);
	assert(hit.Entity == (entt::entity)boxEntity);
	assert(std::abs(hit.Distance - 8.5f) < 0.05f);
	assert(hit.Normal.z < -0.9f);

	std::cout << "[PASS] TestSphereCast\n";
}

static void TestOverlapQueries()
{
	WorldContext context;
	Scene scene(context);

	Entity e1 = Entity::CreateEntity(&scene, "E1");
	e1.AddComponent<TransformComponent>().Location = glm::vec3(0.0f, 0.0f, 0.0f);
	e1.AddComponent<SphereCollider3DComponent>().Radius = 1.0f;

	Entity e2 = Entity::CreateEntity(&scene, "E2");
	e2.AddComponent<TransformComponent>().Location = glm::vec3(3.0f, 0.0f, 0.0f);
	e2.AddComponent<BoxCollider3DComponent>().HalfExtents = glm::vec3(0.5f);

	Entity eFar = Entity::CreateEntity(&scene, "EFar");
	eFar.AddComponent<TransformComponent>().Location = glm::vec3(20.0f, 0.0f, 0.0f);
	eFar.AddComponent<SphereCollider3DComponent>().Radius = 1.0f;

	// OverlapSphere with radius 4.0 at origin should hit E1 and E2, but not EFar
	auto overlaps = PhysicsQuerySystem::OverlapSphere(scene, glm::vec3(0.0f, 0.0f, 0.0f), 4.0f);
	assert(overlaps.size() == 2);

	// OverlapBox
	auto boxOverlaps = PhysicsQuerySystem::OverlapBox(scene, glm::vec3(0.0f, 0.0f, 0.0f), glm::vec3(1.5f, 1.5f, 1.5f));
	assert(boxOverlaps.size() == 1); // Only E1 overlaps

	std::cout << "[PASS] TestOverlapQueries\n";
}

static void TestLayerMaskFiltering()
{
	WorldContext context;
	Scene scene(context);

	Entity e = Entity::CreateEntity(&scene, "LayeredEntity");
	e.AddComponent<TransformComponent>().Location = glm::vec3(0.0f, 0.0f, 5.0f);
	e.AddComponent<BoxCollider3DComponent>().HalfExtents = glm::vec3(1.0f);
	auto& rb = e.AddComponent<RigidBody3DComponent>();
	rb.Layer = 2; // Bit 1

	// Query with mask = 1 (Bit 0): should NOT hit
	RaycastHit hitFiltered = PhysicsQuerySystem::Raycast(scene, glm::vec3(0.0f, 0.0f, 0.0f), glm::vec3(0.0f, 0.0f, 1.0f), 10.0f, 1);
	assert(!hitFiltered.Hit);

	// Query with mask = 2 (Bit 1): should HIT
	RaycastHit hitPass = PhysicsQuerySystem::Raycast(scene, glm::vec3(0.0f, 0.0f, 0.0f), glm::vec3(0.0f, 0.0f, 1.0f), 10.0f, 2);
	assert(hitPass.Hit);
	assert(hitPass.Entity == (entt::entity)e);

	std::cout << "[PASS] TestLayerMaskFiltering\n";
}

int main()
{
	Log::Init();
	std::cout << "Running WorldPhysicsQueryTests...\n";

	TestRaycast3D();
	TestRaycast2D();
	TestSphereCast();
	TestOverlapQueries();
	TestLayerMaskFiltering();

	std::cout << "All WorldPhysicsQueryTests passed successfully.\n";
	return 0;
}
