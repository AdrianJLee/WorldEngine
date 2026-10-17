#include "wldpch.h"
#include "World/Script/Bindings/BindParticle.h"

#include "World/Renderer/ParticleSystem.h"
#include "World/Scene/Components.h"
#include "World/Scene/Entity.h"
#include "World/Scene/Scene.h"
#include "World/Script/Bindings/BindServices.h"
#include "World/Script/Runtime/ScriptEngine.h"
#include "World/Script/Vm/LuauVm.h"
#include "World/Script/Vm/ScriptBindingContext.h"
#include "World/Script/Vm/ScriptRef.h"
#include "World/Script/Vm/ScriptValue.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>

namespace World
{
	namespace
	{
		static ParticleSystem s_ParticleSystem;

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

		bool RequireBool(const std::string& table, const ScriptServiceMethod& method,
		                 const ScriptValue* args, std::size_t count, std::size_t index)
		{
			if (index >= count || args[index].IsNil())
				throw std::logic_error(table + "." + method.Name + ": missing boolean argument " + std::to_string(index + 1));
			bool b = false;
			if (args[index].IsBoolean())
			{
				args[index].AsBool(&b);
				return b;
			}
			if (args[index].IsNumber())
			{
				double n = 0.0;
				args[index].AsNumber(&n);
				return n != 0.0;
			}
			throw std::logic_error(table + "." + method.Name + ": argument " + std::to_string(index + 1) + " must be boolean");
		}

		// ---- Particle Implementation Methods ----
		ScriptValue ParticleEmitImpl(const ScriptValue* args, std::size_t count)
		{
			NormalizeArgs(args, count);
			Scene* scene = ScriptEngine::GetActiveScene();
			if (!scene) throw std::logic_error("Particle.Emit requires an active scene");
			const auto& method = ParticleServiceBindings(nullptr)[0].Methods[0];
			entt::entity ent = RequireEntityId("Particle", method, args, count, 0);
			double emitCount = RequireNum("Particle", method, args, count, 1);

			auto* comp = const_cast<entt::registry&>(static_cast<const Scene*>(scene)->GetRegistry()).try_get<ParticleSystemComponent>(ent);
			if (!comp)
				return ScriptValue::Boolean(false);

			const auto* tc = static_cast<const Scene*>(scene)->GetRegistry().try_get<TransformComponent>(ent);
			glm::vec3 pos = tc ? tc->Location : glm::vec3(0.0f);
			glm::quat rot = tc ? tc->Rotation : glm::quat(1.0f, 0.0f, 0.0f, 0.0f);

			auto& pool = s_ParticleSystem.GetPoolForEntity(static_cast<uint32_t>(ent), comp->MaxParticles);
			pool.Emit(static_cast<std::size_t>(std::max(0.0, emitCount)), *comp, pos, rot);

			return ScriptValue::Boolean(true);
		}

		ScriptValue ParticlePlayImpl(const ScriptValue* args, std::size_t count)
		{
			NormalizeArgs(args, count);
			Scene* scene = ScriptEngine::GetActiveScene();
			if (!scene) throw std::logic_error("Particle.Play requires an active scene");
			const auto& method = ParticleServiceBindings(nullptr)[0].Methods[1];
			entt::entity ent = RequireEntityId("Particle", method, args, count, 0);

			auto* comp = const_cast<entt::registry&>(static_cast<const Scene*>(scene)->GetRegistry()).try_get<ParticleSystemComponent>(ent);
			if (!comp)
				return ScriptValue::Boolean(false);

			comp->IsPlaying = 1;
			return ScriptValue::Boolean(true);
		}

		ScriptValue ParticleStopImpl(const ScriptValue* args, std::size_t count)
		{
			NormalizeArgs(args, count);
			Scene* scene = ScriptEngine::GetActiveScene();
			if (!scene) throw std::logic_error("Particle.Stop requires an active scene");
			const auto& method = ParticleServiceBindings(nullptr)[0].Methods[2];
			entt::entity ent = RequireEntityId("Particle", method, args, count, 0);

			auto* comp = const_cast<entt::registry&>(static_cast<const Scene*>(scene)->GetRegistry()).try_get<ParticleSystemComponent>(ent);
			if (!comp)
				return ScriptValue::Boolean(false);

			comp->IsPlaying = 0;
			return ScriptValue::Boolean(true);
		}

