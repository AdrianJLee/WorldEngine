#include "wldpch.h"
#include "Scene.h"
#include "World/Scene/Components.h"
#include "World/Scene/TransformSystem.h"
#include "World/Scene/CameraSystem.h"
#include "World/Scene/MovementSystem.h"
#include "World/Scene/ScriptEngine.h"
#include "World/Core/Thread/JobSystem.h"
#include "World/Gameplay/SystemRegistry.h"
#include "World/Physics/Physics3D.h"
#include <box2d/box2d.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <mutex>
#include <stdexcept>

namespace World
{
	namespace
	{
		// ---- PLUG-T2c:活场景表 ----------------------------------------------------
		// Scene 构造/析构维护 (WorldContext, registry) 指针;插件卸载前的活实例查询用它
		// 枚举"进程内所有活场景"(可按 WorldContext 过滤)。表本身加锁;注册表内容的读写
		// 仍由场景 owner 线程负责 —— 查询走宿主主线程的卸载路径(与场景同线程)。
		struct LiveSceneEntry
		{
			Scene* Owner = nullptr;
			const WorldContext* Context = nullptr;
			const entt::registry* Registry = nullptr;
		};

		std::mutex g_LiveScenesMutex;
		std::vector<LiveSceneEntry> g_LiveScenes;

		void RegisterLiveScene(Scene* owner, const WorldContext* context, const entt::registry* registry)
		{
			std::lock_guard<std::mutex> lock(g_LiveScenesMutex);
			g_LiveScenes.push_back({ owner, context, registry });
		}

		void UnregisterLiveScene(const entt::registry* registry)
		{
			std::lock_guard<std::mutex> lock(g_LiveScenesMutex);
			g_LiveScenes.erase(std::remove_if(g_LiveScenes.begin(), g_LiveScenes.end(),
				[registry](const LiveSceneEntry& entry) { return entry.Registry == registry; }),
				g_LiveScenes.end());
		}

		template<typename T>
		std::vector<entt::entity> Snapshot(entt::registry& registry)
		{
			auto view = registry.view<T>();
			return { view.begin(), view.end() };
		}

		thread_local bool s_InsideFramePipeline = false;



		// 脚本属性按名字找 schema 字段(大小写敏感;不含嵌套/非叶类型)。
		const Schema::FieldSchema* FindSchemaField(const Schema::TypeSchema& type, const std::string& name)
		{
			for (const Schema::FieldSchema& field : type.Fields)
			{
				if (field.Name == name)
					return &field;
			}
			return nullptr;
		}

		void Report(const std::string& error)
		{
			if (Log::GetCoreLogger()) WLD_CORE_ERROR("{0}", error);
		}

		// W3f:刚体创建的唯一实现(世界启动与运行时 AddComponent 共用)。
		// 形状/质量折算与 OnPhysics2DStart 的既有口径一致:位置/旋转取 Transform,尺寸乘 Scale。
		bool BuildRuntimeBody(b2WorldId worldId, entt::registry& registry, entt::entity entity)
		{
			if (!b2World_IsValid(worldId)) return false;
			auto* rb = registry.try_get<RigidBody2DComponent>(entity);
			if (!rb || b2Body_IsValid(rb->RuntimeBodyId)) return false;
			auto toBox2DType = [](RigidBody2DComponent::BodyType type)
			{
				switch (type)
				{
					case RigidBody2DComponent::BodyType::Static: return b2_staticBody;
					case RigidBody2DComponent::BodyType::Dynamic: return b2_dynamicBody;
					case RigidBody2DComponent::BodyType::Kinematic: return b2_kinematicBody;
				}
				return b2_staticBody;
			};
			auto* transform = registry.try_get<TransformComponent>(entity);
			if (!transform)
			{
				Report("[Physics] Missing Transform for rigid body entity=" + std::to_string(static_cast<uint32_t>(entity)));
				return false;
			}
			b2BodyDef bodyDef = b2DefaultBodyDef();
			bodyDef.type = toBox2DType(rb->Type);
			bodyDef.position = { transform->Location.x, transform->Location.y };
			bodyDef.rotation = b2MakeRot(transform->Rotation.z);
			bodyDef.motionLocks.angularZ = rb->FixedRotation;
			rb->RuntimeBodyId = b2CreateBody(worldId, &bodyDef);
			if (const auto* box = registry.try_get<BoxCollider2DComponent>(entity))
			{
				b2ShapeDef shapeDef = b2DefaultShapeDef();
				shapeDef.density = box->Density;
				shapeDef.material.friction = box->Friction;
				shapeDef.material.restitution = box->Restitution;
				b2Polygon polygon = b2MakeOffsetBox(transform->Scale.x * box->Size.x, transform->Scale.y * box->Size.y,
					{ box->Offset.x, box->Offset.y }, b2MakeRot(0.0f));
				b2CreatePolygonShape(rb->RuntimeBodyId, &shapeDef, &polygon);
			}
			if (const auto* circle = registry.try_get<CircleCollider2DComponent>(entity))
			{
				b2Circle shape;
				shape.center = { circle->Offset.x, circle->Offset.y };
				shape.radius = circle->Radius * transform->Scale.x;
				b2ShapeDef shapeDef = b2DefaultShapeDef();
				shapeDef.density = circle->Density;
				shapeDef.material.friction = circle->Friction;
				shapeDef.material.restitution = circle->Restitution;
				b2CreateCircleShape(rb->RuntimeBodyId, &shapeDef, &shape);
			}
			return true;
		}

	}

