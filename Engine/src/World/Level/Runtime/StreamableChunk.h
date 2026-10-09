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

	private:
		struct Impl;
		std::unique_ptr<Impl> m_Impl;
	};
}
