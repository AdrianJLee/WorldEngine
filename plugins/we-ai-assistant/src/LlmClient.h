#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
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
		std::string provider = "openai_compatible"; // "openai_compatible", "anthropic", "ollama", "mock"
		std::string endpoint = "https://api.openai.com/v1/chat/completions";
		std::string model = "gpt-4o";
		std::string apiKey; // 仅驻留内存，通过 DPAPI 密文存取
		float temperature = 0.7f;
		int timeoutSeconds = 15;
	};

	struct LlmToolCall
	{
		std::string id;
		std::string name;
		std::string argumentsJson;
	};

	struct LlmMessage
	{
		std::string role; // "system", "user", "assistant", "tool"
		std::string content;
	};

	struct LlmResult
	{
		bool success = false;
		std::string content;
		std::string error;
		int httpStatusCode = 0;
		std::vector<LlmToolCall> toolCalls;
	};

	using LlmStreamCallback = std::function<void(const std::string& deltaText)>;

	// §8 抽象大模型提供方接口 (支持流式、工具调用；OpenAI / Anthropic / 本地 Ollama / Mock)
	class ILlmProvider
	{
	public:
		virtual ~ILlmProvider() = default;
		virtual std::string GetProviderName() const = 0;
		virtual LlmResult Chat(const LlmConfig& config,
			const std::vector<LlmMessage>& messages,
			const std::string& toolsJsonSchema = "",
			LlmStreamCallback onStreamDelta = nullptr) = 0;
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

		// 同步聊天补全请求 (支持 OpenAI / Anthropic / DeepSeek / 本地 Ollama)
		LlmResult Chat(const std::vector<LlmMessage>& messages, const std::string& overrideModel = "");

		// 支持流式回调与工具 Schema 注入的扩展请求
		LlmResult ChatWithTools(const std::vector<LlmMessage>& messages,
			const std::string& toolsJsonSchema,
			LlmStreamCallback onStreamDelta = nullptr,
			const std::string& overrideModel = "");

		// 异步非阻塞请求 (供编辑器 WUI 面板调用，彻底杜绝 UI 主线程卡死)
		bool StartChatAsync(const std::vector<LlmMessage>& messages, const std::string& roleId = "producer");
		bool IsAsyncBusy() const { return m_AsyncBusy.load(); }
		bool PollAsyncResult(LlmResult& outResult, std::string& outRoleId);

		// 工具函数: 解析 URL
		static bool ParseUrl(const std::string& url, std::wstring& host, WORD& port, std::wstring& path, bool& isHttps);

	private:
		LlmConfig m_Config;
		mutable std::mutex m_AsyncMutex;
		std::atomic<bool> m_AsyncBusy{ false };
		bool m_AsyncHasResult = false;
		LlmResult m_AsyncResult;
		std::string m_AsyncRoleId;
	};
}
