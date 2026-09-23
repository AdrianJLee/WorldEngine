#pragma once

#include "World/Core/Vfs/Vfs.h"

namespace World::Vfs
{
	class DirectoryProvider final : public IVfsProvider
	{
	public:
		explicit DirectoryProvider(std::filesystem::path root);
		Source SourceType() const override { return Source::Directory; }
		bool Open(const Path& path, std::vector<uint8_t>& out, std::error_code& ec) const override;
		bool Stat(const Path& path, StatInfo& out, std::error_code& ec) const override;

	private:
		const std::filesystem::path& CanonicalRoot() const;

		std::filesystem::path m_Root;
		mutable std::filesystem::path m_CanonicalRoot;
	};
}
