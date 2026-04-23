#pragma once

#include "World/Renderer/OrthographicCamera.h"
#include "World/Core/Timestep.h"
#include "World/Events/Event.h"
#include "World/Events/MouseEvent.h"
#include "World/Events/ApplicationEvent.h"

#include <glm/glm.hpp>	

namespace World
{
	struct OrthographicCameraBounds
	{
		float Left, Right;
		float Bottom, Top;

		float GetWidth() { return Right - Left; }
		float GetHeight() { return Top - Bottom; }
	};

	class OrthographicCameraController
	{
	public:
		OrthographicCameraController(float aspectRatio, bool rotation = false);

		void OnUpdate(Timestep ts);
		void OnEvent(Event& event);
		void OnResize(float width, float height);
		OrthographicCamera& GetCamera() { return m_Camera; }
		const OrthographicCameraBounds& GetBounds() const { return m_Bounds; }
	private:
		bool OnMouseScrolled(MouseScrolledEvent& e);
		bool OnWindowResized(WindowResizeEvent& e);
	private:
		// 长宽比
		float m_AspectRatio;
		// 缩放等级
		float m_ZoomLevel = 1.0f;
		OrthographicCameraBounds m_Bounds;
		OrthographicCamera m_Camera;

		// 缩放速度
		float m_ZoomSpeed = 0.25f;
		float m_ZoomMin = 0.25f;

		bool m_Rotation;

		glm::vec3 m_CameraPosition = { 0.0f,0.0f,0.0f };
		float m_CameraSpeed = 10.0f;

		glm::vec3 m_CameraRotation = { 0.0f,0.0f,0.0f };
		float m_CameraRotationSpeed = 180.0f;
	};

}

