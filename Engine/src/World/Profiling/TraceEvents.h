#pragma once

// =============================================================================
// 遥测事件定义(schema v1)
//
// 单一事实源:字段语义/单位/缺失含义的**权威表**在本文件;镜像表在
// docs/dev/profiling.md 与私有知识库 knowledge/contracts/telemetry-schema.md。
// 三处必须一致 —— 加字段先补表,再改代码。
//
// 硬规则(违反即门禁失败):
//   R1 事件是**定长 POD**、可平凡拷贝;禁止出现 std::string / 堆持有类型。
//   R2 `Name` 必须指向**静态字面量**(或生命周期覆盖整个采集窗口的稳定字符串)。
//      事件只存指针(8B),不存字符串、不驻留 id —— 免掉锁与"并行 Intern"问题。
//   R3 字段只许**追加**;破坏性改动必须递增 kSchemaVersion。
//   R4 写入路径**零堆分配、零锁**(缓冲在采集开始时一次性分配)。
// =============================================================================

#include <cstdint>
#include <type_traits>

namespace World::Profiling
{
	// 破坏性改动递增。导出文件头会写它,解析方据此判断能否读。
	// v2(2026-10-06):计数器 `frameMs` → `frameUs`、`Scene.CullMs` → `Scene.CullUs`
	// —— 名字与单位对齐(原名字是毫秒、实际存微秒,消费者会差 1000 倍)。
	inline constexpr uint32_t kSchemaVersion = 2;

	inline constexpr uint32_t kInvalidFrameIndex = 0xFFFFFFFFu;
	inline constexpr uint16_t kInvalidThreadSlot = 0xFFFFu;

	enum class TraceEventType : uint8_t
	{
		FrameBegin = 0, // 帧开始。Value 未用
		FrameEnd,       // 帧结束
		ScopeBegin,     // 作用域进入
		ScopeEnd,       // 作用域离开
		Counter,        // 计数器采样。Value = 值
	};

	const char* TraceEventTypeName(TraceEventType type);

	// 40 字节(8 字节对齐),可平凡拷贝。
	// 尺寸断言的意义:事件在采集期按百万级写入,**每多 8 字节就是每百万事件 8MB**;
	// 同时保证 memcpy 导出路径成立。
	struct TraceEvent
	{
		uint64_t Timestamp = 0;              // 微秒,steady_clock 起点(单调,非墙钟)
		int64_t Value = 0;                   // Counter 的采样值;其余类型恒为 0
		const char* Name = nullptr;          // 静态字面量指针,见 R2
		uint32_t FrameIndex = 0;             // 帧序号(自进程启动单调递增)
		uint16_t ThreadSlot = 0;             // 紧凑线程槽位(配 Meta 的 thread_name)
		uint16_t Depth = 0;                  // 作用域嵌套深度,0 = 最外层
		TraceEventType Type = TraceEventType::FrameBegin;
		uint8_t Reserved[7] = {};            // 归零;将来追加字段从尾部取用
	};

	static_assert(std::is_trivially_copyable_v<TraceEvent>,
		"TraceEvent 必须可平凡拷贝:导出走 memcpy,且不得在热路径持有堆资源");
	static_assert(sizeof(TraceEvent) == 40,
		"TraceEvent 尺寸变化会放大内存与带宽(每百万事件 8B = 8MB);变更需同步更新 schema 文档");

	// 采集元数据(不进入流式缓冲:只在采集起止各写一次,由采集线程独占)。
	struct TraceMeta
	{
		const char* Key = nullptr;
		const char* Value = nullptr;
	};
}
