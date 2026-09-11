#include "StressTest.h"

namespace World
{
	void StressTest::CreateTest1()
	{
		if (Weight <= 0 || Height <= 0) return;
		const int width = Weight;
		const int height = Height;
		const auto created = m_Created;
		GetEntity().GetScene()->DeferStructuralChange([width, height, created](Scene& scene) {
			const size_t total = static_cast<size_t>(width) * static_cast<size_t>(height);
			auto& registry = scene.GetRegistry();
			std::vector<entt::entity> entities(total);
			created->resize(total);
			registry.create(entities.begin(), entities.end());
			for (size_t i = 0; i < total; ++i) (*created)[i] = Entity(&scene, entities[i]);

			std::vector<TagComponent> tags(total, TagComponent("Empty Entity"));
			std::vector<UUIDComponent> uuids(total);
			std::vector<TransformComponent> transforms(total);
			std::vector<SpriteComponent> sprites(total);
			for (int i = 0; i < width; ++i)
			{
				for (int j = 0; j < height; ++j)
				{
					const size_t index = static_cast<size_t>(i) * height + j;
					uuids[index] = UUIDComponent(UUID());
					transforms[index].SetLocation({ static_cast<float>(i), static_cast<float>(j), -0.5f });
					sprites[index].Color = glm::vec4(static_cast<float>(rand()) / RAND_MAX,
						static_cast<float>(rand()) / RAND_MAX, static_cast<float>(rand()) / RAND_MAX, 1.0f);
				}
			}
			registry.insert<TagComponent>(entities.begin(), entities.end(), tags.begin());
			registry.insert<UUIDComponent>(entities.begin(), entities.end(), uuids.begin());
			registry.insert<TransformComponent>(entities.begin(), entities.end(), transforms.begin());
			registry.insert<SpriteComponent>(entities.begin(), entities.end(), sprites.begin());
		});
	}

	void StressTest::DestroyTest1()
	{
		for (const auto entity : *m_Created)
			Entity::DestroyEntity(GetEntity().GetScene(), entity);
		m_Created->clear();
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