		ScriptValue ParticleRestartImpl(const ScriptValue* args, std::size_t count)
		{
			NormalizeArgs(args, count);
			Scene* scene = ScriptEngine::GetActiveScene();
			if (!scene) throw std::logic_error("Particle.Restart requires an active scene");
			const auto& method = ParticleServiceBindings(nullptr)[0].Methods[3];
			entt::entity ent = RequireEntityId("Particle", method, args, count, 0);

			auto* comp = const_cast<entt::registry&>(static_cast<const Scene*>(scene)->GetRegistry()).try_get<ParticleSystemComponent>(ent);
			if (!comp)
				return ScriptValue::Boolean(false);

			auto& pool = s_ParticleSystem.GetPoolForEntity(static_cast<uint32_t>(ent), comp->MaxParticles);
			pool.Reset();
			comp->IsPlaying = 1;
			return ScriptValue::Boolean(true);
		}

		ScriptValue ParticleGetActiveCountImpl(const ScriptValue* args, std::size_t count)
		{
			NormalizeArgs(args, count);
			Scene* scene = ScriptEngine::GetActiveScene();
			if (!scene) throw std::logic_error("Particle.GetActiveCount requires an active scene");
			const auto& method = ParticleServiceBindings(nullptr)[0].Methods[4];
			entt::entity ent = RequireEntityId("Particle", method, args, count, 0);

			auto* comp = const_cast<entt::registry&>(static_cast<const Scene*>(scene)->GetRegistry()).try_get<ParticleSystemComponent>(ent);
			if (!comp)
				return ScriptValue::Number(0.0);

			auto& pool = s_ParticleSystem.GetPoolForEntity(static_cast<uint32_t>(ent), comp->MaxParticles);
			return ScriptValue::Number(static_cast<double>(pool.ActiveCount()));
		}

		ScriptValue ParticleSetLockToXYImpl(const ScriptValue* args, std::size_t count)
		{
			NormalizeArgs(args, count);
			Scene* scene = ScriptEngine::GetActiveScene();
			if (!scene) throw std::logic_error("Particle.SetLockToXY requires an active scene");
			const auto& method = ParticleServiceBindings(nullptr)[0].Methods[5];
			entt::entity ent = RequireEntityId("Particle", method, args, count, 0);
			bool locked = RequireBool("Particle", method, args, count, 1);

			auto* comp = const_cast<entt::registry&>(static_cast<const Scene*>(scene)->GetRegistry()).try_get<ParticleSystemComponent>(ent);
			if (!comp)
				return ScriptValue::Boolean(false);

			comp->LockToXYPlane = locked ? 1 : 0;
			return ScriptValue::Boolean(true);
		}

		ScriptValue ParticleSetEmissionRateImpl(const ScriptValue* args, std::size_t count)
		{
			NormalizeArgs(args, count);
			Scene* scene = ScriptEngine::GetActiveScene();
			if (!scene) throw std::logic_error("Particle.SetEmissionRate requires an active scene");
			const auto& method = ParticleServiceBindings(nullptr)[0].Methods[6];
			entt::entity ent = RequireEntityId("Particle", method, args, count, 0);
			double rate = RequireNum("Particle", method, args, count, 1);

			auto* comp = const_cast<entt::registry&>(static_cast<const Scene*>(scene)->GetRegistry()).try_get<ParticleSystemComponent>(ent);
			if (!comp)
				return ScriptValue::Boolean(false);

			comp->EmissionRate = static_cast<float>(rate);
			return ScriptValue::Boolean(true);
		}

		ScriptValue ParticleSetSpeedImpl(const ScriptValue* args, std::size_t count)
		{
			NormalizeArgs(args, count);
			Scene* scene = ScriptEngine::GetActiveScene();
			if (!scene) throw std::logic_error("Particle.SetSpeed requires an active scene");
			const auto& method = ParticleServiceBindings(nullptr)[0].Methods[7];
			entt::entity ent = RequireEntityId("Particle", method, args, count, 0);
			double speed = RequireNum("Particle", method, args, count, 1);

			auto* comp = const_cast<entt::registry&>(static_cast<const Scene*>(scene)->GetRegistry()).try_get<ParticleSystemComponent>(ent);
			if (!comp)
				return ScriptValue::Boolean(false);

			comp->Speed = static_cast<float>(speed);
			return ScriptValue::Boolean(true);
		}

		ScriptValue ParticleSetLifetimeImpl(const ScriptValue* args, std::size_t count)
		{
			NormalizeArgs(args, count);
			Scene* scene = ScriptEngine::GetActiveScene();
			if (!scene) throw std::logic_error("Particle.SetLifetime requires an active scene");
			const auto& method = ParticleServiceBindings(nullptr)[0].Methods[8];
			entt::entity ent = RequireEntityId("Particle", method, args, count, 0);
			double lifetime = RequireNum("Particle", method, args, count, 1);

			auto* comp = const_cast<entt::registry&>(static_cast<const Scene*>(scene)->GetRegistry()).try_get<ParticleSystemComponent>(ent);
			if (!comp)
				return ScriptValue::Boolean(false);

			comp->Lifetime = static_cast<float>(lifetime);
			return ScriptValue::Boolean(true);
		}

