#include "wldpch.h"
#include "World/Core/Vfs/PackageProvider.h"
#include "World/Core/Vfs/Crc32.h"

#include <algorithm>
#include <cstring>

namespace World::Vfs
{
	namespace
	{
		constexpr uint32_t kMagic = 0x4B325057u;        // "WP2K"
		constexpr uint32_t kLegacyMagic = 0x4B415057u;  // "WPAK"
		constexpr uint32_t kVersion = 2;
		constexpr size_t kHeaderSize = 40;
		constexpr uint64_t kMaxEntryCount = 1u << 20;
		constexpr uint32_t kMaxPathLength = 1024;

		void WriteU32(uint8_t* out, uint32_t value)
		{
			out[0] = static_cast<uint8_t>(value & 0xFFu);
			out[1] = static_cast<uint8_t>((value >> 8) & 0xFFu);
			out[2] = static_cast<uint8_t>((value >> 16) & 0xFFu);
			out[3] = static_cast<uint8_t>((value >> 24) & 0xFFu);
		}

		void WriteU64(uint8_t* out, uint64_t value)
		{
			for (int i = 0; i < 8; ++i)
				out[i] = static_cast<uint8_t>((value >> (i * 8)) & 0xFFu);
		}

		uint32_t ReadU32(const uint8_t* in)
		{
			return static_cast<uint32_t>(in[0]) |
			       (static_cast<uint32_t>(in[1]) << 8) |
			       (static_cast<uint32_t>(in[2]) << 16) |
			       (static_cast<uint32_t>(in[3]) << 24);
		}

		uint64_t ReadU64(const uint8_t* in)
		{
			uint64_t value = 0;
			for (int i = 7; i >= 0; --i)
				value = (value << 8) | in[i];
			return value;
		}

		uint64_t Align4(uint64_t value)
		{
			return (value + 3u) & ~uint64_t{ 3 };
		}

		struct BuildItem
		{
			Path path;
			std::filesystem::path source;
			uint64_t size = 0;
			uint64_t offset = 0;
		};
	}

	PackageProvider::PackageProvider(std::filesystem::path pak, std::unordered_map<std::string, Entry> entries)
		: m_PakPath(std::move(pak)), m_Entries(std::move(entries))
	{
	}

