#pragma once

#include "World/Core/Export.h"
#include "World/Scene/Components.h"
#include "World/RHI/Rhi.h"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <string>
#include <vector>
#include <unordered_map>
#include <memory>
#include <cstdint>

namespace World
{
	// M1 Material, Sub-UV Flipbook, Soft Particle & HDR Emissive settings per emitter/pool
	struct ParticleMaterialSettings
	{
		std::string TexturePath;
		Rhi::Handle<Rhi::Texture> TextureHandle {};
		uint32_t SubUVTilesX = 1;
		uint32_t SubUVTilesY = 1;
		float SubUVFrameRate = 0.0f;       // 0 = map lifetime [0,1] across total frames; >0 = fixed FPS
		bool SubUVRandomStartFrame = false;
		ParticleBlendMode BlendMode = ParticleBlendMode::AlphaBlend;
		bool SoftParticlesEnabled = false;
		float SoftParticleFadeDistance = 0.5f;
		float EmissiveBoost = 1.0f;        // HDR multiplier (>= 1.0 triggers bloom)
		int32_t SortingLayer = 0;
	};

	struct Particle
	{
		glm::vec3 Position { 0.0f };
		glm::vec3 Velocity { 0.0f };
		glm::vec3 Acceleration { 0.0f };
		glm::vec4 Color { 1.0f };
		glm::vec2 Size { 1.0f };
		float Rotation = 0.0f;
		float AngularVelocity = 0.0f;
		float Life = 0.0f;
		float MaxLife = 1.0f;
		uint32_t SubUVFrameIndex = 0;
		uint32_t InitialFrameOffset = 0;
		bool Active = false;
	};

	struct ParticleVertex
	{
		glm::vec3 Position { 0.0f };
		glm::vec4 Color { 1.0f };
		glm::vec2 UV { 0.0f };
		float SoftFadeFactor = 1.0f;
	};

	// Helper to compute Sub-UV (U_min, V_min, U_max, V_max) for a given frame index and grid
	WLD_API glm::vec4 ComputeSubUVRect(uint32_t frameIndex, uint32_t tilesX, uint32_t tilesY);

	// Helper to compute soft particle depth fade factor in [0, 1]: saturate((sceneDepth - particleDepth) / fadeDist)
	WLD_API float ComputeSoftParticleFade(float sceneDepth, float particleDepth, float fadeDistance);

	inline glm::vec4 UnpackColor(uint32_t c)
	{
		return glm::vec4(
			static_cast<float>(c & 0xFF) / 255.0f,
			static_cast<float>((c >> 8) & 0xFF) / 255.0f,
			static_cast<float>((c >> 16) & 0xFF) / 255.0f,
			static_cast<float>((c >> 24) & 0xFF) / 255.0f
		);
	}

	class WLD_API ParticlePool
	{
	public:
		explicit ParticlePool(size_t capacity = 1000);
		~ParticlePool() = default;

		ParticlePool(const ParticlePool&) = delete;
		ParticlePool& operator=(const ParticlePool&) = delete;
		ParticlePool(ParticlePool&&) noexcept = default;
		ParticlePool& operator=(ParticlePool&&) noexcept = default;

		size_t Capacity() const { return m_Particles.size(); }
		size_t ActiveCount() const { return m_ActiveCount; }

		void Reset();
		void SetSeed(uint32_t seed);
		uint32_t GetSeed() const { return m_InitialSeed; }

		void SetMaterialSettings(const ParticleMaterialSettings& settings) { m_Material = settings; }
		const ParticleMaterialSettings& GetMaterialSettings() const { return m_Material; }
		ParticleMaterialSettings& GetMaterialSettings() { return m_Material; }
		ParticleMaterialSettings& GetMaterialSettingsMut() { return m_Material; }

		void Emit(size_t count, const ParticleSystemComponent& comp, const glm::vec3& emitterPos, const glm::quat& emitterRot);
		void Emit(size_t count, const ParticleSystemComponent& comp, const glm::vec3& emitterPos, const glm::vec3& emitterRotEuler = glm::vec3(0.0f));

		void Update(float dt, const ParticleSystemComponent& comp, const glm::vec3& emitterPos);
		void Update(float dt, const ParticleSystemComponent& comp, const glm::vec3& emitterPos, const glm::quat& emitterRot);

		void GenerateQuadVertices(std::vector<ParticleVertex>& outVertices) const;
		void GenerateBillboardVertices(const glm::vec3& cameraPos, const glm::vec3& cameraUp, std::vector<ParticleVertex>& outVertices) const;
		void GenerateBillboardVerticesWithDepth(const glm::vec3& cameraPos, const glm::vec3& cameraUp, const glm::vec3& cameraForward,
			float sceneReferenceDepth, std::vector<ParticleVertex>& outVertices) const;

		const std::vector<Particle>& GetParticles() const { return m_Particles; }
		std::vector<Particle>& GetParticlesMut() { return m_Particles; }

	private:
		std::vector<Particle> m_Particles;
		ParticleMaterialSettings m_Material {};
		size_t m_ActiveCount = 0;
		size_t m_NextFreeIndex = 0;
		float m_EmissionAccumulator = 0.0f;
		uint32_t m_InitialSeed = 0x12345678;
		uint32_t m_RngState = 0x12345678;

		float RandomFloat();
		float RandomFloatSigned();
		glm::vec3 RandomUnitVector();
	};

	class WLD_API ParticleSystem
	{
	public:
		ParticleSystem() = default;
		~ParticleSystem() = default;

		ParticleSystem(const ParticleSystem&) = delete;
		ParticleSystem& operator=(const ParticleSystem&) = delete;
		ParticleSystem(ParticleSystem&&) noexcept = default;
		ParticleSystem& operator=(ParticleSystem&&) noexcept = default;

		ParticlePool& GetPoolForEntity(uint32_t entityId, size_t capacity = 1000);
		bool HasPoolForEntity(uint32_t entityId) const;
		ParticlePool* GetPool(uint32_t entityId);
		const ParticlePool* GetPool(uint32_t entityId) const;
		void RemoveEntity(uint32_t entityId);
		void ClearAll();
		size_t PoolCount() const { return m_Pools.size(); }

	private:
		std::unordered_map<uint32_t, std::unique_ptr<ParticlePool>> m_Pools;
	};
}
