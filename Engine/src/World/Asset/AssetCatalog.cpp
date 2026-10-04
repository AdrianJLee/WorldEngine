#include "wldpch.h"

#include "World/Asset/AssetCatalog.h"

#include "World/Asset/WModelIO.h"
#include "World/Renderer/Material.h"
#include "World/Renderer/Texture/TextureImportSettings.h"

#include <algorithm>
#include <fstream>
#include <sstream>
#include <system_error>

namespace World
{
	namespace
	{
		// `.wmat` / `.wtex` 是文本资产:身份在头部若干行内,只读到够用为止(不整文件加载)。
		std::string ReadHeadText(const std::filesystem::path& path, std::size_t maxBytes)
		{
			std::ifstream input(path, std::ios::binary);
			if (!input.is_open())
				return {};
			std::string buffer(maxBytes, '\0');
			input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
			buffer.resize(static_cast<std::size_t>(input.gcount()));
			return buffer;
		}

		std::string LowerExtension(const std::filesystem::path& path)
		{
			std::string extension = path.extension().generic_string();
			std::transform(extension.begin(), extension.end(), extension.begin(),
				[](unsigned char c) { return static_cast<char>(std::tolower(c)); });
			return extension;
		}

		// 从 .wmat / .wtex 头文本里取 `assetid:` 行(YAML 是宽松文本,这里只找这一行,
		// 不做完整解析 —— 完整解析由各资产的 Load 负责,目录扫描不该因坏文件而失败)。
		bool IdentityFromHeadText(const std::string& text, AssetId* out)
		{
			std::istringstream stream(text);
			std::string line;
			while (std::getline(stream, line))
			{
				const std::size_t colon = line.find(':');
				if (colon == std::string::npos)
					continue;
				std::string key = line.substr(0, colon);
				std::transform(key.begin(), key.end(), key.begin(),
					[](unsigned char c) { return static_cast<char>(std::tolower(c)); });
				// 只认行首(允许前导空白)的键;不匹配注释行。
				const std::size_t firstNonSpace = key.find_first_not_of(" \t");
				if (firstNonSpace == std::string::npos || key[firstNonSpace] == '#')
					continue;
				if (key.compare(firstNonSpace, std::string::npos, "assetid") != 0)
					continue;
				std::string value = line.substr(colon + 1);
				const std::size_t begin = value.find_first_not_of(" \t\"");
				if (begin == std::string::npos)
					return false;
				const std::size_t end = value.find_last_not_of(" \t\r\n\"");
				return ParseAssetId(value.substr(begin, end - begin + 1), out);
			}
			return false;
		}
	}

	bool AssetCatalog::IsIdentifiedAssetPath(const std::string& logicalPath)
	{
		const std::string extension = LowerExtension(std::filesystem::path(logicalPath));
		return extension == ".wmodel" || extension == ".wmat" || extension == ".wtex";
	}

	void AssetCatalog::Register(const std::string& logicalPath, AssetId identity)
	{
		if (logicalPath.empty() || !identity.IsValid())
			return;
		const PathId path = StringPool::Get().InternPath(logicalPath);

		// 同一路径换了身份(文件被替换):先清旧身份,避免两条 id 指向一个路径。
		if (const auto previous = m_ByPath.find(path); previous != m_ByPath.end())
			m_ByIdentity.erase(previous->second);
		// 同一身份换了路径(改名):先清旧路径,避免一个 id 指向两条路径。
		if (const auto previous = m_ByIdentity.find(identity); previous != m_ByIdentity.end())
			m_ByPath.erase(previous->second);

		m_ByIdentity[identity] = path;
		m_ByPath[path] = identity;
	}

	void AssetCatalog::Unregister(const std::string& logicalPath)
	{
		const PathId path = StringPool::Get().InternPath(logicalPath);
		const auto found = m_ByPath.find(path);
		if (found == m_ByPath.end())
			return;
		m_ByIdentity.erase(found->second);
		m_ByPath.erase(found);
	}

	PathId AssetCatalog::FindPath(AssetId identity) const
	{
		const auto found = m_ByIdentity.find(identity);
		return found == m_ByIdentity.end() ? PathId() : found->second;
	}

	AssetId AssetCatalog::FindIdentity(PathId path) const
	{
		const auto found = m_ByPath.find(path);
		return found == m_ByPath.end() ? AssetId() : found->second;
	}

	void AssetCatalog::Clear()
	{
		m_ByIdentity.clear();
		m_ByPath.clear();
		m_Skipped = 0;
	}

	std::vector<std::string> AssetCatalog::Describe() const
	{
		std::vector<std::string> lines;
		lines.reserve(m_ByIdentity.size());
		for (const auto& [identity, path] : m_ByIdentity)
			lines.push_back(FormatAssetId(identity) + " -> " + std::string(StringPool::Get().PathOf(path)));
		std::sort(lines.begin(), lines.end());
		return lines;
	}

	std::size_t AssetCatalog::Scan(const std::string& contentRootAbsolute)
	{
		m_Skipped = 0;
		if (contentRootAbsolute.empty())
			return 0;

		std::error_code error;
		const std::filesystem::path root(contentRootAbsolute);
		if (!std::filesystem::is_directory(root, error))
			return 0;

		std::size_t registered = 0;
		for (std::filesystem::recursive_directory_iterator it(root, error), end; it != end; it.increment(error))
		{
			if (error)
			{
				error.clear();
				++m_Skipped;
				continue;
			}
			if (!it->is_regular_file(error))
				continue;
			const std::filesystem::path& absolute = it->path();
			const std::string extension = LowerExtension(absolute);
			if (extension != ".wmodel" && extension != ".wmat" && extension != ".wtex")
				continue;

			std::error_code relativeError;
			const std::string logical = std::filesystem::relative(absolute, root, relativeError).generic_string();
			if (relativeError || logical.empty())
				continue;

			AssetId identity;
			bool ok = false;
			if (extension == ".wmodel")
			{
				Asset::WModelData::MetaData meta;
				std::string metaError;
				ok = Asset::WModelIO::ReadMeta(absolute.string(), meta, &metaError)
					&& meta.Valid && meta.Identity.IsValid();
				if (ok)
					identity = meta.Identity;
			}
			else
			{
				// 头部 8KiB 足够覆盖资产头(设置行都在最前面);读不到身份就跳过。
				ok = IdentityFromHeadText(ReadHeadText(absolute, 8192), &identity);
			}

			if (!ok)
			{
				++m_Skipped;
				continue;
			}
			Register(logical, identity);
			++registered;
		}
		return registered;
	}
}