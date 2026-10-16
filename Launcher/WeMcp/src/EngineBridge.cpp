#include "EngineBridge.h"
#include "World/Plugins/PluginManager.h"
#include "World/Core/WorldContext.h"
#include "World/WUI/WuiJson.h"
#include "World/Utils/Paths.h"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <sstream>
#include <thread>
#include <vector>

#if defined(_WIN32)
#  include <winsock2.h>
#  include <ws2tcpip.h>
#  include <windows.h>
#  pragma comment(lib, "ws2_32.lib")
#endif

#ifndef WLD_REPO_ROOT
#  define WLD_REPO_ROOT "."
#endif
#ifndef WLD_BINARY_ROOT
#  define WLD_BINARY_ROOT "build/x64-Debug"
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

#if defined(_WIN32)
		if (m_LaunchedProcessHandle != 0)
		{
			HANDLE hProc = reinterpret_cast<HANDLE>(m_LaunchedProcessHandle);
			TerminateProcess(hProc, 0);
			CloseHandle(hProc);
			m_LaunchedProcessHandle = 0;
		}
#endif

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

		const fs::path repoRoot = fs::exists(fs::path(WLD_REPO_ROOT) / "plugins")
			? fs::path(WLD_REPO_ROOT)
			: fs::current_path();
		const fs::path engineRoot = repoRoot / "plugins";
		const fs::path projectRoot = World::Paths::ProjectDir() / "plugins";

		std::vector<fs::path> binSearchRoots = {
			fs::path(WLD_BINARY_ROOT) / "bin" / WLD_BUILD_TYPE / "plugins" / WLD_BUILD_TYPE,
			fs::path(WLD_BINARY_ROOT) / "bin" / "Debug" / "plugins" / "Debug",
			fs::path(WLD_BINARY_ROOT) / "bin" / "Release" / "plugins" / "Release",
			repoRoot / "build" / "x64-Debug" / "bin" / "Debug" / "plugins" / "Debug",
			repoRoot / "build" / "x64-Debug" / "plugins"
		};

		m_PluginManager->Discover(engineRoot, projectRoot, binSearchRoots);
		m_PluginManager->LoadAll(*m_Context);
	}

	bool EngineBridge::TryConnectSocket(uint16_t port)
	{
		SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
		if (s == INVALID_SOCKET)
			return false;

		// 设置收发超时防止挂死
		DWORD timeoutMs = 5000;
		setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeoutMs), sizeof(timeoutMs));
		setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&timeoutMs), sizeof(timeoutMs));

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

	bool EngineBridge::TryLaunchHeadlessHost(uint16_t port, const std::string& projectDir)
	{
#if defined(_WIN32)
		const fs::path repoRoot = fs::exists(fs::path(WLD_REPO_ROOT) / "Engine")
			? fs::path(WLD_REPO_ROOT)
			: fs::current_path();

		std::vector<fs::path> hostCandidates = {
			fs::path(WLD_BINARY_ROOT) / "Runtime" / WLD_BUILD_TYPE / "Runtime.exe",
			fs::path(WLD_BINARY_ROOT) / "Runtime" / "Debug" / "Runtime.exe",
			repoRoot / "build" / "x64-Debug" / "Runtime" / "Debug" / "Runtime.exe",
			fs::path(WLD_BINARY_ROOT) / "Editor" / WLD_BUILD_TYPE / "Editor.exe",
			fs::path(WLD_BINARY_ROOT) / "Editor" / "Debug" / "Editor.exe",
			repoRoot / "build" / "x64-Debug" / "Editor" / "Debug" / "Editor.exe"
		};

		fs::path hostExe;
		for (const auto& cand : hostCandidates)
		{
			if (fs::exists(cand))
			{
				hostExe = cand;
				break;
			}
		}

		if (hostExe.empty())
			return false;

		std::string cmdLine = "\"" + hostExe.string() + "\" --headless --ai-control=" + std::to_string(port);
		if (!projectDir.empty())
		{
			cmdLine += " --project \"" + projectDir + "\"";
		}

		STARTUPINFOA si {};
		si.cb = sizeof(si);
		si.dwFlags = STARTF_USESHOWWINDOW;
		si.wShowWindow = SW_HIDE;
		PROCESS_INFORMATION pi {};

		std::vector<char> mutableCmd(cmdLine.begin(), cmdLine.end());
		mutableCmd.push_back('\0');

		if (!CreateProcessA(
			nullptr,
			mutableCmd.data(),
			nullptr,
			nullptr,
			FALSE,
			CREATE_NO_WINDOW,
			nullptr,
			repoRoot.string().c_str(),
			&si,
			&pi))
		{
			return false;
		}

		CloseHandle(pi.hThread);
		m_LaunchedProcessHandle = reinterpret_cast<uintptr_t>(pi.hProcess);

		// 轮询等待 Headless 宿主端口就绪
		for (int attempt = 0; attempt < 15; ++attempt)
		{
			std::this_thread::sleep_for(std::chrono::milliseconds(100));
			if (TryConnectSocket(port))
				return true;
		}
#else
		(void)port;
		(void)projectDir;
#endif
		return false;
	}

	bool EngineBridge::Initialize(uint16_t remotePort, const std::string& projectDir, bool allowAutoLaunchHeadless)
	{
		if (!projectDir.empty())
		{
			Paths::SetProjectDirOverride(std::filesystem::u8path(projectDir));
			World::Agent::WeAgentCore::Get().SetProjectRoot(projectDir);
		}

		// 始终加载本地插件以同步策略与命令元数据
		LoadHeadlessPlugins();

		const uint16_t targetPort = remotePort > 0 ? remotePort : 8089;
		if (TryConnectSocket(targetPort))
		{
			m_ReadOnlyFallback = false;
			return true;
		}

		const char* autoLaunchEnv = std::getenv("WLD_MCP_AUTO_LAUNCH");
		if (allowAutoLaunchHeadless || (autoLaunchEnv && std::string(autoLaunchEnv) == "1"))
		{
			if (TryLaunchHeadlessHost(targetPort, projectDir))
			{
				m_ReadOnlyFallback = false;
				return true;
			}
		}

		m_ConnectedToRemote = false;
		// 若本地 WeAgentCore 规则内核正常工作则可使用内嵌 Headless 模式；若策略为空则降级为只读模式 (§3.5)
		m_ReadOnlyFallback = (World::Agent::WeAgentCore::Get().GetPolicy() == nullptr);
		return true;
	}

	bool EngineBridge::SendSocketCommand(const std::string& cmd,
		const std::map<std::string, std::string>& args,
		bool& outCommandOk,
		std::string& outResult,
		std::string& outError)
	{
		outCommandOk = false;
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
			closesocket(s);
			m_Socket = ~0ull;
			m_ConnectedToRemote = false;
			return false;
		}

		// 按行循环读取完整 JSON 响应，彻底消除 4096 字节截断 Bug
		std::string respLine;
		char buf[4096];
		while (respLine.find('\n') == std::string::npos)
		{
			int bytes = recv(s, buf, sizeof(buf) - 1, 0);
			if (bytes <= 0)
			{
				if (respLine.empty())
				{
					closesocket(s);
					m_Socket = ~0ull;
					m_ConnectedToRemote = false;
					return false;
				}
				break;
			}
			buf[bytes] = '\0';
			respLine.append(buf, static_cast<size_t>(bytes));
			if (respLine.size() > 16 * 1024 * 1024) // 16MB 安全上限
				break;
		}

		const size_t newlinePos = respLine.find('\n');
		if (newlinePos != std::string::npos)
			respLine.resize(newlinePos);

		auto jsonOpt = JsonValue::Parse(respLine, nullptr);
		if (!jsonOpt)
		{
			outError = "Malformed JSON from remote host: " + respLine;
			outCommandOk = false;
			return true; // 传输完成但远端返回格式错误，不重跑本地
		}

		const auto* okVal = jsonOpt->Find("ok");
		outCommandOk = okVal ? okVal->AsBool() : false;

		if (outCommandOk)
		{
			const auto* resVal = jsonOpt->Find("result");
			if (resVal)
			{
				outResult = (resVal->type == JsonValue::Type::String) ? resVal->AsString() : resVal->Dump();
			}
			else
			{
				outResult = respLine;
			}
		}
		else
		{
			const auto* errVal = jsonOpt->Find("error");
			outError = errVal ? errVal->AsString() : "Remote command failed";
		}

		return true;
	}

	bool EngineBridge::Dispatch(const std::string& cmd, const std::map<std::string, std::string>& args, std::string& outResult, std::string& outError)
	{
		// §3.5 只读降级保护：当无受控宿主时，严禁放行任何 Write / Exec 命令
		if (m_ReadOnlyFallback)
		{
			const auto* desc = Ai::CommandRegistry::Get().GetDescriptor(cmd);
			if (!desc || desc->effect != Ai::CommandEffect::Read)
			{
				outError = "Forbidden (§3.5): Host rule engine unavailable; WeMcp is restricted to read-only mode.";
				return false;
			}
		}

		if (m_ConnectedToRemote)
		{
			bool remoteCmdOk = false;
			if (SendSocketCommand(cmd, args, remoteCmdOk, outResult, outError))
			{
				// 远端已明确响应 (无论成功或业务报错)，直接返回，绝不静默穿透到本地重跑
				return remoteCmdOk;
			}
		}

		if (!m_PluginManager)
		{
			LoadHeadlessPlugins();
		}

		return Ai::CommandRegistry::Get().Dispatch(cmd, args, outResult, outError);
	}
}
