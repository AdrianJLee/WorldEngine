#include "wldpch.h"
#include "World/Script/Bindings/BindGameplay.h"

#include "World/Gameplay/Framework/DamageSystem.h"
#include "World/Gameplay/Framework/SmartObjectSystem.h"
#include "World/Script/Bindings/BindServices.h"
#include "World/Script/Runtime/ScriptEngine.h"
#include "World/Script/Vm/LuauVm.h"
#include "World/Script/Vm/ScriptBindingContext.h"
#include "World/Script/Vm/ScriptRef.h"
#include "World/Script/Vm/ScriptValue.h"
#include "World/Scene/Components.h"

#include <stdexcept>
#include <string>

namespace World
{
	namespace
	{
		void NormalizeArgs(const ScriptValue*& args, std::size_t& count)
		{
			if (count > 0 && args[0].IsTable())
			{
				++args;
				--count;
			}
		}

		const char* TypeNameOf(const ScriptValue& value)
		{
			return value.TypeName();
		}

		entt::entity RequireEntityId(const std::string& table, const ScriptServiceMethod& method,
		                             const ScriptValue* args, std::size_t count, std::size_t index)
		{
			if (index >= count || args[index].IsNil())
				throw std::logic_error(table + "." + method.Name + ": missing entity argument " + std::to_string(index + 1));

			const ScriptValue& val = args[index];
			if (val.IsNumber())
			{
				double num = 0.0;
				val.AsNumber(&num);
				return static_cast<entt::entity>(static_cast<uint32_t>(num));
			}

			ScriptBindingContext& context = ScriptEngine::GetBindingContext();
			Entity* entityPtr = nullptr;
			if (context.Unwrap<Entity>("Entity", val, &entityPtr) && entityPtr && entityPtr->IsValid())
			{
				return static_cast<entt::entity>(*entityPtr);
			}

			throw std::logic_error(table + "." + method.Name + ": argument " + std::to_string(index + 1) +
				" ('" + method.Params[index].Name + "') must be number or Entity, got " + TypeNameOf(val));
		}

		double RequireNum(const std::string& table, const ScriptServiceMethod& method,
		                  const ScriptValue* args, std::size_t count, std::size_t index)
		{
			if (index >= count || args[index].IsNil())
				throw std::logic_error(table + "." + method.Name + ": missing number argument " + std::to_string(index + 1));
			double num = 0.0;
			if (!args[index].AsNumber(&num))
				throw std::logic_error(table + "." + method.Name + ": argument " + std::to_string(index + 1) + " must be number");
			return num;
		}

		// ---- Health Impls ----
		ScriptValue HealthGetImpl(const ScriptValue* args, std::size_t count)
		{
			NormalizeArgs(args, count);
			Scene* scene = ScriptEngine::GetActiveScene();
			if (!scene) throw std::logic_error("Health.Get requires an active scene");
			const auto& method = GameplayCoreServiceBindings(nullptr)[0].Methods[0];
			entt::entity ent = RequireEntityId("Health", method, args, count, 0);

			const auto* health = scene->GetRegistry().try_get<HealthComponent>(ent);
			if (!health) return ScriptValue::Nil();

			ScriptTableRef res = ScriptEngine::GetState().CreateTable();
			res.SetField("CurrentHealth", ScriptValue::Number(health->CurrentHealth));
			res.SetField("MaxHealth", ScriptValue::Number(health->MaxHealth));
			res.SetField("Shield", ScriptValue::Number(health->Shield));
			res.SetField("MaxShield", ScriptValue::Number(health->MaxShield));
			res.SetField("Armor", ScriptValue::Number(health->Armor));
			res.SetField("MitigationMode", ScriptValue::Number(static_cast<double>(health->MitigationMode)));
			res.SetField("IsDead", ScriptValue::Boolean(health->IsDead != 0));
			res.SetField("Invulnerable", ScriptValue::Boolean(health->InvulnerableTimer > 0.0f));
			return res.ToValue();
		}

