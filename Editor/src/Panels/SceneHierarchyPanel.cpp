#include "SceneHierarchyPanel.h"

#include <exception>
#include <utility>

namespace World
{
	namespace
	{
		template<typename Action>
		void RunPanelAction(const char* description, Action&& action)
		{
			try
			{
				action();
			}
			catch (const std::exception& error)
			{
				WLD_CORE_ERROR("{0}: {1}", description, error.what());
			}
			catch (...)
			{
				WLD_CORE_ERROR("{0}: unknown error.", description);
			}
		}
	}

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
		if (!m_Context)
		{
			m_SelectedEntity = {};
			ImGui::Begin("Scene Hierarchy");
			ImGui::TextDisabled("No scene loaded.");
			ImGui::End();
			ImGui::Begin("Properties");
			ImGui::End();
			return;
		}
		if (!m_SelectedEntity.IsValid() || m_SelectedEntity.GetScene() != m_Context.get() ||
			m_Context->IsPendingDestroy(m_SelectedEntity))
			m_SelectedEntity = {};

		{
			ImGui::Begin("Scene Hierarchy");

			// 将可显示的 entity 提取到连续容器中供 Clipper 索引读取
			// 仅包含可绘制节点，保证 Clipper 的 ItemsCount 与实际行数一致
			std::vector<Entity> entities;
			const auto* storage = std::as_const(*m_Context).GetRegistry().storage<entt::entity>();

			if (storage)
			{
				entities.reserve(storage->size());
				for (const auto rawEntity : *storage)
				{
					Entity entity { m_Context.get(), rawEntity };
					if (entity.IsValid() && !m_Context->IsPendingDestroy(rawEntity) && entity.HasComponent<TagComponent>())
						entities.push_back(entity);
				}
			}

			std::vector<Entity> entitiesToDelete;
			entitiesToDelete.reserve(1);

			// 使用 ImGuiListClipper 仅渲染可视区域内的节点
			ImGuiListClipper clipper;
			clipper.Begin((int)entities.size());
			while (clipper.Step())
			{
				for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; i++)
				{
					DrawEntityNode(entities[i], entitiesToDelete);
				}
			}
			clipper.End();

			for (Entity entity : entitiesToDelete)
			{
				RunPanelAction("Unable to delete entity", [&]()
				{
					if (entity.IsValid() && !m_Context->IsPendingDestroy(entity))
						Entity::DestroyEntity(m_Context.get(), entity);
				});
			}

			// 点击空白处取消选中
			if (ImGui::IsMouseDown(0) && ImGui::IsWindowHovered())
			{
				m_SelectedEntity = {};
			}


			if (ImGui::BeginPopupContextWindow(0, 1 | ImGuiPopupFlags_NoOpenOverItems))
			{

				if (ImGui::MenuItem("Create Empty Entity"))
				{
					RunPanelAction("Unable to create entity", [&]()
					{
						if (m_Context->IsActive())
						{
							if (!m_Context->DeferStructuralChange([](Scene& scene)
							{
								Entity entity = Entity::CreateEntity(&scene, "Empty Entity");
								entity.AddComponent<TransformComponent>();
								// Select from the hierarchy after commit; do not retain the panel in a command.
							}))
								WLD_CORE_WARN("Create entity request was rejected by the scene.");
						}
						else
						{
							m_SelectedEntity = Entity::CreateEntity(m_Context.get(), "Empty Entity");
							m_SelectedEntity.AddComponent<TransformComponent>();
						}
					});
				}

				ImGui::EndPopup();
			}

			ImGui::End();
		}

		{
			ImGui::Begin("Properties");
			if (m_SelectedEntity.IsValid() && !m_Context->IsPendingDestroy(m_SelectedEntity))
			{
				DrawComponents(m_SelectedEntity);

				if (ImGui::BeginPopupContextWindow(0, 1 | ImGuiPopupFlags_NoOpenOverItems))
				{

					if (ImGui::BeginMenu("Add Component"))
					{

						for (const auto& className : TypeRegistry::Get().GetTypesByCategory(TypeCategory::Component))
						{
							if (TypeDescDataComponent* componentInfo = std::any_cast<TypeDescDataComponent>(&TypeRegistry::Get().GetTypeDesc(className)->UserData))
							{
								std::string reason;
								bool canAdd = m_SelectedEntity.CanAddComponent(componentInfo->Id, &reason);
								if (!componentInfo->AddFunc)
								{
									canAdd = false;
									reason = "This component has no registered add operation.";
								}
								if (ImGui::MenuItem(className.c_str(), nullptr, false, canAdd))
								{
									const entt::entity handle = m_SelectedEntity;
									const entt::id_type componentId = componentInfo->Id;
									RunPanelAction("Unable to add component", [&]()
									{
										if (!m_Context->DeferStructuralChange([handle, componentId, className](Scene& scene)
										{
											Entity target(&scene, handle);
											std::string commitReason;
											if (target.CanAddComponent(componentId, &commitReason))
												target.AddComponent(componentId);
											else
												WLD_CORE_WARN("Cannot add {0}: {1}", className, commitReason);
										}))
											WLD_CORE_WARN("Add component request was rejected by the scene.");
									});
								}
								if (!canAdd && !reason.empty() && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
									ImGui::SetTooltip("%s", reason.c_str());
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

	void SceneHierarchyPanel::DrawEntityNode(Entity entity, std::vector<Entity>& entitiesToDelete)
	{
		if (entity.IsValid() && !m_Context->IsPendingDestroy(entity) && entity.HasComponent<TagComponent>())
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

			if (entityDeleted)
			{
				if (m_SelectedEntity == entity)
					m_SelectedEntity = {};
				entitiesToDelete.push_back(entity);
			}
		}
	}
	void SceneHierarchyPanel::DrawComponents(Entity entity)
	{
		for (const auto& className : TypeRegistry::Get().GetTypesByCategory(TypeCategory::Component))
		{
			if (!entity.IsValid() || m_Context->IsPendingDestroy(entity))
				break;
			if (TypeDescDataComponent* componentInfo = std::any_cast<TypeDescDataComponent>(&TypeRegistry::Get().GetTypeDesc(className)->UserData))
			{
				if (entity.HasComponent(componentInfo->Id))
				{
					if (componentInfo->ComponentPropertiesUI)
					{
						RunPanelAction("Unable to draw component properties", [&]()
						{
							componentInfo->ComponentPropertiesUI(entity);
						});
					}
				}
			}
		}
	}
}
