#include "wldpch.h"
#include "World/Script/BindServices.h"

#include "World/Gameplay/GameApp.h"
#include "World/Gameplay/InputMap.h"
#include "World/Gameplay/LevelService.h"
#include "World/Gameplay/SaveService.h"
#include "World/Scene/ScriptEngine.h"
#include "World/Script/LuauVm.h"
#include "World/Script/ScriptRef.h"
#include "World/Script/ScriptValue.h"

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

namespace World
{
	namespace
	{
		using Gameplay::GameApp;
		using Gameplay::InputService;
		using Gameplay::LevelEntry;
		using Gameplay::LevelLoadStateName;
		using Gameplay::LevelService;
		using Gameplay::SaveService;
		using Gameplay::SaveSlotInfo;
		using GlobalValue = SaveService::GlobalValue;

		// ---- 前置条件:会话/服务 ----

		GameApp& RequireGameApp()
		{
			GameApp* app = GameApp::TryGet();
			if (!app)
				throw std::logic_error("no GameApp session is active; script service calls require a running game session");
			return *app;
		}

		SaveService& RequireSaveService()
		{
			SaveService* saves = RequireGameApp().Saves();
			if (!saves)
				throw std::logic_error("no save service is available; the host must call GameApp::CreateSaveService first");
			return *saves;
		}

		ScriptTableRef MakeScriptTable()
		{
			return ScriptEngine::GetState().CreateTable();
		}

		// ---- 参数读取(不匹配 → 可读 C++ 异常 → Lua error) ----

		const char* TypeNameOf(const ScriptValue& value)
		{
			return value.TypeName();
		}

		std::string MethodSignature(const std::string& table, const ScriptServiceMethod& method)
		{
			std::string signature = table + "." + method.Name + "(";
			for (std::size_t i = 0; i < method.ParamCount; ++i)
			{
				if (i) signature += ", ";
				signature += method.Params[i].Name;
				if (!method.Params[i].Required) signature += "?";
				signature += ": ";
				signature += method.Params[i].LuaType;
			}
			return signature + ")";
		}

		void CheckArgumentCount(const std::string& table, const ScriptServiceMethod& method,
			std::size_t count, std::size_t minimum)
		{
			if (count < minimum || count > method.ExpectedArgs)
				throw std::logic_error(MethodSignature(table, method) + " expects between " +
					std::to_string(minimum) + " and " + std::to_string(method.ExpectedArgs) +
					" argument(s); got " + std::to_string(count));
		}

		bool MatchesType(ScriptServiceArgType accepted, const ScriptValue& value)
		{
			if ((static_cast<uint32_t>(accepted) & static_cast<uint32_t>(ScriptServiceArgType::Boolean)) &&
				value.IsBoolean())
				return true;
			if ((static_cast<uint32_t>(accepted) & static_cast<uint32_t>(ScriptServiceArgType::Number)) &&
				value.IsNumber())
				return true;
			if ((static_cast<uint32_t>(accepted) & static_cast<uint32_t>(ScriptServiceArgType::String)) &&
				value.IsString())
				return true;
			if ((static_cast<uint32_t>(accepted) & static_cast<uint32_t>(ScriptServiceArgType::Table)) &&
				value.IsTable())
				return true;
			return false;
		}

		const ScriptValue& RequireArg(const std::string& table, const ScriptServiceMethod& method,
			const ScriptValue* args, std::size_t argCount, std::size_t index)
		{
			if (index >= argCount || args[index].IsNil())
				throw std::logic_error(MethodSignature(table, method) + " is missing required argument " +
					std::to_string(index + 1));
			if (!MatchesType(method.Params[index].Accepted, args[index]))
				throw std::logic_error(MethodSignature(table, method) + ": argument " + std::to_string(index + 1) +
					" ('" + method.Params[index].Name + "') must be " + method.Params[index].LuaType +
					", got " + TypeNameOf(args[index]));
			return args[index];
		}

		bool OptionalBool(const std::string& table, const ScriptServiceMethod& method,
			const ScriptValue* args, std::size_t argCount, std::size_t index, bool fallback)
		{
			if (index >= argCount || args[index].IsNil())
				return fallback;
			CheckArgumentCount(table, method, argCount, 0);
			if (!MatchesType(method.Params[index].Accepted, args[index]))
				throw std::logic_error(MethodSignature(table, method) + ": argument " + std::to_string(index + 1) +
					" ('" + method.Params[index].Name + "') must be " + method.Params[index].LuaType +
					", got " + TypeNameOf(args[index]));
			bool result = fallback;
			args[index].AsBool(&result);
			return result;
		}

