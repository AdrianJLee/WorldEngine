// P2 W3b:游戏服务面的脚本绑定(Input / Level / Save)。
//
// 覆盖:按键轴驱动脚本写 TransformComponent.Location(逐帧单调);
// Pressed/Released 跨帧边沿(证明 GameHost 帧末 EndFrame 生效);未知动作/轴与越界玩家安全返回;
// 切关卡(Request → IsLoading → Pump → Primary)与未知 id 的 LastError;
// 存读档(SetGlobal/Save/Load/List 往返、坏档 Load 失败且 LastError 非空)。
#include "wldpch.h"
#include "World/Core/Log.h"
#include "World/Core/WorldContext.h"
#include "World/Gameplay/GameApp.h"
#include "World/Gameplay/GameHost.h"
#include "World/Gameplay/InputMap.h"
#include "World/Gameplay/LevelList.h"
#include "World/Gameplay/SaveService.h"
#include "World/Scene/Components.h"
#include "World/Scene/Entity.h"
#include "World/Scene/Scene.h"
#include "World/Scene/ScriptEngine.h"
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

	WorldContext& TestContext()
	{
		static WorldContext context;
		return context;
	}

	void RunOk(const std::string& source, const char* chunk, int line)
	{
		std::string error;
		if (!ScriptEngine::GetState().RunString(source, chunk, &error))
			throw std::runtime_error(std::string("line ") + std::to_string(line) + ": " + chunk +
				" failed: " + error);
	}
