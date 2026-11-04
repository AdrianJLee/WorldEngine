#include "InterviewSystem.h"
#include "World/Ai/CommandRegistry.h"
#include "World/WUI/WuiJson.h"
#include "World/Utils/Paths.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>

namespace World::Studio
{
	namespace fs = std::filesystem;
	using namespace World::Wui;

	std::string InterviewQuestion::ToJson() const
	{
		JsonValue root;
		root.type = JsonValue::Type::Object;
		root.Object.push_back({ "id", JsonValue::MakeString(id) });
		root.Object.push_back({ "header", JsonValue::MakeString(header) });
		root.Object.push_back({ "prompt", JsonValue::MakeString(prompt) });
		root.Object.push_back({ "selected", JsonValue::MakeString(selectedOptionId) });

		JsonValue optArr;
		optArr.type = JsonValue::Type::Array;
		for (const auto& o : options)
		{
			JsonValue oObj;
			oObj.type = JsonValue::Type::Object;
			oObj.Object.push_back({ "id", JsonValue::MakeString(o.id) });
			oObj.Object.push_back({ "label", JsonValue::MakeString(o.label) });
			oObj.Object.push_back({ "description", JsonValue::MakeString(o.description) });
			oObj.Object.push_back({ "recommended", JsonValue::MakeBool(o.isRecommended) });
			optArr.Array.push_back(std::move(oObj));
		}
		root.Object.push_back({ "options", std::move(optArr) });
		return root.Dump();
	}

	std::string InterviewRound::ToJson() const
	{
		JsonValue root;
		root.type = JsonValue::Type::Object;
		root.Object.push_back({ "round", JsonValue::MakeNumber(static_cast<double>(round)) });

		JsonValue qArr;
		qArr.type = JsonValue::Type::Array;
		for (const auto& q : questions)
		{
			auto opt = JsonValue::Parse(q.ToJson(), nullptr);
			if (opt)
				qArr.Array.push_back(std::move(*opt));
		}
		root.Object.push_back({ "questions", std::move(qArr) });
		return root.Dump();
	}

	InterviewSystem& InterviewSystem::Get()
	{
		static InterviewSystem instance;
		return instance;
	}

	InterviewRound InterviewSystem::GetCurrentRound(const std::string& /*projectRoot*/, int stage)
	{
		InterviewRound ir;
		ir.round = stage + 1;

		if (stage == 0) // Concept
		{
			InterviewQuestion q1;
			q1.id = "genre_type";
			q1.header = "游戏品类";
			q1.prompt = "您最想做的核心玩法与题材方向是什么？";
			q1.options = {
				{ "2d_platformer", "2D 动作平台跳跃 (Recommended)", "爽快手感、精准跳跃与像素/手绘风格", true },
				{ "3d_action", "3D 次世代动作", "第三人称写实、PBR 高精度与连招战斗", false },
				{ "strategy", "深度数值策略", "网格战棋、卡牌构筑与重策略决策", false }
			};
			ir.questions.push_back(std::move(q1));

			InterviewQuestion q2;
			q2.id = "art_style";
			q2.header = "美术风格";
			q2.prompt = "项目的首要视觉倾向是什么？";
			q2.options = {
				{ "pixel_16bit", "复古 16-bit 精品像素 (Recommended)", "经典低饱和色盘、高辨识度与极速迭代", true },
				{ "stylized_3d", "美式卡通/二次元风格化 3D", "明亮色调、Cel-Shading 描边与轻量资产", false },
				{ "nextgen_pbr", "次世代真实写实", "金属粗糙度 PBR、体积光照与高精法线", false }
			};
			ir.questions.push_back(std::move(q2));

			InterviewQuestion q3;
			q3.id = "target_scale";
			q3.header = "项目规模";
			q3.prompt = "期望单局体验时长与预期发布形态？";
			q3.options = {
				{ "compact_indie", "精品独立小品 (Recommended)", "单局 10-15 分钟，总流程 3-5 小时", true },
				{ "jam_rapid", "Game Jam 极速原型", "单局 3 分钟，极速找乐与玩法验证", false }
			};
			ir.questions.push_back(std::move(q3));
		}
		else
		{
			InterviewQuestion q;
			q.id = "stage_focus";
			q.header = "阶段目标";
			q.prompt = "本阶段工作流首要交付焦点？";
			q.options = {
				{ "focus_gameplay", "手感与核心循环 (Recommended)", "打磨连击手感与手柄震动响应", true },
				{ "focus_visual", "高光视效样板", "确立代表最高品质的垂直切片渲染表现", false }
			};
			ir.questions.push_back(std::move(q));
		}

		return ir;
	}

	bool InterviewSystem::AnswerQuestion(const std::string& projectRoot, const std::string& questionId, const std::string& optionId)
	{
		const fs::path root = fs::u8path(projectRoot.empty() ? Paths::ProjectDir().string() : projectRoot);
		if (!fs::exists(root))
			return false;

		std::error_code ec;
		const fs::path studioDir = root / ".we" / "studio";
		fs::create_directories(studioDir, ec);

		// 1. 结构化读取与更新 taste.yaml (杜绝重复 key 堆叠)
		std::map<std::string, std::string> tasteMap;
		const fs::path tastePath = studioDir / "taste.yaml";
		if (fs::exists(tastePath))
		{
			std::ifstream in(tastePath);
			std::string line;
			while (std::getline(in, line))
			{
				size_t colon = line.find(':');
				if (colon != std::string::npos)
				{
					std::string k = line.substr(0, colon);
					std::string v = line.substr(colon + 1);
					while (!k.empty() && (k.front() == ' ' || k.front() == '\t')) k.erase(0, 1);
					while (!k.empty() && (k.back() == ' ' || k.back() == '\t')) k.pop_back();
					while (!v.empty() && (v.front() == ' ' || v.front() == '\t' || v.front() == '"')) v.erase(0, 1);
					while (!v.empty() && (v.back() == ' ' || v.back() == '\t' || v.back() == '"')) v.pop_back();
					if (!k.empty())
						tasteMap[k] = v;
				}
			}
		}
		tasteMap[questionId] = optionId;

		std::ofstream outTaste(tastePath, std::ios::trunc);
		if (outTaste)
		{
			for (const auto& kv : tasteMap)
			{
				outTaste << kv.first << ": \"" << kv.second << "\"\n";
			}
		}

		// 2. 沉淀进 assumptions.md
		const auto now = std::chrono::system_clock::now().time_since_epoch().count();
		const fs::path assumptionsPath = studioDir / "assumptions.md";
		std::ofstream af(assumptionsPath, std::ios::app);
		if (af)
		{
			af << "- **[Assumption " << now << "]** `" << questionId << "`: Selected `" << optionId << "`.\n";
		}

		return true;
	}

