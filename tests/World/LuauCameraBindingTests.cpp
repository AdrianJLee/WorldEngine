// P2 W3e:相机字段的嵌套对象代理(CameraComponent.Camera → World::SceneCamera)。
//
// 覆盖:
//   1. 嵌套 Object 字段返回 ComponentProxy(tostring/typeof/字段读取与组件根代理同一套 schema 驱动);
//   2. SceneCamera 的 ProjectionType/Orthographic*/Perspective* 读写;枚举名与数值两种写法;
//   3. 写后 C++ 侧读回一致,并且投影矩阵按 PropertiesPanel 同一约定重算(SceneCamera::ApplyEdit);
//   4. 嵌套 Transient 字段(AspectRatio)可读、写被拒;嵌套 Object 字段自身只读;
//   5. 未知嵌套字段/非 schema Object 仍是可读错误(不静默 nil);
//   6. 实体销毁、组件移除、场景析构之后旧嵌套代理安全失败。
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
#include "World/Scene/SceneCamera.h"
#include "World/Scene/ScriptEngine.h"

#include <cstdio>
#include <glm/gtc/matrix_transform.hpp>
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

	void SetGlobal(const char* name, const std::string& value)
	{
		CHECK(ScriptEngine::GetState().SetGlobal(name, ScriptValue::String(value)));
	}

	void SetEntityGlobal(const char* name, Entity entity)
	{
		ScriptBindingContext& bindings = ScriptEngine::GetBindingContext();
		const ScriptValue value = bindings.NewUserdata("Entity");
		Entity* target = nullptr;
		CHECK(bindings.Unwrap<Entity>("Entity", value, &target) && target != nullptr);
		new (target) Entity(entity);
		CHECK(ScriptEngine::GetState().SetGlobal(name, value));
	}

	double ReadGlobalNumber(const char* name)
	{
		double number = 0.0;
		CHECK(ScriptEngine::GetState().GetGlobal(name).AsNumber(&number));
		return number;
	}

	void SetProxyGlobal(const char* name, const Entity& entity, const char* typeName)
	{
		const Schema::TypeSchema* schema = TestContext().Schemas().Find(typeName);
		CHECK(schema != nullptr);
		CHECK(ScriptEngine::GetState().SetGlobal(name,
			MakeComponentProxy(ScriptEngine::GetBindingContext(), entity, *schema)));
	}

	ScriptValue RecordNoop(const ScriptValue*, std::size_t) { return ScriptValue::Nil(); }

	std::string s_LastReport;

	ScriptValue CaptureReport(const ScriptValue* args, std::size_t count)
	{
		std::string report;
		if (count < 1 || !args[0].AsString(&report))
			throw std::runtime_error("TEST_ReportCameraProbe expects a string");
		s_LastReport = report;
		return ScriptValue::Nil();
	}

	bool NearlyEqual(const glm::mat4& left, const glm::mat4& right, float tolerance)
	{
		for (int column = 0; column < 4; ++column)
			for (int row = 0; row < 4; ++row)
				if (std::abs(left[column][row] - right[column][row]) > tolerance)
					return false;
		return true;
	}

	constexpr const char* kCameraProbePath = "scripts/tests/CameraProbe.lua";
	constexpr float kFloatTolerance = 1.0e-4f;

	// 1.SceneCamera 的嵌套 Object 字段在运行时是一个 schema 驱动的字段代理:
	//   读/写/枚举/失败路径都在脚本内断言(夹具 CameraProbe.lua)。
	void NestedSceneCameraProxyIsReadWrite()
	{
		Scene scene(TestContext());
		Entity entity = Entity::CreateEntity(&scene, "camera probe");
		entity.AddComponent<TransformComponent>();
		CameraComponent& camera = entity.AddComponent<CameraComponent>();
		LuauScriptComponent& script = entity.AddComponent<LuauScriptComponent>(kCameraProbePath);
		std::string declarationError;
		CHECK(ScriptEngine::SyncScriptDeclarations(script, nullptr, &declarationError));

		// OnCreate 阶段:读默认投影字段 + 枚举名/数值两种写法(相机模式让 OnUpdate 保持安静)。
		SetGlobal("TEST_LuaPhase", "create");
		SetGlobal("TEST_CameraProbeMode", "none");
		s_LastReport.clear();
		{
			ScriptBindingContext& bindings = ScriptEngine::GetBindingContext();
			CHECK(ScriptEngine::GetState().SetGlobal("TEST_ReportCameraProbe",
				bindings.CreateFunction("TEST_ReportCameraProbe", &CaptureReport)));
		}
		scene.OnScriptStart();
		CHECK(script.Runtime.State == ScriptInstanceState::Running);
		CHECK(script.Runtime.LastError.empty());
		CHECK(s_LastReport == "create-ok");
		CHECK(camera.Camera.GetProjectionType() == SceneCamera::ProjectionType::Orthographic);

		// OnUpdate 阶段:写投影/正交/FOV/裁剪面。
		SetGlobal("TEST_CameraProbeMode", "write");
		scene.OnScriptUpdate(Timestep(1.0f / 60.0f));
		CHECK(script.Runtime.LastError.empty());
		CHECK(camera.Camera.GetProjectionType() == SceneCamera::ProjectionType::Perspective);
		CHECK(std::abs(camera.Camera.GetPerspectiveFOV() - 60.0f) < kFloatTolerance);
		CHECK(std::abs(camera.Camera.GetPerspectiveNearClip() - 0.25f) < kFloatTolerance);
		CHECK(std::abs(camera.Camera.GetPerspectiveFarClip() - 250.0f) < kFloatTolerance);
		CHECK(std::abs(camera.Camera.GetOrthographicZoom() - 2.5f) < kFloatTolerance);
		CHECK(std::abs(camera.Camera.GetOrthographicNearClip() + 2.0f) < kFloatTolerance);
		CHECK(std::abs(camera.Camera.GetOrthographicFarClip() - 2.0f) < kFloatTolerance);
		// 派生投影矩阵必须重算(与 PropertiesPanel 对 SceneCamera 调 ApplyEdit 同一约定)。
		const glm::mat4 expected = glm::perspective(glm::radians(60.0f), camera.Camera.GetAspectRatio(), 0.25f, 250.0f);
		CHECK(NearlyEqual(camera.Camera.GetProjectionMatrix(), expected, 1.0e-5f));

		// Transient 嵌套字段:可读不可写。
		SetGlobal("TEST_CameraProbeMode", "write_transient");
		scene.OnScriptUpdate(Timestep(1.0f / 60.0f));
		CHECK(script.Runtime.LastError.empty());

		// 嵌套 Object 字段自身只读;未知嵌套字段报可读错误。
		SetGlobal("TEST_CameraProbeMode", "write_object");
		scene.OnScriptUpdate(Timestep(1.0f / 60.0f));
		CHECK(script.Runtime.LastError.empty());
		SetGlobal("TEST_CameraProbeMode", "bad_field");
		scene.OnScriptUpdate(Timestep(1.0f / 60.0f));
		CHECK(script.Runtime.LastError.empty());

		scene.OnRuntimeStop();
		CHECK(script.Runtime.State == ScriptInstanceState::Stopped);

		CHECK(ScriptEngine::GetState().ClearGlobal("TEST_ReportCameraProbe"));
		CHECK(ScriptEngine::GetState().ClearGlobal("TEST_LuaPhase"));
		CHECK(ScriptEngine::GetState().ClearGlobal("TEST_CameraProbeMode"));
	}

	// 2.嵌套代理同样支持 AddComponent 白名单后的当帧使用(W3d/W3f 契约):
	//   在脚本回调内 AddComponent("CameraComponent") 当帧就能拿到嵌套代理并读写。
	void NestedProxyWorksForSynchronouslyAddedCamera()
	{
		Scene scene(TestContext());
		Entity entity = Entity::CreateEntity(&scene, "runtime camera");
		entity.AddComponent<TransformComponent>();
		LuauScriptComponent& script = entity.AddComponent<LuauScriptComponent>(kCameraProbePath);
		std::string declarationError;
		CHECK(ScriptEngine::SyncScriptDeclarations(script, nullptr, &declarationError));
		SetGlobal("TEST_CameraProbeMode", "none");

		scene.OnScriptStart();
		CHECK(!entity.HasComponent<CameraComponent>());

		// 注入式回调(与 EntitySpawnProbe.lua 的 T02Action 同一模式):在真实 OnUpdate 回调里
		// 执行"回调内同步加组件 + 立即用嵌套代理读写"。
		std::string compileError;
		const ScriptFunctionRef fixtureUpdate = script.OnUpdateFunc;
		const ScriptFunctionRef action = ScriptEngine::GetState().CompileFunction(R"LUA(
local self, dt = ...
local entity = self.entity
assert(entity:GetComponent("CameraComponent") == nil)
entity:AddComponent("CameraComponent")
assert(entity:HasComponent("CameraComponent"))
local camera = entity:GetComponent("CameraComponent").Camera
assert(typeof(camera) == "ComponentProxy")
assert(camera.m_ProjectionType == 1)
camera.m_OrthographicZoom = 3.0
assert(camera.m_OrthographicZoom == 3.0)
local perspective = camera.m_ProjectionType
camera.m_ProjectionType = "Perspective"
assert(camera.m_ProjectionType == 0)
camera.m_ProjectionType = perspective
assert(camera.m_ProjectionType == 1)
)LUA", "camera_added_synchronously", ScriptTableRef(), &compileError);
		CHECK(action.IsValid() && compileError.empty());
		CHECK(fixtureUpdate.IsValid());
		script.OnUpdateFunc = action;
		scene.OnScriptUpdate(Timestep(1.0f / 60.0f));
		CHECK(script.Runtime.LastError.empty());
		CHECK(entity.HasComponent<CameraComponent>());
		CHECK(std::abs(entity.GetComponent<CameraComponent>().Camera.GetOrthographicZoom() - 3.0f) < kFloatTolerance);
		CHECK(entity.GetComponent<CameraComponent>().Camera.GetProjectionType() == SceneCamera::ProjectionType::Orthographic);

		scene.OnRuntimeStop();
		CHECK(ScriptEngine::GetState().ClearGlobal("TEST_CameraProbeMode"));
	}

	// 3.失败路径:非 schema Object 保持可读错误;实体销毁/组件移除/场景析构后旧代理安全失败。
	void NestedProxyFailsSafely()
	{
		Scene scene(TestContext());
		Entity entity = Entity::CreateEntity(&scene, "camera failure");
		entity.AddComponent<TransformComponent>();
		entity.AddComponent<CameraComponent>();
		SetEntityGlobal("T05FailureEntity", entity);
		SetProxyGlobal("T05FailureCamera", entity, "World::CameraComponent");

		RUN_OK(R"LUA(
function T05ExpectError(fn, needle)
    local ok, err = pcall(fn)
    assert(not ok, "expected an error containing: " .. needle)
    assert(type(err) == "string", "error text must be a string, got " .. type(err))
    assert(string.find(err, needle, 1, true), "expected '" .. needle .. "' in: " .. err)
end

T05RemovedCamera = T05FailureCamera.Camera
assert(typeof(T05RemovedCamera) == "ComponentProxy")
assert(tostring(T05RemovedCamera) == "ComponentProxy(CameraComponent.Camera)")
-- 未知字段 / 错误类型都必须是可读错误,不是 nil 或崩溃。
T05ExpectError(function() return T05RemovedCamera.NoSuchField end, "no field 'NoSuchField'")
T05ExpectError(function() T05RemovedCamera.m_OrthographicZoom = "wide" end, "expects a finite number")
T05ExpectError(function() T05RemovedCamera.m_ProjectionType = "NotAProjection" end, "has no enum name")
T05ExpectError(function() T05RemovedCamera.m_ProjectionType = 9 end, "has no enum value 9")
T05ExpectError(function() T05RemovedCamera.m_ProjectionType = 1.5 end, "expects an integer")
)LUA", "nested_proxy_errors");

		// 组件移除:根代理与已经取出的嵌套代理都要失败(不缓存实例指针)。
		entity.RemoveComponent<CameraComponent>();
		CHECK(!entity.HasComponent<CameraComponent>());
RUN_OK(R"LUA(
T05ExpectError(function() return T05FailureCamera.Camera end, "was removed from the entity")
T05ExpectError(function() return T05RemovedCamera.m_OrthographicZoom end, "was removed from the entity")
)LUA", "nested_proxy_removed_component");

		// 实体销毁之后同样安全失败。
		Entity doomed = Entity::CreateEntity(&scene, "doomed camera");
		doomed.AddComponent<TransformComponent>();
		doomed.AddComponent<CameraComponent>();
		SetProxyGlobal("T05DoomedCamera", doomed, "World::CameraComponent");
		Entity::DestroyEntity(&scene, doomed);
		CHECK(!doomed.IsValid());
		RUN_OK(R"LUA(
T05ExpectError(function() return T05DoomedCamera.Camera end, "invalid or its scene has expired")
)LUA", "nested_proxy_destroyed_entity");

		// 场景析构之后(载荷只持有弱生命周期句柄)。
		{
			Scene inner(TestContext());
			Entity innerEntity = Entity::CreateEntity(&inner, "stale scene camera");
			innerEntity.AddComponent<TransformComponent>();
			innerEntity.AddComponent<CameraComponent>();
			SetProxyGlobal("T05StaleSceneCamera", innerEntity, "World::CameraComponent");
		}
		RUN_OK(R"LUA(
T05ExpectError(function() return T05StaleSceneCamera.Camera end, "invalid or its scene has expired")
)LUA", "nested_proxy_stale_scene");

		CHECK(ScriptEngine::GetState().ClearGlobal("T05FailureEntity"));
		CHECK(ScriptEngine::GetState().ClearGlobal("T05FailureCamera"));
		CHECK(ScriptEngine::GetState().ClearGlobal("T05DoomedCamera"));
		CHECK(ScriptEngine::GetState().ClearGlobal("T05StaleSceneCamera"));
	}

	// 4.非 schema 的 Object:字段有 Object Kind 但嵌套类型未注册/没有字段 → 仍然读写都报错。
	void UnregisteredObjectStillReports()
	{
		Scene scene(TestContext());
		Entity entity = Entity::CreateEntity(&scene, "unregistered object probe");

		Schema::TypeSchema unknown;
		unknown.Id = Schema::TypeId("Test::UnknownNested");
		Schema::FieldSchema field;
		field.Id = Schema::FieldId { 1 };
		field.Name = "Unknown";
		field.K = Schema::Kind::Object;
		field.GetNested = []() -> const Schema::TypeSchema* { return nullptr; };
		unknown.Fields.push_back(field);

		const ScriptFieldMapping mapping = DescribeScriptField(unknown.Fields[0]);
		CHECK(mapping.LuaTypeName == nullptr);
		CHECK(!mapping.UuidIdentity);
		// 既不是 UUID、也拿不到嵌套类型:保持"未映射"(读写都报可读错误,不静默 nil)。
		CHECK(!mapping.NestedObject);
		(void)entity;
	}
}

int main()
{
	try
	{
		World::Log::Init();
		World::ScriptEngine::Init();

		const std::pair<const char*, void(*)()> tests[] = {
			{ "nested SceneCamera proxy is read/write", NestedSceneCameraProxyIsReadWrite },
			{ "nested proxy works for synchronously added camera", NestedProxyWorksForSynchronouslyAddedCamera },
			{ "nested proxy fails safely", NestedProxyFailsSafely },
			{ "unregistered Object fields still report", UnregisteredObjectStillReports },
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
			std::printf("World.LuauCameraBinding: all checks passed\n");
			return 0;
		}
		std::fprintf(stderr, "World.LuauCameraBinding: %d group(s) failed\n", failures);
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
