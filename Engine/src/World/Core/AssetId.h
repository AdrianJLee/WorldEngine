#pragma once

#include "World/Core/Export.h"

#include <cstdint>
#include <string>

namespace World
{
	// ---------------------------------------------------------------------------
	// 资产稳定身份(2026-10-04,contract.asset-identity-and-strings 的 L0 层)。
	//
	// 与 `PathId` 的区别(这是两件事,不要混):
	//   * `PathId` = **进程内**驻留的路径标识,进程重启就重来,只用于热路径省字符串;
	//   * `AssetId` = **跨进程/跨会话稳定**的资产身份,写在资产文件里、跟着文件走 ——
	//     它才是"资产改名/移动之后引用不断链"的依据。
	//
	// 为什么不用路径当身份:路径是会变的名字,不是身份。用路径当身份 = 改名即断链、
	// 移动即断链、Mod 覆盖没有稳定锚点。
	//
	// 分配规则:
	//   * 在**资产第一次被创建/导入**时生成一次,之后永不改变(重新导入/改设置/改名都保持);
	//   * 生成后写进资产文件自身(不是旁路 .meta —— 见 traps/wimport-removed.md:
	//     "同一件事两份真相"被明确否决);
	//   * 0 = 未分配(老资产第一次读取时由编辑器/导入器补齐并写回)。
	//
	// 存储形态:8 字节无符号 + 十六进制文本(资产头 / YAML 行都用 `0x` + 16 位十六进制,
	// 大小写不敏感解析)。文本形态是唯一的外部表示,没有第二份。
	// ---------------------------------------------------------------------------
	struct AssetId
	{
		uint64_t Value = 0;

		constexpr bool IsValid() const { return Value != 0; }
		constexpr bool operator==(const AssetId& other) const { return Value == other.Value; }
		constexpr bool operator!=(const AssetId& other) const { return Value != other.Value; }
		constexpr bool operator<(const AssetId& other) const { return Value < other.Value; }
	};

	// 生成一个新的、实际唯一的身份(随机 64 位;碰撞概率可忽略)。
	WLD_API AssetId GenerateAssetId();

	// 文本形态:`0x` + 16 位十六进制(固定宽度,便于 diff 与测试)。无效 id ⇒ "0x0000000000000000"。
	WLD_API std::string FormatAssetId(AssetId id);
	// 解析:接受带/不带 `0x` 前缀的十六进制(1..16 位);空串/非法 ⇒ 无效 id 且返回 false。
	WLD_API bool ParseAssetId(const std::string& text, AssetId* out);
}

namespace std
{
	template <> struct hash<World::AssetId>
	{
		std::size_t operator()(const World::AssetId& id) const noexcept { return hash<uint64_t>()(id.Value); }
	};
}