	Scene::Scene(WorldContext& context) : m_Context(&context), m_OwnerThread(std::this_thread::get_id())
	{
		RegisterLiveScene(this, m_Context, &m_Registry);
	}

	Scene::~Scene()
	{
		// Destroying a scene on another thread would also destroy Lua references there.
		if (m_OwnerThread != std::this_thread::get_id())
		{
			Report("Scene destruction must run on its owner thread");
			std::terminate();
		}
		StopScene();
		m_Lifetime.reset();
		UnregisterLiveScene(&m_Registry);
	}

	std::size_t Scene::CountLiveComponentInstances(entt::id_type componentId,
		const WorldContext* context)
	{
		std::lock_guard<std::mutex> lock(g_LiveScenesMutex);
		std::size_t count = 0;
		for (const LiveSceneEntry& entry : g_LiveScenes)
		{
			if (context && entry.Context != context)
				continue;
			const auto* storage = entry.Registry->storage(componentId);
			if (storage)
				count += storage->size();
		}
		return count;
	}

	std::vector<Scene*> Scene::LiveScenes(const WorldContext* context)
	{
		std::lock_guard<std::mutex> lock(g_LiveScenesMutex);
		std::vector<Scene*> scenes;
		scenes.reserve(g_LiveScenes.size());
		for (const LiveSceneEntry& entry : g_LiveScenes)
		{
			if (context && entry.Context != context)
				continue;
			if (entry.Owner)
				scenes.push_back(entry.Owner);
		}
		return scenes;
	}

	bool Scene::FindLiveEntity(const UUID& id, const WorldContext* context, const Scene* only,
		Scene** outScene, entt::entity* outEntity)
	{
		std::lock_guard<std::mutex> lock(g_LiveScenesMutex);
		for (const LiveSceneEntry& entry : g_LiveScenes)
		{
			if (!entry.Owner || !entry.Registry)
				continue;
			if (context && entry.Context != context)
				continue;
			if (only && entry.Owner != only)
				continue;
			for (const entt::entity handle : entry.Registry->view<UUIDComponent>())
			{
				const UUIDComponent& identity = entry.Registry->get<UUIDComponent>(handle);
				if (static_cast<uint64_t>(identity.ID) != static_cast<uint64_t>(id))
					continue;
				if (outScene)
					*outScene = entry.Owner;
				if (outEntity)
					*outEntity = handle;
				return true;
			}
		}
		return false;
	}

	void Scene::AssertOwnerThread() const
	{
		if (m_OwnerThread != std::this_thread::get_id())
			throw std::logic_error("Scene access must run on its owner thread");
	}

	void Scene::RegisterFrameSystem(FrameSystem system)
	{
		AssertOwnerThread();
		if (system.Name.empty() || !system.Update)
			throw std::invalid_argument("Scene frame system requires a name and an update function");
		for (const FrameSystem& existing : m_FrameSystemDefinitions)
			if (existing.Name == system.Name)
				throw std::invalid_argument("Scene frame system '" + system.Name + "' is already registered");

		if (!m_FrameSystems)
			m_FrameSystems = std::make_unique<Gameplay::SystemRegistry>();
		Gameplay::SystemDesc desc;
		desc.Name = system.Name;
		desc.Phase = Gameplay::SystemPhase::Update;
		desc.ParallelSafe = system.ParallelSafe;
		Gameplay::SystemRegistry::UpdateFn update = std::move(system.Update);
		if (!m_FrameSystems->Register(desc, std::move(update)))
			throw std::invalid_argument("Scene frame system '" + desc.Name + "' rejected: " + m_FrameSystems->GetLastError());
		m_FrameSystemDefinitions.push_back(std::move(system));
	}

	bool Scene::HasFrameSystem(const std::string& name) const
	{
		for (const FrameSystem& existing : m_FrameSystemDefinitions)
			if (existing.Name == name)
				return true;
		return false;
	}

	bool Scene::UnregisterFrameSystem(const std::string& name)
	{
		AssertOwnerThread();
		if (name.empty())
			return false;
		const auto found = std::find_if(m_FrameSystemDefinitions.begin(), m_FrameSystemDefinitions.end(),
			[&name](const FrameSystem& existing) { return existing.Name == name; });
		if (found == m_FrameSystemDefinitions.end())
			return false;
		m_FrameSystemDefinitions.erase(found);
		if (m_FrameSystems)
			m_FrameSystems->Unregister(name);
		return true;
	}

	// W5-3:帧系统调度统一走 Gameplay::SystemRegistry
	// (阶段/同阶段依赖/并行安全并行派发/逐系统耗时),Scene 只保留面向宿主的薄封装。
	void Scene::RunFrameSystems(Timestep ts)
	{
		AssertOwnerThread();
		m_FrameSystemTimings.clear();
		if (!m_FrameSystems)
			return;

		s_InsideFramePipeline = true;
		struct PipelineScopeGuard
		{
			~PipelineScopeGuard() { s_InsideFramePipeline = false; }
		} guard;

		m_FrameSystems->RunPhase(Gameplay::SystemPhase::Update, ts);
		for (const Gameplay::SystemTiming& timing : m_FrameSystems->GetLastTimings())
			m_FrameSystemTimings.push_back({ timing.Name, timing.ParallelSafe, timing.Milliseconds });
	}
	const char* Scene::GetFrameSystemStatsDescription(const Scene& scene)
	{
		// 静态缓冲:仅供日志/调试打印,不做线程安全承诺。
		static std::string description;
		description.clear();
		for (const FrameSystemTiming& timing : scene.m_FrameSystemTimings)
		{
			if (!description.empty())
				description += ", ";
			description += timing.Name + (timing.ParallelSafe ? "[par]" : "[excl]") + "=" +
				std::to_string(timing.Milliseconds) + "ms";
		}
		return description.c_str();
	}

