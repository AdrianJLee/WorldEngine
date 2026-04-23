#include "wldpch.h"
#include "SceneCamera.h"

#include <glm/gtc/matrix_transform.hpp>

namespace World
{
	SceneCamera::SceneCamera()
	{
		RecalculateProjection();
	}

	SceneCamera::SceneCamera(uint32_t width, uint32_t height, float zoom, float zNear, float zFar)
	{
		m_AspectRatio = (float)width / (float)height;
		m_OrthographicNearClip = zNear;
		m_OrthographicFarClip = zFar;
		m_OrthographicZoom = zoom;
		RecalculateProjection();
	}
	void SceneCamera::SetOrthographic(float zoom, float zNear, float zFar)
	{
		m_OrthographicZoom = zoom;
		m_OrthographicNearClip = zNear;
		m_OrthographicFarClip = zFar;
		RecalculateProjection();
	}
	void SceneCamera::SetViewportSize(uint32_t width, uint32_t height)
	{
		m_AspectRatio = (float)width / (float)height;
		RecalculateProjection();
	}
	void SceneCamera::RecalculateProjection()
	{
		if (m_ProjectionType == ProjectionType::Perspective)
		{
			m_ProjectionMatrix = glm::perspective(glm::radians(m_PerspectiveFOV), m_AspectRatio, m_PerspectiveNearClip, m_PerspectiveFarClip);
		}
		else
		{
			float left = -m_AspectRatio * m_OrthographicZoom;
			float right = m_AspectRatio * m_OrthographicZoom;
			float bottom = -m_OrthographicZoom;
			float top = m_OrthographicZoom;

			m_ProjectionMatrix = glm::ortho(left, right, bottom, top, m_OrthographicNearClip, m_OrthographicFarClip);

		}
	}
}