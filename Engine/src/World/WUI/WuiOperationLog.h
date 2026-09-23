#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace World::Wui
{
	struct WuiOpRecord
	{
		uint64_t Seq = 0;
		uint64_t Frame = 0;
		double TimeMs = 0;
		std::string Category;
		std::string Action;
		std::string Target;
		std::string Detail;
	};

	// 追加式操作记录:定位交互问题、审计与后续撤销系统的数据基础。
	class WuiOperationLog
	{
	public:
		static constexpr size_t MaxRecords = 512;

		void Record(uint64_t frame, double timeMs, std::string category, std::string action, std::string target, std::string detail);
		void Clear();
		// 时间序(旧→新);最近的在末尾。
		const std::vector<WuiOpRecord>& Records() const { return m_Records; }
		size_t Count() const { return m_Records.size(); }
		bool Save(const std::filesystem::path& path, std::string* error) const;

	private:
		std::vector<WuiOpRecord> m_Records;
		uint64_t m_NextSeq = 1;
	};
}
