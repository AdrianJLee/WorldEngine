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

	// =========================================================================
	// OrbitalMagicShield3D
	// =========================================================================
	ParticleAsset shieldAsset;
	shieldAsset.Name = "OrbitalMagicShield3D";
	shieldAsset.Duration = 5.0f;
	shieldAsset.Looping = true;
	{
		// Emitter 1: RunicGroundMatrix
		{
			ParticleEmitterLayer em;
			em.Name = "RunicGroundMatrix";
			em.MaxParticles = 1;
			em.Lifetime = 9999.00f;
			em.EmissionRate = 0.00f;
			em.Speed = 0.00f;
			em.SpeedVariance = 0.00f;
			em.Gravity = glm::vec3(0.00f, 0.00f, 0.00f);
			em.LinearDrag = 0.00f;
			em.VortexStrength = 0.00f;
			em.EmitterType = ParticleEmitterType::Point;
			em.SizeStart = 2.200f;
			em.SizeEnd = 2.200f;
			em.ColorStart = glm::vec4(0.35f, 0.88f, 1.00f, 0.95f);
			em.ColorEnd = glm::vec4(0.35f, 0.88f, 1.00f, 0.95f);
			em.FacingMode = ParticleRenderMode::HorizontalQuad;
			em.Bursts = { { 0.00f, 1u } };
			em.Material.TexturePath = "textures/particles/magic_rune.wtex";
			em.Material.MaterialPath = "materials/particles/particle_additive.wmat";
			em.Material.BlendMode = ParticleBlendMode::Additive;
			em.Material.EmissiveBoost = 6.0f;
			em.Modules = {
				VfxModuleLibrary::CreateSpawnBurstModule(1, 0.0f),
				VfxModuleLibrary::CreateInitParticleModule(9999.0f, 2.2f, 2.2f),
				VfxModuleLibrary::CreatePointSpawnModule(),
				VfxModuleLibrary::CreateRotationOverLifeModule(0.15f, 0.0f),
				VfxModuleLibrary::CreateColorOverLifeModule(glm::vec4(0.35f, 0.88f, 1.0f, 0.95f), glm::vec4(0.35f, 0.88f, 1.0f, 0.95f)),
				VfxModuleLibrary::CreateSolveVelocityModule(),
				VfxModuleLibrary::CreateSpriteRendererModule(true, true, 4.0f)
			};
			if (auto* p = em.FindModule("mod.spawn.point")) p->SetFloat("PointSpeed", 0.0f);
			shieldAsset.Emitters.push_back(em);
		}

		// Emitter 2: EquatorialOrbitOrbs
		{
			ParticleEmitterLayer em;
			em.Name = "EquatorialOrbitOrbs";
			em.MaxParticles = 100;
			em.Lifetime = 2.50f;
			em.EmissionRate = 35.00f;
			em.Speed = 0.00f;
			em.SpeedVariance = 0.00f;
			em.Gravity = glm::vec3(0.00f, 0.00f, 0.00f);
			em.LinearDrag = 0.00f;
			em.VortexStrength = 0.00f;
			em.EmitterType = ParticleEmitterType::Circle;
			em.SizeStart = 0.280f;
			em.SizeEnd = 0.050f;
			em.ColorStart = glm::vec4(0.40f, 0.95f, 1.00f, 1.00f);
			em.ColorEnd = glm::vec4(0.85f, 0.35f, 1.00f, 0.00f);
			em.FacingMode = ParticleRenderMode::Billboard;
			em.Material.TexturePath = "textures/particles/spark_glow.wtex";
			em.Material.MaterialPath = "materials/particles/particle_additive.wmat";
			em.Material.BlendMode = ParticleBlendMode::Additive;
			em.Material.EmissiveBoost = 5.5f;
			em.Modules = {
				VfxModuleLibrary::CreateSpawnRateModule(35.0f),
				VfxModuleLibrary::CreateInitParticleModule(2.5f, 0.28f, 0.05f),
				VfxModuleLibrary::CreateRingSpawnModule(1.8f, 0.0f),
				VfxModuleLibrary::CreateOrbitModule(2.8f, { 0.0f, 1.0f, 0.0f }, { 0.0f, 0.6f, 0.0f }),
				VfxModuleLibrary::CreateScaleOverLifeModule(0.28f, 0.05f),
				VfxModuleLibrary::CreateColorOverLifeModule(glm::vec4(0.40f, 0.95f, 1.0f, 1.0f), glm::vec4(0.85f, 0.35f, 1.0f, 0.0f)),
				VfxModuleLibrary::CreateSolveVelocityModule(),
				VfxModuleLibrary::CreateSpriteRendererModule(true, false, 5.5f)
			};
			shieldAsset.Emitters.push_back(em);
		}

		// Emitter 3: TiltedOrbitOrbs
		{
			ParticleEmitterLayer em;
			em.Name = "TiltedOrbitOrbs";
			em.MaxParticles = 100;
			em.Lifetime = 2.50f;
			em.EmissionRate = 35.00f;
			em.Speed = 0.00f;
			em.SpeedVariance = 0.00f;
			em.Gravity = glm::vec3(0.00f, 0.00f, 0.00f);
			em.LinearDrag = 0.00f;
			em.VortexStrength = 0.00f;
			em.EmitterType = ParticleEmitterType::Circle;
			em.SizeStart = 0.260f;
			em.SizeEnd = 0.050f;
			em.ColorStart = glm::vec4(1.00f, 0.88f, 0.40f, 1.00f);
			em.ColorEnd = glm::vec4(0.95f, 0.20f, 0.60f, 0.00f);
			em.FacingMode = ParticleRenderMode::Billboard;
			em.Material.TexturePath = "textures/particles/spark_glow.wtex";
			em.Material.MaterialPath = "materials/particles/particle_additive.wmat";
			em.Material.BlendMode = ParticleBlendMode::Additive;
			em.Material.EmissiveBoost = 5.2f;
			em.Modules = {
				VfxModuleLibrary::CreateSpawnRateModule(35.0f),
				VfxModuleLibrary::CreateInitParticleModule(2.5f, 0.26f, 0.05f),
				VfxModuleLibrary::CreateRingSpawnModule(1.8f, 0.0f),
				VfxModuleLibrary::CreateOrbitModule(-2.4f, glm::normalize(glm::vec3(0.8f, 0.6f, 0.2f)), { 0.0f, 0.6f, 0.0f }),
				VfxModuleLibrary::CreateScaleOverLifeModule(0.26f, 0.05f),
				VfxModuleLibrary::CreateColorOverLifeModule(glm::vec4(1.0f, 0.88f, 0.40f, 1.0f), glm::vec4(0.95f, 0.20f, 0.60f, 0.0f)),
				VfxModuleLibrary::CreateSolveVelocityModule(),
				VfxModuleLibrary::CreateSpriteRendererModule(true, false, 5.2f)
			};
			shieldAsset.Emitters.push_back(em);
		}

		// Emitter 4: ShieldBarrierBreathing
		{
			ParticleEmitterLayer em;
			em.Name = "ShieldBarrierBreathing";
			em.MaxParticles = 60;
			em.Lifetime = 2.00f;
			em.EmissionRate = 20.00f;
			em.Speed = 0.15f;
			em.SpeedVariance = 0.00f;
			em.Gravity = glm::vec3(0.00f, 0.00f, 0.00f);
			em.LinearDrag = 0.50f;
			em.VortexStrength = 0.00f;
			em.EmitterType = ParticleEmitterType::Sphere;
			em.SizeStart = 1.200f;
			em.SizeEnd = 2.400f;
			em.ColorStart = glm::vec4(0.25f, 0.65f, 1.00f, 0.35f);
			em.ColorEnd = glm::vec4(0.10f, 0.25f, 0.85f, 0.00f);
			em.FacingMode = ParticleRenderMode::Billboard;
			em.Material.TexturePath = "textures/particles/flame_soft.wtex";
			em.Material.MaterialPath = "materials/particles/particle_additive.wmat";
			em.Material.BlendMode = ParticleBlendMode::Additive;
			em.Material.EmissiveBoost = 2.4f;
			em.Modules = {
				VfxModuleLibrary::CreateSpawnRateModule(20.0f),
				VfxModuleLibrary::CreateInitParticleModule(2.0f, 1.2f, 2.4f),
				VfxModuleLibrary::CreateSphereSpawnModule(),
				VfxModuleLibrary::CreateLinearDragModule(),
				VfxModuleLibrary::CreateScaleOverLifeModule(1.2f, 2.4f),
				VfxModuleLibrary::CreateColorOverLifeModule(glm::vec4(0.25f, 0.65f, 1.0f, 0.35f), glm::vec4(0.10f, 0.25f, 0.85f, 0.0f)),
				VfxModuleLibrary::CreateSolveVelocityModule(),
				VfxModuleLibrary::CreateSpriteRendererModule(true, false, 2.4f)
			};
			if (auto* s = em.FindModule("mod.spawn.sphere")) { s->SetFloat("SphereRadius", 1.4f); s->SetFloat("OutwardSpeed", 0.15f); }
			if (auto* d = em.FindModule("mod.update.drag")) d->SetFloat("LinearDrag", 0.5f);
			shieldAsset.Emitters.push_back(em);
		}

		// Emitter 5: AscendingArcaneMotes
		{
			ParticleEmitterLayer em;
			em.Name = "AscendingArcaneMotes";
			em.MaxParticles = 70;
			em.Lifetime = 2.40f;
			em.EmissionRate = 24.00f;
			em.Speed = 0.40f;
			em.SpeedVariance = 0.00f;
			em.Gravity = glm::vec3(0.00f, 0.60f, 0.00f);
			em.LinearDrag = 0.60f;
			em.VortexStrength = 0.00f;
			em.EmitterType = ParticleEmitterType::Circle;
			em.SizeStart = 0.070f;
			em.SizeEnd = 0.015f;
			em.ColorStart = glm::vec4(0.75f, 0.95f, 1.00f, 1.00f);
			em.ColorEnd = glm::vec4(0.95f, 0.80f, 0.30f, 0.00f);
			em.FacingMode = ParticleRenderMode::Billboard;
			em.Material.TexturePath = "textures/particles/spark_glow.wtex";
			em.Material.MaterialPath = "materials/particles/particle_additive.wmat";
			em.Material.BlendMode = ParticleBlendMode::Additive;
			em.Material.EmissiveBoost = 4.5f;
			em.Modules = {
				VfxModuleLibrary::CreateSpawnRateModule(32.0f),
				VfxModuleLibrary::CreateInitParticleModule(2.4f, 0.07f, 0.015f),
				VfxModuleLibrary::CreateRingSpawnModule(1.2f, 0.2f),
				VfxModuleLibrary::CreateGravityModule(),
				VfxModuleLibrary::CreateCurlNoiseModule(),
				VfxModuleLibrary::CreateLinearDragModule(),
				VfxModuleLibrary::CreateScaleOverLifeModule(0.07f, 0.015f),
				VfxModuleLibrary::CreateColorOverLifeModule(glm::vec4(0.75f, 0.95f, 1.0f, 1.0f), glm::vec4(0.95f, 0.80f, 0.30f, 0.0f)),
				VfxModuleLibrary::CreateSolveVelocityModule(),
				VfxModuleLibrary::CreateSpriteRendererModule(true, false, 4.5f)
			};
			if (auto* g = em.FindModule("mod.update.gravity")) g->SetVec4("Gravity", glm::vec4(0.0f, 0.6f, 0.0f, 0.0f));
			if (auto* n = em.FindModule("mod.update.curl_noise")) { n->SetFloat("NoiseStrength", 1.4f); n->SetFloat("NoiseFrequency", 1.2f); }
			if (auto* d = em.FindModule("mod.update.drag")) d->SetFloat("LinearDrag", 0.6f);
			shieldAsset.Emitters.push_back(em);
		}

	}

	// =========================================================================
	// FlameTornado3D
	// =========================================================================
	ParticleAsset tornadoAsset;
	tornadoAsset.Name = "FlameTornado3D";
	tornadoAsset.Duration = 5.0f;
	tornadoAsset.Looping = true;
	{
		// Emitter 1: VortexFlameCore
		{
			ParticleEmitterLayer em;
			em.Name = "VortexFlameCore";
			em.MaxParticles = 260;
			em.Lifetime = 1.60f;
			em.EmissionRate = 110.00f;
			em.Speed = 3.00f;
			em.SpeedVariance = 1.00f;
			em.Gravity = glm::vec3(0.00f, 0.00f, 0.00f);
			em.LinearDrag = 0.20f;
			em.VortexStrength = 15.00f;
			em.EmitterType = ParticleEmitterType::Cone;
			em.SizeStart = 0.350f;
			em.SizeEnd = 1.150f;
			em.ColorStart = glm::vec4(1.00f, 0.95f, 0.55f, 1.00f);
			em.ColorEnd = glm::vec4(0.90f, 0.15f, 0.00f, 0.00f);
			em.FacingMode = ParticleRenderMode::Billboard;
			em.Material.TexturePath = "textures/particles/fire_flipbook_4x4.wtex";
			em.Material.MaterialPath = "materials/particles/particle_additive.wmat";
			em.Material.BlendMode = ParticleBlendMode::Additive;
			em.Material.EmissiveBoost = 4.8f;
			em.Material.SubUVTilesX = 4;
			em.Material.SubUVTilesY = 4;
			em.Modules = {
				VfxModuleLibrary::CreateSpawnRateModule(110.0f),
				VfxModuleLibrary::CreateInitParticleModule(1.6f, 0.35f, 1.15f),
				VfxModuleLibrary::CreateConeSpawnModule(),
				VfxModuleLibrary::CreateVortexTornadoModule(15.0f, 6.5f, 0.45f),
				VfxModuleLibrary::CreateFlipbookModule(4, 4, 18.0f, true),
				VfxModuleLibrary::CreateScaleOverLifeModule(0.35f, 1.15f),
				VfxModuleLibrary::CreateColorOverLifeModule(glm::vec4(1.0f, 0.95f, 0.55f, 1.0f), glm::vec4(0.90f, 0.15f, 0.0f, 0.0f)),
				VfxModuleLibrary::CreateSolveVelocityModule(),
				VfxModuleLibrary::CreateSpriteRendererModule(true, false, 4.8f)
			};
			if (auto* c = em.FindModule("mod.spawn.cone")) { c->SetFloat("ConeAngle", 14.0f); c->SetFloat("ConeRadius", 0.20f); c->SetFloat("InitialSpeed", 3.0f); }
			tornadoAsset.Emitters.push_back(em);
		}

		// Emitter 2: SwirlingOuterSheath
		{
			ParticleEmitterLayer em;
			em.Name = "SwirlingOuterSheath";
			em.MaxParticles = 220;
			em.Lifetime = 1.80f;
			em.EmissionRate = 80.00f;
			em.Speed = 3.50f;
			em.SpeedVariance = 1.20f;
			em.Gravity = glm::vec3(0.00f, 0.00f, 0.00f);
			em.LinearDrag = 0.30f;
			em.VortexStrength = 11.00f;
			em.EmitterType = ParticleEmitterType::Cone;
			em.SizeStart = 0.480f;
			em.SizeEnd = 1.650f;
			em.ColorStart = glm::vec4(1.00f, 0.60f, 0.10f, 0.90f);
			em.ColorEnd = glm::vec4(0.80f, 0.08f, 0.00f, 0.00f);
			em.FacingMode = ParticleRenderMode::Billboard;
			em.Material.TexturePath = "textures/particles/flame_soft.wtex";
			em.Material.MaterialPath = "materials/particles/particle_additive.wmat";
			em.Material.BlendMode = ParticleBlendMode::Additive;
			em.Material.EmissiveBoost = 3.8f;
			em.Modules = {
				VfxModuleLibrary::CreateSpawnRateModule(80.0f),
				VfxModuleLibrary::CreateInitParticleModule(1.8f, 0.48f, 1.65f),
				VfxModuleLibrary::CreateConeSpawnModule(),
				VfxModuleLibrary::CreateVortexTornadoModule(11.0f, 5.0f, 0.65f),
				VfxModuleLibrary::CreateRotationOverLifeModule(1.5f, 0.0f),
				VfxModuleLibrary::CreateScaleOverLifeModule(0.48f, 1.65f),
				VfxModuleLibrary::CreateColorOverLifeModule(glm::vec4(1.0f, 0.60f, 0.10f, 0.9f), glm::vec4(0.80f, 0.08f, 0.0f, 0.0f)),
				VfxModuleLibrary::CreateSolveVelocityModule(),
				VfxModuleLibrary::CreateSpriteRendererModule(true, false, 3.8f)
			};
			if (auto* c = em.FindModule("mod.spawn.cone")) { c->SetFloat("ConeAngle", 22.0f); c->SetFloat("ConeRadius", 0.35f); c->SetFloat("InitialSpeed", 3.5f); }
			tornadoAsset.Emitters.push_back(em);
		}

		// Emitter 3: CentrifugalSparks
		{
			ParticleEmitterLayer em;
			em.Name = "CentrifugalSparks";
			em.MaxParticles = 260;
			em.Lifetime = 1.30f;
			em.EmissionRate = 75.00f;
			em.Speed = 4.80f;
			em.SpeedVariance = 2.00f;
			em.Gravity = glm::vec3(0.00f, -1.00f, 0.00f);
			em.LinearDrag = 0.40f;
			em.VortexStrength = 18.00f;
			em.EmitterType = ParticleEmitterType::Cone;
			em.SizeStart = 0.080f;
			em.SizeEnd = 0.015f;
			em.ColorStart = glm::vec4(1.00f, 0.95f, 0.50f, 1.00f);
			em.ColorEnd = glm::vec4(1.00f, 0.25f, 0.00f, 0.00f);
			em.FacingMode = ParticleRenderMode::StretchedVelocity;
			em.Material.TexturePath = "textures/particles/spark_glow.wtex";
			em.Material.MaterialPath = "materials/particles/particle_additive.wmat";
			em.Material.BlendMode = ParticleBlendMode::Additive;
			em.Material.EmissiveBoost = 5.8f;
			em.Modules = {
				VfxModuleLibrary::CreateSpawnRateModule(75.0f),
				VfxModuleLibrary::CreateInitParticleModule(1.3f, 0.08f, 0.015f),
				VfxModuleLibrary::CreateConeSpawnModule(),
				VfxModuleLibrary::CreateVortexTornadoModule(18.0f, 7.5f, 0.85f),
				VfxModuleLibrary::CreateScaleOverLifeModule(0.08f, 0.015f),
				VfxModuleLibrary::CreateColorOverLifeModule(glm::vec4(1.0f, 0.95f, 0.50f, 1.0f), glm::vec4(1.0f, 0.25f, 0.0f, 0.0f)),
				VfxModuleLibrary::CreateSolveVelocityModule(),
				VfxModuleLibrary::CreateSpriteRendererModule(true, false, 5.8f)
			};
			if (auto* c = em.FindModule("mod.spawn.cone")) { c->SetFloat("ConeAngle", 26.0f); c->SetFloat("ConeRadius", 0.25f); c->SetFloat("InitialSpeed", 4.8f); }
			if (auto* r = em.FindModule("mod.render.sprite")) r->SetFloat("FacingMode", static_cast<float>(ParticleRenderMode::StretchedVelocity));
			tornadoAsset.Emitters.push_back(em);
		}

		// Emitter 4: TopSmokeFunnel
		{
			ParticleEmitterLayer em;
			em.Name = "TopSmokeFunnel";
			em.MaxParticles = 160;
			em.Lifetime = 3.20f;
			em.EmissionRate = 25.00f;
			em.Speed = 2.00f;
			em.SpeedVariance = 0.00f;
			em.Gravity = glm::vec3(0.00f, 2.50f, 0.00f);
			em.LinearDrag = 0.50f;
			em.VortexStrength = 3.00f;
			em.EmitterType = ParticleEmitterType::Cone;
			em.SizeStart = 0.600f;
			em.SizeEnd = 3.200f;
			em.ColorStart = glm::vec4(0.30f, 0.28f, 0.25f, 0.60f);
			em.ColorEnd = glm::vec4(0.10f, 0.10f, 0.10f, 0.00f);
			em.FacingMode = ParticleRenderMode::Billboard;
			em.Material.TexturePath = "textures/particles/smoke_puff.wtex";
			em.Material.MaterialPath = "materials/particles/particle_alphablend.wmat";
			em.Material.BlendMode = ParticleBlendMode::AlphaBlend;
			em.Material.EmissiveBoost = 0.8f;
			em.Modules = {
				VfxModuleLibrary::CreateSpawnRateModule(25.0f),
				VfxModuleLibrary::CreateInitParticleModule(3.2f, 0.6f, 3.2f),
				VfxModuleLibrary::CreateConeSpawnModule(),
				VfxModuleLibrary::CreateGravityModule(),
				VfxModuleLibrary::CreateLinearDragModule(),
				VfxModuleLibrary::CreateVortexModule(),
				VfxModuleLibrary::CreateRotationOverLifeModule(0.7f, 0.0f),
				VfxModuleLibrary::CreateScaleOverLifeModule(0.6f, 3.2f),
				VfxModuleLibrary::CreateColorOverLifeModule(glm::vec4(0.30f, 0.28f, 0.25f, 0.60f), glm::vec4(0.10f, 0.10f, 0.10f, 0.0f)),
				VfxModuleLibrary::CreateSolveVelocityModule(),
				VfxModuleLibrary::CreateSpriteRendererModule(false, false, 0.8f)
			};
			if (auto* c = em.FindModule("mod.spawn.cone")) { c->SetFloat("ConeAngle", 24.0f); c->SetFloat("ConeRadius", 0.50f); c->SetFloat("InitialSpeed", 2.0f); }
			if (auto* g = em.FindModule("mod.update.gravity")) g->SetVec4("Gravity", glm::vec4(0.0f, 2.5f, 0.0f, 0.0f));
			if (auto* d = em.FindModule("mod.update.drag")) d->SetFloat("LinearDrag", 0.5f);
			if (auto* v = em.FindModule("mod.update.vortex")) v->SetFloat("VortexStrength", 3.0f);
			tornadoAsset.Emitters.push_back(em);
		}

		// Emitter 5: GroundIntakeSwirl
		{
			ParticleEmitterLayer em;
			em.Name = "GroundIntakeSwirl";
			em.MaxParticles = 90;
			em.Lifetime = 1.00f;
			em.EmissionRate = 45.00f;
			em.Speed = 0.50f;
			em.SpeedVariance = 0.00f;
			em.Gravity = glm::vec3(0.00f, 0.00f, 0.00f);
			em.LinearDrag = 0.80f;
			em.VortexStrength = 12.00f;
			em.EmitterType = ParticleEmitterType::Circle;
			em.SizeStart = 0.060f;
			em.SizeEnd = 0.010f;
			em.ColorStart = glm::vec4(1.00f, 0.75f, 0.20f, 0.90f);
			em.ColorEnd = glm::vec4(0.80f, 0.10f, 0.00f, 0.00f);
			em.FacingMode = ParticleRenderMode::Billboard;
			em.Material.TexturePath = "textures/particles/spark_glow.wtex";
			em.Material.MaterialPath = "materials/particles/particle_additive.wmat";
			em.Material.BlendMode = ParticleBlendMode::Additive;
			em.Material.EmissiveBoost = 4.5f;
			em.Modules = {
				VfxModuleLibrary::CreateSpawnRateModule(45.0f),
				VfxModuleLibrary::CreateInitParticleModule(1.0f, 0.06f, 0.01f),
				VfxModuleLibrary::CreateRingSpawnModule(1.6f, 0.5f),
				VfxModuleLibrary::CreateVortexModule(),
				VfxModuleLibrary::CreateLinearDragModule(),
				VfxModuleLibrary::CreateScaleOverLifeModule(0.06f, 0.01f),
				VfxModuleLibrary::CreateColorOverLifeModule(glm::vec4(1.0f, 0.75f, 0.20f, 0.9f), glm::vec4(0.8f, 0.10f, 0.0f, 0.0f)),
				VfxModuleLibrary::CreateSolveVelocityModule(),
				VfxModuleLibrary::CreateSpriteRendererModule(true, false, 4.5f)
			};
			if (auto* v = em.FindModule("mod.update.vortex")) v->SetFloat("VortexStrength", 12.0f);
			if (auto* d = em.FindModule("mod.update.drag")) d->SetFloat("LinearDrag", 0.8f);
			tornadoAsset.Emitters.push_back(em);
		}

	}

	// =========================================================================
	// MeteorImpactExplosion3D
	// =========================================================================
	ParticleAsset meteorAsset;
	meteorAsset.Name = "MeteorImpactExplosion3D";
	meteorAsset.Duration = 3.0f;
	meteorAsset.Looping = false;
	{
		// Emitter 1: ImpactFlash
		{
			ParticleEmitterLayer em;
			em.Name = "ImpactFlash";
			em.MaxParticles = 1;
			em.Lifetime = 0.12f;
			em.EmissionRate = 0.00f;
			em.Speed = 0.00f;
			em.SpeedVariance = 0.00f;
			em.Gravity = glm::vec3(0.00f, 0.00f, 0.00f);
			em.LinearDrag = 0.00f;
			em.VortexStrength = 0.00f;
			em.EmitterType = ParticleEmitterType::Point;
			em.SizeStart = 0.500f;
			em.SizeEnd = 3.200f;
			em.ColorStart = glm::vec4(1.00f, 1.00f, 0.95f, 1.00f);
			em.ColorEnd = glm::vec4(1.00f, 0.75f, 0.30f, 0.00f);
			em.FacingMode = ParticleRenderMode::Billboard;
			em.Bursts = { { 0.00f, 1u } };
			em.Material.TexturePath = "textures/particles/flame_soft.wtex";
			em.Material.MaterialPath = "materials/particles/particle_additive.wmat";
			em.Material.BlendMode = ParticleBlendMode::Additive;
			em.Material.EmissiveBoost = 8.5f;
			em.Modules = {
				VfxModuleLibrary::CreateSpawnBurstModule(1, 0.0f),
				VfxModuleLibrary::CreateInitParticleModule(0.12f, 0.5f, 3.2f),
				VfxModuleLibrary::CreatePointSpawnModule(),
				VfxModuleLibrary::CreateScaleOverLifeModule(0.5f, 3.2f),
				VfxModuleLibrary::CreateColorOverLifeModule(glm::vec4(1.0f, 1.0f, 0.95f, 1.0f), glm::vec4(1.0f, 0.75f, 0.30f, 0.0f)),
				VfxModuleLibrary::CreateSolveVelocityModule(),
				VfxModuleLibrary::CreateSpriteRendererModule(true, false, 8.5f)
			};
			if (auto* p = em.FindModule("mod.spawn.point")) p->SetFloat("PointSpeed", 0.0f);
			meteorAsset.Emitters.push_back(em);
		}

		// Emitter 2: GroundShockwaveRing
		{
			ParticleEmitterLayer em;
			em.Name = "GroundShockwaveRing";
			em.MaxParticles = 1;
			em.Lifetime = 0.70f;
			em.EmissionRate = 0.00f;
			em.Speed = 0.00f;
			em.SpeedVariance = 0.00f;
			em.Gravity = glm::vec3(0.00f, 0.00f, 0.00f);
			em.LinearDrag = 0.00f;
			em.VortexStrength = 0.00f;
			em.EmitterType = ParticleEmitterType::Point;
			em.SizeStart = 0.500f;
			em.SizeEnd = 4.200f;
			em.ColorStart = glm::vec4(1.00f, 0.75f, 0.20f, 0.95f);
			em.ColorEnd = glm::vec4(0.80f, 0.15f, 0.00f, 0.00f);
			em.FacingMode = ParticleRenderMode::HorizontalQuad;
			em.Bursts = { { 0.01f, 1u } };
			em.Material.TexturePath = "textures/particles/shockwave_ring.wtex";
			em.Material.MaterialPath = "materials/particles/particle_additive.wmat";
			em.Material.BlendMode = ParticleBlendMode::Additive;
			em.Material.EmissiveBoost = 5.2f;
			em.Modules = {
				VfxModuleLibrary::CreateSpawnBurstModule(1, 0.01f),
				VfxModuleLibrary::CreateInitParticleModule(0.70f, 0.5f, 4.2f),
				VfxModuleLibrary::CreatePointSpawnModule(),
				VfxModuleLibrary::CreateShockwaveExpansionModule(6.0f, 4.2f, 1.6f),
				VfxModuleLibrary::CreateColorOverLifeModule(glm::vec4(1.0f, 0.75f, 0.20f, 0.95f), glm::vec4(0.80f, 0.15f, 0.0f, 0.0f)),
				VfxModuleLibrary::CreateSolveVelocityModule(),
				VfxModuleLibrary::CreateSpriteRendererModule(true, true, 5.2f)
			};
			if (auto* p = em.FindModule("mod.spawn.point")) p->SetFloat("PointSpeed", 0.0f);
			meteorAsset.Emitters.push_back(em);
		}

		// Emitter 3: ExplosionFireball
		{
			ParticleEmitterLayer em;
			em.Name = "ExplosionFireball";
			em.MaxParticles = 80;
			em.Lifetime = 0.75f;
			em.EmissionRate = 0.00f;
			em.Speed = 3.80f;
			em.SpeedVariance = 1.50f;
			em.Gravity = glm::vec3(0.00f, 3.20f, 0.00f);
			em.LinearDrag = 2.40f;
			em.VortexStrength = 0.00f;
			em.EmitterType = ParticleEmitterType::Cone;
			em.SizeStart = 0.700f;
			em.SizeEnd = 2.600f;
			em.ColorStart = glm::vec4(1.00f, 0.90f, 0.45f, 1.00f);
			em.ColorEnd = glm::vec4(0.85f, 0.15f, 0.00f, 0.00f);
			em.FacingMode = ParticleRenderMode::Billboard;
			em.Bursts = { { 0.02f, 35u } };
			em.Material.TexturePath = "textures/particles/fire_flipbook_4x4.wtex";
			em.Material.MaterialPath = "materials/particles/particle_additive.wmat";
			em.Material.BlendMode = ParticleBlendMode::Additive;
			em.Material.EmissiveBoost = 4.5f;
			em.Material.SubUVTilesX = 4;
			em.Material.SubUVTilesY = 4;
			em.Modules = {
				VfxModuleLibrary::CreateSpawnBurstModule(35, 0.02f),
				VfxModuleLibrary::CreateInitParticleModule(0.75f, 0.7f, 2.6f),
				VfxModuleLibrary::CreateConeSpawnModule(),
				VfxModuleLibrary::CreateGravityModule(),
				VfxModuleLibrary::CreateLinearDragModule(),
				VfxModuleLibrary::CreateFlipbookModule(4, 4, 18.0f, false),
				VfxModuleLibrary::CreateScaleOverLifeModule(0.7f, 2.6f),
				VfxModuleLibrary::CreateColorOverLifeModule(glm::vec4(1.0f, 0.90f, 0.45f, 1.0f), glm::vec4(0.85f, 0.15f, 0.0f, 0.0f)),
				VfxModuleLibrary::CreateSolveVelocityModule(),
				VfxModuleLibrary::CreateSpriteRendererModule(true, false, 4.5f)
			};
			if (auto* c = em.FindModule("mod.spawn.cone")) { c->SetFloat("ConeAngle", 35.0f); c->SetFloat("ConeRadius", 0.35f); c->SetFloat("InitialSpeed", 3.8f); }
			if (auto* g = em.FindModule("mod.update.gravity")) g->SetVec4("Gravity", glm::vec4(0.0f, 3.2f, 0.0f, 0.0f));
			if (auto* d = em.FindModule("mod.update.drag")) d->SetFloat("LinearDrag", 2.4f);
			meteorAsset.Emitters.push_back(em);
		}

		// Emitter 4: MoltenDebrisShrapnel
		{
			ParticleEmitterLayer em;
			em.Name = "MoltenDebrisShrapnel";
			em.MaxParticles = 90;
			em.Lifetime = 1.40f;
			em.EmissionRate = 0.00f;
			em.Speed = 12.00f;
			em.SpeedVariance = 4.00f;
			em.Gravity = glm::vec3(0.00f, -14.00f, 0.00f);
			em.LinearDrag = 0.35f;
			em.VortexStrength = 0.00f;
			em.EmitterType = ParticleEmitterType::Cone;
			em.SizeStart = 0.120f;
			em.SizeEnd = 0.020f;
			em.ColorStart = glm::vec4(1.00f, 0.88f, 0.30f, 1.00f);
			em.ColorEnd = glm::vec4(0.50f, 0.08f, 0.00f, 0.00f);
			em.FacingMode = ParticleRenderMode::StretchedVelocity;
			em.Bursts = { { 0.02f, 45u } };
			em.Material.TexturePath = "textures/particles/spark_glow.wtex";
			em.Material.MaterialPath = "materials/particles/particle_additive.wmat";
			em.Material.BlendMode = ParticleBlendMode::Additive;
			em.Material.EmissiveBoost = 6.5f;
			em.Modules = {
				VfxModuleLibrary::CreateSpawnBurstModule(45, 0.02f),
				VfxModuleLibrary::CreateInitParticleModule(1.4f, 0.12f, 0.02f),
				VfxModuleLibrary::CreateConeSpawnModule(),
				VfxModuleLibrary::CreateGravityModule(),
				VfxModuleLibrary::CreateLinearDragModule(),
				VfxModuleLibrary::CreateScaleOverLifeModule(0.12f, 0.02f),
				VfxModuleLibrary::CreateColorOverLifeModule(glm::vec4(1.0f, 0.88f, 0.30f, 1.0f), glm::vec4(0.50f, 0.08f, 0.0f, 0.0f)),
				VfxModuleLibrary::CreateSolveVelocityModule(),
				VfxModuleLibrary::CreateSpriteRendererModule(true, false, 6.5f)
			};
			if (auto* c = em.FindModule("mod.spawn.cone")) { c->SetFloat("ConeAngle", 45.0f); c->SetFloat("ConeRadius", 0.20f); c->SetFloat("InitialSpeed", 12.0f); }
			if (auto* g = em.FindModule("mod.update.gravity")) g->SetVec4("Gravity", glm::vec4(0.0f, -14.0f, 0.0f, 0.0f));
			if (auto* d = em.FindModule("mod.update.drag")) d->SetFloat("LinearDrag", 0.35f);
			if (auto* r = em.FindModule("mod.render.sprite")) r->SetFloat("FacingMode", static_cast<float>(ParticleRenderMode::StretchedVelocity));
			meteorAsset.Emitters.push_back(em);
		}

		// Emitter 5: BillowingMushroomSmoke
		{
			ParticleEmitterLayer em;
			em.Name = "BillowingMushroomSmoke";
			em.MaxParticles = 60;
			em.Lifetime = 2.60f;
			em.EmissionRate = 0.00f;
			em.Speed = 2.20f;
			em.SpeedVariance = 0.80f;
			em.Gravity = glm::vec3(0.00f, 2.40f, 0.00f);
			em.LinearDrag = 0.80f;
			em.VortexStrength = 0.00f;
			em.EmitterType = ParticleEmitterType::Cone;
			em.SizeStart = 0.600f;
			em.SizeEnd = 3.600f;
			em.ColorStart = glm::vec4(0.28f, 0.25f, 0.22f, 0.65f);
			em.ColorEnd = glm::vec4(0.10f, 0.10f, 0.10f, 0.00f);
			em.FacingMode = ParticleRenderMode::Billboard;
			em.Bursts = { { 0.10f, 24u } };
			em.Material.TexturePath = "textures/particles/smoke_puff.wtex";
			em.Material.MaterialPath = "materials/particles/particle_alphablend.wmat";
			em.Material.BlendMode = ParticleBlendMode::AlphaBlend;
			em.Material.EmissiveBoost = 0.6f;
			em.Modules = {
				VfxModuleLibrary::CreateSpawnBurstModule(24, 0.10f),
				VfxModuleLibrary::CreateInitParticleModule(2.6f, 0.6f, 3.6f),
				VfxModuleLibrary::CreateConeSpawnModule(),
				VfxModuleLibrary::CreateGravityModule(),
				VfxModuleLibrary::CreateLinearDragModule(),
				VfxModuleLibrary::CreateRotationOverLifeModule(0.8f, 0.0f),
				VfxModuleLibrary::CreateScaleOverLifeModule(0.6f, 3.6f),
				VfxModuleLibrary::CreateColorOverLifeModule(glm::vec4(0.28f, 0.25f, 0.22f, 0.65f), glm::vec4(0.10f, 0.10f, 0.10f, 0.0f)),
				VfxModuleLibrary::CreateSolveVelocityModule(),
				VfxModuleLibrary::CreateSpriteRendererModule(false, false, 0.6f)
			};
			if (auto* c = em.FindModule("mod.spawn.cone")) { c->SetFloat("ConeAngle", 20.0f); c->SetFloat("ConeRadius", 0.35f); c->SetFloat("InitialSpeed", 2.2f); }
			if (auto* g = em.FindModule("mod.update.gravity")) g->SetVec4("Gravity", glm::vec4(0.0f, 2.4f, 0.0f, 0.0f));
			if (auto* d = em.FindModule("mod.update.drag")) d->SetFloat("LinearDrag", 0.8f);
			meteorAsset.Emitters.push_back(em);
		}

	}

	// =========================================================================
	// LightningPlasmaBeam3D
	// =========================================================================
	ParticleAsset beamAsset;
	beamAsset.Name = "LightningPlasmaBeam3D";
	beamAsset.Duration = 4.0f;
	beamAsset.Looping = true;
	{
		// Emitter 1: ContinuousPlasmaColumn
		{
			ParticleEmitterLayer em;
			em.Name = "ContinuousPlasmaColumn";
			em.MaxParticles = 160;
			em.Lifetime = 0.08f;
			em.EmissionRate = 140.00f;
			em.Speed = 0.00f;
			em.SpeedVariance = 0.00f;
			em.Gravity = glm::vec3(0.00f, 0.00f, 0.00f);
			em.LinearDrag = 0.00f;
			em.VortexStrength = 0.00f;
			em.EmitterType = ParticleEmitterType::Point;
			em.SizeStart = 0.280f;
			em.SizeEnd = 0.450f;
			em.ColorStart = glm::vec4(1.00f, 1.00f, 1.00f, 1.00f);
			em.ColorEnd = glm::vec4(0.35f, 0.85f, 1.00f, 0.00f);
			em.FacingMode = ParticleRenderMode::StretchedVelocity;
			em.Material.TexturePath = "textures/particles/electric_beam.wtex";
			em.Material.MaterialPath = "materials/particles/particle_additive.wmat";
			em.Material.BlendMode = ParticleBlendMode::Additive;
			em.Material.EmissiveBoost = 7.0f;
			em.Modules = {
				VfxModuleLibrary::CreateSpawnRateModule(140.0f, 160),
				VfxModuleLibrary::CreateInitParticleModule(0.08f, 0.28f, 0.45f),
				VfxModuleLibrary::CreateBeamSpawnModule({ 0.0f, 0.4f, 0.0f }, { 0.0f, 0.4f, 8.0f }, 0.03f, 20.0f),
				VfxModuleLibrary::CreateColorOverLifeModule(glm::vec4(1.0f, 1.0f, 1.0f, 1.0f), glm::vec4(0.35f, 0.85f, 1.0f, 0.0f)),
				VfxModuleLibrary::CreateSolveVelocityModule(),
				VfxModuleLibrary::CreateSpriteRendererModule(true, false, 7.0f)
			};
			if (auto* r = em.FindModule("mod.render.sprite")) r->SetFloat("FacingMode", static_cast<float>(ParticleRenderMode::StretchedVelocity));
			beamAsset.Emitters.push_back(em);
		}

		// Emitter 2: ViolentLightningArcs
		{
			ParticleEmitterLayer em;
			em.Name = "ViolentLightningArcs";
			em.MaxParticles = 140;
			em.Lifetime = 0.06f;
			em.EmissionRate = 110.00f;
			em.Speed = 0.00f;
			em.SpeedVariance = 0.00f;
			em.Gravity = glm::vec3(0.00f, 0.00f, 0.00f);
			em.LinearDrag = 0.00f;
			em.VortexStrength = 0.00f;
			em.EmitterType = ParticleEmitterType::Point;
			em.SizeStart = 0.220f;
			em.SizeEnd = 0.350f;
			em.ColorStart = glm::vec4(0.95f, 0.65f, 1.00f, 1.00f);
			em.ColorEnd = glm::vec4(0.35f, 0.15f, 1.00f, 0.00f);
			em.FacingMode = ParticleRenderMode::StretchedVelocity;
			em.Material.TexturePath = "textures/particles/electric_beam.wtex";
			em.Material.MaterialPath = "materials/particles/particle_additive.wmat";
			em.Material.BlendMode = ParticleBlendMode::Additive;
			em.Material.EmissiveBoost = 5.5f;
			em.Modules = {
				VfxModuleLibrary::CreateSpawnRateModule(110.0f, 140),
				VfxModuleLibrary::CreateInitParticleModule(0.06f, 0.22f, 0.35f),
				VfxModuleLibrary::CreateBeamSpawnModule({ 0.0f, 0.4f, 0.0f }, { 0.0f, 0.4f, 8.0f }, 0.45f, 36.0f),
				VfxModuleLibrary::CreateColorOverLifeModule(glm::vec4(0.95f, 0.65f, 1.0f, 1.0f), glm::vec4(0.35f, 0.15f, 1.0f, 0.0f)),
				VfxModuleLibrary::CreateSolveVelocityModule(),
				VfxModuleLibrary::CreateSpriteRendererModule(true, false, 5.5f)
			};
			if (auto* r = em.FindModule("mod.render.sprite")) r->SetFloat("FacingMode", static_cast<float>(ParticleRenderMode::StretchedVelocity));
			beamAsset.Emitters.push_back(em);
		}

		// Emitter 3: HelicalPlasmaTendril
		{
			ParticleEmitterLayer em;
			em.Name = "HelicalPlasmaTendril";
			em.MaxParticles = 100;
			em.Lifetime = 0.08f;
			em.EmissionRate = 80.00f;
			em.Speed = 0.00f;
			em.SpeedVariance = 0.00f;
			em.Gravity = glm::vec3(0.00f, 0.00f, 0.00f);
			em.LinearDrag = 0.00f;
			em.VortexStrength = 0.00f;
			em.EmitterType = ParticleEmitterType::Point;
			em.SizeStart = 0.180f;
			em.SizeEnd = 0.280f;
			em.ColorStart = glm::vec4(0.40f, 0.95f, 1.00f, 1.00f);
			em.ColorEnd = glm::vec4(0.10f, 0.40f, 1.00f, 0.00f);
			em.FacingMode = ParticleRenderMode::StretchedVelocity;
			em.Material.TexturePath = "textures/particles/electric_beam.wtex";
			em.Material.MaterialPath = "materials/particles/particle_additive.wmat";
			em.Material.BlendMode = ParticleBlendMode::Additive;
			em.Material.EmissiveBoost = 5.0f;
			em.Modules = {
				VfxModuleLibrary::CreateSpawnRateModule(80.0f, 100),
				VfxModuleLibrary::CreateInitParticleModule(0.08f, 0.18f, 0.28f),
				VfxModuleLibrary::CreateBeamSpawnModule({ 0.0f, 0.4f, 0.0f }, { 0.0f, 0.4f, 8.0f }, 0.18f, 16.0f),
				VfxModuleLibrary::CreateColorOverLifeModule(glm::vec4(0.40f, 0.95f, 1.0f, 1.0f), glm::vec4(0.10f, 0.40f, 1.0f, 0.0f)),
				VfxModuleLibrary::CreateSolveVelocityModule(),
				VfxModuleLibrary::CreateSpriteRendererModule(true, false, 5.0f)
			};
			if (auto* r = em.FindModule("mod.render.sprite")) r->SetFloat("FacingMode", static_cast<float>(ParticleRenderMode::StretchedVelocity));
			beamAsset.Emitters.push_back(em);
		}

		// Emitter 4: SourcePlasmaCorona
		{
			ParticleEmitterLayer em;
			em.Name = "SourcePlasmaCorona";
			em.MaxParticles = 1;
			em.Lifetime = 9999.00f;
			em.EmissionRate = 0.00f;
			em.Speed = 0.00f;
			em.SpeedVariance = 0.00f;
			em.Gravity = glm::vec3(0.00f, 0.00f, 0.00f);
			em.LinearDrag = 0.00f;
			em.VortexStrength = 0.00f;
			em.EmitterType = ParticleEmitterType::Point;
			em.SizeStart = 0.900f;
			em.SizeEnd = 0.900f;
			em.ColorStart = glm::vec4(0.85f, 0.95f, 1.00f, 1.00f);
			em.ColorEnd = glm::vec4(0.85f, 0.95f, 1.00f, 1.00f);
			em.FacingMode = ParticleRenderMode::Billboard;
			em.Bursts = { { 0.00f, 1u } };
			em.Material.TexturePath = "textures/particles/spark_glow.wtex";
			em.Material.MaterialPath = "materials/particles/particle_additive.wmat";
			em.Material.BlendMode = ParticleBlendMode::Additive;
			em.Material.EmissiveBoost = 6.0f;
			em.Modules = {
				VfxModuleLibrary::CreateSpawnBurstModule(1, 0.0f),
				VfxModuleLibrary::CreateInitParticleModule(9999.0f, 0.9f, 0.9f),
				VfxModuleLibrary::CreatePointSpawnModule(),
				VfxModuleLibrary::CreateColorOverLifeModule(glm::vec4(0.85f, 0.95f, 1.0f, 1.0f), glm::vec4(0.85f, 0.95f, 1.0f, 1.0f)),
				VfxModuleLibrary::CreateSolveVelocityModule(),
				VfxModuleLibrary::CreateSpriteRendererModule(true, false, 6.0f)
			};
			if (auto* p = em.FindModule("mod.spawn.point")) p->SetFloat("PointSpeed", 0.0f);
			beamAsset.Emitters.push_back(em);
		}

		// Emitter 5: TargetImpactShower
		{
			ParticleEmitterLayer em;
			em.Name = "TargetImpactShower";
			em.MaxParticles = 120;
			em.Lifetime = 0.35f;
			em.EmissionRate = 180.00f;
			em.Speed = 8.00f;
			em.SpeedVariance = 0.00f;
			em.Gravity = glm::vec3(0.00f, -9.80f, 0.00f);
			em.LinearDrag = 1.50f;
			em.VortexStrength = 0.00f;
			em.EmitterType = ParticleEmitterType::Cone;
			em.SizeStart = 0.090f;
			em.SizeEnd = 0.010f;
			em.ColorStart = glm::vec4(1.00f, 1.00f, 1.00f, 1.00f);
			em.ColorEnd = glm::vec4(0.30f, 0.70f, 1.00f, 0.00f);
			em.FacingMode = ParticleRenderMode::StretchedVelocity;
			em.Material.TexturePath = "textures/particles/spark_glow.wtex";
			em.Material.MaterialPath = "materials/particles/particle_additive.wmat";
			em.Material.BlendMode = ParticleBlendMode::Additive;
			em.Material.EmissiveBoost = 6.0f;
			em.Modules = {
				VfxModuleLibrary::CreateSpawnRateModule(180.0f, 120),
				VfxModuleLibrary::CreateInitParticleModule(0.35f, 0.09f, 0.01f),
				VfxModuleLibrary::CreateConeSpawnModule(),
				VfxModuleLibrary::CreateGravityModule(),
				VfxModuleLibrary::CreateLinearDragModule(),
				VfxModuleLibrary::CreateScaleOverLifeModule(0.09f, 0.01f),
				VfxModuleLibrary::CreateColorOverLifeModule(glm::vec4(1.0f, 1.0f, 1.0f, 1.0f), glm::vec4(0.30f, 0.70f, 1.0f, 0.0f)),
				VfxModuleLibrary::CreateSolveVelocityModule(),
				VfxModuleLibrary::CreateSpriteRendererModule(true, false, 6.0f)
			};
			if (auto* c = em.FindModule("mod.spawn.cone")) { c->SetFloat("ConeAngle", 60.0f); c->SetFloat("InitialSpeed", 8.0f); }
			if (auto* g = em.FindModule("mod.update.gravity")) g->SetVec4("Gravity", glm::vec4(0.0f, -9.8f, 0.0f, 0.0f));
			if (auto* d = em.FindModule("mod.update.drag")) d->SetFloat("LinearDrag", 1.5f);
			if (auto* r = em.FindModule("mod.render.sprite")) r->SetFloat("FacingMode", static_cast<float>(ParticleRenderMode::StretchedVelocity));
			beamAsset.Emitters.push_back(em);
		}

	}

	// =========================================================================
	// BlizzardFrostNova3D
	// =========================================================================
	ParticleAsset blizzardAsset;
	blizzardAsset.Name = "BlizzardFrostNova3D";
	blizzardAsset.Duration = 3.5f;
	blizzardAsset.Looping = false;
	{
		// Emitter 1: FrostShockwaveRing
		{
			ParticleEmitterLayer em;
			em.Name = "FrostShockwaveRing";
			em.MaxParticles = 1;
			em.Lifetime = 0.80f;
			em.EmissionRate = 0.00f;
			em.Speed = 0.00f;
			em.SpeedVariance = 0.00f;
			em.Gravity = glm::vec3(0.00f, 0.00f, 0.00f);
			em.LinearDrag = 0.00f;
			em.VortexStrength = 0.00f;
			em.EmitterType = ParticleEmitterType::Point;
			em.SizeStart = 0.400f;
			em.SizeEnd = 5.200f;
			em.ColorStart = glm::vec4(0.40f, 0.85f, 1.00f, 0.95f);
			em.ColorEnd = glm::vec4(0.10f, 0.35f, 0.85f, 0.00f);
			em.FacingMode = ParticleRenderMode::HorizontalQuad;
			em.Bursts = { { 0.00f, 1u } };
			em.Material.TexturePath = "textures/particles/shockwave_ring.wtex";
			em.Material.MaterialPath = "materials/particles/particle_additive.wmat";
			em.Material.BlendMode = ParticleBlendMode::Additive;
			em.Material.EmissiveBoost = 4.5f;
			em.Modules = {
				VfxModuleLibrary::CreateSpawnBurstModule(1, 0.0f),
				VfxModuleLibrary::CreateInitParticleModule(0.80f, 0.4f, 5.2f),
				VfxModuleLibrary::CreatePointSpawnModule(),
				VfxModuleLibrary::CreateShockwaveExpansionModule(6.0f, 5.2f, 1.4f),
				VfxModuleLibrary::CreateColorOverLifeModule(glm::vec4(0.40f, 0.85f, 1.0f, 0.95f), glm::vec4(0.10f, 0.35f, 0.85f, 0.0f)),
				VfxModuleLibrary::CreateSolveVelocityModule(),
				VfxModuleLibrary::CreateSpriteRendererModule(true, true, 4.5f)
			};
			if (auto* p = em.FindModule("mod.spawn.point")) p->SetFloat("PointSpeed", 0.0f);
			blizzardAsset.Emitters.push_back(em);
		}

		// Emitter 2: IceCrystalShards
		{
			ParticleEmitterLayer em;
			em.Name = "IceCrystalShards";
			em.MaxParticles = 60;
			em.Lifetime = 1.40f;
			em.EmissionRate = 0.00f;
			em.Speed = 6.50f;
			em.SpeedVariance = 2.00f;
			em.Gravity = glm::vec3(0.00f, -4.50f, 0.00f);
			em.LinearDrag = 1.20f;
			em.VortexStrength = 0.00f;
			em.EmitterType = ParticleEmitterType::Cone;
			em.SizeStart = 0.180f;
			em.SizeEnd = 0.030f;
			em.ColorStart = glm::vec4(0.85f, 0.98f, 1.00f, 1.00f);
			em.ColorEnd = glm::vec4(0.20f, 0.60f, 1.00f, 0.00f);
			em.FacingMode = ParticleRenderMode::StretchedVelocity;
			em.Bursts = { { 0.02f, 40u } };
			em.Material.TexturePath = "textures/particles/ice_crystal.wtex";
			em.Material.MaterialPath = "materials/particles/particle_additive.wmat";
			em.Material.BlendMode = ParticleBlendMode::Additive;
			em.Material.EmissiveBoost = 4.2f;
			em.Modules = {
				VfxModuleLibrary::CreateSpawnBurstModule(40, 0.02f),
				VfxModuleLibrary::CreateInitParticleModule(1.4f, 0.18f, 0.03f),
				VfxModuleLibrary::CreateConeSpawnModule(),
				VfxModuleLibrary::CreateGravityModule(),
				VfxModuleLibrary::CreateLinearDragModule(),
				VfxModuleLibrary::CreateScaleOverLifeModule(0.18f, 0.03f),
				VfxModuleLibrary::CreateColorOverLifeModule(glm::vec4(0.85f, 0.98f, 1.0f, 1.0f), glm::vec4(0.20f, 0.60f, 1.0f, 0.0f)),
				VfxModuleLibrary::CreateSolveVelocityModule(),
				VfxModuleLibrary::CreateSpriteRendererModule(true, false, 4.2f)
			};
			if (auto* c = em.FindModule("mod.spawn.cone")) { c->SetFloat("ConeAngle", 40.0f); c->SetFloat("ConeRadius", 0.25f); c->SetFloat("InitialSpeed", 6.5f); }
			if (auto* g = em.FindModule("mod.update.gravity")) g->SetVec4("Gravity", glm::vec4(0.0f, -4.5f, 0.0f, 0.0f));
			if (auto* d = em.FindModule("mod.update.drag")) d->SetFloat("LinearDrag", 1.2f);
			if (auto* r = em.FindModule("mod.render.sprite")) r->SetFloat("FacingMode", static_cast<float>(ParticleRenderMode::StretchedVelocity));
			blizzardAsset.Emitters.push_back(em);
		}

		// Emitter 3: SubZeroMistWave
		{
			ParticleEmitterLayer em;
			em.Name = "SubZeroMistWave";
			em.MaxParticles = 60;
			em.Lifetime = 1.60f;
			em.EmissionRate = 0.00f;
			em.Speed = 2.50f;
			em.SpeedVariance = 0.00f;
			em.Gravity = glm::vec3(0.00f, 0.00f, 0.00f);
			em.LinearDrag = 1.80f;
			em.VortexStrength = 0.00f;
			em.EmitterType = ParticleEmitterType::Circle;
			em.SizeStart = 0.400f;
			em.SizeEnd = 1.500f;
			em.ColorStart = glm::vec4(0.50f, 0.85f, 1.00f, 0.35f);
			em.ColorEnd = glm::vec4(0.10f, 0.40f, 0.80f, 0.00f);
			em.FacingMode = ParticleRenderMode::HorizontalQuad;
			em.Bursts = { { 0.03f, 25u } };
			em.Material.TexturePath = "textures/particles/smoke_puff.wtex";
			em.Material.MaterialPath = "materials/particles/particle_alphablend.wmat";
			em.Material.BlendMode = ParticleBlendMode::AlphaBlend;
			em.Material.EmissiveBoost = 1.0f;
			em.Modules = {
				VfxModuleLibrary::CreateSpawnBurstModule(25, 0.03f),
				VfxModuleLibrary::CreateInitParticleModule(1.6f, 0.40f, 1.50f),
				VfxModuleLibrary::CreateRingSpawnModule(0.8f, 2.5f),
				VfxModuleLibrary::CreateLinearDragModule(),
				VfxModuleLibrary::CreateRotationOverLifeModule(0.5f, 0.0f),
				VfxModuleLibrary::CreateScaleOverLifeModule(0.40f, 1.50f),
				VfxModuleLibrary::CreateColorOverLifeModule(glm::vec4(0.50f, 0.85f, 1.0f, 0.35f), glm::vec4(0.10f, 0.40f, 0.80f, 0.0f)),
				VfxModuleLibrary::CreateSolveVelocityModule(),
				VfxModuleLibrary::CreateSpriteRendererModule(false, true, 1.0f)
			};
			if (auto* d = em.FindModule("mod.update.drag")) d->SetFloat("LinearDrag", 1.8f);
			blizzardAsset.Emitters.push_back(em);
		}

		// Emitter 4: BlizzardFlurries
		{
			ParticleEmitterLayer em;
			em.Name = "BlizzardFlurries";
			em.MaxParticles = 140;
			em.Lifetime = 2.00f;
			em.EmissionRate = 50.00f;
			em.Speed = 1.20f;
			em.SpeedVariance = 0.00f;
			em.Gravity = glm::vec3(0.00f, -0.60f, 0.00f);
			em.LinearDrag = 0.40f;
			em.VortexStrength = 4.20f;
			em.EmitterType = ParticleEmitterType::Circle;
			em.SizeStart = 0.050f;
			em.SizeEnd = 0.015f;
			em.ColorStart = glm::vec4(0.90f, 0.96f, 1.00f, 1.00f);
			em.ColorEnd = glm::vec4(0.40f, 0.70f, 1.00f, 0.00f);
			em.FacingMode = ParticleRenderMode::Billboard;
			em.Material.TexturePath = "textures/particles/spark_glow.wtex";
			em.Material.MaterialPath = "materials/particles/particle_additive.wmat";
			em.Material.BlendMode = ParticleBlendMode::Additive;
			em.Material.EmissiveBoost = 3.6f;
			em.Modules = {
				VfxModuleLibrary::CreateSpawnRateModule(50.0f),
				VfxModuleLibrary::CreateInitParticleModule(2.0f, 0.05f, 0.015f),
				VfxModuleLibrary::CreateRingSpawnModule(1.8f, 0.6f),
				VfxModuleLibrary::CreateVortexModule(),
				VfxModuleLibrary::CreateCurlNoiseModule(),
				VfxModuleLibrary::CreateGravityModule(),
				VfxModuleLibrary::CreateScaleOverLifeModule(0.05f, 0.015f),
				VfxModuleLibrary::CreateColorOverLifeModule(glm::vec4(0.90f, 0.96f, 1.0f, 1.0f), glm::vec4(0.40f, 0.70f, 1.0f, 0.0f)),
				VfxModuleLibrary::CreateSolveVelocityModule(),
				VfxModuleLibrary::CreateSpriteRendererModule(true, false, 3.6f)
			};
			if (auto* v = em.FindModule("mod.update.vortex")) v->SetFloat("VortexStrength", 4.2f);
			if (auto* n = em.FindModule("mod.update.curl_noise")) { n->SetFloat("NoiseStrength", 1.8f); n->SetFloat("NoiseFrequency", 1.4f); }
			if (auto* g = em.FindModule("mod.update.gravity")) g->SetVec4("Gravity", glm::vec4(0.0f, -0.6f, 0.0f, 0.0f));
			blizzardAsset.Emitters.push_back(em);
		}

	}

	// =========================================================================
	// Campfire
	// =========================================================================
	ParticleAsset campfireAsset;
	campfireAsset.Name = "Campfire";
	campfireAsset.Duration = 4.0f;
	campfireAsset.Looping = true;
	{
		// Emitter 1: GlowingCoalBed
		{
			ParticleEmitterLayer em;
			em.Name = "GlowingCoalBed";
			em.MaxParticles = 1;
			em.Lifetime = 9999.00f;
			em.EmissionRate = 0.00f;
			em.Speed = 0.00f;
			em.SpeedVariance = 0.00f;
			em.Gravity = glm::vec3(0.00f, 0.00f, 0.00f);
			em.LinearDrag = 0.00f;
			em.VortexStrength = 0.00f;
			em.EmitterType = ParticleEmitterType::Point;
			em.SizeStart = 0.750f;
			em.SizeEnd = 0.750f;
			em.ColorStart = glm::vec4(0.95f, 0.28f, 0.05f, 0.85f);
			em.ColorEnd = glm::vec4(0.95f, 0.28f, 0.05f, 0.85f);
			em.FacingMode = ParticleRenderMode::HorizontalQuad;
			em.Bursts = { { 0.00f, 1u } };
			em.Material.TexturePath = "textures/particles/flame_soft.wtex";
			em.Material.MaterialPath = "materials/particles/particle_additive.wmat";
			em.Material.BlendMode = ParticleBlendMode::Additive;
			em.Material.EmissiveBoost = 2.8f;
			em.Modules = {
				VfxModuleLibrary::CreateSpawnBurstModule(1, 0.0f),
				VfxModuleLibrary::CreateInitParticleModule(9999.0f, 0.75f, 0.75f),
				VfxModuleLibrary::CreatePointSpawnModule(),
				VfxModuleLibrary::CreateColorOverLifeModule(glm::vec4(0.95f, 0.28f, 0.05f, 0.85f), glm::vec4(0.95f, 0.28f, 0.05f, 0.85f)),
				VfxModuleLibrary::CreateSolveVelocityModule(),
				VfxModuleLibrary::CreateSpriteRendererModule(true, true, 2.8f)
			};
			if (auto* p = em.FindModule("mod.spawn.point")) p->SetFloat("PointSpeed", 0.0f);
			campfireAsset.Emitters.push_back(em);
		}

		// Emitter 2: FlameCore
		{
			ParticleEmitterLayer em;
			em.Name = "FlameCore";
			em.MaxParticles = 60;
			em.Lifetime = 0.75f;
			em.EmissionRate = 22.00f;
			em.Speed = 1.40f;
			em.SpeedVariance = 0.40f;
			em.Gravity = glm::vec3(0.00f, 2.00f, 0.00f);
			em.LinearDrag = 0.80f;
			em.VortexStrength = 0.00f;
			em.EmitterType = ParticleEmitterType::Cone;
			em.SizeStart = 0.550f;
			em.SizeEnd = 0.050f;
			em.ColorStart = glm::vec4(1.00f, 0.95f, 0.55f, 1.00f);
			em.ColorEnd = glm::vec4(0.85f, 0.15f, 0.00f, 0.00f);
			em.FacingMode = ParticleRenderMode::Billboard;
			em.Material.TexturePath = "textures/particles/fire_flipbook_4x4.wtex";
			em.Material.MaterialPath = "materials/particles/particle_additive.wmat";
			em.Material.BlendMode = ParticleBlendMode::Additive;
			em.Material.EmissiveBoost = 4.5f;
			em.Material.SubUVTilesX = 4;
			em.Material.SubUVTilesY = 4;
			em.Modules = {
				VfxModuleLibrary::CreateSpawnRateModule(22.0f),
				VfxModuleLibrary::CreateInitParticleModule(0.75f, 0.55f, 0.05f),
				VfxModuleLibrary::CreateConeSpawnModule(),
				VfxModuleLibrary::CreateGravityModule(),
				VfxModuleLibrary::CreateLinearDragModule(),
				VfxModuleLibrary::CreateFlipbookModule(4, 4, 18.0f, true),
				VfxModuleLibrary::CreateScaleOverLifeModule(0.55f, 0.05f),
				VfxModuleLibrary::CreateColorOverLifeModule(glm::vec4(1.0f, 0.95f, 0.55f, 1.0f), glm::vec4(0.85f, 0.15f, 0.0f, 0.0f)),
				VfxModuleLibrary::CreateSolveVelocityModule(),
				VfxModuleLibrary::CreateSpriteRendererModule(true, false, 4.5f)
			};
			if (auto* c = em.FindModule("mod.spawn.cone")) { c->SetFloat("ConeAngle", 10.0f); c->SetFloat("ConeRadius", 0.12f); c->SetFloat("InitialSpeed", 1.4f); }
			if (auto* g = em.FindModule("mod.update.gravity")) g->SetVec4("Gravity", glm::vec4(0.0f, 2.0f, 0.0f, 0.0f));
			if (auto* d = em.FindModule("mod.update.drag")) d->SetFloat("LinearDrag", 0.8f);
			campfireAsset.Emitters.push_back(em);
		}

		// Emitter 3: FlameTongues
		{
			ParticleEmitterLayer em;
			em.Name = "FlameTongues";
			em.MaxParticles = 50;
			em.Lifetime = 0.85f;
			em.EmissionRate = 18.00f;
			em.Speed = 1.60f;
			em.SpeedVariance = 0.50f;
			em.Gravity = glm::vec3(0.00f, 2.50f, 0.00f);
			em.LinearDrag = 0.60f;
			em.VortexStrength = 0.00f;
			em.EmitterType = ParticleEmitterType::Cone;
			em.SizeStart = 0.420f;
			em.SizeEnd = 0.060f;
			em.ColorStart = glm::vec4(1.00f, 0.55f, 0.08f, 0.90f);
			em.ColorEnd = glm::vec4(0.70f, 0.05f, 0.00f, 0.00f);
			em.FacingMode = ParticleRenderMode::Billboard;
			em.Material.TexturePath = "textures/particles/flame_soft.wtex";
			em.Material.MaterialPath = "materials/particles/particle_additive.wmat";
			em.Material.BlendMode = ParticleBlendMode::Additive;
			em.Material.EmissiveBoost = 3.6f;
			em.Modules = {
				VfxModuleLibrary::CreateSpawnRateModule(18.0f),
				VfxModuleLibrary::CreateInitParticleModule(0.85f, 0.42f, 0.06f),
				VfxModuleLibrary::CreateConeSpawnModule(),
				VfxModuleLibrary::CreateGravityModule(),
				VfxModuleLibrary::CreateCurlNoiseModule(),
				VfxModuleLibrary::CreateRotationOverLifeModule(1.0f, 0.0f),
				VfxModuleLibrary::CreateScaleOverLifeModule(0.42f, 0.06f),
				VfxModuleLibrary::CreateColorOverLifeModule(glm::vec4(1.0f, 0.55f, 0.08f, 0.9f), glm::vec4(0.70f, 0.05f, 0.0f, 0.0f)),
				VfxModuleLibrary::CreateSolveVelocityModule(),
				VfxModuleLibrary::CreateSpriteRendererModule(true, false, 3.6f)
			};
			if (auto* c = em.FindModule("mod.spawn.cone")) { c->SetFloat("ConeAngle", 14.0f); c->SetFloat("ConeRadius", 0.15f); c->SetFloat("InitialSpeed", 1.6f); }
			if (auto* g = em.FindModule("mod.update.gravity")) g->SetVec4("Gravity", glm::vec4(0.0f, 2.5f, 0.0f, 0.0f));
			if (auto* n = em.FindModule("mod.update.curl_noise")) { n->SetFloat("NoiseStrength", 1.8f); n->SetFloat("NoiseFrequency", 1.5f); }
			campfireAsset.Emitters.push_back(em);
		}

		// Emitter 4: FloatingEmbers
		{
			ParticleEmitterLayer em;
			em.Name = "FloatingEmbers";
			em.MaxParticles = 80;
			em.Lifetime = 1.50f;
			em.EmissionRate = 32.00f;
			em.Speed = 1.40f;
			em.SpeedVariance = 0.80f;
			em.Gravity = glm::vec3(0.00f, 1.20f, 0.00f);
			em.LinearDrag = 0.50f;
			em.VortexStrength = 0.00f;
			em.EmitterType = ParticleEmitterType::Cone;
			em.SizeStart = 0.045f;
			em.SizeEnd = 0.008f;
			em.ColorStart = glm::vec4(1.00f, 0.92f, 0.45f, 1.00f);
			em.ColorEnd = glm::vec4(0.95f, 0.18f, 0.00f, 0.00f);
			em.FacingMode = ParticleRenderMode::Billboard;
			em.Material.TexturePath = "textures/particles/spark_glow.wtex";
			em.Material.MaterialPath = "materials/particles/particle_additive.wmat";
			em.Material.BlendMode = ParticleBlendMode::Additive;
			em.Material.EmissiveBoost = 5.0f;
			em.Modules = {
				VfxModuleLibrary::CreateSpawnRateModule(32.0f),
				VfxModuleLibrary::CreateInitParticleModule(1.5f, 0.045f, 0.008f),
				VfxModuleLibrary::CreateConeSpawnModule(),
				VfxModuleLibrary::CreateGravityModule(),
				VfxModuleLibrary::CreateLinearDragModule(),
				VfxModuleLibrary::CreateCurlNoiseModule(),
				VfxModuleLibrary::CreateScaleOverLifeModule(0.045f, 0.008f),
				VfxModuleLibrary::CreateColorOverLifeModule(glm::vec4(1.0f, 0.92f, 0.45f, 1.0f), glm::vec4(0.95f, 0.18f, 0.0f, 0.0f)),
				VfxModuleLibrary::CreateSolveVelocityModule(),
				VfxModuleLibrary::CreateSpriteRendererModule(true, false, 5.0f)
			};
			if (auto* c = em.FindModule("mod.spawn.cone")) { c->SetFloat("ConeAngle", 18.0f); c->SetFloat("ConeRadius", 0.12f); c->SetFloat("InitialSpeed", 1.2f); }
			if (auto* g = em.FindModule("mod.update.gravity")) g->SetVec4("Gravity", glm::vec4(0.0f, 1.6f, 0.0f, 0.0f));
			if (auto* d = em.FindModule("mod.update.drag")) d->SetFloat("LinearDrag", 0.5f);
			if (auto* n = em.FindModule("mod.update.curl_noise")) { n->SetFloat("NoiseStrength", 2.0f); n->SetFloat("NoiseFrequency", 1.6f); }
			campfireAsset.Emitters.push_back(em);
		}

		// Emitter 5: RisingSmoke
		{
			ParticleEmitterLayer em;
			em.Name = "RisingSmoke";
			em.MaxParticles = 80;
			em.Lifetime = 2.50f;
			em.EmissionRate = 14.00f;
			em.Speed = 1.00f;
			em.SpeedVariance = 0.30f;
			em.Gravity = glm::vec3(0.00f, 1.40f, 0.00f);
			em.LinearDrag = 0.60f;
			em.VortexStrength = 0.00f;
			em.EmitterType = ParticleEmitterType::Cone;
			em.SizeStart = 0.350f;
			em.SizeEnd = 1.500f;
			em.ColorStart = glm::vec4(0.35f, 0.32f, 0.30f, 0.35f);
			em.ColorEnd = glm::vec4(0.15f, 0.15f, 0.15f, 0.00f);
			em.FacingMode = ParticleRenderMode::Billboard;
			em.Material.TexturePath = "textures/particles/smoke_puff.wtex";
			em.Material.MaterialPath = "materials/particles/particle_alphablend.wmat";
			em.Material.BlendMode = ParticleBlendMode::AlphaBlend;
			em.Material.EmissiveBoost = 0.5f;
			em.Modules = {
				VfxModuleLibrary::CreateSpawnRateModule(14.0f),
				VfxModuleLibrary::CreateInitParticleModule(2.5f, 0.35f, 1.50f),
				VfxModuleLibrary::CreateConeSpawnModule(),
				VfxModuleLibrary::CreateGravityModule(),
				VfxModuleLibrary::CreateLinearDragModule(),
				VfxModuleLibrary::CreateRotationOverLifeModule(0.6f, 0.0f),
				VfxModuleLibrary::CreateScaleOverLifeModule(0.35f, 1.50f),
				VfxModuleLibrary::CreateColorOverLifeModule(glm::vec4(0.35f, 0.32f, 0.30f, 0.35f), glm::vec4(0.15f, 0.15f, 0.15f, 0.0f)),
				VfxModuleLibrary::CreateSolveVelocityModule(),
				VfxModuleLibrary::CreateSpriteRendererModule(false, false, 0.5f)
			};
			if (auto* c = em.FindModule("mod.spawn.cone")) { c->SetFloat("ConeAngle", 16.0f); c->SetFloat("ConeRadius", 0.18f); c->SetFloat("InitialSpeed", 1.0f); }
			if (auto* g = em.FindModule("mod.update.gravity")) g->SetVec4("Gravity", glm::vec4(0.0f, 1.4f, 0.0f, 0.0f));
			if (auto* d = em.FindModule("mod.update.drag")) d->SetFloat("LinearDrag", 0.6f);
			campfireAsset.Emitters.push_back(em);
		}

	}

	
	// =======================================================================
	// =========================================================================
	// 7. HighEnergyLaserBeam3D (商业级重型科幻高能激光巨炮管线 - Mega Laser Cannon)
	// =========================================================================
	ParticleAsset laserAsset;
	laserAsset.Name = "HighEnergyLaserBeam3D";
	laserAsset.Duration = 4.0f;
	laserAsset.Looping = true;
	{
		// Emitter 1: ColossalLaserCore (巨型激光炮白炽主光柱，0.7m 毁天灭地巨型光柱)
		{
			ParticleEmitterLayer em;
			em.Name = "ColossalLaserCore";
			em.MaxParticles = 240;
			em.Lifetime = 0.08f;
			em.EmissionRate = 200.0f;
			em.Speed = 0.0f;
			em.Gravity = glm::vec3(0.0f);
			em.LinearDrag = 0.0f;
			em.SizeStart = 0.70f;
			em.SizeEnd = 0.70f;
			em.ColorStart = glm::vec4(1.0f, 1.0f, 1.0f, 1.0f);
			em.ColorEnd = glm::vec4(1.0f, 0.25f, 0.35f, 0.0f);
			em.FacingMode = ParticleRenderMode::StretchedVelocity;
			em.Material.TexturePath = "textures/particles/electric_beam.wtex";
			em.Material.MaterialPath = "materials/particles/particle_additive.wmat";
			em.Material.BlendMode = ParticleBlendMode::Additive;
			em.Material.EmissiveBoost = 9.5f;
			em.Modules = {
				VfxModuleLibrary::CreateSpawnRateModule(200.0f, 240),
				VfxModuleLibrary::CreateInitParticleModule(0.08f, 0.70f, 0.70f),
				VfxModuleLibrary::CreateBeamSpawnModule({ 0.0f, 0.4f, 0.0f }, { 0.0f, 0.4f, 8.0f }, 0.0f, 1.0f),
				VfxModuleLibrary::CreateColorOverLifeModule(glm::vec4(1.0f, 1.0f, 1.0f, 1.0f), glm::vec4(1.0f, 0.25f, 0.35f, 0.0f)),
				VfxModuleLibrary::CreateSolveVelocityModule(),
				VfxModuleLibrary::CreateSpriteRendererModule(true, false, 9.5f)
			};
			if (auto* r = em.FindModule("mod.render.sprite")) r->SetFloat("FacingMode", static_cast<float>(ParticleRenderMode::StretchedVelocity));
			laserAsset.Emitters.push_back(em);
		}

		// Emitter 2: PlasmaConduitVortex (外围高能等离子流光护套，1.6m 粗壮能量场)
		{
			ParticleEmitterLayer em;
			em.Name = "PlasmaConduitVortex";
			em.MaxParticles = 160;
			em.Lifetime = 0.08f;
			em.EmissionRate = 140.0f;
			em.Speed = 0.0f;
			em.Gravity = glm::vec3(0.0f);
			em.LinearDrag = 0.0f;
			em.SizeStart = 1.45f;
			em.SizeEnd = 1.65f;
			em.ColorStart = glm::vec4(1.0f, 0.15f, 0.35f, 0.95f);
			em.ColorEnd = glm::vec4(0.85f, 0.02f, 0.15f, 0.0f);
			em.FacingMode = ParticleRenderMode::StretchedVelocity;
			em.Material.TexturePath = "textures/particles/electric_beam.wtex";
			em.Material.MaterialPath = "materials/particles/particle_additive.wmat";
			em.Material.BlendMode = ParticleBlendMode::Additive;
			em.Material.EmissiveBoost = 6.5f;
			em.Modules = {
				VfxModuleLibrary::CreateSpawnRateModule(140.0f, 160),
				VfxModuleLibrary::CreateInitParticleModule(0.08f, 1.45f, 1.65f),
				VfxModuleLibrary::CreateBeamSpawnModule({ 0.0f, 0.4f, 0.0f }, { 0.0f, 0.4f, 8.0f }, 0.025f, 10.0f),
				VfxModuleLibrary::CreateColorOverLifeModule(glm::vec4(1.0f, 0.15f, 0.35f, 0.95f), glm::vec4(0.85f, 0.02f, 0.15f, 0.0f)),
				VfxModuleLibrary::CreateSolveVelocityModule(),
				VfxModuleLibrary::CreateSpriteRendererModule(true, false, 6.5f)
			};
			if (auto* r = em.FindModule("mod.render.sprite")) r->SetFloat("FacingMode", static_cast<float>(ParticleRenderMode::StretchedVelocity));
			laserAsset.Emitters.push_back(em);
		}

		// Emitter 3: ResonatorShockRings (沿炮轴高速喷射的聚焦激波能量环，18m/s 向前飞驰推动)
		{
			ParticleEmitterLayer em;
			em.Name = "ResonatorShockRings";
			em.MaxParticles = 32;
			em.Lifetime = 0.45f;
			em.EmissionRate = 20.0f;
			em.Speed = 18.0f;
			em.Gravity = glm::vec3(0.0f);
			em.LinearDrag = 0.0f;
			em.SizeStart = 0.85f;
			em.SizeEnd = 1.85f;
			em.ColorStart = glm::vec4(1.0f, 0.90f, 0.95f, 1.0f);
			em.ColorEnd = glm::vec4(1.0f, 0.15f, 0.30f, 0.0f);
			em.FacingMode = ParticleRenderMode::Billboard;
			em.Material.TexturePath = "textures/particles/shockwave_ring.wtex";
			em.Material.MaterialPath = "materials/particles/particle_additive.wmat";
			em.Material.BlendMode = ParticleBlendMode::Additive;
			em.Material.EmissiveBoost = 7.5f;
			em.Modules = {
				VfxModuleLibrary::CreateSpawnRateModule(20.0f, 32),
				VfxModuleLibrary::CreateInitParticleModule(0.45f, 0.85f, 1.85f),
				VfxModuleLibrary::CreateBeamSpawnModule({ 0.0f, 0.4f, 0.0f }, { 0.0f, 0.4f, 0.2f }, 0.0f, 1.0f),
				VfxModuleLibrary::CreateScaleOverLifeModule(0.85f, 1.85f),
				VfxModuleLibrary::CreateColorOverLifeModule(glm::vec4(1.0f, 0.90f, 0.95f, 1.0f), glm::vec4(1.0f, 0.15f, 0.30f, 0.0f)),
				VfxModuleLibrary::CreateSolveVelocityModule(),
				VfxModuleLibrary::CreateSpriteRendererModule(true, false, 7.5f)
			};
			laserAsset.Emitters.push_back(em);
		}

		// Emitter 4: MegaCannonMuzzleFlare (炮口超充能高亮巨型能量球与星芒，1.85m 巨型白热光斑)
		{
			ParticleEmitterLayer em;
			em.Name = "MegaCannonMuzzleFlare";
			em.MaxParticles = 1;
			em.Lifetime = 9999.0f;
			em.EmissionRate = 0.0f;
			em.Speed = 0.0f;
			em.Gravity = glm::vec3(0.0f);
			em.LinearDrag = 0.0f;
			em.SizeStart = 1.85f;
			em.SizeEnd = 1.85f;
			em.ColorStart = glm::vec4(1.0f, 0.95f, 0.98f, 1.0f);
			em.ColorEnd = glm::vec4(1.0f, 0.95f, 0.98f, 1.0f);
			em.FacingMode = ParticleRenderMode::Billboard;
			em.Bursts = { { 0.0f, 1u } };
			em.Material.TexturePath = "textures/particles/spark_glow.wtex";
			em.Material.MaterialPath = "materials/particles/particle_additive.wmat";
			em.Material.BlendMode = ParticleBlendMode::Additive;
			em.Material.EmissiveBoost = 9.0f;
			em.Modules = {
				VfxModuleLibrary::CreateSpawnBurstModule(1, 0.0f),
				VfxModuleLibrary::CreateInitParticleModule(9999.0f, 1.85f, 1.85f),
				VfxModuleLibrary::CreatePointSpawnModule(),
				VfxModuleLibrary::CreateColorOverLifeModule(glm::vec4(1.0f, 0.95f, 0.98f, 1.0f), glm::vec4(1.0f, 0.95f, 0.98f, 1.0f)),
				VfxModuleLibrary::CreateSolveVelocityModule(),
				VfxModuleLibrary::CreateSpriteRendererModule(true, false, 9.0f)
			};
			if (auto* p = em.FindModule("mod.spawn.point")) p->SetFloat("PointSpeed", 0.0f);
			laserAsset.Emitters.push_back(em);
		}

		// Emitter 5: MuzzlePlasmaDischarge (炮口高能等离子爆发溢出扇面)
		{
			ParticleEmitterLayer em;
			em.Name = "MuzzlePlasmaDischarge";
			em.MaxParticles = 60;
			em.Lifetime = 0.35f;
			em.EmissionRate = 50.0f;
			em.Speed = 5.5f;
			em.SpeedVariance = 2.0f;
			em.Gravity = glm::vec3(0.0f, 0.5f, 0.0f);
			em.LinearDrag = 2.0f;
			em.EmitterType = ParticleEmitterType::Cone;
			em.SizeStart = 0.45f;
			em.SizeEnd = 1.25f;
			em.ColorStart = glm::vec4(1.0f, 0.30f, 0.45f, 0.85f);
			em.ColorEnd = glm::vec4(0.80f, 0.05f, 0.15f, 0.0f);
			em.FacingMode = ParticleRenderMode::Billboard;
			em.Material.TexturePath = "textures/particles/flame_soft.wtex";
			em.Material.MaterialPath = "materials/particles/particle_additive.wmat";
			em.Material.BlendMode = ParticleBlendMode::Additive;
			em.Material.EmissiveBoost = 5.5f;
			em.Modules = {
				VfxModuleLibrary::CreateSpawnRateModule(50.0f, 60),
				VfxModuleLibrary::CreateInitParticleModule(0.35f, 0.45f, 1.25f),
				VfxModuleLibrary::CreateConeSpawnModule(),
				VfxModuleLibrary::CreateGravityModule(),
				VfxModuleLibrary::CreateLinearDragModule(),
				VfxModuleLibrary::CreateScaleOverLifeModule(0.45f, 1.25f),
				VfxModuleLibrary::CreateColorOverLifeModule(glm::vec4(1.0f, 0.30f, 0.45f, 0.85f), glm::vec4(0.80f, 0.05f, 0.15f, 0.0f)),
				VfxModuleLibrary::CreateSolveVelocityModule(),
				VfxModuleLibrary::CreateSpriteRendererModule(true, false, 5.5f)
			};
			if (auto* c = em.FindModule("mod.spawn.cone")) { c->SetFloat("ConeAngle", 45.0f); c->SetFloat("InitialSpeed", 5.5f); }
			laserAsset.Emitters.push_back(em);
		}

		// Emitter 6: TargetThermalExplosion (靶点灾难级等离子爆轰火球，2.4m 白炽爆破)
		{
			ParticleEmitterLayer em;
			em.Name = "TargetThermalExplosion";
			em.MaxParticles = 60;
			em.Lifetime = 0.30f;
			em.EmissionRate = 40.0f;
			em.Speed = 0.0f;
			em.Gravity = glm::vec3(0.0f);
			em.LinearDrag = 0.0f;
			em.SizeStart = 0.85f;
			em.SizeEnd = 2.40f;
			em.ColorStart = glm::vec4(1.0f, 1.0f, 0.95f, 1.0f);
			em.ColorEnd = glm::vec4(1.0f, 0.20f, 0.10f, 0.0f);
			em.FacingMode = ParticleRenderMode::Billboard;
			em.Material.TexturePath = "textures/particles/flame_soft.wtex";
			em.Material.MaterialPath = "materials/particles/particle_additive.wmat";
			em.Material.BlendMode = ParticleBlendMode::Additive;
			em.Material.EmissiveBoost = 9.0f;
			em.Modules = {
				VfxModuleLibrary::CreateSpawnRateModule(40.0f, 60),
				VfxModuleLibrary::CreateInitParticleModule(0.30f, 0.85f, 2.40f),
				VfxModuleLibrary::CreateBeamSpawnModule({ 0.0f, 0.4f, 8.0f }, { 0.0f, 0.4f, 8.0f }, 0.0f, 1.0f),
				VfxModuleLibrary::CreateScaleOverLifeModule(0.85f, 2.40f),
				VfxModuleLibrary::CreateColorOverLifeModule(glm::vec4(1.0f, 1.0f, 0.95f, 1.0f), glm::vec4(1.0f, 0.20f, 0.10f, 0.0f)),
				VfxModuleLibrary::CreateSolveVelocityModule(),
				VfxModuleLibrary::CreateSpriteRendererModule(true, false, 9.0f)
			};
			laserAsset.Emitters.push_back(em);
		}

		// Emitter 7: TargetHypervelocitySparks (靶点气化超高速熔融火星群，12m/s 暴烈飞溅)
		{
			ParticleEmitterLayer em;
			em.Name = "TargetHypervelocitySparks";
			em.MaxParticles = 180;
			em.Lifetime = 0.40f;
			em.EmissionRate = 240.0f;
			em.Speed = 12.0f;
			em.SpeedVariance = 4.0f;
			em.Gravity = glm::vec3(0.0f, -14.0f, 0.0f);
			em.LinearDrag = 1.4f;
			em.SizeStart = 0.08f;
			em.SizeEnd = 0.012f;
			em.ColorStart = glm::vec4(1.0f, 0.95f, 0.65f, 1.0f);
			em.ColorEnd = glm::vec4(1.0f, 0.15f, 0.0f, 0.0f);
			em.FacingMode = ParticleRenderMode::StretchedVelocity;
			em.Material.TexturePath = "textures/particles/spark_glow.wtex";
			em.Material.MaterialPath = "materials/particles/particle_additive.wmat";
			em.Material.BlendMode = ParticleBlendMode::Additive;
			em.Material.EmissiveBoost = 7.5f;
			em.Modules = {
				VfxModuleLibrary::CreateSpawnRateModule(240.0f, 180),
				VfxModuleLibrary::CreateInitParticleModule(0.40f, 0.08f, 0.012f),
				VfxModuleLibrary::CreateBeamSpawnModule({ 0.0f, 0.4f, 8.0f }, { 0.0f, 0.4f, 8.0f }, 0.0f, 1.0f),
				VfxModuleLibrary::CreateGravityModule(),
				VfxModuleLibrary::CreateLinearDragModule(),
				VfxModuleLibrary::CreateScaleOverLifeModule(0.08f, 0.012f),
				VfxModuleLibrary::CreateColorOverLifeModule(glm::vec4(1.0f, 0.95f, 0.65f, 1.0f), glm::vec4(1.0f, 0.15f, 0.0f, 0.0f)),
				VfxModuleLibrary::CreateSolveVelocityModule(),
				VfxModuleLibrary::CreateSpriteRendererModule(true, false, 7.5f)
			};
			if (auto* g = em.FindModule("mod.update.gravity")) g->SetVec4("Gravity", glm::vec4(0.0f, -14.0f, 0.0f, 0.0f));
			if (auto* d = em.FindModule("mod.update.drag")) d->SetFloat("LinearDrag", 1.4f);
			if (auto* r = em.FindModule("mod.render.sprite")) r->SetFloat("FacingMode", static_cast<float>(ParticleRenderMode::StretchedVelocity));
			laserAsset.Emitters.push_back(em);
		}

		// Emitter 8: TargetIonPlume (靶点离子羽流微烟)
		{
			ParticleEmitterLayer em;
			em.Name = "TargetIonPlume";
			em.MaxParticles = 40;
			em.Lifetime = 0.95f;
			em.EmissionRate = 25.0f;
			em.Speed = 0.8f;
			em.SpeedVariance = 0.3f;
			em.Gravity = glm::vec3(0.0f, 1.2f, 0.0f);
			em.LinearDrag = 1.0f;
			em.SizeStart = 0.35f;
			em.SizeEnd = 1.40f;
			em.ColorStart = glm::vec4(0.35f, 0.25f, 0.38f, 0.40f);
			em.ColorEnd = glm::vec4(0.12f, 0.08f, 0.15f, 0.0f);
			em.FacingMode = ParticleRenderMode::Billboard;
			em.Material.TexturePath = "textures/particles/smoke_puff.wtex";
			em.Material.MaterialPath = "materials/particles/particle_alpha.wmat";
			em.Material.BlendMode = ParticleBlendMode::AlphaBlend;
			em.Material.EmissiveBoost = 0.8f;
			em.Modules = {
				VfxModuleLibrary::CreateSpawnRateModule(25.0f, 40),
				VfxModuleLibrary::CreateInitParticleModule(0.95f, 0.35f, 1.40f),
				VfxModuleLibrary::CreateBeamSpawnModule({ 0.0f, 0.4f, 8.0f }, { 0.0f, 0.4f, 8.0f }, 0.0f, 1.0f),
				VfxModuleLibrary::CreateGravityModule(),
				VfxModuleLibrary::CreateLinearDragModule(),
				VfxModuleLibrary::CreateRotationOverLifeModule(0.5f, 0.0f),
				VfxModuleLibrary::CreateScaleOverLifeModule(0.35f, 1.40f),
				VfxModuleLibrary::CreateColorOverLifeModule(glm::vec4(0.35f, 0.25f, 0.38f, 0.40f), glm::vec4(0.12f, 0.08f, 0.15f, 0.0f)),
				VfxModuleLibrary::CreateSolveVelocityModule(),
				VfxModuleLibrary::CreateSpriteRendererModule(false, false, 0.8f)
			};
			if (auto* g = em.FindModule("mod.update.gravity")) g->SetVec4("Gravity", glm::vec4(0.0f, 1.2f, 0.0f, 0.0f));
			if (auto* d = em.FindModule("mod.update.drag")) d->SetFloat("LinearDrag", 1.0f);
			laserAsset.Emitters.push_back(em);
		}
	}

	const std::pair<const char*, const ParticleAsset*> allAssets[] = {
		{ "orbital_magic_shield_3d.wvfx", &shieldAsset },
		{ "flame_tornado_3d.wvfx", &tornadoAsset },
		{ "meteor_impact_explosion_3d.wvfx", &meteorAsset },
		{ "lightning_plasma_beam_3d.wvfx", &beamAsset },
		{ "blizzard_frost_nova_3d.wvfx", &blizzardAsset },
		{ "campfire.wvfx", &campfireAsset },
		{ "high_energy_laser_beam_3d.wvfx", &laserAsset }
	};

	std::error_code ec;
	std::filesystem::path repoRoot = ".";
	for (int up = 0; up < 5; ++up)
	{
		if (std::filesystem::exists(repoRoot / "CMakeLists.txt") && std::filesystem::exists(repoRoot / "Engine/content/vfx")) break;
		repoRoot = repoRoot / "..";
	}
	std::filesystem::create_directories(repoRoot / "Engine/content/vfx", ec);
	std::filesystem::create_directories(repoRoot / "tests/fixtures/content/vfx", ec);

	for (const auto& [filename, pAsset] : allAssets)
	{
		std::string enginePath = (repoRoot / "Engine/content/vfx" / filename).generic_string();
		std::string fixturePath = (repoRoot / "tests/fixtures/content/vfx" / filename).generic_string();

		bool ok1 = ParticleAssetSerializer::Serialize(*pAsset, enginePath);
		assert(ok1 && "Failed to serialize to engine content vfx");

		bool ok2 = ParticleAssetSerializer::Serialize(*pAsset, fixturePath);
		assert(ok2 && "Failed to serialize to tests fixture vfx");

		ParticleAsset roundtrip;
		bool ok3 = ParticleAssetSerializer::Deserialize(enginePath, roundtrip);
		assert(ok3 && "Failed to deserialize serialized commercial vfx");
		assert(roundtrip.Emitters.size() == pAsset->Emitters.size());
		for (size_t i = 0; i < roundtrip.Emitters.size(); ++i)
		{
			assert(!roundtrip.Emitters[i].Modules.empty());
			assert(roundtrip.Emitters[i].Modules.size() >= 4);
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

static void TestNiagaraMultiChannelParametersAndCurves()
{
	std::cout << "Testing Niagara Multi-Channel Parameter Splitting and Multi-Curve Evaluation...\n";

	// 1. 向量参数：整体多线曲线控制 vs 拆解 3 通道数 (X / Y / Z)
	{
		VfxModuleParam vecParam;
		vecParam.Name = "Velocity";
		vecParam.Type = VfxParamType::Vec3;
		vecParam.VecVal = glm::vec4(10.0f, 20.0f, 30.0f, 0.0f);
		vecParam.Mode = VfxParamMode::Curve;
		vecParam.EnsureAxisCurves();

		// 设置 X 曲线 (10 -> 20), Y 曲线 (0 -> 10), Z 曲线 (30 -> 10)
		vecParam.AxisCurve[0] = CubicBezierCurve::Linear();
		vecParam.AxisCurve[0].GetKeyframeMut(0).Value = 10.0f;
		vecParam.AxisCurve[0].GetKeyframeMut(1).Value = 20.0f;
		vecParam.AxisCurve[0].PrecomputeLut(256);

		vecParam.AxisCurve[1] = CubicBezierCurve::Linear();
		vecParam.AxisCurve[1].GetKeyframeMut(0).Value = 0.0f;
		vecParam.AxisCurve[1].GetKeyframeMut(1).Value = 10.0f;
		vecParam.AxisCurve[1].PrecomputeLut(256);

		vecParam.AxisCurve[2] = CubicBezierCurve::Linear();
		vecParam.AxisCurve[2].GetKeyframeMut(0).Value = 30.0f;
		vecParam.AxisCurve[2].GetKeyframeMut(1).Value = 10.0f;
		vecParam.AxisCurve[2].PrecomputeLut(256);

		glm::vec3 vMid = vecParam.EvaluateVec3(0.5f);
		assert(std::abs(vMid.x - 15.0f) < 0.2f);
		assert(std::abs(vMid.y - 5.0f) < 0.2f);
		assert(std::abs(vMid.z - 20.0f) < 0.2f);

		// 拆解模式
		vecParam.SplitAxes = true;
		vecParam.AxisMode[0] = VfxParamMode::Constant;
		vecParam.VecVal.x = 99.0f;
		glm::vec3 vSplit = vecParam.EvaluateVec3(0.5f);
		assert(std::abs(vSplit.x - 99.0f) < 0.01f);
	}

	// 2. 颜色参数：整体四线曲线控制 vs 拆解 4 通道数 (R / G / B / A)
	{
		VfxModuleParam colParam;
		colParam.Name = "ColorStart";
		colParam.Type = VfxParamType::Vec4;
		colParam.VecVal = glm::vec4(1.0f, 0.5f, 0.2f, 1.0f);
		colParam.Mode = VfxParamMode::Curve;
		colParam.EnsureColorCurves();

		// R (1 -> 0), G (0.5 -> 1), B (0.2 -> 0.8), A (1 -> 0)
		colParam.ColorCurves[0] = CubicBezierCurve::Linear();
		colParam.ColorCurves[0].GetKeyframeMut(0).Value = 1.0f;
		colParam.ColorCurves[0].GetKeyframeMut(1).Value = 0.0f;
		colParam.ColorCurves[0].PrecomputeLut(256);

		colParam.ColorCurves[1] = CubicBezierCurve::Linear();
		colParam.ColorCurves[1].GetKeyframeMut(0).Value = 0.5f;
		colParam.ColorCurves[1].GetKeyframeMut(1).Value = 1.0f;
		colParam.ColorCurves[1].PrecomputeLut(256);

		colParam.ColorCurves[2] = CubicBezierCurve::Linear();
		colParam.ColorCurves[2].GetKeyframeMut(0).Value = 0.2f;
		colParam.ColorCurves[2].GetKeyframeMut(1).Value = 0.8f;
		colParam.ColorCurves[2].PrecomputeLut(256);

		colParam.ColorCurves[3] = CubicBezierCurve::Linear();
		colParam.ColorCurves[3].GetKeyframeMut(0).Value = 1.0f;
		colParam.ColorCurves[3].GetKeyframeMut(1).Value = 0.0f;
		colParam.ColorCurves[3].PrecomputeLut(256);

		glm::vec4 cMid = colParam.EvaluateColor(0.5f);
		assert(std::abs(cMid.r - 0.5f) < 0.1f);
		assert(std::abs(cMid.g - 0.75f) < 0.1f);
		assert(std::abs(cMid.b - 0.5f) < 0.1f);
		assert(std::abs(cMid.a - 0.5f) < 0.1f);

		// 拆解模式
		colParam.SplitColor = true;
		colParam.ColorMode[0] = VfxParamMode::Constant;
		colParam.VecVal.r = 0.88f;
		glm::vec4 cSplit = colParam.EvaluateColor(0.5f);
		assert(std::abs(cSplit.r - 0.88f) < 0.01f);
	}

	// 3. YAML 序列化与反序列化 Roundtrip 校验
	{
		ParticleAsset asset;
		asset.Name = "MultiChannelTest";
		ParticleEmitterLayer em;
		em.Name = "Emitter0";

		VfxModule testMod;
		testMod.Id = "mod.test.multichannel";
		testMod.Name = "多通道参数测试模块";

		VfxModuleParam pVec;
		pVec.Name = "ForceVec";
		pVec.Type = VfxParamType::Vec3;
		pVec.SplitAxes = true;
		pVec.AxisMode[0] = VfxParamMode::Curve;
		pVec.AxisMode[1] = VfxParamMode::RandomRange;
		pVec.AxisRandomMin[1] = -5.0f;
		pVec.AxisRandomMax[1] = 5.0f;
		pVec.EnsureAxisCurves();

		VfxModuleParam pCol;
		pCol.Name = "ParticleColor";
		pCol.Type = VfxParamType::Vec4;
		pCol.SplitColor = true;
		pCol.ColorMode[0] = VfxParamMode::Curve;
		pCol.ColorMode[3] = VfxParamMode::Curve;
		pCol.EnsureColorCurves();

		testMod.Parameters.push_back(pVec);
		testMod.Parameters.push_back(pCol);
		em.Modules.push_back(testMod);
		asset.Emitters.push_back(em);

		std::string yaml = ParticleAssetSerializer::SerializeToString(asset);
		ParticleAsset loaded;
		bool ok = ParticleAssetSerializer::DeserializeFromString(yaml, loaded);
		assert(ok);
		assert(loaded.Emitters.size() == 1);
		assert(loaded.Emitters[0].Modules.size() == 1);

		const auto& loadedParams = loaded.Emitters[0].Modules[0].Parameters;
		assert(loadedParams.size() == 2);
		assert(loadedParams[0].SplitAxes == true);
		assert(loadedParams[0].AxisMode[0] == VfxParamMode::Curve);
		assert(loadedParams[0].AxisMode[1] == VfxParamMode::RandomRange);
		assert(std::abs(loadedParams[0].AxisRandomMin[1] - (-5.0f)) < 0.01f);

		assert(loadedParams[1].SplitColor == true);
		assert(loadedParams[1].ColorMode[0] == VfxParamMode::Curve);
		assert(loadedParams[1].ColorMode[3] == VfxParamMode::Curve);
	}

	std::cout << "[PASS] TestNiagaraMultiChannelParametersAndCurves\n";
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
	TestNiagaraMultiChannelParametersAndCurves();

	std::cout << "All WorldParticleAssetTests passed successfully.\n";
	return 0;
}
