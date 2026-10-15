#include "LlmClient.h"
#include "World/WUI/WuiJson.h"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <vector>

#if defined(_WIN32)
#  include <windows.h>
#  include <winhttp.h>
#  include <wincrypt.h>
#  pragma comment(lib, "winhttp.lib")
#  pragma comment(lib, "crypt32.lib")
#endif

namespace World::Assistant
{
	namespace fs = std::filesystem;
	using namespace World::Wui;

	namespace
	{
		fs::path GetCredentialsStorageDir()
		{
			const fs::path dir = fs::current_path() / "local" / "credentials";
			std::error_code ec;
			fs::create_directories(dir, ec);
			return dir;
		}

		std::wstring Utf8ToWide(const std::string& str)
		{
			if (str.empty()) return L"";
#if defined(_WIN32)
			int size = MultiByteToWideChar(CP_UTF8, 0, str.c_str(), -1, nullptr, 0);
			if (size <= 0) return L"";
			std::wstring res(size - 1, 0);
			MultiByteToWideChar(CP_UTF8, 0, str.c_str(), -1, &res[0], size);
			return res;
#else
			return std::wstring(str.begin(), str.end());
#endif
		}

		std::string WideToUtf8(const std::wstring& wstr)
		{
			if (wstr.empty()) return "";
#if defined(_WIN32)
			int size = WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), -1, nullptr, 0, nullptr, nullptr);
			if (size <= 0) return "";
			std::string res(size - 1, 0);
			WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), -1, &res[0], size, nullptr, nullptr);
			return res;
#else
			return std::string(wstr.begin(), wstr.end());
