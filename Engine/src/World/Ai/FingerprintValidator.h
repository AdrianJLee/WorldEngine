#pragma once

#include "World/Core/Export.h"

#include <filesystem>
#include <string>
#include <vector>

namespace World::Ai
{
	class WLD_API FingerprintValidator
	{
	public:
		// 计算内存数据的 SHA-256 十六进制字符串 (64 字符)
		static std::string ComputeSha256(const void* data, size_t size);
		static std::string ComputeStringSha256(const std::string& text);

		// 计算文件的 SHA-256 哈希
		static std::string ComputeFileSha256(const std::filesystem::path& path);

		// 校验引擎核心源树完整性与干净度 (检测源码目录内是否有非法生成的中间产物)
		static bool ValidateEngineTree(std::vector<std::string>& outViolations);
	};
}
