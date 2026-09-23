// P2 W4:事件 / 计时器的脚本绑定(events / timers)。
//
// 覆盖(对齐 plan §11 W4 接口冻结草案的验收):
//   1. 订阅顺序 = events:on 调用顺序;off 后不再收到;未知事件名给可读 Lua error;
//      计时器只能从脚本实例回调里创建(owner 语义);
//   2. 盒装往返 number/boolean/string/Entity(C++ emit 与 Lua emit 两条路径)+ emit 帧末投递;
//   3. handler 抛错只把该实例置 Faulted,其余订阅者继续,且该实例的其它订阅被丢弃;
//   4. 实例销毁 = 自动退订(旧闭包不再被调用);
//   5. 计时器回调里创建实体/加组件当帧可见(带 owner 的 Scene 回调作用域);
//       after/every(限次)/cancel 与固定步长计数;
//   6. 热重载成功 = 旧 generation 订阅整体退订(失败保留旧订阅);新版本 OnCreate
//      在 Pending 复活路径上重建订阅(与编辑器 W5b 的 Reload 复活同一条路)。
//
// headless:真实 Scene 调度(OnRuntimeStart/OnScriptUpdate)+ GameApp::Tick,脚本夹具
// `scripts/tests/EventProbe.lua`;热重载脚本写在构建产物的临时目录里,逻辑路径是
// 相对 WLD_ASSETPATH 的带 ".." 路径(与 ScriptHotReloadTests 同一模式)。
#include "wldpch.h"
#include "World/Core/Log.h"
#include "World/Core/Timestep.h"
#include "World/Core/WorldContext.h"
#include "World/Gameplay/EventBus.h"
#include "World/Gameplay/GameApp.h"
#include "World/Gameplay/GameHost.h"
#include "World/Scene/Components.h"
#include "World/Scene/Entity.h"
#include "World/Scene/Scene.h"
#include "World/Scene/ScriptEngine.h"
#include "World/Script/BindEvents.h"
#include "World/Script/LuauVm.h"
#include "World/Script/ScriptBindingContext.h"
#include "World/Script/ScriptRef.h"
#include "World/Script/ScriptValue.h"

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace
{
	using namespace World;
	namespace fs = std::filesystem;

	void Check(bool condition, const char* expression, int line)
	{
		if (!condition)
			throw std::runtime_error(std::string("line ") + std::to_string(line) + ": " + expression);
	}
#define CHECK(expression) Check(static_cast<bool>(expression), #expression, __LINE__)

	constexpr float kStep = 1.0f / 60.0f;
	const char* const kProbePath = "scripts/tests/EventProbe.lua";

	WorldContext& TestContext()
	{
		static WorldContext context;
		return context;
	}

	fs::path g_RunDirectory;

	fs::path ScriptPath(const char* name)
	{
		return g_RunDirectory / name;
	}

	std::string LogicalPath(const fs::path& path)
	{
		return path.lexically_relative(fs::path(WLD_ASSETPATH)).generic_string();
	}

	void WriteScript(const fs::path& path, const std::string& text)
	{
		fs::create_directories(path.parent_path());
		std::ofstream file(path, std::ios::binary | std::ios::trunc);
		file << text;
		file.flush();
		if (!file.good())
			throw std::runtime_error("cannot write " + path.generic_string());
	}

	// ---- 宿主探针:PROBE_EVENT(kind, ...) 把每次上报记成一条调用 ----

	struct ProbeCall
	{
		std::vector<std::string> Args;
	};

	std::vector<ProbeCall>& Calls()
	{
		static std::vector<ProbeCall> calls;
		return calls;
	}

	std::string FormatArg(const ScriptValue& value)
	{
		double number = 0.0;
		if (value.AsNumber(&number))
		{
			char buffer[64] = {};
			if (std::isfinite(number) && number == std::floor(number) && std::fabs(number) < 1e15)
				std::snprintf(buffer, sizeof(buffer), "%.0f", number);
			else
				std::snprintf(buffer, sizeof(buffer), "%g", number);
			return buffer;
		}
		bool boolean = false;
		if (value.AsBool(&boolean))
			return boolean ? "true" : "false";
		std::string text;
		if (value.AsString(&text))
			return text;
		return std::string("<") + value.TypeName() + ">";
	}

	ScriptValue ProbeEventImpl(const ScriptValue* args, std::size_t count)
	{
		ProbeCall call;
		call.Args.reserve(count);
		for (std::size_t index = 0; index < count; ++index)
			call.Args.push_back(FormatArg(args[index]));
		Calls().push_back(std::move(call));
		return ScriptValue::Nil();
	}

	std::vector<ProbeCall> CallsOfKind(const std::string& kind)
	{
		std::vector<ProbeCall> matches;
		for (const ProbeCall& call : Calls())
			if (!call.Args.empty() && call.Args[0] == kind)
				matches.push_back(call);
		return matches;
	}

	std::size_t CountKind(const std::string& kind)
	{
		return CallsOfKind(kind).size();
	}

	std::vector<std::string> TagsOfKind(const std::string& kind)
	{
		std::vector<std::string> tags;
		for (const ProbeCall& call : CallsOfKind(kind))
			if (call.Args.size() > 1)
				tags.push_back(call.Args[1]);
		return tags;
	}

	// 返回该 tag 最近一次上报的完整参数(含 kind 与 tag)。
	std::vector<std::string> LatestCall(const std::string& kind, const std::string& tag)
	{
		std::vector<std::string> found;
		for (const ProbeCall& call : Calls())
			if (call.Args.size() > 1 && call.Args[0] == kind && call.Args[1] == tag)
				found = call.Args;
		return found;
	}

	// ---- VM 辅助 ----

	void RunOk(const std::string& source, const char* chunk, int line)
	{
		std::string error;
		if (!ScriptEngine::GetState().RunString(source, chunk, &error))
			throw std::runtime_error(std::string("line ") + std::to_string(line) + ": " + chunk +
				" failed: " + error);
	}
#define RUN_OK(source, chunk) RunOk(source, chunk, __LINE__)

	double ReadNumber(const char* name, double fallback)
	{
		double value = fallback;
		if (!ScriptEngine::GetState().GetGlobal(name).AsNumber(&value))
			return fallback;
		return value;
	}

	bool ReadBool(const char* name, bool fallback)
	{
		bool value = fallback;
		if (!ScriptEngine::GetState().GetGlobal(name).AsBool(&value))
			return fallback;
		return value;
	}

	std::string ReadString(const char* name)
	{
		std::string value;
		ScriptEngine::GetState().GetGlobal(name).AsString(&value);
		return value;
	}

	void SetProbeMode(const char* mode)
	{
		ScriptEngine::GetState().SetGlobal("PROBE_MODE", ScriptValue::String(mode));
	}

	// ---- 场景辅助 ----

	World::Ref<Scene> MakeProbeScene(const std::vector<std::string>& names)
	{
		World::Ref<Scene> scene = World::CreateRef<Scene>(TestContext());
		for (const std::string& name : names)
		{
			Entity entity = Entity::CreateEntity(scene.get(), name);
			entity.AddComponent<LuaScriptComponent>(kProbePath);
		}
		return scene;
	}

	void StartHost(Gameplay::GameHost& host, const World::Ref<Scene>& scene)
	{
		Gameplay::GameAppDesc desc;
		desc.ProjectId = "worldengine.luau.event.tests";
		desc.ContentRoot = WLD_ASSETPATH;
		desc.FixedStepHz = 60;
		host.Init(desc);
		host.SetScene(scene, /*startRuntime=*/true);
	}

	Entity FindByName(Scene& scene, const std::string& name)
	{
		const entt::registry& registry = static_cast<const Scene&>(scene).GetRegistry();
		for (const entt::entity handle : registry.view<TagComponent>())
		{
			if (registry.get<TagComponent>(handle).Tag == name)
				return Entity(&scene, handle);
		}
		return {};
	}

	// ---- 1. 订阅顺序 / off / 未注册名 / owner 语义 ----

	void SubscriptionOrderOffAndUnknownNames()
	{
		Calls().clear();
		Gameplay::GameHost host;
		World::Ref<Scene> scene = MakeProbeScene({ "probe A", "probe B" });
		SetProbeMode("default");
		StartHost(host, scene);
		const std::vector<std::string> created = TagsOfKind("created");
		CHECK(created.size() == 2);
		const std::vector<std::string> handlesA = LatestCall("handles", created[0]);
		CHECK(handlesA.size() == 10);

		// 队列式 emit:入队不触发,固定点(Tick 帧末)统一派发。
		Calls().clear();
		RUN_OK("assert(events.emit('probe:order', 5) == true)", "events_emit_order");
		CHECK(Gameplay::GameApp::Get().Events().GetPendingCount() == 1);
		CHECK(CountKind("order") == 0);
		host.Tick(Timestep(kStep), false);
		const std::vector<std::string> order = TagsOfKind("order");
		CHECK(order == created);                       // 派发顺序 = events:on 调用顺序
		for (const ProbeCall& call : CallsOfKind("order"))
			CHECK(call.Args.size() == 3 && call.Args[2] == "5");

		// off:退订第一个实例的 order 订阅,只剩第二个实例收到。
		RUN_OK("EVENT_OFF_OK = events.off(" + handlesA[2] + ")", "events_off");
		CHECK(ReadBool("EVENT_OFF_OK", false));
		Calls().clear();
		RUN_OK("events.emit('probe:order', 6)", "events_emit_after_off");
		host.Tick(Timestep(kStep), false);
		const std::vector<std::string> remaining = TagsOfKind("order");
		CHECK(remaining.size() == 1 && remaining[0] == created[1]);

		// 未知 handle / 未知事件名:false 与可读错误(不静默)。
		RUN_OK("EVENT_OFF_UNKNOWN = events.off(999999)", "events_off_unknown");
		CHECK(!ReadBool("EVENT_OFF_UNKNOWN", true));
		RUN_OK(R"LUA(
local onOk, onError = pcall(function() events:on("probe:ghost", function() end) end)
UNKNOWN_ON_OK = onOk
UNKNOWN_ON_ERROR = onError
local emitOk, emitError = pcall(function() events:emit("probe:ghost") end)
UNKNOWN_EMIT_OK = emitOk
UNKNOWN_EMIT_ERROR = emitError
local timerOk, timerError = pcall(function() timers:after(0.1, function() end) end)
TIMER_OWNER_OK = timerOk
TIMER_OWNER_ERROR = timerError
)LUA", "events_unknown_names");
		CHECK(!ReadBool("UNKNOWN_ON_OK", true));
		CHECK(ReadString("UNKNOWN_ON_ERROR").find("probe:ghost") != std::string::npos);
		CHECK(!ReadBool("UNKNOWN_EMIT_OK", true));
		CHECK(ReadString("UNKNOWN_EMIT_ERROR").find("probe:ghost") != std::string::npos);
		// timers:* 的 owner 语义:回调外调用给可读错误。
		CHECK(!ReadBool("TIMER_OWNER_OK", true));
		CHECK(ReadString("TIMER_OWNER_ERROR").find("script instance callback") != std::string::npos);
		host.Shutdown();
	}

	// ---- 2. 盒装往返(number/boolean/string/Entity)与帧末投递 ----

	void PayloadRoundTripAndFrameEndDelivery()
	{
		Calls().clear();
		Gameplay::GameHost host;
		World::Ref<Scene> scene = MakeProbeScene({ "probe A" });
		SetProbeMode("default");
		StartHost(host, scene);
		Calls().clear();

		// Lua emit:OnUpdate 里 emit,handler 在帧末看到 OnUpdate 之后设置的标记(true)。
		RUN_OK("EMIT_PAYLOAD = true", "events_emit_payload_flag");
		host.Tick(Timestep(kStep), false);
		std::vector<ProbeCall> payload = CallsOfKind("payload");
		CHECK(payload.size() == 1);
		CHECK(payload[0].Args.size() == 7);
		CHECK(payload[0].Args[1] == "probe A");
		CHECK(payload[0].Args[2] == "7");
		CHECK(payload[0].Args[3] == "true");
		CHECK(payload[0].Args[4] == "hello");
		CHECK(payload[0].Args[5] == "probe A");    // Entity 参数按原始句柄往返,重包后可 GetName
		CHECK(payload[0].Args[6] == "true");       // 队列式:handler 在 emit 之后(帧末)才被调用

		// C++ emit:同一份序列化载荷(number/boolean/string/Entity)。
		Calls().clear();
		Entity probe = FindByName(*scene, "probe A");
		CHECK(probe.IsValid());
		const std::vector<ScriptEventValue> values = {
			ScriptEventValue::MakeNumber(3),
			ScriptEventValue::MakeBoolean(false),
			ScriptEventValue::MakeString("from C++"),
			ScriptEventValue::MakeEntity(static_cast<std::uint32_t>(probe)),
		};
		std::vector<std::uint8_t> bytes;
		std::string error;
		CHECK(SerializeScriptEventPayload(values, &bytes, &error));
		CHECK(DeserializeScriptEventPayload(bytes, nullptr, &error) == false);   // 空输出指针 → 可读失败
		Gameplay::GameApp::Get().Events().EmitDeferredRaw(
			ScriptEventTypeId("probe:payload"), bytes.data(), bytes.size(), 0);
		host.Tick(Timestep(kStep), false);
		payload = CallsOfKind("payload");
		CHECK(payload.size() == 1);
		CHECK(payload[0].Args[2] == "3");
		CHECK(payload[0].Args[3] == "false");
		CHECK(payload[0].Args[4] == "from C++");
		CHECK(payload[0].Args[5] == "probe A");
		ScriptEngine::GetState().ClearGlobal("EMIT_PAYLOAD");
		host.Shutdown();
	}

	// ---- 3. handler 抛错只 Faulted 该实例 ----

	void FaultedHandlerStopsOnlyItsOwnInstance()
	{
		Calls().clear();
		Gameplay::GameHost host;
		World::Ref<Scene> scene = MakeProbeScene({ "probe A", "probe B" });
		SetProbeMode("default");
		// 只让 probe A 的 handler 抛错:probe B 必须仍被调用并保持 Running。
		ScriptEngine::GetState().SetGlobal("THROWING_TAG", ScriptValue::String("probe A"));
		StartHost(host, scene);
		const std::vector<std::string> created = TagsOfKind("created");
		CHECK(created.size() == 2);
		Calls().clear();

		RUN_OK("events.emit('probe:throw')", "events_emit_throw");
		host.Tick(Timestep(kStep), false);
		// 两个订阅者都收到(抛错实例之后的订阅者不丢),只有抛错的实例被置 Faulted。
		const std::vector<std::string> throwing = TagsOfKind("throwing");
		CHECK(throwing.size() == 2);
		CHECK(throwing == created);                    // 派发顺序 = 订阅顺序
		Entity first = FindByName(*scene, "probe A");
		Entity second = FindByName(*scene, "probe B");
		CHECK(first.IsValid() && second.IsValid());
		LuaScriptComponent& firstScript = first.GetComponent<LuaScriptComponent>();
		CHECK(firstScript.State == ScriptInstanceState::Faulted);
		CHECK(firstScript.LastError.find("probe handler exploded") != std::string::npos);
		CHECK(second.GetComponent<LuaScriptComponent>().State == ScriptInstanceState::Running);

		// Faulted 实例的其它订阅(以及它的计时器)已整体丢弃:后续事件只送达存活实例。
		Calls().clear();
		RUN_OK("events.emit('probe:order', 11)", "events_emit_after_fault");
		host.Tick(Timestep(kStep), false);
		const std::vector<std::string> order = TagsOfKind("order");
		CHECK(order.size() == 1 && order[0] == "probe B");
		ScriptEngine::GetState().ClearGlobal("THROWING_TAG");
		host.Shutdown();
	}

	// ---- 4. 实例销毁 = 自动退订 ----

	void DestroyedInstanceStopsReceivingEvents()
	{
		Calls().clear();
		Gameplay::GameHost host;
		World::Ref<Scene> scene = MakeProbeScene({ "probe A", "probe B" });
		SetProbeMode("default");
		StartHost(host, scene);
		const std::vector<std::string> created = TagsOfKind("created");
		CHECK(created.size() == 2);
		Calls().clear();
		RUN_OK("events.emit('probe:order', 1)", "events_emit_before_destroy");
		host.Tick(Timestep(kStep), false);
		CHECK(TagsOfKind("order").size() == 2);

		// 活动场景里的销毁走延迟请求,在下一个结构提交点执行 OnDestroy + 批量退订。
		Entity doomed = FindByName(*scene, created[0]);
		CHECK(doomed.IsValid());
		Entity::DestroyEntity(scene.get(), doomed);
		host.Tick(Timestep(kStep), false);
		CHECK(!FindByName(*scene, created[0]).IsValid());
		CHECK(CountKind("destroyed") == 1);

		Calls().clear();
		RUN_OK("events.emit('probe:order', 2)", "events_emit_after_destroy");
		host.Tick(Timestep(kStep), false);
		const std::vector<std::string> order = TagsOfKind("order");
		CHECK(order.size() == 1 && order[0] == created[1]);
		CHECK(CountKind("destroyed") == 0);
		host.Shutdown();
	}

	// ---- 5. 计时器:回调里生成实体当帧可见 + after/every/cancel ----

	void TimerCallbackSpawnsEntityInSameFrame()
	{
		Calls().clear();
		Gameplay::GameHost host;
		World::Ref<Scene> scene = MakeProbeScene({ "probe A" });
		SetProbeMode("timer-spawn");
		StartHost(host, scene);
		Calls().clear();

		host.Tick(Timestep(kStep), false);          // 第 1 步:计时器(2/60s)还没到
		CHECK(CountKind("timer-spawn") == 0);
		CHECK(!FindByName(*scene, "timer child").IsValid());

		host.Tick(Timestep(kStep), false);          // 第 2 步:计时器触发
		std::vector<ProbeCall> spawn = CallsOfKind("timer-spawn");
		CHECK(spawn.size() == 1);
		CHECK(spawn[0].Args[1] == "probe A");
		CHECK(spawn[0].Args[2] == "true");          // child:IsValid()
		CHECK(spawn[0].Args[3] == "true");          // FindByName 在回调里立即可见(不是延迟提交)
		// 同一帧:计时器回调里同步创建的实体已经进场景(当帧可见)。
		Entity child = FindByName(*scene, "timer child");
		CHECK(child.IsValid());
		CHECK(child.HasComponent<TransformComponent>());   // 回调里 AddComponent 成功

		// after(0.5s) 恰好一次,every(0.25s, count=2) 恰好两次(固定步长计数)。
		for (int frame = 0; frame < 60; ++frame)
			host.Tick(Timestep(kStep), false);
		CHECK(CountKind("after") == 1);
		CHECK(CountKind("every") == 2);
		host.Shutdown();
	}

	void TimerCancelStopsCallback()
	{
		Calls().clear();
		Gameplay::GameHost host;
		World::Ref<Scene> scene = MakeProbeScene({ "probe A" });
		SetProbeMode("timer-cancel");
		StartHost(host, scene);
		const std::vector<std::string> handles = LatestCall("handles", "probe A");
		CHECK(handles.size() == 10);
		CHECK(handles[8] != "0");                    // handleCancel

		RUN_OK("TIMER_CANCEL_OK = timers.cancel(" + handles[8] + ")", "timers_cancel");
		CHECK(ReadBool("TIMER_CANCEL_OK", false));
		Calls().clear();
		for (int frame = 0; frame < 10; ++frame)     // 0.166s:0.1s 的周期计时器早已到点
			host.Tick(Timestep(kStep), false);
		CHECK(CountKind("cancel-target") == 0);
		CHECK(CountKind("every") == 0);              // 0.166s < 0.25s
		for (int frame = 0; frame < 20; ++frame)     // 总计 0.5s
			host.Tick(Timestep(kStep), false);
		CHECK(CountKind("every") == 2);              // every(0.25s, count=2):恰好两次
		CHECK(CountKind("after") == 1);              // after(0.5s):恰好一次

		RUN_OK("TIMER_CANCEL_UNKNOWN = timers.cancel(999999)", "timers_cancel_unknown");
		CHECK(!ReadBool("TIMER_CANCEL_UNKNOWN", true));
		host.Shutdown();
	}

	// ---- 6. 热重载:旧订阅退订,新 OnCreate 重建订阅 ----

	const char* const kReloadV1 = R"LUA(
---@class EventReloadProbe : WorldScript
local Probe = {}
function Probe:OnCreate()
    self.handle = events:on("probe:order", function(value)
        if PROBE_EVENT then PROBE_EVENT("order", "v1", value) end
    end)
end
function Probe:OnUpdate(dt)
end
return Probe
)LUA";

	const char* const kReloadV2 = R"LUA(
