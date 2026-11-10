#pragma once

#include "World/Core/Export.h"

#include <chrono>
#include <filesystem>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace World::Ai
{
	struct WLD_API FileModification
	{
		std::string relativePath;
		std::string backupPath;   // 备份原文件路径
		bool wasCreated = false;  // 原文件是否不存在（即为新创建文件）
		std::string hashBefore;
		std::string hashAfter;
		std::string contentBefore;
		std::string contentAfter;
	};

	struct WLD_API Changeset
	{
		std::string id;
		std::string description;
		std::string role;
		std::string projectRoot;
		std::vector<FileModification> files;
		bool applied = false;
		bool reverted = false;

		std::string ToJson() const;
	};

	class WLD_API ChangesetTracker
	{
	public:
		static ChangesetTracker& Get();

		// 开始一个新的变更集
		std::string BeginChangeset(const std::string& projectRoot,
			const std::string& role,
			const std::string& description = "");

		// 在指定变更集暂存并安全写入一个文件（自动备份原文件镜像，并通过 WriteGuard 校验）
		bool StageFileWrite(const std::string& changesetId,
			const std::string& projectRoot,
			const std::string& relativePath,
			const std::string& newContent,
			std::string& outError);

		// 应用/确认变更集
		bool ApplyChangeset(const std::string& changesetId,
			const std::string& projectRoot,
			std::string& outError);

		// 原子撤销变更集（逐字节恢复原文件或删除新建文件）
		bool RevertChangeset(const std::string& changesetId,
			const std::string& projectRoot,
			std::string& outError);

		// 生成变更集可读 Unified Diff 预览
		std::string GetChangesetDiff(const std::string& changesetId) const;

		// 获取变更集详情
		const Changeset* GetChangeset(const std::string& changesetId) const;

		// 列出项目全量变更集
		std::vector<Changeset> ListChangesets(const std::string& projectRoot = "") const;

		// 清理内存状态（测试用）
		void Clear();

		static void RegisterCommands(class CommandRegistry& reg);

	private:
		mutable std::mutex m_Mutex;
		std::map<std::string, Changeset> m_Changesets;
		uint64_t m_Counter = 0;
	};
}
