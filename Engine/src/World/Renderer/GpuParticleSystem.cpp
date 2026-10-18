#include "wldpch.h"
#include "World/Renderer/GpuParticleSystem.h"
#include "World/Math/Simd/CurlNoise.h"
#include "World/Core/Log.h"

#include <algorithm>
#include <cstring>

namespace World
{
	GpuParticleSystem::GpuParticleSystem(Rhi::Device* device, size_t maxParticles)
		: m_Device(device)
		, m_MaxParticles(std::max<size_t>(1, maxParticles))
	{
		m_CpuParticlesPing.resize(m_MaxParticles);
		m_CpuParticlesPong.resize(m_MaxParticles);
		m_CpuAliveIndices.reserve(m_MaxParticles);

		m_IndirectCommand.VertexCount = 4;
		m_IndirectCommand.InstanceCount = 0;
		m_IndirectCommand.FirstVertex = 0;
		m_IndirectCommand.FirstInstance = 0;

		if (m_Device != nullptr)
		{
			InitBuffers();
		}
	}

	GpuParticleSystem::~GpuParticleSystem() = default;

	GpuParticleSystem::GpuParticleSystem(GpuParticleSystem&& other) noexcept
		: m_Device(other.m_Device)
		, m_MaxParticles(other.m_MaxParticles)
		, m_ActiveCount(other.m_ActiveCount)
		, m_ComputeSupported(other.m_ComputeSupported)
		, m_PingBufferActive(other.m_PingBufferActive)
		, m_PingBuffer(std::move(other.m_PingBuffer))
		, m_PongBuffer(std::move(other.m_PongBuffer))
		, m_AliveIndexBuffer(std::move(other.m_AliveIndexBuffer))
		, m_IndirectArgsBuffer(std::move(other.m_IndirectArgsBuffer))
		, m_CpuParticlesPing(std::move(other.m_CpuParticlesPing))
		, m_CpuParticlesPong(std::move(other.m_CpuParticlesPong))
		, m_CpuAliveIndices(std::move(other.m_CpuAliveIndices))
		, m_IndirectCommand(other.m_IndirectCommand)
	{
		other.m_Device = nullptr;
		other.m_MaxParticles = 0;
		other.m_ActiveCount = 0;
	}

	GpuParticleSystem& GpuParticleSystem::operator=(GpuParticleSystem&& other) noexcept
	{
		if (this != &other)
		{
			m_Device = other.m_Device;
			m_MaxParticles = other.m_MaxParticles;
			m_ActiveCount = other.m_ActiveCount;
			m_ComputeSupported = other.m_ComputeSupported;
			m_PingBufferActive = other.m_PingBufferActive;
			m_PingBuffer = std::move(other.m_PingBuffer);
			m_PongBuffer = std::move(other.m_PongBuffer);
			m_AliveIndexBuffer = std::move(other.m_AliveIndexBuffer);
			m_IndirectArgsBuffer = std::move(other.m_IndirectArgsBuffer);
			m_CpuParticlesPing = std::move(other.m_CpuParticlesPing);
			m_CpuParticlesPong = std::move(other.m_CpuParticlesPong);
			m_CpuAliveIndices = std::move(other.m_CpuAliveIndices);
			m_IndirectCommand = other.m_IndirectCommand;

			other.m_Device = nullptr;
			other.m_MaxParticles = 0;
			other.m_ActiveCount = 0;
		}
		return *this;
	}

