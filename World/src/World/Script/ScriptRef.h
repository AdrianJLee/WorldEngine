#pragma once

#include "World/Core/Export.h"
#include "World/Script/ScriptValue.h"

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

namespace World
{
	// 对 Luau registry 里一个值的引用(表/函数/userdata)。语义:
	//   - 拷贝共享同一份 registry 引用(shared_ptr);Release() 对全部拷贝生效且幂等;
	//   - VM Shutdown() 之后 IsValid() 一律为 false,析构不会访问已关闭的 registry;
	//   - 引用不延长 VM 的生命周期(VM 关闭后它只是"知道引用已失效")。
	class WLD_API ScriptRef
	{
	public:
		ScriptRef() = default;
		~ScriptRef() = default;
		ScriptRef(const ScriptRef& other) = default;
		ScriptRef& operator=(const ScriptRef& other) = default;
		ScriptRef(ScriptRef&& other) noexcept = default;
		ScriptRef& operator=(ScriptRef&& other) noexcept = default;

		bool IsValid() const;
		void Release();                                   // 幂等;之后 IsValid() == false
		// 装箱时记录的类型(引用被 Release 后为 nil);能不能用一律看 IsValid(),
		// 因为 VM Shutdown() 之后类型标签仍然保留。
		ScriptValueType Type() const;
		lua_State* State() const;                         // 无效 → nullptr
		int RefId() const;                                // 无效 → -1
		bool Push(lua_State* target = nullptr) const;     // target 为空时用自己所属的 VM
		ScriptValue ToValue() const;

		// 引擎内部:registry 载荷(绑定层/派生引用共用,不是脚本 API)。
		const std::shared_ptr<const LuauDetail::ScriptRefPayload>& Payload() const { return m_Payload; }

	protected:
		explicit ScriptRef(std::shared_ptr<const LuauDetail::ScriptRefPayload> payload);

	private:
		std::shared_ptr<const LuauDetail::ScriptRefPayload> m_Payload;
	};

	// 表引用:字段读写、数组部分遍历、元表(含 __index 回退)。
	class WLD_API ScriptTableRef : public ScriptRef
	{
	public:
		ScriptTableRef() = default;
		// 引擎内部/类型转换:类型不符或引用已失效 → 空引用。
		ScriptTableRef(const ScriptRef& reference);
		explicit ScriptTableRef(std::shared_ptr<const LuauDetail::ScriptRefPayload> payload);

		// 字符串键字段。GetField 缺失/失败 → nil;SetField 失败(只读/无效)返回 false。
		ScriptValue GetField(const char* name) const;
		bool SetField(const char* name, const ScriptValue& value) const;
		bool HasField(const char* name) const;

		// 数组部分:从 1 开始"整数值连续"的元素(遇到第一个 nil 结束)。
		// Length() 就是这段的长度;GetArray() 按序返回;SetArrayElement 写回第 index 个(1 起)。
		std::size_t Length() const;
		std::vector<ScriptValue> GetArray() const;
		bool SetArrayElement(std::size_t index, const ScriptValue& value) const;

		// 元表操作。SetIndexFallback(a) 等价于 metatable.__index = a(表回退);
		// 需要函数式 __index 时用 SetMetaField("__index", 函数值)。
		bool SetIndexFallback(const ScriptTableRef& fallback) const;
		ScriptValue GetMetaField(const char* name) const;
		bool SetMetaField(const char* name, const ScriptValue& value) const;
	};

	// 函数引用:受保护调用(错误文本含 chunk 名 + 行号 + stack traceback)。
	// 不能跨 pcall yield:生命周期回调是宿主驱动的同步调用,T2 若需要 yield 语义要单独设计。
	class WLD_API ScriptFunctionRef : public ScriptRef
	{
	public:
		ScriptFunctionRef() = default;
		// 引擎内部/类型转换:类型不符或引用已失效 → 空引用。
		ScriptFunctionRef(const ScriptRef& reference);
		explicit ScriptFunctionRef(std::shared_ptr<const LuauDetail::ScriptRefPayload> payload);

		// 取单个返回值(函数没有返回值 → nil);调用失败返回 false 并写 error。
		bool Call(const ScriptValue* args, std::size_t argCount, ScriptValue* result, std::string* error = nullptr) const;
		bool Call(const std::vector<ScriptValue>& args, ScriptValue* result, std::string* error = nullptr) const;
		// 取全部返回值(多返回值回调)。
		bool CallMulti(const ScriptValue* args, std::size_t argCount, std::vector<ScriptValue>* results, std::string* error = nullptr) const;
	};
}