	std::string InterviewSystem::GetTasteProfile(const std::string& projectRoot) const
	{
		const fs::path root = fs::u8path(projectRoot.empty() ? Paths::ProjectDir().string() : projectRoot);
		const fs::path tasteStd = root / ".we" / "studio" / "taste.yaml";
		const fs::path tasteRoot = root / "taste.yaml";

		const fs::path target = fs::exists(tasteStd) ? tasteStd : (fs::exists(tasteRoot) ? tasteRoot : fs::path());
		if (!target.empty())
		{
			std::ifstream f(target);
			return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
		}
		return "taste_profile: default\n";
	}

	void InterviewSystem::RegisterCommands(World::Ai::CommandRegistry& reg)
	{
		using namespace World::Ai;

		auto registerBoth = [&](CommandDescriptor desc, const std::string& aliasName)
		{
			std::string primaryName = desc.name;
			reg.RegisterCommand(desc);
			desc.name = aliasName;
			reg.RegisterCommand(std::move(desc));
		};

		// 1. studio.interview.get / assistant.interview.get
		{
			CommandDescriptor getCmd;
			getCmd.name = "studio.interview.get";
			getCmd.description = "Get L1 AI Producer 3-question interview card for current stage.";
			getCmd.effect = CommandEffect::Read;
			getCmd.scope = CommandScope::Both;
			getCmd.owner = "plugin:we-studio";
			getCmd.paramsSchema = "{\"type\":\"object\",\"properties\":{\"stage\":{\"type\":\"integer\"},\"root\":{\"type\":\"string\"}}}";
			getCmd.resultSchema = "{\"type\":\"object\",\"properties\":{\"questions\":{\"type\":\"array\"}}}";
			getCmd.handler = [](const std::map<std::string, std::string>& args, std::string& result, std::string& /*error*/)
			{
				int st = 0;
				if (args.count("stage"))
				{
					try { st = std::stoi(args.at("stage")); } catch (...) {}
				}
				const std::string root = args.count("root") ? args.at("root") : "";
				result = Get().GetCurrentRound(root, st).ToJson();
				return true;
			};
			registerBoth(std::move(getCmd), "assistant.interview.get");
		}

		// 2. studio.interview.answer / assistant.interview.answer
		{
			CommandDescriptor ansCmd;
			ansCmd.name = "studio.interview.answer";
			ansCmd.description = "Answer interview question and log to assumptions.md and taste.yaml.";
			ansCmd.effect = CommandEffect::Write;
			ansCmd.scope = CommandScope::Both;
			ansCmd.owner = "plugin:we-studio";
			ansCmd.paramsSchema = "{\"type\":\"object\",\"properties\":{\"question\":{\"type\":\"string\"},\"option\":{\"type\":\"string\"},\"root\":{\"type\":\"string\"}},\"required\":[\"question\",\"option\"]}";
			ansCmd.resultSchema = "{\"type\":\"object\",\"properties\":{\"success\":{\"type\":\"boolean\"}}}";
			ansCmd.handler = [](const std::map<std::string, std::string>& args, std::string& result, std::string& error)
			{
				if (!args.count("question") || !args.count("option"))
				{
					error = "Missing question or option";
					return false;
				}
				const std::string root = args.count("root") ? args.at("root") : "";
				bool ok = Get().AnswerQuestion(root, args.at("question"), args.at("option"));
				result = "{\"success\":" + std::string(ok ? "true" : "false") + "}";
				return ok;
			};
			registerBoth(std::move(ansCmd), "assistant.interview.answer");
		}

		// 3. studio.taste.get / assistant.taste.get
		{
			CommandDescriptor tasteCmd;
			tasteCmd.name = "studio.taste.get";
			tasteCmd.description = "Get project taste profile preferences.";
			tasteCmd.effect = CommandEffect::Read;
			tasteCmd.scope = CommandScope::Both;
			tasteCmd.owner = "plugin:we-studio";
			tasteCmd.paramsSchema = "{\"type\":\"object\",\"properties\":{\"root\":{\"type\":\"string\"}}}";
			tasteCmd.resultSchema = "{\"type\":\"object\",\"properties\":{\"taste\":{\"type\":\"string\"}}}";
			tasteCmd.handler = [](const std::map<std::string, std::string>& args, std::string& result, std::string& /*error*/)
			{
				const std::string root = args.count("root") ? args.at("root") : "";
				std::string t = Get().GetTasteProfile(root);
				JsonValue r;
				r.type = JsonValue::Type::Object;
				r.Object.push_back({ "taste", JsonValue::MakeString(t) });
				result = r.Dump();
				return true;
			};
			registerBoth(std::move(tasteCmd), "assistant.taste.get");
		}
	}
}
