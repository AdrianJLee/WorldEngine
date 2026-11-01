#include "wldpch.h"
#include "WUI/Panels/BehaviorTreeEditorPanel.h"
#include "World/WUI/WuiWidgets.h"
#include "World/WUI/Widgets/WuiChrome.h"
#include "World/WUI/WuiAccessibility.h"
#include "World/Utils/Paths.h"
#include "World/Scene/Components.h"
#include "World/Scene/Entity.h"
#include "World/Scene/Scene.h"
#include "WUI/Common/EditorAssetCatalog.h"
#include "WUI/Shell/EditorShell.h"
#include "WUI/Common/EQSPreviewState.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <sstream>

namespace World
{
	std::string BehaviorTreeEditorPanel::s_PendingOpenPath;

	BehaviorTreeEditorPanel::BehaviorTreeEditorPanel()
	{
		SetDefaultTemplate();
	}

	void BehaviorTreeEditorPanel::RequestOpenPath(const std::string& logicalPath)
	{
		s_PendingOpenPath = logicalPath;
	}

	const char* BehaviorTreeEditorPanel::NodeKindToString(BTNodeKind kind)
	{
		switch (kind)
		{
		case BTNodeKind::Selector: return "Selector (选择器)";
		case BTNodeKind::Sequence: return "Sequence (顺序器)";
		case BTNodeKind::Condition: return "Condition (条件谓词)";
		case BTNodeKind::Action: return "Action (动作节点)";
		case BTNodeKind::Decorator: return "Decorator (装饰器)";
		case BTNodeKind::Parallel: return "Parallel (并行器)";
		default: return "Unknown";
		}
	}

	const char* BehaviorTreeEditorPanel::NodeKindToBadge(BTNodeKind kind)
	{
		switch (kind)
		{
		case BTNodeKind::Selector: return "SEL";
		case BTNodeKind::Sequence: return "SEQ";
		case BTNodeKind::Condition: return "CND";
		case BTNodeKind::Action: return "ACT";
		case BTNodeKind::Decorator: return "DEC";
		case BTNodeKind::Parallel: return "PAR";
		default: return "???";
		}
	}

	Wui::WuiColor BehaviorTreeEditorPanel::NodeKindToColor(BTNodeKind kind)
	{
		switch (kind)
		{
		case BTNodeKind::Selector:
			return Wui::WuiColor { 0.15f, 0.30f, 0.65f, 0.95f }; // 深海蓝
		case BTNodeKind::Sequence:
			return Wui::WuiColor { 0.45f, 0.18f, 0.65f, 0.95f }; // 紫晶色
		case BTNodeKind::Condition:
			return Wui::WuiColor { 0.15f, 0.55f, 0.28f, 0.95f }; // 翡翠绿
		case BTNodeKind::Action:
			return Wui::WuiColor { 0.70f, 0.42f, 0.10f, 0.95f }; // 琥珀橙
		case BTNodeKind::Decorator:
			return Wui::WuiColor { 0.18f, 0.55f, 0.65f, 0.95f }; // 青碧色
		case BTNodeKind::Parallel:
			return Wui::WuiColor { 0.65f, 0.20f, 0.45f, 0.95f }; // 玫瑰紫
		default:
			return Wui::WuiColor { 0.4f, 0.4f, 0.4f, 1.0f };
		}
	}

	void BehaviorTreeEditorPanel::SetDefaultTemplate()
	{
		m_Nodes.clear();
		m_BlackboardKeys.clear();

		m_LogicalPath = "assets/ai/SmartObjectDemo.wbt";
		m_AssetName = "SmartObjectDemo";
		m_Dirty = false;

		// 默认黑板 Key
		m_BlackboardKeys.push_back({ "TargetPos", "Vector3", "[0.0, 0.0, 0.0]" });
		m_BlackboardKeys.push_back({ "TargetEntity", "Entity", "0" });
		m_BlackboardKeys.push_back({ "Health", "Float", "100.0" });

		// 默认节点拓扑: Root Selector -> Sequence(Heal) / Action(Patrol)
		m_Nodes.push_back({ 1, 0, BTNodeKind::Selector, "Root Selector", "顶层主决策选择器: 优先执行急救，平时巡逻", "", "", 0.0f, "" });
		m_Nodes.push_back({ 2, 1, BTNodeKind::Sequence, "Heal Sequence", "生命告急急救序列", "", "", 0.0f, "" });
		m_Nodes.push_back({ 3, 2, BTNodeKind::Condition, "Health Below 30%", "检查当前生命值是否低于 30%", "HealthBelow", "", 30.0f, "" });
		m_Nodes.push_back({ 4, 2, BTNodeKind::Action, "Seek Medical Station", "寻找并前往最近的医疗站智能对象", "", "SeekSmartObject", 0.0f, "MedicalStation" });
		m_Nodes.push_back({ 5, 2, BTNodeKind::Action, "Interact and Heal", "交互使用医疗站持续充能回血", "", "UseSmartObject", 0.0f, "" });
		m_Nodes.push_back({ 6, 1, BTNodeKind::Action, "Patrol Area", "日常区域巡逻漫步", "", "Patrol", 10.0f, "" });

		m_NextNodeId = 7;
		m_SelectedNodeId = 1;
		m_StatusFeedback = "已初始化默认行为树模板 (Default Template Loaded)";
	}

	std::filesystem::path BehaviorTreeEditorPanel::ResolvePhysicalPath(const std::string& logicalPath) const
	{
		std::string logical = logicalPath;
		std::replace(logical.begin(), logical.end(), '\\', '/');
		if (logical.rfind("assets/", 0) == 0)
			logical = logical.substr(7);

		std::filesystem::path p(logical);
		std::error_code ec;
		if (std::filesystem::exists(p, ec) && std::filesystem::is_regular_file(p, ec))
			return std::filesystem::absolute(p);

		const auto& assetRoot = Paths::AssetRoot();
		if (!assetRoot.empty())
		{
			std::filesystem::path fromAssetRoot = assetRoot / p;
			if (std::filesystem::exists(fromAssetRoot, ec) && std::filesystem::is_regular_file(fromAssetRoot, ec))
				return std::filesystem::absolute(fromAssetRoot);
		}

		const auto& projDir = Paths::ProjectDir();
		if (!projDir.empty())
		{
			std::filesystem::path fromProj = projDir / "assets" / p;
			if (std::filesystem::exists(fromProj, ec) && std::filesystem::is_regular_file(fromProj, ec))
				return std::filesystem::absolute(fromProj);
		}

		return std::filesystem::absolute(assetRoot / p);
	}

	void BehaviorTreeEditorPanel::EnsureScriptFileExists(const std::string& logicalPath, ScriptTemplateKind kind)
	{
		if (logicalPath.empty())
			return;

		std::filesystem::path fullPath = ResolvePhysicalPath(logicalPath);
		if (fullPath.empty())
		{
			const auto& projDir = Paths::ProjectDir();
			if (!projDir.empty())
				fullPath = projDir / logicalPath;
			else
				fullPath = logicalPath;
		}

		std::error_code ec;
		if (std::filesystem::exists(fullPath, ec))
			return;

		if (fullPath.has_parent_path())
		{
			std::filesystem::create_directories(fullPath.parent_path(), ec);
		}

		std::ofstream fout(fullPath);
		if (!fout.is_open())
			return;

		if (kind == ScriptTemplateKind::Action)
		{
			fout << "-- Custom AI Action Luau Script\n"
			     << "-- Behavior tree custom action template\n\n"
			     << "local Action = {}\n\n"
			     << "function Action.OnEnter(ctx)\n"
			     << "    -- Called when action starts\n"
			     << "end\n\n"
			     << "function Action.OnTick(ctx, dt)\n"
			     << "    -- Return true for Success, false for Running, nil for Failure\n"
			     << "    return true\n"
			     << "end\n\n"
			     << "function Action.OnExit(ctx)\n"
			     << "    -- Called when action finishes or interrupts\n"
			     << "end\n\n"
			     << "return Action\n";
		}
		else
		{
			fout << "-- Custom AI Condition Luau Script\n"
			     << "-- Behavior tree custom condition predicate template\n\n"
			     << "local Condition = {}\n\n"
			     << "function Condition.Evaluate(ctx)\n"
			     << "    -- Return true if condition passes, false otherwise\n"
			     << "    return true\n"
			     << "end\n\n"
			     << "return Condition\n";
		}
		fout.close();
	}

	void BehaviorTreeEditorPanel::NewAsset()
	{
		m_Nodes.clear();
		m_BlackboardKeys.clear();
		m_NextNodeId = 1;
		m_SelectedNodeId = 0;
		m_LogicalPath = "assets/ai/new_behavior_tree.wbt";
		m_AssetName = "new_behavior_tree";
		m_Dirty = true;
		m_StatusFeedback = "新建空白行为树资产 (New Empty Behavior Tree Created)";
	}

	bool BehaviorTreeEditorPanel::LoadAsset(const std::string& logicalPath)
	{
		std::string normalized = logicalPath;
		std::replace(normalized.begin(), normalized.end(), '\\', '/');
		std::filesystem::path physical = ResolvePhysicalPath(normalized);

		std::string error;
		if (!m_Asset.LoadFromFile(physical.string(), &error))
		{
			// 如果磁盘尚无该文件，载入默认模板并提示
			SetDefaultTemplate();
			m_LogicalPath = normalized;
			m_AssetName = physical.stem().string();
			m_Dirty = true;
			m_StatusFeedback = "资产未就绪或无法读取: " + normalized + " (已切换为默认模板)";
			return true;
		}

		m_Nodes = m_Asset.Nodes();
		m_BlackboardKeys = m_Asset.BlackboardKeys();
		m_LogicalPath = normalized;
		m_AssetName = physical.stem().string();
		m_Dirty = false;
		m_StatusFeedback = "已载入: " + m_LogicalPath;

		m_NextNodeId = 1;
		for (const auto& node : m_Nodes)
		{
			if (node.Id >= m_NextNodeId)
				m_NextNodeId = node.Id + 1;
		}
		m_SelectedNodeId = m_Nodes.empty() ? 1 : m_Nodes[0].Id;
		return true;
	}

	bool BehaviorTreeEditorPanel::SaveAsset()
	{
		std::filesystem::path physical = ResolvePhysicalPath(m_LogicalPath);
		std::error_code ec;
		if (physical.has_parent_path())
			std::filesystem::create_directories(physical.parent_path(), ec);

		m_Asset.Nodes() = m_Nodes;
		m_Asset.BlackboardKeys() = m_BlackboardKeys;
		std::string error;
		if (!m_Asset.SaveToFile(physical.string(), &error))
		{
			m_StatusFeedback = "保存失败: " + error;
			return false;
		}

		m_Dirty = false;
		m_StatusFeedback = "保存成功: " + m_LogicalPath;
		return true;
	}

	bool BehaviorTreeEditorPanel::ReloadAsset()
	{
		return LoadAsset(m_LogicalPath);
	}

