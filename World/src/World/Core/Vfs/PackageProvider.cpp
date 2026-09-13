#include "wldpch.h"
#include "World/Core/Vfs/PackageProvider.h"
#include "World/Core/Vfs/Crc32.h"
#include "World/WUI/WuiJson.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>

namespace World::Vfs
{
	namespace
	{
		// 包格式 v2("WPAK2"):
		//   header(16B): uint32 magic="WPAK" uint32 version=2 uint32 jsonLen uint32 indexCrc32
		//   index: jsonLen 字节 JSON { "version":2, "entries":[ {path,hash,offset,size,crc32}... ] }
		//   blobs: 紧随 index,offset 为文件内绝对偏移
		// 每条目带内容指纹(FNV-1a 64 十六进制)与独立 CRC32;索引整体另有 CRC32;
		// 旧 v1 包返回 not_supported。
		constexpr uint32_t kMagic = 0x4B415057u;  // "WPAK"
		constexpr uint32_t kVersion = 2;
		constexpr uint32_t kLegacyVersion = 1;
		constexpr size_t kHeaderSize = 16;
		constexpr uint64_t kMaxEntryCount = 1u << 20;

		void WriteU32(uint8_t* out, uint32_t value)
		{
			out[0] = static_cast<uint8_t>(value & 0xFFu);
			out[1] = static_cast<uint8_t>((value >> 8) & 0xFFu);
			out[2] = static_cast<uint8_t>((value >> 16) & 0xFFu);
			out[3] = static_cast<uint8_t>((value >> 24) & 0xFFu);
		}

		uint32_t ReadU32(const uint8_t* in)
		{
			return static_cast<uint32_t>(in[0]) |
			       (static_cast<uint32_t>(in[1]) << 8) |
			       (static_cast<uint32_t>(in[2]) << 16) |
			       (static_cast<uint32_t>(in[3]) << 24);
		}

		uint64_t Fnv1a64(const void* data, size_t size)
		{
			uint64_t hash = 14695981039346656037ULL;
			const auto* bytes = static_cast<const uint8_t*>(data);
			for (size_t i = 0; i < size; ++i)
			{
				hash ^= bytes[i];
				hash *= 1099511628211ULL;
			}
			return hash;
		}

		std::string FnvHex(uint64_t hash)
		{
			char buffer[24];
			std::snprintf(buffer, sizeof(buffer), "%016llx",
				static_cast<unsigned long long>(hash));
			return buffer;
		}

		struct BuildItem
		{
			Path path;
			std::filesystem::path source;
			uint64_t size = 0;
			uint64_t offset = 0;
			uint32_t crc32 = 0;
			std::string hash;
		};

		// 内容指纹与 CRC 的第一遍读取。
		bool FingerprintItem(BuildItem& item, std::error_code& ec)
		{
			std::ifstream in(item.source, std::ios::binary);
			if (!in.is_open())
			{
				ec = std::make_error_code(std::errc::io_error);
				return false;
			}
			Crc32 crc;
			uint64_t hash = 14695981039346656037ULL;
			std::vector<char> buffer(4 * 1024 * 1024);
			uint64_t remaining = item.size;
			while (remaining > 0)
			{
				const std::streamsize chunk = static_cast<std::streamsize>(
					std::min<uint64_t>(remaining, buffer.size()));
				in.read(buffer.data(), chunk);
				if (in.gcount() != chunk)
				{
					ec = std::make_error_code(std::errc::io_error);
					return false;
				}
				crc.Update(buffer.data(), static_cast<size_t>(chunk));
				const auto* bytes = reinterpret_cast<const uint8_t*>(buffer.data());
				for (std::streamsize i = 0; i < chunk; ++i)
				{
					hash ^= bytes[i];
					hash *= 1099511628211ULL;
				}
				remaining -= static_cast<uint64_t>(chunk);
			}
			item.crc32 = crc.Final();
			item.hash = FnvHex(hash);
			return true;
		}

