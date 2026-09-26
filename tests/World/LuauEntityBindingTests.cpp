// P2 W3d:Entity 生成/查询/prefab 绑定回归(headless,真实 Scene::OnScriptUpdate 调度)。
//
// 覆盖冻结点:
//  - 回调内 CreateChild/AddComponent(纯数据白名单)同步生效,当帧句柄有效、字段可读写;
//  - OnScriptUpdate 结束后、渲染前实体已在 registry;
//  - 同一帧其它脚本按快照语义看不到新实体(FindByName);
//  - SetParent/ClearParent 走 Hierarchy 正门(环/自身 -> false),Parent 字段直写被拒;
//  - .wprefab 同步实例化(根句柄/UUID/实体数)与相对路径内容根解析;
//  - 失败路径:无效句柄、未知组件、不存在 prefab 都给可读错误或 nil。
#include "wldpch.h"
#include "World/Core/Log.h"
#include "World/Core/WorldContext.h"
#include "World/Gameplay/Prefab.h"
#include "World/Scene/Components.h"
#include "World/Scene/Entity.h"
#include "World/Scene/Hierarchy.h"
#include "World/Scene/LuaStubGenerator.h"
#include "World/Scene/Scene.h"
#include "World/Scene/ScriptEngine.h"
#include "World/Script/LuauVm.h"
#include "World/Script/ScriptBindingContext.h"
#include "World/Script/ScriptProperties.h"
#include "World/Script/ScriptRef.h"
#include "World/Script/ScriptValue.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <new>
#include <stdexcept>
#include <string>
#include <utility>

namespace
{
	using namespace World;
	namespace fs = std::filesystem;

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

	template<typename F>
	bool RejectsLogic(F&& action)
	{
		try { action(); }
		catch (const std::logic_error&) { return true; }
		return false;
	}

	ScriptValue MakeEntityValue(Entity entity)
	{
		ScriptBindingContext& bindings = ScriptEngine::GetBindingContext();
		ScriptValue value = bindings.NewUserdata("Entity");
		Entity* target = nullptr;
		CHECK(bindings.Unwrap<Entity>("Entity", value, &target) && target != nullptr);
		new (target) Entity(entity);
		return value;
	}

	Entity ReadEntityValue(const ScriptValue& value)
	{
		Entity* entity = nullptr;
		ScriptEngine::GetBindingContext().Unwrap<Entity>("Entity", value, &entity);
		if (!entity)
			throw std::runtime_error("expected an Entity userdata");
		return *entity;
	}

	void SetEntityGlobal(const char* name, Entity entity)
	{
		CHECK(ScriptEngine::GetState().SetGlobal(name, MakeEntityValue(entity)));
	}

	void RunLua(const std::string& source)
	{
		std::string error;
		if (!ScriptEngine::GetState().RunString(source, "W3dEntityTest", &error))
			throw std::runtime_error(error);
	}

	Entity s_CapturedEntity;

	ScriptValue CaptureEntity(const ScriptValue* args, std::size_t count)
	{
		if (count < 1)
			throw std::runtime_error("TEST_CaptureEntity expects an Entity");
		s_CapturedEntity = ReadEntityValue(args[0]);
		return ScriptValue::Nil();
	}

	Entity FindTagged(Scene& scene, const std::string& name)
	{
		const entt::registry& registry = static_cast<const Scene&>(scene).GetRegistry();
		for (const entt::entity handle : registry.view<TagComponent>())
			if (registry.get<TagComponent>(handle).Tag == name)
				return Entity(&scene, handle);
		return {};
	}

	std::size_t CountEntities(Scene& scene)
	{
		const entt::registry& registry = static_cast<const Scene&>(scene).GetRegistry();
		std::size_t count = 0;
		for (const entt::entity handle : registry.view<UUIDComponent>())
		{
			(void)handle;
			++count;
		}
		return count;
	}

	struct Fixture
	{
		Ref<Scene> World = CreateRef<Scene>(TestContext());

		~Fixture()
		{
			World.reset();
		}

		Entity AddLua(const std::string& path = "scripts/tests/EntitySpawnProbe.lua")
		{
			Entity entity = Entity::CreateEntity(World.get(), "Lua probe");
			entity.AddComponent<LuauScriptComponent>(path);
			return entity;
		}

