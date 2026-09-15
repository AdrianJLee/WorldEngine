#include "wldpch.h"
#include "World/Renderer/EditorCamera3D.h"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>

namespace World
{
	namespace
	{
		constexpr float kMaxPitchDegrees = 89.0f;
		constexpr float kMinDistance = 0.01f;
		constexpr float kMaxDistance = 100000.0f;
	}

	EditorCamera3D::EditorCamera3D(float fovDegrees, float aspectRatio, float nearClip, float farClip,
		float distance)
		: m_FOV(fovDegrees), m_Aspect(aspectRatio), m_Near(nearClip), m_Far(farClip), m_Distance(distance)
	{
	}

	void EditorCamera3D::ClampPitch()
	{
		m_PitchDegrees = std::clamp(m_PitchDegrees, -kMaxPitchDegrees, kMaxPitchDegrees);
	}

	void EditorCamera3D::Orbit(float deltaYawDegrees, float deltaPitchDegrees)
	{
		m_YawDegrees += deltaYawDegrees;
		m_PitchDegrees += deltaPitchDegrees;
		ClampPitch();
	}

	void EditorCamera3D::Pan(float deltaRight, float deltaUp)
	{
		m_Target += GetRight() * deltaRight + GetUp() * deltaUp;
	}

	void EditorCamera3D::Dolly(float deltaDistance)
	{
		SetDistance(m_Distance + deltaDistance);
	}

	void EditorCamera3D::FocusOn(const glm::vec3& point, float distance)
	{
		m_Target = point;
		SetDistance(distance);
	}

	void EditorCamera3D::Fly(float forward, float right, float up)
	{
		m_Target += GetForward() * forward + GetRight() * right + GetUp() * up;
	}

	void EditorCamera3D::SetViewportSize(uint32_t width, uint32_t height)
	{
		if (width == 0 || height == 0)
			return;
		m_Aspect = static_cast<float>(width) / static_cast<float>(height);
	}

	void EditorCamera3D::SetDistance(float distance)
	{
		m_Distance = std::clamp(distance, kMinDistance, kMaxDistance);
	}

	void EditorCamera3D::SetClipPlanes(float nearClip, float farClip)
	{
		m_Near = std::max(nearClip, 1e-4f);
		m_Far = std::max(farClip, m_Near + 1e-3f);
	}

	void EditorCamera3D::SetYawPitch(float yawDegrees, float pitchDegrees)
	{
		m_YawDegrees = yawDegrees;
		m_PitchDegrees = pitchDegrees;
		ClampPitch();
	}

	glm::vec3 EditorCamera3D::GetPosition() const
	{
		const float yaw = glm::radians(m_YawDegrees);
		const float pitch = glm::radians(m_PitchDegrees);
		// yaw=0/pitch=0 → offset=(0,0,-1),即相机在目标 -Z 侧。
		const glm::vec3 offset {
			std::cos(pitch) * std::sin(yaw),
			std::sin(pitch),
			-std::cos(pitch) * std::cos(yaw) };
		return m_Target + offset * m_Distance;
	}

	glm::vec3 EditorCamera3D::GetForward() const
	{
		const glm::vec3 toTarget = m_Target - GetPosition();
		const float length = glm::length(toTarget);
		if (length < 1e-8f)
			return { 0.0f, 0.0f, 1.0f };
		return toTarget / length;
	}

	glm::vec3 EditorCamera3D::GetRight() const
	{
		const glm::vec3 right = glm::cross(GetForward(), glm::vec3(0.0f, 1.0f, 0.0f));
		const float length = glm::length(right);
		if (length < 1e-6f)
			return { 1.0f, 0.0f, 0.0f };   // 俯视极限:pitch 已限幅,这里只做兜底
		return right / length;
	}

	glm::vec3 EditorCamera3D::GetUp() const
	{
		return glm::normalize(glm::cross(GetRight(), GetForward()));
	}

	glm::mat4 EditorCamera3D::GetViewMatrix() const
	{
		return glm::lookAt(GetPosition(), m_Target, glm::vec3(0.0f, 1.0f, 0.0f));
	}

	glm::mat4 EditorCamera3D::GetProjectionMatrix(bool flipY) const
	{
		glm::mat4 projection = glm::perspective(glm::radians(m_FOV), m_Aspect, m_Near, m_Far);
		if (flipY)
		{
			// 与 SceneRenderer 适配 Vulkan NDC +Y 向下的做法逐元素一致。
			projection[0][1] = -projection[0][1];
			projection[1][1] = -projection[1][1];
			projection[2][1] = -projection[2][1];
			projection[3][1] = -projection[3][1];
		}
		return projection;
	}

	glm::mat4 EditorCamera3D::GetViewProjectionMatrix(bool flipY) const
	{
		return GetProjectionMatrix(flipY) * GetViewMatrix();
	}

	glm::mat4 EditorCamera3D::GetInverseViewProjectionMatrix(bool flipY) const
	{
		return glm::inverse(GetViewProjectionMatrix(flipY));
	}
}