		std::string SerializeIndex(const std::vector<BuildItem>& items)
		{
			Wui::JsonValue root;
			root.type = Wui::JsonValue::Type::Object;
			root.Object.push_back({ "version", Wui::JsonValue::MakeNumber(kVersion) });
			Wui::JsonValue entries;
			entries.type = Wui::JsonValue::Type::Array;
			for (const BuildItem& item : items)
			{
				Wui::JsonValue entry;
				entry.type = Wui::JsonValue::Type::Object;
				entry.Object.push_back({ "path", Wui::JsonValue::MakeString(item.path) });
				entry.Object.push_back({ "hash", Wui::JsonValue::MakeString(item.hash) });
				// 大整数以十进制字符串存储,避免 JSON 数字经 double 丢失精度。
				entry.Object.push_back({ "offset", Wui::JsonValue::MakeString(std::to_string(item.offset)) });
				entry.Object.push_back({ "size", Wui::JsonValue::MakeString(std::to_string(item.size)) });
				entry.Object.push_back({ "crc32", Wui::JsonValue::MakeString(std::to_string(item.crc32)) });
				entries.Array.push_back(std::move(entry));
			}
			root.Object.push_back({ "entries", std::move(entries) });
			return root.Dump();
		}

		// 索引长度与 offset 互相依赖(offset 数字位数随 jsonLen 变化),定点迭代收敛。
		uint64_t LayoutIndex(std::vector<BuildItem>& items)
		{
			uint64_t jsonLen = 0;
			for (int attempt = 0; attempt < 4; ++attempt)
			{
				uint64_t cursor = kHeaderSize + jsonLen;
				for (BuildItem& item : items)
				{
					item.offset = cursor;
					cursor += item.size;
				}
				const std::string text = SerializeIndex(items);
				if (static_cast<uint64_t>(text.size()) == jsonLen)
					return jsonLen;
				jsonLen = text.size();
			}
			return jsonLen;
		}
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

		if (ReadU32(header + 0) != kMagic)
		{
			ec = std::make_error_code(std::errc::invalid_argument);
			return nullptr;
		}
		const uint32_t version = ReadU32(header + 4);
		if (version == kLegacyVersion)
		{
			ec = std::make_error_code(std::errc::not_supported);
			return nullptr;
		}
		if (version != kVersion)
		{
			ec = std::make_error_code(std::errc::invalid_argument);
			return nullptr;
		}

		const uint64_t jsonLen = ReadU32(header + 8);
		const uint32_t indexCrc32 = ReadU32(header + 12);
		file.seekg(0, std::ios::end);
		const std::streamoff endPosition = file.tellg();
		if (endPosition < 0)
		{
			ec = std::make_error_code(std::errc::io_error);
			return nullptr;
		}
		const uint64_t fileSize = static_cast<uint64_t>(endPosition);
		if (jsonLen == 0 || jsonLen > fileSize - kHeaderSize)
		{
			ec = std::make_error_code(std::errc::invalid_argument);
			return nullptr;
		}

		file.seekg(static_cast<std::streamoff>(kHeaderSize), std::ios::beg);
		std::vector<char> jsonBytes(static_cast<size_t>(jsonLen));
		file.read(jsonBytes.data(), static_cast<std::streamsize>(jsonLen));
		if (file.gcount() != static_cast<std::streamsize>(jsonLen))
		{
			ec = std::make_error_code(std::errc::invalid_argument);
			return nullptr;
		}
		if (Crc32::Compute(jsonBytes.data(), jsonBytes.size()) != indexCrc32)
		{
			ec = std::make_error_code(std::errc::invalid_argument);
			return nullptr;
		}

