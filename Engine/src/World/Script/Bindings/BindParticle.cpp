#include "wldpch.h"
#include "World/Script/Bindings/BindParticle.h"

#include "World/Renderer/ParticleSystem.h"
#include "World/Renderer/ParticleAsset.h"
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

		std::string RequireString(const std::string& table, const ScriptServiceMethod& method,
		                          const ScriptValue* args, std::size_t count, std::size_t index)
		{
			if (index >= count || args[index].IsNil())
				throw std::logic_error(table + "." + method.Name + ": missing string argument " + std::to_string(index + 1));
			std::string str;
			if (!args[index].AsString(&str))
				throw std::logic_error(table + "." + method.Name + ": argument " + std::to_string(index + 1) + " must be string");
			return str;
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

			s_ParticleSystem.GetPoolForEntity(static_cast<uint32_t>(ent), comp->MaxParticles).Emit(static_cast<std::size_t>(std::max(0.0, emitCount)), *comp, pos, rot);
			GetGlobalParticleSystem().GetPoolForEntity(static_cast<uint32_t>(ent), comp->MaxParticles).Emit(static_cast<std::size_t>(std::max(0.0, emitCount)), *comp, pos, rot);

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

			s_ParticleSystem.GetPoolForEntity(static_cast<uint32_t>(ent), comp->MaxParticles).Reset();
			GetGlobalParticleSystem().GetPoolForEntity(static_cast<uint32_t>(ent), comp->MaxParticles).Reset();
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

		ScriptValue ParticleSetSeedImpl(const ScriptValue* args, std::size_t count)
		{
			NormalizeArgs(args, count);
			Scene* scene = ScriptEngine::GetActiveScene();
			if (!scene) throw std::logic_error("Particle.SetSeed requires an active scene");
			const auto& method = ParticleServiceBindings(nullptr)[0].Methods[9];
			entt::entity ent = RequireEntityId("Particle", method, args, count, 0);
			double seedVal = RequireNum("Particle", method, args, count, 1);

			auto* comp = const_cast<entt::registry&>(static_cast<const Scene*>(scene)->GetRegistry()).try_get<ParticleSystemComponent>(ent);
			if (!comp)
				return ScriptValue::Boolean(false);

			auto& pool = GetGlobalParticleSystem().GetPoolForEntity(static_cast<uint32_t>(ent), comp->MaxParticles);
			pool.SetSeed(static_cast<uint32_t>(seedVal));
			s_ParticleSystem.GetPoolForEntity(static_cast<uint32_t>(ent), comp->MaxParticles).SetSeed(static_cast<uint32_t>(seedVal));
			return ScriptValue::Boolean(true);
		}

		ScriptValue ParticleSetSubUVImpl(const ScriptValue* args, std::size_t count)
		{
			NormalizeArgs(args, count);
			Scene* scene = ScriptEngine::GetActiveScene();
			if (!scene) throw std::logic_error("Particle.SetSubUV requires an active scene");
			const auto& method = ParticleServiceBindings(nullptr)[0].Methods[10];
			entt::entity ent = RequireEntityId("Particle", method, args, count, 0);
			double tilesX = RequireNum("Particle", method, args, count, 1);
			double tilesY = RequireNum("Particle", method, args, count, 2);
			double frameRate = (count > 3 && !args[3].IsNil()) ? RequireNum("Particle", method, args, count, 3) : 0.0;
			bool randomStart = (count > 4 && !args[4].IsNil()) ? RequireBool("Particle", method, args, count, 4) : false;

			auto* comp = const_cast<entt::registry&>(static_cast<const Scene*>(scene)->GetRegistry()).try_get<ParticleSystemComponent>(ent);
			if (!comp)
				return ScriptValue::Boolean(false);

			const uint32_t tx = std::max(1u, static_cast<uint32_t>(tilesX));
			const uint32_t ty = std::max(1u, static_cast<uint32_t>(tilesY));
			if (tx * ty > 1u)
				comp->VfxFlags |= ParticleVfxFlag_SubUVEnabled;
			else
				comp->VfxFlags &= static_cast<uint8_t>(~ParticleVfxFlag_SubUVEnabled);

			if (randomStart)
				comp->VfxFlags |= ParticleVfxFlag_SubUVRandomStart;
			else
				comp->VfxFlags &= static_cast<uint8_t>(~ParticleVfxFlag_SubUVRandomStart);

			auto applyPool = [&](ParticlePool& pool) {
				auto& mat = pool.GetMaterialSettingsMut();
				mat.SubUVTilesX = tx;
				mat.SubUVTilesY = ty;
				mat.SubUVFrameRate = static_cast<float>(frameRate);
				mat.SubUVRandomStartFrame = randomStart;
			};
			applyPool(GetGlobalParticleSystem().GetPoolForEntity(static_cast<uint32_t>(ent), comp->MaxParticles));
			applyPool(s_ParticleSystem.GetPoolForEntity(static_cast<uint32_t>(ent), comp->MaxParticles));
			return ScriptValue::Boolean(true);
		}

		ScriptValue ParticleSetSoftParticlesImpl(const ScriptValue* args, std::size_t count)
		{
			NormalizeArgs(args, count);
			Scene* scene = ScriptEngine::GetActiveScene();
			if (!scene) throw std::logic_error("Particle.SetSoftParticles requires an active scene");
			const auto& method = ParticleServiceBindings(nullptr)[0].Methods[11];
			entt::entity ent = RequireEntityId("Particle", method, args, count, 0);
			bool enabled = RequireBool("Particle", method, args, count, 1);
			double fadeDist = (count > 2 && !args[2].IsNil()) ? RequireNum("Particle", method, args, count, 2) : 0.5;

			auto* comp = const_cast<entt::registry&>(static_cast<const Scene*>(scene)->GetRegistry()).try_get<ParticleSystemComponent>(ent);
			if (!comp)
				return ScriptValue::Boolean(false);

			if (enabled)
				comp->VfxFlags |= ParticleVfxFlag_SoftParticle;
			else
				comp->VfxFlags &= static_cast<uint8_t>(~ParticleVfxFlag_SoftParticle);

			auto applyPool = [&](ParticlePool& pool) {
				auto& mat = pool.GetMaterialSettingsMut();
				mat.SoftParticlesEnabled = enabled;
				mat.SoftParticleFadeDistance = std::max(0.001f, static_cast<float>(fadeDist));
			};
			applyPool(GetGlobalParticleSystem().GetPoolForEntity(static_cast<uint32_t>(ent), comp->MaxParticles));
			applyPool(s_ParticleSystem.GetPoolForEntity(static_cast<uint32_t>(ent), comp->MaxParticles));
			return ScriptValue::Boolean(true);
		}

		ScriptValue ParticleSetEmissiveBoostImpl(const ScriptValue* args, std::size_t count)
		{
			NormalizeArgs(args, count);
			Scene* scene = ScriptEngine::GetActiveScene();
			if (!scene) throw std::logic_error("Particle.SetEmissiveBoost requires an active scene");
			const auto& method = ParticleServiceBindings(nullptr)[0].Methods[12];
			entt::entity ent = RequireEntityId("Particle", method, args, count, 0);
			double boost = RequireNum("Particle", method, args, count, 1);

			auto* comp = const_cast<entt::registry&>(static_cast<const Scene*>(scene)->GetRegistry()).try_get<ParticleSystemComponent>(ent);
			if (!comp)
				return ScriptValue::Boolean(false);

			const float clampedBoost = std::max(0.0f, static_cast<float>(boost));
			GetGlobalParticleSystem().GetPoolForEntity(static_cast<uint32_t>(ent), comp->MaxParticles).GetMaterialSettingsMut().EmissiveBoost = clampedBoost;
			s_ParticleSystem.GetPoolForEntity(static_cast<uint32_t>(ent), comp->MaxParticles).GetMaterialSettingsMut().EmissiveBoost = clampedBoost;
			return ScriptValue::Boolean(true);
		}

		ScriptValue ParticleSetCollisionImpl(const ScriptValue* args, std::size_t count)
		{
			NormalizeArgs(args, count);
			Scene* scene = ScriptEngine::GetActiveScene();
			if (!scene) throw std::logic_error("Particle.SetCollision requires an active scene");
			const auto& method = ParticleServiceBindings(nullptr)[0].Methods[13];
			entt::entity ent = RequireEntityId("Particle", method, args, count, 0);
			double modeVal = (count > 1 && !args[1].IsNil()) ? RequireNum("Particle", method, args, count, 1) : 1.0;
			double restitution = (count > 2 && !args[2].IsNil()) ? RequireNum("Particle", method, args, count, 2) : 0.5;
			double friction = (count > 3 && !args[3].IsNil()) ? RequireNum("Particle", method, args, count, 3) : 0.1;
			bool killOnHit = (count > 4 && !args[4].IsNil()) ? RequireBool("Particle", method, args, count, 4) : false;
			bool enableEvents = (count > 5 && !args[5].IsNil()) ? RequireBool("Particle", method, args, count, 5) : false;

			auto* comp = const_cast<entt::registry&>(static_cast<const Scene*>(scene)->GetRegistry()).try_get<ParticleSystemComponent>(ent);
			if (!comp)
				return ScriptValue::Boolean(false);

			if (enableEvents)
				comp->VfxFlags |= ParticleVfxFlag_CollisionEvents;
			else
				comp->VfxFlags &= static_cast<uint8_t>(~ParticleVfxFlag_CollisionEvents);

			ParticleCollisionSettings col;
			col.Mode = (modeVal >= 1.0) ? ParticleCollisionMode::Plane : ParticleCollisionMode::None;
			col.Restitution = static_cast<float>(restitution);
			col.Friction = static_cast<float>(friction);
			col.KillOnHit = killOnHit;
			col.EnableEvents = enableEvents;
			col.PlaneOrigin = glm::vec3(0.0f);
			col.PlaneNormal = glm::vec3(0.0f, 1.0f, 0.0f);

			GetGlobalParticleSystem().GetPoolForEntity(static_cast<uint32_t>(ent), comp->MaxParticles).SetCollisionSettings(col);
			s_ParticleSystem.GetPoolForEntity(static_cast<uint32_t>(ent), comp->MaxParticles).SetCollisionSettings(col);
			return ScriptValue::Boolean(true);
		}

		ScriptValue ParticleGetCollisionHitCountImpl(const ScriptValue* args, std::size_t count)
		{
			NormalizeArgs(args, count);
			Scene* scene = ScriptEngine::GetActiveScene();
			if (!scene) throw std::logic_error("Particle.GetCollisionHitCount requires an active scene");
			const auto& method = ParticleServiceBindings(nullptr)[0].Methods[14];
			entt::entity ent = RequireEntityId("Particle", method, args, count, 0);

			auto* comp = const_cast<entt::registry&>(static_cast<const Scene*>(scene)->GetRegistry()).try_get<ParticleSystemComponent>(ent);
			if (!comp)
				return ScriptValue::Number(0.0);

			auto* pool = GetGlobalParticleSystem().GetPool(static_cast<uint32_t>(ent));
			if (!pool) pool = s_ParticleSystem.GetPool(static_cast<uint32_t>(ent));
			if (!pool)
				return ScriptValue::Number(0.0);

			return ScriptValue::Number(static_cast<double>(pool->GetEventQueue().Size()));
		}

		ScriptValue ParticleFlushCollisionHitsImpl(const ScriptValue* args, std::size_t count)
		{
			NormalizeArgs(args, count);
			Scene* scene = ScriptEngine::GetActiveScene();
			if (!scene) throw std::logic_error("Particle.FlushCollisionHits requires an active scene");
			const auto& method = ParticleServiceBindings(nullptr)[0].Methods[15];
			entt::entity ent = RequireEntityId("Particle", method, args, count, 0);

			auto* comp = const_cast<entt::registry&>(static_cast<const Scene*>(scene)->GetRegistry()).try_get<ParticleSystemComponent>(ent);
			if (!comp)
				return ScriptValue::Number(0.0);

			size_t hitsCount = 0;
			auto* pool1 = GetGlobalParticleSystem().GetPool(static_cast<uint32_t>(ent));
			if (pool1) hitsCount += pool1->GetEventQueueMut().Flush().size();
			auto* pool2 = s_ParticleSystem.GetPool(static_cast<uint32_t>(ent));
			if (pool2) hitsCount += pool2->GetEventQueueMut().Flush().size();

			return ScriptValue::Number(static_cast<double>(hitsCount));
		}

		
		inline double GetOptionalNum(const ScriptValue* args, std::size_t count, std::size_t idx, double fallback)
		{
			if (idx < count && !args[idx].IsNil())
			{
				double val = fallback;
				if (args[idx].AsNumber(&val)) return val;
			}
			return fallback;
		}

		ScriptValue ParticleSpawnOneShotImpl(const ScriptValue* args, std::size_t count)
		{
			NormalizeArgs(args, count);
			double x = GetOptionalNum(args, count, 0, 0.0);
			double y = GetOptionalNum(args, count, 1, 0.0);
			double z = GetOptionalNum(args, count, 2, 0.0);
			double particleCount = GetOptionalNum(args, count, 3, 30.0);
			double speed = GetOptionalNum(args, count, 4, 5.0);
			double life = GetOptionalNum(args, count, 5, 1.0);

			ParticleSystemComponent comp;
			comp.MaxParticles = static_cast<uint32_t>(particleCount);
			comp.Speed = static_cast<float>(speed);
			comp.Lifetime = static_cast<float>(life);
			comp.EmissionRate = 0.0f;
			comp.IsPlaying = 1;
			comp.EmitterType = ParticleEmitterType::Sphere;

			ParticleMaterialSettings mat;
			GetGlobalParticleSystem().SpawnOneShot(comp, mat, glm::vec3(x, y, z));
			s_ParticleSystem.SpawnOneShot(comp, mat, glm::vec3(x, y, z));
			return ScriptValue::Boolean(true);
		}

		ScriptValue ParticleAddSubEmitterImpl(const ScriptValue* args, std::size_t count)
		{
			NormalizeArgs(args, count);
			Scene* scene = ScriptEngine::GetActiveScene();
			if (!scene) throw std::logic_error("Particle.AddSubEmitter requires an active scene");
			const auto& method = ParticleServiceBindings(nullptr)[0].Methods[17];
			entt::entity ent = RequireEntityId("Particle", method, args, count, 0);
			double triggerVal = (count > 1 && !args[1].IsNil()) ? RequireNum("Particle", method, args, count, 1) : 1.0;
			double emitCount = (count > 2 && !args[2].IsNil()) ? RequireNum("Particle", method, args, count, 2) : 4.0;

			auto* comp = const_cast<entt::registry&>(static_cast<const Scene*>(scene)->GetRegistry()).try_get<ParticleSystemComponent>(ent);
			if (!comp)
				return ScriptValue::Boolean(false);

			SubEmitterConfig sub;
			sub.Trigger = (triggerVal <= 0.0) ? SubEmitterTrigger::OnBirth : ((triggerVal >= 2.0) ? SubEmitterTrigger::OnDeath : SubEmitterTrigger::OnCollision);
			sub.EmitCount = static_cast<uint32_t>(emitCount);
			sub.SpawnTemplate.Lifetime = 0.5f;
			sub.SpawnTemplate.Speed = 3.0f;
			sub.SpawnTemplate.SizeStart = 0.2f;
			sub.SpawnTemplate.SizeEnd = 0.05f;

			GetGlobalParticleSystem().GetPoolForEntity(static_cast<uint32_t>(ent), comp->MaxParticles).AddSubEmitter(sub);
			s_ParticleSystem.GetPoolForEntity(static_cast<uint32_t>(ent), comp->MaxParticles).AddSubEmitter(sub);
			return ScriptValue::Boolean(true);
		}


		// ---- VFX Authoring & Playback Implementation Methods ----
		ScriptValue VfxCreateImpl(const ScriptValue* args, std::size_t count)
		{
			NormalizeArgs(args, count);
			const auto& method = VfxServiceBindings(nullptr)[0].Methods[0];
			std::string name = RequireString("VFX", method, args, count, 0);

			ParticleAsset asset;
			asset.Name = name;
			static uint64_t s_CreateGuid = 10000;
			asset.Guid = ++s_CreateGuid;
			uint64_t guid = ParticleAssetRegistry::Get().RegisterAssetFromMemory(name, asset);
			return ScriptValue::Number(static_cast<double>(guid));
		}

		ScriptValue VfxCompileFromYamlImpl(const ScriptValue* args, std::size_t count)
		{
			NormalizeArgs(args, count);
			const auto& method = VfxServiceBindings(nullptr)[0].Methods[1];
			std::string yaml = RequireString("VFX", method, args, count, 0);

			ParticleAsset asset;
			if (!ParticleAssetSerializer::DeserializeFromString(yaml, asset))
			{
				throw std::logic_error("VFX.CompileFromYaml: YAML syntax or schema error; failed to parse VFX definition");
			}
			if (asset.Guid == 0)
			{
				static uint64_t s_CompileGuid = 20000;
				asset.Guid = ++s_CompileGuid;
			}
			uint64_t guid = ParticleAssetRegistry::Get().RegisterAssetFromMemory(asset.Name, asset);
			return ScriptValue::Number(static_cast<double>(guid));
		}

		ScriptValue VfxToYamlImpl(const ScriptValue* args, std::size_t count)
		{
			NormalizeArgs(args, count);
			const auto& method = VfxServiceBindings(nullptr)[0].Methods[2];
			double guidNum = RequireNum("VFX", method, args, count, 0);
			uint64_t guid = static_cast<uint64_t>(guidNum);

			const auto* asset = ParticleAssetRegistry::Get().GetAsset(guid);
			if (!asset)
			{
				throw std::logic_error("VFX.ToYaml: asset not found for GUID " + std::to_string(guid));
			}
			return ScriptValue::String(ParticleAssetSerializer::SerializeToString(*asset));
		}

		ScriptValue VfxLoadImpl(const ScriptValue* args, std::size_t count)
		{
			NormalizeArgs(args, count);
			const auto& method = VfxServiceBindings(nullptr)[0].Methods[3];
			std::string path = RequireString("VFX", method, args, count, 0);

			uint64_t guid = ParticleAssetRegistry::Get().RegisterAsset(path);
			if (guid == 0)
			{
				throw std::logic_error("VFX.Load: failed to load or parse asset file at " + path);
			}
			return ScriptValue::Number(static_cast<double>(guid));
		}

		ScriptValue VfxSpawnImpl(const ScriptValue* args, std::size_t count)
		{
			NormalizeArgs(args, count);
			const auto& method = VfxServiceBindings(nullptr)[0].Methods[4];
			double guidNum = RequireNum("VFX", method, args, count, 0);
			double x = RequireNum("VFX", method, args, count, 1);
			double y = RequireNum("VFX", method, args, count, 2);
			double z = RequireNum("VFX", method, args, count, 3);

			uint64_t guid = static_cast<uint64_t>(guidNum);
			const auto* asset = ParticleAssetRegistry::Get().GetAsset(guid);
			if (!asset)
			{
				return ScriptValue::Boolean(false);
			}

			glm::vec3 pos(static_cast<float>(x), static_cast<float>(y), static_cast<float>(z));
			GetGlobalParticleSystem().SpawnOneShotAsset(*asset, pos);
			return ScriptValue::Boolean(true);
		}

		ScriptValue VfxAddEmitterImpl(const ScriptValue* args, std::size_t count)
		{
			NormalizeArgs(args, count);
			const auto& method = VfxServiceBindings(nullptr)[0].Methods[5];
			double guidNum = RequireNum("VFX", method, args, count, 0);
			std::string name = RequireString("VFX", method, args, count, 1);

			uint64_t guid = static_cast<uint64_t>(guidNum);
			auto* asset = ParticleAssetRegistry::Get().GetAssetMut(guid);
			if (!asset)
			{
				throw std::logic_error("VFX.AddEmitter: asset not found for GUID " + std::to_string(guid));
			}

			ParticleEmitterLayer em;
			em.Name = name;
			asset->Emitters.push_back(em);
			return ScriptValue::Number(static_cast<double>(asset->Emitters.size() - 1));
		}

		ScriptValue VfxSetForcesImpl(const ScriptValue* args, std::size_t count)
		{
			NormalizeArgs(args, count);
			const auto& method = VfxServiceBindings(nullptr)[0].Methods[6];
			double guidNum = RequireNum("VFX", method, args, count, 0);
			double idx = RequireNum("VFX", method, args, count, 1);
			double gx = (count > 2 && !args[2].IsNil()) ? RequireNum("VFX", method, args, count, 2) : 0.0;
			double gy = (count > 3 && !args[3].IsNil()) ? RequireNum("VFX", method, args, count, 3) : -9.8;
			double gz = (count > 4 && !args[4].IsNil()) ? RequireNum("VFX", method, args, count, 4) : 0.0;
			double drag = (count > 5 && !args[5].IsNil()) ? RequireNum("VFX", method, args, count, 5) : 0.0;
			double vortex = (count > 6 && !args[6].IsNil()) ? RequireNum("VFX", method, args, count, 6) : 0.0;
			double turb = (count > 7 && !args[7].IsNil()) ? RequireNum("VFX", method, args, count, 7) : 0.0;

			uint64_t guid = static_cast<uint64_t>(guidNum);
			auto* asset = ParticleAssetRegistry::Get().GetAssetMut(guid);
			if (!asset) throw std::logic_error("VFX.SetForces: asset not found for GUID " + std::to_string(guid));

			size_t emIdx = static_cast<size_t>(idx);
			if (emIdx >= asset->Emitters.size()) throw std::logic_error("VFX.SetForces: emitter index out of range");

			auto& em = asset->Emitters[emIdx];
			em.Gravity = glm::vec3(static_cast<float>(gx), static_cast<float>(gy), static_cast<float>(gz));
			em.LinearDrag = static_cast<float>(drag);
			em.VortexStrength = static_cast<float>(vortex);
			em.TurbulenceStrength = static_cast<float>(turb);
			return ScriptValue::Boolean(true);
		}

		ScriptValue VfxSetMaterialImpl(const ScriptValue* args, std::size_t count)
		{
			NormalizeArgs(args, count);
			const auto& method = VfxServiceBindings(nullptr)[0].Methods[7];
			double guidNum = RequireNum("VFX", method, args, count, 0);
			double idx = RequireNum("VFX", method, args, count, 1);
			std::string blendStr = RequireString("VFX", method, args, count, 2);
			std::string facingStr = (count > 3 && !args[3].IsNil()) ? RequireString("VFX", method, args, count, 3) : "Billboard";
			double emissive = (count > 4 && !args[4].IsNil()) ? RequireNum("VFX", method, args, count, 4) : 1.0;

			uint64_t guid = static_cast<uint64_t>(guidNum);
			auto* asset = ParticleAssetRegistry::Get().GetAssetMut(guid);
			if (!asset) throw std::logic_error("VFX.SetMaterial: asset not found for GUID " + std::to_string(guid));

			size_t emIdx = static_cast<size_t>(idx);
			if (emIdx >= asset->Emitters.size()) throw std::logic_error("VFX.SetMaterial: emitter index out of range");

			auto& em = asset->Emitters[emIdx];
			if (blendStr == "Additive") em.Material.BlendMode = ParticleBlendMode::Additive;
			else if (blendStr == "Multiply") em.Material.BlendMode = ParticleBlendMode::Multiply;
			else if (blendStr == "Opaque") em.Material.BlendMode = ParticleBlendMode::Opaque;
			else em.Material.BlendMode = ParticleBlendMode::AlphaBlend;

			if (facingStr == "StretchedVelocity") em.FacingMode = ParticleRenderMode::StretchedVelocity;
			else if (facingStr == "HorizontalQuad") em.FacingMode = ParticleRenderMode::HorizontalQuad;
			else em.FacingMode = ParticleRenderMode::Billboard;
			em.Material.FacingMode = em.FacingMode;

			em.Material.EmissiveBoost = static_cast<float>(emissive);
			return ScriptValue::Boolean(true);
		}

		ScriptValue VfxAddBurstImpl(const ScriptValue* args, std::size_t count)
		{
			NormalizeArgs(args, count);
			const auto& method = VfxServiceBindings(nullptr)[0].Methods[8];
			double guidNum = RequireNum("VFX", method, args, count, 0);
			double idx = RequireNum("VFX", method, args, count, 1);
			double time = RequireNum("VFX", method, args, count, 2);
			double bCount = RequireNum("VFX", method, args, count, 3);

			uint64_t guid = static_cast<uint64_t>(guidNum);
			auto* asset = ParticleAssetRegistry::Get().GetAssetMut(guid);
			if (!asset) throw std::logic_error("VFX.AddBurst: asset not found for GUID " + std::to_string(guid));

			size_t emIdx = static_cast<size_t>(idx);
			if (emIdx >= asset->Emitters.size()) throw std::logic_error("VFX.AddBurst: emitter index out of range");

			asset->Emitters[emIdx].Bursts.push_back({ static_cast<float>(time), static_cast<uint32_t>(bCount) });
			return ScriptValue::Boolean(true);
		}

		ScriptValue VfxGetEmitterCountImpl(const ScriptValue* args, std::size_t count)
		{
			NormalizeArgs(args, count);
			const auto& method = VfxServiceBindings(nullptr)[0].Methods[9];
			double guidNum = RequireNum("VFX", method, args, count, 0);

			uint64_t guid = static_cast<uint64_t>(guidNum);
			const auto* asset = ParticleAssetRegistry::Get().GetAsset(guid);
			if (!asset) return ScriptValue::Number(0.0);
			return ScriptValue::Number(static_cast<double>(asset->Emitters.size()));
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
		static const ScriptServiceParam seedParams[] = {
			{ "entityId", "number", ScriptServiceArgType::Number, true, "Target entity ID or Entity object." },
			{ "seed", "number", ScriptServiceArgType::Number, true, "Deterministic PRNG seed for particle replay." }
		};
		static const ScriptServiceParam subUVParams[] = {
			{ "entityId", "number", ScriptServiceArgType::Number, true, "Target entity ID or Entity object." },
			{ "tilesX", "number", ScriptServiceArgType::Number, true, "Sub-UV horizontal grid columns." },
			{ "tilesY", "number", ScriptServiceArgType::Number, true, "Sub-UV vertical grid rows." },
			{ "frameRate", "number", ScriptServiceArgType::Number, false, "Flipbook playback FPS (0 = map over particle lifetime)." },
			{ "randomStart", "boolean", ScriptServiceArgType::Boolean, false, "Pick random initial frame per particle." }
		};
		static const ScriptServiceParam softParticleParams[] = {
			{ "entityId", "number", ScriptServiceArgType::Number, true, "Target entity ID or Entity object." },
			{ "enabled", "boolean", ScriptServiceArgType::Boolean, true, "Enable soft particle depth fade." },
			{ "fadeDistance", "number", ScriptServiceArgType::Number, false, "Depth fade transition distance." }
		};
		static const ScriptServiceParam emissiveParams[] = {
			{ "entityId", "number", ScriptServiceArgType::Number, true, "Target entity ID or Entity object." },
			{ "boost", "number", ScriptServiceArgType::Number, true, "HDR emissive multiplier for Bloom." }
		};
		static const ScriptServiceParam collisionParams[] = {
			{ "entityId", "number", ScriptServiceArgType::Number, true, "Target entity ID or Entity object." },
			{ "mode", "number", ScriptServiceArgType::Number, false, "Collision mode: 0=None, 1=Plane." },
			{ "restitution", "number", ScriptServiceArgType::Number, false, "Restitution bounce coefficient in [0, 1]." },
			{ "friction", "number", ScriptServiceArgType::Number, false, "Surface friction coefficient in [0, 1]." },
			{ "killOnHit", "boolean", ScriptServiceArgType::Boolean, false, "Kill particle on collision." }
		};
		static const ScriptServiceParam oneShotParams[] = {
			{ "x", "number", ScriptServiceArgType::Number, true, "X world position." },
			{ "y", "number", ScriptServiceArgType::Number, true, "Y world position." },
			{ "z", "number", ScriptServiceArgType::Number, true, "Z world position." },
			{ "count", "number", ScriptServiceArgType::Number, false, "Number of burst particles." },
			{ "speed", "number", ScriptServiceArgType::Number, false, "Initial burst speed." },
			{ "lifetime", "number", ScriptServiceArgType::Number, false, "Particle lifetime." }
		};
		static const ScriptServiceParam subEmitterParams[] = {
			{ "entityId", "number", ScriptServiceArgType::Number, true, "Target entity ID or Entity object." },
			{ "trigger", "number", ScriptServiceArgType::Number, true, "Trigger type: 0=OnBirth, 1=OnCollision, 2=OnDeath." },
			{ "count", "number", ScriptServiceArgType::Number, false, "Number of secondary particles to spawn." }
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
			{ "SetLifetime", [](const ScriptValue* args, std::size_t count) { return ParticleSetLifetimeImpl(args, count); }, lifetimeParams, 2, 2, "boolean", "Set particle lifetime in seconds." },
			{ "SetSeed", [](const ScriptValue* args, std::size_t count) { return ParticleSetSeedImpl(args, count); }, seedParams, 2, 2, "boolean", "Set deterministic PRNG seed for particle replay." },
			{ "SetSubUV", [](const ScriptValue* args, std::size_t count) { return ParticleSetSubUVImpl(args, count); }, subUVParams, 5, 3, "boolean", "Configure Sub-UV flipbook grid and frame rate." },
			{ "SetSoftParticles", [](const ScriptValue* args, std::size_t count) { return ParticleSetSoftParticlesImpl(args, count); }, softParticleParams, 3, 2, "boolean", "Configure soft particle depth fade." },
			{ "SetEmissiveBoost", [](const ScriptValue* args, std::size_t count) { return ParticleSetEmissiveBoostImpl(args, count); }, emissiveParams, 2, 2, "boolean", "Set HDR emissive multiplier for Bloom." },
			{ "SetCollision", [](const ScriptValue* args, std::size_t count) { return ParticleSetCollisionImpl(args, count); }, collisionParams, 5, 1, "boolean", "Configure physics plane collision and bounce properties." },
			{ "GetCollisionHitCount", [](const ScriptValue* args, std::size_t count) { return ParticleGetCollisionHitCountImpl(args, count); }, singleEntityParams, 1, 1, "number", "Get pending collision hits count." },
			{ "FlushCollisionHits", [](const ScriptValue* args, std::size_t count) { return ParticleFlushCollisionHitsImpl(args, count); }, singleEntityParams, 1, 1, "number", "Flush and return buffered collision hits count." },
			{ "SpawnOneShot", [](const ScriptValue* args, std::size_t count) { return ParticleSpawnOneShotImpl(args, count); }, oneShotParams, 6, 3, "boolean", "Spawn an entity-less one-shot particle burst." },
			{ "AddSubEmitter", [](const ScriptValue* args, std::size_t count) { return ParticleAddSubEmitterImpl(args, count); }, subEmitterParams, 3, 2, "boolean", "Register a secondary emitter triggered on birth, collision, or death." }
		};

		static const ScriptServiceParam vfxCreateParams[] = {
			{ "name", "string", ScriptServiceArgType::String, true, "Asset name identifier." }
		};
		static const ScriptServiceParam vfxCompileParams[] = {
			{ "yaml", "string", ScriptServiceArgType::String, true, "YAML asset text to parse and compile." }
		};
		static const ScriptServiceParam vfxToYamlParams[] = {
			{ "guid", "number", ScriptServiceArgType::Number, true, "Asset GUID." }
		};
		static const ScriptServiceParam vfxLoadParams[] = {
			{ "path", "string", ScriptServiceArgType::String, true, "Asset file path." }
		};
		static const ScriptServiceParam vfxSpawnParams[] = {
			{ "guid", "number", ScriptServiceArgType::Number, true, "Asset GUID." },
			{ "x", "number", ScriptServiceArgType::Number, true, "World position X." },
			{ "y", "number", ScriptServiceArgType::Number, true, "World position Y." },
			{ "z", "number", ScriptServiceArgType::Number, true, "World position Z." }
		};
		static const ScriptServiceParam vfxAddEmitterParams[] = {
			{ "guid", "number", ScriptServiceArgType::Number, true, "Asset GUID." },
			{ "name", "string", ScriptServiceArgType::String, true, "Emitter layer name." }
		};
		static const ScriptServiceParam vfxSetForcesParams[] = {
			{ "guid", "number", ScriptServiceArgType::Number, true, "Asset GUID." },
			{ "emitterIndex", "number", ScriptServiceArgType::Number, true, "Emitter layer index." },
			{ "gravityX", "number", ScriptServiceArgType::Number, false, "Gravity X acceleration." },
			{ "gravityY", "number", ScriptServiceArgType::Number, false, "Gravity Y acceleration." },
			{ "gravityZ", "number", ScriptServiceArgType::Number, false, "Gravity Z acceleration." },
			{ "drag", "number", ScriptServiceArgType::Number, false, "Linear drag damping." },
			{ "vortex", "number", ScriptServiceArgType::Number, false, "Vortex swirl strength." },
			{ "turb", "number", ScriptServiceArgType::Number, false, "Turbulence strength." }
		};
		static const ScriptServiceParam vfxSetMaterialParams[] = {
			{ "guid", "number", ScriptServiceArgType::Number, true, "Asset GUID." },
			{ "emitterIndex", "number", ScriptServiceArgType::Number, true, "Emitter layer index." },
			{ "blendMode", "string", ScriptServiceArgType::String, true, "Blend mode (AlphaBlend/Additive/Multiply/Opaque)." },
			{ "facingMode", "string", ScriptServiceArgType::String, false, "Facing mode (Billboard/StretchedVelocity/HorizontalQuad)." },
			{ "emissive", "number", ScriptServiceArgType::Number, false, "HDR emissive boost multiplier." }
		};
		static const ScriptServiceParam vfxAddBurstParams[] = {
			{ "guid", "number", ScriptServiceArgType::Number, true, "Asset GUID." },
			{ "emitterIndex", "number", ScriptServiceArgType::Number, true, "Emitter layer index." },
			{ "time", "number", ScriptServiceArgType::Number, true, "Burst trigger time in seconds." },
			{ "count", "number", ScriptServiceArgType::Number, true, "Burst particle count." }
		};
		static const ScriptServiceParam vfxGetEmitterCountParams[] = {
			{ "guid", "number", ScriptServiceArgType::Number, true, "Asset GUID." }
		};

		static const ScriptServiceMethod vfxMethods[] = {
			{ "Create", [](const ScriptValue* a, std::size_t c) { return VfxCreateImpl(a, c); }, vfxCreateParams, 1, 1, "number", "Create a new in-memory VFX asset." },
			{ "CompileFromYaml", [](const ScriptValue* a, std::size_t c) { return VfxCompileFromYamlImpl(a, c); }, vfxCompileParams, 1, 1, "number", "Compile VFX definition directly from YAML text into memory." },
			{ "ToYaml", [](const ScriptValue* a, std::size_t c) { return VfxToYamlImpl(a, c); }, vfxToYamlParams, 1, 1, "string", "Serialize VFX asset definition to YAML string." },
			{ "Load", [](const ScriptValue* a, std::size_t c) { return VfxLoadImpl(a, c); }, vfxLoadParams, 1, 1, "number", "Load and register VFX asset from disk file." },
			{ "Spawn", [](const ScriptValue* a, std::size_t c) { return VfxSpawnImpl(a, c); }, vfxSpawnParams, 4, 4, "boolean", "Spawn and play multi-emitter VFX instance at world coordinates." },
			{ "AddEmitter", [](const ScriptValue* a, std::size_t c) { return VfxAddEmitterImpl(a, c); }, vfxAddEmitterParams, 2, 2, "number", "Add an emitter layer to a VFX asset." },
			{ "SetForces", [](const ScriptValue* a, std::size_t c) { return VfxSetForcesImpl(a, c); }, vfxSetForcesParams, 8, 2, "boolean", "Configure gravity, drag, vortex, and turbulence on an emitter layer." },
			{ "SetMaterial", [](const ScriptValue* a, std::size_t c) { return VfxSetMaterialImpl(a, c); }, vfxSetMaterialParams, 5, 3, "boolean", "Configure blend mode, facing mode, and emissive boost on an emitter layer." },
			{ "AddBurst", [](const ScriptValue* a, std::size_t c) { return VfxAddBurstImpl(a, c); }, vfxAddBurstParams, 4, 4, "boolean", "Add a timed burst event to an emitter layer." },
			{ "GetEmitterCount", [](const ScriptValue* a, std::size_t c) { return VfxGetEmitterCountImpl(a, c); }, vfxGetEmitterCountParams, 1, 1, "number", "Get emitter count in VFX asset." }
		};

		static const ScriptServiceBinding vfxBindingsTable[] = {
			{ "VFX", "Industrial Visual Effects (VFX) authoring, asset compilation and playback service.", vfxMethods, sizeof(vfxMethods)/sizeof(vfxMethods[0]) }
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



	const ScriptServiceBinding* VfxServiceBindings(std::size_t* count)
	{
		if (count) *count = sizeof(vfxBindingsTable) / sizeof(vfxBindingsTable[0]);
		return vfxBindingsTable;
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

		std::size_t vfxCount = 0;
		const ScriptServiceBinding* vfxServices = VfxServiceBindings(&vfxCount);
		for (std::size_t index = 0; index < vfxCount; ++index)
		{
			if (!RegisterServiceTable(bindings, vfxServices[index], error))
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
