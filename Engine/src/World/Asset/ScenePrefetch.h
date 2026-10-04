#pragma once

#include "World/Core/Export.h"

#include <cstddef>

namespace World
{
	class Scene;
	class WorldContext;

	// ---------------------------------------------------------------------------
	// 场景资产预取(T5c,2026-10-04)。
	//
	// "这个场景需要哪些资产"是**资产层**的问题,不是渲染器或编辑器的问题 —— 所以走查逻辑
	// 只此一处:组件表变了,只有这里要跟着变。
	//
	// 语义:
	//   * 只**登记异步请求**,不阻塞、不返回资产(调用方随后按正常路径解析,命中即用);
	//   * 幂等:同一场景重复调用只是续接未完成的请求;
	//   * 覆盖组件:MeshRenderer.Mesh、SkinnedMeshRenderer.Mesh、Sprite.Texture、
	//     MeshCollider3D.Mesh(材质是编辑器文档模型,不在此预取 —— 见 AssetRegistry 的边界说明)。
	// ---------------------------------------------------------------------------
	struct ScenePrefetchResult
	{
		std::size_t Requested = 0;   // 本次新登记的路径数
		std::size_t AlreadyKnown = 0; // 已在飞/已就绪/已失败而跳过的路径数
	};

	// 走查场景并登记异步加载;需要 WorldContext 上的 AsyncLoader(没有就建一个)。
	WLD_API ScenePrefetchResult PrefetchSceneAssets(WorldContext& context, const Scene& scene);
}
