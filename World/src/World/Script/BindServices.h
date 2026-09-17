#pragma once

#include "World/Core/Export.h"
#include "World/Script/ScriptBindingContext.h"

#include <cstddef>
#include <cstdint>
#include <string>

namespace World
{
	class ScriptTableRef;

	// P2 W3b:游戏服务面的脚本绑定(Input / Level / Save)。
	//
	// 设计要点:
	//   - 三个**只读全局表**(与既有 Entity/vec3 惯例一致);表本身不可脚本构造、不可改写;
	//   - 方法体每次调用懒查 Gameplay::GameApp::TryGet(),不缓存会话指针
	//     (ScriptEngine 先于 GameApp 创建;headless 夹具也可能只有一方);
	//   - 只暴露查询/请求类 API:喂入类(SetKeyState/EndFrame/BuildSnapshot/SetMap)、
	//     宿主回调(Pump/SetSceneLoader/SetProgressCallback)与路径类(GetSlotPath/GetSaveRoot)
	//     一律不进脚本;
	//   - 失败语义:业务失败 = 返回 false/0/空 + 单独调用 LastError();
	//     无 GameApp 会话/无 SaveService 等调用前置条件不满足 → 可读 Lua error;
	//   - 参数校验(个数/类型)在调用处即报可读错误,不静默吞掉。

	// 方法期望的参数类型位(可组合:number|boolean)。
	enum class ScriptServiceArgType : uint32_t
	{
		None = 0,
		Boolean = 1u << 0,
		Number = 1u << 1,
		String = 1u << 2,
		Table = 1u << 3,
	};

	inline constexpr ScriptServiceArgType operator|(ScriptServiceArgType left, ScriptServiceArgType right)
	{
		return static_cast<ScriptServiceArgType>(static_cast<uint32_t>(left) | static_cast<uint32_t>(right));
	}

	// 位置参数描述:名字/类型(存根) + 是否必填 + 说明。
	struct ScriptServiceParam
	{
		const char* Name = nullptr;
		const char* LuaType = nullptr;               // 存根注解里的类型名(必须是已知类型/联合)
		ScriptServiceArgType Accepted = ScriptServiceArgType::None;
		bool Required = false;
		const char* Description = nullptr;
	};

	// 一个方法重载:签名(存根) + 实现 + 失败语义说明。
	struct ScriptServiceMethod
	{
		const char* Name = nullptr;
		ScriptNativeFunction Function;
		const ScriptServiceParam* Params = nullptr;
		std::size_t ParamCount = 0;
		std::size_t ExpectedArgs = 0;                // 除重载外,运行时实际读取的参数个数
		const char* ReturnType = nullptr;            // 空字符串 = 无返回值
		const char* Description = nullptr;
	};

	// 一个只读全局服务表(Input/Level/Save)。
	struct ScriptServiceBinding
	{
		const char* Name = nullptr;
		const char* Description = nullptr;
		const ScriptServiceMethod* Methods = nullptr;
		std::size_t MethodCount = 0;
	};

	// 运行时注册 Input/Level/Save 三个全局表。注册失败(重复注册/名字被占用)返回 false + error。
	WLD_API bool RegisterGameplayServiceBindings(ScriptBindingContext& bindings, std::string* error = nullptr);

	// 服务面 API 的唯一描述表:运行时注册与 LuaStubGenerator 的存根渲染共用同一份,
	// 避免"脚本里能调的方法"和"存根里宣告的方法"两处漂移。
	WLD_API const ScriptServiceBinding* GameplayServiceBindings(std::size_t* count);
}
