#include "World/Ai/SelfValidation.h"
#include "World/Ai/CommandRegistry.h"
#include "World/WUI/WuiJson.h"

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

#define CHECK(expr) \
	do { \
		if (!(expr)) { \
			std::fprintf(stderr, "[FAIL] %s:%d: %s\n", __FILE__, __LINE__, #expr); \
			std::exit(1); \
		} \
	} while (false)

namespace fs = std::filesystem;

int main()
{
	using namespace World::Ai;
	using namespace World::Wui;

	std::printf("Running WorldAiSelfValidationTests...\n");

	auto& registry = CommandRegistry::Get();

	// 1. 验证 M1 命令在注册表中均已注册且可被查找
	{
		CHECK(registry.HasCommand("project.validate"));
		CHECK(registry.HasCommand("asset.validate"));
		CHECK(registry.HasCommand("schema.list"));
		CHECK(registry.HasCommand("schema.describe"));
		CHECK(registry.HasCommand("api.luau_stubs"));
	}

	// 2. 验证 schema.list 与 schema.describe
	{
		std::string result;
		std::string error;
		CHECK(registry.Dispatch("schema.list", {}, result, error));
		CHECK(!result.empty());

		auto jsonList = JsonValue::Parse(result, nullptr);
		CHECK(jsonList.has_value());
		const auto* comps = jsonList->Find("components");
		CHECK(comps != nullptr && comps->type == JsonValue::Type::Array);

		bool foundTransform = false;
		for (const auto& item : comps->Array)
		{
			if (item.AsString() == "TransformComponent")
				foundTransform = true;
		}
		CHECK(foundTransform);

		// describe TransformComponent
		std::map<std::string, std::string> descArgs = { {"component", "TransformComponent"} };
		CHECK(registry.Dispatch("schema.describe", descArgs, result, error));
		auto jsonDesc = JsonValue::Parse(result, nullptr);
		CHECK(jsonDesc.has_value());
		const auto* fields = jsonDesc->Find("fields");
		CHECK(fields != nullptr && fields->type == JsonValue::Type::Array);
		CHECK(!fields->Array.empty());
	}

	// 3. 验证 api.luau_stubs
	{
		std::string result;
		std::string error;
		CHECK(registry.Dispatch("api.luau_stubs", {}, result, error));
		CHECK(!result.empty());
		CHECK(result.find("class") != std::string::npos || result.find("declare") != std::string::npos);
	}

	// 4. 验证 asset.validate 针对不存在文件报错
	{
		std::string result;
		std::string error;
		std::map<std::string, std::string> args = { {"path", "non_existent_asset_xyz.wmat"} };
		CHECK(registry.Dispatch("asset.validate", args, result, error));
		auto jsonReport = JsonValue::Parse(result, nullptr);
		CHECK(jsonReport.has_value());
		const auto* validVal = jsonReport->Find("valid");
		CHECK(validVal && !validVal->AsBool());
		const auto* errs = jsonReport->Find("errors");
		CHECK(errs && !errs->Array.empty());
		const auto* codeVal = errs->Array[0].Find("code");
		CHECK(codeVal && codeVal->AsString() == "ERR_ASSET_NOT_FOUND");
	}

	// 5. 验证 project.validate 构造坏项目逐类报错 (夹具测试)
	{
		const fs::path tempProj = fs::current_path() / "build" / "tmp_test_project_validate";
		std::error_code ec;
		fs::remove_all(tempProj, ec);
		fs::create_directories(tempProj / "assets" / "scripts", ec);

		// 5.1 尚无 project.we.yaml，断言报错 ERR_PROJECT_MANIFEST_MISSING
		auto r1 = SelfValidation::ValidateProject(tempProj.string());
		CHECK(!r1.valid);
		bool foundMissingManifest = false;
		for (const auto& err : r1.errors)
		{
			if (err.code == "ERR_PROJECT_MANIFEST_MISSING")
				foundMissingManifest = true;
		}
		CHECK(foundMissingManifest);

		// 5.2 补充合法 project.we.yaml
		{
			std::ofstream f(tempProj / "project.we.yaml");
			f << "name: TestProject\nversion: 1.0.0\n";
		}

		// 注入一个带语法错误的 Luau 脚本
		{
			std::ofstream s(tempProj / "assets" / "scripts" / "broken.luau");
			s << "function invalid_syntax(\n  -- missing closing parenthesis\nend\n";
		}

		auto r2 = SelfValidation::ValidateProject(tempProj.string());
		CHECK(!r2.valid);
		bool foundSyntaxError = false;
		for (const auto& err : r2.errors)
		{
			if (err.code == "ERR_LUAU_SYNTAX")
				foundSyntaxError = true;
		}
		CHECK(foundSyntaxError);

		// 5.3 修正脚本语法
		{
			std::ofstream s(tempProj / "assets" / "scripts" / "broken.luau");
			s << "function valid_syntax()\n  return 42\nend\n";
		}

		auto r3 = SelfValidation::ValidateProject(tempProj.string());
		CHECK(r3.valid);
		CHECK(r3.errors.empty());
		CHECK(r3.metrics["scripts_checked"] == "1");

		fs::remove_all(tempProj, ec);
	}

	std::printf("WorldAiSelfValidationTests PASSED.\n");
	return 0;
}
