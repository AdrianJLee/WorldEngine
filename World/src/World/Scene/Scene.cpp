#include "wldpch.h"
#include "Scene.h"
#include "World/Scene/Components.h"
#include "World/Scene/ScriptEngine.h"
#include "World/Core/Thread/JobSystem.h"
#include "World/Gameplay/SystemRegistry.h"
#include <box2d/box2d.h>
#include <chrono>
#include <stdexcept>

namespace World
{
	namespace
	{
		template<typename T>
		std::vector<entt::entity> Snapshot(entt::registry& registry)
		{
			auto view = registry.view<T>();
			return { view.begin(), view.end() };
		}

		std::string NativeError(const NativeScriptComponent& script, entt::entity entity, const char* phase, const char* error)
		{
			return "[Native] " + script.ScriptName + " entity=" + std::to_string(static_cast<uint32_t>(entity)) +
				" phase=" + phase + ": " + error;
		}

		void Report(const std::string& error)
		{
			if (Log::GetCoreLogger()) WLD_CORE_ERROR("{0}", error);
		}
	}

	Scene::Scene(WorldContext& context) : m_Context(&context), m_OwnerThread(std::this_thread::get_id()) {}

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

	// W5-3:帧系统调度统一走 Gameplay::SystemRegistry
	// (阶段/同阶段依赖/并行安全并行派发/逐系统耗时),Scene 只保留面向宿主的薄封装。
	void Scene::RunFrameSystems(Timestep ts)
	{
		AssertOwnerThread();
		m_FrameSystemTimings.clear();
		if (!m_FrameSystems)
			return;

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
		if (m_CallbackDepth)
			throw std::logic_error("Synchronous structural writes are forbidden in lifecycle callbacks; use DeferStructuralChange");
		if (m_State == SceneState::Stopping || (IsActive() && !m_Committing))
			throw std::logic_error("An active scene accepts structural writes only while committing a command");
	}

	entt::registry& Scene::GetRegistry() { AssertStructuralWrite(); return m_Registry; }
	const entt::registry& Scene::GetRegistry() const { AssertOwnerThread(); return m_Registry; }

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
		if (source.Component == entt::type_id<NativeScriptComponent>().hash())
		{
			auto* script = m_Registry.try_get<NativeScriptComponent>(source.EntityHandle);
			return script && script->Generation == source.Generation && script->State == ScriptInstanceState::Running;
		}
		if (source.Component == entt::type_id<LuaScriptComponent>().hash())
		{
			auto* script = m_Registry.try_get<LuaScriptComponent>(source.EntityHandle);
			return script && script->Generation == source.Generation && script->State == ScriptInstanceState::Running;
		}
		return false;
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

	void Scene::FlushStructuralChanges()
	{
		AssertOwnerThread();
		if (m_CallbackDepth || m_Committing) throw std::logic_error("Structural changes cannot be recursively committed");
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
		std::string scriptName;
		if (m_Registry.valid(source.EntityHandle))
		{
			if (source.Component == entt::type_id<NativeScriptComponent>().hash())
			{
				if (auto* script = m_Registry.try_get<NativeScriptComponent>(source.EntityHandle)) scriptName = script->ScriptName;
			}
			else if (source.Component == entt::type_id<LuaScriptComponent>().hash())
			{
				if (auto* script = m_Registry.try_get<LuaScriptComponent>(source.EntityHandle)) scriptName = script->ScriptFilePath;
			}
		}
		const std::string message = "[Scene] " + scriptName + " entity=" + std::to_string(static_cast<uint32_t>(source.EntityHandle)) +
			" phase=StructuralChange: " + error;
		Report(message);
		if (!m_Registry.valid(source.EntityHandle)) return;
		if (source.Component == entt::type_id<NativeScriptComponent>().hash())
		{
			auto* script = m_Registry.try_get<NativeScriptComponent>(source.EntityHandle);
			if (script && script->Generation == source.Generation)
			{
				script->LastError = message;
				DestroyNativeScript(source.EntityHandle, true);
			}
		}
		else if (source.Component == entt::type_id<LuaScriptComponent>().hash())
		{
			auto* script = m_Registry.try_get<LuaScriptComponent>(source.EntityHandle);
			if (script && script->Generation == source.Generation)
			{
				script->LastError = message;
				DestroyLuaScript(source.EntityHandle, true);
			}
		}
	}

