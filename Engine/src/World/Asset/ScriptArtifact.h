#pragma once

#include "World/Core/Export.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace World::Asset
{
	// W7-1:脚本字节码容器(WSL1)。
	//
	// 设计契约(与 plan §11 "W7 冻结草案"一致,不要单独改动):
	//   - 包内脚本沿用原逻辑路径,内容换成容器 → 开发树(源码)与发行包(字节码)
	//     共用同一个装载入口,判定只看前 4 字节 magic;
	//   - 容器只承载 Luau 字节码(payload 原样,含它自己的 version 字节),**不嵌源码**;
	//     sourceFnv1a64 只服务增量 cook 与诊断;
	//   - 头命中但任一校验失败 → **硬失败**,绝不回退"按源码编译"
	//     (否则坏包会被当成源码,报出一堆与真因无关的语法错误)。
	//
	// 头布局(全部 little-endian,共 28 字节,随后紧跟 payload):
	//   偏移  大小  字段
	//   0     4     magic "WSL1"
	//   4     2     containerVersion = 1(未知 → 硬失败)
	//   6     2     flags(bit0 = 带源指纹;未知 bit 忽略)
	//   8     2     reserved(必须 0)
	//   10    2     luauBytecodeVersion(= payload[0],必须 ∈ [3,14])
	//   12    8     sourceFnv1a64(未编译源码字节的 FNV-1a64)
	//   20    4     payloadCrc32
	//   24    4     headerCrc32(前 28 字节,本字段按 0 占位)
	//   28    N     payload(Luau 字节码原样)
	//
	// 固定错误短语(测试与上层诊断依赖这些子串,改动前先确认):
	//   "script container is truncated (...)"
	//   "script container version X is not supported"
	//   "script container bytecode version X is not supported"
	//   "script container header checksum mismatch"
	//   "script container payload checksum mismatch"
	class WLD_API ScriptArtifact
	{
	public:
		// 把一段 Luau 源码编译成容器。失败(out 清空 + error)的三种情况:
		//   1) Luau::compile 抛异常;2) 返回 version 0 的错误装载体(带上编译器错误文本);
		//   3) 编译器产出的字节码版本超出 [3,14](理论上不会发生,防御性检查)。
		static bool Pack(std::string_view logicalPath, std::string_view source,
			std::vector<uint8_t>& out, std::string* error = nullptr);

		// 校验容器并取出 payload(字节码)。logicalPath 只用于错误前缀,可为空。
		// 任一校验失败 → false + error,outPayload 清空;调用方不得回退按源码编译。
		static bool Unpack(std::string_view logicalPath, const uint8_t* data, size_t size,
			std::vector<uint8_t>& outPayload, std::string* error = nullptr);

		// 判定顺序的唯一入口:size >= 4 且前 4 字节为 "WSL1"。
		static bool IsArtifactBytes(const void* data, size_t size);

		// 当前链接的 Luau 编译器产出的字节码版本(写入容器头 10 偏移的同一个值)。
		static uint16_t LuauBytecodeVersion();
	};
}