	bool BehaviorTreeEditorPanel::ApplyToSelected(PanelHost& host)
	{
		Entity entity = host.GetSelectedEntity();
		if (!entity.IsValid())
		{
			m_StatusFeedback = "应用失败: 视口中未选中任何实体";
			return false;
		}

		if (!entity.HasComponent<BehaviorTreeComponent>())
		{
			entity.AddComponent<BehaviorTreeComponent>();
		}

		auto& bt = entity.GetComponent<BehaviorTreeComponent>();
		uint32_t assetHash = static_cast<uint32_t>(std::hash<std::string>{}(m_LogicalPath));
		bt.TreeAssetId = assetHash;
		bt.Enabled = 1;
		host.MarkDocumentDirty();

		m_StatusFeedback = "已将行为树 [" + m_LogicalPath + "] 绑定应用到实体 #" + std::to_string(static_cast<uint32_t>(static_cast<entt::entity>(entity)));
		return true;
	}

	std::vector<uint32_t> BehaviorTreeEditorPanel::GetChildren(uint32_t parentId) const
	{
		std::vector<uint32_t> children;
		for (const auto& node : m_Nodes)
		{
			if (node.ParentId == parentId)
				children.push_back(node.Id);
		}
		return children;
	}

	BTNodeDesc* BehaviorTreeEditorPanel::FindNode(uint32_t id)
	{
		for (auto& node : m_Nodes)
		{
			if (node.Id == id)
				return &node;
		}
		return nullptr;
	}

	const BTNodeDesc* BehaviorTreeEditorPanel::FindNode(uint32_t id) const
	{
		for (const auto& node : m_Nodes)
		{
			if (node.Id == id)
				return &node;
		}
		return nullptr;
	}

	void BehaviorTreeEditorPanel::AddChildNode(uint32_t parentId, BTNodeKind kind)
	{
		BTNodeDesc child;
		child.Id = m_NextNodeId++;
		child.ParentId = m_Nodes.empty() ? 0 : parentId;
		child.Kind = kind;

		switch (kind)
		{
		case BTNodeKind::Selector:
			child.Title = "Selector Node " + std::to_string(child.Id);
			break;
		case BTNodeKind::Sequence:
			child.Title = "Sequence Node " + std::to_string(child.Id);
			break;
		case BTNodeKind::Condition:
			child.Title = "Condition Node " + std::to_string(child.Id);
			child.ConditionType = "HasTarget";
			child.FloatParam = 0.0f;
			break;
		case BTNodeKind::Action:
			child.Title = "Action Node " + std::to_string(child.Id);
			child.ActionType = "Wait";
			child.FloatParam = 2.0f;
			break;
		}

		m_Nodes.push_back(child);
		m_SelectedNodeId = child.Id;
		m_Dirty = true;
		m_StatusFeedback = "已添加子节点 #" + std::to_string(child.Id);
	}

	void BehaviorTreeEditorPanel::DeleteNode(uint32_t nodeId)
	{
		if (nodeId == 1 && m_Nodes.size() > 1 && m_Nodes[0].Id == 1)
		{
			m_StatusFeedback = "无法删除根节点 (Root Node Protected)";
			return;
		}

		std::vector<uint32_t> toDelete;
		std::vector<uint32_t> queue = { nodeId };
		while (!queue.empty())
		{
			uint32_t current = queue.back();
			queue.pop_back();
			toDelete.push_back(current);

			for (const auto& n : m_Nodes)
			{
				if (n.ParentId == current)
					queue.push_back(n.Id);
			}
		}

		m_Nodes.erase(std::remove_if(m_Nodes.begin(), m_Nodes.end(),
			[&toDelete](const BTNodeDesc& n)
			{
				return std::find(toDelete.begin(), toDelete.end(), n.Id) != toDelete.end();
			}), m_Nodes.end());

		m_SelectedNodeId = m_Nodes.empty() ? 0 : m_Nodes[0].Id;
		m_Dirty = true;
		m_StatusFeedback = "已删除节点及其所有子节点 (共 " + std::to_string(toDelete.size()) + " 个)";
	}

	void BehaviorTreeEditorPanel::MoveNodeUp(uint32_t nodeId)
	{
		BTNodeDesc* cur = FindNode(nodeId);
		if (!cur || cur->ParentId == 0) return;

		int curIdx = -1;
		int prevSiblingIdx = -1;
		for (int i = 0; i < static_cast<int>(m_Nodes.size()); ++i)
		{
			if (m_Nodes[i].ParentId == cur->ParentId)
			{
				if (m_Nodes[i].Id == nodeId)
				{
					curIdx = i;
					break;
				}
				prevSiblingIdx = i;
			}
		}

		if (curIdx != -1 && prevSiblingIdx != -1)
		{
			std::swap(m_Nodes[curIdx], m_Nodes[prevSiblingIdx]);
			m_Dirty = true;
			m_StatusFeedback = "已上移节点优先级";
		}
	}

	void BehaviorTreeEditorPanel::MoveNodeDown(uint32_t nodeId)
	{
		BTNodeDesc* cur = FindNode(nodeId);
		if (!cur || cur->ParentId == 0) return;

		int curIdx = -1;
		int nextSiblingIdx = -1;
		for (int i = 0; i < static_cast<int>(m_Nodes.size()); ++i)
		{
			if (m_Nodes[i].ParentId == cur->ParentId)
			{
				if (m_Nodes[i].Id == nodeId)
				{
					curIdx = i;
				}
				else if (curIdx != -1)
				{
					nextSiblingIdx = i;
					break;
				}
			}
		}

		if (curIdx != -1 && nextSiblingIdx != -1)
		{
			std::swap(m_Nodes[curIdx], m_Nodes[nextSiblingIdx]);
			m_Dirty = true;
			m_StatusFeedback = "已下移节点优先级";
		}
	}

	bool BehaviorTreeEditorPanel::OnShortcut(uint32_t keyCode, bool ctrl, bool shift, bool alt)
	{
		(void)shift;
		(void)alt;
		if (ctrl && (keyCode == 'S' || keyCode == 's'))
		{
			SaveAsset();
			return true;
		}
		return false;
	}

	void BehaviorTreeEditorPanel::OnRender(Wui::WuiContext& ctx, const Wui::WuiRect& rect, PanelHost& host)
	{
		if (!s_PendingOpenPath.empty())
		{
			std::string path = s_PendingOpenPath;
			s_PendingOpenPath.clear();
			LoadAsset(path);
		}

		const Wui::WuiTheme& theme = host.Theme();
		Wui::PanelBackground(ctx, rect, theme.PanelBg, 0.0f);

		const float toolbarH = 38.0f;
		const float debuggerH = 150.0f;
		const float centerH = std::max(100.0f, rect.H - toolbarH - debuggerH);

		const Wui::WuiRect toolbarRect { rect.X, rect.Y, rect.W, toolbarH };
		const Wui::WuiRect centerRect { rect.X, rect.Y + toolbarH, rect.W, centerH };
		const Wui::WuiRect debuggerRect { rect.X, rect.Y + toolbarH + centerH, rect.W, debuggerH };

		RenderToolbar(ctx, toolbarRect, host);

		if (m_ActiveTab == ViewTab::TreeOutline)
		{
			const float leftW = std::floor(centerRect.W * 0.44f);
			const float rightW = centerRect.W - leftW;
			const Wui::WuiRect leftRect { centerRect.X, centerRect.Y, leftW, centerRect.H };
			const Wui::WuiRect rightRect { centerRect.X + leftW, centerRect.Y, rightW, centerRect.H };

			RenderTreeOutline(ctx, leftRect, host);
			RenderNodeDetails(ctx, rightRect, host);
		}
		else
		{
			RenderBlackboard(ctx, centerRect);
		}

		RenderLiveDebugger(ctx, debuggerRect, host);
	}

	void BehaviorTreeEditorPanel::RenderToolbar(Wui::WuiContext& ctx, const Wui::WuiRect& rect, PanelHost& host)
	{
		const Wui::WuiTheme& theme = host.Theme();
		Wui::PanelBackground(ctx, rect, theme.CardBg, 0.0f);
		Wui::HighlightOutline(ctx, rect, theme.Border, 0.0f, 1.0f);

		float curX = rect.X + 8.0f;
		const float curY = rect.Y + 6.0f;
		const float btnH = 26.0f;

		if (Wui::Button(ctx, Wui::HashId("bt.tb.new"), { curX, curY, 56.0f, btnH }, "新建", theme))
		{
			NewAsset();
		}
		curX += 62.0f;

		if (Wui::Button(ctx, Wui::HashId("bt.tb.open"), { curX, curY, 56.0f, btnH }, "打开", theme))
		{
			std::vector<std::string> bts = Editor::AssetCatalog::PathsFor(EditorAssetKind::BehaviorTree);
			if (!bts.empty())
			{
				LoadAsset(bts[0]);
			}
			else
			{
				m_StatusFeedback = "工程内未检测到其他 .wbt 资产，已打开默认模板";
			}
		}
		curX += 62.0f;

		Wui::WuiTheme saveTheme = theme;
		if (m_Dirty)
		{
			saveTheme.ButtonBg = Wui::WuiColor { 0.15f, 0.45f, 0.25f, 0.95f };
			saveTheme.Border = Wui::WuiColor { 0.25f, 0.85f, 0.45f, 1.0f };
		}
		if (Wui::Button(ctx, Wui::HashId("bt.tb.save"), { curX, curY, 56.0f, btnH }, "保存", saveTheme))
		{
			SaveAsset();
		}
		curX += 62.0f;

		if (Wui::Button(ctx, Wui::HashId("bt.tb.reload"), { curX, curY, 56.0f, btnH }, "重载", theme))
		{
			ReloadAsset();
		}
		curX += 62.0f;

		Wui::WuiTheme applyTheme = theme;
		applyTheme.ButtonBg = Wui::WuiColor { 0.20f, 0.35f, 0.60f, 0.95f };
		applyTheme.Border = Wui::WuiColor { 0.35f, 0.60f, 0.95f, 1.0f };
		if (Wui::Button(ctx, Wui::HashId("bt.tb.apply"), { curX, curY, 130.0f, btnH }, "应用到选中实体", applyTheme))
		{
			ApplyToSelected(host);
		}
		curX += 138.0f;

		const float tabW = 86.0f;
		Wui::WuiTheme tab1Theme = theme;
		if (m_ActiveTab == ViewTab::TreeOutline)
		{
			tab1Theme.ButtonBg = theme.Accent;
			tab1Theme.Border = theme.Accent;
		}
		if (Wui::Button(ctx, Wui::HashId("bt.tb.tab_tree"), { curX, curY, tabW, btnH }, "节点大纲", tab1Theme))
		{
			m_ActiveTab = ViewTab::TreeOutline;
		}
		curX += tabW + 4.0f;

		Wui::WuiTheme tab2Theme = theme;
		if (m_ActiveTab == ViewTab::Blackboard)
		{
			tab2Theme.ButtonBg = theme.Accent;
			tab2Theme.Border = theme.Accent;
		}
		if (Wui::Button(ctx, Wui::HashId("bt.tb.tab_bb"), { curX, curY, tabW, btnH }, "黑板变量", tab2Theme))
		{
			m_ActiveTab = ViewTab::Blackboard;
		}
		curX += tabW + 12.0f;

		std::string pathText = m_LogicalPath;
		if (m_Dirty) pathText += " * (未保存)";
		Wui::WuiColor pathColor = m_Dirty ? theme.Warning : theme.TextMuted;
		Wui::Label(ctx, { curX, curY + 5.0f }, pathText.c_str(), pathColor, theme.FontSizeSmall);

		if (!m_StatusFeedback.empty())
		{
			float msgW = 320.0f;
			float msgX = rect.X + rect.W - msgW - 8.0f;
			if (msgX > curX + 180.0f)
			{
				Wui::Label(ctx, { msgX, curY + 5.0f }, m_StatusFeedback.c_str(), theme.Accent, theme.FontSizeSmall);
			}
		}
	}

