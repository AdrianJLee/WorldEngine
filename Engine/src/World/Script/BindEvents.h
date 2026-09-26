#pragma once

#include "World/Core/Export.h"
#include "World/Scene/Entity.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace World
{
	class ScriptBindingContext;
	struct ScriptServiceBinding;

	// P2 W4:事件 / 计时器的脚本绑定(只读全局表 `events` / `timers`)。
	//
	// 冻结口径(plan §11 W4 接口冻结草案 + 用户 2026-09-17 拍板):
	//   - `events:on(name, fn) -> handle`、`events:off(handle) -> boolean`、
	//     `events:emit(name, ...) -> boolean`;emit 是**队列式**,由 GameApp::Tick 帧末统一投递;
	//   - `timers:after(sec, fn) -> handle`、`timers:every(sec, fn[, count]) -> handle`、
	//     `timers:cancel(handle) -> boolean`;计时器只跟随固定步长推进(确定性);
	//   - 事件名必须在启动期用 RegisterScriptEventName 注册;on/emit 遇未注册名给可读 Lua error;
	//   - 订阅归属"当前脚本实例"(四个生命周期回调周围 push/pop 的 owner):
	//     实例销毁 / 场景停止 / 热重载成功时批量退订(热重载失败保留旧订阅);
	//   - 事件/计时器回调在带 owner 的 Scene 回调作用域内执行:白名单同步结构写
	//     (CreateEntityShell / AddComponent / SetParent)在回调里可用且当帧可见;
	//   - 逐 handler 受保护调用:失败只把该实例置 Faulted 并丢弃它的全部订阅,其余订阅者继续。

	// 事件参数的单值盒装(VM 无关;emit 与派发两侧共用同一份序列化)。
	enum class ScriptEventValueKind : std::uint8_t
	{
		Number = 0,
		Boolean = 1,
		String = 2,
		Entity = 3,
	};

	struct ScriptEventValue
	{
		ScriptEventValueKind Kind = ScriptEventValueKind::Number;
		double Number = 0.0;
		bool Boolean = false;
		std::string String;
		// 原始 entt 句柄(含版本位);取出时由桥层用订阅者的场景重包成带 generation 校验的 Entity。
		std::uint32_t EntityHandle = 0;

		static ScriptEventValue MakeNumber(double value);
		static ScriptEventValue MakeBoolean(bool value);
		static ScriptEventValue MakeString(std::string value);
		static ScriptEventValue MakeEntity(std::uint32_t handle);
	};

	// ---- 事件名目录(启动期注册) ----
	// argSignature = 逗号分隔的类型名:number | integer | boolean | string | Entity;空串 = 无参数。
	// 事件类型 id = Fnv1a32("script-event:" + name),与 C++ EventBus::TypeIdOf<T>() 的 entt 域区分。
	// 名字为空/重复/签名非法 → false + error(可空)。目录是进程级的,注册后不提供注销。
	WLD_API bool RegisterScriptEventName(const char* name, const char* argSignature,
		std::string* error = nullptr);
	WLD_API bool IsScriptEventNameRegistered(const char* name);
	WLD_API std::size_t GetScriptEventNameCount();
	// 未注册名 → 0(0 保留为"无事件")。
	WLD_API std::uint32_t ScriptEventTypeId(const char* name);

	// 载荷盒装(桥层与测试/宿主共用):
	// [uint32 条数][逐条: uint8 kind + 载荷](String = uint32 长度 + 字节,Entity = uint32 句柄)。
	// 上限:32 个值 / 8 KiB 载荷;超限 → false + error,不静默截断。
	WLD_API bool SerializeScriptEventPayload(const std::vector<ScriptEventValue>& values,
		std::vector<std::uint8_t>* out, std::string* error = nullptr);
	WLD_API bool DeserializeScriptEventPayload(const std::vector<std::uint8_t>& payload,
		std::vector<ScriptEventValue>* out, std::string* error = nullptr);

	// ---- 绑定注册 ----
	// 运行时注册 events/timers 两个只读全局表(重名/非法描述 → false + error)。
	WLD_API bool RegisterEventBindings(ScriptBindingContext& bindings, std::string* error = nullptr);
	// 存根渲染的唯一描述表(与 BindServices / ScriptUiBindings 同一条链路)。
	WLD_API const ScriptServiceBinding* ScriptEventBindings(std::size_t* count);

	// ---- 当前脚本实例(owner)作用域 ----
	// ScriptEngine 在 OnCreate/OnUpdate/OnDestroy/OnUI 周围 push/pop;
	// events:on 与 timers:after/every 借它把订阅归属到具体实例。
	struct ScriptEventOwner
	{
		Scene* ScenePtr = nullptr;
		Entity EntityRef;                 // 含场景令牌 + entt 版本位
		std::uint64_t Component = 0;      // LuauScriptComponent 的 entt type hash
		std::uint64_t Generation = 0;     // 实例 generation(热重载会进入另一个域)
	};

	WLD_API void PushScriptEventOwner(const ScriptEventOwner& owner);
	WLD_API void PopScriptEventOwner();
	WLD_API bool TryGetCurrentScriptEventOwner(ScriptEventOwner* out);

	class ScriptEventOwnerScope
	{
	public:
		explicit ScriptEventOwnerScope(const ScriptEventOwner& owner) { PushScriptEventOwner(owner); }
		~ScriptEventOwnerScope() { PopScriptEventOwner(); }

		ScriptEventOwnerScope(const ScriptEventOwnerScope&) = delete;
		ScriptEventOwnerScope& operator=(const ScriptEventOwnerScope&) = delete;
		ScriptEventOwnerScope(ScriptEventOwnerScope&&) = delete;
		ScriptEventOwnerScope& operator=(ScriptEventOwnerScope&&) = delete;
	};

	// ---- 生命周期收口(Scene / ScriptEngine 调用) ----
	// 退订某个实例(generation 精确匹配)的全部事件/计时器订阅;返回退订条数。
	WLD_API std::size_t ReleaseScriptEventOwners(const Entity& owner, std::uint64_t generation);
	// VM 关闭/重建:丢弃全部订阅与总线绑定(事件名目录保留)。
	WLD_API void ResetScriptEventSubscriptions();
}
