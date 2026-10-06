#pragma once

#include "World/Core/Export.h"
#include "World/Profiling/TraceWriter.h"

#include <string>

namespace World::Profiling
{
	// ---------------------------------------------------------------------------
	// 采集后端接口(P1「混合」方案的抽象层)。
	//
	// 为什么要有它:插桩点(宏/作用域/计数器)是**长期资产**,采集后端是**可替换实现**。
	// 把两者绑死,将来接成熟后端(如 Tracy)就要回头改所有插桩点 —— 那正是这次重构
	// 想避免的样子。有了这一层:**换后端 = 换一个 sink,插桩点一行不动**。
	//
	// 契约:
	//   * `Write` 只在**采集结束之后**被调用(低频、可分配),不在帧循环里;
	//   * 允许失败并给可读原因(返回 false + error),不允许静默丢数据;
	//   * 多线程调用由调用方串行化(Telemetry 保证单采集)。
	// ---------------------------------------------------------------------------
	class WLD_API ITraceSink
	{
	public:
		virtual ~ITraceSink() = default;
		virtual const char* Name() const = 0;
		virtual bool Write(const std::string& path, const TraceWriteRequest& request, std::string* error) = 0;
	};

	// 进程内唯一的活动 sink。传 nullptr = 回到内置的 Chrome Trace 后端。
	WLD_API void SetTraceSink(ITraceSink* sink);
	WLD_API ITraceSink* GetTraceSink();
	WLD_API const char* ActiveTraceSinkName();
}