	void BehaviorTreeEditorPanel::RenderTreeOutline(Wui::WuiContext& ctx, const Wui::WuiRect& rect, PanelHost& host)
	{
		const Wui::WuiTheme& theme = host.Theme();
		Wui::PanelBackground(ctx, rect, theme.PanelBg, 0.0f);
		Wui::HighlightOutline(ctx, rect, theme.Border, 0.0f, 1.0f);

		const Wui::WuiRect headRect { rect.X, rect.Y, rect.W, 28.0f };
		Wui::PanelBackground(ctx, headRect, theme.CardBg, 0.0f);
		Wui::HighlightOutline(ctx, headRect, theme.Border, 0.0f, 1.0f);
		Wui::Label(ctx, { rect.X + 8.0f, rect.Y + 6.0f }, "树节点层级拓扑 (Tree Structure)", theme.Accent, theme.FontSizeSmall);

		std::string countStr = std::to_string(m_Nodes.size()) + " Nodes";
		Wui::Label(ctx, { rect.X + rect.W - 70.0f, rect.Y + 6.0f }, countStr.c_str(), theme.TextMuted, theme.FontSizeSmall);

		float toolY = rect.Y + 32.0f;
		float toolX = rect.X + 8.0f;
		const float toolBtnW = (rect.W - 24.0f) / 4.0f;

		Wui::WuiTheme selTheme = theme;
		selTheme.ButtonBg = NodeKindToColor(BTNodeKind::Selector);
		if (Wui::Button(ctx, Wui::HashId("bt.add_quick.sel"), { toolX, toolY, toolBtnW, 22.0f }, "+ 选择器", selTheme))
		{
			AddChildNode(m_SelectedNodeId != 0 ? m_SelectedNodeId : 1, BTNodeKind::Selector);
		}
		toolX += toolBtnW + 3.0f;

		Wui::WuiTheme seqTheme = theme;
		seqTheme.ButtonBg = NodeKindToColor(BTNodeKind::Sequence);
		if (Wui::Button(ctx, Wui::HashId("bt.add_quick.seq"), { toolX, toolY, toolBtnW, 22.0f }, "+ 顺序组", seqTheme))
		{
			AddChildNode(m_SelectedNodeId != 0 ? m_SelectedNodeId : 1, BTNodeKind::Sequence);
		}
		toolX += toolBtnW + 3.0f;

		Wui::WuiTheme cndTheme = theme;
		cndTheme.ButtonBg = NodeKindToColor(BTNodeKind::Condition);
		if (Wui::Button(ctx, Wui::HashId("bt.add_quick.cnd"), { toolX, toolY, toolBtnW, 22.0f }, "+ 条件谓词", cndTheme))
		{
			AddChildNode(m_SelectedNodeId != 0 ? m_SelectedNodeId : 1, BTNodeKind::Condition);
		}
		toolX += toolBtnW + 3.0f;

		Wui::WuiTheme actTheme = theme;
		actTheme.ButtonBg = NodeKindToColor(BTNodeKind::Action);
		if (Wui::Button(ctx, Wui::HashId("bt.add_quick.act"), { toolX, toolY, toolBtnW, 22.0f }, "+ 执行动作", actTheme))
		{
			AddChildNode(m_SelectedNodeId != 0 ? m_SelectedNodeId : 1, BTNodeKind::Action);
		}

		float listY = rect.Y + 60.0f;

		if (m_Nodes.empty())
		{
			float emptyY = rect.Y + 70.0f;
			Wui::Label(ctx, { rect.X + 16.0f, emptyY }, "当前行为树为空 (Empty Tree)", theme.TextMuted, theme.FontSizeSmall);
			emptyY += 26.0f;
			Wui::WuiTheme addRootTheme = theme;
			addRootTheme.ButtonBg = NodeKindToColor(BTNodeKind::Selector);
			if (Wui::Button(ctx, Wui::HashId("bt.add_root_sel"), { rect.X + 16.0f, emptyY, rect.W - 32.0f, 26.0f }, "+ 创建根选择器 (Root Selector)", addRootTheme))
			{
				AddChildNode(0, BTNodeKind::Selector);
			}
			emptyY += 32.0f;
			Wui::WuiTheme addRootSeqTheme = theme;
			addRootSeqTheme.ButtonBg = NodeKindToColor(BTNodeKind::Sequence);
			if (Wui::Button(ctx, Wui::HashId("bt.add_root_seq"), { rect.X + 16.0f, emptyY, rect.W - 32.0f, 26.0f }, "+ 创建根顺序器 (Root Sequence)", addRootSeqTheme))
			{
				AddChildNode(0, BTNodeKind::Sequence);
			}
			return;
		}

		std::vector<uint32_t> rootNodes = GetChildren(0);
		if (rootNodes.empty() && !m_Nodes.empty())
		{
			rootNodes.push_back(m_Nodes[0].Id);
		}

		for (uint32_t rootId : rootNodes)
		{
			RenderNodeCard(ctx, rootId, 0, listY, rect, host);
		}
	}

	void BehaviorTreeEditorPanel::RenderNodeCard(Wui::WuiContext& ctx, uint32_t nodeId, int depth, float& curY, const Wui::WuiRect& rect, PanelHost& host)
	{
		BTNodeDesc* node = FindNode(nodeId);
		if (!node) return;

		const Wui::WuiTheme& theme = host.Theme();
		const float indent = static_cast<float>(depth) * 22.0f;
		const float cardX = rect.X + 8.0f + indent;
		const float cardW = rect.W - 16.0f - indent;
		const float cardH = 34.0f;

		if (curY + cardH > rect.Y + rect.H)
			return;

		const Wui::WuiRect cardRect { cardX, curY, cardW, cardH };

		bool isNodeRunning = false;
		Entity selectedEnt = host.GetSelectedEntity();
		if (selectedEnt.IsValid() && selectedEnt.HasComponent<BehaviorTreeComponent>())
		{
			auto& bt = selectedEnt.GetComponent<BehaviorTreeComponent>();
			if (bt.Status == BehaviorStatus::Running && bt.CurrentNodeIndex == nodeId)
			{
				isNodeRunning = true;
			}
		}

		bool isSelected = (m_SelectedNodeId == nodeId);

		Wui::WuiColor bg = isSelected ? Wui::WuiColor { 0.22f, 0.28f, 0.38f, 0.95f } : theme.CardBg;
		Wui::PanelBackground(ctx, cardRect, bg, theme.Radius);

		if (isNodeRunning)
		{
			Wui::HighlightOutline(ctx, cardRect, Wui::WuiColor { 1.0f, 0.85f, 0.20f, 1.0f }, theme.Radius, 2.0f);
		}
		else if (isSelected)
		{
			Wui::HighlightOutline(ctx, cardRect, theme.Accent, theme.Radius, 1.5f);
		}
		else
		{
			Wui::HighlightOutline(ctx, cardRect, theme.Border, theme.Radius, 1.0f);
		}

		const auto& inputState = ctx.Input();
		if (inputState.MousePos.x >= cardRect.X && inputState.MousePos.x <= cardRect.X + cardRect.W &&
			inputState.MousePos.y >= cardRect.Y && inputState.MousePos.y <= cardRect.Y + cardRect.H)
		{
			if (inputState.MouseClicked[0])
			{
				m_SelectedNodeId = nodeId;
			}
		}

		const Wui::WuiRect badgeRect { cardX + 4.0f, curY + 6.0f, 38.0f, 22.0f };
		Wui::WuiColor badgeBg = NodeKindToColor(node->Kind);
		Wui::PanelBackground(ctx, badgeRect, badgeBg, theme.Radius);
		Wui::Label(ctx, { cardX + 11.0f, curY + 9.0f }, NodeKindToBadge(node->Kind), Wui::WuiColor { 1.0f, 1.0f, 1.0f, 1.0f }, theme.FontSizeSmall);

		float titleX = cardX + 48.0f;
		std::string titleStr = node->Title;
		if (isNodeRunning) titleStr += " [RUNNING]";
		Wui::WuiColor titleColor = isNodeRunning ? Wui::WuiColor { 1.0f, 0.85f, 0.20f, 1.0f } : (isSelected ? theme.Accent : theme.Text);
		Wui::Label(ctx, { titleX, curY + 5.0f }, titleStr.c_str(), titleColor, theme.FontSizeSmall);

		std::string subStr;
		if (node->Kind == BTNodeKind::Condition && !node->ConditionType.empty())
		{
			subStr = "Pred: " + node->ConditionType + " (" + std::to_string(static_cast<int>(node->FloatParam)) + ")";
		}
		else if (node->Kind == BTNodeKind::Action && !node->ActionType.empty())
		{
			subStr = "Act: " + node->ActionType;
			if (!node->StringParam.empty()) subStr += " [\"" + node->StringParam + "\"]";
		}
		if (!subStr.empty())
		{
			Wui::Label(ctx, { titleX, curY + 18.0f }, subStr.c_str(), theme.TextMuted, theme.FontSizeSmall);
		}

		float opBtnW = 20.0f;
		float opBtnX = cardX + cardW - opBtnW * 4.0f - 8.0f;

		Wui::WuiTheme opTheme = theme;
		opTheme.FontSizeSmall = 10;

		if (Wui::Button(ctx, Wui::HashId((std::string("bt.node.add.") + std::to_string(nodeId)).c_str()), { opBtnX, curY + 7.0f, opBtnW, 20.0f }, "+", opTheme))
		{
			AddChildNode(nodeId, BTNodeKind::Action);
		}
		opBtnX += opBtnW + 2.0f;

		if (Wui::Button(ctx, Wui::HashId((std::string("bt.node.up.") + std::to_string(nodeId)).c_str()), { opBtnX, curY + 7.0f, opBtnW, 20.0f }, "^", opTheme))
		{
			MoveNodeUp(nodeId);
		}
		opBtnX += opBtnW + 2.0f;

		if (Wui::Button(ctx, Wui::HashId((std::string("bt.node.dn.") + std::to_string(nodeId)).c_str()), { opBtnX, curY + 7.0f, opBtnW, 20.0f }, "v", opTheme))
		{
			MoveNodeDown(nodeId);
		}
		opBtnX += opBtnW + 2.0f;

		if (Wui::Button(ctx, Wui::HashId((std::string("bt.node.del.") + std::to_string(nodeId)).c_str()), { opBtnX, curY + 7.0f, opBtnW, 20.0f }, "x", opTheme))
		{
			DeleteNode(nodeId);
			return;
		}

		curY += cardH + 4.0f;

		std::vector<uint32_t> children = GetChildren(nodeId);
		for (uint32_t childId : children)
		{
			RenderNodeCard(ctx, childId, depth + 1, curY, rect, host);
		}
	}

