#include "wldpch.h"
#include "World/Renderer/EditorCamera.h"

#include "World/Core/Input.h"
#include "World/Core/KeyCodes.h"
#include "World/Core/MouseCodes.h"

#include <glm/gtc/matrix_transform.hpp>
#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtx/quaternion.hpp>

namespace World
{
	static void GetOrthoAxisBasis(OrthographicAxis axis, glm::vec3* forward, glm::vec3* up, glm::vec3* right)
	{
		switch (axis)
		{
		case OrthographicAxis::Top:
			if (forward) *forward = { 0.0f, -1.0f, 0.0f };
			if (up)      *up      = { 0.0f, 0.0f, -1.0f };
			if (right)   *right   = { 1.0f, 0.0f, 0.0f };
			break;
		case OrthographicAxis::Bottom:
			if (forward) *forward = { 0.0f, 1.0f, 0.0f };
			if (up)      *up      = { 0.0f, 0.0f, 1.0f };
			if (right)   *right   = { 1.0f, 0.0f, 0.0f };
			break;
		case OrthographicAxis::Front:
			if (forward) *forward = { 0.0f, 0.0f, -1.0f };
			if (up)      *up      = { 0.0f, 1.0f, 0.0f };
			if (right)   *right   = { 1.0f, 0.0f, 0.0f };
			break;
		case OrthographicAxis::Back:
			if (forward) *forward = { 0.0f, 0.0f, 1.0f };
			if (up)      *up      = { 0.0f, 1.0f, 0.0f };
			if (right)   *right   = { -1.0f, 0.0f, 0.0f };
			break;
		case OrthographicAxis::Right:
			if (forward) *forward = { -1.0f, 0.0f, 0.0f };
			if (up)      *up      = { 0.0f, 1.0f, 0.0f };
			if (right)   *right   = { 0.0f, 0.0f, -1.0f };
			break;
		case OrthographicAxis::Left:
			if (forward) *forward = { 1.0f, 0.0f, 0.0f };
			if (up)      *up      = { 0.0f, 1.0f, 0.0f };
			if (right)   *right   = { 0.0f, 0.0f, 1.0f };
			break;
		}
	}