		ScriptValue HealthDamageImpl(const ScriptValue* args, std::size_t count)
		{
			NormalizeArgs(args, count);
			Scene* scene = ScriptEngine::GetActiveScene();
			if (!scene) throw std::logic_error("Health.Damage requires an active scene");
			const auto& method = GameplayCoreServiceBindings(nullptr)[0].Methods[1];
			entt::entity ent = RequireEntityId("Health", method, args, count, 0);
			float amount = static_cast<float>(RequireNum("Health", method, args, count, 1));
			uint8_t dmgType = (count >= 3 && args[2].IsNumber()) ? static_cast<uint8_t>(RequireNum("Health", method, args, count, 2)) : 0;
			uint32_t flags = (count >= 4 && args[3].IsNumber()) ? static_cast<uint32_t>(RequireNum("Health", method, args, count, 3)) : 0;

			Gameplay::DamageEvent event;
			event.Target = ent;
			event.Amount = amount;
			event.Type = static_cast<Gameplay::DamageType>(dmgType);
			event.Flags = flags;
			if (count >= 5 && args[4].IsString())
			{
				std::string tagStr;
				args[4].AsString(&tagStr);
				event.Tag = StringPool::Get().InternName(tagStr);
			}

			float dealt = Gameplay::DamageSystem::ApplyDamage(*scene, event);
			return ScriptValue::Number(dealt);
		}

		ScriptValue HealthSetMitigationModeImpl(const ScriptValue* args, std::size_t count)
		{
			NormalizeArgs(args, count);
			Scene* scene = ScriptEngine::GetActiveScene();
			if (!scene) throw std::logic_error("Health.SetMitigationMode requires an active scene");
			const auto& method = GameplayCoreServiceBindings(nullptr)[0].Methods[3];
			entt::entity ent = RequireEntityId("Health", method, args, count, 0);
			uint8_t mode = static_cast<uint8_t>(RequireNum("Health", method, args, count, 1));

			auto* health = scene->GetRegistry().try_get<HealthComponent>(ent);
			if (!health) return ScriptValue::Boolean(false);

			health->MitigationMode = mode;
			return ScriptValue::Boolean(true);
		}

		ScriptValue HealthHealImpl(const ScriptValue* args, std::size_t count)
		{
			NormalizeArgs(args, count);
			Scene* scene = ScriptEngine::GetActiveScene();
			if (!scene) throw std::logic_error("Health.Heal requires an active scene");
			const auto& method = GameplayCoreServiceBindings(nullptr)[0].Methods[2];
			entt::entity ent = RequireEntityId("Health", method, args, count, 0);
			float amount = static_cast<float>(RequireNum("Health", method, args, count, 1));

			auto* health = scene->GetRegistry().try_get<HealthComponent>(ent);
			if (!health) return ScriptValue::Number(0.0);

			float healed = Gameplay::DamageSystem::Heal(*health, amount);
			return ScriptValue::Number(healed);
		}

		// ---- SmartObject Impls ----
		ScriptValue SmartObjectClaimImpl(const ScriptValue* args, std::size_t count)
		{
			NormalizeArgs(args, count);
			Scene* scene = ScriptEngine::GetActiveScene();
			if (!scene) throw std::logic_error("SmartObject.Claim requires an active scene");
			const auto& method = GameplayCoreServiceBindings(nullptr)[1].Methods[0];
			entt::entity so = RequireEntityId("SmartObject", method, args, count, 0);
			entt::entity claimer = RequireEntityId("SmartObject", method, args, count, 1);
			return ScriptValue::Boolean(Gameplay::SmartObjectSystem::ClaimSlot(*scene, so, claimer));
		}

		ScriptValue SmartObjectUseImpl(const ScriptValue* args, std::size_t count)
		{
			NormalizeArgs(args, count);
			Scene* scene = ScriptEngine::GetActiveScene();
			if (!scene) throw std::logic_error("SmartObject.Use requires an active scene");
			const auto& method = GameplayCoreServiceBindings(nullptr)[1].Methods[1];
			entt::entity so = RequireEntityId("SmartObject", method, args, count, 0);
			entt::entity user = RequireEntityId("SmartObject", method, args, count, 1);
			return ScriptValue::Boolean(Gameplay::SmartObjectSystem::UseSlot(*scene, so, user));
		}

