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
	void StressTest::StackTest1()
	{
		WLD_STACK_WIZARD(testStack, 1024 * 1024); // 1 MB 栈空间

		{
			WLD_STACK_NEW(int, testStack, 42); // 在栈上分配一个 int，值为 42

			{
				WLD_STACK_NEW(float, testStack, 3.14f); // 在栈上分配一个 float，值为 3.14
			}
			StackTest2();
			{
				WLD_STACK_NEW(glm::vec4, testStack, 1.0f, 0.0f, 0.0f, 1.0f); // 在栈上分配一个 vec4，值为红色
			}
		}


	}
	void StressTest::StackTest2()
	{
		WLD_STACK_WIZARD(testStack, 512 * 512); // 1 MB 栈空间

		{
			WLD_STACK_NEW(int, testStack, 42); // 在栈上分配一个 int，值为 42
		}

		StackTest3();
	}
	void StressTest::StackTest3()
	{
		WLD_STACK_WIZARD(testStack, 256 * 256); // 1 MB 栈空间

		{
			WLD_STACK_NEW(double, testStack, 42); // 在栈上分配一个 double，值为 42

		}

	}
}

