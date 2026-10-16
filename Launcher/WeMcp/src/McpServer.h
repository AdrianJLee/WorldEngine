#pragma once

#include "McpProtocol.h"
#include "EngineBridge.h"
#include <iostream>
#include <string>
#include <memory>

namespace World::Mcp
{
	class McpServer
	{
	public:
		McpServer();
		~McpServer() = default;

		void SetEngineBridge(std::shared_ptr<EngineBridge> bridge) { m_Bridge = bridge; }
		std::shared_ptr<EngineBridge> GetEngineBridge() const { return m_Bridge; }

		void SetCurrentRole(const std::string& role);
		const std::string& GetCurrentRole() const { return m_CurrentRole; }

		void SetCurrentSessionToken(const std::string& token) { m_CurrentSessionToken = token; }
		const std::string& GetCurrentSessionToken() const { return m_CurrentSessionToken; }

		// 处理单条 JSON-RPC 请求并返回响应文本 (通知无需响应时返回空串)
		std::string HandleMessage(const std::string& jsonText, bool* outShouldNotifyListChanged = nullptr);

		// 运行 stdio 行协议交互事件循环
		void RunStdioLoop(std::istream& in = std::cin, std::ostream& out = std::cout);

	private:
		std::string HandleInitialize(const std::string& id, const Wui::JsonValue& params);
		std::string HandleToolsList(const std::string& id);
		std::string HandleToolsCall(const std::string& id, const Wui::JsonValue& params, bool* outShouldNotifyListChanged);

		std::shared_ptr<EngineBridge> m_Bridge;
		std::string m_CurrentRole;
		std::string m_CurrentSessionToken;
		bool m_Initialized = false;
	};
}
