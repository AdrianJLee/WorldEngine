#include "wldpch.h"
#include "World/Scene/ScriptEngine.h"
#include "World/Scene/Entity.h"
#include "World/Core/WorldContext.h"

#include <any>
#include <stdexcept>

namespace World
{
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
	}

	void RegisterBuiltinEntityLuaType()
	{
		LuaTypeReflection type;
		type.ClassName = "Entity";
		type.Methods = {
			{ "IsValid", {}, "boolean", "Whether this handle and its scene still exist; safe to call on an expired entity." },
			{ "GetID", {}, "integer", "Return this entity's runtime handle (including its generation), not a persistent UUID." },
			{ "HasComponent", { { "componentType", "string", "Registered component type name." } }, "boolean", "Check whether this entity has the component; invalid entities or non-component types raise an error." },
			{ "GetComponent", { { "componentType", "string", "Registered component type name." } }, "userdata|nil", "Return an opaque component pointer, or nil when absent. Component fields are not exposed; do not retain across structural changes." },
			{ "AddComponent", { { "componentType", "string", "Registered component type name." } }, "", "Request a default component. Active scenes commit at a safe point; invalid requests raise an error." },
			{ "RemoveComponent", { { "componentType", "string", "Registered component type name." } }, "", "Remove the component through scene cleanup; requests during callbacks are deferred and duplicates are ignored." },
			{ "Destroy", {}, "", "Destroy this entity through scene cleanup; requests during callbacks are deferred and duplicates are ignored." }
		};
		type.BindFunc = [](sol::state& lua)
		{
			lua.new_usertype<Entity>("Entity", sol::no_constructor,
				"IsValid", [](const Entity& entity) -> bool
				{
					ScriptEngine::AssertOwnerThread();
					return entity.IsValid();
				},
				"GetID", [](const Entity& entity) -> uint32_t
				{
					RequireEntity(entity, "GetID");
					return static_cast<uint32_t>(entity);
				},
				"HasComponent", [](const Entity& entity, const std::string& typeName) -> bool
				{
					RequireEntity(entity, "HasComponent");
					return entity.HasComponent(RequireComponentType(entity, typeName, "HasComponent").Storage->ComponentId);
				},
				"GetComponent", [](Entity& entity, const std::string& typeName) -> sol::object
				{
					RequireEntity(entity, "GetComponent");
					const auto componentId = RequireComponentType(entity, typeName, "GetComponent").Storage->ComponentId;
					if (!entity.HasComponent(componentId)) return sol::nil;
					return sol::make_object(ScriptEngine::GetState(), entity.GetComponent(componentId));
				},
				"AddComponent", [](Entity& entity, const std::string& typeName)
				{
					RequireEntity(entity, "AddComponent");
					const auto componentId = RequireComponentType(entity, typeName, "AddComponent").Storage->ComponentId;
					std::string reason;
					if (!entity.CanAddComponent(componentId, &reason))
						throw std::logic_error("Entity:AddComponent '" + typeName + "': " + reason);
					Scene* scene = entity.GetScene();
					if (scene->IsActive())
					{
						// Capture only a lifetime-checked handle and a type ID, never a component pointer.
						if (!scene->DeferStructuralChange([entity, componentId](Scene& targetScene) mutable
						{
							if (entity.IsValid() && !targetScene.IsPendingDestroy(static_cast<entt::entity>(entity)))
								entity.AddComponent(componentId);
						}))
							throw std::logic_error("Entity:AddComponent '" + typeName + "' was rejected during scene/script shutdown");
					}
					else entity.AddComponent(componentId);
				},
				"RemoveComponent", [](Entity& entity, const std::string& typeName)
				{
					RequireEntity(entity, "RemoveComponent");
					entity.RemoveComponent(RequireComponentType(entity, typeName, "RemoveComponent").Storage->ComponentId);
				},
				"Destroy", [](Entity& entity)
				{
					RequireEntity(entity, "Destroy");
					Entity::DestroyEntity(entity.GetScene(), entity);
				}
			);
		};
		LuaReflectionRegistry::Register(type);
	}
}
