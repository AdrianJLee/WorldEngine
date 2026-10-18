#include "wldpch.h"
#include "World/Renderer/ParticleAsset.h"
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

int main()
{
	Log::Init();
	std::cout << "Running WorldParticleAssetTests...\n";

	TestParticleAssetSerializationRoundtrip();
	TestParticleAssetBackwardCompatibilitySingleEmitter();
	TestParticleAssetRegistry();

	std::cout << "All WorldParticleAssetTests passed successfully.\n";
	return 0;
}
