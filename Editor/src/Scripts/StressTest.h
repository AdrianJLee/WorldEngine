#pragma once
#include "World.h"

namespace World
{
	class StressTest : public ScriptableEntity
	{
		REGISTER_SCRIPT(StressTest);
	public:
		virtual void OnCreate() override
		{
			m_Created.resize(Weight * Height);
			for (int i = 0; i < Weight; i++)
			{
				for (int j = 0; j < Height; j++)
				{
					auto& entity = Entity::CreateEntity(GetEntity().GetScene(), "Empty Entity");
					entity.AddComponent<TransformComponent>(glm::vec3 { i * 1.0f, j * 1.0f, -0.5f });
					entity.AddComponent<SpriteComponent>().Color =
						glm::vec4((float)rand() / RAND_MAX, (float)rand() / RAND_MAX, (float)rand() / RAND_MAX, 1.0f);
					m_Created[i * Height + j] = entity;
				}
			}

		}
		virtual void OnUpdate(Timestep ts) override
		{}
		virtual void OnDestroy() override
		{
			for (auto& entity : m_Created)
			{
				Entity::DestroyEntity(GetEntity().GetScene(), entity);
			}
		}
	private:
		int Weight = 100;
		int Height = 100;
		std::vector<Entity> m_Created;

	};
}