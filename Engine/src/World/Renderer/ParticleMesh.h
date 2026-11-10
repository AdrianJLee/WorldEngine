#pragma once

#include "World/Core/Export.h"
#include "World/Core/Core.h"
#include "World/RHI/Rhi.h"
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <vector>
#include <string>
#include <cstdint>
#include <cstddef>

namespace World
{
	class Mesh;
	class ParticlePool;
	struct ParticleSoA;

	// Per-instance layout (std140 / std430 aligned, 96 bytes)
	struct alignas(16) ParticleMeshInstance
	{
		glm::mat4 Transform { 1.0f };   // 64 bytes
		glm::vec4 Color { 1.0f };       // 16 bytes
		glm::vec4 CustomData { 0.0f };  // 16 bytes: x=LifeRatio, y=EmissiveBoost, z=SubUV, w=Unused
	};
	static_assert(sizeof(ParticleMeshInstance) == 96, "ParticleMeshInstance must be 96 bytes");

	struct ParticleMeshConfig
	{
		std::string MeshAssetPath;
		Ref<Mesh> MeshAsset;
		glm::vec3 MeshScale { 1.0f };
		glm::quat MeshRotationOffset { 1.0f, 0.0f, 0.0f, 0.0f };
		bool AlignToVelocity = false;
		bool ScaleWithParticleSize = true;
	};

	class WLD_API ParticleMeshBatch
	{
	public:
		explicit ParticleMeshBatch(const ParticleMeshConfig& config = ParticleMeshConfig {});
		~ParticleMeshBatch() = default;

		void SetConfig(const ParticleMeshConfig& config) { m_Config = config; }
		const ParticleMeshConfig& GetConfig() const { return m_Config; }
		ParticleMeshConfig& GetConfigMut() { return m_Config; }

		// Builds instance matrix buffer from ParticlePool
		void BuildInstances(const ParticlePool& pool, const glm::vec3& emitterPos = glm::vec3(0.0f));

		// Builds instance matrix buffer from ParticleSoA
		void BuildInstancesFromSoA(const ParticleSoA& soa, const glm::vec3& emitterPos = glm::vec3(0.0f));

		size_t GetInstanceCount() const { return m_Instances.size(); }
		const std::vector<ParticleMeshInstance>& GetInstances() const { return m_Instances; }
		std::vector<ParticleMeshInstance>& GetInstancesMut() { return m_Instances; }

		// GPU Buffer synchronization
		bool UploadToGpu(Rhi::Device* device);
		Rhi::Handle<Rhi::Buffer> GetInstanceBuffer() const { return m_InstanceBuffer; }

		// Submits a single instanced draw call (zero CPU per-particle draw calls)
		void RecordDraw(Rhi::CommandBuffer* cmd, uint32_t vertexOrIndexCount, bool indexed = true);

	private:
		ParticleMeshConfig m_Config;
		std::vector<ParticleMeshInstance> m_Instances;
		Rhi::Handle<Rhi::Buffer> m_InstanceBuffer;
	};
}
