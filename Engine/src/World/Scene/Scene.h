#pragma once
#include "World/Core/Timestep.h"
#include "World/Core/UUID.h"
#include "World/Core/RuntimeContract.h"
#include "World/Core/WorldContext.h"
#include "World/Gameplay/Prefab/PrefabTypes.h"
#include "World/Renderer/EditorCamera.h"
#include "World/Physics/PhysicsEvents.h"
#include "World/Renderer/RenderExtract.h"
#include "World/Scene/ISystem.h"
#include "World/Scene/Query.h"
#include <box2d/id.h>
#include <cstddef>
#include <entt.hpp>
#include <functional>
#include <string>
#include <vector>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <cmath>
#include <limits>
#include <memory>
#include <set>
#include <string>
#include <thread>
#include <type_traits>
#include <typeindex>
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

	// 零内存开销的挂起销毁标签组件:替代过去的 std::unordered_set<entt::entity>
	struct PendingDestroyTag {};

	class Scene
	{
	public:
		// 数据布局门禁 B(标准 docs/dev/performance-and-data-layout.md §4.3 硬规则 2):
		// 成员偏移指纹。类外 inline 定义 ⇒ 每个 TU 用**自己**看到的布局计算,不通过 DLL 调用,
		// 因此宿主与 WorldRuntime.dll 的指纹不一致就等于"两侧来自不同世代头文件"。
		// 覆盖:宿主会直读的关键成员(内联访问器涉及的)+ 尺寸;未覆盖:成员内部深层布局。
		// 定义点在本类右花括号之后(类内定义时 offsetof 面对不完整类型)。
		static uint64_t LayoutFingerprint();
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

		// 场景世代槽位检测:取代原 std::weak_ptr 原子控制块,实现 16 字节平凡拷贝 Entity 句柄
		static bool IsSceneAlive(const Scene* scene, uint16_t slot, uint16_t generation) noexcept;
		uint16_t GetSceneSlot() const noexcept { return m_SceneSlot; }
		uint16_t GetSceneGeneration() const noexcept { return m_SceneGeneration; }

		// M2: 轻量匿名实体与批量创建通道
		Entity CreateRawEntity();
		void CreateEntities(std::size_t count, std::vector<Entity>& out);

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

		// ---- P4-U13b:Prefab 实例注册表(随场景存档) ----
		// 记录"哪些子树是 prefab 实例、来源文件、哪些字段被覆盖";此前只活在编辑器层级面板
		// 的内存里,重开场景即丢;现在由 Scene 持有,并由 SceneSerializer 读写 `.wd` 的
		// `Prefabs:` 块(盘上存实体 UUID,不存 entt 句柄)。
		// 注意:AddPrefabInstance 返回的引用在下一次插入/删除后失效(容器是 std::vector),
		// 需要跨调用保存时请按 Root 句柄重新 FindPrefabInstance。
		std::vector<Gameplay::PrefabInstanceRecord>& PrefabInstances();
		const std::vector<Gameplay::PrefabInstanceRecord>& PrefabInstances() const;
		Gameplay::PrefabInstanceRecord* FindPrefabInstance(entt::entity root);
		const Gameplay::PrefabInstanceRecord* FindPrefabInstance(entt::entity root) const;
		// 同一 root 已存在 → 更新 Path 并返回既有记录(不重复插入)。
		Gameplay::PrefabInstanceRecord& AddPrefabInstance(const std::string& prefabPath, entt::entity root);
		bool RemovePrefabInstance(entt::entity root);

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
			// PURE-ECS:系统阶段与同阶段顺序依赖。此前 FrameSystem 只有 {Name, ParallelSafe, Update},
			// 注册时被硬编码进 `SystemPhase::Update`,而 RunFrameSystems 也只跑 Update ⇒
			// `ISystem::Phase()` / `After()` **全仓没有任何调用点**(阶段与顺序依赖在引擎里
			// 实际不生效,只有 SystemRegistry 自己支持)。这里把它们打通。
			// 位置固定在 `Update` 之后:既有调用方写的 3 元素聚合初始化 `{名字, 并行, 函数}`
			// 因此逐字节不变,新字段走默认值。
			Gameplay::SystemPhase Phase = Gameplay::SystemPhase::Update;
			std::vector<std::string> After;
			std::type_index TypeIndex = std::type_index(typeid(void));
			float Interval = 0.0f;
			std::function<bool()> Condition = nullptr;
			SystemKind Kind = SystemKind::Pipeline;
			// WP6:归属标签(系统面板显示"系统来自谁")。Builtin / Project:<类型> / Lua:<脚本> / Plugin:<名>。
			std::string Owner = "Builtin";
			// WP4:声明式读写集(相位内无冲突自动并行);AccessDeclared=false ⇒ 保守串行 + 尊重 ParallelSafe。
			std::vector<entt::id_type> Reads;
			std::vector<entt::id_type> Writes;
			bool AccessDeclared = false;
		};

		struct LifecycleSystem
		{
			std::string Name;
			std::function<void(Scene&)> Action;
			std::type_index TypeIndex = std::type_index(typeid(void));
		};

		void RegisterStartupSystem(LifecycleSystem system);
		void RegisterTeardownSystem(LifecycleSystem system);
		void RunStartupSystems();
		void RunTeardownSystems();

		uint64_t CurrentWorldTick() const { return m_WorldTick; }
		void AdvanceWorldTick() { ++m_WorldTick; }
		void MarkComponentChanged(entt::id_type componentId);
		uint64_t GetComponentChangeTick(entt::id_type componentId) const;

		template<typename T>
		void MarkChanged() { MarkComponentChanged(entt::type_hash<T>::value()); }

		template<typename T>
		uint64_t GetChangeTick() const { return GetComponentChangeTick(entt::type_hash<T>::value()); }

		using ComponentChangeObserverFn = std::function<void(Entity)>;
		uint64_t AddComponentChangeObserver(entt::id_type componentId, ComponentChangeObserverFn fn);
		bool RemoveComponentChangeObserver(uint64_t handle);
		void NotifyComponentChanged(Entity entity, entt::id_type componentId);
		struct FrameSystemTiming
		{
			std::string Name;
			bool ParallelSafe = false;
			double Milliseconds = 0.0;
			Gameplay::SystemPhase Phase = Gameplay::SystemPhase::Update;
			std::string Owner = "Builtin";
			bool Enabled = true;
		};

		// WP5:场景级时间服务。工业口径:
		//  - ElapsedSeconds 只在运行态累积,暂停(RunFixedWhenPaused=false)时不涨;
		//  - DeltaSeconds = 本可变帧的 dt(可被 TimeScale 缩放),FixedDeltaSeconds = 固定步长;
		//  - FrameCount / FixedStepCount 由宿主驱动递增。
		struct FrameTimeService
		{
			double ElapsedSeconds = 0.0;
			float DeltaSeconds = 0.0f;
			float UnscaledDeltaSeconds = 0.0f;
			float FixedDeltaSeconds = 1.0f / 60.0f;
			uint64_t FrameCount = 0;
			uint64_t FixedStepCount = 0;
			float TimeScale = 1.0f;
		};

		// WP5:一帧的输入快照。**帧首采样一次**、PreFixed/Fixed 只消费 —— 同一可变帧内
		// 跑 0..N 个固定步时,每个固定步看到的输入逐位相同(保 P4/P5 的确定性口径)。
		struct InputSnapshot
		{
			std::unordered_map<std::string, bool> Buttons;   // 动作名 → 是否按下
			std::unordered_map<std::string, float> Axes;     // 动作名 → 轴值 [-1,1]
			glm::vec2 MousePosition { 0.0f };
			glm::vec2 MouseDelta { 0.0f };
			float ScrollDelta = 0.0f;
			uint64_t SampledFrame = 0;
			bool Valid = false;
		};
		void RegisterFrameSystem(FrameSystem system);
		// Pure ECS:撤销一个具名帧系统(系统脚本热重载/替换用)。不存在返回 false。
		bool UnregisterFrameSystem(const std::string& name);
		// WP6:启用/禁用具名帧系统(false = 留在册、面板可见,但本轮不执行)。名字不存在返回 false。
		bool SetFrameSystemEnabled(const std::string& name, bool enabled);
		bool IsFrameSystemEnabled(const std::string& name) const;
		// WP6:设置归属标签(Builtin / Project:<类型> / Lua:<脚本> / Plugin:<名>)。名字不存在返回 false。
		bool SetFrameSystemOwner(const std::string& name, std::string owner);
		// WP5:时间服务(只读查询;推进由宿主/引擎内部走 MutableTime)与输入快照
		// (宿主每**可变帧**采样一次,固定步只消费 —— 保确定性)。
		const FrameTimeService& GetTime() const { return m_TimeService; }
		FrameTimeService& MutableTime() { return m_TimeService; }
		void SetInputSnapshot(InputSnapshot snapshot) { m_InputSnapshot = std::move(snapshot); }
		const InputSnapshot& GetInputSnapshot() const { return m_InputSnapshot; }
		bool UnregisterSystemByType(std::type_index type);
		bool HasFrameSystem(const std::string& name) const;
		bool HasSystemByType(std::type_index type) const;
		void RunFrameSystems(Timestep ts);
		// PURE-ECS(工业口径):**固定步长**阶段(PreFixed + Fixed)。物理与移动在这里推进 ——
		// 它们必须跑固定 dt 才有确定性(同真实时间、不同帧率 ⇒ 同结果);表现层(transform/
		// camera/animation/抽取)留在可变阶段,见 RunFrameSystems。
		// 一帧内由宿主的固定回调调用 **0..N 次**(GameApp 的累加器 + MaxFixedStepsPerFrame)。
		void RunFixedFrameSystems(Timestep fixedDt);
		// 宿主固定回调入口:运行态下跑 RunFixedFrameSystems(不在运行时是 no-op)。
		void OnFixedUpdate(Timestep fixedDt);
		void EnsureDefaultFrameSystems();

		// ---- PURE-ECS:帧内一次的三个"逐实体遍历"步骤 ----
		//
		// 工业口径(Bevy 的 PostUpdate/PreRender/Extract 分组、Unity DOTS 的
		// TransformSystemGroup/AnimationGroup 同一思路):世界矩阵传播、骨骼动画采样、
		// 渲染抽取都是**每帧一次**的逐实体遍历。
		//
		//   * Play / Simulate:由帧系统做(`transform-system` / `animation-system` /
		//     `render-extract`),于是它们出现在 Systems Pipeline 里、可被用户系统用
		//     `after` 排序;
		//   * **编辑态不跑帧系统** ⇒ 渲染前由 `BeginScene` 侧的 Ensure* 兜底跑一次
		//     (否则编辑器里拖父项子项不跟随、蒙皮预览不动)。
		//
		// 三者都**幂等**:同一帧内第二次调用是 no-op。这同时修掉了"`transform-system`
		// 与 `SceneRenderer::RecordSubmit` 各跑一遍世界矩阵传播"的重复计算。
		void BeginFrame();                            // 帧边界:清掉"本帧已做"标记
		void EnsureWorldTransforms();                 // 幂等
		void EnsureAnimationAdvanced(Timestep dt);    // 幂等(步长由调用方给:帧系统用 ts,编辑态用渲染步长)
		// 渲染抽取(相机无关的收集)。幂等;没有 sink 时是 no-op(纯逻辑测试/工具不需要渲染)。
		// sink 由渲染器在 BeginScene 时装上、EndScene 时摘掉 ⇒ 不存在悬垂指针。
		void SetRenderExtractSink(IRenderExtractSink* sink);
		void EnsureRenderExtract(float deltaSeconds);  // 幂等
		// P6:构建"物理插值"的渲染世界矩阵(仅插值开启时非空)。幂等。
		// 语义:插值作用在**局部**变换上,然后按层级合成 —— 父子的相对关系在插值后仍成立。
		// 结果写进 FrameExtract::InterpolatedModels + InterpolatedIndex(draw 只查表)。
		void EnsureInterpolatedWorldTransforms(float alpha);
		// 本帧的抽取结果(由 sink 填;渲染器提交时消费)。Scene 持有 ⇒ 主渲染器与预览渲染器
		// 提交同一场景时看到的是同一份。
		FrameExtract& RenderExtract();
		const FrameExtract& RenderExtract() const;

		template<typename T, typename... Args>
		T& RegisterSystem(Args&&... args)
		{
			static_assert(std::is_base_of_v<ISystem, T>, "T must derive from World::ISystem");
			auto system = std::make_unique<T>(std::forward<Args>(args)...);
			T& ref = *system;

			if (ref.Kind() == SystemKind::Startup)
			{
				RegisterStartupSystem({
					std::string(ref.Name()),
					[sys = std::shared_ptr<ISystem>(std::move(system))](Scene& scene)
					{
						sys->Update(scene, Timestep(0.0f));
					},
					std::type_index(typeid(T))
				});
				return ref;
			}
			if (ref.Kind() == SystemKind::Teardown)
			{
				RegisterTeardownSystem({
					std::string(ref.Name()),
					[sys = std::shared_ptr<ISystem>(std::move(system))](Scene& scene)
					{
						sys->Update(scene, Timestep(0.0f));
					},
					std::type_index(typeid(T))
				});
				return ref;
			}

			EnsureDefaultFrameSystems();
			// WP4:把系统的声明式读写集带进帧系统描述 —— 相位内无冲突就自动并行派发。
			SystemAccess access;
			ref.DeclareAccess(access);
			FrameSystem frameSystem {
				std::string(ref.Name()),
				ref.ParallelSafe(),
				[this, sys = std::shared_ptr<ISystem>(std::move(system))](Timestep ts)
				{
					sys->Update(*this, ts);
				},
				ref.Phase(),
				ref.After(),
				std::type_index(typeid(T)),
				ref.Interval(),
				[this, sysPtr = &ref]() { return sysPtr->ShouldRun(*this); },
				ref.Kind()
			};
			frameSystem.Reads = std::move(access.Reads);
			frameSystem.Writes = std::move(access.Writes);
			frameSystem.AccessDeclared = access.Declared || ref.AccessDeclared();
			RegisterFrameSystem(std::move(frameSystem));
			// WP6:项目层 C++ 系统标归属(面板显示 Project:<类型名>)。
			const std::string systemName(ref.Name());
			SetFrameSystemOwner(systemName, "Project:" + systemName);
			return ref;
		}

		template<typename T>
		bool UnregisterSystem()
		{
			static_assert(std::is_base_of_v<ISystem, T>, "T must derive from World::ISystem");
			return UnregisterSystemByType(std::type_index(typeid(T)));
		}

		template<typename T>
		bool HasSystem() const
		{
			static_assert(std::is_base_of_v<ISystem, T>, "T must derive from World::ISystem");
			return HasSystemByType(std::type_index(typeid(T)));
		}
		const std::vector<FrameSystemTiming>& GetFrameSystemTimings() const { return m_FrameSystemTimings; }
		static const char* GetFrameSystemStatsDescription(const Scene& scene);
		void OnViewportResize(uint32_t width, uint32_t height);
		void OnRuntimeStart();
		void OnRuntimeStop();
		void OnSimulationStart();
		void OnSimulationStop();

		bool IsActive() const { return m_State != SceneState::Stopped; }
		bool IsRunning() const { return m_State == SceneState::Running; }
		bool IsPendingDestroy(entt::entity entity) const;
		void DestroyEntity(entt::entity entity) { RequestDestroy(entity); }
		// ---- W3f:2D 物理运行时 API(仅"世界已启动"时可用) ----
		// 世界只在 OnRuntimeStart/OnSimulationStart → OnRuntimeStop 之间存在;
		// 停止态的刚体只保留组件配置,运行时查询/驱动一律给可读错误(不静默)。
		// 脚本侧入口是 Entity:GetLinearVelocity/SetLinearVelocity/GetAngularVelocity/
		// SetAngularVelocity/ApplyLinearImpulse/ApplyForce/SyncPhysicsBody。
		// 实现在 Scene.cpp(Scene.h 只带 box2d/id.h,b2World_IsValid 的声明在完整 API 头里)。
		bool IsPhysics2DRunning() const;
		// 按实体拿运行中的 Box2D 刚体;没有刚体组件/世界未启动/刚体无效 → false + 可读 error。
		bool TryGetPhysicsBody(entt::entity entity, b2BodyId* bodyId, std::string* error = nullptr);
		// WP3(PECS 1.1):2D 运行态句柄不落组件,这里给出无错误的直查入口(引擎内部/脚本绑定/测试用)。
		// 没有刚体或世界未启动 → b2_nullBodyId / b2_nullJointId;需要可读原因时用上面的 TryGetPhysicsBody。
		b2BodyId GetPhysicsBody2D(entt::entity entity) const;
		b2JointId GetPhysicsJoint2D(entt::entity entity) const;
		// 运动学/静态同步:把当前 Transform 推给 Box2D 刚体(teleport + 唤醒)。
		// 要求世界已启动且有有效刚体;否则抛可读错误。
		void SyncPhysicsBodyFromTransform(entt::entity entity);
		// 运行时建刚体:AddComponent 的 schema 存储绑定只写入组件数据,Box2D 刚体由这里补建
		// (与 OnPhysics2DStart 同一套形状/质量创建逻辑);世界未启动 → 只保留组件配置。
		void EnsurePhysicsBody(entt::entity entity);
		// P5:把组件的 Layer/Mask 同步到物理后端(2D 逐 shape 重设 filter;3D 重映射 ObjectLayer)。
		// 每固定步对每个刚体做一次整数比较 ⇒ 任何写入路径(脚本/面板/读档)都能生效。
		void SyncBodyFilter2D(entt::entity entity);
		// ---- P7:关节(约束)运行时接口 ----
		// 与刚体同一生命周期:世界只在 OnRuntimeStart/OnSimulationStart → OnRuntimeStop 之间存在。
		// 2D 走 Box2D 关节,3D 走 Jolt 约束;两侧都必须是同一后端的刚体。
		// ---- P6:固定步长 → 渲染插值(仅表现层) ----
		// 关闭时(默认)渲染走原路径:draw.Model 直指注册表里的权威矩阵,像素逐字节不变。
		// 开启后由宿主每可变帧推一个 alpha(见 GameApp::LastFixedStepAlpha)。
		void SetPhysicsInterpolationEnabled(bool enabled) { m_PhysicsInterpolationEnabled = enabled; }
		bool IsPhysicsInterpolationEnabled() const { return m_PhysicsInterpolationEnabled; }
		void SetFixedStepAlpha(float alpha);
		float FixedStepAlpha() const { return m_FixedStepAlpha; }
		// 物理系统在 Step **之前**调用:把当前渲染矩阵记成"上一固定步"样本(插值起点)。
		void RecordPhysicsInterpolationState();

		void EnsurePhysicsJoint(entt::entity entity);
		void DestroyPhysicsJoint(entt::entity entity);
		// 某实体被销毁时,把其它实体指向它的关节一起拆掉(否则后端会留悬垂 pair)。
		void DestroyJointsReferencing(entt::entity entity);
		// ---- P1b D6:3D 物理(Jolt)运行时接口 ----
		// 与 2D 相同的生命周期:世界只在 OnRuntimeStart/OnSimulationStart → OnRuntimeStop 之间存在。
		// 同一实体同时挂 2D 与 3D 物理组件 → OnRuntimeStart 抛可读 std::logic_error(整场景拒绝启动,
		// 校验发生在创建任何物理世界之前;编辑器 Play 会捕获并写日志)。
		bool IsPhysics3DRunning() const { return m_Physics3D != nullptr; }
		// 未启动时返回 nullptr;编辑器调试绘制(WLD_PHYSICS_DEBUG)从这里取世界。
		Physics3DWorld* GetPhysics3DWorld() { return m_Physics3D.get(); }
		const Physics3DWorld* GetPhysics3DWorld() const { return m_Physics3D.get(); }
		// ---- P5:物理事实 = ECS 一等公民(场景事件队列) ----
		// 产出:物理系统在**固定步长阶段**把后端(Box2D / Jolt)的事件翻译成下面两种事件;
		// 消费:任意系统在可变阶段(Update / Late / PreRender)读 `GetContactEvents()` / `GetTriggerEvents()`。
		// 清空:每帧恰好一次 —— 本帧首个固定步入口清空;若本帧没有固定步,则由可变阶段入口清空
		//      (见 RunFixedFrameSystems / RunFrameSystems 的 m_FrameTimingsBegun 分支)。
		// 契约:同一固定步输入 ⇒ 事件序列(类型/配对/阶段/顺序)**逐字节可复现**。
		void EnqueueContactEvent(const Physics::ContactEvent& event);
		void EnqueueTriggerEvent(const Physics::TriggerEvent& event);
		const std::vector<Physics::ContactEvent>& GetContactEvents() const { return m_ContactEvents; }
		const std::vector<Physics::TriggerEvent>& GetTriggerEvents() const { return m_TriggerEvents; }
		void ClearPhysicsEvents();
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
		bool CanApplyModuleReload() const;
		// Accepted commands execute once at a safe point; callbacks enqueue the next batch.
		bool DeferStructuralChange(std::function<void(Scene&)> command);
		void FlushStructuralChanges();
		void AssertOwnerThread() const;

		Entity GetPrimaryCameraEntity();
		// ---- PECS(相机):组件只存权威参数,投影矩阵由 CameraSystem 按指纹缓存 ----
		// 结构提交点补建相机帧数据视图(与 EnsurePhysicsBody 同语义:运行态 AddComponent
		// 之后新实体当帧补上)。非相机实体/缺组件是安全的 no-op。
		void EnsureCameraView(entt::entity entity);
		// 取该实体的帧相机数据(投影矩阵)。编辑态允许懒创建视图;运行态只更新(结构写受保护)。
		const Camera& GetCameraView(entt::entity entity);
		WorldContext& GetContext() { return *m_Context; }
		const WorldContext& GetContext() const { return *m_Context; }
		// ---- PLUG-T2c:活实例查询(插件组件存储的卸载前置检查)----
		// 统计进程内活场景(可选:仅同一 WorldContext)里 componentId 的实例总数。
		// 只读:走注册表的 storage(id) → size(),不触发结构写;查询应在场景 owner 线程
		// (宿主主线程)调用。用途 = 卸载插件前确认它的 blob 组件没有活实例:有实例时注销
		// schema 会把场景数据变成"没有 schema 的孤儿"(序列化直接丢数据)。
		static std::size_t CountLiveComponentInstances(entt::id_type componentId,
			const WorldContext* context = nullptr);
		// ---- PLUG-T6:插件组件热重载的定位辅助(不改场景语义)----
		// 进程内活场景列表(稳定顺序 = 构造顺序;可按 WorldContext 过滤)。宿主只拿它定位
		// "插件组件的实例在哪些场景里";真正的读写仍走各场景自己的 owner 线程 / 结构写门禁
		// (GetRegistry 的 AssertStructuralWrite)—— Play/Simulate 场景因此不会被越权改写。
		static std::vector<Scene*> LiveScenes(const WorldContext* context = nullptr);
		// 按实体 UUID 在活场景里找实体(only != nullptr = 只查该场景;命中时写 out*,
		// 未命中保持 out* 不变)。只读遍历,不触发结构写。
		static bool FindLiveEntity(const UUID& id, const WorldContext* context = nullptr,
			const Scene* only = nullptr, Scene** outScene = nullptr, entt::entity* outEntity = nullptr);
		void DuplicateEntity(Entity entity);
		entt::registry& GetRegistry();
		const entt::registry& GetRegistry() const;

		template<typename... Components>
		World::Query<Components...> Query()
		{
			return World::Query<Components...>(m_Registry);
		}

		template<typename... Components>
		World::Query<const Components...> Query() const
		{
			return World::Query<const Components...>(const_cast<entt::registry&>(m_Registry));
		}

		// Pure ECS M3.6: 响应式组件观察者
		struct ComponentObserverEntry
		{
			uint64_t Id = 0;
			entt::id_type ComponentId = 0;
			std::function<void(Entity)> OnAdd;
			std::function<void(Entity)> OnRemove;
		};

		uint64_t AddComponentObserver(entt::id_type componentId, std::function<void(Entity)> onAdd, std::function<void(Entity)> onRemove = nullptr);
		void RemoveComponentObserver(uint64_t observerId);
		void NotifyComponentAdded(Entity entity, entt::id_type componentId);
		void NotifyComponentRemoved(Entity entity, entt::id_type componentId);

		template<typename T>
		uint64_t OnAdd(std::function<void(Entity)> callback)
		{
			return AddComponentObserver(entt::type_id<T>().hash(), std::move(callback), nullptr);
		}

		template<typename T>
		uint64_t OnRemove(std::function<void(Entity)> callback)
		{
			return AddComponentObserver(entt::type_id<T>().hash(), nullptr, std::move(callback));
		}
		static void CopyScene(Ref<Scene>& other, Ref<Scene>& newScene);

	private:
		// "本帧已做"标记(见 BeginFrame / Ensure*)。帧边界由 BeginFrame 推进。
		bool m_WorldTransformsDone = false;
		bool m_AnimationDone = false;
		bool m_RenderExtractDone = false;
		bool m_InterpolatedWorldsDone = false;
		// 一帧的耗时表由**先跑的那个阶段集合**清空(固定先于可变),帧末重置。见 RunFrameSystems。
		bool m_FrameTimingsBegun = false;
		// P5:本帧是否已有固定步跑过 —— 决定物理事件在[本帧第一个固定步]清空,还是由可变阶段入口清空。
		bool m_PhysicsEventsBegun = false;
		// P6:渲染插值开关(默认关)与宿主推来的插值系数。
		bool m_PhysicsInterpolationEnabled = false;
		float m_FixedStepAlpha = 0.0f;
		// 抽取缓冲与它的生产者。缓冲用 unique_ptr:FrameExtract 的完整定义在
		// Renderer/FrameExtract.h,本头文件只前向声明(析构在 Scene.cpp 里定义)。
		std::unique_ptr<FrameExtract> m_RenderExtract;
		IRenderExtractSink* m_RenderExtractSink = nullptr;
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
		// WP5:时间服务与输入快照(见公开段的 FrameTimeService / InputSnapshot)。
		FrameTimeService m_TimeService;
		InputSnapshot m_InputSnapshot;

		entt::registry m_Registry;
		WorldContext* m_Context = nullptr;
		uint16_t m_SceneSlot = 0;
		uint16_t m_SceneGeneration = 0;
		std::thread::id m_OwnerThread;
		SceneState m_State = SceneState::Stopped;
		bool m_StopRequested = false;
		bool m_Committing = false;
		unsigned m_CallbackDepth = 0;
		unsigned m_ScriptWriteDepth = 0;
		ScriptSource m_CallbackSource;
		std::vector<StructuralChange> m_Changes;
		std::unordered_map<entt::entity, std::unordered_set<entt::id_type>> m_PendingRemove;
		uint32_t m_ViewportWidth = 0, m_ViewportHeight = 0;
		b2WorldId m_PhysicsWorldId = b2_nullWorldId;
		// WP3(PECS 1.1):2D runtime handles live here, not inside the components -- the same shape as
		// 3D (Physics3DWorld::Impl::m_Bodies / m_JointRecords). Entries exist only while the world runs;
		// destroying the entity / removing the component / stopping the world erases them.
		std::unordered_map<entt::entity, b2BodyId> m_PhysicsBodies2D;
		std::unordered_map<entt::entity, b2JointId> m_PhysicsJoints2D;
		// P5:建体时用的 (Layer,Mask)(打包成 uint64)。运行时改组件后据此检测变化并
		// b2Shape_SetFilter —— 否则过滤静默不生效(建体时只写过一次)。
		std::unordered_map<entt::entity, uint64_t> m_PhysicsFilter2D;
		// P1b D6:opaque 3D 物理世界(Physics3D.h 不暴露 Jolt;unique_ptr 的删除由 Scene.cpp 承担)。
		std::unique_ptr<Physics3DWorld> m_Physics3D;
		// P5:本帧的物理事件(固定步产出 → 可变阶段消费)。见 EnqueueContactEvent / ClearPhysicsEvents。
		std::vector<Physics::ContactEvent> m_ContactEvents;
		std::vector<Physics::TriggerEvent> m_TriggerEvents;
		// P5:2D 传感器重叠的"仍然在重叠"集合(Box2D 不把传感器重叠当接触,b2Body_GetContactData
		// 拿不到,所以 Persist 由场景自己维护)。键 = PackEntityPair(有序实体对);OnPhysics2DStop 清空。
		std::set<std::uint64_t> m_SensorOverlaps2D;
		// 上次反序列化时未能识别的组件节点（按实体 UUID 保存原始 YAML 片段），供保存时回写，避免缺插件静默丢数据。
		std::unordered_map<UUID, std::string> m_UnknownComponentNodes;
		// P4-U13b:prefab 实例注册表(随 `.wd` 的 Prefabs: 块读写)。
		std::vector<Gameplay::PrefabInstanceRecord> m_PrefabInstances;
		// P4-U4:场景级设置(.wd 头部的 World: 块)。
		WorldSettings m_WorldSettings;

		// Pure ECS M3.6: 响应式组件观察者注册表
		uint64_t m_NextObserverId = 0;
		std::vector<ComponentObserverEntry> m_ComponentObservers;

		// ECS 拓扑扩展: 生命周期系统与变更检测
		std::vector<LifecycleSystem> m_StartupSystems;
		std::vector<LifecycleSystem> m_TeardownSystems;
		uint64_t m_WorldTick = 0;
		std::unordered_map<entt::id_type, uint64_t> m_ComponentVersions;
		struct ComponentChangeObserverEntry
		{
			uint64_t Handle = 0;
			entt::id_type ComponentId = 0;
			ComponentChangeObserverFn Callback;
		};
		std::vector<ComponentChangeObserverEntry> m_ComponentChangeObservers;

	};


	// 有意不放进类体内的 offsetof(类内定义时 Scene 仍是不完整类型):
	// 这里的定义点已在本类右花括号之后,Scene 是完整类型。
}

