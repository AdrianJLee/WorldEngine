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
		// 见下方 SparsePlacement 说明:存盘时是否按"模板引用 + 差异实体"的稀疏形态写。
		bool SparsePlacement = false;

		bool IsValid() const { return Root != entt::null; }
	};

	// Level 2.0 §16.10:实例的**稀疏放置**标记。
	//
	// 为什么需要它:一个预制体实例被摆 200 次时,"每个实体各写一份完整 `.entity`"会让仓库
	// 随摆放量线性膨胀,而且**失去传播**(改模板不生效)。稀疏形态只写实例根(承载模板引用
	// 与覆盖字段)与**与模板不一致**的实体,其余后代由加载端"实例化模板"补齐。
	//
	// 因此这个标记是**持久化状态**,不是运行时状态:由捕获侧(LevelPackage::CaptureFromScene)
	// 在确认"模板可解析且子树与模板同构"后置位,写盘时据此带上 `Entities:` 重建映射。
	// 置位只影响存盘形状;内存里的实例与稠密形态逐字段相同(Revert / Unpack / 覆盖 UI 不受影响)。

	// Level 2.0:实体级 `.entity` 里 `Prefab:` 块的**待解析**形态。
	//
	// 为什么不直接建成 PrefabInstanceRecord:读文件时 entt 句柄一个都不存在,盘上只有
	// 实体 UUID;而实例的根与子实体可能分散在不同 `.entity` 文件(关卡)甚至不同流送块
	// (运行时),读到一半无法反查句柄。所以先按 UUID 记账,等该批实体全部导入后由
	// `SceneSerializer::FinalizeEntityImport` 一次性解析成句柄记录。
	struct PendingPrefabInstanceRecord
	{
		std::string PrefabPath;
		uint64_t RootUuid = 0;
		std::vector<std::pair<uint64_t, std::vector<std::string>>> Overrides;  // (实体 UUID, 被覆盖字段名)
		// 稀疏形态的重建映射:模板实体在**先序**下的 UUID(下标 = 模板先序下标)。
		// 非空 ⇒ 该实例以"实例化模板 + 覆盖"重建,而不是逐条读实体文档。空 = 稠密(旧行为)。
		std::vector<uint64_t> EntityUuids;
	};
}
