#include "wldpch.h"
#include "World/Renderer/ParticleAsset.h"
#include "World/Renderer/VfxModule.h"
#include "World/Renderer/VfxComputeGenerator.h"

#include "World/Core/Log.h"

#include <iostream>
#include <cassert>
#include <cmath>

using namespace World;

static void TestParticleAssetSerializationRoundtrip()
{
	ParticleAsset original;
	original.Name = "CampfireComplete";
	original.Guid = 9876543210ULL;
	original.Seed = 20261018u;
	original.CullDistance = 75.0f;
	original.PauseWhenOffscreen = true;
	original.UserParamsFloat["Intensity"] = 1.5f;

	// Emitter Layer 1: CoreFlame
	ParticleEmitterLayer flame;
	flame.Name = "CoreFlame";
	flame.MaxParticles = 600;
	flame.Lifetime = 1.8f;
	flame.EmissionRate = 75.0f;
	flame.EmitterType = ParticleEmitterType::Cone;
	flame.LockToXY = false;
	flame.Material.TexturePath = "assets/textures/fx/flame.png";
	flame.Material.BlendMode = ParticleBlendMode::Additive;
	flame.Material.EmissiveBoost = 2.8f;
	flame.Material.SoftParticlesEnabled = true;
	flame.Material.SoftParticleFadeDistance = 0.25f;
	flame.Material.SubUVTilesX = 4;
	flame.Material.SubUVTilesY = 4;
	flame.Material.SubUVFrameRate = 24.0f;
	flame.Material.SubUVRandomStartFrame = true;
	flame.Speed = 3.2f;
	flame.SpeedVariance = 0.8f;
	flame.Gravity = glm::vec3(0.0f, 1.5f, 0.0f);
	flame.SizeStart = 0.6f;
	flame.SizeEnd = 0.1f;
	flame.ColorStart = glm::vec4(1.0f, 0.7f, 0.2f, 1.0f);
	flame.ColorEnd = glm::vec4(0.8f, 0.1f, 0.0f, 0.0f);
	flame.Collision.Mode = ParticleCollisionMode::Plane;
	flame.Collision.Restitution = 0.4f;
	flame.Collision.Friction = 0.15f;
	flame.Collision.KillOnHit = false;
	flame.Collision.EnableEvents = true;
	flame.Collision.PlaneOrigin = glm::vec3(0.0f, 0.0f, 0.0f);
	flame.Collision.PlaneNormal = glm::vec3(0.0f, 1.0f, 0.0f);
	original.Emitters.push_back(flame);

	// Emitter Layer 2: Sparks
	ParticleEmitterLayer sparks;
	sparks.Name = "Sparks";
	sparks.MaxParticles = 150;
	sparks.Lifetime = 0.9f;
	sparks.EmissionRate = 20.0f;
	sparks.EmitterType = ParticleEmitterType::Sphere;
	sparks.Material.BlendMode = ParticleBlendMode::Additive;
	sparks.Material.EmissiveBoost = 4.5f;
	sparks.Speed = 6.0f;
	original.Emitters.push_back(sparks);

	// Serialize
	std::string yamlStr = ParticleAssetSerializer::SerializeToString(original);
	assert(!yamlStr.empty());

	// Deserialize
	ParticleAsset deserialized;
	bool ok = ParticleAssetSerializer::DeserializeFromString(yamlStr, deserialized);
	assert(ok);
	assert(deserialized.Name == "CampfireComplete");
	assert(deserialized.Guid == 9876543210ULL);
	assert(deserialized.Seed == 20261018u);
	assert(std::abs(deserialized.CullDistance - 75.0f) < 1e-4f);
	assert(deserialized.PauseWhenOffscreen == true);
	assert(deserialized.UserParamsFloat.count("Intensity") == 1);
	assert(std::abs(deserialized.UserParamsFloat["Intensity"] - 1.5f) < 1e-4f);

	assert(deserialized.Emitters.size() == 2);
	const auto& em0 = deserialized.Emitters[0];
	assert(em0.Name == "CoreFlame");
	assert(em0.MaxParticles == 600);
	assert(std::abs(em0.Lifetime - 1.8f) < 1e-4f);
	assert(std::abs(em0.EmissionRate - 75.0f) < 1e-4f);
	assert(em0.EmitterType == ParticleEmitterType::Cone);
	assert(em0.Material.BlendMode == ParticleBlendMode::Additive);
	assert(std::abs(em0.Material.EmissiveBoost - 2.8f) < 1e-4f);
	assert(em0.Material.SoftParticlesEnabled == true);
	assert(std::abs(em0.Material.SoftParticleFadeDistance - 0.25f) < 1e-4f);
	assert(em0.Material.SubUVTilesX == 4);
	assert(em0.Material.SubUVTilesY == 4);
	assert(std::abs(em0.Material.SubUVFrameRate - 24.0f) < 1e-4f);
	assert(em0.Material.SubUVRandomStartFrame == true);
	assert(em0.Collision.Mode == ParticleCollisionMode::Plane);
	assert(em0.Collision.EnableEvents == true);

	const auto& em1 = deserialized.Emitters[1];
	assert(em1.Name == "Sparks");
	assert(em1.MaxParticles == 150);
	assert(std::abs(em1.Material.EmissiveBoost - 4.5f) < 1e-4f);

	std::cout << "[PASS] TestParticleAssetSerializationRoundtrip\n";
}

static void TestParticleAssetBackwardCompatibilitySingleEmitter()
{
	const std::string legacyYaml = R"(
FormatVersion: 1
Name: LegacyTorch
Lifetime: 2.5
EmissionRate: 40.0
Material:
  TexturePath: textures/fx/torch.png
  BlendMode: Additive
  EmissiveBoost: 2.0
Motion:
  Speed: 4.0
)";

	ParticleAsset asset;
	bool ok = ParticleAssetSerializer::DeserializeFromString(legacyYaml, asset);
	assert(ok);
	assert(asset.Name == "LegacyTorch");
	assert(asset.Emitters.size() == 1);
	assert(asset.Emitters[0].Name == "LegacyTorch");
	assert(std::abs(asset.Emitters[0].Lifetime - 2.5f) < 1e-4f);
	assert(std::abs(asset.Emitters[0].EmissionRate - 40.0f) < 1e-4f);
	assert(asset.Emitters[0].Material.TexturePath == "textures/fx/torch.png");
	assert(asset.Emitters[0].Material.BlendMode == ParticleBlendMode::Additive);
	assert(std::abs(asset.Emitters[0].Material.EmissiveBoost - 2.0f) < 1e-4f);

	std::cout << "[PASS] TestParticleAssetBackwardCompatibilitySingleEmitter\n";
}

