#include "EditorLayer.h"
#include "World/Core/Asset/BuiltinImporters.h"
#include "World/Core/Asset/CookPipeline.h"
#include "World/Core/Asset/ProjectManifest.h"
#include "World/Core/Thread/JobSystem.h"
#include "World/Core/Vfs/DirectoryProvider.h"
#include "World/Core/Vfs/PackageProvider.h"
#include "World/Modules/GameModuleHost.h"
#include "World/Scene/ScriptEngine.h"
#include "World/WUI/WuiRhiBackend.h"
#include "World/WUI/WuiTextureRegistry.h"
#include <filesystem>
#include <stdexcept>
namespace World
{
	EditorLayer::EditorLayer()
		: Layer("EditorLayer"), m_Document(Application::Get().GetContext()), m_Shell(*this)
	{
		m_Commands.Register({ Wui::HashId("cmd.new"), "New", KeyCodes::N, true, false, [this] { NewScene(); } });
		m_Commands.Register({ Wui::HashId("cmd.open"), "Open", KeyCodes::O, true, false, [this] { OpenScene(); } });
		m_Commands.Register({ Wui::HashId("cmd.save"), "Save", KeyCodes::S, true, false, [this] { SaveScene(); } });
		m_Commands.Register({ Wui::HashId("cmd.duplicate"), "Duplicate", KeyCodes::D, true, false, [this] { DuplicateSelectedEntity(); } });
		m_Commands.Register({ Wui::HashId("cmd.gizmo_none"), "Gizmo None", KeyCodes::Q, false, false, [this] { SetGizmoOperation(Wui::GizmoOperation::None); } });
		m_Commands.Register({ Wui::HashId("cmd.gizmo_move"), "Gizmo Move", KeyCodes::W, false, false, [this] { SetGizmoOperation(Wui::GizmoOperation::Translate); } });
		m_Commands.Register({ Wui::HashId("cmd.gizmo_rotate"), "Gizmo Rotate", KeyCodes::E, false, false, [this] { SetGizmoOperation(Wui::GizmoOperation::Rotate); } });
		m_Commands.Register({ Wui::HashId("cmd.gizmo_scale"), "Gizmo Scale", KeyCodes::R, false, false, [this] { SetGizmoOperation(Wui::GizmoOperation::Scale); } });
		m_Commands.Register({ Wui::HashId("cmd.play"), "Play", KeyCodes::F5, false, false, [this] { TogglePlay(); } });
		m_Commands.Register({ Wui::HashId("cmd.simulate"), "Simulate", KeyCodes::F6, false, false, [this] { ToggleSimulate(); } });
		m_Commands.Register({ Wui::HashId("cmd.pause"), "Pause", KeyCodes::F7, false, false, [this] { TogglePause(); } });
	}
	void EditorLayer::OnAttach()
	{
		WLD_PROFILE_FUNCTION();
		std::string moduleError;
		if (!Modules::GameModuleHost::LoadDefault(Application::Get().GetContext(), &moduleError))
			WLD_CORE_ERROR("Failed to load Game module: {0}", moduleError);

		// 开发期资产:编辑器与 Runtime 一致,经 VFS 目录 provider 读内容。
		Application::Get().GetContext().Vfs().Mount("dir:game-assets",
			std::make_shared<World::Vfs::DirectoryProvider>(std::string(WLD_ASSETPATH)), 100);

		// Application initialized Lua before attach; Game registration is now merged.
		if (!ScriptEngine::GenerateLuaStubs())
			WLD_CORE_ERROR("Automatic Lua API stub generation failed; keeping the last valid declarations.");

		m_SceneRenderer = CreateRef<SceneRenderer>();
		m_SceneRenderer->Init();

		m_IconPlay = Texture2D::Create("Resource/Icons/Icon_Play.png");

		m_IconStop = Texture2D::Create("Resource/Icons/Icon_Stop.png");

		m_IconPause = Texture2D::Create("Resource/Icons/Icon_Pause.png");
		m_IconContinue = Texture2D::Create("Resource/Icons/Icon_Continue.png");

		m_IconSimulate = Texture2D::Create("Resource/Icons/Icon_SimulateStart.png");
		m_IconSimulateStop = Texture2D::Create("Resource/Icons/Icon_SimulateStop.png");
		m_IconSimulatePause = Texture2D::Create("Resource/Icons/Icon_SimulatePause.png");
		m_IconSimulateContinue = Texture2D::Create("Resource/Icons/Icon_SimulateContinue.png");

		RegisterUiTextures();

		NewScene();

		m_EditorCamera = EditorCamera(45.0f, 1.6f / 0.9f, 0.1f, 1000.0f);
	}

