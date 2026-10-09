#pragma once

#include "World/Level/Core/IStreamableChunk.h"
#include "World/Level/Core/DeltaState.h"
#include <filesystem>
#include <memory>

namespace World::Level
{
	class StreamableChunk : public IStreamableChunk
	{
	public:
		StreamableChunk();
		virtual ~StreamableChunk() override;

		bool LoadFromFile(const std::filesystem::path& chunkPath, std::string* outError = nullptr);

		virtual uint64_t GetChunkID() const override;
		virtual SubspaceID GetSubspaceID() const override;
		virtual const ChunkBounds& GetBounds() const override;
		virtual ChunkState GetState() const override;
		void SetState(ChunkState state);

		virtual void OnActivate(Scene& targetScene) override;
		virtual void OnDeactivate(Scene& targetScene) override;

		void OnActivateWithDelta(Scene& targetScene, const DeltaStateRegistry* deltaState);

		const ChunkHeader& GetHeader() const;
		size_t GetRecordCount() const;
		const StaticEntityRecord* GetRecords() const;
		size_t GetSpawnedCount() const;

		// 块内是否带**组件载荷**(v2 的实体文档段)。带载荷时激活会还原实体全部组件;
		// 不带(v1 老块)时退化为"名字 + Transform"。
		bool HasComponentPayload() const;
		size_t GetDocumentCount() const;

	private:
		struct Impl;
		std::unique_ptr<Impl> m_Impl;
	};
}
