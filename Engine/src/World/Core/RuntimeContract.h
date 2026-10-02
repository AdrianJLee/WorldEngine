#pragma once

#include "World/Core/Export.h"

#include <cstdint>

// 宿主可执行程序(Editor / Runtime)与 WorldRuntime.dll 的**运行期布局契约**。
//
// 为什么需要它(2026-10-02 实测崩溃):`Scene` 这类引擎类型的数据成员布局**同时**被两侧
// 使用 —— DLL 里分配/维护对象,宿主侧的头文件内联访问器(`IsPhysics3DRunning()`、
// `GetWorldSettings()`、`GetPhysics3DWorld()` …)按**宿主编译时**的成员偏移直接读内存。
// 一旦两侧来自不同世代的头文件(典型成因:改了头文件却只重建了一侧,或增量构建没把
// 所有宿主 TU 重编),宿主就会按**错误的偏移**读对象:实测表现为
// `IsPhysics3DRunning()` 返回 true、`PhysicsDebug` 返回 true,而物理世界根本没启动过,
// 于是拿一个垃圾指针去 `CollectDebugLines()` ⇒ 0xC0000005(崩在 DLL 内部,栈上看不到
// 真实原因)。这类"世代不一致"在增量构建里不会报错,只会崩。
//
// 契约:DLL 侧导出本函数,宿主在 `main` 早期把**自己看到的**尺寸/ABI 传进来核对;
// 不一致就返回 false,宿主必须打出可读原因并**干净退出**(退出码 3),不要继续跑到崩溃。
//
// 覆盖范围与边界(不夸大):
//   * 抓得住:任何改变 `sizeof(Scene)` / `sizeof(WorldContext)` 的改动 —— 增删数据成员、
//     改变成员类型大小都算(2026-10-02 的 M8 删了 3 个 Scene 成员,正是这一类);
//   * 抓不住:不改变尺寸的改动(等大小重排成员、纯虚函数/函数增删),以及只改了某个
//     成员**内部**布局、而外层尺寸恰好不变的情况;
//   * 也不替代 Game.dll 的 `WE_MODULE_ABI_VERSION` 等值门(那是模块 ABI,另一条通道)。
namespace World
{
	// hostSceneSize / hostWorldContextSize = 宿主 TU 的 sizeof(...);
	// hostModuleAbiVersion = 宿主的 World::Modules::WE_MODULE_ABI_VERSION。
	// 返回 true = 两侧一致;false = 世代不一致(宿主必须停下)。
	WLD_API bool RuntimeLayoutMatches(uint64_t hostSceneSize, uint64_t hostWorldContextSize,
		uint32_t hostModuleAbiVersion);
}

// 宿主侧取值宏:sizeof 在**调用方 TU** 展开(宏不会被 DLL 的导出符号"顶替"),
// 所以传进去的一定是宿主自己看到的布局。需要 Scene / WorldContext / WeModule.h 的完整定义。
#define WE_RUNTIME_LAYOUT_MATCHES() \
	::World::RuntimeLayoutMatches(sizeof(::World::Scene), sizeof(::World::WorldContext), \
		::World::Modules::WE_MODULE_ABI_VERSION)
