#pragma once

// CPPT-2(T5b,2026-09-27 用户裁决 D-B):模块(`Game.dll`)级热重载编排。
//
// 语义(与 plan.md §6.1/§8.5 的验收口径一致):
//   * 重载单位 = 整个 Game 模块;重载后同一实体的 C++ 脚本是**新实例**:
//     旧实例收一次 OnDestroy(若 CreateEntered)→ 卸载旧 DLL → 加载/注册新 DLL →
//     新实例 State=Pending,由 Scene 既有 pending 机制在下一安全点 OnCreate;
//   * 迁移 = 配置态(ScriptName + Properties):同名同类型保旧值,类型变化/字段删除/新增字段
//     各出一条可读诊断(引擎侧在 Scene::RestoreNativeScriptInstances 用
//     HotReload::DescribeScriptFieldMigration 收集进本结果的 Diagnostics;复用同一份规则,
//     编辑器只负责显示诊断计数/文本);
//   * 不迁移 = 实例 C++ 成员可变状态、事件/计时器订阅、以实例指针为键的外部注册 ——
//     它们随旧实例销毁,由新实例 OnCreate 重建(不假装迁移);Luau 实例不受影响;
//   * 回滚副本 = 卸载前把当前 DLL 拷成同目录 `Game.rollback-<abi>.dll`(不入库,只留最近一份);
//     Load 段失败(ABI/入口/注册)→ 自动加载回滚副本重新注册 + 同一套属性迁移,
//     结果里 RolledBack=true 且 Diagnostics 可读;
//   * 崩溃隔离边界(不夸大):新代码第一次执行发生在旧实例已销毁、旧注册表已清空、
//     旧 DLL 已卸载之后 ⇒ 硬崩溃不会留下半注册/半迁移的持久化产物(重载不写项目/场景文件);
//     不做 SEH 与进程沙箱 —— 硬崩溃仍是进程终止。
//
// 本头文件属于 WorldRuntime.dll 内部公共面;不改 WeModule/Schema.h 的跨 DLL 布局(ABI 冻结)。

#include "World/Modules/ModuleManager.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace World
{
	class WorldContext;
	class Scene;
}

namespace World::Modules
{
	// 编辑器入口/AI module.* 命令/探针断言的数据面(扁平、可读、无跨 DLL 结构)。
	struct GameModuleReloadResult
	{
		ModuleManager::Status Status = ModuleManager::Status::Ok;
		bool RolledBack = false;            // Load 失败后已回到回滚副本
		bool ModuleUnloaded = false;        // Unload 段完成:当前没有 Game 模块(文件可重编)
		uint32_t AbiVersion = 0;            // 新模块实际报告的 ABI 值(等值门证据)
		std::size_t InstancesDrained = 0;   // 收到 OnDestroy 的旧实例数
		std::size_t InstancesRestored = 0;  // 置回 Pending 的组件数
		std::string Message;                // 可读(含路径/阶段)
		std::vector<std::string> Diagnostics;   // 迁移/回滚诊断(稳定顺序)
		std::string ModulePath;             // 本次加载/重载目标 DLL
		std::string RollbackPath;           // 回滚副本路径(存在时)
	};

	class GameModuleReload
	{
	public:
		// 模块 id 与 Game/src/GameAPI.cpp 的 WeModule::Id 一致。
		static constexpr const char* GameModuleId = "game";

		// 安全点判定 = scene->CanApplyScriptReload()(无回调/结构提交/写窗口/停止中);
		// scene 为空 = 编辑态无场景(只做模块交换,不动实例)。
		static bool Unload(WorldContext& context, Scene* scene, GameModuleReloadResult* result = nullptr);
		// 当前必须没有 Game 模块;失败(ABI/入口/注册)自动回滚到回滚副本。
		static bool Load(WorldContext& context, Scene* scene, GameModuleReloadResult* result = nullptr);
		// = Unload + Load(一步式;编辑器工具栏/AI module.reload 用这条)。
		static bool Reload(WorldContext& context, Scene* scene, GameModuleReloadResult* result = nullptr);
		static bool IsUnloaded(const WorldContext& context);
	};
}
