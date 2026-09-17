#include "World/Script/ScriptValue.h"

#include "World/Script/LuauHeaders.h"
#include "World/Script/LuauVm.h"
#include "World/Script/ScriptRef.h"

#include <utility>

namespace World
{
	const char* ScriptValueTypeName(ScriptValueType type)
	{
		switch (type)
		{
		case ScriptValueType::Nil: return "nil";
		case ScriptValueType::Boolean: return "boolean";
		case ScriptValueType::Number: return "number";
		case ScriptValueType::String: return "string";
		case ScriptValueType::Table: return "table";
		case ScriptValueType::Function: return "function";
		case ScriptValueType::Userdata: return "userdata";
		default: return "other";
		}
	}

	namespace
	{
		ScriptValueType TypeFromLua(int luaType)
		{
			switch (luaType)
			{
			case LUA_TNIL: return ScriptValueType::Nil;
			case LUA_TBOOLEAN: return ScriptValueType::Boolean;
			case LUA_TNUMBER: return ScriptValueType::Number;
			case LUA_TSTRING: return ScriptValueType::String;
			case LUA_TTABLE: return ScriptValueType::Table;
			case LUA_TFUNCTION: return ScriptValueType::Function;
			case LUA_TUSERDATA: return ScriptValueType::Userdata;
			default: return ScriptValueType::Other;
			}
		}

		bool IsReferenceType(ScriptValueType type)
		{
			return type == ScriptValueType::Table || type == ScriptValueType::Function || type == ScriptValueType::Userdata;
		}
	}

	namespace LuauDetail
	{
		bool IsPayloadLive(const std::shared_ptr<const ScriptRefPayload>& payload)
		{
			return payload && payload->Ref > 0 && payload->Vm && payload->Vm->State != nullptr;
		}

		ScriptRefPayload::ScriptRefPayload(std::shared_ptr<LuauVmState> vm, int ref, ScriptValueType type)
			: Vm(std::move(vm)), Ref(ref), Type(type)
		{
		}

		ScriptRefPayload::~ScriptRefPayload()
		{
			// VM 关闭后 registry 已经不在了(Shutdown 会先把令牌里的 State 清空):
			// 这里必须只释放 C++ 侧内存,否则就是 use-after-free。
			if (Ref > 0 && Vm && Vm->State)
				lua_unref(Vm->State, Ref);
		}

		std::shared_ptr<const ScriptRefPayload> MakeRefFromStack(lua_State* state, int index, ScriptValueType* type)
		{
			const ScriptValueType valueType = TypeFromLua(lua_type(state, index));
			if (type)
				*type = valueType;
			if (!IsReferenceType(valueType))
				return nullptr;

			// 拿不到令牌说明这不是本引擎建的 VM:宁可不建引用,也不要存一个无法判活的状态。
			std::shared_ptr<LuauVmState> vm = VmStateFromLuaState(state);
			if (!vm || !vm->State)
				return nullptr;

			const int ref = lua_ref(state, index);
			if (ref <= 0)   // LUA_REFNIL:值不可引用(nil)
				return nullptr;
			return std::make_shared<const ScriptRefPayload>(std::move(vm), ref, valueType);
		}

		bool PushValueToStack(lua_State* state, const ScriptValue& value, std::string* error)
		{
			auto fail = [error](const char* message)
			{
				if (error)
					*error = message;
				return false;
			};

			if (!state)
				return fail("no script vm");

			switch (value.Type())
			{
			case ScriptValueType::Nil:
				lua_pushnil(state);
				return true;
			case ScriptValueType::Boolean:
			{
				bool boolean = false;
				value.AsBool(&boolean);
				lua_pushboolean(state, boolean ? 1 : 0);
				return true;
			}
			case ScriptValueType::Number:
			{
				double number = 0.0;
				value.AsNumber(&number);
				lua_pushnumber(state, number);
				return true;
			}
			case ScriptValueType::String:
			{
				std::string text;
				value.AsString(&text);
				lua_pushlstring(state, text.data(), text.size());
				return true;
			}
			case ScriptValueType::Table:
			case ScriptValueType::Function:
			case ScriptValueType::Userdata:
			{
				const std::shared_ptr<const ScriptRefPayload>& payload = value.Payload();
				if (!payload)
					return fail("script value has no registry reference");
				if (!payload->Vm || !payload->Vm->State)
					return fail("script value belongs to a closed vm");

				std::shared_ptr<LuauVmState> target = VmStateFromLuaState(state);
				if (!target || target->State == nullptr)
					return fail("target state does not belong to a worldengine script vm");
				if (target.get() != payload->Vm.get())
					return fail("script value belongs to a different vm");

				lua_rawgeti(state, LUA_REGISTRYINDEX, payload->Ref);
				return true;
			}
			default:
				return fail("script value type cannot cross the binding boundary");
			}
		}

		namespace
		{
			// 受保护调用的 message handler。
			// 本 Luau 版本的 luaL_traceback **不带** "stack traceback:" 标题(与 PUC Lua 不同),
			// 这里自己补一段,让错误文本契约对上层稳定:
			//   "<chunk>:<line>: <message>\nstack traceback:\n<frame>\n..."
			int TracebackErrorHandler(lua_State* state)
			{
				const int errorType = lua_type(state, 1);
				if (errorType == LUA_TSTRING || errorType == LUA_TNUMBER)
					lua_settop(state, 1);
				else
				{
					// 非字符串错误对象(表/自定义值)不做 tostring:__tostring 可能在错误处理里再抛一次。
					lua_settop(state, 0);
					lua_pushliteral(state, "(error object is not a string)");
				}

				lua_pushliteral(state, "\nstack traceback:\n");
				luaL_traceback(state, state, nullptr, 1);
				lua_concat(state, 3);
				return 1;
			}
		}

