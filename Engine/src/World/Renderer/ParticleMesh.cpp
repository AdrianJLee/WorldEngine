#include "wldpch.h"
#include "World/Renderer/ParticleMesh.h"
#include "World/Renderer/ParticleSystem.h"
#include "World/Renderer/ParticleBuffer.h"
#include "World/Renderer/Mesh.h"

#include <glm/gtc/matrix_transform.hpp>
#include <algorithm>

namespace World
{
	namespace
	{
		glm::quat ComputeAlignQuaternion(const glm::vec3& dir)
		{
			const glm::vec3 forward(0.0f, 0.0f, 1.0f);
			float dot = glm::dot(forward, dir);

			if (dot >= 0.9999f)
			{
				return glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
			}
			if (dot <= -0.9999f)
			{
				return glm::angleAxis(glm::pi<float>(), glm::vec3(0.0f, 1.0f, 0.0f));
			}

			glm::vec3 axis = glm::cross(forward, dir);
			glm::quat q(1.0f + dot, axis.x, axis.y, axis.z);
			return glm::normalize(q);
		}
	}

	ParticleMeshBatch::ParticleMeshBatch(const ParticleMeshConfig& config)
		: m_Config(config)
	{
	}

	void ParticleMeshBatch::BuildInstances(const ParticlePool& pool, const glm::vec3& emitterPos)
	{
		m_Instances.clear();
		const auto& particles = pool.GetParticles();
		m_Instances.reserve(pool.ActiveCount());

		const float emissiveBoost = pool.GetMaterialSettings().EmissiveBoost;

		for (const auto& p : particles)
		{
			if (!p.Active || p.Life <= 0.0f)
				continue;

			glm::quat orientation = m_Config.MeshRotationOffset;
			if (m_Config.AlignToVelocity && glm::dot(p.Velocity, p.Velocity) > 1e-4f)
			{
				glm::vec3 dir = glm::normalize(p.Velocity);
				orientation = ComputeAlignQuaternion(dir) * m_Config.MeshRotationOffset;
			}
			else if (std::abs(p.Rotation) > 1e-4f)
			{
				orientation = glm::angleAxis(p.Rotation, glm::vec3(0.0f, 0.0f, 1.0f)) * m_Config.MeshRotationOffset;
			}

			glm::vec3 scale = m_Config.MeshScale;
			if (m_Config.ScaleWithParticleSize)
			{
				scale *= glm::vec3(p.Size.x, p.Size.y, (p.Size.x + p.Size.y) * 0.5f);
			}

			glm::mat4 model = glm::translate(glm::mat4(1.0f), p.Position)
				* glm::mat4_cast(orientation)
				* glm::scale(glm::mat4(1.0f), scale);

			float lifeRatio = (p.MaxLife > 1e-4f) ? glm::clamp(1.0f - (p.Life / p.MaxLife), 0.0f, 1.0f) : 0.0f;

			ParticleMeshInstance inst;
			inst.Transform = model;
			inst.Color = p.Color;
			inst.CustomData = glm::vec4(lifeRatio, emissiveBoost, static_cast<float>(p.SubUVFrameIndex), 0.0f);

			m_Instances.push_back(inst);
		}
	}

	void ParticleMeshBatch::BuildInstancesFromSoA(const ParticleSoA& soa, const glm::vec3& emitterPos)
	{
		m_Instances.clear();
		m_Instances.reserve(soa.ActiveCount);

		for (size_t i = 0; i < soa.Capacity; ++i)
		{
			if (soa.Active[i] == 0 || soa.Life[i] <= 0.0f)
				continue;

			glm::vec3 pos(soa.PosX[i], soa.PosY[i], soa.PosZ[i]);
			glm::vec3 vel(soa.VelX[i], soa.VelY[i], soa.VelZ[i]);

			glm::quat orientation = m_Config.MeshRotationOffset;
			if (m_Config.AlignToVelocity && glm::dot(vel, vel) > 1e-4f)
			{
				glm::vec3 dir = glm::normalize(vel);
				orientation = ComputeAlignQuaternion(dir) * m_Config.MeshRotationOffset;
			}
			else if (std::abs(soa.Rotation[i]) > 1e-4f)
			{
				orientation = glm::angleAxis(soa.Rotation[i], glm::vec3(0.0f, 0.0f, 1.0f)) * m_Config.MeshRotationOffset;
			}

			glm::vec3 scale = m_Config.MeshScale;
			if (m_Config.ScaleWithParticleSize)
			{
				scale *= glm::vec3(soa.SizeX[i], soa.SizeY[i], (soa.SizeX[i] + soa.SizeY[i]) * 0.5f);
			}

			glm::mat4 model = glm::translate(glm::mat4(1.0f), pos)
				* glm::mat4_cast(orientation)
				* glm::scale(glm::mat4(1.0f), scale);

			float lifeRatio = (soa.MaxLife[i] > 1e-4f) ? glm::clamp(1.0f - (soa.Life[i] / soa.MaxLife[i]), 0.0f, 1.0f) : 0.0f;

			ParticleMeshInstance inst;
			inst.Transform = model;
			inst.Color = glm::vec4(soa.ColorR[i], soa.ColorG[i], soa.ColorB[i], soa.ColorA[i]);
			inst.CustomData = glm::vec4(lifeRatio, 1.0f, static_cast<float>(soa.SubUVFrameIndex[i]), 0.0f);

			m_Instances.push_back(inst);
		}
	}

	bool ParticleMeshBatch::UploadToGpu(Rhi::Device* device)
	{
		if (!device || m_Instances.empty())
			return false;

		uint64_t dataSize = sizeof(ParticleMeshInstance) * m_Instances.size();

		if (!m_InstanceBuffer || m_InstanceBuffer->GetDesc().Size < dataSize)
		{
			Rhi::BufferDesc desc {};
			desc.Size = dataSize;
			desc.Usage = Rhi::BufferUsageVertex | Rhi::BufferUsageStorage | Rhi::BufferUsageTransferDst;
			desc.Memory = Rhi::MemoryHint::DeviceLocal;
			desc.DebugName = "ParticleMeshInstances_Buffer";
			desc.InitialData = m_Instances.data();
			m_InstanceBuffer = device->CreateBuffer(desc);
		}
		else
		{
			m_InstanceBuffer->SetData(m_Instances.data(), dataSize, 0);
		}

		return m_InstanceBuffer != nullptr;
	}

	void ParticleMeshBatch::RecordDraw(Rhi::CommandBuffer* cmd, uint32_t vertexOrIndexCount, bool indexed)
	{
		if (!cmd || m_Instances.empty() || vertexOrIndexCount == 0)
			return;

		uint32_t instanceCount = static_cast<uint32_t>(m_Instances.size());
		if (indexed)
		{
			cmd->DrawIndexed(vertexOrIndexCount, instanceCount, 0, 0, 0);
		}
		else
		{
			cmd->Draw(vertexOrIndexCount, instanceCount, 0, 0);
		}
	}
}
