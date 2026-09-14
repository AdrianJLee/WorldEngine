#include "wldpch.h"
#include "FloatWindowHost.h"

#include "World/Core/Application.h"
#include "World/Events/ApplicationEvent.h"
#include "World/Events/KeyEvent.h"
#include "World/Events/MouseEvent.h"
#include "World/WUI/Widgets/WuiChrome.h"

#include <algorithm>
#include <cstdlib>

namespace World
{
	FloatWindowHost::FloatWindowHost(std::string panel, std::string title, const Wui::WuiRect& screenRect, Callbacks callbacks)
		: m_Panel(std::move(panel)), m_Title(std::move(title)), m_Callbacks(std::move(callbacks))
	{
		m_Panels.push_back(m_Panel);
		m_Active = 0;
		WindowProps props(m_Title,
			static_cast<uint32_t>(std::max(240.0f, screenRect.W)),
			static_cast<uint32_t>(std::max(160.0f, screenRect.H)),
			true /* 无边框:标题栏由 WUI 绘制,顶部即标签/附加区域 */);
		Window* main = Application::HasInstance() ? &Application::Get().GetWindow() : nullptr;
		m_Window = Window::CreateAuxiliary(props, main);
		if (!m_Window)
			return;
		m_Window->SetPosition(static_cast<int>(screenRect.X), static_cast<int>(screenRect.Y));
		m_Window->SetEventCallback(WLD_BIND_EVENT_FN(FloatWindowHost::OnEvent));
		WLD_CORE_INFO("[float] independent window created: {0} ({1}x{2})", m_Panel,
			m_Window->GetWidth(), m_Window->GetHeight());
		// 创建附加窗口会切换当前 GL 上下文,恢复主窗口的上下文。
		if (main)
			main->MakeCurrent();

		m_Backend.UseLocalInput({ static_cast<float>(m_Window->GetWidth()), static_cast<float>(m_Window->GetHeight()) });
		m_Backend.SetCursorWindow(m_Window->GetNativeWindow());
		const bool skipTarget = std::getenv("WLD_FLOAT_NO_TARGET") != nullptr;
		if (!skipTarget && Renderer::GetBackendName() == "vulkan")
		{
			PresentTargetDesc desc;
			desc.NativeWindow = m_Window->GetNativeWindow();
			desc.Width = m_Window->GetWidth();
			desc.Height = m_Window->GetHeight();
			desc.DebugName = "Float:" + m_Panel;
			m_Target = Renderer::CreatePresentTarget(desc);
		}
	}

	FloatWindowHost::~FloatWindowHost()
	{
		// 先断开事件回调:GLFW 在销毁窗口时会派发消息,若回调仍指向正在析构的
		// 本对象(或其成员),会造成访问违例。
		if (m_Window)
			m_Window->SetEventCallback(Window::EventCallbackFn {});
		// 诊断开关:WLD_FLOAT_LEAK_TARGET 跳过呈现目标销毁,用于区分
		// "交换链销毁"与"窗口销毁"两类崩溃来源。
		if (m_Target && !std::getenv("WLD_FLOAT_LEAK_TARGET"))
			Renderer::DestroyPresentTarget(m_Target);
		delete m_Window;
	}