		void Step()
		{
			World->OnScriptUpdate(Timestep(1.0f / 60.0f));
		}

		void Stop()
		{
			World->OnRuntimeStop();
		}
	};

	void SetLuaString(Entity entity, const std::string& name, const std::string& value)
	{
		std::vector<ScriptProperty>& properties = entity.GetComponent<LuauScriptComponent>().Properties;
		if (ScriptProperty* property = ScriptProperties::Find(properties, name))
		{
			property->Type = Schema::Kind::String;
			property->Value = value;
			return;
		}
		properties.push_back(ScriptProperty{ name, Schema::Kind::String, Schema::Value(value) });
	}

	// 回调内 CreateChild + 白名单 AddComponent 当帧生效;第二个脚本同帧按快照看不到它。
	void SameFrameCreationAndSnapshotVisibility()
	{
		Fixture fixture;
		Entity spawner = fixture.AddLua("scripts/tests/EntitySpawnProbe.lua");
		Entity observer = fixture.AddLua("scripts/tests/EntitySpawnProbe.lua");
		SetLuaString(spawner, "Mode", "spawn");
		SetLuaString(observer, "Mode", "query");
		fixture.World->OnScriptStart();
		fixture.Step();

		CHECK(spawner.GetComponent<LuauScriptComponent>().Runtime.State == ScriptInstanceState::Running);
		CHECK(spawner.GetComponent<LuauScriptComponent>().Runtime.LastError.empty());
		CHECK(observer.GetComponent<LuauScriptComponent>().Runtime.State == ScriptInstanceState::Running);
		CHECK(observer.GetComponent<LuauScriptComponent>().Runtime.LastError.empty());

		// OnScriptUpdate 结束后的宿主视角:实体、Transform、Sprite 与父层级都已提交。
		Entity child = FindTagged(*fixture.World, "A");
		CHECK(child && child.IsValid());
		CHECK(child.HasComponent<TransformComponent>());
		CHECK(child.HasComponent<SpriteComponent>());
		const auto& transform = child.GetComponent<TransformComponent>();
		CHECK(transform.Location.x == 1.0f && transform.Location.y == 2.0f && transform.Location.z == 3.0f);
		CHECK(child.GetComponent<HierarchyComponent>().Parent == static_cast<entt::entity>(spawner));
		fixture.Stop();
	}

