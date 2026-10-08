#include "wldpch.h"
#include "World/Profiling/ProfilingMacros.h"
#include "World/Script/Bindings/BindECS.h"
#include "World/Script/Bindings/BindServices.h"

#include "World/Core/Log.h"
#include "World/Core/WorldContext.h"
#include "World/Gameplay/Runtime/SystemRegistry.h"
#include "World/Physics/PhysicsEvents.h"
#include "World/Scene/Components.h"
#include "World/Scene/Entity.h"
#include "World/Script/LuaType/LuaTypeHelpers.h"
#include "World/Scene/Scene.h"
#include "World/Script/Runtime/ScriptEngine.h"
#include "World/Schema/SchemaRegistry.h"
#include "World/Script/Bindings/BindComponentAccess.h"
#include "World/Script/Vm/LuauHeaders.h"
#include "World/Script/Vm/LuauVm.h"
#include "World/Script/Vm/ScriptBindingContext.h"
#include "World/Script/Vm/ScriptRef.h"
#include "World/Script/Vm/ScriptValue.h"

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
			std::vector<std::string> changedNames;
			if (index < count)
			{
				ScriptTableRef options;
				if (!args[index].IsTable() || !args[index].AsTable(&options) || !options.IsValid())
					throw std::logic_error("ecs:Query: options must be a table");
				++index;

				RejectUnknownOptionKeys(options, { "without", "changed" }, "ecs:Query");

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

				ScriptValue changedValue = options.GetField("changed");
				if (!changedValue.IsNil())
				{
					ScriptTableRef changedTable;
					if (!changedValue.IsTable() || !changedValue.AsTable(&changedTable) || !changedTable.IsValid())
						throw std::logic_error("ecs:Query: option 'changed' must be an array of component name strings");
					for (const ScriptValue& item : changedTable.GetArray())
					{
						std::string name;
						if (item.AsString(&name) && !name.empty())
							changedNames.push_back(std::move(name));
						else
							throw std::logic_error("ecs:Query: option 'changed' elements must be component name strings");
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

			std::vector<const Schema::TypeSchema*> changedSchemas;
			changedSchemas.reserve(changedNames.size());
			for (const auto& name : changedNames)
			{
				const Schema::TypeSchema* type = schemaRegistry.Find(name);
				if (!type || type->Category != Schema::TypeCategory::Component || !type->Storage)
					throw std::logic_error("ecs:Query: '" + name + "' is not a registered component type in changed list");
				changedSchemas.push_back(type);
			}

			auto lastQueryTick = std::make_shared<uint64_t>(0);
			auto pooledCallArgs = std::make_shared<std::vector<ScriptValue>>();

			// Method: Each(callback)
			queryTable.SetField("Each", bindings.CreateFunction("Query:Each",
				[schemas, excludeSchemas, changedSchemas, lastQueryTick, pooledCallArgs](const ScriptValue* eachArgs, std::size_t eachCount) -> ScriptValue
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

					const auto& registry = static_cast<const Scene*>(scene)->GetRegistry();
					ScriptBindingContext& context = ScriptEngine::GetBindingContext();

					std::vector<const entt::sparse_set*> storages;
					storages.reserve(schemas.size());
					for (const auto* schema : schemas)
					{
						const auto* storage = registry.storage(schema->Storage->ComponentId);
						if (!storage || storage->empty())
							return ScriptValue::Nil();
						storages.push_back(storage);
					}

					// 排除集合:存储不存在或为空 ⇒ 没有任何实体带该组件,无需过滤。
					std::vector<const entt::sparse_set*> excludeStorages;
					excludeStorages.reserve(excludeSchemas.size());
					for (const auto* schema : excludeSchemas)
					{
						const auto* storage = registry.storage(schema->Storage->ComponentId);
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

					const entt::sparse_set* leadStorage = storages[minIdx];
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
						for (const entt::sparse_set* excludeStorage : excludeStorages)
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

					// 变更检测:若配置了 changed 且上轮查询后无目标组件变动,O(1) 整块跳过
					if (!changedSchemas.empty() && *lastQueryTick > 0)
					{
						bool anyChanged = false;
						for (const auto* cs : changedSchemas)
						{
							if (scene->GetComponentChangeTick(cs->Storage->ComponentId) >= *lastQueryTick)
							{
								anyChanged = true;
								break;
							}
						}
						if (!anyChanged)
							return ScriptValue::Nil();
					}
					*lastQueryTick = scene->CurrentWorldTick();

					// 零 GC 享元代理复用:在查询对象生命周期内仅分配单套实参与组件代理,循环内外就地更新载荷
					if (pooledCallArgs->empty())
					{
						pooledCallArgs->reserve(1 + schemas.size());
						pooledCallArgs->push_back(NewUserdataOf(context, "Entity", Entity()));
						for (const auto* schema : schemas)
						{
							pooledCallArgs->push_back(MakeComponentProxy(context, Entity(), *schema));
						}
					}
					auto& callArgs = *pooledCallArgs;

					for (entt::entity entity : matches)
					{
						if (!registry.valid(entity) || scene->IsPendingDestroy(entity))
							continue;

						Entity entityHandle(scene, entity);
						if (!entityHandle.IsValid())
							continue;

						Entity* entPtr = nullptr;
						if (context.Unwrap<Entity>("Entity", callArgs[0], &entPtr) && entPtr)
							*entPtr = entityHandle;

						for (std::size_t i = 0; i < schemas.size(); ++i)
						{
							UpdateComponentProxyEntity(context, callArgs[1 + i], entityHandle);
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

					const auto& registry = static_cast<const Scene*>(scene)->GetRegistry();

					std::vector<const entt::sparse_set*> storages;
					storages.reserve(schemas.size());
					for (const auto* schema : schemas)
					{
						const auto* storage = registry.storage(schema->Storage->ComponentId);
						if (!storage || storage->empty())
							return ScriptValue::Number(0.0);
						storages.push_back(storage);
					}

					std::vector<const entt::sparse_set*> excludeStorages;
					excludeStorages.reserve(excludeSchemas.size());
					for (const auto* schema : excludeSchemas)
					{
						const auto* storage = registry.storage(schema->Storage->ComponentId);
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

					const entt::sparse_set* leadStorage = storages[minIdx];
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
						for (const entt::sparse_set* excludeStorage : excludeStorages)
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
			if (remaining < 1)
				throw std::logic_error("ecs:AddSystem expects (name, fn [, phase | options]) or a table { name, fn/update, phase, after }");

			std::string name;
			Gameplay::SystemPhase phase = Gameplay::SystemPhase::Update;
			std::vector<std::string> after;
			float interval = 0.0f;
			std::function<bool()> condition = nullptr;
			ScriptFunctionRef updateFn;

			if (remaining == 1 && args[startIndex].IsTable())
			{
				ScriptTableRef config;
				if (!args[startIndex].AsTable(&config) || !config.IsValid())
					throw std::logic_error("ecs:AddSystem: configuration table is not valid");
				RejectUnknownOptionKeys(config, { "name", "fn", "update", "phase", "after", "interval", "condition" }, "ecs:AddSystem");

				const ScriptValue nameVal = config.GetField("name");
				if (!nameVal.AsString(&name) || name.empty())
					throw std::logic_error("ecs:AddSystem: table must contain a non-empty 'name' string");

				ScriptValue fnVal = config.GetField("fn");
				if (fnVal.IsNil())
					fnVal = config.GetField("update");
				if (!fnVal.IsFunction() || !fnVal.AsFunction(&updateFn) || !updateFn.IsValid())
					throw std::logic_error("ecs:AddSystem('" + name + "'): expected 'fn' or 'update' function in table");

				const ScriptValue phaseVal = config.GetField("phase");
				if (!phaseVal.IsNil())
					phase = RequireSystemPhase(name, phaseVal);

				after = RequireAfterSystems(name, config.GetField("after"));

				const ScriptValue intervalVal = config.GetField("interval");
				if (intervalVal.IsNumber())
				{
					double dInt = 0.0;
					intervalVal.AsNumber(&dInt);
					interval = static_cast<float>(dInt);
				}

				const ScriptValue condVal = config.GetField("condition");
				if (condVal.IsFunction())
				{
					ScriptFunctionRef condFn;
					if (condVal.AsFunction(&condFn) && condFn.IsValid())
					{
						condition = [condFn]() mutable -> bool
						{
							ScriptValue res;
							std::string err;
							if (!condFn.Call(nullptr, 0, &res, &err))
								return false;
							bool ok = true;
							if (res.IsBoolean()) res.AsBool(&ok);
							return ok;
						};
					}
				}
			}
			else
			{
				if (remaining < 2)
					throw std::logic_error("ecs:AddSystem expects (name, fn [, phase | options])");

				if (!args[startIndex].AsString(&name) || name.empty())
					throw std::logic_error("ecs:AddSystem: system name must be a non-empty string");

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
							RejectUnknownOptionKeys(options, { "phase", "after", "interval", "condition" }, "ecs:AddSystem");

							const ScriptValue phaseValue = options.GetField("phase");
							if (!phaseValue.IsNil())
								phase = RequireSystemPhase(name, phaseValue);
							after = RequireAfterSystems(name, options.GetField("after"));

							const ScriptValue intervalVal = options.GetField("interval");
							if (intervalVal.IsNumber())
							{
								double dInt = 0.0;
								intervalVal.AsNumber(&dInt);
								interval = static_cast<float>(dInt);
							}

							const ScriptValue condVal = options.GetField("condition");
							if (condVal.IsFunction())
							{
								ScriptFunctionRef condFn;
								if (condVal.AsFunction(&condFn) && condFn.IsValid())
								{
									condition = [condFn]() mutable -> bool
									{
										ScriptValue res;
										std::string err;
										if (!condFn.Call(nullptr, 0, &res, &err))
											return false;
										bool ok = true;
										if (res.IsBoolean()) res.AsBool(&ok);
										return ok;
									};
								}
							}
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
					// 脚本 tick 的显式作用域:**外层**由 SystemRegistry 按系统名包
					// (告诉你"是哪个 Lua 系统"),这里这一层是 **VM 调用本身**的耗时。
					// 名字必须是静态字面量(见 TraceEvents.h R2),故不拼 name。
					WLD_TRACE_SCOPE("Lua.Update");
					WLD_MEM_TAG("Script");
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
				std::move(after),
				std::type_index(typeid(void)),
				interval,
				std::move(condition)
			});
			// WP6:脚本系统标归属(面板显示 Lua:<系统名>)。
			activeScene->SetFrameSystemOwner(name, "Lua:" + name);

			// 归属到当前正在执行的系统脚本(不在加载系统脚本时是 no-op)。
			ScriptEngine::NoteScriptSystem(name);

			return ScriptValue::Boolean(true);
		}

		// -------------------------------------------------------------------------
		// ecs:AddStartupSystem(name, fn) 实现
		// -------------------------------------------------------------------------
		ScriptValue AddStartupSystemImpl(const ScriptValue* args, std::size_t count)
		{
			std::size_t startIndex = 0;
			if (count >= 1 && args[0].IsTable())
				startIndex = 1;

			std::size_t remaining = count - startIndex;
			if (remaining < 2)
				throw std::logic_error("ecs:AddStartupSystem expects (name, fn)");

			std::string name;
			if (!args[startIndex].AsString(&name) || name.empty())
				throw std::logic_error("ecs:AddStartupSystem: system name must be a non-empty string");

			ScriptFunctionRef fn;
			if (!args[startIndex + 1].AsFunction(&fn) || !fn.IsValid())
				throw std::logic_error("ecs:AddStartupSystem: expected a function");

			Scene* activeScene = ScriptEngine::GetActiveScene();
			if (!activeScene)
				throw std::logic_error("ecs:AddStartupSystem requires an active scene");

			activeScene->RegisterStartupSystem({
				name,
				[fn, name](Scene&)
				{
					ScriptValue res;
					std::string err;
					if (!fn.Call(nullptr, 0, &res, &err))
					{
						if (Log::GetCoreLogger())
							WLD_CORE_ERROR("[Luau ECS StartupSystem('{}')] failed: {}", name, err);
					}
				}
			});
			ScriptEngine::NoteScriptSystem(name);
			return ScriptValue::Boolean(true);
		}

		// -------------------------------------------------------------------------
		// ecs:AddTeardownSystem(name, fn) 实现
		// -------------------------------------------------------------------------
		ScriptValue AddTeardownSystemImpl(const ScriptValue* args, std::size_t count)
		{
			std::size_t startIndex = 0;
			if (count >= 1 && args[0].IsTable())
				startIndex = 1;

			std::size_t remaining = count - startIndex;
			if (remaining < 2)
				throw std::logic_error("ecs:AddTeardownSystem expects (name, fn)");

			std::string name;
			if (!args[startIndex].AsString(&name) || name.empty())
				throw std::logic_error("ecs:AddTeardownSystem: system name must be a non-empty string");

			ScriptFunctionRef fn;
			if (!args[startIndex + 1].AsFunction(&fn) || !fn.IsValid())
				throw std::logic_error("ecs:AddTeardownSystem: expected a function");

			Scene* activeScene = ScriptEngine::GetActiveScene();
			if (!activeScene)
				throw std::logic_error("ecs:AddTeardownSystem requires an active scene");

			activeScene->RegisterTeardownSystem({
				name,
				[fn, name](Scene&)
				{
					ScriptValue res;
					std::string err;
					if (!fn.Call(nullptr, 0, &res, &err))
					{
						if (Log::GetCoreLogger())
							WLD_CORE_ERROR("[Luau ECS TeardownSystem('{}')] failed: {}", name, err);
					}
				}
			});
			ScriptEngine::NoteScriptSystem(name);
			return ScriptValue::Boolean(true);
		}

		// -------------------------------------------------------------------------
		// ecs:OnChange(componentName, fn) 实现
		// -------------------------------------------------------------------------
		ScriptValue OnChangeImpl(const ScriptValue* args, std::size_t count)
		{
			std::size_t startIndex = 0;
			if (count >= 1 && args[0].IsTable())
				startIndex = 1;

			std::size_t remaining = count - startIndex;
			if (remaining < 2)
				throw std::logic_error("ecs:OnChange expects (componentName, callbackFn)");

			std::string compName;
			if (!args[startIndex].AsString(&compName) || compName.empty())
				throw std::logic_error("ecs:OnChange: component name must be a non-empty string");

			ScriptFunctionRef fn;
			if (!args[startIndex + 1].AsFunction(&fn) || !fn.IsValid())
				throw std::logic_error("ecs:OnChange: callback must be a valid function");

			Scene* activeScene = ScriptEngine::GetActiveScene();
			if (!activeScene)
				throw std::logic_error("ecs:OnChange requires an active scene");

			auto& schemaRegistry = activeScene->GetContext().Schemas();
			const Schema::TypeSchema* schema = schemaRegistry.Find(compName);
			if (!schema || schema->Category != Schema::TypeCategory::Component || !schema->Storage)
				throw std::logic_error("ecs:OnChange: '" + compName + "' is not a registered component type");

			const entt::id_type componentId = schema->Storage->ComponentId;
			const uint64_t handle = activeScene->AddComponentChangeObserver(
				componentId,
				[fn, compName, schema](Entity entity)
				{
					if (!entity.IsValid())
						return;

					ScriptBindingContext& context = ScriptEngine::GetBindingContext();
					const ScriptValue callArgs[] = {
						NewUserdataOf(context, "Entity", entity),
						MakeComponentProxy(context, entity, *schema)
					};
					ScriptValue result;
					std::string callError;
					if (!fn.Call(callArgs, 2, &result, &callError))
					{
						if (Log::GetCoreLogger())
							WLD_CORE_ERROR("[Luau ECS OnChange('{}')] callback error: {}", compName, callError);
					}
				});

			return ScriptValue::Number(static_cast<double>(handle));
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
		// ecs:CreateRawEntity() 实现 (零 Tag / 零 UUID 的轻量匿名实体)
		// -------------------------------------------------------------------------
		ScriptValue CreateRawEntityImpl(const ScriptValue*, std::size_t)
		{
			ScriptBindingContext& bindings = ScriptEngine::GetBindingContext();
			Scene* activeScene = ScriptEngine::GetActiveScene();
			if (!activeScene)
				throw std::logic_error("ecs:CreateRawEntity requires an active scene");

			Entity entity = activeScene->CreateRawEntity();
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
			const auto& registry = static_cast<const Scene*>(activeScene)->GetRegistry();
			const auto* entityStorage = registry.storage<entt::entity>();
			if (entityStorage)
			{
				for (auto it = entityStorage->begin(); it != entityStorage->end(); ++it)
				{
					entt::entity entity = *it;
					if (registry.valid(entity) && !activeScene->IsPendingDestroy(entity))
						entityCount += 1.0;
				}
			}

			return ScriptValue::Number(entityCount);
		}

		// -------------------------------------------------------------------------
		// P5:物理事件(ecs:OnContact / ecs:OnTrigger)
		//
		// 物理事实由**固定步长阶段**产出到 Scene 的事件队列,可变阶段(Update/Late/PreRender)
		// 每帧读一次(队列由 RunFixedFrameSystems / RunFrameSystems 每帧恰好清空一次)。
		// 绑定层用**脚本层自己注册的帧系统** `lua-physics-events`(Late 阶段)做每帧派发 ——
		// 不需要给 Scene.cpp 加钩子(与 ecs:AddSystem 同一条 RegisterFrameSystem 通道)。
		//
		// 生命周期与既有 ecs:OnAdd/OnRemove 观察者一致:订阅保存在脚本层,只有 ecs:Off
		// 显式注销;系统脚本热重载是"整份重跑",重跑时再次订阅就会新增一条(与 OnAdd 同口径);
		// VM Shutdown 后 ScriptFunctionRef 自动失效(IsValid()==false),不会访问已关闭的 registry。
		//
		// 句柄全局唯一且与 Scene 的组件观察者 id 用不相交区段(见 kPhysicsEventHandleBase),
		// 所以单个 ecs:Off 能同时注销两类句柄且不会误伤。
		constexpr const char* kPhysicsEventSystemName = "lua-physics-events";
		constexpr std::uint64_t kPhysicsEventHandleBase = 1ull << 40;   // < 2^53,Lua number 可精确表示

		struct PhysicsEventSubscription
		{
			std::uint64_t Id = 0;
			bool Trigger = false;             // false = ContactEvent,true = TriggerEvent
			ScriptFunctionRef Callback;
		};

		struct PhysicsEventDispatchState
		{
			// 只在帧系统存活期间被访问:State 由该场景的帧系统 lambda 以 shared_ptr 持有,
			// 场景销毁 ⇒ 帧系统销毁 ⇒ State 释放,OwnerScene 不会悬垂。
			Scene* OwnerScene = nullptr;
			std::vector<PhysicsEventSubscription> Subscriptions;
		};

		std::vector<std::weak_ptr<PhysicsEventDispatchState>>& PhysicsEventStates()
		{
			static std::vector<std::weak_ptr<PhysicsEventDispatchState>> states;
			return states;
		}

		std::uint64_t NextPhysicsEventHandle()
		{
			static std::uint64_t next = kPhysicsEventHandleBase;
			return ++next;
		}

		const char* ContactPhaseToLua(Physics::ContactPhase phase)
		{
			switch (phase)
			{
				case Physics::ContactPhase::Begin: return "Begin";
				case Physics::ContactPhase::Persist: return "Persist";
				case Physics::ContactPhase::End: return "End";
			}
			return "Begin";
		}

		ScriptValue BoxEntityForScene(Scene& scene, entt::entity handle)
		{
			ScriptBindingContext& bindings = ScriptEngine::GetBindingContext();
			return NewUserdataOf(bindings, "Entity", Entity(&scene, handle));
		}

		ScriptValue BoxVector3(const glm::vec3& value)
		{
			ScriptTableRef table = ScriptEngine::GetState().CreateTable();
			table.SetField("x", ScriptValue::Number(static_cast<double>(value.x)));
			table.SetField("y", ScriptValue::Number(static_cast<double>(value.y)));
			table.SetField("z", ScriptValue::Number(static_cast<double>(value.z)));
			return table.ToValue();
		}

		void ReportPhysicsCallbackError(const char* api, const std::string& error)
		{
			if (Log::GetCoreLogger())
				WLD_CORE_ERROR("[Luau ECS {}] callback error: {}", api, error);
		}

		// 单条事件的隔离派发:构造载荷 + 保护调用;失败只记录,不打断其它订阅者、不打断帧。
		void DispatchContactToLua(const ScriptFunctionRef& callback, Scene& scene, const Physics::ContactEvent& event)
		{
			try
			{
				ScriptTableRef payload = ScriptEngine::GetState().CreateTable();
				payload.SetField("a", BoxEntityForScene(scene, event.EntityA));
				payload.SetField("b", BoxEntityForScene(scene, event.EntityB));
				payload.SetField("phase", ScriptValue::String(ContactPhaseToLua(event.Phase)));
				payload.SetField("point", BoxVector3(event.Point));
				payload.SetField("normal", BoxVector3(event.Normal));
				payload.SetField("depth", ScriptValue::Number(static_cast<double>(event.PenetrationDepth)));
				if (event.PointCount > 0)
				{
					ScriptTableRef pointsTable = ScriptEngine::GetState().CreateTable();
					for (uint8_t i = 0; i < event.PointCount; ++i)
					{
						const auto& pt = event.Points[i];
						ScriptTableRef ptTable = ScriptEngine::GetState().CreateTable();
						ptTable.SetField("point", BoxVector3(pt.Position));
						ptTable.SetField("normal", BoxVector3(pt.Normal));
						ptTable.SetField("depth", ScriptValue::Number(static_cast<double>(pt.PenetrationDepth)));
						ptTable.SetField("colliderIndexA", ScriptValue::Number(pt.ColliderIndexA));
						ptTable.SetField("colliderIndexB", ScriptValue::Number(pt.ColliderIndexB));
						pointsTable.SetArrayElement(i + 1, ptTable.ToValue());
					}
					payload.SetField("points", pointsTable.ToValue());
				}

				const ScriptValue callArgs[] = { payload.ToValue() };
				ScriptValue result;
				std::string callError;
				if (!callback.Call(callArgs, 1, &result, &callError))
					ReportPhysicsCallbackError("OnContact", callError);
			}
			catch (const std::exception& error)
			{
				ReportPhysicsCallbackError("OnContact", error.what());
			}
			catch (...)
			{
				ReportPhysicsCallbackError("OnContact", "unknown exception");
			}
		}

		void DispatchTriggerToLua(const ScriptFunctionRef& callback, Scene& scene, const Physics::TriggerEvent& event)
		{
			try
			{
				ScriptTableRef payload = ScriptEngine::GetState().CreateTable();
				payload.SetField("sensor", BoxEntityForScene(scene, event.SensorEntity));
				payload.SetField("other", BoxEntityForScene(scene, event.OtherEntity));
				payload.SetField("phase", ScriptValue::String(ContactPhaseToLua(event.Phase)));

				const ScriptValue callArgs[] = { payload.ToValue() };
				ScriptValue result;
				std::string callError;
				if (!callback.Call(callArgs, 1, &result, &callError))
					ReportPhysicsCallbackError("OnTrigger", callError);
			}
			catch (const std::exception& error)
			{
				ReportPhysicsCallbackError("OnTrigger", error.what());
			}
			catch (...)
			{
				ReportPhysicsCallbackError("OnTrigger", "unknown exception");
			}
		}

		// 每帧一次(Late 阶段):按事件在队列里的顺序派发 —— 确定性来自"物理产出顺序"。
		void DispatchPhysicsEventsToLua(Scene& scene, PhysicsEventDispatchState& state)
		{
			if (!ScriptEngine::IsInitialized())
				return;
			const std::vector<Physics::ContactEvent>& contacts = scene.GetContactEvents();
			const std::vector<Physics::TriggerEvent>& triggers = scene.GetTriggerEvents();
			if (state.Subscriptions.empty() || (contacts.empty() && triggers.empty()))
				return;

			// 快照:回调里再 OnContact/Off 不会破坏本次遍历(与 Scene::NotifyComponentAdded 同口径)。
			struct Subscriber
			{
				ScriptFunctionRef Callback;
				bool Trigger = false;
			};
			std::vector<Subscriber> subscribers;
			subscribers.reserve(state.Subscriptions.size());
			for (const PhysicsEventSubscription& subscription : state.Subscriptions)
			{
				if (!subscription.Callback.IsValid())
					continue;
				subscribers.push_back({ subscription.Callback, subscription.Trigger });
			}

			for (const Physics::ContactEvent& event : contacts)
				for (const Subscriber& subscriber : subscribers)
					if (!subscriber.Trigger)
						DispatchContactToLua(subscriber.Callback, scene, event);

			for (const Physics::TriggerEvent& event : triggers)
				for (const Subscriber& subscriber : subscribers)
					if (subscriber.Trigger)
						DispatchTriggerToLua(subscriber.Callback, scene, event);
		}

		PhysicsEventDispatchState& EnsurePhysicsEventDispatch(Scene& scene)
		{
			std::vector<std::weak_ptr<PhysicsEventDispatchState>>& states = PhysicsEventStates();
			for (auto it = states.begin(); it != states.end();)
			{
				std::shared_ptr<PhysicsEventDispatchState> live = it->lock();
				if (!live)
				{
					it = states.erase(it);   // 场景已销毁:顺手清掉失效登记
					continue;
				}
				if (live->OwnerScene == &scene)
					return *live;
				++it;
			}

			std::shared_ptr<PhysicsEventDispatchState> state = std::make_shared<PhysicsEventDispatchState>();
			state->OwnerScene = &scene;
			if (!scene.HasFrameSystem(kPhysicsEventSystemName))
			{
				Scene* target = &scene;
				std::shared_ptr<PhysicsEventDispatchState> captured = state;
				scene.RegisterFrameSystem({
					kPhysicsEventSystemName,
					false,
					[target, captured](Timestep) { DispatchPhysicsEventsToLua(*target, *captured); },
					Gameplay::SystemPhase::Late,
					{}
				});
				scene.SetFrameSystemOwner(kPhysicsEventSystemName, "Lua:physics-events");
			}
			states.push_back(state);
			return *state;
		}

		bool RemovePhysicsEventSubscription(std::uint64_t handle)
		{
			if (handle < kPhysicsEventHandleBase)
				return false;
			for (const std::weak_ptr<PhysicsEventDispatchState>& weak : PhysicsEventStates())
			{
				std::shared_ptr<PhysicsEventDispatchState> state = weak.lock();
				if (!state)
					continue;
				const auto found = std::remove_if(state->Subscriptions.begin(), state->Subscriptions.end(),
					[handle](const PhysicsEventSubscription& subscription) { return subscription.Id == handle; });
				if (found == state->Subscriptions.end())
					continue;
				state->Subscriptions.erase(found, state->Subscriptions.end());
				return true;
			}
			return false;
		}

		ScriptValue SubscribePhysicsEventsImpl(const ScriptValue* args, std::size_t count, bool trigger, const char* api)
		{
			std::size_t startIndex = 0;
			if (count >= 1 && args[0].IsTable())
				startIndex = 1;   // 冒号调用:args[0] 是 ecs/world 表(self)

			if (count <= startIndex)
				throw std::logic_error(std::string(api) + " expects (fn)");

			ScriptFunctionRef fn;
			if (!args[startIndex].AsFunction(&fn) || !fn.IsValid())
				throw std::logic_error(std::string(api) + ": callback must be a valid function");

			Scene* activeScene = ScriptEngine::GetActiveScene();
			if (!activeScene)
				throw std::logic_error(std::string(api) + " requires an active scene");

			PhysicsEventDispatchState& state = EnsurePhysicsEventDispatch(*activeScene);
			const std::uint64_t handle = NextPhysicsEventHandle();
			state.Subscriptions.push_back({ handle, trigger, fn });
			return ScriptValue::Number(static_cast<double>(handle));
		}

		ScriptValue OnContactImpl(const ScriptValue* args, std::size_t count)
		{
			return SubscribePhysicsEventsImpl(args, count, false, "ecs:OnContact");
		}

		ScriptValue OnTriggerImpl(const ScriptValue* args, std::size_t count)
		{
			return SubscribePhysicsEventsImpl(args, count, true, "ecs:OnTrigger");
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

			// P5:物理事件订阅句柄走独立区段(见 kPhysicsEventHandleBase);先查它,
			// 命中就不再落回组件观察者。两个区段不相交,Off 不需要知道调用方来源。
			const uint64_t handleId = static_cast<uint64_t>(handleNum);
			if (RemovePhysicsEventSubscription(handleId))
				return ScriptValue::Boolean(true);

			activeScene->RemoveComponentObserver(handleId);
			return ScriptValue::Boolean(true);
		}

		// -------------------------------------------------------------------------
		// ecs:RequireLib(name) 实现 —— scripts/lib/ 受限加载通道的绑定入口。
		// 路径守卫 / 模块缓存 / 循环检测 / 同一沙箱执行都在 ScriptEngine::RequireScriptLib
		// (Scene/ScriptEngine.cpp);这里只做参数校验与"失败 ⇒ 可读 Lua error"的转换。
		// -------------------------------------------------------------------------
		ScriptValue RequireLibImpl(const ScriptValue* args, std::size_t count)
		{
			std::size_t startIndex = 0;
			if (count >= 1 && args[0].IsTable())
				startIndex = 1;   // 冒号调用:args[0] 是 ecs/world 表(self)

			if (count <= startIndex)
				throw std::logic_error("ecs:RequireLib expects (name)");

			std::string name;
			if (!args[startIndex].AsString(&name))
				throw std::logic_error("ecs:RequireLib expects a library name string");

			ScriptValue value;
			std::string error;
			if (!ScriptEngine::RequireScriptLib(name, value, &error))
				throw std::logic_error(error.empty() ? ("ecs:RequireLib('" + name + "') failed") : error);
			return value;
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
		static const ScriptServiceParam onChangeParams[] = {
			{ "component", "string", ScriptServiceArgType::String, true, "Registered component type name to observe." },
			{ "fn", "function", ScriptServiceArgType::None, true, "Called with (entity, component) whenever the component data changes." },
		};
		static const ScriptServiceParam addLifecycleSystemParams[] = {
			{ "name", "string", ScriptServiceArgType::String, true, "System name." },
			{ "fn", "function", ScriptServiceArgType::None, true, "Called on the corresponding lifecycle event." },
		};
		static const ScriptServiceParam offParams[] = {
			{ "handle", "number", ScriptServiceArgType::Number, true, "Handle returned by ecs:OnAdd / ecs:OnRemove / ecs:OnContact / ecs:OnTrigger." },
		};
		static const ScriptServiceParam onContactParams[] = {
			{ "fn", "function", ScriptServiceArgType::None, true, "Called once per contact event of the current frame with a table { a, b, phase, point, normal, depth }." },
		};
		static const ScriptServiceParam onTriggerParams[] = {
			{ "fn", "function", ScriptServiceArgType::None, true, "Called once per sensor/trigger event of the current frame with a table { sensor, other, phase }." },
		};
		static const ScriptServiceParam requireLibParams[] = {
			{ "name", "string", ScriptServiceArgType::String, true, "Library path relative to the content root's scripts/lib/, without extension and using '/' separators (e.g. \"util/math\"); .luau is preferred and .lua is the fallback." },
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
			{ "CreateRawEntity", &CreateRawEntityImpl, nullptr, 0, 0, "Entity",
				"Create a lightweight anonymous entity (no Tag, no UUID) in the active scene and return its handle." },
			{ "DestroyEntity", &DestroyEntityImpl, destroyEntityParams, 1, 1, "boolean",
				"Queue an entity for destruction at the next safe point." },
			{ "EntityCount", &EntityCountImpl, nullptr, 0, 0, "number",
				"Number of live entities in the active scene." },
			{ "OnAdd", &OnAddImpl, onAddParams, 2, 2, "number",
				"Observe component additions; returns a handle for ecs:Off." },
			{ "OnRemove", &OnRemoveImpl, onRemoveParams, 2, 2, "number",
				"Observe component removals; returns a handle for ecs:Off." },
			{ "OnChange", &OnChangeImpl, onChangeParams, 2, 2, "number",
				"Observe component data changes; returns a handle for ecs:Off." },
			{ "AddStartupSystem", &AddStartupSystemImpl, addLifecycleSystemParams, 2, 2, "boolean",
				"Register a startup system that runs once when the active scene starts running." },
			{ "AddTeardownSystem", &AddTeardownSystemImpl, addLifecycleSystemParams, 2, 2, "boolean",
				"Register a teardown system that runs once when the active scene stops running." },
			{ "OnContact", &OnContactImpl, onContactParams, 1, 1, "number",
				"Subscribe to this frame's contact events; called once per event with { a, b, phase, point, normal, depth }. Events are produced in the fixed step and read once per frame, so call it from a variable-phase system; returns a handle for ecs:Off." },
			{ "OnTrigger", &OnTriggerImpl, onTriggerParams, 1, 1, "number",
				"Subscribe to this frame's sensor/trigger events; called once per event with { sensor, other, phase }; returns a handle for ecs:Off." },
			{ "Off", &OffImpl, offParams, 1, 1, "boolean",
				"Cancel a handle from OnAdd / OnRemove / OnContact / OnTrigger; false when the handle is unknown." },
			{ "RequireLib", &RequireLibImpl, requireLibParams, 1, 1, "any",
				"Load a library module from <content root>/scripts/lib/ inside the same sandbox (no io/os/require/load); the module body runs once per path and the returned value is cached (content changes re-run it); rejects absolute paths, drive letters, '.'/'..' segments, backslashes and non-.luau/.lua names." },
		};
		static const ScriptServiceBinding tables[] = {
			{ "ecs", "Read-only pure-ECS table (also exposed as \"world\"): queries, systems, entity lifecycle, component observers and physics events.",
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

		// Comp & Components: 组件名常量表 (Comp.Transform -> "TransformComponent")
		ScriptTableRef compTable = vm->CreateTable();
		if (compTable.IsValid())
		{
			struct CompPair { const char* ShortName; const char* FullName; };
			static const CompPair kBuiltins[] = {
				{ "Transform", "TransformComponent" },
				{ "TransformComponent", "TransformComponent" },
				{ "Velocity", "VelocityComponent" },
				{ "VelocityComponent", "VelocityComponent" },
				{ "Camera", "CameraComponent" },
				{ "CameraComponent", "CameraComponent" },
				{ "MeshRenderer", "MeshRendererComponent" },
				{ "MeshRendererComponent", "MeshRendererComponent" },
				{ "SpriteRenderer", "SpriteRendererComponent" },
				{ "SpriteRendererComponent", "SpriteRendererComponent" },
				{ "Circle", "CircleComponent" },
				{ "CircleComponent", "CircleComponent" },
				{ "Hierarchy", "HierarchyComponent" },
				{ "HierarchyComponent", "HierarchyComponent" },
				{ "Tag", "TagComponent" },
				{ "TagComponent", "TagComponent" },
				{ "RigidBody2D", "RigidBody2DComponent" },
				{ "RigidBody2DComponent", "RigidBody2DComponent" },
				{ "BoxCollider2D", "BoxCollider2DComponent" },
				{ "BoxCollider2DComponent", "BoxCollider2DComponent" },
				{ "CircleCollider2D", "CircleCollider2DComponent" },
				{ "CircleCollider2DComponent", "CircleCollider2DComponent" },
			};
			for (const auto& pair : kBuiltins)
			{
				compTable.SetField(pair.ShortName, ScriptValue::String(pair.FullName));
			}

			compTable.SetMetaField("__index", bindings.CreateFunction("Comp.__index",
				[](const ScriptValue* metaArgs, std::size_t metaCount) -> ScriptValue
				{
					if (metaCount < 2 || !metaArgs[1].IsString())
						return ScriptValue::Nil();
					std::string key;
					metaArgs[1].AsString(&key);
					if (key.empty())
						return ScriptValue::Nil();

					Scene* scene = ScriptEngine::GetActiveScene();
					if (scene)
					{
						auto& schemas = scene->GetContext().Schemas();
						const Schema::TypeSchema* schema = schemas.Find(key);
						if (schema && schema->Category == Schema::TypeCategory::Component)
							return ScriptValue::String(schema->Id.Name);
						schema = schemas.Find(key + "Component");
						if (schema && schema->Category == Schema::TypeCategory::Component)
							return ScriptValue::String(schema->Id.Name);
					}
					if (key.size() > 9 && key.substr(key.size() - 9) == "Component")
						return ScriptValue::String(key);
					return ScriptValue::String(key + "Component");
				}));

			compTable.SetMetaField("__newindex", bindings.CreateFunction("Comp.__newindex",
				[](const ScriptValue*, std::size_t) -> ScriptValue
				{
					throw std::logic_error("Comp/Components table is read-only");
				}));

			if (!vm->SetGlobal("Comp", compTable.ToValue()) || !vm->SetGlobal("Components", compTable.ToValue()))
			{
				if (error) *error = "failed to set global 'Comp'";
				return false;
			}
		}

		// Phase: 阶段常量表 (Phase.Update -> "Update")
		ScriptTableRef phaseTable = vm->CreateTable();
		if (phaseTable.IsValid())
		{
			phaseTable.SetField("PreFixed", ScriptValue::String("PreFixed"));
			phaseTable.SetField("Fixed", ScriptValue::String("Fixed"));
			phaseTable.SetField("Update", ScriptValue::String("Update"));
			phaseTable.SetField("Late", ScriptValue::String("Late"));
			phaseTable.SetField("PreRender", ScriptValue::String("PreRender"));

			phaseTable.SetMetaField("__newindex", bindings.CreateFunction("Phase.__newindex",
				[](const ScriptValue*, std::size_t) -> ScriptValue
				{
					throw std::logic_error("Phase table is read-only");
				}));

			if (!vm->SetGlobal("Phase", phaseTable.ToValue()))
			{
				if (error) *error = "failed to set global 'Phase'";
				return false;
			}
		}

		if (error) error->clear();
		return true;
	}
}
