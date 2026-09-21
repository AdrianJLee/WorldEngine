#pragma once
#include "World/Core/Timestep.h"
#include "World/Core/UUID.h"
#include "World/Core/WorldContext.h"
#include "World/Renderer/EditorCamera.h"
#include <box2d/id.h>
#include <entt.hpp>
#include <functional>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <cmath>
#include <limits>
#include <memory>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace World
{
	class Entity;
	class Physics3DWorld;
	namespace Gameplay { class SystemRegistry; }
	namespace Gameplay { class SaveService; }   // P2a W8:存档服务需要只读遍历 registry
	enum class SceneState { Stopped, Starting, Running, Stopping };

	class Scene
	{
	private:
		// 脚本回调/派发作用域的归属令牌:实体句柄(含版本位)+ 组件 id + 实例 generation。
		// 生命周期的 InvokeCallback 与 W4 的 ScriptCallbackScope 共用它做判活
		// (IsSourceAlive:注册表有效 + 未排队销毁/移除 + generation 相等 + State==Running)。
		struct ScriptSource
		{
			entt::entity EntityHandle = entt::null;
			entt::id_type Component = 0;
			uint64_t Generation = 0;
			bool Destroying = false;
		};

	public:
		explicit Scene(WorldContext& context);
		~Scene();
		Scene(const Scene&) = delete;
		Scene& operator=(const Scene&) = delete;

		// P4-U4:场景级(World)设置 —— 存进 `.wd` 头部的 `World:` 块。
		// 口径:只放"引擎**已经有实现**、但过去只能靠环境变量/项目清单"的 knob;
		// 未设置 = 跟随项目/引擎默认(所以重力用 NaN 表达"不覆盖")。
		struct WorldSettings
		{
			// 场景级重力(Y 轴,单位 m/s²)。NaN = 跟随 project.we.yaml 的 physics.gravity。
			float Gravity = std::numeric_limits<float>::quiet_NaN();
			// 视口里画物理碰撞体/接触点调试线段(过去只能 `WLD_PHYSICS_DEBUG=1`)。
			bool PhysicsDebug = false;

			bool HasGravityOverride() const { return std::isfinite(Gravity); }
			// 是否有任何非默认值(决定要不要在 .wd 里写 `World:` 块)。
			bool IsDefault() const { return !HasGravityOverride() && !PhysicsDebug; }
		};
		WorldSettings& GetWorldSettings() { return m_WorldSettings; }
		const WorldSettings& GetWorldSettings() const { return m_WorldSettings; }

		void OnUpdateEditor(Timestep ts, const EditorCamera& camera);
		void OnUpdateRuntime(Timestep ts);
		void OnUpdateSimulation(Timestep ts, const EditorCamera& camera);

		// ---- B3 帧系统管线 ----
		// 每个系统显式声明是否"并行安全"(只读写自己独占的数据,不触碰注册表结构):
		// 并行安全系统先由 JobSystem 并发执行并汇合,独占系统再在主线程按注册顺序串行执行,
		// 因此顺序与结果与纯串行实现一致。物理(Box2D)与脚本默认独占。
		struct FrameSystem
		{
			std::string Name;
			bool ParallelSafe = false;
			std::function<void(Timestep)> Update;
		};
		struct FrameSystemTiming
		{
			std::string Name;
			bool ParallelSafe = false;
			double Milliseconds = 0.0;
		};
		void RegisterFrameSystem(FrameSystem system);
		void RunFrameSystems(Timestep ts);
		void EnsureDefaultFrameSystems();
		const std::vector<FrameSystemTiming>& GetFrameSystemTimings() const { return m_FrameSystemTimings; }
		static const char* GetFrameSystemStatsDescription(const Scene& scene);
		void OnViewportResize(uint32_t width, uint32_t height);
		void OnRuntimeStart();
		void OnRuntimeStop();
		void OnSimulationStart();
		void OnSimulationStop();
		void OnScriptStart();
		void OnScriptUpdate(Timestep ts);
		void OnScriptDestroy();

		bool IsActive() const { return m_State != SceneState::Stopped; }
		bool IsRunning() const { return m_State == SceneState::Running; }
		bool IsPendingDestroy(entt::entity entity) const;
		// ---- W3f:2D 物理运行时 API(仅"世界已启动"时可用) ----
		// 世界只在 OnRuntimeStart/OnSimulationStart → OnRuntimeStop 之间存在;
		// 停止态的刚体只保留组件配置,运行时查询/驱动一律给可读错误(不静默)。
		// 脚本侧入口是 Entity:GetLinearVelocity/SetLinearVelocity/GetAngularVelocity/
		// SetAngularVelocity/ApplyLinearImpulse/ApplyForce/SyncPhysicsBody。
		// 实现在 Scene.cpp(Scene.h 只带 box2d/id.h,b2World_IsValid 的声明在完整 API 头里)。
		bool IsPhysics2DRunning() const;
		// 按实体拿运行中的 Box2D 刚体;没有刚体组件/世界未启动/刚体无效 → false + 可读 error。
		bool TryGetPhysicsBody(entt::entity entity, b2BodyId* bodyId, std::string* error = nullptr);
		// 运动学/静态同步:把当前 Transform 推给 Box2D 刚体(teleport + 唤醒)。
		// 要求世界已启动且有有效刚体;否则抛可读错误。
		void SyncPhysicsBodyFromTransform(entt::entity entity);
		// 运行时建刚体:AddComponent 的 schema 存储绑定只写入组件数据,Box2D 刚体由这里补建
		// (与 OnPhysics2DStart 同一套形状/质量创建逻辑);世界未启动 → 只保留组件配置。
		void EnsurePhysicsBody(entt::entity entity);
		// ---- P1b D6:3D 物理(Jolt)运行时接口 ----
		// 与 2D 相同的生命周期:世界只在 OnRuntimeStart/OnSimulationStart → OnRuntimeStop 之间存在。
		// 同一实体同时挂 2D 与 3D 物理组件 → OnRuntimeStart 抛可读 std::logic_error(整场景拒绝启动,
		// 校验发生在创建任何物理世界之前;编辑器 Play 会捕获并写日志)。
		bool IsPhysics3DRunning() const { return m_Physics3D != nullptr; }
		// 未启动时返回 nullptr;编辑器调试绘制(WLD_PHYSICS_DEBUG)从这里取世界。
		Physics3DWorld* GetPhysics3DWorld() { return m_Physics3D.get(); }
		const Physics3DWorld* GetPhysics3DWorld() const { return m_Physics3D.get(); }
		// 3D 接触事件的引擎侧钩子(脚本面 events:emit 接线留后续);回调里不得增删钩子表。
		void AddPhysics3DContactCallback(std::function<void(bool added, entt::entity entityA, entt::entity entityB)> callback);
		void ClearPhysics3DContactCallbacks();
		// Physics3DWorld::SyncTransforms 的写口:只在位姿变化时写 TransformComponent
		// (避免每帧标脏/打断层级);Static 由调用方过滤;PendingDestroy/PendingRemoval 跳过。
		// 返回是否真的写了。
		bool SyncPhysics3DTransform(entt::entity entity, const glm::vec3& location, const glm::quat& rotation);
		// ---- W3d:脚本回调内的白名单同步结构写 ----
		// 只建实体槽并挂 Tag+UUID(不挂 Transform),返回的句柄当帧有效。
		// 与 Entity::CreateEntity 的差别:回调内绕过 AssertStructuralWrite 的回调深度检查,
		// 但仍拒绝 Stop 流程/回调外活动场景的结构写。
		Entity CreateEntityShell(const std::string& name = "Empty Entity");
		// 当前是否在脚本生命周期回调(OnCreate/OnUpdate/OnDestroy)内。
		bool IsInsideScriptCallback() const;
		// OnScriptUpdate 的实体可见性快照:回调内同步创建的新实体当帧对其它脚本的
		// FindByName 不可见,下一帧自动进入快照。没有活动快照时一律可见。
		bool IsVisibleToCurrentScriptUpdate(entt::entity entity) const;
		// 白名单同步结构写的临时窗口。只在回调内或结构提交点内可构造,否则抛可读错误;
		// 窗口内 AssertStructuralWrite 放行回调深度/活动场景检查(嵌套按深度恢复)。
		class ScriptWriteScope
		{
		public:
			explicit ScriptWriteScope(Scene& scene);
			~ScriptWriteScope();
			ScriptWriteScope(const ScriptWriteScope&) = delete;
			ScriptWriteScope& operator=(const ScriptWriteScope&) = delete;
		private:
			Scene* m_Scene = nullptr;
		};
		// 当前是否在 ScriptWriteScope 内(脚本回调或结构提交点的白名单结构写窗口)。
		// Entity 的动态 AddComponent 用它区分"外部调用(延迟提交)"与"窗口内(同步提交)"。
		bool IsInsideScriptWriteScope() const { return m_ScriptWriteDepth != 0; }
		// ---- W4:事件/计时器派发作用域 ----
		// 事件/计时器回调不在生命周期回调里,但语义要与脚本回调一致:抬升回调深度、
		// 设置"当前脚本来源",并进入白名单结构写窗口(与 ScriptWriteScope 同源),
		// 使 CreateEntityShell / AddComponent / SetParent 在回调里可用且当帧可见。
		// 构造时按 owner(场景令牌 + 实体句柄含版本 + 组件 id + generation)判活:
		// 实例已销毁/已热重载/非 Running/场景正在停止 → IsValid()==false,调用方不得执行回调体。
		class ScriptCallbackScope
		{
		public:
			ScriptCallbackScope(Scene& scene, Entity owner, entt::id_type component, uint64_t generation);
			~ScriptCallbackScope();
			ScriptCallbackScope(const ScriptCallbackScope&) = delete;
			ScriptCallbackScope& operator=(const ScriptCallbackScope&) = delete;
			ScriptCallbackScope(ScriptCallbackScope&&) = delete;
			ScriptCallbackScope& operator=(ScriptCallbackScope&&) = delete;
			bool IsValid() const { return m_Valid; }
		private:
			Scene* m_Scene = nullptr;
			ScriptSource m_PreviousSource;
			bool m_Valid = false;
		};
		// W5:脚本热重载只允许在安全点提交——不在脚本回调内、不在结构提交点内、
		// 也不在 Stop 流程中。宿主(编辑器/Runtime)应在帧边界调用,并以此判定是否可重载。
		bool CanApplyScriptReload() const;
		// Accepted commands execute once at a safe point; callbacks enqueue the next batch.
		bool DeferStructuralChange(std::function<void(Scene&)> command);
		void FlushStructuralChanges();
		void AssertOwnerThread() const;

		Entity GetPrimaryCameraEntity();
		WorldContext& GetContext() { return *m_Context; }
		const WorldContext& GetContext() const { return *m_Context; }
		void DuplicateEntity(Entity entity);
		entt::registry& GetRegistry();
		const entt::registry& GetRegistry() const;
		static void CopyScene(Ref<Scene>& other, Ref<Scene>& newScene);

	private:
		friend class Entity;
		friend class SceneRenderer;
		friend class SceneHierarchyPanel;
		friend class SceneSerializer;
		friend class Gameplay::SaveService;

		enum class ChangeKind { General, DestroyEntity, RemoveComponent };
		struct StructuralChange
		{
			ChangeKind Kind = ChangeKind::General;
			std::function<void(Scene&)> Command;
			ScriptSource Source;
			entt::entity Target = entt::null;
			entt::id_type Component = 0;
		};

		void AssertStructuralWrite() const;
		bool IsPendingRemoval(entt::entity entity, entt::id_type component) const;
		bool IsSourceAlive(const ScriptSource& source) const;
		void RequestDestroy(entt::entity entity);
		void RequestRemove(entt::entity entity, entt::id_type component);
		void DestroyEntityNow(entt::entity entity);
		void RemoveComponentNow(entt::entity entity, entt::id_type component);
		void InvokeCallback(const ScriptSource& source, const std::function<void()>& callback);
		void StartPendingScripts();
		void UpdateScriptSnapshot(Timestep ts, const std::vector<entt::entity>& native, const std::vector<entt::entity>& lua);
		void BeginScriptUpdateSnapshot();
		void EndScriptUpdateSnapshot();
		void DestroyNativeScript(entt::entity entity, bool faulted = false);
		void DestroyLuaScript(entt::entity entity, bool faulted = false);
		void FaultSource(const ScriptSource& source, const std::string& error);
		void StopScene();
		void OnPhysics2DStart();
		void OnUpdatePhysics2D(Timestep ts);
		void OnPhysics2DStop();
		void DestroyPhysicsBody(entt::entity entity);
		void OnPhysics3DStart();
		void OnUpdatePhysics3D(Timestep ts);
		void OnPhysics3DStop();

		// W5-3:帧系统调度统一交给 Gameplay::SystemRegistry(阶段/依赖/并行/耗时),
		// 用 pimpl 避免核心层头文件依赖 Gameplay 实现细节。
		std::unique_ptr<Gameplay::SystemRegistry> m_FrameSystems;
		std::vector<FrameSystem> m_FrameSystemDefinitions;
		std::vector<FrameSystemTiming> m_FrameSystemTimings;

		entt::registry m_Registry;
		WorldContext* m_Context = nullptr;
		std::shared_ptr<const uint8_t> m_Lifetime = std::make_shared<const uint8_t>(0);
		std::thread::id m_OwnerThread;
		SceneState m_State = SceneState::Stopped;
		bool m_StopRequested = false;
		bool m_Committing = false;
		unsigned m_CallbackDepth = 0;
		unsigned m_ScriptWriteDepth = 0;
		bool m_ScriptUpdateSnapshotActive = false;
		std::unordered_set<entt::entity> m_ScriptUpdateSnapshot;
		uint64_t m_NextGeneration = 0;
		ScriptSource m_CallbackSource;
		std::vector<StructuralChange> m_Changes;
		std::unordered_set<entt::entity> m_PendingDestroy;
		std::unordered_map<entt::entity, std::unordered_set<entt::id_type>> m_PendingRemove;
		uint32_t m_ViewportWidth = 0, m_ViewportHeight = 0;
		b2WorldId m_PhysicsWorldId = b2_nullWorldId;
		// P1b D6:opaque 3D 物理世界(Physics3D.h 不暴露 Jolt;unique_ptr 的删除由 Scene.cpp 承担)。
		std::unique_ptr<Physics3DWorld> m_Physics3D;
		std::vector<std::function<void(bool added, entt::entity entityA, entt::entity entityB)>> m_Physics3DContactCallbacks;
		// 上次反序列化时未能识别的组件节点（按实体 UUID 保存原始 YAML 片段），供保存时回写，避免缺插件静默丢数据。
		std::unordered_map<UUID, std::string> m_UnknownComponentNodes;
		// P4-U4:场景级设置(.wd 头部的 World: 块)。
		WorldSettings m_WorldSettings;
	};
}
