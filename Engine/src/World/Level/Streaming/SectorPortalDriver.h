#pragma once

#include "World/Core/Export.h"

#include "World/Level/Core/IStreamingDriver.h"
#include <memory>

namespace World::Level
{
	struct Portal
	{
		uint64_t TargetChunkID{ 0 };
		glm::vec3 Center{ 0.0f };
		glm::vec3 Extents{ 1.0f, 2.0f, 0.1f };
		bool IsOpen{ true };
	};

	class WLD_API SectorPortalDriver : public IStreamingDriver
	{
	public:
		SectorPortalDriver();
		virtual ~SectorPortalDriver() override;

		virtual const char* GetDriverName() const override { return "SectorPortalDriver"; }

		void AddSector(uint64_t chunkId, const ChunkBounds& bounds);
		void AddPortal(uint64_t fromChunkId, uint64_t toChunkId, const glm::vec3& center, const glm::vec3& extents, bool isOpen = true);
		void SetPortalOpen(uint64_t fromChunkId, uint64_t toChunkId, bool isOpen);

		virtual void EvaluateStreaming(
			const glm::vec3& viewPosition,
			const glm::mat4& viewProjection,
			std::vector<uint64_t>& outChunksToLoad,
			std::vector<uint64_t>& outChunksToUnload) override;

		size_t GetSectorCount() const;
		size_t GetPortalCount() const;

	private:
		struct Impl;
		std::unique_ptr<Impl> m_Impl;
	};
}
