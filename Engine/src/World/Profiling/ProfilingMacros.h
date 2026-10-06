#pragma once

// =============================================================================
// 插桩宏。
//
// 编译期开关 `WLD_TELEMETRY_ENABLED`:
//   * 默认**开启**(采集本身由运行期开关门控;关闭时的代价 = 一次 relaxed 原子读 + 分支);
//   * 定义 `WLD_TELEMETRY_ENABLED=0` 可编译期整体剥离 —— 发行版打包用它做到**零成本**。
//
// 名字必须指向**静态字面量**(见 TraceEvents.h R2):事件里只存指针,不存字符串、不驻留 id,
// 因此热路径没有锁、没有分配。写 `WLD_TRACE_SCOPE(variableName)` 会破坏这一前提。
// =============================================================================

#include "World/Profiling/MemoryTrack.h"
#include "World/Profiling/Telemetry.h"

#define WLD_TELEMETRY_CONCAT_INNER(a, b) a##b
#define WLD_TELEMETRY_CONCAT(a, b) WLD_TELEMETRY_CONCAT_INNER(a, b)

#ifndef WLD_TELEMETRY_ENABLED
#define WLD_TELEMETRY_ENABLED 1
#endif

#if WLD_TELEMETRY_ENABLED

// 作用域(推荐):给一段代码计时,名字是本作用域内唯一的标识符字面量。
#define WLD_TRACE_SCOPE(name) \
	::World::Profiling::TraceScope WLD_TELEMETRY_CONCAT(_wldTraceScope_, __COUNTER__)(name)

// 函数作用域:名字取 __FUNCTION__(MSVC 下是静态字面量)。
#define WLD_TRACE_FUNCTION() WLD_TRACE_SCOPE(__FUNCTION__)

// 计数器:采集窗口内记录一个瞬时值(内存字节、draw call、任务数…)。
#define WLD_TRACE_COUNTER(name, value) \
	::World::Profiling::Telemetry::SetCounter(name, static_cast<int64_t>(value))

// 线程名(并行系统建议设置一次,便于 trace 里区分轨道)。
#define WLD_TRACE_THREAD_NAME(name) ::World::Profiling::Telemetry::SetThreadName(name)

// 内存标签作用域:作用域内的全局堆分配归到这个标签下(RAII,零分配)。
// 名字必须是**静态字面量**(见 TraceEvents.h R2):事件只存指针,不受控字符串会悬垂。
#define WLD_MEM_TAG(name) \
	::World::Profiling::MemoryTrack::MemoryTagScope WLD_TELEMETRY_CONCAT(_wldMemTag_, __COUNTER__)(name)

#else // WLD_TELEMETRY_ENABLED == 0 —— 整体剥离,零成本

#define WLD_TRACE_SCOPE(name) ((void)0)
#define WLD_TRACE_FUNCTION() ((void)0)
#define WLD_TRACE_COUNTER(name, value) ((void)0)
#define WLD_TRACE_THREAD_NAME(name) ((void)0)
#define WLD_MEM_TAG(name) ((void)0)

#endif
