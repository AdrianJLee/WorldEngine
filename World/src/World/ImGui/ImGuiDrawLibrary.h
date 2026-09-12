#pragma once

#include "World/Scene/Entity.h"
#include "World/WUI/WuiCore.h"

#include <glm/glm.hpp>
#include <imgui.h>
#include <ImGuizmo.h>

namespace World
{
	class ImGuiDrawLibrary
	{
	public:
		static void DrawGizmo(const EditorCamera& editorCamera, Entity targetEntity, ImGuizmo::OPERATION operation,
			const Wui::WuiRect& rect, float snap = 0);
		static bool GizmoIsUsing();
		static bool GizmoIsOver();

	};

}

