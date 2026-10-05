#pragma once

// 组件/脚本 schema 与 EnTT/Entity 之间的桥接。
// Schema 核心不依赖 entt/Scene;这里提供 MakeComponentStorage,
// 由 schema-compiler 生成的注册 TU 调用。只做类型转换,不持状态。

#include "World/Core/Timestep.h"
#include "World/Schema/Schema.h"
#include "World/Scene/Components.h"
#include "World/Scene/ComponentLayoutBudget.h"
#include "World/Scene/Entity.h"

#include <entt.hpp>
#include <type_traits>
#include <unordered_map>

namespace World::Schema
{
	inline bool IsTagComponent(uint32_t componentId)
	{
		return componentId == static_cast<uint32_t>(entt::type_id<World::UUIDComponent>().hash());
	}

	inline World::UUID GetEntityUUID(entt::registry& registry, entt::entity entity)
	{
		return registry.get<World::UUIDComponent>(entity).ID;
	}

	template <typename T>
	StorageBinding MakeComponentStorage()
	{
		// 数据布局门禁(标准 docs/dev/performance-and-data-layout.md §4.2 C1)。
		// 编译期棘轮,对**每个** schema 组件自动生效,不需要逐组件手写断言:
		//   1) 组件必须是平凡可拷贝 POD —— 字符串/容器/Ref<T>/虚函数/裸指针句柄一律走
		//      PathId / NameId / InlineString<N>,或移到 Scene 内部表(见 Components.h 注释);
		//   2) 组件必须装进单条 cache line(64B)。确需超过时,在 ComponentLayoutBudget.h
		//      登记豁免并写明理由与复核日期 —— 豁免是白名单,不是宽容度。
		// 豁免(ComponentLayoutExempt<T>)是**白名单**:声明在 ComponentLayoutBudget.h(引擎组件)
		// 或测试夹具自己的头文件里(测试组件)。两条断言都尊重它,但豁免本身必须是
		// "有意识、可见、带理由"的一次改动 —— 不是绕门禁的开关。
		static_assert(ComponentLayoutExempt<T>::value || std::is_trivially_copyable_v<T>,
			"Component must stay trivially copyable (no std::string/container/Ref<T>/virtual/raw handle). "
			"Use PathId/NameId/InlineString<N> or move runtime state into a Scene-internal table. "
			"See docs/dev/performance-and-data-layout.md §4.2 C1.");
		static_assert(ComponentLayoutExempt<T>::value || sizeof(T) <= kComponentCacheLineBytes,
			"Component must fit one 64B cache line. Narrow the fields, split hot/cold data, or register an "
			"exemption (with reason + review date) in Engine/src/World/Scene/ComponentLayoutBudget.h. "
			"See docs/dev/performance-and-data-layout.md §4.2 C1 / §4.6.");

		StorageBinding binding;
		binding.ComponentId = static_cast<uint32_t>(entt::type_id<T>().hash());
		binding.Add = [](void* rawEntity)
		{
			static_cast<World::Entity*>(rawEntity)->AddComponent<T>();
		};
		binding.Copy = IsTagComponent(binding.ComponentId) ? nullptr : +[](void* rawDst, void* rawSrc)
		{
			auto* dst = static_cast<World::Entity*>(rawDst);
			auto* src = static_cast<World::Entity*>(rawSrc);
			dst->AddOrReplaceComponent<T>(src->GetComponent<T>());
		};
		binding.CopyAll = [](void* rawDstRegistry, void* rawSrcRegistry, const void* rawEntityMap)
		{
			auto& dstRegistry = *static_cast<entt::registry*>(rawDstRegistry);
			auto& srcRegistry = *static_cast<entt::registry*>(rawSrcRegistry);
			const auto& entityMap = *static_cast<const std::unordered_map<World::UUID, entt::entity>*>(rawEntityMap);
			for (auto entity : srcRegistry.view<T>())
			{
				const World::UUID entityId = GetEntityUUID(srcRegistry, entity);
				const auto it = entityMap.find(entityId);
				if (it != entityMap.end())
					dstRegistry.emplace_or_replace<T>(it->second, srcRegistry.get<T>(entity));
			}
		};
		return binding;
	}
}
