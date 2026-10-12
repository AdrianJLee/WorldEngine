#include "wldpch.h"
#include "World/Renderer/EditorCamera3D.h"
#include "World/Renderer/ProjectionConventions.h"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>

namespace World
{
	namespace
	{
		constexpr float kMaxPitchDegrees = 89.0f;
		constexpr float kMinDistance = 0.5f;
		constexpr float kMaxDistance = 1000.0f;
		constexpr float kMinOrthoSize = 0.5f;
		constexpr float kMaxOrthoSize = 1000.0f;
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
		// 纯 2D 锁定模式下严格锁定相机旋转, 防止误触 3D 偏航/俯仰倾斜
		if (m_Is2DMode)
			return;

		m_YawDegrees += deltaYawDegrees;
		m_PitchDegrees += deltaPitchDegrees;
		ClampPitch();
		// 主动旋转后自动转入自由视角
		m_Viewpoint = ViewportViewpoint::Free;
	}

	void EditorCamera3D::Set2DMode(bool enabled)
	{
		m_Is2DMode = enabled;
		if (enabled)
		{
			SetProjection(ViewportProjection::Orthographic);
			SetViewpoint(ViewportViewpoint::Front);
		}
	}

	void EditorCamera3D::SetProjection(ViewportProjection proj)
	{
		if (m_Projection == proj)
			return;
		m_Projection = proj;
		const float halfFov = glm::radians(m_FOV * 0.5f);
		if (m_Projection == ViewportProjection::Orthographic)
		{
			// 从透视切正交: 由当前 distance 与 FOV 换算对应的 OrthographicSize
			m_OrthographicSize = std::clamp(2.0f * m_Distance * std::tan(halfFov), kMinOrthoSize, kMaxOrthoSize);
		}
		else
		{
			// 从正交切透视: 由当前 OrthographicSize 反算 distance
			m_Distance = std::clamp((m_OrthographicSize * 0.5f) / std::tan(halfFov), kMinDistance, kMaxDistance);
		}
	}

	void EditorCamera3D::SetViewpoint(ViewportViewpoint vp)
	{
		m_Viewpoint = vp;
		switch (vp)
		{
		case ViewportViewpoint::Top:
			SetYawPitch(180.0f, 89.0f);
			break;
		case ViewportViewpoint::Bottom:
			SetYawPitch(180.0f, -89.0f);
			break;
		case ViewportViewpoint::Front:
			SetYawPitch(180.0f, 0.0f);
			break;
		case ViewportViewpoint::Back:
			SetYawPitch(0.0f, 0.0f);
			break;
		case ViewportViewpoint::Right:
			SetYawPitch(90.0f, 0.0f);
			break;
		case ViewportViewpoint::Left:
			SetYawPitch(270.0f, 0.0f);
			break;
		case ViewportViewpoint::Free:
			break;
		}
	}

	void EditorCamera3D::SetOrthographicSize(float size)
	{
		m_OrthographicSize = std::clamp(size, kMinOrthoSize, kMaxOrthoSize);
		const float halfFov = glm::radians(m_FOV * 0.5f);
		m_Distance = std::clamp((m_OrthographicSize * 0.5f) / std::tan(halfFov), kMinDistance, kMaxDistance);
	}

	void EditorCamera3D::Pan(float deltaRight, float deltaUp)
	{
		m_Target += GetRight() * deltaRight + GetUp() * deltaUp;
	}

	void EditorCamera3D::Dolly(float deltaDistance)
	{
		if (m_Projection == ViewportProjection::Orthographic)
		{
			const float factor = std::clamp(1.0f + deltaDistance * 0.08f, 0.05f, 20.0f);
			SetOrthographicSize(m_OrthographicSize * factor);
		}
		else
		{
			SetDistance(m_Distance + deltaDistance);
		}
	}

	void EditorCamera3D::FocusOn(const glm::vec3& point, float distance)
	{
		m_Target = point;
		if (m_Projection == ViewportProjection::Orthographic)
		{
			const float halfFov = glm::radians(m_FOV * 0.5f);
			SetOrthographicSize(2.0f * distance * std::tan(halfFov));
		}
		else
		{
			SetDistance(distance);
		}
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

	glm::mat4 EditorCamera3D::GetProjectionMatrix(bool vulkan) const
	{
		glm::mat4 projection;
		if (m_Projection == ViewportProjection::Orthographic)
		{
			const float orthoH = m_OrthographicSize;
			const float orthoW = orthoH * m_Aspect;
			const float clipExtent = std::max({ m_Far, orthoH * 3.5f, 2000.0f });
			projection = glm::ortho(
				-orthoW * 0.5f, orthoW * 0.5f,
				-orthoH * 0.5f, orthoH * 0.5f,
				-clipExtent, clipExtent
			);
		}
		else
		{
			projection = glm::perspective(glm::radians(m_FOV), m_Aspect, m_Near, m_Far);
		}
		// 与渲染路径共用同一份适配(Y 翻转 + 深度范围重映射),避免"渲染对了、拾取反了"。
		return AdaptViewProjectionToBackend(projection, vulkan);
	}

	glm::mat4 EditorCamera3D::GetViewProjectionMatrix(bool vulkan) const
	{
		return GetProjectionMatrix(vulkan) * GetViewMatrix();
	}

	glm::mat4 EditorCamera3D::GetInverseViewProjectionMatrix(bool vulkan) const
	{
		return glm::inverse(GetViewProjectionMatrix(vulkan));
	}
}
