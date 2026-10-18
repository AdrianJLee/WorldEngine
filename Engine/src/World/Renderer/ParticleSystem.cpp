#include "wldpch.h"
#include "World/Renderer/ParticleSystem.h"

#include <cmath>
#include <algorithm>
#include <glm/gtc/matrix_transform.hpp>

namespace World
{
	ParticlePool::ParticlePool(size_t capacity)
	{
		m_Particles.resize(capacity);
		m_ActiveCount = 0;
		m_NextFreeIndex = 0;
		m_EmissionAccumulator = 0.0f;
		m_InitialSeed = 0x12345678u;
		m_RngState = m_InitialSeed;
	}

	ParticlePool::~ParticlePool() = default;

	ParticlePool::ParticlePool(ParticlePool&&) noexcept = default;
	ParticlePool& ParticlePool::operator=(ParticlePool&&) noexcept = default;

	void ParticlePool::SetSeed(uint32_t seed)
	{
		m_InitialSeed = (seed != 0u) ? seed : 0x12345678u;
		m_RngState = m_InitialSeed;
	}

	glm::vec4 ComputeSubUVRect(uint32_t frameIndex, uint32_t tilesX, uint32_t tilesY)
	{
		const uint32_t safeTilesX = std::max(1u, tilesX);
		const uint32_t safeTilesY = std::max(1u, tilesY);
		const uint32_t totalFrames = safeTilesX * safeTilesY;
		const uint32_t clampedFrame = frameIndex % totalFrames;
		const uint32_t frameX = clampedFrame % safeTilesX;
		const uint32_t frameY = clampedFrame / safeTilesX;
		const float uSize = 1.0f / static_cast<float>(safeTilesX);
		const float vSize = 1.0f / static_cast<float>(safeTilesY);
		const float uMin = static_cast<float>(frameX) * uSize;
		const float vMin = static_cast<float>(frameY) * vSize;
		return glm::vec4(uMin, vMin, uMin + uSize, vMin + vSize);
	}

	float ComputeSoftParticleFade(float sceneDepth, float particleDepth, float fadeDistance)
	{
		if (fadeDistance <= 1e-5f)
			return (sceneDepth >= particleDepth) ? 1.0f : 0.0f;
		return std::clamp((sceneDepth - particleDepth) / fadeDistance, 0.0f, 1.0f);
	}

	float ParticlePool::RandomFloat()
	{
		m_RngState = m_RngState * 1664525u + 1013904223u;
		return static_cast<float>(m_RngState & 0x00FFFFFF) / static_cast<float>(0x01000000);
	}

	float ParticlePool::RandomFloatSigned()
	{
		return RandomFloat() * 2.0f - 1.0f;
	}

	glm::vec3 ParticlePool::RandomUnitVector()
	{
		float z = RandomFloatSigned();
		float a = RandomFloat() * 6.28318530718f;
		float r = std::sqrt(std::max(0.0f, 1.0f - z * z));
		return glm::vec3(r * std::cos(a), r * std::sin(a), z);
	}

	void ParticlePool::Reset()
	{
		m_ActiveCount = 0;
		m_NextFreeIndex = 0;
		m_EmissionAccumulator = 0.0f;
		m_RngState = m_InitialSeed;
		m_EventQueue.Clear();
		if (m_SecondaryPool)
			m_SecondaryPool->Reset();

		for (auto& p : m_Particles)
		{
			p.Active = false;
			p.Life = 0.0f;
			p.SubUVFrameIndex = 0;
			p.InitialFrameOffset = 0;
		}
	}

	void ParticlePool::AddSubEmitter(const SubEmitterConfig& config)
	{
		m_SubEmitters.push_back(config);
		if (!m_SecondaryPool)
		{
			m_SecondaryPool = std::make_unique<ParticlePool>(512);
			m_SecondaryPool->SetSeed(m_InitialSeed + 1337u);
		}
	}

