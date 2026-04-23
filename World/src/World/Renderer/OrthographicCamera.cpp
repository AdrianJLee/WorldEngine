#include "wldpch.h"
#include "OrthographicCamera.h"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>


namespace World
{
	OrthographicCamera::OrthographicCamera(float left, float right, float bottom, float top)
		:m_ProjectionMatrix(glm::ortho(left, right, bottom, top, -1.0f, 1.0f))
	{
		RecalculateViewMatrix();
	}
	OrthographicCamera::OrthographicCamera(float left, float right, float bottom, float top, float nearClip, float farClip)
		:m_ProjectionMatrix(glm::ortho(left, right, bottom, top, nearClip, farClip))
	{
		RecalculateViewMatrix();
	}
	void OrthographicCamera::SetProjection(float left, float right, float bottom, float top)
	{
		WLD_PROFILE_FUNCTION();

		m_ProjectionMatrix = glm::ortho(left, right, bottom, top, -1.0f, 1.0f);
		m_ViewProjectionMatrix = m_ProjectionMatrix * m_ViewMatrix;
	}
	void OrthographicCamera::RecalculateViewMatrix()
	{
		WLD_PROFILE_FUNCTION();

		// transform = translation * rotation * scale
		glm::mat4 transform =
			glm::translate(glm::mat4(1.0f), m_Position) *
			glm::mat4_cast(glm::quat(glm::radians(m_Rotation))) *
			glm::scale(glm::mat4(1.0f), glm::vec3(1.0f));

		// The view matrix is the inverse of the camera's transform matrix
		m_ViewMatrix = glm::inverse(transform);
		// The view-projection matrix is the product of the projection matrix and the view matrix
		m_ViewProjectionMatrix = m_ProjectionMatrix * m_ViewMatrix;
	}
}