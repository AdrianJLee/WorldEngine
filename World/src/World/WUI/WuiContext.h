#pragma once

#include "World/WUI/WuiCore.h"

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
		float Wheel = 0;
		bool WantKeyboard = false;
	};

	enum class WuiDrawKind : uint8_t
	{
		Rect,
		RectOutline,
		Text,
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
		bool IsHovered(const WuiRect& rect) const { return HitTest(rect, m_Input.MousePos); }
		bool IsClicked(const WuiRect& rect, int button = 0) const
		{
			return HitTest(rect, m_Input.MousePos) && m_Input.MouseClicked[button];
		}

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
	};
}
