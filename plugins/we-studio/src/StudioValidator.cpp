#include "AgentsDocGenerator.h"
#include "StudioValidator.h"
#include "BacklogManager.h"
#include "World/Ai/CommandRegistry.h"
#include "World/WUI/WuiJson.h"
#include "World/Utils/Paths.h"

#include <filesystem>
#include <fstream>

namespace World::Studio
{
	namespace fs = std::filesystem;
	using namespace World::Wui;

	std::string StudioValidationResult::ToJson() const
	{
		JsonValue root;
		root.type = JsonValue::Type::Object;
		root.Object.push_back({ "valid", JsonValue::MakeBool(valid) });

		JsonValue eArr;
		eArr.type = JsonValue::Type::Array;
		for (const auto& e : errors)
			eArr.Array.push_back(JsonValue::MakeString(e));
		root.Object.push_back({ "errors", std::move(eArr) });

		JsonValue wArr;
		wArr.type = JsonValue::Type::Array;
		for (const auto& w : warnings)
			wArr.Array.push_back(JsonValue::MakeString(w));
		root.Object.push_back({ "warnings", std::move(wArr) });

		return root.Dump();
	}

	StudioValidationResult StudioValidator::Validate(const StudioPolicy& policy, const std::string& projectRoot)
	{
		StudioValidationResult res;
		const auto& roles = policy.GetRoles();

		// 1. 策略静态契约校验 (循环评审/自审自过检测 与 写入范围越界检测)
		for (const auto& kv : roles)
		{
			const auto& r = kv.second;

			for (const auto& revTarget : r.reviews)
			{
				if (revTarget == r.id)
				{
					res.errors.push_back("Circular review error: Role '" + r.id + "' cannot review itself.");
					res.valid = false;
				}
			}

			for (const auto& w : r.write)
			{
				if (w.rfind("..", 0) == 0 || w.find("/../") != std::string::npos)
				{
					res.errors.push_back("Illegal write path: Role '" + r.id + "' write scope '" + w + "' escapes project root.");
					res.valid = false;
				}
				if (w.rfind("Engine/", 0) == 0 || w.rfind("Editor/", 0) == 0 || w.rfind("Runtime/", 0) == 0)
				{
					res.errors.push_back("Security violation: Role '" + r.id + "' attempts to write into protected engine tree: " + w);
					res.valid = false;
				}
			}
		}

		// 2. 真实项目客观健康度校验 (坚决杜绝假自检)
		const std::string root = projectRoot.empty() ? Paths::ProjectDir().string() : projectRoot;
		if (!root.empty())
		{
			// 项目级客观健康度:清单与内容根必须真实存在,不再只查角色字符串。
			const fs::path rootPath = fs::u8path(root);
			if (!fs::exists(rootPath / "project.we.yaml"))
			{
				res.errors.push_back("Project manifest missing: " + (rootPath / "project.we.yaml").string());
				res.valid = false;
			}
			if (!fs::exists(rootPath / "assets") || !fs::is_directory(rootPath / "assets"))
			{
				res.errors.push_back("Content root missing: " + (rootPath / "assets").string());
				res.valid = false;
			}
			else
			{
				size_t levelCount = 0;
				std::error_code ec;
				for (const auto& entry : fs::recursive_directory_iterator(rootPath / "assets", ec))
				{
					if (entry.is_regular_file() && entry.path().extension() == ".wlevel")
						++levelCount;
				}
				if (levelCount == 0)
					res.warnings.push_back("No .wlevel scene found under assets/ (nothing playable yet).");
			}

			const auto bugs = BacklogManager::Get().ListBugs(root, "blocker");
			for (const auto& b : bugs)
			{
				if (b.status != "closed" && b.status != "resolved")
				{
					res.errors.push_back("Active Blocker Bug detected: [" + b.id + "] " + b.title);
					res.valid = false;
				}
			}
		}

		res.valid = res.errors.empty();
		return res;
	}

