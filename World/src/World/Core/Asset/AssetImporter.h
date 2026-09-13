#pragma once

#include "World/Core/Export.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

namespace World::Asset
{
	struct ImportRequest
	{
		std::string LogicalPath;             // 相对 content_root 的逻辑路径
		std::filesystem::path Source;        // 源文件绝对路径
	};

	struct ImportResult
	{
		bool Ok = false;
		std::string Error;
		std::vector<uint8_t> Data;           // cooked 产物字节
		uint64_t Fingerprint = 0;            // 源内容指纹(FNV-1a 64)
	};

	// 资产导入器:插件的可扩展点。P1 只内置基础导入器,Shader importer 留 W6。
	class WLD_API IAssetImporter
	{
	public:
		virtual ~IAssetImporter() = default;
		virtual std::string Name() const = 0;
		virtual uint32_t Version() const = 0;
		virtual bool Matches(const std::filesystem::path& source) const = 0;
		virtual ImportResult Import(const ImportRequest& request, std::error_code& ec) const = 0;
	};
}