		std::string jsonError;
		const std::optional<Wui::JsonValue> root =
			Wui::JsonValue::Parse(std::string(jsonBytes.data(), static_cast<size_t>(jsonLen)), &jsonError);
		if (!root)
		{
			ec = std::make_error_code(std::errc::invalid_argument);
			return nullptr;
		}
		const Wui::JsonValue* entriesNode = root->Find("entries");
		if (!entriesNode || entriesNode->type != Wui::JsonValue::Type::Array ||
			entriesNode->Array.size() > kMaxEntryCount)
		{
			ec = std::make_error_code(std::errc::invalid_argument);
			return nullptr;
		}

		const uint64_t dataStart = kHeaderSize + jsonLen;
		std::unordered_map<std::string, Entry> entries;
		entries.reserve(entriesNode->Array.size());
		for (const Wui::JsonValue& node : entriesNode->Array)
		{
			const Wui::JsonValue* pathNode = node.Find("path");
			const Wui::JsonValue* offsetNode = node.Find("offset");
			const Wui::JsonValue* sizeNode = node.Find("size");
			const Wui::JsonValue* crcNode = node.Find("crc32");
			const Wui::JsonValue* hashNode = node.Find("hash");
			if (!pathNode || !offsetNode || !sizeNode || !crcNode || !hashNode)
			{
				ec = std::make_error_code(std::errc::invalid_argument);
				return nullptr;
			}

			Path normalized;
			std::error_code normalizeEc;
			if (!Normalize(pathNode->AsString(""), normalized, normalizeEc))
			{
				ec = std::make_error_code(std::errc::invalid_argument);
				return nullptr;
			}

			const std::string offsetText = offsetNode->AsString("");
			const std::string sizeText = sizeNode->AsString("");
			const std::string crcText = crcNode->AsString("");
			char* offsetEnd = nullptr;
			char* sizeEnd = nullptr;
			char* crcEnd = nullptr;
			const uint64_t offset = offsetText.empty() ? UINT64_MAX :
				std::strtoull(offsetText.c_str(), &offsetEnd, 10);
			const uint64_t size = sizeText.empty() ? UINT64_MAX :
				std::strtoull(sizeText.c_str(), &sizeEnd, 10);
			const uint64_t crc64 = crcText.empty() ? UINT64_MAX :
				std::strtoull(crcText.c_str(), &crcEnd, 10);
			if (!offsetEnd || *offsetEnd != '\0' || !sizeEnd || *sizeEnd != '\0' ||
				!crcEnd || *crcEnd != '\0' || crc64 > UINT32_MAX ||
				offset < dataStart || offset > fileSize || size > fileSize - offset)
			{
				ec = std::make_error_code(std::errc::invalid_argument);
				return nullptr;
			}
			Entry entry;
			entry.offset = offset;
			entry.size = size;
			entry.crc32 = static_cast<uint32_t>(crc64);
			entry.hash = hashNode->AsString("");
			if (!entries.emplace(std::move(normalized), std::move(entry)).second)
			{
				ec = std::make_error_code(std::errc::invalid_argument);
				return nullptr;
			}
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
		for (BuildItem& item : items)
			if (!FingerprintItem(item, ec))
				return false;

		const uint64_t jsonLen = LayoutIndex(items);
		const std::string indexText = SerializeIndex(items);

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
			WriteU32(header.data() + 8, static_cast<uint32_t>(jsonLen));
			WriteU32(header.data() + 12,
				Crc32::Compute(indexText.data(), indexText.size()));
			out.write(reinterpret_cast<const char*>(header.data()), static_cast<std::streamsize>(header.size()));
			out.write(indexText.data(), static_cast<std::streamsize>(indexText.size()));

			std::vector<char> buffer(4 * 1024 * 1024);
			for (const BuildItem& item : items)
			{
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
					remaining -= static_cast<uint64_t>(chunk);
				}
			}
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

		// 先写临时文件再替换;目标已存在时删除后重试。
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

		// 每条目 CRC 校验:内容被篡改时仅该条目失败。
		if (Crc32::Compute(out.data(), out.size()) != it->second.crc32)
		{
			out.clear();
			ec = std::make_error_code(std::errc::invalid_argument);
			return false;
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
