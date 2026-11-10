#pragma once

#include "World/Core/Export.h"
#include "World/Renderer/Camera.h"
#include "World/Events/MouseEvent.h"
#include "World/Core/Timestep.h"

#include <glm/glm.hpp>

namespace World
{
	enum class OrthographicAxis
	{
		Top = 0,     // 顶视: 视线朝 -Y, Up 为 -Z, Right 为 +X (看 XZ 平面)
		Bottom = 1,  // 底视: 视线朝 +Y, Up 为 +Z, Right 为 +X (看 XZ 平面)
		Front = 2,   // 前视: 视线朝 -Z, Up 为 +Y, Right 为 +X (看 XY 平面)
		Back = 3,    // 后视: 视线朝 +Z, Up 为 +Y, Right 为 -X (看 XY 平面)
		Right = 4,   // 右视: 视线朝 -X, Up 为 +Y, Right 为 -Z (看 ZY 平面)
		Left = 5,    // 左视: 视线朝 +X, Up 为 +Y, Right 为 +Z (看 ZY 平面)
	};

	class WLD_API EditorCamera : public Camera
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

		// 正交模式与 2D 视口支持
		void SetOrthographic(bool orthographic);
		bool IsOrthographic() const { return m_IsOrthographic; }
		void SetOrthographicSize(float size);
		float GetOrthographicSize() const { return m_OrthographicSize; }
		const glm::vec3& GetFocalPoint() const { return m_FocalPoint; }
		void SetFocalPoint(const glm::vec3& focalPoint);
		void SetOrthographicAxis(OrthographicAxis axis);
		OrthographicAxis GetOrthographicAxis() const { return m_OrthoAxis; }
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
		bool m_IsOrthographic = false;
		float m_OrthographicSize = 10.0f;
		OrthographicAxis m_OrthoAxis = OrthographicAxis::Front;
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
