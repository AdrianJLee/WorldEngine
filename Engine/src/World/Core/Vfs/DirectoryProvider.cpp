#include "wldpch.h"
#include "World/Core/Vfs/DirectoryProvider.h"

namespace World::Vfs
{
	namespace
	{
		// 逐组件比较:resolved 必须以 root 为前缀(严格目录树内,防符号链接逃逸)。
		bool IsUnderRoot(const std::filesystem::path& resolved, const std::filesystem::path& root)
		{
			auto a = resolved.begin();
			auto b = root.begin();
			for (; b != root.end(); ++a, ++b)
			{
				if (a == resolved.end() || *a != *b)
					return false;
			}
			return true;
		}
	}

	DirectoryProvider::DirectoryProvider(std::filesystem::path root)
		: m_Root(std::filesystem::absolute(root).lexically_normal())
	{
	}

	const std::filesystem::path& DirectoryProvider::CanonicalRoot() const
	{
		if (m_CanonicalRoot.empty())
		{
			std::error_code ec;
			const std::filesystem::path canonical = std::filesystem::canonical(m_Root, ec);
			m_CanonicalRoot = ec ? std::filesystem::absolute(m_Root).lexically_normal() : canonical;
		}
		return m_CanonicalRoot;
	}

	bool DirectoryProvider::Open(const Path& path, std::vector<uint8_t>& out, std::error_code& ec) const
	{
		out.clear();
		ec.clear();

		Path normalized;
		if (!Normalize(path, normalized, ec))
			return false;

		std::error_code fsEc;
		const std::filesystem::path candidate = m_Root / std::filesystem::path(normalized);
		const std::filesystem::path resolved = std::filesystem::weakly_canonical(candidate, fsEc);
		if (fsEc)
		{
			ec = fsEc;
			return false;
		}
		if (!IsUnderRoot(resolved, CanonicalRoot()))
		{
			ec = std::make_error_code(std::errc::permission_denied);
			return false;
		}
		if (!std::filesystem::is_regular_file(resolved, fsEc))
		{
			if (fsEc)
				ec = fsEc;
			else
				ec = std::make_error_code(std::errc::no_such_file_or_directory);
			return false;
		}
		const uint64_t size = std::filesystem::file_size(resolved, fsEc);
		if (fsEc)
		{
			ec = fsEc;
			return false;
		}

		std::ifstream file(resolved, std::ios::binary);
		if (!file.is_open())
		{
			ec = std::make_error_code(std::errc::io_error);
			return false;
		}
		out.resize(static_cast<size_t>(size));
		if (size > 0)
		{
			file.read(reinterpret_cast<char*>(out.data()), static_cast<std::streamsize>(size));
			if (static_cast<uint64_t>(file.gcount()) != size)
			{
				out.clear();
				ec = std::make_error_code(std::errc::io_error);
				return false;
			}
		}
		return true;
	}

	bool DirectoryProvider::Stat(const Path& path, StatInfo& out, std::error_code& ec) const
	{
		out = StatInfo{};
		ec.clear();

		Path normalized;
		if (!Normalize(path, normalized, ec))
			return false;

		std::error_code fsEc;
		const std::filesystem::path candidate = m_Root / std::filesystem::path(normalized);
		const std::filesystem::path resolved = std::filesystem::weakly_canonical(candidate, fsEc);
		if (fsEc)
		{
			ec = fsEc;
			return false;
		}
		if (!IsUnderRoot(resolved, CanonicalRoot()))
		{
			ec = std::make_error_code(std::errc::permission_denied);
			return false;
		}

		const std::filesystem::file_status status = std::filesystem::status(resolved, fsEc);
		if (fsEc)
		{
			ec = fsEc;
			return false;
		}
		if (std::filesystem::is_directory(status))
		{
			out.isDirectory = true;
			return true;
		}
		if (std::filesystem::is_regular_file(status))
		{
			const uint64_t size = std::filesystem::file_size(resolved, fsEc);
			if (fsEc)
			{
				ec = fsEc;
				return false;
			}
			out.size = size;
			return true;
		}
		ec = std::make_error_code(std::errc::no_such_file_or_directory);
		return false;
	}
}
