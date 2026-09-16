#pragma once

#include "World/Core/Window.h"
#include "World/Renderer/Renderer.h"
#include "World/WUI/WuiContext.h"
#include "World/WUI/WuiRhiBackend.h"
#include "World/WUI/WuiWidgets.h"

#include <functional>
#include <string>
#include <vector>

namespace World
{
	// 独立浮动窗口容器(与主窗口的停靠面板是两套容器组件):
	// 拥有自己的 OS 窗口、WUI 上下文与渲染后端;一个窗口可承载多个面板(标签栏),
	// 标签标题与面板内容由编辑器面板注册表通过回调注入(DockNode/EditorPanel 仍只负责主窗口停靠)。
	class FloatWindowHost
	{
	public:
		// 容器渲染回调:标题查询 + 活动面板内容渲染(不依赖 EditorShell 具体类型)。
		struct Callbacks
		{
			Wui::WuiTheme Theme;
			std::function<std::string(const std::string&)> Title;
			std::function<void(Wui::WuiContext&, const Wui::WuiRect&, const std::string&)> Content;
			// 标签按下并拖动超过阈值时触发;EditorShell 据此启动跨窗口附加拖拽。
			std::function<void(const std::string& panel)> TabDragStart;
			// 窗口自带菜单的动作(每个独立窗口有自己的菜单栏)。
			std::function<void(const std::string& panel)> DockToMain;
			std::function<void(const std::string& panel)> CloseWindow;
			// 该面板是否允许挂靠到主窗口顶部挂靠栏(只有声明为独立窗口的面板允许)。
			std::function<bool(const std::string& panel)> CanAttach;
		};

		FloatWindowHost(std::string panel, std::string title, const Wui::WuiRect& screenRect, Callbacks callbacks);
		~FloatWindowHost();

		FloatWindowHost(const FloatWindowHost&) = delete;
		FloatWindowHost& operator=(const FloatWindowHost&) = delete;

		// 当前活动面板(等价于 ActivePanel,保留旧调用点语义)。
		const std::string& Panel() const { return ActivePanel(); }
		const std::string& ActivePanel() const;
		// 窗口承载的全部面板(即标签顺序);W7.3 跨窗口附加按此迁移。
		const std::vector<std::string>& Panels() const { return m_Panels; }
		bool Contains(const std::string& panel) const;
		bool Empty() const { return m_Panels.empty(); }
		// 面板加入/移出本窗口:只维护标签集合与活动索引,
		// 布局记录(floating Rect)由调用方(EditorShell)按窗口屏幕矩形写回。
		bool AddPanel(const std::string& panel, bool activate = true);
		bool RemovePanel(const std::string& panel);
		// 把面板移到标签序的指定位置(拖拽重排)。
		bool MovePanelTo(const std::string& panel, size_t index);
		bool ActivatePanel(const std::string& panel);
		// 标签栏 x 的关闭请求:Render 后由 EditorShell 取走并处理(隐藏该面板)。
		std::string TakeCloseRequest();
		// 标签拖拽请求(一次性):Render 后由 EditorShell 取走并进入跨窗口拖拽。
		std::string TakePendingTabDrag();
		// 其他窗口正在被拖拽悬停:高亮本窗口的标签栏(放置目标指示)。
		void SetTabDropHighlight(bool highlighted) { m_TabDropHighlight = highlighted; }
		// 跨窗口拖拽期间置位:抑制本窗口的标签按下与空区系统拖动,避免叠加。
		void SetTabDragActive(bool active) { m_TabDragActive = active; }
		// 隐藏/复用:关闭或挂靠时隐藏窗口而不是销毁(运行期销毁在 Vulkan 下会崩)。
		bool IsHidden() const { return m_Hidden; }
		void SetHidden(bool hidden);
		// 渲染后端切换:重建 OS 窗口与呈现目标(GL 上下文/ Vulkan 表面与窗口绑定)。
		void RecreateWindow();

		// 渲染该独立窗口;返回 false 表示 OS 窗口已关闭(其全部面板应隐藏)。
		bool Render();
		// 屏幕坐标(用于布局持久化)。
		Wui::WuiRect ScreenRect() const;
		// 拖动标签时让窗口跟随光标(跨窗口拖拽期间由外壳驱动)。
		void SetScreenPosition(float x, float y);
		void Focus();

	private:
		void OnEvent(Event& e);
		void RenderTabBar(Wui::WuiContext& ctx, const Wui::WuiRect& area);
		void RenderWindowMenu(Wui::WuiContext& ctx, const Wui::WuiRect& bar);
		// 诊断(无障碍):按 WLD_SCREEN_CAPTURE_* 把本窗口的合成画面连续写成
		// <dir>/float-<面板>-<n>.ppm,与主窗口的 screen-<n>.ppm 配套 ——
		// "独立窗口里在闪什么"必须能像主窗口一样被逐帧比较。
		void CaptureScreenSequence(float width, float height);
		int m_CaptureFrame = 0;
		int m_CaptureWritten = 0;
		bool m_MenuOpen = false;
		std::string TitleOf(const std::string& panel) const;

		std::string m_Panel;   // 窗口创建时的首个面板(日志/截图命名用,窗口身份 = 面板集合)
		std::string m_Title;
		std::vector<std::string> m_Panels;
		size_t m_Active = 0;
		std::string m_CloseRequest;
		std::string m_PendingTabDrag;
		std::string m_PressedTab;
		bool m_TabPressArmed = false;
		glm::vec2 m_TabPressPos { 0, 0 };
		bool m_TabDropHighlight = false;
		// 左键按下必须发生在本窗口内,才允许解读为拖拽:
		// 避免主窗口拖动时,经过本窗口的"悬空按键"被误当成新的拖拽。
		bool m_PressSeenInWindow = false;
		bool m_TabDragActive = false;
		bool m_Hidden = false;
		Callbacks m_Callbacks;
		Window* m_Window = nullptr;
		PresentTarget* m_Target = nullptr;
		Wui::WuiContext m_Context;
		Wui::WuiRhiBackend m_Backend;
	};
}