		ScriptValue SmartObjectReleaseImpl(const ScriptValue* args, std::size_t count)
		{
			NormalizeArgs(args, count);
			Scene* scene = ScriptEngine::GetActiveScene();
			if (!scene) throw std::logic_error("SmartObject.Release requires an active scene");
			const auto& method = GameplayCoreServiceBindings(nullptr)[1].Methods[2];
			entt::entity so = RequireEntityId("SmartObject", method, args, count, 0);
			return ScriptValue::Boolean(Gameplay::SmartObjectSystem::ReleaseSlot(*scene, so));
		}

		ScriptValue SmartObjectGetStateImpl(const ScriptValue* args, std::size_t count)
		{
			NormalizeArgs(args, count);
			Scene* scene = ScriptEngine::GetActiveScene();
			if (!scene) throw std::logic_error("SmartObject.GetState requires an active scene");
			const auto& method = GameplayCoreServiceBindings(nullptr)[1].Methods[3];
			entt::entity so = RequireEntityId("SmartObject", method, args, count, 0);
			const auto* comp = scene->GetRegistry().try_get<SmartObjectComponent>(so);
			if (!comp) return ScriptValue::Nil();
			return ScriptValue::Number(static_cast<double>(static_cast<uint8_t>(comp->SlotState)));
		}

		ScriptValue SmartObjectGetPositionImpl(const ScriptValue* args, std::size_t count)
		{
			NormalizeArgs(args, count);
			Scene* scene = ScriptEngine::GetActiveScene();
			if (!scene) throw std::logic_error("SmartObject.GetPosition requires an active scene");
			const auto& method = GameplayCoreServiceBindings(nullptr)[1].Methods[4];
			entt::entity so = RequireEntityId("SmartObject", method, args, count, 0);
			glm::vec3 pos = Gameplay::SmartObjectSystem::GetSlotWorldPosition(*scene, so);

			ScriptTableRef res = ScriptEngine::GetState().CreateTable();
			res.SetField("x", ScriptValue::Number(pos.x));
			res.SetField("y", ScriptValue::Number(pos.y));
			res.SetField("z", ScriptValue::Number(pos.z));
			return res.ToValue();
		}

		// Service descriptor definitions
		static const ScriptServiceParam healthGetParams[] = {
			{ "entityId", "number", ScriptServiceArgType::Number, true, "Target entity ID or Entity object." }
		};
		static const ScriptServiceParam healthDamageParams[] = {
			{ "targetId", "number", ScriptServiceArgType::Number, true, "Target entity ID." },
			{ "amount", "number", ScriptServiceArgType::Number, true, "Raw damage amount." },
			{ "damageType", "number", ScriptServiceArgType::Number, false, "Damage type (0=Physical, 1=Fire, 2=TrueDamage, 3=Custom)." },
			{ "flags", "number", ScriptServiceArgType::Number, false, "Damage flags (1=IgnoreArmor, 2=IgnoreShield, 4=IgnoreInvulnerability)." },
			{ "tag", "string", ScriptServiceArgType::String, false, "Optional damage tag name (e.g. Damage.Fire)." }
		};
		static const ScriptServiceParam healthHealParams[] = {
			{ "targetId", "number", ScriptServiceArgType::Number, true, "Target entity ID." },
			{ "amount", "number", ScriptServiceArgType::Number, true, "Healing amount." }
		};
		static const ScriptServiceParam healthModeParams[] = {
			{ "targetId", "number", ScriptServiceArgType::Number, true, "Target entity ID." },
			{ "mode", "number", ScriptServiceArgType::Number, true, "Mitigation mode (0=Hyperbolic, 1=Flat, 2=Percentage)." }
		};

		static const ScriptServiceMethod healthMethods[] = {
			{ "Get", [](const ScriptValue* args, std::size_t count) { return HealthGetImpl(args, count); }, healthGetParams, 1, 1, "table", "Query entity health status." },
			{ "Damage", [](const ScriptValue* args, std::size_t count) { return HealthDamageImpl(args, count); }, healthDamageParams, 5, 2, "number", "Apply mitigated damage to target entity." },
			{ "Heal", [](const ScriptValue* args, std::size_t count) { return HealthHealImpl(args, count); }, healthHealParams, 2, 2, "number", "Heal target entity up to MaxHealth." },
			{ "SetMitigationMode", [](const ScriptValue* args, std::size_t count) { return HealthSetMitigationModeImpl(args, count); }, healthModeParams, 2, 2, "boolean", "Set damage mitigation formula mode (0=Hyperbolic, 1=Flat, 2=Percentage)." }
		};

