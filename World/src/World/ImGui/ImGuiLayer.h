#pragma once

#include "World/Core/Layer.h"
#include "World/Events/KeyEvent.h"
#include "World/Events/ApplicationEvent.h"
#include "World/Events/MouseEvent.h"

struct ImFont;

namespace World
{
	class ImGuiLayer : public Layer
	{
	public:
		ImGuiLayer();
		~ImGuiLayer() override;

		void OnAttach() override;
		void OnDetach() override;
		void OnImGuiRender() override;
		void OnEvent(Event& event) override;

		void Begin();
		void End();

		void SetBlockEvents(bool block) { m_BlockEvents = block; }
		void SetDarkThemeColors();
		static ImFont* GetBoldFont() { return s_BoldFont; }
		static ImFont* GetCjkFont() { return s_CjkFont; }
		static ImFont* GetDefaultFont();
		// 编辑器 IME 开关(视口聚焦/文本输入状态由宿主统一管理)。
		static void ApplyImeState(bool enabled);
	private:
		bool m_BlockEvents = true;
		float m_Time = 0.0f;
		static ImFont* s_BoldFont;
		static ImFont* s_CjkFont;

	};
}
