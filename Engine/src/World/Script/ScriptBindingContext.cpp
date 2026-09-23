#include "World/Script/ScriptBindingContext.h"

#include "World/Script/LuauHeaders.h"
#include "World/Script/LuauVm.h"
#include "World/Script/ScriptRef.h"

#include <cstring>
#include <new>
#include <utility>

namespace World
{
	namespace
	{
		// 闭包 upvalue:C++ 可调用对象的共享所有权。
		// Luau 的 GC 调用 dtor 时才析构,所以脚本侧持有该函数多久,std::function 就活多久。
		struct NativeCallableHolder
		{
			ScriptNativeFunction Function;
		};

		struct ConstructorHolder
		{
			ScriptConstructorFunction Function;
			ScriptDestructorFunction Destructor;   // 非空 → userdata 载荷前有 UserdataHeader
			std::size_t UserdataSize = 0;
			std::size_t UserdataOffset = 0;
			int UserdataTag = -1;
		};

		// T2 追加:带析构的类型在对象前放一个头,头里存析构函数本身。
		// 这样 lua_setuserdatadtor 的转发函数不需要按 tag 区分类型(内存里就带着答案)。
		struct UserdataHeader
		{
			ScriptDestructorFunction Destructor;
		};

		void DestroyUserdataWithHeader(lua_State*, void* payload)
		{
			auto* header = static_cast<UserdataHeader*>(payload);
			if (header->Destructor)
				header->Destructor(static_cast<char*>(payload) + sizeof(UserdataHeader));
			header->~UserdataHeader();
		}

		void DestroyNativeCallable(void* data)
		{
			static_cast<NativeCallableHolder*>(data)->~NativeCallableHolder();
		}

		void DestroyConstructor(void* data)
		{
			static_cast<ConstructorHolder*>(data)->~ConstructorHolder();
		}

		// 把 C++ 异常/失败文本变成 "chunk:line: <文本>" 的 Lua error。
		// 调用前栈顶必须已经是错误文本;此处只加位置前缀,不分配 C++ 对象(longjmp 安全)。
		[[noreturn]] void RaiseWithLocation(lua_State* state)
		{
			luaL_where(state, 1);
			lua_insert(state, -2);
			lua_concat(state, 2);
			lua_error(state);
		}

		// 成员函数 trampoline:参数按 Lua 原样装箱(含 `obj:Method()` 的 self),
		// 返回值装箱回栈;C++ 异常转成脚本可捕获的 Lua error。
		int NativeFunctionTrampoline(lua_State* state)
		{
			const auto* holder = static_cast<const NativeCallableHolder*>(lua_touserdata(state, lua_upvalueindex(1)));
			if (!holder || !holder->Function)
			{
				lua_pushliteral(state, "unbound native function");
				RaiseWithLocation(state);
			}

			const int argumentCount = lua_gettop(state);
			bool ok = false;
			{
				// 所有 C++ 对象都活在这个作用域里:错误文本先压进 Lua 栈(那边会拷贝),
				// 离开作用域析构完再 lua_error —— longjmp 不跳过析构。
				std::string message;
				ScriptValue result;
				{
					std::vector<ScriptValue> args;
					args.reserve(argumentCount > 0 ? static_cast<std::size_t>(argumentCount) : 0);
					for (int index = 1; index <= argumentCount; ++index)
						args.push_back(ScriptValue::FromStack(state, index));
					try
					{
						result = holder->Function(args.data(), args.size());
						ok = true;
					}
					catch (const std::exception& exception)
					{
						message = exception.what();
					}
					catch (...)
					{
						message = "unknown C++ exception";
					}
				}

				if (ok && !LuauDetail::PushValueToStack(state, result, &message))
					ok = false;
				if (!ok)
				{
					if (message.empty())
						message = "native call failed";
					lua_pushstring(state, message.c_str());
				}
			}

			if (!ok)
				RaiseWithLocation(state);
			return 1;
		}

