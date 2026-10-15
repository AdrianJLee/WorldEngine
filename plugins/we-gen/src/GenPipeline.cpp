#include "GenPipeline.h"
#include "World/Ai/CommandRegistry.h"
#include "World/Ai/FingerprintValidator.h"
#include "World/WUI/WuiJson.h"
#include "World/Utils/Paths.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace World::Gen
{
	namespace fs = std::filesystem;
	using namespace World::Wui;

	std::string GenResult::ToJson() const
	{
		JsonValue root;
		root.type = JsonValue::Type::Object;
		root.Object.push_back({ "success", JsonValue::MakeBool(success) });
		root.Object.push_back({ "output_path", JsonValue::MakeString(outputPath) });
		root.Object.push_back({ "sidecar_path", JsonValue::MakeString(provenanceSidecarPath) });
		root.Object.push_back({ "injected_prompt", JsonValue::MakeString(injectedPrompt) });
		root.Object.push_back({ "cost_incurred", JsonValue::MakeNumber(costIncurred) });
		root.Object.push_back({ "error", JsonValue::MakeString(error) });
		return root.Dump();
	}

	GenResult GenPipeline::Generate(const std::string& projectRoot, const GenRequest& request)
	{
		GenResult result;

		if (request.prompt.empty() || request.targetPath.empty())
		{
			result.success = false;
			result.error = "Prompt and targetPath cannot be empty.";
			return result;
		}

		// 1. 预算守卫校验
		std::string budgetReason;
		if (!BudgetGuard::Get().CanAfford(request.estimatedCost, budgetReason))
		{
			result.success = false;
			result.error = budgetReason;
			return result;
		}

		const fs::path root = fs::u8path(projectRoot.empty() ? Paths::ProjectDir().string() : projectRoot);
		const fs::path absOutput = root / request.targetPath;

		// 2. 风格锁注入
		const StyleLock lock = StyleLock::Load(root.string());
		result.injectedPrompt = lock.Apply(request.prompt);

		std::error_code ec;
		fs::create_directories(absOutput.parent_path(), ec);

		// 3. 生成产物写入 (Mock / 基础占位模式生成合法占位文件)
		std::ofstream out(absOutput, std::ios::binary);
		if (!out)
		{
			result.success = false;
			result.error = "Cannot open destination asset path: " + absOutput.string();
			return result;
		}
		out << "WORLDENGINE_GENERATED_ASSET:" << request.type << "\nPROMPT:" << result.injectedPrompt << "\n";
		out.close();

		result.outputPath = fs::relative(absOutput, root, ec).generic_u8string();

		// 4. 来源追溯与版权归属侧车文件 (.gen.yaml)
		const fs::path sidecarPath = fs::u8path(absOutput.string() + ".gen.yaml");
		std::ofstream sc(sidecarPath);
		if (sc)
		{
			const auto now = std::chrono::system_clock::now().time_since_epoch().count();
			sc << "version: 1.0.0\n";
			sc << "asset: \"" << result.outputPath << "\"\n";
			sc << "provider: \"" << request.provider << "\"\n";
			sc << "user_prompt: \"" << request.prompt << "\"\n";
			sc << "injected_prompt: \"" << result.injectedPrompt << "\"\n";
			sc << "negative_prompt: \"" << lock.negativePrompt << "\"\n";
			sc << "cost_usd: " << request.estimatedCost << "\n";
			sc << "timestamp: " << now << "\n";
			sc << "license: \"proprietary\"\n";
		}
		result.provenanceSidecarPath = fs::relative(sidecarPath, root, ec).generic_u8string();

		// 5. 记账
		BudgetGuard::Get().RecordCost(request.estimatedCost);
		result.costIncurred = request.estimatedCost;
		result.success = true;

		return result;
	}

	void GenPipeline::RegisterCommands(World::Ai::CommandRegistry& reg)
	{
		using namespace World::Ai;

		// 1. gen.generate
		CommandDescriptor genCmd;
		genCmd.name = "gen.generate";
		genCmd.description = "Generate multimodal game asset with style lock injection, budget guard, and provenance sidecar.";
		genCmd.effect = CommandEffect::Write;
		genCmd.scope = CommandScope::Both;
		genCmd.owner = "plugin:we-gen";
		genCmd.paramsSchema = "{\"type\":\"object\",\"properties\":{\"prompt\":{\"type\":\"string\"},\"target\":{\"type\":\"string\"},\"type\":{\"type\":\"string\"},\"cost\":{\"type\":\"number\"}},\"required\":[\"prompt\",\"target\"]}";
		genCmd.resultSchema = "{\"type\":\"object\",\"properties\":{\"success\":{\"type\":\"boolean\"},\"output_path\":{\"type\":\"string\"},\"sidecar_path\":{\"type\":\"string\"}}}";
		genCmd.handler = [](const std::map<std::string, std::string>& args, std::string& result, std::string& error)
		{
			GenRequest req;
			req.prompt = args.count("prompt") ? args.at("prompt") : "";
			req.targetPath = args.count("target") ? args.at("target") : "";
			if (req.prompt.empty() || req.targetPath.empty())
			{
				error = "Missing required prompt or target";
				return false;
			}
			req.type = args.count("type") ? args.at("type") : "2d_sprite";
			if (args.count("cost"))
			{
				try { req.estimatedCost = std::stod(args.at("cost")); } catch (...) {}
			}
			const std::string root = args.count("root") ? args.at("root") : "";
			auto r = Generate(root, req);
			result = r.ToJson();
			return r.success;
		};
		reg.RegisterCommand(std::move(genCmd));

		// 2. gen.style_lock.get
		CommandDescriptor getLockCmd;
		getLockCmd.name = "gen.style_lock.get";
		getLockCmd.description = "Get active style lock configuration.";
		getLockCmd.effect = CommandEffect::Read;
		getLockCmd.scope = CommandScope::Both;
		getLockCmd.owner = "plugin:we-gen";
		getLockCmd.paramsSchema = "{\"type\":\"object\",\"properties\":{\"root\":{\"type\":\"string\"}}}";
		getLockCmd.resultSchema = "{\"type\":\"object\"}";
		getLockCmd.handler = [](const std::map<std::string, std::string>& args, std::string& result, std::string& /*error*/)
		{
			const std::string root = args.count("root") ? args.at("root") : "";
			StyleLock lock = StyleLock::Load(root);
			result = lock.ToYaml();
			return true;
		};
		reg.RegisterCommand(std::move(getLockCmd));

		// 3. gen.budget.get
		CommandDescriptor getBudgetCmd;
		getBudgetCmd.name = "gen.budget.get";
		getBudgetCmd.description = "Get current monthly asset generation budget limit and expenditure.";
		getBudgetCmd.effect = CommandEffect::Read;
		getBudgetCmd.scope = CommandScope::Both;
		getBudgetCmd.owner = "plugin:we-gen";
		getBudgetCmd.paramsSchema = "{\"type\":\"object\"}";
		getBudgetCmd.resultSchema = "{\"type\":\"object\",\"properties\":{\"monthly_limit\":{\"type\":\"number\"},\"current_spent\":{\"type\":\"number\"}}}";
		getBudgetCmd.handler = [](const std::map<std::string, std::string>& /*args*/, std::string& result, std::string& /*error*/)
		{
			JsonValue r; r.type = JsonValue::Type::Object;
			r.Object.push_back({ "monthly_limit", JsonValue::MakeNumber(BudgetGuard::Get().GetMonthlyLimit()) });
			r.Object.push_back({ "current_spent", JsonValue::MakeNumber(BudgetGuard::Get().GetCurrentExpenditure()) });
			result = r.Dump();
			return true;
		};
		reg.RegisterCommand(std::move(getBudgetCmd));
	}
}
