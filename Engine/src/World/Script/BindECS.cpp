#include "wldpch.h"
#include "World/Script/BindECS.h"
#include "World/Script/BindServices.h"

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
		ScriptValue QueryImpl(const ScriptValue* args, std::size_t count)
		{
			ScriptBindingContext& bindings = ScriptEngine::GetBindingContext();
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
		ScriptValue AddSystemImpl(const ScriptValue* args, std::size_t count)
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

			// 规范形式 (name, fn [, phase]);兼容旧顺序 (name, phase, fn)。
			std::string phase = "Update";
			ScriptFunctionRef updateFn;

			if (args[startIndex + 1].IsFunction())
			{
				if (!args[startIndex + 1].AsFunction(&updateFn) || !updateFn.IsValid())
					throw std::logic_error("ecs:AddSystem: expected a function for system update");
				if (remaining >= 3 && !args[startIndex + 2].IsNil())
					args[startIndex + 2].AsString(&phase);
			}
			else
			{
				args[startIndex + 1].AsString(&phase);
				if (remaining < 3 || !args[startIndex + 2].AsFunction(&updateFn) || !updateFn.IsValid())
					throw std::logic_error("ecs:AddSystem: expected a function for system update");
			}

			if (!updateFn.IsValid())
				throw std::logic_error("ecs:AddSystem: invalid update callback function");

			Scene* activeScene = ScriptEngine::GetActiveScene();
			if (!activeScene)
				throw std::logic_error("ecs:AddSystem requires an active scene");

			activeScene->EnsureDefaultFrameSystems();

			// 幂等:同名系统先撤销再注册 —— 同一场景二次 OnRuntimeStart、以及系统脚本
			// 热重载都是"整份重跑",不该因为重名抛错。
			activeScene->UnregisterFrameSystem(name);

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

			// 归属到当前正在执行的系统脚本(不在加载系统脚本时是 no-op)。
			ScriptEngine::NoteScriptSystem(name);

			return ScriptValue::Boolean(true);
		}

		// -------------------------------------------------------------------------
		// ecs:RemoveSystem(name) 实现
		// -------------------------------------------------------------------------
		ScriptValue RemoveSystemImpl(const ScriptValue* args, std::size_t count)
		{
			std::size_t startIndex = 0;
			if (count >= 1 && args[0].IsTable())
				startIndex = 1;

			if (count <= startIndex)
				throw std::logic_error("ecs:RemoveSystem expects (name)");

			std::string name;
			if (!args[startIndex].AsString(&name) || name.empty())
				throw std::logic_error("ecs:RemoveSystem: system name must be a non-empty string");

			Scene* activeScene = ScriptEngine::GetActiveScene();
			if (!activeScene)
				throw std::logic_error("ecs:RemoveSystem requires an active scene");

			return ScriptValue::Boolean(activeScene->UnregisterFrameSystem(name));
		}

		// -------------------------------------------------------------------------
		// ecs:CreateEntity([name]) 实现
		// -------------------------------------------------------------------------
		ScriptValue CreateEntityImpl(const ScriptValue* args, std::size_t count)
		{
			ScriptBindingContext& bindings = ScriptEngine::GetBindingContext();
			std::size_t startIndex = 0;
			if (count >= 1 && args[0].IsTable())
			{
				startIndex = 1;
			}

			std::string name = "Empty Entity";
			if (count > startIndex && !args[startIndex].IsNil())
			{
				if (!args[startIndex].AsString(&name))
					throw std::logic_error("ecs:CreateEntity expects an optional string name");
			}

			Scene* activeScene = ScriptEngine::GetActiveScene();
			if (!activeScene)
				throw std::logic_error("ecs:CreateEntity requires an active scene");

			Entity entity = Entity::CreateEntity(activeScene, name);
			return NewUserdataOf(bindings, "Entity", entity);
		}

		// -------------------------------------------------------------------------
		// ecs:DestroyEntity(entity) 实现
		// -------------------------------------------------------------------------
		ScriptValue DestroyEntityImpl(const ScriptValue* args, std::size_t count)
		{
			ScriptBindingContext& bindings = ScriptEngine::GetBindingContext();
			std::size_t startIndex = 0;
			if (count >= 1 && args[0].IsTable())
			{
				startIndex = 1;
			}

			if (count <= startIndex)
				throw std::logic_error("ecs:DestroyEntity expects an Entity argument");

			Entity* entity = nullptr;
			if (!bindings.Unwrap<Entity>("Entity", args[startIndex], &entity) || !entity)
				throw std::logic_error("ecs:DestroyEntity expects an Entity userdata");

			if (!entity->IsValid())
				return ScriptValue::Nil();

			Scene* activeScene = ScriptEngine::GetActiveScene();
			Scene* scene = entity->GetScene() ? entity->GetScene() : activeScene;
			if (!scene)
				throw std::logic_error("ecs:DestroyEntity requires an active scene");

			scene->DestroyEntity(*entity);
			return ScriptValue::Nil();
		}

		// -------------------------------------------------------------------------
		// ecs:EntityCount() 实现
		// -------------------------------------------------------------------------
		ScriptValue EntityCountImpl(const ScriptValue*, std::size_t)
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

		// -------------------------------------------------------------------------
		// ecs:OnAdd(componentName, fn) 实现
		// -------------------------------------------------------------------------
		ScriptValue OnAddImpl(const ScriptValue* args, std::size_t count)
		{
			std::size_t startIndex = 0;
			if (count >= 1 && args[0].IsTable())
			{
				startIndex = 1;
			}

			std::size_t remaining = count - startIndex;
			if (remaining < 2)
				throw std::logic_error("ecs:OnAdd expects (componentName, callbackFn)");

			std::string compName;
			if (!args[startIndex].AsString(&compName) || compName.empty())
				throw std::logic_error("ecs:OnAdd: component name must be a non-empty string");

			ScriptFunctionRef fn;
			if (!args[startIndex + 1].AsFunction(&fn) || !fn.IsValid())
				throw std::logic_error("ecs:OnAdd: callback must be a valid function");

			Scene* activeScene = ScriptEngine::GetActiveScene();
			if (!activeScene)
				throw std::logic_error("ecs:OnAdd requires an active scene");

			auto& schemaRegistry = activeScene->GetContext().Schemas();
			const Schema::TypeSchema* schema = schemaRegistry.Find(compName);
			if (!schema || schema->Category != Schema::TypeCategory::Component || !schema->Storage)
				throw std::logic_error("ecs:OnAdd: '" + compName + "' is not a registered component type");

			const entt::id_type componentId = schema->Storage->ComponentId;
			const uint64_t handle = activeScene->AddComponentObserver(
				componentId,
				[fn, compName](Entity entity)
				{
					if (!entity.IsValid())
						return;

					ScriptBindingContext& context = ScriptEngine::GetBindingContext();
					const ScriptValue callArgs[] = { NewUserdataOf(context, "Entity", entity) };
					ScriptValue result;
					std::string callError;
					if (!fn.Call(callArgs, 1, &result, &callError))
					{
						if (Log::GetCoreLogger())
							WLD_CORE_ERROR("[Luau ECS OnAdd('{}')] callback error: {}", compName, callError);
					}
				},
				nullptr);

			return ScriptValue::Number(static_cast<double>(handle));
		}

		// -------------------------------------------------------------------------
		// ecs:OnRemove(componentName, fn) 实现
		// -------------------------------------------------------------------------
		ScriptValue OnRemoveImpl(const ScriptValue* args, std::size_t count)
		{
			std::size_t startIndex = 0;
			if (count >= 1 && args[0].IsTable())
			{
				startIndex = 1;
			}

			std::size_t remaining = count - startIndex;
			if (remaining < 2)
				throw std::logic_error("ecs:OnRemove expects (componentName, callbackFn)");

			std::string compName;
			if (!args[startIndex].AsString(&compName) || compName.empty())
				throw std::logic_error("ecs:OnRemove: component name must be a non-empty string");

			ScriptFunctionRef fn;
			if (!args[startIndex + 1].AsFunction(&fn) || !fn.IsValid())
				throw std::logic_error("ecs:OnRemove: callback must be a valid function");

			Scene* activeScene = ScriptEngine::GetActiveScene();
			if (!activeScene)
				throw std::logic_error("ecs:OnRemove requires an active scene");

			auto& schemaRegistry = activeScene->GetContext().Schemas();
			const Schema::TypeSchema* schema = schemaRegistry.Find(compName);
			if (!schema || schema->Category != Schema::TypeCategory::Component || !schema->Storage)
				throw std::logic_error("ecs:OnRemove: '" + compName + "' is not a registered component type");

			const entt::id_type componentId = schema->Storage->ComponentId;
			const uint64_t handle = activeScene->AddComponentObserver(
				componentId,
				nullptr,
				[fn, compName](Entity entity)
				{
					if (!entity.IsValid())
						return;

					ScriptBindingContext& context = ScriptEngine::GetBindingContext();
					const ScriptValue callArgs[] = { NewUserdataOf(context, "Entity", entity) };
					ScriptValue result;
					std::string callError;
					if (!fn.Call(callArgs, 1, &result, &callError))
					{
						if (Log::GetCoreLogger())
							WLD_CORE_ERROR("[Luau ECS OnRemove('{}')] callback error: {}", compName, callError);
					}
				});

			return ScriptValue::Number(static_cast<double>(handle));
		}

		// -------------------------------------------------------------------------
		// ecs:Off(handle) 实现
		// -------------------------------------------------------------------------
		ScriptValue OffImpl(const ScriptValue* args, std::size_t count)
		{
			std::size_t startIndex = 0;
			if (count >= 1 && args[0].IsTable())
			{
				startIndex = 1;
			}

			if (count <= startIndex)
				throw std::logic_error("ecs:Off expects an observer handle number");

			double handleNum = 0.0;
			if (!args[startIndex].AsNumber(&handleNum) || handleNum <= 0.0)
				return ScriptValue::Boolean(false);

			Scene* activeScene = ScriptEngine::GetActiveScene();
			if (!activeScene)
				throw std::logic_error("ecs:Off requires an active scene");

			const uint64_t observerId = static_cast<uint64_t>(handleNum);
			activeScene->RemoveComponentObserver(observerId);
			return ScriptValue::Boolean(true);
		}
	}

	// ecs 面 API 的唯一描述表:运行时注册循环与 LuaStubGenerator 的存根渲染共用同一份
	// (与 GameplayServiceBindings / ScriptEventBindings / ScriptUiBindings 同一条链路)。
	const ScriptServiceBinding* ScriptEcsBindings(std::size_t* count)
	{
		static const ScriptServiceParam queryParams[] = {
			{ "components", "table", ScriptServiceArgType::Table, true, "Array of registered component type names, e.g. { \"TransformComponent\", \"VelocityComponent\" }." },
		};
		static const ScriptServiceParam addSystemParams[] = {
			{ "name", "string", ScriptServiceArgType::String, true, "System name; re-registering the same name replaces the previous system." },
			{ "fn", "function", ScriptServiceArgType::None, true, "Called once per frame with dt in seconds." },
			{ "phase", "string", ScriptServiceArgType::String, false, "Pipeline phase; defaults to \"Update\". The legacy (name, phase, fn) order is also accepted." },
		};
		static const ScriptServiceParam removeSystemParams[] = {
			{ "name", "string", ScriptServiceArgType::String, true, "System name previously passed to ecs:AddSystem." },
		};
		static const ScriptServiceParam createEntityParams[] = {
			{ "name", "string", ScriptServiceArgType::String, false, "Optional entity name; defaults to \"Empty Entity\"." },
		};
		static const ScriptServiceParam destroyEntityParams[] = {
			{ "entity", "userdata", ScriptServiceArgType::None, true, "Entity to destroy; queued, committed at the next safe point." },
		};
		static const ScriptServiceParam onAddParams[] = {
			{ "component", "string", ScriptServiceArgType::String, true, "Registered component type name to observe." },
			{ "fn", "function", ScriptServiceArgType::None, true, "Called with the entity whenever the component is added." },
		};
		static const ScriptServiceParam onRemoveParams[] = {
			{ "component", "string", ScriptServiceArgType::String, true, "Registered component type name to observe." },
			{ "fn", "function", ScriptServiceArgType::None, true, "Called with the entity whenever the component is removed." },
		};
		static const ScriptServiceParam offParams[] = {
			{ "handle", "number", ScriptServiceArgType::Number, true, "Observer handle returned by ecs:OnAdd / ecs:OnRemove." },
		};
		static const ScriptServiceMethod methods[] = {
			{ "Query", &QueryImpl, queryParams, 1, 1, "table",
				"Build a query over the listed component types; the result has :Each(fn) and :Count()." },
			{ "AddSystem", &AddSystemImpl, addSystemParams, 3, 3, "boolean",
				"Register a named system on the active scene; idempotent (a repeated name replaces the previous system)." },
			{ "RemoveSystem", &RemoveSystemImpl, removeSystemParams, 1, 1, "boolean",
				"Unregister a named system; false when no such system is registered." },
			{ "CreateEntity", &CreateEntityImpl, createEntityParams, 1, 1, "Entity",
				"Create an entity (Tag + UUID) in the active scene and return its handle." },
			{ "DestroyEntity", &DestroyEntityImpl, destroyEntityParams, 1, 1, "boolean",
				"Queue an entity for destruction at the next safe point." },
			{ "EntityCount", &EntityCountImpl, nullptr, 0, 0, "number",
				"Number of live entities in the active scene." },
			{ "OnAdd", &OnAddImpl, onAddParams, 2, 2, "number",
				"Observe component additions; returns a handle for ecs:Off." },
			{ "OnRemove", &OnRemoveImpl, onRemoveParams, 2, 2, "number",
				"Observe component removals; returns a handle for ecs:Off." },
			{ "Off", &OffImpl, offParams, 1, 1, "boolean",
				"Cancel an observer handle; false when the handle is unknown." },
		};
		static const ScriptServiceBinding tables[] = {
			{ "ecs", "Read-only pure-ECS table (also exposed as \"world\"): queries, systems, entity lifecycle and component observers.",
				methods, sizeof(methods) / sizeof(methods[0]) },
		};
		if (count)
			*count = sizeof(tables) / sizeof(tables[0]);
		return tables;
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

		// 表驱动注册:与 ScriptEcsBindings() 共用同一份描述(名字 + 实现 + 存根签名)。
		std::size_t methodCount = 0;
		const ScriptServiceBinding* ecsDescriptor = ScriptEcsBindings(&methodCount);
		if (!ecsDescriptor || ecsDescriptor->MethodCount == 0)
		{
			if (error) *error = "the ecs binding descriptor is empty";
			return false;
		}
		for (std::size_t index = 0; index < ecsDescriptor->MethodCount; ++index)
		{
			const ScriptServiceMethod& method = ecsDescriptor->Methods[index];
			if (!method.Name || !method.Function || !method.Name[0])
			{
				if (error) *error = "the ecs binding descriptor has an invalid entry";
				return false;
			}
			if (!ecsTable.SetField(method.Name, bindings.CreateFunction(method.Name, method.Function)))
			{
				if (error) *error = std::string("failed to bind ecs.") + method.Name;
				return false;
			}
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