		bool ProtectedCall(lua_State* state, int nargs, int nresults, std::string* error)
		{
			if (!state)
			{
				if (error)
					*error = "no script vm";
				return false;
			}

			const int functionIndex = lua_gettop(state) - nargs;
			if (functionIndex < 1)
			{
				if (error)
					*error = "protected call stack underflow";
				return false;
			}

			// 标准 msgh 布局:把 handler 插到"函数 + 参数"下面(errfunc 位置就是函数位置)。
			lua_pushcfunction(state, &TracebackErrorHandler, "World.TracebackErrorHandler");
			lua_insert(state, functionIndex);

			const int status = lua_pcall(state, nargs, nresults, functionIndex);
			if (status != 0)
			{
				const char* message = lua_tostring(state, -1);
				if (error)
					*error = message ? message : "script error";
				lua_settop(state, functionIndex - 1);
				return false;
			}

			lua_remove(state, functionIndex);   // 去掉 handler,结果留在原函数位置
			return true;
		}
	}

	ScriptValue::ScriptValue() = default;
	ScriptValue::~ScriptValue() = default;
	ScriptValue::ScriptValue(const ScriptValue& other) = default;
	ScriptValue::ScriptValue(ScriptValue&& other) noexcept = default;
	ScriptValue& ScriptValue::operator=(const ScriptValue& other) = default;
	ScriptValue& ScriptValue::operator=(ScriptValue&& other) noexcept = default;

	ScriptValue ScriptValue::Nil()
	{
		return ScriptValue();
	}

	ScriptValue ScriptValue::Boolean(bool value)
	{
		ScriptValue result;
		result.m_Type = ScriptValueType::Boolean;
		result.m_Bool = value;
		return result;
	}

	ScriptValue ScriptValue::Number(double value)
	{
		ScriptValue result;
		result.m_Type = ScriptValueType::Number;
		result.m_Number = value;
		return result;
	}

	ScriptValue ScriptValue::String(std::string value)
	{
		ScriptValue result;
		result.m_Type = ScriptValueType::String;
		result.m_String = std::move(value);
		return result;
	}

	ScriptValue ScriptValue::String(const char* value)
	{
		return ScriptValue::String(value ? std::string(value) : std::string());
	}

	ScriptValue ScriptValue::FromStack(lua_State* state, int index)
	{
		if (!state || lua_ispseudo(index))
			return ScriptValue();

		switch (lua_type(state, index))
		{
		case LUA_TNIL:
			return ScriptValue();
		case LUA_TBOOLEAN:
			return ScriptValue::Boolean(lua_toboolean(state, index) != 0);
		case LUA_TNUMBER:
			return ScriptValue::Number(lua_tonumber(state, index));
		case LUA_TSTRING:
		{
			size_t length = 0;
			const char* text = lua_tolstring(state, index, &length);
			return ScriptValue::String(text ? std::string(text, length) : std::string());
		}
		default:
			break;
		}

		ScriptValueType type = ScriptValueType::Other;
		std::shared_ptr<const LuauDetail::ScriptRefPayload> payload = LuauDetail::MakeRefFromStack(state, index, &type);

		ScriptValue result;
		result.m_Type = payload ? type : ScriptValueType::Other;
		result.m_Payload = std::move(payload);
		return result;
	}

	ScriptValue ScriptValue::FromPayload(std::shared_ptr<const LuauDetail::ScriptRefPayload> payload)
	{
		ScriptValue result;
		result.m_Type = payload ? payload->Type : ScriptValueType::Nil;
		result.m_Payload = std::move(payload);
		return result;
	}

	bool ScriptValue::AsBool(bool* out) const
	{
		if (m_Type != ScriptValueType::Boolean)
			return false;
		if (out)
			*out = m_Bool;
		return true;
	}

	bool ScriptValue::AsNumber(double* out) const
	{
		if (m_Type != ScriptValueType::Number)
			return false;
		if (out)
			*out = m_Number;
		return true;
	}

	bool ScriptValue::AsString(std::string* out) const
	{
		if (m_Type != ScriptValueType::String)
			return false;
		if (out)
			*out = m_String;
		return true;
	}

	bool ScriptValue::AsTable(ScriptTableRef* out) const
	{
		if (m_Type != ScriptValueType::Table || !IsPayloadLive(m_Payload))
			return false;
		if (out)
			*out = ScriptTableRef(m_Payload);
		return true;
	}

	bool ScriptValue::AsFunction(ScriptFunctionRef* out) const
	{
		if (m_Type != ScriptValueType::Function || !IsPayloadLive(m_Payload))
			return false;
		if (out)
			*out = ScriptFunctionRef(m_Payload);
		return true;
	}

	void* ScriptValue::AsUserdata() const
	{
		if (m_Type != ScriptValueType::Userdata || !IsPayloadLive(m_Payload))
			return nullptr;
		lua_State* state = m_Payload->Vm->State;
		lua_rawgeti(state, LUA_REGISTRYINDEX, m_Payload->Ref);
		void* pointer = lua_touserdata(state, -1);
		lua_pop(state, 1);
		return pointer;
	}

	int ScriptValue::UserdataTag() const
	{
		if (m_Type != ScriptValueType::Userdata || !IsPayloadLive(m_Payload))
			return -1;
		lua_State* state = m_Payload->Vm->State;
		lua_rawgeti(state, LUA_REGISTRYINDEX, m_Payload->Ref);
		const int tag = lua_userdatatag(state, -1);
		lua_pop(state, 1);
		return tag;
	}
}
