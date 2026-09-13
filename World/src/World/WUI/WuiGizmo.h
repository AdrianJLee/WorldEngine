#pragma once

#include "World/WUI/WuiContext.h"
#include "World/Renderer/EditorCamera.h"
#include "World/Scene/Components.h"

namespace World::Wui
{
	enum class GizmoOperation
	{
		None = -1,
		Translate,
		Rotate,
		Scale,
	};

	// WUI Gizmo:在视口内直接操纵实体的 Transform。返回本帧是否正在拖拽。
	WLD_API bool ManipulateGizmo(const EditorCamera& camera, GizmoOperation operation,
		TransformComponent& transform, const WuiRect& viewport, WuiContext& ctx);
}
