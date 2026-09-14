#include "wldpch.h"
#include "FloatWindowHost.h"

#include "World/Core/Application.h"
#include "World/Events/ApplicationEvent.h"
#include "World/Events/KeyEvent.h"
#include "World/Events/MouseEvent.h"

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
		if (m_Target)
			Renderer::DestroyPresentTarget(m_Target);
		delete m_Window;
	}

	bool FloatWindowHost::Render()
	{
		if (!m_Window)
			return false;
		if (std::getenv("WLD_FLOAT_NO_RENDER"))
			return true; // 诊断:只创建窗口,不渲染内容
		if (m_Window->ShouldClose())
			return false;
		const glm::vec2 size { static_cast<float>(m_Window->GetWidth()), static_cast<float>(m_Window->GetHeight()) };
		if (size.x < 8.0f || size.y < 8.0f)
			return true; // 最小化/尚未布局

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

	// 标签栏(高度 24):点击切换活动面板,x 关闭只登记请求,由 EditorShell 统一处理
	// (窗口计数/布局记录属于外壳状态,容器不直接改动)。
	void FloatWindowHost::RenderTabBar(Wui::WuiContext& ctx, const Wui::WuiRect& area)
	{
		const Wui::WuiTheme& theme = m_Callbacks.Theme;
		constexpr float tabH = 24.0f;

		// ---- 标签栏(浏览器式):附加目标 + 切换/关闭标签 ----
		const float tabTop = area.Y;
		ctx.Commands().push_back({ Wui::WuiDrawKind::Rect, { area.X, tabTop, area.W, tabH }, theme.PanelHeader, 0.0f });
		// 放置目标高亮:其他窗口的标签正被拖到本窗口上方。
		if (m_TabDropHighlight)
			ctx.Commands().push_back({ Wui::WuiDrawKind::Rect, { area.X, tabTop, area.W, tabH },
				Wui::WuiColor { 0.3f, 0.5f, 0.9f, 0.55f }, 0.0f });

		std::string closeRequest;
		float lastTabEnd = area.X + 4.0f;
		if (!m_Panels.empty())
		{
			const float slot = std::max(1.0f, (area.W - 8.0f) / static_cast<float>(m_Panels.size()));
			const float width = std::min(150.0f, slot);
			float x = area.X + 4.0f;
			for (size_t i = 0; i < m_Panels.size(); ++i)
			{
				const std::string& panel = m_Panels[i];
				const Wui::WuiRect tab { x, tabTop + 2.0f, width, tabH - 2.0f };
				const Wui::WuiRect close { tab.X + tab.W - 18.0f, tab.Y + 4.0f, 14.0f, 14.0f };
				// 标签按下:记录来源,拖动超过阈值后发出拖拽请求。
				if (ctx.Input().MouseDown[0] && ctx.IsHovered(tab) && !ctx.IsHovered(close))
				{
					m_TabPressArmed = true;
					m_PressedTab = panel;
					m_TabPressPos = ctx.Input().MousePos;
				}
				if (i == m_Active)
					ctx.Commands().push_back({ Wui::WuiDrawKind::Rect, tab, theme.PanelBg, 2.0f });
				else if (ctx.IsHovered(tab))
					ctx.Commands().push_back({ Wui::WuiDrawKind::Rect, tab, theme.ButtonHover, 2.0f });
				if (ctx.IsHovered(tab))
					ctx.SetCursor(Wui::WuiCursor::Hand);
				if (ctx.IsClicked(tab))
					m_Active = i;

				ctx.Commands().push_back({ Wui::WuiDrawKind::Text, { tab.X + 6.0f, tab.Y + 3.0f, 0, 0 },
					theme.Text, 0, 1.0f, TitleOf(panel), 14.0f, false });

				if (ctx.IsHovered(close))
				{
					ctx.Commands().push_back({ Wui::WuiDrawKind::Rect, close, theme.ButtonHover, 2.0f });
					ctx.SetCursor(Wui::WuiCursor::Hand);
				}
				ctx.Commands().push_back({ Wui::WuiDrawKind::Text, { close.X + 3.0f, close.Y - 1.0f, 0, 0 },
					theme.TextMuted, 0, 1.0f, "x", 13.0f, false });
				if (ctx.IsClicked(close))
					closeRequest = panel;
				x += width;
			}
			lastTabEnd = x;
		}

		// 窗口级关闭按钮(标签栏最右侧);标签栏右侧空白 = 拖动移动窗口。
		const Wui::WuiRect windowClose { area.X + area.W - 20.0f, tabTop + 5.0f, 14.0f, 14.0f };
		if (ctx.IsHovered(windowClose))
		{
			ctx.Commands().push_back({ Wui::WuiDrawKind::Rect, windowClose, theme.ButtonHover, 2.0f });
			ctx.SetCursor(Wui::WuiCursor::Hand);
		}
		ctx.Commands().push_back({ Wui::WuiDrawKind::Text, { windowClose.X + 3.0f, windowClose.Y - 1.0f, 0, 0 },
			theme.TextMuted, 0, 1.0f, "x", 13.0f, false });
		if (ctx.IsClicked(windowClose))
			m_Window->SetShouldClose(true);
		else if (ctx.Input().MouseDown[0] && ctx.IsHovered({ lastTabEnd, tabTop,
			std::max(0.0f, area.W - lastTabEnd - 24.0f), tabH }))
			m_Window->BeginSystemDrag();

		// 拖拽阈值:按下后移动超过 4px 即发起一次跨窗口拖拽请求。
		if (m_TabPressArmed)
		{
			if (!ctx.Input().MouseDown[0])
			{
				m_TabPressArmed = false;
				m_PressedTab.clear();
			}
			else if (glm::length(ctx.Input().MousePos - m_TabPressPos) > 4.0f)
			{
				m_TabPressArmed = false;
				m_PendingTabDrag = m_PressedTab;
				if (m_Callbacks.TabDragStart)
					m_Callbacks.TabDragStart(m_PressedTab);
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
			{ m_Backend.LocalMouseButton(ev.GetMouseButton(), true); return false; });
		dispatcher.Dispatch<MouseButtonReleasedEvent>([&](MouseButtonReleasedEvent& ev)
			{ m_Backend.LocalMouseButton(ev.GetMouseButton(), false); return false; });
		dispatcher.Dispatch<MouseMovedEvent>([&](MouseMovedEvent& ev)
			{ m_Backend.LocalMouseMove(ev.GetX(), ev.GetY()); return false; });
		dispatcher.Dispatch<MouseScrolledEvent>([&](MouseScrolledEvent& ev)
			{ m_Backend.LocalMouseScroll(ev.GetXOffset(), ev.GetYOffset()); return false; });
	}
}
