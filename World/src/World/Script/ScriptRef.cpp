#include "World/Script/ScriptRef.h"

#include "World/Script/LuauHeaders.h"
#include "World/Script/LuauVm.h"

#include <utility>

namespace World
{
	namespace
	{
		std::shared_ptr<const LuauDetail::ScriptRefPayload> PayloadIfType(const ScriptRef& reference, ScriptValueType expected)
		{
			if (!reference.IsValid() || reference.Type() != expected)
				return nullptr;
			return reference.Payload();
		}

		// 写操作前的只读检查:Luau 的 lua_setfield/rawset 在只读表上会直接抛 Lua 错误
		// (C++ 侧就是 longjmp),布尔返回值的 API 不能把这当成"失败"。见 T1 报告的限制说明。
		bool IsTableReadonly(lua_State* state, int tableIndex)
		{
			return lua_getreadonly(state, tableIndex) != 0;
		}
	}

	ScriptRef::ScriptRef(std::shared_ptr<const LuauDetail::ScriptRefPayload> payload)
		: m_Payload(std::move(payload))
	{
	}

	bool ScriptRef::IsValid() const
	{
		return LuauDetail::IsPayloadLive(m_Payload);
	}

	void ScriptRef::Release()
	{
		m_Payload.reset();
	}

	ScriptValueType ScriptRef::Type() const
	{
		return m_Payload ? m_Payload->Type : ScriptValueType::Nil;
	}

	lua_State* ScriptRef::State() const
	{
		return IsValid() ? m_Payload->Vm->State : nullptr;
	}

	int ScriptRef::RefId() const
	{
		return IsValid() ? m_Payload->Ref : -1;
	}

	bool ScriptRef::Push(lua_State* target) const
	{
		if (!IsValid())
			return false;
		if (!target)
			target = m_Payload->Vm->State;
		return LuauDetail::PushValueToStack(target, ToValue(), nullptr);
	}

	ScriptValue ScriptRef::ToValue() const
	{
		return ScriptValue::FromPayload(m_Payload);
	}

	ScriptTableRef::ScriptTableRef(const ScriptRef& reference)
		: ScriptRef(PayloadIfType(reference, ScriptValueType::Table))
	{
	}

	ScriptTableRef::ScriptTableRef(std::shared_ptr<const LuauDetail::ScriptRefPayload> payload)
		: ScriptRef(std::move(payload))
	{
	}

	ScriptValue ScriptTableRef::GetField(const char* name) const
	{
		if (!IsValid() || !name)
			return ScriptValue();

		lua_State* state = State();
		const int base = lua_gettop(state);
		if (!Push(state))
		{
			lua_settop(state, base);
			return ScriptValue();
		}
		lua_getfield(state, -1, name);
		ScriptValue value = ScriptValue::FromStack(state, -1);
		lua_settop(state, base);
		return value;
	}

	bool ScriptTableRef::SetField(const char* name, const ScriptValue& value) const
	{
		if (!IsValid() || !name)
			return false;

		lua_State* state = State();
		const int base = lua_gettop(state);
		if (!Push(state))
		{
			lua_settop(state, base);
			return false;
		}
		const int tableIndex = lua_gettop(state);
		if (IsTableReadonly(state, tableIndex))
		{
			lua_settop(state, base);
			return false;
		}
		if (!LuauDetail::PushValueToStack(state, value, nullptr))
		{
			lua_settop(state, base);
			return false;
		}
		lua_setfield(state, tableIndex, name);
		lua_settop(state, base);
		return true;
	}

	bool ScriptTableRef::HasField(const char* name) const
	{
		if (!IsValid() || !name)
			return false;

		lua_State* state = State();
		const int base = lua_gettop(state);
		if (!Push(state))
		{
			lua_settop(state, base);
			return false;
		}
		lua_rawgetfield(state, -1, name);
		const bool present = lua_type(state, -1) != LUA_TNIL;
		lua_settop(state, base);
		return present;
	}

	std::size_t ScriptTableRef::Length() const
	{
		if (!IsValid())
			return 0;

		lua_State* state = State();
		const int base = lua_gettop(state);
		if (!Push(state))
		{
			lua_settop(state, base);
			return 0;
		}

		// 先取表长(border)当上界,再按 1,2,3... 走到第一个 nil:
		// 这样既保证"整数值连续"的语义,也不会被 t[1e9]=1 这种稀疏表拖住。
		const int border = lua_objlen(state, -1);
		std::size_t count = 0;
		for (int index = 1; index <= border; ++index)
		{
			lua_rawgeti(state, -1, index);
			const bool present = lua_type(state, -1) != LUA_TNIL;
			lua_pop(state, 1);
			if (!present)
				break;
			++count;
		}

		lua_settop(state, base);
		return count;
	}

	std::vector<ScriptValue> ScriptTableRef::GetArray() const
	{
		std::vector<ScriptValue> values;
		if (!IsValid())
			return values;

		lua_State* state = State();
		const int base = lua_gettop(state);
		if (!Push(state))
		{
			lua_settop(state, base);
			return values;
		}

		const int border = lua_objlen(state, -1);
		// 预留上限 4096:表长可能是病态的大值,不应该因为 reserve 直接抛 bad_alloc。
		const std::size_t reserve = border > 0 ? static_cast<std::size_t>(border) : 0;
		values.reserve(reserve < 4096 ? reserve : 4096);
		for (int index = 1; index <= border; ++index)
		{
			lua_rawgeti(state, -1, index);
			if (lua_type(state, -1) == LUA_TNIL)
			{
				lua_pop(state, 1);
				break;
			}
			values.push_back(ScriptValue::FromStack(state, -1));
			lua_pop(state, 1);
		}

		lua_settop(state, base);
		return values;
	}

