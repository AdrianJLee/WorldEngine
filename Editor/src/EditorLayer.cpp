#include "EditorLayer.h"
#include "World/Core/Thread/JobSystem.h"
#include "World/Core/Cook/VFS.h"
#include "World/Modules/GameModuleHost.h"
#include "World/Scene/ScriptEngine.h"
#include <filesystem>
#include <stdexcept>
namespace World
{
	EditorLayer::EditorLayer()
		: Layer("EditorLayer"), m_Document(Application::Get().GetContext())
	{
		m_ContentBrowserPanel.RegisterOpenAction(".wd", [this](const std::filesystem::path& filepath)
			{
				OpenScene(filepath);
			});
		m_SceneHierarchyPanel.SetEditCallback([this]()
			{
				if (m_SceneState == SceneState::Edit && m_ActiveScene == m_Document.GetScene())
					m_Document.MarkDirty();
			});
	}
	void EditorLayer::OnAttach()
	{
		WLD_PROFILE_FUNCTION();
		std::string moduleError;
		if (!Modules::GameModuleHost::LoadDefault(Application::Get().GetContext(), &moduleError))
			WLD_CORE_ERROR("Failed to load Game module: {0}", moduleError);

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
		m_SceneHierarchyPanel.SetContext(nullptr);
		m_ActiveScene.reset();
		m_RuntimeScene.reset();
		m_Document = EditorDocument(Application::Get().GetContext());
		if (m_SceneRenderer)
		{
			m_SceneRenderer->Shutdown();
			m_SceneRenderer.reset();
		}
	}

