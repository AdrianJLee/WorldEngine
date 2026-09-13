#pragma once

#include "World/Core/Vfs/Vfs.h"

#include <unordered_map>

namespace World::Vfs
{
	class PackageProvider final : public IVfsProvider
	{
	public:
		Source SourceType() const override { return Source::Package; }
		bool Open(const Path& path, std::vector<uint8_t>& out, std::error_code& ec) const override;
		bool Stat(const Path& path, StatInfo& out, std::error_code& ec) const override;

		static std::shared_ptr<PackageProvider> Open(const std::filesystem::path& pak,
		                                             std::error_code& ec);   // 失败返回 nullptr
		static bool BuildFromDirectory(const std::filesystem::path& srcDir,
		                               const std::filesystem::path& outPak,
		                               std::error_code& ec);

	private:
		struct Entry
		{
			uint64_t offset = 0;
			uint64_t size = 0;
		};

		PackageProvider(std::filesystem::path pak, std::unordered_map<std::string, Entry> entries);

		std::filesystem::path m_PakPath;
		std::unordered_map<std::string, Entry> m_Entries;
	};
}
