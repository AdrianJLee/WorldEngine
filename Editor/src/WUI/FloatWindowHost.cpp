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
	FloatWindowHost::FloatWindowHost(std::string panel, std::string title, const Wui::WuiRect& screenRect, ContentRenderer content)
		: m_Panel(std::move(panel)), m_Title(std::move(title)), m_Content(std::move(content))
	{
		WindowProps props(m_Title,
			static_cast<uint32_t>(std::max(240.0f, screenRect.W)),
			static_cast<uint32_t>(std::max(160.0f, screenRect.H)));
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
		if (Renderer::GetBackendName() == "vulkan")
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
		m_Content(m_Context, { 0, 0, size.x, size.y });
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