	void EditorLayer::OnDetach()
	{
		WLD_PROFILE_FUNCTION();
		if (m_CookingThread.joinable())
			m_CookingThread.join();
		m_ShowCookingProgress = false;
		m_HasRenderedScene = false;

		SetSceneState(SceneState::Edit);
		
		m_ActiveScene.reset();
		m_RuntimeScene.reset();
		m_Document = EditorDocument(Application::Get().GetContext());
		if (m_SceneRenderer)
		{
			m_SceneRenderer->Shutdown();
			m_SceneRenderer.reset();
		}
		ExportOperationLog();
	}

	void EditorLayer::OnUpdate(Timestep ts)
	{
		WLD_PROFILE_FUNCTION();
		ProcessPendingRendererChange();
		m_HasRenderedScene = false;
		if (!m_ActiveScene || !m_SceneRenderer)
			return;
		//WLD_CORE_TRACE("Delta Time: {0} ({1} FPS)", ts.GetSeconds(), ts.GetFPS());

		{
			// TODO: 停止聚焦时，摄像机不再更新,这不太合理，应该让摄像机在停止聚焦时继续更新，但不处理输入事件
			if (m_ViewportFocused)
			{
				m_EditorCamera.OnUpdate(ts);
			}
		}

		Renderer2D::ResetStats();


		WLD_PROFILE_SCOPE("Renderer Clear");
		Camera* renderCamera = &m_EditorCamera;
		glm::mat4 renderCameraTransform = m_EditorCamera.GetTransform();


		{
			WLD_PROFILE_SCOPE("Renderer Draw");
			switch (m_SceneState)
			{
				case SceneState::Edit:
					m_ActiveScene->OnUpdateEditor(ts, m_EditorCamera);
					break;
				case SceneState::Play:
					if (!m_ScenePaused)
					{
						m_ActiveScene->OnUpdateRuntime(ts);
					}
					else
						m_ActiveScene->FlushStructuralChanges();
					break;
				case SceneState::Simulate:
					if (!m_ScenePaused)
						m_ActiveScene->OnUpdateSimulation(ts, m_EditorCamera);
					else
						m_ActiveScene->FlushStructuralChanges();
					break;
			}

		}

		Entity selectedEntity = m_SelectedEntity;
		if (!selectedEntity.IsValid() || selectedEntity.GetScene() != m_ActiveScene.get() ||
			m_ActiveScene->IsPendingDestroy(selectedEntity))
		{
			m_SelectedEntity = {};
			selectedEntity = {};
		}
		else if (!selectedEntity.HasComponent<TransformComponent>())
			selectedEntity = {}; // Keep Inspector selection, but omit the renderer's outline.

		// No component reference survives Scene update or its structural flush.
		if (m_SceneState == SceneState::Play && !m_ScenePaused)
		{
			Entity cameraEntity = m_ActiveScene->GetPrimaryCameraEntity();
			if (!cameraEntity.IsValid() || m_ActiveScene->IsPendingDestroy(cameraEntity) ||
				!cameraEntity.HasComponent<CameraComponent>() || !cameraEntity.HasComponent<TransformComponent>())
				return;
			renderCamera = &cameraEntity.GetComponent<CameraComponent>().Camera;
			renderCameraTransform = cameraEntity.GetComponent<TransformComponent>().Transform;
		}

		m_SceneRenderer->BeginScene(m_ActiveScene.get(), m_RendererOptions);
		m_SceneRenderer->SubmitScene(*renderCamera, renderCameraTransform, selectedEntity);
		m_SceneRenderer->EndScene();
		m_HasRenderedScene = true;
	}


