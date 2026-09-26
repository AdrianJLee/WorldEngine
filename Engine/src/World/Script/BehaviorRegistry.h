#pragma once

// P2 W2a:语言无关的行为注册层。
//
// 行为 = 挂在实体上的一段生命周期逻辑。当前引擎里有两类现成行为:
//   - C++:CppScriptComponent + ScriptableEntity,字段来自 schema 反射(TypeSchema);
//   - Luau:LuauScriptComponent,字段来自组件的属性表(ScriptProperty:名字 + Schema 值类型)。
// 本层把"行为"从"具体组件类型"里抽象出来:稳定模块 id / 显示名 / 语言标记 /
// 字段描述(名字 + schema 值类型 + 稳定 field id)/ 四个生命周期槽位。
//
// 明确不做:不改 Scene 的脚本调度顺序、不动 T02 状态机、不合并或重命名组件、
// 不改序列化、不接事件总线(W4)。注册表只登记与查询,不参与任何执行。

#include "World/Core/Export.h"
#include "World/Schema/Schema.h"

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include <entt.hpp>

namespace World
{
	class Entity;
	class Scene;
	struct LuauScriptComponent;

	// 行为前端语言标记。
	enum class BehaviorLanguage : uint8_t
	{
		Cpp = 0,
		Luau = 1,
	};

	// "Cpp"/"Luau";未知值返回 "Unknown"。
	WLD_API const char* BehaviorLanguageName(BehaviorLanguage language);

	// 行为模块的一个字段:稳定 field id + 显示名 + schema 值类型。
	// 类型直接用现成的 Schema::Kind(生成器写入 FieldSchema.K 的同一个枚举),不另造类型系统。
	struct BehaviorFieldDesc
	{
		uint64_t FieldId = 0;
		std::string Name;
		Schema::Kind Type = Schema::Kind::None;
	};

	// 四个生命周期槽位的**能力声明**:不保存回调、不规定调用约定、不参与调度。
	//   C++  -> ScriptableEntity 的 OnCreate/OnUpdate/OnDestroy 虚函数面;
	//   Luau -> ScriptEngine 在脚本表上查找 OnCreate/OnUpdate/OnDestroy 的前端面。
	// 是否真的实现了某个回调仍由各前端在加载期判定(Lua 允许回调为 nil)。
	// OnEvent 预留给 W4 事件总线,W2a 恒为 false(没有任何前端接线)。
	struct BehaviorLifecycleSlots
	{
		bool OnCreate = false;
		bool OnUpdate = false;
		bool OnDestroy = false;
		bool OnEvent = false;
	};

	// 一个行为模块的稳定描述。
	struct BehaviorDesc
	{
		// 稳定唯一 id,由 NativeModuleId()/LuaModuleId() 生成;调用方不要手拼。
		std::string ModuleId;
		std::string DisplayName;
		BehaviorLanguage Language = BehaviorLanguage::Cpp;
		// 按 FieldId 升序:字段身份只看 FieldId,与声明顺序无关。
		std::vector<BehaviorFieldDesc> Fields;
		BehaviorLifecycleSlots Lifecycle;
	};

	// 描述等价判定:字段顺序无关(内部按 FieldId 排序后逐项比较)。
	WLD_API bool BehaviorDescEquals(const BehaviorDesc& left, const BehaviorDesc& right);

	// 进程内行为注册表。与 SchemaRegistry 一样属于宿主状态:WorldRuntime.dll 持有唯一实例,
	// 不做跨 DLL 共享;只允许脚本/场景 owner 线程访问(内部不加锁)。
	class WLD_API BehaviorRegistry
	{
	public:
		// 进程内默认实例。
		static BehaviorRegistry& Instance();

		BehaviorRegistry() = default;

		// 同模块 id 重复注册一律拒绝:返回 false 并写入可读 error,**不覆盖**已有描述。
		bool Register(BehaviorDesc desc, std::string* error = nullptr);
		// 显式覆盖:脚本编辑/热重载(W5)刷新同 id 描述;模块不存在时等同 Register。
		bool Replace(BehaviorDesc desc, std::string* error = nullptr);
		// 不存在时返回 false。
		bool Unregister(const std::string& moduleId);

		// 按模块 id 精确查询;不存在返回 nullptr。
		const BehaviorDesc* Find(const std::string& moduleId) const;
		// 全部描述,按 ModuleId 升序(稳定顺序,与插入顺序无关)。
		std::vector<const BehaviorDesc*> List() const;
		std::size_t Size() const;
		void Clear();

		// 现有两类行为的描述构造(纯数据;字段/生命周期见各前端规则)。
		static BehaviorDesc MakeNativeDesc(const Schema::TypeSchema& type);
		// 属性表的类型就是 Schema::Kind(脚本声明阶段已归一):None 不出现,其余原样带进描述。
		static BehaviorDesc MakeLuaDesc(const LuauScriptComponent& script);
		// 严格注册(重复即拒绝);类型不是 Category==Script / 路径为空时返回 false + error。
		bool RegisterNative(const Schema::TypeSchema& type, std::string* error = nullptr);
		bool RegisterLua(const LuauScriptComponent& script, std::string* error = nullptr);

		// 稳定 id 规则(唯一来源,调用方不要重复实现):
		//   NativeModuleId = schema TypeId 全名,例如 "Game::ExampleScript";
		//   LuaModuleId    = "Lua:" + 脚本逻辑路径,例如 "Lua:scripts/player.luau";
		//   LuaFieldId     = Fnv1a64("World::LuauScriptComponent." + 字段名)
		//                    (与 schema-compiler 的 Fnv1a64("Module::Type.FieldName") 同一条规则)。
		static std::string NativeModuleId(const Schema::TypeSchema& type);
		static std::string LuaModuleId(const std::string& scriptFilePath);
		static uint64_t LuaFieldId(const std::string& fieldName);

		// 给定实体枚举行为描述:C++ 取 CppScriptComponent.ScriptName(全名直查,未命中再按
		// 该场景的 schema 注册表解析,与 Scene::StartPendingScripts 同一条路径);Luau 取
		// LuauScriptComponent.ScriptPath。结果按 ModuleId 升序;未注册的模块不出现。
		// 只读场景(走 const registry),Play/Simulate 下也可调用。
		std::vector<const BehaviorDesc*> DescribeEntity(const Scene& scene, entt::entity entity) const;
		std::vector<const BehaviorDesc*> DescribeEntity(const Entity& entity) const;

	private:
		std::map<std::string, BehaviorDesc> m_Behaviors; // 有序容器 → 枚举天然稳定
	};
}
