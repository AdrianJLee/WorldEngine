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
		static bool DrawComponent(const std::string& name, Entity entity, UIFunction uiFunction, bool removable = true)
		{
			if (entity && !entity.GetScene()->IsPendingDestroy(entity) && entity.HasComponent<T>())
			{
				ImGui::Separator();
				ImGuiTreeNodeFlags flags =
					ImGuiTreeNodeFlags_DefaultOpen
					| ImGuiTreeNodeFlags_Framed
					| ImGuiTreeNodeFlags_SpanAvailWidth
					| ImGuiTreeNodeFlags_AllowItemOverlap;
				auto& component = entity.GetComponent<T>();
				std::string removalReason;
				const bool canRemove = removable && entity.CanRemoveComponent(entt::type_id<T>().hash(), &removalReason);
				bool changed = false;
				if (canRemove)
				{
					bool closable_group = true;
					if (ImGui::CollapsingHeader(name.c_str(), &closable_group, flags))
					{
						changed = uiFunction(component) || changed;
					}
					if (!closable_group)
					{
						try { entity.RemoveComponent<T>(); changed = true; }
						catch (const std::exception& error) { ImGui::TextWrapped("Cannot remove: %s", error.what()); }
					}
				}
				else
				{
					if (ImGui::CollapsingHeader(name.c_str(), flags))
					{
						if (removable && !removalReason.empty())
							ImGui::TextWrapped("Cannot remove: %s", removalReason.c_str());
						changed = uiFunction(component) || changed;
					}
				}

				return changed;
			}
			return false;
		}

		static bool DrawVec3Control(const std::string& label, glm::vec3& values, float resetValue = 0.0f, float columnWidth = 100.0f);


		static void DrawGizmo(const EditorCamera& editorCamera, Entity targetEntity, ImGuizmo::OPERATION operation, float snap = 0);

	};

}