	void EditorLayer::OnImGuiRender()
	{
		WLD_PROFILE_FUNCTION();

		static Wui::WuiRhiBackend wuiBackend;

		Wui::WuiInputState input;
		if (wuiBackend.BeginFrame(input))
		{
			m_WuiContext.BeginFrame(input);
			if (m_SceneRenderer && m_SceneRenderer->GetColorTexture())
			{
				if (!m_SceneTextureId)
					m_SceneTextureId = Wui::WuiTextureRegistry::Get().Register(m_SceneRenderer->GetColorTexture());
				else
					Wui::WuiTextureRegistry::Get().Update(m_SceneTextureId, m_SceneRenderer->GetColorTexture());
			}
			m_Shell.OnRender(m_WuiContext);
			m_WuiContext.EndFrame();
			wuiBackend.Render(m_WuiContext.Commands(), m_WuiContext.OverlayCommands());
		}
		wuiBackend.EndFrame(m_WuiContext.Cursor());
	}

	void EditorLayer::ExportOperationLog()
	{
		const std::string path = std::string(WLD_OUTPUT_DIR) + "wui-ops.json";
		std::string error;
		if (m_WuiContext.Ops().Save(path, &error))
			WLD_CORE_INFO("WUI operation log exported to {0}", path);
		else
			WLD_CORE_WARN("Failed to export WUI operation log: {0}", error);
	}

	void EditorLayer::OnEvent(Event& event)
	{
		WLD_PROFILE_FUNCTION();

		if (m_ViewportFocused && m_ViewportHovered)
			m_EditorCamera.OnEvent(event);

		EventDispatcher dispatcher(event);

		dispatcher.Dispatch<WindowCloseEvent>(WLD_BIND_EVENT_FN(EditorLayer::OnWindowClose));
		dispatcher.Dispatch<KeyPressedEvent>(WLD_BIND_EVENT_FN(EditorLayer::OnKeyPressed));
	}
	bool EditorLayer::OnWindowClose(WindowCloseEvent& e)
	{
		// 确认框已打开：继续拦截关闭，等待用户在框内选择。
		if (m_ShowUnsavedModal)
		{
			e.m_Handled = true;
			return true;
		}
		if (m_Document.IsDirty())
		{
			RequestAction([this]() { World::Application::Get().Close(); });
			e.m_Handled = true;
			return true;
		}
		return false;
	}
	void EditorLayer::NewScene()
	{
		RequestAction([this]() { DoNewScene(); });
	}
	void EditorLayer::DoNewScene()
	{
		SetSceneState(SceneState::Edit);

		m_Document.New();
		UpdateSceneContext(m_Document.GetScene());
	}
	void EditorLayer::OpenScene()
	{
		std::string path = FileDialogs::OpenFile("Scene File (*.wd)\0*.wd\0");
		if (path.empty())
			return;
		OpenScene(std::filesystem::path(path));
	}
	void EditorLayer::OpenScene(const std::filesystem::path& path)
	{
		if (path.empty())
			return;
		RequestAction([this, path]() { DoOpenScene(path); });
	}
	void EditorLayer::DoOpenScene(const std::filesystem::path& path)
	{
		if (!m_Document.LoadFromFile(path))
		{
			ShowError(m_Document.GetLastError());
			return;
		}
		SetSceneState(SceneState::Edit);
		UpdateSceneContext(m_Document.GetScene());
	}
	bool EditorLayer::SaveScene()
	{
		if (TrySave())
			return true;
		if (!m_Document.GetLastError().empty())
			ShowError(m_Document.GetLastError());
		return false;
	}
	void EditorLayer::StartCookingAction()
	{
		const std::string target =
			World::FileDialogs::SelectFolder("Select the output folder for the game package");
		if (!target.empty())
			StartCooking(target);
	}

	void EditorLayer::GenerateLuaStubsAction()
	{
		if (!ScriptEngine::GenerateLuaStubs())
			WLD_CORE_ERROR("Lua API stub generation failed; keeping the last valid declarations.");
	}

	void EditorLayer::CloseAction()
	{
		RequestAction([this]() { World::Application::Get().Close(); });
	}

