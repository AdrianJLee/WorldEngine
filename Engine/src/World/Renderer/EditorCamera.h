#pragma once
#include "World/Renderer/Camera.h"
#include "World/Events/MouseEvent.h"
#include "World/Core/Timestep.h"

#include <glm/glm.hpp>

namespace World
{
	class EditorCamera :public Camera
	{
	public:
		EditorCamera() = default;
		EditorCamera(float fov, float aspectRatio, float nearClip, float farClip);

		void OnUpdate(Timestep ts);
		void OnEvent(Event& e);

		inline float GetDistance() const { return m_Distance; }
		inline void SetDistance(float distance) { m_Distance = distance; }

		inline void SetViewportSize(float width, float height) { m_ViewportWidth = width; m_ViewportHeight = height; UpdateProjection(); }

		const glm::mat4& GetViewMatrix() const { return m_ViewMatrix; }
		glm::mat4 GetViewProjection() const { return m_ProjectionMatrix * m_ViewMatrix; }
		glm::mat4 GetTransform() const { return m_Transform; }

		glm::vec3 GetUpDirection() const;
		glm::vec3 GetRightDirection() const;
		glm::vec3 GetForwardDirection() const;
		const glm::vec3& GetPosition() const { return m_Position; }
		// 返回一个四元数，表示摄像机的旋转
		glm::quat GetOrientation() const;

		float GetPitch() const { return m_Pitch; }
		float GetYaw() const { return m_Yaw; }
		// Gizmo 需要"世界单位 → 屏幕像素"的换算(依赖 FOV),因此暴露只读访问。
		float GetFov() const { return m_Fov; }
	private:

		void UpdateProjection();
		// 根据摄像机的位置、旋转和焦点计算视图矩阵
		void UpdateView();

		bool OnMouseScroll(MouseScrolledEvent& e);
		void MousePan(const glm::vec2& delta);
		void MousePan(const glm::vec3& delta);
		void MouseRotate(const glm::vec2& delta);
		void MouseZoom(float delta);

		glm::vec3 CalculatePosition() const;

		// 计算移动速度，距离越远速度越快
		std::pair<float, float> PanSpeed() const;
		glm::vec3 PanSeed() const;
		float RotationSpeed() const;
		// 计算缩放速度，距离越远速度越快
		float ZoomSpeed() const;
	private:

		float m_Fov = 45.0f, m_AspectRatio = 16.0f / 9.0f, m_NearClip = 0.1f, m_FarClip = 1000.0f;
		glm::mat4 m_Transform { 1.0f };
		glm::mat4 m_ViewMatrix { 1.0f };
		// 摄像机位置
		glm::vec3 m_Position { 0.0f, 0.0f, 0.0f };

		// 焦点
		glm::vec3 m_FocalPoint { 0.0f, 0.0f, 0.0f };
		// 鼠标初始位置
		glm::vec2 m_InitialMousePosition { 0.0f, 0.0f };
		// 摄像机与焦点的距离
		float m_Distance = 10.0f;
		// 绕X轴旋转的角度，绕Y轴旋转的角度
		float m_Pitch = 0.0f, m_Yaw = 0.0f;

		float m_ViewportWidth = 1280.0f, m_ViewportHeight = 720.0f;
	};

}
