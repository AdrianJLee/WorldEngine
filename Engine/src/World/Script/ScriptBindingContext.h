#pragma once

#include "World/Core/Export.h"
#include "World/Script/ScriptValue.h"

#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace World
{
	class LuauVm;
	class ScriptRef;
	class ScriptTableRef;

	// C++ 侧可绑定函数:参数已装箱(不含隐式 self;用 `obj:Method(a)` 调用时 args[0] 就是接收者),
	// 返回值装箱后回到 Lua。
	// 抛出的 C++ 异常会被转成 Lua error:pcall 可捕,错误文本保留异常内容(并加 "chunk:line:" 前缀)。
	using ScriptNativeFunction = std::function<ScriptValue(const ScriptValue* args, std::size_t argCount)>;
	// 构造:data 已按 ScriptUserTypeDesc::UserdataSize 分配好(对象尚未构造);
	// 抛异常同样转成 Lua error,此时对象未构造完成,不会调用任何析构。
	using ScriptConstructorFunction = std::function<void(void* data, const ScriptValue* args, std::size_t argCount)>;
	// 析构(追加于 T2,2026-09-17):userdata 被 GC 回收前调用,data 就是构造完成的对象地址。
	// 没有它,userdata 里的非平凡成员(std::weak_ptr/std::string/std::vector)只会泄漏不会析构。
	// 构造抛出异常时对象未构造完成,析构不会被调用(见 RegisterUserType 的实现说明)。
	using ScriptDestructorFunction = std::function<void(void* data)>;

	struct ScriptMethodBinding
	{
		const char* Name = nullptr;
		ScriptNativeFunction Function;
	};

	struct ScriptMetaMethodBinding
	{
		const char* Name = nullptr;   // 必须是 "__index"/"__newindex"/"__add"/"__eq"/"__tostring" 一类
		ScriptNativeFunction Function;
	};

	struct ScriptUserTypeDesc
	{
		const char* Name = nullptr;   // 全局名与类型名(例:"Entity")
		std::size_t UserdataSize = 0; // sizeof(T)
		ScriptConstructorFunction Constructor;                // 可空:该类型不可从脚本构造
		const ScriptMethodBinding* Methods = nullptr;
		std::size_t MethodCount = 0;
		const ScriptMetaMethodBinding* MetaMethods = nullptr;
		std::size_t MetaMethodCount = 0;
		// 追加(T2):可空。登记后该类型的 userdata 会在 GC 时调用析构;
		// 注意此时对象地址由 Unwrap/UserdataPointer 返回(内部在对象前留了头),
		// ScriptValue::AsUserdata() 返回的是载荷起始地址,不要用它解包带析构的类型。
		ScriptDestructorFunction Destructor;
	};

	// P2 绑定层的 usertype 注册:
	//   - 每种类型一个 Luau userdata tag,同名 Lua 类型通过 tag 校验(不是靠 metatable 比较);
	//   - 方法表同时是全局表 Type 与 userdata 元表的 __index(除非显式登记函数式 __index);
	//   - 类型表与元表都设只读,脚本不能改写其他实例看到的 API(与库表只读策略一致)。
	class WLD_API ScriptBindingContext
	{
	public:
		explicit ScriptBindingContext(LuauVm& vm);
		~ScriptBindingContext();
		ScriptBindingContext(const ScriptBindingContext&) = delete;
		ScriptBindingContext& operator=(const ScriptBindingContext&) = delete;

		LuauVm* Vm() const;
		bool IsValid() const;   // VM 已初始化

		// 注册并立即生效(全局表 + userdata 元表)。重复注册/名字已被占用 → false + error。
		bool RegisterUserType(const ScriptUserTypeDesc& desc, std::string* error = nullptr);

		bool IsUserTypeRegistered(const char* typeName) const;
		std::size_t UserTypeSize(const char* typeName) const;   // 未注册 → 0
		std::vector<std::string> RegisteredUserTypes() const;
		// 已注册类型的方法表(自定义 __index 可用它做"字段 → 方法"回退);未注册 → 空引用。
		ScriptTableRef MethodsTable(const char* typeName) const;

		// 装箱/解包:类型不符一律失败/nullptr,不抛异常、不触发 Lua error。
		ScriptValue NewUserdata(const char* typeName) const;
		bool IsUserdataOfType(const char* typeName, const ScriptValue& value) const;
		void* UserdataPointer(const char* typeName, const ScriptValue& value) const;

		// ---- T2 迁移追加(2026-09-17) ----
		// 把宿主可调用对象装箱成 Lua 函数值(用于注入全局,例:`print` 与测试夹具的注入函数)。
		// function 为空 / VM 未初始化 → 返回 nil 值。
		ScriptValue CreateFunction(const char* debugName, ScriptNativeFunction function) const;
		// 装箱一个不透明地址(无元表、无方法表的 tag 0 userdata,内容按 size 字节复制)。
		// 用于 `Entity:GetComponent` 的 "opaque component pointer" 语义:只能当作不透明句柄传递。
		ScriptValue NewOpaqueUserdata(const void* data, std::size_t size) const;

		template <typename T>
		bool Unwrap(const char* typeName, const ScriptValue& value, T** out) const
		{
			void* pointer = UserdataPointer(typeName, value);
			if (out)
				*out = static_cast<T*>(pointer);
			return pointer != nullptr;
		}

	private:
		struct Impl;
		std::unique_ptr<Impl> m_Impl;
	};
}