		std::string OptionalString(const std::string& table, const ScriptServiceMethod& method,
			const ScriptValue* args, std::size_t argCount, std::size_t index, const std::string& fallback)
		{
			if (index >= argCount || args[index].IsNil())
				return fallback;
			CheckArgumentCount(table, method, argCount, 0);
			if (!MatchesType(method.Params[index].Accepted, args[index]))
				throw std::logic_error(MethodSignature(table, method) + ": argument " + std::to_string(index + 1) +
					" ('" + method.Params[index].Name + "') must be " + method.Params[index].LuaType +
					", got " + TypeNameOf(args[index]));
			std::string result = fallback;
			args[index].AsString(&result);
			return result;
		}

		std::string RequireString(const std::string& table, const ScriptServiceMethod& method,
			const ScriptValue* args, std::size_t argCount, std::size_t index)
		{
			const ScriptValue& value = RequireArg(table, method, args, argCount, index);
			std::string result;
			if (!value.AsString(&result))
				throw std::logic_error(MethodSignature(table, method) + ": argument " + std::to_string(index + 1) +
					" ('" + method.Params[index].Name + "') must be " + method.Params[index].LuaType);
			return result;
		}

		uint32_t RequireSlot(const std::string& table, const ScriptServiceMethod& method,
			const ScriptValue* args, std::size_t argCount, std::size_t index)
		{
			const ScriptValue& value = RequireArg(table, method, args, argCount, index);
			double number = 0.0;
			if (!value.AsNumber(&number) || number < 0.0 || number > 4294967295.0 ||
				number != static_cast<double>(static_cast<uint64_t>(number)))
				throw std::logic_error(MethodSignature(table, method) + ": argument " + std::to_string(index + 1) +
					" ('" + method.Params[index].Name + "') must be an integer in [0, 4294967295]");
			return static_cast<uint32_t>(number);
		}

		uint32_t RequirePlayer(const std::string& table, const ScriptServiceMethod& method,
			const ScriptValue* args, std::size_t argCount, std::size_t index)
		{
			if (index >= argCount || args[index].IsNil())
				return 0;
			const ScriptValue& value = args[index];
			double number = 0.0;
			if (!MatchesType(method.Params[index].Accepted, value) || !value.AsNumber(&number) ||
				number < 0.0 || number > 4294967295.0 || number != static_cast<double>(static_cast<uint64_t>(number)))
				throw std::logic_error(MethodSignature(table, method) + ": argument " + std::to_string(index + 1) +
					" ('" + method.Params[index].Name + "') must be a non-negative integer player slot");
			return static_cast<uint32_t>(number);
		}

		// 脚本侧只能写入 uint32 槽位;内部时间戳等 uint64 值按 double 输出(与 Lua number 精度一致)。
		double ToLuaNumber(uint64_t value)
		{
			return static_cast<double>(value);
		}

		// ---- 每个方法的实现(独立函数,便于方法描述先构造、方法体后引用) ----

		ScriptValue InputDownImpl(const ScriptValue* args, std::size_t count)
		{
			const ScriptServiceMethod& method = GameplayServiceBindings(nullptr)[0].Methods[0];
			const std::string action = RequireString("Input", method, args, count, 0);
			const uint32_t player = RequirePlayer("Input", method, args, count, 1);
			bool down = false;
			GameApp* session = GameApp::TryGet();
			if (session)
				down = session->Input().ActionDown(action, player);
			return ScriptValue::Boolean(down);
		}

		ScriptValue InputPressedImpl(const ScriptValue* args, std::size_t count)
		{
			const ScriptServiceMethod& method = GameplayServiceBindings(nullptr)[0].Methods[1];
			const std::string action = RequireString("Input", method, args, count, 0);
			const uint32_t player = RequirePlayer("Input", method, args, count, 1);
			bool pressed = false;
			if (GameApp* app = GameApp::TryGet())
				pressed = app->Input().ActionPressed(action, player);
			return ScriptValue::Boolean(pressed);
		}

