#include "World/Core/WorldContext.h"
#include "World/Core/Log.h"
#include "World/Schema/Schema.h"
#include "World/Scene/Components.h"
#include "World/Scene/Entity.h"
#include "World/Scene/SceneSerializer.h"
#include "World/Script/ScriptProperties.h"
#include "schema/FixtureTypes.h"
#include "schema/Generated/TestKit/TestKitSchemaRegistration.h"

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{
	using namespace World;
	using namespace World::Schema;
	using namespace World::TestSchema;

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
		World::Log::Init();

		// WorldContext 独立于窗口/Application:核心可无界面验证。
		WorldContext context;
		const size_t baselineTypes = context.Schemas().TypeCount();
		const size_t baselineEnums = context.Schemas().EnumCount();

		CHECK(RegisterTestKitSchemaModule(context.Schemas()));
		// CPPT-6:夹具新增 StatEntry / ContainerFixture 两个 struct(容器元素与容器宿主);
		// CPPT-6-FIX2:再加 DefaultKindFixture(每个 kind 一份"没有 Default(...)"的字段)。
		CHECK(context.Schemas().TypeCount() == baselineTypes + 6);
		CHECK(context.Schemas().EnumCount() == baselineEnums + 1);

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
		CHECK(context.Schemas().TypeCount() == baselineTypes + 6);

		// 7. RegisterModule 事务化:批内失败不留下任何条目
		{
			TypeSchema manualOnly{ TypeId{ "TestKit::ManualOnly" }, "ManualOnly", WE_SCHEMA_ABI_VERSION, sizeof(uint32_t), TypeCategory::Struct, {}, nullptr };
			const std::vector<TypeSchema> batch = { *context.Schemas().Find("TestKit::HealthFixture"), manualOnly };
			CHECK(context.Schemas().RegisterModule({ "TestKit", 1 }, batch) == SchemaRegistry::Status::DuplicateType);
			CHECK(context.Schemas().Find("TestKit::ManualOnly") == nullptr);
		}

		// 8. 跨模块同名 Struct 共存;Find 歧义返回 nullptr;卸载只清本模块
		CHECK(context.Schemas().Register({ "Other", 1 }, *context.Schemas().Find("TestKit::HealthFixture")) == SchemaRegistry::Status::Ok);
		CHECK(context.Schemas().TypeCount() == baselineTypes + 7);
		CHECK(context.Schemas().Find("TestKit::HealthFixture") == nullptr);
		context.Schemas().UnregisterModule({ "Other", 1 });
		CHECK(context.Schemas().TypeCount() == baselineTypes + 6);
		const TypeSchema* healthAgain = context.Schemas().Find("TestKit::HealthFixture");
		CHECK(healthAgain != nullptr);
		CHECK(healthAgain->Fields.size() == 5);

		// 9. ABI 版本超出宿主支持
		{
			TypeSchema tooNew{ TypeId{ "TestKit::TooNew" }, "TooNew", WE_SCHEMA_ABI_VERSION + 1, sizeof(uint32_t), TypeCategory::Struct, {}, nullptr };
			CHECK(context.Schemas().Register({ "TestKit", 1 }, tooNew) == SchemaRegistry::Status::AbiMismatch);
		}

		// 10. 模块卸载后重新注册可恢复
		UnregisterTestKitSchemaModule(context.Schemas());
		CHECK(context.Schemas().TypeCount() == baselineTypes);
		CHECK(context.Schemas().EnumCount() == baselineEnums);
		CHECK(context.Schemas().Find("TestKit::HealthFixture") == nullptr);
		CHECK(RegisterTestKitSchemaModule(context.Schemas()));
		CHECK(context.Schemas().TypeCount() == baselineTypes + 6);

			// 10b. CPPT-6:容器字段(Array/Map;元素含叶子、命名 struct、enum)——
			// schema 形状描述 + Accessor 读写 + SceneSerializer 场景往返
			// (夹具声明成 Component,走的正是编辑器/运行时的组件读写路径)。
		{
			const TypeSchema* containers = context.Schemas().Find("TestKit::ContainerFixture");
			CHECK(containers != nullptr);
			const FieldSchema* scores = FindField(*containers, "Scores");
			const FieldSchema* costs = FindField(*containers, "Costs");
			const FieldSchema* stats = FindField(*containers, "Stats");
			const FieldSchema* lookup = FindField(*containers, "Lookup");
			const FieldSchema* modes = FindField(*containers, "Modes");
			CHECK(scores && scores->K == Kind::Object && scores->Collection == CollectionKind::Array);
			CHECK(scores->Get && scores->Set && scores->ElementKind == Kind::Float);
			CHECK(scores->KeyKind == Kind::None && scores->GetElementNested == nullptr);
			CHECK(costs && costs->Collection == CollectionKind::Map && costs->KeyKind == Kind::String);
			CHECK(costs->ElementKind == Kind::Float);
			CHECK(stats && stats->Collection == CollectionKind::Array && stats->ElementKind == Kind::Object);
			CHECK(stats->GetElementNested && stats->GetElementNested()->Id.Name == "TestKit::StatEntry");
			CHECK(stats->ElementTypeName && std::string(stats->ElementTypeName) == "TestKit::StatEntry");
			CHECK(lookup && lookup->Collection == CollectionKind::Map && lookup->ElementKind == Kind::Object);
			CHECK(modes && modes->Collection == CollectionKind::Array && modes->ElementKind == Kind::Enum);
			CHECK(modes->GetEnum && modes->GetEnum()->Name == "TestEnum");
			CHECK(std::string(CollectionKindName(scores->Collection)) == "Array");
			CHECK(std::string(CollectionKindName(lookup->Collection)) == "Map");

			ContainerFixture source;
			source.Scores = { 1.0f, 2.5f };
			source.Costs.emplace("gold", 3.0f);
			source.Stats.push_back(StatEntry { 7.0f, 2 });
			source.Lookup.emplace("boss", StatEntry { 9.0f, 3 });
			source.Modes.push_back(TestEnum::B);

			// Get:容器 → 容器值(命名 struct 的元素 = 字段名 → Value 的 map;enum = 底层整数)。
			const Value scoresValue = scores->Get(&source);
			const ValueList* scoreList = std::get_if<ValueList>(&scoresValue);
			CHECK(scoreList && scoreList->size() == 2);
			CHECK(std::get<float>((*scoreList)[0]) == 1.0f && std::get<float>((*scoreList)[1]) == 2.5f);
			const Value statsValue = stats->Get(&source);
			const ValueList* statList = std::get_if<ValueList>(&statsValue);
			CHECK(statList && statList->size() == 1);
			const ValueMap* firstStat = std::get_if<ValueMap>(&(*statList)[0]);
			CHECK(firstStat && std::get<float>(firstStat->at("Health")) == 7.0f);
			CHECK(firstStat && std::get<int32_t>(firstStat->at("Count")) == 2);
			const Value lookupValue = lookup->Get(&source);
			const ValueMap* lookupMap = std::get_if<ValueMap>(&lookupValue);
			CHECK(lookupMap && lookupMap->size() == 1);
			CHECK(std::get<int32_t>(std::get<ValueMap>(lookupMap->at("boss")).at("Count")) == 3);
			const Value modesValue = modes->Get(&source);
			const ValueList* modeList = std::get_if<ValueList>(&modesValue);
			CHECK(modeList && modeList->size() == 1
				&& std::get<int64_t>((*modeList)[0]) == static_cast<int64_t>(TestEnum::B));

			// Set:容器值 → 成员(struct 元素递归写回;未设/形状不符不动目标)。
			ContainerFixture target;
			target.Scores = { 9.0f };
			scores->Set(&target, scoresValue);
			CHECK(target.Scores.size() == 2 && target.Scores[0] == 1.0f && target.Scores[1] == 2.5f);
			stats->Set(&target, statsValue);
			CHECK(target.Stats.size() == 1 && target.Stats[0].Health == 7.0f && target.Stats[0].Count == 2);
			lookup->Set(&target, lookupValue);
			CHECK(target.Lookup.size() == 1 && target.Lookup.at("boss").Count == 3);
			modes->Set(&target, modesValue);
			CHECK(target.Modes.size() == 1 && target.Modes[0] == TestEnum::B);
			target.Costs = { { "keep", 1.0f } };
			costs->Set(&target, Value {});
			CHECK(target.Costs.size() == 1 && target.Costs.at("keep") == 1.0f);   // 未设 = 不覆盖

			// 场景往返:写出 YAML(flow seq/map)→ 读到新场景 → 逐值一致
			// (Array(Struct) / Map(Struct) 的元素是"字段名 → 值"的递归 map)。
			{
				const std::filesystem::path path =
					std::filesystem::temp_directory_path() / "worldengine-schema-containers.wd";
				{
					Ref<Scene> scene = CreateRef<Scene>(context);
					Entity entity = Entity::CreateEntity(scene.get(), "ContainerRoundTrip", World::UUID(uint64_t(0x2233)));
					auto& component = entity.AddComponent<ContainerFixture>();
					component.Scores = { 1.0f, 2.5f };
					component.Costs.emplace("gold", 3.0f);
					component.Stats.push_back(StatEntry { 7.0f, 2 });
					component.Lookup.emplace("boss", StatEntry { 9.0f, 3 });
					component.Modes.push_back(TestEnum::B);

					SceneSerializer writer(scene);
					CHECK(writer.Serialize(path.string()));
				}
				std::string yaml;
				{
					std::ifstream stream(path);
					CHECK(stream.is_open());
					yaml.assign(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
				}
				CHECK(yaml.find("Scores: [1, 2.5]") != std::string::npos);
				CHECK(yaml.find("Costs: {gold: 3}") != std::string::npos);
				CHECK(yaml.find("Stats: [{Health: 7, Count: 2}]") != std::string::npos);
				CHECK(yaml.find("Lookup: {boss: {Health: 9, Count: 3}}") != std::string::npos);
				CHECK(yaml.find("Modes: [2]") != std::string::npos);   // TestEnum::B

				Ref<Scene> loaded = CreateRef<Scene>(context);
				SceneSerializer reader(loaded);
				CHECK(reader.Deserialize(path.string()));
				bool found = false;
				for (const entt::entity handle : loaded->GetRegistry().view<ContainerFixture>())
				{
					const auto& component = loaded->GetRegistry().get<ContainerFixture>(handle);
					found = true;
					CHECK(component.Scores.size() == 2 && component.Scores[0] == 1.0f && component.Scores[1] == 2.5f);
					CHECK(component.Costs.size() == 1 && component.Costs.at("gold") == 3.0f);
					CHECK(component.Stats.size() == 1 && component.Stats[0].Health == 7.0f && component.Stats[0].Count == 2);
					CHECK(component.Lookup.size() == 1 && component.Lookup.at("boss").Health == 9.0f);
					CHECK(component.Lookup.at("boss").Count == 3);
					CHECK(component.Modes.size() == 1 && component.Modes[0] == TestEnum::B);
				}
				CHECK(found);
				std::error_code ignored;
				std::filesystem::remove(path, ignored);
			}
		}

		// 11. 场景序列化往返(无 GL 组件):schema 驱动的保存/加载保持数据一致。
		{
			Ref<Scene> scene = CreateRef<Scene>(context);
			Entity entity = Entity::CreateEntity(scene.get(), "RoundTrip", World::UUID(uint64_t(0x1122334455)));
			entity.AddComponent<TransformComponent>().SetTransform(glm::vec3(1.0f, 2.0f, 3.0f), glm::vec3(0.1f, 0.2f, 0.3f), glm::vec3(2.0f));
			entity.AddComponent<BoxCollider2DComponent>().Density = 3.5f;

			const std::filesystem::path path = std::filesystem::temp_directory_path() / "worldengine-schema-roundtrip.wd";
			SceneSerializer writer(scene);
			CHECK(writer.Serialize(path.string()));

			Ref<Scene> loaded = CreateRef<Scene>(context);
			SceneSerializer reader(loaded);
			CHECK(reader.Deserialize(path.string()));

			bool found = false;
			for (auto handle : loaded->GetRegistry().view<TagComponent>())
			{
				Entity loadedEntity(loaded.get(), handle);
				if (loadedEntity.GetComponent<TagComponent>().Tag != "RoundTrip")
					continue;
				found = true;
				const auto& transform = loadedEntity.GetComponent<TransformComponent>();
				CHECK(transform.Location.x == 1.0f && transform.Location.y == 2.0f && transform.Location.z == 3.0f);
				CHECK(transform.Scale.x == 2.0f);
				CHECK(loadedEntity.GetComponent<BoxCollider2DComponent>().Density == 3.5f);
				CHECK(static_cast<uint64_t>(loadedEntity.GetComponent<UUIDComponent>().ID) == uint64_t(0x1122334455));
			}
			CHECK(found);
			std::error_code ignored;
			std::filesystem::remove(path, ignored);
		}

		// 12.(CPPT-6-FIX2)声明默认值的 variant 备选必须与 Kind 一致。没有 `Default(...)` 的
		// Struct/Component 字段由 schema-compiler 的 DefaultValue 生成**类型零值**:它必须是该 kind
		// 对应的那一支(旧实现给 Float 写 double、给整数族写 int)。不符时嵌套 struct 的子字段会被
		// 检视器判"值与声明类型不符"——只画 `—` 且不可编辑(Stats.Health 实测)。
		{
			const TypeSchema* kinds = context.Schemas().Find("TestKit::DefaultKindFixture");
			CHECK(kinds != nullptr);
			CHECK(kinds->Fields.size() == 24);   // Bool/整数族/Float/Double/Vec*/IVec*/UVec*/Quat/Mat*/String
			for (const FieldSchema& field : kinds->Fields)
			{
				CHECK(!std::holds_alternative<std::monostate>(field.Default));
				// 只读摘要 Kind(IVec*/UVec*/Quat/Mat*)不在 ValueMatchesKind 的放行集合里(面板没有行控件),
				// 它们的精确备选由下面的逐条 holds_alternative 断言钉住。
				if (!ScriptProperties::IsSummaryKind(field.K))
					CHECK(ScriptProperties::ValueMatchesKind(field.Default, field.K));
			}
			// 每个 kind 的零值都必须是**它自己那一支**(旧口径下 Float=double、整数族=int ⇒ 全假)。
			CHECK(std::holds_alternative<bool>(FindField(*kinds, "BoolValue")->Default));
			CHECK(std::holds_alternative<int8_t>(FindField(*kinds, "Int8Value")->Default));
			CHECK(std::holds_alternative<int16_t>(FindField(*kinds, "Int16Value")->Default));
			CHECK(std::holds_alternative<int32_t>(FindField(*kinds, "Int32Value")->Default));
			CHECK(std::holds_alternative<int64_t>(FindField(*kinds, "Int64Value")->Default));
			CHECK(std::holds_alternative<uint8_t>(FindField(*kinds, "UInt8Value")->Default));
			CHECK(std::holds_alternative<uint16_t>(FindField(*kinds, "UInt16Value")->Default));
			CHECK(std::holds_alternative<uint32_t>(FindField(*kinds, "UInt32Value")->Default));
			CHECK(std::holds_alternative<uint64_t>(FindField(*kinds, "UInt64Value")->Default));
			CHECK(std::holds_alternative<float>(FindField(*kinds, "FloatValue")->Default));
			CHECK(std::holds_alternative<double>(FindField(*kinds, "DoubleValue")->Default));
			CHECK(std::holds_alternative<glm::vec2>(FindField(*kinds, "Vec2Value")->Default));
			CHECK(std::holds_alternative<glm::vec3>(FindField(*kinds, "Vec3Value")->Default));
			CHECK(std::holds_alternative<glm::vec4>(FindField(*kinds, "Vec4Value")->Default));
			CHECK(std::holds_alternative<glm::ivec2>(FindField(*kinds, "IVec2Value")->Default));
			CHECK(std::holds_alternative<glm::ivec3>(FindField(*kinds, "IVec3Value")->Default));
			CHECK(std::holds_alternative<glm::ivec4>(FindField(*kinds, "IVec4Value")->Default));
			CHECK(std::holds_alternative<glm::uvec2>(FindField(*kinds, "UVec2Value")->Default));
			CHECK(std::holds_alternative<glm::uvec3>(FindField(*kinds, "UVec3Value")->Default));
			CHECK(std::holds_alternative<glm::uvec4>(FindField(*kinds, "UVec4Value")->Default));
			CHECK(std::holds_alternative<glm::quat>(FindField(*kinds, "QuatValue")->Default));
			CHECK(std::holds_alternative<glm::mat3>(FindField(*kinds, "Mat3Value")->Default));
			CHECK(std::holds_alternative<glm::mat4>(FindField(*kinds, "Mat4Value")->Default));
			CHECK(std::holds_alternative<std::string>(FindField(*kinds, "StringValue")->Default));

			// 嵌套 struct 的 Float 子字段(Stats.Health 的同类):声明默认值可被检视器直接当展示值。
			const TypeSchema* nestedFixture = context.Schemas().Find("TestKit::NestedFixture");
			CHECK(nestedFixture != nullptr);
			const FieldSchema* inner = FindField(*nestedFixture, "Inner");
			CHECK(inner != nullptr && inner->GetNested != nullptr);
			const FieldSchema* nestedHealth = FindField(*inner->GetNested(), "Health");
			CHECK(nestedHealth != nullptr && nestedHealth->K == Kind::Float);
			CHECK(ScriptProperties::ValueMatchesKind(nestedHealth->Default, Kind::Float));
		}

		std::printf("World.Schema: all checks passed\n");
		return 0;
	}
	catch (const std::exception& error)
	{
		std::fprintf(stderr, "World.Schema: FAILED: %s\n", error.what());
		return 1;
	}
}
