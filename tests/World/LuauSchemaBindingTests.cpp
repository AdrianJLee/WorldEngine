// P2 W3a-A1:脚本字段代理(Entity:GetComponent 返回 schema 驱动的字段代理)。
//
// 覆盖:Kind → Lua 映射表(单一入口,W3a-A2 存根渲染复用)、字段读取(vec/mat/标量/字符串/
// 资产路径/UUID 身份)、字段写回(类型校验 + 与 PropertiesPanel 同一套派生状态刷新)、
// 同一脚本运行内写字段在下一帧 C++ 侧可见(真实 Scene::OnScriptUpdate 调度),以及全部失败路径
// (未知组件/非组件类型/非法或过期实体/未知字段/写错类型/Transient/只读/未映射 Kind/
// 已移除组件/已销毁场景)都必须给可读错误或 nil,不能崩。
#include "wldpch.h"
#include "World/Core/Log.h"
#include "World/Core/WorldContext.h"
#include "World/Schema/Schema.h"
#include "World/Schema/SchemaRegistry.h"
#include "World/Script/BindComponentAccess.h"
#include "World/Script/LuauVm.h"
#include "World/Script/ScriptBindingContext.h"
#include "World/Script/ScriptRef.h"
#include "World/Script/ScriptValue.h"
#include "World/Scene/Components.h"
#include "World/Scene/Entity.h"
#include "World/Scene/Scene.h"
#include "World/Scene/ScriptEngine.h"

#include <cstdio>
#include <new>
#include <stdexcept>
#include <string>
#include <utility>

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

	void RunExpectOk(const std::string& source, const char* chunk, int line)
	{
		std::string error;
		if (!ScriptEngine::GetState().RunString(source, chunk, &error))
			throw std::runtime_error(std::string("line ") + std::to_string(line) + ": " + chunk +
				" failed: " + error);
	}
