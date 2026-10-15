#include "World/Ai/CommandRegistry.h"
#include "World/WUI/WuiJson.h"

// 引入 we-gen 插件源码进行单元测试
#include "../../plugins/we-gen/src/CredentialManager.h"
#include "../../plugins/we-gen/src/StyleLock.h"
#include "../../plugins/we-gen/src/BudgetGuard.h"
#include "../../plugins/we-gen/src/GenPipeline.h"

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
	using namespace World::Gen;
	using namespace World::Ai;
	using namespace World::Wui;

	std::printf("Running WorldAiGenTests...\n");

	// 1. 验证 Windows DPAPI 凭据安全加密与解密托管
	{
		auto& creds = CredentialManager::Get();
		const std::string rawKey = "sk-live-test-secure-api-key-998877";
		CHECK(creds.StoreCredential("test_service", rawKey));
		CHECK(creds.HasCredential("test_service"));

		// 验证磁盘上的密文绝对不含明文字符串
		const fs::path cipherPath = fs::current_path() / "local" / "credentials" / "test_service.bin";
		CHECK(fs::exists(cipherPath));
		{
			std::ifstream f(cipherPath, std::ios::binary);
			std::string cipherText((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
			CHECK(cipherText.find(rawKey) == std::string::npos); // 密文绝对不含明文
		}

		// 验证解密还原
		const std::string decrypted = creds.RetrieveCredential("test_service");
		CHECK(decrypted == rawKey);

		CHECK(creds.DeleteCredential("test_service"));
		CHECK(!creds.HasCredential("test_service"));
	}

	const fs::path tempProj = fs::current_path() / "build" / "tmp_test_gen";
	std::error_code ec;
	fs::remove_all(tempProj, ec);
	fs::create_directories(tempProj / "assets" / "textures", ec);

	// 2. 验证 StyleLock 风格锁解析与提示词注入
	{
		StyleLock lock;
		lock.promptTemplate = "{prompt}, 16-bit pixel art, fantasy roguelike";
		lock.negativePrompt = "photorealistic, 3d, noisy";
		CHECK(StyleLock::Save(tempProj.string(), lock));
		CHECK(fs::exists(tempProj / "art" / "style-lock.yaml"));

		StyleLock loaded = StyleLock::Load(tempProj.string());
		CHECK(loaded.promptTemplate == lock.promptTemplate);

		const std::string injected = loaded.Apply("Knight running animation");
		CHECK(injected == "Knight running animation, 16-bit pixel art, fantasy roguelike");
	}

	// 3. 验证 BudgetGuard 成本与额度超限拦截
	{
		auto& budget = BudgetGuard::Get();
		budget.ResetExpenditure();
		budget.SetMonthlyLimit(10.0);

		std::string reason;
		CHECK(budget.CanAfford(0.05, reason));

		// 设超低额度模拟超预算拦截
		budget.SetMonthlyLimit(0.01);
		CHECK(!budget.CanAfford(0.05, reason));
		CHECK(reason.find("Budget limit exceeded") != std::string::npos);

		budget.SetMonthlyLimit(50.0);
	}

	// 4. 验证 GenPipeline 资产生成与伴生侧车文件 (.gen.yaml)
	{
		GenRequest req;
		req.type = "2d_sprite";
		req.prompt = "Cyberpunk character sprite";
		req.targetPath = "assets/textures/cyber_char.png";
		req.estimatedCost = 0.05;

		auto res = GenPipeline::Generate(tempProj.string(), req);
		CHECK(res.success);
		CHECK(fs::exists(tempProj / req.targetPath));

		// 验证伴生来源审计文件 (.gen.yaml)
		const fs::path sidecar = tempProj / (req.targetPath + ".gen.yaml");
		CHECK(fs::exists(sidecar));

		std::ifstream sc(sidecar);
		std::string scText((std::istreambuf_iterator<char>(sc)), std::istreambuf_iterator<char>());
		CHECK(scText.find("provider: \"mock\"") != std::string::npos);
		CHECK(scText.find("injected_prompt:") != std::string::npos);
		CHECK(scText.find("license: \"proprietary\"") != std::string::npos);
	}

	// 5. 验证 CommandRegistry 集成
	{
		auto& reg = CommandRegistry::Get();
		GenPipeline::RegisterCommands(reg);

		CHECK(reg.HasCommand("gen.generate"));
		CHECK(reg.HasCommand("gen.style_lock.get"));
		CHECK(reg.HasCommand("gen.budget.get"));

		std::string result;
		std::string error;
		CHECK(reg.Dispatch("gen.budget.get", {}, result, error));
		auto json = JsonValue::Parse(result, nullptr);
		CHECK(json.has_value());
		CHECK(json->Find("monthly_limit") != nullptr);
	}

	fs::remove_all(tempProj, ec);

	std::printf("WorldAiGenTests PASSED.\n");
	return 0;
}
