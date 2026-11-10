#pragma once

#include "WUI/Common/EditorPanel.h"
#include "World/Gameplay/Framework/BehaviorTreeAsset.h"
#include "World/Level/AI/EQS/EQSAsset.h"
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace World
{
	using BTNodeKind = Gameplay::BehaviorTreeNodeKind;
	using BTNodeDesc = Gameplay::BehaviorTreeNodeDesc;
	using BTBlackboardKeyDesc = Gameplay::BehaviorBlackboardKeyDesc;

	class BehaviorTreeEditorPanel final : public EditorPanel
	{
	public:
		enum class EditorMode
		{
			BehaviorTree,
			Blackboard,
			EQSQuery
		};

		BehaviorTreeEditorPanel();
		~BehaviorTreeEditorPanel() override = default;

		const char* Id() const override { return "behaviortree"; }
		const char* Title() const override
		{
			if (m_Mode == EditorMode::Blackboard)
				return m_Dirty ? "Blackboard Editor *" : "Blackboard Editor";
			if (m_Mode == EditorMode::EQSQuery)
				return m_Dirty ? "EQS Query Editor *" : "EQS Query Editor";
			return m_Dirty ? "Behavior Tree *" : "Behavior Tree";
		}

		void OnRender(Wui::WuiContext& ctx, const Wui::WuiRect& rect, PanelHost& host) override;
		bool OnShortcut(uint32_t keyCode, bool ctrl, bool shift, bool alt) override;

		static void RequestOpenPath(const std::string& logicalPath);

		void NewAsset();
		bool LoadAsset(const std::string& logicalPath);
		bool SaveAsset();
		bool ReloadAsset();
		bool ApplyToSelected(PanelHost& host);

		bool IsDirty() const noexcept { return m_Dirty; }
		const std::string& GetLogicalPath() const noexcept { return m_LogicalPath; }
		const Gameplay::BehaviorTreeAsset& GetAsset() const noexcept { return m_Asset; }

	private:
		enum class ViewTab
		{
			TreeOutline,
			Blackboard
		};

		enum class ScriptTemplateKind
		{
			Action,
			Condition
		};

		void SetDefaultTemplate();
		std::filesystem::path ResolvePhysicalPath(const std::string& logicalPath) const;
		void EnsureScriptFileExists(const std::string& logicalPath, ScriptTemplateKind kind);

		// 树结构操作辅助
		void AddChildNode(uint32_t parentId, BTNodeKind kind);
		void DeleteNode(uint32_t nodeId);
		void MoveNodeUp(uint32_t nodeId);
		void MoveNodeDown(uint32_t nodeId);
		std::vector<uint32_t> GetChildren(uint32_t parentId) const;
		BTNodeDesc* FindNode(uint32_t id);
		const BTNodeDesc* FindNode(uint32_t id) const;

		// 局部 UI 绘制
		void RenderToolbar(Wui::WuiContext& ctx, const Wui::WuiRect& rect, PanelHost& host);
		void RenderTreeOutline(Wui::WuiContext& ctx, const Wui::WuiRect& rect, PanelHost& host);
		void RenderNodeCard(Wui::WuiContext& ctx, uint32_t nodeId, int depth, float& curY, const Wui::WuiRect& rect, PanelHost& host);
		void RenderNodeDetails(Wui::WuiContext& ctx, const Wui::WuiRect& rect, PanelHost& host);
		void RenderBlackboard(Wui::WuiContext& ctx, const Wui::WuiRect& rect);
		void RenderEQSQueryEditor(Wui::WuiContext& ctx, const Wui::WuiRect& rect, PanelHost& host);
		void RenderLiveDebugger(Wui::WuiContext& ctx, const Wui::WuiRect& rect, PanelHost& host);

		static std::string s_PendingOpenPath;

		std::string m_LogicalPath;
		std::string m_AssetName;
		bool m_Dirty = false;

		EditorMode m_Mode = EditorMode::BehaviorTree;
		Gameplay::BehaviorTreeAsset m_Asset;
		Gameplay::BlackboardAsset m_BlackboardAsset;
		EQS::EQSAsset m_EQSAsset;
		std::string m_BoundBlackboardPath;

		std::vector<BTNodeDesc> m_Nodes;
		std::vector<BTBlackboardKeyDesc> m_BlackboardKeys;
		uint32_t m_NextNodeId = 1;
		uint32_t m_SelectedNodeId = 1;
		int m_SelectedRuleIdx = 0;

		ViewTab m_ActiveTab = ViewTab::TreeOutline;
		std::string m_StatusFeedback;

		static const char* NodeKindToString(BTNodeKind kind);
		static const char* NodeKindToBadge(BTNodeKind kind);
		static Wui::WuiColor NodeKindToColor(BTNodeKind kind);
	};
}