	std::shared_ptr<PackageProvider> PackageProvider::Open(const std::filesystem::path& pak, std::error_code& ec)
	{
		ec.clear();

		std::ifstream file(pak, std::ios::binary);
		if (!file.is_open())
		{
			ec = std::make_error_code(std::errc::no_such_file_or_directory);
			return nullptr;
		}

		uint8_t header[kHeaderSize] = {};
		file.read(reinterpret_cast<char*>(header), kHeaderSize);
		if (file.gcount() != static_cast<std::streamsize>(kHeaderSize))
		{
			ec = std::make_error_code(std::errc::invalid_argument);
			return nullptr;
		}

		const uint32_t magic = ReadU32(header + 0);
		if (magic == kLegacyMagic)
		{
			ec = std::make_error_code(std::errc::not_supported);
			return nullptr;
		}
		if (magic != kMagic)
		{
			ec = std::make_error_code(std::errc::invalid_argument);
			return nullptr;
		}
		if (ReadU32(header + 4) != kVersion)
		{
			ec = std::make_error_code(std::errc::invalid_argument);
			return nullptr;
		}

		const uint64_t indexOffset = ReadU64(header + 8);
		const uint64_t indexSize = ReadU64(header + 16);
		const uint64_t entryCount = ReadU64(header + 24);
		const uint32_t storedCrc = ReadU32(header + 32);

		file.seekg(0, std::ios::end);
		const std::streamoff endPosition = file.tellg();
		if (endPosition < 0)
		{
			ec = std::make_error_code(std::errc::io_error);
			return nullptr;
		}
		const uint64_t fileSize = static_cast<uint64_t>(endPosition);

		if (entryCount > kMaxEntryCount ||
			indexOffset < kHeaderSize ||
			indexOffset > fileSize ||
			indexSize > fileSize - indexOffset)
		{
			ec = std::make_error_code(std::errc::invalid_argument);
			return nullptr;
		}

		file.seekg(static_cast<std::streamoff>(indexOffset), std::ios::beg);
		std::vector<uint8_t> indexBytes(static_cast<size_t>(indexSize));
		file.read(reinterpret_cast<char*>(indexBytes.data()), static_cast<std::streamsize>(indexSize));
		if (file.gcount() != static_cast<std::streamsize>(indexSize))
		{
			ec = std::make_error_code(std::errc::invalid_argument);
			return nullptr;
		}

		const uint64_t dataStart = indexOffset + indexSize;
		std::unordered_map<std::string, Entry> entries;
		entries.reserve(static_cast<size_t>(entryCount));

		size_t cursor = 0;
		for (uint64_t i = 0; i < entryCount; ++i)
		{
			if (cursor + 4 > indexBytes.size())
			{
				ec = std::make_error_code(std::errc::invalid_argument);
				return nullptr;
			}
			const uint32_t pathLength = ReadU32(indexBytes.data() + cursor);
			cursor += 4;
			if (pathLength == 0 || pathLength > kMaxPathLength ||
				cursor + pathLength + 16 > indexBytes.size())
			{
				ec = std::make_error_code(std::errc::invalid_argument);
				return nullptr;
			}
			const std::string rawPath(reinterpret_cast<const char*>(indexBytes.data() + cursor), pathLength);
			cursor += pathLength;

			Path normalized;
			if (!Normalize(rawPath, normalized, ec))
			{
				ec = std::make_error_code(std::errc::invalid_argument);
				return nullptr;
			}

			const uint64_t offset = ReadU64(indexBytes.data() + cursor);
			const uint64_t size = ReadU64(indexBytes.data() + cursor + 8);
			cursor += 16;

			if (offset < dataStart || offset > fileSize || size > fileSize - offset || (offset % 4) != 0)
			{
				ec = std::make_error_code(std::errc::invalid_argument);
				return nullptr;
			}
			if (!entries.emplace(std::move(normalized), Entry{ offset, size }).second)
			{
				ec = std::make_error_code(std::errc::invalid_argument);
				return nullptr;
			}
		}
		if (cursor != indexBytes.size())
		{
			ec = std::make_error_code(std::errc::invalid_argument);
			return nullptr;
		}

		// CRC 覆盖整个文件,Header 内 crc32 字段按 0 参与计算。
		uint8_t headerForCrc[kHeaderSize];
		std::memcpy(headerForCrc, header, kHeaderSize);
		headerForCrc[32] = headerForCrc[33] = headerForCrc[34] = headerForCrc[35] = 0;

		Crc32 crc;
		crc.Update(headerForCrc, kHeaderSize);

		file.seekg(static_cast<std::streamoff>(kHeaderSize), std::ios::beg);
		std::vector<char> buffer(1024 * 1024);   // 堆上分块缓冲,避免 1MB 栈帧溢出
		uint64_t remaining = fileSize - kHeaderSize;
		while (remaining > 0)
		{
			const std::streamsize chunk = static_cast<std::streamsize>(
				std::min<uint64_t>(remaining, buffer.size()));
			file.read(buffer.data(), chunk);
			if (file.gcount() != chunk)
			{
				ec = std::make_error_code(std::errc::invalid_argument);
				return nullptr;
			}
			crc.Update(buffer.data(), static_cast<size_t>(chunk));
			remaining -= static_cast<uint64_t>(chunk);
		}
		if (crc.Final() != storedCrc)
		{
			ec = std::make_error_code(std::errc::invalid_argument);
			return nullptr;
		}

		return std::shared_ptr<PackageProvider>(new PackageProvider(pak, std::move(entries)));
	}