static void TestParticleAssetRegistry()
{
	auto& registry = ParticleAssetRegistry::Get();
	registry.Clear();
	assert(registry.Count() == 0);

	ParticleAsset asset;
	asset.Name = "LaserBlast";
	asset.Seed = 42;
	ParticleEmitterLayer layer;
	layer.Name = "Beam";
	layer.MaxParticles = 100;
	asset.Emitters.push_back(layer);

	uint64_t guid = registry.RegisterAssetFromMemory("assets/vfx/laser.wvfx", asset);
	assert(guid != 0);
	assert(registry.Count() == 1);

	const ParticleAsset* foundByGuid = registry.GetAsset(guid);
	assert(foundByGuid != nullptr);
	assert(foundByGuid->Name == "LaserBlast");
	assert(foundByGuid->Emitters.size() == 1);

	const ParticleAsset* foundByPath = registry.GetAssetByPath("assets/vfx/laser.wvfx");
	assert(foundByPath != nullptr);
	assert(foundByPath == foundByGuid);

	registry.Clear();
	assert(registry.Count() == 0);
	assert(registry.GetAsset(guid) == nullptr);

	std::cout << "[PASS] TestParticleAssetRegistry\n";
}

static void TestParticleAssetNewDynamicsAndBursts()
{
	ParticleAsset original;
	original.Name = "ExplosionComplete";
	original.Guid = 555666777ULL;
	original.Seed = 2026;
	original.Duration = 2.5f;
	original.Looping = false;
	original.WarmupTime = 0.1f;

	ParticleEmitterLayer fireball;
	fireball.Name = "Fireball";
	fireball.Enabled = true;
	fireball.MaxParticles = 500;
	fireball.Lifetime = 1.2f;
	fireball.EmissionRate = 0.0f;
	fireball.FacingMode = ParticleRenderMode::StretchedVelocity;
	fireball.Material.BlendMode = ParticleBlendMode::Additive;
	fireball.LinearDrag = 1.5f;
	fireball.VortexStrength = 2.8f;
	fireball.VortexAxis = glm::vec3(0.0f, 1.0f, 0.0f);
	fireball.TurbulenceStrength = 3.2f;
	fireball.TurbulenceFrequency = 2.0f;
	fireball.Bursts.push_back({ 0.0f, 40 });
	fireball.Bursts.push_back({ 0.3f, 60 });
	original.Emitters.push_back(fireball);

	std::string yamlStr = ParticleAssetSerializer::SerializeToString(original);
	assert(!yamlStr.empty());
	assert(yamlStr.find("Duration: 2.5") != std::string::npos);
	assert(yamlStr.find("Looping: false") != std::string::npos);
	assert(yamlStr.find("WarmupTime: 0.1") != std::string::npos);
	assert(yamlStr.find("FacingMode: StretchedVelocity") != std::string::npos);
	assert(yamlStr.find("LinearDrag: 1.5") != std::string::npos);
	assert(yamlStr.find("VortexStrength:") != std::string::npos);
	assert(yamlStr.find("TurbulenceStrength:") != std::string::npos);

	ParticleAsset deserialized;
	bool ok = ParticleAssetSerializer::DeserializeFromString(yamlStr, deserialized);
	assert(ok);
	assert(deserialized.Name == "ExplosionComplete");
	assert(std::abs(deserialized.Duration - 2.5f) < 1e-4f);
	assert(deserialized.Looping == false);
	assert(std::abs(deserialized.WarmupTime - 0.1f) < 1e-4f);

	assert(deserialized.Emitters.size() == 1);
	const auto& em = deserialized.Emitters[0];
	assert(em.Name == "Fireball");
	assert(em.FacingMode == ParticleRenderMode::StretchedVelocity);
	assert(std::abs(em.LinearDrag - 1.5f) < 1e-4f);
	assert(std::abs(em.VortexStrength - 2.8f) < 1e-4f);
	assert(std::abs(em.TurbulenceStrength - 3.2f) < 1e-4f);
	assert(std::abs(em.TurbulenceFrequency - 2.0f) < 1e-4f);
	assert(em.Bursts.size() == 2);
	assert(std::abs(em.Bursts[0].Time - 0.0f) < 1e-4f && em.Bursts[0].Count == 40);
	assert(std::abs(em.Bursts[1].Time - 0.3f) < 1e-4f && em.Bursts[1].Count == 60);

	std::cout << "[PASS] TestParticleAssetNewDynamicsAndBursts\n";
}

