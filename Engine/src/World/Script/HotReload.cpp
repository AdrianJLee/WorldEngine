#include "wldpch.h"
#include "World/Script/HotReload.h"

#include "World/Core/Application.h"
#include "World/Script/BehaviorRegistry.h"
#include "World/Script/ScriptProperties.h"

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

		// W8 脚手架固定内容:与入库的 projects/default/.vscode/settings.json、projects/default/.luau-lsp/config.json
		// 逐字节一致(World.LuauStubSchema 的漂移门禁只盯存根,这两份由 World.ScriptWorkflow 对照)。
		// luau-lsp 的路径相对工作区根(项目目录,清单与 .vscode/ 同级):存根在 assets/scripts/intermediate/ 下。
		constexpr const char* kEditorVsCodeSettings =
			"{\n"
			"  \"luau-lsp.types.definitionFiles\": [\n"
			"    \"assets/scripts/intermediate/WorldEngineAPI.luau\"\n"
			"  ],\n"
			"  \"luau-lsp.types.ignoreGlobs\": [\n"
			"    \"assets/scripts/intermediate/**\"\n"
			"  ],\n"
			"  \"files.associations\": {\n"
			"    \"*.luau\": \"luau\"\n"
			"  }\n"
			"}\n";
		constexpr const char* kEditorLuauLspConfig =
			"{\n"
			"  \"definitions\": [\n"
			"    \"assets/scripts/intermediate/WorldEngineAPI.luau\"\n"
			"  ],\n"
			"  \"ignoreGlobs\": [\n"
			"    \"assets/scripts/intermediate/**\"\n"
			"  ]\n"
			"}\n";

		bool WriteFileIfMissing(const std::filesystem::path& path, const char* content, std::string* error)
		{
			std::error_code existsError;
			if (std::filesystem::exists(path, existsError))
				return true;   // 已存在(无论内容)= 用户文件:create-if-missing 绝不覆盖
			std::error_code directoryError;
			if (!path.parent_path().empty())
				std::filesystem::create_directories(path.parent_path(), directoryError);
			std::ofstream file(path, std::ios::binary | std::ios::out | std::ios::trunc);
			if (!file.is_open())
			{
				if (error) *error = "cannot create " + path.string();
				return false;
			}
			file << content;
			file.flush();
			if (!file.good())
			{
				if (error) *error = "cannot write " + path.string();
				return false;
			}
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

	bool ResolveScriptSourceBytes(const std::string& logicalPath, std::vector<uint8_t>& out, std::string* error)
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
				out = std::move(bytes);
				if (error) error->clear();
				return true;
			}
		}

		// 2) 磁盘回退(与 ScriptEngine::ReadScriptBytes 同一位置)。
		std::string diskSource;
		if (!ReadDiskFile(DiskPathFor(logicalPath), diskSource))
		{
			if (error) *error = "script not found: " + logicalPath;
			return false;
		}
		out.assign(diskSource.begin(), diskSource.end());
		if (error) error->clear();
		return true;
	}

	bool ResolveScriptSource(const std::string& logicalPath, std::string& source, std::string* error)
	{
		std::vector<uint8_t> bytes;
		if (!ResolveScriptSourceBytes(logicalPath, bytes, error))
			return false;
		source.assign(bytes.begin(), bytes.end());
		return true;
	}

	// W8:ResolveScriptDiskPath 的实现放在 Scene/ScriptEngine.cpp —— 它要复用 ReadScriptBytes
	// 的"内容上下文优先"VFS 选择顺序,而登记的内容上下文是 ScriptEngine.cpp 的文件内静态。

	bool EnsureScriptEditorScaffold(const std::filesystem::path& contentRoot, std::string* error)
	{
		if (contentRoot.empty())
		{
			if (error) *error = "content root is empty";
			return false;
		}
		if (!WriteFileIfMissing(contentRoot / ".vscode" / "settings.json", kEditorVsCodeSettings, error))
			return false;
		if (!WriteFileIfMissing(contentRoot / ".luau-lsp" / "config.json", kEditorLuauLspConfig, error))
			return false;
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

		// 内容哈希优先(容器字节/源码字节都走同一份字节):同内容重写(只动 mtime)不产生变化。
		std::vector<uint8_t> bytes;
		std::string readError;
		if (ResolveScriptSourceBytes(logicalPath, bytes, &readError))
		{
			result.Value = FingerprintScriptBytes(bytes.data(), bytes.size());
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
		const std::vector<ScriptProperty>& previous,
		const std::vector<ScriptProperty>& next,
		const std::string& scriptPath,
		std::vector<std::string>* diagnostics)
	{
		if (!diagnostics)
			return;

		// 先按字段名收集再排序,保证诊断文本可断言/可复现(与容器顺序无关)。
		std::vector<std::pair<std::string, std::string>> lines;
		lines.reserve(previous.size());
		for (const ScriptProperty& newField : next)
		{
			const ScriptProperty* old = ScriptProperties::Find(previous, newField.Name);
			if (!old)
				continue;   // 新增字段:取新脚本默认值,不产生诊断。
			if (old->Type == newField.Type)
				continue;   // 同名同类型:同步时已保留旧值。
			lines.emplace_back(newField.Name,
				"[hot-reload] " + scriptPath + ": field '" + newField.Name + "' (id=" + FieldIdText(newField.Name) +
				") type changed " + ScriptProperties::KindName(old->Type) + " -> " +
				ScriptProperties::KindName(newField.Type) + "; value reset to the new default");
		}
		for (const ScriptProperty& oldField : previous)
		{
			if (ScriptProperties::Find(next, oldField.Name))
				continue;
			lines.emplace_back(oldField.Name,
				"[hot-reload] " + scriptPath + ": field '" + oldField.Name + "' (id=" + FieldIdText(oldField.Name) +
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