	void EditorLayer::DuplicateSelectedEntity()
	{
		Entity selectedEntity = m_SelectedEntity;
		if (!m_ActiveScene || !selectedEntity.IsValid() || selectedEntity.GetScene() != m_ActiveScene.get() ||
			m_ActiveScene->IsPendingDestroy(selectedEntity))
		{
			m_SelectedEntity = {};
			return;
		}
		try
		{
			if (m_ActiveScene->IsActive() &&
				(selectedEntity.HasComponent<RigidBody2DComponent>() || selectedEntity.HasComponent<BoxCollider2DComponent>() ||
					selectedEntity.HasComponent<CircleCollider2DComponent>()))
			{
				WLD_CORE_WARN("Cannot duplicate physics entities while the scene is active. Stop the scene first.");
				return;
			}
			const entt::entity handle = selectedEntity;
			if (m_ActiveScene->DeferStructuralChange([handle](Scene& scene)
			{
				Entity source(&scene, handle);
				if (source.IsValid() && !scene.IsPendingDestroy(handle))
					scene.DuplicateEntity(source);
			}))
			{
				MarkDocumentDirty();
			}
			else
				WLD_CORE_WARN("Duplicate entity request was rejected by the scene.");
		}
		catch (const std::exception& error)
		{
			WLD_CORE_ERROR("Unable to duplicate entity: {0}", error.what());
		}
		catch (...)
		{
			WLD_CORE_ERROR("Unable to duplicate entity: unknown error.");
		}
	}

	void EditorLayer::ApplyRendererChange(const std::string& name)
	{
		if (!Renderer::SetRequestedRenderer(name))
			return;
		m_RendererChangeName = name;
		m_RendererChangePending = true;
	}

	void EditorLayer::ProcessPendingRendererChange()
	{
		if (!m_RendererChangePending)
			return;
		m_RendererChangePending = false;

		// 在渲染开始前重建:上一帧的绘制命令已消费完,销毁旧设备/目标是安全的。
		if (m_SceneRenderer)
			m_SceneRenderer->Shutdown();
		Renderer::Shutdown();
		Renderer::Init(m_RendererChangeName);
		if (m_SceneRenderer)
			m_SceneRenderer->Init();
		RegisterUiTextures();

		m_ViewportSize = { 0, 0 };
		WLD_CORE_INFO("Editor renderer switched to {0}", Renderer::GetBackendName());
	}

	void EditorLayer::RegisterUiTextures()
	{
		auto& registry = Wui::WuiTextureRegistry::Get();
		if (m_SceneRenderer && m_SceneRenderer->GetColorTexture())
			m_SceneTextureId = registry.Register(m_SceneRenderer->GetColorTexture());
		const Ref<Texture2D> icons[8] = {
			m_IconPlay, m_IconStop, m_IconPause, m_IconContinue,
			m_IconSimulate, m_IconSimulateStop, m_IconSimulatePause, m_IconSimulateContinue,
		};
		for (int i = 0; i < 8; ++i)
			m_IconIds[i] = registry.RegisterTexture2D(icons[i]);
	}

	uint64_t EditorLayer::GetIconId(int index) const
	{
		if (index < 0 || index >= 8)
			return 0;
		return m_IconIds[index];
	}

	void EditorLayer::TogglePlay()
	{
		SetSceneState(m_SceneState == SceneState::Play ? SceneState::Edit : SceneState::Play);
	}

	void EditorLayer::ToggleSimulate()
	{
		SetSceneState(m_SceneState == SceneState::Simulate ? SceneState::Edit : SceneState::Simulate);
	}

	void EditorLayer::TogglePause()
	{
		if (m_SceneState == SceneState::Play || m_SceneState == SceneState::Simulate)
			m_ScenePaused = !m_ScenePaused;
	}

	Ref<Texture2D> EditorLayer::GetIcon(int index) const
	{
		switch (index)
		{
			case 1: return m_IconStop;
			case 2: return m_IconPause;
			case 3: return m_IconContinue;
			case 4: return m_IconSimulate;
			case 5: return m_IconSimulateStop;
			case 6: return m_IconSimulatePause;
			case 7: return m_IconSimulateContinue;
			default: return m_IconPlay;
		}
	}

