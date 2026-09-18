#pragma once
#include "World.h"
#include "World/Gameplay/GameHost.h"
#include "World/Events/KeyEvent.h"
#include "World/Events/ApplicationEvent.h"
#include "World/Renderer/SceneRenderer.h"
#include "World/Renderer/EditorCamera3D.h"
#include "World/Script/ScriptFileWatch.h"
#include "Document/EditorDocument.h"
#include "World/WUI/WuiCommand.h"
#include "World/WUI/WuiGizmo.h"
#include "WUI/EditorShell.h"
#include "AiControl/AiControlServer.h"
#include <atomic>
#include <functional>
#include <string>
#include <thread>
namespace World
{
	// P2 W5b:脚本热重载只按引用操作组件(完整定义在 Scene/Components.h)。
	struct LuaScriptComponent;

	class EditorLayer : public Layer
	{
	public:
		EditorLayer();
		virtual ~EditorLayer() = default;
		virtual void OnAttach() override;
		virtual void OnDetach() override;
		virtual void OnUpdate(Timestep ts) override;
		virtual void OnUiFrame() override;
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
		// D7-1a:视口 3D 相机(轨道/飞行)。与 2D EditorCamera 二选一用于视口渲染。
		EditorCamera3D& GetEditorCamera3D() { return m_EditorCamera3D; }
		bool IsViewportCamera3D() const { return m_Viewport3D; }
		void SetViewportCamera3D(bool enabled) { m_Viewport3D = enabled; }
		void ToggleViewportCamera3D() { m_Viewport3D = !m_Viewport3D; }
		Ref<SceneRenderer>& GetSceneRenderer() { return m_SceneRenderer; }
		// 相机可视化:预览小窗(用场景相机渲一份 PiP)与视锥显示。
		uint64_t GetCameraPreviewTextureId() const { return m_PreviewTextureId; }
		bool IsCameraPreviewEnabled() const { return m_CameraPreviewEnabled; }
		void ToggleCameraPreview() { m_CameraPreviewEnabled = !m_CameraPreviewEnabled; }
		std::string CameraPreviewLabel() const;
		// W8:编辑器的存档服务(场景来源 = 当前活动场景),供存档面板使用。
		Gameplay::SaveService* GetSaveService() { return m_SaveService.get(); }
		bool IsPlaying() const { return m_SceneState == SceneState::Play; }
		bool IsSimulating() const { return m_SceneState == SceneState::Simulate; }
		bool IsPaused() const { return m_ScenePaused; }
		void TogglePlay();
		void ToggleSimulate();
		void TogglePause();
		void SetGizmoOperation(Wui::GizmoOperation operation) { m_CurrentGizmoOperation = operation; }
		Wui::GizmoOperation GetGizmoOperation() const { return m_CurrentGizmoOperation; }
		Entity PickEntityAt(glm::vec2 viewportLocal) { return GetEntityAtMousePosition(viewportLocal); }
		Ref<Texture2D> GetIcon(int index) const;
		uint64_t GetIconId(int index) const;
		// 旧式纹理纪元:窗口/上下文重建后自增,面板据此重载自己的 GL 图标。
		uint32_t TextureEpoch() const { return m_TextureEpoch; }
		uint64_t GetSceneTextureId() const { return m_SceneTextureId; }
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
		// 屏幕快照钩子(诊断):WLD_SCREEN_CAPTURE_DIR 存在时,把用户看到的最终窗口
		// 连续写成 PPM(screen-<n>.ppm),供自动化比对"闪烁"这类最终画面问题。
		void CaptureScreenSequence();
		Wui::WuiCommandRegistry& Commands() { return m_Commands; }

		// ---- P2 W5b:脚本热重载(编辑器侧接线)----
		// 唯一的重载入口:帧边界轮询、属性面板 Reload 按钮、AI 通道 script.reload 共用。
		//   - Running + IsLoaded:ScriptEngine::ReloadScript(失败保留旧版本,诊断写进组件);
		//   - Faulted/未加载:复位 Pending(IsLoaded=false、清 LastError,保留 CachedFields 与
		//     ScriptFilePath),交给 Scene 既有的 pending 机制在下一个安全点重新实例化 —— 也就是
		//     "把脚本写坏 → 改好 → Reload 救回来";
		//   - Creating/Destroying:拒绝(不打断生命周期)。
		// scene 只用于把"编辑态场景不跑脚本"写进 message,可为 null。
		static bool ReloadLuaScriptComponent(LuaScriptComponent& script, Scene* scene, std::string* message = nullptr);

