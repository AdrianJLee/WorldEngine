#pragma once

#include "World/Schema/Schema.h"

#include <glm/glm.hpp>
#include <cstdint>
#include <string>

namespace World::TestSchema
{
	struct HealthFixture
	{
		float Health = 100.0f;
		int32_t Count = 0;
		std::string Name;
		glm::vec2 Offset { 0.0f, 0.0f };
		bool Enabled = true;

		WE_SCHEMA_BODY(TestKit, HealthFixture, Struct)
			WE_FIELD(Health, Float, Group("Stats"), Range(0.0f, 9999.0f));
			WE_FIELD(Count, Int32);
			WE_FIELD(Name, String, ReadOnly);
			WE_FIELD(Offset, Vec2);
			WE_FIELD(Enabled, Bool);
		WE_SCHEMA_END
	};

	struct ReorderFixture
	{
		bool Enabled = false;
		int32_t Count = 7;
		float Health = 1.0f;
		float DebugOnly = 2.0f;

		WE_SCHEMA_BODY(TestKit, ReorderFixture, Struct)
			WE_FIELD(Enabled, Bool);
			WE_FIELD(Count, Int32);
			WE_FIELD(Health, Float);
			WE_FIELD(DebugOnly, Float, Transient);
		WE_SCHEMA_END
	};

	enum class TestEnum : int32_t
	{
		None = 0,
		A = 1,
		B = 2,
	};
	WE_ENUM_SCHEMA(TestKit, TestEnum, Int32)
		WE_ENUM_VALUE(None);
		WE_ENUM_VALUE(A);
		WE_ENUM_VALUE(B);
	WE_ENUM_END

	struct NestedFixture
	{
		HealthFixture Inner {};
		uint32_t Level = 1;
		TestEnum Mode = TestEnum::None;

		WE_SCHEMA_BODY(TestKit, NestedFixture, Struct)
			WE_FIELD(Inner, Object, Of(HealthFixture));
			WE_FIELD(Level, UInt32);
			WE_FIELD(Mode, Enum, Of(TestEnum));
		WE_SCHEMA_END
	};
}
