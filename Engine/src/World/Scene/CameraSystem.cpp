#include "CameraSystem.h"
#include "World/Scene/Components.h"

namespace World
{
	void CameraSystem::SetViewportSize(CameraComponent& camera, uint32_t width, uint32_t height)
	{
		if (!camera.FixedAspectRatio && width && height)
		{
			camera.Camera.SetViewportSize(width, height);
		}
	}

	void CameraSystem::UpdateProjection(CameraComponent& camera)
	{
		camera.Camera.ApplyEdit();
	}

	void CameraSystem::UpdateAllCameras(entt::registry& registry, uint32_t viewportWidth, uint32_t viewportHeight)
	{
		if (!viewportWidth || !viewportHeight)
			return;

		for (const auto entity : registry.view<CameraComponent>())
		{
			auto& camera = registry.get<CameraComponent>(entity);
			SetViewportSize(camera, viewportWidth, viewportHeight);
		}
	}

	entt::entity CameraSystem::FindPrimaryCameraEntity(const entt::registry& registry)
	{
		for (const auto entity : registry.view<CameraComponent, TransformComponent>())
		{
			const auto& camera = registry.get<CameraComponent>(entity);
			if (camera.Primary)
				return entity;
		}
		return entt::null;
	}
}
