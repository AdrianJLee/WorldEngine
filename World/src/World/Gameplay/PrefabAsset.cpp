#include "wldpch.h"
#include "World/Gameplay/Prefab.h"

#include "World/Core/Log.h"
#include "World/Scene/Components.h"
#include "World/Scene/SceneSerializer.h"

#include <memory>

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
		if (!std::filesystem::exists(path))
		{
			WLD_CORE_ERROR("Prefab::InstantiateFromFile: file not found: {0}", path.generic_string());
			return result;
		}

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
}
