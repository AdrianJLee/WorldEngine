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
		float a = RandomFloat() * 6.28318530717958647692f;
		float r = std::sqrt(std::max(0.0f, 1.0f - z * z));
		return glm::vec3(r * std::cos(a), r * std::sin(a), z);
	}

	void ParticlePool::Reset()
	{
		m_ActiveCount = 0;
		m_NextFreeIndex = 0;
		m_EmissionAccumulator = 0.0f;
		for (auto& p : m_Particles)
		{
			p.Active = false;
			p.Life = 0.0f;
		}
	}

	void ParticlePool::Emit(size_t count, const ParticleSystemComponent& comp, const glm::vec3& emitterPos, const glm::vec3& emitterRotEuler)
	{
		glm::quat rot = glm::quat(glm::radians(emitterRotEuler));
		Emit(count, comp, emitterPos, rot);
	}

	void ParticlePool::Emit(size_t count, const ParticleSystemComponent& comp, const glm::vec3& emitterPos, const glm::quat& emitterRot)
	{
		if (m_Particles.empty() || count == 0) return;

		const size_t capacity = m_Particles.size();
		size_t emitted = 0;

		for (size_t i = 0; i < capacity && emitted < count; ++i)
		{
			size_t idx = (m_NextFreeIndex + i) % capacity;
			if (m_Particles[idx].Active) continue;

			Particle& p = m_Particles[idx];
			p.Active = true;
			p.MaxLife = comp.Lifetime > 0.0f ? comp.Lifetime : 1.0f;
			p.Life = p.MaxLife;
			p.Rotation = 0.0f;
			p.AngularVelocity = 0.0f;

			glm::vec3 offset(0.0f);
			glm::vec3 dir(0.0f, 1.0f, 0.0f);

			switch (comp.EmitterType)
			{
			case ParticleEmitterType::Point:
			{
				offset = glm::vec3(0.0f);
				if (comp.LockToXYPlane != 0)
				{
					float ang = RandomFloat() * 6.28318530717958647692f;
					dir = glm::vec3(std::cos(ang), std::sin(ang), 0.0f);
				}
				else
				{
					dir = RandomUnitVector();
				}
				break;
			}
			case ParticleEmitterType::Line:
			{
				float t = RandomFloatSigned();
				offset = glm::vec3(t, 0.0f, 0.0f);
				dir = glm::vec3(0.0f, 1.0f, 0.0f);
				break;
			}
			case ParticleEmitterType::Box:
			{
				offset = glm::vec3(RandomFloatSigned(), RandomFloatSigned(), comp.LockToXYPlane != 0 ? 0.0f : RandomFloatSigned());
				float len = glm::length(offset);
				dir = len > 1e-5f ? offset / len : glm::vec3(0.0f, 1.0f, 0.0f);
				break;
			}
			case ParticleEmitterType::Sphere:
			{
				if (comp.LockToXYPlane != 0)
				{
					float ang = RandomFloat() * 6.28318530717958647692f;
					float r = std::sqrt(RandomFloat());
					offset = glm::vec3(std::cos(ang) * r, std::sin(ang) * r, 0.0f);
					dir = glm::vec3(std::cos(ang), std::sin(ang), 0.0f);
				}
				else
				{
					offset = RandomUnitVector() * std::cbrt(RandomFloat());
					float len = glm::length(offset);
					dir = len > 1e-5f ? offset / len : glm::vec3(0.0f, 1.0f, 0.0f);
				}
				break;
			}
			case ParticleEmitterType::Cone:
			{
				constexpr float coneAngle = 0.52359877f; // ~30 deg
				if (comp.LockToXYPlane != 0)
				{
					float ang = RandomFloatSigned() * coneAngle;
					dir = glm::vec3(std::sin(ang), std::cos(ang), 0.0f);
				}
				else
				{
					float theta = RandomFloat() * 6.28318530717958647692f;
					float phi = RandomFloat() * coneAngle;
					dir = glm::vec3(std::sin(phi) * std::cos(theta), std::cos(phi), std::sin(phi) * std::sin(theta));
				}
				offset = glm::vec3(0.0f);
				break;
			}
			case ParticleEmitterType::Circle:
			{
				float ang = RandomFloat() * 6.28318530717958647692f;
				float r = std::sqrt(RandomFloat());
				offset = glm::vec3(std::cos(ang) * r, std::sin(ang) * r, 0.0f);
				dir = glm::vec3(std::cos(ang), std::sin(ang), 0.0f);
				break;
			}
			default:
				offset = glm::vec3(0.0f);
				dir = glm::vec3(0.0f, 1.0f, 0.0f);
				break;
			}

			offset = emitterRot * offset;
			dir = emitterRot * dir;
			if (glm::dot(dir, dir) > 1e-6f)
				dir = glm::normalize(dir);
			else
				dir = glm::vec3(0.0f, 1.0f, 0.0f);

			p.Position = emitterPos + offset;

			float speed = comp.Speed + RandomFloatSigned() * comp.SpeedVariance;
			if (speed < 0.0f) speed = 0.0f;
			p.Velocity = dir * speed;
			p.Acceleration = comp.Gravity;

			if (comp.LockToXYPlane != 0)
			{
				p.Position.z = emitterPos.z;
				p.Velocity.z = 0.0f;
				p.Acceleration.z = 0.0f;
			}

			p.Color = UnpackColor(comp.ColorStart);
			p.Size = glm::vec2(comp.SizeStart);

			m_ActiveCount++;
			emitted++;
			m_NextFreeIndex = (idx + 1) % capacity;
		}
	}

	void ParticlePool::Update(float dt, const ParticleSystemComponent& comp, const glm::vec3& emitterPos)
	{
		if (comp.IsPlaying != 0 && comp.EmissionRate > 0.0f)
		{
			m_EmissionAccumulator += comp.EmissionRate * dt;
			size_t toEmit = static_cast<size_t>(m_EmissionAccumulator);
			if (toEmit > 0)
			{
				m_EmissionAccumulator -= static_cast<float>(toEmit);
				Emit(toEmit, comp, emitterPos);
			}
		}

		const glm::vec4 cStart = UnpackColor(comp.ColorStart);
		const glm::vec4 cEnd = UnpackColor(comp.ColorEnd);

		for (auto& p : m_Particles)
		{
			if (!p.Active) continue;

			p.Life -= dt;
			if (p.Life <= 0.0f)
			{
				p.Active = false;
				if (m_ActiveCount > 0) m_ActiveCount--;
				continue;
			}

			p.Velocity += p.Acceleration * dt;
			p.Position += p.Velocity * dt;
			p.Rotation += p.AngularVelocity * dt;

			if (comp.LockToXYPlane != 0)
			{
				p.Position.z = emitterPos.z;
				p.Velocity.z = 0.0f;
				p.Acceleration.z = 0.0f;
			}

			float t = 1.0f - (p.Life / p.MaxLife);
			t = glm::clamp(t, 0.0f, 1.0f);

			p.Color = glm::mix(cStart, cEnd, t);

			float s = glm::mix(comp.SizeStart, comp.SizeEnd, t);
			p.Size = glm::vec2(s);
		}
	}

	void ParticlePool::GenerateQuadVertices(std::vector<ParticleVertex>& outVertices) const
	{
		outVertices.clear();
		outVertices.reserve(m_ActiveCount * 6);

		for (const auto& p : m_Particles)
		{
			if (!p.Active) continue;

			const glm::vec2 halfSize = p.Size * 0.5f;
			const float cosR = std::cos(p.Rotation);
			const float sinR = std::sin(p.Rotation);

			auto rotate2D = [&](float x, float y) -> glm::vec3 {
				return glm::vec3(
					x * cosR - y * sinR + p.Position.x,
					x * sinR + y * cosR + p.Position.y,
					p.Position.z
				);
			};

			const glm::vec3 p0 = rotate2D(-halfSize.x, -halfSize.y);
			const glm::vec3 p1 = rotate2D( halfSize.x, -halfSize.y);
			const glm::vec3 p2 = rotate2D( halfSize.x,  halfSize.y);
			const glm::vec3 p3 = rotate2D(-halfSize.x,  halfSize.y);

			outVertices.push_back({ p0, p.Color, glm::vec2(0.0f, 0.0f) });
			outVertices.push_back({ p1, p.Color, glm::vec2(1.0f, 0.0f) });
			outVertices.push_back({ p2, p.Color, glm::vec2(1.0f, 1.0f) });

			outVertices.push_back({ p0, p.Color, glm::vec2(0.0f, 0.0f) });
			outVertices.push_back({ p2, p.Color, glm::vec2(1.0f, 1.0f) });
			outVertices.push_back({ p3, p.Color, glm::vec2(0.0f, 1.0f) });
		}
	}

	void ParticlePool::GenerateBillboardVertices(const glm::vec3& cameraPos, const glm::vec3& cameraUp, std::vector<ParticleVertex>& outVertices) const
	{
		outVertices.clear();
		outVertices.reserve(m_ActiveCount * 6);

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

			outVertices.push_back({ p0, p.Color, glm::vec2(0.0f, 0.0f) });
			outVertices.push_back({ p1, p.Color, glm::vec2(1.0f, 0.0f) });
			outVertices.push_back({ p2, p.Color, glm::vec2(1.0f, 1.0f) });

			outVertices.push_back({ p0, p.Color, glm::vec2(0.0f, 0.0f) });
			outVertices.push_back({ p2, p.Color, glm::vec2(1.0f, 1.0f) });
			outVertices.push_back({ p3, p.Color, glm::vec2(0.0f, 1.0f) });
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

	void ParticleSystem::RemoveEntity(uint32_t entityId)
	{
		m_Pools.erase(entityId);
	}

	void ParticleSystem::ClearAll()
	{
		m_Pools.clear();
	}
}