	// SetParent/ClearParent/GetParent/GetChildren/GetName/SetName/FindByName + Parent 禁写。
	void HierarchyAndParentWriteBan()
	{
		Fixture fixture;
		Entity parent = Entity::CreateEntity(fixture.World.get(), "Parent");
		Entity first = Entity::CreateEntity(fixture.World.get(), "First");
		Entity second = Entity::CreateEntity(fixture.World.get(), "Second");
		Entity third = Entity::CreateEntity(fixture.World.get(), "Third");
		SetEntityGlobal("W3dP", parent);
		SetEntityGlobal("W3dC1", first);
		SetEntityGlobal("W3dC2", second);
		SetEntityGlobal("W3dC3", third);

		RunLua(R"lua(
assert(W3dC1:SetParent(W3dP) == true)
assert(W3dC2:SetParent(W3dP) == true)
assert(W3dC3:SetParent(W3dP) == true)
local kids = W3dP:GetChildren()
assert(#kids == 3)
assert(kids[1]:GetID() == W3dC1:GetID() and kids[2]:GetID() == W3dC2:GetID() and kids[3]:GetID() == W3dC3:GetID())
assert(W3dC1:GetParent():GetID() == W3dP:GetID())
assert(W3dP:SetParent(W3dC3) == false)   -- would create a cycle
assert(W3dP:SetParent(W3dP) == false)    -- self parent
assert(W3dC2:ClearParent() == true)
assert(W3dC2:GetParent() == nil)
local after = W3dP:GetChildren()
assert(#after == 2 and after[1]:GetID() == W3dC1:GetID() and after[2]:GetID() == W3dC3:GetID())
assert(W3dP:FindByName("Parent"):GetID() == W3dP:GetID())
W3dP:SetName("Renamed")
assert(W3dP:GetName() == "Renamed")
local orphan = W3dC3:CreateChild()
assert(orphan:IsValid() and orphan:GetName() == "Empty Entity")
assert(orphan:GetParent():GetID() == W3dC3:GetID())

local hierarchy = W3dP:GetComponent("HierarchyComponent")
assert(hierarchy ~= nil)
local ok, err = pcall(function() hierarchy.Parent = 7 end)
assert(not ok and err ~= nil)
assert(tostring(err):find("SetParent") ~= nil and tostring(err):find("ClearParent") ~= nil)
local bad, badErr = pcall(function() return W3dP:AddComponent("NoSuchComponent") end)
assert(not bad and badErr ~= nil)
)lua");

		CHECK(parent.GetComponent<TagComponent>().Tag == "Renamed");
		CHECK(parent.GetComponent<HierarchyComponent>().Children.size() == 2);
		CHECK(first.GetComponent<HierarchyComponent>().Parent == static_cast<entt::entity>(parent));
		CHECK(second.GetComponent<HierarchyComponent>().Parent == entt::null);

		ScriptEngine::GetState().ClearGlobal("W3dP");
		ScriptEngine::GetState().ClearGlobal("W3dC1");
		ScriptEngine::GetState().ClearGlobal("W3dC2");
		ScriptEngine::GetState().ClearGlobal("W3dC3");
	}

	// 无效句柄/未知组件/不存在 prefab 都必须是可读错误或 nil。
	void InvalidHandlesAndFailurePaths()
	{
		Fixture fixture;
		Entity doomed = Entity::CreateEntity(fixture.World.get(), "Doomed");
		Entity target = Entity::CreateEntity(fixture.World.get(), "Target");
		SetEntityGlobal("W3dDoomed", doomed);
		SetEntityGlobal("W3dTarget", target);
		Entity::DestroyEntity(fixture.World.get(), doomed);
		fixture.World->FlushStructuralChanges();
		CHECK(!doomed);

		RunLua(R"lua(
assert(W3dDoomed:IsValid() == false)
local ok, err = pcall(function() return W3dDoomed:GetName() end)
assert(not ok and err ~= nil)
local ok2, err2 = pcall(function() return W3dTarget:InstantiatePrefab("does-not-exist.wprefab") end)
assert(not ok2 and err2 ~= nil)
assert(W3dTarget:SetParent(W3dDoomed) == false)
)lua");

		ScriptEngine::GetState().ClearGlobal("W3dDoomed");
		ScriptEngine::GetState().ClearGlobal("W3dTarget");
	}

	// .wprefab 同步实例化:根句柄当帧有效、UUID 重发、实体数与层级与源一致、相对路径按内容根解析。
	void PrefabInstantiation()
	{
		const fs::path output = fs::path(WORLD_ENTITY_TEST_OUTPUT_DIR) / "WorldLuauEntityBinding";
		std::error_code ec;
		fs::create_directories(output, ec);
		const fs::path prefabPath = output / "ProbeRoot.wprefab";

		Ref<Scene> source = CreateRef<Scene>(TestContext());
		Entity root = Entity::CreateEntity(source.get(), "PrefabRoot");
		root.AddComponent<TransformComponent>();
		root.AddComponent<MeshRendererComponent>();
		Entity child = Entity::CreateEntity(source.get(), "PrefabChild");
		child.AddComponent<TransformComponent>();
		child.AddComponent<SpriteComponent>();
		CHECK(Hierarchy::SetParent(source->GetRegistry(), static_cast<entt::entity>(child), static_cast<entt::entity>(root)));
		const UUID sourceUuid = root.GetComponent<UUIDComponent>().ID;
		const std::size_t sourceCount = CountEntities(*source);
		std::string error;
		CHECK(Gameplay::SaveFromScene(*source, root, prefabPath, &error));
		CHECK(error.empty());

		Fixture fixture;
		Entity instantiator = fixture.AddLua("scripts/tests/EntitySpawnProbe.lua");
		SetLuaString(instantiator, "Mode", "prefab");
		SetLuaString(instantiator, "PrefabPath", prefabPath.generic_string());
		s_CapturedEntity = Entity {};
		CHECK(ScriptEngine::GetState().SetGlobal("TEST_CaptureEntity",
			ScriptEngine::GetBindingContext().CreateFunction("TEST_CaptureEntity", &CaptureEntity)));
		fixture.World->OnScriptStart();
		fixture.Step();

		CHECK(instantiator.GetComponent<LuauScriptComponent>().Runtime.State == ScriptInstanceState::Running);
		CHECK(instantiator.GetComponent<LuauScriptComponent>().Runtime.LastError.empty());
		CHECK(s_CapturedEntity.IsValid());
		CHECK(static_cast<uint64_t>(s_CapturedEntity.GetComponent<UUIDComponent>().ID) != static_cast<uint64_t>(sourceUuid));
		CHECK(s_CapturedEntity.GetComponent<TagComponent>().Tag == "PrefabRoot");
		CHECK(CountEntities(*fixture.World) == sourceCount + 1); // instantiator + instantiated subtree
		const auto& hierarchy = s_CapturedEntity.GetComponent<HierarchyComponent>();
		CHECK(hierarchy.Children.size() == 1);
		fixture.Stop();
		ScriptEngine::GetState().ClearGlobal("TEST_CaptureEntity");
		s_CapturedEntity = Entity {};

		// 相对路径:先按内容根(WLD_ASSETPATH)解析,而不是相对进程 CWD。
		Fixture relative;
		Entity relativeInstantiator = relative.AddLua("scripts/tests/EntitySpawnProbe.lua");
		SetLuaString(relativeInstantiator, "Mode", "prefab");
		SetLuaString(relativeInstantiator, "PrefabPath", "prefabs/ExampleSprite.wprefab");
		CHECK(ScriptEngine::GetState().SetGlobal("TEST_CaptureEntity",
			ScriptEngine::GetBindingContext().CreateFunction("TEST_CaptureEntity", &CaptureEntity)));
		relative.World->OnScriptStart();
		relative.Step();
		CHECK(relativeInstantiator.GetComponent<LuauScriptComponent>().Runtime.State == ScriptInstanceState::Running);
		CHECK(relativeInstantiator.GetComponent<LuauScriptComponent>().Runtime.LastError.empty());
		CHECK(s_CapturedEntity.IsValid());
		CHECK(s_CapturedEntity.GetComponent<TagComponent>().Tag == "Example Sprite");
		relative.Stop();
		ScriptEngine::GetState().ClearGlobal("TEST_CaptureEntity");
		s_CapturedEntity = Entity {};
	}

	// 存根元数据必须能通过生成器的类型校验,并包含新方法(Editor 侧真实写盘由 W3d 验收步骤覆盖)。
	void StubMetadataIncludesNewMethods()
	{
		const auto& types = LuaReflectionRegistry::GetTable();
		const auto entity = std::find_if(types.begin(), types.end(),
			[](const LuaTypeReflection& type) { return type.ClassName == "Entity"; });
		CHECK(entity != types.end());
		const char* expected[] = { "CreateChild", "SetParent", "ClearParent", "GetParent", "GetChildren",
			"GetName", "SetName", "FindByName", "InstantiatePrefab" };
		for (const char* name : expected)
			CHECK(std::any_of(entity->Methods.begin(), entity->Methods.end(),
				[name](const LuaFunctionDesc& method) { return method.Name == name; }));

		std::string output, error;
		CHECK(LuaStubGenerator::Render(types, output, error));
		CHECK(output.find("function Entity:CreateChild(") != std::string::npos);
		CHECK(output.find("Entity|nil") != std::string::npos);
		CHECK(output.find("Entity[]") != std::string::npos);
	}
}

int main()
{
	try
	{
		World::Log::Init();
		World::ScriptEngine::Init();

		const std::pair<const char*, void(*)()> tests[] = {
			{ "same-frame creation and query snapshot", SameFrameCreationAndSnapshotVisibility },
			{ "hierarchy operations and Parent write ban", HierarchyAndParentWriteBan },
			{ "invalid handles and failure paths", InvalidHandlesAndFailurePaths },
			{ "synchronous prefab instantiation", PrefabInstantiation },
			{ "stub metadata includes new Entity methods", StubMetadataIncludesNewMethods },
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
			std::printf("World.LuauEntityBinding: all checks passed\n");
			return 0;
		}
		std::fprintf(stderr, "World.LuauEntityBinding: %d group(s) failed\n", failures);
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