	void ParticlePool::TriggerSubEmitters(SubEmitterTrigger trigger, const glm::vec3& pos, const glm::vec3& normal, const glm::vec3& vel)
	{
		if (m_SubEmitters.empty()) return;

		for (const auto& sub : m_SubEmitters)
		{
			if (sub.Trigger != trigger) continue;
			if (!m_SecondaryPool)
			{
				m_SecondaryPool = std::make_unique<ParticlePool>(512);
				m_SecondaryPool->SetSeed(m_InitialSeed + 1337u);
			}

			m_SecondaryPool->SetMaterialSettings(sub.MaterialTemplate);
			ParticleSystemComponent burstComp = sub.SpawnTemplate;
			burstComp.IsPlaying = 1;
			burstComp.Looping = 0;
			burstComp.EmissionRate = 0.0f;

			glm::vec3 emitDir = (glm::length(normal) > 1e-4f) ? glm::normalize(normal) : glm::vec3(0.0f, 1.0f, 0.0f);
			m_SecondaryPool->Emit(sub.EmitCount, burstComp, pos, emitDir);

			if (std::abs(sub.InheritVelocityRatio) > 1e-4f)
			{
				auto& secParts = m_SecondaryPool->GetParticlesMut();
				for (auto& sp : secParts)
				{
					if (sp.Active)
						sp.Velocity += vel * sub.InheritVelocityRatio;
				}
			}
		}
	}

	void ParticlePool::Emit(size_t count, const ParticleSystemComponent& comp, const glm::vec3& emitterPos, const glm::vec3& emitterRotEuler)
	{
		Emit(count, comp, emitterPos, glm::quat(emitterRotEuler));
	}

	void ParticlePool::Emit(size_t count, const ParticleSystemComponent& comp, const glm::vec3& emitterPos, const glm::quat& emitterRot)
	{
		size_t emitted = 0;
		const size_t capacity = m_Particles.size();

		for (size_t i = 0; i < capacity && emitted < count; ++i)
		{
			size_t idx = (m_NextFreeIndex + i) % capacity;
			if (m_Particles[idx].Active) continue;

			Particle& p = m_Particles[idx];
			p.Active = true;
			p.Life = comp.Lifetime;
			p.MaxLife = comp.Lifetime;

			glm::vec3 localDir(0.0f, 1.0f, 0.0f);
			glm::vec3 localOffset(0.0f);

			switch (comp.EmitterType)
			{
			case ParticleEmitterType::Point:
				localDir = RandomUnitVector();
				break;
			case ParticleEmitterType::Box:
				localOffset = glm::vec3(RandomFloatSigned(), RandomFloatSigned(), RandomFloatSigned()) * 0.5f;
				localDir = glm::vec3(0.0f, 1.0f, 0.0f);
				break;
			case ParticleEmitterType::Sphere:
				localDir = RandomUnitVector();
				localOffset = localDir * RandomFloat() * 0.5f;
				break;
			case ParticleEmitterType::Cone:
			{
				float angle = RandomFloat() * 0.5f;
				float phi = RandomFloat() * 6.28318530718f;
				localDir = glm::vec3(std::sin(angle) * std::cos(phi), std::cos(angle), std::sin(angle) * std::sin(phi));
				break;
			}
			default:
				localDir = RandomUnitVector();
				break;
			}

			if (comp.LockToXYPlane)
			{
				localDir.z = 0.0f;
				localOffset.z = 0.0f;
				if (glm::length(localDir) > 1e-4f)
					localDir = glm::normalize(localDir);
				else
					localDir = glm::vec3(0.0f, 1.0f, 0.0f);
			}

			glm::vec3 worldDir = emitterRot * localDir;
			float speed = comp.Speed + RandomFloatSigned() * comp.SpeedVariance;
			p.Position = emitterPos + (emitterRot * localOffset);
			p.Velocity = worldDir * speed;
			p.Acceleration = glm::vec3(0.0f);
			p.Rotation = RandomFloat() * 6.28318530718f;
			p.AngularVelocity = RandomFloatSigned() * 2.0f;

			if (comp.LockToXYPlane)
			{
				p.Position.z = emitterPos.z;
				p.Velocity.z = 0.0f;
			}

			p.Color = UnpackColor(comp.ColorStart);
			const float emissive = std::max(0.0f, m_Material.EmissiveBoost);
			p.Color.r *= emissive;
			p.Color.g *= emissive;
			p.Color.b *= emissive;
			p.Size = glm::vec2(comp.SizeStart);

			const uint32_t totalFrames = std::max(1u, m_Material.SubUVTilesX * m_Material.SubUVTilesY);
			const bool randomStart = m_Material.SubUVRandomStartFrame || ((comp.VfxFlags & ParticleVfxFlag_SubUVRandomStart) != 0);
			if (randomStart && totalFrames > 1u)
			{
				p.InitialFrameOffset = static_cast<uint32_t>(RandomFloat() * static_cast<float>(totalFrames)) % totalFrames;
			}
			else
			{
				p.InitialFrameOffset = 0u;
			}
			p.SubUVFrameIndex = p.InitialFrameOffset;

			m_ActiveCount++;
			emitted++;
			m_NextFreeIndex = (idx + 1) % capacity;

			TriggerSubEmitters(SubEmitterTrigger::OnBirth, p.Position, worldDir, p.Velocity);
		}
	}

