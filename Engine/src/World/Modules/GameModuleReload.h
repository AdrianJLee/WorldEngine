#pragma once

// CPPT-2(T5b,2026-09-27 用户裁决 D-B):模块(`Game.dll`)级热重载编排。
//
// 语义(PURE-ECS 2026-10-02 起;单实体脚本实例已整体删除):
//   * 重载单位 = 整个 Game 模块;重载后**组件 schema** 与**项目系统**(`AttachProjectSystems`)
//     都换成新 DLL 里的那一份:卸载旧 DLL → 加载/注册新 DLL。组件是纯数据,实体上的字段值
//     由 Scene 自己持有,不随 DLL 交换而丢;系统只在一次运行时内存在 ⇒ 重载后**下次 Play** 才生效;
//   * 安全点 = `scene->CanApplyScriptReload()`(无回调/结构提交/写窗口/停止中) **且**
//     同一 WorldContext 下没有 Running 场景(模块登记过场景系统钩子时)—— 否则运行中的场景
//     会持有指向已卸载 DLL 的帧系统函数指针;拒绝时 Status=NotSafePoint + 可读 Message;
//   * 不迁移 = 系统的 C++ 成员可变状态、事件/计时器订阅、以实例指针为键的外部注册 ——
//     它们随旧 DLL 卸载消失,由新系统在下次 Attach 时重建(不假装迁移);
//   * 回滚副本 = 卸载前把当前 DLL 拷成同目录 `Game.rollback-<abi>.dll`(不入库,只留最近一份);
//     Load 段失败(ABI/入口/注册)→ 自动加载回滚副本重新注册,
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
		// PURE-ECS:单实体脚本实例已删除 ⇒ 这两个计数恒为 0,只为结果结构的前后兼容保留。
		std::size_t InstancesDrained = 0;
		std::size_t InstancesRestored = 0;
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
