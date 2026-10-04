#include "wldpch.h"

#include "World/Asset/ScenePrefetch.h"

#include "World/Asset/AsyncLoader.h"
#include "World/Core/WorldContext.h"
#include "World/Scene/Components.h"
#include "World/Scene/Scene.h"

namespace World
{
	namespace
	{
		// 一个 AssetRef 是否值得请求:有路径才算"能按路径读"。身份-only(路径已失效)的引用
		// 需要先过 AssetCatalog 找回 —— 那是解析路径的事,不该在这里复制一份目录逻辑。
		bool HasResolvablePath(const AssetRef& asset) { return asset.HasPath(); }
	}

	ScenePrefetchResult PrefetchSceneAssets(WorldContext& context, const Scene& scene)
	{
		ScenePrefetchResult result;
		if (!context.Resources().Has<AsyncLoader>())
			context.Resources().Emplace<AsyncLoader>();
		AsyncLoader& loader = context.Resources().Get<AsyncLoader>();

		const auto request = [&](const AssetRef& asset)
		{
			if (!HasResolvablePath(asset))
				return;
			bool startedNew = false;
			loader.RequestMesh(asset.Path, &startedNew);
			if (startedNew)
				++result.Requested;
			else
				++result.AlreadyKnown;
		};

		const entt::registry& registry = scene.GetRegistry();
		for (const entt::entity entity : registry.view<MeshRendererComponent>())
			request(registry.get<MeshRendererComponent>(entity).Mesh);
		for (const entt::entity entity : registry.view<SkinnedMeshRendererComponent>())
			request(registry.get<SkinnedMeshRendererComponent>(entity).Mesh);
		for (const entt::entity entity : registry.view<MeshCollider3DComponent>())
			request(registry.get<MeshCollider3DComponent>(entity).Mesh);
		for (const entt::entity entity : registry.view<SpriteComponent>())
			request(registry.get<SpriteComponent>(entity).Texture);

		return result;
	}
}
