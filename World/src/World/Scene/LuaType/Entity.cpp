#include "wldpch.h"
#include "World/Scene/ScriptEngine.h"
#include "World/Scene/Entity.h"
#include "World/Scene/Components.h"
#include "World/Scene/Hierarchy.h"
#include "World/Core/WorldContext.h"
#include "World/Gameplay/Prefab.h"
#include "World/Script/BindComponentAccess.h"
#include "World/Script/LuauVm.h"
#include "World/Script/ScriptBindingContext.h"
#include "World/Script/ScriptValue.h"
#include "LuaTypeHelpers.h"

#include <box2d/box2d.h>
#include <filesystem>
#include <new>
#include <stdexcept>
#include <string>

namespace World
{
	using namespace LuaTypeDetail;

	namespace
	{
		void RequireEntity(const Entity& entity, const char* operation)
		{
			ScriptEngine::AssertOwnerThread();
			// IsValid checks the weak scene lifetime before dereferencing its Scene*.
			if (!entity.IsValid())
				throw std::logic_error(std::string("Entity:") + operation + " on invalid/expired handle " + std::to_string(static_cast<uint32_t>(entity)));
		}

		const Schema::TypeSchema& RequireComponentType(const Entity& entity, const std::string& name, const char* operation)
		{
			const Schema::TypeSchema* type = entity.GetScene()->GetContext().Schemas().Find(name);
			if (!type || type->Category != Schema::TypeCategory::Component || !type->Storage)
				throw std::logic_error(std::string("Entity:") + operation + " requires a registered component type; got '" + name + "'");
			return *type;
		}

		Entity* Receiver(ScriptBindingContext& bindings, const ScriptValue* args, std::size_t count, const char* operation)
		{
			Entity* self = nullptr;
			if (count < 1 || !bindings.Unwrap<Entity>("Entity", args[0], &self) || !self)
				throw std::logic_error(std::string("Entity:") + operation + " requires an Entity receiver");
			return self;
		}

		// W3d 白名单:回调内同步增删无运行时副作用的纯数据组件。
		// 脚本/2D 物理组件不在表内,仍走既有的延迟提交/拒绝规则。
		bool IsSynchronousScriptComponent(entt::id_type component)
		{
			return component == entt::type_id<TransformComponent>().hash()
				|| component == entt::type_id<SpriteComponent>().hash()
				|| component == entt::type_id<CircleRendererComponent>().hash()
				|| component == entt::type_id<MeshRendererComponent>().hash()
				|| component == entt::type_id<CameraComponent>().hash()
				|| component == entt::type_id<HierarchyComponent>().hash();
		}

		// W3f:2D 物理组件在脚本回调内也允许同步新增(实体层补建 Box2D 刚体)。
		bool IsPhysicsComponent(entt::id_type component)
		{
			return component == entt::type_id<RigidBody2DComponent>().hash()
				|| component == entt::type_id<BoxCollider2DComponent>().hash()
				|| component == entt::type_id<CircleCollider2DComponent>().hash();
		}

		// 相对路径先按内容根解析;绝对路径原样使用(与 ScriptEngine 读脚本源码同一口径)。
		std::string ResolvePrefabPath(const std::string& path)
		{
			const std::filesystem::path requested(path);
			if (requested.is_absolute())
				return requested.string();
			return (std::filesystem::path(WLD_ASSETPATH) / requested).string();
		}

		// 回调内走白名单结构写窗口;停止(含 OnDestroy)与回调外的活动场景由
		// ScriptWriteScope / AssertStructuralWrite 给出可读错误。
		template <typename Fn>
		auto WithScriptWrite(Scene& scene, Fn&& fn) -> decltype(fn())
		{
			if (scene.IsActive())
			{
				if (!scene.IsInsideScriptCallback())
					throw std::logic_error("Entity structural writes are only available inside script lifecycle callbacks while the scene is running");
				Scene::ScriptWriteScope scope(scene);
				return fn();
			}
			return fn();
		}