static void TestParticleAssetShowcasePresets3D()
{
	std::cout << "Building and Verifying Commercial 3D VFX Showcase Presets with Pure Niagara Modular Stacks...\n";

	// 1. OrbitalMagicShield3D
	ParticleAsset shieldAsset;
	shieldAsset.Name = "OrbitalMagicShield3D";
	shieldAsset.Duration = 5.0f;
	shieldAsset.Looping = true;
	{
		// Emitter 1: RunicGroundCircle
		ParticleEmitterLayer em1;
		em1.Name = "RunicShieldRing";
		em1.MaxParticles = 1;
		em1.Lifetime = 9999.0f;
		em1.EmissionRate = 0.0f;
		em1.FacingMode = ParticleRenderMode::HorizontalQuad;
		em1.Bursts = { { 0.0f, 1u } };
		em1.Material.TexturePath = "textures/particles/magic_rune.wtex";
		em1.Material.MaterialPath = "materials/particles/particle_additive.wmat";
		em1.Material.BlendMode = ParticleBlendMode::Additive;
		em1.Material.EmissiveBoost = 2.5f;
		em1.Modules = {
			VfxModuleLibrary::CreateSpawnBurstModule(1, 0.0f),
			VfxModuleLibrary::CreateInitParticleModule(9999.0f, 2.5f, 2.5f),
			VfxModuleLibrary::CreatePointSpawnModule(),
			VfxModuleLibrary::CreateSolveVelocityModule(),
			VfxModuleLibrary::CreateSpriteRendererModule(true, true, 2.5f)
		};
		shieldAsset.Emitters.push_back(em1);

		// Emitter 2: OrbitingEnergySpheres
		ParticleEmitterLayer em2;
		em2.Name = "OrbitingEnergySpheres";
		em2.MaxParticles = 60;
		em2.Lifetime = 2.2f;
		em2.EmissionRate = 30.0f;
		em2.FacingMode = ParticleRenderMode::Billboard;
		em2.Material.TexturePath = "textures/particles/spark_glow.wtex";
		em2.Material.MaterialPath = "materials/particles/particle_additive.wmat";
		em2.Material.BlendMode = ParticleBlendMode::Additive;
		em2.Material.EmissiveBoost = 3.5f;
		em2.Modules = {
			VfxModuleLibrary::CreateSpawnRateModule(30.0f),
			VfxModuleLibrary::CreateInitParticleModule(2.2f, 0.22f, 0.06f),
			VfxModuleLibrary::CreateRingSpawnModule(1.8f, 0.0f),
			VfxModuleLibrary::CreateOrbitModule(3.5f, { 0.0f, 1.0f, 0.0f }, { 0.0f, 0.8f, 0.0f }),
			VfxModuleLibrary::CreateScaleOverLifeModule(0.22f, 0.06f),
			VfxModuleLibrary::CreateColorOverLifeModule(glm::vec4(0.4f, 0.9f, 1.0f, 1.0f), glm::vec4(0.8f, 0.3f, 1.0f, 0.0f)),
			VfxModuleLibrary::CreateSolveVelocityModule(),
			VfxModuleLibrary::CreateSpriteRendererModule(true, false, 3.5f)
		};
		shieldAsset.Emitters.push_back(em2);

		// Emitter 3: ShieldBarrierPulse
		ParticleEmitterLayer em3;
		em3.Name = "ShieldBarrierPulse";
		em3.MaxParticles = 80;
		em3.Lifetime = 1.8f;
		em3.EmissionRate = 20.0f;
		em3.FacingMode = ParticleRenderMode::Billboard;
		em3.Material.TexturePath = "textures/particles/flame_soft.wtex";
		em3.Material.MaterialPath = "materials/particles/particle_additive.wmat";
		em3.Material.BlendMode = ParticleBlendMode::Additive;
		em3.Material.EmissiveBoost = 2.0f;
		em3.Modules = {
			VfxModuleLibrary::CreateSpawnRateModule(20.0f),
			VfxModuleLibrary::CreateInitParticleModule(1.8f, 0.5f, 1.8f),
			VfxModuleLibrary::CreateSphereSpawnModule(),
			VfxModuleLibrary::CreateScaleOverLifeModule(0.5f, 1.8f),
			VfxModuleLibrary::CreateColorOverLifeModule(glm::vec4(0.5f, 0.2f, 1.0f, 0.6f), glm::vec4(0.2f, 0.7f, 1.0f, 0.0f)),
			VfxModuleLibrary::CreateSolveVelocityModule(),
			VfxModuleLibrary::CreateSpriteRendererModule(true, false, 2.0f)
		};
		shieldAsset.Emitters.push_back(em3);
	}

	// 2. FlameTornado3D
	ParticleAsset tornadoAsset;
	tornadoAsset.Name = "FlameTornado3D";
	tornadoAsset.Duration = 5.0f;
	tornadoAsset.Looping = true;
	{
		// Emitter 1: CoreFireVortex
		ParticleEmitterLayer em1;
		em1.Name = "CoreFireVortex";
		em1.MaxParticles = 450;
		em1.Lifetime = 1.5f;
		em1.EmissionRate = 85.0f;
		em1.Material.TexturePath = "textures/particles/flame_soft.wtex";
		em1.Material.MaterialPath = "materials/particles/particle_additive.wmat";
		em1.Material.BlendMode = ParticleBlendMode::Additive;
		em1.Material.EmissiveBoost = 3.2f;
		em1.Modules = {
			VfxModuleLibrary::CreateSpawnRateModule(85.0f),
			VfxModuleLibrary::CreateInitParticleModule(1.5f, 0.45f, 0.1f),
			VfxModuleLibrary::CreateConeSpawnModule(),
			VfxModuleLibrary::CreateGravityModule(),
			VfxModuleLibrary::CreateVortexModule(),
			VfxModuleLibrary::CreateCurlNoiseModule(),
			VfxModuleLibrary::CreateLinearDragModule(),
			VfxModuleLibrary::CreateScaleOverLifeModule(0.45f, 0.1f),
			VfxModuleLibrary::CreateColorOverLifeModule(glm::vec4(1.0f, 0.75f, 0.2f, 1.0f), glm::vec4(0.9f, 0.1f, 0.0f, 0.0f)),
			VfxModuleLibrary::CreateSolveVelocityModule(),
			VfxModuleLibrary::CreateSpriteRendererModule(true, false, 3.2f)
		};
		if (auto* g = em1.FindModule("mod.update.gravity")) g->SetVec4("Gravity", glm::vec4(0.0f, 4.5f, 0.0f, 0.0f));
		if (auto* v = em1.FindModule("mod.update.vortex")) v->SetFloat("VortexSpeed", 8.0f);
		if (auto* n = em1.FindModule("mod.update.noise")) n->SetFloat("NoiseGain", 4.5f);
		if (auto* d = em1.FindModule("mod.update.drag")) d->SetFloat("DragCoefficient", 0.35f);
		tornadoAsset.Emitters.push_back(em1);

		// Emitter 2: SwirlingSparks
		ParticleEmitterLayer em2;
		em2.Name = "SwirlingSparks";
		em2.MaxParticles = 300;
		em2.Lifetime = 1.2f;
		em2.EmissionRate = 50.0f;
		em2.FacingMode = ParticleRenderMode::StretchedVelocity;
		em2.Material.TexturePath = "textures/particles/spark_glow.wtex";
		em2.Material.MaterialPath = "materials/particles/particle_additive.wmat";
		em2.Material.BlendMode = ParticleBlendMode::Additive;
		em2.Material.EmissiveBoost = 4.5f;
		em2.Modules = {
			VfxModuleLibrary::CreateSpawnRateModule(50.0f),
			VfxModuleLibrary::CreateInitParticleModule(1.2f, 0.12f, 0.02f),
			VfxModuleLibrary::CreateConeSpawnModule(),
			VfxModuleLibrary::CreateGravityModule(),
			VfxModuleLibrary::CreateVortexModule(),
			VfxModuleLibrary::CreateCurlNoiseModule(),
			VfxModuleLibrary::CreateLinearDragModule(),
			VfxModuleLibrary::CreateScaleOverLifeModule(0.12f, 0.02f),
			VfxModuleLibrary::CreateSolveVelocityModule(),
			VfxModuleLibrary::CreateSpriteRendererModule(true, false, 4.5f)
		};
		if (auto* g = em2.FindModule("mod.update.gravity")) g->SetVec4("Gravity", glm::vec4(0.0f, 5.0f, 0.0f, 0.0f));
		if (auto* v = em2.FindModule("mod.update.vortex")) v->SetFloat("VortexSpeed", 12.0f);
		if (auto* n = em2.FindModule("mod.update.noise")) n->SetFloat("NoiseGain", 6.0f);
		if (auto* r = em2.FindModule("mod.render.sprite")) r->SetFloat("FacingMode", static_cast<float>(ParticleRenderMode::StretchedVelocity));
		tornadoAsset.Emitters.push_back(em2);

		// Emitter 3: RisingSmokePillar
		ParticleEmitterLayer em3;
		em3.Name = "RisingSmokePillar";
		em3.MaxParticles = 200;
		em3.Lifetime = 3.0f;
		em3.EmissionRate = 25.0f;
		em3.Material.TexturePath = "textures/particles/smoke_puff.wtex";
		em3.Material.MaterialPath = "materials/particles/particle_alpha.wmat";
		em3.Material.BlendMode = ParticleBlendMode::AlphaBlend;
		em3.Material.EmissiveBoost = 1.0f;
		em3.Modules = {
			VfxModuleLibrary::CreateSpawnRateModule(25.0f),
			VfxModuleLibrary::CreateInitParticleModule(3.0f, 0.4f, 2.2f),
			VfxModuleLibrary::CreateConeSpawnModule(),
			VfxModuleLibrary::CreateGravityModule(),
			VfxModuleLibrary::CreateVortexModule(),
			VfxModuleLibrary::CreateLinearDragModule(),
			VfxModuleLibrary::CreateScaleOverLifeModule(0.4f, 2.2f),
			VfxModuleLibrary::CreateColorOverLifeModule(glm::vec4(0.25f, 0.25f, 0.25f, 0.6f), glm::vec4(0.08f, 0.08f, 0.08f, 0.0f)),
			VfxModuleLibrary::CreateSolveVelocityModule(),
			VfxModuleLibrary::CreateSpriteRendererModule(false, false, 1.0f)
		};
		if (auto* g = em3.FindModule("mod.update.gravity")) g->SetVec4("Gravity", glm::vec4(0.0f, 2.5f, 0.0f, 0.0f));
		if (auto* v = em3.FindModule("mod.update.vortex")) v->SetFloat("VortexSpeed", 3.0f);
		if (auto* d = em3.FindModule("mod.update.drag")) d->SetFloat("DragCoefficient", 0.6f);
		tornadoAsset.Emitters.push_back(em3);
	}

	// 3. MeteorImpactExplosion3D
	ParticleAsset meteorAsset;
	meteorAsset.Name = "MeteorImpactExplosion3D";
	meteorAsset.Duration = 2.5f;
	meteorAsset.Looping = false;
	{
		// Emitter 1: BlastShockwave
		ParticleEmitterLayer em1;
		em1.Name = "BlastShockwave";
		em1.MaxParticles = 1;
		em1.Lifetime = 0.75f;
		em1.EmissionRate = 0.0f;
		em1.FacingMode = ParticleRenderMode::HorizontalQuad;
		em1.Bursts = { { 0.0f, 1u } };
		em1.Material.TexturePath = "textures/particles/shockwave_ring.wtex";
		em1.Material.MaterialPath = "materials/particles/particle_additive.wmat";
		em1.Material.BlendMode = ParticleBlendMode::Additive;
		em1.Material.EmissiveBoost = 3.5f;
		em1.Modules = {
			VfxModuleLibrary::CreateSpawnBurstModule(1, 0.0f),
			VfxModuleLibrary::CreateInitParticleModule(0.75f, 0.6f, 7.5f),
			VfxModuleLibrary::CreatePointSpawnModule(),
			VfxModuleLibrary::CreateScaleOverLifeModule(0.6f, 7.5f),
			VfxModuleLibrary::CreateColorOverLifeModule(glm::vec4(1.0f, 0.85f, 0.4f, 1.0f), glm::vec4(0.9f, 0.2f, 0.0f, 0.0f)),
			VfxModuleLibrary::CreateSolveVelocityModule(),
			VfxModuleLibrary::CreateSpriteRendererModule(true, true, 3.5f)
		};
		meteorAsset.Emitters.push_back(em1);

		// Emitter 2: CoreFireball
		ParticleEmitterLayer em2;
		em2.Name = "CoreFireball";
		em2.MaxParticles = 60;
		em2.Lifetime = 1.0f;
		em2.EmissionRate = 0.0f;
		em2.Bursts = { { 0.0f, 45u } };
		em2.Material.TexturePath = "textures/particles/flame_soft.wtex";
		em2.Material.MaterialPath = "materials/particles/particle_additive.wmat";
		em2.Material.BlendMode = ParticleBlendMode::Additive;
		em2.Material.EmissiveBoost = 4.0f;
		em2.Modules = {
			VfxModuleLibrary::CreateSpawnBurstModule(45, 0.0f),
			VfxModuleLibrary::CreateInitParticleModule(1.0f, 0.5f, 2.5f),
			VfxModuleLibrary::CreateSphereSpawnModule(),
			VfxModuleLibrary::CreateLinearDragModule(),
			VfxModuleLibrary::CreateScaleOverLifeModule(0.5f, 2.5f),
			VfxModuleLibrary::CreateColorOverLifeModule(glm::vec4(1.0f, 0.9f, 0.5f, 1.0f), glm::vec4(0.8f, 0.1f, 0.0f, 0.0f)),
			VfxModuleLibrary::CreateSolveVelocityModule(),
			VfxModuleLibrary::CreateSpriteRendererModule(true, false, 4.0f)
		};
		if (auto* s = em2.FindModule("mod.spawn.sphere")) { s->SetFloat("SphereRadius", 0.3f); s->SetFloat("OutwardSpeed", 12.0f); }
		if (auto* d = em2.FindModule("mod.update.drag")) d->SetFloat("DragCoefficient", 1.8f);
		meteorAsset.Emitters.push_back(em2);

		// Emitter 3: FlyingSparksAndDebris
		ParticleEmitterLayer em3;
		em3.Name = "FlyingSparksAndDebris";
		em3.MaxParticles = 80;
		em3.Lifetime = 1.3f;
		em3.EmissionRate = 0.0f;
		em3.FacingMode = ParticleRenderMode::StretchedVelocity;
		em3.Bursts = { { 0.0f, 65u } };
		em3.Material.TexturePath = "textures/particles/spark_glow.wtex";
		em3.Material.MaterialPath = "materials/particles/particle_additive.wmat";
		em3.Material.BlendMode = ParticleBlendMode::Additive;
		em3.Material.EmissiveBoost = 4.5f;
		em3.Modules = {
			VfxModuleLibrary::CreateSpawnBurstModule(65, 0.0f),
			VfxModuleLibrary::CreateInitParticleModule(1.3f, 0.15f, 0.02f),
			VfxModuleLibrary::CreateSphereSpawnModule(),
			VfxModuleLibrary::CreateGravityModule(),
			VfxModuleLibrary::CreateLinearDragModule(),
			VfxModuleLibrary::CreateScaleOverLifeModule(0.15f, 0.02f),
			VfxModuleLibrary::CreateColorOverLifeModule(glm::vec4(1.0f, 0.9f, 0.4f, 1.0f), glm::vec4(1.0f, 0.2f, 0.0f, 0.0f)),
			VfxModuleLibrary::CreateSolveVelocityModule(),
			VfxModuleLibrary::CreateSpriteRendererModule(true, false, 4.5f)
		};
		if (auto* s = em3.FindModule("mod.spawn.sphere")) { s->SetFloat("SphereRadius", 0.2f); s->SetFloat("OutwardSpeed", 15.0f); }
		if (auto* g = em3.FindModule("mod.update.gravity")) g->SetVec4("Gravity", glm::vec4(0.0f, -14.0f, 0.0f, 0.0f));
		if (auto* d = em3.FindModule("mod.update.drag")) d->SetFloat("DragCoefficient", 0.2f);
		if (auto* r = em3.FindModule("mod.render.sprite")) r->SetFloat("FacingMode", static_cast<float>(ParticleRenderMode::StretchedVelocity));
		meteorAsset.Emitters.push_back(em3);

		// Emitter 4: ExplosionSmoke
		ParticleEmitterLayer em4;
		em4.Name = "ExplosionSmoke";
		em4.MaxParticles = 40;
		em4.Lifetime = 3.5f;
		em4.EmissionRate = 0.0f;
		em4.Bursts = { { 0.05f, 30u } };
		em4.Material.TexturePath = "textures/particles/smoke_puff.wtex";
		em4.Material.MaterialPath = "materials/particles/particle_alpha.wmat";
		em4.Material.BlendMode = ParticleBlendMode::AlphaBlend;
		em4.Material.EmissiveBoost = 1.0f;
		em4.Modules = {
			VfxModuleLibrary::CreateSpawnBurstModule(30, 0.05f),
			VfxModuleLibrary::CreateInitParticleModule(3.5f, 0.4f, 2.8f),
			VfxModuleLibrary::CreateConeSpawnModule(),
			VfxModuleLibrary::CreateGravityModule(),
			VfxModuleLibrary::CreateCurlNoiseModule(),
			VfxModuleLibrary::CreateLinearDragModule(),
			VfxModuleLibrary::CreateScaleOverLifeModule(0.4f, 2.8f),
			VfxModuleLibrary::CreateColorOverLifeModule(glm::vec4(0.3f, 0.3f, 0.3f, 0.75f), glm::vec4(0.1f, 0.1f, 0.1f, 0.0f)),
			VfxModuleLibrary::CreateSolveVelocityModule(),
			VfxModuleLibrary::CreateSpriteRendererModule(false, false, 1.0f)
		};
		if (auto* g = em4.FindModule("mod.update.gravity")) g->SetVec4("Gravity", glm::vec4(0.0f, 1.8f, 0.0f, 0.0f));
		if (auto* n = em4.FindModule("mod.update.noise")) n->SetFloat("NoiseGain", 3.0f);
		if (auto* d = em4.FindModule("mod.update.drag")) d->SetFloat("DragCoefficient", 0.8f);
		meteorAsset.Emitters.push_back(em4);
	}

	// 4. LightningPlasmaBeam3D
	ParticleAsset beamAsset;
	beamAsset.Name = "LightningPlasmaBeam3D";
	beamAsset.Duration = 3.0f;
	beamAsset.Looping = true;
	{
		// Emitter 1: PlasmaBeamCore
		ParticleEmitterLayer em1;
		em1.Name = "PlasmaBeamCore";
		em1.MaxParticles = 150;
		em1.Lifetime = 0.5f;
		em1.EmissionRate = 140.0f;
		em1.FacingMode = ParticleRenderMode::StretchedVelocity;
		em1.Material.TexturePath = "textures/particles/electric_beam.wtex";
		em1.Material.MaterialPath = "materials/particles/particle_additive.wmat";
		em1.Material.BlendMode = ParticleBlendMode::Additive;
		em1.Material.EmissiveBoost = 4.5f;
		em1.Modules = {
			VfxModuleLibrary::CreateSpawnRateModule(140.0f),
			VfxModuleLibrary::CreateInitParticleModule(0.5f, 0.3f, 0.08f),
			VfxModuleLibrary::CreatePointSpawnModule(),
			VfxModuleLibrary::CreateSolveVelocityModule(),
			VfxModuleLibrary::CreateSpriteRendererModule(true, false, 4.5f)
		};
		if (auto* p = em1.FindModule("mod.spawn.point")) p->SetFloat("PointSpeed", 24.0f);
		if (auto* r = em1.FindModule("mod.render.sprite")) r->SetFloat("FacingMode", static_cast<float>(ParticleRenderMode::StretchedVelocity));
		beamAsset.Emitters.push_back(em1);

		// Emitter 2: SpiralElectricArcs
		ParticleEmitterLayer em2;
		em2.Name = "SpiralElectricArcs";
		em2.MaxParticles = 80;
		em2.Lifetime = 0.8f;
		em2.EmissionRate = 60.0f;
		em2.Material.TexturePath = "textures/particles/spark_glow.wtex";
		em2.Material.MaterialPath = "materials/particles/particle_additive.wmat";
		em2.Material.BlendMode = ParticleBlendMode::Additive;
		em2.Material.EmissiveBoost = 3.5f;
		em2.Modules = {
			VfxModuleLibrary::CreateSpawnRateModule(60.0f),
			VfxModuleLibrary::CreateInitParticleModule(0.8f, 0.15f, 0.03f),
			VfxModuleLibrary::CreateRingSpawnModule(0.45f, 0.0f),
			VfxModuleLibrary::CreateJitterModule(2.5f, 35.0f),
			VfxModuleLibrary::CreateVortexModule(),
			VfxModuleLibrary::CreateSolveVelocityModule(),
			VfxModuleLibrary::CreateSpriteRendererModule(true, false, 3.5f)
		};
		if (auto* v = em2.FindModule("mod.update.vortex")) v->SetFloat("VortexSpeed", 8.0f);
		beamAsset.Emitters.push_back(em2);

		// Emitter 3: ScatterImpactSparks
		ParticleEmitterLayer em3;
		em3.Name = "ScatterImpactSparks";
		em3.MaxParticles = 100;
		em3.Lifetime = 0.9f;
		em3.EmissionRate = 45.0f;
		em3.FacingMode = ParticleRenderMode::StretchedVelocity;
		em3.Material.TexturePath = "textures/particles/spark_glow.wtex";
		em3.Material.MaterialPath = "materials/particles/particle_additive.wmat";
		em3.Material.BlendMode = ParticleBlendMode::Additive;
		em3.Material.EmissiveBoost = 4.0f;
		em3.Modules = {
			VfxModuleLibrary::CreateSpawnRateModule(45.0f),
			VfxModuleLibrary::CreateInitParticleModule(0.9f, 0.1f, 0.01f),
			VfxModuleLibrary::CreateSphereSpawnModule(),
			VfxModuleLibrary::CreateGravityModule(),
			VfxModuleLibrary::CreateLinearDragModule(),
			VfxModuleLibrary::CreateSolveVelocityModule(),
			VfxModuleLibrary::CreateSpriteRendererModule(true, false, 4.0f)
		};
		if (auto* s = em3.FindModule("mod.spawn.sphere")) { s->SetFloat("SphereRadius", 0.1f); s->SetFloat("OutwardSpeed", 7.0f); }
		if (auto* g = em3.FindModule("mod.update.gravity")) g->SetVec4("Gravity", glm::vec4(0.0f, -9.8f, 0.0f, 0.0f));
		if (auto* d = em3.FindModule("mod.update.drag")) d->SetFloat("DragCoefficient", 0.2f);
		if (auto* r = em3.FindModule("mod.render.sprite")) r->SetFloat("FacingMode", static_cast<float>(ParticleRenderMode::StretchedVelocity));
		beamAsset.Emitters.push_back(em3);
	}

	// 5. BlizzardFrostNova3D
	ParticleAsset blizzardAsset;
	blizzardAsset.Name = "BlizzardFrostNova3D";
	blizzardAsset.Duration = 5.0f;
	blizzardAsset.Looping = true;
	{
		// Emitter 1: FrostBlastRing
		ParticleEmitterLayer em1;
		em1.Name = "FrostBlastRing";
		em1.MaxParticles = 1;
		em1.Lifetime = 1.2f;
		em1.EmissionRate = 0.0f;
		em1.FacingMode = ParticleRenderMode::HorizontalQuad;
		em1.Bursts = { { 0.0f, 1u } };
		em1.Material.TexturePath = "textures/particles/shockwave_ring.wtex";
		em1.Material.MaterialPath = "materials/particles/particle_additive.wmat";
		em1.Material.BlendMode = ParticleBlendMode::Additive;
		em1.Material.EmissiveBoost = 3.0f;
		em1.Modules = {
			VfxModuleLibrary::CreateSpawnBurstModule(1, 0.0f),
			VfxModuleLibrary::CreateInitParticleModule(1.2f, 0.5f, 6.0f),
			VfxModuleLibrary::CreatePointSpawnModule(),
			VfxModuleLibrary::CreateScaleOverLifeModule(0.5f, 6.0f),
			VfxModuleLibrary::CreateColorOverLifeModule(glm::vec4(0.4f, 0.85f, 1.0f, 1.0f), glm::vec4(0.1f, 0.3f, 0.8f, 0.0f)),
			VfxModuleLibrary::CreateSolveVelocityModule(),
			VfxModuleLibrary::CreateSpriteRendererModule(true, true, 3.0f)
		};
		blizzardAsset.Emitters.push_back(em1);

		// Emitter 2: IceCrystals
		ParticleEmitterLayer em2;
		em2.Name = "IceCrystals";
		em2.MaxParticles = 250;
		em2.Lifetime = 2.5f;
		em2.EmissionRate = 80.0f;
		em2.Material.TexturePath = "textures/particles/spark_glow.wtex";
		em2.Material.MaterialPath = "materials/particles/particle_additive.wmat";
		em2.Material.BlendMode = ParticleBlendMode::Additive;
		em2.Material.EmissiveBoost = 2.2f;
		em2.Modules = {
			VfxModuleLibrary::CreateSpawnRateModule(80.0f),
			VfxModuleLibrary::CreateInitParticleModule(2.5f, 0.12f, 0.03f),
			VfxModuleLibrary::CreateBoxSpawnModule(),
			VfxModuleLibrary::CreateGravityModule(),
			VfxModuleLibrary::CreateVortexModule(),
			VfxModuleLibrary::CreateCurlNoiseModule(),
			VfxModuleLibrary::CreateLinearDragModule(),
			VfxModuleLibrary::CreateSolveVelocityModule(),
			VfxModuleLibrary::CreateSpriteRendererModule(true, false, 2.2f)
		};
		if (auto* g = em2.FindModule("mod.update.gravity")) g->SetVec4("Gravity", glm::vec4(0.0f, -1.8f, 0.0f, 0.0f));
		if (auto* v = em2.FindModule("mod.update.vortex")) v->SetFloat("VortexSpeed", 3.0f);
		if (auto* n = em2.FindModule("mod.update.noise")) n->SetFloat("NoiseGain", 2.5f);
		if (auto* d = em2.FindModule("mod.update.drag")) d->SetFloat("DragCoefficient", 0.3f);
		blizzardAsset.Emitters.push_back(em2);

		// Emitter 3: SubZeroFog
		ParticleEmitterLayer em3;
		em3.Name = "SubZeroFog";
		em3.MaxParticles = 120;
		em3.Lifetime = 3.0f;
		em3.EmissionRate = 30.0f;
		em3.Material.TexturePath = "textures/particles/smoke_puff.wtex";
		em3.Material.MaterialPath = "materials/particles/particle_alpha.wmat";
		em3.Material.BlendMode = ParticleBlendMode::AlphaBlend;
		em3.Material.EmissiveBoost = 1.0f;
		em3.Modules = {
			VfxModuleLibrary::CreateSpawnRateModule(30.0f),
			VfxModuleLibrary::CreateInitParticleModule(3.0f, 0.6f, 2.2f),
			VfxModuleLibrary::CreateRingSpawnModule(2.2f, 0.0f),
			VfxModuleLibrary::CreateCurlNoiseModule(),
			VfxModuleLibrary::CreateScaleOverLifeModule(0.6f, 2.2f),
			VfxModuleLibrary::CreateColorOverLifeModule(glm::vec4(0.4f, 0.7f, 0.95f, 0.5f), glm::vec4(0.2f, 0.5f, 0.9f, 0.0f)),
			VfxModuleLibrary::CreateSolveVelocityModule(),
			VfxModuleLibrary::CreateSpriteRendererModule(false, false, 1.0f)
		};
		if (auto* n = em3.FindModule("mod.update.noise")) n->SetFloat("NoiseGain", 1.5f);
		blizzardAsset.Emitters.push_back(em3);
	}

	// 6. Campfire
	ParticleAsset campfireAsset;
	campfireAsset.Name = "Campfire";
	campfireAsset.Duration = 4.0f;
	campfireAsset.Looping = true;
	{
		// Emitter 1: CampfireFlame
		ParticleEmitterLayer em1;
		em1.Name = "CampfireFlame";
		em1.MaxParticles = 200;
		em1.Lifetime = 1.2f;
		em1.EmissionRate = 50.0f;
		em1.Material.TexturePath = "textures/particles/flame_soft.wtex";
		em1.Material.MaterialPath = "materials/particles/particle_additive.wmat";
		em1.Material.BlendMode = ParticleBlendMode::Additive;
		em1.Material.EmissiveBoost = 3.0f;
		em1.Modules = {
			VfxModuleLibrary::CreateSpawnRateModule(50.0f),
			VfxModuleLibrary::CreateInitParticleModule(1.2f, 0.45f, 0.08f),
			VfxModuleLibrary::CreateConeSpawnModule(),
			VfxModuleLibrary::CreateGravityModule(),
			VfxModuleLibrary::CreateCurlNoiseModule(),
			VfxModuleLibrary::CreateLinearDragModule(),
			VfxModuleLibrary::CreateScaleOverLifeModule(0.45f, 0.08f),
			VfxModuleLibrary::CreateColorOverLifeModule(glm::vec4(1.0f, 0.7f, 0.15f, 1.0f), glm::vec4(0.9f, 0.1f, 0.0f, 0.0f)),
			VfxModuleLibrary::CreateSolveVelocityModule(),
			VfxModuleLibrary::CreateSpriteRendererModule(true, false, 3.0f)
		};
		if (auto* c = em1.FindModule("mod.spawn.cone")) { c->SetFloat("ConeAngle", 14.0f); c->SetFloat("ConeRadius", 0.18f); c->SetFloat("InitialSpeed", 2.5f); }
		if (auto* g = em1.FindModule("mod.update.gravity")) g->SetVec4("Gravity", glm::vec4(0.0f, 3.0f, 0.0f, 0.0f));
		if (auto* n = em1.FindModule("mod.update.noise")) n->SetFloat("NoiseGain", 2.5f);
		if (auto* d = em1.FindModule("mod.update.drag")) d->SetFloat("DragCoefficient", 0.3f);
		campfireAsset.Emitters.push_back(em1);

		// Emitter 2: RisingSmoke
		ParticleEmitterLayer em2;
		em2.Name = "RisingSmoke";
		em2.MaxParticles = 100;
		em2.Lifetime = 3.5f;
		em2.EmissionRate = 15.0f;
		em2.Material.TexturePath = "textures/particles/smoke_puff.wtex";
		em2.Material.MaterialPath = "materials/particles/particle_alpha.wmat";
		em2.Material.BlendMode = ParticleBlendMode::AlphaBlend;
		em2.Material.EmissiveBoost = 0.8f;
		em2.Modules = {
			VfxModuleLibrary::CreateSpawnRateModule(15.0f),
			VfxModuleLibrary::CreateInitParticleModule(3.5f, 0.3f, 1.8f),
			VfxModuleLibrary::CreateConeSpawnModule(),
			VfxModuleLibrary::CreateGravityModule(),
			VfxModuleLibrary::CreateCurlNoiseModule(),
			VfxModuleLibrary::CreateLinearDragModule(),
			VfxModuleLibrary::CreateScaleOverLifeModule(0.3f, 1.8f),
			VfxModuleLibrary::CreateColorOverLifeModule(glm::vec4(0.35f, 0.35f, 0.35f, 0.6f), glm::vec4(0.1f, 0.1f, 0.1f, 0.0f)),
			VfxModuleLibrary::CreateSolveVelocityModule(),
			VfxModuleLibrary::CreateSpriteRendererModule(false, false, 0.8f)
		};
		if (auto* c = em2.FindModule("mod.spawn.cone")) { c->SetFloat("ConeAngle", 20.0f); c->SetFloat("ConeRadius", 0.25f); c->SetFloat("InitialSpeed", 1.8f); }
		if (auto* g = em2.FindModule("mod.update.gravity")) g->SetVec4("Gravity", glm::vec4(0.0f, 1.8f, 0.0f, 0.0f));
		if (auto* n = em2.FindModule("mod.update.noise")) n->SetFloat("NoiseGain", 2.0f);
		if (auto* d = em2.FindModule("mod.update.drag")) d->SetFloat("DragCoefficient", 0.55f);
		campfireAsset.Emitters.push_back(em2);

		// Emitter 3: FloatingEmbers
		ParticleEmitterLayer em3;
		em3.Name = "FloatingEmbers";
		em3.MaxParticles = 80;
		em3.Lifetime = 2.0f;
		em3.EmissionRate = 12.0f;
		em3.Material.TexturePath = "textures/particles/spark_glow.wtex";
		em3.Material.MaterialPath = "materials/particles/particle_additive.wmat";
		em3.Material.BlendMode = ParticleBlendMode::Additive;
		em3.Material.EmissiveBoost = 4.0f;
		em3.Modules = {
			VfxModuleLibrary::CreateSpawnRateModule(12.0f),
			VfxModuleLibrary::CreateInitParticleModule(2.0f, 0.08f, 0.01f),
			VfxModuleLibrary::CreateConeSpawnModule(),
			VfxModuleLibrary::CreateGravityModule(),
			VfxModuleLibrary::CreateCurlNoiseModule(),
			VfxModuleLibrary::CreateLinearDragModule(),
			VfxModuleLibrary::CreateScaleOverLifeModule(0.08f, 0.01f),
			VfxModuleLibrary::CreateColorOverLifeModule(glm::vec4(1.0f, 0.85f, 0.3f, 1.0f), glm::vec4(0.8f, 0.2f, 0.0f, 0.0f)),
			VfxModuleLibrary::CreateSolveVelocityModule(),
			VfxModuleLibrary::CreateSpriteRendererModule(true, false, 4.0f)
		};
		if (auto* c = em3.FindModule("mod.spawn.cone")) { c->SetFloat("ConeAngle", 22.0f); c->SetFloat("ConeRadius", 0.15f); c->SetFloat("InitialSpeed", 3.2f); }
		if (auto* g = em3.FindModule("mod.update.gravity")) g->SetVec4("Gravity", glm::vec4(0.0f, 1.2f, 0.0f, 0.0f));
		if (auto* n = em3.FindModule("mod.update.noise")) n->SetFloat("NoiseGain", 3.5f);
		if (auto* d = em3.FindModule("mod.update.drag")) d->SetFloat("DragCoefficient", 0.25f);
		campfireAsset.Emitters.push_back(em3);
	}

	const std::pair<const char*, const ParticleAsset*> allAssets[] = {
		{ "orbital_magic_shield_3d.wvfx", &shieldAsset },
		{ "flame_tornado_3d.wvfx", &tornadoAsset },
		{ "meteor_impact_explosion_3d.wvfx", &meteorAsset },
		{ "lightning_plasma_beam_3d.wvfx", &beamAsset },
		{ "blizzard_frost_nova_3d.wvfx", &blizzardAsset },
		{ "campfire.wvfx", &campfireAsset }
	};

	std::error_code ec;
	std::filesystem::create_directories("Engine/content/vfx", ec);
	std::filesystem::create_directories("tests/fixtures/content/vfx", ec);

	for (const auto& [filename, pAsset] : allAssets)
	{
		std::string enginePath = std::string("Engine/content/vfx/") + filename;
		std::string fixturePath = std::string("tests/fixtures/content/vfx/") + filename;

		bool ok1 = ParticleAssetSerializer::Serialize(*pAsset, enginePath);
		assert(ok1 && "Failed to serialize to engine content vfx");

		bool ok2 = ParticleAssetSerializer::Serialize(*pAsset, fixturePath);
		assert(ok2 && "Failed to serialize to tests fixture vfx");

		// 反序列化并断言每个发射器都完整持有模块堆栈
		ParticleAsset loaded;
		bool ok3 = ParticleAssetSerializer::Deserialize(enginePath, loaded);
		assert(ok3 && "Failed to deserialize newly generated VFX asset");
		assert(loaded.Name == pAsset->Name);
		assert(loaded.Emitters.size() == pAsset->Emitters.size());

		for (size_t i = 0; i < loaded.Emitters.size(); ++i)
		{
			const auto& em = loaded.Emitters[i];
			assert(!em.Modules.empty() && "Commercial VFX preset emitter must contain non-empty Niagara modules!");
			assert(em.Modules.size() >= 4 && "Each emitter in showcase VFX must contain full modular pipeline!");
		}
	}

	std::cout << "[PASS] TestParticleAssetShowcasePresets3D\n";
}


