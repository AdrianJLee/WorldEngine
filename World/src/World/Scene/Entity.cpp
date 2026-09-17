#include "wldpch.h"
#include "Entity.h"
#include "World/Scene/Components.h"

namespace World
{
	namespace
	{
		bool Reject(std::string* reason, const char* message)
		{
			if (reason) *reason = message;
			return false;
		}

		const Schema::TypeSchema* FindComponentSchema(Scene* scene, entt::id_type id)
		{
			const Schema::TypeSchema* schema = scene->GetContext().Schemas().FindByComponentId(static_cast<uint32_t>(id));
			return (schema && schema->Storage) ? schema : nullptr;
		}
	}

	Entity Entity::CreateEntity(Scene* scene, const std::string& name, const UUID& id)
	{
		if (!scene) throw std::logic_error("Cannot create an entity without a scene");
		scene->AssertStructuralWrite();
		Entity entity(scene, scene->m_Registry.create());
		entity.AddComponent<TagComponent>(name);
		entity.AddComponent<UUIDComponent>(id);
		return entity;
	}

	void Entity::DestroyEntity(Scene* scene, Entity entity)
	{
		// Inspect the weak token before touching the caller's possibly stale Scene pointer.
		if (!entity.IsValid()) return;
		if (scene != entity.m_Scene) throw std::logic_error("Entity belongs to a different scene");
		scene->RequestDestroy(entity.m_EntityHandle);
	}

	Entity::Entity(Scene* scene, entt::entity handle)
		: m_EntityHandle(handle), m_Scene(scene), m_Lifetime(scene ? scene->m_Lifetime : std::weak_ptr<const uint8_t> {})
	{}

	bool Entity::IsValid() const
	{
		if (!m_Scene || m_EntityHandle == entt::null || m_Lifetime.expired()) return false;
		m_Scene->AssertOwnerThread();
		return m_Scene->m_Registry.valid(m_EntityHandle);
	}

	void Entity::RequireValid() const
	{
		if (!IsValid()) throw std::logic_error("Entity handle is invalid or its scene has expired");
	}

	bool Entity::HasComponent(entt::id_type componentId) const
	{
		RequireValid();
		auto* storage = m_Scene->m_Registry.storage(componentId);
		return storage && storage->contains(m_EntityHandle);
	}

	void* Entity::GetComponent(entt::id_type componentId)
	{
		RequireValid();
		auto* storage = m_Scene->m_Registry.storage(componentId);
		return storage && storage->contains(m_EntityHandle) ? storage->value(m_EntityHandle) : nullptr;
	}

	bool Entity::CheckAdd(entt::id_type component, bool replace, bool requireDependencies, std::string* reason) const
	{
		if (reason) reason->clear();
		if (!IsValid()) return Reject(reason, "Entity is no longer valid");
		if (m_Scene->IsPendingDestroy(m_EntityHandle)) return Reject(reason, "Entity is pending destruction");
		if (m_Scene->m_State == SceneState::Stopping) return Reject(reason, "Scene is stopping");
		if (m_Scene->IsPendingRemoval(m_EntityHandle, component)) return Reject(reason, "Component removal has not been committed yet");
		const bool exists = HasComponent(component);
		if (exists && !replace) return Reject(reason, "Entity already has this component");
		const bool script = component == entt::type_id<NativeScriptComponent>().hash() || component == entt::type_id<LuaScriptComponent>().hash();
		if (m_Scene->IsActive() && exists && script)
			return Reject(reason, "Stop the scene or remove the old script before replacing its configuration");
		const bool physics = component == entt::type_id<RigidBody2DComponent>().hash() ||
			component == entt::type_id<BoxCollider2DComponent>().hash() || component == entt::type_id<CircleCollider2DComponent>().hash();
		// W3f 例外:脚本生命周期回调内新增刚性体/碰撞体允许同步提交并立即补建 Box2D 刚体
		// (与 W3d 纯数据组件同一条 ScriptWriteScope 白名单路径);其它活动场景入口保持既有拒绝。
		const bool physicsRuntimeAdd = physics && m_Scene->IsActive() &&
			(m_Scene->IsInsideScriptCallback() || m_Scene->IsInsideScriptWriteScope());
		if (m_Scene->IsActive() && physics && !physicsRuntimeAdd)
			return Reject(reason, "Adding or replacing physics components requires a stopped scene");
		if (requireDependencies && (physics || component == entt::type_id<CameraComponent>().hash()) && !HasComponent<TransformComponent>())
			return Reject(reason, "This component requires a Transform component");
		return true;
	}

	bool Entity::CanAddComponent(entt::id_type component, std::string* reason) const
	{
		return CheckAdd(component, false, true, reason);
	}