		// 构造函数 trampoline:先按登记尺寸分配带元表的 userdata,再交给 C++ 就地构造。
		int ConstructorTrampoline(lua_State* state)
		{
			const auto* holder = static_cast<const ConstructorHolder*>(lua_touserdata(state, lua_upvalueindex(1)));
			if (!holder || !holder->Function)
			{
				lua_pushliteral(state, "unbound user type constructor");
				RaiseWithLocation(state);
			}

			const int argumentCount = lua_gettop(state);
			void* payload = lua_newuserdatataggedwithmetatable(state, holder->UserdataOffset + holder->UserdataSize, holder->UserdataTag);
			if (!payload)
			{
				lua_pushliteral(state, "failed to allocate userdata");
				RaiseWithLocation(state);
			}
			if (holder->UserdataOffset)
				new (payload) UserdataHeader{ holder->Destructor };
			void* data = static_cast<char*>(payload) + holder->UserdataOffset;

			bool ok = false;
			{
				std::string message;
				{
					std::vector<ScriptValue> args;
					args.reserve(argumentCount > 0 ? static_cast<std::size_t>(argumentCount) : 0);
					for (int index = 1; index <= argumentCount; ++index)
						args.push_back(ScriptValue::FromStack(state, index));
					try
					{
						holder->Function(data, args.data(), args.size());
						ok = true;
					}
					catch (const std::exception& exception)
					{
						message = exception.what();
					}
					catch (...)
					{
						message = "unknown C++ exception";
					}
				}

				if (!ok)
				{
					if (message.empty())
						message = "constructor failed";
					// 构造未完成 → 不能调用对象析构;把 tag 换到 0(无析构)再抛错。
					if (holder->UserdataOffset)
						lua_setuserdatatag(state, -1, 0);
					lua_pushstring(state, message.c_str());
				}
			}

			if (!ok)
				RaiseWithLocation(state);
			return 1;
		}

		void PushNativeClosure(lua_State* state, const ScriptNativeFunction& function, const char* name)
		{
			void* raw = lua_newuserdatadtor(state, sizeof(NativeCallableHolder), &DestroyNativeCallable);
			new (raw) NativeCallableHolder{ function };
			lua_pushcclosurek(state, &NativeFunctionTrampoline, name, 1, nullptr);
		}

		void PushConstructorClosure(lua_State* state, const ScriptConstructorFunction& function,
			const ScriptDestructorFunction& destructor, std::size_t userdataSize, std::size_t userdataOffset, int tag)
		{
			void* raw = lua_newuserdatadtor(state, sizeof(ConstructorHolder), &DestroyConstructor);
			new (raw) ConstructorHolder{ function, destructor, userdataSize, userdataOffset, tag };
			lua_pushcclosurek(state, &ConstructorTrampoline, "new", 1, nullptr);
		}

		// 栈顶 userdata 的 tag/尺寸双重校验(tag 相同但尺寸不同 = 调用方串了类型)。
		// 返回对象地址:带析构的类型对象前有一个 UserdataHeader,offset 就是它。
		void* CheckUserdataAtTop(lua_State* state, int tag, std::size_t expectedSize, std::size_t offset)
		{
			if (lua_type(state, -1) != LUA_TUSERDATA)
				return nullptr;
			char* payload = static_cast<char*>(lua_touserdatatagged(state, -1, tag));
			if (!payload)
				return nullptr;
			const int size = lua_objlen(state, -1);
			if (size < 0 || static_cast<std::size_t>(size) != expectedSize + offset)
				return nullptr;
			return payload + offset;
		}

		bool StartsWithDoubleUnderscore(const char* name)
		{
			return name && name[0] == '_' && name[1] == '_';
		}
	}

	struct ScriptBindingContext::Impl
	{
		struct TypeEntry
		{
			std::string Name;
			int Tag = -1;
			std::size_t UserdataSize = 0;
			std::size_t UserdataOffset = 0;   // >0 = 对象前有 UserdataHeader
			ScriptDestructorFunction Destructor;
			ScriptTableRef Methods;
			ScriptTableRef Metatable;
		};

		LuauVm* Vm = nullptr;
		std::vector<TypeEntry> Types;

		const TypeEntry* Find(const char* name) const
		{
			if (!name)
				return nullptr;
			for (const TypeEntry& entry : Types)
			{
				if (entry.Name == name)
					return &entry;
			}
			return nullptr;
		}
	};

	ScriptBindingContext::ScriptBindingContext(LuauVm& vm)
		: m_Impl(std::make_unique<Impl>())
	{
		m_Impl->Vm = &vm;
	}

	ScriptBindingContext::~ScriptBindingContext() = default;

	LuauVm* ScriptBindingContext::Vm() const
	{
		return m_Impl ? m_Impl->Vm : nullptr;
	}

	bool ScriptBindingContext::IsValid() const
	{
		return m_Impl && m_Impl->Vm && m_Impl->Vm->IsInitialized();
	}

