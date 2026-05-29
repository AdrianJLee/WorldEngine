#include "GameLayer.h"

namespace World
{
	GameLayer::GameLayer()
		:Layer("GameLayer")
	{}
	void GameLayer::OnAttach()
	{
		WLD_PROFILE_FUNCTION();
		LoadScene();
	}
	void GameLayer::OnDetach()
	{
		WLD_PROFILE_FUNCTION();
	}
	void GameLayer::OnUpdate(Timestep ts)
	{
		WLD_PROFILE_FUNCTION();

	}
	void GameLayer::OnImGuiRender()
	{

	}
	void GameLayer::OnEvent(Event& event)
	{}
	void GameLayer::LoadScene()
	{
		m_ActiveScene = CreateRef<Scene>();
		SceneSerializer serializer(m_ActiveScene);
		std::filesystem::path scenePath = "Resource/Scenes/TestScene.wdscene";
		if (std::filesystem::exists(scenePath))
		{
			WLD_CORE_INFO("Loading scene from {0}", scenePath.string());
			if (!serializer.Deserialize(scenePath.string()))
			{
				WLD_CORE_ERROR("Failed to load scene!");
			}
			else
			{
				WLD_CORE_INFO("Scene loaded successfully!");
			}
		}
		else
		{
			WLD_CORE_ERROR("Scene file does not exist: {0}", scenePath.string());
			return;
		}
	}
}