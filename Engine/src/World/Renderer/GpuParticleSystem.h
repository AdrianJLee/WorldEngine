#pragma once

#include "World/Core/Export.h"
#include "World/RHI/Rhi.h"
#include <glm/glm.hpp>
#include <vector>
#include <cstdint>
#include <cstddef>
#include <memory>

namespace World
{
	// 64-byte std430-aligned GPU particle structure for compute shaders & SSBOs
	struct alignas(16) GpuParticle
	{
		glm::vec4 PositionLife { 0.0f };       // xyz = WorldPos, w = Remaining Life
		glm::vec4 VelocityMaxLife { 0.0f };    // xyz = Velocity, w = Max Life
		glm::vec4 Color { 1.0f };             // rgba (HDR)
		glm::vec4 SizeRotation { 1.0f, 1.0f, 0.0f, 0.0f }; // xy = Size, z = Rotation, w = AngularVelocity
	};
	static_assert(sizeof(GpuParticle) == 64, "GpuParticle must be exactly 64 bytes");

	// DrawArraysIndirect command matching Vulkan / OpenGL indirect rendering layout
	struct DrawArraysIndirectCommand
	{
		uint32_t VertexCount = 4;
		uint32_t InstanceCount = 0;
		uint32_t FirstVertex = 0;
		uint32_t FirstInstance = 0;
	};
	static_assert(sizeof(DrawArraysIndirectCommand) == 16, "DrawArraysIndirectCommand must be 16 bytes");

	struct alignas(16) GpuSimulationParams
	{
		glm::vec4 GravityDt { 0.0f, -9.8f, 0.0f, 0.016f };        // xyz = Gravity, w = Dt
		glm::vec4 DragTurbulence { 0.0f, 0.0f, 1.0f, 0.0f };       // x = Drag, y = TurbStrength, z = TurbFreq, w = CurlStrength
		glm::vec4 EmitterPosMaxCount { 0.0f, 0.0f, 0.0f, 100000.0f }; // xyz = EmitterPos, w = MaxParticles
		glm::vec4 BoundsMin { -1000.0f, -1000.0f, -1000.0f, 0.0f };
		glm::vec4 BoundsMax {  1000.0f,  1000.0f,  1000.0f, 0.0f };
	};

	class WLD_API GpuParticleSystem
	{
	public:
		explicit GpuParticleSystem(Rhi::Device* device = nullptr, size_t maxParticles = 100000);
		~GpuParticleSystem();

		GpuParticleSystem(const GpuParticleSystem&) = delete;
		GpuParticleSystem& operator=(const GpuParticleSystem&) = delete;
		GpuParticleSystem(GpuParticleSystem&&) noexcept;
		GpuParticleSystem& operator=(GpuParticleSystem&&) noexcept;

		// Initializes SSBOs and indirect arguments on GPU if device is available
		bool InitBuffers();

		// Resets all particle states
		void Reset();

		// Emits particles into the system
		void Emit(size_t count, const GpuParticle& prototype);
		void SetParticle(size_t index, const GpuParticle& particle);
		const GpuParticle& GetParticle(size_t index) const;

		// Advances simulation by dt. When RHI compute is supported, dispatches compute shader.
		// Otherwise, executes high-throughput CPU simulation fallback.
		void Simulate(float dt, const GpuSimulationParams& params, Rhi::CommandBuffer* cmd = nullptr);

		// Dispatches compute shader pass on command buffer
		void DispatchCompute(Rhi::CommandBuffer* cmd, const Rhi::Handle<Rhi::Pipeline>& computePipeline, uint32_t groupSizeX = 256);

		// Records indirect draw call using the indirect arguments buffer
		void RecordDrawIndirect(Rhi::CommandBuffer* cmd);

		// Ping-pong buffer swap
		void SwapBuffers();

		// Reads back active particle data to CPU memory (for debugging, verification, or CPU systems)
		void ReadbackParticles(std::vector<GpuParticle>& outParticles);
		void ReadbackAliveIndices(std::vector<uint32_t>& outIndices);

		// Buffer and state getters
		size_t GetMaxParticles() const { return m_MaxParticles; }
		size_t GetActiveCount() const { return m_ActiveCount; }
		bool IsComputeSupported() const { return m_ComputeSupported; }

		Rhi::Handle<Rhi::Buffer> GetPingBuffer() const { return m_PingBuffer; }
		Rhi::Handle<Rhi::Buffer> GetPongBuffer() const { return m_PongBuffer; }
		Rhi::Handle<Rhi::Buffer> GetCurrentReadBuffer() const { return m_PingBufferActive ? m_PingBuffer : m_PongBuffer; }
		Rhi::Handle<Rhi::Buffer> GetCurrentWriteBuffer() const { return m_PingBufferActive ? m_PongBuffer : m_PingBuffer; }
		Rhi::Handle<Rhi::Buffer> GetAliveIndexBuffer() const { return m_AliveIndexBuffer; }
		Rhi::Handle<Rhi::Buffer> GetIndirectArgsBuffer() const { return m_IndirectArgsBuffer; }
		const DrawArraysIndirectCommand& GetIndirectCommand() const { return m_IndirectCommand; }

	private:
		Rhi::Device* m_Device = nullptr;
		size_t m_MaxParticles = 100000;
		size_t m_ActiveCount = 0;
		bool m_ComputeSupported = false;
		bool m_PingBufferActive = true;

		// RHI GPU Buffers
		Rhi::Handle<Rhi::Buffer> m_PingBuffer;
		Rhi::Handle<Rhi::Buffer> m_PongBuffer;
		Rhi::Handle<Rhi::Buffer> m_AliveIndexBuffer;
		Rhi::Handle<Rhi::Buffer> m_IndirectArgsBuffer;

		// CPU backing storage
		std::vector<GpuParticle> m_CpuParticlesPing;
		std::vector<GpuParticle> m_CpuParticlesPong;
		std::vector<uint32_t> m_CpuAliveIndices;
		DrawArraysIndirectCommand m_IndirectCommand {};

		void SimulateCpuFallback(float dt, const GpuSimulationParams& params);
		void SyncToGpu();
	};
}