	bool ScriptTableRef::SetArrayElement(std::size_t index, const ScriptValue& value) const
	{
		if (!IsValid() || index == 0)
			return false;

		lua_State* state = State();
		const int base = lua_gettop(state);
		if (!Push(state))
		{
			lua_settop(state, base);
			return false;
		}
		const int tableIndex = lua_gettop(state);
		if (IsTableReadonly(state, tableIndex))
		{
			lua_settop(state, base);
			return false;
		}
		if (!LuauDetail::PushValueToStack(state, value, nullptr))
		{
			lua_settop(state, base);
			return false;
		}
		lua_rawseti(state, tableIndex, static_cast<int>(index));
		lua_settop(state, base);
		return true;
	}

	bool ScriptTableRef::SetIndexFallback(const ScriptTableRef& fallback) const
	{
		if (!IsValid() || !fallback.IsValid() || fallback.State() != State())
			return false;

		lua_State* state = State();
		const int base = lua_gettop(state);
		if (!Push(state))
		{
			lua_settop(state, base);
			return false;
		}
		const int tableIndex = lua_gettop(state);
		if (!lua_getmetatable(state, tableIndex))
			lua_newtable(state);
		const int metaIndex = lua_gettop(state);
		if (IsTableReadonly(state, metaIndex))
		{
			lua_settop(state, base);
			return false;
		}
		if (!fallback.Push(state))
		{
			lua_settop(state, base);
			return false;
		}
		lua_setfield(state, metaIndex, "__index");
		lua_setmetatable(state, tableIndex);
		lua_settop(state, base);
		return true;
	}

	ScriptValue ScriptTableRef::GetMetaField(const char* name) const
	{
		if (!IsValid() || !name)
			return ScriptValue();

		lua_State* state = State();
		const int base = lua_gettop(state);
		if (!Push(state))
		{
			lua_settop(state, base);
			return ScriptValue();
		}
		ScriptValue value;
		if (lua_getmetatable(state, -1))
		{
			lua_getfield(state, -1, name);
			value = ScriptValue::FromStack(state, -1);
		}
		lua_settop(state, base);
		return value;
	}

	bool ScriptTableRef::SetMetaField(const char* name, const ScriptValue& value) const
	{
		if (!IsValid() || !name)
			return false;

		lua_State* state = State();
		const int base = lua_gettop(state);
		if (!Push(state))
		{
			lua_settop(state, base);
			return false;
		}
		const int tableIndex = lua_gettop(state);
		if (!lua_getmetatable(state, tableIndex))
			lua_newtable(state);
		const int metaIndex = lua_gettop(state);
		if (IsTableReadonly(state, metaIndex))
		{
			lua_settop(state, base);
			return false;
		}
		if (!LuauDetail::PushValueToStack(state, value, nullptr))
		{
			lua_settop(state, base);
			return false;
		}
		lua_setfield(state, metaIndex, name);
		lua_setmetatable(state, tableIndex);
		lua_settop(state, base);
		return true;
	}

	ScriptFunctionRef::ScriptFunctionRef(const ScriptRef& reference)
		: ScriptRef(PayloadIfType(reference, ScriptValueType::Function))
	{
	}

	ScriptFunctionRef::ScriptFunctionRef(std::shared_ptr<const LuauDetail::ScriptRefPayload> payload)
		: ScriptRef(std::move(payload))
	{
	}

	bool ScriptFunctionRef::Call(const ScriptValue* args, std::size_t argCount, ScriptValue* result, std::string* error) const
	{
		std::vector<ScriptValue> results;
		if (!CallMulti(args, argCount, &results, error))
			return false;
		if (result)
			*result = results.empty() ? ScriptValue::Nil() : results.front();
		return true;
	}

	bool ScriptFunctionRef::Call(const std::vector<ScriptValue>& args, ScriptValue* result, std::string* error) const
	{
		return Call(args.data(), args.size(), result, error);
	}

	bool ScriptFunctionRef::CallMulti(const ScriptValue* args, std::size_t argCount, std::vector<ScriptValue>* results, std::string* error) const
	{
		if (results)
			results->clear();

		lua_State* state = State();
		if (!state)
		{
			if (error)
				*error = "invalid script function reference";
			return false;
		}

		const int base = lua_gettop(state);
		if (!Push(state))
		{
			if (error)
				*error = "failed to push script function";
			lua_settop(state, base);
			return false;
		}
		const int functionIndex = lua_gettop(state);

		for (std::size_t index = 0; index < argCount; ++index)
		{
			std::string pushError;
			if (!LuauDetail::PushValueToStack(state, args[index], &pushError))
			{
				if (error)
					*error = std::string("argument ") + std::to_string(index + 1) + ": " + pushError;
				lua_settop(state, base);
				return false;
			}
		}

		// ProtectedCall 内部会插入 message handler 并移除它;成功时结果落在 functionIndex 起。
		if (!LuauDetail::ProtectedCall(state, static_cast<int>(argCount), LUA_MULTRET, error))
		{
			lua_settop(state, base);
			return false;
		}

		if (results)
		{
			const int top = lua_gettop(state);
			results->reserve(static_cast<std::size_t>(top - functionIndex + 1));
			for (int index = functionIndex; index <= top; ++index)
				results->push_back(ScriptValue::FromStack(state, index));
		}
		lua_settop(state, base);
		return true;
	}
}
