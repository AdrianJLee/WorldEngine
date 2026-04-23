#pragma once

#include "World/Core/Layer.h"
#include "World/Events/KeyEvent.h"
#include "World/Events/ApplicationEvent.h"
#include "World/Events/MouseEvent.h"

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

		static void ShowDockSpaceBack(bool autoEnd = true);

		void SetBlockEvents(bool block) { m_BlockEvents = block; }
		void SetDarkThemeColors();
	public:
		static bool m_Show;
	private:
		bool m_BlockEvents = true;
		float m_Time;

	};
}