#pragma once

#include "World/Schema/Schema.h"

namespace World
{
	class UUID
	{
	public:
		UUID();
		UUID(uint64_t uuid);
		UUID(const UUID& other) = default;
		operator uint64_t() const { return m_UUID; }
	private:
		uint64_t GenerateUUID();
	private:
		uint64_t m_UUID;

		WE_SCHEMA_BODY(World, UUID, Struct)
			WE_FIELD(m_UUID, UInt64);
		WE_SCHEMA_END
	};
}

namespace std
{
	template<>
	struct hash<World::UUID>
	{
		std::size_t operator()(const World::UUID& uuid) const
		{
			return hash<uint64_t>()((uint64_t)uuid);
		}
	};
}