	bool PackageProvider::BuildFromDirectory(const std::filesystem::path& srcDir,
	                                         const std::filesystem::path& outPak,
	                                         std::error_code& ec)
	{
		ec.clear();

		std::error_code fsEc;
		if (!std::filesystem::is_directory(srcDir, fsEc))
		{
			ec = fsEc ? fsEc : std::make_error_code(std::errc::not_a_directory);
			return false;
		}

		std::vector<BuildItem> items;
		{
			std::filesystem::recursive_directory_iterator it(
				srcDir, std::filesystem::directory_options::skip_permission_denied, fsEc);
			const std::filesystem::recursive_directory_iterator end;
			if (fsEc)
			{
				ec = fsEc;
				return false;
			}
			for (; it != end; it.increment(fsEc))
			{
				if (fsEc)
				{
					ec = fsEc;
					return false;
				}
				const std::filesystem::directory_entry& entry = *it;
				if (!entry.is_regular_file(fsEc))
				{
					if (fsEc)
					{
						ec = fsEc;
						return false;
					}
					continue;
				}
				Path relative;
				if (!Normalize(std::filesystem::relative(entry.path(), srcDir, fsEc).generic_string(), relative, ec))
				{
					if (fsEc)
						ec = fsEc;
					else
						ec = std::make_error_code(std::errc::invalid_argument);
					return false;
				}
				const uint64_t size = entry.file_size(fsEc);
				if (fsEc)
				{
					ec = fsEc;
					return false;
				}
				BuildItem item;
				item.path = std::move(relative);
				item.source = entry.path();
				item.size = size;
				items.push_back(std::move(item));
			}
		}

		std::sort(items.begin(), items.end(),
			[](const BuildItem& a, const BuildItem& b) { return a.path < b.path; });

		uint64_t indexSize = 0;
		for (const BuildItem& item : items)
			indexSize += 4 + item.path.size() + 16;

		const uint64_t dataStart = kHeaderSize + indexSize;
		uint64_t cursor = Align4(dataStart);
		for (BuildItem& item : items)
		{
			item.offset = cursor;
			cursor = Align4(cursor + item.size);
		}

		std::filesystem::path tempPath = outPak;
		tempPath += ".tmp";

		{
			std::ofstream out(tempPath, std::ios::binary | std::ios::trunc);
			if (!out.is_open())
			{
				ec = std::make_error_code(std::errc::io_error);
				return false;
			}

			std::array<uint8_t, kHeaderSize> header{};
			WriteU32(header.data() + 0, kMagic);
			WriteU32(header.data() + 4, kVersion);
			WriteU64(header.data() + 8, kHeaderSize);
			WriteU64(header.data() + 16, indexSize);
			WriteU64(header.data() + 24, items.size());
			// header[32..35] crc 暂为 0,header[36..39] pad 为 0。

			Crc32 crc;
			out.write(reinterpret_cast<const char*>(header.data()), static_cast<std::streamsize>(header.size()));
			crc.Update(header.data(), header.size());

			std::vector<uint8_t> index(static_cast<size_t>(indexSize));
			size_t indexCursor = 0;
			for (const BuildItem& item : items)
			{
				WriteU32(index.data() + indexCursor, static_cast<uint32_t>(item.path.size()));
				indexCursor += 4;
				std::memcpy(index.data() + indexCursor, item.path.data(), item.path.size());
				indexCursor += item.path.size();
				WriteU64(index.data() + indexCursor, item.offset);
				WriteU64(index.data() + indexCursor + 8, item.size);
				indexCursor += 16;
			}
			out.write(reinterpret_cast<const char*>(index.data()), static_cast<std::streamsize>(index.size()));
			crc.Update(index.data(), index.size());

			uint64_t written = kHeaderSize + indexSize;
			std::vector<char> buffer(4 * 1024 * 1024);
			for (const BuildItem& item : items)
			{
				while (written < item.offset)
				{
					const char zero = '\0';
					out.put(zero);
					crc.Update(&zero, 1);
					++written;
				}

				std::ifstream in(item.source, std::ios::binary);
				if (!in.is_open())
				{
					out.close();
					std::error_code cleanupEc;
					std::filesystem::remove(tempPath, cleanupEc);
					ec = std::make_error_code(std::errc::io_error);
					return false;
				}

				uint64_t remaining = item.size;
				while (remaining > 0)
				{
					const std::streamsize chunk = static_cast<std::streamsize>(
						std::min<uint64_t>(remaining, buffer.size()));
					in.read(buffer.data(), chunk);
					if (in.gcount() != chunk)
					{
						out.close();
						std::error_code cleanupEc;
						std::filesystem::remove(tempPath, cleanupEc);
						ec = std::make_error_code(std::errc::io_error);
						return false;
					}
					out.write(buffer.data(), chunk);
					if (!out)
					{
						out.close();
						std::error_code cleanupEc;
						std::filesystem::remove(tempPath, cleanupEc);
						ec = std::make_error_code(std::errc::io_error);
						return false;
					}
					crc.Update(buffer.data(), static_cast<size_t>(chunk));
					remaining -= static_cast<uint64_t>(chunk);
					written += static_cast<uint64_t>(chunk);
				}
			}

			const uint32_t finalCrc = crc.Final();
			uint8_t crcBytes[4];
			WriteU32(crcBytes, finalCrc);
			out.seekp(32, std::ios::beg);
			out.write(reinterpret_cast<const char*>(crcBytes), 4);
			out.flush();
			if (!out)
			{
				out.close();
				std::error_code cleanupEc;
				std::filesystem::remove(tempPath, cleanupEc);
				ec = std::make_error_code(std::errc::io_error);
				return false;
			}
		}

		// 先写临时文件再替换,失败不残留半成品;目标已存在时删除后重试。
		std::error_code renameEc;
		std::filesystem::rename(tempPath, outPak, renameEc);
		if (renameEc)
		{
			std::error_code removeEc;
			std::filesystem::remove(outPak, removeEc);
			std::filesystem::rename(tempPath, outPak, renameEc);
			if (renameEc)
			{
				std::error_code cleanupEc;
				std::filesystem::remove(tempPath, cleanupEc);
				ec = renameEc;
				return false;
			}
		}
		return true;
	}

