// P2 W7-1:脚本字节码容器(WSL1)与 LuauVm::LoadChunk 统一装载入口的 headless 回归。
//
// 覆盖派工单 w7_1_script_artifact 的 U2 清单:
//   ① Pack → Unpack → luau_load 执行;LoadChunk(源码) 与 LoadChunk(容器) 行为一致;
//      同一源重复 Pack 字节完全相同(确定性);
//   ② 头布局逐字段独立复算(magic/版本/flags/reserved/字节码版本/指纹/两个 CRC);
//   ③ 坏 magic → 不进容器分支(按源码编译),容器分支绝不接手;
//   ④ 截断(4/20/27 字节与 payload 尾部)各自的失败短语;
//   ⑤ containerVersion=2 / luauBytecodeVersion=15 → 固定短语;
//   ⑥ 只改 payload 一字节 → payload checksum mismatch;
//   ⑦ 只改头里的字节码版本字节 → header checksum mismatch;
//   ⑧ 同步改 payload 版本字节 + 两个 CRC → luau_load 自己报 bytecode version mismatch;
//   ⑨ 非容器源码兼容:仍按源码编译,错误带 chunk 名与行号;
//   ⑩ Pack 失败(语法错误)→ false + 带编译器文本(含行号),不写容器。
//
// 头偏移(little-endian,共 28 字节,其后紧跟 payload):
//   0 magic "WSL1" | 4 containerVersion | 6 flags | 8 reserved | 10 luauBytecodeVersion
//   12 sourceFnv1a64 | 20 payloadCrc32 | 24 headerCrc32 | 28 payload

