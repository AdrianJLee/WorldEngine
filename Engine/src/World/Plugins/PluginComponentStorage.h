#pragma once

// ============================================================================
// PLUG-T2c:插件组件存储桥(宿主侧)。
//
// 把 T2b 的 "schema-only 插件组件" 变成真能挂到场景实体上的组件:
//   * 宿主为每个声明了 `Size` 的插件组件合成一个**固定尺寸 blob 存储类**
//     `PluginComponentBlob<Id>`(alignas(16),字节数 = 尺寸档位,见下);
//   * StorageBinding 的 Add/Copy/CopyAll 与引擎组件(ComponentSchemaBridge.h 的
//     MakeComponentStorage<T>)同语义:Add = 给实体加一个清零的 blob;
//     Copy = Scene::DuplicateEntity;CopyAll = Scene::CopyScene(编辑器 Play 的活动场景副本)。
//   * 字段 Get/Set 仍然走 PluginManager.cpp 的 256 槽访问器(按 Kind/Offset 在实例内存上
//     读写);实例指针 = blob 地址,blob 的 `Bytes` 在 offset 0 ⇒ 访问器写在 "Bytes + offset"。
//
// 组件 id 段(与 entt 的类型哈希段区分;**文档写死**,见 docs/dev/plugin-framework.md):
//
//   bit31     = 1                 插件组件标记(保留高位段)
//   bit 8..10 = 尺寸档位 0..7     对应 kPluginComponentTierBytes
//   bit 0..7  = 槽位 0..63        管理器分配的**在册序号**
//
//   id = MakePluginComponentId(档位, 槽位)
//
// 档位刻意进 id:entt 的 `registry.storage<Blob>(id)` 会把 id 绑到具体存储类型上,
// 同一个 id 换档位会触发 entt 的 "Unexpected type" 断言 ⇒ 档位 + 槽位一起决定 id,
// 复用槽位换档位一定落在不同的 id 上。槽位复用换**同档位** = 同类型同 id,安全。
//
// 与引擎/Game 现有组件 id 冲突的兜底:SchemaRegistry 的 DuplicateComponentId 会让
// 同 id 的插件注册干净失败(false + 可读诊断),不会半注册、也不会用到冲突的存储。
// ============================================================================

#include "World/Schema/Schema.h"

#include <cstdint>
#include <string>

namespace World::Plugins
{
	// 插件组件 id 的保留段标记(bit31)。
	constexpr uint32_t kPluginComponentIdFlag = 0x80000000u;
	// 尺寸档位数(bit 8..10 的容量)。
	constexpr uint32_t kPluginComponentTierCount = 8;
	// 每个管理器同时在册的插件组件槽位数(bit 0..7 的容量,首期只开 64)。
	constexpr uint32_t kPluginComponentSlotCount = 64;
	// 最大档字节数(超过 = 干净拒绝)。
	constexpr uint32_t kPluginComponentMaxBytes = 2048;
	// 宿主 blob 的对齐上限(alignas(16)):插件声明的 Alignment 超过它 = 干净拒绝。
	constexpr uint32_t kPluginComponentAlignmentCap = 16;

	// 尺寸档位 → 字节数(最小覆盖声明大小的一档)。
	inline constexpr uint32_t kPluginComponentTierBytes[kPluginComponentTierCount] =
		{ 16u, 32u, 64u, 128u, 256u, 512u, 1024u, 2048u };

	// 覆盖 componentSize 的最小档位;超上限 = kPluginComponentTierCount(调用方据此拒绝)。
	constexpr uint32_t PluginComponentTierFor(uint32_t componentSize)
	{
		for (uint32_t tier = 0; tier < kPluginComponentTierCount; ++tier)
			if (componentSize <= kPluginComponentTierBytes[tier])
				return tier;
		return kPluginComponentTierCount;
	}

	// 组件 id:保留段 | 档位 << 8 | 槽位。
	constexpr uint32_t MakePluginComponentId(uint32_t tier, uint32_t slot)
	{
		return kPluginComponentIdFlag | ((tier & 0x7u) << 8) | (slot & 0xFFu);
	}

	// 合成一个插件组件的存储绑定(Add/Copy/CopyAll + ComponentId)。
	//   componentSize:插件声明的结构总大小(必须 > 0 —— 0 = 保持 T2b 的 schema-only 行为);
	//   alignment:插件声明的 alignof(0 = 未声明;非 0 必须是 2 的幂且不超过 16);
	//   slot:管理器分配的在册槽位(< kPluginComponentSlotCount)。
	// 失败 = false + *error 可读原因(调用方记 WARN 并拒绝注册;不半注册)。
	bool MakePluginComponentStorageBinding(uint32_t componentSize, uint32_t alignment,
		uint32_t slot, World::Schema::StorageBinding* outBinding, std::string* error);
}
