#include "wldpch.h"
#include "World/Script/BindECS.h"
#include "World/Script/BindServices.h"

#include "World/Core/Log.h"
#include "World/Core/WorldContext.h"
#include "World/Gameplay/SystemRegistry.h"
#include "World/Scene/Components.h"
#include "World/Scene/Entity.h"
#include "World/Scene/LuaType/LuaTypeHelpers.h"
#include "World/Scene/Scene.h"
#include "World/Scene/ScriptEngine.h"
#include "World/Schema/SchemaRegistry.h"
#include "World/Script/BindComponentAccess.h"
#include "World/Script/LuauHeaders.h"
#include "World/Script/LuauVm.h"
#include "World/Script/ScriptBindingContext.h"
#include "World/Script/ScriptRef.h"
#include "World/Script/ScriptValue.h"

#include <cstddef>
#include <initializer_list>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace World
{
	namespace
	{
		using LuaTypeDetail::NewUserdataOf;

		// 系统阶段名 → 枚举。合法取值与 SystemPhaseName() 的输出**逐字一致**(大小写敏感):
		// 这里遍历枚举取值比对,而不是另抄一份字符串,保证与单一事实源不漂移。
		bool ParseSystemPhase(const std::string& name, Gameplay::SystemPhase* out)
		{
			for (int index = 0; index < static_cast<int>(Gameplay::SystemPhase::Count); ++index)
			{
				const auto phase = static_cast<Gameplay::SystemPhase>(index);
				const char* phaseName = Gameplay::SystemPhaseName(phase);
				if (phaseName && name == phaseName)
				{
					if (out)
						*out = phase;
					return true;
				}
			}
			return false;
		}

		std::string LegalSystemPhaseList()
		{
			std::string list;
			for (int index = 0; index < static_cast<int>(Gameplay::SystemPhase::Count); ++index)
			{
				if (!list.empty())
					list += ", ";
				list += "'";
				list += Gameplay::SystemPhaseName(static_cast<Gameplay::SystemPhase>(index));
				list += "'";
			}
			return list;
		}

		Gameplay::SystemPhase RequireSystemPhase(const std::string& systemName, const ScriptValue& value)
		{
			std::string phaseName;
			if (!value.IsString() || !value.AsString(&phaseName) || phaseName.empty())
				throw std::logic_error("ecs:AddSystem('" + systemName + "'): phase must be a non-empty string");
			Gameplay::SystemPhase phase = Gameplay::SystemPhase::Update;
			if (!ParseSystemPhase(phaseName, &phase))
				throw std::logic_error("ecs:AddSystem('" + systemName + "'): unknown phase '" + phaseName +
					"'; valid phases are " + LegalSystemPhaseList());
			return phase;
		}

		// after 只接受字符串数组;依赖是否存在交由 SystemRegistry::RunPhase 判定
		// (缺失依赖 = 跳过该系统 + 记警告),绑定层不另立更严的存在性校验。
		std::vector<std::string> RequireAfterSystems(const std::string& systemName, const ScriptValue& value)
		{
			std::vector<std::string> after;
			if (value.IsNil())
				return after;

			ScriptTableRef table;
			if (!value.IsTable() || !value.AsTable(&table) || !table.IsValid())
				throw std::logic_error("ecs:AddSystem('" + systemName +
					"'): option 'after' must be an array of system name strings");

			for (const ScriptValue& item : table.GetArray())
			{
				std::string dependency;
				if (!item.AsString(&dependency) || dependency.empty())
					throw std::logic_error("ecs:AddSystem('" + systemName +
						"'): option 'after' elements must be non-empty system name strings");
				after.push_back(std::move(dependency));
			}
			return after;
		}

		// 选项表的未知键校验:ScriptTableRef 只暴露字段读写、不枚举键,这里用 Lua 表遍历。
		// 只接受白名单键(含数组下标在内的其它键一律拒绝),报错时列出合法键,不静默忽略。
		void RejectUnknownOptionKeys(const ScriptTableRef& table, std::initializer_list<const char*> allowedKeys,
			const char* api)
		{
			lua_State* state = table.State();
			if (!state || !table.Push(state))
				throw std::logic_error(std::string(api) + ": options table is not valid");

			lua_pushnil(state);
			while (lua_next(state, -2) != 0)
			{
				// 栈: table, key, value
				bool known = false;
				if (lua_type(state, -2) == LUA_TSTRING)
				{
					const char* key = lua_tostring(state, -2);
					if (key)
					{
						for (const char* allowed : allowedKeys)
						{
							if (std::string(key) == allowed)
							{
								known = true;
								break;
							}
						}
					}
				}

				if (!known)
				{
					std::string allowedList;
					for (const char* allowed : allowedKeys)
					{
						if (!allowedList.empty())
							allowedList += ", ";
						allowedList += "'";
						allowedList += allowed;
						allowedList += "'";
					}
					lua_pop(state, 2);   // value + table
					throw std::logic_error(std::string(api) + ": unknown option key (allowed: " + allowedList + ")");
				}

				lua_pop(state, 1);       // 弹出 value,保留 key 供 lua_next 继续
			}
			lua_pop(state, 1);           // 弹出 options table
		}

		// -------------------------------------------------------------------------
		// ecs:Query(componentNames [, options]) 实现
		// -------------------------------------------------------------------------
		ScriptValue QueryImpl(const ScriptValue* args, std::size_t count)
		{
			ScriptBindingContext& bindings = ScriptEngine::GetBindingContext();

			// 参数形态(按**内容**判 self,不按参数个数):
			//   ecs:Query({...})                      冒号调用 ⇒ args[0] 是 ecs 表(self),组件列表 = args[1]
			//   ecs:Query({...}, { without = {...} }) 同上,选项表 = args[2]
			//   ecs.Query({...}) / ecs.Query({...}, {...})  点调用 ⇒ args[0] 就是组件列表
			//   ecs:Query("A", "B")                   冒号调用 + 连续字符串组件名
			//   ecs:Query("A", { without = {...} })   冒号调用 + 字符串组件名 + 选项表
			// 判据:`ecs`/`world` 表的字段是命名的 ⇒ GetArray() 为空;组件列表有字符串数组元素。
			std::size_t index = 0;
			if (count >= 1 && args[0].IsTable())
			{
				ScriptTableRef first;
				if (args[0].AsTable(&first) && first.IsValid() && first.GetArray().empty())
					index = 1;   // 无数组元素的表 = ecs/world 表(self)
			}

			// 组件名阶段(必填):连续的字符串 / 字符串数组都算组件名。
			// 遇到"没有数组部分的表"即视为选项表,组件名阶段结束。
			std::vector<std::string> componentNames;
			for (; index < count; ++index)
			{
				const ScriptValue& arg = args[index];
				if (arg.IsString())
				{
					std::string name;
					arg.AsString(&name);
					if (!name.empty())
						componentNames.push_back(std::move(name));
					continue;
				}
				if (!arg.IsTable())
					throw std::logic_error("ecs:Query: argument must be a table or string of component names");

				ScriptTableRef table;
				if (!arg.AsTable(&table) || !table.IsValid())
					throw std::logic_error("ecs:Query: argument must be a table or string of component names");

				std::vector<ScriptValue> arr = table.GetArray();
				if (arr.empty())
					break;   // 无数组部分 ⇒ 选项表,交给下面的选项阶段

				for (const ScriptValue& item : arr)
				{
					std::string name;
					if (item.AsString(&name) && !name.empty())
						componentNames.push_back(std::move(name));
					else
						throw std::logic_error("ecs:Query: array elements must be component name strings");
				}
			}

			// 选项表阶段(可选,最多一个):目前只认 `without`(排除过滤);未知键报可读错误。
			std::vector<std::string> withoutNames;
			if (index < count)
			{
				ScriptTableRef options;
				if (!args[index].IsTable() || !args[index].AsTable(&options) || !options.IsValid())
					throw std::logic_error("ecs:Query: options must be a table");
				++index;

				RejectUnknownOptionKeys(options, { "without" }, "ecs:Query");

				ScriptValue withoutValue = options.GetField("without");
				if (!withoutValue.IsNil())
				{
					ScriptTableRef withoutTable;
					if (!withoutValue.IsTable() || !withoutValue.AsTable(&withoutTable) || !withoutTable.IsValid())
						throw std::logic_error("ecs:Query: option 'without' must be an array of component name strings");
					for (const ScriptValue& item : withoutTable.GetArray())
					{
						std::string name;
						if (item.AsString(&name) && !name.empty())
							withoutNames.push_back(std::move(name));
						else
							throw std::logic_error("ecs:Query: option 'without' elements must be component name strings");
					}
				}
			}
			if (index < count)
				throw std::logic_error("ecs:Query: too many arguments");

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

			// 排除集合与包含集合同一口径:未注册/非组件/无存储的名字都报可读错误。
			std::vector<const Schema::TypeSchema*> excludeSchemas;
			excludeSchemas.reserve(withoutNames.size());
			for (const auto& name : withoutNames)
			{
				const Schema::TypeSchema* type = schemaRegistry.Find(name);
				if (!type || type->Category != Schema::TypeCategory::Component || !type->Storage)
					throw std::logic_error("ecs:Query: '" + name + "' is not a registered component type");
				excludeSchemas.push_back(type);
			}

			LuauVm* vm = bindings.Vm();
			if (!vm)
				throw std::logic_error("ecs:Query: Luau VM is not valid");

			ScriptTableRef queryTable = vm->CreateTable();
			if (!queryTable.IsValid())
				throw std::logic_error("ecs:Query: failed to create query table");

			// Method: Each(callback)
			queryTable.SetField("Each", bindings.CreateFunction("Query:Each",
				[schemas, excludeSchemas](const ScriptValue* eachArgs, std::size_t eachCount) -> ScriptValue
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

					// 排除集合:存储不存在或为空 ⇒ 没有任何实体带该组件,无需过滤。
					std::vector<entt::sparse_set*> excludeStorages;
					excludeStorages.reserve(excludeSchemas.size());
					for (const auto* schema : excludeSchemas)
					{
						auto* storage = registry.storage(schema->Storage->ComponentId);
						if (storage && !storage->empty())
							excludeStorages.push_back(storage);
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
						if (!matchesAll)
							continue;

						bool excluded = false;
						for (entt::sparse_set* excludeStorage : excludeStorages)
						{
							if (excludeStorage->contains(entity))
							{
								excluded = true;
								break;
							}
						}
						if (!excluded)
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
				[schemas, excludeSchemas](const ScriptValue*, std::size_t) -> ScriptValue
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

					std::vector<entt::sparse_set*> excludeStorages;
					excludeStorages.reserve(excludeSchemas.size());
					for (const auto* schema : excludeSchemas)
					{
						auto* storage = registry.storage(schema->Storage->ComponentId);
						if (storage && !storage->empty())
							excludeStorages.push_back(storage);
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
						if (!matchesAll)
							continue;

						bool excluded = false;
						for (entt::sparse_set* excludeStorage : excludeStorages)
						{
							if (excludeStorage->contains(entity))
							{
								excluded = true;
								break;
							}
						}
						if (!excluded)
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
		// ecs:AddSystem(name, fn [, phase | options]) 实现
		//   (name, fn)                                     —— phase 默认 "Update"
		//   (name, fn, "Late")                             —— 字符串 phase
		//   (name, fn, { phase = "Late", after = { "X" } }) —— 表形态(新)
		//   (name, "Update", fn)                           —— 最旧参数序,继续兼容
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
				throw std::logic_error("ecs:AddSystem expects (name, fn [, phase | options])");

			std::string name;
			if (!args[startIndex].AsString(&name) || name.empty())
				throw std::logic_error("ecs:AddSystem: system name must be a non-empty string");

			Gameplay::SystemPhase phase = Gameplay::SystemPhase::Update;
			std::vector<std::string> after;
			ScriptFunctionRef updateFn;

			if (args[startIndex + 1].IsFunction())
			{
				if (!args[startIndex + 1].AsFunction(&updateFn) || !updateFn.IsValid())
					throw std::logic_error("ecs:AddSystem: expected a function for system update");

				if (remaining >= 3 && !args[startIndex + 2].IsNil())
				{
					const ScriptValue& spec = args[startIndex + 2];
					if (spec.IsString())
					{
						phase = RequireSystemPhase(name, spec);
					}
					else if (spec.IsTable())
					{
						ScriptTableRef options;
						if (!spec.AsTable(&options) || !options.IsValid())
							throw std::logic_error("ecs:AddSystem('" + name + "'): options table is not valid");
						RejectUnknownOptionKeys(options, { "phase", "after" }, "ecs:AddSystem");

						const ScriptValue phaseValue = options.GetField("phase");
						if (!phaseValue.IsNil())
							phase = RequireSystemPhase(name, phaseValue);
						after = RequireAfterSystems(name, options.GetField("after"));
					}
					else
					{
						throw std::logic_error("ecs:AddSystem('" + name +
							"'): third argument must be a phase string or an options table { phase = ..., after = { ... } }");
					}
				}
			}
			else
			{
				// 最旧参数序 (name, phase, fn):phase 仍只接受字符串。
				if (!args[startIndex + 1].IsString())
					throw std::logic_error("ecs:AddSystem('" + name +
						"'): expected a function for system update, or the legacy (name, phase, fn) order with a phase string");
				phase = RequireSystemPhase(name, args[startIndex + 1]);
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
				},
				phase,
				std::move(after)
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
			{ "options", "table", ScriptServiceArgType::Table, false, "Optional options table; only 'without' is recognized: { without = { \"DeadTag\" } } excludes entities that carry those components." },
		};
		static const ScriptServiceParam addSystemParams[] = {
			{ "name", "string", ScriptServiceArgType::String, true, "System name; re-registering the same name replaces the previous system." },
			{ "fn", "function", ScriptServiceArgType::None, true, "Called once per frame with dt in seconds." },
			{ "phase", "string|table", ScriptServiceArgType::String | ScriptServiceArgType::Table, false, "Pipeline phase name (PreFixed/Fixed/Update/Late/PreRender; defaults to \"Update\"), or an options table { phase = \"Late\", after = { \"OtherSystem\" } }. The legacy (name, phase, fn) order is also accepted." },
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
			{ "Query", &QueryImpl, queryParams, 2, 2, "table",
				"Build a query over the listed component types, optionally excluding entities that carry the components named in options.without; the result has :Each(fn) and :Count()." },
			{ "AddSystem", &AddSystemImpl, addSystemParams, 3, 3, "boolean",
				"Register a named system on the active scene at an optional pipeline phase with optional same-phase ordering dependencies (options table: phase, after); idempotent (a repeated name replaces the previous system)." },
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