#include "World/Core/Asset/ScriptArtifact.h"
#include "World/Core/Core.h"
#include "World/Core/Log.h"
#include "World/Core/Vfs/Crc32.h"
#include "World/Script/LuauVm.h"
#include "World/Script/ScriptRef.h"
#include "World/Script/ScriptValue.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace
{
	constexpr size_t kHeaderSize = 28;
	constexpr size_t kPayloadOffset = 28;
	constexpr size_t kContainerVersionOffset = 4;
	constexpr size_t kFlagsOffset = 6;
	constexpr size_t kReservedOffset = 8;
	constexpr size_t kBytecodeVersionOffset = 10;
	constexpr size_t kSourceFingerprintOffset = 12;
	constexpr size_t kPayloadCrcOffset = 20;
	constexpr size_t kHeaderCrcOffset = 24;

	void Check(bool condition, const char* expression, int line)
	{
		if (!condition)
			throw std::runtime_error(std::string("line ") + std::to_string(line) + ": " + expression);
	}
#define CHECK(expression) Check(static_cast<bool>(expression), #expression, __LINE__)

	bool Contains(const std::string& haystack, const std::string& needle)
	{
		return haystack.find(needle) != std::string::npos;
	}

	uint16_t ReadU16(const std::vector<uint8_t>& bytes, size_t offset)
	{
		return static_cast<uint16_t>(bytes[offset]) | static_cast<uint16_t>(bytes[offset + 1]) << 8;
	}

	uint32_t ReadU32(const std::vector<uint8_t>& bytes, size_t offset)
	{
		return static_cast<uint32_t>(bytes[offset])
			| static_cast<uint32_t>(bytes[offset + 1]) << 8
			| static_cast<uint32_t>(bytes[offset + 2]) << 16
			| static_cast<uint32_t>(bytes[offset + 3]) << 24;
	}

	void WriteU16(std::vector<uint8_t>& bytes, size_t offset, uint16_t value)
	{
		bytes[offset] = static_cast<uint8_t>(value & 0xFFu);
		bytes[offset + 1] = static_cast<uint8_t>((value >> 8) & 0xFFu);
	}

	void WriteU32(std::vector<uint8_t>& bytes, size_t offset, uint32_t value)
	{
		bytes[offset] = static_cast<uint8_t>(value & 0xFFu);
		bytes[offset + 1] = static_cast<uint8_t>((value >> 8) & 0xFFu);
		bytes[offset + 2] = static_cast<uint8_t>((value >> 16) & 0xFFu);
		bytes[offset + 3] = static_cast<uint8_t>((value >> 24) & 0xFFu);
	}

	uint64_t ReadU64(const std::vector<uint8_t>& bytes, size_t offset)
	{
		uint64_t value = 0;
		for (int index = 0; index < 8; ++index)
			value |= static_cast<uint64_t>(bytes[offset + index]) << (index * 8);
		return value;
	}

	// 测试侧独立实现(与 ScriptArtifact.cpp 不同代码路径):FNV-1a64 / CRC 都自己算一遍。
	uint64_t Fnv1a64(std::string_view text)
	{
		uint64_t hash = 0xcbf29ce484222325ull;
		for (unsigned char byte : text)
		{
			hash ^= byte;
			hash *= 0x100000001b3ull;
		}
		return hash;
	}

	uint32_t HeaderCrc(const std::vector<uint8_t>& bytes)
	{
		std::vector<uint8_t> header(bytes.begin(), bytes.begin() + static_cast<ptrdiff_t>(kHeaderSize));
		WriteU32(header, kHeaderCrcOffset, 0);
		return World::Vfs::Crc32::Compute(header.data(), header.size());
	}

	void FixPayloadCrc(std::vector<uint8_t>& bytes)
	{
		WriteU32(bytes, kPayloadCrcOffset,
			World::Vfs::Crc32::Compute(bytes.data() + kPayloadOffset, bytes.size() - kPayloadOffset));
	}

	void FixHeaderCrc(std::vector<uint8_t>& bytes)
	{
		WriteU32(bytes, kHeaderCrcOffset, HeaderCrc(bytes));
	}

	const char* kDemoSource = R"LUA(
W7Counter = (W7Counter or 0) + 1
return W7Counter * 10
)LUA";

	// 在指定 environment 里装载容器并执行,返回 "结果值 + 环境计数"。
	bool RunContainerInEnvironment(World::LuauVm& vm, const std::vector<uint8_t>& container,
		const World::ScriptTableRef& environment, double* result, double* counter, std::string* error)
	{
		World::ScriptFunctionRef function =
			vm.LoadChunk(container, "scripts/tests/ArtifactDemo.lua", environment, error);
		if (!function.IsValid())
			return false;
		World::ScriptValue returned;
		if (!function.Call(nullptr, 0, &returned, error))
			return false;
		if (!returned.AsNumber(result))
			return false;
		World::ScriptValue stored = environment.GetField("W7Counter");
		return stored.AsNumber(counter);
	}
}

