#pragma once

// ============================================================================
// PLUG-T6:插件热重载(L3)的数据面。
//
// 两段式(与 Modules/GameModuleReload.h 同款,原因是 Windows 会锁住已加载 DLL):
//   第一段 `PluginManager::UnloadForReload`:
//     * 按"实体 UUID + 组件 id + 字段 id"把目标场景里的插件组件实例快照进管理器
//       (字段值走 Schema 的 Get,快照在卸载前完成);
//     * 从场景里移除这些实例(否则 T2c 的活实例门会拒绝卸载);
//     * 卸载插件(账本回收 + Unregister + FreeLibrary)⇒ DLL 文件锁释放,外部可重编。
//   第二段 `PluginManager::LoadForReload`:
//     * 重读 `plugin.we.yaml`(拾取版本/声明变化)并载入新 DLL;
//     * 按字段 id(回退字段名)把快照写回(新 schema 的访问器;类型不符 = 诊断 + 跳过);
//     * 任一步失败 ⇒ 载入卸载前拷贝的 `.rollback-<abi>.dll` 副本(旧 DLL)并写回原状态,
//       结果 RolledBack=true + 可读诊断。
//
// 语义边界(不夸大):
//   * 快照只覆盖**目标场景**里该插件的 blob 组件实例;同一 WorldContext 的其它活场景
//     还有实例时第一段直接干净拒绝(HasLiveInstances,T2c 语义不变);
//   * Play/Simulate 场景的结构写被 Scene 门禁拒绝 ⇒ 两段都在编辑态安全点(停 Play)执行;
//   * 快照在管理器内存里:未完成第二段就退出编辑器 = 那些实例数据只在内存里(与
//     GameModuleReload 的"重载不落盘"同一风险等级;管理器析构/UnloadAll 会记 ERROR)。
// ============================================================================

#include "World/Core/UUID.h"
#include "World/Schema/Schema.h"

#include <cstdint>
#include <string>
#include <vector>

namespace World::Plugins
{
	// 一个字段的快照:按 FieldId(字段名 FNV-1a)匹配;Name 作为回退与可读诊断。
	struct PluginComponentFieldSnapshot
	{
		uint64_t FieldId = 0;
		std::string Name;
		World::Schema::Value Value;
	};

	// 一个组件实例的快照:实体 UUID 是跨 DLL 重载的稳定身份;ComponentId 只是快照时的
	// 存储 id(新 DLL 可能落在另一个档位/槽位,恢复以新 schema 为准)。
	struct PluginComponentInstanceSnapshot
	{
		World::UUID EntityId;
		uint32_t ComponentId = 0;
		std::string ComponentType;   // 类型全名(诊断用)
		std::vector<PluginComponentFieldSnapshot> Fields;
	};

	// 一次 Begin/End 调用的可观测结果(AI plugin.reload 的 JSON 字段来源)。
	struct PluginReloadResult
	{
		enum class Phase : uint8_t
		{
			Unloaded = 0,   // 第一段完成:插件已卸载,DLL 可重编
			Loaded = 1,     // 第二段完成:新 DLL 已加载,快照已写回
			RolledBack = 2, // 第二段失败但已回滚到旧 DLL(状态保留)
			Failed = 3,     // 失败且没有可用回滚(诊断可读)
		};

		Phase ResultPhase = Phase::Failed;
		bool Ok = false;
		bool RolledBack = false;
		bool Unloaded = false;
		uint32_t InstancesSnapshotted = 0;
		uint32_t InstancesRestored = 0;
		uint32_t InstancesSkipped = 0;   // 实体消失 / 字段缺失 / 类型不符
		std::string PluginId;
		std::string Message;
		std::vector<std::string> Diagnostics;   // 稳定顺序(快照 → 加载 → 回放)
		std::string LibraryPath;    // 本次目标 DLL(重编的那份)
		std::string RollbackPath;   // 回滚副本(存在时)
	};

	inline const char* PluginReloadPhaseName(PluginReloadResult::Phase phase)
	{
		switch (phase)
		{
			case PluginReloadResult::Phase::Unloaded: return "unloaded";
			case PluginReloadResult::Phase::Loaded: return "loaded";
			case PluginReloadResult::Phase::RolledBack: return "rolled-back";
			case PluginReloadResult::Phase::Failed: return "failed";
		}
		return "unknown";
	}
}