		ScriptValue InputReleasedImpl(const ScriptValue* args, std::size_t count)
		{
			const ScriptServiceMethod& method = GameplayServiceBindings(nullptr)[0].Methods[2];
			const std::string action = RequireString("Input", method, args, count, 0);
			const uint32_t player = RequirePlayer("Input", method, args, count, 1);
			bool released = false;
			if (GameApp* app = GameApp::TryGet())
				released = app->Input().ActionReleased(action, player);
			return ScriptValue::Boolean(released);
		}

		ScriptValue InputAxisImpl(const ScriptValue* args, std::size_t count)
		{
			const ScriptServiceMethod& method = GameplayServiceBindings(nullptr)[0].Methods[3];
			const std::string axis = RequireString("Input", method, args, count, 0);
			const uint32_t player = RequirePlayer("Input", method, args, count, 1);
			float value = 0.0f;
			if (GameApp* app = GameApp::TryGet())
				value = app->Input().Axis(axis, player);
			return ScriptValue::Number(static_cast<double>(value));
		}

		ScriptValue InputPlayerCountImpl(const ScriptValue* args, std::size_t count)
		{
			(void)args;
			CheckArgumentCount("Input", GameplayServiceBindings(nullptr)[0].Methods[4], count, 0);
			uint32_t players = 0;
			if (GameApp* app = GameApp::TryGet())
				players = app->Input().GetPlayerCount();
			return ScriptValue::Number(static_cast<double>(players));
		}

		ScriptValue LevelRequestImpl(const ScriptValue* args, std::size_t count)
		{
			const ScriptServiceMethod& method = GameplayServiceBindings(nullptr)[1].Methods[0];
			const std::string id = RequireString("Level", method, args, count, 0);
			const bool additive = OptionalBool("Level", method, args, count, 1, false);
			return ScriptValue::Boolean(RequireGameApp().Levels().RequestLoad(id, additive));
		}

		ScriptValue LevelIsLoadingImpl(const ScriptValue* args, std::size_t count)
		{
			(void)args;
			CheckArgumentCount("Level", GameplayServiceBindings(nullptr)[1].Methods[1], count, 0);
			return ScriptValue::Boolean(RequireGameApp().Levels().IsLoading());
		}

		ScriptValue LevelStateImpl(const ScriptValue* args, std::size_t count)
		{
			(void)args;
			CheckArgumentCount("Level", GameplayServiceBindings(nullptr)[1].Methods[2], count, 0);
			return ScriptValue::String(LevelLoadStateName(RequireGameApp().Levels().GetState()));
		}

		ScriptValue LevelLastErrorImpl(const ScriptValue* args, std::size_t count)
		{
			(void)args;
			CheckArgumentCount("Level", GameplayServiceBindings(nullptr)[1].Methods[3], count, 0);
			return ScriptValue::String(RequireGameApp().Levels().GetLastError());
		}

		ScriptValue LevelPrimaryImpl(const ScriptValue* args, std::size_t count)
		{
			(void)args;
			CheckArgumentCount("Level", GameplayServiceBindings(nullptr)[1].Methods[4], count, 0);
			return ScriptValue::String(RequireGameApp().Levels().GetPrimaryLevel());
		}

		ScriptValue LevelActiveLevelsImpl(const ScriptValue* args, std::size_t count)
		{
			(void)args;
			CheckArgumentCount("Level", GameplayServiceBindings(nullptr)[1].Methods[5], count, 0);
			const std::vector<std::string>& levels = RequireGameApp().Levels().GetActiveLevels();
			ScriptTableRef result = MakeScriptTable();
			for (std::size_t index = 0; index < levels.size(); ++index)
				result.SetArrayElement(index + 1, ScriptValue::String(levels[index]));
			return result.ToValue();
		}

		ScriptValue LevelUnloadImpl(const ScriptValue* args, std::size_t count)
		{
			const ScriptServiceMethod& method = GameplayServiceBindings(nullptr)[1].Methods[6];
			const std::string id = RequireString("Level", method, args, count, 0);
			RequireGameApp().Levels().Unload(id);
			return ScriptValue::Nil();
		}

		ScriptValue LevelUnloadAllImpl(const ScriptValue* args, std::size_t count)
		{
			(void)args;
			CheckArgumentCount("Level", GameplayServiceBindings(nullptr)[1].Methods[7], count, 0);
			RequireGameApp().Levels().UnloadAll();
			return ScriptValue::Nil();
		}

