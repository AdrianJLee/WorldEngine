#include "World/Core/Asset/ScriptArtifact.h"

#include "World/Core/Vfs/Crc32.h"

#include <Luau/Bytecode.h>
#include <Luau/Compiler.h>

#include <cstring>
#include <exception>

namespace World::Asset
{
	namespace
	{
		constexpr char kMagic[4] = { 'W', 'S', 'L', '1' };
		constexpr size_t kHeaderSize = 28;
		constexpr uint16_t kContainerVersion = 1;
		constexpr uint16_t kFlagSourceFingerprint = 0x1;
		constexpr uint16_t kMinBytecodeVersion = 3;
		constexpr uint16_t kMaxBytecodeVersion = 14;

		// FNV-1a64(与 Script/HotReload.cpp 同一族算法与常量;这里独立实现,
		// 避免 Core → Script 的反向依赖)。
		constexpr uint64_t kFnvOffsetBasis = 0xcbf29ce484222325ull;
		constexpr uint64_t kFnvPrime = 0x100000001b3ull;

		uint64_t Fnv1a64(const void* data, size_t size)
		{
			uint64_t hash = kFnvOffsetBasis;
			const auto* bytes = static_cast<const uint8_t*>(data);
			for (size_t index = 0; index < size; ++index)
			{
				hash ^= bytes[index];
				hash *= kFnvPrime;
			}
			return hash;
		}

		uint16_t ReadU16(const uint8_t* data)
		{
			return static_cast<uint16_t>(data[0]) | static_cast<uint16_t>(data[1]) << 8;
		}

		uint32_t ReadU32(const uint8_t* data)
		{
			return static_cast<uint32_t>(data[0])
				| static_cast<uint32_t>(data[1]) << 8
				| static_cast<uint32_t>(data[2]) << 16
				| static_cast<uint32_t>(data[3]) << 24;
		}

		void WriteU16(uint8_t* data, uint16_t value)
		{
			data[0] = static_cast<uint8_t>(value & 0xFFu);
			data[1] = static_cast<uint8_t>((value >> 8) & 0xFFu);
		}

		void WriteU32(uint8_t* data, uint32_t value)
		{
			data[0] = static_cast<uint8_t>(value & 0xFFu);
			data[1] = static_cast<uint8_t>((value >> 8) & 0xFFu);
			data[2] = static_cast<uint8_t>((value >> 16) & 0xFFu);
			data[3] = static_cast<uint8_t>((value >> 24) & 0xFFu);
		}

		void WriteU64(uint8_t* data, uint64_t value)
		{
			for (int index = 0; index < 8; ++index)
				data[index] = static_cast<uint8_t>((value >> (index * 8)) & 0xFFu);
		}

		// 头校验和不含本字段:把 24..28 视为 0 后对前 28 字节取 CRC32。
		uint32_t HeaderCrc(const uint8_t* data)
		{
			uint8_t header[kHeaderSize] = {};
			std::memcpy(header, data, kHeaderSize);
			WriteU32(header + 24, 0);
			return Vfs::Crc32::Compute(header, kHeaderSize);
		}

		std::string WithPath(std::string_view logicalPath, const std::string& message)
		{
			if (logicalPath.empty())
				return message;
			return std::string(logicalPath) + ": " + message;
		}

		void SetError(std::string* error, const std::string& message)
		{
			if (error)
				*error = message;
		}
	}

	uint16_t ScriptArtifact::LuauBytecodeVersion()
	{
		// 编译产物 payload[0] 由 Luau 编译器按 LBC_VERSION_TARGET 写;同一常量
		// 也用于头字段的语义,非法时编译路径的防御性检查会拦下。注意 Luau 在显式
		// 打开特性开关时可能产出更高版本,所以容器的头字段一律以实际 payload[0] 为准。
		static_assert(LBC_VERSION_TARGET >= kMinBytecodeVersion
			&& LBC_VERSION_TARGET <= kMaxBytecodeVersion,
			"Luau bytecode target is outside the container's supported range");
		return static_cast<uint16_t>(LBC_VERSION_TARGET);
	}

	bool ScriptArtifact::IsArtifactBytes(const void* data, size_t size)
	{
		return data != nullptr && size >= sizeof(kMagic) && std::memcmp(data, kMagic, sizeof(kMagic)) == 0;
	}