	void EditorLayer::SetViewportState(bool focused, bool hovered, glm::vec2 size, glm::vec2 bounds[2])
	{
		m_ViewportFocused = focused;
		m_ViewportHovered = hovered;
		m_ViewportBounds[0] = bounds[0];
		m_ViewportBounds[1] = bounds[1];
		if (size.x > 0 && size.y > 0 && (m_ViewportSize.x != size.x || m_ViewportSize.y != size.y))
		{
			m_ViewportSize = size;
			if (m_SceneRenderer)
				m_SceneRenderer->GetTargetFramebuffer()->Resize((uint32_t)size.x, (uint32_t)size.y);
			m_EditorCamera.SetViewportSize(size.x, size.y);
			if (m_ActiveScene)
				m_ActiveScene->OnViewportResize((uint32_t)size.x, (uint32_t)size.y);
		}

	}

	void EditorLayer::ResolveUnsavedModal(bool save)
	{
		std::function<void()> action = std::move(m_PendingAction);
		m_PendingAction = nullptr;
		m_ShowUnsavedModal = false;
		if (save && !TrySave())
		{
			if (!m_Document.GetLastError().empty())
				WLD_CORE_ERROR("{0}", m_Document.GetLastError());
			return;
		}
		if (action)
			action();
	}

	void EditorLayer::CancelUnsavedModal()
	{
		m_PendingAction = nullptr;
		m_ShowUnsavedModal = false;
	}

	bool EditorLayer::TrySave()
	{
		m_Document.ClearError();
		if (!m_Document.HasPath())
		{
			std::string path = FileDialogs::SaveFile("Scene File (*.wd)\0*.wd\0");
			if (path.empty())
				return false;
			return m_Document.SaveTo(std::filesystem::path(path));
		}
		return m_Document.SaveTo(m_Document.GetPath());
	}
	bool EditorLayer::OnKeyPressed(KeyPressedEvent& e)
	{
		if (e.GetRepeatCount() > 0)
			return false;

		bool control = Input::IsKeyPressed(KeyCodes::LeftControl) || Input::IsKeyPressed(KeyCodes::RightControl);
		bool shift = Input::IsKeyPressed(KeyCodes::LeftShift) || Input::IsKeyPressed(KeyCodes::RightShift);


		return m_Commands.HandleKey(e.GetKeyCode(), control, shift);
	}
	Entity EditorLayer::GetEntityAtMousePosition(glm::vec2 viewportLocal)
	{
		if (!m_HasRenderedScene || !m_ActiveScene || !m_SceneRenderer)
			return {};
		// --- 鼠标交互逻辑 ---

		// 获取鼠标在屏幕上的绝对位置
		glm::vec2 viewportSizeAvail = { m_ViewportBounds[1].x - m_ViewportBounds[0].x, m_ViewportBounds[1].y - m_ViewportBounds[0].y };

		// 转换到视口局部坐标 (0,0) 是左上角

		// 翻转 Y 轴，因为 OpenGL 的 (0,0) 在左下角，而 ImGui 在左上角
		int mouseX = (int)viewportLocal.x;
		int mouseY = (int)(viewportSizeAvail.y - viewportLocal.y);

		int pixelData = -1;
		// 边界检查：只有当鼠标在黑色内容区内时才读取
		if (mouseX >= 0 && mouseY >= 0 && mouseX < (int)viewportSizeAvail.x && mouseY < (int)viewportSizeAvail.y)
		{
			pixelData = m_SceneRenderer->GetTargetFramebuffer()->ReadPixel(1, mouseX, mouseY);
			//WLD_CORE_TRACE("Pixel Data at ({0}, {1}): {2}", mouseX, mouseY, pixelData);
		}

		Entity result = pixelData == -1 ? Entity() : Entity(m_ActiveScene.get(), (entt::entity)pixelData);

		return result.IsValid() && !m_ActiveScene->IsPendingDestroy(result) ? result : Entity{};
	}
	void EditorLayer::SetSceneState(SceneState state)
	{
		if (m_SceneState == state) return;

		// End the previous mode before replacing any scene references.
		if (m_RuntimeScene)
		{
			if (m_SceneState == SceneState::Play)
				m_RuntimeScene->OnRuntimeStop();
			else if (m_SceneState == SceneState::Simulate)
				m_RuntimeScene->OnSimulationStop();
		}
		m_SceneState = SceneState::Edit;
		m_ScenePaused = false;
		UpdateSceneContext(m_Document.GetScene());
		m_RuntimeScene.reset();
		if (state == SceneState::Edit || !m_Document.GetScene())
			return;

		try
		{
			m_RuntimeScene = CreateRef<Scene>(Application::Get().GetContext());
			Ref<Scene> editorScene = m_Document.GetScene();
			Scene::CopyScene(editorScene, m_RuntimeScene);
			m_SceneState = state;
			UpdateSceneContext(m_RuntimeScene);
			if (state == SceneState::Play)
				m_RuntimeScene->OnRuntimeStart();
			else
				m_RuntimeScene->OnSimulationStart();
		}
		catch (const std::exception& error)
		{
			WLD_CORE_ERROR("Unable to start scene: {0}", error.what());
			if (m_RuntimeScene)
				m_RuntimeScene->OnRuntimeStop();
			UpdateSceneContext(m_Document.GetScene());
			m_RuntimeScene.reset();
			m_SceneState = SceneState::Edit;
		}
		catch (...)
		{
			WLD_CORE_ERROR("Unable to start scene: unknown error.");
			if (m_RuntimeScene)
				m_RuntimeScene->OnRuntimeStop();
			UpdateSceneContext(m_Document.GetScene());
			m_RuntimeScene.reset();
			m_SceneState = SceneState::Edit;
		}
	}
	void EditorLayer::UpdateSceneContext(Ref<Scene> scene)
	{
		m_HasRenderedScene = false;
		m_ActiveScene = scene;
		if (m_ActiveScene && m_ViewportSize.x > 0.0f && m_ViewportSize.y > 0.0f)
		{
			m_ActiveScene->OnViewportResize((uint32_t)m_ViewportSize.x, (uint32_t)m_ViewportSize.y);
		}
	}

