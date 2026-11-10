#pragma once

#include "World/Core/Export.h"
#include "World/Level/Core/ChunkHeader.h"
#include "World/Level/Core/ChunkTypes.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace World::Level
{
	// 烘焙后关卡里的一块流送单元(索引条目)。
	//
	// 索引存在的原因:发行包(vpak)里**没有目录遍历**能力,`Vfs` 只有 Exists/Read。
	// 因此 cook 必须写一份显式清单,运行期才知道"这个关卡有哪几块、各自在哪、包围盒多大"。
	// 空间驱动器要靠包围盒在**不打开任何块**的前提下决定该流送谁。
	struct ChunkIndexEntry
	{
		uint64_t ChunkID{ 0 };
		std::string File;              // 相对索引文件所在目录的文件名
		ChunkBounds Bounds;
		uint32_t EntityCount{ 0 };
		uint32_t DataLayerMask{ 0 };
	};

	// `index.yaml`:一个关卡的流送块清单(与块文件同目录)。开发树与发行包同形 ——
	// 前者走文件系统,后者走 Vfs,读的都是这一个已知路径。
	class WLD_API ChunkIndex
	{
	public:
		static constexpr const char* FileName = "index.yaml";

		const std::vector<ChunkIndexEntry>& Entries() const { return m_Entries; }
		std::vector<ChunkIndexEntry>& Entries() { return m_Entries; }
		bool IsEmpty() const { return m_Entries.empty(); }

		// 索引目录下的文件 → 块文件的绝对/物理路径(不做存在性检查)。
		std::filesystem::path ResolveFile(const std::filesystem::path& indexDir,
			const ChunkIndexEntry& entry) const;

		static bool Parse(const std::string& yaml, ChunkIndex* out, std::string* error = nullptr);
		static bool LoadFromFile(const std::filesystem::path& path, ChunkIndex* out,
			std::string* error = nullptr);
		static bool Save(const std::filesystem::path& path, const ChunkIndex& index,
			std::string* error = nullptr);

	private:
		std::vector<ChunkIndexEntry> m_Entries;
	};
}