	bool FloatWindowHost::Render()
	{
		if (!m_Window)
			return false;
		if (m_Hidden)
			return true; // 已隐藏(复用中):不渲染、不处理输入
		if (std::getenv("WLD_FLOAT_NO_RENDER"))
			return true; // 诊断:只创建窗口,不渲染内容
		if (m_Window->ShouldClose())
			return false;
		const glm::vec2 size { static_cast<float>(m_Window->GetWidth()), static_cast<float>(m_Window->GetHeight()) };
		if (size.x < 8.0f || size.y < 8.0f)
			return true; // 最小化/尚未布局
		// 若系统左键已释放(可能释放在别的窗口),清除"本窗口按下"标志。
		if (!(GetAsyncKeyState(VK_LBUTTON) & 0x8000))
			m_PressSeenInWindow = false;

		m_Backend.SetViewportSize(size);
		Wui::WuiInputState input;
		if (!m_Backend.BeginFrame(input))
			return true;
		m_Context.BeginFrame(input);
		RenderTabBar(m_Context, { 0.0f, 0.0f, size.x, size.y });
		m_Context.EndFrame();

		if (m_Target)
			Renderer::ResizePresentTarget(m_Target, static_cast<uint32_t>(size.x), static_cast<uint32_t>(size.y));
		m_Window->MakeCurrent();
		if (Renderer::BeginFramePresent(m_Target))
		{
			m_Backend.Render(m_Context.Commands(), m_Context.OverlayCommands());
			Renderer::EndFramePresent(m_Target);
		}
		m_Backend.EndFrame(m_Context.Cursor());
		// 开发验证:读回独立窗口的默认帧缓冲(WLD_CAPTURE_FLOAT=<路径前缀>)。
		if (const char* capturePrefix = std::getenv("WLD_CAPTURE_FLOAT"))
		{
			static int frameCount = 0;
			if (++frameCount == 120)
				Renderer::CaptureDefaultFramebuffer(std::string(capturePrefix) + m_Panel + ".ppm",
					static_cast<uint32_t>(size.x), static_cast<uint32_t>(size.y));
		}
		m_Window->SwapBuffers();
		return true;
	}

	Wui::WuiRect FloatWindowHost::ScreenRect() const
	{
		Wui::WuiRect rect { 0, 0, 480, 320 };
		if (!m_Window)
			return rect;
		int x = 0, y = 0;
		m_Window->GetPosition(&x, &y);
		rect.X = static_cast<float>(x);
		rect.Y = static_cast<float>(y);
		rect.W = static_cast<float>(m_Window->GetWidth());
		rect.H = static_cast<float>(m_Window->GetHeight());
		return rect;
	}

	void FloatWindowHost::SetScreenPosition(float x, float y)
	{
		if (m_Window)
			m_Window->SetPosition(static_cast<int>(x), static_cast<int>(y));
	}

	// 渲染后端切换:同一个 HWND 上 GL 上下文与 Vulkan 表面无法可靠共存,
	// 独立窗口也必须按新后端重建(位置/尺寸/可见性保留)。
	void FloatWindowHost::RecreateWindow()
	{
		if (!m_Window)
			return;

		int x = 0, y = 0;
		m_Window->GetPosition(&x, &y);
		const uint32_t width = std::max(240u, m_Window->GetWidth());
		const uint32_t height = std::max(160u, m_Window->GetHeight());
		const bool visible = !m_Hidden;

		if (m_Target)
		{
			Renderer::DestroyPresentTarget(m_Target);
			m_Target = nullptr;
		}
		m_Window->SetEventCallback(Window::EventCallbackFn {});
		delete m_Window;
		m_Window = nullptr;

		WindowProps props(m_Title, width, height, true);
		Window* main = Application::HasInstance() ? &Application::Get().GetWindow() : nullptr;
		m_Window = Window::CreateAuxiliary(props, main);
		if (!m_Window)
			return;
		m_Window->SetPosition(x, y);
		m_Window->SetEventCallback(WLD_BIND_EVENT_FN(FloatWindowHost::OnEvent));
		if (!visible)
			m_Window->SetVisible(false);
		if (main)
			main->MakeCurrent();

		m_Backend.SetCursorWindow(m_Window->GetNativeWindow());
		m_Backend.SetViewportSize({ static_cast<float>(m_Window->GetWidth()),
			static_cast<float>(m_Window->GetHeight()) });
		if (Renderer::GetBackendName() == "vulkan")
		{
			PresentTargetDesc desc;
			desc.NativeWindow = m_Window->GetNativeWindow();
			desc.Width = m_Window->GetWidth();
			desc.Height = m_Window->GetHeight();
			desc.DebugName = "Float:" + m_Panel;
			m_Target = Renderer::CreatePresentTarget(desc);
		}
		WLD_CORE_INFO("[float] independent window recreated for backend '{0}': {1}",
			Renderer::GetBackendName(), m_Panel);
	}

	void FloatWindowHost::SetHidden(bool hidden)
	{
		m_Hidden = hidden;
		if (!m_Window)
			return;
		m_PressSeenInWindow = false;
		m_TabPressArmed = false;
		m_PressedTab.clear();
		if (hidden)
		{
			m_Window->SetVisible(false);
		}
		else
		{
			m_Window->SetShouldClose(false);
			m_Window->SetVisible(true);
		}
	}

