#pragma once

#include "World/Core/Export.h"

#include <glm/glm.hpp>

namespace World
{
	enum class ViewportProjection
	{
		Perspective = 0,   // 透视投影 (近大远小)
		Orthographic = 1,  // 正交投影 (平行投影)
	};

	enum class ViewportViewpoint
	{
		Free = 0,    // 自由视角 (Orbit 自由漫游)
		Top = 1,     // 顶视 (Top [XZ], 俯视角)
		Bottom = 2,  // 底视 (Bottom [XZ], 仰视角)
		Front = 3,   // 前视 (Front [XY], 正视立面)
		Back = 4,    // 后视 (Back [XY], 背视立面)
		Right = 5,   // 右视 (Right [ZY], 侧视立面)
		Left = 6,    // 左视 (Left [ZY], 侧视立面)
	};

	// 统一三维视口相机:目标点 + 距离 + yaw/pitch 轨道模型,兼具透视与平行正交双投影模式。
	//
	// 约定:
	//  - yaw=0、pitch=0 时相机位于 Target 的 -Z 侧,视线指向 Target;
	//  - 投影矩阵按 GL 约定编写(NDC +Y 向上、z∈[-1,1]);vulkan=true 时走
	//    `ProjectionConventions.h` 的同一份适配(翻转 Y 行 + 把深度重映射到 [0,1]),
	//    与 SceneRenderer 的渲染路径逐元素一致 —— 否则会出现"渲染对了、拾取反了"。
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

		// ---- 投影与方位 (图形学第一性原理) ----
		void SetProjection(ViewportProjection proj);
		ViewportProjection GetProjection() const { return m_Projection; }
		bool IsOrthographic() const { return m_Projection == ViewportProjection::Orthographic; }

		void SetViewpoint(ViewportViewpoint vp);
		ViewportViewpoint GetViewpoint() const { return m_Viewpoint; }

		void SetOrthographicSize(float size);
		float GetOrthographicSize() const { return m_OrthographicSize; }

		// 纯 2D 视口锁定模式 (锁定正交投影与 XY 平面, 禁用 3D 旋转)
		void Set2DMode(bool enabled);
		bool Is2DMode() const { return m_Is2DMode; }

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
		glm::mat4 GetProjectionMatrix(bool vulkan = false) const;
		glm::mat4 GetViewProjectionMatrix(bool vulkan = false) const;

		// 供拾取使用:屏幕 NDC → 世界射线的逆矩阵。
		glm::mat4 GetInverseViewProjectionMatrix(bool vulkan = false) const;

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
		ViewportProjection m_Projection = ViewportProjection::Perspective;
		ViewportViewpoint m_Viewpoint = ViewportViewpoint::Free;
		float m_OrthographicSize = 10.0f;
		bool m_Is2DMode = false;
	};
}
