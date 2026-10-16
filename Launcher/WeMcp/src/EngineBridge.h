#pragma once

#include "World/Ai/CommandRegistry.h"
#include "World/Agent/WeAgentCore.h"
#include <string>
#include <map>
#include <memory>

namespace World
{
	class WorldContext;
}

namespace World::Plugins
{
	class PluginManager;
}

namespace World::Mcp
{
	class EngineBridge
	{
	public:
		EngineBridge();
		~EngineBridge();

		bool Initialize(uint16_t remotePort = 0, const std::string& projectDir = "", bool allowAutoLaunchHeadless = false);
		bool Dispatch(const std::string& cmd, const std::map<std::string, std::string>& args, std::string& outResult, std::string& outError);

		bool IsConnectedToRemote() const { return m_ConnectedToRemote; }
		uint16_t RemotePort() const { return m_RemotePort; }

		void SetReadOnlyFallback(bool readOnly) { m_ReadOnlyFallback = readOnly; }
		bool IsReadOnlyFallback() const { return m_ReadOnlyFallback; }

		bool TryLaunchHeadlessHost(uint16_t port, const std::string& projectDir = "");

	private:
		bool TryConnectSocket(uint16_t port);
		// 返回 true 表示网络传输正常完成 (业务成功与否写入 outCommandOk)
		bool SendSocketCommand(const std::string& cmd, const std::map<std::string, std::string>& args, bool& outCommandOk, std::string& outResult, std::string& outError);
		void LoadHeadlessPlugins();

		bool m_ConnectedToRemote = false;
		bool m_ReadOnlyFallback = false;
		uint16_t m_RemotePort = 0;
		uintptr_t m_Socket = ~0ull; // INVALID_SOCKET
		uintptr_t m_LaunchedProcessHandle = 0;

		std::unique_ptr<World::WorldContext> m_Context;
		std::unique_ptr<World::Plugins::PluginManager> m_PluginManager;
	};
}
