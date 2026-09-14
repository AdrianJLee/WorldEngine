#pragma once

#include "World/Core/Window.h"
#include "World/Renderer/Renderer.h"
#include "World/WUI/WuiContext.h"
#include "World/WUI/WuiRhiBackend.h"

#include <functional>
#include <string>

namespace World
{
	// 独立浮动窗口组件(与停靠面板是两套不同组件):
	// 拥有自己的 OS 窗口、WUI 上下文与渲染后端;内容由编辑器面板注册表通过回调提供。
	// 停靠面板继续由 DockNode + EditorPanel 负责,二者的生命周期与渲染路径互不影响。
	class FloatWindowHost
	{
	public:
		using ContentRenderer = std::function<void(Wui::WuiContext&, const Wui::WuiRect&)>;

		FloatWindowHost(std::string panel, std::string title, const Wui::WuiRect& screenRect, ContentRenderer content);
		~FloatWindowHost();

		FloatWindowHost(const FloatWindowHost&) = delete;
		FloatWindowHost& operator=(const FloatWindowHost&) = delete;

		const std::string& Panel() const { return m_Panel; }
		// 渲染该独立窗口;返回 false 表示用户关闭了窗口(面板应隐藏)。
		bool Render();
		// 屏幕坐标(用于布局持久化)。
		Wui::WuiRect ScreenRect() const;
		void Focus();

	private:
		void OnEvent(Event& e);

		std::string m_Panel;
		std::string m_Title;
		ContentRenderer m_Content;
		Window* m_Window = nullptr;
		PresentTarget* m_Target = nullptr;
		Wui::WuiContext m_Context;
		Wui::WuiRhiBackend m_Backend;
	};
}
