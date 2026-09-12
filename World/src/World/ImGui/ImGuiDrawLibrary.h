#pragma once

#include "World/Scene/Entity.h"

#include <glm/glm.hpp>
#include <imgui.h>
#include <ImGuizmo.h>

namespace World
{
	class ImGuiDrawLibrary
	{
	public:
		static void DrawGizmo(const EditorCamera& editorCamera, Entity targetEntity, ImGuizmo::OPERATION operation, float snap = 0);

	};

}

