// P2 W2a:语言无关的行为注册层回归。
//
// 覆盖:两类现成行为的描述(语言标记 / 字段名 + Schema 值类型 + 稳定 field id /
// 四个生命周期槽位)、重复注册被拒且错误可读、按实体枚举的确定性(顺序不依赖
// 哈希遍历)、以及 C++ 与 Luau 行为在同一实体上共存且仍由现有调度路径各自执行。
#include "wldpch.h"
#include "World/Core/Log.h"
#include "World/Core/WorldContext.h"
#include "World/Schema/ComponentSchemaBridge.h"
#include "World/Schema/Schema.h"
#include "World/Schema/SchemaRegistry.h"
#include "World/Script/BehaviorRegistry.h"
#include "World/Script/LuauVm.h"
#include "World/Script/ScriptBindingContext.h"
#include "World/Script/ScriptValue.h"
#include "World/Scene/Components.h"
#include "World/Scene/Entity.h"
#include "World/Scene/Scene.h"
#include "World/Scene/ScriptEngine.h"

#include <cstdio>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace
{
	using namespace World;

	WorldContext& TestContext()
	{
		static WorldContext context;
		return context;
	}

	void Check(bool condition, const char* expression, int line)
	{
		if (!condition)
			throw std::runtime_error(std::string("line ") + std::to_string(line) + ": " + expression);
	}
#define CHECK(expression) Check(static_cast<bool>(expression), #expression, __LINE__)

	// ---- 测试用 C++ 行为:字段是普通成员,schema 访问器直接读写它们 ----

	int s_NativeCreates = 0;
	int s_NativeUpdates = 0;
	int s_NativeDestroys = 0;
	int s_LuaCreates = 0;
	int s_LuaUpdates = 0;
	int s_LuaDestroys = 0;

	class ProbeBehavior final : public ScriptableEntity
	{
	public:
		float Value = 0.0f;
		std::string Label;

	protected:
		void OnCreate() override { ++s_NativeCreates; }
		void OnUpdate(Timestep) override { ++s_NativeUpdates; }
		void OnDestroy() override { ++s_NativeDestroys; }
	};

	class ZetaBehavior final : public ScriptableEntity
	{
	public:
		int32_t Score = 0;

	protected:
		void OnCreate() override {}
	};

	// 字段 id 与 schema-compiler 同规则:Fnv1a64("<Module>::<Type>.<FieldName>")。
	void RegisterTestSchemas()
	{
		static const Schema::ScriptBinding probeBinding = Schema::MakeScriptBinding<ProbeBehavior>();
		static const Schema::FieldSchema probeValue = {
			Schema::FieldId{ Schema::Fnv1a64("BehaviorTest::ProbeBehavior.Value") },
			"Value",
			Schema::Kind::Float,
			[](const void* instance) { return Schema::Value(static_cast<const ProbeBehavior*>(instance)->Value); },
			[](void* instance, const Schema::Value& value) { static_cast<ProbeBehavior*>(instance)->Value = std::get<float>(value); },
			nullptr, nullptr, nullptr, nullptr, nullptr,
			Schema::FieldMetadata{},
			Schema::Value(0.0f),
		};
		static const Schema::FieldSchema probeLabel = {
			Schema::FieldId{ Schema::Fnv1a64("BehaviorTest::ProbeBehavior.Label") },
			"Label",
			Schema::Kind::String,
			[](const void* instance) { return Schema::Value(static_cast<const ProbeBehavior*>(instance)->Label); },
			[](void* instance, const Schema::Value& value) { static_cast<ProbeBehavior*>(instance)->Label = std::get<std::string>(value); },
			nullptr, nullptr, nullptr, nullptr, nullptr,
			Schema::FieldMetadata{},
			Schema::Value(std::string()),
		};
		static const Schema::TypeSchema probeSchema = {
			Schema::TypeId{ "BehaviorTest::ProbeBehavior" },
			"ProbeBehavior",
			Schema::WE_SCHEMA_ABI_VERSION,
			sizeof(ProbeBehavior),
			Schema::TypeCategory::Script,
			{ probeValue, probeLabel },
			nullptr,
			&probeBinding,
		};

		static const Schema::ScriptBinding zetaBinding = Schema::MakeScriptBinding<ZetaBehavior>();
		static const Schema::FieldSchema zetaScore = {
			Schema::FieldId{ Schema::Fnv1a64("BehaviorTest::ZetaBehavior.Score") },
			"Score",
			Schema::Kind::Int32,
			[](const void* instance) { return Schema::Value(static_cast<const ZetaBehavior*>(instance)->Score); },
			[](void* instance, const Schema::Value& value) { static_cast<ZetaBehavior*>(instance)->Score = std::get<int32_t>(value); },
			nullptr, nullptr, nullptr, nullptr, nullptr,
			Schema::FieldMetadata{},
			Schema::Value(int32_t(0)),
		};
		static const Schema::TypeSchema zetaSchema = {
			Schema::TypeId{ "BehaviorTest::ZetaBehavior" },
			"ZetaBehavior",
			Schema::WE_SCHEMA_ABI_VERSION,
			sizeof(ZetaBehavior),
			Schema::TypeCategory::Script,
			{ zetaScore },
			nullptr,
			&zetaBinding,
		};

		// 注册顺序刻意与模块 id 字典序相反(Zeta 先、Probe 后),用来证明枚举顺序
		// 来自注册表的稳定排序而不是插入顺序。
		CHECK(TestContext().Schemas().Register({ "BehaviorTest", 1 }, zetaSchema) == Schema::SchemaRegistry::Status::Ok);
		CHECK(TestContext().Schemas().Register({ "BehaviorTest", 1 }, probeSchema) == Schema::SchemaRegistry::Status::Ok);
	}

	// ---- 夹具脚本(Game/assets/scripts/tests/LifecycleProbe.lua)要求的注入函数 ----

	ScriptValue RecordLuaPhase(const ScriptValue* args, std::size_t count)
	{
		if (count < 3) throw std::runtime_error("T02Record expects (id, phase, localUpdates)");
		std::string phase;
		if (!args[1].AsString(&phase)) throw std::runtime_error("T02Record phase must be a string");
		if (phase == "create") ++s_LuaCreates;
		else if (phase == "update") ++s_LuaUpdates;
		else if (phase == "destroy") ++s_LuaDestroys;
		return ScriptValue::Nil();
	}

	ScriptValue RecordLuaAction(const ScriptValue* args, std::size_t count)
	{
		if (count < 2) throw std::runtime_error("T02Action expects (entity, phase)");
		Entity* entity = nullptr;
		if (!ScriptEngine::GetBindingContext().Unwrap("Entity", args[0], &entity) || !entity)
			throw std::runtime_error("T02Action entity must be an Entity");
		std::string phase;
		if (!args[1].AsString(&phase)) throw std::runtime_error("T02Action phase must be a string");
		return ScriptValue::Nil();
	}

	void InstallRecorderGlobals()
	{
		auto& bindings = ScriptEngine::GetBindingContext();
		CHECK(ScriptEngine::GetState().SetGlobal("T02Record", bindings.CreateFunction("T02Record", &RecordLuaPhase)));
		CHECK(ScriptEngine::GetState().SetGlobal("T02Action", bindings.CreateFunction("T02Action", &RecordLuaAction)));
	}

	void ClearRecorderGlobals()
	{
		if (!ScriptEngine::IsInitialized()) return;
		ScriptEngine::GetState().ClearGlobal("T02Record");
		ScriptEngine::GetState().ClearGlobal("T02Action");
	}

	constexpr const char* kLuaFixturePath = "scripts/tests/LifecycleProbe.lua";

	// ---- 用例 ----

	// 1. C++(schema Script 类型)能产出 BehaviorDesc:语言/显示名/字段/生命周期槽位,
	//    字段 id 与 Kind 直接来自 TypeSchema;重复注册被拒且错误可读。
	void NativeBehaviorDescriptions()
	{
		BehaviorRegistry& registry = BehaviorRegistry::Instance();
		std::vector<std::string> errors;
		const std::size_t ensured = ScriptEngine::EnsureSchemaBehaviors(TestContext().Schemas(), &errors);
		CHECK(errors.empty());
		CHECK(ensured == 2);

		const BehaviorDesc* probe = registry.Find("BehaviorTest::ProbeBehavior");
		CHECK(probe != nullptr);
		CHECK(probe->ModuleId == BehaviorRegistry::NativeModuleId(*TestContext().Schemas().Find("BehaviorTest::ProbeBehavior")));
		CHECK(probe->Language == BehaviorLanguage::Cpp);
		CHECK(std::string(BehaviorLanguageName(probe->Language)) == "Cpp");
		CHECK(probe->DisplayName == "ProbeBehavior");
		CHECK(probe->Fields.size() == 2);
		// 字段按 FieldId 升序:Value(0x4F74...) 在 Label(0xDFFC...) 之前。
		CHECK(probe->Fields[0].Name == "Value" && probe->Fields[0].Type == Schema::Kind::Float);
		CHECK(probe->Fields[0].FieldId == 0x4F7425542AC41032ull);
		CHECK(probe->Fields[1].Name == "Label" && probe->Fields[1].Type == Schema::Kind::String);
		CHECK(probe->Fields[1].FieldId == 0xDFFCF630BC252255ull);
		CHECK(probe->Lifecycle.OnCreate && probe->Lifecycle.OnUpdate && probe->Lifecycle.OnDestroy);
		CHECK(!probe->Lifecycle.OnEvent); // 预留槽:W2a 没有事件前端

		// 与 schema 注册表逐字段一致(单一事实源:没有在行为层重新算 id 或类型)。
		const Schema::TypeSchema* schema = TestContext().Schemas().Find("BehaviorTest::ProbeBehavior");
		CHECK(schema != nullptr && schema->Fields.size() == probe->Fields.size());
		for (std::size_t index = 0; index < schema->Fields.size(); ++index)
		{
			const Schema::FieldSchema& source = schema->Fields[index];
			const BehaviorFieldDesc& copied = probe->Fields[index];
			CHECK(source.Id.Value == copied.FieldId);
			CHECK(source.Name == copied.Name);
			CHECK(source.K == copied.Type);
		}

		// 重复注册(同 id,同内容 / 同 id,不同内容)都必须被拒,且错误可读、原描述不被覆盖。
		std::string error;
		BehaviorDesc duplicate = *probe;
		CHECK(!registry.Register(duplicate, &error));
		CHECK(error.find("BehaviorTest::ProbeBehavior") != std::string::npos);
		CHECK(error.find("already registered") != std::string::npos);

		BehaviorDesc conflicting = *probe;
		conflicting.Fields.clear();
		error.clear();
		CHECK(!registry.Register(conflicting, &error));
		CHECK(!error.empty());
		CHECK(registry.Find("BehaviorTest::ProbeBehavior")->Fields.size() == 2);

		// 非 Script 类型(组件)不能登记成行为。
		const Schema::TypeSchema* component = TestContext().Schemas().Find("World::TagComponent");
		CHECK(component != nullptr);
		error.clear();
		CHECK(!registry.RegisterNative(*component, &error) && !error.empty());

		// 枚举按 ModuleId 升序:插入顺序是 Zeta→Probe,List() 必须是 Probe→Zeta。
		const std::vector<const BehaviorDesc*> listed = registry.List();
		CHECK(listed.size() >= 2);
		for (std::size_t index = 1; index < listed.size(); ++index)
			CHECK(listed[index - 1]->ModuleId < listed[index]->ModuleId);
		CHECK(listed[0]->ModuleId == "BehaviorTest::ProbeBehavior");
		CHECK(listed[1]->ModuleId == "BehaviorTest::ZetaBehavior");

		// 幂等:同一批 schema 再登记一次不新增也不报错(宿主可每帧调用)。
		errors.clear();
		CHECK(ScriptEngine::EnsureSchemaBehaviors(TestContext().Schemas(), &errors) == 2 && errors.empty());
		CHECK(registry.Size() == listed.size());
	}

	// 2. Luau 行为能从 CachedFields 产出 BehaviorDesc(真实编辑器加载路径),
	//    field id 复用现成的 Fnv1a64;描述变化走 Replace 刷新而不是新增。
	void LuaBehaviorDescriptions()
	{
		BehaviorRegistry& registry = BehaviorRegistry::Instance();
		Scene scene(TestContext());
		Entity entity = Entity::CreateEntity(&scene, "lua behavior");
		LuaScriptComponent& script = entity.AddComponent<LuaScriptComponent>(kLuaFixturePath);
		CHECK(ScriptEngine::InitScriptForEditor(script));

		const std::string moduleId = BehaviorRegistry::LuaModuleId(kLuaFixturePath);
		CHECK(moduleId == std::string("Lua:") + kLuaFixturePath);
		const BehaviorDesc* desc = registry.Find(moduleId);
		CHECK(desc != nullptr);
		CHECK(desc->Language == BehaviorLanguage::Luau);
		CHECK(std::string(BehaviorLanguageName(desc->Language)) == "Luau");
		CHECK(desc->DisplayName == kLuaFixturePath);
		CHECK(desc->Fields.size() == 2);
		// 夹具注解:---@field LocalUpdates integer / ---@field Mode string
		CHECK(desc->Fields[0].Name == "LocalUpdates" && desc->Fields[0].Type == Schema::Kind::Int32);
		CHECK(desc->Fields[0].FieldId == 0x4BDD718EF303013Cull);
		CHECK(desc->Fields[1].Name == "Mode" && desc->Fields[1].Type == Schema::Kind::String);
		CHECK(desc->Fields[1].FieldId == 0x7FC545586A0ED318ull);
		CHECK(desc->Lifecycle.OnCreate && desc->Lifecycle.OnUpdate && desc->Lifecycle.OnDestroy);
		CHECK(!desc->Lifecycle.OnEvent);

		// field id 派生规则 = 现成的 Fnv1a64,不是新哈希;跨进程稳定(硬编码值见上)。
		CHECK(BehaviorRegistry::LuaFieldId("Mode") == Schema::Fnv1a64(std::string("World::LuaScriptComponent.Mode")));
		CHECK(BehaviorRegistry::LuaFieldId("Mode") != BehaviorRegistry::LuaFieldId("LocalUpdates"));
		CHECK(BehaviorRegistry::LuaFieldId("") == Schema::Fnv1a64(std::string("World::LuaScriptComponent.")));

		// 幂等:同模块同描述 → true 且条数不变。
		const std::size_t before = registry.Size();
		std::string error;
		CHECK(ScriptEngine::EnsureLuaBehavior(script, &error) && error.empty());
		CHECK(registry.Size() == before);
		CHECK(BehaviorDescEquals(*registry.Find(moduleId), *desc));

		// 脚本编辑(字段集合变化)→ 刷新同一条描述,而不是新增/被拒。
		script.CachedFields.erase("Mode");
		script.CachedFields["Speed"] = { LuaFieldType::Float, 3.5f };
		error.clear();
		CHECK(ScriptEngine::EnsureLuaBehavior(script, &error) && error.empty());
		CHECK(registry.Size() == before);
		desc = registry.Find(moduleId);
		CHECK(desc != nullptr && desc->Fields.size() == 2);
		CHECK(desc->Fields[0].Name == "LocalUpdates" && desc->Fields[0].Type == Schema::Kind::Int32);
		CHECK(desc->Fields[1].Name == "Speed" && desc->Fields[1].Type == Schema::Kind::Float);
		CHECK(desc->Fields[1].FieldId == 0xEDB8CFC04F67C11Aull);

		// 空路径 / 未登记脚本:不产生描述。
		LuaScriptComponent empty;
		error.clear();
		CHECK(!ScriptEngine::EnsureLuaBehavior(empty, &error) && !error.empty());
		CHECK(registry.Find(BehaviorRegistry::LuaModuleId("scripts/tests/NotLoaded.lua")) == nullptr);

		// 严格注册接口:同 id 再 Register 会被拒(与 §1 的失败路径同源)。
		error.clear();
		CHECK(!registry.RegisterLua(script, &error) && error.find(moduleId) != std::string::npos);
	}

	// 3. 给定实体枚举行为描述:顺序稳定(字典序),不依赖哈希遍历;空/无效实体返回空。
	void EntityBehaviorEnumeration()
	{
		BehaviorRegistry& registry = BehaviorRegistry::Instance();
		Scene scene(TestContext());

		Entity both = Entity::CreateEntity(&scene, "two behaviors");
		both.AddComponent<NativeScriptComponent>().ScriptName = "ProbeBehavior"; // 短名,走 schema 解析
		LuaScriptComponent& lua = both.AddComponent<LuaScriptComponent>(kLuaFixturePath);
		CHECK(ScriptEngine::InitScriptForEditor(lua));

		const auto first = registry.DescribeEntity(scene, static_cast<entt::entity>(both));
		CHECK(first.size() == 2);
		CHECK(first[0]->ModuleId == "BehaviorTest::ProbeBehavior");
		CHECK(first[0]->Language == BehaviorLanguage::Cpp);
		CHECK(first[1]->ModuleId == std::string("Lua:") + kLuaFixturePath);
		CHECK(first[1]->Language == BehaviorLanguage::Luau);

		// 重复调用 / Entity 重载 → 同一顺序、同一对象(注册表不重建描述)。
		const auto second = registry.DescribeEntity(scene, static_cast<entt::entity>(both));
		CHECK(first == second);
		const auto viaEntity = registry.DescribeEntity(both);
		CHECK(viaEntity == first);

		// 全名直查路径:ScriptName 用模块 id 全名时也能解析。
		Entity named = Entity::CreateEntity(&scene, "full name");
		named.AddComponent<NativeScriptComponent>().ScriptName = "BehaviorTest::ZetaBehavior";
		const auto listed = registry.DescribeEntity(named);
		CHECK(listed.size() == 1 && listed[0]->ModuleId == "BehaviorTest::ZetaBehavior");

		// 未注册脚本(只有组件、没有描述)→ 不出现,不抛。
		Entity unknown = Entity::CreateEntity(&scene, "unknown behavior");
		unknown.AddComponent<NativeScriptComponent>().ScriptName = "BehaviorTest::MissingBehavior";
		unknown.AddComponent<LuaScriptComponent>("scripts/tests/NotRegistered.lua");
		CHECK(registry.DescribeEntity(scene, static_cast<entt::entity>(unknown)).empty());

		Entity empty = Entity::CreateEntity(&scene, "no behaviors");
		CHECK(registry.DescribeEntity(empty).empty());
		CHECK(registry.DescribeEntity(scene, entt::null).empty());
		CHECK(registry.DescribeEntity(Entity()).empty());
	}

	// 4. C++ 与 Luau 行为在同一实体上共存,并沿现有调度路径各自拿到 OnCreate/OnUpdate/OnDestroy。
	void CppAndLuaBehaviorsCoexist()
	{
		s_NativeCreates = s_NativeUpdates = s_NativeDestroys = 0;
		s_LuaCreates = s_LuaUpdates = s_LuaDestroys = 0;

		BehaviorRegistry& registry = BehaviorRegistry::Instance();
		Scene scene(TestContext());
		Entity entity = Entity::CreateEntity(&scene, "coexistence");
		NativeScriptComponent& native = entity.AddComponent<NativeScriptComponent>();
		native.ScriptName = "ProbeBehavior";
		LuaScriptComponent& lua = entity.AddComponent<LuaScriptComponent>(kLuaFixturePath);
		CHECK(ScriptEngine::InitScriptForEditor(lua));
		CHECK(lua.State == ScriptInstanceState::Stopped);

		CHECK(registry.DescribeEntity(entity).size() == 2);

		scene.OnScriptStart();
		CHECK(native.State == ScriptInstanceState::Running);
		CHECK(native.Instance != nullptr);
		CHECK(lua.State == ScriptInstanceState::Running);
		CHECK(s_NativeCreates == 1 && s_LuaCreates == 1);
		CHECK(s_NativeUpdates == 0 && s_LuaUpdates == 0);

		scene.OnScriptUpdate(Timestep(1.0f / 60.0f));
		scene.OnScriptUpdate(Timestep(1.0f / 60.0f));
		CHECK(s_NativeUpdates == 2 && s_LuaUpdates == 2);
		CHECK(registry.DescribeEntity(entity).size() == 2);

		scene.OnRuntimeStop();
		CHECK(s_NativeDestroys == 1 && s_LuaDestroys == 1);
		CHECK(native.State == ScriptInstanceState::Stopped);
		CHECK(lua.State == ScriptInstanceState::Stopped);
	}

	// 5. 描述等价判定:字段顺序无关(身份只看 id/名字/类型)。
	void DescriptionEquality()
	{
		BehaviorDesc left;
		left.ModuleId = "Lua:scripts/equality.luau";
		left.DisplayName = "equality";
		left.Language = BehaviorLanguage::Luau;
		left.Fields = {
			{ BehaviorRegistry::LuaFieldId("Alpha"), "Alpha", Schema::Kind::Float },
			{ BehaviorRegistry::LuaFieldId("Beta"), "Beta", Schema::Kind::String },
		};
		left.Lifecycle = BehaviorLifecycleSlots{ true, true, true, false };
		BehaviorDesc right = left;
		std::swap(right.Fields[0], right.Fields[1]);
		CHECK(BehaviorDescEquals(left, right));
		right.Fields[1].Type = Schema::Kind::Int32;
		CHECK(!BehaviorDescEquals(left, right));
		right = left;
		right.Lifecycle.OnEvent = true;
		CHECK(!BehaviorDescEquals(left, right));
		right = left;
		right.ModuleId = "Lua:scripts/other.luau";
		CHECK(!BehaviorDescEquals(left, right));
	}
}

