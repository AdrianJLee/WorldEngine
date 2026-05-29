#include "EditorLayer.h"
#include "World/Core/Thread/JobSystem.h"
#include "World/Core/Cook/VFS.h"
namespace World
{
	EditorLayer::EditorLayer()
		:Layer("EditorLayer")
	{
		m_ContentBrowserPanel.RegisterOpenAction(".wd", [this](const std::filesystem::path& filepath)
			{
				NewScene();
				// 调用 EditorLayer 的加载场景函数
				OpenScene(filepath);
			});
	}
	void EditorLayer::OnAttach()
	{
		WLD_PROFILE_FUNCTION();
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
	}

	void EditorLayer::OnUpdate(Timestep ts)
	{
		WLD_PROFILE_FUNCTION();
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
		glm::mat4* renderCameraTransform = WLD_FRAME_NEW(glm::mat4, m_EditorCamera.GetTransform());


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
						renderCamera = &m_ActiveScene->GetPrimaryCameraEntity().GetComponent<CameraComponent>().Camera;
						renderCameraTransform = &m_ActiveScene->GetPrimaryCameraEntity().GetComponent<TransformComponent>().Transform;
					}
					else
						m_ActiveScene->OnUpdateEditor(ts, m_EditorCamera);
					break;
				case SceneState::Simulate:
					if (!m_ScenePaused)
						m_ActiveScene->OnUpdateSimulation(ts, m_EditorCamera);
					else
						m_ActiveScene->OnUpdateEditor(ts, m_EditorCamera);
					break;
			}

		}

		m_SceneRenderer->BeginScene(m_ActiveScene.get(), m_RendererOptions);
		m_SceneRenderer->SubmitScene(*renderCamera, *renderCameraTransform, m_SceneHierarchyPanel.GetSelectedEntity()); // 内部遍历实体并调用 Renderer2D
		m_SceneRenderer->EndScene();
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
					NewScene();
					OpenScene();
				}
				if (ImGui::MenuItem("Cooking"))
				{
					std::string outPath = World::FileDialogs::SaveFile("Pak (*.pak)\0*.pak\0");
					if (!outPath.empty())
					{
						// 记录状态，准备显示弹窗
						m_ShowCookingProgress = true;
						m_CookingFinished = false;

						// 告诉 ImGui 下一帧打开模态弹窗
						ImGui::OpenPopup("Cooking Progress");

						std::string sourceDir = "assets"; // 你的源资源目录

						// 开启独立线程进行打包操作，防止编辑器卡死
						std::thread([sourceDir, outPath, this]()
							{
								VFS::BuildPakFromDirectory(sourceDir, outPath);

								// 任务完成后标记状态
								this->m_CookingFinished = true;
							}).detach();
					}
				}

				if (ImGui::MenuItem("Exit"))
					World::Application::Get().Close();

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
			ImGui::Image((void*)m_SceneRenderer->GetTargetFramebuffer()->GetColorAttachmentRendererID(),
				ImVec2 { m_ViewportSize.x,m_ViewportSize.y },
				ImVec2 { 0,1 },
				ImVec2 { 1,0 });

			// 工具栏
			UI_Toolbar();

			if (auto selectedEntity = m_SceneHierarchyPanel.GetSelectedEntity())
			{
				// Draw Gizmo
				ImGuiDrawLibrary::DrawGizmo(m_EditorCamera, selectedEntity, m_CurrentGizmoOperation);

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
	}

	void EditorLayer::OnEvent(Event& event)
	{
		WLD_PROFILE_FUNCTION();

		m_EditorCamera.OnEvent(event);

		EventDispatcher dispatcher(event);

		dispatcher.Dispatch<KeyPressedEvent>(WLD_BIND_EVENT_FN(EditorLayer::OnKeyPressed));
		dispatcher.Dispatch<MouseButtonPressedEvent>(WLD_BIND_EVENT_FN(EditorLayer::OnMouseButtonPressed));
	}
	void EditorLayer::NewScene()
	{
		SetSceneState(SceneState::Edit);

		m_EditorScene = CreateRef<Scene>();
		UpdateSceneContext(m_EditorScene);
		m_ScenePath = std::filesystem::path();
	}
	void EditorLayer::OpenScene()
	{
		std::string Path = FileDialogs::OpenFile("Scene File (*.wd)\0*.wd\0");

		OpenScene(Path);

	}
	void EditorLayer::OpenScene(const std::filesystem::path& path)
	{
		SetSceneState(SceneState::Edit);

		m_ScenePath = path;
		Ref<Scene> newScene = CreateRef<Scene>();
		if (!path.empty())
		{
			SceneSerializer serializer(newScene);
			serializer.Deserialize(path.string());
		}
		m_EditorScene = newScene;
		UpdateSceneContext(m_EditorScene);
	}
	void EditorLayer::SaveScene()
	{
		if (m_ScenePath.empty())
		{
			std::string Path = FileDialogs::SaveFile("Scene File (*.wd)\0*.wd\0");
			if (!Path.empty())
			{
				SceneSerializer serializer(m_EditorScene);
				serializer.Serialize(Path);
			}
		}
		else
		{
			SceneSerializer serializer(m_EditorScene);
			serializer.Serialize(m_ScenePath.string());
		}
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
					if (selectedEntity)
					{
						m_ActiveScene->DuplicateEntity(selectedEntity);
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

		return result;
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

		if (state == SceneState::Play)
		{
			m_RuntimeScene = CreateRef<Scene>();
			Scene::CopyScene(m_EditorScene, m_RuntimeScene);

			m_SceneState = SceneState::Play;
			UpdateSceneContext(m_RuntimeScene);
			m_ActiveScene->OnRuntimeStart();
		}
		else if (state == SceneState::Simulate)
		{
			m_RuntimeScene = CreateRef<Scene>();
			Scene::CopyScene(m_EditorScene, m_RuntimeScene);

			m_SceneState = SceneState::Simulate;
			UpdateSceneContext(m_RuntimeScene);
			m_ActiveScene->OnSimulationStart();
		}
		else if (state == SceneState::Edit)
		{
			if (m_SceneState == SceneState::Play)
				m_ActiveScene->OnRuntimeStop();
			else if (m_SceneState == SceneState::Simulate)
				m_ActiveScene->OnSimulationStop();

			m_SceneState = SceneState::Edit;
			UpdateSceneContext(m_EditorScene);
			m_RuntimeScene = nullptr;
		}

		m_ScenePaused = false; // 切换状态时重置暂停状态
	}
	void EditorLayer::UpdateSceneContext(Ref<Scene> scene)
	{
		m_ActiveScene = scene;
		if (m_ViewportSize.x > 0.0f && m_ViewportSize.y > 0.0f)
		{
			m_ActiveScene->OnViewportResize((uint32_t)m_ViewportSize.x, (uint32_t)m_ViewportSize.y);
		}
		m_SceneHierarchyPanel.SetContext(m_ActiveScene);
	}

	void EditorLayer::OnCooking()
	{

		if (!ImGui::IsPopupOpen("Cooking Progress"))
		{
			ImGui::OpenPopup("Cooking Progress");
		}

		// 始终让弹窗居中
		ImGuiWindowFlags window_flags = ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse;
		bool p_open = true; // 控制右上角是否有X，如果不需要能关掉，填NULL
		if (ImGui::BeginPopupModal("Cooking Progress", NULL, window_flags))
		{
			ImGui::Text("Packing assets...");

			// 动态显示一个来回滚动的无极进度条以表示程序没死机
			// （如果有真实的进度数值，可以将下面这行替换为: ImGui::ProgressBar(m_progressPercent);）
			ImGui::ProgressBar(-1.0f * (float)ImGui::GetTime(), ImVec2(200.0f, 0.0f), "Cooking...");

			if (m_CookingFinished)
			{
				// 打包结束后，延迟关闭或提示完成
				ImGui::TextColored(ImVec4(0, 1, 0, 1), "Cooking Complete!");
				if (ImGui::Button("Close", ImVec2(120, 0)))
				{
					m_ShowCookingProgress = false;
					ImGui::CloseCurrentPopup();
				}
			}

			ImGui::EndPopup();
		}
	}


}