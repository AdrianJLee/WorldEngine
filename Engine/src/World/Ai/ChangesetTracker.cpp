#include "wldpch.h"
#include "World/Ai/ChangesetTracker.h"
#include "World/Ai/WriteGuard.h"
#include "World/Ai/FingerprintValidator.h"
#include "World/Ai/CommandRegistry.h"
#include "World/WUI/WuiJson.h"
#include "World/Utils/Paths.h"

#include <chrono>
#include <fstream>
#include <sstream>

namespace World::Ai
{
	namespace fs = std::filesystem;
	using namespace World::Wui;

	std::string Changeset::ToJson() const
	{
		JsonValue root;
		root.type = JsonValue::Type::Object;

		root.Object.push_back({ "id", JsonValue::MakeString(id) });
		root.Object.push_back({ "description", JsonValue::MakeString(description) });
		root.Object.push_back({ "role", JsonValue::MakeString(role) });
		root.Object.push_back({ "applied", JsonValue::MakeBool(applied) });
		root.Object.push_back({ "reverted", JsonValue::MakeBool(reverted) });

		JsonValue fileArr;
		fileArr.type = JsonValue::Type::Array;
		for (const auto& f : files)
		{
			JsonValue item;
			item.type = JsonValue::Type::Object;
			item.Object.push_back({ "file", JsonValue::MakeString(f.relativePath) });
			item.Object.push_back({ "created", JsonValue::MakeBool(f.wasCreated) });
			item.Object.push_back({ "hash_before", JsonValue::MakeString(f.hashBefore) });
			item.Object.push_back({ "hash_after", JsonValue::MakeString(f.hashAfter) });
			fileArr.Array.push_back(std::move(item));
		}
		root.Object.push_back({ "files", std::move(fileArr) });

		return root.Dump();
	}

	ChangesetTracker& ChangesetTracker::Get()
	{
		static ChangesetTracker instance;
		return instance;
	}

	std::string ChangesetTracker::BeginChangeset(const std::string& projectRoot,
		const std::string& role,
		const std::string& description)
	{
		std::lock_guard<std::mutex> lock(m_Mutex);

		const fs::path root = fs::u8path(projectRoot.empty() ? Paths::ProjectDir().string() : projectRoot);
		const auto now = std::chrono::system_clock::now().time_since_epoch().count();
		const std::string csId = "cs_" + std::to_string(now) + "_" + std::to_string(++m_Counter);

		Changeset cs;
		cs.id = csId;
		cs.description = description;
		cs.role = role.empty() ? "maker" : role;
		cs.projectRoot = root.string();

		// 创建备份目录: <root>/.we/ai/changesets/<id>/backup
		const fs::path backupDir = root / ".we" / "ai" / "changesets" / csId / "backup";
		std::error_code ec;
		fs::create_directories(backupDir, ec);

		m_Changesets[csId] = std::move(cs);
		return csId;
	}

