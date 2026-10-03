#pragma once

#include "World/Core/Export.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

struct lua_State;

namespace World
{
	class ScriptRef;
	class ScriptTableRef;
	class ScriptFunctionRef;

	namespace LuauDetail
	{
		class LuauVmState;
		struct ScriptRefPayload;
	}

	// 脚本值类型标签(与 Luau 的 lua_type 对应;这里不暴露 lua.h)。
	enum class ScriptValueType : std::uint8_t
	{
		Nil = 0,
		Boolean,
		Number,
		String,
		Table,
		Function,
		Userdata,
		Other,   // thread / buffer / lightuserdata 等本阶段不建模的值
	};

	WLD_API const char* ScriptValueTypeName(ScriptValueType type);

	// P2 绑定层的值装箱:
	//   - 标量(bool/number/string)按值持有,跨 VM 也能读;
	//   - 表/函数/userdata 持有 registry 引用(ScriptRefPayload),VM Shutdown() 后自动失效;
	//   - 取用走 As* 系列:类型不符返回 false,不抛异常 —— 绑定层才能把类型错误变成诊断而不是崩溃。
	//
	// 线程约定:值本身可在任意线程搬运,但压栈/取栈只能在该 VM 的 owner 线程上进行
	// (Luau 不是线程安全的;与 LuauVm 相同)。
	class WLD_API ScriptValue
	{
	public:
		ScriptValue();
		~ScriptValue();
		ScriptValue(const ScriptValue& other);
		ScriptValue(ScriptValue&& other) noexcept;
		ScriptValue& operator=(const ScriptValue& other);
		ScriptValue& operator=(ScriptValue&& other) noexcept;

		static ScriptValue Nil();
		static ScriptValue Boolean(bool value);
		static ScriptValue Number(double value);
		static ScriptValue String(std::string value);
		static ScriptValue String(const char* value);

		// 从 Luau 栈上的值装箱:标量拷贝,表/函数/userdata 建 registry 引用。
		// 拿不到 VM 状态(不是本引擎的 VM)时,引用类型退化为 Other,不抛异常。
		static ScriptValue FromStack(lua_State* state, int index);

		ScriptValueType Type() const { return m_Type; }
		const char* TypeName() const { return ScriptValueTypeName(m_Type); }
		bool IsNil() const { return m_Type == ScriptValueType::Nil; }
		bool IsBoolean() const { return m_Type == ScriptValueType::Boolean; }
		bool IsNumber() const { return m_Type == ScriptValueType::Number; }
		bool IsString() const { return m_Type == ScriptValueType::String; }
		bool IsTable() const { return m_Type == ScriptValueType::Table; }
		bool IsFunction() const { return m_Type == ScriptValueType::Function; }
		bool IsUserdata() const { return m_Type == ScriptValueType::Userdata; }

		// 类型不符时返回 false 且不改动 *out。
		bool AsBool(bool* out) const;
		bool AsNumber(double* out) const;
		bool AsString(std::string* out) const;
		bool AsTable(ScriptTableRef* out) const;
		bool AsFunction(ScriptFunctionRef* out) const;

		// userdata 的数据指针(供绑定层解包);非 userdata/引用已失效 → nullptr。
		void* AsUserdata() const;
		// userdata 的 tag;非 userdata → -1。
		int UserdataTag() const;

		// 引擎内部:引用类值的 registry 载荷(绑定层/ScriptRef 共用,不是脚本 API)。
		const std::shared_ptr<const LuauDetail::ScriptRefPayload>& Payload() const { return m_Payload; }
		// 引擎内部:用已有载荷构造同类型值。
		static ScriptValue FromPayload(std::shared_ptr<const LuauDetail::ScriptRefPayload> payload);

	private:
		ScriptValueType m_Type = ScriptValueType::Nil;
		bool m_Bool = false;
		double m_Number = 0.0;
		std::string m_String;
		std::shared_ptr<const LuauDetail::ScriptRefPayload> m_Payload;
	};

	namespace LuauDetail
	{
		// 引擎内部:一个 registry 引用。最后一个 shared_ptr 持有者析构时释放槽位;
		// VM 已 Shutdown 时只释放 C++ 侧内存,不碰 registry。
		struct ScriptRefPayload
		{
			std::shared_ptr<LuauVmState> Vm;
			int Ref = -1;                       // LUA_NOREF
			ScriptValueType Type = ScriptValueType::Other;

			ScriptRefPayload() = default;
			ScriptRefPayload(std::shared_ptr<LuauVmState> vm, int ref, ScriptValueType type);
			~ScriptRefPayload();
			ScriptRefPayload(const ScriptRefPayload&) = delete;
			ScriptRefPayload& operator=(const ScriptRefPayload&) = delete;
		};

		// 引擎内部:把栈上的值做成 registry 引用(标量/无效值返回空载荷)。
		std::shared_ptr<const ScriptRefPayload> MakeRefFromStack(lua_State* state, int index, ScriptValueType* type);

		// 引擎内部:把一个 ScriptValue 压到指定 VM 的栈上;引用类型在 VM 不匹配/已失效时失败并写 error。
		bool PushValueToStack(lua_State* state, const ScriptValue& value, std::string* error);

		// 引擎内部:统一的受保护调用入口(自建 message handler → luaL_traceback)。
		// 进入时栈顶是"函数 + nargs 个参数";成功时结果(函数原来的位置起)留在栈上,
		// 失败时把错误文本(含 chunk 名 + 行号 + "stack traceback" 段)写进 error 并恢复进入前的栈。
		bool ProtectedCall(lua_State* state, int nargs, int nresults, std::string* error);

		// 引擎内部:引用载荷是否仍指向一个活着的 VM(VM 关闭后一律 false)。
		bool IsPayloadLive(const std::shared_ptr<const ScriptRefPayload>& payload);
	}
}
