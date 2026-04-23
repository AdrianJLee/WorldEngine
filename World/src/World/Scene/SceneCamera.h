#pragma once
#include "World/Renderer/Camera.h"
namespace World
{
	class SceneCamera :public Camera
	{
	public:
		enum class ProjectionType { Perspective = 0, Orthographic = 1 };
	public:
		SceneCamera();
		SceneCamera(uint32_t width, uint32_t height, float zoom = 1.0f, float zNear = -1.0f, float zFar = 1.0f);

		~SceneCamera() = default;

		ProjectionType GetProjectionType() const { return m_ProjectionType; }
		void SetProjectionType(ProjectionType type) { m_ProjectionType = type; RecalculateProjection(); }

		void SetViewportSize(uint32_t width, uint32_t height);
		float GetAspectRatio() const { return m_AspectRatio; }

		#pragma region Perspective

		void SetPerspectiveFOV(float fov) { m_PerspectiveFOV = fov; RecalculateProjection(); }
		float GetPerspectiveFOV() const { return m_PerspectiveFOV; }

		void SetPerspectiveNearClip(float zNear) { m_PerspectiveNearClip = zNear; RecalculateProjection(); }
		float GetPerspectiveNearClip() const { return m_PerspectiveNearClip; }

		void SetPerspectiveFarClip(float zFar) { m_PerspectiveFarClip = zFar; RecalculateProjection(); }
		float GetPerspectiveFarClip() const { return m_PerspectiveFarClip; }

		#pragma endregion

		#pragma region Orthographic

		void SetOrthographic(float zoom = 1.0f, float zNear = -1.0f, float zFar = 1.0f);

		void SetOrthographicZoom(float zoom) { m_OrthographicZoom = zoom; RecalculateProjection(); }
		float GetOrthographicZoom() const { return m_OrthographicZoom; }

		void SetOrthographicNearClip(float zNear) { m_OrthographicNearClip = zNear; RecalculateProjection(); }
		float GetOrthographicNearClip() const { return m_OrthographicNearClip; }

		void SetOrthographicFarClip(float zFar) { m_OrthographicFarClip = zFar; RecalculateProjection(); }
		float GetOrthographicFarClip() const { return m_OrthographicFarClip; }

		#pragma endregion

	private:
		void RecalculateProjection();
	private:
		// Common
		ProjectionType m_ProjectionType = ProjectionType::Orthographic;
		float m_AspectRatio = 1.0f;

		// Orthographic
		float m_OrthographicZoom = 1.0f;
		float m_OrthographicNearClip = -1.0f;
		float m_OrthographicFarClip = 1.0f;

		// Perspective
		float m_PerspectiveFOV = 45.0f;
		float m_PerspectiveNearClip = 0.1f;
		float m_PerspectiveFarClip = 100.0f;
	};

}

