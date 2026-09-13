#pragma once
#include "World.h"
#include "World/Renderer/SceneRenderer.h"
#include "Document/EditorDocument.h"
#include "World/WUI/WuiCommand.h"
#include "WUI/EditorShell.h"
#include <atomic>
#include <functional>
#include <string>
#include <thread>
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
		bool SaveScene();
		void StartCookingAction();
		void GenerateLuaStubsAction();
		void CloseAction();
		void DuplicateSelectedEntity();
		// 请求切换渲染后端;在下一帧 OnUpdate 开头(渲染前)安全重建 GPU 资源。
		void ApplyRendererChange(const std::string& name);

		// ---- WUI 面板访问(W2) ----
		Ref<Scene> GetActiveScene() const { return m_ActiveScene; }
		EditorDocument& GetDocument() { return m_Document; }
		Entity GetSelectedEntity() const { return m_SelectedEntity; }
		void SetSelectedEntity(Entity entity) { m_SelectedEntity = entity; }
		EditorCamera& GetEditorCamera() { return m_EditorCamera; }
		Ref<SceneRenderer>& GetSceneRenderer() { return m_SceneRenderer; }
		bool IsPlaying() const { return m_SceneState == SceneState::Play; }
		bool IsSimulating() const { return m_SceneState == SceneState::Simulate; }
		bool IsPaused() const { return m_ScenePaused; }
		void TogglePlay();
		void ToggleSimulate();
		void TogglePause();
		void SetGizmoOperation(ImGuizmo::OPERATION operation) { m_CurrentGizmoOperation = operation; }
		int GetGizmoOperation() const { return m_CurrentGizmoOperation; }
		Entity PickEntityAt(glm::vec2 viewportLocal) { return GetEntityAtMousePosition(viewportLocal); }
		Ref<Texture2D> GetIcon(int index) const;
		// 视口状态(由 WUI 视口面板回填)
		void SetViewportState(bool focused, bool hovered, glm::vec2 size, glm::vec2 bounds[2]);
		glm::vec2 GetViewportSize() const { return m_ViewportSize; }
		void MarkDocumentDirty() { if (m_SceneState == SceneState::Edit && m_ActiveScene == m_Document.GetScene()) m_Document.MarkDirty(); }

		// ---- 模态状态(WUI 读取) ----
		bool& ShowUnsavedModal() { return m_ShowUnsavedModal; }
		bool& ShowErrorModal() { return m_ShowErrorModal; }
		std::string& ErrorText() { return m_ErrorText; }
		bool& ShowCookingProgress() { return m_ShowCookingProgress; }
		bool CookingFinished() const { return m_CookingFinished.load(std::memory_order_acquire); }
		bool CookingSucceeded() const { return m_CookingSucceeded; }
		const std::string& CookingError() const { return m_CookingError; }
		void ResolveUnsavedModal(bool save);
		void CancelUnsavedModal();
		bool HasRenderedScene() const { return m_HasRenderedScene; }
		void ExportOperationLog();
		Wui::WuiCommandRegistry& Commands() { return m_Commands; }

		bool OnKeyPressed(KeyPressedEvent& e);
		bool OnWindowClose(WindowCloseEvent& e);

	private:
		enum class SceneState
		{
			Edit = 0, Play = 1, Simulate = 2
		};
	private:
		Entity GetEntityAtMousePosition(glm::vec2 viewportLocal);

		void SetSceneState(SceneState state);
		void UpdateSceneContext(Ref<Scene> scene);
		void DoNewScene();
		void DoOpenScene(const std::filesystem::path& path);
		bool TrySave();
		void RequestAction(std::function<void()> action);
		void ProcessPendingRendererChange();
		void ShowError(const std::string& message);
		void StartCooking(const std::string& target);
	private:
		Ref<SceneRenderer> m_SceneRenderer;
		SceneRendererOptions m_RendererOptions;

		Ref<Scene> m_ActiveScene;
		Ref<Scene> m_RuntimeScene;
		EditorDocument m_Document;

		EditorCamera m_EditorCamera;

		Entity m_SelectedEntity;

		glm::vec2 m_ViewportSize = { 0,0 };
		glm::vec2 m_ViewportBounds[2] = { {0,0}, {0,0} };

		bool m_ViewportFocused = false, m_ViewportHovered = false;
		bool m_HasRenderedScene = false;

		// Gizmo operation type
		ImGuizmo::OPERATION m_CurrentGizmoOperation = (ImGuizmo::OPERATION)-1;


		Ref<Texture2D> m_IconPlay, m_IconStop;
		Ref<Texture2D> m_IconPause, m_IconContinue;


		SceneState m_SceneState = SceneState::Edit;
		bool m_ScenePaused = false;

		Ref<Texture2D> m_IconSimulate, m_IconSimulateStop;
		Ref<Texture2D> m_IconSimulatePause, m_IconSimulateContinue;

		bool m_ShowCookingProgress = false;    // 是否显示打包弹窗
		std::atomic<bool> m_CookingFinished = false; // 打包是否完成
		std::thread m_CookingThread;
		// Published by m_CookingFinished; the UI reads these only after completion.
		bool m_CookingSucceeded = false;
		std::string m_CookingError;

		// 未保存确认与错误提示
		std::function<void()> m_PendingAction;
		bool m_ShowUnsavedModal = false;
		bool m_ShowErrorModal = false;
		std::string m_ErrorText;

		// Gizmo 拖动前后快照，仅用于标脏（不做撤销）。
		bool m_GizmoDragging = false;
		TransformComponent m_GizmoDragBefore;

		Wui::WuiCommandRegistry m_Commands;
		EditorShell m_Shell;
		Wui::WuiContext m_WuiContext;
		bool m_RendererChangePending = false;
		std::string m_RendererChangeName;
	};

}
