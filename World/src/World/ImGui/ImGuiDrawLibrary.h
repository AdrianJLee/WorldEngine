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
		template<typename T, typename UIFunction>
		static void DrawComponent(const std::string& name, Entity entity, UIFunction uiFunction, bool removable = true)
		{
			if (entity.HasComponent<T>())
			{
				ImGui::Separator();
				ImGuiTreeNodeFlags flags =
					ImGuiTreeNodeFlags_DefaultOpen
					| ImGuiTreeNodeFlags_Framed
					| ImGuiTreeNodeFlags_SpanAvailWidth
					| ImGuiTreeNodeFlags_AllowItemOverlap;
				auto& component = entity.GetComponent<T>();
				if (removable)
				{
					bool closable_group = true;
					if (ImGui::CollapsingHeader(name.c_str(), &closable_group, flags))
					{
						uiFunction(component);
					}
					if (!closable_group)
					{
						entity.RemoveComponent<T>();
					}
				}
				else
				{
					if (ImGui::CollapsingHeader(name.c_str(), flags))
					{
						uiFunction(component);
					}
				}

			}
		}

		static void DrawVec3Control(const std::string& label, glm::vec3& values, float resetValue = 0.0f, float columnWidth = 100.0f);


		static void DrawGizmo(const EditorCamera& editorCamera, Entity targetEntity, ImGuizmo::OPERATION operation, float snap = 0);

	};

}