	void Scene::AssertStructuralWrite() const
	{
		AssertOwnerThread();
		if (m_CallbackDepth && m_ScriptWriteDepth == 0)
			throw std::logic_error("Synchronous structural writes are forbidden in lifecycle callbacks; use DeferStructuralChange");
		if (m_State == SceneState::Stopping || (IsActive() && !m_Committing && m_ScriptWriteDepth == 0))
			throw std::logic_error("An active scene accepts structural writes only while committing a command");
	}

	Scene::ScriptWriteScope::ScriptWriteScope(Scene& scene) : m_Scene(&scene)
	{
		scene.AssertOwnerThread();
		if (scene.m_State == SceneState::Stopping || scene.m_StopRequested)
			throw std::logic_error("Script structural writes are forbidden while the scene is stopping");
		// 只放行生命周期回调内的白名单写;回调外仍需走既有的结构提交点。
		if (scene.m_CallbackDepth == 0 && !scene.m_Committing)
			throw std::logic_error("Script structural writes are only allowed inside lifecycle callbacks or structural commits");
		++scene.m_ScriptWriteDepth;
	}

	Scene::ScriptWriteScope::~ScriptWriteScope()
	{
		if (m_Scene && m_Scene->m_ScriptWriteDepth)
			--m_Scene->m_ScriptWriteDepth;
	}

	// W4:事件/计时器回调的派发作用域。与生命周期回调的 InvokeCallback 同源地抬升
	// m_CallbackDepth 并设置"当前脚本来源",同时打开白名单结构写窗口(m_ScriptWriteDepth),
	// 使 CreateEntityShell / AddComponent / SetParent 在事件/计时器回调里当帧生效。
	// owner(句柄含场景令牌 + generation)不可用时 IsValid()==false,调用方不得执行回调体。
	Scene::ScriptCallbackScope::ScriptCallbackScope(Scene& scene, Entity owner, entt::id_type component,
		uint64_t generation)
		: m_Scene(&scene)
	{
		scene.AssertOwnerThread();
		if (!owner.IsValid() || owner.GetScene() != &scene)
			return;
		if (scene.m_State == SceneState::Stopping || scene.m_StopRequested)
			return;
		const ScriptSource source { static_cast<entt::entity>(owner), component, generation, false };
		if (!scene.IsSourceAlive(source))
			return;
		m_PreviousSource = scene.m_CallbackSource;
		scene.m_CallbackSource = source;
		++scene.m_CallbackDepth;
		++scene.m_ScriptWriteDepth;
		m_Valid = true;
	}

	Scene::ScriptCallbackScope::~ScriptCallbackScope()
	{
		if (!m_Valid || !m_Scene)
			return;
		m_Scene->m_CallbackSource = m_PreviousSource;
		if (m_Scene->m_CallbackDepth)
			--m_Scene->m_CallbackDepth;
		if (m_Scene->m_ScriptWriteDepth)
			--m_Scene->m_ScriptWriteDepth;
		m_Valid = false;
	}

	Entity Scene::CreateEntityShell(const std::string& name)
	{
		AssertOwnerThread();
		if (m_State == SceneState::Stopping || m_StopRequested)
			throw std::logic_error("Cannot create an entity shell while the scene is stopping");
		// 回调深度是本契约唯一放行的检查;活动场景在回调外仍只接受提交点写入。
		if (m_CallbackDepth == 0 && IsActive() && !m_Committing)
			throw std::logic_error("An active scene accepts structural writes only while committing a command or inside a script callback");
		const entt::entity handle = m_Registry.create();
		m_Registry.emplace<TagComponent>(handle, name);
		m_Registry.emplace<UUIDComponent>(handle, UUID());
		Entity entity(this, handle);
		NotifyComponentAdded(entity, entt::type_id<TagComponent>().hash());
		NotifyComponentAdded(entity, entt::type_id<UUIDComponent>().hash());
		return entity;
	}

	bool Scene::IsInsideScriptCallback() const
	{
		AssertOwnerThread();
		return m_CallbackDepth != 0;
	}

	bool Scene::IsVisibleToCurrentScriptUpdate(entt::entity) const
	{
		AssertOwnerThread();
		return true;
	}

	entt::registry& Scene::GetRegistry() { AssertStructuralWrite(); return m_Registry; }
	const entt::registry& Scene::GetRegistry() const { AssertOwnerThread(); return m_Registry; }

	// P4-U13b:Prefab 实例注册表的访问器。它们只碰 std::vector 成员,不写 entt 注册表结构,
	// 所以只做线程归属检查(与 GetWorldSettings 同类);结构写保护仍由 GetRegistry 承担。
	std::vector<Gameplay::PrefabInstanceRecord>& Scene::PrefabInstances()
	{
		AssertOwnerThread();
		return m_PrefabInstances;
	}

	const std::vector<Gameplay::PrefabInstanceRecord>& Scene::PrefabInstances() const
	{
		AssertOwnerThread();
		return m_PrefabInstances;
	}

