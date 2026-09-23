#pragma once
#include "World/Core/Core.h"
#include "World/Core/Timestep.h"
#include "World/Events/Event.h"

namespace World
{
	class Layer
	{
	public:
		Layer(const std::string& name = "Layer");
		virtual ~Layer();

		virtual void OnAttach() {}
		virtual void OnDetach() {}
		virtual void OnUpdate(Timestep ts) {}
		// 每帧 UI 钩子：WUI 在当前呈现目标上构建并绘制界面。
		virtual void OnUiFrame() {}
		virtual void OnEvent(Event& event) {}

		inline const std::string& GetName() const { return m_DebugName; };

	protected:
		std::string m_DebugName;
	};
}
