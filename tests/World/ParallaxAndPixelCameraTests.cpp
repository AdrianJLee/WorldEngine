#include "World/Scene/Components.h"
#include "World/Scene/Scene.h"
#include "World/Scene/Entity.h"
#include "World/Scene/Systems/CameraSystem.h"
#include "World/Core/Log.h"

#include <iostream>
#include <cassert>
#include <cmath>

using namespace World;

static void TestPixelPerfectCameraMath()
{
	static_assert(sizeof(PixelPerfectCameraComponent) == 24, "PixelPerfectCameraComponent size contract");
	static_assert(std::is_trivially_copyable_v<PixelPerfectCameraComponent>, "PixelPerfectCameraComponent trivially copyable");

	entt::registry registry;
	const auto entity = registry.create();

	auto& cam = registry.emplace<CameraComponent>(entity);
	auto& view = registry.emplace<CameraViewComponent>(entity);
	auto& pixelCam = registry.emplace<PixelPerfectCameraComponent>(entity);

	pixelCam.ReferenceResolution = { 320, 180 };
	pixelCam.PixelsPerUnit = 16;
	pixelCam.SnapToPixelGrid = true;

	// Case 1: 1920x1080 (16:9 full HD) -> 1920/320 = 6, 1080/180 = 6 -> scale = 6x exact
	{
		CameraSystem::EnsureView(registry, entity, 1920, 1080);
		assert(pixelCam.CurrentScale == 6);
		// orthoZoom half-height = (1080 * 0.5) / (16 * 6) = 540 / 96 = 5.625 world units
		assert(std::abs(cam.Camera.m_OrthographicZoom - 5.625f) < 1e-4f);
		assert(cam.Camera.m_ProjectionType == CameraSettings::ProjectionType::Orthographic);
	}

	// Case 2: 1280x720 (16:9 HD) -> 1280/320 = 4, 720/180 = 4 -> scale = 4x exact
	{
		CameraSystem::EnsureView(registry, entity, 1280, 720);
		assert(pixelCam.CurrentScale == 4);
		// orthoZoom half-height = (720 * 0.5) / (16 * 4) = 360 / 64 = 5.625 world units
		assert(std::abs(cam.Camera.m_OrthographicZoom - 5.625f) < 1e-4f);
	}

	// Case 3: 800x600 (4:3 SVGA) -> min(800/320, 600/180) = min(2, 3) = 2x integer scale
	{
		CameraSystem::EnsureView(registry, entity, 800, 600);
		assert(pixelCam.CurrentScale == 2);
	}

	// Case 4: Pixel snap unit quantization
	{
		const float ppu = 16.0f;
		const float scale = 4.0f;
		const float pixelSizeWorld = 1.0f / (ppu * scale); // 1 / 64 = 0.015625
		const float rawCamX = 3.14159f;
		const float snappedX = std::round(rawCamX / pixelSizeWorld) * pixelSizeWorld;
		// snappedX must be an integer multiple of pixelSizeWorld
		const float rem = std::fmod(snappedX, pixelSizeWorld);
		assert(std::abs(rem) < 1e-5f || std::abs(rem - pixelSizeWorld) < 1e-5f);
	}

	std::cout << "[PASS] TestPixelPerfectCameraMath\n";
}

static void TestParallaxLayerMath()
{
	static_assert(sizeof(ParallaxLayerComponent) == 32, "ParallaxLayerComponent size contract");
	static_assert(std::is_trivially_copyable_v<ParallaxLayerComponent>, "ParallaxLayerComponent trivially copyable");

	ParallaxLayerComponent sky;
	sky.ScrollFactor = { 0.0f, 0.0f }; // Sky remains completely stationary with camera
	sky.Offset = { 0.0f, 2.0f };

	ParallaxLayerComponent mountains;
	mountains.ScrollFactor = { 0.2f, 0.1f }; // Distant mountains scroll at 20% horizontal speed

	ParallaxLayerComponent foreground;
	foreground.ScrollFactor = { 1.2f, 1.0f }; // Close foreground scrolls faster than camera

	const glm::vec2 camPos { 100.0f, 20.0f };

	// Parallax offset formula: (camPos * (1.0f - factor)) + offset
	const glm::vec2 skyOffset = camPos * (glm::vec2(1.0f) - sky.ScrollFactor) + sky.Offset;
	assert(std::abs(skyOffset.x - 100.0f) < 1e-4f);
	assert(std::abs(skyOffset.y - 22.0f) < 1e-4f);

	const glm::vec2 mtnOffset = camPos * (glm::vec2(1.0f) - mountains.ScrollFactor) + mountains.Offset;
	assert(std::abs(mtnOffset.x - 80.0f) < 1e-4f); // 100 * 0.8 = 80
	assert(std::abs(mtnOffset.y - 18.0f) < 1e-4f); // 20 * 0.9 = 18

	const glm::vec2 fgOffset = camPos * (glm::vec2(1.0f) - foreground.ScrollFactor) + foreground.Offset;
	assert(std::abs(fgOffset.x - (-20.0f)) < 1e-4f); // 100 * -0.2 = -20
	assert(std::abs(fgOffset.y - 0.0f) < 1e-4f);

	std::cout << "[PASS] TestParallaxLayerMath\n";
}

int main()
{
	Log::Init();
	std::cout << "Running WorldParallaxAndPixelCameraTests...\n";

	TestPixelPerfectCameraMath();
	TestParallaxLayerMath();

	std::cout << "All WorldParallaxAndPixelCameraTests passed successfully.\n";
	return 0;
}
