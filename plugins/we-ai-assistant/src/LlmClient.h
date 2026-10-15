#pragma once

#include <cstdint>
#include <string>
#include <vector>

#if defined(_WIN32)
#  include <windows.h>
#else
using WORD = uint16_t;
#endif

namespace World::Assistant
{
	struct LlmConfig
	{
		std::string provider = "openai_compatible"; // "openai_compatible", "ollama", "mock"
		std::string endpoint = "https://api.openai.com/v1/chat/completions";
		std::string model = "gpt-4o";
		std::string apiKey; // 仅驻留内存，通过 DPAPI 密文存取
		float temperature = 0.7f;
		int timeoutSeconds = 15;
	};

	struct LlmMessage
	{
		std::string role; // "system", "user", "assistant"
		std::string content;
	};

	struct LlmResult
	{
		bool success = false;
		std::string content;
		std::string error;
		int httpStatusCode = 0;
	};

	class LlmClient
	{
	public:
		static LlmClient& Get();

		// 配置管理
		void SetConfig(const LlmConfig& config);
		LlmConfig& GetConfig();
		const LlmConfig& GetConfig() const;

		// DPAPI 凭据安全存取 (Windows CryptProtectData/CryptUnprotectData)
		bool SaveApiKeyToDpapi(const std::string& apiKey);
		std::string LoadApiKeyFromDpapi() const;
		bool HasDpapiApiKey() const;
		bool DeleteDpapiApiKey();

		// 执行聊天补全请求 (支持 OpenAI / DeepSeek / 本地 Ollama)
		LlmResult Chat(const std::vector<LlmMessage>& messages, const std::string& overrideModel = "");

		// 工具函数: 解析 URL
		static bool ParseUrl(const std::string& url, std::wstring& host, WORD& port, std::wstring& path, bool& isHttps);

	private:
		LlmConfig m_Config;
	};
}