	void Entity::RequireCanAdd(entt::id_type component, bool replace) const
	{
		std::string reason;
		// Stopped typed additions also serve the existing serializer, whose component
		// registration order is not a dependency order. UI/dynamic additions stay strict.
		if (!CheckAdd(component, replace, m_Scene->IsActive(), &reason)) throw std::logic_error(reason);
	}

	bool Entity::CanRemoveComponent(entt::id_type component, std::string* reason) const
	{
		if (reason) reason->clear();
		if (!IsValid()) return Reject(reason, "Entity is no longer valid");
		if (!HasComponent(component)) return Reject(reason, "Entity does not have this component");
		if (component == entt::type_id<TransformComponent>().hash() &&
			(HasComponent<CameraComponent>() || HasComponent<RigidBody2DComponent>() ||
				HasComponent<BoxCollider2DComponent>() || HasComponent<CircleCollider2DComponent>()))
			return Reject(reason, "Remove the camera, rigid body and colliders before removing Transform");
		if (m_Scene->IsActive() && HasComponent<RigidBody2DComponent>() &&
			(component == entt::type_id<BoxCollider2DComponent>().hash() || component == entt::type_id<CircleCollider2DComponent>().hash()))
			return Reject(reason, "Removing a collider from a running rigid body requires a stopped scene");
		return true;
	}

	void Entity::AddComponent(entt::id_type componentId, const void* data)
	{
		RequireValid();
		const bool physicsComponent = componentId == entt::type_id<RigidBody2DComponent>().hash() ||
			componentId == entt::type_id<BoxCollider2DComponent>().hash() ||
			componentId == entt::type_id<CircleCollider2DComponent>().hash();
		std::string reason;
		if (!CheckAdd(componentId, false, true, &reason)) throw std::logic_error(reason);
		const Schema::TypeSchema* schema = FindComponentSchema(m_Scene, componentId);
		if (!schema || !schema->Storage || !schema->Storage->Add) throw std::logic_error("Type is not a registered component");
		// W3f:脚本回调内或 ScriptWriteScope(结构提交点白名单)内的新增走下面的同步路径
		// (当帧可见);其余情况保持既有延迟语义。
		if (!data && m_Scene->m_CallbackDepth && !m_Scene->IsInsideScriptCallback() &&
			!m_Scene->IsInsideScriptWriteScope())
		{
			Entity target = *this;
			if (!m_Scene->DeferStructuralChange([target, componentId](Scene&) mutable
				{
					if (target.IsValid()) target.AddComponent(componentId);
				})) throw std::logic_error("Scene rejected the component addition request");
			return;
		}
		// W3f:活动场景里的物理组件同步新增只允许在脚本回调内(CheckAdd 已放行),
		// 并且新增后立即补建 Box2D 刚体;碰撞体要求同实体已有 RigidBody2DComponent。
		// 抛异常的位置都放在 ScriptWriteScope 之外,避免把异常从析构路径带出去。
		auto* componentStorage = data ? m_Scene->m_Registry.storage(componentId) : nullptr;
		if (data && !componentStorage) throw std::logic_error("Data-bearing component addition requires existing storage");
		const bool physicsRuntimeAdd = physicsComponent && m_Scene->IsActive();
		if (physicsRuntimeAdd && !m_Scene->IsInsideScriptCallback() && !m_Scene->IsInsideScriptWriteScope())
			throw std::logic_error("Adding physics components at runtime is only allowed inside script callbacks or an explicit script write scope");
		if (physicsRuntimeAdd && componentId != entt::type_id<RigidBody2DComponent>().hash())
		{
			const Schema::TypeSchema* bodySchema = FindComponentSchema(m_Scene, entt::type_id<RigidBody2DComponent>().hash());
			auto* bodyStorage = bodySchema && bodySchema->Storage
				? m_Scene->m_Registry.storage(bodySchema->Storage->ComponentId)
				: nullptr;
			if (!bodyStorage || !bodyStorage->contains(m_EntityHandle))
				throw std::logic_error("Adding a 2D collider at runtime requires the entity to already have a RigidBody2DComponent");
		}

		if (physicsRuntimeAdd)
		{
			// ScriptWriteScope 放行脚本回调内的同步结构写(与 W3d 纯数据组件同一条路径)。
			Scene::ScriptWriteScope scope(*m_Scene);
			if (data) componentStorage->push(m_EntityHandle, data);
			else schema->Storage->Add(static_cast<void*>(this));
			m_Scene->EnsurePhysicsBody(m_EntityHandle);
			return;
		}

		m_Scene->AssertStructuralWrite();
		if (data) componentStorage->push(m_EntityHandle, data);
		else schema->Storage->Add(static_cast<void*>(this));
	}

	void Entity::RemoveComponent(entt::id_type componentId)
	{
		if (!IsValid() || !HasComponent(componentId)) return;
		m_Scene->RequestRemove(m_EntityHandle, componentId);
	}

}
