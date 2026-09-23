#include "wldpch.h"
#include "World/Core/Asset/CookPipeline.h"
#include "World/WUI/WuiJson.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <unordered_map>

namespace World::Asset
{
	namespace
	{
		struct DatabaseEntry
		{
			uint64_t Fingerprint = 0;
			uint64_t Size = 0;
		};

		uint64_t Fnv1a64(const void* data, size_t size)
		{
			uint64_t hash = 14695981039346656037ULL;
			const auto* bytes = static_cast<const uint8_t*>(data);
			for (size_t i = 0; i < size; ++i)
			{
				hash ^= bytes[i];
				hash *= 1099511628211ULL;
			}
			return hash;
		}

		uint64_t Fnv1a64String(const std::string& text)
		{
			return Fnv1a64(text.data(), text.size());
		}

		std::string Hex(uint64_t value)
		{
			char buffer[24];
			std::snprintf(buffer, sizeof(buffer), "%016llx",
				static_cast<unsigned long long>(value));
			return buffer;
		}

		// 源内容指纹复合导入器身份 + 逐源导入设置:导入器升级或设置变化时产物失效重烘焙。
		uint64_t CompositeFingerprint(const IAssetImporter& importer, const std::vector<uint8_t>& bytes,
			const std::filesystem::path& source)
		{
			uint64_t hash = Fnv1a64(bytes.data(), bytes.size());
			hash ^= Fnv1a64String(importer.Name());
			hash *= 1099511628211ULL;
			hash ^= importer.Version();
			hash *= 1099511628211ULL;

			// P4-U11:逐源设置存在 .wmodel 的 meta 里 —— 用导入器自己的设置指纹参与,
			// "改了设置 → 重烘"由它负责(不存在第二份需要盯着的设置文件)。
			hash ^= importer.SettingsFingerprint(source);
			hash *= 1099511628211ULL;
			return hash;
		}

