#pragma once

#include "World/WUI/WuiCore.h"
#include "World/WUI/WuiOperationLog.h"
#include "World/WUI/WuiUndoStack.h"
#include "World/Core/Export.h"
#include "World/Core/Log.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <typeinfo>
#include <unordered_map>
#include <vector>

namespace World::Wui
{
	struct WuiInputState
	{
		glm::vec2 MousePos { 0, 0 };
		bool MouseDown[3] = { false, false, false };
		bool MouseClicked[3] = { false, false, false };
		bool MouseReleased[3] = { false, false, false };
		bool MouseDoubleClicked[3] = { false, false, false };
		float Wheel = 0;
		bool WantKeyboard = false;
		bool Ctrl = false, Shift = false, Alt = false;
		std::vector<uint32_t> KeyDown;   // 引擎 KeyCodes
		std::vector<uint32_t> TextInput; // 本帧输入字符(UTF-32)
		glm::vec2 ViewportSize { 1280, 720 };
		float FPS = 0;
	};

	enum class WuiDrawKind : uint8_t
	{
		Rect,
		RectOutline,
		Text,
		Image,
		// 任意四边形(顶点按左上/右上/右下/左下顺序):画斜线、箭头、圆环等
		// 轴对齐矩形覆盖不到的形状。4 个顶点放在 WuiDrawCommand::Vertices。
		Quad,
		ClipPush,
		ClipPop,
	};

	struct WuiDrawCommand
	{
		WuiDrawKind Kind = WuiDrawKind::Rect;
		WuiRect Rect;
		WuiColor Color;
		float Rounding = 0;
		float Thickness = 1;
		std::string Text;
		float FontSize = 15;
		bool Bold = false;
		uint64_t Image = 0;
		WuiRect Uv { 0, 0, 1, 1 };
		int TextSelStart = -1;
		int TextSelEnd = -1;
		int TextCursorByte = -1;
		// Quad 命令的 4 个顶点(屏幕坐标);其余命令忽略。
		std::array<glm::vec2, 4> Vertices { glm::vec2 { 0, 0 }, glm::vec2 { 0, 0 }, glm::vec2 { 0, 0 }, glm::vec2 { 0, 0 } };
		// W9:字体族选择(默认 Ui,追加在末尾保持既有聚合初始化兼容)。
		WuiFontFamily Family = WuiFontFamily::Ui;
		// W9:代码编辑器的闪烁 caret(逐行局部字节偏移,-1 = 不画)。与 TextCursorByte
		// 的区别:该字段由后端做 0.5s 闪烁,且按真实度量画 1.5px。
		int TextCaretByte = -1;
	};

	// ---- 文本度量钩子 ----
	// WUI 核心是纯逻辑,不知道字体;渲染后端在初始化时注册真实字形度量,headless
	// 测试可注入假度量(逐码点宽度自定)。未注册时回退为估算宽度(ASCII 0.6em,
	// 其余 1.0em),保证纯逻辑路径不依赖后端。
	using WuiTextMeasureFn = std::function<float(std::string_view utf8, float fontSize, WuiFontFamily family)>;
	// owner 用于注销:只有注册者本人可以清除钩子(多后端/多窗口下不会互相踩)。
	void WLD_API SetTextMeasureHook(void* owner, WuiTextMeasureFn fn);
	void WLD_API ClearTextMeasureHook(void* owner);
	float WLD_API MeasureTextWithHook(std::string_view utf8, float fontSize, WuiFontFamily family);

	// ---- 全局文本焦点登记 ----
	// 仿 WuiAccessibility:多个窗口(WuiContext 实例)共享一份登记,每帧由各窗口重建。
	// 用途:GLFW 事件在 UI 帧之后分发,EditorLayer::OnKeyPressed 只能用"上一帧登记的
	// 文本焦点"决定按键是给文本控件还是引擎全局快捷键(W9-2 的三层路由)。
	class WLD_API WuiTextFocus
	{
	public:
		struct Entry
		{
			WuiId Id = 0;
			std::string Window; // "main" / "float:<面板>"(来自 WuiAccessibility,关闭控制通道时可能为空)
			std::string Panel;  // 面板 id
		};

		static WuiTextFocus& Get();

		// 每个窗口(上下文)在 BeginFrame 时调用:清掉本上下文上一帧的登记。
		void BeginContextFrame(const void* context);
		// 取得焦点的文本控件在绘制时调用(每帧重建)。
		void Set(const void* context, WuiId id, std::string window, std::string panel);
		void Clear();