int main()
{
	World::Log::Init();
	try
	{
		using World::Asset::ScriptArtifact;

		World::LuauVm vm;
		std::string error;
		CHECK(vm.Init(&error));

		const std::string demoSource = kDemoSource;

		// ① Pack:成功 + 头布局逐字段复算。
		std::vector<uint8_t> container;
		error.clear();
		CHECK(ScriptArtifact::Pack("scripts/tests/ArtifactDemo.lua", demoSource, container, &error));
		CHECK(error.empty());
		CHECK(container.size() > kHeaderSize);
		CHECK(std::memcmp(container.data(), "WSL1", 4) == 0);
		CHECK(ReadU16(container, kContainerVersionOffset) == 1);
		CHECK(ReadU16(container, kFlagsOffset) == 0x1);            // bit0 = 带源指纹
		CHECK(ReadU16(container, kReservedOffset) == 0);
		const uint16_t headerBytecodeVersion = ReadU16(container, kBytecodeVersionOffset);
		const uint16_t payloadBytecodeVersion = container[kPayloadOffset];
		CHECK(headerBytecodeVersion >= 3 && headerBytecodeVersion <= 14);
		CHECK(headerBytecodeVersion == payloadBytecodeVersion);
		CHECK(headerBytecodeVersion == ScriptArtifact::LuauBytecodeVersion());
		CHECK(ReadU64(container, kSourceFingerprintOffset) == Fnv1a64(demoSource));
		CHECK(ReadU32(container, kPayloadCrcOffset)
			== World::Vfs::Crc32::Compute(container.data() + kPayloadOffset, container.size() - kPayloadOffset));
		CHECK(ReadU32(container, kHeaderCrcOffset) == HeaderCrc(container));
		CHECK(ScriptArtifact::IsArtifactBytes(container.data(), container.size()));
		CHECK(!ScriptArtifact::IsArtifactBytes("WSL", 3));
		CHECK(!ScriptArtifact::IsArtifactBytes(demoSource.data(), demoSource.size()));
		std::printf("[W7-1] (1) container: %zu bytes, header=%u bytes payload, bytecode version=%u, source fnv=0x%016llx\n",
			container.size(), static_cast<unsigned>(container.size() - kHeaderSize),
			static_cast<unsigned>(headerBytecodeVersion),
			static_cast<unsigned long long>(ReadU64(container, kSourceFingerprintOffset)));

		// ① 确定性:同一源重复 Pack 字节完全相同(第 3 份用来跑后续破坏性用例前的基线)。
		{
			std::vector<uint8_t> again;
			std::vector<uint8_t> third;
			CHECK(ScriptArtifact::Pack("scripts/tests/ArtifactDemo.lua", demoSource, again, &error));
			CHECK(ScriptArtifact::Pack("scripts/other/Path.lua", demoSource, third, &error));
			CHECK(again == container);
			CHECK(third == container);   // 逻辑路径只进错误前缀,不进容器
			std::printf("[W7-1] (1) deterministic: two extra Pack() calls byte-identical (%zu bytes each)\n",
				container.size());
		}

		// ① Unpack 取出的 payload 就是头后的原样字节,且能经 LoadChunk(容器) 执行。
		std::vector<uint8_t> unpackedPayload;
		error.clear();
		CHECK(ScriptArtifact::Unpack("scripts/tests/ArtifactDemo.lua", container.data(), container.size(),
			unpackedPayload, &error));
		CHECK(unpackedPayload.size() == container.size() - kHeaderSize);
		CHECK(std::memcmp(unpackedPayload.data(), container.data() + kPayloadOffset, unpackedPayload.size()) == 0);

		// ① LoadChunk(源码) 与 LoadChunk(容器) 行为一致:两个独立 environment 各跑两轮,
		//    返回值与环境计数都逐轮相同。
		{
			World::ScriptTableRef sourceEnv = vm.CreateEnvironment();
			World::ScriptTableRef containerEnv = vm.CreateEnvironment();
			CHECK(sourceEnv.IsValid() && containerEnv.IsValid());
			for (int round = 1; round <= 2; ++round)
			{
				double expectedResult = 0.0;
				double expectedCounter = 0.0;
				error.clear();
				CHECK(RunContainerInEnvironment(vm, container, containerEnv, &expectedResult, &expectedCounter, &error));
				CHECK(expectedResult == round * 10.0);
				CHECK(expectedCounter == round);

				World::ScriptFunctionRef sourceFunction =
					vm.LoadChunk(std::string_view(demoSource), "scripts/tests/ArtifactDemo.lua", sourceEnv, &error);
				CHECK(sourceFunction.IsValid());
				World::ScriptValue returned;
				CHECK(sourceFunction.Call(nullptr, 0, &returned, &error));
				double sourceResult = 0.0;
				double sourceCounter = 0.0;
				CHECK(returned.AsNumber(&sourceResult));
				CHECK(sourceEnv.GetField("W7Counter").AsNumber(&sourceCounter));
				CHECK(sourceResult == expectedResult);
				CHECK(sourceCounter == expectedCounter);
			}
			std::printf("[W7-1] (1) source/container equivalence: 2 rounds, both returned 10/20 with env counter 1/2\n");
		}

		// ③ 坏 magic:不是容器 → 走源码编译(错误里带 chunk 名;不出现任何 "script container" 短语)。
		{
			std::vector<uint8_t> badMagic = container;
			badMagic[0] = 'X';
			CHECK(!ScriptArtifact::IsArtifactBytes(badMagic.data(), badMagic.size()));
			std::string unpackError;
			std::vector<uint8_t> payload;
			CHECK(!ScriptArtifact::Unpack("scripts/tests/ArtifactDemo.lua", badMagic.data(), badMagic.size(),
				payload, &unpackError));
			CHECK(Contains(unpackError, "magic"));

			error.clear();
			World::ScriptFunctionRef function =
				vm.LoadChunk(badMagic, "=artifact-bad-magic", World::ScriptTableRef(), &error);
			CHECK(!function.IsValid());
			CHECK(!Contains(error, "script container"));
			CHECK(Contains(error, "artifact-bad-magic"));
			std::printf("[W7-1] (3) bad magic: Unpack='%s' | LoadChunk(source branch)='%s'\n",
				unpackError.c_str(), error.substr(0, error.find('\n')).c_str());
		}

		// ④ 截断:4 / 20 / 27(头内)< 28 → 固定短语;payload 尾部截断 → 校验和短语。
		{
			for (size_t truncatedSize : { size_t(4), size_t(20), size_t(27) })
			{
				std::vector<uint8_t> truncated(container.begin(), container.begin() + static_cast<ptrdiff_t>(truncatedSize));
				std::string truncatedError;
				std::vector<uint8_t> payload;
				CHECK(!ScriptArtifact::Unpack("scripts/tests/ArtifactDemo.lua", truncated.data(), truncated.size(),
					payload, &truncatedError));
				CHECK(payload.empty());
				CHECK(Contains(truncatedError, "script container is truncated"));

				// 容器头命中后的失败必须硬失败:LoadChunk 不得回退源码编译。
				error.clear();
				World::ScriptFunctionRef function =
					vm.LoadChunk(truncated, "=artifact-truncated", World::ScriptTableRef(), &error);
				CHECK(!function.IsValid());
				CHECK(Contains(error, "script container is truncated"));
				std::printf("[W7-1] (4) truncated to %zu bytes: '%s'\n", truncatedSize, error.c_str());
			}

			// payload 尾部截断:头里没有 payload 长度字段,唯一的完整性证据是 CRC32,
			// 因此报 payload checksum mismatch(见 W7-1 报告的"未决问题")。
			std::vector<uint8_t> cutTail(container.begin(), container.end() - 1);
			std::string cutError;
			std::vector<uint8_t> payload;
			CHECK(!ScriptArtifact::Unpack("scripts/tests/ArtifactDemo.lua", cutTail.data(), cutTail.size(),
				payload, &cutError));
			CHECK(Contains(cutError, "script container payload checksum mismatch"));
			error.clear();
			CHECK(!vm.LoadChunk(cutTail, "=artifact-cut-tail", World::ScriptTableRef(), &error).IsValid());
			CHECK(Contains(error, "script container payload checksum mismatch"));
			std::printf("[W7-1] (4) payload tail cut: '%s'\n", error.c_str());
		}

		// ⑤ containerVersion=2 / luauBytecodeVersion=15:版本类检查先于校验和,报固定短语。
		{
			std::vector<uint8_t> wrongContainerVersion = container;
			WriteU16(wrongContainerVersion, kContainerVersionOffset, 2);
			std::string versionError;
			std::vector<uint8_t> payload;
			CHECK(!ScriptArtifact::Unpack("scripts/tests/ArtifactDemo.lua", wrongContainerVersion.data(),
				wrongContainerVersion.size(), payload, &versionError));
			CHECK(Contains(versionError, "script container version 2 is not supported"));
			error.clear();
			CHECK(!vm.LoadChunk(wrongContainerVersion, "=artifact-version", World::ScriptTableRef(), &error).IsValid());
			CHECK(Contains(error, "script container version 2 is not supported"));
			std::printf("[W7-1] (5) containerVersion=2: '%s'\n", error.c_str());

			std::vector<uint8_t> wrongBytecodeVersion = container;
			WriteU16(wrongBytecodeVersion, kBytecodeVersionOffset, 15);
			std::string bytecodeError;
			CHECK(!ScriptArtifact::Unpack("scripts/tests/ArtifactDemo.lua", wrongBytecodeVersion.data(),
				wrongBytecodeVersion.size(), payload, &bytecodeError));
			CHECK(Contains(bytecodeError, "script container bytecode version 15 is not supported"));
			error.clear();
			CHECK(!vm.LoadChunk(wrongBytecodeVersion, "=artifact-bytecode-version", World::ScriptTableRef(), &error).IsValid());
			CHECK(Contains(error, "script container bytecode version 15 is not supported"));
			std::printf("[W7-1] (5) luauBytecodeVersion=15: '%s'\n", error.c_str());
		}

		// ⑥ 只改 payload 一字节(不动 CRC)→ payload checksum mismatch。
		{
			std::vector<uint8_t> corrupt = container;
			corrupt.back() ^= 0x5A;
			std::string corruptError;
			std::vector<uint8_t> payload;
			CHECK(!ScriptArtifact::Unpack("scripts/tests/ArtifactDemo.lua", corrupt.data(), corrupt.size(),
				payload, &corruptError));
			CHECK(Contains(corruptError, "script container payload checksum mismatch"));
			error.clear();
			CHECK(!vm.LoadChunk(corrupt, "=artifact-payload-crc", World::ScriptTableRef(), &error).IsValid());
			CHECK(Contains(error, "script container payload checksum mismatch"));
			std::printf("[W7-1] (6) one payload byte flipped: '%s'\n", error.c_str());
		}

		// ⑦ 只改头里的字节码版本字节(值仍在 [3,14],不动 headerCrc)→ header checksum mismatch。
		{
			std::vector<uint8_t> corrupt = container;
			WriteU16(corrupt, kBytecodeVersionOffset, static_cast<uint16_t>(headerBytecodeVersion - 1));
			std::string corruptError;
			std::vector<uint8_t> payload;
			CHECK(!ScriptArtifact::Unpack("scripts/tests/ArtifactDemo.lua", corrupt.data(), corrupt.size(),
				payload, &corruptError));
			CHECK(Contains(corruptError, "script container header checksum mismatch"));
			error.clear();
			CHECK(!vm.LoadChunk(corrupt, "=artifact-header-crc", World::ScriptTableRef(), &error).IsValid());
			CHECK(Contains(error, "script container header checksum mismatch"));
			std::printf("[W7-1] (7) header bytecode version byte changed (%u->%u, CRC untouched): '%s'\n",
				static_cast<unsigned>(headerBytecodeVersion),
				static_cast<unsigned>(headerBytecodeVersion - 1), error.c_str());
		}

		// ⑧ 同步改 payload 版本字节 + 两个 CRC → 容器校验全部通过,由 luau_load 自己报版本不匹配。
		{
			std::vector<uint8_t> forged = container;
			forged[kPayloadOffset] = 15;
			FixPayloadCrc(forged);
			FixHeaderCrc(forged);
			std::string payloadError;
			std::vector<uint8_t> payload;
			CHECK(ScriptArtifact::Unpack("scripts/tests/ArtifactDemo.lua", forged.data(), forged.size(),
				payload, &payloadError));
			CHECK(payloadError.empty());
			error.clear();
			CHECK(!vm.LoadChunk(forged, "=artifact-forged-version", World::ScriptTableRef(), &error).IsValid());
			CHECK(Contains(error, "bytecode version mismatch"));
			std::printf("[W7-1] (8) payload version byte 15 with consistent CRCs reaches luau_load: '%s'\n", error.c_str());
		}

		// ⑨ 非容器源码兼容:仍按源码编译,错误带 chunk 名与行号(第 3 行)。
		{
			const char* brokenSource = "local value = 1\nlocal other = 2\nreturn value +";
			std::vector<uint8_t> bytes(brokenSource, brokenSource + std::strlen(brokenSource));
			CHECK(!ScriptArtifact::IsArtifactBytes(bytes.data(), bytes.size()));
			error.clear();
			CHECK(!vm.LoadChunk(bytes, "=artifact-source-error", World::ScriptTableRef(), &error).IsValid());
			CHECK(Contains(error, "artifact-source-error"));
			CHECK(Contains(error, "3"));
			std::printf("[W7-1] (9) non-container source error (chunk name + line): '%s'\n", error.c_str());

			const char* goodSource = "return 7";
			std::vector<uint8_t> goodBytes(goodSource, goodSource + std::strlen(goodSource));
			error.clear();
			World::ScriptFunctionRef function =
				vm.LoadChunk(goodBytes, "=artifact-source-good", World::ScriptTableRef(), &error);
			CHECK(function.IsValid());
			World::ScriptValue returned;
			CHECK(function.Call(nullptr, 0, &returned, &error));
			double value = 0.0;
			CHECK(returned.AsNumber(&value));
			CHECK(value == 7.0);
			std::printf("[W7-1] (9) non-container source executed through LoadChunk(bytes): return 7\n");
		}

		// ⑩ Pack 失败:语法错误 → false + 编译器文本(带行号),且不写容器。
		{
			std::vector<uint8_t> out(8, 0xEE);   // 预填垃圾:失败必须把它清空
			std::string packError;
			CHECK(!ScriptArtifact::Pack("scripts/tests/Broken.lua", "local value = 1\nlocal bad = \n",
				out, &packError));
			CHECK(out.empty());
			std::printf("[W7-1] (10) Pack(broken source) failed: '%s'\n", packError.c_str());
			CHECK(Contains(packError, "compile error"));
			CHECK(Contains(packError, "Expected"));
		}

		// 附加(契约边角):unknown flag bit 忽略;reserved 非 0 硬失败。
		{
			std::vector<uint8_t> flags = container;
			WriteU16(flags, kFlagsOffset, static_cast<uint16_t>(0x1u | 0x8000u));
			FixHeaderCrc(flags);
			std::vector<uint8_t> payload;
			CHECK(ScriptArtifact::Unpack("scripts/tests/ArtifactDemo.lua", flags.data(), flags.size(), payload, &error));
			error.clear();
			World::ScriptFunctionRef function =
				vm.LoadChunk(flags, "=artifact-flag-bit", World::ScriptTableRef(), &error);
			CHECK(function.IsValid());
			std::printf("[W7-1] (extra) unknown flag bit 0x8000 ignored (container still loads)\n");

			std::vector<uint8_t> reserved = container;
			WriteU16(reserved, kReservedOffset, 1);
			FixHeaderCrc(reserved);
			std::string reservedError;
			CHECK(!ScriptArtifact::Unpack("scripts/tests/ArtifactDemo.lua", reserved.data(), reserved.size(),
				payload, &reservedError));
			CHECK(Contains(reservedError, "reserved must be 0"));
			error.clear();
			CHECK(!vm.LoadChunk(reserved, "=artifact-reserved", World::ScriptTableRef(), &error).IsValid());
			CHECK(Contains(error, "reserved must be 0"));
			std::printf("[W7-1] (extra) reserved=1: '%s'\n", error.c_str());
		}

		vm.Shutdown();
		CHECK(!vm.IsInitialized());
		std::printf("World.ScriptArtifact: all checks passed\n");
		return 0;
	}
	catch (const std::exception& exception)
	{
		std::fprintf(stderr, "World.ScriptArtifact: FAILED: %s\n", exception.what());
		return 1;
	}
}