int main()
{
	try
	{
		World::Log::Init();
		World::ScriptEngine::Init();
		World::BehaviorRegistry::Instance().Clear();
		RegisterTestSchemas();
		InstallRecorderGlobals();

		const std::pair<const char*, void(*)()> tests[] = {
			{ "native schema descriptions and duplicate rejection", NativeBehaviorDescriptions },
			{ "lua cached-field descriptions and refresh", LuaBehaviorDescriptions },
			{ "entity behavior enumeration is deterministic", EntityBehaviorEnumeration },
			{ "C++ and Luau behaviors coexist on one entity", CppAndLuaBehaviorsCoexist },
			{ "behavior description equality", DescriptionEquality },
		};
		int failures = 0;
		for (const auto& [name, test] : tests)
		{
			try { test(); std::printf("[PASS] %s\n", name); }
			catch (const std::exception& error) { ++failures; std::fprintf(stderr, "[FAIL] %s: %s\n", name, error.what()); }
			catch (...) { ++failures; std::fprintf(stderr, "[FAIL] %s: unknown exception\n", name); }
		}
		ClearRecorderGlobals();
		World::ScriptEngine::Shutdown();
		if (failures == 0)
		{
			std::printf("World.BehaviorRegistry: all checks passed\n");
			return 0;
		}
		std::fprintf(stderr, "World.BehaviorRegistry: %d group(s) failed\n", failures);
		return 1;
	}
	catch (const std::exception& error)
	{
		std::fprintf(stderr, "Test setup failed: %s\n", error.what());
		if (World::ScriptEngine::IsInitialized()) World::ScriptEngine::Shutdown();
		return 1;
	}
	catch (...)
	{
		std::fprintf(stderr, "Test setup failed: unknown exception\n");
		if (World::ScriptEngine::IsInitialized()) World::ScriptEngine::Shutdown();
		return 1;
	}
}