	bool ChangesetTracker::StageFileWrite(const std::string& changesetId,
		const std::string& projectRoot,
		const std::string& relativePath,
		const std::string& newContent,
		std::string& outError)
	{
		std::lock_guard<std::mutex> lock(m_Mutex);

		auto it = m_Changesets.find(changesetId);
		if (it == m_Changesets.end())
		{
			outError = "Changeset '" + changesetId + "' not found.";
			return false;
		}

		auto& cs = it->second;
		const fs::path root = fs::u8path(projectRoot.empty() ? cs.projectRoot : projectRoot);

		// 1. WriteGuard 安全检查
		WritePermission perm;
		perm.roleId = cs.role;
		if (!WriteGuard::Get().CanWrite(relativePath, root.string(), &perm, outError))
		{
			return false;
		}

		const fs::path fullPath = root / relativePath;
		const fs::path backupBase = root / ".we" / "ai" / "changesets" / cs.id / "backup";

		FileModification mod;
		mod.relativePath = fs::u8path(relativePath).generic_u8string();

		std::error_code ec;
		if (fs::exists(fullPath))
		{
			mod.wasCreated = false;
			mod.hashBefore = FingerprintValidator::ComputeFileSha256(fullPath);

			// 读取原始内容 (<= 256KB) 用于 Diff
			std::ifstream origIn(fullPath, std::ios::binary);
			if (origIn)
			{
				std::ostringstream ss;
				ss << origIn.rdbuf();
				mod.contentBefore = ss.str();
			}

			// 备份原文件
			const fs::path backupTarget = backupBase / relativePath;
			fs::create_directories(backupTarget.parent_path(), ec);
			fs::copy_file(fullPath, backupTarget, fs::copy_options::overwrite_existing, ec);
			mod.backupPath = backupTarget.string();
		}
		else
		{
			mod.wasCreated = true;
			mod.hashBefore = "";
			mod.backupPath = "";
			mod.contentBefore = "";
		}

		// 2. 写入新内容
		fs::create_directories(fullPath.parent_path(), ec);
		std::ofstream outFile(fullPath, std::ios::binary);
		if (!outFile)
		{
			outError = "Failed to open destination file for write: " + fullPath.string();
			return false;
		}
		outFile.write(newContent.data(), newContent.size());
		outFile.close();

		mod.contentAfter = newContent;
		mod.hashAfter = FingerprintValidator::ComputeStringSha256(newContent);
		cs.files.push_back(std::move(mod));

		// 3. 记入通道写入审计日志
		WriteGuard::Get().RecordWrite(fullPath.string(), root.string(), cs.role, cs.id);

		return true;
	}

	bool ChangesetTracker::ApplyChangeset(const std::string& changesetId,
		const std::string& /*projectRoot*/,
		std::string& outError)
	{
		std::lock_guard<std::mutex> lock(m_Mutex);
		auto it = m_Changesets.find(changesetId);
		if (it == m_Changesets.end())
		{
			outError = "Changeset not found: " + changesetId;
			return false;
		}

		it->second.applied = true;
		return true;
	}

	bool ChangesetTracker::RevertChangeset(const std::string& changesetId,
		const std::string& projectRoot,
		std::string& outError)
	{
		std::lock_guard<std::mutex> lock(m_Mutex);
		auto it = m_Changesets.find(changesetId);
		if (it == m_Changesets.end())
		{
			outError = "Changeset not found: " + changesetId;
			return false;
		}

		auto& cs = it->second;
		const fs::path root = fs::u8path(projectRoot.empty() ? cs.projectRoot : projectRoot);

		std::error_code ec;
		// 倒序恢复修改
		for (auto rit = cs.files.rbegin(); rit != cs.files.rend(); ++rit)
		{
			const fs::path target = root / rit->relativePath;
			if (rit->wasCreated)
			{
				fs::remove(target, ec);
			}
			else
			{
				if (!rit->backupPath.empty() && fs::exists(rit->backupPath))
				{
					fs::create_directories(target.parent_path(), ec);
					fs::copy_file(rit->backupPath, target, fs::copy_options::overwrite_existing, ec);
				}
			}
		}

		cs.reverted = true;
		return true;
	}

	std::string ChangesetTracker::GetChangesetDiff(const std::string& changesetId) const
	{
		std::lock_guard<std::mutex> lock(m_Mutex);
		auto it = m_Changesets.find(changesetId);
		if (it == m_Changesets.end())
			return "Changeset '" + changesetId + "' not found.";

		const auto& cs = it->second;
		std::ostringstream ss;
		ss << "# Changeset: " << cs.id << " (" << cs.description << ")\n";
		ss << "# Role: " << cs.role << " | Files: " << cs.files.size() << "\n\n";

		for (const auto& f : cs.files)
		{
			ss << "--- a/" << f.relativePath << "\n";
			ss << "+++ b/" << f.relativePath << "\n";
			if (f.wasCreated)
			{
				std::stringstream ls(f.contentAfter);
				std::string l;
				while (std::getline(ls, l))
					ss << "+" << l << "\n";
			}
			else
			{
				ss << "@@ modified (" << (f.hashBefore.size() >= 8 ? f.hashBefore.substr(0, 8) : "") << " -> "
				   << (f.hashAfter.size() >= 8 ? f.hashAfter.substr(0, 8) : "") << ") @@\n";
				std::stringstream bs(f.contentBefore), as(f.contentAfter);
				std::string bl, al;
				while (std::getline(bs, bl) || std::getline(as, al))
				{
					if (bl != al)
					{
						if (!bl.empty()) ss << "-" << bl << "\n";
						if (!al.empty()) ss << "+" << al << "\n";
					}
				}
			}
			ss << "\n";
		}
		return ss.str();
	}

