#include "wldpch.h"
#include "World/Core/Vfs/Vfs.h"

#include <algorithm>

namespace World::Vfs
{
	namespace
	{
		std::error_code InvalidArgument()
		{
			return std::make_error_code(std::errc::invalid_argument);
		}
	}

	bool Normalize(std::string_view raw, Path& out, std::error_code& ec)
	{
		out.clear();
		ec.clear();

		if (raw.empty() || raw.find('\0') != std::string_view::npos)
		{
			ec = InvalidArgument();
			return false;
		}

		// 绝对路径与 Windows 盘符直接拒绝(UNC 也以 '/' 或 '\\' 开头)。
		const char first = raw.front();
		if (first == '/' || first == '\\')
		{
			ec = InvalidArgument();
			return false;
		}
		if (raw.size() >= 2 &&
			((first >= 'a' && first <= 'z') || (first >= 'A' && first <= 'Z')) && raw[1] == ':')
		{
			ec = InvalidArgument();
			return false;
		}

		std::string normalized;
		normalized.reserve(raw.size());
		bool previousSlash = false;
		for (char ch : raw)
		{
			if (ch == '\\')
				ch = '/';
			if (ch == '/')
			{
				if (previousSlash)
					continue;
				previousSlash = true;
			}
			else
			{
				previousSlash = false;
			}
			normalized.push_back(ch);
		}
		while (!normalized.empty() && normalized.back() == '/')
			normalized.pop_back();

		if (normalized.empty())
		{
			ec = InvalidArgument();
			return false;
		}

		// 逐段检查:空段(折叠后不应出现)、"."、 ".." 一律拒绝。
		size_t segmentStart = 0;
		for (;;)
		{
			const size_t slash = normalized.find('/', segmentStart);
			const size_t segmentEnd = (slash == std::string::npos) ? normalized.size() : slash;
			const size_t length = segmentEnd - segmentStart;
			if (length == 0 ||
				(length == 1 && normalized[segmentStart] == '.') ||
				(length == 2 && normalized[segmentStart] == '.' && normalized[segmentStart + 1] == '.'))
			{
				ec = InvalidArgument();
				return false;
			}
			if (slash == std::string::npos)
				break;
			segmentStart = slash + 1;
		}

		out = std::move(normalized);
		return true;
	}

	MountId Vfs::Mount(std::string id, ProviderPtr provider, int priority)
	{
		if (id.empty() || !provider)
		{
			if (Log::GetCoreLogger())
				WLD_CORE_ERROR("Vfs::Mount: empty mount id or null provider");
			return 0;
		}
		for (const MountEntry& mount : m_Mounts)
		{
			if (mount.name == id)
			{
				if (Log::GetCoreLogger())
					WLD_CORE_ERROR("Vfs::Mount: duplicate mount id '{0}'", id);
				return 0;
			}
		}

		const MountId assigned = m_NextId++;
		MountEntry entry;
		entry.id = assigned;
		entry.name = std::move(id);
		entry.provider = std::move(provider);
		entry.priority = priority;
		entry.sequence = m_Sequence++;
		m_Mounts.push_back(std::move(entry));
		return assigned;
	}

	bool Vfs::Unmount(MountId id)
	{
		const auto it = std::find_if(m_Mounts.begin(), m_Mounts.end(),
			[id](const MountEntry& mount) { return mount.id == id; });
		if (it == m_Mounts.end())
			return false;
		m_Mounts.erase(it);
		return true;
	}

	const Vfs::MountEntry* Vfs::ResolveMount(const Path& normalized, StatInfo& out) const
	{
		std::vector<const MountEntry*> ordered;
		ordered.reserve(m_Mounts.size());
		for (const MountEntry& mount : m_Mounts)
			ordered.push_back(&mount);
		std::stable_sort(ordered.begin(), ordered.end(),
			[](const MountEntry* a, const MountEntry* b)
			{
				if (a->priority != b->priority)
					return a->priority > b->priority;
				return a->sequence > b->sequence;
			});

		for (const MountEntry* mount : ordered)
		{
			std::error_code localEc;
			if (mount->provider->Stat(normalized, out, localEc))
				return mount;
		}
		return nullptr;
	}

	bool Vfs::Resolve(const Path& path, StatInfo& out, std::error_code& ec) const
	{
		out = StatInfo{};
		ec.clear();

		Path normalized;
		if (!Normalize(path, normalized, ec))
			return false;

		const MountEntry* mount = ResolveMount(normalized, out);
		if (!mount)
		{
			ec = std::make_error_code(std::errc::no_such_file_or_directory);
			return false;
		}
		out.source = mount->provider->SourceType();
		out.mountId = mount->name;
		return true;
	}

	bool Vfs::Read(const Path& path, std::vector<uint8_t>& out, std::error_code& ec) const
	{
		out.clear();

		StatInfo stat;
		if (!Resolve(path, stat, ec))
			return false;

		const auto mount = std::find_if(m_Mounts.begin(), m_Mounts.end(),
			[&stat](const MountEntry& entry) { return entry.name == stat.mountId; });
		if (mount == m_Mounts.end())
		{
			ec = std::make_error_code(std::errc::no_such_file_or_directory);
			return false;
		}

		Path normalized;
		std::error_code normalizeEc;
		if (!Normalize(path, normalized, normalizeEc))
		{
			ec = normalizeEc;
			return false;
		}
		return mount->provider->Open(normalized, out, ec);
	}

	bool Vfs::Exists(const Path& path) const
	{
		StatInfo stat;
		std::error_code ec;
		return Stat(path, stat, ec);
	}
}
