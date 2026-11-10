#include "wldpch.h"
#include "World/Gameplay/Framework/GlobalStateStore.h"

#include <yaml-cpp/yaml.h>

#include <fstream>
#include <sstream>

namespace World::Gameplay
{
	GlobalStateStore::GlobalStateStore(const std::filesystem::path& storageDirectory)
		: m_StorageDir(storageDirectory)
	{
	}

	GlobalStateStore::~GlobalStateStore() = default;

	void GlobalStateStore::SetStorageDirectory(const std::filesystem::path& dir)
	{
		m_StorageDir = dir;
	}

	void GlobalStateStore::AppendWal(const std::string& op, const std::string& type, const std::string& key, const std::string& val)
	{
		if (m_StorageDir.empty()) return;
		std::filesystem::create_directories(m_StorageDir);
		auto walPath = m_StorageDir / "global_state.wal";
		std::ofstream wal(walPath, std::ios::app);
		if (wal.is_open())
		{
			wal << op << "|" << type << "|" << key << "|" << val << "\n";
		}
	}

	void GlobalStateStore::SetString(const std::string& key, const std::string& val)
	{
		GlobalValue gv;
		gv.Type = GlobalValueType::String;
		gv.StringVal = val;
		m_Entries[key] = gv;
		AppendWal("SET", "STR", key, val);
	}

	std::string GlobalStateStore::GetString(const std::string& key, const std::string& fallback) const
	{
		auto it = m_Entries.find(key);
		if (it != m_Entries.end() && it->second.Type == GlobalValueType::String)
			return it->second.StringVal;
		return fallback;
	}

	void GlobalStateStore::SetNumber(const std::string& key, double val)
	{
		GlobalValue gv;
		gv.Type = GlobalValueType::Number;
		gv.NumberVal = val;
		m_Entries[key] = gv;
		AppendWal("SET", "NUM", key, std::to_string(val));
	}

	double GlobalStateStore::GetNumber(const std::string& key, double fallback) const
	{
		auto it = m_Entries.find(key);
		if (it != m_Entries.end() && it->second.Type == GlobalValueType::Number)
			return it->second.NumberVal;
		return fallback;
	}

	void GlobalStateStore::SetBool(const std::string& key, bool val)
	{
		GlobalValue gv;
		gv.Type = GlobalValueType::Boolean;
		gv.BoolVal = val;
		m_Entries[key] = gv;
		AppendWal("SET", "BOOL", key, val ? "1" : "0");
	}

	bool GlobalStateStore::GetBool(const std::string& key, bool fallback) const
	{
		auto it = m_Entries.find(key);
		if (it != m_Entries.end() && it->second.Type == GlobalValueType::Boolean)
			return it->second.BoolVal;
		return fallback;
	}

	bool GlobalStateStore::HasKey(const std::string& key) const
	{
		return m_Entries.find(key) != m_Entries.end();
	}

	void GlobalStateStore::DeleteKey(const std::string& key)
	{
		m_Entries.erase(key);
		AppendWal("DEL", "NONE", key, "");
	}

	void GlobalStateStore::Clear()
	{
		m_Entries.clear();
		AppendWal("CLR", "NONE", "", "");
	}

	bool GlobalStateStore::CommitSnapshot()
	{
		if (m_StorageDir.empty()) return false;
		try
		{
			std::filesystem::create_directories(m_StorageDir);
			auto snapshotPath = m_StorageDir / "global_state.wstate";

			YAML::Emitter out;
			out << YAML::BeginMap;
			out << YAML::Key << "version" << YAML::Value << 1;
			out << YAML::Key << "entries" << YAML::Value << YAML::BeginMap;

			for (const auto& [k, v] : m_Entries)
			{
				out << YAML::Key << k << YAML::Value << YAML::BeginMap;
				out << YAML::Key << "type" << YAML::Value << static_cast<int>(v.Type);
				switch (v.Type)
				{
				case GlobalValueType::String:
					out << YAML::Key << "val" << YAML::Value << v.StringVal;
					break;
				case GlobalValueType::Number:
					out << YAML::Key << "val" << YAML::Value << v.NumberVal;
					break;
				case GlobalValueType::Boolean:
					out << YAML::Key << "val" << YAML::Value << v.BoolVal;
					break;
				}
				out << YAML::EndMap;
			}
			out << YAML::EndMap;
			out << YAML::EndMap;

			std::ofstream fout(snapshotPath, std::ios::trunc);
			if (!fout.is_open()) return false;
			fout << out.c_str();
			fout.close();

			// Truncate WAL after snapshot is safely flushed to disk
			auto walPath = m_StorageDir / "global_state.wal";
			std::ofstream wal(walPath, std::ios::trunc);
			return true;
		}
		catch (...)
		{
			return false;
		}
	}

	bool GlobalStateStore::RecoverFromWal()
	{
		if (m_StorageDir.empty()) return false;
		auto snapshotPath = m_StorageDir / "global_state.wstate";
		auto walPath = m_StorageDir / "global_state.wal";

		m_Entries.clear();

		// 1. Load baseline snapshot if it exists
		if (std::filesystem::exists(snapshotPath))
		{
			try
			{
				YAML::Node root = YAML::LoadFile(snapshotPath.string());
				if (root && root["entries"] && root["entries"].IsMap())
				{
					for (auto it = root["entries"].begin(); it != root["entries"].end(); ++it)
					{
						std::string key = it->first.as<std::string>();
						auto entryNode = it->second;
						if (entryNode.IsMap() && entryNode["type"])
						{
							GlobalValue gv;
							gv.Type = static_cast<GlobalValueType>(entryNode["type"].as<int>());
							if (entryNode["val"])
							{
								if (gv.Type == GlobalValueType::String) gv.StringVal = entryNode["val"].as<std::string>();
								else if (gv.Type == GlobalValueType::Number) gv.NumberVal = entryNode["val"].as<double>();
								else if (gv.Type == GlobalValueType::Boolean) gv.BoolVal = entryNode["val"].as<bool>();
							}
							m_Entries[key] = gv;
						}
					}
				}
			}
			catch (...) {}
		}

		// 2. Replay uncommitted transactions from WAL
		if (std::filesystem::exists(walPath))
		{
			std::ifstream wal(walPath);
			std::string line;
			while (std::getline(wal, line))
			{
				if (line.empty()) continue;
				std::stringstream ss(line);
				std::string op, type, key, val;
				if (std::getline(ss, op, '|') &&
				    std::getline(ss, type, '|') &&
				    std::getline(ss, key, '|') &&
				    std::getline(ss, val))
				{
					if (op == "SET")
					{
						GlobalValue gv;
						if (type == "STR") { gv.Type = GlobalValueType::String; gv.StringVal = val; }
						else if (type == "NUM") { gv.Type = GlobalValueType::Number; gv.NumberVal = std::stod(val); }
						else if (type == "BOOL") { gv.Type = GlobalValueType::Boolean; gv.BoolVal = (val == "1"); }
						m_Entries[key] = gv;
					}
					else if (op == "DEL")
					{
						m_Entries.erase(key);
					}
					else if (op == "CLR")
					{
						m_Entries.clear();
					}
				}
			}
		}

		return true;
	}
}
