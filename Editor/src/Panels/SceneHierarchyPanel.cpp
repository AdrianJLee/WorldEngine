#include "SceneHierarchyPanel.h"

#include <type_traits>

namespace World
{
	SceneHierarchyPanel::SceneHierarchyPanel(const Ref<Scene>& scene)
	{
		SetContext(scene);
	}
	void SceneHierarchyPanel::SetContext(const Ref<Scene>& scene)
	{
		m_Context = scene;
		m_SelectedEntity = {};
	}
	void SceneHierarchyPanel::OnImGuiRender()
	{
		{
			ImGui::Begin("Scene Hierarchy");

			// 将所有的 entity 提取到一个连续容器中供 Clipper 索引读取
			// 因为 Storage 的迭代器不持支持 Clipper 需要的通过索引直接访问
			std::vector<entt::entity> entities;
			auto& storage = m_Context->m_Registry.storage<entt::entity>();

			entities.reserve(storage.size());
			for (const auto entity : storage)
			{
				entities.push_back(entity);
			}

			// 使用 ImGuiListClipper 仅渲染可视区域内的节点
			ImGuiListClipper clipper;
			clipper.Begin((int)entities.size());
			while (clipper.Step())
			{
				for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; i++)
				{
					Entity ent { m_Context.get(), entities[i] };
					DrawEntityNode(ent);
				}
			}
			clipper.End();

			// 点击空白处取消选中
			if (ImGui::IsMouseDown(0) && ImGui::IsWindowHovered())
			{
				m_SelectedEntity = {};
			}


			if (ImGui::BeginPopupContextWindow(0, 1 | ImGuiPopupFlags_NoOpenOverItems))
			{

				if (ImGui::MenuItem("Create Empty Entity"))
				{
					m_SelectedEntity = Entity::CreateEntity(m_Context.get(), "Empty Entity");
					m_SelectedEntity.AddComponent<TransformComponent>();
				}

				ImGui::EndPopup();
			}

			ImGui::End();
		}

		{
			ImGui::Begin("Properties");
			if (m_SelectedEntity)
			{
				DrawComponents(m_SelectedEntity);

				if (ImGui::BeginPopupContextWindow(0, 1 | ImGuiPopupFlags_NoOpenOverItems))
				{

					if (ImGui::BeginMenu("Add Component"))
					{
						for (const auto& componentInfo : World::ComponentRegistry::GetList())
						{
							bool hasComponent = m_SelectedEntity.HasComponent(componentInfo.Id);

							if (ImGui::MenuItem(componentInfo.Name.c_str(), nullptr, false, !hasComponent))
							{
								componentInfo.AddFunc(m_SelectedEntity);
							}
						}
						ImGui::EndMenu();
					}

					ImGui::EndPopup();
				}
			}
			ImGui::End();
		}
	}

	void SceneHierarchyPanel::DrawEntityNode(Entity entity)
	{
		if (entity.HasComponent<TagComponent>())
		{
			auto& tag = entity.GetComponent<TagComponent>().Tag;

			// 基础Flag：点击箭头展开，占据可用全宽
			ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_OpenOnDoubleClick | ImGuiTreeNodeFlags_SpanAvailWidth;

			if (m_SelectedEntity == entity)
				flags |= ImGuiTreeNodeFlags_Selected;

			// 注意：如果你的引擎还没实现真正的父子实体层级（Hierarchy），可以加上 ImGuiTreeNodeFlags_Leaf 隐藏展开箭头
			// 此时 TreeNode 只当作一个可选中的列表项
			// flags |= ImGuiTreeNodeFlags_Leaf;

			bool opened = ImGui::TreeNodeEx((void*)(uint64_t)(uint32_t)entity, flags, tag.c_str());

			if (ImGui::IsItemClicked())
			{
				m_SelectedEntity = entity;
			}

			bool entityDeleted = false;
			if (ImGui::BeginPopupContextItem())
			{
				if (ImGui::MenuItem("Delete Entity"))
					entityDeleted = true;

				ImGui::EndPopup();
			}

			if (opened)
			{
				// 这里未来可以用来递归绘制子实体
				// 目前把没用的假节点测试代码去掉，保持整洁
				ImGui::TreePop();
			}

			// 将删除操作移到下面，避免在渲染 UI 的中间状态破坏实体
			if (entityDeleted)
			{
				if (m_SelectedEntity == entity)
					m_SelectedEntity = {};
				Entity::DestroyEntity(m_Context.get(), entity);
			}
		}
	}
	void SceneHierarchyPanel::DrawComponents(Entity entity)
	{
		for (const auto& componentInfo : World::ComponentRegistry::GetList())
		{
			if (entity.HasComponent(componentInfo.Id))
			{
				if (componentInfo.ComponentPropertiesUI)
				{
					componentInfo.ComponentPropertiesUI(entity);
				}
			}
		}

	}
}