		// 当前是否存在持有焦点的文本控件。
		bool Active() const { return !m_Entries.empty(); }
		// 最近登记的文本焦点(多窗口同时有文本焦点时,以最后一次绘制登记为准);无则 nullptr。
		const Entry* Current() const { return m_Entries.empty() ? nullptr : &m_Entries.back().Info; }
		WuiId Id() const { return m_Entries.empty() ? 0 : m_Entries.back().Info.Id; }
		const std::string& Window() const;
		const std::string& Panel() const;

	private:
		struct Item
		{
			const void* Context = nullptr;
			Entry Info;
		};
		std::vector<Item> m_Entries;
	};

	// 帧级上下文:输入、持久状态、样式栈、焦点、绘制命令。
	class WuiContext
	{
	public:
		WuiContext();
		// 销毁时清掉本窗口的文本焦点登记:否则关掉的窗口会永远吞掉全局快捷键。
		~WuiContext();

		void BeginFrame(const WuiInputState& input);
		void EndFrame();

		WuiInputState& Input() { return m_Input; }
		const WuiInputState& Input() const { return m_Input; }
		std::vector<WuiDrawCommand>& Commands() { return m_OverlayDepth > 0 ? m_OverlayCommands : m_Commands; }
		const std::vector<WuiDrawCommand>& OverlayCommands() const { return m_OverlayCommands; }
		// 进入/退出顶层绘制:弹出菜单、模态等画在普通 UI 之上。
		void PushOverlay() { ++m_OverlayDepth; }
		void PopOverlay() { if (m_OverlayDepth > 0) --m_OverlayDepth; }
		WuiStyleSheet& Sheet() { return m_Sheet; }

		template <typename T>
		T& Persist(WuiId id, const T& initial)
		{
			struct Holder : WuiStateBase
			{
				Holder() { TypeName = typeid(T).name(); }
				T Value;
			};
			auto it = m_State.find(id);
			if (it == m_State.end())
			{
				auto holder = std::make_shared<Holder>();
				holder->Value = initial;
				m_State[id] = holder;
				return static_cast<Holder*>(holder.get())->Value;
			}
			if (!it->second->TypeName || std::strcmp(it->second->TypeName, typeid(T).name()) != 0)
				WLD_CORE_ERROR("WUI persisted state id {0} reused with different types ({1} vs {2})",
					id, it->second->TypeName ? it->second->TypeName : "null", typeid(T).name());
			return static_cast<Holder*>(it->second.get())->Value;
		}

		void PushStyle(const WuiStyle& style);
		void PopStyle();
		WuiStyle CurrentStyle() const;

		void SetFocus(WuiId id) { m_Focus = id; }
		WuiId Focus() const { return m_Focus; }
		// W9 review:窗口/面板身份由宿主显式告知,不依赖仅在 --ai-control 下才启用的
		// WuiAccessibility(否则日常会话里文本焦点登记的 Window/Panel 为空,快捷键路由错位)。
		void SetWindowKey(std::string windowKey) { m_WindowKey = std::move(windowKey); }
		void SetPanelId(std::string panelId) { m_PanelId = std::move(panelId); }
		const std::string& WindowKey() const { return m_WindowKey; }
		const std::string& PanelId() const { return m_PanelId; }
		// 文本输入态 + 全局文本焦点登记(聚焦的文本控件每帧调用一次 SetTextInputActive(true))。
		void SetTextInputActive(bool active);
		bool IsTextInputActive() const { return m_TextInputActive; }
		// 文本度量(命中测试/行宽/横向滚动)。family 决定字体族,后端注册真实度量。
		float MeasureTextWidth(std::string_view utf8, float fontSize, WuiFontFamily family = WuiFontFamily::Ui) const;
		void SetCursor(WuiCursor cursor) { m_Cursor = cursor; }
		WuiCursor Cursor() const { return m_Cursor; }
		uint64_t Frame() const { return m_Frame; }
		WuiOperationLog& Ops() { return m_Ops; }
		const WuiOperationLog& Ops() const { return m_Ops; }
		WuiUndoStack& History() { return m_History; }
		void RecordOp(std::string category, std::string action, std::string target, std::string detail);
		bool IsKeyPressed(uint32_t keyCode) const
		{
			return std::find(m_Input.KeyDown.begin(), m_Input.KeyDown.end(), keyCode) != m_Input.KeyDown.end();
		}
		bool IsHovered(const WuiRect& rect) const { return HitTest(rect, m_Input.MousePos); }
		bool IsClicked(const WuiRect& rect, int button = 0) const
		{
			return HitTest(rect, m_Input.MousePos) && m_Input.MouseClicked[button];
		}
		bool IsDoubleClicked(const WuiRect& rect, int button = 0) const
		{
			return HitTest(rect, m_Input.MousePos) && m_Input.MouseDoubleClicked[button];
		}

