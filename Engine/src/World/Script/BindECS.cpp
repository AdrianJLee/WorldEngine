#include "wldpch.h"
#include "World/Script/BindECS.h"

#include "World/Core/Log.h"
#include "World/Core/WorldContext.h"
#include "World/Scene/Components.h"
#include "World/Scene/Entity.h"
#include "World/Scene/LuaType/LuaTypeHelpers.h"
#include "World/Scene/Scene.h"
#include "World/Scene/ScriptEngine.h"
#include "World/Schema/SchemaRegistry.h"
#include "World/Script/BindComponentAccess.h"
#include "World/Script/LuauVm.h"
#include "World/Script/ScriptBindingContext.h"
#include "World/Script/ScriptRef.h"
#include "World/Script/ScriptValue.h"

#include <cstddef>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace World
{
	namespace
	{
		using LuaTypeDetail::NewUserdataOf;

		// -------------------------------------------------------------------------
		// ecs:Query(componentNames) 实现
		// -------------------------------------------------------------------------
		ScriptValue QueryImpl(ScriptBindingContext& bindings, const ScriptValue* args, std::size_t count)
		{
			std::size_t startIndex = 0;
			if (count >= 1 && args[0].IsTable())
			{
				if (count >= 2)
					startIndex = 1;
				else
					startIndex = 0;
			}

			std::vector<std::string> componentNames;
			for (std::size_t i = startIndex; i < count; ++i)
			{
				if (args[i].IsString())
				{
					std::string name;
					args[i].AsString(&name);
					if (!name.empty())
						componentNames.push_back(std::move(name));
				}
				else if (args[i].IsTable())
				{
					ScriptTableRef table;
					if (args[i].AsTable(&table) && table.IsValid())
					{
						std::vector<ScriptValue> arr = table.GetArray();
						for (const auto& item : arr)
						{
							std::string name;
							if (item.AsString(&name) && !name.empty())
								componentNames.push_back(std::move(name));
							else
								throw std::logic_error("ecs:Query: array elements must be component name strings");
						}
					}
				}
				else
				{
					throw std::logic_error("ecs:Query: argument must be a table or string of component names");
				}
			}

			if (componentNames.empty())
				throw std::logic_error("ecs:Query requires at least one component name");

			Scene* activeScene = ScriptEngine::GetActiveScene();
			if (!activeScene)
				throw std::logic_error("ecs:Query requires an active scene");

			auto& schemaRegistry = activeScene->GetContext().Schemas();
			std::vector<const Schema::TypeSchema*> schemas;
			schemas.reserve(componentNames.size());
			for (const auto& name : componentNames)
			{
				const Schema::TypeSchema* type = schemaRegistry.Find(name);
				if (!type || type->Category != Schema::TypeCategory::Component || !type->Storage)
					throw std::logic_error("ecs:Query: '" + name + "' is not a registered component type");
				schemas.push_back(type);
			}

			LuauVm* vm = bindings.Vm();
			if (!vm)
				throw std::logic_error("ecs:Query: Luau VM is not valid");

			ScriptTableRef queryTable = vm->CreateTable();
			if (!queryTable.IsValid())
				throw std::logic_error("ecs:Query: failed to create query table");

			// Method: Each(callback)
			queryTable.SetField("Each", bindings.CreateFunction("Query:Each",
				[schemas](const ScriptValue* eachArgs, std::size_t eachCount) -> ScriptValue
				{
					ScriptFunctionRef callback;
					if (eachCount >= 2 && eachArgs[1].AsFunction(&callback))
					{
					}
					else if (eachCount >= 1 && eachArgs[0].AsFunction(&callback))
					{
					}
					else
					{
						throw std::logic_error("Query:Each requires a callback function");
					}

					if (!callback.IsValid())
						throw std::logic_error("Query:Each callback is invalid");

					Scene* scene = ScriptEngine::GetActiveScene();
					if (!scene)
						throw std::logic_error("Query:Each requires an active scene");

					auto& registry = scene->GetRegistry();
					ScriptBindingContext& context = ScriptEngine::GetBindingContext();

					std::vector<entt::sparse_set*> storages;
					storages.reserve(schemas.size());
					for (const auto* schema : schemas)
					{
						auto* storage = registry.storage(schema->Storage->ComponentId);
						if (!storage || storage->empty())
							return ScriptValue::Nil();
						storages.push_back(storage);
					}

					std::size_t minIdx = 0;
					std::size_t minSize = storages[0]->size();
					for (std::size_t i = 1; i < storages.size(); ++i)
					{
						if (storages[i]->size() < minSize)
						{
							minSize = storages[i]->size();
							minIdx = i;
						}
					}

					entt::sparse_set* leadStorage = storages[minIdx];
					std::vector<entt::entity> matches;
					matches.reserve(minSize);
					for (auto it = leadStorage->begin(); it != leadStorage->end(); ++it)
					{
						entt::entity entity = *it;
						if (!registry.valid(entity) || scene->IsPendingDestroy(entity))
							continue;

						bool matchesAll = true;
						for (std::size_t i = 0; i < storages.size(); ++i)
						{
							if (i == minIdx) continue;
							if (!storages[i]->contains(entity))
							{
								matchesAll = false;
								break;
							}
						}
						if (matchesAll)
							matches.push_back(entity);
					}

					for (entt::entity entity : matches)
					{
						if (!registry.valid(entity) || scene->IsPendingDestroy(entity))
							continue;

						Entity entityHandle(scene, entity);
						if (!entityHandle.IsValid())
							continue;

						std::vector<ScriptValue> callArgs;
						callArgs.reserve(1 + schemas.size());
						callArgs.push_back(NewUserdataOf(context, "Entity", entityHandle));

						for (const auto* schema : schemas)
						{
							callArgs.push_back(MakeComponentProxy(context, entityHandle, *schema));
						}

						ScriptValue result;
						std::string callError;
						if (!callback.Call(callArgs, &result, &callError))
						{
							throw std::runtime_error("[Luau ECS Query:Each] callback failed: " + callError);
						}
					}

					return ScriptValue::Nil();
				}));

			// Method: Count()
			queryTable.SetField("Count", bindings.CreateFunction("Query:Count",
				[schemas](const ScriptValue*, std::size_t) -> ScriptValue
				{
					Scene* scene = ScriptEngine::GetActiveScene();
					if (!scene)
						throw std::logic_error("Query:Count requires an active scene");

					auto& registry = scene->GetRegistry();

					std::vector<entt::sparse_set*> storages;
					storages.reserve(schemas.size());
					for (const auto* schema : schemas)
					{
						auto* storage = registry.storage(schema->Storage->ComponentId);
						if (!storage || storage->empty())
							return ScriptValue::Number(0.0);
						storages.push_back(storage);
					}

					std::size_t minIdx = 0;
					std::size_t minSize = storages[0]->size();
					for (std::size_t i = 1; i < storages.size(); ++i)
					{
						if (storages[i]->size() < minSize)
						{
							minSize = storages[i]->size();
							minIdx = i;
						}
					}

					entt::sparse_set* leadStorage = storages[minIdx];
					double countMatches = 0.0;
					for (auto it = leadStorage->begin(); it != leadStorage->end(); ++it)
					{
						entt::entity entity = *it;
						if (!registry.valid(entity) || scene->IsPendingDestroy(entity))
							continue;

						bool matchesAll = true;
						for (std::size_t i = 0; i < storages.size(); ++i)
						{
							if (i == minIdx) continue;
							if (!storages[i]->contains(entity))
							{
								matchesAll = false;
								break;
							}
						}
						if (matchesAll)
							countMatches += 1.0;
					}

					return ScriptValue::Number(countMatches);
				}));

			queryTable.SetMetaField("__type", ScriptValue::String("Query"));
			queryTable.SetMetaField("__tostring", bindings.CreateFunction("Query:ToString",
				[](const ScriptValue*, std::size_t) -> ScriptValue
				{
					return ScriptValue::String("Query");
				}));

			return queryTable.ToValue();
		}

		// -------------------------------------------------------------------------
		// ecs:AddSystem(name, [phase,] updateFn) 实现
		// -------------------------------------------------------------------------
		ScriptValue AddSystemImpl(ScriptBindingContext&, const ScriptValue* args, std::size_t count)
		{
			std::size_t startIndex = 0;
			if (count >= 1 && args[0].IsTable())
			{
				startIndex = 1;
			}

			std::size_t remaining = count - startIndex;
			if (remaining < 2)
				throw std::logic_error("ecs:AddSystem expects (name, [phase,] updateFn)");

			std::string name;
			if (!args[startIndex].AsString(&name) || name.empty())
				throw std::logic_error("ecs:AddSystem: system name must be a non-empty string");

			std::string phase = "Update";
			ScriptFunctionRef updateFn;

			if (remaining == 2)
			{
				if (!args[startIndex + 1].AsFunction(&updateFn) || !updateFn.IsValid())
					throw std::logic_error("ecs:AddSystem: expected a function for system update");
			}
			else
			{
				if (args[startIndex + 1].IsFunction())
				{
					args[startIndex + 1].AsFunction(&updateFn);
				}
				else
				{
					args[startIndex + 1].AsString(&phase);
					if (!args[startIndex + 2].AsFunction(&updateFn) || !updateFn.IsValid())
						throw std::logic_error("ecs:AddSystem: expected a function for system update");
				}
			}

			if (!updateFn.IsValid())
				throw std::logic_error("ecs:AddSystem: invalid update callback function");

			Scene* activeScene = ScriptEngine::GetActiveScene();
			if (!activeScene)
				throw std::logic_error("ecs:AddSystem requires an active scene");

			activeScene->EnsureDefaultFrameSystems();

			activeScene->RegisterFrameSystem({
				name,
				false,
				[updateFn, name](Timestep ts)
				{
					ScriptValue result;
					std::string callError;
					const ScriptValue fnArgs[] = { ScriptValue::Number(static_cast<double>(ts.GetSeconds())) };
					if (!updateFn.Call(fnArgs, 1, &result, &callError))
					{
						if (Log::GetCoreLogger())
							WLD_CORE_ERROR("[Luau ECS System '{}'] update error: {}", name, callError);
					}
				}
			});

			return ScriptValue::Boolean(true);
		}

		// -------------------------------------------------------------------------
		// ecs:EntityCount() 实现
		// -------------------------------------------------------------------------
		ScriptValue EntityCountImpl(ScriptBindingContext&, const ScriptValue*, std::size_t)
		{
			Scene* activeScene = ScriptEngine::GetActiveScene();
			if (!activeScene)
				throw std::logic_error("ecs:EntityCount requires an active scene");

			double entityCount = 0.0;
			const auto& entityStorage = activeScene->GetRegistry().storage<entt::entity>();
			for (auto it = entityStorage.begin(); it != entityStorage.end(); ++it)
			{
				entt::entity entity = *it;
				if (activeScene->GetRegistry().valid(entity) && !activeScene->IsPendingDestroy(entity))
					entityCount += 1.0;
			}

			return ScriptValue::Number(entityCount);
		}
	}

	bool RegisterEcsBindings(ScriptBindingContext& bindings, std::string* error)
	{
		LuauVm* vm = bindings.Vm();
		if (!vm)
		{
			if (error) *error = "invalid Luau VM in binding context";
			return false;
		}

		if (vm->GetGlobal("ecs").Type() != ScriptValueType::Nil)
		{
			if (error) *error = "global 'ecs' is already defined";
			return false;
		}

		if (vm->GetGlobal("world").Type() != ScriptValueType::Nil)
		{
			if (error) *error = "global 'world' is already defined";
			return false;
		}

		ScriptTableRef ecsTable = vm->CreateTable();
		if (!ecsTable.IsValid())
		{
			if (error) *error = "failed to create the ecs table";
			return false;
		}

		if (!ecsTable.SetField("Query", bindings.CreateFunction("ecs:Query",
			[&bindings](const ScriptValue* args, std::size_t count) -> ScriptValue
			{
				return QueryImpl(bindings, args, count);
			})))
		{
			if (error) *error = "failed to bind ecs.Query";
			return false;
		}

		if (!ecsTable.SetField("AddSystem", bindings.CreateFunction("ecs:AddSystem",
			[&bindings](const ScriptValue* args, std::size_t count) -> ScriptValue
			{
				return AddSystemImpl(bindings, args, count);
			})))
		{
			if (error) *error = "failed to bind ecs.AddSystem";
			return false;
		}

		if (!ecsTable.SetField("EntityCount", bindings.CreateFunction("ecs:EntityCount",
			[&bindings](const ScriptValue* args, std::size_t count) -> ScriptValue
			{
				return EntityCountImpl(bindings, args, count);
			})))
		{
			if (error) *error = "failed to bind ecs.EntityCount";
			return false;
		}

		// Read-only metatable protection
		if (!ecsTable.SetMetaField("__newindex", bindings.CreateFunction("ecs",
			[](const ScriptValue*, std::size_t) -> ScriptValue
			{
				throw std::logic_error("ecs/world table is read-only; use the documented methods");
			})))
		{
			if (error) *error = "failed to lock the ecs table";
			return false;
		}

		// Expose both 'ecs' and 'world' pointing to the exact same table
		if (!vm->SetGlobal("ecs", ecsTable.ToValue()))
		{
			if (error) *error = "failed to set global 'ecs'";
			return false;
		}

		if (!vm->SetGlobal("world", ecsTable.ToValue()))
		{
			if (error) *error = "failed to set global 'world'";
			return false;
		}

		if (error) error->clear();
		return true;
	}
}
