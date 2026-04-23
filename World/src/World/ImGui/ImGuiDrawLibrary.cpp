#include "wldpch.h"
#include "ImGuiDrawLibrary.h"

#include "World/Scene/Scene.h"
#include "World/Scene/Components.h"

#include <imgui_internal.h>
#include <glm/gtc/type_ptr.hpp>

namespace World
{

	void ImGuiDrawLibrary::DrawVec3Control(const std::string& label, glm::vec3& values, float resetValue, float columnWidth)
	{
		ImGuiIO& io = ImGui::GetIO();
		auto boldFont = io.Fonts->Fonts[1];

		ImGui::PushID(label.c_str());

		// 1. 创建表格：2列，开启可调宽度，但不显示分割线以保持美观
		if (ImGui::BeginTable("##Vec3ControlTable", 2, ImGuiTableFlags_Resizable))
		{
			// 2. 设置列宽策略：第一列固定宽度，第二列拉伸填充剩余空间
			ImGui::TableSetupColumn("Label", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_NoResize, columnWidth);
			ImGui::TableSetupColumn("Controls", ImGuiTableColumnFlags_WidthStretch);

			// 3. 消除默认的 ItemSpacing，让按钮和输入框紧密排列
			ImGui::TableNextRow();

			// --- 第一列：Label ---
			ImGui::TableNextColumn();
			// 关键点：让 Text 的基准线和右侧的输入框对齐
			ImGui::AlignTextToFramePadding();

			// 居中计算
			float columnWidth = ImGui::GetContentRegionAvail().x;
			float textWidth = ImGui::CalcTextSize(label.c_str()).x;
			ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (columnWidth - textWidth) * 0.5f);

			ImGui::Text(label.c_str());

			// --- 第二列：X, Y, Z 控制器 ---
			ImGui::TableNextColumn();

			// 1. 获取当前单元格实际可用的总宽度
			float totalWidth = ImGui::GetContentRegionAvail().x;

			// 2. 预先算好按钮尺寸，以便精准扣除
			float lineHeight = ImGui::GetFontSize() + ImGui::GetStyle().FramePadding.y * 2.0f;
			ImVec2 buttonSize = { lineHeight + 3.0f, lineHeight };

			// 3. 计算【除去 3 个按钮后】剩余给 DragFloat 的总空间
			// 注意：因为我们用了 PushStyleVar(ItemSpacing, 0)，所以不需要扣除项间距
			float totalDragFloatWidth = totalWidth - (buttonSize.x * 3.0f);

			// 4. 将剩余空间平分为 3 份
			ImGui::PushMultiItemsWidths(3, totalDragFloatWidth);

			// 临时消除间距，让按钮和输入框贴合
			ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2 { 0, 0 });

			// --- X 轴 ---
			{
				ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.70f, 0.20f, 0.25f, 1.0f));
				ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.85f, 0.30f, 0.35f, 1.0f));
				ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.55f, 0.15f, 0.20f, 1.0f));
				ImGui::PushFont(boldFont);
				if (ImGui::Button("X", buttonSize)) values.x = resetValue;
				ImGui::PopFont();
				ImGui::PopStyleColor(3);
				ImGui::SameLine();
				ImGui::DragFloat("##X", &values.x, 0.1f, 0.0f, 0.0f, "%.3f");
				ImGui::PopItemWidth(); // 对应第一个宽度
				ImGui::SameLine();
			}
			// --- Y 轴 ---
			ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.20f, 0.60f, 0.30f, 1.0f));
			ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.30f, 0.75f, 0.40f, 1.0f));
			ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.15f, 0.45f, 0.25f, 1.0f));
			ImGui::PushFont(boldFont);
			if (ImGui::Button("Y", buttonSize)) values.y = resetValue;
			ImGui::PopFont();
			ImGui::PopStyleColor(3);
			ImGui::SameLine();
			ImGui::DragFloat("##Y", &values.y, 0.1f, 0.0f, 0.0f, "%.3f");
			ImGui::PopItemWidth(); // 对应第二个宽度
			ImGui::SameLine();

			// --- Z 轴 ---
			ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.20f, 0.45f, 0.75f, 1.0f));
			ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.30f, 0.60f, 0.90f, 1.0f));
			ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.15f, 0.35f, 0.60f, 1.0f));
			ImGui::PushFont(boldFont);
			if (ImGui::Button("Z", buttonSize)) values.z = resetValue;
			ImGui::PopFont();
			ImGui::PopStyleColor(3);
			ImGui::SameLine();
			ImGui::DragFloat("##Z", &values.z, 0.1f, 0.0f, 0.0f, "%.3f");
			ImGui::PopItemWidth(); // 对应第三个宽度

			ImGui::PopStyleVar(); // 恢复 ItemSpacing
			ImGui::EndTable();
		}

		ImGui::PopID();
	}

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