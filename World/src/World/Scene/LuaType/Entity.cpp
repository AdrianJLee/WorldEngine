#include "wldpch.h"
#include "World/Scene/ScriptEngine.h"
#include "World/Scene/Entity.h"
#include "World/Core/ComponentRegistry.h"
namespace World
{
	static LuaTypeRegistrar s_EntityRegistrar(
		{
			"Entity",
			{
				{"GetID","fun():number", "Get the unique ID of the entity"},
				{"HasComponent", "fun(componentType:string):boolean", "Check if the entity has a component of the specified type"},
				{"GetComponent", "fun(componentType:string):table", "Get the component of the specified type as a Lua table"},
				{"AddComponent", "fun(componentType:string)", "Add a component of the specified type to the entity"},
				{"RemoveComponent", "fun(componentType:string)", "Remove the component of the specified type from the entity"}
			},
			[](sol::state& lua)
			{
				lua.new_usertype<Entity>("Entity",
					sol::no_constructor, // 禁止 Lua 直接创建 Entity 实例
					"GetID", [](Entity& entity) -> uint32_t
					{
						return (uint32_t)entity;
					},
					"HasComponent", [](Entity& entity, const std::string& typeName) -> bool
					{
						const TypeDesc* typeDesc = TypeRegistry::Get().GetTypeDesc(typeName);
						if (!typeDesc)
						{
							return false;
						}
						TypeDescDataComponent componentData = std::any_cast<TypeDescDataComponent>(typeDesc->UserData);
						return entity.HasComponent(componentData.Id);
					},
					"GetComponent", [](Entity& entity, const std::string& typeName) -> sol::object
					{
						const TypeDesc* typeDesc = TypeRegistry::Get().GetTypeDesc(typeName);
						if (!typeDesc)
						{
							return sol::nil;
						}
						TypeDescDataComponent componentData = std::any_cast<TypeDescDataComponent>(typeDesc->UserData);
						auto componentPtr = entity.GetComponent(componentData.Id);

						return sol::make_object(ScriptEngine::GetState(), componentPtr);
					},
					"AddComponent", [](Entity& entity, const std::string& typeName)
					{
						const TypeDesc* typeDesc = TypeRegistry::Get().GetTypeDesc(typeName);
						if (!typeDesc)
						{
							WLD_CORE_ERROR("Type '{}' not found in TypeRegistry!", typeName);
							return;
						}
						TypeDescDataComponent componentData = std::any_cast<TypeDescDataComponent>(typeDesc->UserData);
						entity.AddComponent(componentData.Id);
					},
					"RemoveComponent", [](Entity& entity, const std::string& typeName)
					{
						const TypeDesc* typeDesc = TypeRegistry::Get().GetTypeDesc(typeName);
						if (!typeDesc)
						{
							WLD_CORE_ERROR("Type '{}' not found in TypeRegistry!", typeName);
							return;
						}
						TypeDescDataComponent componentData = std::any_cast<TypeDescDataComponent>(typeDesc->UserData);
						entity.RemoveComponent(componentData.Id);
					}
				);
			}
		});
}