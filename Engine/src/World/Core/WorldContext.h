#pragma once

#include "World/Core/RuntimeContract.h"
#include "World/Modules/ModuleManager.h"
#include "World/Schema/SchemaRegistry.h"
#include "World/Core/Vfs/Vfs.h"
#include "World/Core/ResourceTable.h"

#include <cstddef>
#include <vector>

namespace World
{
	class Scene;

	// 引擎所有可扩展状态的显式所有者。宿主在 main 中创建,模块只拿到引用。
	class WorldContext
	{
	public:
		WorldContext();
		~WorldContext();
		WorldContext(const WorldContext&) = delete;
		WorldContext& operator=(const WorldContext&) = delete;

		Schema::SchemaRegistry& Schemas() { return m_Schemas; }
		const Schema::SchemaRegistry& Schemas() const { return m_Schemas; }
		Modules::ModuleManager& Modules() { return m_Modules; }
		const Modules::ModuleManager& Modules() const { return m_Modules; }
		World::Vfs::Vfs& Vfs() { return m_Vfs; }
		const World::Vfs::Vfs& Vfs() const { return m_Vfs; }

		// ---- 世界级单例的正式归属 ----
		// 引擎的可扩展状态分两层:
		//   * 上面的显式成员(类型固定、永远存在):Vfs / Schemas / Modules;
		//   * Resources() 是开放登记表(世界级服务:资产目录、驻留注册表、作业统计…):
		//     服务在初始化时 Emplace,系统经 `scene.GetContext().Resources()` 取。
		// 用显式表而不是 entt `ctx()` —— registry 会被整体复制,而世界单例必须不被复制
		// (见 Core/ResourceTable.h 的契约)。
		ResourceTable& Resources() { return m_Resources; }
		const ResourceTable& Resources() const { return m_Resources; }

		// ---- PURE-ECS:场景系统挂载钩子 ----
		// 模块在 `WeModule::Register(context)` 里登记一对回调,引擎在**每次场景进入/离开运行时**
		// 调用它们(编辑器 Play / Simulate、独立 Runtime、Game.dll 自带宿主走同一条路径):
		//
		//   Attach(scene) —— `Scene::OnRuntimeStart()` 末尾(物理世界与引擎内建帧系统都已就绪);
		//   Detach(scene) —— `Scene::OnRuntimeStop()` 开头(帧系统撤销;场景数据仍可读)。
		//
		// 这是"项目用 C++ 写系统"唯一的挂载时机:模块加载时还没有场景,场景创建时模块也拿不到
		// 通知 —— 只有这里两者同时在手。典型实现在 Attach 里 `scene.RegisterSystem<MySystem>()`,
		// Detach 里 `scene.UnregisterFrameSystem(MySystem::Name())`。
		//
		// 契约:
		//   * 同一对函数指针重复登记是 no-op;
		//   * Detach 按登记顺序**逆序**执行(与 Attach 对称);
		//   * **模块卸载前必须 RemoveSceneSystemsHook**(否则钩子指向已卸载的 DLL);
		//     宿主也拒绝"运行时开着就卸载模块"(ModuleManager 的安全点检查)。
		void AddSceneSystemsHook(void (*attach)(Scene&), void (*detach)(Scene&));
		void RemoveSceneSystemsHook(void (*attach)(Scene&), void (*detach)(Scene&));
		void RunSceneAttachHooks(Scene& scene);
		void RunSceneDetachHooks(Scene& scene);
		// 是否登记过任何场景系统钩子:模块卸载的安全点检查据此判断"卸载会不会让运行中的场景
		// 丢掉帧系统函数指针"(没有钩子的模块按老语义可随时卸载)。
		bool HasSceneSystemsHooks() const { return !m_SceneSystemsHooks.empty(); }

		// 数据布局门禁 B(标准 docs/dev/performance-and-data-layout.md §4.3 硬规则 2):成员偏移指纹。
		// 与 Scene::LayoutFingerprint() 同口径:类外 inline 定义 ⇒ 每个 TU 按自己的头文件计算。
		static uint64_t LayoutFingerprint();
	private:
		struct SceneSystemsHook
		{
			void (*Attach)(Scene&) = nullptr;
			void (*Detach)(Scene&) = nullptr;
		};

		World::Vfs::Vfs m_Vfs;
		Schema::SchemaRegistry m_Schemas;
		Modules::ModuleManager m_Modules;
		std::vector<SceneSystemsHook> m_SceneSystemsHooks;
		ResourceTable m_Resources;
	};

}

namespace World
{
	inline uint64_t WorldContext::LayoutFingerprint()
	{
		uint64_t h = LayoutHash::kOffsetBasis;
		h = LayoutHash::Mix(h, static_cast<uint64_t>(offsetof(WorldContext, m_Vfs)));
		h = LayoutHash::Mix(h, static_cast<uint64_t>(offsetof(WorldContext, m_Schemas)));
		h = LayoutHash::Mix(h, static_cast<uint64_t>(offsetof(WorldContext, m_Modules)));
		h = LayoutHash::Mix(h, static_cast<uint64_t>(offsetof(WorldContext, m_SceneSystemsHooks)));
		h = LayoutHash::Mix(h, static_cast<uint64_t>(offsetof(WorldContext, m_Resources)));
		h = LayoutHash::Mix(h, static_cast<uint64_t>(sizeof(WorldContext)));
		return h;
	}
}