		// ---- W3f:2D 物理运行时 API 的参数/前置条件 ----
		// 错误文本与 Entity 其它方法的既有口径一致:Entity:<方法名> + 可读原因。
		// 返回 Box2D 刚体句柄(不是组件引用):调用方只读组件,避免在回调内触发结构写检查。
		b2BodyId RequirePhysicsBody(Entity& entity, const char* operation)
		{
			if (!entity.IsValid())
				throw std::logic_error(std::string("Entity:") + operation + " requires a live entity");
			// 用 const registry:非 const 重载带 AssertStructuralWrite,而物理读写可以从脚本回调内调用。
			const Scene& scene = *entity.GetScene();
			auto* body = scene.GetRegistry().try_get<RigidBody2DComponent>(static_cast<entt::entity>(entity));
			if (!body)
				throw std::logic_error(std::string("Entity:") + operation +
					" requires a 2D rigid body; this entity has no RigidBody2DComponent");
			if (!scene.IsPhysics2DRunning())
				throw std::logic_error(std::string("Entity:") + operation +
					" requires a running 2D physics world; start the runtime first");
			if (!b2Body_IsValid(body->RuntimeBodyId))
				throw std::logic_error(std::string("Entity:") + operation +
					" has a RigidBody2DComponent but no live Box2D body; this entity was not added to the running world");
			return body->RuntimeBodyId;
		}

		glm::vec2 RequireVec2(const ScriptValue* args, std::size_t count, const char* operation, std::size_t index)
		{
			ScriptBindingContext& bindings = ScriptEngine::GetBindingContext();
			glm::vec2* value = nullptr;
			if (count <= index || !bindings.Unwrap<glm::vec2>("vec2", args[index], &value) || !value)
				throw std::logic_error(std::string("Entity:") + operation + " expects a vec2");
			return *value;
		}

		float RequireFiniteFloat(const ScriptValue* args, std::size_t count, const char* operation, std::size_t index)
		{
			if (count <= index || !args[index].IsNumber())
				throw std::logic_error(std::string("Entity:") + operation + " expects a number");
			const double number = RequireNumber(args[index], operation);
			if (!std::isfinite(number))
				throw std::logic_error(std::string("Entity:") + operation + " expects a finite number");
			return static_cast<float>(number);
		}

		ScriptValue Box2DVec2ToScript(const b2Vec2& value)
		{
			ScriptBindingContext& bindings = ScriptEngine::GetBindingContext();
			ScriptValue result = bindings.NewUserdata("vec2");
			glm::vec2* target = nullptr;
			if (!bindings.Unwrap<glm::vec2>("vec2", result, &target) || !target)
				throw std::logic_error("vec2: failed to allocate a script value");
			new (target) glm::vec2(value.x, value.y);
			return result;
		}
	}

