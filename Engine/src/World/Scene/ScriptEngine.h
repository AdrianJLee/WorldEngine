#pragma once
#include "World/Core/Export.h"
#include "World/Script/Sandbox.h"
#include "Scene.h"
#include <algorithm>
#include <cstddef>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace World
{
	class LuauVm;
	class ScriptBindingContext;
	class BehaviorRegistry;
	class WorldContext;

	namespace Schema { class SchemaRegistry; }
	// W3c:宿主 UI 阶段入口只需要 WuiContext 的引用,避免在此处引入 WUI 重头。
	namespace Wui { class WuiContext; }

	// 描述数学类的一个属性（变量或函数）
	struct LuaPropDesc
	{
		std::string Name;
		std::string LuaType;
		std::string Description; // 注释信息
	};

	struct LuaFunctionDesc
	{
		std::string Name;
		std::vector<LuaPropDesc> Parameters;
		std::string ReturnType;
		std::string Description;
		bool IsMethod = true;
	};

	struct LuaOperatorDesc
	{
		std::string Name;
		std::string OperandType;
		std::string ReturnType;
	};

	// Metadata describes only APIs actually bound into the Lua VM.
	struct LuaTypeReflection
	{
		std::string ClassName;
		std::vector<LuaPropDesc> Properties;  // 属性列表
		// 核心：把该类型注册进当前 VM 的绑定上下文（W1b 起不再依赖 sol2）。
		std::function<void(ScriptBindingContext&)> BindFunc;
		std::vector<LuaFunctionDesc> Methods;
		std::vector<LuaFunctionDesc> Constructors;
		std::vector<LuaOperatorDesc> Operators;
	};

	class LuaReflectionRegistry
	{
	public:
		static std::vector<LuaTypeReflection>& GetTable();
		static bool Register(const LuaTypeReflection& desc)
		{
			auto& table = GetTable();
			if (std::any_of(table.begin(), table.end(), [&](const auto& item) { return item.ClassName == desc.ClassName; }))
				return false;
			table.push_back(desc);
			return true;
		}
	};

	// Lua类型注册器
	struct LuaTypeRegistrar
	{
		LuaTypeRegistrar(const LuaTypeReflection& desc)
		{
			LuaReflectionRegistry::Register(desc);
		}
	};

	// Explicit references also retain the binding objects in static-library hosts.
	void RegisterBuiltinEntityLuaType();
	void RegisterBuiltinMat3LuaType();
	void RegisterBuiltinMat4LuaType();
	// vec2/vec3/vec4 的绑定实现位于 LuaType/Vec2.cpp、Vec3.cpp、Vec4.cpp。
	void RegisterBuiltinVec2Binding(ScriptBindingContext& bindings);
	void RegisterBuiltinVec3Binding(ScriptBindingContext& bindings);
	void RegisterBuiltinVec4Binding(ScriptBindingContext& bindings);
	// W3a-A1:组件字段代理(Entity:GetComponent 的返回值;Kind → Lua 映射表见 Script/BindComponentAccess.h)。
	WLD_API bool RegisterComponentProxyBinding(ScriptBindingContext& bindings, std::string* error);

	struct LuauScriptComponent;
	class ScriptEngine
	{
	public:
		static void Init();      // 在 Application 启动时调用：建立 Luau VM、沙箱、绑定层与 API 注册
		// P2 W7-3:登记"内容上下文" —— 脚本字节读取(ReadScriptBytes)优先用它做 VFS 查询;
		// 未登记时回退 Application::HasInstance() 的既有行为,两者都没命中再回退磁盘
		// (WLD_ASSETPATH/<逻辑路径>)。可重复调用(覆盖登记),宿主不需要反登记;
		// 进程内登记随 Shutdown() 失效(避免宿主销毁 WorldContext 后留下悬垂指针)。
		// U3:无源码树 headless(只挂包 provider)靠它把包内容接进 ScriptEngine。
		static void Init(World::WorldContext& context);
		static void Shutdown();  // 在 Application 关闭时调用
		static bool IsInitialized();
		static void AssertOwnerThread();

		static void DefineMathType();
		static void RegisterMathTypes();

		static bool GenerateLuaStubs();
		// W3a-A2(追加):带显式 schema 注册表的存根生成;组件块来自
		// schemas.List(TypeCategory::Component)。零参调用在宿主上下文中委托到这里。
		static bool GenerateLuaStubs(const Schema::SchemaRegistry& schemas);
		// 编辑态预览(2026-09-26 重写):执行脚本取默认值表 → 同步组件的属性表(声明顺序/类型,
		// 同名同类型保留场景里已存的值),**不创建实例、不调 OnCreate、不保留环境引用**。
		static bool InitScriptForEditor(LuauScriptComponent& component);
		// 从 Lua 脚本源码文本静态解析 `---@field Name Type` 注解，返回字段名到 Lua 类型名的映射（不执行脚本）。
		static std::unordered_map<std::string, std::string> ParseFieldAnnotations(const std::string& scriptText);
		// 核心：处理单个实体的脚本实例化和每帧更新
		static void OnCreateScript(LuauScriptComponent& scriptComponent, Entity entity);
		static void OnUpdateScript(LuauScriptComponent& scriptComponent, Timestep ts);
		static void OnDestroyScript(LuauScriptComponent& scriptComponent);

		// ---- P2 W3c:脚本 UI 的宿主入口(UI 阶段每帧一次) ----
		// 遍历场景里"已运行且带 OnUI 回调"的 LuauScriptComponent,逐个建立当前 UI 上下文
		// (ui.* 读取的 WuiContext + 脚本逻辑路径前缀)并调用 OnUI(self)。
		// 单个脚本回调出错只把该实例置 Faulted(ReportLuaError),其它实例继续绘制;
		// 返回本帧出错的脚本数(0 = 全部成功)。
		static std::size_t DrawScriptUi(Scene& scene, Wui::WuiContext& context);

		// ---- P2 W4:事件/计时器的实例收口 ----
		// 事件/计时器回调(由 Script/BindEvents 的桥层驱动)失败时把该实例置 Faulted,
		// 与 OnUpdate 失败同一落点(State=Faulted + LastError);generation 不匹配
		// (已热重载/已重建)或实例无效时静默忽略。
		static void FaultScriptInstance(Entity entity, uint64_t generation, const char* phase,
			const std::string& error);

		// 方便获取全局状态（W1b 起返回 Luau VM 门面；未初始化时抛 logic_error）
		static LuauVm& GetState();
		// 当前 VM 的绑定上下文（类型注册/宿主函数装箱；未初始化时抛 logic_error）
		static ScriptBindingContext& GetBindingContext();

		// ---- P2 W2a:行为注册层（只登记/查询，不参与调度）----
		// 进程内行为注册表：与 VM 生命周期无关（Init/Shutdown 不清空），宿主与测试共用。
		static BehaviorRegistry& Behaviors();
		// 幂等登记/刷新一个 Luau 行为（字段来自组件的属性表；路径为空 → false + error）。
		// 同模块 id 描述一致 → 直接返回 true；描述变化（脚本编辑/热重载）→ Replace 刷新。
		static bool EnsureLuaBehavior(LuauScriptComponent& script, std::string* error = nullptr);
		// 幂等登记/刷新 schema 注册表里全部 Category==Script 的 C++ 行为；
		// 返回已登记/已确认的模块数，失败项写入 errors（可为 null）。
		static std::size_t EnsureSchemaBehaviors(const Schema::SchemaRegistry& schemas,
			std::vector<std::string>* errors = nullptr);

		// ---- P2 W5:L2 脚本热重载(引擎侧)----
		// 重新加载组件当前脚本并**整体交换**实例引用(环境/脚本表/四个回调)与属性表,
		// 属性按"活表 > 场景保存值(同名+同类型) > 新脚本默认值"迁移(见 Script/HotReload.h),
		// 成功后 Runtime.Generation 进入热重载域并刷新
		// BehaviorRegistry 描述;任一步失败都保留旧版本,只写 ReloadDiagnostic。
		//   - 成功:ReloadDiagnostic 清空;diagnostics(可空)收迁移警告(类型变化/字段删除);
		//   - 失败:ReloadDiagnostic 与 diagnostics 都是可读诊断(含脚本路径,编译器给了行号时
		//     行号原样保留);State 不会被置 Faulted,旧引用不会被清;
		//   - 拒绝:Runtime.State ∈ {Creating, Destroying}、没有活动实例(Runtime.State != Running
		//     或无脚本表/环境引用)、
		//     或 RuntimeEntity 所属场景不在安全点(Scene::CanApplyScriptReload()==false)。
		// 宿主应在帧边界调用;引擎在能取到场景时会再校验一次安全点。
		static bool ReloadScript(LuauScriptComponent& component, std::string* diagnostics = nullptr);

		// ---- P2 W6:沙箱预算旋钮 ----
		// 进程内默认策略(Init 时下发到 VM;不随 Shutdown 复位)。0 = 该维度不限;
		// 引擎默认 Instructions = 1'000'000、TimeMs = 0(指令口径,确定性优先)。
		// 已开始执行的受保护调用不受影响:策略在作用域开始时固定。
		static void SetSandboxPolicy(const Sandbox::Policy& policy);
		static Sandbox::Policy GetSandboxPolicy();
	};
}
