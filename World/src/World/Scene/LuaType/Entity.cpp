#include "wldpch.h"
#include "World/Scene/ScriptEngine.h"
#include "World/Scene/Entity.h"
#include "World/Core/WorldContext.h"
#include "World/Script/BindComponentAccess.h"
#include "World/Script/ScriptBindingContext.h"
#include "World/Script/ScriptValue.h"
#include "LuaTypeHelpers.h"

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
	}

	void RegisterBuiltinEntityLuaType()
	{
		LuaTypeReflection type;
		type.ClassName = "Entity";
		type.Methods = {
			{ "IsValid", {}, "boolean", "Whether this handle and its scene still exist; safe to call on an expired entity." },
			{ "GetID", {}, "integer", "Return this entity's runtime handle (including its generation), not a persistent UUID." },
			{ "HasComponent", { { "componentType", "string", "Registered component type name." } }, "boolean", "Check whether this entity has the component; invalid entities or non-component types raise an error." },
			{ "GetComponent", { { "componentType", "string", "Registered component type name." } }, "userdata|nil", "Return a schema-driven field proxy, or nil when the component is absent. Every field access re-checks the entity and component; writes type-check against the schema." },
			{ "AddComponent", { { "componentType", "string", "Registered component type name." } }, "", "Request a default component. Active scenes commit at a safe point; invalid requests raise an error." },
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
						const entt::id_type componentId = RequireComponentType(*entity, typeName, "AddComponent").Storage->ComponentId;
						std::string reason;
						if (!entity->CanAddComponent(componentId, &reason))
							throw std::logic_error("Entity:AddComponent '" + typeName + "': " + reason);
						Scene* scene = entity->GetScene();
						if (scene->IsActive())
						{
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