	void BehaviorTreeEditorPanel::RenderNodeDetails(Wui::WuiContext& ctx, const Wui::WuiRect& rect, PanelHost& host)
	{
		const Wui::WuiTheme& theme = host.Theme();
		Wui::PanelBackground(ctx, rect, theme.CardBg, 0.0f);
		Wui::HighlightOutline(ctx, rect, theme.Border, 0.0f, 1.0f);

		const Wui::WuiRect headRect { rect.X, rect.Y, rect.W, 28.0f };
		Wui::PanelBackground(ctx, headRect, theme.PanelBg, 0.0f);
		Wui::HighlightOutline(ctx, headRect, theme.Border, 0.0f, 1.0f);
		Wui::Label(ctx, { rect.X + 8.0f, rect.Y + 6.0f }, "节点详细属性与参数卡片 (Node Details)", theme.Accent, theme.FontSizeSmall);

		if (m_Nodes.empty())
		{
			Wui::Label(ctx, { rect.X + 16.0f, rect.Y + 48.0f }, "当前行为树无节点。请点击左侧 [+ 创建根节点] 或上方快捷按钮开始编排。", theme.TextMuted, theme.FontSizeSmall);
			return;
		}

		BTNodeDesc* node = FindNode(m_SelectedNodeId);
		if (!node)
		{
			Wui::Label(ctx, { rect.X + 16.0f, rect.Y + 48.0f }, "请在左侧大纲树中选择一个节点进行编辑", theme.TextMuted, theme.FontSizeSmall);
			return;
		}

		float startX = rect.X + 16.0f;
		float curY = rect.Y + 38.0f;
		float fieldW = rect.W - 32.0f;
		float labelW = 120.0f;
		float inputW = fieldW - labelW;

		std::string idText = "节点 ID: #" + std::to_string(node->Id) + "  |  父节点 ID: #" + std::to_string(node->ParentId);
		Wui::Label(ctx, { startX, curY }, idText.c_str(), theme.Accent, theme.FontSizeSmall);
		curY += 24.0f;

		Wui::Label(ctx, { startX, curY + 2.0f }, "节点标题 (Title):", theme.Text, theme.FontSizeSmall);
		if (Wui::TextField(ctx, Wui::HashId((std::string("bt.det.title.") + std::to_string(node->Id)).c_str()), { startX + labelW, curY, inputW, 24.0f }, node->Title, theme))
		{
			m_Dirty = true;
		}
		curY += 28.0f;

		Wui::Label(ctx, { startX, curY + 2.0f }, "节点描述 (Desc):", theme.Text, theme.FontSizeSmall);
		if (Wui::TextField(ctx, Wui::HashId((std::string("bt.det.desc.") + std::to_string(node->Id)).c_str()), { startX + labelW, curY, inputW, 24.0f }, node->Description, theme))
		{
			m_Dirty = true;
		}
		curY += 32.0f;

		Wui::Label(ctx, { startX, curY + 2.0f }, "节点类型 (Kind):", theme.Text, theme.FontSizeSmall);
		float kindBtnW = (inputW - 12.0f) / 4.0f;
		float kindX = startX + labelW;

		auto renderKindBtn = [&](BTNodeKind k, const char* name, Wui::WuiId btnId)
		{
			Wui::WuiTheme kTheme = theme;
			if (node->Kind == k)
			{
				kTheme.ButtonBg = NodeKindToColor(k);
				kTheme.Border = Wui::WuiColor { 1.0f, 1.0f, 1.0f, 1.0f };
			}
			if (Wui::Button(ctx, btnId, { kindX, curY, kindBtnW, 24.0f }, name, kTheme))
			{
				node->Kind = k;
				if (k == BTNodeKind::Condition && node->ConditionType.empty())
				{
					node->ConditionType = "HasTarget";
				}
				else if (k == BTNodeKind::Action && node->ActionType.empty())
				{
					node->ActionType = "Wait";
					node->FloatParam = 2.0f;
				}
				m_Dirty = true;
			}
			kindX += kindBtnW + 4.0f;
		};

		renderKindBtn(BTNodeKind::Selector, "Selector", Wui::HashId((std::string("bt.det.k_sel.") + std::to_string(node->Id)).c_str()));
		renderKindBtn(BTNodeKind::Sequence, "Sequence", Wui::HashId((std::string("bt.det.k_seq.") + std::to_string(node->Id)).c_str()));
		renderKindBtn(BTNodeKind::Condition, "Condition", Wui::HashId((std::string("bt.det.k_cnd.") + std::to_string(node->Id)).c_str()));
		renderKindBtn(BTNodeKind::Action, "Action", Wui::HashId((std::string("bt.det.k_act.") + std::to_string(node->Id)).c_str()));
		curY += 34.0f;

		if (node->Kind == BTNodeKind::Condition)
		{
			static const std::vector<std::string> kConditionTypes = {
				"HasTarget",             // 目标有效性 (Target is Valid)
				"TargetInRange",         // 目标在距离内 (Target In Range)
				"HealthBelow",           // 生命值低于 (Health Below Threshold)
				"HealthAbove",           // 生命值高于 (Health Above Threshold)
				"TimerElapsed",          // 计时器超时 (Timer Elapsed)
				"BlackboardTagEquals",   // 黑板Tag匹配 (Blackboard Tag Equals)
				"BlackboardValueGreater",// 黑板数值大于 (Blackboard Value > Threshold)
				"BlackboardValueLess",   // 黑板数值小于 (Blackboard Value < Threshold)
				"ScriptCondition",       // Luau 自定义脚本条件 (Custom Script Predicate)
				"Custom"                 // 自定义名称 (Custom Predicate)
			};

			const bool isCustom = (node->ConditionType == "Custom");
			const float cardH = isCustom ? 145.0f : 125.0f;
			const Wui::WuiRect condCard { startX, curY, fieldW, cardH };
			Wui::PanelBackground(ctx, condCard, Wui::WuiColor { 0.12f, 0.25f, 0.18f, 0.85f }, theme.Radius);
			Wui::HighlightOutline(ctx, condCard, Wui::WuiColor { 0.25f, 0.70f, 0.35f, 1.0f }, theme.Radius, 1.0f);

			float cardY = curY + 8.0f;
			Wui::Label(ctx, { startX + 8.0f, cardY }, "[条件谓词参数设置] (Condition Predicate)", theme.Success, theme.FontSizeSmall);
			cardY += 24.0f;

			// 条件类型下拉选择
			int condIdx = -1;
			for (size_t i = 0; i < kConditionTypes.size(); ++i)
			{
				if (kConditionTypes[i] == node->ConditionType)
				{
					condIdx = static_cast<int>(i);
					break;
				}
			}
			if (condIdx < 0)
			{
				condIdx = static_cast<int>(kConditionTypes.size() - 1); // Custom
			}

			Wui::Label(ctx, { startX + 8.0f, cardY + 2.0f }, "条件类型 (Type):", theme.Text, theme.FontSizeSmall);
			if (Wui::Combo(ctx, Wui::HashId((std::string("bt.cnd.combo.") + std::to_string(node->Id)).c_str()),
				{ startX + labelW, cardY, inputW - 16.0f, 22.0f }, "条件类型", kConditionTypes, condIdx, theme))
			{
				if (condIdx >= 0 && condIdx < static_cast<int>(kConditionTypes.size()))
				{
					node->ConditionType = kConditionTypes[static_cast<size_t>(condIdx)];
					if (node->ConditionType == "ScriptCondition" && node->StringParam.empty())
					{
						node->StringParam = "assets/scripts/ai/conditions/SampleCustomCondition.luau";
					}
					m_Dirty = true;
				}
			}
			cardY += 28.0f;

			// 自适应参数配置
			if (node->ConditionType == "ScriptCondition")
			{
				Wui::Label(ctx, { startX + 8.0f, cardY + 2.0f }, "脚本选择 (Script):", theme.Text, theme.FontSizeSmall);
				const float btnW = 100.0f;
				const float scriptInputW = std::max(80.0f, inputW - 16.0f - btnW - 6.0f);

				std::vector<std::string> scriptOptions = Editor::AssetCatalog::PathsFor(EditorAssetKind::Script);
				if (scriptOptions.empty())
				{
					scriptOptions.push_back("assets/scripts/ai/conditions/SampleCustomCondition.luau");
				}
				int selectedScriptIdx = -1;
				for (size_t s = 0; s < scriptOptions.size(); ++s)
				{
					if (scriptOptions[s] == node->StringParam)
					{
						selectedScriptIdx = static_cast<int>(s);
						break;
					}
				}
				if (selectedScriptIdx < 0)
				{
					if (!node->StringParam.empty())
					{
						scriptOptions.insert(scriptOptions.begin(), node->StringParam);
						selectedScriptIdx = 0;
					}
					else
					{
						selectedScriptIdx = 0;
						node->StringParam = scriptOptions[0];
					}
				}

				int chosenScript = selectedScriptIdx;
				if (Wui::Combo(ctx, Wui::HashId((std::string("bt.cnd.script_combo.") + std::to_string(node->Id)).c_str()),
					{ startX + labelW, cardY, scriptInputW, 22.0f }, "脚本选择", scriptOptions, chosenScript, theme))
				{
					if (chosenScript >= 0 && chosenScript < static_cast<int>(scriptOptions.size()))
					{
						node->StringParam = scriptOptions[static_cast<size_t>(chosenScript)];
						m_Dirty = true;
					}
				}

				if (Wui::Button(ctx, Wui::HashId((std::string("bt.cnd.open_script.") + std::to_string(node->Id)).c_str()),
					{ startX + labelW + scriptInputW + 6.0f, cardY, btnW, 22.0f }, "[</>] 在编辑器中打开", theme))
				{
					EnsureScriptFileExists(node->StringParam, ScriptTemplateKind::Condition);
					host.OpenScriptEditor(node->StringParam);
					m_StatusFeedback = "已在脚本编辑器中打开: " + node->StringParam;
				}
				cardY += 26.0f;
				Wui::Label(ctx, { startX + labelW, cardY }, "提示: 在 Luau 脚本的 Evaluate(ctx) 中编写返回布尔的谓词逻辑", theme.TextMuted, theme.FontSizeSmall);
			}
			else if (node->ConditionType == "Custom")
			{
				Wui::Label(ctx, { startX + 8.0f, cardY + 2.0f }, "自定义条件名:", theme.Text, theme.FontSizeSmall);
				if (Wui::TextField(ctx, Wui::HashId((std::string("bt.cnd.custom_name.") + std::to_string(node->Id)).c_str()),
					{ startX + labelW, cardY, inputW - 16.0f, 22.0f }, node->ConditionType, theme))
				{
					m_Dirty = true;
				}
				cardY += 28.0f;

				Wui::Label(ctx, { startX + 8.0f, cardY + 2.0f }, "数值阈值 (Float):", theme.Text, theme.FontSizeSmall);
				if (Wui::DragFloat(ctx, Wui::HashId((std::string("bt.cnd.param.") + std::to_string(node->Id)).c_str()),
					{ startX + labelW, cardY, inputW - 16.0f, 22.0f }, node->FloatParam, 1.0f, -9999.0f, 9999.0f, theme))
				{
					m_Dirty = true;
				}
			}
			else if (node->ConditionType == "HasTarget")
			{
				Wui::Label(ctx, { startX + 8.0f, cardY + 2.0f }, "目标检查说明:", theme.Text, theme.FontSizeSmall);
				Wui::Label(ctx, { startX + labelW, cardY + 2.0f }, "当黑板 TargetEntity 非零或 TargetPos 坐标有效时判定通过", theme.TextMuted, theme.FontSizeSmall);
			}
			else
			{
				const char* paramLabel = "数值阈值 (Float):";
				const char* hintStr = "当数值满足比较关系时判定通过";
				float speed = 1.0f;
				float minVal = 0.0f;
				float maxVal = 9999.0f;

				if (node->ConditionType == "HealthBelow")
				{
					paramLabel = "生命阈值低于 (HP <):";
					hintStr = "当实体生命值或黑板数值低于该阈值时通过";
				}
				else if (node->ConditionType == "HealthAbove")
				{
					paramLabel = "生命阈值高于 (HP >):";
					hintStr = "当实体生命值或黑板数值高于该阈值时通过";
				}
				else if (node->ConditionType == "TargetInRange")
				{
					paramLabel = "目标距离阈值 (m):";
					hintStr = "当与目标的实际距离小于设定米数时通过";
					speed = 0.5f;
				}
				else if (node->ConditionType == "TimerElapsed")
				{
					paramLabel = "计时超时时长 (s):";
					hintStr = "当执行计时器已达此设定秒数时通过";
					speed = 0.1f;
					maxVal = 3600.0f;
				}
				else if (node->ConditionType == "BlackboardTagEquals")
				{
					paramLabel = "匹配标签 (Tag ID):";
					hintStr = "当黑板标签整数等于此设定值时通过";
				}
				else if (node->ConditionType == "BlackboardValueGreater")
				{
					paramLabel = "数值大于 (Value >):";
					hintStr = "当黑板 BlackboardValue 大于此设定值时通过";
					minVal = -9999.0f;
				}
				else if (node->ConditionType == "BlackboardValueLess")
				{
					paramLabel = "数值小于 (Value <):";
					hintStr = "当黑板 BlackboardValue 小于此设定值时通过";
					minVal = -9999.0f;
				}

				Wui::Label(ctx, { startX + 8.0f, cardY + 2.0f }, paramLabel, theme.Text, theme.FontSizeSmall);
				if (Wui::DragFloat(ctx, Wui::HashId((std::string("bt.cnd.param.") + std::to_string(node->Id)).c_str()),
					{ startX + labelW, cardY, inputW - 16.0f, 22.0f }, node->FloatParam, speed, minVal, maxVal, theme))
				{
					m_Dirty = true;
				}
				cardY += 26.0f;
				Wui::Label(ctx, { startX + labelW, cardY }, hintStr, theme.TextMuted, theme.FontSizeSmall);
			}

			curY += cardH + 12.0f;
		}
		else if (node->Kind == BTNodeKind::Action)
		{
			static const std::vector<std::string> kActionTypes = {
				"Wait",                     // 等待延时 (Wait Delay)
				"Patrol",                   // 区域巡逻 (Patrol Area)
				"MoveToTarget",             // 移动向目标点 (Move To Target Position)
				"SeekSmartObject",          // 寻找智能对象 (Seek Smart Object)
				"UseSmartObject",           // 交互使用智能对象 (Use Smart Object)
				"RunEQSQuery",              // 执行空间查询 (Run EQS Query)
				"FindTacticalCover",        // 寻找战术掩体 (Find Tactical Cover)
				"MoveToEQSPosition",        // 移动至战术点 (Move To EQS Position)
				"ExecuteEQSTacticalAction", // 一站式战术机动 (Execute EQS Tactical Action)
				"SetBlackboardValue",       // 写入黑板数值 (Set Blackboard Value)
				"IncrementBlackboardValue", // 累加黑板数值 (Increment Blackboard Value)
				"SetBlackboardTag",         // 写入黑板标签 (Set Blackboard Tag)
				"ResetTimer",               // 重置计时器 (Reset Timer)
				"ScriptAction",             // 执行自定义脚本动作 (Execute Luau Script Action)
				"Custom"                    // 自定义动作名 (Custom Named Action)
			};

			const bool isEqsAction = (node->ActionType == "RunEQSQuery" ||
			                          node->ActionType == "FindTacticalCover" ||
			                          node->ActionType == "MoveToEQSPosition" ||
			                          node->ActionType == "ExecuteEQSTacticalAction");
			const bool hasExtraRow = (node->ActionType == "SeekSmartObject" || node->ActionType == "Custom");
			const float cardH = isEqsAction ? 335.0f : (hasExtraRow ? 175.0f : 125.0f);
			const Wui::WuiRect actCard { startX, curY, fieldW, cardH };
			Wui::PanelBackground(ctx, actCard, Wui::WuiColor { 0.28f, 0.18f, 0.10f, 0.85f }, theme.Radius);
			Wui::HighlightOutline(ctx, actCard, Wui::WuiColor { 0.85f, 0.50f, 0.15f, 1.0f }, theme.Radius, 1.0f);

			float cardY = curY + 8.0f;
			if (isEqsAction)
			{
				Wui::Label(ctx, { startX + 8.0f, cardY }, "[EQS 空间决策工作台] (EQS Spatial Decision)", theme.Warning, theme.FontSizeSmall);
			}
			else
			{
				Wui::Label(ctx, { startX + 8.0f, cardY }, "[执行动作参数设置] (Action Execution)", theme.Warning, theme.FontSizeSmall);
			}
			cardY += 24.0f;

			// 动作类型下拉选择
			int actIdx = -1;
			for (size_t i = 0; i < kActionTypes.size(); ++i)
			{
				if (kActionTypes[i] == node->ActionType)
				{
					actIdx = static_cast<int>(i);
					break;
				}
			}
			if (actIdx < 0)
			{
				actIdx = static_cast<int>(kActionTypes.size() - 1); // Custom
			}

			Wui::Label(ctx, { startX + 8.0f, cardY + 2.0f }, "动作类型 (Type):", theme.Text, theme.FontSizeSmall);
			if (Wui::Combo(ctx, Wui::HashId((std::string("bt.act.combo.") + std::to_string(node->Id)).c_str()),
				{ startX + labelW, cardY, inputW - 16.0f, 22.0f }, "动作类型", kActionTypes, actIdx, theme))
			{
				if (actIdx >= 0 && actIdx < static_cast<int>(kActionTypes.size()))
				{
					node->ActionType = kActionTypes[static_cast<size_t>(actIdx)];
					if (node->ActionType == "ScriptAction" && node->StringParam.empty())
					{
						node->StringParam = "assets/scripts/ai/actions/SampleCustomAction.luau";
					}
					m_Dirty = true;
				}
			}
			cardY += 28.0f;

			// 自适应动作参数配置
			if (isEqsAction)
			{
				// ① 战术决策预设 (Tactical Preset)
				Wui::Label(ctx, { startX + 8.0f, cardY + 2.0f }, "战术预设 (Preset):", theme.Accent, theme.FontSizeSmall);
				int presetIdx = 0;
				if (node->StringParam == "Flank") presetIdx = 1;
				else if (node->StringParam == "Retreat") presetIdx = 2;
				else if (node->StringParam == "Sniper") presetIdx = 3;

				const std::vector<std::string> presetOptions = {
					"🛡 掩体 (Cover)", "⚔ 包抄 (Flank)", "🏃 撤退 (Retreat)", "🎯 狙击 (Sniper)"
				};
				if (Wui::Segmented(ctx, Wui::HashId((std::string("bt.act.eqs_preset.") + std::to_string(node->Id)).c_str()),
					{ startX + labelW, cardY, inputW - 16.0f, 22.0f }, presetOptions, presetIdx, theme))
				{
					const char* presetNames[] = { "Cover", "Flank", "Retreat", "Sniper" };
					node->StringParam = presetNames[presetIdx];
					LoadEQSPreset(static_cast<uint8_t>(presetIdx), nullptr, g_EQSPreviewState);
					m_Dirty = true;
				}
				cardY += 26.0f;

				// ② 空间搜索生成器与范围 (Spatial Generator)
				const std::vector<std::string> genOptions = {
					"贴地网格 (Projected Grid)",
					"同心圆环 (Concentric Circle)",
					"掩体边缘贴靠 (Around Obstacles)"
				};
				int genIdx = std::clamp(static_cast<int>(g_EQSPreviewState.GeneratorMode), 0, 2);
				Wui::Label(ctx, { startX + 8.0f, cardY + 2.0f }, "生成模式 (Gen):", theme.Text, theme.FontSizeSmall);
				if (Wui::Combo(ctx, Wui::HashId((std::string("bt.act.eqs_gen.") + std::to_string(node->Id)).c_str()),
					{ startX + labelW, cardY, inputW - 16.0f, 22.0f }, "", genOptions, genIdx, theme))
				{
					g_EQSPreviewState.GeneratorMode = static_cast<uint8_t>(genIdx);
					m_Dirty = true;
				}
				cardY += 26.0f;

				const char* paramLabel = (node->ActionType == "MoveToEQSPosition") ? "机动速度 (m/s):" : "搜索半径 (Radius m):";
				if (node->FloatParam <= 0.0f)
				{
					node->FloatParam = (node->ActionType == "MoveToEQSPosition") ? 3.5f : 20.0f;
				}
				Wui::Label(ctx, { startX + 8.0f, cardY + 2.0f }, paramLabel, theme.Text, theme.FontSizeSmall);
				const float maxRange = (node->ActionType == "MoveToEQSPosition") ? 50.0f : 100.0f;
				if (Wui::DragFloat(ctx, Wui::HashId((std::string("bt.act.eqs_radius.") + std::to_string(node->Id)).c_str()),
					{ startX + labelW, cardY, inputW - 16.0f, 22.0f }, node->FloatParam, 0.5f, 0.5f, maxRange, theme))
				{
					node->FloatParam = std::clamp(node->FloatParam, 0.5f, maxRange);
					if (node->ActionType != "MoveToEQSPosition")
					{
						g_EQSPreviewState.QueryRadius = node->FloatParam;
					}
					m_Dirty = true;
				}
				cardY += 26.0f;

				// ③ 战术权重微调 (Scoring Weights)
				const float halfW = (inputW - 16.0f - 8.0f) * 0.5f;

				Wui::Label(ctx, { startX + 8.0f, cardY + 2.0f }, "遮蔽/贴墙权重:", theme.TextMuted, theme.FontSizeSmall);
				if (Wui::DragFloat(ctx, Wui::HashId((std::string("bt.act.eqs_occ_wt.") + std::to_string(node->Id)).c_str()),
					{ startX + labelW, cardY, halfW, 22.0f }, g_EQSPreviewState.OcclusionWeight, 0.05f, 0.0f, 3.0f, theme))
				{
					g_EQSPreviewState.OcclusionWeight = std::clamp(g_EQSPreviewState.OcclusionWeight, 0.0f, 3.0f);
					m_Dirty = true;
				}
				Wui::Tooltip(ctx, { startX + labelW, cardY, halfW, 22.0f }, "视线遮蔽评分权重 (0.0~3.0)：值越高越偏好躲入敌方枪线死角");

				if (Wui::DragFloat(ctx, Wui::HashId((std::string("bt.act.eqs_wall_wt.") + std::to_string(node->Id)).c_str()),
					{ startX + labelW + halfW + 8.0f, cardY, halfW, 22.0f }, g_EQSPreviewState.WallProximityWeight, 0.05f, 0.0f, 3.0f, theme))
				{
					g_EQSPreviewState.WallProximityWeight = std::clamp(g_EQSPreviewState.WallProximityWeight, 0.0f, 3.0f);
					m_Dirty = true;
				}
				Wui::Tooltip(ctx, { startX + labelW + halfW + 8.0f, cardY, halfW, 22.0f }, "掩体贴墙度权重 (0.0~3.0)：值越高越偏好紧贴障碍物边缘");
				cardY += 26.0f;

				Wui::Label(ctx, { startX + 8.0f, cardY + 2.0f }, "理想距/制高权重:", theme.TextMuted, theme.FontSizeSmall);
				if (Wui::DragFloat(ctx, Wui::HashId((std::string("bt.act.eqs_dist_val.") + std::to_string(node->Id)).c_str()),
					{ startX + labelW, cardY, halfW, 22.0f }, g_EQSPreviewState.OptimalDistance, 0.5f, 1.0f, 50.0f, theme))
				{
					g_EQSPreviewState.OptimalDistance = std::clamp(g_EQSPreviewState.OptimalDistance, 1.0f, 50.0f);
					m_Dirty = true;
				}
				Wui::Tooltip(ctx, { startX + labelW, cardY, halfW, 22.0f }, "理想战术交战/掩蔽距离 (米)");

				if (Wui::DragFloat(ctx, Wui::HashId((std::string("bt.act.eqs_elev_wt.") + std::to_string(node->Id)).c_str()),
					{ startX + labelW + halfW + 8.0f, cardY, halfW, 22.0f }, g_EQSPreviewState.ElevationWeight, 0.05f, 0.0f, 2.0f, theme))
				{
					g_EQSPreviewState.ElevationWeight = std::clamp(g_EQSPreviewState.ElevationWeight, 0.0f, 2.0f);
					m_Dirty = true;
				}
				Wui::Tooltip(ctx, { startX + labelW + halfW + 8.0f, cardY, halfW, 22.0f }, "制高点高差权重 (0.0~2.0)：值越高越偏好高坡地势");
				cardY += 26.0f;

				// ④ 黑板目标槽位绑定 (Blackboard Output Binding)
				Entity selEnt = host.GetSelectedEntity();
				std::string threatDesc = "未锁定";
				if (selEnt.IsValid() && selEnt.HasComponent<BehaviorTreeComponent>())
				{
					const auto& bt = selEnt.GetComponent<BehaviorTreeComponent>();
					if (bt.TargetEntity != 0)
					{
						threatDesc = "威胁: #" + std::to_string(bt.TargetEntity);
					}
					else
					{
						threatDesc = "威胁: 自动最近敌人";
					}
				}
				char bbBindStr[160];
				std::snprintf(bbBindStr, sizeof(bbBindStr), "输出: TargetPos | 得分: BBValue | 标签: BBTag(88)  [%s]", threatDesc.c_str());
				Wui::Label(ctx, { startX + 8.0f, cardY + 2.0f }, "黑板目标槽位:", theme.Accent, theme.FontSizeSmall);
				Wui::Label(ctx, { startX + labelW, cardY + 2.0f }, bbBindStr, theme.TextMuted, theme.FontSizeSmall);
				cardY += 26.0f;

				// ⑤ 行为树内直接模拟测试与视口热力图联动
				if (Wui::Button(ctx, Wui::HashId((std::string("bt.act.eqs_sim.") + std::to_string(node->Id)).c_str()),
					{ startX + 8.0f, cardY, fieldW - 16.0f, 26.0f }, "✦ 模拟此节点空间推理 (Simulate Node EQS)", theme))
				{
					if (node->ActionType != "MoveToEQSPosition" && node->FloatParam > 0.0f)
					{
						g_EQSPreviewState.QueryRadius = node->FloatParam;
					}
					if (g_EQSPreviewState.PipelineTests.empty())
					{
						LoadEQSPreset(static_cast<uint8_t>(presetIdx), nullptr, g_EQSPreviewState);
					}
					for (auto& t : g_EQSPreviewState.PipelineTests)
					{
						if (t.Type == 1) t.Weight = g_EQSPreviewState.OcclusionWeight;
						else if (t.Type == 3) t.Weight = g_EQSPreviewState.WallProximityWeight;
						else if (t.Type == 0) t.IdealValue = g_EQSPreviewState.OptimalDistance;
						else if (t.Type == 4) t.Weight = g_EQSPreviewState.ElevationWeight;
					}

					Scene* scenePtr = host.GetActiveScene().get();
					Entity simEnt = host.GetSelectedEntity();
					EQSComponent* compPtr = (simEnt.IsValid() && simEnt.HasComponent<EQSComponent>())
						? &simEnt.GetComponent<EQSComponent>() : nullptr;

					ExecuteEQSPipeline(scenePtr, simEnt, compPtr, g_EQSPreviewState);

					if (simEnt.IsValid() && simEnt.HasComponent<BehaviorTreeComponent>())
					{
						auto& bt = simEnt.GetComponent<BehaviorTreeComponent>();
						bt.BlackboardTargetPos = g_EQSPreviewState.BestCoverPos;
						bt.BlackboardValue = g_EQSPreviewState.BestScore;
						bt.BlackboardTag = 88;
					}

					m_StatusFeedback = g_EQSPreviewState.LastStatus;
					host.MarkDocumentDirty();
				}
				cardY += 30.0f;

				// 结果反馈卡片
				const Wui::WuiRect resRect { startX + 8.0f, cardY, fieldW - 16.0f, 30.0f };
				Wui::PanelBackground(ctx, resRect, Wui::WuiColor{ 0.08f, 0.14f, 0.18f, 0.95f }, 3.0f);
				Wui::HighlightOutline(ctx, resRect, Wui::WuiColor{ 0.20f, 0.85f, 0.95f, 0.65f }, 3.0f, 1.0f);

				std::string resStr = g_EQSPreviewState.Evaluated
					? g_EQSPreviewState.LastStatus
					: "待模拟 (点击上方按钮在视口实时投射 3D 热力图与金标)";
				Wui::Label(ctx, { resRect.X + 6.0f, resRect.Y + 7.0f }, resStr.c_str(),
					g_EQSPreviewState.Evaluated ? theme.Success : theme.TextMuted, theme.FontSizeSmall);
				cardY += 36.0f;
			}
			else if (node->ActionType == "ScriptAction")
			{
				Wui::Label(ctx, { startX + 8.0f, cardY + 2.0f }, "脚本选择 (Script):", theme.Text, theme.FontSizeSmall);
				const float btnW = 100.0f;
				const float scriptInputW = std::max(80.0f, inputW - 16.0f - btnW - 6.0f);

				std::vector<std::string> scriptOptions = Editor::AssetCatalog::PathsFor(EditorAssetKind::Script);
				if (scriptOptions.empty())
				{
					scriptOptions.push_back("assets/scripts/ai/actions/SampleCustomAction.luau");
				}
				int selectedScriptIdx = -1;
				for (size_t s = 0; s < scriptOptions.size(); ++s)
				{
					if (scriptOptions[s] == node->StringParam)
					{
						selectedScriptIdx = static_cast<int>(s);
						break;
					}
				}
				if (selectedScriptIdx < 0)
				{
					if (!node->StringParam.empty())
					{
						scriptOptions.insert(scriptOptions.begin(), node->StringParam);
						selectedScriptIdx = 0;
					}
					else
					{
						selectedScriptIdx = 0;
						node->StringParam = scriptOptions[0];
					}
				}

				int chosenScript = selectedScriptIdx;
				if (Wui::Combo(ctx, Wui::HashId((std::string("bt.act.script_combo.") + std::to_string(node->Id)).c_str()),
					{ startX + labelW, cardY, scriptInputW, 22.0f }, "脚本选择", scriptOptions, chosenScript, theme))
				{
					if (chosenScript >= 0 && chosenScript < static_cast<int>(scriptOptions.size()))
					{
						node->StringParam = scriptOptions[static_cast<size_t>(chosenScript)];
						m_Dirty = true;
					}
				}

				if (Wui::Button(ctx, Wui::HashId((std::string("bt.act.open_script.") + std::to_string(node->Id)).c_str()),
					{ startX + labelW + scriptInputW + 6.0f, cardY, btnW, 22.0f }, "[</>] 编写脚本", theme))
				{
					EnsureScriptFileExists(node->StringParam, ScriptTemplateKind::Action);
					host.OpenScriptEditor(node->StringParam);
					m_StatusFeedback = "已在脚本编辑器中打开: " + node->StringParam;
				}
				cardY += 26.0f;
				Wui::Label(ctx, { startX + labelW, cardY }, "提示: 在 Luau 脚本的 OnEnter/OnTick(ctx,dt)/OnExit 中实现具体行为", theme.TextMuted, theme.FontSizeSmall);
			}
			else if (node->ActionType == "SeekSmartObject")
			{
				Wui::Label(ctx, { startX + 8.0f, cardY + 2.0f }, "搜索半径 (m):", theme.Text, theme.FontSizeSmall);
				if (Wui::DragFloat(ctx, Wui::HashId((std::string("bt.act.float.") + std::to_string(node->Id)).c_str()),
					{ startX + labelW, cardY, inputW - 16.0f, 22.0f }, node->FloatParam, 1.0f, 1.0f, 500.0f, theme))
				{
					m_Dirty = true;
				}
				cardY += 28.0f;

				Wui::Label(ctx, { startX + 8.0f, cardY + 2.0f }, "对象标签类别:", theme.Text, theme.FontSizeSmall);
				if (Wui::TextField(ctx, Wui::HashId((std::string("bt.act.str.") + std::to_string(node->Id)).c_str()),
					{ startX + labelW, cardY, inputW - 16.0f, 22.0f }, node->StringParam, theme))
				{
					m_Dirty = true;
				}
				cardY += 26.0f;
				Wui::Label(ctx, { startX + labelW, cardY }, "提示: 寻找匹配该标签的最近智能对象并写入黑板目标", theme.TextMuted, theme.FontSizeSmall);
			}
			else if (node->ActionType == "Custom")
			{
				Wui::Label(ctx, { startX + 8.0f, cardY + 2.0f }, "自定义动作名:", theme.Text, theme.FontSizeSmall);
				if (Wui::TextField(ctx, Wui::HashId((std::string("bt.act.custom_name.") + std::to_string(node->Id)).c_str()),
					{ startX + labelW, cardY, inputW - 16.0f, 22.0f }, node->ActionType, theme))
				{
					m_Dirty = true;
				}
				cardY += 28.0f;

				Wui::Label(ctx, { startX + 8.0f, cardY + 2.0f }, "浮点数值 (Float):", theme.Text, theme.FontSizeSmall);
				if (Wui::DragFloat(ctx, Wui::HashId((std::string("bt.act.float.") + std::to_string(node->Id)).c_str()),
					{ startX + labelW, cardY, inputW - 16.0f, 22.0f }, node->FloatParam, 0.5f, 0.0f, 3600.0f, theme))
				{
					m_Dirty = true;
				}
				cardY += 28.0f;

				Wui::Label(ctx, { startX + 8.0f, cardY + 2.0f }, "字符串 (String):", theme.Text, theme.FontSizeSmall);
				if (Wui::TextField(ctx, Wui::HashId((std::string("bt.act.str.") + std::to_string(node->Id)).c_str()),
					{ startX + labelW, cardY, inputW - 16.0f, 22.0f }, node->StringParam, theme))
				{
					m_Dirty = true;
				}
			}
			else
			{
				const char* floatLabel = "浮点数值 (Float):";
				const char* hintStr = "动作执行参数设置";
				float speed = 0.5f;
				float minVal = 0.0f;
				float maxVal = 3600.0f;

				if (node->ActionType == "Wait")
				{
					floatLabel = "等待时长 (秒/s):";
					hintStr = "持续暂停等待指定秒数，超时后返回成功";
				}
				else if (node->ActionType == "Patrol")
				{
					floatLabel = "巡逻速度 (m/s):";
					hintStr = "控制实体在巡逻区域内漫步移动的速度";
					minVal = 0.1f;
					maxVal = 100.0f;
				}
				else if (node->ActionType == "MoveToTarget")
				{
					floatLabel = "移动速度 (m/s):";
					hintStr = "向黑板 TargetPos 或 TargetEntity 持续导航移动";
					minVal = 0.1f;
					maxVal = 100.0f;
				}
				else if (node->ActionType == "UseSmartObject")
				{
					floatLabel = "交互时长 (秒/s):";
					hintStr = "对锁定的智能对象执行交互使用操作的时长";
				}
				else if (node->ActionType == "SetBlackboardValue")
				{
					floatLabel = "写入数值 (Float):";
					hintStr = "将设定浮点值覆盖写入当前实体的 BlackboardValue";
					speed = 1.0f;
					minVal = -9999.0f;
					maxVal = 9999.0f;
				}
				else if (node->ActionType == "IncrementBlackboardValue")
				{
					floatLabel = "累加数值 (Float):";
					hintStr = "将设定浮点值累加到当前实体的 BlackboardValue";
					speed = 1.0f;
					minVal = -9999.0f;
					maxVal = 9999.0f;
				}
				else if (node->ActionType == "SetBlackboardTag")
				{
					floatLabel = "写入标签 (Tag ID):";
					hintStr = "将设定整数标签写入当前实体的 BlackboardTag";
					speed = 1.0f;
					minVal = 0.0f;
					maxVal = 999.0f;
				}
				else if (node->ActionType == "ResetTimer")
				{
					floatLabel = "重置后秒数 (s):";
					hintStr = "将实体的 ExecutionTimer 重置为指定数值";
					speed = 0.1f;
				}

				Wui::Label(ctx, { startX + 8.0f, cardY + 2.0f }, floatLabel, theme.Text, theme.FontSizeSmall);
				if (Wui::DragFloat(ctx, Wui::HashId((std::string("bt.act.float.") + std::to_string(node->Id)).c_str()),
					{ startX + labelW, cardY, inputW - 16.0f, 22.0f }, node->FloatParam, speed, minVal, maxVal, theme))
				{
					m_Dirty = true;
				}
				cardY += 26.0f;
				Wui::Label(ctx, { startX + labelW, cardY }, hintStr, theme.TextMuted, theme.FontSizeSmall);
			}

			curY += cardH + 12.0f;
		}

		Wui::Label(ctx, { startX, curY + 2.0f }, "节点拓扑控制 (Topology Actions):", theme.TextMuted, theme.FontSizeSmall);
		curY += 24.0f;

		float actBtnW = (fieldW - 12.0f) / 3.0f;
		if (Wui::Button(ctx, Wui::HashId((std::string("bt.det.add_child.") + std::to_string(node->Id)).c_str()), { startX, curY, actBtnW, 26.0f }, "+ 添加子节点", theme))
		{
			AddChildNode(node->Id, BTNodeKind::Action);
		}
		if (Wui::Button(ctx, Wui::HashId((std::string("bt.det.up.") + std::to_string(node->Id)).c_str()), { startX + actBtnW + 6.0f, curY, actBtnW, 26.0f }, "上移优先级 (^)", theme))
		{
			MoveNodeUp(node->Id);
		}
		Wui::WuiTheme delTheme = theme;
		delTheme.ButtonBg = Wui::WuiColor { 0.45f, 0.15f, 0.15f, 0.95f };
		delTheme.Border = Wui::WuiColor { 0.85f, 0.25f, 0.25f, 1.0f };
		if (Wui::Button(ctx, Wui::HashId((std::string("bt.det.del.") + std::to_string(node->Id)).c_str()), { startX + (actBtnW + 6.0f) * 2.0f, curY, actBtnW, 26.0f }, "删除该节点 (x)", delTheme))
		{
			DeleteNode(node->Id);
		}
	}

void BehaviorTreeEditorPanel::RenderBlackboard(Wui::WuiContext& ctx, const Wui::WuiRect& rect)
	{
		const Wui::WuiTheme& theme = Wui::CurrentTheme();
		Wui::PanelBackground(ctx, rect, theme.PanelBg, 0.0f);
		Wui::HighlightOutline(ctx, rect, theme.Border, 0.0f, 1.0f);

		const Wui::WuiRect headRect { rect.X, rect.Y, rect.W, 28.0f };
		Wui::PanelBackground(ctx, headRect, theme.CardBg, 0.0f);
		Wui::HighlightOutline(ctx, headRect, theme.Border, 0.0f, 1.0f);
		Wui::Label(ctx, { rect.X + 8.0f, rect.Y + 6.0f }, "黑板数据键值定义 (Blackboard Keys Registry)", theme.Accent, theme.FontSizeSmall);

		static const std::vector<std::string> kBlackboardTemplates = {
			"TargetPos (Vector3)",
			"TargetEntity (Entity)",
			"Health (Float)",
			"MoveSpeed (Float)",
			"PatrolRadius (Float)",
			"Custom (自由新建)"
		};

		float curX = rect.X + 16.0f;
		float curY = rect.Y + 36.0f;

		if (Wui::Button(ctx, Wui::HashId("bt.bb.add"), { curX, curY, 100.0f, 24.0f }, "+ 添加变量", theme))
		{
			switch (m_SelectedBbTemplateIndex)
			{
			case 0:
				m_BlackboardKeys.push_back({ "TargetPos", "Vector3", "[0.0, 0.0, 0.0]" });
				break;
			case 1:
				m_BlackboardKeys.push_back({ "TargetEntity", "Entity", "0" });
				break;
			case 2:
				m_BlackboardKeys.push_back({ "Health", "Float", "100.0" });
				break;
			case 3:
				m_BlackboardKeys.push_back({ "MoveSpeed", "Float", "3.5" });
				break;
			case 4:
				m_BlackboardKeys.push_back({ "PatrolRadius", "Float", "15.0" });
				break;
			default:
				m_BlackboardKeys.push_back({ "NewKey_" + std::to_string(m_BlackboardKeys.size()), "Float", "0.0" });
				break;
			}
			m_Dirty = true;
		}
		curX += 108.0f;

		Wui::Label(ctx, { curX, curY + 4.0f }, "变量模板:", theme.TextMuted, theme.FontSizeSmall);
		curX += 60.0f;

		if (Wui::Combo(ctx, Wui::HashId("bt.bb.template_combo"), { curX, curY, 180.0f, 24.0f }, "常用模板", kBlackboardTemplates, m_SelectedBbTemplateIndex, theme))
		{
			// Template selection changed
		}
		curX += 188.0f;

		if (Wui::Button(ctx, Wui::HashId("bt.bb.quick_custom"), { curX, curY, 80.0f, 24.0f }, "+ 自由变量", theme))
		{
			m_BlackboardKeys.push_back({ "NewKey_" + std::to_string(m_BlackboardKeys.size()), "Float", "0.0" });
			m_Dirty = true;
		}

		curY += 34.0f;

		float tableW = rect.W - 32.0f;
		float colKeyW = tableW * 0.35f;
		float colTypeW = tableW * 0.25f;
		float colValW = tableW * 0.30f;
		float colDelW = tableW * 0.10f;

		const Wui::WuiRect thRect { rect.X + 16.0f, curY, tableW, 24.0f };
		Wui::PanelBackground(ctx, thRect, theme.CardBg, theme.Radius);
		Wui::HighlightOutline(ctx, thRect, theme.Border, theme.Radius, 1.0f);

		Wui::Label(ctx, { thRect.X + 8.0f, curY + 4.0f }, "变量名称 (KeyName)", theme.Accent, theme.FontSizeSmall);
		Wui::Label(ctx, { thRect.X + colKeyW + 8.0f, curY + 4.0f }, "类型 (TypeName)", theme.Accent, theme.FontSizeSmall);
		Wui::Label(ctx, { thRect.X + colKeyW + colTypeW + 8.0f, curY + 4.0f }, "默认初始值 (Default)", theme.Accent, theme.FontSizeSmall);
		Wui::Label(ctx, { thRect.X + colKeyW + colTypeW + colValW + 8.0f, curY + 4.0f }, "操作", theme.Accent, theme.FontSizeSmall);

		curY += 28.0f;

		static const std::vector<std::string> kBlackboardTypes = {
			"Float",
			"Vector3",
			"Entity",
			"Bool",
			"String",
			"Int"
		};

		int toDeleteIdx = -1;
		for (int i = 0; i < static_cast<int>(m_BlackboardKeys.size()); ++i)
		{
			auto& bb = m_BlackboardKeys[i];
			const Wui::WuiRect rowRect { rect.X + 16.0f, curY, tableW, 26.0f };
			Wui::PanelBackground(ctx, rowRect, (i % 2 == 0) ? theme.PanelBg : theme.CardBg, 0.0f);

			if (Wui::TextField(ctx, Wui::HashId((std::string("bt.bb.k.") + std::to_string(i)).c_str()), { rowRect.X + 4.0f, curY + 2.0f, colKeyW - 8.0f, 22.0f }, bb.KeyName, theme))
			{
				m_Dirty = true;
			}

			int typeIdx = -1;
			for (size_t t = 0; t < kBlackboardTypes.size(); ++t)
			{
				if (kBlackboardTypes[t] == bb.TypeName)
				{
					typeIdx = static_cast<int>(t);
					break;
				}
			}
			if (typeIdx < 0) typeIdx = 0;

			if (Wui::Combo(ctx, Wui::HashId((std::string("bt.bb.t.") + std::to_string(i)).c_str()),
				{ rowRect.X + colKeyW + 4.0f, curY + 2.0f, colTypeW - 8.0f, 22.0f }, "类型", kBlackboardTypes, typeIdx, theme))
			{
				if (typeIdx >= 0 && typeIdx < static_cast<int>(kBlackboardTypes.size()))
				{
					bb.TypeName = kBlackboardTypes[static_cast<size_t>(typeIdx)];
					m_Dirty = true;
				}
			}

			if (Wui::TextField(ctx, Wui::HashId((std::string("bt.bb.v.") + std::to_string(i)).c_str()), { rowRect.X + colKeyW + colTypeW + 4.0f, curY + 2.0f, colValW - 8.0f, 22.0f }, bb.DefaultValue, theme))
			{
				m_Dirty = true;
			}

			Wui::WuiTheme delTheme = theme;
			delTheme.ButtonBg = Wui::WuiColor { 0.45f, 0.15f, 0.15f, 0.95f };
			if (Wui::Button(ctx, Wui::HashId((std::string("bt.bb.d.") + std::to_string(i)).c_str()), { rowRect.X + colKeyW + colTypeW + colValW + 4.0f, curY + 2.0f, colDelW - 8.0f, 22.0f }, "删除", delTheme))
			{
				toDeleteIdx = i;
			}

			curY += 28.0f;
		}

		if (toDeleteIdx >= 0 && toDeleteIdx < static_cast<int>(m_BlackboardKeys.size()))
		{
			m_BlackboardKeys.erase(m_BlackboardKeys.begin() + toDeleteIdx);
			m_Dirty = true;
		}
	}

void BehaviorTreeEditorPanel::RenderLiveDebugger(Wui::WuiContext& ctx, const Wui::WuiRect& rect, PanelHost& host)
	{
		const Wui::WuiTheme& theme = host.Theme();
		Wui::PanelBackground(ctx, rect, Wui::WuiColor { 0.10f, 0.12f, 0.16f, 1.0f }, 0.0f);
		Wui::HighlightOutline(ctx, rect, theme.Border, 0.0f, 1.0f);

		const Wui::WuiRect headRect { rect.X, rect.Y, rect.W, 26.0f };
		Wui::PanelBackground(ctx, headRect, Wui::WuiColor { 0.14f, 0.18f, 0.24f, 1.0f }, 0.0f);
		Wui::HighlightOutline(ctx, headRect, theme.Border, 0.0f, 1.0f);

		Wui::Label(ctx, { rect.X + 8.0f, rect.Y + 5.0f }, "AI 运行时动态调试器与黑板快照 (Live Debugger & Blackboard Monitor)", theme.Warning, theme.FontSizeSmall);

		Entity entity = host.GetSelectedEntity();
		bool hasBt = entity.IsValid() && entity.HasComponent<BehaviorTreeComponent>();

		float curY = rect.Y + 32.0f;
		float startX = rect.X + 12.0f;

		if (!hasBt)
		{
			Wui::Label(ctx, { startX, curY + 8.0f }, "当前视口中未选中挂载了 BehaviorTreeComponent 的实体 (可在视口中选中 NPC 测试)", theme.TextMuted, theme.FontSizeSmall);

			if (entity.IsValid())
			{
				if (Wui::Button(ctx, Wui::HashId("bt.dbg.mount"), { startX + 480.0f, curY + 4.0f, 160.0f, 24.0f }, "一键为该实体挂载行为树", theme))
				{
					ApplyToSelected(host);
				}
			}
			return;
		}

		auto& bt = entity.GetComponent<BehaviorTreeComponent>();

		// 1. 实体状态行
		std::string entInfo = "已绑定实体: #" + std::to_string(static_cast<uint32_t>(static_cast<entt::entity>(entity)));
		if (entity.HasComponent<TagComponent>())
		{
			entInfo += " (" + StringPool::Get().NameOf(entity.GetComponent<TagComponent>().Tag) + ")";
		}
		Wui::Label(ctx, { startX, curY + 2.0f }, entInfo.c_str(), theme.Accent, theme.FontSizeSmall);

		const char* statusStr = "[Inactive]";
		Wui::WuiColor stColor = theme.TextMuted;
		switch (bt.Status)
		{
		case BehaviorStatus::Running: statusStr = "[Running]"; stColor = theme.Warning; break;
		case BehaviorStatus::Success: statusStr = "[Success]"; stColor = theme.Success; break;
		case BehaviorStatus::Failure: statusStr = "[Failure]"; stColor = theme.Danger; break;
		default: break;
		}

		const Wui::WuiRect stBadge { startX + 220.0f, curY, 80.0f, 20.0f };
		Wui::PanelBackground(ctx, stBadge, Wui::WuiColor { stColor.R * 0.25f, stColor.G * 0.25f, stColor.B * 0.25f, 0.9f }, theme.Radius);
		Wui::HighlightOutline(ctx, stBadge, stColor, theme.Radius, 1.0f);
		Wui::Label(ctx, { stBadge.X + 8.0f, stBadge.Y + 3.0f }, statusStr, stColor, theme.FontSizeSmall);

		std::string nodeName = "活跃节点: #" + std::to_string(bt.CurrentNodeIndex);
		BTNodeDesc* activeNode = FindNode(bt.CurrentNodeIndex);
		if (activeNode) nodeName += " (" + activeNode->Title + ")";
		Wui::Label(ctx, { startX + 310.0f, curY + 2.0f }, nodeName.c_str(), theme.Text, theme.FontSizeSmall);

		char timerBuf[32];
		snprintf(timerBuf, sizeof(timerBuf), "Timer: %.2fs", bt.ExecutionTimer);
		Wui::Label(ctx, { startX + 530.0f, curY + 2.0f }, timerBuf, theme.Warning, theme.FontSizeSmall);

		char stateBuf[32];
		snprintf(stateBuf, sizeof(stateBuf), "State ID: #%u", static_cast<unsigned int>(bt.StateMachineState));
		Wui::Label(ctx, { startX + 640.0f, curY + 2.0f }, stateBuf, theme.Accent, theme.FontSizeSmall);

		std::string stackStr = "调用栈深度 [" + std::to_string(bt.ActiveDepth) + "]: ";
		for (uint8_t d = 0; d < bt.ActiveDepth && d < 8; ++d) {
			stackStr += "-> #" + std::to_string(bt.ActivePath[d]);
		}
		Wui::Label(ctx, { startX + 750.0f, curY + 2.0f }, stackStr.c_str(), theme.TextMuted, theme.FontSizeSmall);

		curY += 26.0f;

		// 2. 黑板实时快照
		std::ostringstream ssBb;
		ssBb << "TargetPos: (" << bt.BlackboardTargetPos.x << ", " << bt.BlackboardTargetPos.y << ", " << bt.BlackboardTargetPos.z << ")  |  ";
		ssBb << "TargetEntity: #" << bt.TargetEntity << "  |  ";
		char valBuf[32];
		snprintf(valBuf, sizeof(valBuf), "BBValue: %.1f", bt.BlackboardValue);
		ssBb << valBuf << "  |  ";
		ssBb << "BBTag: " << bt.BlackboardTag;

		const Wui::WuiRect bbSnapRect { startX, curY, rect.W - 24.0f, 24.0f };
		Wui::PanelBackground(ctx, bbSnapRect, theme.CardBg, theme.Radius);
		Wui::HighlightOutline(ctx, bbSnapRect, theme.Border, theme.Radius, 1.0f);
		Wui::Label(ctx, { bbSnapRect.X + 8.0f, bbSnapRect.Y + 4.0f }, ssBb.str().c_str(), theme.Text, theme.FontSizeSmall);

		curY += 28.0f;

		// 3. 黑板交互热调 (Blackboard Live Tweaker)
		float twkX = startX;
		Wui::Label(ctx, { twkX, curY + 4.0f }, "黑板微调:", theme.Accent, theme.FontSizeSmall);
		twkX += 65.0f;

		Wui::Label(ctx, { twkX, curY + 4.0f }, "数值 (Value):", theme.TextMuted, theme.FontSizeSmall);
		twkX += 80.0f;

		if (Wui::DragFloat(ctx, Wui::HashId("bt.dbg.twk_val"), { twkX, curY, 80.0f, 22.0f }, bt.BlackboardValue, 1.0f, -9999.0f, 9999.0f, theme))
		{
			host.MarkDocumentDirty();
		}
		twkX += 90.0f;

		Wui::Label(ctx, { twkX, curY + 4.0f }, "标签 (Tag):", theme.TextMuted, theme.FontSizeSmall);
		twkX += 70.0f;

		int64_t tagVal = static_cast<int64_t>(bt.BlackboardTag);
		if (Wui::DragInt(ctx, Wui::HashId("bt.dbg.twk_tag"), { twkX, curY, 65.0f, 22.0f }, tagVal, 0, 99, theme))
		{
			bt.BlackboardTag = static_cast<uint32_t>(std::clamp<int64_t>(tagVal, 0, 99));
			host.MarkDocumentDirty();
		}
		twkX += 75.0f;

		if (Wui::Button(ctx, Wui::HashId("bt.dbg.clear_target"), { twkX, curY, 110.0f, 22.0f }, "清除目标实体", theme))
		{
			bt.TargetEntity = 0;
			host.MarkDocumentDirty();
			m_StatusFeedback = "已重置黑板目标实体 (TargetEntity = 0)";
		}

		curY += 28.0f;

		// 4. 通用控制动作按钮
		float btnX = startX;
		const float btnH = 22.0f;

		if (Wui::Button(ctx, Wui::HashId("bt.dbg.step"), { btnX, curY, 120.0f, btnH }, "单步推进 (Step)", theme))
		{
			bt.ExecutionTimer += 0.1f;
			host.MarkDocumentDirty();
			m_StatusFeedback = "已触发单步步进 (+0.1s)";
		}
		btnX += 128.0f;

		if (Wui::Button(ctx, Wui::HashId("bt.dbg.reset"), { btnX, curY, 130.0f, btnH }, "重置树 (Reset Tree)", theme))
		{
			bt.Status = BehaviorStatus::Inactive;
			bt.ExecutionTimer = 0.0f;
			bt.CurrentNodeIndex = 0;
			host.MarkDocumentDirty();
			m_StatusFeedback = "已重置行为树状态 (Inactive, Timer=0)";
		}
		btnX += 138.0f;

		if (Wui::Button(ctx, Wui::HashId("bt.dbg.halt"), { btnX, curY, 120.0f, btnH }, "暂停运行 (Halt)", theme))
		{
			bt.Status = BehaviorStatus::Inactive;
			host.MarkDocumentDirty();
			m_StatusFeedback = "已暂停行为树执行 (Inactive)";
		}
		btnX += 128.0f;

		Wui::WuiTheme evalTheme = theme;
		evalTheme.ButtonBg = Wui::WuiColor { 0.15f, 0.40f, 0.25f, 0.95f };
		evalTheme.Border = Wui::WuiColor { 0.30f, 0.80f, 0.45f, 1.0f };
		if (Wui::Button(ctx, Wui::HashId("bt.dbg.eval"), { btnX, curY, 130.0f, btnH }, "强制评估 (Evaluate)", evalTheme))
		{
			bt.Status = BehaviorStatus::Running;
			host.MarkDocumentDirty();
			m_StatusFeedback = "已触发行为树强制评估运行 (Running)";
		}
	}
}
