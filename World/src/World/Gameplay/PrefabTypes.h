#pragma once

// P4-U13b:Prefab 实例记录的纯数据类型头。
//
// 为什么单独一个头:Scene 需要持有实例注册表(std::vector<PrefabInstanceRecord>),
// 而 Prefab.h 依赖 Scene/Entity(实例化函数签名要用 Scene&/Entity);把记录类型
// 拆到这里后,Scene.h 只包含本头,不会形成 Prefab.h ⇄ Scene.h 的循环包含。
// 本头只能依赖标准库与 entt,不得包含任何 World 场景头。

#include <entt.hpp>

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace World::Gameplay
{
	// W4-3:实例覆盖记录。
	// 由"改动发生处"登记(属性面板/脚本),而不是靠全量 diff 反推——这样覆盖信息永远与真实编辑一致。
	struct PrefabInstanceRecord
	{
		std::string PrefabPath;                 // 来源 prefab(空 = 非 prefab 实例)
		entt::entity Root = entt::null;         // 实例子树根
		// 实体 -> 被覆盖的字段名(如 "TransformComponent.Location");空集合表示该实体无覆盖。
		std::unordered_map<uint32_t, std::vector<std::string>> Overrides;

		bool IsValid() const { return Root != entt::null; }
	};
}