	bool ScriptBindingContext::RegisterUserType(const ScriptUserTypeDesc& desc, std::string* error)
	{
		auto fail = [error](const std::string& message)
		{
			if (error)
				*error = message;
			return false;
		};

		if (!IsValid())
			return fail("script vm is not initialized");
		if (!desc.Name || !desc.Name[0])
			return fail("user type name is required");
		if (desc.UserdataSize == 0)
			return fail("userdata size must be non-zero");
		if (m_Impl->Find(desc.Name))
			return fail(std::string("user type already registered: ") + desc.Name);
		for (std::size_t index = 0; index < desc.MethodCount; ++index)
		{
			if (!desc.Methods[index].Name || !desc.Methods[index].Name[0])
				return fail("method name is required");
			if (!desc.Methods[index].Function)
				return fail(std::string("method has no implementation: ") + desc.Methods[index].Name);
		}
		for (std::size_t index = 0; index < desc.MetaMethodCount; ++index)
		{
			if (!StartsWithDoubleUnderscore(desc.MetaMethods[index].Name))
				return fail("metamethod name must start with '__'");
			if (!desc.MetaMethods[index].Function)
				return fail(std::string("metamethod has no implementation: ") + desc.MetaMethods[index].Name);
		}

		LuauVm& vm = *m_Impl->Vm;
		const std::shared_ptr<LuauDetail::LuauVmState>& token = vm.SharedState();
		if (!token || !token->State)
			return fail("script vm is not initialized");
		if (token->NextUserdataTag > LuauDetail::kLastScriptUserdataTag)
			return fail("script userdata tag pool is exhausted");

		lua_State* state = vm.State();
		// 全局名不能被占用:注册盖掉库表(或另一个类型)会让"注册生效"变成静默的行为改变。
		lua_getfield(state, LUA_GLOBALSINDEX, desc.Name);
		const bool occupied = lua_type(state, -1) != LUA_TNIL;
		lua_pop(state, 1);
		if (occupied)
			return fail(std::string("global name is already in use: ") + desc.Name);

		const int tag = token->NextUserdataTag++;
		const int base = lua_gettop(state);
		// 有析构回调的类型在对象前放一个头(对象地址 = 载荷 + offset);其余类型布局保持不变。
		const std::size_t userdataOffset = desc.Destructor ? sizeof(UserdataHeader) : 0;

		// 方法表:既是全局表 Type,也是 userdata 元表的 __index(除非显式登记函数式 __index)。
		const std::size_t methodHint = desc.MethodCount < 64 ? desc.MethodCount + 1 : 64;
		lua_createtable(state, 0, static_cast<int>(methodHint));
		const int methodsIndex = lua_gettop(state);
		for (std::size_t index = 0; index < desc.MethodCount; ++index)
		{
			PushNativeClosure(state, desc.Methods[index].Function, desc.Methods[index].Name);
			lua_setfield(state, methodsIndex, desc.Methods[index].Name);
		}
		if (desc.Constructor)
		{
			PushConstructorClosure(state, desc.Constructor, desc.Destructor, desc.UserdataSize, userdataOffset, tag);
			lua_setfield(state, methodsIndex, "new");
		}
		lua_setreadonly(state, methodsIndex, true);

		lua_newtable(state);
		const int metaIndex = lua_gettop(state);
		bool hasIndexMeta = false;
		for (std::size_t index = 0; index < desc.MetaMethodCount; ++index)
		{
			const char* name = desc.MetaMethods[index].Name;
			PushNativeClosure(state, desc.MetaMethods[index].Function, name);
			lua_setfield(state, metaIndex, name);
			if (std::strcmp(name, "__index") == 0)
				hasIndexMeta = true;
		}
		lua_pushstring(state, desc.Name);
		lua_setfield(state, metaIndex, "__type");   // 让类型错误的文本显示注册名(见 luaT_objtypenamestr)
		if (!hasIndexMeta)
		{
			lua_pushvalue(state, methodsIndex);
			lua_setfield(state, metaIndex, "__index");
		}
		lua_setreadonly(state, metaIndex, true);
		lua_setuserdatametatable(state, tag);       // 弹出元表并绑定到 tag(不可重复赋值)
		if (desc.Destructor)
			lua_setuserdatadtor(state, tag, &DestroyUserdataWithHeader);

		lua_pushvalue(state, methodsIndex);
		lua_setfield(state, LUA_GLOBALSINDEX, desc.Name);

		Impl::TypeEntry entry;
		entry.Name = desc.Name;
		entry.Tag = tag;
		entry.UserdataSize = desc.UserdataSize;
		entry.UserdataOffset = userdataOffset;
		entry.Destructor = desc.Destructor;
		ScriptValue methodsValue = ScriptValue::FromStack(state, methodsIndex);
		methodsValue.AsTable(&entry.Methods);
		ScriptValue metaValue = ScriptValue::FromStack(state, metaIndex);
		metaValue.AsTable(&entry.Metatable);
		m_Impl->Types.push_back(std::move(entry));

		lua_settop(state, base);
		return true;
	}

