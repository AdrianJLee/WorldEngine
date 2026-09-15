// P2a W4:Prefab 子树实例化(深拷贝 + UUID 重发 + 层级重建 + 挂到目标父节点)。
#include "World/Core/WorldContext.h"
#include "World/Gameplay/Prefab.h"
#include "World/Scene/Components.h"
#include "World/Scene/Hierarchy.h"
#include "World/Scene/Scene.h"

#include <cstdio>
#include <stdexcept>
#include <string>
#include <unordered_set>

namespace
{
	void Check(bool condition, const char* expression, int line)
	{
		if (!condition)
			throw std::runtime_error(std::string("line ") + std::to_string(line) + ": " + expression);
	}
#define CHECK(expression) Check(static_cast<bool>(expression), #expression, __LINE__)
}

int main()
{
	try
	{
		using namespace World;
		using namespace World::Gameplay;

		WorldContext context;
		Scene source(context);
		Scene destination(context);

		// 源场景:Root(带 MeshRenderer)-> Child(带 Transform) / Root2(独立实体,不应被复制)
		auto& sourceRegistry = source.GetRegistry();
		const entt::entity root = sourceRegistry.create();
		sourceRegistry.emplace<UUIDComponent>(root, UUID());
		sourceRegistry.emplace<TagComponent>(root, "Prefab Root");
		sourceRegistry.emplace<TransformComponent>(root, TransformComponent(glm::vec3(1.0f, 2.0f, 3.0f)));
		MeshRendererComponent mesh;
		mesh.Primitive = "cube";
		mesh.Color = { 0.2f, 0.6f, 0.9f, 1.0f };
		sourceRegistry.emplace<MeshRendererComponent>(root, mesh);

		const entt::entity child = sourceRegistry.create();
		sourceRegistry.emplace<UUIDComponent>(child, UUID());
		sourceRegistry.emplace<TagComponent>(child, "Prefab Child");
		sourceRegistry.emplace<TransformComponent>(child, TransformComponent(glm::vec3(0.0f, 1.0f, 0.0f)));
		CHECK(Hierarchy::SetParent(sourceRegistry, child, root));

		const entt::entity unrelated = sourceRegistry.create();
		sourceRegistry.emplace<UUIDComponent>(unrelated, UUID());
		sourceRegistry.emplace<TagComponent>(unrelated, "Not In Prefab");

		// 目标场景:先放一个宿主父节点,实例应挂到它下面。
		auto& destinationRegistry = destination.GetRegistry();
		const entt::entity host = destinationRegistry.create();
		destinationRegistry.emplace<UUIDComponent>(host, UUID());
		destinationRegistry.emplace<TagComponent>(host, "Host");
		destinationRegistry.emplace<TransformComponent>(host, TransformComponent(glm::vec3(10.0f, 0.0f, 0.0f)));

		// 1. 实例化:只复制子树(2 个实体),挂到 Host 下。
		const PrefabInstanceResult instance =
			Instantiate(source, Entity(&source, root), destination, host);
		CHECK(instance.IsValid());
		CHECK(instance.EntityCount == 2);
		CHECK(instance.Root.IsValid());
		CHECK(instance.Root.GetScene() == &destination);

		// 2. 组件值被复制,但 UUID 必须重新生成(不与源共享身份)。
		const auto instanceMesh = destinationRegistry.get<MeshRendererComponent>(
			static_cast<entt::entity>(instance.Root));
		CHECK(instanceMesh.Primitive == "cube");
		CHECK(std::fabs(instanceMesh.Color.z - 0.9f) < 1e-5f);
		const UUID sourceUuid = sourceRegistry.get<UUIDComponent>(root).ID;
		const UUID instanceUuid = destinationRegistry.get<UUIDComponent>(static_cast<entt::entity>(instance.Root)).ID;
		CHECK(!(sourceUuid == instanceUuid));

		// 3. 层级重建:实例根挂在 Host 下,子节点仍在实例根下,且世界矩阵按新父求解。
		const entt::entity instanceRootHandle = static_cast<entt::entity>(instance.Root);
		CHECK(destinationRegistry.get<HierarchyComponent>(instanceRootHandle).Parent == host);
		const auto& instanceChildren = destinationRegistry.get<HierarchyComponent>(instanceRootHandle).Children;
		CHECK(instanceChildren.size() == 1);
		const entt::entity instanceChild = instanceChildren[0];
		CHECK(destinationRegistry.get<HierarchyComponent>(instanceChild).Parent == instanceRootHandle);
		CHECK(destinationRegistry.get<TagComponent>(instanceChild).Tag == "Prefab Child");
		// Host(10,0,0) + Root(1,2,3) + Child(0,1,0) → 子节点世界位置 (11,3,3)。
		const glm::vec3 childWorld =
			glm::vec3(destinationRegistry.get<WorldTransformComponent>(instanceChild).Matrix[3]);
		CHECK(glm::length(childWorld - glm::vec3(11.0f, 3.0f, 3.0f)) < 1e-3f);

		// 4. 未参与子树的实体不被复制。
		size_t notInPrefab = 0;
		for (const auto entity : destinationRegistry.view<TagComponent>())
			if (destinationRegistry.get<TagComponent>(entity).Tag == "Not In Prefab")
				notInPrefab++;
		CHECK(notInPrefab == 0);

		// 5. 目标场景内 UUID 唯一(4 个实体:Host + 实例 2 + 无);第二次实例化也不撞。
		const PrefabInstanceResult second =
			Instantiate(source, Entity(&source, root), destination, entt::null);
		CHECK(second.IsValid());
		std::unordered_set<uint64_t> uuids;
		for (const auto entity : destinationRegistry.view<UUIDComponent>())
			uuids.insert(static_cast<uint64_t>(destinationRegistry.get<UUIDComponent>(entity).ID));
		CHECK(uuids.size() == static_cast<size_t>(destinationRegistry.view<UUIDComponent>().size()));

		std::printf("World.Prefab: all checks passed\n");
		return 0;
	}
	catch (const std::exception& error)
	{
		std::fprintf(stderr, "World.Prefab FAILED: %s\n", error.what());
		return 1;
	}
}
