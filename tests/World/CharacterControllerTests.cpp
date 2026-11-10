#include "World/Physics/CharacterController.h"
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

static void TestProjectTangentAndCrease()
{
	// 1. ProjectTangent
	glm::vec3 v(1.0f, -2.0f, 0.0f);
	glm::vec3 normal(0.0f, 1.0f, 0.0f);
	glm::vec3 tangent = CharacterControllerSystem::ProjectTangent(v, normal);
	assert(std::abs(tangent.x - 1.0f) < 1e-5f);
	assert(std::abs(tangent.y - 0.0f) < 1e-5f);
	assert(std::abs(tangent.z - 0.0f) < 1e-5f);

	// 2. ProjectCrease: intersection of floor (0, 1, 0) and wall (0, 0, 1) -> crease is along X (1, 0, 0)
	glm::vec3 nFloor(0.0f, 1.0f, 0.0f);
	glm::vec3 nWall(0.0f, 0.0f, 1.0f);
	glm::vec3 diagonalMove(3.0f, -1.0f, -2.0f);
	glm::vec3 crease = CharacterControllerSystem::ProjectCrease(diagonalMove, nFloor, nWall);
	assert(std::abs(crease.x - 3.0f) < 1e-4f);
	assert(std::abs(crease.y - 0.0f) < 1e-4f);
	assert(std::abs(crease.z - 0.0f) < 1e-4f);

	std::cout << "[PASS] TestProjectTangentAndCrease\n";
}

static void TestCoyoteTimeAndCanJump()
{
	CharacterControllerComponent kcc;
	kcc.Grounded = 0;
	kcc.CoyoteDuration = 0.12f;
	kcc.CoyoteTimer = 0.12f; // within coyote window

	assert(CharacterControllerSystem::CanJump(kcc));
	bool jumped = CharacterControllerSystem::TryJump(kcc, 6.0f);
	assert(jumped);
	assert(kcc.Velocity.y == 6.0f);
	assert(kcc.CoyoteTimer == 0.0f);

	// Now in midair, coyote timer is 0
	assert(!CharacterControllerSystem::CanJump(kcc));
	bool jumpedAgain = CharacterControllerSystem::TryJump(kcc, 6.0f);
	assert(!jumpedAgain);
	// Should be saved into JumpBufferTimer
	assert(kcc.JumpBufferTimer == kcc.JumpBufferDuration);

	std::cout << "[PASS] TestCoyoteTimeAndCanJump\n";
}

static void TestJumpBufferingAndUpdate()
{
	WorldContext context;
	Scene scene(context);

	Entity e = Entity::CreateEntity(&scene, "Player");
	auto& kcc = e.AddComponent<CharacterControllerComponent>();
	kcc.Grounded = 0;
	kcc.JumpBufferTimer = 0.10f;
	kcc.CoyoteTimer = 0.05f;

	CharacterControllerSystem::Update(scene, 0.02f);
	assert(std::abs(kcc.JumpBufferTimer - 0.08f) < 1e-5f);
	assert(std::abs(kcc.CoyoteTimer - 0.03f) < 1e-5f);
	// Gravity applied: -9.81 * 0.02 = -0.1962
	assert(kcc.Velocity.y < -0.18f);

	std::cout << "[PASS] TestJumpBufferingAndUpdate\n";
}

static void TestMoveAndSlideUnobstructed()
{
	WorldContext context;
	Scene scene(context);

	Entity player = Entity::CreateEntity(&scene, "Player");
	auto& tr = player.AddComponent<TransformComponent>();
	tr.Location = glm::vec3(0.0f, 1.0f, 0.0f);
	player.AddComponent<CharacterControllerComponent>();

	glm::vec3 disp(2.0f, 0.0f, 0.0f);
	glm::vec3 moved = CharacterControllerSystem::MoveAndSlide(scene, (entt::entity)player, disp, 0.016f);

	assert(std::abs(moved.x - 2.0f) < 1e-4f);
	assert(std::abs(tr.Location.x - 2.0f) < 1e-4f);

	std::cout << "[PASS] TestMoveAndSlideUnobstructed\n";
}

static void TestMoveAndSlideWallCollision()
{
	WorldContext context;
	Scene scene(context);

	// Wall at x = 2.0 (box halfExtents = 0.5, so left face is at x = 1.5)
	Entity wall = Entity::CreateEntity(&scene, "Wall");
	wall.AddComponent<TransformComponent>().Location = glm::vec3(2.0f, 0.0f, 0.0f);
	auto& wallBox = wall.AddComponent<BoxCollider3DComponent>();
	wallBox.HalfExtents = glm::vec3(0.5f, 2.0f, 2.0f);

	// Player at x = 0.0 with sphere collider radius 0.5
	Entity player = Entity::CreateEntity(&scene, "Player");
	auto& tr = player.AddComponent<TransformComponent>();
	tr.Location = glm::vec3(0.0f, 0.0f, 0.0f);
	auto& kcc = player.AddComponent<CharacterControllerComponent>();
	kcc.SkinWidth = 0.02f;
	auto& sphere = player.AddComponent<SphereCollider3DComponent>();
	sphere.Radius = 0.5f;

	// Desired displacement: 3.0 along +X (would penetrate through wall without collision)
	glm::vec3 disp(3.0f, 0.0f, 0.0f);
	glm::vec3 moved = CharacterControllerSystem::MoveAndSlide(scene, (entt::entity)player, disp, 0.016f);

	// Wall surface is at x = 1.5. Sphere radius is 0.5, so player center cannot exceed x = 1.0!
	assert(tr.Location.x <= 1.01f);
	assert((kcc.CollisionFlags & CharacterCollisionFlags::Sides) != 0);

	std::cout << "[PASS] TestMoveAndSlideWallCollision\n";
}

