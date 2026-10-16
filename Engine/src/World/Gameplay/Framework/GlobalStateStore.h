#pragma once

#include "World/Core/Export.h"
#include <cstdint>
#include <filesystem>
#include <string>
#include <unordered_map>

namespace World::Gameplay
{
	enum class GlobalValueType : uint8_t
	{
		String = 0,
		Number = 1,
		Boolean = 2
	};

	struct GlobalValue
	{
		GlobalValueType Type = GlobalValueType::String;
		std::string StringVal;
		double NumberVal = 0.0;
		bool BoolVal = false;
	};

	class WLD_API GlobalStateStore
	{
	public:
		explicit GlobalStateStore(const std::filesystem::path& storageDirectory = "");
		~GlobalStateStore();

		void SetStorageDirectory(const std::filesystem::path& dir);
		const std::filesystem::path& GetStorageDirectory() const { return m_StorageDir; }

		void SetString(const std::string& key, const std::string& val);
		std::string GetString(const std::string& key, const std::string& fallback = "") const;

		void SetNumber(const std::string& key, double val);
		double GetNumber(const std::string& key, double fallback = 0.0) const;

		void SetBool(const std::string& key, bool val);
		bool GetBool(const std::string& key, bool fallback = false) const;

		bool HasKey(const std::string& key) const;
		void DeleteKey(const std::string& key);
		void Clear();

		size_t EntryCount() const { return m_Entries.size(); }

		// Write-Ahead-Log (WAL) 事务提交与故障恢复
		bool CommitSnapshot();
		bool RecoverFromWal();

	private:
		void AppendWal(const std::string& op, const std::string& type, const std::string& key, const std::string& val);

		std::filesystem::path m_StorageDir;
		std::unordered_map<std::string, GlobalValue> m_Entries;
	};
}