		ScriptValue LevelEntriesImpl(const ScriptValue* args, std::size_t count)
		{
			(void)args;
			CheckArgumentCount("Level", GameplayServiceBindings(nullptr)[1].Methods[8], count, 0);
			const std::vector<LevelEntry>& entries = RequireGameApp().Levels().GetLevelList().Entries();
			ScriptTableRef result = MakeScriptTable();
			for (std::size_t index = 0; index < entries.size(); ++index)
			{
				ScriptTableRef entry = MakeScriptTable();
				entry.SetField("Id", ScriptValue::String(entries[index].Id));
				entry.SetField("Name", ScriptValue::String(entries[index].DisplayName));
				entry.SetField("Scene", ScriptValue::String(entries[index].ScenePath));
				result.SetArrayElement(index + 1, entry.ToValue());
			}
			return result.ToValue();
		}

		ScriptValue SaveToSlot(const ScriptServiceMethod& method, const ScriptValue* args, std::size_t count)
		{
			const uint32_t slot = RequireSlot("Save", method, args, count, 0);
			// 第二参数缺省 = 当前主关卡(与 LevelService::GetPrimaryLevel 同口径;空表示无活动关卡)。
			const std::string fallback = RequireGameApp().Levels().GetPrimaryLevel();
			const std::string levelId = OptionalString("Save", method, args, count, 1, fallback);
			SaveService& saves = RequireSaveService();
			return ScriptValue::Boolean(saves.Save(slot, levelId));
		}

		ScriptValue SaveLoadImpl(const ScriptValue* args, std::size_t count)
		{
			const ScriptServiceMethod& method = GameplayServiceBindings(nullptr)[2].Methods[1];
			const uint32_t slot = RequireSlot("Save", method, args, count, 0);
			SaveService& saves = RequireSaveService();
			return ScriptValue::Boolean(saves.Load(slot));
		}

		ScriptValue SaveDeleteImpl(const ScriptValue* args, std::size_t count)
		{
			const ScriptServiceMethod& method = GameplayServiceBindings(nullptr)[2].Methods[2];
			const uint32_t slot = RequireSlot("Save", method, args, count, 0);
			SaveService& saves = RequireSaveService();
			return ScriptValue::Boolean(saves.Delete(slot));
		}

		ScriptValue SaveListImpl(const ScriptValue* args, std::size_t count)
		{
			(void)args;
			CheckArgumentCount("Save", GameplayServiceBindings(nullptr)[2].Methods[3], count, 0);
			const std::vector<SaveSlotInfo> slots = RequireSaveService().ListSaves();
			ScriptTableRef result = MakeScriptTable();
			for (std::size_t index = 0; index < slots.size(); ++index)
			{
				const SaveSlotInfo& info = slots[index];
				ScriptTableRef entry = MakeScriptTable();
				entry.SetField("slot", ScriptValue::Number(static_cast<double>(info.Slot)));
				entry.SetField("level_id", ScriptValue::String(info.Header.LevelId));
				entry.SetField("version", ScriptValue::Number(static_cast<double>(info.Header.Version)));
				entry.SetField("timestamp", ScriptValue::Number(ToLuaNumber(info.Header.Timestamp)));
				entry.SetField("valid", ScriptValue::Boolean(info.Valid));
				entry.SetField("error", ScriptValue::String(info.Error));
				result.SetArrayElement(index + 1, entry.ToValue());
			}
			return result.ToValue();
		}

		ScriptValue SaveSetGlobalImpl(const ScriptValue* args, std::size_t count)
		{
			const ScriptServiceMethod& method = GameplayServiceBindings(nullptr)[2].Methods[4];
			const std::string key = RequireString("Save", method, args, count, 0);
			const ScriptValue& value = RequireArg("Save", method, args, count, 1);
			GlobalValue global;
			if (value.IsBoolean())
			{
				bool boolean = false;
				value.AsBool(&boolean);
				global.Type = GlobalValue::Kind::Bool;
				global.Bool = boolean;
			}
			else if (value.IsNumber())
			{
				double number = 0.0;
				value.AsNumber(&number);
				// Lua 只有 number:整数值在 int64 范围内才落成 Int,否则落成 Float(类型随值保留)。
				if (number == static_cast<double>(static_cast<int64_t>(number)))
				{
					global.Type = GlobalValue::Kind::Int;
					global.Int = static_cast<int64_t>(number);
				}
				else
				{
					global.Type = GlobalValue::Kind::Float;
					global.Float = number;
				}
			}
			else if (value.IsString())
			{
				std::string text;
				value.AsString(&text);
				global.Type = GlobalValue::Kind::String;
				global.String = std::move(text);
			}
			else
			{
				throw std::logic_error(MethodSignature("Save", method) +
					": argument 2 ('value') must be number, boolean or string, got " + TypeNameOf(value));
			}
			RequireSaveService().SetGlobal(key, std::move(global));
			return ScriptValue::Boolean(true);
		}

