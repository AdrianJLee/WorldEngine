#include "wldpch.h"

#include "World/Core/StringPool.h"

#include <filesystem>
#include <mutex>
#include <utility>

namespace World
{
	namespace
	{
		const std::string kEmpty;
	}

	StringPool& StringPool::Get()
	{
		// 函数内静态:首次调用时构造,随进程存活;符号经 WorldRuntime.dll 导出,
		// 因此 Editor / Runtime / Game 三个模块拿到的是同一个实例。
		static StringPool instance;
		return instance;
	}

	std::string StringPool::NormalizePath(std::string_view path)
	{
		if (path.empty())
			return std::string();

		// 先统一分隔符:Windows 反斜杠在 POSIX 语义下不是分隔符,先换掉再规范化。
		std::string unified(path);
		for (char& c : unified)
		{
			if (c == '\\')
				c = '/';
		}

		// 折叠 "." / ".." / 冗余分隔符(纯词法,不碰文件系统)。
		std::string normalized = std::filesystem::path(unified).lexically_normal().generic_string();

		// lexically_normal 保留末尾分隔符(如 "a/"),去掉它,保证 "a" 与 "a/" 同键。
		while (normalized.size() > 1 && normalized.back() == '/')
			normalized.pop_back();

		// 规范化后可能退化成 "."(输入 "./"):统一成空串。
		if (normalized == ".")
			normalized.clear();

		return normalized;
	}

	uint32_t StringPool::Intern(Table& table, const std::string& value)
	{
		if (value.empty())
			return 0;

		const auto found = table.Index.find(std::string_view(value));
		if (found != table.Index.end())
			return found->second;

		table.Storage.push_back(value);
		const uint32_t id = static_cast<uint32_t>(table.Storage.size());
		const std::string& stored = table.Storage.back();
		table.Index.emplace(std::string_view(stored), id);
		table.Bytes += stored.capacity() + 1 + sizeof(void*) * 2;   // 字符串 + 索引节点近似
		return id;
	}

	const std::string& StringPool::Lookup(const Table& table, uint32_t id)
	{
		if (id == 0 || id > table.Storage.size())
			return kEmpty;
		return table.Storage[static_cast<std::size_t>(id) - 1];
	}

	PathId StringPool::InternPath(std::string_view logicalPath)
	{
		const std::string normalized = NormalizePath(logicalPath);
		if (normalized.empty())
			return PathId {};

		std::unique_lock<std::shared_mutex> lock(m_Mutex);
		return PathId { Intern(m_Paths, normalized) };
	}

	NameId StringPool::InternName(std::string_view name)
	{
		if (name.empty())
			return NameId {};

		std::unique_lock<std::shared_mutex> lock(m_Mutex);
		return NameId { Intern(m_Names, std::string(name)) };
	}

	const std::string& StringPool::PathOf(PathId id) const
	{
		std::shared_lock<std::shared_mutex> lock(m_Mutex);
		return Lookup(m_Paths, id.Value);
	}

	const std::string& StringPool::NameOf(NameId id) const
	{
		std::shared_lock<std::shared_mutex> lock(m_Mutex);
		return Lookup(m_Names, id.Value);
	}

	std::size_t StringPool::Count() const
	{
		std::shared_lock<std::shared_mutex> lock(m_Mutex);
		return m_Paths.Storage.size() + m_Names.Storage.size();
	}

	std::size_t StringPool::BytesInUse() const
	{
		std::shared_lock<std::shared_mutex> lock(m_Mutex);
		return m_Paths.Bytes + m_Names.Bytes;
	}
}
