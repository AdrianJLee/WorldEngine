// P2 W1b:绑定层地基的回归(不依赖窗口/渲染,可 headless 跑)。
//
// 覆盖:
//   1. 每实例 environment:同名全局互不影响,__index 回退到线程全局(C++ 注册的库可见);
//   2. 宿主全局注入(SetGlobal/ClearGlobal/GetGlobal);
//   3. 受保护调用:错误文本含 chunk 名 + 行号 + "stack traceback" 段;
//   4. 引用(ScriptRef/ScriptValue)在 VM Shutdown / VM 对象销毁后失效,Release 幂等;
//   5. 表操作:字段、数组部分、__index 回退、只读表写失败;
//   6. usertype 注册:构造 + 方法 + 运算符 + 字段读写 + C++ 异常可被 pcall 捕获;
//   7. 沙箱不被绑定层破坏(禁用全局仍为 nil、库表仍只读)。

#include "World/Core/Core.h"
#include "World/Core/Log.h"
#include "World/Script/LuauVm.h"
#include "World/Script/ScriptBindingContext.h"
#include "World/Script/ScriptRef.h"
#include "World/Script/ScriptValue.h"

#include <cmath>
#include <cstdio>
#include <memory>
#include <new>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{
	void Check(bool condition, const char* expression, int line)
	{
		if (!condition)
			throw std::runtime_error(std::string("line ") + std::to_string(line) + ": " + expression);
	}
#define CHECK(expression) Check(static_cast<bool>(expression), #expression, __LINE__)

	bool Contains(const std::string& haystack, const std::string& needle)
	{
		return haystack.find(needle) != std::string::npos;
	}

	// 最小 usertype 样例:纯 POD(没有需要析构的成员)。
	// 注意:T1 的注册接口还没有"每种类型的 C++ 析构"参数,所以夹具刻意不持有 std::string 一类资源 ——
	// 这一点已写进 T1 报告留给 T3。
	struct Vec2
	{
		double X = 0.0;
		double Y = 0.0;
		bool Flag = false;
		char Label[32] = {};
	};

	struct Point
	{
		double X = 0.0;
		double Y = 0.0;
	};

	void ConstructVec2(void* data, double x, double y)
	{
		new (data) Vec2{ x, y };
	}
}