	Gameplay::PrefabInstanceRecord* Scene::FindPrefabInstance(entt::entity root)
	{
		AssertOwnerThread();
		for (Gameplay::PrefabInstanceRecord& record : m_PrefabInstances)
			if (record.Root == root)
				return &record;
		return nullptr;
	}

	const Gameplay::PrefabInstanceRecord* Scene::FindPrefabInstance(entt::entity root) const
	{
		AssertOwnerThread();
		for (const Gameplay::PrefabInstanceRecord& record : m_PrefabInstances)
			if (record.Root == root)
				return &record;
		return nullptr;
	}

	Gameplay::PrefabInstanceRecord& Scene::AddPrefabInstance(const std::string& prefabPath, entt::entity root)
	{
		AssertOwnerThread();
		if (Gameplay::PrefabInstanceRecord* existing = FindPrefabInstance(root))
		{
			existing->PrefabPath = prefabPath;
			return *existing;
		}
		Gameplay::PrefabInstanceRecord record;
		record.PrefabPath = prefabPath;
		record.Root = root;
		m_PrefabInstances.push_back(std::move(record));
		return m_PrefabInstances.back();
	}

	bool Scene::RemovePrefabInstance(entt::entity root)
	{
		AssertOwnerThread();
		for (auto it = m_PrefabInstances.begin(); it != m_PrefabInstances.end(); ++it)
		{
			if (it->Root != root)
				continue;
			m_PrefabInstances.erase(it);
			return true;
		}
		return false;
	}

	bool Scene::IsPendingDestroy(entt::entity entity) const
	{
		AssertOwnerThread();
		return m_PendingDestroy.count(entity) != 0;
	}

	bool Scene::IsPendingRemoval(entt::entity entity, entt::id_type component) const
	{
		auto it = m_PendingRemove.find(entity);
		return it != m_PendingRemove.end() && it->second.count(component) != 0;
	}

	bool Scene::IsSourceAlive(const ScriptSource& source) const
	{
		if (source.EntityHandle == entt::null) return true;
		if (!m_Registry.valid(source.EntityHandle) || IsPendingDestroy(source.EntityHandle) ||
			IsPendingRemoval(source.EntityHandle, source.Component)) return false;
		return true;
	}

	bool Scene::DeferStructuralChange(std::function<void(Scene&)> command)
	{
		AssertOwnerThread();
		if (!command || m_State == SceneState::Stopping || m_StopRequested || m_CallbackSource.Destroying) return false;
		if (!IsActive() && !m_CallbackDepth && !m_Committing)
		{
			try { command(*this); return true; }
			catch (const std::exception& error) { Report(std::string("[Scene] StructuralChange: ") + error.what()); }
			catch (...) { Report("[Scene] StructuralChange: unknown exception"); }
			return false;
		}
		m_Changes.push_back({ ChangeKind::General, std::move(command), m_CallbackSource });
		return true;
	}

	void Scene::RequestDestroy(entt::entity entity)
	{
		AssertOwnerThread();
		if (!m_Registry.valid(entity) || !m_PendingDestroy.insert(entity).second) return;
		m_Changes.push_back({ ChangeKind::DestroyEntity, {}, {}, entity });
		if (!IsActive() && !m_CallbackDepth && !m_Committing) FlushStructuralChanges();
	}

	void Scene::RequestRemove(entt::entity entity, entt::id_type component)
	{
		AssertOwnerThread();
		if (!m_Registry.valid(entity) || IsPendingDestroy(entity) || IsPendingRemoval(entity, component)) return;
		Entity target(this, entity);
		if (!target.HasComponent(component)) return;
		std::string reason;
		if (!target.CanRemoveComponent(component, &reason)) throw std::logic_error(reason);
		m_PendingRemove[entity].insert(component);
		m_Changes.push_back({ ChangeKind::RemoveComponent, {}, {}, entity, component });
		if (!IsActive() && !m_CallbackDepth && !m_Committing) FlushStructuralChanges();
	}

	bool Scene::CanApplyScriptReload() const
	{
		// 与 FlushStructuralChanges 的守卫同源:回调内/结构提交点内禁止改脚本实例,
		// 停止流程中也不允许(即将销毁全部实例)。
		return m_CallbackDepth == 0 && !m_Committing && m_ScriptWriteDepth == 0 && !m_StopRequested
			&& m_State != SceneState::Stopping;
	}

	void Scene::FlushStructuralChanges()
	{
		AssertOwnerThread();
		if (m_CallbackDepth || m_Committing || m_ScriptWriteDepth)
			throw std::logic_error("Structural changes cannot be recursively committed");
		if (m_StopRequested) { StopScene(); return; }
		std::vector<StructuralChange> batch;
		batch.swap(m_Changes);
		m_Committing = true;
		for (auto& change : batch)
		{
			const auto previousSource = m_CallbackSource;
			// Commands submitted by a command retain its original script owner too.
			m_CallbackSource = change.Source;
			try
			{
				switch (change.Kind)
				{
					case ChangeKind::General:
						if (m_State != SceneState::Stopping && IsSourceAlive(change.Source)) change.Command(*this);
						break;
					case ChangeKind::DestroyEntity: DestroyEntityNow(change.Target); break;
					case ChangeKind::RemoveComponent: RemoveComponentNow(change.Target, change.Component); break;
				}
			}
			catch (const std::exception& error) { FaultSource(change.Source, error.what()); }
			catch (...) { FaultSource(change.Source, "Unknown exception in structural command"); }
			m_CallbackSource = previousSource;
			if (m_StopRequested) break;
		}
		m_Committing = false;
		if (m_StopRequested) StopScene();
	}

