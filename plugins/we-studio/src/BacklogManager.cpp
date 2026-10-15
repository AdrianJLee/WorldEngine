#include "BacklogManager.h"
#include "World/Ai/CommandRegistry.h"
#include "World/WUI/WuiJson.h"
#include "World/Utils/Paths.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace World::Studio
{
	namespace fs = std::filesystem;
	using namespace World::Wui;

	std::string StudioTask::ToMarkdown() const
	{
		std::ostringstream ss;
		ss << "---\n";
		ss << "id: " << id << "\n";
		ss << "title: " << title << "\n";
		ss << "department: " << department << "\n";
		ss << "role: " << role << "\n";
		ss << "stage: " << stage << "\n";
		ss << "status: " << status << "\n";
		if (!acceptanceCriteria.empty())
		{
			ss << "acceptance_criteria: \"" << acceptanceCriteria << "\"\n";
		}
		ss << "outputs: [";
		for (size_t i = 0; i < outputs.size(); ++i)
		{
			if (i) ss << ", ";
			ss << outputs[i];
		}
		ss << "]\n";
		ss << "---\n\n";
		ss << "## 任务目标与技术规范\n" << description << "\n";
		if (!acceptanceCriteria.empty())
		{
			ss << "\n## 验收准则与门禁指标 (Definition of Done)\n- " << acceptanceCriteria << "\n";
		}
		return ss.str();
	}

	std::string StudioTask::ToJson() const
	{
		JsonValue root;
		root.type = JsonValue::Type::Object;
		root.Object.push_back({ "id", JsonValue::MakeString(id) });
		root.Object.push_back({ "title", JsonValue::MakeString(title) });
		root.Object.push_back({ "role", JsonValue::MakeString(role) });
		root.Object.push_back({ "stage", JsonValue::MakeString(stage) });
		root.Object.push_back({ "status", JsonValue::MakeString(status) });
		root.Object.push_back({ "description", JsonValue::MakeString(description) });

		JsonValue outArr; outArr.type = JsonValue::Type::Array;
		for (const auto& o : outputs) outArr.Array.push_back(JsonValue::MakeString(o));
		root.Object.push_back({ "outputs", std::move(outArr) });

		return root.Dump();
	}

	std::string StudioBug::ToMarkdown() const
	{
		std::ostringstream ss;
		ss << "---\n";
		ss << "id: " << id << "\n";
		ss << "title: " << title << "\n";
		ss << "severity: " << severity << "\n";
		ss << "status: " << status << "\n";
		ss << "role: " << role << "\n";
		ss << "entity: " << entity << "\n";
		ss << "asset: " << asset << "\n";
		ss << "---\n\n";
		ss << repro << "\n";
		return ss.str();
	}

	std::string StudioBug::ToJson() const
	{
		JsonValue root;
		root.type = JsonValue::Type::Object;
		root.Object.push_back({ "id", JsonValue::MakeString(id) });
		root.Object.push_back({ "title", JsonValue::MakeString(title) });
		root.Object.push_back({ "severity", JsonValue::MakeString(severity) });
		root.Object.push_back({ "status", JsonValue::MakeString(status) });
		root.Object.push_back({ "role", JsonValue::MakeString(role) });
		root.Object.push_back({ "entity", JsonValue::MakeString(entity) });
		root.Object.push_back({ "asset", JsonValue::MakeString(asset) });
		root.Object.push_back({ "repro", JsonValue::MakeString(repro) });
		return root.Dump();
	}

	namespace
	{
		std::string MapRoleToDepartment(const std::string& role)
		{
			std::string r = role;
			std::transform(r.begin(), r.end(), r.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

			if (r.find("creative") != std::string::npos || r.find("designer") != std::string::npos || r.find("narrative") != std::string::npos)
				return "creative";
			if (r.find("programmer") != std::string::npos || r.find("tech") != std::string::npos || r.find("engine") != std::string::npos || r.find("graphics") != std::string::npos)
				return "tech";
			if (r.find("art") != std::string::npos || r.find("vfx") != std::string::npos || r.find("ui") != std::string::npos || r.find("concept") != std::string::npos)
				return "art";
			if (r.find("audio") != std::string::npos || r.find("composer") != std::string::npos || r.find("experience") != std::string::npos || r.find("sound") != std::string::npos)
				return "experience";
			if (r.find("qa") != std::string::npos || r.find("test") != std::string::npos)
				return "qa";
			if (r.find("market") != std::string::npos || r.find("publish") != std::string::npos || r.find("ops") != std::string::npos)
				return "publishing";
			return "general";
		}
	}

	BacklogManager& BacklogManager::Get()
	{
		static BacklogManager instance;
		return instance;
	}

	std::string BacklogManager::CreateTask(const std::string& projectRoot, const StudioTask& task)
	{
		const fs::path root = fs::u8path(projectRoot.empty() ? Paths::ProjectDir().string() : projectRoot);
		const fs::path backlogDir = root / ".we" / "studio" / "backlog";

		StudioTask t = task;
		if (t.department.empty())
		{
			t.department = MapRoleToDepartment(t.role);
		}

		// 按专业部门归口存放: .we/studio/backlog/<department>/
		const fs::path deptDir = backlogDir / t.department;
		std::error_code ec;
		fs::create_directories(deptDir, ec);

		if (t.id.empty())
		{
			const auto now = std::chrono::system_clock::now().time_since_epoch().count();
			t.id = "TASK-" + std::to_string(now) + "-" + std::to_string(++m_TaskCounter);
		}

		const fs::path filePath = deptDir / (t.id + ".md");
		std::ofstream f(filePath);
		if (f)
		{
			f << t.ToMarkdown();
		}

		return t.id;
	}

	bool BacklogManager::UpdateTask(const std::string& projectRoot, const std::string& taskId, const std::string& status, const std::string& notes)
	{
		const fs::path root = fs::u8path(projectRoot.empty() ? Paths::ProjectDir().string() : projectRoot);
		const fs::path backlogDir = root / ".we" / "studio" / "backlog";

		fs::path filePath = backlogDir / (taskId + ".md");
		if (!fs::exists(filePath))
		{
			// 递归在部门子目录中查找任务文件
			std::error_code ec;
			for (const auto& entry : fs::recursive_directory_iterator(backlogDir, ec))
			{
				if (entry.is_regular_file() && entry.path().stem() == taskId)
				{
					filePath = entry.path();
					break;
				}
			}
		}

		if (!fs::exists(filePath))
			return false;

		std::ifstream in(filePath);
		std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
		in.close();

		// 更新 status 字段
		const size_t pos = content.find("status: ");
		if (pos != std::string::npos)
		{
			const size_t eol = content.find('\n', pos);
			if (eol != std::string::npos)
			{
				content.replace(pos, eol - pos, "status: " + status);
			}
		}

		if (!notes.empty())
		{
			content += "\n\n## Update Notes\n" + notes + "\n";
		}

		std::ofstream out(filePath, std::ios::trunc);
		if (out)
		{
			out << content;
			return true;
		}

		return false;
	}

	std::vector<StudioTask> BacklogManager::ListTasks(const std::string& projectRoot, const std::string& statusFilter, const std::string& roleFilter) const
	{
		std::vector<StudioTask> list;
		const fs::path root = fs::u8path(projectRoot.empty() ? Paths::ProjectDir().string() : projectRoot);
		const fs::path backlogDir = root / ".we" / "studio" / "backlog";

		if (!fs::exists(backlogDir))
			return list;

		std::error_code ec;
		for (const auto& entry : fs::recursive_directory_iterator(backlogDir, ec))
		{
			if (!entry.is_regular_file() || entry.path().extension() != ".md")
				continue;

			std::ifstream f(entry.path());
			std::string line;
			StudioTask t;
			t.id = entry.path().stem().string();
			if (entry.path().parent_path() != backlogDir)
			{
				t.department = entry.path().parent_path().filename().string();
			}

			bool inFrontMatter = false;
			while (std::getline(f, line))
			{
				if (line == "---")
				{
					inFrontMatter = !inFrontMatter;
					continue;
				}
				if (inFrontMatter)
				{
					if (line.rfind("title: ", 0) == 0) t.title = line.substr(7);
					else if (line.rfind("role: ", 0) == 0) t.role = line.substr(6);
					else if (line.rfind("stage: ", 0) == 0) t.stage = line.substr(7);
					else if (line.rfind("status: ", 0) == 0) t.status = line.substr(8);
				}
				else
				{
					t.description += line + "\n";
				}
			}

			if (!statusFilter.empty() && t.status != statusFilter)
				continue;
			if (!roleFilter.empty() && t.role != roleFilter)
				continue;

			list.push_back(std::move(t));
		}

		return list;
	}

	std::string BacklogManager::CreateBug(const std::string& projectRoot, const StudioBug& bug)
	{
		const fs::path root = fs::u8path(projectRoot.empty() ? Paths::ProjectDir().string() : projectRoot);
		const fs::path bugsDir = root / ".we" / "studio" / "bugs";

		std::error_code ec;
		fs::create_directories(bugsDir, ec);

		StudioBug b = bug;
		if (b.id.empty())
		{
			const auto now = std::chrono::system_clock::now().time_since_epoch().count();
			b.id = "BUG-" + std::to_string(now) + "-" + std::to_string(++m_BugCounter);
		}

		const fs::path filePath = bugsDir / (b.id + ".md");
		std::ofstream f(filePath);
		if (f)
		{
			f << b.ToMarkdown();
		}

		return b.id;
	}

	bool BacklogManager::UpdateBug(const std::string& projectRoot, const std::string& bugId, const std::string& status, const std::string& resolution)
	{
		const fs::path root = fs::u8path(projectRoot.empty() ? Paths::ProjectDir().string() : projectRoot);
		const fs::path filePath = root / ".we" / "studio" / "bugs" / (bugId + ".md");

		if (!fs::exists(filePath))
			return false;

		std::ifstream in(filePath);
		std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
		in.close();

		const size_t pos = content.find("status: ");
		if (pos != std::string::npos)
		{
			const size_t eol = content.find('\n', pos);
			if (eol != std::string::npos)
			{
				content.replace(pos, eol - pos, "status: " + status);
			}
		}

		if (!resolution.empty())
		{
			content += "\n\n## Resolution\n" + resolution + "\n";
		}

		std::ofstream out(filePath, std::ios::trunc);
		if (out)
		{
			out << content;
			return true;
		}

		return false;
	}

	std::vector<StudioBug> BacklogManager::ListBugs(const std::string& projectRoot, const std::string& severityFilter, const std::string& statusFilter) const
	{
		std::vector<StudioBug> list;
		const fs::path root = fs::u8path(projectRoot.empty() ? Paths::ProjectDir().string() : projectRoot);
		const fs::path bugsDir = root / ".we" / "studio" / "bugs";

		if (!fs::exists(bugsDir))
			return list;

		std::error_code ec;
		for (const auto& entry : fs::directory_iterator(bugsDir, ec))
		{
			if (!entry.is_regular_file() || entry.path().extension() != ".md")
				continue;

			std::ifstream f(entry.path());
			std::string line;
			StudioBug b;
			b.id = entry.path().stem().string();

			bool inFrontMatter = false;
			while (std::getline(f, line))
			{
				if (line == "---")
				{
					inFrontMatter = !inFrontMatter;
					continue;
				}
				if (inFrontMatter)
				{
					if (line.rfind("title: ", 0) == 0) b.title = line.substr(7);
					else if (line.rfind("severity: ", 0) == 0) b.severity = line.substr(10);
					else if (line.rfind("status: ", 0) == 0) b.status = line.substr(8);
					else if (line.rfind("role: ", 0) == 0) b.role = line.substr(6);
					else if (line.rfind("entity: ", 0) == 0) b.entity = line.substr(8);
					else if (line.rfind("asset: ", 0) == 0) b.asset = line.substr(7);
				}
				else
				{
					b.repro += line + "\n";
				}
			}

			if (!severityFilter.empty() && b.severity != severityFilter)
				continue;
			if (!statusFilter.empty() && b.status != statusFilter)
				continue;

			list.push_back(std::move(b));
		}

		return list;
	}

	void BacklogManager::RegisterCommands(World::Ai::CommandRegistry& reg)
	{
		using namespace World::Ai;

		// 1. task.create
		CommandDescriptor taskCreateCmd;
		taskCreateCmd.name = "task.create";
		taskCreateCmd.description = "Create a project backlog work item (.we/studio/backlog/<id>.md).";
		taskCreateCmd.effect = CommandEffect::Write;
		taskCreateCmd.scope = CommandScope::Both;
		taskCreateCmd.owner = "plugin:we-studio";
		taskCreateCmd.paramsSchema = "{\"type\":\"object\",\"properties\":{\"title\":{\"type\":\"string\"},\"role\":{\"type\":\"string\"},\"stage\":{\"type\":\"string\"},\"desc\":{\"type\":\"string\"}},\"required\":[\"title\",\"role\"]}";
		taskCreateCmd.resultSchema = "{\"type\":\"object\",\"properties\":{\"task_id\":{\"type\":\"string\"}}}";
		taskCreateCmd.handler = [](const std::map<std::string, std::string>& args, std::string& result, std::string& error)
		{
			StudioTask t;
			t.title = args.count("title") ? args.at("title") : "";
			t.role = args.count("role") ? args.at("role") : "";
			if (t.title.empty() || t.role.empty())
			{
				error = "Missing required title or role";
				return false;
			}
			t.stage = args.count("stage") ? args.at("stage") : "prototype";
			t.description = args.count("desc") ? args.at("desc") : "";
			const std::string root = args.count("root") ? args.at("root") : "";

			const std::string id = Get().CreateTask(root, t);
			JsonValue r; r.type = JsonValue::Type::Object;
			r.Object.push_back({ "task_id", JsonValue::MakeString(id) });
			result = r.Dump();
			return true;
		};
		reg.RegisterCommand(std::move(taskCreateCmd));

		// 2. task.list
		CommandDescriptor taskListCmd;
		taskListCmd.name = "task.list";
		taskListCmd.description = "List backlog tasks with optional status/role filter.";
		taskListCmd.effect = CommandEffect::Read;
		taskListCmd.scope = CommandScope::Both;
		taskListCmd.owner = "plugin:we-studio";
		taskListCmd.paramsSchema = "{\"type\":\"object\",\"properties\":{\"status\":{\"type\":\"string\"},\"role\":{\"type\":\"string\"},\"root\":{\"type\":\"string\"}}}";
		taskListCmd.resultSchema = "{\"type\":\"object\",\"properties\":{\"tasks\":{\"type\":\"array\"}}}";
		taskListCmd.handler = [](const std::map<std::string, std::string>& args, std::string& result, std::string& /*error*/)
		{
			const std::string root = args.count("root") ? args.at("root") : "";
			const std::string status = args.count("status") ? args.at("status") : "";
			const std::string role = args.count("role") ? args.at("role") : "";

			const auto list = Get().ListTasks(root, status, role);
			JsonValue r; r.type = JsonValue::Type::Object;
			JsonValue arr; arr.type = JsonValue::Type::Array;
			for (const auto& item : list)
			{
				auto opt = JsonValue::Parse(item.ToJson(), nullptr);
				if (opt) arr.Array.push_back(std::move(*opt));
			}
			r.Object.push_back({ "tasks", std::move(arr) });
			result = r.Dump();
			return true;
		};
		reg.RegisterCommand(std::move(taskListCmd));

		// 3. bug.create
		CommandDescriptor bugCreateCmd;
		bugCreateCmd.name = "bug.create";
		bugCreateCmd.description = "File a project bug (.we/studio/bugs/<id>.md).";
		bugCreateCmd.effect = CommandEffect::Write;
		bugCreateCmd.scope = CommandScope::Both;
		bugCreateCmd.owner = "plugin:we-studio";
		bugCreateCmd.paramsSchema = "{\"type\":\"object\",\"properties\":{\"title\":{\"type\":\"string\"},\"severity\":{\"type\":\"string\"},\"repro\":{\"type\":\"string\"}},\"required\":[\"title\"]}";
		bugCreateCmd.resultSchema = "{\"type\":\"object\",\"properties\":{\"bug_id\":{\"type\":\"string\"}}}";
		bugCreateCmd.handler = [](const std::map<std::string, std::string>& args, std::string& result, std::string& error)
		{
			StudioBug b;
			b.title = args.count("title") ? args.at("title") : "";
			if (b.title.empty())
			{
				error = "Missing required title";
				return false;
			}
			b.severity = args.count("severity") ? args.at("severity") : "major";
			b.repro = args.count("repro") ? args.at("repro") : "";
			b.role = args.count("role") ? args.at("role") : "qa";
			b.entity = args.count("entity") ? args.at("entity") : "";
			b.asset = args.count("asset") ? args.at("asset") : "";
			const std::string root = args.count("root") ? args.at("root") : "";

			const std::string id = Get().CreateBug(root, b);
			JsonValue r; r.type = JsonValue::Type::Object;
			r.Object.push_back({ "bug_id", JsonValue::MakeString(id) });
			result = r.Dump();
			return true;
		};
		reg.RegisterCommand(std::move(bugCreateCmd));

		// 4. bug.list
		CommandDescriptor bugListCmd;
		bugListCmd.name = "bug.list";
		bugListCmd.description = "List filed bugs with optional severity/status filter.";
		bugListCmd.effect = CommandEffect::Read;
		bugListCmd.scope = CommandScope::Both;
		bugListCmd.owner = "plugin:we-studio";
		bugListCmd.paramsSchema = "{\"type\":\"object\",\"properties\":{\"severity\":{\"type\":\"string\"},\"status\":{\"type\":\"string\"},\"root\":{\"type\":\"string\"}}}";
		bugListCmd.resultSchema = "{\"type\":\"object\",\"properties\":{\"bugs\":{\"type\":\"array\"}}}";
		bugListCmd.handler = [](const std::map<std::string, std::string>& args, std::string& result, std::string& /*error*/)
		{
			const std::string root = args.count("root") ? args.at("root") : "";
			const std::string sev = args.count("severity") ? args.at("severity") : "";
			const std::string status = args.count("status") ? args.at("status") : "";

			const auto list = Get().ListBugs(root, sev, status);
			JsonValue r; r.type = JsonValue::Type::Object;
			JsonValue arr; arr.type = JsonValue::Type::Array;
			for (const auto& item : list)
			{
				auto opt = JsonValue::Parse(item.ToJson(), nullptr);
				if (opt) arr.Array.push_back(std::move(*opt));
			}
			r.Object.push_back({ "bugs", std::move(arr) });
			result = r.Dump();
			return true;
		};
		reg.RegisterCommand(std::move(bugListCmd));

		// 5. task.update
		CommandDescriptor taskUpdateCmd;
		taskUpdateCmd.name = "task.update";
		taskUpdateCmd.description = "Update task status (open, in_progress, review, done) and notes.";
		taskUpdateCmd.effect = CommandEffect::Write;
		taskUpdateCmd.scope = CommandScope::Both;
		taskUpdateCmd.owner = "plugin:we-studio";
		taskUpdateCmd.paramsSchema = R"raw({"type":"object","properties":{"task_id":{"type":"string"},"status":{"type":"string"},"notes":{"type":"string"}},"required":["task_id","status"]})raw";
		taskUpdateCmd.resultSchema = R"raw({"type":"object","properties":{"updated":{"type":"boolean"}}})raw";
		taskUpdateCmd.handler = [](const std::map<std::string, std::string>& args, std::string& result, std::string& error)
		{
			const std::string id = args.count("task_id") ? args.at("task_id") : (args.count("id") ? args.at("id") : "");
			const std::string status = args.count("status") ? args.at("status") : "";
			if (id.empty() || status.empty())
			{
				error = "Missing required task_id or status";
				return false;
			}
			const std::string notes = args.count("notes") ? args.at("notes") : "";
			const std::string root = args.count("root") ? args.at("root") : "";

			const bool ok = Get().UpdateTask(root, id, status, notes);
			JsonValue r; r.type = JsonValue::Type::Object;
			r.Object.push_back({ "updated", JsonValue::MakeBool(ok) });
			result = r.Dump();
			return ok;
		};
		reg.RegisterCommand(std::move(taskUpdateCmd));

		// 6. bug.update
		CommandDescriptor bugUpdateCmd;
		bugUpdateCmd.name = "bug.update";
		bugUpdateCmd.description = "Update bug status (open, in_progress, resolved, closed) and resolution.";
		bugUpdateCmd.effect = CommandEffect::Write;
		bugUpdateCmd.scope = CommandScope::Both;
		bugUpdateCmd.owner = "plugin:we-studio";
		bugUpdateCmd.paramsSchema = R"raw({"type":"object","properties":{"bug_id":{"type":"string"},"status":{"type":"string"},"resolution":{"type":"string"}},"required":["bug_id","status"]})raw";
		bugUpdateCmd.resultSchema = R"raw({"type":"object","properties":{"updated":{"type":"boolean"}}})raw";
		bugUpdateCmd.handler = [](const std::map<std::string, std::string>& args, std::string& result, std::string& error)
		{
			const std::string id = args.count("bug_id") ? args.at("bug_id") : (args.count("id") ? args.at("id") : "");
			const std::string status = args.count("status") ? args.at("status") : "";
			if (id.empty() || status.empty())
			{
				error = "Missing required bug_id or status";
				return false;
			}
			const std::string res = args.count("resolution") ? args.at("resolution") : "";
			const std::string root = args.count("root") ? args.at("root") : "";

			const bool ok = Get().UpdateBug(root, id, status, res);
			JsonValue r; r.type = JsonValue::Type::Object;
			r.Object.push_back({ "updated", JsonValue::MakeBool(ok) });
			result = r.Dump();
			return ok;
		};
		reg.RegisterCommand(std::move(bugUpdateCmd));

		// 7. review.submit
		CommandDescriptor reviewSubmitCmd;
		reviewSubmitCmd.name = "review.submit";
		reviewSubmitCmd.description = "Submit role/department review for task (verdict: approved, rejected, blocker).";
		reviewSubmitCmd.effect = CommandEffect::Write;
		reviewSubmitCmd.scope = CommandScope::Both;
		reviewSubmitCmd.owner = "plugin:we-studio";
		reviewSubmitCmd.paramsSchema = R"raw({"type":"object","properties":{"task_id":{"type":"string"},"reviewer":{"type":"string"},"verdict":{"type":"string"},"comments":{"type":"string"}},"required":["task_id","reviewer","verdict"]})raw";
		reviewSubmitCmd.resultSchema = R"raw({"type":"object","properties":{"submitted":{"type":"boolean"},"blocking":{"type":"boolean"}}})raw";
		reviewSubmitCmd.handler = [](const std::map<std::string, std::string>& args, std::string& result, std::string& error)
		{
			const std::string taskId = args.count("task_id") ? args.at("task_id") : "";
			const std::string reviewer = args.count("reviewer") ? args.at("reviewer") : "";
			const std::string verdict = args.count("verdict") ? args.at("verdict") : "";
			if (taskId.empty() || reviewer.empty() || verdict.empty())
			{
				error = "Missing required task_id, reviewer, or verdict";
				return false;
			}
			const std::string comments = args.count("comments") ? args.at("comments") : "";
			const std::string root = args.count("root") ? args.at("root") : "";

			const bool isBlocker = (verdict == "blocker");
			if (isBlocker)
			{
				StudioBug b;
				b.title = "Review Blocker for " + taskId + ": " + comments;
				b.severity = "blocker";
				b.role = reviewer;
				b.status = "open";
				Get().CreateBug(root, b);
			}

			std::string newStatus = (verdict == "approved") ? "done" : "review";
			Get().UpdateTask(root, taskId, newStatus, "Review by " + reviewer + ": " + verdict + " (" + comments + ")");

			JsonValue r; r.type = JsonValue::Type::Object;
			r.Object.push_back({ "submitted", JsonValue::MakeBool(true) });
			r.Object.push_back({ "blocking", JsonValue::MakeBool(isBlocker) });
			result = r.Dump();
			return true;
		};
		reg.RegisterCommand(std::move(reviewSubmitCmd));
	}
}
