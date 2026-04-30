#pragma once
#include "World.h"
#include "Panels/SceneHierarchyPanel.h"
#include "Panels/ContentBrowserPanel.h"
#include "World/Renderer/SceneRenderer.h"
namespace World
{
	class EditorLayer : public Layer
	{
	public:
		EditorLayer();
		virtual ~EditorLayer() = default;
		virtual void OnAttach() override;
		virtual void OnDetach() override;
		virtual void OnUpdate(Timestep ts) override;
		virtual void OnImGuiRender() override;
		virtual void OnEvent(Event& event) override;

		void NewScene();
		void OpenScene();
		void OpenScene(const std::filesystem::path& path);
		void SaveScene();

		bool OnKeyPressed(KeyPressedEvent& e);
		bool OnMouseButtonPressed(MouseButtonPressedEvent& e);

	private:
		enum class SceneState
		{
			Edit = 0, Play = 1, Simulate = 2
		};
	private:
		// 更新视口边界，获取视口在屏幕上的坐标范围
		void UpdateViewBounds();

		Entity GetEntityAtMousePosition();


		void UI_Toolbar();

		void SetSceneState(SceneState state);
		void UpdateSceneContext(Ref<Scene> scene);
	private:
		Ref<SceneRenderer> m_SceneRenderer;
		SceneRendererOptions m_RendererOptions;

		Ref<Scene> m_ActiveScene;
		Ref<Scene> m_EditorScene, m_RuntimeScene;

		std::filesystem::path m_ScenePath;

		EditorCamera m_EditorCamera;

		glm::vec2 m_ViewportSize = { 0,0 };
		glm::vec2 m_ViewportBounds[2] = { {0,0}, {0,0} };

		bool m_ViewportFocused = false, m_ViewportHovered = false;

		SceneHierarchyPanel m_SceneHierarchyPanel;
		ContentBrowserPanel m_ContentBrowserPanel;

		// Gizmo operation type
		ImGuizmo::OPERATION m_CurrentGizmoOperation = (ImGuizmo::OPERATION)-1;


		Ref<Texture2D> m_IconPlay, m_IconStop;
		Ref<Texture2D> m_IconPause, m_IconContinue;


		SceneState m_SceneState = SceneState::Edit;
		bool m_ScenePaused = false;

		Ref<Texture2D> m_IconSimulate, m_IconSimulateStop;
		Ref<Texture2D> m_IconSimulatePause, m_IconSimulateContinue;
	};

}