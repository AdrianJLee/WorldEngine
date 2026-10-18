#pragma once

#include "World/Core/Export.h"
#include "World/Renderer/ParticleSystem.h"

#include <string>
#include <vector>
#include <unordered_map>
#include <memory>
#include <cstdint>

namespace World
{
	struct ParticleEmitterLayer
	{
		std::string Name = "Emitter";
		uint32_t MaxParticles = 500;
		float Lifetime = 2.0f;
		float EmissionRate = 50.0f;
		ParticleEmitterType EmitterType = ParticleEmitterType::Point;
		bool Enabled = true;
		ParticleRenderMode FacingMode = ParticleRenderMode::Billboard;
		bool LockToXY = false;

		ParticleMaterialSettings Material;
		ParticleCollisionSettings Collision;

		glm::vec3 Gravity { 0.0f, -9.8f, 0.0f };
		float LinearDrag = 0.0f;
		float VortexStrength = 0.0f;
		glm::vec3 VortexAxis { 0.0f, 1.0f, 0.0f };
		float TurbulenceStrength = 0.0f;
		float TurbulenceFrequency = 1.0f;
		float Speed = 5.0f;
		float SpeedVariance = 1.0f;
		float SizeStart = 0.5f;
		float SizeEnd = 0.1f;
		glm::vec4 ColorStart { 1.0f };
		glm::vec4 ColorEnd { 1.0f, 1.0f, 1.0f, 0.0f };

		std::vector<ParticleBurst> Bursts;
		std::vector<SubEmitterConfig> SubEmitters;
	};

	struct ParticleAsset
	{
		uint64_t Guid = 0;
		std::string Name = "UnnamedVFX";
		std::string AssetPath;
		uint32_t Seed = 1337;
		float Duration = 0.0f;
		bool Looping = true;
		float WarmupTime = 0.0f;
		float CullDistance = 60.0f;
		bool PauseWhenOffscreen = true;

		std::unordered_map<std::string, float> UserParamsFloat;
		std::unordered_map<std::string, glm::vec4> UserParamsVec4;

		std::vector<ParticleEmitterLayer> Emitters;
	};

	class WLD_API ParticleAssetSerializer
	{
	public:
		static bool Serialize(const ParticleAsset& asset, const std::string& filepath);
		static bool Deserialize(const std::string& filepath, ParticleAsset& outAsset);
		static std::string SerializeToString(const ParticleAsset& asset);
		static bool DeserializeFromString(const std::string& yamlString, ParticleAsset& outAsset);
	};

	class WLD_API ParticleAssetRegistry
	{
	public:
		static ParticleAssetRegistry& Get();

		uint64_t RegisterAsset(const std::string& filepath);
		uint64_t RegisterAssetFromMemory(const std::string& name, const ParticleAsset& asset);
		const ParticleAsset* GetAsset(uint64_t guid) const;
		ParticleAsset* GetAssetMut(uint64_t guid);
		const ParticleAsset* GetAssetByPath(const std::string& path) const;
		void Clear();
		size_t Count() const { return m_Assets.size(); }

	private:
		ParticleAssetRegistry() = default;
		std::unordered_map<uint64_t, ParticleAsset> m_Assets;
		std::unordered_map<std::string, uint64_t> m_PathToGuid;
	};
}
