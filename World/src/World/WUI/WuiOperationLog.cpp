#include "wldpch.h"
#include "World/WUI/WuiOperationLog.h"
#include "World/WUI/WuiJson.h"

#include <fstream>

namespace World::Wui
{
	void WuiOperationLog::Record(uint64_t frame, double timeMs, std::string category, std::string action, std::string target, std::string detail)
	{
		if (m_Records.size() >= MaxRecords)
			m_Records.erase(m_Records.begin(), m_Records.begin() + static_cast<ptrdiff_t>(m_Records.size() - MaxRecords + 1));
		m_Records.push_back({ m_NextSeq++, frame, timeMs, std::move(category), std::move(action), std::move(target), std::move(detail) });
	}

	void WuiOperationLog::Clear()
	{
		m_Records.clear();
		m_NextSeq = 1;
	}

	bool WuiOperationLog::Save(const std::filesystem::path& path, std::string* error) const
	{
		try
		{
			JsonValue root;
			root.type = JsonValue::Type::Array;
			for (const WuiOpRecord& record : m_Records)
			{
				JsonValue entry;
				entry.type = JsonValue::Type::Object;
				entry.Object.push_back({ "seq", JsonValue::MakeNumber(static_cast<double>(record.Seq)) });
				entry.Object.push_back({ "frame", JsonValue::MakeNumber(static_cast<double>(record.Frame)) });
				entry.Object.push_back({ "time", JsonValue::MakeNumber(record.TimeMs) });
				entry.Object.push_back({ "category", JsonValue::MakeString(record.Category) });
				entry.Object.push_back({ "action", JsonValue::MakeString(record.Action) });
				entry.Object.push_back({ "target", JsonValue::MakeString(record.Target) });
				entry.Object.push_back({ "detail", JsonValue::MakeString(record.Detail) });
				root.Array.push_back(std::move(entry));
			}
			std::ofstream stream(path, std::ios::binary | std::ios::trunc);
			if (!stream)
			{
				if (error) *error = "cannot open operation log for writing: " + path.string();
				return false;
			}
			stream << root.Dump();
			return stream.good();
		}
		catch (const std::exception& exception)
		{
			if (error) *error = exception.what();
			return false;
		}
	}
}
