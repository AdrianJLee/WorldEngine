#pragma once

#include "World/Core/Export.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace World::Crypto
{
	// 最小 SHA-256(FIPS 180-4)。用途:烘焙产物的"源字节指纹"(缓存键 + 陈旧检测)。
	// 不是安全边界:只做内容寻址,不做密钥/认证。
	struct Sha256Digest
	{
		uint8_t Bytes[32] = {};
	};

	WLD_API Sha256Digest Sha256(const uint8_t* data, size_t size);
	WLD_API Sha256Digest Sha256(const std::vector<uint8_t>& bytes);
	// 小写十六进制(64 字符)。
	WLD_API std::string Sha256Hex(const Sha256Digest& digest);
	WLD_API std::string Sha256Hex(const std::vector<uint8_t>& bytes);
}