int main()
{
	World::Log::Init();
	try
	{
		World::LuauVm vm;
		std::string error;
		CHECK(vm.Init(&error));
		CHECK(vm.IsInitialized());

		// ------------------------------------------------------------------
		// 1) 每实例 environment:隔离 + __index 回退
		// ------------------------------------------------------------------
		World::ScriptTableRef envA = vm.CreateEnvironment();
		World::ScriptTableRef envB = vm.CreateEnvironment();
		CHECK(envA.IsValid());
		CHECK(envB.IsValid());
		CHECK(envA.RefId() != envB.RefId());

		// 回退:C++ 注册的库(此处是 Luau 自带的 math)在实例 env 里必须可见。
		CHECK(vm.RunStringInEnvironment(R"LUA(
			local floor = math.floor(3.7)
			assert(floor == 3, "math must be visible through environment __index")
		)LUA", "=env_index", envA, &error));

		// 同名全局各自独立:写 A 不影响 B,也不影响线程全局。
		CHECK(vm.RunStringInEnvironment("counter = 1", "=env_a_write", envA, &error));
		CHECK(vm.RunStringInEnvironment("counter = 2", "=env_b_write", envB, &error));
		CHECK(vm.RunStringInEnvironment(
			"assert(counter == 1, 'environment A must keep its own global: ' .. tostring(counter))",
			"=env_a_read", envA, &error));
		CHECK(vm.RunStringInEnvironment(
			"assert(counter == 2, 'environment B must keep its own global: ' .. tostring(counter))",
			"=env_b_read", envB, &error));
		CHECK(vm.GetGlobal("counter").IsNil());

		double counter = 0.0;
		CHECK(envA.GetField("counter").AsNumber(&counter) && counter == 1.0);
		CHECK(envB.GetField("counter").AsNumber(&counter) && counter == 2.0);

		// ------------------------------------------------------------------
		// 2) 宿主全局注入
		// ------------------------------------------------------------------
		CHECK(vm.SetGlobal("host_dt", World::ScriptValue::Number(0.5)));
		CHECK(vm.RunStringInEnvironment(
			"assert(host_dt == 0.5, 'injected global must be visible inside an environment')",
			"=host_env", envA, &error));
		CHECK(vm.RunString("assert(host_dt == 0.5, 'injected global must be visible to hosted chunks')",
			"=host_thread", &error));

		World::ScriptTableRef config = vm.CreateTable();
		CHECK(config.SetField("name", World::ScriptValue::String("world")));
		CHECK(vm.SetGlobal("host_config", config.ToValue()));
		CHECK(vm.RunStringInEnvironment(R"LUA(
			assert(host_config.name == "world", "injected tables must be readable")
		)LUA", "=host_table", envA, &error));

		bool hostBool = false;
		CHECK(vm.GetGlobal("host_dt").AsNumber(&counter) && counter == 0.5);
		CHECK(vm.GetGlobal("host_config").AsTable(&config));

		CHECK(vm.ClearGlobal("host_config"));
		CHECK(vm.ClearGlobal("host_dt"));
		CHECK(vm.GetGlobal("host_dt").IsNil());
		CHECK(vm.RunStringInEnvironment(
			"assert(host_dt == nil and host_config == nil, 'cleared globals must be nil')",
			"=host_clear", envA, &error));
		CHECK(vm.GetGlobal("host_config").AsBool(&hostBool) == false);

		// ------------------------------------------------------------------
		// 3) 受保护调用:参数/返回值 + 错误文本(traceback + 行号)
		// ------------------------------------------------------------------
		// chunk 本身就是 vararg 函数:直接用它验证"参数传进脚本、返回值装箱回 C++"。
		World::ScriptFunctionRef add = vm.CompileFunction(
			"local a, b = ...\nreturn a + b, 'ok'", "=args", envA, &error);
		CHECK(add.IsValid());
		World::ScriptValue args[2] = { World::ScriptValue::Number(2.0), World::ScriptValue::Number(3.0) };
		World::ScriptValue result;
		CHECK(add.Call(args, 2, &result, &error));
		double sum = 0.0;
		CHECK(result.AsNumber(&sum) && sum == 5.0);

		std::vector<World::ScriptValue> results;
		CHECK(add.CallMulti(args, 2, &results, &error));
		CHECK(results.size() == 2);
		std::string tag;
		CHECK(results[0].AsNumber(&sum) && sum == 5.0);
		CHECK(results[1].AsString(&tag) && tag == "ok");

		World::ScriptFunctionRef emptyFunction;
		CHECK(!emptyFunction.Call(nullptr, 0, nullptr, &error));
		CHECK(!emptyFunction.IsValid());

		// 导入解析时机(2026-09-17 实测,留给 T2/T3 的口径):
		//   - 无 env 的函数:线程全局代理的 safeenv=true → Luau 在 **load 期**就把 import 解析进
		//     原型常量表,因此"注入后编译"的函数在 ClearGlobal 之后仍然看得到旧值;
		//   - 每实例 environment:safeenv=false → 每次执行都按 env 现场解析,ClearGlobal 立刻可见。
		// 结论:宿主注入必须发生在编译脚本之前;撤销 API 不能只靠 ClearGlobal。
		{
			CHECK(vm.SetGlobal("probe_value", World::ScriptValue::Number(11.0)));
			World::ScriptFunctionRef probe = vm.CompileFunction("return probe_value", "=probe", World::ScriptTableRef(), &error);
			CHECK(probe.IsValid());
			CHECK(vm.ClearGlobal("probe_value"));

			World::ScriptValue probeResult;
			CHECK(probe.Call(nullptr, 0, &probeResult, &error));
			double probeNumber = -1.0;
			CHECK(probeResult.AsNumber(&probeNumber) && probeNumber == 11.0);

			CHECK(vm.SetGlobal("probe_value", World::ScriptValue::Number(22.0)));
			World::ScriptFunctionRef probeEnv = vm.CompileFunction("return probe_value", "=probe_env", envA, &error);
			CHECK(probeEnv.IsValid());
			CHECK(vm.ClearGlobal("probe_value"));

			World::ScriptValue probeEnvResult;
			CHECK(probeEnv.Call(nullptr, 0, &probeEnvResult, &error));
			CHECK(probeEnvResult.IsNil());
		}

		// 失败调用:message 前缀带 chunk 名与行号,traceback 里有调用点行号。
		World::ScriptFunctionRef failing = vm.CompileFunction(R"LUA(
local function inner()
	error("boom from script")
end
inner()
)LUA", "=call_error", envA, &error);
		CHECK(failing.IsValid());
		std::string callError;
		CHECK(!failing.Call(nullptr, 0, nullptr, &callError));
		CHECK(Contains(callError, "call_error:3:"));     // chunk 名 + 出错行号
		CHECK(Contains(callError, "boom from script"));  // 原始错误文本
		CHECK(Contains(callError, "call_error:5"));      // traceback 里的调用点行号
		CHECK(Contains(callError, "stack traceback"));

		// 运行期错误(不是调用一个函数,而是 chunk 自身失败)也要带 traceback。
		std::string chunkError;
		CHECK(!vm.RunStringInEnvironment("local x = 1\nassert(x == 2, 'chunk failed')", "=chunk_error", envA, &chunkError));
		CHECK(Contains(chunkError, "chunk_error:2:"));
		CHECK(Contains(chunkError, "chunk failed"));
		CHECK(Contains(chunkError, "stack traceback"));

		// ------------------------------------------------------------------
		// 4) 沙箱没有被绑定层破坏
		// ------------------------------------------------------------------
		CHECK(vm.RunStringInEnvironment(R"LUA(
			assert(io == nil and os == nil and debug == nil and require == nil,
				"forbidden globals must stay nil inside an environment")
			assert(pcall(function() math.floor = function() return 0 end end) == false,
				"library tables must stay read-only")
		)LUA", "=sandbox_env", envA, &error));

		// ------------------------------------------------------------------
		// 5) 表操作
		// ------------------------------------------------------------------
		World::ScriptTableRef table = vm.CreateTable();
		CHECK(table.SetField("name", World::ScriptValue::String("root")));
		CHECK(table.HasField("name"));
		std::string name;
		CHECK(table.GetField("name").AsString(&name) && name == "root");
		CHECK(table.GetField("missing").IsNil());

		CHECK(table.SetArrayElement(1, World::ScriptValue::Number(10.0)));
		CHECK(table.SetArrayElement(2, World::ScriptValue::Number(20.0)));
		CHECK(table.SetArrayElement(4, World::ScriptValue::Number(40.0)));
		CHECK(table.Length() == 2);   // 第 3 个是洞 → 连续段只有 2
		std::vector<World::ScriptValue> array = table.GetArray();
		CHECK(array.size() == 2);
		double element = 0.0;
		CHECK(array[0].AsNumber(&element) && element == 10.0);
		CHECK(array[1].AsNumber(&element) && element == 20.0);

		World::ScriptTableRef fallback = vm.CreateTable();
		CHECK(fallback.SetField("inherited", World::ScriptValue::Number(7.0)));
		CHECK(table.SetIndexFallback(fallback));
		CHECK(vm.SetGlobal("host_table", table.ToValue()));
		CHECK(vm.RunStringInEnvironment(R"LUA(
			assert(host_table.name == "root", "field read")
			assert(host_table.inherited == 7, "__index fallback")
		)LUA", "=table_index", envA, &error));

		// 只读表:库表写失败返回 false(而不是把 Lua 错误抛进 C++)
		World::ScriptTableRef mathTable;
		CHECK(vm.GetGlobal("math").AsTable(&mathTable));
		CHECK(mathTable.IsValid());
		CHECK(mathTable.HasField("floor"));
		CHECK(!mathTable.SetField("floor", World::ScriptValue::Nil()));
		CHECK(mathTable.HasField("floor"));

		// ------------------------------------------------------------------
		// 6) usertype 注册
		// ------------------------------------------------------------------
		World::ScriptBindingContext bindings(vm);
		CHECK(bindings.IsValid());

		const World::ScriptMethodBinding vecMethods[] = {
			{ "Length", [&bindings](const World::ScriptValue* callArgs, std::size_t count) -> World::ScriptValue
				{
					if (count < 1)
						throw std::runtime_error("Vec2:Length needs self");
					Vec2* self = nullptr;
					if (!bindings.Unwrap("Vec2", callArgs[0], &self))
						throw std::runtime_error("Vec2:Length self is not a Vec2");
					return World::ScriptValue::Number(std::sqrt(self->X * self->X + self->Y * self->Y));
				} },
			{ "SetLabel", [](const World::ScriptValue* callArgs, std::size_t count) -> World::ScriptValue
				{
					if (count < 2)
						throw std::runtime_error("Vec2:SetLabel needs (self, text)");
					Vec2* self = nullptr;
					if (!callArgs[0].AsUserdata())
						throw std::runtime_error("Vec2:SetLabel self must be userdata");
					self = static_cast<Vec2*>(callArgs[0].AsUserdata());
					std::string text;
					if (!callArgs[1].AsString(&text))
						throw std::runtime_error("Vec2:SetLabel expects a string");
					std::snprintf(self->Label, sizeof(self->Label), "%s", text.c_str());
					return World::ScriptValue::Nil();
				} },
			{ "Describe", [](const World::ScriptValue* callArgs, std::size_t count) -> World::ScriptValue
				{
					if (count < 2)
						throw std::runtime_error("Vec2:Describe needs (self, flag)");
					Vec2* self = static_cast<Vec2*>(callArgs[0].AsUserdata());
					if (!self)
						throw std::runtime_error("Vec2:Describe self must be userdata");
					bool flag = false;
					if (!callArgs[1].AsBool(&flag))
						throw std::runtime_error("Vec2:Describe expects a boolean");
					return World::ScriptValue::String(std::string(self->Label) + (flag ? "+on" : "+off"));
				} },
			{ "Explode", [](const World::ScriptValue* callArgs, std::size_t count) -> World::ScriptValue
				{
					if (count < 1)
						throw std::runtime_error("Vec2:Explode needs self");
					throw std::runtime_error("explode from C++");
				} },
		};
		const World::ScriptMetaMethodBinding vecMeta[] = {
			{ "__index", [&bindings](const World::ScriptValue* callArgs, std::size_t count) -> World::ScriptValue
				{
					Vec2* self = count > 0 ? static_cast<Vec2*>(callArgs[0].AsUserdata()) : nullptr;
					std::string key;
					if (!self || count < 2 || !callArgs[1].AsString(&key))
						throw std::runtime_error("Vec2 __index expects (self, key)");
					if (key == "x")
						return World::ScriptValue::Number(self->X);
					if (key == "y")
						return World::ScriptValue::Number(self->Y);
					if (key == "flag")
						return World::ScriptValue::Boolean(self->Flag);
					if (key == "label")
						return World::ScriptValue::String(std::string(self->Label));
					// 字段 → 方法 回退:方法表同时挂在全局表上,这里把方法值本身返回给 Lua。
					return bindings.MethodsTable("Vec2").GetField(key.c_str());
				} },
			{ "__newindex", [&bindings](const World::ScriptValue* callArgs, std::size_t count) -> World::ScriptValue
				{
					Vec2* self = count > 0 ? static_cast<Vec2*>(callArgs[0].AsUserdata()) : nullptr;
					std::string key;
					if (!self || count < 3 || !callArgs[1].AsString(&key))
						throw std::runtime_error("Vec2 __newindex expects (self, key, value)");
					if (key == "x" || key == "y")
					{
						double number = 0.0;
						if (!callArgs[2].AsNumber(&number))
							throw std::runtime_error("Vec2." + key + " expects a number");
						(key == "x" ? self->X : self->Y) = number;
						return World::ScriptValue::Nil();
					}
					if (key == "flag")
					{
						bool flag = false;
						if (!callArgs[2].AsBool(&flag))
							throw std::runtime_error("Vec2.flag expects a boolean");
						self->Flag = flag;
						return World::ScriptValue::Nil();
					}
					if (key == "label")
					{
						std::string text;
						if (!callArgs[2].AsString(&text))
							throw std::runtime_error("Vec2.label expects a string");
						std::snprintf(self->Label, sizeof(self->Label), "%s", text.c_str());
						return World::ScriptValue::Nil();
					}
					throw std::runtime_error("Vec2 has no writable member '" + key + "'");
				} },
			{ "__add", [&bindings](const World::ScriptValue* callArgs, std::size_t count) -> World::ScriptValue
				{
					if (count < 2)
						throw std::runtime_error("Vec2 __add expects two operands");
					Vec2* left = static_cast<Vec2*>(callArgs[0].AsUserdata());
					Vec2* right = static_cast<Vec2*>(callArgs[1].AsUserdata());
					if (!left || !right)
						throw std::runtime_error("Vec2 __add only supports Vec2 + Vec2");
					World::ScriptValue sum_ = bindings.NewUserdata("Vec2");
					Vec2* target = static_cast<Vec2*>(sum_.AsUserdata());
					if (!target)
						throw std::runtime_error("Vec2 __add could not allocate a result");
					new (target) Vec2{ left->X + right->X, left->Y + right->Y };
					return sum_;
				} },
			{ "__tostring", [](const World::ScriptValue* callArgs, std::size_t count) -> World::ScriptValue
				{
					Vec2* self = count > 0 ? static_cast<Vec2*>(callArgs[0].AsUserdata()) : nullptr;
					if (!self)
						throw std::runtime_error("Vec2 __tostring expects self");
					char buffer[64];
					std::snprintf(buffer, sizeof(buffer), "Vec2(%g, %g)", self->X, self->Y);
					return World::ScriptValue::String(buffer);
				} },
		};

		World::ScriptUserTypeDesc vecDesc;
		vecDesc.Name = "Vec2";
		vecDesc.UserdataSize = sizeof(Vec2);
		vecDesc.Constructor = [](void* data, const World::ScriptValue* callArgs, std::size_t count)
			{
				double x = 0.0;
				double y = 0.0;
				if (count > 0 && !callArgs[0].AsNumber(&x))
					throw std::runtime_error("Vec2.new expects a number for x");
				if (count > 1 && !callArgs[1].AsNumber(&y))
					throw std::runtime_error("Vec2.new expects a number for y");
				ConstructVec2(data, x, y);
			};
		vecDesc.Methods = vecMethods;
		vecDesc.MethodCount = sizeof(vecMethods) / sizeof(vecMethods[0]);
		vecDesc.MetaMethods = vecMeta;
		vecDesc.MetaMethodCount = sizeof(vecMeta) / sizeof(vecMeta[0]);
		CHECK(bindings.RegisterUserType(vecDesc, &error));

		// 没有显式 __index 的类型:默认回退到方法表。
		const World::ScriptMethodBinding pointMethods[] = {
			{ "Sum", [](const World::ScriptValue* callArgs, std::size_t count) -> World::ScriptValue
				{
					Point* self = count > 0 ? static_cast<Point*>(callArgs[0].AsUserdata()) : nullptr;
					if (!self)
						throw std::runtime_error("Point:Sum needs self");
					return World::ScriptValue::Number(self->X + self->Y);
				} },
		};
		World::ScriptUserTypeDesc pointDesc;
		pointDesc.Name = "Point";
		pointDesc.UserdataSize = sizeof(Point);
		pointDesc.Constructor = [](void* data, const World::ScriptValue* callArgs, std::size_t count)
			{
				double x = 0.0;
				double y = 0.0;
				if (count > 0 && !callArgs[0].AsNumber(&x))
					throw std::runtime_error("Point.new expects a number for x");
				if (count > 1 && !callArgs[1].AsNumber(&y))
					throw std::runtime_error("Point.new expects a number for y");
				new (data) Point{ x, y };
			};
		pointDesc.Methods = pointMethods;
		pointDesc.MethodCount = sizeof(pointMethods) / sizeof(pointMethods[0]);
		CHECK(bindings.RegisterUserType(pointDesc, &error));

		CHECK(bindings.IsUserTypeRegistered("Vec2"));
		CHECK(bindings.UserTypeSize("Vec2") == sizeof(Vec2));
		CHECK(bindings.RegisteredUserTypes().size() == 2);
		CHECK(bindings.MethodsTable("Vec2").IsValid());
		CHECK(!bindings.IsUserTypeRegistered("Nope"));

		// 重复注册 / 占用全局名必须失败,不能静默盖掉已有 API。
		{
			std::string duplicateError;
			CHECK(!bindings.RegisterUserType(vecDesc, &duplicateError));
			CHECK(Contains(duplicateError, "already registered"));
			World::ScriptUserTypeDesc clash = pointDesc;
			clash.Name = "math";
			std::string clashError;
			CHECK(!bindings.RegisterUserType(clash, &clashError));
			CHECK(Contains(clashError, "already in use"));
		}

		// C++ 侧装箱/解包。
		{
			World::ScriptValue value = bindings.NewUserdata("Vec2");
			CHECK(value.IsUserdata());
			Vec2* pointer = nullptr;
			CHECK(bindings.Unwrap("Vec2", value, &pointer));
			CHECK(pointer != nullptr);
			CHECK(bindings.UserdataPointer("Vec2", value) == pointer);
			Point* wrong = nullptr;
			CHECK(!bindings.Unwrap("Point", value, &wrong));
			CHECK(wrong == nullptr);
			CHECK(!bindings.IsUserdataOfType("Point", value));
			CHECK(bindings.IsUserdataOfType("Vec2", value));
			CHECK(bindings.UserdataPointer("Vec2", World::ScriptValue::Number(1.0)) == nullptr);
		}

		// 脚本侧:构造 + 方法 + 运算符 + 字段读写 + 异常可捕。
		CHECK(vm.RunStringInEnvironment(R"LUA(
			local a = Vec2.new(3, 4)
			assert(a:Length() == 5, "method call with self")
			assert(Vec2.Length(a) == 5, "methods are also reachable through the type table")

			a.x = 1.5           -- __newindex
			a.label = "hello"   -- __newindex + string 解包
			a.flag = true       -- __newindex + bool 解包
			assert(a.x == 1.5, "__index must read the C++ field")
			assert(a.label == "hello", "__index must read the C++ label")
			assert(a.flag == true, "__index must read the C++ bool")
			assert(a:Describe(true) == "hello+on", "method argument unpacking")

			a:SetLabel("renamed")
			assert(a.label == "renamed", "string argument must reach C++")

			local b = Vec2.new(1, 2)
			local c = a + b     -- __add
			assert(c.x == 2.5 and c.y == 6, "__add must call the registered C++ operator")
			assert(tostring(Vec2.new(1, 1)) == "Vec2(1, 1)", "__tostring must be honoured")

			local ok, message = pcall(function() Vec2.new(0, 0):Explode() end)
			assert(not ok, "C++ exception must surface as a Lua error")
			assert(string.find(message, "explode from C++", 1, true) ~= nil,
				"error text must keep the C++ exception message: " .. tostring(message))
			assert(string.find(message, "usertype", 1, true) ~= nil,
				"error text must keep the script location: " .. tostring(message))

			local okWrite = pcall(function() Vec2.new(0, 0).x = "not a number" end)
			assert(not okWrite, "__newindex type errors must be raised")

			local p = Point.new(2, 3)
			assert(p:Sum() == 5, "default __index must expose the method table")
			assert(Point.Sum(p) == 5, "methods are also reachable through the global type table")
		)LUA", "=usertype", envA, &error));

		// 类型名进 Lua 的类型错误文本(__type 元字段)。
		CHECK(vm.RunStringInEnvironment(R"LUA(
			local ok, message = pcall(function() return Point.new(1, 1):Sum(1) end)
			assert(ok, "extra arguments must be ignored by the C++ method")
		)LUA", "=usertype_extra", envA, &error));

		// ------------------------------------------------------------------
		// 7) Shutdown 后引用/值必须失效且不碰已关闭的 registry
		// ------------------------------------------------------------------
		World::ScriptValue tableValue = table.ToValue();
		World::ScriptValue configValue = config.ToValue();
		World::ScriptFunctionRef functionCopy = failing;
		CHECK(table.IsValid());
		CHECK(tableValue.IsTable());
		CHECK(functionCopy.IsValid());

		vm.Shutdown();
		CHECK(!vm.IsInitialized());
		CHECK(vm.State() == nullptr);
		CHECK(!table.IsValid());
		CHECK(!envA.IsValid());
		CHECK(!functionCopy.IsValid());
		CHECK(!failing.IsValid());
		CHECK(!add.IsValid());
		CHECK(table.RefId() == -1);
		CHECK(table.State() == nullptr);
		// 类型标签是"装箱时的事实":引用失效后仍然报告它曾经是什么,是否可用只看 IsValid()。
		CHECK(table.Type() == World::ScriptValueType::Table);
		CHECK(table.GetField("name").IsNil());
		CHECK(table.Length() == 0);
		CHECK(table.GetArray().empty());
		CHECK(!table.SetField("name", World::ScriptValue::String("x")));
		CHECK(!table.SetArrayElement(1, World::ScriptValue::Number(0.0)));
		CHECK(!table.HasField("name"));
		CHECK(!functionCopy.Call(nullptr, 0, nullptr, &error));
		CHECK(!functionCopy.CallMulti(nullptr, 0, &results, &error));
		CHECK(!vm.RunString("return 1", "=after_shutdown", &error));
		CHECK(!vm.SetGlobal("after", World::ScriptValue::Number(1.0)));
		CHECK(vm.GetGlobal("math").IsNil());
		CHECK(envA.GetField("counter").IsNil());

		World::ScriptTableRef invalidTable;
		CHECK(!tableValue.AsTable(&invalidTable));
		CHECK(!invalidTable.IsValid());
		CHECK(tableValue.AsUserdata() == nullptr);
		CHECK(tableValue.UserdataTag() == -1);
		CHECK(configValue.IsTable());   // 类型标签是装箱时的事实

		table.Release();
		table.Release();                // 幂等
		CHECK(!table.IsValid());
		functionCopy.Release();
		CHECK(!functionCopy.IsValid());

		// 引用活得比 VM 对象更久(VM 已销毁)也必须安全。
		{
			std::unique_ptr<World::LuauVm> temporary = std::make_unique<World::LuauVm>();
			CHECK(temporary->Init(&error));
			World::ScriptTableRef foreign = temporary->CreateTable();
			World::ScriptValue foreignValue = foreign.ToValue();
			CHECK(foreign.IsValid());
			temporary->Shutdown();
			temporary.reset();          // VM 对象消失,引用仍在
			CHECK(!foreign.IsValid());
			CHECK(!foreignValue.AsTable(&invalidTable));
			foreign.Release();
			CHECK(foreign.RefId() == -1);
		}

		vm.Shutdown();                  // 重复 Shutdown 是空操作
		CHECK(!vm.IsInitialized());

		std::printf("World.LuauBinding: all checks passed\n");
		return 0;
	}
	catch (const std::exception& exception)
	{
		std::fprintf(stderr, "World.LuauBinding: FAILED: %s\n", exception.what());
		return 1;
	}
}