		bool LoadDatabase(const std::filesystem::path& path, std::unordered_map<std::string, DatabaseEntry>& out)
		{
			out.clear();
			std::ifstream stream(path, std::ios::binary);
			if (!stream)
				return true;   // 无数据库 = 全量烘焙
			std::string text((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
			std::string error;
			const std::optional<Wui::JsonValue> root = Wui::JsonValue::Parse(text, &error);
			if (!root)
				return false;
			const Wui::JsonValue* entries = root->Find("entries");
			if (!entries)
				return false;
			for (const Wui::JsonValue& node : entries->Array)
			{
				const std::string path = node.Find("path") ? node.Find("path")->AsString("") : "";
				const std::string fingerprint = node.Find("fingerprint") ? node.Find("fingerprint")->AsString("") : "";
				const uint64_t size = node.Find("size") ?
					static_cast<uint64_t>(node.Find("size")->AsNumber(0)) : 0;
				if (path.empty() || fingerprint.size() != 16)
					continue;
				DatabaseEntry entry;
				entry.Fingerprint = std::strtoull(fingerprint.c_str(), nullptr, 16);
				entry.Size = size;
				out[path] = entry;
			}
			return true;
		}

		bool SaveDatabase(const std::filesystem::path& path,
			const std::unordered_map<std::string, DatabaseEntry>& entries)
		{
			Wui::JsonValue root;
			root.type = Wui::JsonValue::Type::Object;
			Wui::JsonValue array;
			array.type = Wui::JsonValue::Type::Array;
			for (const auto& [logical, entry] : entries)
			{
				Wui::JsonValue node;
				node.type = Wui::JsonValue::Type::Object;
				node.Object.push_back({ "path", Wui::JsonValue::MakeString(logical) });
				node.Object.push_back({ "fingerprint", Wui::JsonValue::MakeString(Hex(entry.Fingerprint)) });
				node.Object.push_back({ "size", Wui::JsonValue::MakeString(std::to_string(entry.Size)) });
				array.Array.push_back(std::move(node));
			}
			root.Object.push_back({ "entries", std::move(array) });

			std::error_code ec;
			std::filesystem::create_directories(path.parent_path(), ec);
			if (ec)
				return false;
			std::ofstream stream(path, std::ios::binary | std::ios::trunc);
			if (!stream)
				return false;
			stream << root.Dump();
			return static_cast<bool>(stream);
		}

		bool WriteArtifact(const std::filesystem::path& outputDir, const std::string& logical,
			const std::vector<uint8_t>& data)
		{
			std::error_code ec;
			const std::filesystem::path target = outputDir / "cooked" / logical;
			std::filesystem::create_directories(target.parent_path(), ec);
			if (ec)
				return false;
			std::ofstream stream(target, std::ios::binary | std::ios::trunc);
			if (!stream)
				return false;
			stream.write(reinterpret_cast<const char*>(data.data()),
				static_cast<std::streamsize>(data.size()));
			return static_cast<bool>(stream);
		}
	}

	CookPipeline::CookPipeline(std::vector<std::shared_ptr<IAssetImporter>> importers)
		: m_Importers(std::move(importers))
	{
	}

	std::vector<CookEntryResult> CookPipeline::Cook(const ProjectManifest& manifest,
		const std::filesystem::path& manifestPath,
		const std::filesystem::path& outputDir,
		bool force,
		CookSummary* summary)
	{
		std::vector<CookEntryResult> results;
		CookSummary stats;

		std::unordered_map<std::string, DatabaseEntry> database;
		LoadDatabase(outputDir / "cook.db.json", database);
		std::unordered_map<std::string, DatabaseEntry> nextDatabase;

		const std::filesystem::path contentRoot = manifest.ResolveContentRoot(manifestPath);
		std::error_code walkEc;
		std::filesystem::recursive_directory_iterator it(
			contentRoot, std::filesystem::directory_options::skip_permission_denied, walkEc);
		const std::filesystem::recursive_directory_iterator end;
		if (walkEc)
		{
			CookEntryResult failure;
			failure.Path = contentRoot.string();
			failure.Failed = true;
			failure.Error = walkEc.message();
			results.push_back(std::move(failure));
			stats.Failed = 1;
			if (summary) *summary = stats;
			return results;
		}

		for (; it != end; it.increment(walkEc))
		{
			if (walkEc)
				break;
			const std::filesystem::directory_entry& entry = *it;
			std::error_code fileEc;
			if (!entry.is_regular_file(fileEc))
				continue;
			std::filesystem::path relative;
			{
				std::error_code relEc;
				relative = std::filesystem::relative(entry.path(), contentRoot, relEc);
				if (relEc)
					continue;
			}
			const std::string logical = relative.generic_string();
			CookEntryResult result;
			result.Path = logical;
			++stats.Total;

			const IAssetImporter* importer = nullptr;
			for (const auto& candidate : m_Importers)
				if (candidate && candidate->Matches(entry.path()))
				{
					importer = candidate.get();
					break;
				}
			if (!importer)
			{
				result.Failed = true;
				result.Error = "no importer matches " + logical;
				++stats.Failed;
				results.push_back(std::move(result));
				continue;
			}

			// 指纹:源文件字节 FNV-1a64 复合导入器身份。
			std::ifstream sourceStream(entry.path(), std::ios::binary);
			if (!sourceStream)
			{
				result.Failed = true;
				result.Error = "cannot open " + logical;
				++stats.Failed;
				results.push_back(std::move(result));
				continue;
			}
			std::vector<uint8_t> sourceBytes((std::istreambuf_iterator<char>(sourceStream)),
				std::istreambuf_iterator<char>());
			const uint64_t fingerprint = CompositeFingerprint(*importer, sourceBytes, entry.path());

			const auto existing = database.find(logical);
			if (!force && existing != database.end() && existing->second.Fingerprint == fingerprint)
			{
				// 未变化:数据库保留,产物保留。
				nextDatabase[logical] = existing->second;
				result.Changed = false;
				++stats.Skipped;
				results.push_back(std::move(result));
				continue;
			}

			std::error_code importEc;
			ImportRequest request;
			request.LogicalPath = logical;
			request.Source = entry.path();
			ImportResult imported = importer->Import(request, importEc);
			if (!imported.Ok || importEc)
			{
				result.Failed = true;
				result.Error = imported.Error.empty() ? importEc.message() : imported.Error;
				++stats.Failed;
				results.push_back(std::move(result));
				continue;
			}

			// D5b 多产物契约:Outputs 非空时 Data 必须为空(单产物路径行为不变)。
			std::vector<std::pair<std::string, const std::vector<uint8_t>*>> artifacts;
			artifacts.reserve(imported.Outputs.empty() ? 1u : imported.Outputs.size());
			if (!imported.Outputs.empty())
			{
				if (!imported.Data.empty())
				{
					result.Failed = true;
					result.Error = "importer '" + importer->Name() + "' returned both Data and Outputs for "
						+ logical + " (contract: Outputs non-empty requires Data empty)";
					++stats.Failed;
					results.push_back(std::move(result));
					continue;
				}
				for (const ImportOutput& output : imported.Outputs)
					artifacts.emplace_back(output.LogicalPath, &output.Data);
			}
			else
			{
				artifacts.emplace_back(logical, &imported.Data);
			}

			bool writeFailed = false;
			for (const auto& [logicalOutput, data] : artifacts)
			{
				if (logicalOutput.empty() || !data)
				{
					result.Failed = true;
					result.Error = "importer '" + importer->Name() + "' produced an empty logical path for "
						+ logical;
					writeFailed = true;
					break;
				}
				if (!WriteArtifact(outputDir, logicalOutput, *data))
				{
					result.Failed = true;
					result.Error = "cannot write cooked artifact for " + logicalOutput;
					writeFailed = true;
					break;
				}
			}
			if (writeFailed)
			{
				++stats.Failed;
				results.push_back(std::move(result));
				continue;
			}

			DatabaseEntry stored;
			stored.Fingerprint = fingerprint;
			stored.Size = imported.Outputs.empty() ? imported.Data.size()
				: static_cast<uint64_t>(imported.Outputs.size());
			nextDatabase[logical] = stored;
			result.Changed = true;
			++stats.Changed;
			results.push_back(std::move(result));
		}

		SaveDatabase(outputDir / "cook.db.json", nextDatabase);
		if (summary)
			*summary = stats;
		return results;
	}
}
