#pragma once

#include "World/WUI/WuiCore.h"

#include <algorithm>
#include <memory>
#include <string>
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
		std::vector<WuiDrawCommand>& Commands() { return m_Commands; }
		WuiStyleSheet& Sheet() { return m_Sheet; }

		template <typename T>
		T& Persist(WuiId id, const T& initial)
		{
			struct Holder : WuiStateBase
			{
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
		void CloseAllPopups() { m_OpenPopups.clear(); }
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

	private:
		struct WuiStateBase
		{
			virtual ~WuiStateBase() = default;
		};

		WuiInputState m_Input;
		std::vector<WuiDrawCommand> m_Commands;
		std::unordered_map<WuiId, std::shared_ptr<WuiStateBase>> m_State;
		std::vector<WuiStyle> m_StyleStack;
		WuiStyleSheet m_Sheet;
		WuiId m_Focus = 0;
		bool m_TextInputActive = false;
		WuiCursor m_Cursor = WuiCursor::Arrow;
		std::vector<WuiId> m_OpenPopups;
		WuiId m_Modal = 0;
		glm::vec2 m_ViewportSize { 1280, 720 };
		bool m_Dragging = false;
		WuiId m_DragId = 0;
		std::string m_DragPayload;
		bool m_DropArmed = false;
		bool m_DropAccepted = false;
		bool m_DragPending = false;
		glm::vec2 m_DragPressPos { 0, 0 };
	};
}