	bool ScriptBindingContext::IsUserTypeRegistered(const char* typeName) const
	{
		return m_Impl && m_Impl->Find(typeName) != nullptr;
	}

	std::size_t ScriptBindingContext::UserTypeSize(const char* typeName) const
	{
		const Impl::TypeEntry* entry = m_Impl ? m_Impl->Find(typeName) : nullptr;
		return entry ? entry->UserdataSize : 0;
	}

	std::vector<std::string> ScriptBindingContext::RegisteredUserTypes() const
	{
		std::vector<std::string> names;
		if (!m_Impl)
			return names;
		names.reserve(m_Impl->Types.size());
		for (const Impl::TypeEntry& entry : m_Impl->Types)
			names.push_back(entry.Name);
		return names;
	}

	ScriptTableRef ScriptBindingContext::MethodsTable(const char* typeName) const
	{
		const Impl::TypeEntry* entry = m_Impl ? m_Impl->Find(typeName) : nullptr;
		return entry ? entry->Methods : ScriptTableRef();
	}

	ScriptValue ScriptBindingContext::NewUserdata(const char* typeName) const
	{
		const Impl::TypeEntry* entry = m_Impl ? m_Impl->Find(typeName) : nullptr;
		if (!entry || !IsValid())
			return ScriptValue();

		lua_State* state = m_Impl->Vm->State();
		const int base = lua_gettop(state);
		void* payload = lua_newuserdatataggedwithmetatable(state, entry->UserdataOffset + entry->UserdataSize, entry->Tag);
		if (!payload)
		{
			lua_settop(state, base);
			return ScriptValue();
		}
		if (entry->UserdataOffset)
			new (payload) UserdataHeader{ entry->Destructor };
		ScriptValue value = ScriptValue::FromStack(state, -1);
		lua_settop(state, base);
		return value;
	}

	bool ScriptBindingContext::IsUserdataOfType(const char* typeName, const ScriptValue& value) const
	{
		const Impl::TypeEntry* entry = m_Impl ? m_Impl->Find(typeName) : nullptr;
		if (!entry || !IsValid())
			return false;
		const std::shared_ptr<const LuauDetail::ScriptRefPayload>& payload = value.Payload();
		if (!payload || payload->Vm.get() != m_Impl->Vm->SharedState().get())
			return false;
		return value.UserdataTag() == entry->Tag;
	}

	void* ScriptBindingContext::UserdataPointer(const char* typeName, const ScriptValue& value) const
	{
		const Impl::TypeEntry* entry = m_Impl ? m_Impl->Find(typeName) : nullptr;
		if (!entry || !IsValid())
			return nullptr;
		const std::shared_ptr<const LuauDetail::ScriptRefPayload>& payload = value.Payload();
		if (!payload || payload->Vm.get() != m_Impl->Vm->SharedState().get())
			return nullptr;

		lua_State* state = m_Impl->Vm->State();
		const int base = lua_gettop(state);
		if (!LuauDetail::PushValueToStack(state, value, nullptr))
		{
			lua_settop(state, base);
			return nullptr;
		}
		void* pointer = CheckUserdataAtTop(state, entry->Tag, entry->UserdataSize, entry->UserdataOffset);
		lua_settop(state, base);
		return pointer;
	}

	ScriptValue ScriptBindingContext::CreateFunction(const char* debugName, ScriptNativeFunction function) const
	{
		if (!IsValid() || !function)
			return ScriptValue();

		lua_State* state = m_Impl->Vm->State();
		const int base = lua_gettop(state);
		PushNativeClosure(state, function, debugName ? debugName : "host function");
		ScriptValue value = ScriptValue::FromStack(state, -1);
		lua_settop(state, base);
		return value;
	}

	ScriptValue ScriptBindingContext::NewOpaqueUserdata(const void* data, std::size_t size) const
	{
		if (!IsValid() || size == 0)
			return ScriptValue();

		lua_State* state = m_Impl->Vm->State();
		const int base = lua_gettop(state);
		void* payload = lua_newuserdatatagged(state, size, 0);   // tag 0:无元表、无析构
		if (!payload)
		{
			lua_settop(state, base);
			return ScriptValue();
		}
		if (data)
			std::memcpy(payload, data, size);
		ScriptValue value = ScriptValue::FromStack(state, -1);
		lua_settop(state, base);
		return value;
	}
}
