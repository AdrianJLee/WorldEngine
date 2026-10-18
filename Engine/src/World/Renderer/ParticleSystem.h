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
#include <algorithm>

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

	enum class ParticleCollisionMode : uint8_t
	{
		None = 0,
		Plane = 1,
		WorldRaycast = 2
	};

	struct ParticleCollisionSettings
	{
		ParticleCollisionMode Mode = ParticleCollisionMode::None;
		float Restitution = 0.5f;       // 反弹弹性 (0=无反弹, 1=完全弹性)
		float Friction = 0.1f;          // 切向摩擦阻尼 (0=无摩擦, 1=完全阻滞)
		bool KillOnHit = false;         // 碰撞后粒子立即消亡
		bool EnableEvents = false;      // 通信开关: 是否捕获并入队碰撞事件 (默认关闭, 零队列开销)
		glm::vec3 PlaneOrigin { 0.0f }; // 平面点
		glm::vec3 PlaneNormal { 0.0f, 1.0f, 0.0f }; // 平面法线
		uint32_t CollisionLayerMask = 0xFFFFFFFF;   // 场景射线图层掩码
		float DamageAmount = 0.0f;      // 碰撞伤害值
	};

	struct ParticleHitRecord
	{
		glm::vec3 Position { 0.0f };
		glm::vec3 Normal { 0.0f, 1.0f, 0.0f };
		glm::vec3 Velocity { 0.0f };
		uint32_t HitEntityId = 0;
		uint32_t ParticleIndex = 0;
	};

	class WLD_API ParticleCollisionEventQueue
	{
	public:
		void Enqueue(const ParticleHitRecord& record) { m_Buffer.push_back(record); }
		const std::vector<ParticleHitRecord>& GetHits() const { return m_Buffer; }
		std::vector<ParticleHitRecord> Flush()
		{
			std::vector<ParticleHitRecord> hits = std::move(m_Buffer);
			m_Buffer.clear();
			return hits;
		}
		void Clear() { m_Buffer.clear(); }
		bool HasHits() const { return !m_Buffer.empty(); }
		size_t Size() const { return m_Buffer.size(); }

	private:
		std::vector<ParticleHitRecord> m_Buffer;
	};

	enum class SubEmitterTrigger : uint8_t
	{
		OnBirth = 0,
		OnCollision = 1,
		OnDeath = 2
	};

	struct SubEmitterConfig
	{
		SubEmitterTrigger Trigger = SubEmitterTrigger::OnCollision;
		uint32_t EmitCount = 4;
		float InheritVelocityRatio = 0.2f;
		ParticleSystemComponent SpawnTemplate {};
		ParticleMaterialSettings MaterialTemplate {};
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
		~ParticlePool();

		ParticlePool(const ParticlePool&) = delete;
		ParticlePool& operator=(const ParticlePool&) = delete;
		ParticlePool(ParticlePool&&) noexcept;
		ParticlePool& operator=(ParticlePool&&) noexcept;

		size_t Capacity() const { return m_Particles.size(); }
		size_t ActiveCount() const { return m_ActiveCount; }

		void Reset();
		void SetSeed(uint32_t seed);
		uint32_t GetSeed() const { return m_InitialSeed; }

		void SetMaterialSettings(const ParticleMaterialSettings& settings) { m_Material = settings; }
		const ParticleMaterialSettings& GetMaterialSettings() const { return m_Material; }
		ParticleMaterialSettings& GetMaterialSettings() { return m_Material; }
		ParticleMaterialSettings& GetMaterialSettingsMut() { return m_Material; }

		void SetCollisionSettings(const ParticleCollisionSettings& col) { m_Collision = col; }
		const ParticleCollisionSettings& GetCollisionSettings() const { return m_Collision; }
		ParticleCollisionSettings& GetCollisionSettings() { return m_Collision; }
		ParticleCollisionSettings& GetCollisionSettingsMut() { return m_Collision; }

		const ParticleCollisionEventQueue& GetEventQueue() const { return m_EventQueue; }
		ParticleCollisionEventQueue& GetEventQueueMut() { return m_EventQueue; }

		void AddSubEmitter(const SubEmitterConfig& config);
		const std::vector<SubEmitterConfig>& GetSubEmitters() const { return m_SubEmitters; }
		void ClearSubEmitters() { m_SubEmitters.clear(); }
		ParticlePool* GetSecondaryPool() { return m_SecondaryPool.get(); }
		const ParticlePool* GetSecondaryPool() const { return m_SecondaryPool.get(); }

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
		ParticleCollisionSettings m_Collision {};
		ParticleCollisionEventQueue m_EventQueue {};
		std::vector<SubEmitterConfig> m_SubEmitters;
		std::unique_ptr<ParticlePool> m_SecondaryPool;

		size_t m_ActiveCount = 0;
		size_t m_NextFreeIndex = 0;
		float m_EmissionAccumulator = 0.0f;
		uint32_t m_InitialSeed = 0x12345678;
		uint32_t m_RngState = 0x12345678;

		float RandomFloat();
		float RandomFloatSigned();
		glm::vec3 RandomUnitVector();
		void TriggerSubEmitters(SubEmitterTrigger trigger, const glm::vec3& pos, const glm::vec3& normal, const glm::vec3& vel);
	};

	struct OneShotEmitter
	{
		std::unique_ptr<ParticlePool> Pool;
		ParticleSystemComponent Comp;
		float Elapsed = 0.0f;
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

		void SpawnOneShot(const ParticleSystemComponent& comp, const ParticleMaterialSettings& mat,
			const glm::vec3& position, const glm::quat& rotation = glm::quat(1.0f, 0.0f, 0.0f, 0.0f));
		void UpdateOneShots(float dt);
		const std::vector<OneShotEmitter>& GetOneShots() const { return m_OneShots; }
		std::vector<OneShotEmitter>& GetOneShotsMut() { return m_OneShots; }
		size_t ActiveOneShotCount() const;
		void ClearOneShots() { m_OneShots.clear(); }

	private:
		std::unordered_map<uint32_t, std::unique_ptr<ParticlePool>> m_Pools;
		std::vector<OneShotEmitter> m_OneShots;
	};

	WLD_API ParticleSystem& GetGlobalParticleSystem();
}
