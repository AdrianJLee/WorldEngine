#pragma once
#include "World/Reflection/Reflection.h"
namespace World
{
	class UUID
	{
		REFLECT_BODY(UUID, TypeCategory::NormalClass);
	public:
		UUID();
		UUID(uint64_t uuid);
		UUID(const UUID& other) = default;
		operator uint64_t() const { return m_UUID; }
	private:
		uint64_t GenerateUUID();
	private:
		PROPERTY(m_UUID);
		uint64_t m_UUID;
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