		static const ScriptServiceParam soSlotParams[] = {
			{ "smartObjectId", "number", ScriptServiceArgType::Number, true, "Smart object entity ID." },
			{ "userId", "number", ScriptServiceArgType::Number, true, "User entity ID." }
		};
		static const ScriptServiceParam soSingleParams[] = {
			{ "smartObjectId", "number", ScriptServiceArgType::Number, true, "Smart object entity ID." }
		};

		static const ScriptServiceMethod soMethods[] = {
			{ "Claim", [](const ScriptValue* args, std::size_t count) { return SmartObjectClaimImpl(args, count); }, soSlotParams, 2, 2, "boolean", "Claim an available interaction slot." },
			{ "Use", [](const ScriptValue* args, std::size_t count) { return SmartObjectUseImpl(args, count); }, soSlotParams, 2, 2, "boolean", "Begin using an interaction slot." },
			{ "Release", [](const ScriptValue* args, std::size_t count) { return SmartObjectReleaseImpl(args, count); }, soSingleParams, 1, 1, "boolean", "Release an interaction slot." },
			{ "GetState", [](const ScriptValue* args, std::size_t count) { return SmartObjectGetStateImpl(args, count); }, soSingleParams, 1, 1, "number", "Query interaction slot state (0=Available, 1=Claimed, 2=InUse, 3=Cooldown)." },
			{ "GetPosition", [](const ScriptValue* args, std::size_t count) { return SmartObjectGetPositionImpl(args, count); }, soSingleParams, 1, 1, "table", "Get interaction slot world position." }
		};

		static const ScriptServiceBinding gameplayBindingsTable[] = {
			{ "Health", "Generic gameplay health, damage mitigation, and healing service.", healthMethods, sizeof(healthMethods)/sizeof(healthMethods[0]) },
			{ "SmartObject", "Smart Object interaction slot reservation and querying service.", soMethods, sizeof(soMethods)/sizeof(soMethods[0]) }
		};

		bool RegisterServiceTable(ScriptBindingContext& bindings, const ScriptServiceBinding& service, std::string* error)
		{
			LuauVm* vm = bindings.Vm();
			if (!vm)
			{
				if (error) *error = std::string("cannot bind ") + service.Name + ": Luau VM is not available";
				return false;
			}
			ScriptTableRef table = vm->CreateTable();
			for (std::size_t index = 0; index < service.MethodCount; ++index)
			{
				const ScriptServiceMethod& method = service.Methods[index];
				if (!table.SetField(method.Name, bindings.CreateFunction(method.Name, method.Function)))
				{
					if (error) *error = std::string("failed to bind ") + std::string(service.Name) + "." + std::string(method.Name);
					return false;
				}
			}
			if (!table.SetMetaField("__newindex", bindings.CreateFunction(service.Name,
				[](const ScriptValue*, std::size_t) -> ScriptValue
				{
					throw std::logic_error("service tables are read-only; use the documented methods");
				})))
			{
				if (error) *error = std::string("failed to lock the service table: ") + service.Name;
				return false;
			}
			if (!vm->SetGlobal(service.Name, table.ToValue()))
			{
				if (error) *error = std::string("failed to publish the service table: ") + service.Name;
				return false;
			}
			return true;
		}
	}

	const ScriptServiceBinding* GameplayCoreServiceBindings(std::size_t* count)
	{
		if (count) *count = sizeof(gameplayBindingsTable) / sizeof(gameplayBindingsTable[0]);
		return gameplayBindingsTable;
	}

	bool RegisterGameplayBindings(ScriptBindingContext& bindings, std::string* error)
	{
		std::size_t count = 0;
		const ScriptServiceBinding* services = GameplayCoreServiceBindings(&count);
		for (std::size_t index = 0; index < count; ++index)
		{
			if (!RegisterServiceTable(bindings, services[index], error))
				return false;
		}
		if (error) error->clear();
		return true;
	}
}
