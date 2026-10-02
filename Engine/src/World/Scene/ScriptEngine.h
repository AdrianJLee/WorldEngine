#pragma once
#include "World/Core/Export.h"
#include "World/Script/Sandbox.h"
#include "World/Script/ScriptProperties.h"
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

		// Pure ECS: 全局活动场景上下文（供 ecs/world 脚本绑定访问当前场景）
		static void SetActiveScene(Scene* scene);
		static Scene* GetActiveScene();

		// Pure ECS: 项目系统脚本(scripts/systems/*.luau|*.lua)——
		// 脚本内用 ecs:AddSystem 把系统注册进当前场景的帧管线。
		// 加载 = 整份重跑:同名系统先撤销再注册,因此**重复调用是幂等的**。
		static std::size_t LoadSystemScripts(Scene& scene, const std::filesystem::path& systemsDir);
		static std::size_t LoadSystemScripts(Scene& scene);
		// 撤销全部系统脚本注册过的系统(场景停止/换场景用)。返回撤销的系统数。
		static std::size_t UnloadSystemScripts(Scene& scene);
		// 单份系统脚本热重载:撤销该文件上次注册的系统 → 重读 → 整份重跑。
		static std::size_t ReloadSystemScript(Scene& scene, const std::string& logicalPath);
		// 宿主每帧驱动:轮询系统脚本目录,把稳定变化过的文件逐份热重载。返回处理过的文件数。
		static std::size_t PollSystemScriptReload(Scene& scene, double deltaSeconds);
		// ecs:AddSystem 成功后由绑定层调用:把系统名归属到"当前正在执行的系统脚本"名下。
		// 不在加载系统脚本时是 no-op(宿主手写 ecs:AddSystem 的系统不参与脚本热重载)。
		static void NoteScriptSystem(const std::string& systemName);

		// UI 阶段兼容占位 (纯 ECS 脚本通过 WUI / System 渲染)
		static std::size_t DrawScriptUi(Scene&, Wui::WuiContext&) { return 0; }

		static void DefineMathType();
		static void RegisterMathTypes();

		static bool GenerateLuaStubs();
		// W3a-A2(追加):带显式 schema 注册表的存根生成;组件块来自
		// schemas.List(TypeCategory::Component)。零参调用在宿主上下文中委托到这里。
		static bool GenerateLuaStubs(const Schema::SchemaRegistry& schemas);
		// 从 Lua 脚本源码文本静态解析 `---@field Name Type` 注解，返回字段名到 Lua 类型名的映射（不执行脚本）。
		static std::unordered_map<std::string, std::string> ParseFieldAnnotations(const std::string& scriptText);

		// ---- V1(2026-09-26 用户反馈):脚本声明的**唯一入口** ----
		// 编辑器/检视器只许调这里,不许再自己扫注解(声明顺序 / 类型映射 / 诊断只有这一份)。
		//
		// 按脚本路径取有序声明(name → Schema::Kind → Doc → 默认值):
		//   * 顺序 = 注解顺序(先声明先显示)→ 脚本表里其余字段的顺序;
		//   * Doc = 注解第三段的整段剩余文本(`---@field Speed number 移动速度` → "移动速度"),
		//     没有第三段则空;
		//   * 默认值 = 脚本模块自己的默认值:在沙箱 VM 里只 **load 模块 + 读返回的表**
		//     (`local PlayerScript = { Speed = 5.0 }`),**不调 OnCreate/OnUpdate、不建实例、
		//     不注册行为**;VM 不可用 / 容器字节 / 字段不在表里 → Default 为 monostate(未设);
		//   * 注解与脚本表值类型不符、未知注解类型 → 跳过该字段 + 一条诊断(沿用旧口径)。
		// 结果按(路径 + 内容指纹 + VM 是否可用)做单槽缓存 → 检视器可以每次绘制都调。
		// 路径为空 / 读不到脚本 → false + error;此时 out 清空。


		// 方便获取全局状态（W1b 起返回 Luau VM 门面；未初始化时抛 logic_error）
		static LuauVm& GetState();
		// 当前 VM 的绑定上下文（类型注册/宿主函数装箱；未初始化时抛 logic_error）
		static ScriptBindingContext& GetBindingContext();

		// ---- P2 W2a:行为注册层（只登记/查询，不参与调度）----
		// 进程内行为注册表：与 VM 生命周期无关（Init/Shutdown 不清空），宿主与测试共用。
		static BehaviorRegistry& Behaviors();
		// 幂等登记/刷新一个 Luau 行为（字段来自组件的属性表；路径为空 → false + error）。
		// 同模块 id 描述一致 → 直接返回 true；描述变化（脚本编辑/热重载）→ Replace 刷新。
		// 幂等登记/刷新 schema 注册表里全部 Category==Script 的 C++ 行为；
		// 返回已登记/已确认的模块数，失败项写入 errors（可为 null）。
		static std::size_t EnsureSchemaBehaviors(const Schema::SchemaRegistry& schemas,
			std::vector<std::string>* errors = nullptr);

		// ---- P2 W6:沙箱预算旋钮 ----
		// 进程内默认策略(Init 时下发到 VM;不随 Shutdown 复位)。0 = 该维度不限;
		// 引擎默认 Instructions = 1'000'000、TimeMs = 0(指令口径,确定性优先)。
		// 已开始执行的受保护调用不受影响:策略在作用域开始时固定。
		static void SetSandboxPolicy(const Sandbox::Policy& policy);
		static Sandbox::Policy GetSandboxPolicy();
	};
}