static void TestModularPipelineSlangGeneration()
{
	std::vector<VfxModule> modules = VfxModuleLibrary::CreateDefaultEmitterModuleStack();
	assert(!modules.empty());
	assert(modules.size() >= 4);

	std::unordered_map<std::string, float> userFloats;
	userFloats["ChaosGain"] = 2.5f;
	std::unordered_map<std::string, glm::vec4> userVecs;
	userVecs["BlackHolePos"] = glm::vec4(0.0f, 5.0f, 0.0f, 0.0f);

	std::string slangCode = VfxComputeGenerator::GenerateComputeShaderSource("VortexCore", modules, userFloats, userVecs);
	assert(!slangCode.empty());
	assert(slangCode.find("void CS_Spawn") != std::string::npos);
	assert(slangCode.find("void CS_Update") != std::string::npos);
	assert(slangCode.find("u_ChaosGain") != std::string::npos);
	assert(slangCode.find("u_BlackHolePos") != std::string::npos);
	assert(slangCode.find("CurlNoise3D") != std::string::npos);

	std::cout << "[PASS] TestModularPipelineSlangGeneration\n";
}

static void TestModularPipelineCustomSlangModule()
{
	ParticleAsset asset;
	asset.Name = "CustomLorenzVfx";
	asset.Guid = 88887777ULL;

	ParticleEmitterLayer layer;
	layer.Name = "LorenzChaosEmitter";

	VfxModule customMod = VfxModuleLibrary::CreateCustomSlangModule("LorenzAttractor");
	customMod.SlangSource = R"(
    float3 lorenz = float3(
        10.0f * (p.Position.y - p.Position.x),
        p.Position.x * (28.0f - p.Position.z) - p.Position.y,
        p.Position.x * p.Position.y - 2.666f * p.Position.z
    );
    p.Velocity += lorenz * (u_ChaosGain * dt);
)";
	customMod.Parameters.push_back({ "ChaosGain", VfxParamType::Float, 3.2f, {}, 0, false, 0.0f, 10.0f, 0.1f, "混沌增益", true, "ChaosGain" });
	layer.Modules.push_back(customMod);

	asset.Emitters.push_back(layer);

	std::string yamlStr = ParticleAssetSerializer::SerializeToString(asset);
	assert(!yamlStr.empty());
	assert(yamlStr.find("LorenzAttractor") != std::string::npos);
	assert(yamlStr.find("ChaosGain") != std::string::npos);

	ParticleAsset deserialized;
	bool ok = ParticleAssetSerializer::DeserializeFromString(yamlStr, deserialized);
	assert(ok);
	assert(deserialized.Emitters.size() == 1);
	assert(deserialized.Emitters[0].Modules.size() == 1);
	assert(deserialized.Emitters[0].Modules[0].Name == "LorenzAttractor");
	assert(deserialized.Emitters[0].Modules[0].IsCustom == true);
	assert(deserialized.Emitters[0].Modules[0].SlangSource.find("lorenz") != std::string::npos);

	std::cout << "[PASS] TestModularPipelineCustomSlangModule\n";
}

