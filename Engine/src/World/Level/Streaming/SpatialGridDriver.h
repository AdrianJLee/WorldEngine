#pragma once

#include "World/Core/Export.h"

#include "World/Level/Core/IStreamingDriver.h"
#include <memory>

namespace World::Level
{
	class WLD_API SpatialGridDriver : public IStreamingDriver
	{
	public:
		SpatialGridDriver(float loadDistance = 128.0f, float unloadDistance = 160.0f);
		virtual ~SpatialGridDriver() override;

		virtual const char* GetDriverName() const override { return "SpatialGridDriver"; }

		void AddChunk(uint64_t chunkId, const ChunkBounds& bounds);
		void RemoveChunk(uint64_t chunkId);

		virtual void EvaluateStreaming(
			const glm::vec3& viewPosition,
			const glm::mat4& viewProjection,
			std::vector<uint64_t>& outChunksToLoad,
			std::vector<uint64_t>& outChunksToUnload) override;

		void SetLoadDistance(float d);
		float GetLoadDistance() const;

		void SetUnloadDistance(float d);
		float GetUnloadDistance() const;

		void SetFrustumCullingEnabled(bool enable);
		bool IsFrustumCullingEnabled() const;

		size_t GetTrackedChunkCount() const;

	private:
		struct Impl;
		std::unique_ptr<Impl> m_Impl;
	};
}