---@class EventReloadProbe : WorldScript
local Probe = {}
function Probe:OnCreate()
    self.handle = events:on("probe:order", function(value)
        if PROBE_EVENT then PROBE_EVENT("order", "v2", value) end
    end)
end
function Probe:OnUpdate(dt)
end
return Probe
)LUA";

	const char* const kReloadBroken = R"LUA(
---@class EventReloadProbe : WorldScript
local Probe = {}
function Probe:OnCreate(
return Probe
)LUA";

	void HotReloadDropsOldSubscriptionsAndRebuildsThem()
	{
		const fs::path scriptPath = ScriptPath("EventReloadProbe.lua");
		WriteScript(scriptPath, kReloadV1);
		Calls().clear();

		Gameplay::GameHost host;
		World::Ref<Scene> scene = World::CreateRef<Scene>(TestContext());
		Entity entity = Entity::CreateEntity(scene.get(), "reload probe");
		entity.AddComponent<LuaScriptComponent>(LogicalPath(scriptPath));
		SetProbeMode("default");
		StartHost(host, scene);
		CHECK(entity.GetComponent<LuaScriptComponent>().State == ScriptInstanceState::Running);

		Calls().clear();
		RUN_OK("events.emit('probe:order', 1)", "reload_v1_emit");
		host.Tick(Timestep(kStep), false);
		std::vector<std::string> order = TagsOfKind("order");
		CHECK(order.size() == 1 && order[0] == "v1");

		// 热重载失败:旧订阅与旧闭包原样保留(State 仍 Running,失败不置 Faulted)。
		WriteScript(scriptPath, kReloadBroken);
		std::string diagnostics;
		LuaScriptComponent& script = entity.GetComponent<LuaScriptComponent>();
		CHECK(!ScriptEngine::ReloadScript(script, &diagnostics));
		CHECK(!diagnostics.empty());
		CHECK(script.State == ScriptInstanceState::Running);
		Calls().clear();
		RUN_OK("events.emit('probe:order', 2)", "reload_failed_emit");
		host.Tick(Timestep(kStep), false);
		order = TagsOfKind("order");
		CHECK(order.size() == 1 && order[0] == "v1");

		// 热重载成功:旧 generation 的订阅整体退订,旧闭包不再被调用。
		WriteScript(scriptPath, kReloadV2);
		CHECK(ScriptEngine::ReloadScript(entity.GetComponent<LuaScriptComponent>(), &diagnostics));
		Calls().clear();
		RUN_OK("events.emit('probe:order', 3)", "reload_success_emit");
		host.Tick(Timestep(kStep), false);
		CHECK(CountKind("order") == 0);

		// Pending 复活路径(编辑器 W5b Reload 对 Faulted/未加载实例走的就是它):
		// 新版本的 OnCreate 重新建立订阅,之后事件送达新闭包。
		LuaScriptComponent& revived = entity.GetComponent<LuaScriptComponent>();
		revived.State = ScriptInstanceState::Pending;
		scene->OnScriptUpdate(Timestep(kStep));
		CHECK(revived.State == ScriptInstanceState::Running);
		Calls().clear();
		RUN_OK("events.emit('probe:order', 4)", "reload_revived_emit");
		host.Tick(Timestep(kStep), false);
		order = TagsOfKind("order");
		CHECK(order.size() == 1 && order[0] == "v2");
		host.Shutdown();
	}
}

