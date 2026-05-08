#pragma once
#include "World.h"

namespace World
{
	class MemoryTraceLayer : public Layer
	{
	public:
		MemoryTraceLayer();
		virtual ~MemoryTraceLayer() = default;
		virtual void OnAttach() override;
		virtual void OnDetach() override;
		virtual void OnUpdate(Timestep ts) override;
		virtual void OnImGuiRender() override;
		virtual void OnEvent(Event& event) override;
	private:
	};
}