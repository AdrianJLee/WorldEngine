#pragma once

#include "World/WUI/WuiCore.h"
#include "World/WUI/WuiOperationLog.h"
#include "World/WUI/WuiUndoStack.h"
#include "World/Core/Log.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <memory>
#include <string>
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
	};

	// 帧级上下文:输入、持久状态、样式栈、焦点、绘制命令。
	class WuiContext
	{
	public:
		WuiContext();

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
		void SetTextInputActive(bool active) { m_TextInputActive = active; }
		bool IsTextInputActive() const { return m_TextInputActive; }
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
