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

	// D5b 多产物:一个源文件产出的单个 cooked 产物(如 .wmodel/.wmat/贴图)。
	// LogicalPath 是相对 content_root 的逻辑路径,与 .wmodel 的材质槽同一约定。
	struct ImportOutput
	{
		std::string LogicalPath;
		std::vector<uint8_t> Data;
	};

	struct ImportResult
	{
		bool Ok = false;
		std::string Error;
		std::vector<uint8_t> Data;           // 单产物 cooked 字节
		uint64_t Fingerprint = 0;            // 源内容指纹(FNV-1a 64)
		// D5b 多产物(空 = 单产物,走 Data;非空时 Data 必须为空)。
		std::vector<ImportOutput> Outputs;
		// 导入成功但需要让用户看到的问题(降级/忽略项);失败原因放 Error。
		std::vector<std::string> Warnings;
	};

	// 资产导入器:插件的可扩展点。P1 只内置基础导入器,Shader importer 留 W6。
	class WLD_API IAssetImporter
	{
	public:
		virtual ~IAssetImporter() = default;
		virtual std::string Name() const = 0;
		virtual uint32_t Version() const = 0;
		virtual bool Matches(const std::filesystem::path& source) const = 0;
		// P4-U11:导入设置指纹(改了设置 → 产物必须重烘)。默认 0 = 该导入器没有逐源设置;
		// ModelImporter 覆盖它并走 ModelImportSettings::ResolveForImport(与真正导入同一入口)。
		virtual uint64_t SettingsFingerprint(const std::filesystem::path& source) const
		{
			(void)source;
			return 0;
		}
		virtual ImportResult Import(const ImportRequest& request, std::error_code& ec) const = 0;
	};
}