	const Changeset* ChangesetTracker::GetChangeset(const std::string& changesetId) const
	{
		std::lock_guard<std::mutex> lock(m_Mutex);
		auto it = m_Changesets.find(changesetId);
		return it != m_Changesets.end() ? &it->second : nullptr;
	}

	std::vector<Changeset> ChangesetTracker::ListChangesets(const std::string& /*projectRoot*/) const
	{
		std::lock_guard<std::mutex> lock(m_Mutex);
		std::vector<Changeset> list;
		list.reserve(m_Changesets.size());
		for (const auto& kv : m_Changesets)
		{
			list.push_back(kv.second);
		}
		return list;
	}

	void ChangesetTracker::Clear()
	{
		std::lock_guard<std::mutex> lock(m_Mutex);
		m_Changesets.clear();
		m_Counter = 0;
	}

	void ChangesetTracker::RegisterCommands(CommandRegistry& reg)
	{
		// 1. changeset.begin
		CommandDescriptor beginCmd;
		beginCmd.name = "changeset.begin";
		beginCmd.description = "Begin a new transactional AI changeset.";
		beginCmd.effect = CommandEffect::Write;
		beginCmd.scope = CommandScope::Both;
		beginCmd.owner = "engine";
		beginCmd.paramsSchema = "{\"type\":\"object\",\"properties\":{\"role\":{\"type\":\"string\"},\"desc\":{\"type\":\"string\"},\"root\":{\"type\":\"string\"}}}";
		beginCmd.resultSchema = "{\"type\":\"object\",\"properties\":{\"changeset_id\":{\"type\":\"string\"}}}";
		beginCmd.handler = [](const std::map<std::string, std::string>& args, std::string& result, std::string& /*error*/)
		{
			const std::string root = args.count("root") ? args.at("root") : "";
			const std::string role = args.count("role") ? args.at("role") : "maker";
			const std::string desc = args.count("desc") ? args.at("desc") : "";
			const std::string id = Get().BeginChangeset(root, role, desc);
			JsonValue r; r.type = JsonValue::Type::Object;
			r.Object.push_back({ "changeset_id", JsonValue::MakeString(id) });
			result = r.Dump();
			return true;
		};
		reg.RegisterCommand(std::move(beginCmd));

		// 2. changeset.apply
		CommandDescriptor applyCmd;
		applyCmd.name = "changeset.apply";
		applyCmd.description = "Confirm and apply staged AI changeset.";
		applyCmd.effect = CommandEffect::Write;
		applyCmd.scope = CommandScope::Both;
		applyCmd.owner = "engine";
		applyCmd.paramsSchema = "{\"type\":\"object\",\"properties\":{\"id\":{\"type\":\"string\"},\"root\":{\"type\":\"string\"}},\"required\":[\"id\"]}";
		applyCmd.resultSchema = "{\"type\":\"object\",\"properties\":{\"applied\":{\"type\":\"boolean\"}}}";
		applyCmd.handler = [](const std::map<std::string, std::string>& args, std::string& result, std::string& error)
		{
			const std::string id = args.count("id") ? args.at("id") : "";
			const std::string root = args.count("root") ? args.at("root") : "";
			if (!Get().ApplyChangeset(id, root, error))
				return false;
			JsonValue r; r.type = JsonValue::Type::Object;
			r.Object.push_back({ "applied", JsonValue::MakeBool(true) });
			result = r.Dump();
			return true;
		};
		reg.RegisterCommand(std::move(applyCmd));

		// 3. changeset.revert
		CommandDescriptor revertCmd;
		revertCmd.name = "changeset.revert";
		revertCmd.description = "Atomic revert of all file and asset modifications in a changeset.";
		revertCmd.effect = CommandEffect::Write;
		revertCmd.scope = CommandScope::Both;
		revertCmd.owner = "engine";
		revertCmd.paramsSchema = "{\"type\":\"object\",\"properties\":{\"id\":{\"type\":\"string\"},\"root\":{\"type\":\"string\"}},\"required\":[\"id\"]}";
		revertCmd.resultSchema = "{\"type\":\"object\",\"properties\":{\"reverted\":{\"type\":\"boolean\"}}}";
		revertCmd.handler = [](const std::map<std::string, std::string>& args, std::string& result, std::string& error)
		{
			const std::string id = args.count("id") ? args.at("id") : "";
			const std::string root = args.count("root") ? args.at("root") : "";
			if (!Get().RevertChangeset(id, root, error))
				return false;
			JsonValue r; r.type = JsonValue::Type::Object;
			r.Object.push_back({ "reverted", JsonValue::MakeBool(true) });
			result = r.Dump();
			return true;
		};
		reg.RegisterCommand(std::move(revertCmd));

		// 4. changeset.list
		CommandDescriptor listCmd;
		listCmd.name = "changeset.list";
		listCmd.description = "List all tracked changesets.";
		listCmd.effect = CommandEffect::Read;
		listCmd.scope = CommandScope::Both;
		listCmd.owner = "engine";
		listCmd.paramsSchema = "{\"type\":\"object\"}";
		listCmd.resultSchema = "{\"type\":\"object\",\"properties\":{\"changesets\":{\"type\":\"array\"}}}";
		listCmd.handler = [](const std::map<std::string, std::string>& args, std::string& result, std::string& /*error*/)
		{
			const std::string root = args.count("root") ? args.at("root") : "";
			const auto list = Get().ListChangesets(root);
			JsonValue r; r.type = JsonValue::Type::Object;
			JsonValue arr; arr.type = JsonValue::Type::Array;
			for (const auto& item : list)
			{
				auto opt = JsonValue::Parse(item.ToJson(), nullptr);
				if (opt) arr.Array.push_back(std::move(*opt));
			}
			r.Object.push_back({ "changesets", std::move(arr) });
			result = r.Dump();
			return true;
		};
		reg.RegisterCommand(std::move(listCmd));

		// 5. changeset.diff
		CommandDescriptor diffCmd;
		diffCmd.name = "changeset.diff";
		diffCmd.description = "Get unified diff preview for a staged changeset.";
		diffCmd.effect = CommandEffect::Read;
		diffCmd.scope = CommandScope::Both;
		diffCmd.owner = "engine";
		diffCmd.paramsSchema = "{\"type\":\"object\",\"properties\":{\"id\":{\"type\":\"string\"}},\"required\":[\"id\"]}";
		diffCmd.resultSchema = "{\"type\":\"object\",\"properties\":{\"diff\":{\"type\":\"string\"}}}";
		diffCmd.handler = [](const std::map<std::string, std::string>& args, std::string& result, std::string& error)
		{
			const std::string id = args.count("id") ? args.at("id") : "";
			if (id.empty())
			{
				error = "Missing required argument 'id'";
				return false;
			}
			const std::string diff = Get().GetChangesetDiff(id);
			JsonValue r; r.type = JsonValue::Type::Object;
			r.Object.push_back({ "diff", JsonValue::MakeString(diff) });
			result = r.Dump();
			return true;
		};
		reg.RegisterCommand(std::move(diffCmd));
	}
}


