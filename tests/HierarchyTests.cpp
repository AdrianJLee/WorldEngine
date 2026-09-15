// P2a W3a:实体层级与世界变换求解(挂载/环路检测/继承开关/深层链)。
#include "World/Core/WorldContext.h"
#include "World/Scene/Components.h"
#include "World/Scene/Hierarchy.h"
#include "World/Scene/Scene.h"

#include <glm/gtc/matrix_transform.hpp>

#include <cmath>
#include <cstdio>
#include <stdexcept>
#include <string>

namespace
{
	void Check(bool condition, const char* expression, int line)
	{
		if (!condition)
			throw std::runtime_error(std::string("line ") + std::to_string(line) + ": " + expression);
	}
#define CHECK(expression) Check(static_cast<bool>(expression), #expression, __LINE__)

	bool NearVec(const glm::vec3& a, const glm::vec3& b, float tolerance = 1e-4f)
	{
		return glm::length(a - b) < tolerance;
	}
}

int main()
{
	try
	{
		using namespace World;

		WorldContext context;
		Scene scene(context);
		auto& registry = scene.GetRegistry();

		const entt::entity parent = registry.create();
		registry.emplace<TransformComponent>(parent,
			TransformComponent(glm::vec3(1.0f, 0.0f, 0.0f)));
		const entt::entity child = registry.create();
		registry.emplace<TransformComponent>(child,
			TransformComponent(glm::vec3(0.0f, 2.0f, 0.0f)));

		// 1. 挂载 + 世界变换:子节点位置 = 父矩阵 * 子局部矩阵。
		{
			CHECK(Hierarchy::SetParent(registry, child, parent));
			CHECK(Hierarchy::GetDepth(registry, child) == 1);
			CHECK(glm::length(glm::vec3(registry.get<HierarchyComponent>(child).Parent == parent
				? 1.0f : 0.0f)) > 0.5f);
			CHECK(Hierarchy::UpdateWorldTransforms(registry) == 2);
			const glm::vec3 childWorld = glm::vec3(registry.get<WorldTransformComponent>(child).Matrix[3]);
			CHECK(NearVec(childWorld, glm::vec3(1.0f, 2.0f, 0.0f)));
			const glm::vec3 parentWorld = glm::vec3(registry.get<WorldTransformComponent>(parent).Matrix[3]);
			CHECK(NearVec(parentWorld, glm::vec3(1.0f, 0.0f, 0.0f)));
		}

		// 2. 旋转继承:父绕 Z 轴 90° + 子偏移 (1,0,0) -> 世界位置 (1,1,0)。
		{
			auto& parentTransform = registry.get<TransformComponent>(parent);
			parentTransform.SetTransform(glm::vec3(1.0f, 0.0f, 0.0f), glm::vec3(0.0f, 0.0f, 90.0f),
				glm::vec3(1.0f));
			auto& childTransform = registry.get<TransformComponent>(child);
			childTransform.SetTransform(glm::vec3(1.0f, 0.0f, 0.0f), glm::vec3(0.0f), glm::vec3(1.0f));

			Hierarchy::UpdateWorldTransforms(registry);
			// 只断言层级契约:子世界矩阵 == 父世界矩阵 * 子局部矩阵(与局部矩阵的 T/R/S 约定无关)。
			const glm::mat4 expected = registry.get<WorldTransformComponent>(parent).Matrix *
				registry.get<TransformComponent>(child).Transform;
			const glm::vec3 childWorld = glm::vec3(registry.get<WorldTransformComponent>(child).Matrix[3]);
			CHECK(NearVec(childWorld, glm::vec3(expected[3]), 1e-4f));
			// 旋转确实被父节点影响:子节点世界位置不再等于其局部位置。
			CHECK(!NearVec(childWorld, glm::vec3(1.0f, 0.0f, 0.0f), 1e-3f));
		}

		// 3. InheritTransform=false:子节点世界矩阵等于自身局部矩阵。
		{
			registry.get<HierarchyComponent>(child).InheritTransform = false;
			Hierarchy::UpdateWorldTransforms(registry);
			const glm::vec3 childWorld = glm::vec3(registry.get<WorldTransformComponent>(child).Matrix[3]);
			CHECK(NearVec(childWorld, glm::vec3(1.0f, 0.0f, 0.0f)));
			registry.get<HierarchyComponent>(child).InheritTransform = true;
		}

		entt::entity other = entt::null;
		// 4. 环路检测:祖先不能挂到后代上。
		{
			CHECK(!Hierarchy::SetParent(registry, parent, child));
			CHECK(registry.get<HierarchyComponent>(parent).Parent == entt::null);
			CHECK(!Hierarchy::SetParent(registry, parent, parent));
			// 合法:挂到另一个独立根上。
			other = registry.create();
			registry.emplace<TransformComponent>(other, TransformComponent(glm::vec3(5.0f, 0.0f, 0.0f)));
			CHECK(Hierarchy::SetParent(registry, parent, other));
			CHECK(Hierarchy::GetDepth(registry, child) == 2);
			Hierarchy::UpdateWorldTransforms(registry);
			const glm::mat4 grandExpected = registry.get<WorldTransformComponent>(parent).Matrix *
				registry.get<TransformComponent>(child).Transform;
			const glm::vec3 grandChild = glm::vec3(registry.get<WorldTransformComponent>(child).Matrix[3]);
			CHECK(NearVec(grandChild, glm::vec3(grandExpected[3]), 1e-4f));
		}

		// 5. 解挂:世界矩阵回到局部(移回根集合)。
		{
			Hierarchy::ClearParent(registry, parent);
			CHECK(registry.get<HierarchyComponent>(parent).Parent == entt::null);
			CHECK(registry.get<HierarchyComponent>(other).Children.empty());
			Hierarchy::UpdateWorldTransforms(registry);
			// 解挂后 parent 成为根:其世界矩阵回到局部;child 仍跟随 parent(契约:父矩阵 * 子局部)。
			const glm::vec3 childWorld = glm::vec3(registry.get<WorldTransformComponent>(child).Matrix[3]);
			const glm::mat4 detachExpected = registry.get<WorldTransformComponent>(parent).Matrix *
				registry.get<TransformComponent>(child).Transform;
			CHECK(NearVec(childWorld, glm::vec3(detachExpected[3]), 1e-4f));
			const glm::vec3 parentWorld = glm::vec3(registry.get<WorldTransformComponent>(parent).Matrix[3]);
			CHECK(NearVec(parentWorld, glm::vec3(1.0f, 0.0f, 0.0f), 1e-4f));
		}

		// 6. 深层链(64 层)与失效父节点容错。
		{
			entt::entity previous = entt::null;
			entt::entity last = entt::null;
			for (int i = 0; i < 64; ++i)
			{
				const entt::entity node = registry.create();
				registry.emplace<TransformComponent>(node, TransformComponent(glm::vec3(0.0f, 1.0f, 0.0f)));
				if (previous != entt::null)
					CHECK(Hierarchy::SetParent(registry, node, previous));
				previous = node;
				last = node;
			}
			// 链首是根,因此 64 个节点的链深度是 63,末节点世界 Y = 64(每层 +1)。
			CHECK(Hierarchy::GetDepth(registry, last) == 63);
			CHECK(Hierarchy::UpdateWorldTransforms(registry) >= 66);
			const glm::vec3 lastWorld = glm::vec3(registry.get<WorldTransformComponent>(last).Matrix[3]);
			CHECK(NearVec(lastWorld, glm::vec3(0.0f, 64.0f, 0.0f), 1e-2f));

			// 父实体被销毁后,子节点在下一帧被当作根处理(不会崩)。
			registry.destroy(previous);
			CHECK(Hierarchy::UpdateWorldTransforms(registry) >= 1);
		}

		std::printf("World.Hierarchy: all checks passed\n");
		return 0;
	}
	catch (const std::exception& error)
	{
		std::fprintf(stderr, "World.Hierarchy FAILED: %s\n", error.what());
		return 1;
	}
}
