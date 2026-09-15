// P2a W4:Prefab 子树实例化(深拷贝 + UUID 重发 + 层级重建 + 挂到目标父节点)。
#include "World/Core/WorldContext.h"
#include "World/Gameplay/Prefab.h"
#include "World/Scene/Components.h"
#include "World/Scene/Hierarchy.h"
#include "World/Scene/Scene.h"

#include <cstdio>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <algorithm>

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

		// 6. 资产往返:.wprefab 保存后能从文件实例化(与 .wd 同格式,复用同一序列化器)。
		{
			const std::filesystem::path prefabPath =
				std::filesystem::temp_directory_path() / "worldengine-prefab-test.wprefab";
			std::string error;
			CHECK(SaveFromScene(source, Entity(&source, root), prefabPath, &error));
			CHECK(error.empty());
			CHECK(std::filesystem::exists(prefabPath));

			Scene loaded(context);
			auto& loadedRegistry = loaded.GetRegistry();
			const entt::entity host2 = loadedRegistry.create();
			loadedRegistry.emplace<UUIDComponent>(host2, UUID());
			loadedRegistry.emplace<TagComponent>(host2, "Host2");
			loadedRegistry.emplace<TransformComponent>(host2, TransformComponent(glm::vec3(-5.0f, 0.0f, 0.0f)));

			const PrefabInstanceResult fromFile = InstantiateFromFile(prefabPath, loaded, host2);
			CHECK(fromFile.IsValid());
			CHECK(fromFile.EntityCount == 2);

			const entt::entity fileRoot = static_cast<entt::entity>(fromFile.Root);
			CHECK(loadedRegistry.get<TagComponent>(fileRoot).Tag == "Prefab Root");
			CHECK(loadedRegistry.get<HierarchyComponent>(fileRoot).Parent == host2);
			const auto& fileChildren = loadedRegistry.get<HierarchyComponent>(fileRoot).Children;
			CHECK(fileChildren.size() == 1);
			CHECK(loadedRegistry.get<TagComponent>(fileChildren[0]).Tag == "Prefab Child");
			// Host2(-5,0,0) + Root(1,2,3) + Child(0,1,0) = (-4,3,3)
			const glm::vec3 fileChildWorld =
				glm::vec3(loadedRegistry.get<WorldTransformComponent>(fileChildren[0]).Matrix[3]);
			CHECK(glm::length(fileChildWorld - glm::vec3(-4.0f, 3.0f, 3.0f)) < 1e-3f);

			// 缺失文件与非法来源都应安全失败(不抛异常)。
			CHECK(!InstantiateFromFile(std::filesystem::temp_directory_path() / "missing.wprefab", loaded).IsValid());
			std::filesystem::remove(prefabPath);
		}
		// 7. 覆盖记录与回滚:改动登记 -> HasOverride/计数 -> RevertInstance 恢复 prefab 原值。
		{
			const std::filesystem::path prefabPath =
				std::filesystem::temp_directory_path() / "worldengine-prefab-override.wprefab";
			std::string error;
			CHECK(SaveFromScene(source, Entity(&source, root), prefabPath, &error));

			Scene target(context);
			const PrefabInstanceResult instance = InstantiateFromFile(prefabPath, target);
			CHECK(instance.IsValid());
			const entt::entity instanceRoot = static_cast<entt::entity>(instance.Root);
			auto& targetRegistry = target.GetRegistry();

			PrefabInstanceRecord record;
			record.PrefabPath = prefabPath.string();
			record.Root = instanceRoot;
			CHECK(record.IsValid());
			CHECK(!HasOverride(record, instanceRoot));

			// 模拟属性面板改动:改 Tag 与 MeshRenderer 颜色并登记覆盖。
			targetRegistry.get<TagComponent>(instanceRoot).Tag = "Edited Tag";
			targetRegistry.get<MeshRendererComponent>(instanceRoot).Color = { 1.0f, 0.0f, 0.0f, 1.0f };
			MarkOverride(record, instanceRoot, "TagComponent.Tag");
			MarkOverride(record, instanceRoot, "MeshRendererComponent.Color");
			MarkOverride(record, instanceRoot, "TagComponent.Tag");   // 重复登记不叠加
			CHECK(HasOverride(record, instanceRoot));
			CHECK(GetOverrideCount(record) == 2);

			// 回滚:Tag 与颜色回到 prefab 原值,覆盖记录被清空。
			CHECK(RevertInstance(record, target));
			CHECK(targetRegistry.get<TagComponent>(instanceRoot).Tag == "Prefab Root");
			CHECK(std::fabs(targetRegistry.get<MeshRendererComponent>(instanceRoot).Color.z - 0.9f) < 1e-5f);
			CHECK(!HasOverride(record, instanceRoot));
			CHECK(GetOverrideCount(record) == 0);

			// 无来源的实例记录必须安全失败。
			PrefabInstanceRecord empty;
			CHECK(!RevertInstance(empty, target));
			std::filesystem::remove(prefabPath);
		}
		std::printf("World.Prefab: all checks passed\n");
		return 0;
	}
	catch (const std::exception& error)
	{
		std::fprintf(stderr, "World.Prefab FAILED: %s\n", error.what());
		return 1;
	}
}