#endif
		}
	}

	LlmClient& LlmClient::Get()
	{
		static LlmClient instance;
		return instance;
	}

	void LlmClient::SetConfig(const LlmConfig& config)
	{
		m_Config = config;
	}

	LlmConfig& LlmClient::GetConfig()
	{
		return m_Config;
	}

	const LlmConfig& LlmClient::GetConfig() const
	{
		return m_Config;
	}

	bool LlmClient::SaveApiKeyToDpapi(const std::string& apiKey)
	{
		if (apiKey.empty())
			return false;

#if defined(_WIN32)
		DATA_BLOB dataIn;
		dataIn.pbData = reinterpret_cast<BYTE*>(const_cast<char*>(apiKey.data()));
		dataIn.cbData = static_cast<DWORD>(apiKey.size());

		DATA_BLOB dataOut;
		if (!CryptProtectData(&dataIn, L"WorldEngine LLM Key", nullptr, nullptr, nullptr, 0, &dataOut))
			return false;

		const fs::path target = GetCredentialsStorageDir() / "llm_key.bin";
		std::ofstream out(target, std::ios::binary);
		if (!out)
		{
			LocalFree(dataOut.pbData);
			return false;
		}

		out.write(reinterpret_cast<const char*>(dataOut.pbData), dataOut.cbData);
		out.close();

		LocalFree(dataOut.pbData);
		m_Config.apiKey = apiKey;
		return true;
#else
		return false;
#endif
	}

	std::string LlmClient::LoadApiKeyFromDpapi() const
	{
#if defined(_WIN32)
		const fs::path target = GetCredentialsStorageDir() / "llm_key.bin";
		if (!fs::exists(target))
			return "";

		std::ifstream in(target, std::ios::binary);
		if (!in)
			return "";

		std::vector<BYTE> cipherData((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
		in.close();

		DATA_BLOB dataIn;
		dataIn.pbData = cipherData.data();
		dataIn.cbData = static_cast<DWORD>(cipherData.size());

		DATA_BLOB dataOut;
		if (!CryptUnprotectData(&dataIn, nullptr, nullptr, nullptr, nullptr, 0, &dataOut))
			return "";

		std::string result(reinterpret_cast<const char*>(dataOut.pbData), dataOut.cbData);
		LocalFree(dataOut.pbData);
		return result;
#else
		return "";
#endif
	}

	bool LlmClient::HasDpapiApiKey() const
	{
		const fs::path target = GetCredentialsStorageDir() / "llm_key.bin";
		return fs::exists(target);
	}

	bool LlmClient::DeleteDpapiApiKey()
	{
		m_Config.apiKey.clear();
		const fs::path target = GetCredentialsStorageDir() / "llm_key.bin";
		std::error_code ec;
		return fs::remove(target, ec);
	}

	bool LlmClient::ParseUrl(const std::string& url, std::wstring& host, WORD& port, std::wstring& path, bool& isHttps)
	{
		isHttps = false;
		port = 80;
		std::string s = url;
		if (s.rfind("https://", 0) == 0)
		{
			isHttps = true;
			port = 443;
			s = s.substr(8);
		}
		else if (s.rfind("http://", 0) == 0)
		{
			isHttps = false;
			port = 80;
			s = s.substr(7);
		}

		size_t slash = s.find('/');
		std::string hostPort = (slash == std::string::npos) ? s : s.substr(0, slash);
		std::string pathPart = (slash == std::string::npos) ? "/" : s.substr(slash);

		size_t colon = hostPort.find(':');
		if (colon != std::string::npos)
		{
			try
			{
				port = static_cast<WORD>(std::stoi(hostPort.substr(colon + 1)));
			}
			catch (...) {}
			hostPort = hostPort.substr(0, colon);
		}

		host = Utf8ToWide(hostPort);
		path = Utf8ToWide(pathPart);
		return !host.empty();
	}

	LlmResult LlmClient::Chat(const std::vector<LlmMessage>& messages, const std::string& overrideModel)
	{
		LlmResult result;

		// 检查是否有可用 Key (内存、DPAPI 或环境变量)
		std::string activeKey = m_Config.apiKey;
		if (activeKey.empty())
		{
			activeKey = LoadApiKeyFromDpapi();
		}
		if (activeKey.empty())
		{
			const char* envKey = std::getenv("OPENAI_API_KEY");
			if (!envKey) envKey = std::getenv("DEEPSEEK_API_KEY");
			if (envKey) activeKey = envKey;
		}

		const std::string activeModel = overrideModel.empty() ? m_Config.model : overrideModel;

		// 构造符合 OpenAI / Ollama 协议的 JSON Request
		JsonValue reqRoot;
		reqRoot.type = JsonValue::Type::Object;
		reqRoot.Object.push_back({ "model", JsonValue::MakeString(activeModel) });
		reqRoot.Object.push_back({ "temperature", JsonValue::MakeNumber(static_cast<double>(m_Config.temperature)) });

		JsonValue msgArr;
		msgArr.type = JsonValue::Type::Array;
		for (const auto& m : messages)
		{
			JsonValue mObj;
			mObj.type = JsonValue::Type::Object;
			mObj.Object.push_back({ "role", JsonValue::MakeString(m.role) });
			mObj.Object.push_back({ "content", JsonValue::MakeString(m.content) });
			msgArr.Array.push_back(std::move(mObj));
		}
		reqRoot.Object.push_back({ "messages", std::move(msgArr) });

		const std::string jsonBody = reqRoot.Dump();

#if defined(_WIN32)
		std::wstring host;
		WORD port = 80;
		std::wstring path;
		bool isHttps = false;
		if (!ParseUrl(m_Config.endpoint, host, port, path, isHttps))
		{
			result.error = "Invalid endpoint URL: " + m_Config.endpoint;
			return result;
		}

		HINTERNET hSession = WinHttpOpen(L"WorldEngine-AI-Assistant/1.0",
			WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
		if (!hSession)
		{
			result.error = "Failed to initialize WinHTTP session";
			return result;
		}

		const DWORD timeoutMs = static_cast<DWORD>(m_Config.timeoutSeconds * 1000);
		WinHttpSetTimeouts(hSession, 4000, 4000, timeoutMs, timeoutMs);

		HINTERNET hConnect = WinHttpConnect(hSession, host.c_str(), port, 0);
		if (!hConnect)
		{
			WinHttpCloseHandle(hSession);
			result.error = "Failed to connect to host: " + WideToUtf8(host);
			return result;
		}

		DWORD openFlags = isHttps ? WINHTTP_FLAG_SECURE : 0;
		HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"POST", path.c_str(), nullptr,
			nullptr, WINHTTP_DEFAULT_ACCEPT_TYPES, openFlags);
		if (!hRequest)
		{
			WinHttpCloseHandle(hConnect);
			WinHttpCloseHandle(hSession);
			result.error = "Failed to open WinHTTP request";
			return result;
		}

		std::wstring headers = L"Content-Type: application/json\r\n";
		if (!activeKey.empty())
		{
			headers += L"Authorization: Bearer " + Utf8ToWide(activeKey) + L"\r\n";
		}

		BOOL sendOk = WinHttpSendRequest(hRequest, headers.c_str(), static_cast<DWORD>(headers.size()),
			const_cast<char*>(jsonBody.data()), static_cast<DWORD>(jsonBody.size()),
			static_cast<DWORD>(jsonBody.size()), 0);

		if (!sendOk || !WinHttpReceiveResponse(hRequest, nullptr))
		{
			DWORD err = GetLastError();
			WinHttpCloseHandle(hRequest);
			WinHttpCloseHandle(hConnect);
			WinHttpCloseHandle(hSession);
			result.error = "WinHTTP request failed (error " + std::to_string(err) + ", endpoint=" + m_Config.endpoint + ")";
			return result;
		}

		DWORD statusCode = 0;
		DWORD statusSize = sizeof(statusCode);
		WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
			WINHTTP_HEADER_NAME_BY_INDEX, &statusCode, &statusSize, WINHTTP_NO_HEADER_INDEX);
		result.httpStatusCode = static_cast<int>(statusCode);

		std::string responseBody;
		DWORD bytesAvailable = 0;
		while (WinHttpQueryDataAvailable(hRequest, &bytesAvailable) && bytesAvailable > 0)
		{
			std::vector<char> buffer(bytesAvailable);
			DWORD bytesRead = 0;
			if (WinHttpReadData(hRequest, buffer.data(), bytesAvailable, &bytesRead) && bytesRead > 0)
			{
				responseBody.append(buffer.data(), bytesRead);
			}
		}

		WinHttpCloseHandle(hRequest);
		WinHttpCloseHandle(hConnect);
		WinHttpCloseHandle(hSession);

		if (statusCode != 200)
		{
			result.error = "HTTP " + std::to_string(statusCode) + ": " + responseBody;
			return result;
		}

		// 解析返回结果: 兼容 OpenAI choices[0].message.content 和 Ollama message.content
		auto jsonOpt = JsonValue::Parse(responseBody, nullptr);
		if (!jsonOpt)
		{
			result.error = "Failed to parse JSON response: " + responseBody;
			return result;
		}

		// 1. 尝试 OpenAI 结构: choices[0].message.content
		if (const auto* choices = jsonOpt->Find("choices"))
		{
			if (choices->type == JsonValue::Type::Array && !choices->Array.empty())
			{
				if (const auto* msg = choices->Array[0].Find("message"))
				{
					if (const auto* content = msg->Find("content"))
					{
						result.content = content->AsString();
						result.success = true;
						return result;
					}
				}
			}
		}

		// 2. 尝试 Ollama 结构: message.content
		if (const auto* msg = jsonOpt->Find("message"))
		{
			if (const auto* content = msg->Find("content"))
			{
				result.content = content->AsString();
				result.success = true;
				return result;
			}
		}

		result.content = responseBody;
		result.success = true;
		return result;
#else
		result.error = "WinHTTP is only supported on Windows";
		return result;
#endif
	}
}
