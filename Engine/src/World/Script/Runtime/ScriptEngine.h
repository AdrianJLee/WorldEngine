#pragma once
#include "World/Core/Export.h"
#include "World/Script/Vm/Sandbox.h"
#include "World/Script/Runtime/ComponentPropertyModel.h"
#include "World/Scene/Scene.h"
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
	class ScriptValue;
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

		// T13:受限库加载通道(ecs:RequireLib 的引擎侧入口)。
		// 名字 = 相对 <内容根>/scripts/lib/ 的逻辑路径(可含斜杠分段;不带扩展名,
		// 按 .luau 优先、.lua 回退,与 IsSystemScriptFile 同口径);只在该子树内解析 ——
		// 绝对路径/盘符/'.'/'..' 段/反斜杠/其它扩展名一律可读错误(复用 Vfs::Normalize)。
		// 模块语义:同一路径只执行一次(命中缓存返回同一值);失败不入缓存;
		// 命中"正在加载"集合 ⇒ 循环依赖可读错误(不递归、不栈溢出);
		// 库在与系统脚本**同一份沙箱全局**下执行(io/os/require/load 等仍为 nil)。
		// 库源内容变化后,下一次调用重新执行(内容哈希,与系统脚本热重载同一口径)。
		// out = 库 `return` 的值;失败返回 false 并把可读原因写进 error。
		static bool RequireScriptLib(const std::string& name, ScriptValue& out, std::string* error = nullptr);

		// ---- M38:UI 绑定只读值查询(仅追加;不改任何既有入口语义)----
		// `script:<path>.<field>` 的求值端:按逻辑路径 load 脚本模块,读它 `return` 的表里的字段,
		// 转成**属性文本协议**(与 `UiBindingSources.cpp` 的 FloatToText/值编码一致:
		// bool "true"/"false"、数值整数不带小数点/其余 6 位有效数字、字符串原样)。
		// **路径口径与 ecs 脚本一致:扩展名可省** —— 先试 `.luau`、再试 `.lua`;已带 `.luau`/`.lua`
		// = 按给定路径。省略与带扩展名两种写法命中同一份模块缓存(缓存键 = 解析后的逻辑路径)。
		// 与 `ecs:RequireLib` 同一沙箱、同一"load 模块 + 读返回表"口径;同一路径按内容指纹缓存
		// 返回表(脚本内容变 ⇒ 重新执行),字段每次从缓存表重读(脚本对表的改动可见)。
		// **只读**:不执行 OnCreate/OnUpdate、不建实例、不注册系统、不写任何脚本状态。
		// 失败一律 false + 可读 error(**不抛**):VM 未初始化 / 线程不符 / 路径或字段为空 /
		// 脚本不存在 / 编译或执行失败 / 返回值不是表 / 字段不存在 / 值类型不可转文本。
		static bool ReadScriptValue(const std::string& scriptPath, const std::string& field,
			std::string& out, std::string* error = nullptr);

		// M55:脚本 UI 阶段。系统脚本用 `ui.onDraw(fn)` 注册回调,宿主每帧在 UI 阶段调用一次:
		//   * 每个回调都在 `ScriptUiScope(context, 脚本逻辑路径)` 里执行 ⇒ id 带脚本前缀不互相撞;
		//   * **按回调隔离错误**(单个脚本抛错只停它自己,计数返回,日志记 [Luau] ... ui.onDraw failed);
		//   * 返回失败回调数(0 = 全部成功;与 RuntimeLayer 的"只在本帧无错且确有命令时才标记画过"配合)。
		// 只读语义:UI 阶段晚于 Update,`ui.*` 是**返回值式交互**(不做长期回调);此时对 ECS 的结构写
		// 仍受既有结构写门禁约束(会给出可读错误),不是"悄悄允许"。
		static std::size_t DrawScriptUi(Scene& scene, Wui::WuiContext& context);
		// `ui.onDraw(fn)` 的引擎侧入口(绑定层调用):归属到**当前正在加载的系统脚本**;
		// 不在加载期调用 = 可读 error(不猜归属)。
		static void RegisterScriptUiDraw(const ScriptValue& fn, std::string* error = nullptr);
		// 撤销一份系统脚本注册的全部 UI 回调(整份重跑/卸载时用)。
		static void ClearScriptUiDraws(const std::string& logicalPath);

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

		// 幂等登记/刷新一个 Luau 行为（字段来自组件的属性表；路径为空 → false + error）。
		// 同模块 id 描述一致 → 直接返回 true；描述变化（脚本编辑/热重载）→ Replace 刷新。

		// ---- P2 W6:沙箱预算旋钮 ----
		// 进程内默认策略(Init 时下发到 VM;不随 Shutdown 复位)。0 = 该维度不限;
		// 引擎默认 Instructions = 1'000'000、TimeMs = 0(指令口径,确定性优先)。
		// 已开始执行的受保护调用不受影响:策略在作用域开始时固定。
		static void SetSandboxPolicy(const Sandbox::Policy& policy);
		static Sandbox::Policy GetSandboxPolicy();
	};
}