	void ParticlePool::Update(float dt, const ParticleSystemComponent& comp, const glm::vec3& emitterPos)
	{
		Update(dt, comp, emitterPos, glm::quat(1.0f, 0.0f, 0.0f, 0.0f));
	}

	void ParticlePool::Update(float dt, const ParticleSystemComponent& comp, const glm::vec3& emitterPos, const glm::quat& emitterRot)
	{
		if (comp.IsPlaying != 0 && comp.EmissionRate > 0.0f)
		{
			m_EmissionAccumulator += comp.EmissionRate * dt;
			size_t toEmit = static_cast<size_t>(m_EmissionAccumulator);
			if (toEmit > 0)
			{
				m_EmissionAccumulator -= static_cast<float>(toEmit);
				Emit(toEmit, comp, emitterPos, emitterRot);
			}
		}

		const glm::vec4 cStart = UnpackColor(comp.ColorStart);
		const glm::vec4 cEnd = UnpackColor(comp.ColorEnd);
		const float emissive = std::max(0.0f, m_Material.EmissiveBoost);
		const uint32_t totalFrames = std::max(1u, m_Material.SubUVTilesX * m_Material.SubUVTilesY);

		const bool hasPlaneCollision = (m_Collision.Mode == ParticleCollisionMode::Plane);
		const glm::vec3 planeNorm = (glm::length(m_Collision.PlaneNormal) > 1e-4f)
			? glm::normalize(m_Collision.PlaneNormal)
			: glm::vec3(0.0f, 1.0f, 0.0f);
		const float restitution = std::clamp(m_Collision.Restitution, 0.0f, 1.0f);
		const float friction = std::clamp(m_Collision.Friction, 0.0f, 1.0f);

		for (size_t idx = 0; idx < m_Particles.size(); ++idx)
		{
			auto& p = m_Particles[idx];
			if (!p.Active) continue;

			p.Life -= dt;
			if (p.Life <= 0.0f)
			{
				p.Active = false;
				if (m_ActiveCount > 0) m_ActiveCount--;
				TriggerSubEmitters(SubEmitterTrigger::OnDeath, p.Position, glm::vec3(0.0f, 1.0f, 0.0f), p.Velocity);
				continue;
			}

			p.Velocity += (comp.Gravity + p.Acceleration) * dt;
			p.Position += p.Velocity * dt;
			p.Rotation += p.AngularVelocity * dt;

			if (comp.LockToXYPlane)
			{
				p.Position.z = emitterPos.z;
				p.Velocity.z = 0.0f;
			}

			if (hasPlaneCollision)
			{
				const float dist = glm::dot(p.Position - m_Collision.PlaneOrigin, planeNorm);
				const float velAlongNorm = glm::dot(p.Velocity, planeNorm);
				if (dist <= 0.0f && velAlongNorm < 0.0f)
				{
					const bool needEvent = m_Collision.EnableEvents || (m_Collision.DamageAmount > 0.0f);
					if (needEvent)
					{
						ParticleHitRecord hit;
						hit.Position = p.Position - dist * planeNorm;
						hit.Normal = planeNorm;
						hit.Velocity = p.Velocity;
						hit.HitEntityId = 0;
						hit.ParticleIndex = static_cast<uint32_t>(idx);
						m_EventQueue.Enqueue(hit);
					}

					const glm::vec3 hitPos = p.Position - dist * planeNorm;
					TriggerSubEmitters(SubEmitterTrigger::OnCollision, hitPos, planeNorm, p.Velocity);

					if (m_Collision.KillOnHit)
					{
						p.Active = false;
						p.Life = 0.0f;
						if (m_ActiveCount > 0) m_ActiveCount--;
						continue;
					}
					else
					{
						const glm::vec3 vn = velAlongNorm * planeNorm;
						const glm::vec3 vt = p.Velocity - vn;
						p.Velocity = -vn * restitution + vt * (1.0f - friction);
						p.Position = hitPos + planeNorm * 0.001f;
					}
				}
			}

			float t = 1.0f - (p.Life / p.MaxLife);
			t = glm::clamp(t, 0.0f, 1.0f);

			p.Color = glm::mix(cStart, cEnd, t);
			p.Color.r *= emissive;
			p.Color.g *= emissive;
			p.Color.b *= emissive;

			float s = glm::mix(comp.SizeStart, comp.SizeEnd, t);
			p.Size = glm::vec2(s);

			if (totalFrames > 1u)
			{
				if (m_Material.SubUVFrameRate > 0.0f)
				{
					const float elapsed = std::max(0.0f, p.MaxLife - p.Life);
					const uint32_t step = static_cast<uint32_t>(elapsed * m_Material.SubUVFrameRate);
					p.SubUVFrameIndex = (p.InitialFrameOffset + step) % totalFrames;
				}
				else
				{
					const uint32_t lifeFrame = std::min(totalFrames - 1u, static_cast<uint32_t>(t * static_cast<float>(totalFrames)));
					p.SubUVFrameIndex = (p.InitialFrameOffset + lifeFrame) % totalFrames;
				}
			}
			else
			{
				p.SubUVFrameIndex = 0u;
			}
		}

		if (m_SecondaryPool && m_SecondaryPool->ActiveCount() > 0)
		{
			ParticleSystemComponent dummyComp;
			dummyComp.IsPlaying = 1;
			dummyComp.EmissionRate = 0.0f;
			m_SecondaryPool->Update(dt, dummyComp, glm::vec3(0.0f));
		}
	}

