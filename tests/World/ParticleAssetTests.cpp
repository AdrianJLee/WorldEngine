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
	const char* presetFiles[] = {
		"tests/fixtures/content/vfx/flame_tornado_3d.wvfx",
		"tests/fixtures/content/vfx/lightning_plasma_beam_3d.wvfx",
		"tests/fixtures/content/vfx/orbital_magic_shield_3d.wvfx",
		"tests/fixtures/content/vfx/meteor_impact_explosion_3d.wvfx",
		"tests/fixtures/content/vfx/blizzard_frost_nova_3d.wvfx"
	};

	for (const char* file : presetFiles)
	{
		ParticleAsset asset;
		bool ok = ParticleAssetSerializer::Deserialize(file, asset);
		assert(ok && "Preset file must deserialize successfully");
		assert(!asset.Name.empty());
		assert(!asset.Emitters.empty());
		for (const auto& em : asset.Emitters)
		{
			assert(!em.Name.empty());
			assert(em.MaxParticles > 0);
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
