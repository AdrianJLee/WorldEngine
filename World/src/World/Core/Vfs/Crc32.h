#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace World::Vfs
{
	namespace Detail
	{
		// IEEE 802.3 CRC-32 查找表 (poly 0xEDB88320),进程内生成一次。
		inline const uint32_t* CrcTable()
		{
			static const std::array<uint32_t, 256> table = []() {
				std::array<uint32_t, 256> values{};
				for (uint32_t i = 0; i < 256; ++i)
				{
					uint32_t crc = i;
					for (int bit = 0; bit < 8; ++bit)
						crc = (crc & 1u) != 0u ? (0xEDB88320u ^ (crc >> 1)) : (crc >> 1);
					values[i] = crc;
				}
				return values;
			}();
			return table.data();
		}
	}

	// 流式 CRC-32:构造后即可 Update,Final 取结果。
	class Crc32
	{
	public:
		Crc32() = default;

		void Reset() { m_Crc = 0xFFFFFFFFu; }
		void Update(const void* data, size_t size);
		uint32_t Final() const { return m_Crc ^ 0xFFFFFFFFu; }

		static uint32_t Compute(const void* data, size_t size);

	private:
		uint32_t m_Crc = 0xFFFFFFFFu;
	};

	inline void Crc32::Update(const void* data, size_t size)
	{
		const auto* bytes = static_cast<const uint8_t*>(data);
		const uint32_t* table = Detail::CrcTable();
		uint32_t crc = m_Crc;
		for (size_t i = 0; i < size; ++i)
			crc = (crc >> 8) ^ table[(crc ^ bytes[i]) & 0xFFu];
		m_Crc = crc;
	}

	inline uint32_t Crc32::Compute(const void* data, size_t size)
	{
		Crc32 crc;
		crc.Update(data, size);
		return crc.Final();
	}
}