	void FloatWindowHost::Focus()
	{
		if (m_Window)
			m_Window->Focus();
	}

	// ---- 窗口内的面板集合(标签栏模型) ----

	const std::string& FloatWindowHost::ActivePanel() const
	{
		static const std::string empty;
		return m_Active < m_Panels.size() ? m_Panels[m_Active] : empty;
	}

	bool FloatWindowHost::Contains(const std::string& panel) const
	{
		return std::find(m_Panels.begin(), m_Panels.end(), panel) != m_Panels.end();
	}

	bool FloatWindowHost::AddPanel(const std::string& panel, bool activate)
	{
		if (panel.empty() || Contains(panel))
			return false;
		m_Panels.push_back(panel);
		if (activate)
			m_Active = m_Panels.size() - 1;
		WLD_CORE_INFO("[float] panel '{0}' added to independent window '{1}' ({2} tabs)",
			panel, m_Panel, m_Panels.size());
		return true;
	}

	bool FloatWindowHost::RemovePanel(const std::string& panel)
	{
		auto it = std::find(m_Panels.begin(), m_Panels.end(), panel);
		if (it == m_Panels.end())
			return false;
		const size_t index = static_cast<size_t>(it - m_Panels.begin());
		m_Panels.erase(it);
		// 活动索引:删除位置之前的面板左移,删除的若是活动标签则顺延到后一个;
		// 删掉最后一个时回退到末尾,窗口为空时归零。
		if (m_Panels.empty())
			m_Active = 0;
		else if (index < m_Active)
			--m_Active;
		else if (m_Active >= m_Panels.size())
			m_Active = m_Panels.size() - 1;
		WLD_CORE_INFO("[float] panel '{0}' removed from independent window '{1}' ({2} tabs left)",
			panel, m_Panel, m_Panels.size());
		return true;
	}

	bool FloatWindowHost::MovePanelTo(const std::string& panel, size_t index)
	{
		auto it = std::find(m_Panels.begin(), m_Panels.end(), panel);
		if (it == m_Panels.end() || m_Panels.empty())
			return false;
		const bool wasActive = ActivePanel() == panel;
		const size_t from = static_cast<size_t>(it - m_Panels.begin());
		const size_t target = std::min(index, m_Panels.size() - 1);
		if (from == target)
			return true;
		m_Panels.erase(it);
		m_Panels.insert(m_Panels.begin() + static_cast<std::ptrdiff_t>(target), panel);
		if (wasActive)
			m_Active = target;
		else if (from < m_Active && target >= m_Active)
			--m_Active;
		else if (from > m_Active && target <= m_Active)
			++m_Active;
		return true;
	}

	bool FloatWindowHost::ActivatePanel(const std::string& panel)
	{
		auto it = std::find(m_Panels.begin(), m_Panels.end(), panel);
		if (it == m_Panels.end())
			return false;
		m_Active = static_cast<size_t>(it - m_Panels.begin());
		return true;
	}

	std::string FloatWindowHost::TakeCloseRequest()
	{
		std::string request = std::move(m_CloseRequest);
		m_CloseRequest.clear();
		return request;
	}

	std::string FloatWindowHost::TakePendingTabDrag()
	{
		std::string request = std::move(m_PendingTabDrag);
		m_PendingTabDrag.clear();
		return request;
	}

	std::string FloatWindowHost::TitleOf(const std::string& panel) const
	{
		return m_Callbacks.Title ? m_Callbacks.Title(panel) : panel;
	}

