#include "StressTest.h"

namespace World
{
	void StressTest::CreateTest1()
	{
		uint32_t totalCount = Weight * Height;
		if (totalCount == 0) return;

		auto scene = GetEntity().GetScene();
		auto& registry = scene->GetRegistry();

		// 1. 批量申请纯净的实体 ID
		std::vector<entt::entity> entities(totalCount);
		registry.create(entities.begin(), entities.end());

		// 2. 准备组件数据数组
		std::vector<TagComponent> tags(totalCount, TagComponent("Empty Entity"));
		std::vector<UUIDComponent> uuids(totalCount);
		std::vector<TransformComponent> transforms(totalCount);
		std::vector<SpriteComponent> sprites(totalCount);

		for (int i = 0; i < Weight; i++)
		{
			for (int j = 0; j < Height; j++)
			{
				int index = i * Height + j;

				// 填充 UUID（每实体必须有一个唯一ID）
				uuids[index] = UUIDComponent(UUID());

				// 计算出坐标点
				transforms[index].SetLocation(glm::vec3 { i * 1.0f, j * 1.0f, -0.5f });

				// 预生成颜色数据 (避免跨线程锁和频繁取随机数)
				sprites[index].Color = glm::vec4(
					(float)rand() / RAND_MAX,
					(float)rand() / RAND_MAX,
					(float)rand() / RAND_MAX,
					1.0f
				);

				// 留存供摧毁时使用
				m_Created[index] = Entity(scene, entities[index]);
			}
		}

		// 3. 极速批处理插入到底层内存！
		// 它们将在底层引擎被直接 memcpy 或者连续构造进去，而不用一次次的寻找可用内存
		registry.insert<TagComponent>(entities.begin(), entities.end(), tags.begin());
		registry.insert<UUIDComponent>(entities.begin(), entities.end(), uuids.begin());
		registry.insert<TransformComponent>(entities.begin(), entities.end(), transforms.begin());
		registry.insert<SpriteComponent>(entities.begin(), entities.end(), sprites.begin());
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
		//WLD_STACK_WIZARD(testStack, 1024 * 1024); // 1 MB 栈空间

		{
			WLD_STACK_NEW(int, m_Stack, 42); // 在栈上分配一个 int，值为 42

			{
				WLD_STACK_NEW(float, m_Stack, 3.14f); // 在栈上分配一个 float，值为 3.14
			}
			StackTest2();
			{
				WLD_STACK_NEW(glm::vec4, m_Stack, 1.0f, 0.0f, 0.0f, 1.0f); // 在栈上分配一个 vec4，值为红色
			}

		}


	}
	void StressTest::StackTest2()
	{
		WLD_STACK_WIZARD(testStack, 512 * 512, true); // 1 MB 栈空间

		{
			WLD_STACK_NEW(int, testStack, 42); // 在栈上分配一个 int，值为 42
		}

		StackTest3();
	}
	void StressTest::StackTest3()
	{
		WLD_STACK_WIZARD(testStack, 256 * 256, true); // 1 MB 栈空间

		{
			WLD_STACK_NEW(double, testStack, 42); // 在栈上分配一个 double，值为 42

		}

	}
	struct FrameTestStruct
	{
		glm::mat4 a[10000];
	};

	void StressTest::FrameTest1()
	{
		WLD_FRAME_NEW(glm::mat4, glm::mat4(1.0f)); // 在帧分配器上分配一个 mat4x4，值为单位矩阵
		WLD_FRAME_NEW(FrameTestStruct); // 在帧分配器上分配一个 int，值为 42


	}
	void StressTest::FrameTest2()
	{

	}
}