		ScriptValue SaveGetGlobalImpl(const ScriptValue* args, std::size_t count)
		{
			const ScriptServiceMethod& method = GameplayServiceBindings(nullptr)[2].Methods[5];
			const std::string key = RequireString("Save", method, args, count, 0);
			GlobalValue stored;
			if (!RequireSaveService().TryGetGlobal(key, &stored))
				return ScriptValue::Nil();
			switch (stored.Type)
			{
				case GlobalValue::Kind::Int:    return ScriptValue::Number(static_cast<double>(stored.Int));
				case GlobalValue::Kind::Float:  return ScriptValue::Number(stored.Float);
				case GlobalValue::Kind::Bool:   return ScriptValue::Boolean(stored.Bool);
				case GlobalValue::Kind::String: return ScriptValue::String(stored.String);
			}
			return ScriptValue::Nil();
		}

		ScriptValue SaveLastErrorImpl(const ScriptValue* args, std::size_t count)
		{
			(void)args;
			CheckArgumentCount("Save", GameplayServiceBindings(nullptr)[2].Methods[6], count, 0);
			return ScriptValue::String(RequireSaveService().GetLastError());
		}

		// 创建 table,写入全部方法,注册为全局。
		bool RegisterServiceTable(ScriptBindingContext& bindings, const ScriptServiceBinding& service,
			std::string* error)
		{
			LuauVm* vm = bindings.Vm();
			if (!vm || !service.Name || !service.Name[0] || !service.Methods || service.MethodCount == 0)
			{
				if (error) *error = "invalid service binding descriptor";
				return false;
			}
			if (vm->GetGlobal(service.Name).Type() != ScriptValueType::Nil)
			{
				if (error) *error = std::string("global name is already in use: ") + service.Name;
				return false;
			}
			ScriptTableRef table = vm->CreateTable();
			if (!table.IsValid())
			{
				if (error) *error = std::string("failed to create the service table: ") + service.Name;
				return false;
			}
			for (std::size_t index = 0; index < service.MethodCount; ++index)
			{
				const ScriptServiceMethod& method = service.Methods[index];
				if (!method.Name || !method.Name[0] || !method.Function)
				{
					if (error) *error = std::string("service '") + service.Name + "' has an invalid method entry";
					return false;
				}
				std::size_t required = 0;
				for (std::size_t param = 0; param < method.ParamCount; ++param)
					if (method.Params[param].Required) ++required;
				if (required > method.ExpectedArgs || method.ExpectedArgs > method.ParamCount)
				{
					if (error) *error = std::string("service '") + service.Name + "." + method.Name +
						"' has an inconsistent parameter descriptor";
					return false;
				}
				if (!table.SetField(method.Name, bindings.CreateFunction(method.Name, method.Function)))
				{
					if (error) *error = std::string("failed to bind ") + service.Name + "." + method.Name;
					return false;
				}
			}
			if (!table.SetMetaField("__newindex", bindings.CreateFunction(service.Name,
				[](const ScriptValue*, std::size_t) -> ScriptValue
				{
					throw std::logic_error("service tables are read-only; use the documented methods");
				})))
			{
				if (error) *error = std::string("failed to lock the service table: ") + service.Name;
				return false;
			}
			if (!vm->SetGlobal(service.Name, table.ToValue()))
			{
				if (error) *error = std::string("failed to publish the service table: ") + service.Name;
				return false;
			}
			return true;
		}
	}

	bool RegisterGameplayServiceBindings(ScriptBindingContext& bindings, std::string* error)
	{
		std::size_t count = 0;
		const ScriptServiceBinding* services = GameplayServiceBindings(&count);
		for (std::size_t index = 0; index < count; ++index)
		{
			if (!RegisterServiceTable(bindings, services[index], error))
				return false;
		}
		if (error) error->clear();
		return true;
	}