	EditorCamera::EditorCamera(float fov, float aspectRatio, float nearClip, float farClip)
		:m_Fov(fov), m_AspectRatio(aspectRatio), m_NearClip(nearClip), m_FarClip(farClip),
		Camera(glm::perspective(glm::radians(fov), aspectRatio, nearClip, farClip))
	{
		UpdateView();
	}
	void EditorCamera::OnUpdate(Timestep ts)
	{
		if (Input::IsKeyPressed(KeyCodes::LeftAlt))
		{
			// 获取当前鼠标位置，并计算与初始位置的差值，乘以一个缩放因子来调整灵敏度
			glm::vec2 mouse { Input::GetMouseX(), Input::GetMouseY() };
			glm::vec2 delta = (mouse - m_InitialMousePosition) * 0.003f;
			m_InitialMousePosition = mouse;


			if (Input::IsMouseButtonPressed(MouseCodes::ButtonMiddle))
			{
				MousePan(delta);
			}
			else if (Input::IsMouseButtonPressed(MouseCodes::ButtonLeft))
			{
				MouseRotate(delta);
			}
			else if (Input::IsMouseButtonPressed(MouseCodes::ButtonRight))
			{
				MouseZoom(delta.y);
			}
			UpdateView();
		}
		//else if (Input::IsMouseButtonPressed(MouseCodes::ButtonRight))
		//{
		//	// TODO: 鼠标右键时可以自由移动旋转控制,方向错乱

		//	glm::vec3 delta = glm::vec3(0.0f);

		//	if (Input::IsKeyPressed(KeyCodes::W))
		//	{
		//		delta.y += 10.0f;
		//	}
		//	if (Input::IsKeyPressed(KeyCodes::S))
		//	{
		//		delta.y -= 10.0f;
		//	}
		//	if (Input::IsKeyPressed(KeyCodes::A))
		//	{
		//		delta.x -= 10.0f;
		//	}
		//	if (Input::IsKeyPressed(KeyCodes::D))
		//	{
		//		delta.x += 10.0f;
		//	}
		//	if (Input::IsKeyPressed(KeyCodes::LeftControl))
		//	{
		//		delta.z -= 10.0f;

		//	}
		//	if (Input::IsKeyPressed(KeyCodes::Space))
		//	{
		//		delta.z += 20.0f;
		//	}
		//	MousePan(delta);
		//	UpdateView();
		//}

	}
	void EditorCamera::OnEvent(Event& e)
	{
		EventDispatcher dispatcher(e);
		dispatcher.Dispatch<MouseScrolledEvent>(WLD_BIND_EVENT_FN(EditorCamera::OnMouseScroll));
	}
	glm::vec3 EditorCamera::GetUpDirection() const
	{
		if (m_IsOrthographic)
		{
			glm::vec3 up;
			GetOrthoAxisBasis(m_OrthoAxis, nullptr, &up, nullptr);
			return up;
		}
		return glm::rotate(GetOrientation(), glm::vec3(0.0f, 1.0f, 0.0f));
	}
	glm::vec3 EditorCamera::GetRightDirection() const
	{
		if (m_IsOrthographic)
		{
			glm::vec3 right;
			GetOrthoAxisBasis(m_OrthoAxis, nullptr, nullptr, &right);
			return right;
		}
		return glm::rotate(GetOrientation(), glm::vec3(1.0f, 0.0f, 0.0f));
	}
	glm::vec3 EditorCamera::GetForwardDirection() const
	{
		if (m_IsOrthographic)
		{
			glm::vec3 forward;
			GetOrthoAxisBasis(m_OrthoAxis, &forward, nullptr, nullptr);
			return forward;
		}
		return glm::rotate(GetOrientation(), glm::vec3(0.0f, 0.0f, -1.0f));
	}
	glm::quat EditorCamera::GetOrientation() const
	{
		return glm::quat(glm::vec3(-m_Pitch, -m_Yaw, 0.0f));
	}
	void EditorCamera::SetOrthographic(bool orthographic)
	{
		m_IsOrthographic = orthographic;
		if (m_IsOrthographic)
		{
			m_Pitch = 0.0f;
			m_Yaw = 0.0f;
		}
		UpdateProjection();
		UpdateView();
	}

	void EditorCamera::SetOrthographicSize(float size)
	{
		m_OrthographicSize = std::clamp(size, 0.05f, 50000.0f);
		UpdateProjection();
	}

	void EditorCamera::SetFocalPoint(const glm::vec3& focalPoint)
	{
		m_FocalPoint = focalPoint;
		UpdateView();
	}

	void EditorCamera::SetOrthographicAxis(OrthographicAxis axis)
	{
		m_OrthoAxis = axis;
		if (m_IsOrthographic)
		{
			UpdateView();
		}
	}

	void EditorCamera::UpdateProjection()
	{
		m_AspectRatio = m_ViewportWidth / std::max(m_ViewportHeight, 1.0f);
		if (m_IsOrthographic)
		{
			const float orthoH = m_OrthographicSize;
			const float orthoW = orthoH * m_AspectRatio;
			m_ProjectionMatrix = glm::ortho(
				-orthoW * 0.5f, orthoW * 0.5f,
				-orthoH * 0.5f, orthoH * 0.5f,
				-1000.0f, 1000.0f
			);
		}
		else
		{
			m_ProjectionMatrix = glm::perspective(glm::radians(m_Fov), m_AspectRatio, m_NearClip, m_FarClip);
		}
	}

	void EditorCamera::UpdateView()
	{
		if (m_IsOrthographic)
		{
			m_Pitch = 0.0f;
			m_Yaw = 0.0f;
			glm::vec3 forward, up, right;
			GetOrthoAxisBasis(m_OrthoAxis, &forward, &up, &right);
			m_Position = m_FocalPoint - forward * m_Distance;
			m_ViewMatrix = glm::lookAt(m_Position, m_FocalPoint, up);
			m_Transform = glm::inverse(m_ViewMatrix);
		}
		else
		{
			m_Position = CalculatePosition();
			glm::quat orientation = GetOrientation();
			m_Transform = glm::translate(glm::mat4(1.0f), m_Position) * glm::toMat4(orientation);
			m_ViewMatrix = glm::inverse(m_Transform);
		}
	}