	void RegisterBuiltinEntityLuaType()
	{
		LuaTypeReflection type;
		type.ClassName = "Entity";
		type.Methods = {
			{ "IsValid", {}, "boolean", "Whether this handle and its scene still exist; safe to call on an expired entity." },
			{ "GetID", {}, "integer", "Return this entity's runtime handle (including its generation), not a persistent UUID." },
			{ "GetName", {}, "string", "Return the TagComponent name; raises a readable error when the tag is missing." },
			{ "SetName", { { "name", "string", "New TagComponent name." } }, "", "Set the TagComponent name immediately; this is data, not a structural write." },
			{ "CreateChild", {}, "Entity", "Create a child shell (Tag+UUID only) and attach it under this entity in the current frame." },
			{ "CreateChild", { { "name", "string", "Optional child name; defaults to Empty Entity." } }, "Entity", "Create a named child shell (Tag+UUID only) and attach it under this entity in the current frame." },
			{ "SetParent", { { "parent", "Entity", "New parent entity in the same scene." } }, "boolean", "Attach under parent synchronously; cycles, self-parenting and depth overflow return false. HierarchyComponent.Parent stays read-only." },
			{ "ClearParent", {}, "boolean", "Detach this entity to the scene root synchronously; returns false when the handle is invalid." },
			{ "GetParent", {}, "Entity|nil", "Return the current parent handle immediately, or nil when this entity is a scene root." },
			{ "GetChildren", {}, "Entity[]", "Return direct children in HierarchyComponent order (immediate live query)." },
			{ "FindByName", { { "name", "string", "TagComponent name to find." } }, "Entity|nil", "Find the first matching entity in the scene. Entities synchronously created during the current script update stay hidden until the next frame." },
			{ "InstantiatePrefab", { { "path", "string", "Prefab path; relative paths resolve under the content root (WLD_ASSETPATH)." } }, "Entity", "Instantiate a .wprefab synchronously and return its root handle. Synchronous I/O: large prefabs can stall the frame; raises on a missing or invalid prefab." },
			{ "HasComponent", { { "componentType", "string", "Registered component type name." } }, "boolean", "Check whether this entity has the component; invalid entities or non-component types raise an error." },
			{ "GetComponent", { { "componentType", "string", "Registered component type name." } }, "userdata|nil", "Return a schema-driven field proxy, or nil when the component is absent. Every field access re-checks the entity and component; writes type-check against the schema." },
			{ "AddComponent", { { "componentType", "string", "Registered component type name." } }, "", "Add a default component. Pure-data components (Transform/Sprite/Circle/MeshRenderer/Camera/Hierarchy) commit synchronously inside callbacks; script/physics components keep their existing deferred or rejected rules." },
			{ "RemoveComponent", { { "componentType", "string", "Registered component type name." } }, "", "Remove the component through scene cleanup; requests during callbacks are deferred and duplicates are ignored." },
			{ "Destroy", {}, "", "Destroy this entity through scene cleanup; requests during callbacks are deferred and duplicates are ignored." },
			{ "GetLinearVelocity", {}, "vec2", "Return the Box2D linear velocity (metres per second) of this entity's 2D rigid body; raises when the entity has no live body or the 2D physics world is not running." },
			{ "SetLinearVelocity", { { "velocity", "vec2", "New linear velocity in metres per second." } }, "", "Set the linear velocity of this entity's 2D rigid body; raises when the entity has no live body or the 2D physics world is not running." },
			{ "GetAngularVelocity", {}, "number", "Return the Box2D angular velocity (radians per second) of this entity's 2D rigid body; raises when the entity has no live body or the 2D physics world is not running." },
			{ "SetAngularVelocity", { { "velocity", "number", "New angular velocity in radians per second." } }, "", "Set the angular velocity of this entity's 2D rigid body; raises when the entity has no live body or the 2D physics world is not running." },
			{ "ApplyLinearImpulse", { { "impulse", "vec2", "Impulse vector in kilogram-metres per second, applied at the centre of mass." } }, "", "Apply a linear impulse to this entity's dynamic 2D rigid body (also wakes it); raises when the entity has no live body or the 2D physics world is not running." },
			{ "ApplyForce", { { "force", "vec2", "Force vector in newtons, applied at the centre of mass for one step." } }, "", "Apply a force to this entity's dynamic 2D rigid body (also wakes it); raises when the entity has no live body or the 2D physics world is not running." },
			{ "SyncPhysicsBody", {}, "", "Teleport this entity's 2D rigid body to the current Transform location/rotation (Z radian) and wake it; used for kinematic/static bodies; raises when the entity has no live body or the 2D physics world is not running." }
		};
		type.BindFunc = [](ScriptBindingContext& bindings)
		{
			const ScriptMethodBinding methods[] = {
				{ "IsValid", [](const ScriptValue* args, std::size_t count) -> ScriptValue
					{
						ScriptBindingContext& context = ScriptEngine::GetBindingContext();
						Entity* entity = Receiver(context, args, count, "IsValid");
						return ScriptValue::Boolean(entity->IsValid());
					} },
				{ "GetID", [](const ScriptValue* args, std::size_t count) -> ScriptValue
					{
						ScriptBindingContext& context = ScriptEngine::GetBindingContext();
						Entity* entity = Receiver(context, args, count, "GetID");
						RequireEntity(*entity, "GetID");
						return ScriptValue::Number(static_cast<double>(static_cast<uint32_t>(*entity)));
					} },
				{ "GetName", [](const ScriptValue* args, std::size_t count) -> ScriptValue
					{
						ScriptBindingContext& context = ScriptEngine::GetBindingContext();
						Entity* entity = Receiver(context, args, count, "GetName");
						RequireEntity(*entity, "GetName");
						const entt::id_type tagId = entt::type_id<TagComponent>().hash();
						if (!entity->HasComponent(tagId))
							throw std::logic_error("Entity:GetName requires a TagComponent");
						return ScriptValue::String(static_cast<TagComponent*>(entity->GetComponent(tagId))->Tag);
					} },
				{ "SetName", [](const ScriptValue* args, std::size_t count) -> ScriptValue
					{
						ScriptBindingContext& context = ScriptEngine::GetBindingContext();
						Entity* entity = Receiver(context, args, count, "SetName");
						RequireEntity(*entity, "SetName");
						const std::string name = RequireStringArgument(args, count, 1, "Entity:SetName");
						const entt::id_type tagId = entt::type_id<TagComponent>().hash();
						if (!entity->HasComponent(tagId))
							throw std::logic_error("Entity:SetName requires a TagComponent");
						static_cast<TagComponent*>(entity->GetComponent(tagId))->Tag = name;
						return ScriptValue::Nil();
					} },
				{ "CreateChild", [](const ScriptValue* args, std::size_t count) -> ScriptValue
					{
						ScriptBindingContext& context = ScriptEngine::GetBindingContext();
						Entity* entity = Receiver(context, args, count, "CreateChild");
						RequireEntity(*entity, "CreateChild");
						std::string name = "Empty Entity";
						if (count >= 2) name = RequireString(args[1], "Entity:CreateChild");
						Scene* scene = entity->GetScene();
						Entity child = WithScriptWrite(*scene, [&]() -> Entity
						{
							Entity created = scene->CreateEntityShell(name);
							if (!Hierarchy::SetParent(scene->GetRegistry(), static_cast<entt::entity>(created),
								static_cast<entt::entity>(*entity)))
								throw std::logic_error("Entity:CreateChild could not attach the new entity to its parent");
							return created;
						});
						return NewUserdataOf(context, "Entity", child);
					} },
				{ "SetParent", [](const ScriptValue* args, std::size_t count) -> ScriptValue
					{
						ScriptBindingContext& context = ScriptEngine::GetBindingContext();
						Entity* entity = Receiver(context, args, count, "SetParent");
						RequireEntity(*entity, "SetParent");
						Entity* parent = nullptr;
						if (count < 2 || !context.Unwrap<Entity>("Entity", args[1], &parent) || !parent)
							throw std::logic_error("Entity:SetParent expects an Entity parent");
						if (!parent->IsValid() || parent->GetScene() != entity->GetScene())
							return ScriptValue::Boolean(false);
						Scene* scene = entity->GetScene();
						const bool attached = WithScriptWrite(*scene, [&]()
						{
							return Hierarchy::SetParent(scene->GetRegistry(), static_cast<entt::entity>(*entity),
								static_cast<entt::entity>(*parent));
						});
						return ScriptValue::Boolean(attached);
					} },
				{ "ClearParent", [](const ScriptValue* args, std::size_t count) -> ScriptValue
					{
						ScriptBindingContext& context = ScriptEngine::GetBindingContext();
						Entity* entity = Receiver(context, args, count, "ClearParent");
						RequireEntity(*entity, "ClearParent");
						Scene* scene = entity->GetScene();
						const bool detached = WithScriptWrite(*scene, [&]()
						{
							return Hierarchy::SetParent(scene->GetRegistry(), static_cast<entt::entity>(*entity), entt::null);
						});
						return ScriptValue::Boolean(detached);
					} },
				{ "GetParent", [](const ScriptValue* args, std::size_t count) -> ScriptValue
					{
						ScriptBindingContext& context = ScriptEngine::GetBindingContext();
						Entity* entity = Receiver(context, args, count, "GetParent");
						RequireEntity(*entity, "GetParent");
						const entt::id_type hierarchyId = entt::type_id<HierarchyComponent>().hash();
						if (!entity->HasComponent(hierarchyId))
							return ScriptValue::Nil();
						const auto* hierarchy = static_cast<const HierarchyComponent*>(entity->GetComponent(hierarchyId));
						if (!hierarchy || hierarchy->Parent == entt::null)
							return ScriptValue::Nil();
						Entity parent(entity->GetScene(), hierarchy->Parent);
						if (!parent.IsValid())
							return ScriptValue::Nil();
						return NewUserdataOf(context, "Entity", parent);
					} },
				{ "GetChildren", [](const ScriptValue* args, std::size_t count) -> ScriptValue
					{
						ScriptBindingContext& context = ScriptEngine::GetBindingContext();
						Entity* entity = Receiver(context, args, count, "GetChildren");
						RequireEntity(*entity, "GetChildren");
						ScriptTableRef children = ScriptEngine::GetState().CreateTable();
						const entt::id_type hierarchyId = entt::type_id<HierarchyComponent>().hash();
						if (entity->HasComponent(hierarchyId))
						{
							const auto* hierarchy = static_cast<const HierarchyComponent*>(entity->GetComponent(hierarchyId));
							std::size_t index = 1;
							if (hierarchy)
							{
								for (const entt::entity childHandle : hierarchy->Children)
								{
									Entity child(entity->GetScene(), childHandle);
									if (!child.IsValid())
										continue;
									children.SetArrayElement(index++, NewUserdataOf(context, "Entity", child));
								}
							}
						}
						return children.ToValue();
					} },
				{ "FindByName", [](const ScriptValue* args, std::size_t count) -> ScriptValue
					{
						ScriptBindingContext& context = ScriptEngine::GetBindingContext();
						Entity* entity = Receiver(context, args, count, "FindByName");
						RequireEntity(*entity, "FindByName");
						const std::string name = RequireStringArgument(args, count, 1, "Entity:FindByName");
						Scene* scene = entity->GetScene();
						const entt::registry& registry = static_cast<const Scene&>(*scene).GetRegistry();
						for (const entt::entity handle : registry.view<TagComponent>())
						{
							if (!scene->IsVisibleToCurrentScriptUpdate(handle))
								continue;
							if (registry.get<TagComponent>(handle).Tag == name)
								return NewUserdataOf(context, "Entity", Entity(scene, handle));
						}
						return ScriptValue::Nil();
					} },
				{ "InstantiatePrefab", [](const ScriptValue* args, std::size_t count) -> ScriptValue
					{
						ScriptBindingContext& context = ScriptEngine::GetBindingContext();
						Entity* entity = Receiver(context, args, count, "InstantiatePrefab");
						RequireEntity(*entity, "InstantiatePrefab");
						const std::string path = RequireStringArgument(args, count, 1, "Entity:InstantiatePrefab");
						const std::string resolved = ResolvePrefabPath(path);
						Scene* scene = entity->GetScene();
						const Gameplay::PrefabInstanceResult result = WithScriptWrite(*scene, [&]()
						{
							return Gameplay::InstantiateFromFile(resolved, *scene, entt::null);
						});
						if (!result.IsValid())
							throw std::logic_error("Entity:InstantiatePrefab could not load '" + path +
								"' (resolved: " + resolved + "); synchronous I/O large prefabs can stall the frame");
						return NewUserdataOf(context, "Entity", result.Root);
					} },
				{ "HasComponent", [](const ScriptValue* args, std::size_t count) -> ScriptValue
					{
						ScriptBindingContext& context = ScriptEngine::GetBindingContext();
						Entity* entity = Receiver(context, args, count, "HasComponent");
						RequireEntity(*entity, "HasComponent");
						const std::string typeName = RequireStringArgument(args, count, 1, "Entity:HasComponent");
						return ScriptValue::Boolean(entity->HasComponent(RequireComponentType(*entity, typeName, "HasComponent").Storage->ComponentId));
					} },
				{ "GetComponent", [](const ScriptValue* args, std::size_t count) -> ScriptValue
					{
						ScriptBindingContext& context = ScriptEngine::GetBindingContext();
						Entity* entity = Receiver(context, args, count, "GetComponent");
						RequireEntity(*entity, "GetComponent");
						const std::string typeName = RequireStringArgument(args, count, 1, "Entity:GetComponent");
						const Schema::TypeSchema& schema = RequireComponentType(*entity, typeName, "GetComponent");
						if (!entity->HasComponent(schema.Storage->ComponentId))
							return ScriptValue::Nil();
						// W3a-A1:返回 schema 驱动的字段代理(读写按 FieldSchema::Get/Set,零 per-field 生成物)。
						return MakeComponentProxy(context, *entity, schema);
					} },
				{ "AddComponent", [](const ScriptValue* args, std::size_t count) -> ScriptValue
					{
						ScriptBindingContext& context = ScriptEngine::GetBindingContext();
						Entity* entity = Receiver(context, args, count, "AddComponent");
						RequireEntity(*entity, "AddComponent");
						const std::string typeName = RequireStringArgument(args, count, 1, "Entity:AddComponent");
						const Schema::TypeSchema& schema = RequireComponentType(*entity, typeName, "AddComponent");
						const entt::id_type componentId = schema.Storage->ComponentId;
						std::string reason;
						if (!entity->CanAddComponent(componentId, &reason))
							throw std::logic_error("Entity:AddComponent '" + typeName + "': " + reason);
						Scene* scene = entity->GetScene();
						if (scene->IsActive())
						{
							// W3d:白名单纯数据组件在回调内同步提交,当帧即可读/渲染;
							// W3f:2D 物理组件(刚性体/碰撞体)走同一条白名单路径,由实体层立即补建 Box2D 刚体;
							// 其余组件保持既有延迟提交(提交点/下一帧生效)。
							// 注意:实体层的 CheckAdd 只在回调内放行物理组件,提交点仍按既有规则拒绝。
							if (scene->IsInsideScriptCallback() &&
								(IsSynchronousScriptComponent(componentId) || IsPhysicsComponent(componentId)))
							{
								Scene::ScriptWriteScope scope(*scene);
								Entity handle = *entity;
								handle.AddComponent(componentId);
								return ScriptValue::Nil();
							}
							// Capture only a lifetime-checked handle and a type ID, never a component pointer.
							Entity handle = *entity;
							if (!scene->DeferStructuralChange([handle, componentId](Scene& targetScene) mutable
							{
								if (handle.IsValid() && !targetScene.IsPendingDestroy(static_cast<entt::entity>(handle)))
									handle.AddComponent(componentId);
							}))
								throw std::logic_error("Entity:AddComponent '" + typeName + "' was rejected during scene/script shutdown");
						}
						else entity->AddComponent(componentId);
						return ScriptValue::Nil();
					} },
				{ "RemoveComponent", [](const ScriptValue* args, std::size_t count) -> ScriptValue
					{
						ScriptBindingContext& context = ScriptEngine::GetBindingContext();
						Entity* entity = Receiver(context, args, count, "RemoveComponent");
						RequireEntity(*entity, "RemoveComponent");
						const std::string typeName = RequireStringArgument(args, count, 1, "Entity:RemoveComponent");
						entity->RemoveComponent(RequireComponentType(*entity, typeName, "RemoveComponent").Storage->ComponentId);
						return ScriptValue::Nil();
					} },
				{ "Destroy", [](const ScriptValue* args, std::size_t count) -> ScriptValue
					{
						ScriptBindingContext& context = ScriptEngine::GetBindingContext();
						Entity* entity = Receiver(context, args, count, "Destroy");
						RequireEntity(*entity, "Destroy");
						Entity::DestroyEntity(entity->GetScene(), *entity);
						return ScriptValue::Nil();
					} },
				{ "GetLinearVelocity", [](const ScriptValue* args, std::size_t count) -> ScriptValue
					{
						ScriptBindingContext& context = ScriptEngine::GetBindingContext();
						Entity* entity = Receiver(context, args, count, "GetLinearVelocity");
						return Box2DVec2ToScript(b2Body_GetLinearVelocity(RequirePhysicsBody(*entity, "GetLinearVelocity")));
					} },
				{ "SetLinearVelocity", [](const ScriptValue* args, std::size_t count) -> ScriptValue
					{
						ScriptBindingContext& context = ScriptEngine::GetBindingContext();
						Entity* entity = Receiver(context, args, count, "SetLinearVelocity");
						const glm::vec2 velocity = RequireVec2(args, count, "SetLinearVelocity", 1);
						b2Body_SetLinearVelocity(RequirePhysicsBody(*entity, "SetLinearVelocity"), { velocity.x, velocity.y });
						return ScriptValue::Nil();
					} },
				{ "GetAngularVelocity", [](const ScriptValue* args, std::size_t count) -> ScriptValue
					{
						ScriptBindingContext& context = ScriptEngine::GetBindingContext();
						Entity* entity = Receiver(context, args, count, "GetAngularVelocity");
						return ScriptValue::Number(b2Body_GetAngularVelocity(RequirePhysicsBody(*entity, "GetAngularVelocity")));
					} },
				{ "SetAngularVelocity", [](const ScriptValue* args, std::size_t count) -> ScriptValue
					{
						ScriptBindingContext& context = ScriptEngine::GetBindingContext();
						Entity* entity = Receiver(context, args, count, "SetAngularVelocity");
						const float velocity = RequireFiniteFloat(args, count, "SetAngularVelocity", 1);
						b2Body_SetAngularVelocity(RequirePhysicsBody(*entity, "SetAngularVelocity"), velocity);
						return ScriptValue::Nil();
					} },
				{ "ApplyLinearImpulse", [](const ScriptValue* args, std::size_t count) -> ScriptValue
					{
						ScriptBindingContext& context = ScriptEngine::GetBindingContext();
						Entity* entity = Receiver(context, args, count, "ApplyLinearImpulse");
						const glm::vec2 impulse = RequireVec2(args, count, "ApplyLinearImpulse", 1);
						b2Body_ApplyLinearImpulseToCenter(RequirePhysicsBody(*entity, "ApplyLinearImpulse"), { impulse.x, impulse.y }, true);
						return ScriptValue::Nil();
					} },
				{ "ApplyForce", [](const ScriptValue* args, std::size_t count) -> ScriptValue
					{
						ScriptBindingContext& context = ScriptEngine::GetBindingContext();
						Entity* entity = Receiver(context, args, count, "ApplyForce");
						const glm::vec2 force = RequireVec2(args, count, "ApplyForce", 1);
						b2Body_ApplyForceToCenter(RequirePhysicsBody(*entity, "ApplyForce"), { force.x, force.y }, true);
						return ScriptValue::Nil();
					} },
				{ "SyncPhysicsBody", [](const ScriptValue* args, std::size_t count) -> ScriptValue
					{
						ScriptBindingContext& context = ScriptEngine::GetBindingContext();
						Entity* entity = Receiver(context, args, count, "SyncPhysicsBody");
						RequirePhysicsBody(*entity, "SyncPhysicsBody");
						entity->GetScene()->SyncPhysicsBodyFromTransform(static_cast<entt::entity>(*entity));
						return ScriptValue::Nil();
					} },
			};

			ScriptUserTypeDesc desc;
			desc.Name = "Entity";
			desc.UserdataSize = sizeof(Entity);
			desc.Methods = methods;
			desc.MethodCount = sizeof(methods) / sizeof(methods[0]);
			// Entity 里有 std::weak_ptr 生命周期令牌:必须登记析构,否则每次 GC 都泄漏控制块。
			desc.Destructor = [](void* data) { static_cast<Entity*>(data)->~Entity(); };

			std::string error;
			if (!bindings.RegisterUserType(desc, &error))
				throw std::logic_error("Entity registration failed: " + error);
		};
		LuaReflectionRegistry::Register(type);
	}
}
