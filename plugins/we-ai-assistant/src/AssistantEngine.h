#pragma once

#include "LlmClient.h"
#include <map>
#include <string>
#include <vector>

namespace World::Ai
{
	class CommandRegistry;
}

namespace World::Assistant
{
	// §8 权限三档 (与协作模式正交)
	enum class PermissionMode : uint8_t
	{
		ReadOnlySuggest = 0,     // 只读建议：仅提供方案与代码预览，不执行写操作
		PreviewBeforeApply = 1,  // 预览后应用 (默认)：生成 Changeset 预览，待用户确认应用
		StageAutoApply = 2       // 阶段内自动：在当前阶段与角色允许集内自动执行并落盘
	};

	// §6.2 / §8 协作模式三档
	enum class CollaborationMode : uint8_t
	{
		Managed = 0,       // 托管 (默认)：制作人决定大部分事，只在关键点问出品人
		Collaborative = 1, // 协作：每阶段关键产物与门禁由出品人确认
		Expert = 2         // 专家：人类主导，AI 作为执行职能按指令工作
	};

	class AssistantEngine
	{
	public:
		// 智能问答与指令调度
		static std::string Chat(const std::string& message, const std::string& roleId);
		static void RegisterCommands(World::Ai::CommandRegistry& reg);

		// 权限档位与协作模式治理
		static void SetPermissionMode(PermissionMode mode);
		static PermissionMode GetPermissionMode();
		static const char* PermissionModeToString(PermissionMode mode);

		static void SetCollaborationMode(CollaborationMode mode);
		static CollaborationMode GetCollaborationMode();
		static const char* CollaborationModeToString(CollaborationMode mode);

		// 按角色隔离的会话上下文历史 (§8)
		static std::vector<LlmMessage> GetRoleHistory(const std::string& roleId);
		static void AppendRoleMessage(const std::string& roleId, const std::string& senderRole, const std::string& content);
		static void ClearRoleHistory(const std::string& roleId = "");
	};
}