	void Scene::StartPendingScripts()
	{
		for (const auto entity : Snapshot<NativeScriptComponent>(m_Registry))
		{
			if (m_StopRequested) break;
			if (!m_Registry.valid(entity) || IsPendingDestroy(entity) || IsPendingRemoval(entity, entt::type_id<NativeScriptComponent>().hash())) continue;
			auto& script = m_Registry.get<NativeScriptComponent>(entity);
			if (script.State != ScriptInstanceState::Pending) continue;
			script.Generation = ++m_NextGeneration;
			script.State = ScriptInstanceState::Creating;
			script.LastError.clear();
			const ScriptSource source{ entity, entt::type_id<NativeScriptComponent>().hash(), script.Generation };
			try
			{
				InvokeCallback(source, [&] {
					const Schema::TypeSchema* type = m_Context ? m_Context->Schemas().Find(script.ScriptName) : nullptr;
					if (!script.InstantiateScript && type && type->Script)
						type->Script->Bind(static_cast<void*>(&script));
					if (!script.InstantiateScript && script.ScriptName.empty()) { script.State = ScriptInstanceState::Stopped; return; }
					if (!script.InstantiateScript || !script.DestroyScript) throw std::logic_error("Native script requires paired instantiate/destroy functions");
					script.Instance = script.InstantiateScript();
					if (!script.Instance) throw std::runtime_error("Native script factory returned null");
					script.Instance->m_Entity = Entity(this, entity);
					if (type)
					{
						for (const Schema::FieldSchema& field : type->Fields)
						{
							const auto it = script.FieldValues.find(field.Name);
							if (it != script.FieldValues.end())
								field.Set(dynamic_cast<void*>(script.Instance), it->second);
						}
					}
					script.CreateEntered = true;
					script.Instance->OnCreate();
					script.State = ScriptInstanceState::Running;
				});
			}
			catch (const std::exception& error) { script.LastError = NativeError(script, entity, "OnCreate", error.what()); Report(script.LastError); DestroyNativeScript(entity, true); }
			catch (...) { script.LastError = NativeError(script, entity, "OnCreate", "Unknown exception"); Report(script.LastError); DestroyNativeScript(entity, true); }
		}
		for (const auto entity : Snapshot<LuaScriptComponent>(m_Registry))
		{
			if (m_StopRequested) break;
			if (!m_Registry.valid(entity) || IsPendingDestroy(entity) || IsPendingRemoval(entity, entt::type_id<LuaScriptComponent>().hash())) continue;
			auto& script = m_Registry.get<LuaScriptComponent>(entity);
			if (script.State != ScriptInstanceState::Pending) continue;
			script.Generation = ++m_NextGeneration;
			const ScriptSource source{ entity, entt::type_id<LuaScriptComponent>().hash(), script.Generation };
			try { InvokeCallback(source, [&] { ScriptEngine::OnCreateScript(script, Entity(this, entity)); }); }
			catch (const std::exception& error) { script.LastError = error.what(); script.State = ScriptInstanceState::Faulted; Report(script.LastError); }
			catch (...) { script.LastError = "Unknown Lua creation exception"; script.State = ScriptInstanceState::Faulted; Report(script.LastError); }
			if (script.State == ScriptInstanceState::Faulted) DestroyLuaScript(entity, true);
		}
	}

	void Scene::DestroyNativeScript(entt::entity entity, bool faulted)
	{
		auto* script = m_Registry.try_get<NativeScriptComponent>(entity);
		if (!script || script->State == ScriptInstanceState::Destroying) return;
		faulted = faulted || script->State == ScriptInstanceState::Faulted;
		script->State = ScriptInstanceState::Destroying;
		const ScriptSource source{ entity, entt::type_id<NativeScriptComponent>().hash(), script->Generation, true };
		try
		{
			if (script->Instance && script->CreateEntered)
			{
				script->CreateEntered = false;
				InvokeCallback(source, [&] { script->Instance->OnDestroy(); });
			}
		}
		catch (const std::exception& error) { script->LastError += "\n" + NativeError(*script, entity, "OnDestroy", error.what()); Report(script->LastError); faulted = true; }
		catch (...) { script->LastError += "\n" + NativeError(*script, entity, "OnDestroy", "Unknown exception"); Report(script->LastError); faulted = true; }
		try
		{
			if (script->Instance && script->DestroyScript)
				InvokeCallback(source, [&] { script->DestroyScript(script->Instance); });
		}
		catch (const std::exception& error) { script->LastError += "\n" + NativeError(*script, entity, "Release", error.what()); Report(script->LastError); faulted = true; }
		catch (...) { Report("Native script release threw an unknown exception"); faulted = true; }
		script->Instance = nullptr;
		script->CreateEntered = false;
		script->State = faulted ? ScriptInstanceState::Faulted : ScriptInstanceState::Stopped;
	}

