#pragma once
#include "World/Renderer/Camera.h"
#include "World/Schema/Schema.h"
namespace World
{
	class SceneCamera :public Camera
	{
	public:

		enum class ProjectionType :int
		{
			Perspective = 0,
			Orthographic = 1
		};
		WE_ENUM_SCHEMA(World, ProjectionType, Int32)
			WE_ENUM_VALUE(Perspective);
			WE_ENUM_VALUE(Orthographic);
		WE_ENUM_END
	public:
		SceneCamera();
		SceneCamera(uint32_t width, uint32_t height, float zoom = 1.0f, float zNear = -1.0f, float zFar = 1.0f);

		~SceneCamera() = default;

		ProjectionType GetProjectionType() const { return m_ProjectionType; }
		void SetProjectionType(ProjectionType type) { m_ProjectionType = type; RecalculateProjection(); }

		void SetViewportSize(uint32_t width, uint32_t height);
		float GetAspectRatio() const { return m_AspectRatio; }
		// WUI 泛型检查器直接改字段后重算投影。
		void ApplyEdit() { RecalculateProjection(); }

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

		WE_SCHEMA_BODY(World, SceneCamera, Struct)
			WE_FIELD(m_ProjectionType, Enum, Of(ProjectionType), Group("Projection"));
			WE_FIELD(m_AspectRatio, Float, Transient);
			WE_FIELD(m_OrthographicZoom, Float, Group("Orthographic"));
			WE_FIELD(m_OrthographicNearClip, Float, Group("Orthographic"));
			WE_FIELD(m_OrthographicFarClip, Float, Group("Orthographic"));
			WE_FIELD(m_PerspectiveFOV, Float, Group("Perspective"));
			WE_FIELD(m_PerspectiveNearClip, Float, Group("Perspective"));
			WE_FIELD(m_PerspectiveFarClip, Float, Group("Perspective"));
		WE_SCHEMA_END
	};

}

