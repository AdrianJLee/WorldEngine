#pragma once

#include "World/Core/Export.h"

#include <entt.hpp>
#include <glm/glm.hpp>

#include <cstdint>
#include <vector>

namespace World
{
	// 运行时组件(P2a W3a):层级关系与世界变换缓存。
	// 说明:本阶段先只做运行时数据与求解,序列化与编辑器层级树在 W3b 接入 schema
	// (避免在 schema-compiler 缺位时继续手写生成代码)。
	struct HierarchyComponent
	{
		entt::entity Parent = entt::null;
		std::vector<entt::entity> Children;
		bool InheritTransform = true;
	};

	struct WorldTransformComponent
	{
		glm::mat4 Matrix { 1.0f };
	};

	// 层级操作(全部在场景所有者线程调用):
	//  - SetParent 做环路检测(把 child 挂到自己的后代上会被拒绝);
	//  - ClearParent 恢复为根;
	//  - UpdateWorldTransforms 先序遍历整片森林,按父矩阵求解世界矩阵,
	//    并做一次浅层循环保护(非法环只警告并跳过,不会死循环)。
	namespace Hierarchy
	{
		WLD_API bool SetParent(entt::registry& registry, entt::entity child, entt::entity parent);
		WLD_API void ClearParent(entt::registry& registry, entt::entity child);
		WLD_API bool IsAncestorOf(const entt::registry& registry, entt::entity ancestor, entt::entity node);
		WLD_API uint32_t GetDepth(const entt::registry& registry, entt::entity node);
		// 返回参与求解的实体数。
		WLD_API uint32_t UpdateWorldTransforms(entt::registry& registry);
	}
}