	bool GpuParticleSystem::InitBuffers()
	{
		if (!m_Device)
			return false;

		const auto& caps = m_Device->GetCapabilities();
		m_ComputeSupported = caps.Compute;

		Rhi::BufferDesc ssboDesc {};
		ssboDesc.Size = sizeof(GpuParticle) * m_MaxParticles;
		ssboDesc.Usage = Rhi::BufferUsageStorage | Rhi::BufferUsageTransferDst | Rhi::BufferUsageTransferSrc;
		ssboDesc.Memory = Rhi::MemoryHint::DeviceLocal;
		ssboDesc.DebugName = "GpuParticles_PingSSBO";
		m_PingBuffer = m_Device->CreateBuffer(ssboDesc);

		ssboDesc.DebugName = "GpuParticles_PongSSBO";
		m_PongBuffer = m_Device->CreateBuffer(ssboDesc);

		Rhi::BufferDesc idxDesc {};
		idxDesc.Size = sizeof(uint32_t) * m_MaxParticles;
		idxDesc.Usage = Rhi::BufferUsageStorage | Rhi::BufferUsageIndex | Rhi::BufferUsageTransferDst;
		idxDesc.Memory = Rhi::MemoryHint::DeviceLocal;
		idxDesc.DebugName = "GpuParticles_AliveIndexBuffer";
		m_AliveIndexBuffer = m_Device->CreateBuffer(idxDesc);

		Rhi::BufferDesc indirectDesc {};
		indirectDesc.Size = sizeof(DrawArraysIndirectCommand);
		indirectDesc.Usage = Rhi::BufferUsageStorage | Rhi::BufferUsageIndirect | Rhi::BufferUsageTransferDst;
		indirectDesc.Memory = Rhi::MemoryHint::DeviceLocal;
		indirectDesc.DebugName = "GpuParticles_IndirectArgs";
		indirectDesc.InitialData = &m_IndirectCommand;
		m_IndirectArgsBuffer = m_Device->CreateBuffer(indirectDesc);

		return (m_PingBuffer && m_PongBuffer && m_AliveIndexBuffer && m_IndirectArgsBuffer);
	}

	void GpuParticleSystem::Reset()
	{
		m_ActiveCount = 0;
		m_CpuAliveIndices.clear();
		for (auto& p : m_CpuParticlesPing)
			p.PositionLife.w = 0.0f;
		for (auto& p : m_CpuParticlesPong)
			p.PositionLife.w = 0.0f;

		m_IndirectCommand.InstanceCount = 0;
		SyncToGpu();
	}

	void GpuParticleSystem::Emit(size_t count, const GpuParticle& prototype)
	{
		size_t emitted = 0;
		auto& currentBuffer = m_PingBufferActive ? m_CpuParticlesPing : m_CpuParticlesPong;

		for (size_t i = 0; i < m_MaxParticles && emitted < count; ++i)
		{
			if (currentBuffer[i].PositionLife.w <= 0.0f)
			{
				currentBuffer[i] = prototype;
				emitted++;
			}
		}

		m_ActiveCount = std::min(m_MaxParticles, m_ActiveCount + emitted);
		m_IndirectCommand.InstanceCount = static_cast<uint32_t>(m_ActiveCount);
		SyncToGpu();
	}

	void GpuParticleSystem::SetParticle(size_t index, const GpuParticle& particle)
	{
		if (index >= m_MaxParticles)
			return;

		auto& currentBuffer = m_PingBufferActive ? m_CpuParticlesPing : m_CpuParticlesPong;
		bool wasAlive = (currentBuffer[index].PositionLife.w > 0.0f);
		bool nowAlive = (particle.PositionLife.w > 0.0f);

		currentBuffer[index] = particle;

		if (!wasAlive && nowAlive)
			m_ActiveCount++;
		else if (wasAlive && !nowAlive && m_ActiveCount > 0)
			m_ActiveCount--;

		m_IndirectCommand.InstanceCount = static_cast<uint32_t>(m_ActiveCount);
	}

	const GpuParticle& GpuParticleSystem::GetParticle(size_t index) const
	{
		static const GpuParticle s_DefaultParticle {};
		if (index >= m_MaxParticles)
			return s_DefaultParticle;

		const auto& currentBuffer = m_PingBufferActive ? m_CpuParticlesPing : m_CpuParticlesPong;
		return currentBuffer[index];
	}

	void GpuParticleSystem::Simulate(float dt, const GpuSimulationParams& params, Rhi::CommandBuffer* cmd)
	{
		if (m_ComputeSupported && cmd != nullptr)
		{
			// GPU Compute path (dispatched via DispatchCompute)
			// Swapping buffers after GPU dispatch
		}
		else
		{
			SimulateCpuFallback(dt, params);
		}
	}

