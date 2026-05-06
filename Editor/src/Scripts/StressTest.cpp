#include "StressTest.h"

namespace World
{
	void StressTest::CreateTest1()
	{
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
	void StressTest::DestroyTest1()
	{
		for (auto& entity : m_Created)
		{
			Entity::DestroyEntity(GetEntity().GetScene(), entity);
		}
	}
	Entity* StressTest::Create()
	{
		const auto& scene = GetEntity().GetScene();
		auto entity = WLD_POOL_NEW(Entity, scene, scene->GetRegistry().create());
		entity->AddComponent<TagComponent>("Empty Entity");
		entity->AddComponent<UUIDComponent>(UUID());
		return entity;
	}
	void StressTest::CreateTest2()
	{
		for (int i = 0; i < Weight; i++)
		{
			for (int j = 0; j < Height; j++)
			{
				m_Created2[i * Height + j] = Create();
			}
		}

	}
	void StressTest::DestroyTest2()
	{
		for (auto& entity : m_Created2)
		{
			Entity::DestroyEntity(GetEntity().GetScene(), *entity);
			WLD_POOL_DELETE(Entity, PoolTag::General, entity);
		}

	}
}