	void Scene::InvokeCallback(const ScriptSource& source, const std::function<void()>& callback)
	{
		const auto previous = m_CallbackSource;
		m_CallbackSource = source;
		++m_CallbackDepth;
		try { callback(); }
		catch (...) { --m_CallbackDepth; m_CallbackSource = previous; throw; }
		--m_CallbackDepth;
		m_CallbackSource = previous;
	}

	void Scene::FaultSource(const ScriptSource& source, const std::string& error)
	{
		Report("[Scene] entity=" + std::to_string(static_cast<uint32_t>(source.EntityHandle)) +
			" phase=StructuralChange: " + error);
	}



	void Scene::DestroyPhysicsBody(entt::entity entity)
	{
		auto* body = m_Registry.try_get<RigidBody2DComponent>(entity);
		if (!body) return;
		b2BodyId bodyId = body->RuntimeBodyId;
		// 先把组件切到"无刚体"状态,再销毁 Box2D 刚体(失败/重入都不会留下悬垂句柄)。
		body->RuntimeBodyId = b2_nullBodyId;
		if (b2Body_IsValid(bodyId)) b2DestroyBody(bodyId);
	}

	// ---- W3f:2D 物理运行时 API ----
	bool Scene::IsPhysics2DRunning() const
	{
		return b2World_IsValid(m_PhysicsWorldId);
	}

	// 组件 id 走 schema 的 StorageBinding(entt 类型 hash 已隐式注册,取不到不代表"没有该组件")。
	bool Scene::TryGetPhysicsBody(entt::entity entity, b2BodyId* bodyId, std::string* error)
	{
		auto Fail = [&](const std::string& message)
		{
			if (error) *error = message;
			return false;
		};
		if (bodyId) *bodyId = b2_nullBodyId;
		if (error) error->clear();
		if (!m_Registry.valid(entity))
			return Fail("requires a live entity");
		const Schema::TypeSchema* schema = m_Context->Schemas().Find("World::RigidBody2DComponent");
		if (!schema || !schema->Storage)
			return Fail("requires the RigidBody2DComponent schema to be registered");
		if (!m_Registry.all_of<RigidBody2DComponent>(entity))
			return Fail("requires a 2D rigid body; this entity has no RigidBody2DComponent");
		if (!b2World_IsValid(m_PhysicsWorldId))
			return Fail("requires a running 2D physics world; start the runtime (Scene:OnRuntimeStart) first");
		const RigidBody2DComponent& rigidBody = m_Registry.get<RigidBody2DComponent>(entity);
		if (!b2Body_IsValid(rigidBody.RuntimeBodyId))
			return Fail("has a RigidBody2DComponent but no live Box2D body; the component was not added to the running world");
		if (bodyId) *bodyId = rigidBody.RuntimeBodyId;
		return true;
	}

	void Scene::SyncPhysicsBodyFromTransform(entt::entity entity)
	{
		b2BodyId bodyId = b2_nullBodyId;
		std::string error;
		if (!TryGetPhysicsBody(entity, &bodyId, &error))
			throw std::logic_error("Entity:SyncPhysicsBody " + error);
		auto* transform = m_Registry.try_get<TransformComponent>(entity);
		if (!transform)
			throw std::logic_error("Entity:SyncPhysicsBody requires a TransformComponent");
		b2Body_SetTransform(bodyId, { transform->Location.x, transform->Location.y }, b2MakeRot(transform->Rotation.z));
		b2Body_SetAwake(bodyId, true);
	}

	void Scene::EnsurePhysicsBody(entt::entity entity)
	{
		if (!m_Registry.valid(entity)) return;
		auto* rigidBody = m_Registry.try_get<RigidBody2DComponent>(entity);
		if (!rigidBody) return;
		if (!b2World_IsValid(m_PhysicsWorldId))
		{
			// 世界未启动:AddComponent 只落组件配置;下一次 OnRuntimeStart 会照配置建刚体。
			rigidBody->RuntimeBodyId = b2_nullBodyId;
			return;
		}
		if (BuildRuntimeBody(m_PhysicsWorldId, m_Registry, entity)) return;
		// 已存在的刚体:组件是唯一事实源,类型在脚本侧改过后同步给 Box2D
		// (含 shape 的质量/惯量重算)。Transform 保持 Box2D 的权威状态,不在这里回推。
		if (!b2Body_IsValid(rigidBody->RuntimeBodyId)) return;
		b2BodyType box2dType = b2_staticBody;
		switch (rigidBody->Type)
		{
			case RigidBody2DComponent::BodyType::Static: box2dType = b2_staticBody; break;
			case RigidBody2DComponent::BodyType::Dynamic: box2dType = b2_dynamicBody; break;
			case RigidBody2DComponent::BodyType::Kinematic: box2dType = b2_kinematicBody; break;
		}
		if (b2Body_GetType(rigidBody->RuntimeBodyId) != box2dType)
			b2Body_SetType(rigidBody->RuntimeBodyId, box2dType);
	}

	void Scene::DestroyEntityNow(entt::entity entity)
	{
		if (m_Registry.valid(entity))
		{
			DestroyPhysicsBody(entity);
			if (m_Physics3D) m_Physics3D->DestroyBody(entity);
			m_Registry.destroy(entity);
		}
		m_PendingDestroy.erase(entity);
		m_PendingRemove.erase(entity);
	}

