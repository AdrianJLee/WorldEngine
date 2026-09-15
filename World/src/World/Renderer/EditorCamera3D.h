#pragma once

#include "World/Core/Export.h"

#include <glm/glm.hpp>

namespace World
{
	// 3D 编辑器相机(P1b D1):目标点 + 距离 + yaw/pitch 的轨道模型,兼作飞行相机。
	//
	// 约定:
	//  - yaw=0、pitch=0 时相机位于 Target 的 -Z 侧,视线指向 Target;
	//  - 投影矩阵默认按 NDC +Y 向上编写;flipY=true 时对第 0..3 列的 Y 分量取反,
	//    与 SceneRenderer 给 Vulkan 后端做的适配完全一致。
	class WLD_API EditorCamera3D
	{
	public:
		EditorCamera3D() = default;
		EditorCamera3D(float fovDegrees, float aspectRatio, float nearClip, float farClip, float distance);

		// ---- 操作 ----
		void Orbit(float deltaYawDegrees, float deltaPitchDegrees);
		void Pan(float deltaRight, float deltaUp);
		void Dolly(float deltaDistance);
		void FocusOn(const glm::vec3& point, float distance);
		// 飞行:沿相机前/右/上平移(目标点与相机一起移动)。
		void Fly(float forward, float right, float up);

		void SetViewportSize(uint32_t width, uint32_t height);
		void SetTarget(const glm::vec3& target) { m_Target = target; }
		void SetDistance(float distance);
		void SetFOV(float fovDegrees) { m_FOV = fovDegrees; }
		void SetClipPlanes(float nearClip, float farClip);
		void SetYawPitch(float yawDegrees, float pitchDegrees);

		// ---- 查询 ----
		const glm::vec3& GetTarget() const { return m_Target; }
		float GetDistance() const { return m_Distance; }
		float GetYaw() const { return m_YawDegrees; }
		float GetPitch() const { return m_PitchDegrees; }
		float GetFOV() const { return m_FOV; }
		float GetAspectRatio() const { return m_Aspect; }
		float GetNearClip() const { return m_Near; }
		float GetFarClip() const { return m_Far; }

		glm::vec3 GetPosition() const;
		glm::vec3 GetForward() const;
		glm::vec3 GetRight() const;
		glm::vec3 GetUp() const;

		glm::mat4 GetViewMatrix() const;
		glm::mat4 GetProjectionMatrix(bool flipY = false) const;
		glm::mat4 GetViewProjectionMatrix(bool flipY = false) const;

		// 供拾取使用:屏幕 NDC → 世界射线的逆矩阵。
		glm::mat4 GetInverseViewProjectionMatrix(bool flipY = false) const;

	private:
		void ClampPitch();

		float m_FOV = 60.0f;
		float m_Aspect = 16.0f / 9.0f;
		float m_Near = 0.1f;
		float m_Far = 1000.0f;
		glm::vec3 m_Target { 0.0f, 0.0f, 0.0f };
		float m_Distance = 10.0f;
		float m_YawDegrees = 0.0f;
		float m_PitchDegrees = 0.0f;
	};
}
