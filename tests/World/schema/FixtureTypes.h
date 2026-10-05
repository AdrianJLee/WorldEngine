#pragma once

#include "World/Core/InlineString.h"
#include "World/Schema/Schema.h"

#include <glm/glm.hpp>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

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

	// CPPT-6:容器(Array/Map)夹具 —— 元素类型覆盖叶子、命名 struct 与 enum。
	struct StatEntry
	{
		float Health = 5.0f;
		int32_t Count = 1;

		WE_SCHEMA_BODY(TestKit, StatEntry, Struct)
			WE_FIELD(Health, Float);
			WE_FIELD(Count, Int32);
		WE_SCHEMA_END
	};

	struct ContainerFixture
	{
		std::vector<float> Scores;
		std::map<std::string, float> Costs;
		std::vector<StatEntry> Stats;
		std::map<std::string, StatEntry> Lookup;
		std::vector<TestEnum> Modes;

		// CPPT-6:声明成 Component(仅测试夹具)才能走 SceneSerializer 的真实组件读写路径
		// (Struct 只覆盖 schema/YAML 层,没有 Storage 绑定,进不了场景)。
		WE_SCHEMA_BODY(TestKit, ContainerFixture, Component)
			WE_FIELD(Scores, Array, Of(Float));
			WE_FIELD(Costs, Map, Of(Float));
			WE_FIELD(Stats, Array, Of(StatEntry));
			WE_FIELD(Lookup, Map, Of(StatEntry));
			WE_FIELD(Modes, Array, Of(TestEnum));
		WE_SCHEMA_END
	};

	// CPPT-6-FIX2:schema-compiler 的 DefaultValue 回归夹具 —— 每个 kind 一条**没有显式 Default(...)**
	// 的字段(Struct 类别 → 生成物写类型零值)。生成物的默认值必须是该 kind 对应的**那一支** variant
	// 备选(旧实现给 Float 写 double、给整数族写 int):否则嵌套 struct 的子字段在检视器里被判
	// "值与声明类型不符",只画 `—` 且不可编辑(Stats.Health 实测)。
	struct DefaultKindFixture
	{
		bool BoolValue = false;
		int8_t Int8Value = 0;
		int16_t Int16Value = 0;
		int32_t Int32Value = 0;
		int64_t Int64Value = 0;
		uint8_t UInt8Value = 0;
		uint16_t UInt16Value = 0;
		uint32_t UInt32Value = 0;
		uint64_t UInt64Value = 0;
		float FloatValue = 0.0f;
		double DoubleValue = 0.0;
		glm::vec2 Vec2Value { 0.0f };
		glm::vec3 Vec3Value { 0.0f };
		glm::vec4 Vec4Value { 0.0f };
		glm::ivec2 IVec2Value { 0 };
		glm::ivec3 IVec3Value { 0 };
		glm::ivec4 IVec4Value { 0 };
		glm::uvec2 UVec2Value { 0u };
		glm::uvec3 UVec3Value { 0u };
		glm::uvec4 UVec4Value { 0u };
		glm::quat QuatValue { 1.0f, 0.0f, 0.0f, 0.0f };
		glm::mat3 Mat3Value { 1.0f };
		glm::mat4 Mat4Value { 1.0f };
		std::string StringValue;
		// Text = 有界内联文本:无堆、平凡可拷贝(对比上面 StringValue 的 std::string)。
		World::InlineString<20> TextValue;

		WE_SCHEMA_BODY(TestKit, DefaultKindFixture, Struct)
			WE_FIELD(BoolValue, Bool);
			WE_FIELD(Int8Value, Int8);
			WE_FIELD(Int16Value, Int16);
			WE_FIELD(Int32Value, Int32);
			WE_FIELD(Int64Value, Int64);
			WE_FIELD(UInt8Value, UInt8);
			WE_FIELD(UInt16Value, UInt16);
			WE_FIELD(UInt32Value, UInt32);
			WE_FIELD(UInt64Value, UInt64);
			WE_FIELD(FloatValue, Float);
			WE_FIELD(DoubleValue, Double);
			WE_FIELD(Vec2Value, Vec2);
			WE_FIELD(Vec3Value, Vec3);
			WE_FIELD(Vec4Value, Vec4);
			WE_FIELD(IVec2Value, IVec2);
			WE_FIELD(IVec3Value, IVec3);
			WE_FIELD(IVec4Value, IVec4);
			WE_FIELD(UVec2Value, UVec2);
			WE_FIELD(UVec3Value, UVec3);
			WE_FIELD(UVec4Value, UVec4);
			WE_FIELD(QuatValue, Quat);
			WE_FIELD(Mat3Value, Mat3);
			WE_FIELD(Mat4Value, Mat4);
			WE_FIELD(StringValue, String);
			WE_FIELD(TextValue, Text);
		WE_SCHEMA_END
	};
}
