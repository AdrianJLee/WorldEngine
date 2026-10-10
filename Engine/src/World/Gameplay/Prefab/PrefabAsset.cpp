#include "wldpch.h"
#include "World/Gameplay/Prefab/Prefab.h"

#include "World/Core/Log.h"
#include "World/Scene/Components.h"
#include "World/Scene/SceneSerializer.h"

#include <memory>
#include <algorithm>

namespace World::Gameplay
{
	namespace
	{
		// prefab 文件里"没有父节点"的实体就是实例根(导出时只会有一个)。
		entt::entity FindPrefabRoot(const Scene& scene)
		{
			const entt::registry& registry = scene.GetRegistry();
			for (const auto entity : registry.view<UUIDComponent>())
			{
				const auto* hierarchy = registry.try_get<HierarchyComponent>(entity);
				if (!hierarchy || hierarchy->Parent == entt::null || !registry.valid(hierarchy->Parent))
					return entity;
			}
			return entt::null;
		}
	}

	bool SaveFromScene(Scene& source, Entity root, const std::filesystem::path& path,
		std::string* error)
	{
		if (!root.IsValid() || root.GetScene() != &source)
		{
			if (error) *error = "prefab root is not valid in the given scene";
			return false;
		}

		// 临时场景承载子树:实例化内核会重新生成 UUID,满足序列化器"实体必须有 UUID"的约定。
		const Ref<Scene> staging = CreateRef<Scene>(source.GetContext());
		const PrefabInstanceResult staged = Instantiate(source, root, *staging, entt::null);
		if (!staged.IsValid())
		{
			if (error) *error = "failed to stage prefab subtree";
			return false;
		}

		// 契约(而非巧合):预制体**不携带世界设置**。抽取虽然走新建暂存场景、
		// 其 WorldSettings 恰为默认,但那是副产品;这里显式清空,免得将来换成
		// "从现有场景另存"时把重力覆盖等世界级设置带进 .wprefab。
		staging->GetWorldSettings() = Scene::WorldSettings {};

		SceneSerializer serializer(staging);
		if (!serializer.Serialize(path.string()))
		{
			if (error) *error = serializer.GetLastError().empty()
				? ("failed to write prefab: " + path.string()) : serializer.GetLastError();
			return false;
		}
		if (error) error->clear();
		WLD_CORE_INFO("Prefab saved: {0} ({1} entities)", path.generic_string(), staged.EntityCount);
		return true;
	}

	PrefabInstanceResult InstantiateFromFile(const std::filesystem::path& path,
		Scene& destination, entt::entity parent)
	{
		PrefabInstanceResult result;
		// 不做 std::filesystem::exists 预检:内容根下的相对路径(如 "prefabs/X.wprefab")
		// 由引擎的 VFS/磁盘解析链路处理,这里只以反序列化结果为准。
		const Ref<Scene> staging = CreateRef<Scene>(destination.GetContext());
		SceneSerializer serializer(staging);
		if (!serializer.Deserialize(path.string()))
		{
			WLD_CORE_ERROR("Prefab::InstantiateFromFile: {0} ({1})", path.generic_string(),
				serializer.GetLastError());
			return result;
		}

		const entt::entity prefabRoot = FindPrefabRoot(*staging);
		if (prefabRoot == entt::null)
		{
			WLD_CORE_ERROR("Prefab::InstantiateFromFile: no root entity in {0}", path.generic_string());
			return result;
		}

		result = Instantiate(*staging, Entity(staging.get(), prefabRoot), destination, parent);
		if (result.IsValid())
			WLD_CORE_INFO("Prefab instantiated: {0} ({1} entities)", path.generic_string(), result.EntityCount);
		return result;
	}

	PrefabInstanceResult InstantiateFromFileWithIds(const std::filesystem::path& path,
		Scene& destination, entt::entity parent, const std::vector<uint64_t>& entityUuids)
	{
		PrefabInstanceResult result = InstantiateFromFile(path, destination, parent);
		if (!result.IsValid())
			return result;
		if (entityUuids.empty())
			return result;

		// 与 Instantiate 内部同一份先序口径:下标 i 的实体拿 entityUuids[i]。
		// 长度不足 ⇒ 多出来的实体保持新生成的 UUID(告警而不是失败:结构漂移的实例仍应能打开)。
		std::vector<entt::entity> subtree;
		FlattenInstanceSubtree(destination.GetRegistry(), static_cast<entt::entity>(result.Root), subtree);
		const size_t count = std::min(subtree.size(), entityUuids.size());
		for (size_t i = 0; i < count; ++i)
			destination.GetRegistry().emplace_or_replace<UUIDComponent>(subtree[i], UUID(entityUuids[i]));
		if (subtree.size() != entityUuids.size())
			WLD_CORE_WARN("Prefab::InstantiateFromFileWithIds: '{0}' has {1} entities but {2} recorded id(s); "
				"the extra entities keep freshly generated ids",
				path.generic_string(), subtree.size(), entityUuids.size());
		return result;
	}
}