		// ---- P2 W8:Scripts 面板的宿主能力(EditorShell 作为 PanelHost 转发到这里)----
		// 重载指定实体的脚本:组件查找走 Entity 的组件指针入口(Play/Simulate 下也不触发
		// "活动场景禁止结构写"断言),重载本身仍复用上面的唯一入口。
		bool ScriptsReloadInstance(entt::entity handle, std::string* message = nullptr);
		// 用系统默认程序打开磁盘上的脚本(ResolveScriptDiskPath 解析;包内/非法路径 → false)。
		bool ScriptsOpenExternal(const std::string& logicalPath, std::string* message = nullptr);
		// 从 scripts/templates/WorldScript.lua 复制出 scripts/script_<n>.lua(冲突递增、永不覆盖)。
		bool ScriptsCreateFromTemplate(std::string& outLogicalPath, std::string* message = nullptr);

		bool OnKeyPressed(KeyPressedEvent& e);
		bool OnWindowClose(WindowCloseEvent& e);

	private:
		enum class SceneState
		{
			Edit = 0, Play = 1, Simulate = 2
		};
	private:
		Entity GetEntityAtMousePosition(glm::vec2 viewportLocal);
		// 预览用相机:选中的相机实体(属于活动场景)优先,否则场景主相机;没有则无效实体。
		Entity GetPreviewCameraEntity() const;
		// 用预览相机渲染一份小尺寸画面(相机可视化 PiP)。
		void RenderCameraPreview();
		// 相机实体的世界矩阵(优先 WorldTransformComponent)。
		static glm::mat4 EntityWorldMatrix(Entity entity);

		void SetSceneState(SceneState state);
		void UpdateSceneContext(Ref<Scene> scene);
		// P2 W5b:每帧在帧边界轮询脚本文件监听(编辑态=文档场景,Play/Simulate=正在跑的场景),
		// 安全点不满足时把变化顺延到下一帧,不丢。
		void PollScriptHotReload(float deltaSeconds);
		void DoNewScene();
		void DoOpenScene(const std::filesystem::path& path);
		bool TrySave();
		void RequestAction(std::function<void()> action);
		void ProcessPendingRendererChange();
		// 渲染后端切换:保存设置后自动重启编辑器进程(热切换会串资源)。
		void RestartForRendererChange();
		void RegisterUiTextures();
		// 重载工具栏/AI 图标(旧式 GL 纹理):窗口或上下文重建后必须重新创建。
		void LoadIconTextures();
		void ShowError(const std::string& message);
		void StartCooking(const std::string& target);
		// 开发验证:WLD_CAPTURE_FRAMES=N 后把场景渲染目标写 PPM(后端无关 RHI 读回)。
		void CaptureFrameIfRequested();
		// 开发验证:WLD_HIERARCHY_CLICK=<进入 Play 后的帧数> 触发层级面板首行的真实点击回调,
		// 再等 3 帧断言选择仍属于活动场景,输出 "[dev] hierarchy-click check: PASS|FAIL" 后退出。
		// 复现的是"Play 下点层级行 → 属性面板只有 No entity selected"这条路径。
		void RunHierarchyClickCheck();
		// 开发验证:WLD_PICK_AT="x,y;x,y;…"(视口局部坐标,左上角原点)在渲染稳定后逐点拾取,
		// 打印 [dev] pick 结果并退出。用于双后端拾取回归(D7-1c:GL 与 Vulkan 必须一致)。
		void RunPickCheck();
	private:
		Ref<SceneRenderer> m_SceneRenderer;
		// 相机预览:独立的小尺寸渲染目标,与主视口共用同一条提交路径(同一台相机=同一张图)。
		Ref<SceneRenderer> m_PreviewRenderer;
		uint64_t m_PreviewTextureId = 0;
		uint32_t m_PreviewTextureGeneration = 0;
		bool m_CameraPreviewEnabled = true;
		std::unique_ptr<Gameplay::SaveService> m_SaveService;
		SceneRendererOptions m_RendererOptions;

