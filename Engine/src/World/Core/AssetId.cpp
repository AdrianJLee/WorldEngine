#include "wldpch.h"

#include "World/Core/AssetId.h"

#include "World/Core/UUID.h"

#include <cstdio>
#include <random>

namespace World
{
	AssetId GenerateAssetId()
	{
		// 与 UUID 同一个随机源口径,但**独立**的调用点:实体身份与资产身份是两个命名空间,
		// 共享生成器不代表共享 id 空间(判等只在本类型内做)。
		static std::random_device rd;
		static std::mt19937_64 gen(rd());
		static std::uniform_int_distribution<uint64_t> dis;
		uint64_t value = dis(gen);
		if (value == 0)
			value = 1;   // 0 保留给"未分配"
		return AssetId { value };
	}

	std::string FormatAssetId(AssetId id)
	{
		char buffer[19] = {};
		std::snprintf(buffer, sizeof(buffer), "0x%016llX", static_cast<unsigned long long>(id.Value));
		return std::string(buffer);
	}

	bool ParseAssetId(const std::string& text, AssetId* out)
	{
		if (!out)
			return false;
		out->Value = 0;

		std::size_t begin = 0;
		if (text.size() >= 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X'))
			begin = 2;
		if (begin >= text.size() || text.size() - begin > 16)
			return false;

		uint64_t value = 0;
		for (std::size_t i = begin; i < text.size(); ++i)
		{
			const char c = text[i];
			uint64_t digit = 0;
			if (c >= '0' && c <= '9')      digit = static_cast<uint64_t>(c - '0');
			else if (c >= 'a' && c <= 'f') digit = static_cast<uint64_t>(c - 'a' + 10);
			else if (c >= 'A' && c <= 'F') digit = static_cast<uint64_t>(c - 'A' + 10);
			else return false;
			value = (value << 4) | digit;
		}

		if (value == 0)
			return false;   // 显式 0 = "未分配",不当作有效身份
		out->Value = value;
		return true;
	}
}