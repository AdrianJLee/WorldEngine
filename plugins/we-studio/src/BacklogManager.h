#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace World::Ai
{
	class CommandRegistry;
}

namespace World::Studio
{
	struct StudioTask
	{
		std::string id;
		std::string title;
		std::string department;
		std::string role;
		std::string stage;
		std::string status = "open"; // open | in_progress | review | done
		std::string acceptanceCriteria;
		std::vector<std::string> outputs;
		std::string description;

		std::string ToMarkdown() const;
		std::string ToJson() const;
	};

	struct StudioBug
	{
		std::string id;
		std::string title;
		std::string severity = "major"; // blocker | critical | major | minor
		std::string status = "open";   // open | in_progress | resolved | closed
		std::string role;
		std::string entity;
		std::string asset;
		std::string repro;

		std::string ToMarkdown() const;
		std::string ToJson() const;
	};

	class BacklogManager
	{
	public:
		static BacklogManager& Get();

		// Task 任务管理
		std::string CreateTask(const std::string& projectRoot, const StudioTask& task);
		bool UpdateTask(const std::string& projectRoot, const std::string& taskId, const std::string& status, const std::string& notes);
		std::vector<StudioTask> ListTasks(const std::string& projectRoot, const std::string& statusFilter = "", const std::string& roleFilter = "") const;

		// Bug 缺陷管理
		std::string CreateBug(const std::string& projectRoot, const StudioBug& bug);
		bool UpdateBug(const std::string& projectRoot, const std::string& bugId, const std::string& status, const std::string& resolution);
		std::vector<StudioBug> ListBugs(const std::string& projectRoot, const std::string& severityFilter = "", const std::string& statusFilter = "") const;

		// 注册命令到 CommandRegistry
		static void RegisterCommands(World::Ai::CommandRegistry& reg);

	private:
		uint64_t m_TaskCounter = 0;
		uint64_t m_BugCounter = 0;
	};
}