namespace World
{
	inline uint64_t Scene::LayoutFingerprint()
	{
		uint64_t h = LayoutHash::kOffsetBasis;
		// 每次混入一个"偏移 + 尺寸对":偏移位移或成员换位都会改变结果。
		h = LayoutHash::Mix(h, static_cast<uint64_t>(offsetof(Scene, m_Registry)));
		h = LayoutHash::Mix(h, static_cast<uint64_t>(offsetof(Scene, m_Context)));
		h = LayoutHash::Mix(h, static_cast<uint64_t>(offsetof(Scene, m_SceneSlot)));
		h = LayoutHash::Mix(h, static_cast<uint64_t>(offsetof(Scene, m_SceneGeneration)));
		h = LayoutHash::Mix(h, static_cast<uint64_t>(offsetof(Scene, m_OwnerThread)));
		h = LayoutHash::Mix(h, static_cast<uint64_t>(offsetof(Scene, m_State)));
		h = LayoutHash::Mix(h, static_cast<uint64_t>(offsetof(Scene, m_ViewportWidth)));
		h = LayoutHash::Mix(h, static_cast<uint64_t>(offsetof(Scene, m_ViewportHeight)));
		h = LayoutHash::Mix(h, static_cast<uint64_t>(offsetof(Scene, m_PhysicsWorldId)));
		h = LayoutHash::Mix(h, static_cast<uint64_t>(offsetof(Scene, m_PhysicsBodies2D)));
		h = LayoutHash::Mix(h, static_cast<uint64_t>(offsetof(Scene, m_Physics3D)));
		h = LayoutHash::Mix(h, static_cast<uint64_t>(offsetof(Scene, m_WorldSettings)));
		h = LayoutHash::Mix(h, static_cast<uint64_t>(offsetof(Scene, m_RenderExtract)));
		h = LayoutHash::Mix(h, static_cast<uint64_t>(offsetof(Scene, m_FrameSystems)));
		h = LayoutHash::Mix(h, static_cast<uint64_t>(offsetof(Scene, m_StartupSystems)));
		h = LayoutHash::Mix(h, static_cast<uint64_t>(offsetof(Scene, m_ComponentVersions)));
		h = LayoutHash::Mix(h, static_cast<uint64_t>(offsetof(Scene, m_WorldTick)));
		h = LayoutHash::Mix(h, static_cast<uint64_t>(sizeof(Scene)));
		return h;
	}
}