	bool PackageProvider::Open(const Path& path, std::vector<uint8_t>& out, std::error_code& ec) const
	{
		out.clear();
		ec.clear();

		Path normalized;
		if (!Normalize(path, normalized, ec))
			return false;

		const auto it = m_Entries.find(normalized);
		if (it == m_Entries.end())
		{
			ec = std::make_error_code(std::errc::no_such_file_or_directory);
			return false;
		}

		std::ifstream file(m_PakPath, std::ios::binary);
		if (!file.is_open())
		{
			ec = std::make_error_code(std::errc::io_error);
			return false;
		}
		file.seekg(static_cast<std::streamoff>(it->second.offset), std::ios::beg);
		out.resize(static_cast<size_t>(it->second.size));
		if (it->second.size > 0)
		{
			file.read(reinterpret_cast<char*>(out.data()), static_cast<std::streamsize>(it->second.size));
			if (static_cast<uint64_t>(file.gcount()) != it->second.size)
			{
				out.clear();
				ec = std::make_error_code(std::errc::io_error);
				return false;
			}
		}
		return true;
	}

	bool PackageProvider::Stat(const Path& path, StatInfo& out, std::error_code& ec) const
	{
		out = StatInfo{};
		ec.clear();

		Path normalized;
		if (!Normalize(path, normalized, ec))
			return false;

		const auto it = m_Entries.find(normalized);
		if (it == m_Entries.end())
		{
			ec = std::make_error_code(std::errc::no_such_file_or_directory);
			return false;
		}
		out.isDirectory = false;
		out.size = it->second.size;
		return true;
	}
}
