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
//   * 抓得住(2026-10-05 起):等大小的成员**重排**、成员换成等大小的另一类型、成员位移 ——
//     由"布局指纹"(关键成员的 offsetof + 尺寸混合哈希)发现,不再只靠 sizeof;
//   * 仍抓不住:函数/纯虚表项增删等与内存布局无关的改动,以及只改了某个成员**内部**
//     布局、而该成员自身位置与尺寸都不变的情况;
//   * 也不替代 Game.dll 的 `WE_MODULE_ABI_VERSION` 等值门(那是模块 ABI,另一条通道)。
namespace World

{
	// 布局指纹:成员偏移 + 尺寸的混合哈希(FNV-1a)。用途见文件头"覆盖范围"——
	// `sizeof` 只能抓住"变大/变小",抓不住**等大小的成员重排**(把两个同尺寸成员换位,
	// 或把成员换成等大小的另一种类型)。指纹把"关键成员的偏移"也钉进契约,于是:
	//   * 增删成员、改成员类型导致任何偏移位移 ⇒ 指纹变化(与 sizeof 等价或更强);
	//   * 等大小重排 ⇒ **只**有指纹能发现(这正是加它的理由)。
	// 有意不覆盖:只在成员**内部**深层的改动(如某容器的节点布局)——那属实现细节,
	// 不属于宿主直读内存的契约面。
	namespace LayoutHash
	{
		constexpr uint64_t kOffsetBasis = 1469598103934665603ull;
		constexpr uint64_t kPrime = 1099511628211ull;

		constexpr uint64_t Mix(uint64_t hash, uint64_t value)
		{
			return (hash ^ value) * kPrime;
		}
	}
	// hostSceneSize / hostWorldContextSize = 宿主 TU 的 sizeof(...);
	// hostSceneFingerprint / hostWorldContextFingerprint = 宿主 TU 算出的成员偏移指纹
	//   (Scene::LayoutFingerprint() / WorldContext::LayoutFingerprint());
	// hostModuleAbiVersion = 宿主的 World::Modules::WE_MODULE_ABI_VERSION。
	// 返回 true = 两侧一致;false = 世代不一致(宿主必须停下)。
	WLD_API bool RuntimeLayoutMatches(uint64_t hostSceneSize, uint64_t hostWorldContextSize,
		uint64_t hostSceneFingerprint, uint64_t hostWorldContextFingerprint,
		uint32_t hostModuleAbiVersion);
}

// 宿主侧取值宏:sizeof 在**调用方 TU** 展开(宏不会被 DLL 的导出符号"顶替"),
// 所以传进去的一定是宿主自己看到的布局。需要 Scene / WorldContext / WeModule.h 的完整定义。
#define WE_RUNTIME_LAYOUT_MATCHES() \
	::World::RuntimeLayoutMatches(sizeof(::World::Scene), sizeof(::World::WorldContext), \
		::World::Scene::LayoutFingerprint(), ::World::WorldContext::LayoutFingerprint(), \
		::World::Modules::WE_MODULE_ABI_VERSION)
