#pragma once
#include "World/Schema/Schema.h"

namespace World
{
	// PECS(相机组件数据导向化):纯相机参数聚合。
	//
	// 这里**不再**继承渲染层 Camera,也不持有投影矩阵 —— 组件只存权威参数;
	// 投影矩阵是派生量,由 CameraSystem 按(参数, 有效宽高比, 视口)指纹脏标记缓存
	// (见 Components.h 的 CameraViewComponent)。
	//
	// 字段名与声明顺序、默认值都与迁移前的 SceneCamera 完全一致(m_* 是 .wd 里
	// 实际存储的名字),所以存量场景零迁移。唯一变化是类型名(SceneCamera -> CameraSettings)。
	struct CameraSettings
	{
		enum class ProjectionType :int
		{
			Perspective = 0,
			Orthographic = 1
		};
		WE_ENUM_SCHEMA(World, ProjectionType, Int32)
			WE_ENUM_VALUE(Perspective);
			WE_ENUM_VALUE(Orthographic);
		WE_ENUM_END

		// Common
		ProjectionType m_ProjectionType = ProjectionType::Orthographic;
		float m_AspectRatio = 1.0f;

		// Orthographic
		float m_OrthographicZoom = 1.0f;
		float m_OrthographicNearClip = -1.0f;
		float m_OrthographicFarClip = 1.0f;

		// Perspective
		float m_PerspectiveFOV = 45.0f;
		float m_PerspectiveNearClip = 0.1f;
		float m_PerspectiveFarClip = 100.0f;

		WE_SCHEMA_BODY(World, CameraSettings, Struct)
			WE_FIELD(m_ProjectionType, Enum, Of(ProjectionType), Group("Projection"));
			WE_FIELD(m_AspectRatio, Float, Transient);
			WE_FIELD(m_OrthographicZoom, Float, Group("Orthographic"));
			WE_FIELD(m_OrthographicNearClip, Float, Group("Orthographic"));
			WE_FIELD(m_OrthographicFarClip, Float, Group("Orthographic"));
			WE_FIELD(m_PerspectiveFOV, Float, Group("Perspective"));
			WE_FIELD(m_PerspectiveNearClip, Float, Group("Perspective"));
			WE_FIELD(m_PerspectiveFarClip, Float, Group("Perspective"));
		WE_SCHEMA_END
	};

	// 指纹逐字节比较的前提:enum(int) + 7 个 float,无填充。
	static_assert(sizeof(CameraSettings) == 32, "CameraSettings must be exactly 32 bytes (enum int + 7 floats)");
}