	bool ScriptArtifact::Pack(std::string_view logicalPath, std::string_view source,
		std::vector<uint8_t>& out, std::string* error)
	{
		// 失败一律不写容器:先把 out 清掉,保证调用方不会误用上一份产物。
		out.clear();

		std::string bytecode;
		try
		{
			bytecode = Luau::compile(std::string(source), {});
		}
		catch (const std::exception& exception)
		{
			SetError(error, WithPath(logicalPath, std::string("compile error: ") + exception.what()));
			return false;
		}

		if (bytecode.empty())
		{
			SetError(error, WithPath(logicalPath, "compile error: compiler returned no bytecode"));
			return false;
		}

		// version 0 = 错误装载体:第 1 字节起是编译器错误文本(见 BytecodeBuilder::getError)。
		// 错误文本形如 ":3: Expected ..."(自带行号),所以这里拼成 "<path>: compile error:3: ..."。
		const uint8_t bytecodeVersion = static_cast<uint8_t>(bytecode[0]);
		if (bytecodeVersion == 0)
		{
			SetError(error, WithPath(logicalPath,
				"compile error" + std::string(bytecode.data() + 1, bytecode.size() - 1)));
			return false;
		}
		if (bytecodeVersion < kMinBytecodeVersion || bytecodeVersion > kMaxBytecodeVersion)
		{
			SetError(error, WithPath(logicalPath,
				"compiler bytecode version " + std::to_string(bytecodeVersion) + " is not supported"));
			return false;
		}

		out.resize(kHeaderSize + bytecode.size(), 0);
		std::memcpy(out.data(), kMagic, sizeof(kMagic));
		WriteU16(out.data() + 4, kContainerVersion);
		WriteU16(out.data() + 6, kFlagSourceFingerprint);
		WriteU16(out.data() + 8, 0);
		WriteU16(out.data() + 10, bytecodeVersion);
		WriteU64(out.data() + 12, Fnv1a64(source.data(), source.size()));
		std::memcpy(out.data() + kHeaderSize, bytecode.data(), bytecode.size());
		WriteU32(out.data() + 20, Vfs::Crc32::Compute(out.data() + kHeaderSize, bytecode.size()));
		WriteU32(out.data() + 24, HeaderCrc(out.data()));
		return true;
	}

	bool ScriptArtifact::Unpack(std::string_view logicalPath, const uint8_t* data, size_t size,
		std::vector<uint8_t>& outPayload, std::string* error)
	{
		outPayload.clear();

		// 判定顺序:截断 → 容器版本 → reserved → 字节码版本 → 头校验和 → payload 校验和。
		// 版本类检查必须早于校验和,否则"改版本字节"只会报 checksum,测试与用户都看不出真因。
		if (data == nullptr || size < kHeaderSize)
		{
			SetError(error, WithPath(logicalPath,
				"script container is truncated (size " + std::to_string(size)
					+ " < header " + std::to_string(kHeaderSize) + ")"));
			return false;
		}
		if (std::memcmp(data, kMagic, sizeof(kMagic)) != 0)
		{
			SetError(error, WithPath(logicalPath, "script container is malformed (magic is not \"WSL1\")"));
			return false;
		}

		const uint16_t containerVersion = ReadU16(data + 4);
		if (containerVersion != kContainerVersion)
		{
			SetError(error, WithPath(logicalPath,
				"script container version " + std::to_string(containerVersion)
					+ " is not supported (expected " + std::to_string(kContainerVersion) + ")"));
			return false;
		}

		const uint16_t reserved = ReadU16(data + 8);
		if (reserved != 0)
		{
			SetError(error, WithPath(logicalPath,
				"script container is malformed (reserved must be 0, got " + std::to_string(reserved) + ")"));
			return false;
		}

		const uint16_t bytecodeVersion = ReadU16(data + 10);
		if (bytecodeVersion < kMinBytecodeVersion || bytecodeVersion > kMaxBytecodeVersion)
		{
			SetError(error, WithPath(logicalPath,
				"script container bytecode version " + std::to_string(bytecodeVersion)
					+ " is not supported (expected [" + std::to_string(kMinBytecodeVersion)
					+ ".." + std::to_string(kMaxBytecodeVersion) + "])"));
			return false;
		}

		if (ReadU32(data + 24) != HeaderCrc(data))
		{
			SetError(error, WithPath(logicalPath, "script container header checksum mismatch"));
			return false;
		}

		const size_t payloadSize = size - kHeaderSize;
		const uint8_t* payload = data + kHeaderSize;
		if (Vfs::Crc32::Compute(payload, payloadSize) != ReadU32(data + 20))
		{
			SetError(error, WithPath(logicalPath, "script container payload checksum mismatch"));
			return false;
		}

		// 最小结构检查:payload 为空或短于 version(+typesversion)字节时,luau_load 会
		// 越界读取(它的 read<T> 不做边界检查)。CRC 只能拦住意外截断,这里补一道门。
		const bool needsTypesVersion = payloadSize >= 1 && payload[0] >= 4;
		if (payloadSize == 0 || (needsTypesVersion && payloadSize < 2))
		{
			SetError(error, WithPath(logicalPath,
				"script container is truncated (payload has " + std::to_string(payloadSize)
					+ " bytes, bytecode header needs more)"));
			return false;
		}

		outPayload.assign(payload, payload + payloadSize);
		return true;
	}
}