#define RUN_OK(source, chunk) RunExpectOk(source, chunk, __LINE__)

	void SetEntityGlobal(const char* name, Entity entity)
	{
		ScriptBindingContext& bindings = ScriptEngine::GetBindingContext();
		const ScriptValue value = bindings.NewUserdata("Entity");
		Entity* target = nullptr;
		CHECK(bindings.Unwrap("Entity", value, &target) && target != nullptr);
		new (target) Entity(entity);
		CHECK(ScriptEngine::GetState().SetGlobal(name, value));
	}

	void SetProxyGlobal(const char* name, const Entity& entity, const char* typeName)
	{
		const Schema::TypeSchema* schema = TestContext().Schemas().Find(typeName);
		CHECK(schema != nullptr);
		CHECK(ScriptEngine::GetState().SetGlobal(name,
			MakeComponentProxy(ScriptEngine::GetBindingContext(), entity, *schema)));
	}

	Schema::FieldSchema MakeField(Schema::Kind kind)
	{
		Schema::FieldSchema field;
		field.Name = "Probe";
		field.K = kind;
		return field;
	}

	// 未映射的 Kind 返回 nullptr;这里统一转成 "" 便于断言,也避免 std::string(nullptr)。
	std::string MappingType(Schema::Kind kind)
	{
		const char* name = DescribeScriptField(MakeField(kind)).LuaTypeName;
		return name ? name : std::string();
	}

	// 1.映射表集中在一处:Kind → Lua 类型;Object(UUID) 是唯一的嵌套特例(只读十进制字符串)。
	void MappingTableIsSchemaDriven()
	{
		CHECK(MappingType(Schema::Kind::Bool) == "boolean");
		CHECK(MappingType(Schema::Kind::Int8) == "number");
		CHECK(MappingType(Schema::Kind::Int32) == "number");
		CHECK(MappingType(Schema::Kind::Int64) == "number");
		CHECK(MappingType(Schema::Kind::UInt8) == "number");
		CHECK(MappingType(Schema::Kind::UInt64) == "number");
		CHECK(MappingType(Schema::Kind::Float) == "number");
		CHECK(MappingType(Schema::Kind::Double) == "number");
		CHECK(MappingType(Schema::Kind::Enum) == "number");
		CHECK(MappingType(Schema::Kind::String) == "string");
		CHECK(MappingType(Schema::Kind::Asset) == "string");
		CHECK(MappingType(Schema::Kind::Vec2) == "vec2");
		CHECK(MappingType(Schema::Kind::Vec3) == "vec3");
		CHECK(MappingType(Schema::Kind::Vec4) == "vec4");
		CHECK(MappingType(Schema::Kind::Mat3) == "mat3");
		CHECK(MappingType(Schema::Kind::Mat4) == "mat4");

		// 本包未映射的 Kind:读写都会报可读错误,而不是静默 nil。
		CHECK(MappingType(Schema::Kind::None).empty());
		CHECK(MappingType(Schema::Kind::Quat).empty());
		CHECK(MappingType(Schema::Kind::IVec3).empty());
		CHECK(MappingType(Schema::Kind::UVec4).empty());

		static const Schema::TypeSchema* s_UuidSchema = TestContext().Schemas().Find("World::UUID");
		static const Schema::TypeSchema* s_CameraSchema = TestContext().Schemas().Find("World::SceneCamera");
		CHECK(s_UuidSchema != nullptr && s_CameraSchema != nullptr);

		Schema::FieldSchema uuidField = MakeField(Schema::Kind::Object);
		uuidField.GetNested = []() -> const Schema::TypeSchema* { return s_UuidSchema; };
		const ScriptFieldMapping uuidMapping = DescribeScriptField(uuidField);
		CHECK(uuidMapping.LuaTypeName != nullptr && std::string(uuidMapping.LuaTypeName) == "string");
		CHECK(uuidMapping.ReadOnly && uuidMapping.UuidIdentity);

		// 其它嵌套结构(SceneCamera 等)本包未映射。
		Schema::FieldSchema cameraField = MakeField(Schema::Kind::Object);
		cameraField.GetNested = []() -> const Schema::TypeSchema* { return s_CameraSchema; };
		CHECK(DescribeScriptField(cameraField).LuaTypeName == nullptr);

		// 代理类型已注册,且脚本不能自己构造代理(方法表里没有 new)。
		CHECK(ScriptEngine::GetBindingContext().IsUserTypeRegistered(ComponentProxyLuaTypeName));
		CHECK(ScriptEngine::GetBindingContext().UserTypeSize(ComponentProxyLuaTypeName) > 0);
		RUN_OK(R"LUA(
assert(type(ComponentProxy) == "table")
assert(ComponentProxy.new == nil)
)LUA", "proxy_type_shape");
	}

	// 2.读路径:vec3/mat4/字符串/布尔/资产路径/UUID 身份/nil 语义。
	void ProxyReadsSchemaFields()
	{
		Scene scene(TestContext());
		Entity entity = Entity::CreateEntity(&scene, "proxy read probe");
		TransformComponent& transform = entity.AddComponent<TransformComponent>();
		transform.SetLocation(glm::vec3(3.5f, -4.0f, 0.25f));
		entity.AddComponent<SpriteComponent>();
		entity.AddComponent<HierarchyComponent>();
		Entity empty = Entity::CreateEntity(&scene, "no transform");

		SetEntityGlobal("T02ReadEntity", entity);
		SetEntityGlobal("T02EmptyEntity", empty);

		RUN_OK(R"LUA(
local entity = T02ReadEntity
local transform = entity:GetComponent("TransformComponent")
assert(type(transform) == "userdata")
assert(typeof(transform) == "ComponentProxy")
local location = transform.Location
assert(typeof(location) == "vec3")
assert(location.x == 3.5 and location.y == -4.0 and location.z == 0.25)
assert(tostring(transform) == "ComponentProxy(TransformComponent)")
-- 字段读取按值快照:两次取用的值一致(代理身份/句柄语义归 W4)。
local first = transform.Location
local second = transform.Location
assert(first.x == second.x and first.y == second.y and first.z == second.z)
local matrix = transform.Transform
assert(typeof(matrix) == "mat4")
assert(matrix[3].x == 3.5 and matrix[3].y == -4.0 and matrix[3].z == 0.25 and matrix[3].w == 1.0)
assert(entity:GetComponent("TagComponent").Tag == "proxy read probe")
assert(entity:GetComponent("HierarchyComponent").InheritTransform == true)
assert(entity:GetComponent("SpriteComponent").Texture == "")
assert(T02EmptyEntity:GetComponent("TransformComponent") == nil)
T02UuidText = entity:GetComponent("UUIDComponent").ID
assert(type(T02UuidText) == "string" and #T02UuidText > 0)
)LUA", "proxy_reads");

		const uint64_t uuid = static_cast<uint64_t>(entity.GetComponent<UUIDComponent>().ID);
		std::string uuidText;
		CHECK(ScriptEngine::GetState().GetGlobal("T02UuidText").AsString(&uuidText));
		CHECK(uuidText == std::to_string(uuid));
	}

	// 3.写路径:vec3/标量/布尔/字符串/枚举写回,C++ 侧立即一致;派生矩阵按既有约定重算。
	void ProxyWritesRoundTrip()
	{
		Scene scene(TestContext());
		Entity entity = Entity::CreateEntity(&scene, "proxy write probe");
		TransformComponent& transform = entity.AddComponent<TransformComponent>();
		SpriteComponent& sprite = entity.AddComponent<SpriteComponent>();
		HierarchyComponent& hierarchy = entity.AddComponent<HierarchyComponent>();
		RigidBody2DComponent& body = entity.AddComponent<RigidBody2DComponent>();
		SetEntityGlobal("T02WriteEntity", entity);

		RUN_OK(R"LUA(
local entity = T02WriteEntity
entity:GetComponent("TransformComponent").Location = vec3.new(1, 2, 3)
entity:GetComponent("SpriteComponent").TilingFactor = 2.5
entity:GetComponent("HierarchyComponent").InheritTransform = false
entity:GetComponent("TagComponent").Tag = "scripted tag"
local body = entity:GetComponent("RigidBody2DComponent")
body.Type = "Dynamic"
assert(body.Type == 1)
body.Type = 2
assert(body.Type == 2)
body.FixedRotation = true
)LUA", "proxy_writes");

		CHECK(transform.Location.x == 1.0f && transform.Location.y == 2.0f && transform.Location.z == 3.0f);
		// Hierarchy/渲染读的是 Transform 缓存矩阵(Hierarchy.cpp:166),写入后必须已重算。
		CHECK(transform.Transform[3].x == 1.0f && transform.Transform[3].y == 2.0f && transform.Transform[3].z == 3.0f);
		CHECK(transform.Transform[3].w == 1.0f);
		CHECK(transform.RotationQuat.w == 1.0f);
		CHECK(sprite.TilingFactor == 2.5f);
		CHECK(!hierarchy.InheritTransform);
		CHECK(entity.GetComponent<TagComponent>().Tag == "scripted tag");
		CHECK(body.Type == RigidBody2DComponent::BodyType::Kinematic);
		CHECK(body.FixedRotation);
	}

	constexpr const char* kLuaFixturePath = "scripts/tests/LifecycleProbe.lua";

	ScriptValue RecordNoop(const ScriptValue*, std::size_t) { return ScriptValue::Nil(); }

	// 4.真实调度:同一个脚本运行里通过代理写字段,下一帧 C++ 侧可见(不需要任何 flush)。
	void ProxyWriteFromScriptUpdateIsVisibleNextFrame()
	{
		Scene scene(TestContext());
		Entity entity = Entity::CreateEntity(&scene, "proxy dispatch");
		TransformComponent& transform = entity.AddComponent<TransformComponent>();
		transform.SetLocation(glm::vec3(0.0f));
		LuauScriptComponent& script = entity.AddComponent<LuauScriptComponent>(kLuaFixturePath);
		CHECK(ScriptEngine::InitScriptForEditor(script));

		LuauVm& vm = ScriptEngine::GetState();
		ScriptBindingContext& bindings = ScriptEngine::GetBindingContext();
		CHECK(vm.SetGlobal("T02Record", bindings.CreateFunction("T02Record", &RecordNoop)));
		std::string error;
		const ScriptFunctionRef action = vm.CompileFunction(R"LUA(
local entity, phase = ...
if phase == "update" then
    local component = entity:GetComponent("TransformComponent")
    assert(component ~= nil)
    component.Location = vec3.new(1, 2, 3)
end
)LUA", "LuauSchemaBindingTests.UpdateAction", ScriptTableRef(), &error);
		CHECK(action.IsValid() && error.empty());
		CHECK(vm.SetGlobal("T02Action", action.ToValue()));

		scene.OnScriptStart();
		CHECK(script.Runtime.State == ScriptInstanceState::Running);
		CHECK(transform.Location.x == 0.0f);   // OnCreate 阶段没有写
		scene.OnScriptUpdate(Timestep(1.0f / 60.0f));
		CHECK(script.Runtime.LastError.empty());
		CHECK(transform.Location.x == 1.0f && transform.Location.y == 2.0f && transform.Location.z == 3.0f);
		CHECK(transform.Transform[3].x == 1.0f);
		scene.OnRuntimeStop();
		CHECK(script.Runtime.State == ScriptInstanceState::Stopped);

		vm.ClearGlobal("T02Action");
		vm.ClearGlobal("T02Record");
	}

	// 5.失败路径:全部是可读错误/ nil,不是崩溃。
	void ProxyAccessFailsSafely()
	{
		Scene scene(TestContext());
		Entity entity = Entity::CreateEntity(&scene, "proxy error probe");
		entity.AddComponent<TransformComponent>();
		entity.AddComponent<SpriteComponent>();
		entity.AddComponent<HierarchyComponent>();
		entity.AddComponent<RigidBody2DComponent>();

		Entity staleComponentEntity = Entity::CreateEntity(&scene, "stale component");
		staleComponentEntity.AddComponent<TransformComponent>();
		SetProxyGlobal("T02StaleComponent", staleComponentEntity, "World::TransformComponent");
		staleComponentEntity.RemoveComponent<TransformComponent>();   // 停止态下立即提交,代理随之失效
		CHECK(!staleComponentEntity.HasComponent<TransformComponent>());

		Entity staleEntityEntity = Entity::CreateEntity(&scene, "stale entity");
		staleEntityEntity.AddComponent<TransformComponent>();
		SetProxyGlobal("T02StaleEntity", staleEntityEntity, "World::TransformComponent");
		Entity::DestroyEntity(&scene, staleEntityEntity);
		CHECK(!staleEntityEntity.IsValid());

		SetEntityGlobal("T02ErrorEntity", entity);
		SetEntityGlobal("T02InvalidEntity", Entity());
		SetProxyGlobal("T02ErrorProxy", entity, "World::TransformComponent");
		SetProxyGlobal("T02ErrorSprite", entity, "World::SpriteComponent");
		SetProxyGlobal("T02ErrorHierarchy", entity, "World::HierarchyComponent");
		SetProxyGlobal("T02ErrorBody", entity, "World::RigidBody2DComponent");
		SetProxyGlobal("T02ErrorUuid", entity, "World::UUIDComponent");

		RUN_OK(R"LUA(
function T02ExpectError(fn, needle)
    local ok, err = pcall(fn)
    assert(not ok, "expected an error containing: " .. needle)
    assert(type(err) == "string", "error text must be a string, got " .. type(err))
    assert(string.find(err, needle, 1, true), "expected '" .. needle .. "' in: " .. err)
end

-- 未知组件名 / 非组件类型 / 非法实体:与现有 Entity 方法同一条错误路径。
T02ExpectError(function() return T02ErrorEntity:GetComponent("NopeComponent") end, "requires a registered component type")
T02ExpectError(function() return T02ErrorEntity:GetComponent("World::UUID") end, "requires a registered component type")
T02ExpectError(function() return T02ErrorEntity:HasComponent("World::SceneCamera") end, "requires a registered component type")
T02ExpectError(function() return T02InvalidEntity:GetComponent("TransformComponent") end, "invalid/expired handle")

-- 未知字段(读/写)与写错类型。
T02ExpectError(function() return T02ErrorProxy.Loction end, "no field 'Loction'")
T02ExpectError(function() T02ErrorProxy.Loction = 1 end, "no field 'Loction'")
T02ExpectError(function() T02ErrorProxy.Location = 5 end, "expects a vec3 userdata")
T02ExpectError(function() T02ErrorProxy.Location = {} end, "expects a vec3 userdata")
T02ExpectError(function() T02ErrorSprite.TilingFactor = "x" end, "expects a finite number")
T02ExpectError(function() T02ErrorSprite.TilingFactor = 0 / 0 end, "expects a finite number")
T02ExpectError(function() T02ErrorHierarchy.InheritTransform = 1 end, "expects a boolean")
T02ExpectError(function() T02ErrorEntity:GetComponent("TagComponent").Tag = 7 end, "expects a string")
T02ExpectError(function() T02ErrorSprite.Texture = 7 end, "expects an asset path string")
T02ExpectError(function() T02ErrorBody.Type = "Nope" end, "has no enum name 'Nope'")
T02ExpectError(function() T02ErrorBody.Type = 9 end, "has no enum value 9")

-- Transient(派生状态)与未映射 Kind:写/读都必须显式报错。
T02ExpectError(function() T02ErrorProxy.Transform = mat4.new(1.0) end, "transient")
T02ExpectError(function() return T02ErrorProxy.RotationQuat end, "no script mapping yet")
T02ExpectError(function() T02ErrorProxy.RotationQuat = 1 end, "no script mapping yet")

-- UUID 身份:读=十进制字符串,写=只读错误。
T02ExpectError(function() T02ErrorUuid.ID = "1" end, "read-only")

-- 非字符串键。
T02ExpectError(function() return T02ErrorProxy[1] end, "string field name")
T02ExpectError(function() T02ErrorProxy[1] = 2 end, "string field name")

-- 组件被移除 / 实体被销毁之后继续用旧代理。
T02ExpectError(function() return T02StaleComponent.Location end, "was removed from the entity")
T02ExpectError(function() T02StaleComponent.Location = vec3.new(1, 1, 1) end, "was removed from the entity")
T02ExpectError(function() return T02StaleEntity.Location end, "invalid or its scene has expired")
)LUA", "proxy_errors");

		// 场景析构之后访问旧代理(载荷只持有弱生命周期句柄)。
		{
			Scene inner(TestContext());
			Entity innerEntity = Entity::CreateEntity(&inner, "stale scene");
			innerEntity.AddComponent<TransformComponent>();
			SetProxyGlobal("T02StaleScene", innerEntity, "World::TransformComponent");
		}
		RUN_OK(R"LUA(
T02ExpectError(function() return T02StaleScene.Location end, "invalid or its scene has expired")
T02ExpectError(function() T02StaleScene.Location = vec3.new(1, 1, 1) end, "invalid or its scene has expired")
)LUA", "proxy_stale_scene");

		ScriptEngine::GetState().ClearGlobal("T02StaleScene");
	}
}

int main()
{
	try
	{
		World::Log::Init();
		World::ScriptEngine::Init();

		const std::pair<const char*, void(*)()> tests[] = {
			{ "Kind to Lua mapping table is centralized", MappingTableIsSchemaDriven },
			{ "component proxy reads schema fields", ProxyReadsSchemaFields },
			{ "component proxy writes round-trip into the component", ProxyWritesRoundTrip },
			{ "proxy writes inside a script update are visible next frame", ProxyWriteFromScriptUpdateIsVisibleNextFrame },
			{ "proxy access fails safely", ProxyAccessFailsSafely },
		};
		int failures = 0;
		for (const auto& [name, test] : tests)
		{
			try { test(); std::printf("[PASS] %s\n", name); }
			catch (const std::exception& error) { ++failures; std::fprintf(stderr, "[FAIL] %s: %s\n", name, error.what()); }
			catch (...) { ++failures; std::fprintf(stderr, "[FAIL] %s: unknown exception\n", name); }
		}
		World::ScriptEngine::Shutdown();
		if (failures == 0)
		{
			std::printf("World.LuauSchemaBinding: all checks passed\n");
			return 0;
		}
		std::fprintf(stderr, "World.LuauSchemaBinding: %d group(s) failed\n", failures);
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