		static const ScriptServiceParam emitParams[] = {
			{ "entityId", "number", ScriptServiceArgType::Number, true, "Target entity ID or Entity object." },
			{ "count", "number", ScriptServiceArgType::Number, true, "Number of particles to emit." }
		};
		static const ScriptServiceParam singleEntityParams[] = {
			{ "entityId", "number", ScriptServiceArgType::Number, true, "Target entity ID or Entity object." }
		};
		static const ScriptServiceParam lockParams[] = {
			{ "entityId", "number", ScriptServiceArgType::Number, true, "Target entity ID or Entity object." },
			{ "locked", "boolean", ScriptServiceArgType::Boolean, true, "Lock particle motion to XY plane." }
		};
		static const ScriptServiceParam rateParams[] = {
			{ "entityId", "number", ScriptServiceArgType::Number, true, "Target entity ID or Entity object." },
			{ "rate", "number", ScriptServiceArgType::Number, true, "Emission rate in particles per second." }
		};
		static const ScriptServiceParam speedParams[] = {
			{ "entityId", "number", ScriptServiceArgType::Number, true, "Target entity ID or Entity object." },
			{ "speed", "number", ScriptServiceArgType::Number, true, "Initial particle speed." }
		};
		static const ScriptServiceParam lifetimeParams[] = {
			{ "entityId", "number", ScriptServiceArgType::Number, true, "Target entity ID or Entity object." },
			{ "lifetime", "number", ScriptServiceArgType::Number, true, "Particle base lifetime in seconds." }
		};

		static const ScriptServiceMethod particleMethods[] = {
			{ "Emit", [](const ScriptValue* args, std::size_t count) { return ParticleEmitImpl(args, count); }, emitParams, 2, 2, "boolean", "Emit particles from entity's ParticleSystemComponent." },
			{ "Play", [](const ScriptValue* args, std::size_t count) { return ParticlePlayImpl(args, count); }, singleEntityParams, 1, 1, "boolean", "Start or resume particle emission." },
			{ "Stop", [](const ScriptValue* args, std::size_t count) { return ParticleStopImpl(args, count); }, singleEntityParams, 1, 1, "boolean", "Stop particle emission." },
			{ "Restart", [](const ScriptValue* args, std::size_t count) { return ParticleRestartImpl(args, count); }, singleEntityParams, 1, 1, "boolean", "Reset particle pool and restart emission." },
			{ "GetActiveCount", [](const ScriptValue* args, std::size_t count) { return ParticleGetActiveCountImpl(args, count); }, singleEntityParams, 1, 1, "number", "Get active simulated particle count." },
			{ "SetLockToXY", [](const ScriptValue* args, std::size_t count) { return ParticleSetLockToXYImpl(args, count); }, lockParams, 2, 2, "boolean", "Lock or unlock particle motion to 2D XY plane." },
			{ "SetEmissionRate", [](const ScriptValue* args, std::size_t count) { return ParticleSetEmissionRateImpl(args, count); }, rateParams, 2, 2, "boolean", "Set particle emission rate." },
			{ "SetSpeed", [](const ScriptValue* args, std::size_t count) { return ParticleSetSpeedImpl(args, count); }, speedParams, 2, 2, "boolean", "Set particle emission speed." },
			{ "SetLifetime", [](const ScriptValue* args, std::size_t count) { return ParticleSetLifetimeImpl(args, count); }, lifetimeParams, 2, 2, "boolean", "Set particle lifetime in seconds." }
		};

		static const ScriptServiceBinding particleBindingsTable[] = {
			{ "Particle", "Unified 2D and 3D particle simulation and playback service.", particleMethods, sizeof(particleMethods)/sizeof(particleMethods[0]) }
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

	ParticleSystem& GetGlobalParticleSystem()
	{
		return s_ParticleSystem;
	}

	const ScriptServiceBinding* ParticleServiceBindings(std::size_t* count)
	{
		if (count) *count = sizeof(particleBindingsTable) / sizeof(particleBindingsTable[0]);
		return particleBindingsTable;
	}

	bool RegisterParticleBindings(ScriptBindingContext& bindings, std::string* error)
	{
		std::size_t count = 0;
		const ScriptServiceBinding* services = ParticleServiceBindings(&count);
		for (std::size_t index = 0; index < count; ++index)
		{
			if (!RegisterServiceTable(bindings, services[index], error))
				return false;
		}
		if (error) error->clear();
		return true;
	}

	bool BindParticle(lua_State* L, Scene* scene)
	{
		(void)L;
		if (scene)
			ScriptEngine::SetActiveScene(scene);
		if (ScriptEngine::IsInitialized())
			return RegisterParticleBindings(ScriptEngine::GetBindingContext());
		return true;
	}
}
