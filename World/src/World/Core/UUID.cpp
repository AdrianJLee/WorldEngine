#include "wldpch.h"
#include "UUID.h"

#include <random>

namespace World
{
	UUID::UUID()
	{
		m_UUID = GenerateUUID();
	}
	UUID::UUID(uint64_t uuid)
		: m_UUID(uuid)
	{}

	uint64_t UUID::GenerateUUID()
	{
		static std::random_device rd;
		static std::mt19937_64 gen(rd());
		static std::uniform_int_distribution<uint64_t> dis;
		return dis(gen);
	}
}