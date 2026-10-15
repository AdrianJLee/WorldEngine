#include "EngineBridge.h"
#include "World/Plugins/PluginManager.h"
#include "World/Core/WorldContext.h"
#include "World/WUI/WuiJson.h"
#include "World/Utils/Paths.h"

#include <filesystem>
#include <sstream>

#if defined(_WIN32)
#  include <winsock2.h>
#  include <ws2tcpip.h>
#  pragma comment(lib, "ws2_32.lib")
#endif

#ifndef WLD_REPO_ROOT
#  define WLD_REPO_ROOT "."
#endif
#ifndef WLD_OUTPUT_DIR
#  define WLD_OUTPUT_DIR ""
#endif
#ifndef WLD_BUILD_TYPE
#  define WLD_BUILD_TYPE "Debug"
#endif

namespace World::Mcp
{
	namespace fs = std::filesystem;

	namespace
	{
		void InitWinsock()
		{
			static bool s_Init = false;
			if (!s_Init)
			{
				WSADATA data;
				WSAStartup(MAKEWORD(2, 2), &data);
				s_Init = true;
			}
		}
	}

	EngineBridge::EngineBridge()
	{
		InitWinsock();
	}

	EngineBridge::~EngineBridge()
	{
		if (m_Socket != ~0ull)
		{
			closesocket(static_cast<SOCKET>(m_Socket));
			m_Socket = ~0ull;
		}

		if (m_PluginManager)
		{
			World::Agent::WeAgentCore::Get().ResetToMinimalPolicy();
		}
	}

	void EngineBridge::LoadHeadlessPlugins()
	{
		if (m_PluginManager)
			return;

		m_Context = std::make_unique<World::WorldContext>();
		m_PluginManager = std::make_unique<World::Plugins::PluginManager>();

		const fs::path engineRoot = fs::path(WLD_REPO_ROOT) / "plugins";
		const fs::path projectRoot = World::Paths::ProjectDir() / "plugins";
		const fs::path devEnginePluginBinRoot = fs::path(WLD_REPO_ROOT)
			/ WLD_OUTPUT_DIR / "bin" / WLD_BUILD_TYPE / "plugins" / WLD_BUILD_TYPE;

		std::string discoverErr;
		m_PluginManager->Discover(engineRoot, projectRoot, { devEnginePluginBinRoot });
		m_PluginManager->LoadAll(*m_Context);
	}

	bool EngineBridge::TryConnectSocket(uint16_t port)
	{
		SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
		if (s == INVALID_SOCKET)
			return false;

		sockaddr_in addr {};
		addr.sin_family = AF_INET;
		addr.sin_port = htons(port);
		inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);

		if (connect(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR)
		{
			closesocket(s);
			return false;
		}

		m_Socket = static_cast<uintptr_t>(s);
		m_RemotePort = port;
		m_ConnectedToRemote = true;
		return true;
	}

	bool EngineBridge::Initialize(uint16_t remotePort, const std::string& projectDir)
	{
		if (!projectDir.empty())
		{
			Paths::SetProjectDirOverride(std::filesystem::u8path(projectDir));
		}

		if (remotePort > 0)
		{
			if (TryConnectSocket(remotePort))
				return true;
		}
		else
		{
			// 探测默认编辑器端口 8089
			if (TryConnectSocket(8089))
				return true;
		}

		// 离线/内置进程通道生效: 自动加载 we-studio, we-gen, we-ai-assistant 插件
		m_ConnectedToRemote = false;
		LoadHeadlessPlugins();
		return true;
	}

	bool EngineBridge::SendSocketCommand(const std::string& cmd, const std::map<std::string, std::string>& args, std::string& outResult, std::string& outError)
	{
		if (!m_ConnectedToRemote || m_Socket == ~0ull)
			return false;

		using namespace World::Wui;
		JsonValue req;
		req.type = JsonValue::Type::Object;
		req.Object.push_back({ "cmd", JsonValue::MakeString(cmd) });
		for (const auto& kv : args)
		{
			req.Object.push_back({ kv.first, JsonValue::MakeString(kv.second) });
		}

		std::string reqStr = req.Dump() + "\n";
		SOCKET s = static_cast<SOCKET>(m_Socket);

		if (send(s, reqStr.data(), static_cast<int>(reqStr.size()), 0) == SOCKET_ERROR)
		{
			m_ConnectedToRemote = false;
			return false;
		}

		// 接收单行响应
		char buf[4096];
		int bytes = recv(s, buf, sizeof(buf) - 1, 0);
		if (bytes <= 0)
		{
			m_ConnectedToRemote = false;
			return false;
		}
		buf[bytes] = '\0';

		std::string respLine(buf);
		auto jsonOpt = JsonValue::Parse(respLine, nullptr);
		if (!jsonOpt)
		{
			outError = "Malformed JSON from remote editor: " + respLine;
			return false;
		}

		const auto* okVal = jsonOpt->Find("ok");
		bool ok = okVal ? okVal->AsBool() : false;

		if (ok)
		{
			const auto* resVal = jsonOpt->Find("result");
			outResult = resVal ? resVal->AsString() : "";
			return true;
		}
		else
		{
			const auto* errVal = jsonOpt->Find("error");
			outError = errVal ? errVal->AsString() : "Remote command failed";
			return false;
		}
	}

	bool EngineBridge::Dispatch(const std::string& cmd, const std::map<std::string, std::string>& args, std::string& outResult, std::string& outError)
	{
		if (m_ConnectedToRemote)
		{
			if (SendSocketCommand(cmd, args, outResult, outError))
				return true;
		}

		// 确保插件已加载
		if (!m_PluginManager)
		{
			LoadHeadlessPlugins();
		}

		// 统一走引擎命令注册表
		return Ai::CommandRegistry::Get().Dispatch(cmd, args, outResult, outError);
	}
}