static void TestProjectGlobalCustomModuleRoundtrip()
{
	std::cout << "Testing Project-Global Custom Slang Module Serialization and Library...\n";

	VfxModule customMod = VfxModuleLibrary::CreateCustomSlangModule("SpiralVortexTest");
	customMod.Stage = VfxModuleStage::Update;
	customMod.Parameters.push_back({ "SwirlRate", VfxParamType::Float, 5.5f, {}, 0, false, 0.0f, 20.0f, 0.1f, "旋涡率", true, "SwirlRate" });
	customMod.SlangSource = "p.Velocity += cross(float3(0,1,0), p.Position) * (u_SwirlRate * dt);\n";

	// 1. YAML 单体序列化与反序列化
	std::string yaml = customMod.SerializeToYaml();
	assert(!yaml.empty());
	assert(yaml.find("SpiralVortexTest") != std::string::npos);
	assert(yaml.find("SwirlRate") != std::string::npos);

	VfxModule deserialized;
	bool ok = VfxModule::DeserializeFromYaml(yaml, deserialized);
	assert(ok);
	assert(deserialized.Name == "SpiralVortexTest");
	assert(deserialized.IsCustom == true);
	assert(deserialized.Stage == VfxModuleStage::Update);
	assert(deserialized.Parameters.size() == 2); // default + new

	// 2. 项目全局磁盘存储与管理
	bool saved = VfxModuleLibrary::SaveProjectCustomModule(customMod);
	assert(saved);

	auto loadedList = VfxModuleLibrary::LoadProjectCustomModules();
	bool found = false;
	for (const auto& m : loadedList)
	{
		if (m.Name == "SpiralVortexTest")
		{
			found = true;
			assert(m.IsCustom == true);
			assert(m.SlangSource.find("SwirlRate") != std::string::npos);
			break;
		}
	}
	assert(found);

	// 3. 清理测试文件
	bool deleted = VfxModuleLibrary::DeleteProjectCustomModule("SpiralVortexTest");
	assert(deleted);

	std::cout << "[PASS] TestProjectGlobalCustomModuleRoundtrip\n";
}

int main()
{
	Log::Init();
	std::cout << "Running WorldParticleAssetTests...\n";

	TestParticleAssetSerializationRoundtrip();
	TestParticleAssetBackwardCompatibilitySingleEmitter();
	TestParticleAssetRegistry();
	TestParticleAssetNewDynamicsAndBursts();
	TestModularPipelineSlangGeneration();
	TestModularPipelineCustomSlangModule();
	TestProjectGlobalCustomModuleRoundtrip();
	TestParticleAssetShowcasePresets3D();

	std::cout << "All WorldParticleAssetTests passed successfully.\n";
	return 0;
}