static void TestSteepSlopeSliding()
{
	WorldContext context;
	Scene scene(context);

	// A steep inclined box with 60 deg pitch
	Entity slope = Entity::CreateEntity(&scene, "Slope");
	auto& slopeTr = slope.AddComponent<TransformComponent>();
	slopeTr.Location = glm::vec3(2.0f, 0.0f, 0.0f);
	// Rotate 60 degrees around Z axis
	slopeTr.Rotation = glm::angleAxis(glm::radians(60.0f), glm::vec3(0.0f, 0.0f, 1.0f));
	auto& box = slope.AddComponent<BoxCollider3DComponent>();
	box.HalfExtents = glm::vec3(2.0f, 0.2f, 2.0f);

	Entity player = Entity::CreateEntity(&scene, "Player");
	auto& tr = player.AddComponent<TransformComponent>();
	tr.Location = glm::vec3(0.0f, 0.0f, 0.0f);
	auto& kcc = player.AddComponent<CharacterControllerComponent>();
	kcc.SlopeLimitDeg = 45.0f; // Max walkable slope 45 deg

	// Move into the steep slope
	CharacterControllerSystem::MoveAndSlide(scene, (entt::entity)player, glm::vec3(2.0f, 0.0f, 0.0f), 0.016f);

	// Steep slope must set IsSliding = 1 and Grounded = 0
	assert(kcc.IsSliding == 1);
	assert(kcc.Grounded == 0);

	std::cout << "[PASS] TestSteepSlopeSliding\n";
}

static void TestStepUpAndStepDown()
{
	WorldContext context;
	Scene scene(context);

	// 1. Low step obstacle at x = 1.0: top surface at y = -0.3
	Entity step = Entity::CreateEntity(&scene, "Step");
	auto& stepTr = step.AddComponent<TransformComponent>();
	stepTr.Location = glm::vec3(1.0f, -0.4f, 0.0f);
	auto& stepBox = step.AddComponent<BoxCollider3DComponent>();
	stepBox.HalfExtents = glm::vec3(0.5f, 0.1f, 1.0f);

	// Lower floor after step at x = 2.5: top surface at y = -0.5
	Entity lowerFloor = Entity::CreateEntity(&scene, "LowerFloor");
	auto& floorTr = lowerFloor.AddComponent<TransformComponent>();
	floorTr.Location = glm::vec3(2.5f, -0.6f, 0.0f);
	auto& floorBox = lowerFloor.AddComponent<BoxCollider3DComponent>();
	floorBox.HalfExtents = glm::vec3(1.0f, 0.1f, 1.0f);

	Entity player = Entity::CreateEntity(&scene, "Player");
	auto& tr = player.AddComponent<TransformComponent>();
	tr.Location = glm::vec3(0.0f, 0.0f, 0.0f);
	auto& kcc = player.AddComponent<CharacterControllerComponent>();
	kcc.StepOffset = 0.3f; // Can step up/down to 0.3m
	auto& cap = player.AddComponent<CapsuleCollider3DComponent>();
	cap.Radius = 0.2f;
	cap.HalfHeight = 0.3f;

	// Move forward by 1.2m
	glm::vec3 moved = CharacterControllerSystem::MoveAndSlide(scene, (entt::entity)player, glm::vec3(1.2f, 0.0f, 0.0f), 0.016f);
	// Step-up succeeded: player moved forward onto the step and landed on top
	assert(tr.Location.x > 0.5f);

	// 2. Step down: start grounded on top of step, move off the edge onto lower floor
	kcc.Grounded = 1;
	glm::vec3 movedDown = CharacterControllerSystem::MoveAndSlide(scene, (entt::entity)player, glm::vec3(1.0f, 0.0f, 0.0f), 0.016f);
	// Should remain grounded via step-down snapping
	assert(kcc.Grounded == 1);

	std::cout << "[PASS] TestStepUpAndStepDown\n";
}

int main()
{
	Log::Init();
	std::cout << "Running WorldCharacterControllerTests...\n";

	TestProjectTangentAndCrease();
	TestCoyoteTimeAndCanJump();
	TestJumpBufferingAndUpdate();
	TestMoveAndSlideUnobstructed();
	TestMoveAndSlideWallCollision();
	TestSteepSlopeSliding();
	TestStepUpAndStepDown();

	std::cout << "All WorldCharacterControllerTests passed successfully.\n";
	return 0;
}
