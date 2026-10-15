#include "InterviewSystem.h"
#include "World/Ai/CommandRegistry.h"
#include "World/WUI/WuiJson.h"
#include "World/Utils/Paths.h"

#include <filesystem>
#include <fstream>
#include <sstream>

namespace World::Assistant
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

		JsonValue optArr; optArr.type = JsonValue::Type::Array;
		for (const auto& o : options)
		{
			JsonValue oObj; oObj.type = JsonValue::Type::Object;
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

		JsonValue qArr; qArr.type = JsonValue::Type::Array;
		for (const auto& q : questions)
		{
			auto opt = JsonValue::Parse(q.ToJson(), nullptr);
			if (opt) qArr.Array.push_back(std::move(*opt));
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

		// 1. 沉淀进 taste.yaml
		const fs::path tastePath = root / "taste.yaml";
		std::ofstream tf(tastePath, std::ios::app);
		if (tf)
		{
			tf << questionId << ": \"" << optionId << "\"\n";
		}

		// 2. 沉淀进 assumptions.md
		const fs::path assumptionsPath = root / "assumptions.md";
		std::ofstream af(assumptionsPath, std::ios::app);
		if (af)
		{
			const auto now = std::chrono::system_clock::now().time_since_epoch().count();
			af << "- **[Assumption " << now << "]** `" << questionId << "`: Selected `" << optionId << "`.\n";
		}

		return true;
	}

	std::string InterviewSystem::GetTasteProfile(const std::string& projectRoot) const
	{
		const fs::path root = fs::u8path(projectRoot.empty() ? Paths::ProjectDir().string() : projectRoot);
		const fs::path tastePath = root / "taste.yaml";

		if (fs::exists(tastePath))
		{
			std::ifstream f(tastePath);
			return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
		}
		return "taste_profile: default\n";
	}

	void InterviewSystem::RegisterCommands(World::Ai::CommandRegistry& reg)
	{
		using namespace World::Ai;

		// 1. assistant.interview.get
		CommandDescriptor getCmd;
		getCmd.name = "assistant.interview.get";
		getCmd.description = "Get L1 AI Producer 3-question interview card for current stage.";
		getCmd.effect = CommandEffect::Read;
		getCmd.scope = CommandScope::Both;
		getCmd.owner = "plugin:we-ai-assistant";
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
		reg.RegisterCommand(std::move(getCmd));

		// 2. assistant.interview.answer
		CommandDescriptor ansCmd;
		ansCmd.name = "assistant.interview.answer";
		ansCmd.description = "Answer interview question and log to assumptions.md and taste.yaml.";
		ansCmd.effect = CommandEffect::Write;
		ansCmd.scope = CommandScope::Both;
		ansCmd.owner = "plugin:we-ai-assistant";
		ansCmd.paramsSchema = "{\"type\":\"object\",\"properties\":{\"question\":{\"type\":\"string\"},\"option\":{\"type\":\"string\"},\"root\":{\"type\":\"string\"}},\"required\":[\"question\",\"option\"]}";
		ansCmd.resultSchema = "{\"type\":\"object\",\"properties\":{\"recorded\":{\"type\":\"boolean\"}}}";
		ansCmd.handler = [](const std::map<std::string, std::string>& args, std::string& result, std::string& error)
		{
			const std::string q = args.count("question") ? args.at("question") : "";
			const std::string opt = args.count("option") ? args.at("option") : "";
			if (q.empty() || opt.empty())
			{
				error = "Missing question or option";
				return false;
			}
			const std::string root = args.count("root") ? args.at("root") : "";
			const bool ok = Get().AnswerQuestion(root, q, opt);
			JsonValue r; r.type = JsonValue::Type::Object;
			r.Object.push_back({ "recorded", JsonValue::MakeBool(ok) });
			result = r.Dump();
			return ok;
		};
		reg.RegisterCommand(std::move(ansCmd));

		// 3. assistant.taste.get
		CommandDescriptor tasteCmd;
		tasteCmd.name = "assistant.taste.get";
		tasteCmd.description = "Get project taste profile preferences.";
		tasteCmd.effect = CommandEffect::Read;
		tasteCmd.scope = CommandScope::Both;
		tasteCmd.owner = "plugin:we-ai-assistant";
		tasteCmd.paramsSchema = "{\"type\":\"object\",\"properties\":{\"root\":{\"type\":\"string\"}}}";
		tasteCmd.resultSchema = "{\"type\":\"string\"}";
		tasteCmd.handler = [](const std::map<std::string, std::string>& args, std::string& result, std::string& /*error*/)
		{
			const std::string root = args.count("root") ? args.at("root") : "";
			result = Get().GetTasteProfile(root);
			return true;
		};
		reg.RegisterCommand(std::move(tasteCmd));
	}
}