#define RUN_OK(source, chunk) RunOk(source, chunk, __LINE__)

	bool TryRun(const std::string& source, const char* chunk)
	{
		std::string error;
		return ScriptEngine::GetState().RunString(source, chunk, &error);
	}

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

	double NumberArg(const ScriptValue& value, const char* what)
	{
		double number = 0.0;
		if (!value.AsNumber(&number))
			throw std::runtime_error(std::string(what) + " must be a number");
		return number;
	}

	bool BoolArg(const ScriptValue& value, const char* what)
	{
		bool flag = false;
		if (!value.AsBool(&flag))
			throw std::runtime_error(std::string(what) + " must be a boolean");
		return flag;
	}

	struct InputState
	{
		bool Pressed = false;
		bool Down = false;
		bool Released = false;
		bool UnknownDown = true;
		double UnknownAxis = -1.0;
		bool OutOfRangeDown = true;
		double OutOfRangeAxis = -1.0;
	};

	World::Ref<Scene> CreateProbeScene(Entity* outEntity)
	{
		World::Ref<Scene> scene = World::CreateRef<Scene>(TestContext());
		Entity entity = Entity::CreateEntity(scene.get(), "services probe host");
		// CreateEntity 已经带 TagComponent + UUIDComponent,这里只补探针需要的组件。
		entity.AddComponent<TransformComponent>();
		entity.AddComponent<LuauScriptComponent>("scripts/tests/ServicesProbe.lua");
		if (outEntity)
			*outEntity = entity;
		return scene;
	}

	struct Cleanup
	{
		fs::path Path;
		~Cleanup()
		{
			std::error_code ignored;
			fs::remove_all(Path, ignored);
		}
	};

	World::Ref<Scene> CreateLevelScene(const std::string& path, std::string* error)
	{
		(void)path;
		(void)error;
		return CreateProbeScene(nullptr);
	}

	// 1.Input:轴 → 脚本写 Transform(Axis 驱动移动);未知动作/轴与越界玩家安全返回;
	// Pressed/Released 的真实边沿(依赖 GameHost 帧末 EndFrame)。
	void InputServiceDrivesScriptAndEdges()
	{
		const fs::path tempRoot = fs::temp_directory_path() /
			("WorldLuauServiceBindingTests-" + std::to_string(static_cast<unsigned long long>(GetCurrentProcessId())));
		std::error_code ignored;
		fs::remove_all(tempRoot, ignored);
		fs::create_directories(tempRoot);
		Cleanup cleanup { tempRoot };
		_putenv_s("WLD_SAVE_DIR", tempRoot.string().c_str());

		Entity probeEntity;
		World::Ref<Scene> initialScene = CreateProbeScene(&probeEntity);
		Entity movingEntity = probeEntity;
		const Timestep step(0.05f);

		Gameplay::GameHost host;
		Gameplay::GameAppDesc desc;
		desc.ProjectId = "worldengine.luau.service.tests";
		desc.ContentRoot = WLD_ASSETPATH;
		host.Init(desc);
		host.SetScene(initialScene, true);
		CHECK(initialScene->IsRunning());
		CHECK(movingEntity.GetComponent<LuauScriptComponent>().Runtime.State == ScriptInstanceState::Running);

		// 输入映射:Move = 手柄 LX 轴(宿主不喂手柄,状态由测试注入);Jump = 鼠标键 0(宿主会喂,
		// 测试在喂入之后覆写以制造真实边沿)。
		Gameplay::InputMap inputMap;
		Gameplay::InputAction jump;
		jump.Name = "Jump";
		jump.Bindings = { { Gameplay::InputDevice::Mouse, 0 } };
		Gameplay::InputAxis move;
		move.Name = "Move";
		move.GamepadAxis = "LX";
		move.DeadZone = 0.1f;
		inputMap.Actions() = { jump };
		inputMap.Axes() = { move };
		Gameplay::InputService& input = Gameplay::GameApp::Get().Input();
		input.SetMap(inputMap);
		input.SetPlayerCount(2);
		CHECK(input.GetPlayerCount() == 2);

		// 探针把每帧的轴值/位置与 Pressed/Down/Released 回调给测试宿主(与 T02Record 同一套注入路径;
		// 脚本自己的 environment 与 SetGlobal 的线程全局是两份表,读取应走回调而不是回读全局)。
		std::vector<double> moves;
		std::vector<InputState> states;
		{
			ScriptBindingContext& bindings = ScriptEngine::GetBindingContext();
			CHECK(ScriptEngine::GetState().SetGlobal("PROBE_MOVE",
				bindings.CreateFunction("PROBE_MOVE", [&moves](const ScriptValue* args, std::size_t count) -> ScriptValue
				{
					if (count < 2)
						throw std::runtime_error("PROBE_MOVE expects (axis, z)");
					(void)NumberArg(args[0], "PROBE_MOVE axis");
					moves.push_back(NumberArg(args[1], "PROBE_MOVE z"));
					return ScriptValue::Nil();
				})));
			CHECK(ScriptEngine::GetState().SetGlobal("PROBE_STATE",
				bindings.CreateFunction("PROBE_STATE", [&states](const ScriptValue* args, std::size_t count) -> ScriptValue
				{
					if (count < 7)
						throw std::runtime_error("PROBE_STATE expects (pressed, down, released, unknownDown, unknownAxis, badPlayerDown, badPlayerAxis)");
					InputState state;
					state.Pressed = BoolArg(args[0], "PROBE_STATE pressed");
					state.Down = BoolArg(args[1], "PROBE_STATE down");
					state.Released = BoolArg(args[2], "PROBE_STATE released");
					state.UnknownDown = BoolArg(args[3], "PROBE_STATE unknown action");
					state.UnknownAxis = NumberArg(args[4], "PROBE_STATE unknown axis");
					state.OutOfRangeDown = BoolArg(args[5], "PROBE_STATE out-of-range action");
					state.OutOfRangeAxis = NumberArg(args[6], "PROBE_STATE out-of-range axis");
					states.push_back(state);
					return ScriptValue::Nil();
				})));
		}

		// 未知动作/轴与越界玩家:与 C++ 一致返回 false/0;探针里的 UNKNOWN_* 每帧同样断言。
		RUN_OK(R"LUA(
INPUT_PLAYER_COUNT = Input.PlayerCount()
)LUA", "services_input_probe");
		CHECK(ReadNumber("INPUT_PLAYER_COUNT", -1.0) == 2.0);

		// 轴驱动:脚本每帧把 axis*dt 写到 TransformComponent.Location.Z,记录单调增长。
		input.SetGamepadAxis(0, "LX", 1.0f);
		CHECK(std::fabs(input.Axis("Move", 0) - 1.0f) < 1e-5f);
		for (int frame = 0; frame < 3; ++frame)
		{
			host.Tick(step, false);
			CHECK(moves.size() == static_cast<std::size_t>(frame + 1));
			CHECK(states.size() == static_cast<std::size_t>(frame + 1));
			CHECK(!states.back().UnknownDown && states.back().UnknownAxis == 0.0);
			CHECK(!states.back().OutOfRangeDown && states.back().OutOfRangeAxis == 0.0);
		}
		CHECK(moves.size() == 3);
		double previous = -1.0;
		for (const double z : moves)
		{
			CHECK(z > previous);
			previous = z;
		}
		CHECK(movingEntity.HasComponent<TransformComponent>());
		CHECK(movingEntity.GetComponent<TransformComponent>().Location.z > 0.14f);
		CHECK(std::fabs(movingEntity.GetComponent<TransformComponent>().Location.z - previous) < 1e-5f);

		// 轴回零:不再记录新移动,位置保持。
		input.SetGamepadAxis(0, "LX", 0.0f);
		host.Tick(step, false);
		host.Tick(step, false);
		CHECK(moves.size() == 3);

		// Pressed/Released:按住跨两帧后 Pressed 必须变 false,松开帧 Released 为 true。
		states.clear();
		const uint64_t endFramesBefore = input.GetEndFrameCount();
		// Jump 绑到鼠标键:测试在宿主喂入后覆写成"按下"以制造真实边沿。
		input.SetKeyState(0, Gameplay::InputDevice::Mouse, 0, true);
		host.Tick(step, false);
		CHECK(states.size() == 1);
		CHECK(states[0].Down && states[0].Pressed && !states[0].Released);
		CHECK(input.GetEndFrameCount() == endFramesBefore + 1);
		host.Tick(step, false);
		CHECK(states.size() == 2);
		CHECK(states[1].Down && !states[1].Pressed && !states[1].Released);
		input.SetKeyState(0, Gameplay::InputDevice::Mouse, 0, false);
		host.Tick(step, false);
		CHECK(states.size() == 3);
		CHECK(!states[2].Down && !states[2].Pressed && states[2].Released);

		ScriptEngine::GetState().ClearGlobal("PROBE_MOVE");
		ScriptEngine::GetState().ClearGlobal("PROBE_STATE");
		host.Shutdown();
		_putenv_s("WLD_SAVE_DIR", "");
	}

	// 2.Level:Request → IsLoading → 宿主 Pump → Primary 切换;未知 id 返回 false 且 LastError 非空。
	void LevelServiceRequestsAndSwitches()
	{
		Entity probeEntity;
		World::Ref<Scene> initialScene = CreateProbeScene(&probeEntity);

		Gameplay::GameHost host;
		Gameplay::GameAppDesc desc;
		desc.ProjectId = "worldengine.luau.level.tests";
		desc.ContentRoot = WLD_ASSETPATH;
		host.Init(desc);
		host.SetScene(initialScene, true);

		Gameplay::LevelList levels;
		levels.Entries().push_back({ "alpha", "Alpha", "scenes/test-alpha.wd", {} });
		levels.Entries().push_back({ "beta", "Beta", "scenes/test-beta.wd", {} });
		Gameplay::LevelService& service = Gameplay::GameApp::Get().Levels();
		service.SetLevelList(levels);
		service.SetSceneLoader(&CreateLevelScene);

		RUN_OK(R"LUA(
assert(Level.LastError() == "")
LEVEL_BAD_OK = Level.Request("ghost")
LEVEL_BAD_ERROR = Level.LastError()
assert(Level.Request("beta") == true)
assert(Level.IsLoading() == true)
LEVEL_LOADING_STATE = Level.State()
LEVEL_ENTRY_COUNT = 0
local entries = Level.Entries()
for _ in pairs(entries) do LEVEL_ENTRY_COUNT = LEVEL_ENTRY_COUNT + 1 end
)LUA", "services_level_request");
		CHECK(!ReadBool("LEVEL_BAD_OK", true));
		CHECK(!ReadString("LEVEL_BAD_ERROR").empty());
		CHECK(ReadString("LEVEL_LOADING_STATE") == "Reading");
		CHECK(ReadNumber("LEVEL_ENTRY_COUNT", -1.0) == 2.0);
		CHECK(Gameplay::GameApp::Get().Levels().IsLoading());

		host.Tick(Timestep(1.0f / 60.0f), false);
		CHECK(!Gameplay::GameApp::Get().Levels().IsLoading());
		CHECK(host.GetScene() != nullptr && host.GetScene() != initialScene);
		CHECK(!initialScene->IsRunning());
		CHECK(host.IsRuntimeStarted());

		RUN_OK(R"LUA(
assert(Level.IsLoading() == false)
assert(Level.Primary() == "beta")
assert(Level.State() == "Idle")
assert(Level.LastError() == "")
local active = Level.ActiveLevels()
assert(active[1] == "beta" and active[2] == nil)
)LUA", "services_level_loaded");

		RUN_OK(R"LUA(
assert(Level.Request("alpha") == true)
assert(Level.Primary() == "beta")
assert(Level.IsLoading() == true)
)LUA", "services_level_second_request");
		host.Tick(Timestep(1.0f / 60.0f), false);
		CHECK(Gameplay::GameApp::Get().Levels().GetPrimaryLevel() == "alpha");
		RUN_OK(R"LUA(
assert(Level.Primary() == "alpha")
Level.UnloadAll()
assert(Level.Primary() == "")
assert(Level.IsLoading() == false)
)LUA", "services_level_unload");

		host.Shutdown();
	}

	// 3.Save:SetGlobal + Save → 改 Transform → Load 还原 + 全局值往返;List 至少一条 valid;坏档报错。
	void SaveServiceRoundTrips()
	{
		const std::string projectId = "worldengine.luau.save.tests";
		const fs::path tempRoot = fs::temp_directory_path() /
			("WorldLuauServiceBindingSave-" + std::to_string(static_cast<unsigned long long>(GetCurrentProcessId())));
		std::error_code ignored;
		fs::remove_all(tempRoot, ignored);
		fs::create_directories(tempRoot);
		Cleanup cleanup { tempRoot };
		_putenv_s("WLD_SAVE_DIR", tempRoot.string().c_str());

		Gameplay::GameHost host;
		Gameplay::GameAppDesc desc;
		desc.ProjectId = projectId;
		desc.ContentRoot = WLD_ASSETPATH;
		host.Init(desc);

		Entity probeEntity;
		World::Ref<Scene> scene = CreateProbeScene(&probeEntity);
		host.SetScene(scene, true);
		CHECK(Gameplay::GameApp::Get().Saves() != nullptr);
		const std::string uuidText = std::to_string(static_cast<uint64_t>(probeEntity.GetComponent<UUIDComponent>().ID));

		RUN_OK(R"LUA(
assert(Save.LastError() == "")
local slots = Save.List()
assert(#slots == 0)
assert(Save.Load(0) == false)
assert(Save.LastError() ~= "")
)LUA", "services_save_empty");

		RUN_OK(R"LUA(
assert(Save.SetGlobal("progress", 42) == true)
assert(Save.SetGlobal("flag", true) == true)
assert(Save.SetGlobal("name", "chapter-one") == true)
assert(Save.Save(0) == true)
assert(Save.LastError() == "")
local slots = Save.List()
assert(#slots >= 1)
local found = false
for _, slot in ipairs(slots) do
    if slot.slot == 0 then
        found = true
        assert(slot.valid == true)
        assert(slot.error == "")
        assert(type(slot.level_id) == "string")
        assert(type(slot.version) == "number")
        assert(type(slot.timestamp) == "number")
    end
end
assert(found == true)
)LUA", "services_save_write");

		const fs::path slotPath = Gameplay::GameApp::Get().Saves()->GetSlotPath(0);
		CHECK(fs::exists(slotPath));
		std::string savedText;
		{
			std::ifstream stream(slotPath, std::ios::binary);
			CHECK(static_cast<bool>(stream));
			savedText.assign(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
		}
		CHECK(savedText.find(projectId) != std::string::npos);
		CHECK(savedText.find(uuidText) != std::string::npos);

		// 篡改场景与全局块,再读档:两者都要回到存档时的值。
		{
			TransformComponent& transform = probeEntity.GetComponent<TransformComponent>();
			transform.SetLocation(glm::vec3(9.0f, 9.0f, 9.0f));
			Gameplay::SaveService::GlobalValue value;
			value.Type = Gameplay::SaveService::GlobalValue::Kind::Int;
			value.Int = 999;
			Gameplay::GameApp::Get().Saves()->SetGlobal("progress", value);
		}
		RUN_OK(R"LUA(
assert(Save.Load(0) == true)
assert(Save.LastError() == "")
assert(Save.GetGlobal("progress") == 42)
assert(Save.GetGlobal("flag") == true)
assert(Save.GetGlobal("name") == "chapter-one")
assert(Save.GetGlobal("missing") == nil)
)LUA", "services_save_load");
		const glm::vec3 restored = probeEntity.GetComponent<TransformComponent>().Location;
		CHECK(std::fabs(restored.z - 0.0f) < 1e-4f);

		// 坏档:Load 返回 false 且 LastError 非空,不抛异常、不覆盖原档。
		{
			std::ofstream stream(slotPath, std::ios::binary | std::ios::trunc);
			stream << "SaveHeader: [not a valid save]\n\t- broken";
		}
		RUN_OK(R"LUA(
SAVE_BAD_OK = Save.Load(0)
SAVE_BAD_ERROR = Save.LastError()
)LUA", "services_save_corrupt");
		CHECK(!ReadBool("SAVE_BAD_OK", true));
		CHECK(!ReadString("SAVE_BAD_ERROR").empty());
		CHECK(fs::exists(slotPath));

		host.Shutdown();
		_putenv_s("WLD_SAVE_DIR", "");
	}
}

int main()
{
	try
	{
		std::setvbuf(stdout, nullptr, _IONBF, 0);
		World::Log::Init();
		World::ScriptEngine::Init();

		const std::pair<const char*, void(*)()> tests[] = {
			{ "Input axis drives script movement and pressed/released edges", InputServiceDrivesScriptAndEdges },
			{ "Level requests switch the primary level", LevelServiceRequestsAndSwitches },
			{ "Save round-trips entities and globals", SaveServiceRoundTrips },
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
			std::printf("World.LuauServiceBinding: all checks passed\n");
			return 0;
		}
		std::fprintf(stderr, "World.LuauServiceBinding: %d group(s) failed\n", failures);
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
