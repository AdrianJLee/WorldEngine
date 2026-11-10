#pragma once

#include "World/Core/Export.h"

#include "World/Level/Core/IStreamableChunk.h"
#include "World/Level/Core/DeltaState.h"
#include <filesystem>
#include <string>
#include <vector>
#include <memory>

namespace World::Level
{
	class WLD_API StreamableChunk : public IStreamableChunk
	{
	public:
		StreamableChunk();
		virtual ~StreamableChunk() override;

		bool LoadFromFile(const std::filesystem::path& chunkPath, std::string* outError = nullptr);
		// 从内存字节载入(发行包里块在 vpak 内,只能经 Vfs::Read 取字节,没有文件路径)。
		// 与 LoadFromFile 共用同一套解析与校验 —— 载入来源有两处,格式只有一处。
		bool LoadFromMemory(const std::vector<uint8_t>& bytes, const std::string& sourceName = {},
			std::string* outError = nullptr);

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