	void EditorLayer::RequestAction(std::function<void()> action)
	{
		if (m_Document.IsDirty())
		{
			m_PendingAction = std::move(action);
			m_ShowUnsavedModal = true;
			return;
		}
		action();
	}

	void EditorLayer::ShowError(const std::string& message)
	{
		m_ErrorText = message;
		m_ShowErrorModal = true;
		WLD_CORE_ERROR("{0}", message);
	}



	void EditorLayer::StartCooking(const std::string& target)
	{
		if (m_CookingThread.joinable())
		{
			if (!m_CookingFinished.load(std::memory_order_acquire))
				return;
			m_CookingThread.join();
		}
		m_ShowCookingProgress = true;
		m_CookingSucceeded = false;
		m_CookingError.clear();
		m_CookingFinished.store(false, std::memory_order_relaxed);
		try
		{
			m_CookingThread = std::thread([target, this]()
			{
				try
				{
					namespace fs = std::filesystem;
					fs::path publishDir(target);
					fs::create_directories(publishDir);

					// 1. 项目清单(单一事实源)。
					const fs::path projectManifestPath =
						std::string(WLD_GAME_DIR) + "project.we.yaml";
					World::Asset::ProjectManifest manifest;
					std::string manifestError;
					if (!World::Asset::ProjectManifest::Load(projectManifestPath, &manifest, &manifestError))
						throw std::runtime_error("Project manifest load failed: " + manifestError);
					if (manifest.Packages.empty())
						throw std::runtime_error("Project manifest declares no packages");

					// 2. 当前打开场景若在内容根内,覆盖启动场景。
					if (m_Document.HasPath())
					{
						const fs::path contentRoot = manifest.ResolveContentRoot(projectManifestPath);
						const fs::path relative = fs::relative(m_Document.GetPath(), contentRoot);
						if (!relative.empty() && relative.generic_string().find("..") == std::string::npos)
							manifest.StartScene = relative.generic_string();
					}

					// 3. 增量烘焙(构建期缓存目录,仅此一处使用构建路径)。
					const fs::path cookedDir = fs::absolute(std::string(WLD_OUTPUT_DIR) + "cooked");
					World::Asset::CookPipeline pipeline(World::Asset::DefaultImporters());
					World::Asset::CookSummary summary;
					const std::vector<World::Asset::CookEntryResult> results =
						pipeline.Cook(manifest, projectManifestPath, cookedDir, false, &summary);
					for (const World::Asset::CookEntryResult& result : results)
						if (result.Failed)
							WLD_CORE_ERROR("Cook failed: {0}: {1}", result.Path, result.Error);
					WLD_CORE_INFO("Cooked {0} assets ({1} changed, {2} skipped, {3} failed)",
						summary.Total, summary.Changed, summary.Skipped, summary.Failed);
					if (summary.Failed)
						throw std::runtime_error("Asset cooking failed");

					// 4. 打包 cooked 产物为发行包。
					const fs::path outPakFile = publishDir / manifest.Packages[0];
					fs::create_directories(outPakFile.parent_path());
					std::error_code pakEc;
					if (!World::Vfs::PackageProvider::BuildFromDirectory(cookedDir / "cooked", outPakFile, pakEc))
						throw std::runtime_error("Package build failed: " +
							(pakEc ? pakEc.message() : outPakFile.string()));

					// 5. 拷贝运行时:Runtime.exe + 单份 WorldRuntime.dll + bin/Game.dll。
					fs::path srcRuntimeOutputDir = fs::absolute(std::string(WLD_OUTPUT_DIR) + "Runtime/" + WLD_BUILD_TYPE);
					fs::path srcRuntimeExe = srcRuntimeOutputDir / "Runtime.exe";
					if (!fs::is_regular_file(srcRuntimeExe))
						throw std::runtime_error("Runtime.exe could not be located: " + srcRuntimeExe.string());
					fs::copy_file(srcRuntimeExe, publishDir / "Runtime.exe", fs::copy_options::overwrite_existing);
					WLD_CORE_INFO("Copied Runtime executable from: {0}", srcRuntimeExe.string());

					fs::path srcRuntimeDll = srcRuntimeOutputDir / "WorldRuntime.dll";
					if (!fs::is_regular_file(srcRuntimeDll))
						throw std::runtime_error("WorldRuntime.dll could not be located: " + srcRuntimeDll.string());
					fs::copy_file(srcRuntimeDll, publishDir / "WorldRuntime.dll", fs::copy_options::overwrite_existing);
					WLD_CORE_INFO("Copied WorldRuntime.dll from: {0}", srcRuntimeDll.string());

					fs::path srcGameDll = fs::absolute(std::string(WLD_OUTPUT_DIR) +
						"bin/" + WLD_BUILD_TYPE + "/Game/" + WLD_BUILD_TYPE + "/Game.dll");
					if (!fs::is_regular_file(srcGameDll))
						throw std::runtime_error("Game.dll could not be located: " + srcGameDll.string());
					fs::create_directories(publishDir / "bin");
					fs::copy_file(srcGameDll, publishDir / "bin" / "Game.dll",
						fs::copy_options::overwrite_existing);
					WLD_CORE_INFO("Copied Game.dll into bin/");

					// 6. 写发行清单(start_scene 已在第 2 步写入 manifest)。
					std::string saveError;
					if (!World::Asset::ProjectManifest::Save(publishDir / "project.we.yaml", manifest, &saveError))
						throw std::runtime_error("Project manifest save failed: " + saveError);

					WLD_CORE_INFO("Game Cooked Successfully to {0}", publishDir.string());
					m_CookingSucceeded = true;
				}
				catch (const std::exception& error)
				{
					m_CookingError = error.what();
					WLD_CORE_ERROR("Game cooking failed: {0}", m_CookingError);
				}
				catch (...)
				{
					m_CookingError = "Unknown background cooking error.";
					WLD_CORE_ERROR("{0}", m_CookingError);
				}
				m_CookingFinished.store(true, std::memory_order_release);
			});
		}
		catch (const std::exception& error)
		{
			m_CookingError = error.what();
			m_CookingFinished.store(true, std::memory_order_release);
			WLD_CORE_ERROR("Unable to start cooking: {0}", m_CookingError);
		}
	}



}
