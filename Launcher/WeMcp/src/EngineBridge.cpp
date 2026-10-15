#include "EngineBridge.h"
#include "World/WUI/WuiJson.h"
#include "World/Utils/Paths.h"

#include <winsock2.h>
#include <ws2tcpip.h>

#include <iostream>
#include <sstream>

namespace World::Mcp
{
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

		// 离线/内置进程通道生效 (无外部编辑器运行时走本地 CommandRegistry)
		m_ConnectedToRemote = false;
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

		std::string buffer;
		char chunk[4096];
		while (buffer.find('\n') == std::string::npos)
		{
			int received = recv(s, chunk, sizeof(chunk) - 1, 0);
			if (received <= 0)
			{
				m_ConnectedToRemote = false;
				return false;
			}
			chunk[received] = '\0';
			buffer.append(chunk, received);
		}

		std::string err;
		auto respOpt = JsonValue::Parse(buffer, &err);
		if (!respOpt || respOpt->type != JsonValue::Type::Object)
		{
			outError = "Failed to parse JSON response from remote editor: " + err;
			return false;
		}

		const auto* okVal = respOpt->Find("ok");
		const bool ok = okVal ? okVal->AsBool(false) : false;

		const auto* resVal = respOpt->Find("result");
		if (resVal) outResult = resVal->AsString();

		const auto* errVal = respOpt->Find("error");
		if (errVal) outError = errVal->AsString();

		return ok;
	}

	bool EngineBridge::Dispatch(const std::string& cmd, const std::map<std::string, std::string>& args, std::string& outResult, std::string& outError)
	{
		// 1. 若连接到远程 Editor 实例，优先转发
		if (m_ConnectedToRemote)
		{
			if (SendSocketCommand(cmd, args, outResult, outError))
				return true;
		}

		// 2. 本地引擎内核派发
		return Ai::CommandRegistry::Get().Dispatch(cmd, args, outResult, outError);
	}
}
