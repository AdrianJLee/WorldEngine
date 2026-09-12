#include "World/Core/WorldContext.h"
#include "World/Schema/Schema.h"
#include "schema/FixtureTypes.h"
#include "schema/Generated/TestKit/TestKitSchemaRegistration.h"

#include <cstdint>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{
	using namespace World;
	using namespace World::Schema;

	void Check(bool condition, const char* expression, int line)
	{
		if (!condition)
			throw std::runtime_error(std::string("line ") + std::to_string(line) + ": " + expression);
	}
#define CHECK(expression) Check(static_cast<bool>(expression), #expression, __LINE__)

	const FieldSchema* FindField(const TypeSchema& schema, const char* name)
	{
		for (const FieldSchema& field : schema.Fields)
			if (field.Name == name)
				return &field;
		return nullptr;
	}
}

int main()
{
	try
	{
		// WorldContext 独立于窗口/Application:核心可无界面验证。
		WorldContext context;

		CHECK(RegisterTestKitSchemaModule(context.Schemas()));
		CHECK(context.Schemas().TypeCount() == 3);
		CHECK(context.Schemas().EnumCount() == 1);

		// 1. 查找与元数据
		const TypeSchema* health = context.Schemas().Find("TestKit::HealthFixture");
		CHECK(health != nullptr);
		CHECK(health->Fields.size() == 5);
		const FieldSchema* healthField = FindField(*health, "Health");
		CHECK(healthField != nullptr);
		CHECK(healthField->K == Kind::Float);
		CHECK(healthField->Meta.Group == "Stats");
		CHECK(healthField->Meta.Min.has_value() && healthField->Meta.Min.value() == 0.0f);
		CHECK(healthField->Meta.Max.has_value() && healthField->Meta.Max.value() == 9999.0f);
		CHECK(FindField(*health, "Name")->Meta.ReadOnly);

		// 2. 生成访问器往返(类型化,无 std::any/偏移)
		TestSchema::HealthFixture instance;
		CHECK(std::get<float>(healthField->Get(&instance)) == 100.0f);
		healthField->Set(&instance, Value(250.0f));
		CHECK(instance.Health == 250.0f);

		const FieldSchema* offsetField = FindField(*health, "Offset");
		offsetField->Set(&instance, Value(glm::vec2(3.0f, 4.0f)));
		CHECK(instance.Offset.x == 3.0f && instance.Offset.y == 4.0f);

		const FieldSchema* nameField = FindField(*health, "Name");
		nameField->Set(&instance, Value(std::string("hero")));
		CHECK(instance.Name == "hero");

		// 3. FieldId 只依赖 "模块.类型.字段名"(确定性公式,与声明顺序/索引无关)
		const TypeSchema* reorder = context.Schemas().Find("TestKit::ReorderFixture");
		CHECK(reorder != nullptr);
		CHECK(FindField(*health, "Health")->Id.Value == Fnv1a64("TestKit::HealthFixture.Health"));
		CHECK(FindField(*reorder, "Count")->Id.Value == Fnv1a64("TestKit::ReorderFixture.Count"));
		// 同名字段属于不同类型时身份不同(类型限定)
		CHECK(FindField(*health, "Health")->Id.Value != FindField(*reorder, "Health")->Id.Value);
		CHECK(FindField(*reorder, "DebugOnly")->Meta.Transient);

		// 4. 枚举 schema
		const EnumSchema* testEnum = context.Schemas().FindEnum("TestEnum");
		CHECK(testEnum != nullptr);
		CHECK(testEnum->IsSigned);
		CHECK(testEnum->UnderlyingSize == 4);
		CHECK(testEnum->Values.size() == 3);
		CHECK(std::string(testEnum->FindName(1)) == "A");

		// 5. 嵌套对象与枚举字段
		const TypeSchema* nestedSchema = context.Schemas().Find("TestKit::NestedFixture");
		CHECK(nestedSchema != nullptr);
		TestSchema::NestedFixture nested;
		const FieldSchema* inner = FindField(*nestedSchema, "Inner");
		CHECK(inner->K == Kind::Object);
		CHECK(inner->GetNested() != nullptr);
		CHECK(inner->GetNested()->Id.Name == "TestKit::HealthFixture");
		CHECK(inner->GetPtrConst(&nested) == &nested.Inner);

		const FieldSchema* level = FindField(*nestedSchema, "Level");
		level->Set(&nested, Value(uint32_t(42)));
		CHECK(nested.Level == 42);

		const FieldSchema* mode = FindField(*nestedSchema, "Mode");
		CHECK(mode->K == Kind::Enum);
		CHECK(mode->GetEnum() != nullptr && mode->GetEnum()->Name == "TestEnum");
		CHECK(std::get<int64_t>(mode->Get(&nested)) == 0);
		mode->Set(&nested, Value(int64_t(2)));
		CHECK(nested.Mode == TestSchema::TestEnum::B);

		// 通过嵌套 schema 直接修改内层对象
		FindField(*health, "Health")->Set(inner->GetPtr(&nested), Value(321.0f));
		CHECK(nested.Inner.Health == 321.0f);

		// 6. 同模块重复注册被拒绝且不改变现有条目
		CHECK(context.Schemas().Register({ "TestKit", 1 }, *context.Schemas().Find("TestKit::HealthFixture")) == SchemaRegistry::Status::DuplicateType);
		CHECK(context.Schemas().TypeCount() == 3);

		// 7. RegisterModule 事务化:批内失败不留下任何条目
		{
			TypeSchema manualOnly{ TypeId{ "TestKit::ManualOnly" }, "ManualOnly", WE_SCHEMA_ABI_VERSION, sizeof(uint32_t), TypeCategory::Struct, {}, nullptr, nullptr };
			const std::vector<TypeSchema> batch = { *context.Schemas().Find("TestKit::HealthFixture"), manualOnly };
			CHECK(context.Schemas().RegisterModule({ "TestKit", 1 }, batch) == SchemaRegistry::Status::DuplicateType);
			CHECK(context.Schemas().Find("TestKit::ManualOnly") == nullptr);
		}

		// 8. 跨模块同名 Struct 共存;Find 歧义返回 nullptr;卸载只清本模块
		CHECK(context.Schemas().Register({ "Other", 1 }, *context.Schemas().Find("TestKit::HealthFixture")) == SchemaRegistry::Status::Ok);
		CHECK(context.Schemas().TypeCount() == 4);
		CHECK(context.Schemas().Find("TestKit::HealthFixture") == nullptr);
		context.Schemas().UnregisterModule({ "Other", 1 });
		CHECK(context.Schemas().TypeCount() == 3);
		const TypeSchema* healthAgain = context.Schemas().Find("TestKit::HealthFixture");
		CHECK(healthAgain != nullptr);
		CHECK(healthAgain->Fields.size() == 5);

		// 9. ABI 版本超出宿主支持
		{
			TypeSchema tooNew{ TypeId{ "TestKit::TooNew" }, "TooNew", WE_SCHEMA_ABI_VERSION + 1, sizeof(uint32_t), TypeCategory::Struct, {}, nullptr, nullptr };
			CHECK(context.Schemas().Register({ "TestKit", 1 }, tooNew) == SchemaRegistry::Status::AbiMismatch);
		}

		// 10. 模块卸载后重新注册可恢复
		UnregisterTestKitSchemaModule(context.Schemas());
		CHECK(context.Schemas().TypeCount() == 0);
		CHECK(context.Schemas().EnumCount() == 0);
		CHECK(context.Schemas().Find("TestKit::HealthFixture") == nullptr);
		CHECK(RegisterTestKitSchemaModule(context.Schemas()));
		CHECK(context.Schemas().TypeCount() == 3);

		std::printf("World.Schema: all checks passed\n");
		return 0;
	}
	catch (const std::exception& error)
	{
		std::fprintf(stderr, "World.Schema: FAILED: %s\n", error.what());
		return 1;
	}
}
