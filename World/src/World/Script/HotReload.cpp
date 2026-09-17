#include "wldpch.h"
#include "World/Script/HotReload.h"

#include "World/Core/Application.h"
#include "World/Script/BehaviorRegistry.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <system_error>
#include <utility>

namespace World
{
	namespace
	{
		// FNV-1a64(与 schema-compiler / 现有哈希同一族算法,输入换成文件内容)。
		constexpr uint64_t kFnvOffsetBasis = 0xcbf29ce484222325ull;
		constexpr uint64_t kFnvPrime = 0x100000001b3ull;

		uint64_t HashBytes(const void* data, std::size_t size)
		{
			uint64_t hash = kFnvOffsetBasis;
			const auto* bytes = static_cast<const unsigned char*>(data);
			for (std::size_t index = 0; index < size; ++index)
			{
				hash ^= bytes[index];
				hash *= kFnvPrime;
			}
			return hash;
		}

		// 逻辑路径的磁盘回退位置:与 ScriptEngine 的既有回退路径一致。
		std::filesystem::path DiskPathFor(const std::string& logicalPath)
		{
			return std::filesystem::path(WLD_ASSETPATH + std::string("/") + logicalPath);
		}

		std::string FieldIdText(const std::string& fieldName)
		{
			char buffer[32] = {};
			std::snprintf(buffer, sizeof(buffer), "0x%016llx",
				static_cast<unsigned long long>(BehaviorRegistry::LuaFieldId(fieldName)));
			return std::string(buffer);
		}

		bool ReadDiskFile(const std::filesystem::path& path, std::string& out)
		{
			std::ifstream file(path, std::ios::binary);
			if (!file.is_open())
				return false;
			std::stringstream buffer;
			buffer << file.rdbuf();
			out = buffer.str();
			return true;
		}
	}

	uint64_t FingerprintScriptBytes(const void* data, std::size_t size)
	{
		if (!data || size == 0)
			return kFnvOffsetBasis;
		return HashBytes(data, size);
	}

	uint64_t FingerprintScriptText(const std::string& text)
	{
		return FingerprintScriptBytes(text.data(), text.size());
	}

	bool ResolveScriptSource(const std::string& logicalPath, std::string& source, std::string* error)
	{
		if (logicalPath.empty())
		{
			if (error) *error = "script path is empty";
			return false;
		}

		// 1) VFS 优先:开发目录 provider 每次 Read 都走 provider,改盘立即可见。
		if (Application::HasInstance())
		{
			std::error_code ec;
			std::vector<uint8_t> bytes;
			if (Application::Get().GetContext().Vfs().Read(logicalPath, bytes, ec) && !bytes.empty())
			{
				source.assign(bytes.begin(), bytes.end());
				if (error) error->clear();
				return true;
			}
		}

		// 2) 磁盘回退(与既有 ScriptEngine::ReadScriptSource 同一位置)。
		std::string diskSource;
		if (!ReadDiskFile(DiskPathFor(logicalPath), diskSource))
		{
			if (error) *error = "script not found: " + logicalPath;
			return false;
		}
		source = std::move(diskSource);
		if (error) error->clear();
		return true;
	}

	ScriptSourceFingerprint FingerprintScriptSource(const std::string& logicalPath, std::string* error)
	{
		ScriptSourceFingerprint result;
		if (logicalPath.empty())
		{
			if (error) *error = "script path is empty";
			return result;
		}

		// 内容哈希优先:同内容重写(只动 mtime)不产生变化。
		std::string source;
		std::string readError;
		if (ResolveScriptSource(logicalPath, source, &readError))
		{
			result.Value = FingerprintScriptText(source);
			result.FromContent = true;
			result.Exists = true;
			if (error) error->clear();
			return result;
		}

		// 内容读不到 → mtime+size 兜底(只有磁盘回退路径能提供 mtime)。
		std::error_code ec;
		const std::filesystem::path diskPath = DiskPathFor(logicalPath);
		const uintmax_t size = std::filesystem::file_size(diskPath, ec);
		if (ec)
		{
			if (error) *error = readError;
			return result;
		}
		const std::filesystem::file_time_type writeTime = std::filesystem::last_write_time(diskPath, ec);
		if (ec)
		{
			if (error) *error = readError;
			return result;
		}
		const std::string key = std::to_string(static_cast<unsigned long long>(size)) + "|" +
			std::to_string(static_cast<long long>(writeTime.time_since_epoch().count()));
		result.Value = FingerprintScriptText(key);
		result.FromContent = false;
		result.Exists = true;
		if (error) error->clear();
		return result;
	}

	void DescribeScriptFieldMigration(
		const std::unordered_map<std::string, LuaScriptField>& previous,
		const std::unordered_map<std::string, LuaScriptField>& next,
		const std::string& scriptPath,
		std::vector<std::string>* diagnostics)
	{
		if (!diagnostics)
			return;

		// unordered_map 遍历顺序不稳定 → 先按字段名收集再排序,保证诊断文本可断言/可复现。
		std::vector<std::pair<std::string, std::string>> lines;
		lines.reserve(previous.size());
		for (const auto& [name, newField] : next)
		{
			const auto old = previous.find(name);
			if (old == previous.end())
				continue;   // 新增字段:取新脚本默认值,不产生诊断。
			if (old->second.Type == newField.Type)
				continue;   // 同名同类型:BuildFieldCache 已保留旧值。
			lines.emplace_back(name,
				"[hot-reload] " + scriptPath + ": field '" + name + "' (id=" + FieldIdText(name) +
				") type changed " + LuaScriptField::GetLuaTypeName(old->second.Type) + " -> " +
				LuaScriptField::GetLuaTypeName(newField.Type) + "; value reset to the new default");
		}
		for (const auto& [name, oldField] : previous)
		{
			if (next.find(name) != next.end())
				continue;
			lines.emplace_back(name,
				"[hot-reload] " + scriptPath + ": field '" + name + "' (id=" + FieldIdText(name) +
				") is missing in the new script; its value was dropped");
		}
		std::sort(lines.begin(), lines.end(),
			[](const std::pair<std::string, std::string>& left, const std::pair<std::string, std::string>& right)
			{
				return left.first < right.first;
			});
		for (auto& line : lines)
			diagnostics->push_back(std::move(line.second));
	}

	std::string FormatScriptReloadFailure(const std::string& scriptPath, const char* phase,
		const std::string& error)
	{
		std::string message = "[hot-reload] ";
		message += scriptPath.empty() ? std::string("<unset script path>") : scriptPath;
		message += ": ";
		message += phase ? phase : "reload";
		message += " failed: ";
		message += error.empty() ? "unknown error" : error;
		return message;
	}
}