	void GpuParticleSystem::SimulateCpuFallback(float dt, const GpuSimulationParams& params)
	{
		auto& readBuffer = m_PingBufferActive ? m_CpuParticlesPing : m_CpuParticlesPong;
		auto& writeBuffer = m_PingBufferActive ? m_CpuParticlesPong : m_CpuParticlesPing;

		m_CpuAliveIndices.clear();
		size_t aliveCount = 0;

		const glm::vec3 gravity(params.GravityDt.x, params.GravityDt.y, params.GravityDt.z);
		const float drag = params.DragTurbulence.x;
		const float dragFactor = std::max(0.0f, 1.0f - drag * dt);
		const float curlStrength = params.DragTurbulence.w;
		const float turbFreq = params.DragTurbulence.z;

		for (size_t i = 0; i < m_MaxParticles; ++i)
		{
			const auto& src = readBuffer[i];
			float remainingLife = src.PositionLife.w - dt;

			if (remainingLife > 0.0f)
			{
				GpuParticle dst = src;
				dst.PositionLife.w = remainingLife;

				glm::vec3 pos(src.PositionLife.x, src.PositionLife.y, src.PositionLife.z);
				glm::vec3 vel(src.VelocityMaxLife.x, src.VelocityMaxLife.y, src.VelocityMaxLife.z);

				vel = vel * dragFactor + gravity * dt;

				if (curlStrength > 1e-4f)
				{
					glm::vec3 curl = Math::Simd::ComputeCurlNoise(pos, turbFreq, 1u);
					vel += curl * (curlStrength * dt);
				}

				pos += vel * dt;
				dst.PositionLife.x = pos.x;
				dst.PositionLife.y = pos.y;
				dst.PositionLife.z = pos.z;

				dst.VelocityMaxLife.x = vel.x;
				dst.VelocityMaxLife.y = vel.y;
				dst.VelocityMaxLife.z = vel.z;

				dst.SizeRotation.z += dst.SizeRotation.w * dt;

				writeBuffer[aliveCount] = dst;
				m_CpuAliveIndices.push_back(static_cast<uint32_t>(aliveCount));
				aliveCount++;
			}
		}

		// Clear remaining tail
		for (size_t i = aliveCount; i < m_MaxParticles; ++i)
		{
			writeBuffer[i].PositionLife.w = 0.0f;
		}

		m_ActiveCount = aliveCount;
		m_IndirectCommand.InstanceCount = static_cast<uint32_t>(m_ActiveCount);

		SwapBuffers();
		SyncToGpu();
	}

	void GpuParticleSystem::DispatchCompute(Rhi::CommandBuffer* cmd, const Rhi::Handle<Rhi::Pipeline>& computePipeline, uint32_t groupSizeX)
	{
		if (!cmd || !m_ComputeSupported || !computePipeline)
			return;

		uint32_t safeGroupSize = std::max(1u, groupSizeX);
		uint32_t groupCount = static_cast<uint32_t>((m_MaxParticles + safeGroupSize - 1) / safeGroupSize);

		cmd->BindPipeline(computePipeline);
		cmd->Dispatch(groupCount, 1, 1);
		SwapBuffers();
	}

	void GpuParticleSystem::RecordDrawIndirect(Rhi::CommandBuffer* cmd)
	{
		if (!cmd || !m_IndirectArgsBuffer)
			return;

		cmd->DrawIndirect(m_IndirectArgsBuffer, 0, 1, sizeof(DrawArraysIndirectCommand));
	}

	void GpuParticleSystem::SwapBuffers()
	{
		m_PingBufferActive = !m_PingBufferActive;
	}

	void GpuParticleSystem::SyncToGpu()
	{
		if (!m_Device)
			return;

		const auto& currentBuffer = m_PingBufferActive ? m_CpuParticlesPing : m_CpuParticlesPong;
		if (m_PingBuffer)
		{
			m_PingBuffer->SetData(currentBuffer.data(), sizeof(GpuParticle) * m_MaxParticles, 0);
		}
		if (m_IndirectArgsBuffer)
		{
			m_IndirectArgsBuffer->SetData(&m_IndirectCommand, sizeof(DrawArraysIndirectCommand), 0);
		}
	}

	void GpuParticleSystem::ReadbackParticles(std::vector<GpuParticle>& outParticles)
	{
		const auto& currentBuffer = m_PingBufferActive ? m_CpuParticlesPing : m_CpuParticlesPong;
		outParticles.assign(currentBuffer.begin(), currentBuffer.begin() + m_ActiveCount);
	}

	void GpuParticleSystem::ReadbackAliveIndices(std::vector<uint32_t>& outIndices)
	{
		outIndices = m_CpuAliveIndices;
	}
}