int main()
{
	try
	{
		std::setvbuf(stdout, nullptr, _IONBF, 0);
		World::Log::Init();
		World::ScriptEngine::Init();
		// 宿主探针必须在任何脚本编译之前注入(脚本 environment 的 __index 回退到线程全局)。
		CHECK(World::ScriptEngine::GetState().SetGlobal("PROBE_EVENT",
			World::ScriptEngine::GetBindingContext().CreateFunction("PROBE_EVENT", &ProbeEventImpl)));

		// 事件名目录:启动期注册 + 重复/非法签名拒绝 + id 域(FNV-1a("script-event:" + name))。
		std::string error;
		CHECK(World::RegisterScriptEventName("probe:order", "number", &error));
		CHECK(World::RegisterScriptEventName("probe:payload", "number,boolean,string,Entity", &error));
		CHECK(World::RegisterScriptEventName("probe:throw", "", &error));
		CHECK(World::RegisterScriptEventName("probe:spawn", "", &error));
		CHECK(!World::RegisterScriptEventName("probe:order", "number", &error));
		CHECK(!World::RegisterScriptEventName("probe:bad", "wibble", &error));
		CHECK(!World::RegisterScriptEventName("", "", &error));
		CHECK(World::GetScriptEventNameCount() == 4);
		CHECK(World::IsScriptEventNameRegistered("probe:order"));
		CHECK(!World::IsScriptEventNameRegistered("probe:ghost"));
		CHECK(World::ScriptEventTypeId("probe:ghost") == 0);
		std::uint32_t expected = 2166136261u;
		for (const unsigned char character : std::string("script-event:probe:order"))
			expected = (expected ^ character) * 16777619u;
		CHECK(World::ScriptEventTypeId("probe:order") == expected);

		g_RunDirectory = fs::path(WORLD_EVENT_TEST_OUTPUT_DIR) /
			("run-" + std::to_string(::GetCurrentProcessId()));
		fs::create_directories(g_RunDirectory);

		const std::pair<const char*, void (*)()> tests[] = {
			{ "subscription order, off, unknown names and timer ownership", SubscriptionOrderOffAndUnknownNames },
			{ "payload round-trip and frame-end delivery", PayloadRoundTripAndFrameEndDelivery },
			{ "a faulted handler only stops its own instance", FaultedHandlerStopsOnlyItsOwnInstance },
			{ "a destroyed instance stops receiving events", DestroyedInstanceStopsReceivingEvents },
			{ "a timer callback spawns an entity visible in the same frame", TimerCallbackSpawnsEntityInSameFrame },
			{ "timers.cancel stops the callback", TimerCancelStopsCallback },
			{ "hot reload drops old subscriptions and OnCreate rebuilds them", HotReloadDropsOldSubscriptionsAndRebuildsThem },
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
			std::printf("World.LuauEventBinding: all checks passed\n");
			std::error_code cleanupError;
			fs::remove_all(g_RunDirectory, cleanupError);
			return 0;
		}
		std::fprintf(stderr, "World.LuauEventBinding: %d group(s) failed\n", failures);
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
