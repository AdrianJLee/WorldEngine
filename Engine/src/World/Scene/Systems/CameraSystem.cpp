#include "World/Scene/Systems/CameraSystem.h"
#include "World/Scene/Components.h"
#include "World/Core/Log.h"

#include <glm/gtc/matrix_transform.hpp>

#include <cmath>
#include <cstring>

namespace World
{
	namespace
	{
		constexpr float kDefaultPerspectiveFOV = 45.0f;
		constexpr float kDefaultPerspectiveNear = 0.1f;
		constexpr float kDefaultPerspectiveFar = 100.0f;

		// 进程级投影重建计数(测试/诊断用):只在 EnsureView 真的重算时 +1。
		uint64_t s_ProjectionRebuildCount = 0;

		bool IsFinite(float value)
		{
			return std::isfinite(value);
		}

		float SafePositive(float value, float fallback)
		{
			return (IsFinite(value) && value > 0.0f) ? value : fallback;
		}

		// 有效宽高比:FixedAspectRatio 时取组件里的权威值,否则跟视口;两者都退化时回退 1:1。
		float EffectiveAspect(const CameraSettings& settings, bool fixedAspectRatio,
			uint32_t viewportWidth, uint32_t viewportHeight)
		{
			float aspect = 0.0f;
			if (fixedAspectRatio)
				aspect = settings.m_AspectRatio;
			else if (viewportWidth && viewportHeight)
				aspect = static_cast<float>(viewportWidth) / static_cast<float>(viewportHeight);

			if (!(IsFinite(aspect) && aspect > 0.0f))
				aspect = SafePositive(settings.m_AspectRatio, 1.0f);
			return aspect;
		}

		bool SameParams(const CameraSettings& left, const CameraSettings& right)
		{
			// sizeof==32 且无填充(SceneCamera.h 的 static_assert),可以逐字节比较。
			return std::memcmp(&left, &right, sizeof(CameraSettings)) == 0;
		}
	}

	void CameraSystem::ClampSettings(CameraSettings& settings, float aspectRatio)
	{
		const float safeAspect = SafePositive(aspectRatio, 1.0f);

		if (settings.m_ProjectionType != CameraSettings::ProjectionType::Perspective &&
			settings.m_ProjectionType != CameraSettings::ProjectionType::Orthographic)
		{
			settings.m_ProjectionType = CameraSettings::ProjectionType::Orthographic;
		}

		settings.m_AspectRatio = SafePositive(settings.m_AspectRatio, safeAspect);

		// Perspective:FOV ∈ (0,180),near>0,far>near。
		settings.m_PerspectiveFOV = (IsFinite(settings.m_PerspectiveFOV) &&
			settings.m_PerspectiveFOV > 0.0f && settings.m_PerspectiveFOV < 180.0f)
			? settings.m_PerspectiveFOV : kDefaultPerspectiveFOV;
		settings.m_PerspectiveNearClip = SafePositive(settings.m_PerspectiveNearClip, kDefaultPerspectiveNear);
		settings.m_PerspectiveFarClip = SafePositive(settings.m_PerspectiveFarClip, kDefaultPerspectiveFar);
		if (!(settings.m_PerspectiveFarClip > settings.m_PerspectiveNearClip))
			settings.m_PerspectiveFarClip = settings.m_PerspectiveNearClip + 1.0f;

		// Orthographic:zoom>0,far>near(near/far 允许为负,与原语义一致)。
		settings.m_OrthographicZoom = SafePositive(settings.m_OrthographicZoom, 1.0f);
		if (!IsFinite(settings.m_OrthographicNearClip))
			settings.m_OrthographicNearClip = -1.0f;
		if (!IsFinite(settings.m_OrthographicFarClip))
			settings.m_OrthographicFarClip = 1.0f;
		if (!(settings.m_OrthographicFarClip > settings.m_OrthographicNearClip))
			settings.m_OrthographicFarClip = settings.m_OrthographicNearClip + 1.0f;
	}

	const Camera& CameraSystem::EnsureView(entt::registry& registry, entt::entity entity,
		uint32_t viewportWidth, uint32_t viewportHeight)
	{
		static const Camera s_FallbackCamera { glm::mat4(1.0f) };

		if (!registry.valid(entity))
			return s_FallbackCamera;

		auto* component = registry.try_get<CameraComponent>(entity);
		auto* view = registry.try_get<CameraViewComponent>(entity);
		if (!component || !view)
		{
			// 结构写只允许在提交点做,这里绝不补建 ⇒ 兜底并只警告一次。
			static bool s_WarnedMissingView = false;
			if (!s_WarnedMissingView)
			{
				s_WarnedMissingView = true;
				WLD_CORE_WARN("CameraSystem::EnsureView: entity {0} has no CameraComponent/CameraViewComponent; using identity projection",
					static_cast<uint32_t>(entity));
			}
			return s_FallbackCamera;
		}

		const bool fixedAspectRatio = component->FixedAspectRatio;
		const float aspect = EffectiveAspect(component->Camera, fixedAspectRatio, viewportWidth, viewportHeight);

		CameraSettings params = component->Camera;
		ClampSettings(params, aspect);

		const bool viewportChanged = view->LastViewportWidth != viewportWidth ||
			view->LastViewportHeight != viewportHeight;
		const bool aspectChanged = view->LastAspectRatio != aspect;
		const bool paramsChanged = !SameParams(params, view->LastParams);

		if (!view->Valid || paramsChanged || aspectChanged || viewportChanged)
		{
			view->View = Camera(ComputeProjection(params, aspect));
			view->LastParams = params;
			view->LastAspectRatio = aspect;
			view->LastViewportWidth = viewportWidth;
			view->LastViewportHeight = viewportHeight;
			view->Valid = true;
			++s_ProjectionRebuildCount;
		}

		return view->View;
	}

	void CameraSystem::UpdateAllCameras(entt::registry& registry, uint32_t width, uint32_t height)
	{
		if (!width || !height)
			return;

		for (const auto entity : registry.view<CameraComponent>())
			EnsureView(registry, entity, width, height);
	}

	entt::entity CameraSystem::FindPrimaryCameraEntity(const entt::registry& registry)
	{
		for (const auto entity : registry.view<CameraComponent, TransformComponent>())
			if (registry.get<CameraComponent>(entity).Primary)
				return entity;
		return entt::null;
	}

	glm::mat4 CameraSystem::ComputeProjection(const CameraSettings& settings, float aspectRatio)
	{
		const float aspect = SafePositive(aspectRatio, 1.0f);

		if (settings.m_ProjectionType == CameraSettings::ProjectionType::Perspective)
		{
			return glm::perspective(glm::radians(settings.m_PerspectiveFOV), aspect,
				settings.m_PerspectiveNearClip, settings.m_PerspectiveFarClip);
		}

		const float zoom = settings.m_OrthographicZoom;
		return glm::ortho(-aspect * zoom, aspect * zoom, -zoom, zoom,
			settings.m_OrthographicNearClip, settings.m_OrthographicFarClip);
	}

	uint64_t CameraSystem::ProjectionRebuildCount()
	{
		return s_ProjectionRebuildCount;
	}
}
