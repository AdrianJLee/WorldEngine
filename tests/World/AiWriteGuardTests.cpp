#include "World/Ai/WriteGuard.h"
#include "World/Ai/ChangesetTracker.h"
#include "World/Ai/FingerprintValidator.h"
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

	std::printf("Running WorldAiWriteGuardTests...\n");

	// 1. 验证 SHA-256 计算契约
	{
		const std::string text = "hello";
		const std::string sha = FingerprintValidator::ComputeStringSha256(text);
		CHECK(sha == "2cf24dba5fb0a30e26e83b2ac5b9e29e1b161e5c1fa7425e73043362938b9824");
	}

	const fs::path tempProj = fs::current_path() / "build" / "tmp_test_writeguard";
	std::error_code ec;
	fs::remove_all(tempProj, ec);
	fs::create_directories(tempProj / "assets" / "scripts", ec);
	fs::create_directories(tempProj / "design", ec);

	// 2. 验证 WriteGuard 硬边界拦截
	{
		auto& guard = WriteGuard::Get();
		std::string reason;

		// 2.1 引擎源树绝对禁止写入
		CHECK(!guard.CanWrite("Engine/src/Core.cpp", tempProj.string(), nullptr, reason));
		CHECK(reason.find("Forbidden: Cannot write into protected engine source tree") != std::string::npos);

		CHECK(!guard.CanWrite("third_party/yaml/foo.cpp", tempProj.string(), nullptr, reason));
		CHECK(!guard.CanWrite("Editor/src/AiControl/bar.cpp", tempProj.string(), nullptr, reason));

		// 2.2 路径穿越逃逸项目根绝对禁止写入
		CHECK(!guard.CanWrite("../../outside.txt", tempProj.string(), nullptr, reason));

		// 2.3 角色权限 Glob 白名单拦截
		WritePermission designerPerm;
		designerPerm.roleId = "designer";
		designerPerm.allowedGlobs = { "design/**", "assets/data/**" };

		// 允许在 design/** 写入
		CHECK(guard.CanWrite("design/gdd.md", tempProj.string(), &designerPerm, reason));

		// 禁止越权写入程序代码
		CHECK(!guard.CanWrite("assets/scripts/player.luau", tempProj.string(), &designerPerm, reason));
		CHECK(reason.find("not in role's allowed globs") != std::string::npos);
	}

	// 3. 验证通道写入审计日志 (.we/ai/writes/<sessionId>.jsonl)
	{
		auto& guard = WriteGuard::Get();
		const fs::path testFile = tempProj / "design" / "audit_test.md";
		{
			std::ofstream f(testFile);
			f << "audit log test content\n";
		}

		CHECK(guard.RecordWrite(testFile.string(), tempProj.string(), "designer", "session_123"));

		const fs::path logFile = tempProj / ".we" / "ai" / "writes" / "session_123.jsonl";
		CHECK(fs::exists(logFile));

		std::ifstream log(logFile);
		std::string line;
		CHECK(std::getline(log, line));
		CHECK(line.find("\"role\":\"designer\"") != std::string::npos);
		CHECK(line.find("audit_test.md") != std::string::npos);
		CHECK(line.find("\"sha256\":") != std::string::npos);
	}

	// 4. 验证 ChangesetTracker 暂存、应用与逐字节无损撤销
	{
		auto& tracker = ChangesetTracker::Get();
		tracker.Clear();

		// 先建立一个已有文件
		const fs::path origFile = tempProj / "design" / "config.yaml";
		const std::string originalContent = "version: 1.0\nauthor: human\n";
		{
			std::ofstream f(origFile, std::ios::binary);
			f.write(originalContent.data(), originalContent.size());
		}

		// 开始变更集
		const std::string csId = tracker.BeginChangeset(tempProj.string(), "designer", "Tweak config");
		CHECK(!csId.empty());

		// 4.1 修改已有文件
		std::string err;
		const std::string modifiedContent = "version: 2.0\nauthor: ai\n";
		CHECK(tracker.StageFileWrite(csId, tempProj.string(), "design/config.yaml", modifiedContent, err));

		// 4.2 新增一个文件
		const std::string newContent = "# New Feature\n";
		CHECK(tracker.StageFileWrite(csId, tempProj.string(), "design/new_feature.md", newContent, err));

		// 验证当前磁盘内容已更新
		{
			std::ifstream f(origFile, std::ios::binary);
			std::string cur((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
			CHECK(cur == modifiedContent);
		}
		CHECK(fs::exists(tempProj / "design" / "new_feature.md"));

		// 4.3 执行原子撤销
		CHECK(tracker.RevertChangeset(csId, tempProj.string(), err));

		// 验证修改的文件逐字节恢复为原始内容
		{
			std::ifstream f(origFile, std::ios::binary);
			std::string restored((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
			CHECK(restored == originalContent);
		}

		// 验证新建的文件已被彻底删除
		CHECK(!fs::exists(tempProj / "design" / "new_feature.md"));
	}

	// 5. 验证 Changeset 命令注册与派发
	{
		auto& reg = CommandRegistry::Get();
		CHECK(reg.HasCommand("changeset.begin"));
		CHECK(reg.HasCommand("changeset.apply"));
		CHECK(reg.HasCommand("changeset.revert"));
		CHECK(reg.HasCommand("changeset.list"));

		std::string result;
		std::string error;
		std::map<std::string, std::string> args = {
			{"role", "artist"},
			{"desc", "Create sprite textures"},
			{"root", tempProj.string()}
		};
		CHECK(reg.Dispatch("changeset.begin", args, result, error));
		auto json = JsonValue::Parse(result, nullptr);
		CHECK(json.has_value());
		CHECK(json->Find("changeset_id") != nullptr);
	}

	fs::remove_all(tempProj, ec);

	std::printf("WorldAiWriteGuardTests PASSED.\n");
	return 0;
}