	// 每个独立窗口自己的菜单栏(左下角 ☰):关闭本窗口 / 挂靠回主窗口。
	void FloatWindowHost::RenderWindowMenu(Wui::WuiContext& ctx, const Wui::WuiRect& bar)
	{
		const Wui::WuiTheme& theme = m_Callbacks.Theme;
		const Wui::WuiId menuId = Wui::HashId("float.window.menu");
		const std::string panel = ActivePanel();
		Wui::Label(ctx, { bar.X + 6.0f, bar.Y + 4.0f }, "M",
			m_MenuOpen || ctx.IsHovered(bar) ? theme.Text : theme.TextMuted, 13.0f);
		if (ctx.IsClicked(bar))
		{
			m_MenuOpen = !m_MenuOpen;
			if (m_MenuOpen)
				ctx.OpenPopup(menuId);
			else
				ctx.ClosePopup(menuId);
		}
		if (!m_MenuOpen)
			return;

		const Wui::WuiRect panelRect { bar.X, bar.Y + bar.H + 2.0f, 160.0f, 2 * 22.0f + 8.0f };
		ctx.PushOverlay();
		Wui::DrawPanelSurface(ctx, panelRect, theme);
		if (Wui::MenuItem(ctx, Wui::HashId("float.window.menu.dock"),
			{ panelRect.X + 4.0f, panelRect.Y + 4.0f, panelRect.W - 8.0f, 22.0f }, "Dock to Main", true, theme))
		{
			m_MenuOpen = false;
			ctx.CloseAllPopups();
			if (m_Callbacks.DockToMain)
				m_Callbacks.DockToMain(panel);
		}
		if (Wui::MenuItem(ctx, Wui::HashId("float.window.menu.close"),
			{ panelRect.X + 4.0f, panelRect.Y + 26.0f, panelRect.W - 8.0f, 22.0f }, "Close Window", true, theme))
		{
			m_MenuOpen = false;
			ctx.CloseAllPopups();
			if (m_Callbacks.CloseWindow)
				m_Callbacks.CloseWindow(panel);
		}
		ctx.ClosePopupsOnOutsideClick({ menuId }, panelRect);
		ctx.PopOverlay();
	}

	// 标签栏(高度 24):点击切换活动面板,x 关闭只登记请求,由 EditorShell 统一处理
	// (窗口计数/布局记录属于外壳状态,容器不直接改动)。
	void FloatWindowHost::RenderTabBar(Wui::WuiContext& ctx, const Wui::WuiRect& area)
	{
		const Wui::WuiTheme& theme = m_Callbacks.Theme;
		constexpr float tabH = 24.0f;

		// ---- 标签栏(浏览器式):附加目标 + 切换/关闭标签 ----
		// 菜单栏为空时不占位:标签栏直接在最顶部(窗口顶栏 = 标签栏)。
		const float tabTop = area.Y;
		Wui::PanelBackground(ctx, { area.X, tabTop, area.W, tabH }, theme.PanelHeader);
		// 放置目标高亮:其他窗口的标签正被拖到本窗口上方。
		if (m_TabDropHighlight)
			Wui::PanelBackground(ctx, { area.X, tabTop, area.W, tabH }, { 0.3f, 0.5f, 0.9f, 0.55f });

		std::string closeRequest;
		float lastTabEnd = area.X + 4.0f;
		if (!m_Panels.empty())
		{
			// 与主窗口停靠标签栏共用同一组件(WuiChrome::DockTabBar)。
			std::vector<Wui::DockTab> tabs;
			tabs.reserve(m_Panels.size());
			for (size_t i = 0; i < m_Panels.size(); ++i)
				tabs.push_back({ Wui::HashId(("float.tab." + m_Panels[i]).c_str()), TitleOf(m_Panels[i]), i == m_Active });
			const Wui::DockTabBarResult bar = Wui::DockTabBar(ctx,
				{ area.X, tabTop, std::max(0.0f, area.W - 102.0f), tabH }, tabs, theme);
			if (bar.Clicked >= 0)
				m_Active = static_cast<size_t>(bar.Clicked);
			if (bar.Closed >= 0)
				closeRequest = m_Panels[static_cast<size_t>(bar.Closed)];
			// 按下标签(非关闭键):记录起点,移动超阈值后发起跨窗口拖拽。
			if (!m_TabDragActive && m_PressSeenInWindow && bar.DragStart >= 0
				&& ctx.Input().MouseClicked[0])
			{
				m_TabPressArmed = true;
				m_PressedTab = m_Panels[static_cast<size_t>(bar.DragStart)];
				m_TabPressPos = ctx.Input().MousePos;
			}
			const float width = std::min(150.0f,
				std::max(1.0f, (area.W - 8.0f) / static_cast<float>(m_Panels.size())));
			lastTabEnd = area.X + 4.0f + width * static_cast<float>(m_Panels.size());
		}

		// 标准窗口控制(最小化/最大化/关闭)位于标签栏最右侧;其余空白拖动移动窗口。
		const Wui::WindowControl control = Wui::WindowControls(ctx,
			{ area.X + area.W - 102.0f, tabTop, 102.0f, tabH }, theme, m_Window->IsMaximized());
		if (control == Wui::WindowControl::Minimize)
			m_Window->Minimize();
		else if (control == Wui::WindowControl::Maximize)
			m_Window->MaximizeOrRestore();
		else if (control == Wui::WindowControl::Close)
			m_Window->SetShouldClose(true);
		else if (!m_TabDragActive && ctx.Input().MouseClicked[0] && m_PressSeenInWindow
			&& ctx.IsHovered({ lastTabEnd, tabTop,
				std::max(0.0f, area.W - lastTabEnd - 110.0f), tabH }))
		{
			// 空白区与标签统一:走同一条"按住即跟随 + 实时高亮目标"的自定义拖拽。
			m_PressedTab = m_Panels.empty() ? std::string() : m_Panels.front();
			m_TabPressArmed = true;
			m_TabPressPos = ctx.Input().MousePos;
		}

		// 拖拽阈值:按下后移动超过 4px 即发起一次跨窗口拖拽请求。
		if (m_TabPressArmed)
		{
			if (!ctx.Input().MouseDown[0])
			{
				m_TabPressArmed = false;
				m_PressedTab.clear();
			}
			else if (glm::length(ctx.Input().MousePos - m_TabPressPos) > 2.0f)
			{
				m_TabPressArmed = false;
				// 自定义拖拽:窗口每帧跟随光标(不阻塞主循环),因此拖动期间
				// 主窗口能实时高亮挂靠栏;松手时按落点挂靠/附加/留在原地。
				m_PendingTabDrag = m_PressedTab;
				if (m_Callbacks.TabDragStart)
					m_Callbacks.TabDragStart(m_PressedTab);
				m_PressedTab.clear();
			}
		}

		const Wui::WuiRect content { area.X, tabTop + tabH, area.W, std::max(0.0f, area.H - tabH) };
		if (!m_Panels.empty() && m_Callbacks.Content)
			m_Callbacks.Content(ctx, content, ActivePanel());
		if (!closeRequest.empty())
			m_CloseRequest = closeRequest;
	}