	void Scene::RemoveComponentNow(entt::entity entity, entt::id_type component)
	{
		if (m_Registry.valid(entity) && !IsPendingDestroy(entity))
		{
			Entity target(this, entity);
			std::string reason;
			if (target.HasComponent(component) && target.CanRemoveComponent(component, &reason))
			{
				if (component == entt::type_id<RigidBody2DComponent>().hash()) DestroyPhysicsBody(entity);
				else if (component == entt::type_id<RigidBody3DComponent>().hash()) { if (m_Physics3D) m_Physics3D->DestroyBody(entity); }
				if (auto* storage = m_Registry.storage(component))
				{
					storage->remove(entity);
					NotifyComponentRemoved(target, component);
				}
			}
			else if (!reason.empty()) Report("[Scene] RemoveComponent: " + reason);
		}
		auto it = m_PendingRemove.find(entity);
		if (it != m_PendingRemove.end())
		{
			it->second.erase(component);
			if (it->second.empty()) m_PendingRemove.erase(it);
		}
	}

	void Scene::OnUpdateEditor(Timestep, const EditorCamera&) { AssertOwnerThread(); }
	// 内置帧系统:运行/模拟更新保持"脚本 + 物理"的原有顺序与语义,标记为独占(主线程)。
	// 宿主可再注册 parallel-safe 系统(不得触碰注册表结构),它们会在内置阶段之前并行执行。
	void Scene::EnsureDefaultFrameSystems()
	{
		if (!m_FrameSystemDefinitions.empty())
			return;
		RegisterFrameSystem({ "physics-2d", false, [this](Timestep ts) { OnUpdatePhysics2D(ts); } });
		RegisterFrameSystem({ "physics-3d", false, [this](Timestep ts) { OnUpdatePhysics3D(ts); } });
		RegisterFrameSystem({ "movement-system", false, [this](Timestep ts) { MovementSystem().Update(*this, ts); } });
		RegisterFrameSystem({ "transform-system", false, [this](Timestep ts) { (void)ts; TransformSystem::UpdateWorldTransforms(m_Registry); } });
		RegisterFrameSystem({ "camera-system", false, [this](Timestep ts) { (void)ts; CameraSystem::UpdateAllCameras(m_Registry, m_ViewportWidth, m_ViewportHeight); } });
	}

	void Scene::OnUpdateRuntime(Timestep ts)
	{
		EnsureDefaultFrameSystems();
		// Pure ECS:系统脚本热重载 —— 每帧轮询 scripts/systems/,变化过的文件整份重跑。
		if (ScriptEngine::IsInitialized())
			ScriptEngine::PollSystemScriptReload(*this, ts.GetSeconds());
		RunFrameSystems(ts);
	}

	void Scene::OnUpdateSimulation(Timestep ts, const EditorCamera&)
	{
		EnsureDefaultFrameSystems();
		RunFrameSystems(ts);
	}

	void Scene::OnRuntimeStart()
	{
		AssertOwnerThread();
		if (IsActive()) return;
		// P1b D6:2D/3D 物理同实体互斥 —— 在创建任何物理世界之前校验;失败时场景保持 Stopped
		// 并抛出可读错误(编辑器 Play 捕获后回 Edit,日志里能看到原因)。
		std::string physicsError;
		if (!Physics3DWorld::ValidateScene(*this, &physicsError))
		{
			Report("[Physics3D] " + physicsError);
			throw std::logic_error(physicsError);
		}
		OnPhysics2DStart();
		OnPhysics3DStart();
		EnsureDefaultFrameSystems();
		if (ScriptEngine::IsInitialized())
		{
			ScriptEngine::LoadSystemScripts(*this);
		}
		m_State = SceneState::Running;
	}
	void Scene::OnSimulationStart() { OnRuntimeStart(); }
	void Scene::OnRuntimeStop() { StopScene(); }
	void Scene::OnSimulationStop() { StopScene(); }



	void Scene::StopScene()
	{
		AssertOwnerThread();
		if (m_State == SceneState::Stopping) return;
		m_State = SceneState::Stopping;
		m_StopRequested = false;
		m_Changes.clear();
		// Pure ECS:场景停止时撤销系统脚本注册的帧系统,下一次启动重新装载。
		if (ScriptEngine::IsInitialized())
			ScriptEngine::UnloadSystemScripts(*this);
		OnPhysics2DStop();
		OnPhysics3DStop();
		// Destruction callbacks may request further idempotent deletes/removals, but no general work.
		while (!m_PendingDestroy.empty() || !m_PendingRemove.empty())
		{
			const auto destroys = m_PendingDestroy;
			for (const auto entity : destroys) DestroyEntityNow(entity);
			const auto removals = m_PendingRemove;
			for (const auto& [entity, components] : removals)
				for (const auto component : components) RemoveComponentNow(entity, component);
		}
		m_Changes.clear();
		m_StopRequested = false;
		m_State = SceneState::Stopped;
	}

	void Scene::OnViewportResize(uint32_t width, uint32_t height)
	{
		AssertOwnerThread();
		m_ViewportWidth = width;
		m_ViewportHeight = height;
		CameraSystem::UpdateAllCameras(m_Registry, width, height);
	}

	Entity Scene::GetPrimaryCameraEntity()
	{
		AssertOwnerThread();
		for (const auto entity : m_Registry.view<CameraComponent, TransformComponent>())
			if (!IsPendingDestroy(entity) && !IsPendingRemoval(entity, entt::type_id<CameraComponent>().hash()) &&
				m_Registry.get<CameraComponent>(entity).Primary) return Entity(this, entity);
		return {};
	}

