#include "wldpch.h"
#include "OrthographicCameraController.h"
#include "World/Core/Input.h"
#include "World/Core/KeyCodes.h"

namespace World
{
	OrthographicCameraController::OrthographicCameraController(float aspectRatio, bool rotation)
		:m_AspectRatio(aspectRatio),
		m_Bounds({ -m_AspectRatio * m_ZoomLevel, m_AspectRatio * m_ZoomLevel, -m_ZoomLevel, m_ZoomLevel }),
		m_Camera(m_Bounds.Left, m_Bounds.Right, m_Bounds.Bottom, m_Bounds.Top),
		m_Rotation(rotation)
	{}
	void OrthographicCameraController::OnUpdate(Timestep ts)
	{
		WLD_PROFILE_FUNCTION();

		if (Input::IsKeyPressed(KeyCodes::A))
			m_CameraPosition.x -= m_CameraSpeed * ts;
		else if (Input::IsKeyPressed(KeyCodes::D))
			m_CameraPosition.x += m_CameraSpeed * ts;

		if (Input::IsKeyPressed(KeyCodes::W))
			m_CameraPosition.y += m_CameraSpeed * ts;
		else if (Input::IsKeyPressed(KeyCodes::S))
			m_CameraPosition.y -= m_CameraSpeed * ts;

		if (m_Rotation)
		{
			if (Input::IsKeyPressed(KeyCodes::Q))
				m_CameraRotation.z += m_CameraRotationSpeed * ts;
			else if (Input::IsKeyPressed(KeyCodes::E))
				m_CameraRotation.z -= m_CameraRotationSpeed * ts;

			m_Camera.SetRotation(m_CameraRotation);
		}

		m_Camera.SetPosition(m_CameraPosition);

		m_CameraSpeed = m_ZoomLevel;
	}
	void OrthographicCameraController::OnResize(float width, float height)
	{
		m_AspectRatio = width / height;
		m_Bounds = { -m_AspectRatio * m_ZoomLevel, m_AspectRatio * m_ZoomLevel, -m_ZoomLevel, m_ZoomLevel };
		m_Camera.SetProjection(m_Bounds.Left, m_Bounds.Right, m_Bounds.Bottom, m_Bounds.Top);
	}
	void OrthographicCameraController::OnEvent(Event& event)
	{
		WLD_PROFILE_FUNCTION();
		EventDispatcher dispather(event);
		dispather.Dispatch<MouseScrolledEvent>(WLD_BIND_EVENT_FN(OrthographicCameraController::OnMouseScrolled));
		dispather.Dispatch<WindowResizeEvent>(WLD_BIND_EVENT_FN(OrthographicCameraController::OnWindowResized));
	}
	bool OrthographicCameraController::OnMouseScrolled(MouseScrolledEvent& e)
	{
		m_ZoomLevel -= e.GetYOffset() * m_ZoomSpeed;
		m_ZoomLevel = std::max(m_ZoomLevel, m_ZoomMin);
		m_Bounds = { -m_AspectRatio * m_ZoomLevel, m_AspectRatio * m_ZoomLevel, -m_ZoomLevel, m_ZoomLevel };
		m_Camera.SetProjection(m_Bounds.Left, m_Bounds.Right, m_Bounds.Bottom, m_Bounds.Top);
		return false;
	}
	bool OrthographicCameraController::OnWindowResized(WindowResizeEvent& e)
	{
		OnResize(e.GetWidth(), e.GetHeight());
		return false;
	}
}