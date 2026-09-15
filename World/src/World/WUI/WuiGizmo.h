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
	//  - 平移:三根轴箭头(X 红 / Y 绿 / Z 蓝),沿轴约束拖动;
	//  - 缩放:三根轴 + 方块端点(中心方块 = 等比缩放);
	//  - 旋转:中心圆点周围的方向环(拖 X 改 Z 轴旋转,简化实现);
	//  - allowManipulation=false(Play/Simulate 只读查看):只画不响应拖拽。
	WLD_API bool ManipulateGizmo(const EditorCamera& camera, GizmoOperation operation,
		TransformComponent& transform, const WuiRect& viewport, WuiContext& ctx,
		bool allowManipulation = true);
}
