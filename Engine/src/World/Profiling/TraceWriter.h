#pragma once

#include "World/Profiling/TraceEvents.h"

#include <cstdint>
#include <string>

namespace World::Profiling
{
	// ---------------------------------------------------------------------------
	// Chrome Trace Event JSON 导出。
	//
	// 为什么选这个格式:
	//   * 事实标准 —— Perfetto / chrome://tracing 直接打开,零依赖、零工具链;
	//   * **人和 AI 都能读**:结构简单(事件数组),脚本用 JSON 解析器即可断言;
	//   * 与既有验证纪律吻合(可脚本化、可复现、能进 CI)。
	//
	// 落盘只在**采集结束后**发生(不在帧循环内),因此此处允许分配。
	// ---------------------------------------------------------------------------
	inline constexpr uint32_t kMaxThreads = 64;

	struct TraceWriteRequest
	{
		const TraceEvent* Events = nullptr;
		uint32_t EventCount = 0;
		uint64_t DroppedEvents = 0;              // 写进文件头:>0 说明数据不全
		const TraceMeta* Meta = nullptr;
		uint32_t MetaCount = 0;
		const char* ThreadNames[kMaxThreads] = {}; // 索引 = thread slot
		uint32_t ThreadNameCount = 0;
		uint32_t SchemaVersion = kSchemaVersion;
	};

	// 成功返回 true;失败返回 false 并写 error。
	bool WriteChromeTrace(const std::string& path, const TraceWriteRequest& request, std::string* error);
}
