#pragma once

#include "World.h"
#include <functional>

namespace World
{
	class SceneHierarchyPanel
	{
	public:
		SceneHierarchyPanel() = default;
		SceneHierarchyPanel(const Ref<Scene>& scene);
		~SceneHierarchyPanel() = default;

		void SetContext(const Ref<Scene>& scene);
		void OnImGuiRender();
		Entity GetSelectedEntity() const { return m_SelectedEntity; }
		void SetSelectedEntity(Entity entity) { m_SelectedEntity = entity; }
		// 当组件属性被编辑、实体/组件被增删时回调（仅在面板发起这些操作后触发）。
		void SetEditCallback(const std::function<void()>& callback) { m_EditCallback = callback; }
	private:
		void DrawEntityNode(Entity entity, std::vector<Entity>& entitiesToDelete);
		void DrawComponents(Entity entity);
	private:
		Ref<Scene> m_Context;
		Entity m_SelectedEntity;
		std::function<void()> m_EditCallback;
	};

}
