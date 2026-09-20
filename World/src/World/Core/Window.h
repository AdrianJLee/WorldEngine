#pragma once
#include "wldpch.h"

#include "World/Core/Core.h"
#include "World/Events/Event.h"

#include <glm/glm.hpp>

#include <string>
#include <vector>

namespace World
{
	struct WindowProps
	{
		std::string Title;
		uint32_t Widdth;
		uint32_t Height;
		bool Frameless = false;

		WindowProps(const std::string& title = "World Engine",
			uint32_t width = 1280, uint32_t height = 720, bool frameless = false)
			: Title(title), Widdth(width), Height(height), Frameless(frameless)
		{

		}
	};

	class Window
	{
	public:
		//函数容器
		using EventCallbackFn = std::function<void(Event&)>;

		virtual ~Window() {}

		virtual void OnUpdate() = 0;

		virtual uint32_t GetWidth() const = 0;

		virtual uint32_t GetHeight() const = 0;

		// Window attributes

		// Sets the event callback function
		virtual void SetEventCallback(const EventCallbackFn& callback) = 0;
		// Sets the VSync state
		virtual void SetVsync(bool enabled) = 0;
		// Gets the VSync state
		virtual bool IsVsync() const = 0;


		virtual void* GetNativeWindow() const = 0;
		// 无边框窗口的"边缘缩放带"厚度(物理像素);0 = 平台不提供/窗口有系统边框。
		// WUI 用它来给出 ResizeEW/NS/NWSE 光标 —— 否则用户看不出边缘可以拖。
		virtual int ResizeBorderPixels() const { return 0; }

		// ---- 多窗口(独立浮动窗口)支持 ----
		// GL:把该窗口的上下文设为当前;其他后端为 no-op。
		virtual void MakeCurrent() = 0;
		// GL:交换该窗口的缓冲;Vulkan 由 present target 负责。
		virtual void SwapBuffers() = 0;
		virtual void SetPosition(int x, int y) = 0;
		virtual void GetPosition(int* x, int* y) const = 0;
		virtual void SetSize(uint32_t width, uint32_t height) = 0;
		virtual bool ShouldClose() const = 0;
		virtual void SetShouldClose(bool shouldClose) = 0;
		virtual void Focus() = 0;
		// 显示/隐藏窗口(独立窗口复用:避免运行期销毁窗口)。
		virtual void SetVisible(bool visible) = 0;
		// P4-UX10:显示但不激活(不抢焦点)。恢复上次开着的浮窗时用 —— 用户正在主窗口
		// 工作,不该被一个"恢复出来"的窗口打断;平台不支持时退回 SetVisible。
		virtual void SetVisibleNoActivate(bool visible) { SetVisible(visible); }
		// 无边框窗口自定义标题栏:让系统进入"窗口移动"循环。
		virtual void BeginSystemDrag() = 0;
		// ---- P4-UX9:拖动期间的"投放提示"(不动窗口位置)----
		// 拖动窗口靠系统移动循环(SC_MOVE)才可能 1:1 跟手:它由系统按输入频率移动窗口,
		// 不依赖宿主是否在跑渲染帧(本引擎 Debug 下一帧 50ms,帧驱动拖动必然发飘)。
		// zoneScreenRect = 挂靠栏的屏幕矩形(x,y,w,h;z<=0 = 关闭提示)。
		// 光标进入该矩形时,在它上面画一层独立的半透明提示(不阻塞、不限制窗口移动)——
		// 之前把窗口"压到栏下方"会让用户觉得拖到顶部被挡住(2026-09-20 反馈),已改掉。
		virtual void SetSystemDragDropHint(const glm::vec4& zoneScreenRect)
		{
			(void)zoneScreenRect;
		}
		// 运行时切换无边框(去掉系统标题栏),并安装边缘缩放的命中测试。
		virtual void SetFrameless(bool frameless) = 0;
		virtual bool IsFrameless() const = 0;
		virtual void Minimize() = 0;
		virtual void MaximizeOrRestore() = 0;
		virtual bool IsMaximized() const = 0;

		// ---- W9-2:系统剪贴板与窗口焦点(最小封装)----
		// 供脚本编辑器把 Ctrl+C/X/V 接到系统剪贴板、把快捷键路由接到"窗口是否在前台"。
		// 无窗口/平台不支持时返回空串 / false(宿主不需要额外判空)。
		virtual std::string GetClipboardText() const { return std::string(); }
		virtual void SetClipboardText(const std::string& text) { (void)text; }
		virtual bool IsFocused() const { return false; }

		// ---- D10:OS 文件拖放(资源管理器 → 窗口)----
		// 返回自上次调用以来拖入的文件绝对路径并清空队列;没有拖放时返回空 vector。
		// 主窗口与独立窗口各自维护自己的队列。
		virtual std::vector<std::string> ConsumeDroppedFiles() { return std::vector<std::string>(); }

		// Creates a window
		static Window* Create(const WindowProps& props = WindowProps());
		// 创建共享主窗口图形资源(GL 共享上下文 / Vulkan 同设备)的附加窗口。
		static Window* CreateAuxiliary(const WindowProps& props, Window* share);

	};
}
