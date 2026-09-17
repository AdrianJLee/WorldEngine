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
			{ "Destroy", {}, "", "Destroy this entity through scene cleanup; requests during callbacks are deferred and duplicates are ignored." }
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
							// 其余组件保持既有延迟提交(提交点/下一帧生效)。
							if (scene->IsInsideScriptCallback() && IsSynchronousScriptComponent(componentId))
							{
								Scene::ScriptWriteScope scope(*scene);
								Entity handle = *entity;
								if (!schema.Storage->Add)
									throw std::logic_error("Entity:AddComponent '" + typeName + "' has no storage binding");
								schema.Storage->Add(static_cast<void*>(&handle));
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