	const ScriptServiceBinding* GameplayServiceBindings(std::size_t* count)
	{
		// 方法描述是纯数据;方法体是独立函数,因此可以在描述表里直接引用、无需数组自引用。
		// 描述表必须是 static:GameplayServiceBindings() 每次调用返回同一份只读描述,
		// 存根渲染/运行时注册/方法体的签名诊断都引用它;放在栈上会让上一次调用的指针变悬垂。
		static const ScriptServiceParam inputActionPlayer[] = {
			{ "action", "string", ScriptServiceArgType::String, true, "Registered action name." },
			{ "player", "integer", ScriptServiceArgType::Number, false, "Player slot; 0 is the primary player." },
		};
		static const ScriptServiceParam inputAxis[] = {
			{ "axis", "string", ScriptServiceArgType::String, true, "Registered axis name." },
			{ "player", "integer", ScriptServiceArgType::Number, false, "Player slot; 0 is the primary player." },
		};
		static const ScriptServiceMethod inputMethods[] = {
			// 方法体先包装成 ScriptNativeFunction:描述表是静态的,函数指针需要显式构造。
			{ "Down", [](const ScriptValue* args, std::size_t count) -> ScriptValue
			{
				return InputDownImpl(args, count);
			}, inputActionPlayer, 2, 2, "boolean",
				"Whether any binding for the action is held; unknown actions and out-of-range players return false." },
			{ "Pressed", [](const ScriptValue* args, std::size_t count) -> ScriptValue
			{
				return InputPressedImpl(args, count);
			}, inputActionPlayer, 2, 2, "boolean",
				"Whether the action went down this frame; requires the host's end-of-frame update." },
			{ "Released", [](const ScriptValue* args, std::size_t count) -> ScriptValue
			{
				return InputReleasedImpl(args, count);
			}, inputActionPlayer, 2, 2, "boolean",
				"Whether the action went up this frame; requires the host's end-of-frame update." },
			{ "Axis", [](const ScriptValue* args, std::size_t count) -> ScriptValue
			{
				return InputAxisImpl(args, count);
			}, inputAxis, 2, 2, "number",
				"Axis value in [-1, 1]; unknown axes and out-of-range players return 0." },
			{ "PlayerCount", [](const ScriptValue* args, std::size_t count) -> ScriptValue
			{
				return InputPlayerCountImpl(args, count);
			}, nullptr, 0, 0, "integer",
				"Number of configured player slots." },
		};

		static const ScriptServiceParam levelRequest[] = {
			{ "id", "string", ScriptServiceArgType::String, true, "Level id from the level list." },
			{ "additive", "boolean", ScriptServiceArgType::Boolean, false, "Push the level on top of the current stack instead of replacing it." },
		};
		static const ScriptServiceParam levelUnload[] = {
			{ "id", "string", ScriptServiceArgType::String, true, "Active level id to unload." },
		};
		static const ScriptServiceMethod levelMethods[] = {
			{ "Request", [](const ScriptValue* args, std::size_t count) -> ScriptValue
			{
				return LevelRequestImpl(args, count);
			}, levelRequest, 2, 2, "boolean",
				"Request a level load; the next host frame pumps it. Unknown ids return false and set LastError." },
			{ "IsLoading", [](const ScriptValue* args, std::size_t count) -> ScriptValue
			{
				return LevelIsLoadingImpl(args, count);
			}, nullptr, 0, 0, "boolean",
				"Whether a level request is being processed." },
			{ "State", [](const ScriptValue* args, std::size_t count) -> ScriptValue
			{
				return LevelStateImpl(args, count);
			}, nullptr, 0, 0, "string",
				"Current load state: Idle/Reading/Deserializing/Activating/Failed." },
			{ "LastError", [](const ScriptValue* args, std::size_t count) -> ScriptValue
			{
				return LevelLastErrorImpl(args, count);
			}, nullptr, 0, 0, "string",
				"Last level-load error; empty when the last request succeeded." },
			{ "Primary", [](const ScriptValue* args, std::size_t count) -> ScriptValue
			{
				return LevelPrimaryImpl(args, count);
			}, nullptr, 0, 0, "string",
				"Id of the primary (index 0) active level; empty when nothing is active." },
			{ "ActiveLevels", [](const ScriptValue* args, std::size_t count) -> ScriptValue
			{
				return LevelActiveLevelsImpl(args, count);
			}, nullptr, 0, 0, "string[]",
				"Active level ids, primary level first." },
			{ "Unload", [](const ScriptValue* args, std::size_t count) -> ScriptValue
			{
				return LevelUnloadImpl(args, count);
			}, levelUnload, 1, 1, "",
				"Unload one active level; unknown ids are a no-op." },
			{ "UnloadAll", [](const ScriptValue* args, std::size_t count) -> ScriptValue
			{
				return LevelUnloadAllImpl(args, count);
			}, nullptr, 0, 0, "",
				"Unload every active level." },
			{ "Entries", [](const ScriptValue* args, std::size_t count) -> ScriptValue
			{
				return LevelEntriesImpl(args, count);
			}, nullptr, 0, 0, "table",
				"Level list entries as {Id, Name, Scene} tables." },
		};

		static const ScriptServiceParam saveSlot[] = {
			{ "slot", "integer", ScriptServiceArgType::Number, true, "Save slot index." },
		};
		static const ScriptServiceParam saveSlotLevel[] = {
			{ "slot", "integer", ScriptServiceArgType::Number, true, "Save slot index." },
			{ "levelId", "string", ScriptServiceArgType::String, false, "Level id written into the save header; defaults to Level.Primary()." },
		};
		static const ScriptServiceParam saveGlobal[] = {
			{ "key", "string", ScriptServiceArgType::String, true, "Global value key." },
			{ "value", "number|boolean|string", ScriptServiceArgType::Number | ScriptServiceArgType::Boolean | ScriptServiceArgType::String, true, "Typed global value." },
		};
		static const ScriptServiceParam saveKey[] = {
			{ "key", "string", ScriptServiceArgType::String, true, "Global value key." },
		};
		static const ScriptServiceMethod saveMethods[] = {
			{ "Save", [](const ScriptValue* args, std::size_t count)
				{
					return SaveToSlot(GameplayServiceBindings(nullptr)[2].Methods[0], args, count);
				},
				saveSlotLevel, 2, 2, "boolean",
				"Write a save slot; the level id defaults to the current primary level. Failure returns false and sets LastError." },
			{ "Load", [](const ScriptValue* args, std::size_t count) -> ScriptValue
			{
				return SaveLoadImpl(args, count);
			}, saveSlot, 1, 1, "boolean",
				"Load a save slot into the active scene. Failure returns false and sets LastError." },
			{ "Delete", [](const ScriptValue* args, std::size_t count) -> ScriptValue
			{
				return SaveDeleteImpl(args, count);
			}, saveSlot, 1, 1, "boolean", "Delete one save slot." },
			{ "List", [](const ScriptValue* args, std::size_t count) -> ScriptValue
			{
				return SaveListImpl(args, count);
			}, nullptr, 0, 0, "table",
				"Existing save slots as {slot, level_id, version, timestamp, valid, error} tables." },
			{ "SetGlobal", [](const ScriptValue* args, std::size_t count) -> ScriptValue
			{
				return SaveSetGlobalImpl(args, count);
			}, saveGlobal, 2, 2, "boolean",
				"Store a typed global value; the type is preserved across save/load." },
			{ "GetGlobal", [](const ScriptValue* args, std::size_t count) -> ScriptValue
			{
				return SaveGetGlobalImpl(args, count);
			}, saveKey, 1, 1, "number|boolean|string|nil",
				"Read a stored global value; missing keys return nil." },
			{ "LastError", [](const ScriptValue* args, std::size_t count) -> ScriptValue
			{
				return SaveLastErrorImpl(args, count);
			}, nullptr, 0, 0, "string",
				"Last save/load error; empty when the last operation succeeded." },
		};
		static const ScriptServiceBinding services[] = {
			{ "Input", "Read-only input service table; no raw key/device feed is exposed to scripts.", inputMethods, 5 },
			{ "Level", "Read-only level/flow service table; scene handles and host callbacks are not exposed.", levelMethods, 9 },
			{ "Save", "Read-only save service table; migrations, traits and paths are not exposed.", saveMethods, 7 },
		};
		if (count)
			*count = sizeof(services) / sizeof(services[0]);
		return services;
	}
}