	void ParticlePool::GenerateQuadVertices(std::vector<ParticleVertex>& outVertices) const
	{
		outVertices.clear();
		size_t totalCount = m_ActiveCount + (m_SecondaryPool ? m_SecondaryPool->ActiveCount() : 0);
		outVertices.reserve(totalCount * 6);

		for (const auto& p : m_Particles)
		{
			if (!p.Active) continue;

			const glm::vec2 halfSize = p.Size * 0.5f;
			const float cosR = std::cos(p.Rotation);
			const float sinR = std::sin(p.Rotation);

			auto rotate2D = [&](float x, float y) -> glm::vec3 {
				return glm::vec3(
					p.Position.x + (x * cosR - y * sinR),
					p.Position.y + (x * sinR + y * cosR),
					p.Position.z
				);
			};

			const glm::vec3 p0 = rotate2D(-halfSize.x, -halfSize.y);
			const glm::vec3 p1 = rotate2D( halfSize.x, -halfSize.y);
			const glm::vec3 p2 = rotate2D( halfSize.x,  halfSize.y);
			const glm::vec3 p3 = rotate2D(-halfSize.x,  halfSize.y);

			const glm::vec4 uvRect = ComputeSubUVRect(p.SubUVFrameIndex, m_Material.SubUVTilesX, m_Material.SubUVTilesY);
			const glm::vec2 uv0(uvRect.x, uvRect.y);
			const glm::vec2 uv1(uvRect.z, uvRect.y);
			const glm::vec2 uv2(uvRect.z, uvRect.w);
			const glm::vec2 uv3(uvRect.x, uvRect.w);

			outVertices.push_back({ p0, p.Color, uv0, 1.0f });
			outVertices.push_back({ p1, p.Color, uv1, 1.0f });
			outVertices.push_back({ p2, p.Color, uv2, 1.0f });

			outVertices.push_back({ p0, p.Color, uv0, 1.0f });
			outVertices.push_back({ p2, p.Color, uv2, 1.0f });
			outVertices.push_back({ p3, p.Color, uv3, 1.0f });
		}

		if (m_SecondaryPool && m_SecondaryPool->ActiveCount() > 0)
		{
			std::vector<ParticleVertex> secVerts;
			m_SecondaryPool->GenerateQuadVertices(secVerts);
			outVertices.insert(outVertices.end(), secVerts.begin(), secVerts.end());
		}
	}