	void FloatWindowHost::OnEvent(Event& e)
	{
		EventDispatcher dispatcher(e);
		dispatcher.Dispatch<KeyPressedEvent>([&](KeyPressedEvent& ev)
			{ m_Backend.LocalKey(static_cast<uint32_t>(ev.GetKeyCode()), true, ev.GetRepeatCount() > 0); return false; });
		dispatcher.Dispatch<KeyReleasedEvent>([&](KeyReleasedEvent& ev)
			{ m_Backend.LocalKey(static_cast<uint32_t>(ev.GetKeyCode()), false, false); return false; });
		dispatcher.Dispatch<KeyTypedEvent>([&](KeyTypedEvent& ev)
			{ m_Backend.LocalChar(static_cast<uint32_t>(ev.GetKeyCode())); return false; });
		dispatcher.Dispatch<MouseButtonPressedEvent>([&](MouseButtonPressedEvent& ev)
			{ m_PressSeenInWindow = true; m_Backend.LocalMouseButton(ev.GetMouseButton(), true); return false; });
		dispatcher.Dispatch<MouseButtonReleasedEvent>([&](MouseButtonReleasedEvent& ev)
			{ m_Backend.LocalMouseButton(ev.GetMouseButton(), false); m_PressSeenInWindow = false; return false; });
		dispatcher.Dispatch<MouseMovedEvent>([&](MouseMovedEvent& ev)
			{ m_Backend.LocalMouseMove(ev.GetX(), ev.GetY()); return false; });
		dispatcher.Dispatch<MouseScrolledEvent>([&](MouseScrolledEvent& ev)
			{ m_Backend.LocalMouseScroll(ev.GetXOffset(), ev.GetYOffset()); return false; });
	}
}