		// ---- 弹窗/模态 ----
		void OpenPopup(WuiId id);
		void ClosePopup(WuiId id);
		void CloseAllPopups() { m_OpenPopups.clear(); m_PopupOpenFrame.clear(); }
		bool IsPopupOpen(WuiId id) const;
		bool ClosePopupsOnOutsideClick(const std::vector<WuiId>& popups, const WuiRect& ignoreRect);
		void SetModal(WuiId id) { m_Modal = id; }
		void ClearModal() { m_Modal = 0; }
		WuiId Modal() const { return m_Modal; }
		void SetViewportSize(glm::vec2 size) { m_ViewportSize = size; }
		glm::vec2 ViewportSize() const { return m_ViewportSize; }

		// ---- 拖拽/放置 ----
		void BeginDrag(WuiId id, const std::string& payload);
		bool IsDragActive(std::string* payload) const;
		void EndDrag();
		// 拖动期间每帧调用以保持目标矩形;释放后 AcceptDrop 返回 true。
		bool DropTarget(const WuiRect& rect);
		// 仅当 payload 以指定前缀开头时武装落点(区分面板拖拽与文件拖拽)。
		bool DropTarget(const WuiRect& rect, const std::string& payloadPrefix);
		// 每帧构建 UI 前调用:清空上一帧的落点,只保留本帧悬停命中的目标。
		void ClearDropTarget() { m_DropArmed = false; }
		bool AcceptDrop(std::string* payload);
		// 仅当 payload 以指定前缀开头时才消费;不匹配返回 false 且不消耗,
		// 把 drop 留给声明了匹配前缀的其他消费者。
		bool AcceptDrop(std::string* payload, const std::string& payloadPrefix);

		// ---- 悬停遮挡区(叠放在上层的浮动面板用)----
		// 本 UI 的命中测试就是矩形包含判定,没有 z 序;窗口内浮动面板画在停靠区之上时,
		// 需要把它的矩形登记成遮挡区,让下面的面板在命中测试里判为未命中,
		// 否则浮动面板与它下面的面板会同时响应同一次点击。
		// 用法:画下层之前 Push,画浮动面板之前 Clear(浮动面板自身要能命中)。
		void PushHoverBlocker(const WuiRect& rect) { m_HoverBlockers.push_back(rect); }
		void ClearHoverBlockers() { m_HoverBlockers.clear(); }

	private:
		// 带遮挡区判定的命中测试:IsHovered/IsClicked/DropTarget 都走它。
		bool HitTest(const WuiRect& rect, glm::vec2 point) const;
		struct WuiStateBase
		{
			const char* TypeName = nullptr;
			virtual ~WuiStateBase() = default;
		};

		WuiInputState m_Input;
		std::vector<WuiDrawCommand> m_Commands;
		std::vector<WuiDrawCommand> m_OverlayCommands;
		int m_OverlayDepth = 0;
		std::unordered_map<WuiId, std::shared_ptr<WuiStateBase>> m_State;
		std::vector<WuiStyle> m_StyleStack;
		WuiStyleSheet m_Sheet;
		WuiId m_Focus = 0;
		bool m_TextInputActive = false;
		std::string m_WindowKey;
		std::string m_PanelId;
		WuiCursor m_Cursor = WuiCursor::Arrow;
		std::vector<WuiId> m_OpenPopups;
		std::unordered_map<WuiId, uint64_t> m_PopupOpenFrame;
		WuiId m_Modal = 0;
		glm::vec2 m_ViewportSize { 1280, 720 };
		bool m_Dragging = false;
		WuiId m_DragId = 0;
		std::string m_DragPayload;
		bool m_DropArmed = false;
		bool m_DropAccepted = false;
		bool m_DragPending = false;
		std::vector<WuiRect> m_HoverBlockers;
		glm::vec2 m_DragPressPos { 0, 0 };
		uint64_t m_Frame = 0;
		WuiOperationLog m_Ops;
		WuiUndoStack m_History;
	};
}