	void ParticlePool::GenerateBillboardVertices(const glm::vec3& cameraPos, const glm::vec3& cameraUp, std::vector<ParticleVertex>& outVertices) const
	{
		GenerateBillboardVerticesWithDepth(cameraPos, cameraUp, glm::vec3(0.0f, 0.0f, -1.0f), 1e6f, outVertices);
	}

	void ParticlePool::GenerateBillboardVerticesWithDepth(
		const glm::vec3& cameraPos,
		const glm::vec3& cameraUp,
		const glm::vec3& cameraForward,
		float sceneReferenceDepth,
		std::vector<ParticleVertex>& outVertices) const
	{
		outVertices.clear();
		size_t totalCount = m_ActiveCount + (m_SecondaryPool ? m_SecondaryPool->ActiveCount() : 0);
		outVertices.reserve(totalCount * 6);

		const float fwdLen2 = glm::dot(cameraForward, cameraForward);
		const glm::vec3 fwd = fwdLen2 > 1e-6f ? glm::normalize(cameraForward) : glm::vec3(0.0f, 0.0f, -1.0f);

		for (const auto& p : m_Particles)
		{
			if (!p.Active) continue;

			const glm::vec2 halfSize = p.Size * 0.5f;
			const glm::vec3 toCam = cameraPos - p.Position;
			const float dist2 = glm::dot(toCam, toCam);
			const glm::vec3 normal = dist2 > 1e-6f ? glm::normalize(toCam) : glm::vec3(0.0f, 0.0f, 1.0f);

			glm::vec3 right = glm::cross(cameraUp, normal);
			const float rightDist2 = glm::dot(right, right);
			right = rightDist2 > 1e-6f ? glm::normalize(right) : glm::vec3(1.0f, 0.0f, 0.0f);

			glm::vec3 up = glm::cross(normal, right);

			if (std::abs(p.Rotation) > 1e-6f)
			{
				const float cosR = std::cos(p.Rotation);
				const float sinR = std::sin(p.Rotation);
				const glm::vec3 newRight = right * cosR + up * sinR;
				const glm::vec3 newUp = -right * sinR + up * cosR;
				right = newRight;
				up = newUp;
			}

			const glm::vec3 p0 = p.Position - right * halfSize.x - up * halfSize.y;
			const glm::vec3 p1 = p.Position + right * halfSize.x - up * halfSize.y;
			const glm::vec3 p2 = p.Position + right * halfSize.x + up * halfSize.y;
			const glm::vec3 p3 = p.Position - right * halfSize.x + up * halfSize.y;

			const glm::vec4 uvRect = ComputeSubUVRect(p.SubUVFrameIndex, m_Material.SubUVTilesX, m_Material.SubUVTilesY);
			const glm::vec2 uv0(uvRect.x, uvRect.y);
			const glm::vec2 uv1(uvRect.z, uvRect.y);
			const glm::vec2 uv2(uvRect.z, uvRect.w);
			const glm::vec2 uv3(uvRect.x, uvRect.w);

			float fade = 1.0f;
			if (m_Material.SoftParticlesEnabled)
			{
				const float particleDepth = glm::dot(p.Position - cameraPos, fwd);
				fade = ComputeSoftParticleFade(sceneReferenceDepth, particleDepth, m_Material.SoftParticleFadeDistance);
			}

			glm::vec4 finalColor = p.Color;
			finalColor.a *= fade;

			outVertices.push_back({ p0, finalColor, uv0, fade });
			outVertices.push_back({ p1, finalColor, uv1, fade });
			outVertices.push_back({ p2, finalColor, uv2, fade });

			outVertices.push_back({ p0, finalColor, uv0, fade });
			outVertices.push_back({ p2, finalColor, uv2, fade });
			outVertices.push_back({ p3, finalColor, uv3, fade });
		}

		if (m_SecondaryPool && m_SecondaryPool->ActiveCount() > 0)
		{
			std::vector<ParticleVertex> secVerts;
			m_SecondaryPool->GenerateBillboardVerticesWithDepth(cameraPos, cameraUp, cameraForward, sceneReferenceDepth, secVerts);
			outVertices.insert(outVertices.end(), secVerts.begin(), secVerts.end());
		}
	}

