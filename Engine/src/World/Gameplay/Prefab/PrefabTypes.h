#pragma once

// P4-U13b & Prefab 2.0:Prefab 实例记录的纯数据类型头。
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
	// Prefab 2.0:模板局部实体标识(模板内部实体的 UUID 固化值)。
	// 无论模板如何增删子节点、调整顺序、重命名，只要实体未在模板中被彻底删除，其 LocalId 永恒不变。
	using PrefabLocalId = uint64_t;

	// Prefab 2.0:层级重挂记录(将模板内子实体重新挂接至其他父实体)。
	struct PrefabReparentRecord
	{
		PrefabLocalId TargetLocalId = 0;
		uint64_t NewParentUuid = 0;
	};

	// Prefab 2.0:结构覆写集(墓碑删除 / 重挂 / 新增外来实体挂载)。
	struct PrefabStructuralOverrides
	{
		std::vector<PrefabLocalId> Tombstones;
		std::vector<PrefabReparentRecord> Reparented;
		std::vector<uint64_t> AddedEntities;
	};

	// W4-3 & Prefab 2.0:实例覆盖与映射记录。
	// 由"改动发生处"登记(属性面板/脚本)或由保存期逆向 Diffing 提取。
	struct PrefabInstanceRecord
	{
		uint32_t Version = 1;                   // 1 = 旧先序数组, 2 = Prefab 2.0 工业级 LocalId 字典
		std::string PrefabPath;                 // 来源 prefab(空 = 非 prefab 实例)
		entt::entity Root = entt::null;         // 实例子树根
		// 实体 -> 被覆盖的字段名(如 "TransformComponent.Location");空集合表示该实体无覆盖。
		std::unordered_map<uint32_t, std::vector<std::string>> Overrides;
		// 见下方 SparsePlacement 说明:存盘时是否按"模板引用 + 差异"的稀疏形态写。
		bool SparsePlacement = false;

		// Prefab 2.0 规格扩展:
		// 模板 LocalId -> 关卡实例 UUID
		std::unordered_map<PrefabLocalId, uint64_t> EntityMapping;
		// 内联属性差异 YAML 片段: LocalId -> (ComponentName -> ComponentYamlFields)
		std::unordered_map<PrefabLocalId, std::unordered_map<std::string, std::string>> PropertyOverrides;
		// 结构覆写(墓碑 / 重挂 / 新增挂载)
		PrefabStructuralOverrides StructuralOverrides;

		bool IsValid() const { return Root != entt::null; }
	};

	// Level 2.0 & Prefab 2.0:实体级 .entity 里 Prefab: 块的待解析形态。
	struct PendingPrefabInstanceRecord
	{
		uint32_t Version = 1;
		std::string PrefabPath;
		uint64_t RootUuid = 0;
		std::vector<std::pair<uint64_t, std::vector<std::string>>> Overrides;  // (实体 UUID, 被覆盖字段名)
		// 稀疏形态的重建映射(V1):模板实体在**先序**下的 UUID(下标 = 模板先序下标)。
		std::vector<uint64_t> EntityUuids;

		// V2 规格:
		std::unordered_map<PrefabLocalId, uint64_t> EntityMapping;
		std::unordered_map<PrefabLocalId, std::unordered_map<std::string, std::string>> PropertyOverrides;
		PrefabStructuralOverrides StructuralOverrides;
	};
}
