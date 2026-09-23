#pragma once

#include "World/Core/Export.h"
#include "World/Scene/Components.h"

#include <cstdint>

namespace World
{
	// 层级操作(全部在场景所有者线程调用):
	//  - SetParent 做环路检测(把 child 挂到自己的后代上会被拒绝);
	//  - ClearParent 恢复为根;
	//  - UpdateWorldTransforms 先序遍历整片森林,按父矩阵求解世界矩阵,
	//    并做一次浅层循环保护(非法环只警告并跳过,不会死循环)。
	namespace Hierarchy
	{
		WLD_API bool SetParent(entt::registry& registry, entt::entity child, entt::entity parent);
		// 设父并把 child 插入 parent 的子节点列表指定位置(同级重排用);index 会被夹到合法范围。
		WLD_API bool InsertChild(entt::registry& registry, entt::entity child, entt::entity parent,
			size_t index);
		WLD_API int32_t GetChildIndex(const entt::registry& registry, entt::entity child);
		WLD_API void ClearParent(entt::registry& registry, entt::entity child);
		WLD_API bool IsAncestorOf(const entt::registry& registry, entt::entity ancestor, entt::entity node);
		WLD_API uint32_t GetDepth(const entt::registry& registry, entt::entity node);
		// 返回参与求解的实体数。
		WLD_API uint32_t UpdateWorldTransforms(entt::registry& registry);
	}
}
