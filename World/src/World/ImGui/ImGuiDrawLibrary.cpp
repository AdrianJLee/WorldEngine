#include "wldpch.h"
#include "ImGuiDrawLibrary.h"

#include "World/Scene/Scene.h"
#include "World/Scene/Components.h"

#include <glm/gtc/type_ptr.hpp>

namespace World
{
	void ImGuiDrawLibrary::DrawGizmo(const EditorCamera& editorCamera, Entity targetEntity, ImGuizmo::OPERATION operation, float snap)
	{
		// 设置 ImGuizmo 的操作模式和坐标系统
		ImGuizmo::SetOrthographic(false);
		// 注意：ImGuizmo 的坐标系统是左手系，且默认使用列主序矩阵
		ImGuizmo::SetDrawlist();

		float windowWidth = ImGui::GetWindowWidth();
		float windowHeight = ImGui::GetWindowHeight();

		// 设置操作区域为当前 ImGui 窗口的大小和位置
		ImGuizmo::SetRect(ImGui::GetWindowPos().x, ImGui::GetWindowPos().y, windowWidth, windowHeight);

		glm::mat4 cameraView = editorCamera.GetViewMatrix();
		glm::mat4 camerProjection = editorCamera.GetProjectionMatrix();

		auto& targetTransform = targetEntity.GetComponent<TransformComponent>();
		glm::mat4 entityTransform = targetTransform.Transform;

		float snapValues[3] = { snap, snap, snap };
		ImGuizmo::Manipulate(glm::value_ptr(cameraView), glm::value_ptr(camerProjection),
			operation, ImGuizmo::LOCAL, glm::value_ptr(entityTransform), nullptr, snapValues);

		if (ImGuizmo::IsUsing())
		{
			targetTransform.SetTransform(entityTransform);
		}
	}
}
