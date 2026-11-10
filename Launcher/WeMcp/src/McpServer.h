#pragma once

#include "McpProtocol.h"
#include "EngineBridge.h"
#include <iostream>
#include <string>
#include <memory>
#include <vector>
#include <set>

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

		// Tool Groups 动态分组接口
		const std::set<std::string>& GetActiveToolGroups() const { return m_ActiveToolGroups; }
		void ActivateToolGroup(const std::string& group);
		void DeactivateToolGroup(const std::string& group);
		void ResetToolGroups();
		static std::string ClassifyToolGroup(const std::string& cmdName);

		// 处理单条 JSON-RPC 请求并返回响应文本 (通知无需响应时返回空串)
		std::string HandleMessage(const std::string& jsonText, bool* outShouldNotifyListChanged = nullptr);

		// 运行 stdio 行协议交互事件循环
		void RunStdioLoop(std::istream& in = std::cin, std::ostream& out = std::cout);

	private:
		std::string HandleInitialize(const Wui::JsonValue& id, const Wui::JsonValue& params);
		std::string HandleToolsList(const Wui::JsonValue& id);
		std::string HandleToolsCall(const Wui::JsonValue& id, const Wui::JsonValue& params, bool* outShouldNotifyListChanged);

		// 批量执行引擎
		std::string HandleBatchExecute(const Wui::JsonValue& id, const Wui::JsonValue& params, bool* outShouldNotifyListChanged);
		bool DispatchSingleTool(const std::string& cmd, const std::map<std::string, std::string>& args,
			std::string& outResult, std::string& outError, bool* outShouldNotifyListChanged);

		// 工具组管理
		std::string HandleManageTools(const Wui::JsonValue& id, const Wui::JsonValue& params, bool* outShouldNotifyListChanged);

		// 原生 MCP Resources 资源化
		std::string HandleResourcesList(const Wui::JsonValue& id);
		std::string HandleResourcesRead(const Wui::JsonValue& id, const Wui::JsonValue& params);

		std::shared_ptr<EngineBridge> m_Bridge;
		std::string m_CurrentRole;
		std::string m_CurrentSessionToken;
		bool m_Initialized = false;

		std::set<std::string> m_ActiveToolGroups;

		// 远端宿主(Editor)的完整创作命令表:首次 tools/list 拉取并缓存,避免每次重连抖动。
		std::vector<Ai::CommandDescriptor> m_RemoteCommands;
		bool m_RemoteCommandsLoaded = false;
	};
}
