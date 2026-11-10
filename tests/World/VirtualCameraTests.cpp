#include "World/Renderer/VirtualCameraSystem.h"
#include "World/Scene/Components.h"
#include "World/Scene/Scene.h"
#include "World/Scene/Entity.h"
#include "World/Core/Log.h"
#include "World/Core/WorldContext.h"

#include <cassert>
#include <cmath>
#include <iostream>

using namespace World;

static void TestSmoothDampAndTracking()
{
	WorldContext context;
	Scene scene(context);

	Entity target = Entity::CreateEntity(&scene, "Target");
	auto& targetTr = target.AddComponent<TransformComponent>();
	targetTr.Location = glm::vec3(10.0f, 0.0f, 0.0f);

	Entity cam = Entity::CreateEntity(&scene, "MainCamera");
	auto& camTr = cam.AddComponent<TransformComponent>();
	camTr.Location = glm::vec3(0.0f, 0.0f, 10.0f);
	cam.AddComponent<CameraComponent>();

	Entity vcam = Entity::CreateEntity(&scene, "VCam");
	auto& vcamComp = vcam.AddComponent<VirtualCameraComponent>();
	vcamComp.FollowTarget = static_cast<uint32_t>(static_cast<entt::entity>(target));
	vcamComp.FollowOffset = glm::vec3(0.0f, 0.0f, 10.0f);
	vcamComp.Damping = glm::vec3(0.1f);
	vcamComp.Priority = 20;

	for (int i = 0; i < 10; ++i)
	{
		VirtualCameraSystem::Update(scene, 0.016f);
	}

	assert(camTr.Location.x > 0.0f);
	assert(camTr.Location.x < 10.0f);

	std::cout << "[PASS] TestSmoothDampAndTracking\n";
}

static void TestScreenShakeTraumaDecay()
{
	VirtualCameraComponent vcam;
	vcam.ScreenShakeTrauma = 0.8f;
	vcam.ScreenShakeDecay = 2.0f;
	vcam.ScreenShakeFrequency = 15.0f;

	glm::vec3 shake1 = VirtualCameraSystem::EvaluateShakeOffset(vcam, 0.5f);
	assert(glm::length(shake1) > 0.0f);

	VirtualCameraSystem::AddTrauma(vcam, 0.5f);
	assert(vcam.ScreenShakeTrauma == 1.0f);

	WorldContext context;
	Scene scene(context);
	Entity vcamEnt = Entity::CreateEntity(&scene, "VCam");
	vcamEnt.AddComponent<VirtualCameraComponent>() = vcam;

	VirtualCameraSystem::Update(scene, 0.6f);
	auto& updatedVcam = vcamEnt.GetComponent<VirtualCameraComponent>();
	assert(updatedVcam.ScreenShakeTrauma == 0.0f);

	glm::vec3 shakeZero = VirtualCameraSystem::EvaluateShakeOffset(updatedVcam, 1.0f);
	assert(glm::length(shakeZero) == 0.0f);

	std::cout << "[PASS] TestScreenShakeTraumaDecay\n";
}

static void TestDeadZoneCalculation()
{
	glm::vec2 deadZone(0.2f, 0.2f);

	glm::vec2 inside = VirtualCameraSystem::ApplyDeadZone(glm::vec2(0.1f, -0.15f), deadZone);
	assert(inside.x == 0.0f && inside.y == 0.0f);

	glm::vec2 outside = VirtualCameraSystem::ApplyDeadZone(glm::vec2(0.5f, 0.0f), deadZone);
	assert(std::abs(outside.x - 0.3f) < 1e-5f);
	assert(outside.y == 0.0f);

	std::cout << "[PASS] TestDeadZoneCalculation\n";
}

int main()
{
	Log::Init();
	std::cout << "Running WorldVirtualCameraTests...\n";

	TestSmoothDampAndTracking();
	TestScreenShakeTraumaDecay();
	TestDeadZoneCalculation();

	std::cout << "All WorldVirtualCameraTests passed successfully.\n";
	return 0;
}