	void EditorLayer::OnUpdate(Timestep ts)
	{
		WLD_PROFILE_FUNCTION();
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

		Entity selectedEntity = m_SceneHierarchyPanel.GetSelectedEntity();
		if (!selectedEntity.IsValid() || selectedEntity.GetScene() != m_ActiveScene.get() ||
			m_ActiveScene->IsPendingDestroy(selectedEntity))
		{
			m_SceneHierarchyPanel.SetSelectedEntity({});
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

		// DockSpace
		ImGuiLayer::ShowDockSpaceBack(false);
		// 菜单栏
		if (ImGui::BeginMenuBar())
		{
			if (ImGui::BeginMenu("File"))
			{
				if (ImGui::MenuItem("Show Demo Window", nullptr, ImGuiLayer::m_Show))
					ImGuiLayer::m_Show = !ImGuiLayer::m_Show;

				if (ImGui::MenuItem("New", "Ctrl+N"))
				{
					NewScene();
				}
				if (ImGui::MenuItem("Save", "Ctrl+S"))
				{
					SaveScene();
				}
				if (ImGui::MenuItem("Load", "Ctrl+O"))
				{
					OpenScene();
				}
				if (ImGui::MenuItem("Cooking", nullptr, false, !m_ShowCookingProgress))
				{
					std::string cookTarget = World::FileDialogs::SaveFile("Game Package\0*.*\0");
					if (!cookTarget.empty())
						StartCooking(cookTarget);
				}
				if (ImGui::MenuItem("Generate Lua API Stubs"))
				{
					if (!ScriptEngine::GenerateLuaStubs())
						WLD_CORE_ERROR("Lua API stub generation failed; keeping the last valid declarations.");
				}
				if (ImGui::MenuItem("Exit"))
					RequestAction([this]() { World::Application::Get().Close(); });

				ImGui::EndMenu();
			}
			ImGui::EndMenuBar();
		}

		ImGui::End();

		// 场景层级面板
		m_SceneHierarchyPanel.OnImGuiRender();
		m_ContentBrowserPanel.OnImGuiRender();


		// 视口
		{
			ImGui::Begin("View");

			UpdateViewBounds();

			m_ViewportFocused = ImGui::IsWindowFocused();
			m_ViewportHovered = ImGui::IsWindowHovered();
			// 如果视口没有被聚焦或悬停，则阻止事件传递给主程序
			Application::Get().GetImGuiLayer()->SetBlockEvents(!m_ViewportFocused && !m_ViewportHovered);

			void* windowHandle = ImGui::GetMainViewport()->PlatformHandleRaw;
			if (windowHandle)
			{
				// 如果当前 View 视口聚焦，并且用户没有在其他 UI 里打字，则禁止操作系统唤卡输入法
				bool disableIME = m_ViewportFocused && !ImGui::GetIO().WantTextInput;
				SystemUtils::SetIMEState(!disableIME, windowHandle);
			}

			ImVec2 viewportPanelSize = ImGui::GetContentRegionAvail();
			if (viewportPanelSize.x > 0.0f && viewportPanelSize.y > 0.0f &&
				(m_ViewportSize.x != viewportPanelSize.x || m_ViewportSize.y != viewportPanelSize.y))
			{
				m_SceneRenderer->GetTargetFramebuffer()->Resize((uint32_t)viewportPanelSize.x, (uint32_t)viewportPanelSize.y);
				m_ViewportSize = { viewportPanelSize.x, viewportPanelSize.y };
				m_EditorCamera.SetViewportSize(viewportPanelSize.x, viewportPanelSize.y);
				m_ActiveScene->OnViewportResize((uint32_t)viewportPanelSize.x, (uint32_t)viewportPanelSize.y);

			}
			if (m_HasRenderedScene)
				ImGui::Image((void*)m_SceneRenderer->GetTargetFramebuffer()->GetColorAttachmentRendererID(),
					ImVec2 { m_ViewportSize.x,m_ViewportSize.y }, ImVec2 { 0,1 }, ImVec2 { 1,0 });
			else
			{
				const ImVec2 messagePosition = ImGui::GetCursorScreenPos();
				ImGui::Dummy(ImVec2 { m_ViewportSize.x, m_ViewportSize.y });
				ImGui::GetWindowDrawList()->AddText(ImVec2(messagePosition.x + 10.0f, messagePosition.y + 65.0f),
					ImGui::GetColorU32(ImGuiCol_TextDisabled), "No scene view available. Play requires a Camera and Transform.");
			}

			// 工具栏
			UI_Toolbar();

			Entity selectedEntity = m_SceneHierarchyPanel.GetSelectedEntity();
			if (selectedEntity.IsValid() && selectedEntity.GetScene() == m_ActiveScene.get() &&
				!m_ActiveScene->IsPendingDestroy(selectedEntity) && selectedEntity.HasComponent<TransformComponent>() && m_HasRenderedScene)
			{
				// Draw Gizmo：拖动起止沿用于标脏（不做撤销）。
				auto& transform = selectedEntity.GetComponent<TransformComponent>();
				const bool wasUsing = ImGuizmo::IsUsing();
				const TransformComponent beforeTransform = transform;
				ImGuiDrawLibrary::DrawGizmo(m_EditorCamera, selectedEntity, m_CurrentGizmoOperation);
				const bool nowUsing = ImGuizmo::IsUsing();
				if (!wasUsing && nowUsing)
				{
					m_GizmoDragging = true;
					m_GizmoDragBefore = beforeTransform;
				}
				else if (wasUsing && !nowUsing && m_GizmoDragging)
				{
					m_GizmoDragging = false;
					const auto& afterTransform = selectedEntity.GetComponent<TransformComponent>();
					if (m_SceneState == SceneState::Edit && selectedEntity.GetScene() == m_Document.GetScene().get() &&
						(afterTransform.Location != m_GizmoDragBefore.Location ||
							afterTransform.Rotation != m_GizmoDragBefore.Rotation ||
							afterTransform.Scale != m_GizmoDragBefore.Scale ||
							afterTransform.RotationQuat != m_GizmoDragBefore.RotationQuat))
					{
						m_Document.MarkDirty();
					}
				}
			}

			ImGui::End();
		}

		// 统计面板
		{
			ImGui::Begin("Settings");

			{
				ImGui::Separator();
				ImGui::Text("Application average %.1f FPS", ImGui::GetIO().Framerate);
				ImGui::Text("Renderer2D Stats:");
				ImGui::Text("Draw Calls: %d", Renderer2D::GetStats().DrawCalls);
				ImGui::Text("Quads: %d", Renderer2D::GetStats().QuadCount);
				ImGui::Text("Circles: %d", Renderer2D::GetStats().CircleCount);
				ImGui::Text("Vertices: %d", Renderer2D::GetStats().GetTotalVertexCount());
				ImGui::Text("Indices: %d", Renderer2D::GetStats().GetTotalIndexCount());
				ImGui::Separator();
			}

			ImGui::End();
		}

		if (m_ShowCookingProgress)
		{
			OnCooking();
		}

		DrawUnsavedModal();
		DrawErrorModal();
	}

	void EditorLayer::OnEvent(Event& event)
	{
		WLD_PROFILE_FUNCTION();

		m_EditorCamera.OnEvent(event);

		EventDispatcher dispatcher(event);

		dispatcher.Dispatch<WindowCloseEvent>(WLD_BIND_EVENT_FN(EditorLayer::OnWindowClose));
		dispatcher.Dispatch<KeyPressedEvent>(WLD_BIND_EVENT_FN(EditorLayer::OnKeyPressed));
		dispatcher.Dispatch<MouseButtonPressedEvent>(WLD_BIND_EVENT_FN(EditorLayer::OnMouseButtonPressed));
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


		switch (e.GetKeyCode())
		{
			case KeyCodes::N:
			{
				if (control)
					NewScene();
				break;
			}

			case KeyCodes::O:
			{
				if (control)
					OpenScene();
				break;
			}

			case KeyCodes::S:
			{
				if (control)
					SaveScene();
				break;
			}

			case KeyCodes::Q:
			{
				m_CurrentGizmoOperation = (ImGuizmo::OPERATION)-1;
				break;
			}
			case KeyCodes::W:
			{
				m_CurrentGizmoOperation = ImGuizmo::TRANSLATE;
				break;
			}
			case KeyCodes::E:
			{
				m_CurrentGizmoOperation = ImGuizmo::ROTATE;
				break;
			}
			case KeyCodes::R:
			{
				m_CurrentGizmoOperation = ImGuizmo::SCALE;
				break;
			}
			case KeyCodes::D:
			{
				if (control)
				{
					Entity selectedEntity = m_SceneHierarchyPanel.GetSelectedEntity();
					if (!m_ActiveScene || !selectedEntity.IsValid() || selectedEntity.GetScene() != m_ActiveScene.get() ||
						m_ActiveScene->IsPendingDestroy(selectedEntity))
					{
						m_SceneHierarchyPanel.SetSelectedEntity({});
						break;
					}
					try
					{
						if (m_ActiveScene->IsActive() &&
							(selectedEntity.HasComponent<RigidBody2DComponent>() || selectedEntity.HasComponent<BoxCollider2DComponent>() ||
							 selectedEntity.HasComponent<CircleCollider2DComponent>()))
						{
							WLD_CORE_WARN("Cannot duplicate physics entities while the scene is active. Stop the scene first.");
							break;
						}
						const entt::entity handle = selectedEntity;
						if (m_ActiveScene->DeferStructuralChange([handle](Scene& scene)
						{
							Entity source(&scene, handle);
							if (source.IsValid() && !scene.IsPendingDestroy(handle))
								scene.DuplicateEntity(source);
						}))
						{
							if (m_SceneState == SceneState::Edit && m_ActiveScene == m_Document.GetScene())
								m_Document.MarkDirty();
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
				break;
			}
		}

		return false;
	}
	bool EditorLayer::OnMouseButtonPressed(MouseButtonPressedEvent& e)
	{
		if (e.GetMouseButton() == MouseCodes::ButtonLeft)
		{
			if (m_ViewportHovered && !ImGuizmo::IsOver() && !Input::IsKeyPressed(KeyCodes::LeftAlt))
			{
				//GetEntityAtMousePosition();
				m_SceneHierarchyPanel.SetSelectedEntity(GetEntityAtMousePosition());
			}
		}
		return false;
	}
	void EditorLayer::UpdateViewBounds()
	{
		// 1. 获取视口（黑色绘图区）的左上角屏幕坐标
		// GetCursorScreenPos 会自动处理标题栏高度，直接指向绘图区的起始点
		ImVec2 minBound = ImGui::GetCursorScreenPos();

		// 2. 获取视口（黑色绘图区）的实际可用尺寸
		// 这不含标题栏和滚动条，正是你 Framebuffer 渲染的大小
		ImVec2 viewportSizeAvail = ImGui::GetContentRegionAvail();

		// 3. 计算右下角坐标
		ImVec2 maxBound = { minBound.x + viewportSizeAvail.x, minBound.y + viewportSizeAvail.y };

		// 保存边界（用于后续坐标转换）
		m_ViewportBounds[0] = { minBound.x, minBound.y };
		m_ViewportBounds[1] = { maxBound.x, maxBound.y };
	}
	Entity EditorLayer::GetEntityAtMousePosition()
	{
		if (!m_HasRenderedScene || !m_ActiveScene || !m_SceneRenderer)
			return {};
		// --- 鼠标交互逻辑 ---

		// 获取鼠标在屏幕上的绝对位置
		glm::vec2 viewportSizeAvail = { m_ViewportBounds[1].x - m_ViewportBounds[0].x, m_ViewportBounds[1].y - m_ViewportBounds[0].y };
		auto [mouseX_Screen, mouseY_Screen] = ImGui::GetMousePos();

		// 转换到视口局部坐标 (0,0) 是左上角
		float localX = mouseX_Screen - m_ViewportBounds[0].x;
		float localY = mouseY_Screen - m_ViewportBounds[0].y;

		// 翻转 Y 轴，因为 OpenGL 的 (0,0) 在左下角，而 ImGui 在左上角
		int mouseX = (int)localX;
		int mouseY = (int)(viewportSizeAvail.y - localY);

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
	void EditorLayer::UI_Toolbar()
	{

		// 去除默认按钮背景，以便我们自己控制底板
		ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
		auto& colors = ImGui::GetStyle().Colors;
		ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(colors[ImGuiCol_ButtonHovered].x, colors[ImGuiCol_ButtonHovered].y, colors[ImGuiCol_ButtonHovered].z, 0.5f));
		ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(colors[ImGuiCol_ButtonActive].x, colors[ImGuiCol_ButtonActive].y, colors[ImGuiCol_ButtonActive].z, 0.5f));

		ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
		ImGui::PushStyleVar(ImGuiStyleVar_ItemInnerSpacing, ImVec2(0, 0));
		ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 0));
		ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0, 0)); // 让 Button 的实际大小精准等于传入的 iconSize


