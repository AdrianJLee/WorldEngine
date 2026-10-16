#pragma once

#include "World/Core/Export.h"
#include "World/Scene/Components.h"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <vector>
#include <unordered_map>
#include <memory>
#include <cstdint>

namespace World
{
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
		bool Active = false;
	};

	struct ParticleVertex
	{
		glm::vec3 Position { 0.0f };
		glm::vec4 Color { 1.0f };
		glm::vec2 UV { 0.0f };
	};

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

		void Emit(size_t count, const ParticleSystemComponent& comp, const glm::vec3& emitterPos, const glm::quat& emitterRot);
		void Emit(size_t count, const ParticleSystemComponent& comp, const glm::vec3& emitterPos, const glm::vec3& emitterRotEuler = glm::vec3(0.0f));

		void Update(float dt, const ParticleSystemComponent& comp, const glm::vec3& emitterPos);

		void GenerateQuadVertices(std::vector<ParticleVertex>& outVertices) const;
		void GenerateBillboardVertices(const glm::vec3& cameraPos, const glm::vec3& cameraUp, std::vector<ParticleVertex>& outVertices) const;

		const std::vector<Particle>& GetParticles() const { return m_Particles; }
		std::vector<Particle>& GetParticlesMut() { return m_Particles; }

	private:
		std::vector<Particle> m_Particles;
		size_t m_ActiveCount = 0;
		size_t m_NextFreeIndex = 0;
		float m_EmissionAccumulator = 0.0f;
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
		void RemoveEntity(uint32_t entityId);
		void ClearAll();
		size_t PoolCount() const { return m_Pools.size(); }

	private:
		std::unordered_map<uint32_t, std::unique_ptr<ParticlePool>> m_Pools;
	};
}
