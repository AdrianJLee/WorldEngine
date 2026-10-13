#pragma once
#include "World/Scene/Components.h"
#include "World/Scene/Entity.h"
#include "World/WUI/WuiGizmo.h"
#include "World/WUI/WuiWidgets.h"

namespace World
{
	bool ManipulateFreeformLight2D(const Wui::GizmoCamera& camera,
		const TransformComponent& transform, FreeformLight2DComponent& light,
		const Wui::WuiRect& viewport, Wui::WuiContext& ctx,
		bool allowManipulation = true);

	bool ManipulateFreeformLight2D(const Wui::GizmoCamera& camera,
		Entity selected, const Wui::WuiRect& viewport, Wui::WuiContext& ctx,
		bool allowManipulation = true);
}