	ParticlePool& ParticleSystem::GetPoolForEntity(uint32_t entityId, size_t capacity)
	{
		auto it = m_Pools.find(entityId);
		if (it != m_Pools.end())
			return *(it->second);

		auto pool = std::make_unique<ParticlePool>(capacity);
		ParticlePool& ref = *pool;
		m_Pools[entityId] = std::move(pool);
		return ref;
	}

	bool ParticleSystem::HasPoolForEntity(uint32_t entityId) const
	{
		return m_Pools.find(entityId) != m_Pools.end();
	}

	ParticlePool* ParticleSystem::GetPool(uint32_t entityId)
	{
		auto it = m_Pools.find(entityId);
		return (it != m_Pools.end()) ? it->second.get() : nullptr;
	}

	const ParticlePool* ParticleSystem::GetPool(uint32_t entityId) const
	{
		auto it = m_Pools.find(entityId);
		return (it != m_Pools.end()) ? it->second.get() : nullptr;
	}

	void ParticleSystem::RemoveEntity(uint32_t entityId)
	{
		m_Pools.erase(entityId);
	}

	void ParticleSystem::ClearAll()
	{
		m_Pools.clear();
		m_OneShots.clear();
	}

	void ParticleSystem::SpawnOneShot(const ParticleSystemComponent& comp, const ParticleMaterialSettings& mat,
		const glm::vec3& position, const glm::quat& rotation)
	{
		const size_t capacity = std::max(comp.MaxParticles, 64u);
		auto pool = std::make_unique<ParticlePool>(capacity);
		pool->SetMaterialSettings(mat);
		ParticleSystemComponent burstComp = comp;
		burstComp.IsPlaying = 1;
		burstComp.Looping = 0;
		burstComp.EmissionRate = 0.0f;
		pool->Emit(std::max(comp.MaxParticles, 16u), burstComp, position, rotation);

		OneShotEmitter emitter;
		emitter.Pool = std::move(pool);
		emitter.Comp = burstComp;
		emitter.Elapsed = 0.0f;
		m_OneShots.push_back(std::move(emitter));
	}

	void ParticleSystem::UpdateOneShots(float dt)
	{
		for (auto it = m_OneShots.begin(); it != m_OneShots.end(); )
		{
			it->Elapsed += dt;
			if (it->Pool)
			{
				it->Pool->Update(dt, it->Comp, glm::vec3(0.0f));
				if (it->Pool->ActiveCount() == 0)
				{
					it = m_OneShots.erase(it);
					continue;
				}
			}
			else
			{
				it = m_OneShots.erase(it);
				continue;
			}
			++it;
		}
	}

	size_t ParticleSystem::ActiveOneShotCount() const
	{
		size_t total = 0;
		for (const auto& os : m_OneShots)
		{
			if (os.Pool) total += os.Pool->ActiveCount();
		}
		return total;
	}

	ParticleSystem& GetGlobalParticleSystem()
	{
		static ParticleSystem s_GlobalInstance;
		return s_GlobalInstance;
	}

}
