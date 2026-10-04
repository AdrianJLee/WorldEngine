#pragma once

#include "World/Core/AssetId.h"
#include "World/Core/StringPool.h"

#include <cstdint>

namespace World
{
	// ---------------------------------------------------------------------------
	// 组件里的资产引用(2026-10-04,contract.asset-identity-and-strings)。
	//
	// 两件事分开存,各有各的用途:
	//   * `Path`  = 驻留路径 id(`PathId`):**热路径**的键,进程内解析 O(1),不物化字符串;
	//   * `Identity` = 跨会话稳定身份(`AssetId`):写进 .wd / 用于"路径失效时按身份找回"。
	//
	// 为什么两个都要:路径是"现在在哪",身份是"是哪一个"。只用路径 ⇒ 改名即断链;
	// 只用身份 ⇒ 每次解析都要过目录(慢),而且人读不懂。
	//
	// 组件里**只能**放这个(16 字节、平凡可拷贝);绝不放 `Ref<T>`(运行态对象不进组件,
	// 见 contract.runtime-state-outside-components)。
	// ---------------------------------------------------------------------------
	struct AssetRef
	{
		PathId Path;
		AssetId Identity;

		constexpr bool HasPath() const { return Path.IsValid(); }
	};
	static_assert(sizeof(AssetRef) == 16, "AssetRef must be exactly 16 bytes (PathId + AssetId)");
	static_assert(std::is_trivially_copyable_v<AssetRef>, "AssetRef must be trivially copyable");
}