	void Scene::DuplicateEntity(Entity entity)
	{
		AssertStructuralWrite();
		if (!entity.IsValid() || entity.GetScene() != this || IsPendingDestroy(entity)) return;
		if (IsActive() && (entity.HasComponent<RigidBody2DComponent>() || entity.HasComponent<BoxCollider2DComponent>() || entity.HasComponent<CircleCollider2DComponent>()))
			throw std::logic_error("Duplicating physics components requires a stopped scene");
		const std::string name = entity.HasComponent<TagComponent>() ? entity.GetComponent<TagComponent>().Tag : "Empty Entity";
		Entity copy = Entity::CreateEntity(this, name);
		if (entity.HasComponent<TransformComponent>()) copy.AddComponent<TransformComponent>(entity.GetComponent<TransformComponent>());
		for (const Schema::TypeSchema* schema : m_Context->Schemas().List(Schema::TypeCategory::Component))
		{
			if (!schema || !schema->Storage || !schema->Storage->Copy) continue;
			if (entity.HasComponent(schema->Storage->ComponentId))
				schema->Storage->Copy(static_cast<void*>(&copy), static_cast<void*>(&entity));
		}
	}

	void Scene::CopyScene(Ref<Scene>& other, Ref<Scene>& newScene)
	{
		if (!other || !newScene || other == newScene) throw std::logic_error("Scene clone requires distinct source and destination scenes");
		other->AssertOwnerThread();
		newScene->AssertStructuralWrite();
		if (newScene->IsActive()) throw std::logic_error("Scene clone destination must be stopped");
		newScene->m_ViewportHeight = other->m_ViewportHeight;
		newScene->m_ViewportWidth = other->m_ViewportWidth;
		std::unordered_map<UUID, entt::entity> entityMap;
		for (const auto entity : other->m_Registry.view<UUIDComponent>())
		{
			const auto id = other->m_Registry.get<UUIDComponent>(entity).ID;
			const auto* tag = other->m_Registry.try_get<TagComponent>(entity);
			entityMap[id] = Entity::CreateEntity(newScene.get(), tag ? tag->Tag : "Empty Entity", id);
		}
		for (const Schema::TypeSchema* schema : other->m_Context->Schemas().List(Schema::TypeCategory::Component))
		{
			if (!schema || !schema->Storage || !schema->Storage->CopyAll) continue;
			schema->Storage->CopyAll(static_cast<void*>(&newScene->m_Registry), static_cast<void*>(&other->m_Registry), static_cast<const void*>(&entityMap));
		}
		// P4-U13b:prefab 实例注册表跟着场景一起克隆(编辑器 Play 的活动场景就是这份副本),
		// 句柄按 UUID 映射重定向——不复制的话 Play 期间层级/属性面板会看不到实例与覆盖。
		newScene->m_PrefabInstances.clear();
		for (const Gameplay::PrefabInstanceRecord& source : other->m_PrefabInstances)
		{
			const auto* sourceIdentity = other->m_Registry.try_get<UUIDComponent>(source.Root);
			if (!sourceIdentity)
				continue;
			const auto rootIt = entityMap.find(sourceIdentity->ID);
			if (rootIt == entityMap.end())
				continue;
			Gameplay::PrefabInstanceRecord& record =
				newScene->AddPrefabInstance(source.PrefabPath, rootIt->second);
			for (const auto& [handle, fields] : source.Overrides)
			{
				const auto* identity = other->m_Registry.try_get<UUIDComponent>(static_cast<entt::entity>(handle));
				if (!identity)
					continue;
				const auto entityIt = entityMap.find(identity->ID);
				if (entityIt == entityMap.end())
					continue;
				record.Overrides[static_cast<uint32_t>(entityIt->second)] = fields;
			}
		}
	}

	void Scene::OnPhysics2DStart()
	{
		if (b2World_IsValid(m_PhysicsWorldId)) return;
		b2WorldDef worldDef = b2DefaultWorldDef();
		worldDef.gravity = { 0.0f, -9.8f };
		worldDef.restitutionThreshold = 0.5f;
		m_PhysicsWorldId = b2CreateWorld(&worldDef);
		for (const auto entity : m_Registry.view<RigidBody2DComponent>())
		{
			auto& rb = m_Registry.get<RigidBody2DComponent>(entity);
			rb.RuntimeBodyId = b2_nullBodyId;
			// W3f:创建逻辑抽到 BuildRuntimeBody,与运行时 AddComponent 补建共用同一套形状/质量口径。
			BuildRuntimeBody(m_PhysicsWorldId, m_Registry, entity);
		}
	}

	void Scene::OnUpdatePhysics2D(Timestep ts)
	{
		if (!b2World_IsValid(m_PhysicsWorldId)) return;
		b2World_Step(m_PhysicsWorldId, ts.GetSeconds(), 4);
		for (const auto entity : m_Registry.view<RigidBody2DComponent>())
		{
			if (IsPendingDestroy(entity) || IsPendingRemoval(entity, entt::type_id<RigidBody2DComponent>().hash())) continue;
			auto& rb = m_Registry.get<RigidBody2DComponent>(entity);
			auto* transform = m_Registry.try_get<TransformComponent>(entity);
			if (!transform || !b2Body_IsValid(rb.RuntimeBodyId))
			{
				Report("[Physics] Skipping invalid body or missing Transform entity=" + std::to_string(static_cast<uint32_t>(entity)));
				continue;
			}
			const b2Transform body = b2Body_GetTransform(rb.RuntimeBodyId);
			transform->SetTransform({ body.p.x, body.p.y, transform->Location.z },
				{ transform->Rotation.x, transform->Rotation.y, b2Rot_GetAngle(body.q) }, transform->Scale);
		}
	}