		Ref<Scene> m_ActiveScene;
		Ref<Scene> m_RuntimeScene;
		// W1:Play 会话走 GameApp/GameHost(与 Runtime 同一条更新路径)。
		// P2 W4:Simulate 并入同一条会话路径 —— 事件/计时器/固定步长只在 GameApp::Tick 里推进,
		// 编辑器不再单独调用 OnUpdateSimulation(视图口仍按 m_SceneState 选择相机)。
		Gameplay::GameHost m_PlayHost;
		EditorDocument m_Document;

		EditorCamera m_EditorCamera;
		EditorCamera3D m_EditorCamera3D;
		bool m_Viewport3D = false;
		enum class Viewport3DDrag { None, Orbit, Pan };
		Viewport3DDrag m_Viewport3DDragging = Viewport3DDrag::None;
		glm::vec2 m_Viewport3DLastMouse { 0.0f, 0.0f };

		Entity m_SelectedEntity;

		glm::vec2 m_ViewportSize = { 0,0 };
		glm::vec2 m_ViewportBounds[2] = { {0,0}, {0,0} };

		bool m_ViewportFocused = false, m_ViewportHovered = false;
		bool m_HasRenderedScene = false;

		// Gizmo operation type
		Wui::GizmoOperation m_CurrentGizmoOperation = Wui::GizmoOperation::None;


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
		// AI 控制通道(默认关闭;EditorApp 解析 --ai-control=<port> 后开启)。
		std::unique_ptr<Editor::AiControlServer> m_AiServer;
		// 命令分发:在主线程帧内执行,结果/错误以文本返回给控制通道。
		bool ExecuteAiCommand(const std::string& cmd, const std::map<std::string, std::string>& args,
			std::string& result, std::string& error);
		std::string DescribeAiScene() const;
		// 录制:把一次通道会话写成可回放的 JSON 行脚本(record.start/stop)。
		void StartAiRecording(const std::string& path);
		size_t StopAiRecording();
		void AiRecordCommand(const std::string& cmd, const std::map<std::string, std::string>& args);
		std::string m_AiRecordPath;
		size_t m_AiRecordCount = 0;
		// P2 W5b:脚本文件监听(轮询式,150ms debounce)与"不安全顺延"的未决变化集合。
		ScriptFileWatch m_ScriptWatch;
		// ScriptFileWatch 没有枚举接口,这里镜像"当前登记过的逻辑路径"以便换场景/改路径时 Unwatch。
		std::vector<std::string> m_WatchedScriptPaths;
		std::vector<std::string> m_PendingScriptReloads;
		// 监听目标场景:换场景(进入/退出 Play、开关文档)时必须重建基线,不能跨场景复用。
		Scene* m_ScriptWatchScene = nullptr;
		Wui::WuiContext m_WuiContext;
		bool m_RendererChangePending = false;
		std::string m_RendererChangeName;
		uint64_t m_SceneTextureId = 0;
		uint64_t m_IconIds[8] = {};
		uint32_t m_TextureEpoch = 0;
		uint32_t m_UiTextureGeneration = 0;
		// 视口目标延迟重建:拖拽分隔条时尺寸每帧变化,逐帧销毁/重建渲染目标
		// 会在 GPU 仍采样旧纹理时释放资源(Vulkan 下会卡死)。尺寸稳定后再重建。
		glm::vec2 m_PendingViewportSize { 0, 0 };
		float m_ViewportResizeDelay = 0.0f;
		// WLD_HIERARCHY_CLICK 自动化状态:-2 = 未启用。
		int m_DevClickFramesAfterPlay = -1;
		int m_DevClickPlayFrames = 0;
		int m_DevClickVerifyCountdown = -1;
		// WLD_PICK_AT 自动化状态:-2 = 未启用。
		int m_DevPickFrames = -1;
		int m_DevPickFrameCount = 0;
		bool m_DevPickSelfTest = false;
		std::vector<glm::vec2> m_DevPickPoints;
	};

}