		float iconSize = 28.0f; // 图标尺寸
		float spacing = 8.0f;   // 按钮间距
		float padding = 8.0f;   // 底板内边距

		// 当前我们需要4个按钮组 Play | Pause | Simulate | SimulatePause 
		float buttonsWidth = (iconSize * 3.0f) + (spacing * 2.0f);
		float panelWidth = buttonsWidth + (padding * 2.0f);
		float panelHeight = iconSize + (padding * 2.0f);

		// 设置底板在视口中的悬浮位置（横向居中，纵向靠上）
		ImVec2 contentMin = ImGui::GetWindowContentRegionMin();
		ImGui::SetCursorPos(ImVec2(contentMin.x + (m_ViewportSize.x - panelWidth) * 0.5f, contentMin.y + 15.0f));

		// 绘制半透明圆角底板
		ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.12f, 0.12f, 0.12f, 0.85f));
		ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 6.0f);

		if (ImGui::BeginChild("##ToolbarOverlay", ImVec2(panelWidth, panelHeight), false, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse))
		{
			ImGui::SetCursorPos(ImVec2(padding, padding)); // 将绘制起点往内收起 Padding 的距离

			bool isPlayMode = m_SceneState == SceneState::Play;
			bool isSimulateMode = m_SceneState == SceneState::Simulate;

			// ==== 1. Play / Stop 按钮 ====
			Ref<Texture2D> playIcon = isPlayMode ? m_IconStop : m_IconPlay;
			if (isSimulateMode) ImGui::PushStyleVar(ImGuiStyleVar_Alpha, 0.5f); // 模拟状态下 Play 置灰

			if (ImGui::ImageButton((ImTextureID)(uint64_t)playIcon->GetRendererID(), ImVec2(iconSize, iconSize), { 0, 1 }, { 1, 0 }) && !isSimulateMode)
			{
				SetSceneState(isPlayMode ? SceneState::Edit : SceneState::Play);
			}
			if (isSimulateMode) ImGui::PopStyleVar();

			ImGui::SameLine(0, spacing);

			// ==== 2. Simulate / Stop 按钮 ====
			Ref<Texture2D> simulateIcon = isSimulateMode ? m_IconSimulateStop : m_IconSimulate;
			if (isPlayMode) ImGui::PushStyleVar(ImGuiStyleVar_Alpha, 0.5f);

			if (ImGui::ImageButton((ImTextureID)(uint64_t)simulateIcon->GetRendererID(), ImVec2(iconSize, iconSize), { 0, 1 }, { 1, 0 }) && !isPlayMode)
			{
				SetSceneState(isSimulateMode ? SceneState::Edit : SceneState::Simulate);
			}
			if (isPlayMode) ImGui::PopStyleVar();

			ImGui::SameLine(0, spacing);


			// ==== 3. Pause / Continue 按钮 ====
			Ref<Texture2D> pauseIcon;
			if (m_ScenePaused)
				pauseIcon = isSimulateMode ? m_IconSimulateContinue : m_IconContinue;
			else
				pauseIcon = isSimulateMode ? m_IconSimulatePause : m_IconPause;

			bool isPausedDisabled = (!isPlayMode && !isSimulateMode);

			if (isPausedDisabled)
			{
				ImGui::PushStyleVar(ImGuiStyleVar_Alpha, 0.5f); // 降低透明度变灰
			}

			if (ImGui::ImageButton((ImTextureID)(uint64_t)pauseIcon->GetRendererID(), ImVec2(iconSize, iconSize), { 0, 1 }, { 1, 0 }))
			{
				if (!isPausedDisabled)
				{
					// 布尔值反转替代 OnSceneContinue 和 OnScenePause
					m_ScenePaused = !m_ScenePaused;
				}
			}

			if (isPausedDisabled)
			{
				ImGui::PopStyleVar();
			}
		}
		ImGui::EndChild();
		ImGui::PopStyleVar();   // Pop ChildRounding
		ImGui::PopStyleColor(); // Pop ChildBg

		ImGui::PopStyleVar(4);
		ImGui::PopStyleColor(3);
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
		m_SceneHierarchyPanel.SetContext(m_ActiveScene);
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

	void EditorLayer::DrawUnsavedModal()
	{
		if (!m_ShowUnsavedModal)
			return;
		ImGui::OpenPopup("Unsaved Changes");
		const ImGuiWindowFlags flags = ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse;
		if (ImGui::BeginPopupModal("Unsaved Changes", nullptr, flags))
		{
			ImGui::TextWrapped("The current scene has unsaved changes.");
			bool takeAction = false;
			if (ImGui::Button("Save", ImVec2(120, 0)))
			{
				if (TrySave())
					takeAction = true;
				else if (!m_Document.GetLastError().empty())
					WLD_CORE_ERROR("{0}", m_Document.GetLastError());
			}
			ImGui::SameLine();
			if (ImGui::Button("Don't Save", ImVec2(120, 0)))
				takeAction = true;
			ImGui::SameLine();
			if (ImGui::Button("Cancel", ImVec2(120, 0)))
			{
				m_ShowUnsavedModal = false;
				m_PendingAction = nullptr;
				ImGui::CloseCurrentPopup();
			}
			if (takeAction)
			{
				std::function<void()> action = std::move(m_PendingAction);
				m_PendingAction = nullptr;
				m_ShowUnsavedModal = false;
				ImGui::CloseCurrentPopup();
				if (action)
					action();
			}
			ImGui::EndPopup();
		}
	}

	void EditorLayer::DrawErrorModal()
	{
		if (!m_ShowErrorModal)
			return;
		ImGui::OpenPopup("Error");
		const ImGuiWindowFlags flags = ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse;
		if (ImGui::BeginPopupModal("Error", nullptr, flags))
		{
			ImGui::TextWrapped("%s", m_ErrorText.c_str());
			if (ImGui::Button("OK", ImVec2(120, 0)))
			{
				m_ShowErrorModal = false;
				m_ErrorText.clear();
				ImGui::CloseCurrentPopup();
			}
			ImGui::EndPopup();
		}
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
					publishDir.replace_extension("");
					fs::create_directories(publishDir);
					fs::path srcRuntimeOutputDir = fs::absolute(std::string(WLD_OUTPUT_DIR) + "Runtime/" + WLD_BUILD_TYPE);
					fs::path srcRuntimeExe = srcRuntimeOutputDir / "Runtime.exe";
					if (!fs::is_regular_file(srcRuntimeExe))
						throw std::runtime_error("Runtime.exe could not be located: " + srcRuntimeExe.string());
					fs::copy_file(srcRuntimeExe, publishDir / "Runtime.exe", fs::copy_options::overwrite_existing);
					WLD_CORE_INFO("Copied Runtime executable from: {0}", srcRuntimeExe.string());

					fs::path srcGameOutputDir = fs::absolute(std::string(WLD_OUTPUT_DIR) + "bin/");
					for (const auto& entry : fs::recursive_directory_iterator(srcGameOutputDir))
					{
						if (entry.is_regular_file() && entry.path().extension() == ".dll")
						{
							fs::path destPath = publishDir / "bin" / fs::relative(entry.path(), srcGameOutputDir);
							fs::create_directories(destPath.parent_path());
							fs::copy_file(entry.path(), destPath, fs::copy_options::overwrite_existing);
						}
					}

					fs::path contentDir = publishDir / "content";
					fs::create_directories(contentDir);
					fs::path sourceAssetsDir = std::string(WLD_GAME_DIR) + "assets";
					fs::path outPakFile = contentDir / "Base.wpak";
					VFS::BuildPakFromDirectory(sourceAssetsDir, outPakFile);
					if (!fs::is_regular_file(outPakFile) || fs::file_size(outPakFile) == 0)
						throw std::runtime_error("Asset package was not created: " + outPakFile.string());
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

	void EditorLayer::OnCooking()
	{
		if (!ImGui::IsPopupOpen("Cooking Progress"))
		{
			ImGui::OpenPopup("Cooking Progress");
		}

		// 始终让弹窗居中
		ImGuiWindowFlags window_flags = ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse;
		if (ImGui::BeginPopupModal("Cooking Progress", NULL, window_flags))
		{
			if (m_CookingFinished.load(std::memory_order_acquire))
			{
				if (m_CookingThread.joinable())
					m_CookingThread.join();
				if (m_CookingSucceeded)
					ImGui::TextColored(ImVec4(0, 1, 0, 1), "Cooking Complete!");
				else
				{
					ImGui::TextColored(ImVec4(1, 0.3f, 0.3f, 1), "Cooking Failed");
					ImGui::TextWrapped("%s", m_CookingError.c_str());
				}
				if (ImGui::Button("Close", ImVec2(120, 0)))
				{
					m_ShowCookingProgress = false;
					ImGui::CloseCurrentPopup();
				}
			}
			else
			{
				ImGui::Text("Packing assets...");
				ImGui::ProgressBar(-1.0f * (float)ImGui::GetTime(), ImVec2(200.0f, 0.0f), "Cooking...");
			}

			ImGui::EndPopup();
		}
	}


}