	void Scene::OnPhysics2DStop()
	{
		if (b2World_IsValid(m_PhysicsWorldId)) b2DestroyWorld(m_PhysicsWorldId);
		m_PhysicsWorldId = b2_nullWorldId;
		for (const auto entity : m_Registry.view<RigidBody2DComponent>())
			m_Registry.get<RigidBody2DComponent>(entity).RuntimeBodyId = b2_nullBodyId;
	}

	// ---- P1b D6:3D 物理(Jolt) ----
	void Scene::AddPhysics3DContactCallback(std::function<void(bool added, entt::entity entityA, entt::entity entityB)> callback)
	{
		AssertOwnerThread();
		if (!callback) throw std::invalid_argument("Physics3D contact callback must be callable");
		m_Physics3DContactCallbacks.push_back(std::move(callback));
	}

	void Scene::ClearPhysics3DContactCallbacks()
	{
		AssertOwnerThread();
		m_Physics3DContactCallbacks.clear();
	}

	bool Scene::SyncPhysics3DTransform(entt::entity entity, const glm::vec3& location, const glm::quat& rotation)
	{
		AssertOwnerThread();
		if (!m_Registry.valid(entity)) return false;
		if (IsPendingDestroy(entity) || IsPendingRemoval(entity, entt::type_id<RigidBody3DComponent>().hash())) return false;
		auto* transform = m_Registry.try_get<TransformComponent>(entity);
		if (!transform) return false;
		// 位姿变化才写:位置用绝对容差、旋转用四元数点积(避免浮点噪声导致每帧重算 Transform 矩阵、
		// 打断层级/标脏)。
		const bool locationChanged = glm::distance(transform->Location, location) > 1e-4f;
		const bool rotationChanged = std::fabs(glm::dot(transform->RotationQuat, rotation)) < 1.0f - 1e-4f;
		if (!locationChanged && !rotationChanged) return false;
		transform->Location = location;
		transform->RotationQuat = rotation;
		transform->Rotation = glm::eulerAngles(rotation);
		transform->RecalculateTransform();
		return true;
	}

	void Scene::OnPhysics3DStart()
	{
		if (m_Physics3D) return;
		auto world = std::make_unique<Physics3DWorld>();
		// 引擎侧钩子表:物理步进里的回调只做转发,钩子抛异常不能带走物理步进。
		world->SetContactCallback([this](bool added, entt::entity entityA, entt::entity entityB)
		{
			for (std::size_t index = 0; index < m_Physics3DContactCallbacks.size(); ++index)
			{
				const auto& hook = m_Physics3DContactCallbacks[index];
				if (!hook) continue;
				try { hook(added, entityA, entityB); }
				catch (const std::exception& error) { Report(std::string("[Physics3D] contact hook failed: ") + error.what()); }
				catch (...) { Report("[Physics3D] contact hook failed: unknown exception"); }
			}
		});
		world->Start(*this);
		m_Physics3D = std::move(world);
	}

	void Scene::OnUpdatePhysics3D(Timestep ts)
	{
		if (!m_Physics3D) return;
		m_Physics3D->Step(ts.GetSeconds());
		m_Physics3D->SyncTransforms();
	}

	void Scene::OnPhysics3DStop()
	{
		if (!m_Physics3D) return;
		m_Physics3D->Stop();
		m_Physics3D.reset();
	}

	uint64_t Scene::AddComponentObserver(entt::id_type componentId, std::function<void(Entity)> onAdd, std::function<void(Entity)> onRemove)
	{
		AssertOwnerThread();
		const uint64_t id = ++m_NextObserverId;
		m_ComponentObservers.push_back({ id, componentId, std::move(onAdd), std::move(onRemove) });
		return id;
	}

	void Scene::RemoveComponentObserver(uint64_t observerId)
	{
		AssertOwnerThread();
		auto it = std::remove_if(m_ComponentObservers.begin(), m_ComponentObservers.end(),
			[observerId](const ComponentObserverEntry& entry) {
				return entry.Id == observerId;
			});
		m_ComponentObservers.erase(it, m_ComponentObservers.end());
	}

	void Scene::NotifyComponentAdded(Entity entity, entt::id_type componentId)
	{
		AssertOwnerThread();
		std::vector<std::function<void(Entity)>> callbacks;
		callbacks.reserve(m_ComponentObservers.size());
		for (const auto& observer : m_ComponentObservers)
		{
			if (observer.ComponentId == componentId && observer.OnAdd)
				callbacks.push_back(observer.OnAdd);
		}
		for (const auto& cb : callbacks)
		{
			cb(entity);
		}
	}

	void Scene::NotifyComponentRemoved(Entity entity, entt::id_type componentId)
	{
		AssertOwnerThread();
		std::vector<std::function<void(Entity)>> callbacks;
		callbacks.reserve(m_ComponentObservers.size());
		for (const auto& observer : m_ComponentObservers)
		{
			if (observer.ComponentId == componentId && observer.OnRemove)
				callbacks.push_back(observer.OnRemove);
		}
		for (const auto& cb : callbacks)
		{
			cb(entity);
		}
	}
}