	void StudioValidator::RegisterCommands(World::Ai::CommandRegistry& reg)
	{
		using namespace World::Ai;
		CommandDescriptor desc;
		desc.name = "studio.validate";
		desc.description = "Validate studio preset, role definitions, circular reviews, and active project blocker defects.";
		desc.effect = CommandEffect::Read;
		desc.scope = CommandScope::Both;
		desc.owner = "plugin:we-studio";
		desc.paramsSchema = "{\"type\":\"object\",\"properties\":{\"root\":{\"type\":\"string\"}}}";
		desc.resultSchema = "{\"type\":\"object\",\"properties\":{\"valid\":{\"type\":\"boolean\"},\"errors\":{\"type\":\"array\"}}}";
		desc.handler = [](const std::map<std::string, std::string>& args, std::string& result, std::string& /*error*/)
		{
			const std::string root = args.count("root") ? args.at("root") : "";
			StudioPolicy policy;
			result = Validate(policy, root).ToJson();
			return true;
		};
		reg.RegisterCommand(std::move(desc));

		// studio.generate_agents_doc
		CommandDescriptor docCmd;
		docCmd.name = "studio.generate_agents_doc";
		docCmd.description = "Generate project AGENTS.md and CLAUDE.md guidelines from studio policy.";
		docCmd.effect = CommandEffect::Write;
		docCmd.scope = CommandScope::Both;
		docCmd.owner = "plugin:we-studio";
		docCmd.paramsSchema = R"raw({"type":"object","properties":{"root":{"type":"string"}}})raw";
		docCmd.resultSchema = R"raw({"type":"object","properties":{"generated":{"type":"boolean"}}})raw";
		docCmd.handler = [](const std::map<std::string, std::string>& args, std::string& result, std::string& /*error*/)
		{
			const std::string root = args.count("root") ? args.at("root") : "";
			StudioPolicy policy;
			const bool ok = AgentsDocGenerator::WriteToProject(root, policy);
			JsonValue r;
			r.type = JsonValue::Type::Object;
			r.Object.push_back({ "generated", JsonValue::MakeBool(ok) });
			result = r.Dump();
			return ok;
		};
		reg.RegisterCommand(std::move(docCmd));

		// studio.guide.get:把"良品标准 + 当前角色守则"真正送进模型上下文。
		// 以前 guide 字段被解析后全仓无人读取,模型只拿到"你是制作人"的人设。
		CommandDescriptor guideCmd;
		guideCmd.name = "studio.guide.get";
		guideCmd.description = "Return the Good Game standard plus the guide of a studio role (default: producer).";
		guideCmd.effect = CommandEffect::Read;
		guideCmd.scope = CommandScope::Both;
		guideCmd.owner = "plugin:we-studio";
		guideCmd.paramsSchema = "{\"type\":\"object\",\"properties\":{\"role\":{\"type\":\"string\"},\"root\":{\"type\":\"string\"}}}";
		guideCmd.resultSchema = "{\"type\":\"object\",\"properties\":{\"role\":{\"type\":\"string\"},\"guide\":{\"type\":\"string\"},\"standard\":{\"type\":\"string\"}}}";
		guideCmd.handler = [](const std::map<std::string, std::string>& args, std::string& result, std::string& error)
		{
			StudioPolicy policy;
			const std::string roleId = args.count("role") ? args.at("role") : "producer";
			const auto& roles = policy.GetRoles();
			const auto it = roles.find(roleId);
			if (it == roles.end())
			{
				JsonValue err; err.type = JsonValue::Type::Object;
				JsonValue names; names.type = JsonValue::Type::Array;
				for (const auto& kv : roles)
					names.Array.push_back(JsonValue::MakeString(kv.first));
				err.Object.push_back({ "error", JsonValue::MakeString("unknown role: " + roleId) });
				err.Object.push_back({ "roles", std::move(names) });
				error = "unknown role: " + roleId;
				result = err.Dump();
				return false;
			}

			fs::path presets;
#if defined(WLD_REPO_ROOT)
			presets = fs::u8path(WLD_REPO_ROOT) / "plugins" / "we-studio" / "presets";
#endif
			if (presets.empty() || !fs::exists(presets))
				presets = fs::current_path() / "plugins" / "we-studio" / "presets";

			auto readFile = [](const fs::path& path) -> std::string
			{
				std::ifstream f(path, std::ios::binary);
				if (!f)
					return std::string();
				return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
			};

			const std::string guide = readFile(presets / "default" / it->second.guide);
			const std::string standard = readFile(presets / "default" / "guides" / "good_game_standard.md");

			JsonValue out; out.type = JsonValue::Type::Object;
			out.Object.push_back({ "role", JsonValue::MakeString(roleId) });
			out.Object.push_back({ "guide", JsonValue::MakeString(guide) });
			out.Object.push_back({ "standard", JsonValue::MakeString(standard) });
			result = out.Dump();
			return true;
		};
		reg.RegisterCommand(std::move(guideCmd));

	}
}