	void Scene::DestroyLuaScript(entt::entity entity, bool faulted)
	{
		auto* script = m_Registry.try_get<LuaScriptComponent>(entity);
		if (!script || script->State == ScriptInstanceState::Destroying) return;
		if (faulted) script->State = ScriptInstanceState::Faulted;
		const ScriptSource source{ entity, entt::type_id<LuaScriptComponent>().hash(), script->Generation, true };
		InvokeCallback(source, [&] { ScriptEngine::OnDestroyScript(*script); });
	}

	void Scene::DestroyPhysicsBody(entt::entity entity)
	{
		if (auto* body = m_Registry.try_get<RigidBody2DComponent>(entity))
		{
			if (b2Body_IsValid(body->RuntimeBodyId)) b2DestroyBody(body->RuntimeBodyId);
			body->RuntimeBodyId = b2_nullBodyId;
		}
	}

	void Scene::DestroyEntityNow(entt::entity entity)
	{
		if (m_Registry.valid(entity))
		{
			DestroyNativeScript(entity);
			DestroyLuaScript(entity);
			DestroyPhysicsBody(entity);
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
				if (component == entt::type_id<NativeScriptComponent>().hash()) DestroyNativeScript(entity);
				else if (component == entt::type_id<LuaScriptComponent>().hash()) DestroyLuaScript(entity);
				else if (component == entt::type_id<RigidBody2DComponent>().hash()) DestroyPhysicsBody(entity);
				if (auto* storage = m_Registry.storage(component)) storage->remove(entity);
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
		RegisterFrameSystem({ "scene-update", false, [this](Timestep ts) { OnScriptUpdate(ts); } });
	}

	void Scene::OnUpdateRuntime(Timestep ts)
	{
		EnsureDefaultFrameSystems();
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
		OnPhysics2DStart();
		OnScriptStart();
	}
	void Scene::OnSimulationStart() { OnRuntimeStart(); }
	void Scene::OnRuntimeStop() { OnScriptDestroy(); }
	void Scene::OnSimulationStop() { OnScriptDestroy(); }

	void Scene::OnScriptStart()
	{
		AssertOwnerThread();
		if (IsActive()) return;
		if (m_CallbackDepth || m_Committing) throw std::logic_error("Scene cannot start inside a callback or structural command");
		m_State = SceneState::Starting;
		m_StopRequested = false;
		for (const auto entity : Snapshot<NativeScriptComponent>(m_Registry))
		{
			auto& script = m_Registry.get<NativeScriptComponent>(entity);
			script.State = ScriptInstanceState::Pending;
			script.LastError.clear();
		}
		for (const auto entity : Snapshot<LuaScriptComponent>(m_Registry))
		{
			auto& script = m_Registry.get<LuaScriptComponent>(entity);
			script.State = ScriptInstanceState::Pending;
			script.LastError.clear();
		}
		StartPendingScripts();
		if (m_StopRequested) StopScene();
		else m_State = SceneState::Running;
	}

	void Scene::OnScriptUpdate(Timestep ts)
	{
		AssertOwnerThread();
		if (m_CallbackDepth || m_Committing) throw std::logic_error("Scene updates cannot be reentrant");
		if (!IsRunning()) return;
		FlushStructuralChanges();
		if (!IsRunning()) return;
		// Only instances already running at this update boundary may receive OnUpdate.
		std::vector<entt::entity> native, lua;
		for (const auto entity : Snapshot<NativeScriptComponent>(m_Registry))
			if (m_Registry.get<NativeScriptComponent>(entity).State == ScriptInstanceState::Running) native.push_back(entity);
		for (const auto entity : Snapshot<LuaScriptComponent>(m_Registry))
			if (m_Registry.get<LuaScriptComponent>(entity).State == ScriptInstanceState::Running) lua.push_back(entity);
		StartPendingScripts();
		if (m_StopRequested) { StopScene(); return; }
		OnUpdatePhysics2D(ts);
		UpdateScriptSnapshot(ts, native, lua);
		if (m_StopRequested) StopScene();
		else FlushStructuralChanges();
	}

	void Scene::UpdateScriptSnapshot(Timestep ts, const std::vector<entt::entity>& native, const std::vector<entt::entity>& lua)
	{
		for (const auto entity : native)
		{
			if (m_StopRequested) break;
			if (!m_Registry.valid(entity) || IsPendingDestroy(entity) || IsPendingRemoval(entity, entt::type_id<NativeScriptComponent>().hash())) continue;
			auto* script = m_Registry.try_get<NativeScriptComponent>(entity);
			if (!script || script->State != ScriptInstanceState::Running || !script->Instance) continue;
			const ScriptSource source{ entity, entt::type_id<NativeScriptComponent>().hash(), script->Generation };
			try { InvokeCallback(source, [&] { script->Instance->OnUpdate(ts); }); }
			catch (const std::exception& error) { script->LastError = NativeError(*script, entity, "OnUpdate", error.what()); Report(script->LastError); DestroyNativeScript(entity, true); }
			catch (...) { script->LastError = NativeError(*script, entity, "OnUpdate", "Unknown exception"); Report(script->LastError); DestroyNativeScript(entity, true); }
		}
		for (const auto entity : lua)
		{
			if (m_StopRequested) break;
			if (!m_Registry.valid(entity) || IsPendingDestroy(entity) || IsPendingRemoval(entity, entt::type_id<LuaScriptComponent>().hash())) continue;
			auto* script = m_Registry.try_get<LuaScriptComponent>(entity);
			if (!script || script->State != ScriptInstanceState::Running) continue;
			const ScriptSource source{ entity, entt::type_id<LuaScriptComponent>().hash(), script->Generation };
			try { InvokeCallback(source, [&] { ScriptEngine::OnUpdateScript(*script, ts); }); }
			catch (const std::exception& error) { script->LastError = error.what(); script->State = ScriptInstanceState::Faulted; Report(script->LastError); }
			catch (...) { script->LastError = "Unknown Lua update exception"; script->State = ScriptInstanceState::Faulted; Report(script->LastError); }
			if (script->State == ScriptInstanceState::Faulted) DestroyLuaScript(entity, true);
		}
	}

	void Scene::OnScriptDestroy()
	{
		AssertOwnerThread();
		if (m_CallbackDepth || m_Committing) { m_StopRequested = true; return; }
		StopScene();
	}

	void Scene::StopScene()
	{
		AssertOwnerThread();
		if (m_State == SceneState::Stopping) return;
		m_State = SceneState::Stopping;
		m_StopRequested = false;
		// Never instantiate Pending scripts during stop. Keep component storage readable in OnDestroy.
		m_Changes.clear();
		for (const auto entity : Snapshot<NativeScriptComponent>(m_Registry)) DestroyNativeScript(entity);
		for (const auto entity : Snapshot<LuaScriptComponent>(m_Registry)) DestroyLuaScript(entity);
		OnPhysics2DStop();
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
		for (const auto entity : m_Registry.view<CameraComponent>())
		{
			auto& camera = m_Registry.get<CameraComponent>(entity);
			if (!camera.FixedAspectRatio && width && height) camera.Camera.SetViewportSize(width, height);
		}
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
			auto* transform = m_Registry.try_get<TransformComponent>(entity);
			if (!transform) { Report("[Physics] Missing Transform for rigid body entity=" + std::to_string(static_cast<uint32_t>(entity))); continue; }
			b2BodyDef bodyDef = b2DefaultBodyDef();
			switch (rb.Type)
			{
				case RigidBody2DComponent::BodyType::Static: bodyDef.type = b2_staticBody; break;
				case RigidBody2DComponent::BodyType::Dynamic: bodyDef.type = b2_dynamicBody; break;
				case RigidBody2DComponent::BodyType::Kinematic: bodyDef.type = b2_kinematicBody; break;
			}
			bodyDef.position = { transform->Location.x, transform->Location.y };
			bodyDef.rotation = b2MakeRot(transform->Rotation.z);
			bodyDef.motionLocks.angularZ = rb.FixedRotation;
			rb.RuntimeBodyId = b2CreateBody(m_PhysicsWorldId, &bodyDef);
			if (const auto* box = m_Registry.try_get<BoxCollider2DComponent>(entity))
			{
				b2ShapeDef shapeDef = b2DefaultShapeDef();
				shapeDef.density = box->Density;
				shapeDef.material.friction = box->Friction;
				shapeDef.material.restitution = box->Restitution;
				b2Polygon polygon = b2MakeOffsetBox(transform->Scale.x * box->Size.x, transform->Scale.y * box->Size.y,
					{ box->Offset.x, box->Offset.y }, b2MakeRot(0.0f));
				b2CreatePolygonShape(rb.RuntimeBodyId, &shapeDef, &polygon);
			}
			if (const auto* circle = m_Registry.try_get<CircleCollider2DComponent>(entity))
			{
				b2Circle shape;
				shape.center = { circle->Offset.x, circle->Offset.y };
				shape.radius = circle->Radius * transform->Scale.x;
				b2ShapeDef shapeDef = b2DefaultShapeDef();
				shapeDef.density = circle->Density;
				shapeDef.material.friction = circle->Friction;
				shapeDef.material.restitution = circle->Restitution;
				b2CreateCircleShape(rb.RuntimeBodyId, &shapeDef, &shape);
			}
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
}
