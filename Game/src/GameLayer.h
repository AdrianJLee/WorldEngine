#pragma once
#include "GameAPI.h"
#include "World.h"
#include "World/Gameplay/GameHost.h"

namespace World
{
	class GameLayer : public Layer
	{
	public:
		explicit GameLayer(WorldContext& context);
		virtual ~GameLayer() = default;
		virtual void OnAttach() override;
		virtual void OnDetach() override;
		virtual void OnUpdate(Timestep ts) override;
		virtual void OnUiFrame() override;
		virtual void OnEvent(Event& event) override;
	private:
		bool OnWindowResize(WindowResizeEvent& e);
		void LoadLevel();
	private:
		WorldContext* m_Context = nullptr;
		// W1:宿主收敛——场景加载/更新/渲染统一走 Gameplay::GameHost。
		Gameplay::GameHost m_Host;
		Ref<SceneRenderer> m_SceneRenderer;
	};
}