	bool EditorCamera::OnMouseScroll(MouseScrolledEvent& e)
	{
		float delta = e.GetYOffset() * 0.1f;
		MouseZoom(delta);
		UpdateView();
		return false;
	}

	void EditorCamera::MousePan(const glm::vec2& delta)
	{
		if (m_IsOrthographic)
		{
			const float orthoW = m_OrthographicSize * m_AspectRatio;
			const float unitsPerNormX = orthoW * 0.5f;
			const float unitsPerNormY = m_OrthographicSize * 0.5f;
			m_FocalPoint += -GetRightDirection() * (delta.x * unitsPerNormX * 10.0f);
			m_FocalPoint += GetUpDirection() * (delta.y * unitsPerNormY * 10.0f);
			UpdateView();
			return;
		}
		auto [xSpeed, ySpeed] = PanSpeed();
		m_FocalPoint += -GetRightDirection() * delta.x * xSpeed * m_Distance;
		m_FocalPoint += GetUpDirection() * delta.y * ySpeed * m_Distance;
	}

	void EditorCamera::MousePan(const glm::vec3& delta)
	{
		if (m_IsOrthographic)
		{
			m_FocalPoint.x += delta.x;
			m_FocalPoint.y += delta.y;
			return;
		}
		const glm::vec3& speed = glm::vec3 { 0.3 };
		m_FocalPoint += -GetRightDirection() * delta.x * speed.x * m_Distance;
		m_FocalPoint += GetForwardDirection() * delta.y * speed.y * m_Distance;
		m_FocalPoint += GetUpDirection() * delta.z * speed.z * m_Distance;
	}

	void EditorCamera::MouseRotate(const glm::vec2& delta)
	{
		if (m_IsOrthographic)
			return;
		float yawSign = GetUpDirection().y < 0 ? -1.0f : 1.0f;
		m_Yaw += yawSign * delta.x * RotationSpeed();
		m_Pitch += delta.y * RotationSpeed();
	}

	void EditorCamera::MouseZoom(float delta)
	{
		if (m_IsOrthographic)
		{
			m_OrthographicSize -= delta * m_OrthographicSize * 0.2f;
			m_OrthographicSize = std::clamp(m_OrthographicSize, 0.05f, 50000.0f);
			UpdateProjection();
			return;
		}
		m_Distance -= delta * ZoomSpeed();
		if (m_Distance < 1.0f)
		{
			m_FocalPoint += GetForwardDirection();
			m_Distance = 1.0f;
		}
	}
	glm::vec3 EditorCamera::CalculatePosition() const
	{
		return m_FocalPoint - GetForwardDirection() * m_Distance;
	}
	std::pair<float, float> EditorCamera::PanSpeed() const
	{
		float x = std::min(m_ViewportWidth / 1000.0f, 2.4f); // max = 2.4f
		float xFactor = 0.0366f * (x * x) - 0.1778f * x + 0.3021f;

		float y = std::min(m_ViewportHeight / 1000.0f, 2.4f); // max = 2.4f
		float yFactor = 0.0366f * (y * y) - 0.1778f * y + 0.3021f;
		return { xFactor,yFactor };
	}
	glm::vec3 EditorCamera::PanSeed() const
	{
		float x = std::min(m_ViewportWidth / 1000.0f, 2.4f); // max = 2.4f
		float xFactor = 0.0366f * (x * x) - 0.1778f * x + 0.3021f;

		float y = std::min(m_ViewportHeight / 1000.0f, 2.4f); // max = 2.4f
		float yFactor = 0.0366f * (y * y) - 0.1778f * y + 0.3021f;

		float z = std::min(m_ViewportHeight / 1000.0f, 2.4f); // max = 2.4f
		float zFactor = 0.0366f * (z * z) - 0.1778f * z + 0.3021f;
		return glm::vec3(xFactor, yFactor, zFactor);
	}
	float EditorCamera::RotationSpeed() const
	{
		return 0.8f;
	}
	float EditorCamera::ZoomSpeed() const
	{
		float distance = m_Distance * 0.2f;
		distance = std::max(distance, 0.0f);
		float speed = distance * distance;
		speed = std::min(speed, 100.0f); // max speed = 100
		return speed;
	}
}