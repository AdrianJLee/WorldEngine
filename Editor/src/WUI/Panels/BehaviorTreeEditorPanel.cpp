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
		std::filesystem::path p(logicalPath);
		if (std::filesystem::exists(p))
			return std::filesystem::absolute(p);
		std::filesystem::path fromProj = Paths::ProjectDir() / p;
		if (std::filesystem::exists(fromProj))
			return std::filesystem::absolute(fromProj);
		std::filesystem::path fromAssetRoot = Paths::AssetRoot() / p;
		if (std::filesystem::exists(fromAssetRoot))
			return std::filesystem::absolute(fromAssetRoot);
		return std::filesystem::absolute(fromAssetRoot);
	}

	void BehaviorTreeEditorPanel::NewAsset()
	{
		SetDefaultTemplate();
		m_LogicalPath = "assets/ai/new_behavior_tree.wbt";
		m_AssetName = "new_behavior_tree";
		m_Dirty = true;
		m_StatusFeedback = "新建行为树资产模板 (New Asset Created)";
	}

	bool BehaviorTreeEditorPanel::LoadAsset(const std::string& logicalPath)
	{
		std::string normalized = logicalPath;
		std::replace(normalized.begin(), normalized.end(), '\\', '/');
		std::filesystem::path physical = ResolvePhysicalPath(normalized);

		std::ifstream file(physical);
		if (!file.is_open())
		{
			m_StatusFeedback = "无法打开文件: " + physical.string();
			return false;
		}

		m_Nodes.clear();
		m_BlackboardKeys.clear();
		m_LogicalPath = normalized;
		m_AssetName = physical.stem().string();

		std::string line;
		std::string currentSection;
		BTNodeDesc currentNode;
		bool inNode = false;

		BTBlackboardKeyDesc currentBb;
		bool inBb = false;

		auto flushNode = [this, &currentNode, &inNode]()
		{
			if (inNode && currentNode.Id != 0)
			{
				m_Nodes.push_back(currentNode);
				inNode = false;
			}
		};

		auto flushBb = [this, &currentBb, &inBb]()
		{
			if (inBb && !currentBb.KeyName.empty())
			{
				m_BlackboardKeys.push_back(currentBb);
				inBb = false;
			}
		};

		uint32_t maxId = 0;

		while (std::getline(file, line))
		{
			size_t start = line.find_first_not_of(" \t\r\n");
			if (start == std::string::npos || line[start] == '#')
				continue;

			std::string trimmed = line.substr(start);

			if (trimmed.rfind("Blackboard:", 0) == 0)
			{
				flushNode();
				currentSection = "Blackboard";
				continue;
			}
			if (trimmed.rfind("Nodes:", 0) == 0)
			{
				flushBb();
				currentSection = "Nodes";
				continue;
			}

			if (currentSection == "Blackboard")
			{
				if (trimmed.rfind("- Key:", 0) == 0)
				{
					flushBb();
					inBb = true;
					currentBb = {};
					currentBb.KeyName = trimmed.substr(6);
					size_t kStart = currentBb.KeyName.find_first_not_of(" \t\"");
					size_t kEnd = currentBb.KeyName.find_last_not_of(" \t\"");
					if (kStart != std::string::npos && kEnd != std::string::npos)
						currentBb.KeyName = currentBb.KeyName.substr(kStart, kEnd - kStart + 1);
				}
				else if (inBb && trimmed.rfind("Type:", 0) == 0)
				{
					currentBb.TypeName = trimmed.substr(5);
					size_t tStart = currentBb.TypeName.find_first_not_of(" \t\"");
					size_t tEnd = currentBb.TypeName.find_last_not_of(" \t\"");
					if (tStart != std::string::npos && tEnd != std::string::npos)
						currentBb.TypeName = currentBb.TypeName.substr(tStart, tEnd - tStart + 1);
				}
				else if (inBb && trimmed.rfind("Default:", 0) == 0)
				{
					currentBb.DefaultValue = trimmed.substr(8);
					size_t dStart = currentBb.DefaultValue.find_first_not_of(" \t\"");
					size_t dEnd = currentBb.DefaultValue.find_last_not_of(" \t\"");
					if (dStart != std::string::npos && dEnd != std::string::npos)
						currentBb.DefaultValue = currentBb.DefaultValue.substr(dStart, dEnd - dStart + 1);
				}
			}
			else if (currentSection == "Nodes")
			{
				if (trimmed.rfind("- Id:", 0) == 0)
				{
					flushNode();
					inNode = true;
					currentNode = {};
					try { currentNode.Id = static_cast<uint32_t>(std::stoul(trimmed.substr(5))); }
					catch (...) { currentNode.Id = 0; }
					if (currentNode.Id > maxId) maxId = currentNode.Id;
				}
				else if (inNode && trimmed.rfind("ParentId:", 0) == 0)
				{
					try { currentNode.ParentId = static_cast<uint32_t>(std::stoul(trimmed.substr(9))); }
					catch (...) { currentNode.ParentId = 0; }
				}
				else if (inNode && trimmed.rfind("Kind:", 0) == 0)
				{
					std::string kStr = trimmed.substr(5);
					size_t kStart = kStr.find_first_not_of(" \t\"");
					size_t kEnd = kStr.find_last_not_of(" \t\"");
					if (kStart != std::string::npos && kEnd != std::string::npos)
						kStr = kStr.substr(kStart, kEnd - kStart + 1);

					if (kStr == "Sequence") currentNode.Kind = BTNodeKind::Sequence;
					else if (kStr == "Condition") currentNode.Kind = BTNodeKind::Condition;
					else if (kStr == "Action") currentNode.Kind = BTNodeKind::Action;
					else currentNode.Kind = BTNodeKind::Selector;
				}
				else if (inNode && trimmed.rfind("Title:", 0) == 0)
				{
					currentNode.Title = trimmed.substr(6);
					size_t tStart = currentNode.Title.find_first_not_of(" \t\"");
					size_t tEnd = currentNode.Title.find_last_not_of(" \t\"");
					if (tStart != std::string::npos && tEnd != std::string::npos)
						currentNode.Title = currentNode.Title.substr(tStart, tEnd - tStart + 1);
				}
				else if (inNode && trimmed.rfind("Description:", 0) == 0)
				{
					currentNode.Description = trimmed.substr(12);
					size_t dStart = currentNode.Description.find_first_not_of(" \t\"");
					size_t dEnd = currentNode.Description.find_last_not_of(" \t\"");
					if (dStart != std::string::npos && dEnd != std::string::npos)
						currentNode.Description = currentNode.Description.substr(dStart, dEnd - dStart + 1);
				}
				else if (inNode && trimmed.rfind("ConditionType:", 0) == 0)
				{
					currentNode.ConditionType = trimmed.substr(14);
					size_t cStart = currentNode.ConditionType.find_first_not_of(" \t\"");
					size_t cEnd = currentNode.ConditionType.find_last_not_of(" \t\"");
					if (cStart != std::string::npos && cEnd != std::string::npos)
						currentNode.ConditionType = currentNode.ConditionType.substr(cStart, cEnd - cStart + 1);
				}
				else if (inNode && trimmed.rfind("ActionType:", 0) == 0)
				{
					currentNode.ActionType = trimmed.substr(11);
					size_t aStart = currentNode.ActionType.find_first_not_of(" \t\"");
					size_t aEnd = currentNode.ActionType.find_last_not_of(" \t\"");
					if (aStart != std::string::npos && aEnd != std::string::npos)
						currentNode.ActionType = currentNode.ActionType.substr(aStart, aEnd - aStart + 1);
				}
				else if (inNode && trimmed.rfind("FloatParam:", 0) == 0)
				{
					try { currentNode.FloatParam = std::stof(trimmed.substr(11)); }
					catch (...) { currentNode.FloatParam = 0.0f; }
				}
				else if (inNode && trimmed.rfind("StringParam:", 0) == 0)
				{
					currentNode.StringParam = trimmed.substr(12);
					size_t sStart = currentNode.StringParam.find_first_not_of(" \t\"");
					size_t sEnd = currentNode.StringParam.find_last_not_of(" \t\"");
					if (sStart != std::string::npos && sEnd != std::string::npos)
						currentNode.StringParam = currentNode.StringParam.substr(sStart, sEnd - sStart + 1);
				}
			}
		}

		flushNode();
		flushBb();

		m_NextNodeId = maxId + 1;
		if (m_NextNodeId < 2) m_NextNodeId = 2;
		m_SelectedNodeId = m_Nodes.empty() ? 1 : m_Nodes[0].Id;
		m_Dirty = false;
		m_StatusFeedback = "载入成功: " + m_LogicalPath + " (" + std::to_string(m_Nodes.size()) + " 个节点)";
		m_Nodes = m_Asset.Nodes();
		m_BlackboardKeys = m_Asset.BlackboardKeys();
		return true;
	}

	bool BehaviorTreeEditorPanel::SaveAsset()
	{
		m_Asset.Nodes() = m_Nodes;
		m_Asset.BlackboardKeys() = m_BlackboardKeys;
		std::filesystem::path physical = ResolvePhysicalPath(m_LogicalPath);
		std::filesystem::create_directories(physical.parent_path());

		std::ofstream out(physical, std::ios::binary | std::ios::trunc);
		if (!out.is_open())
		{
			m_StatusFeedback = "保存失败，无法写入: " + physical.string();
			return false;
		}

		out << "FormatVersion: 1\n";
		out << "Name: " << (m_AssetName.empty() ? physical.stem().string() : m_AssetName) << "\n";
		out << "Blackboard:\n";
		for (const auto& bb : m_BlackboardKeys)
		{
			out << "  - Key: " << bb.KeyName << "\n";
			out << "    Type: " << bb.TypeName << "\n";
			out << "    Default: " << bb.DefaultValue << "\n";
		}

		out << "Nodes:\n";
		for (const auto& node : m_Nodes)
		{
			out << "  - Id: " << node.Id << "\n";
			out << "    ParentId: " << node.ParentId << "\n";
			const char* kStr = "Selector";
			if (node.Kind == BTNodeKind::Sequence) kStr = "Sequence";
			else if (node.Kind == BTNodeKind::Condition) kStr = "Condition";
			else if (node.Kind == BTNodeKind::Action) kStr = "Action";

			out << "    Kind: " << kStr << "\n";
			out << "    Title: \"" << node.Title << "\"\n";
			if (!node.Description.empty())
				out << "    Description: \"" << node.Description << "\"\n";
			if (!node.ConditionType.empty())
				out << "    ConditionType: \"" << node.ConditionType << "\"\n";
			if (!node.ActionType.empty())
				out << "    ActionType: \"" << node.ActionType << "\"\n";
			if (node.FloatParam != 0.0f)
				out << "    FloatParam: " << node.FloatParam << "\n";
			if (!node.StringParam.empty())
				out << "    StringParam: \"" << node.StringParam << "\"\n";
		}
		out.close();

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
		child.ParentId = parentId;
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
			child.ConditionType = "HealthBelow";
			child.FloatParam = 50.0f;
			break;
		case BTNodeKind::Action:
			child.Title = "Action Node " + std::to_string(child.Id);
			child.ActionType = "Patrol";
			child.FloatParam = 5.0f;
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
		const float debuggerH = 130.0f;
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

		BTNodeDesc* node = FindNode(m_SelectedNodeId);
		if (!node)
		{
			Wui::Label(ctx, { rect.X + 16.0f, rect.Y + 48.0f }, "请在左侧大纲树中选择一个节点进行编辑", theme.TextMuted, theme.FontSizeSmall);
			return;
		}

		float startX = rect.X + 16.0f;
		float curY = rect.Y + 40.0f;
		float fieldW = rect.W - 32.0f;
		float labelW = 120.0f;
		float inputW = fieldW - labelW;

		std::string idText = "节点 ID: #" + std::to_string(node->Id) + "  |  父节点 ID: #" + std::to_string(node->ParentId);
		Wui::Label(ctx, { startX, curY }, idText.c_str(), theme.Accent, theme.FontSizeSmall);
		curY += 26.0f;

		Wui::Label(ctx, { startX, curY + 2.0f }, "节点标题 (Title):", theme.Text, theme.FontSizeSmall);
		if (Wui::TextField(ctx, Wui::HashId((std::string("bt.det.title.") + std::to_string(node->Id)).c_str()), { startX + labelW, curY, inputW, 24.0f }, node->Title, theme))
		{
			m_Dirty = true;
		}
		curY += 30.0f;

		Wui::Label(ctx, { startX, curY + 2.0f }, "节点描述 (Desc):", theme.Text, theme.FontSizeSmall);
		if (Wui::TextField(ctx, Wui::HashId((std::string("bt.det.desc.") + std::to_string(node->Id)).c_str()), { startX + labelW, curY, inputW, 24.0f }, node->Description, theme))
		{
			m_Dirty = true;
		}
		curY += 34.0f;

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
				m_Dirty = true;
			}
			kindX += kindBtnW + 4.0f;
		};

		renderKindBtn(BTNodeKind::Selector, "Selector", Wui::HashId((std::string("bt.det.k_sel.") + std::to_string(node->Id)).c_str()));
		renderKindBtn(BTNodeKind::Sequence, "Sequence", Wui::HashId((std::string("bt.det.k_seq.") + std::to_string(node->Id)).c_str()));
		renderKindBtn(BTNodeKind::Condition, "Condition", Wui::HashId((std::string("bt.det.k_cnd.") + std::to_string(node->Id)).c_str()));
		renderKindBtn(BTNodeKind::Action, "Action", Wui::HashId((std::string("bt.det.k_act.") + std::to_string(node->Id)).c_str()));
		curY += 36.0f;

		if (node->Kind == BTNodeKind::Condition)
		{
			const Wui::WuiRect condCard { startX, curY, fieldW, 140.0f };
			Wui::PanelBackground(ctx, condCard, Wui::WuiColor { 0.12f, 0.25f, 0.18f, 0.85f }, theme.Radius);
			Wui::HighlightOutline(ctx, condCard, Wui::WuiColor { 0.25f, 0.70f, 0.35f, 1.0f }, theme.Radius, 1.0f);

			float cardY = curY + 8.0f;
			Wui::Label(ctx, { startX + 8.0f, cardY }, "[条件谓词参数设置] (Condition Predicate)", theme.Success, theme.FontSizeSmall);
			cardY += 24.0f;

			float preW = (fieldW - 32.0f) / 3.0f;
			if (Wui::Button(ctx, Wui::HashId((std::string("bt.cnd.pre1.") + std::to_string(node->Id)).c_str()), { startX + 8.0f, cardY, preW, 22.0f }, "HealthBelow", theme))
			{
				node->ConditionType = "HealthBelow";
				node->FloatParam = 30.0f;
				m_Dirty = true;
			}
			if (Wui::Button(ctx, Wui::HashId((std::string("bt.cnd.pre2.") + std::to_string(node->Id)).c_str()), { startX + 12.0f + preW, cardY, preW, 22.0f }, "HasTarget", theme))
			{
				node->ConditionType = "HasTarget";
				m_Dirty = true;
			}
			if (Wui::Button(ctx, Wui::HashId((std::string("bt.cnd.pre3.") + std::to_string(node->Id)).c_str()), { startX + 16.0f + preW * 2.0f, cardY, preW, 22.0f }, "IsEnemyInRange", theme))
			{
				node->ConditionType = "IsEnemyInRange";
				node->FloatParam = 15.0f;
				m_Dirty = true;
			}
			cardY += 28.0f;

			Wui::Label(ctx, { startX + 8.0f, cardY + 2.0f }, "条件类型 (Type):", theme.Text, theme.FontSizeSmall);
			if (Wui::TextField(ctx, Wui::HashId((std::string("bt.cnd.type.") + std::to_string(node->Id)).c_str()), { startX + labelW, cardY, inputW - 16.0f, 22.0f }, node->ConditionType, theme))
			{
				m_Dirty = true;
			}
			cardY += 28.0f;

			Wui::Label(ctx, { startX + 8.0f, cardY + 2.0f }, "数值阈值 (Float):", theme.Text, theme.FontSizeSmall);
			if (Wui::DragFloat(ctx, Wui::HashId((std::string("bt.cnd.param.") + std::to_string(node->Id)).c_str()), { startX + labelW, cardY, inputW - 16.0f, 22.0f }, node->FloatParam, 1.0f, 0.0f, 9999.0f, theme))
			{
				m_Dirty = true;
			}
			curY += 150.0f;
		}
		else if (node->Kind == BTNodeKind::Action)
		{
			const Wui::WuiRect actCard { startX, curY, fieldW, 170.0f };
			Wui::PanelBackground(ctx, actCard, Wui::WuiColor { 0.28f, 0.18f, 0.10f, 0.85f }, theme.Radius);
			Wui::HighlightOutline(ctx, actCard, Wui::WuiColor { 0.85f, 0.50f, 0.15f, 1.0f }, theme.Radius, 1.0f);

			float cardY = curY + 8.0f;
			Wui::Label(ctx, { startX + 8.0f, cardY }, "[执行动作参数设置] (Action Execution)", theme.Warning, theme.FontSizeSmall);
			cardY += 24.0f;

			float preW = (fieldW - 36.0f) / 4.0f;
			if (Wui::Button(ctx, Wui::HashId((std::string("bt.act.pre1.") + std::to_string(node->Id)).c_str()), { startX + 8.0f, cardY, preW, 22.0f }, "SeekSO", theme))
			{
				node->ActionType = "SeekSmartObject";
				node->StringParam = "MedicalStation";
				m_Dirty = true;
			}
			if (Wui::Button(ctx, Wui::HashId((std::string("bt.act.pre2.") + std::to_string(node->Id)).c_str()), { startX + 12.0f + preW, cardY, preW, 22.0f }, "UseSO", theme))
			{
				node->ActionType = "UseSmartObject";
				m_Dirty = true;
			}
			if (Wui::Button(ctx, Wui::HashId((std::string("bt.act.pre3.") + std::to_string(node->Id)).c_str()), { startX + 16.0f + preW * 2.0f, cardY, preW, 22.0f }, "Patrol", theme))
			{
				node->ActionType = "Patrol";
				node->FloatParam = 10.0f;
				m_Dirty = true;
			}
			if (Wui::Button(ctx, Wui::HashId((std::string("bt.act.pre4.") + std::to_string(node->Id)).c_str()), { startX + 20.0f + preW * 3.0f, cardY, preW, 22.0f }, "Wait", theme))
			{
				node->ActionType = "Wait";
				node->FloatParam = 3.0f;
				m_Dirty = true;
			}
			cardY += 28.0f;

			Wui::Label(ctx, { startX + 8.0f, cardY + 2.0f }, "动作类型 (Type):", theme.Text, theme.FontSizeSmall);
			if (Wui::TextField(ctx, Wui::HashId((std::string("bt.act.type.") + std::to_string(node->Id)).c_str()), { startX + labelW, cardY, inputW - 16.0f, 22.0f }, node->ActionType, theme))
			{
				m_Dirty = true;
			}
			cardY += 28.0f;

			Wui::Label(ctx, { startX + 8.0f, cardY + 2.0f }, "浮点数值 (Float):", theme.Text, theme.FontSizeSmall);
			if (Wui::DragFloat(ctx, Wui::HashId((std::string("bt.act.float.") + std::to_string(node->Id)).c_str()), { startX + labelW, cardY, inputW - 16.0f, 22.0f }, node->FloatParam, 0.5f, 0.0f, 3600.0f, theme))
			{
				m_Dirty = true;
			}
			cardY += 28.0f;

			Wui::Label(ctx, { startX + 8.0f, cardY + 2.0f }, "字符串 (String):", theme.Text, theme.FontSizeSmall);
			if (Wui::TextField(ctx, Wui::HashId((std::string("bt.act.str.") + std::to_string(node->Id)).c_str()), { startX + labelW, cardY, inputW - 16.0f, 22.0f }, node->StringParam, theme))
			{
				m_Dirty = true;
			}
			curY += 180.0f;
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

		float curX = rect.X + 16.0f;
		float curY = rect.Y + 36.0f;

		if (Wui::Button(ctx, Wui::HashId("bt.bb.add"), { curX, curY, 110.0f, 24.0f }, "+ 添加变量", theme))
		{
			m_BlackboardKeys.push_back({ "NewKey_" + std::to_string(m_BlackboardKeys.size()), "Float", "0.0" });
			m_Dirty = true;
		}
		curX += 118.0f;

		if (Wui::Button(ctx, Wui::HashId("bt.bb.pre_target"), { curX, curY, 130.0f, 24.0f }, "+ 预置 TargetPos", theme))
		{
			m_BlackboardKeys.push_back({ "TargetPos", "Vector3", "[0.0, 0.0, 0.0]" });
			m_Dirty = true;
		}
		curX += 138.0f;

		if (Wui::Button(ctx, Wui::HashId("bt.bb.pre_ent"), { curX, curY, 130.0f, 24.0f }, "+ 预置 TargetEntity", theme))
		{
			m_BlackboardKeys.push_back({ "TargetEntity", "Entity", "0" });
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

			if (Wui::Button(ctx, Wui::HashId((std::string("bt.bb.t.") + std::to_string(i)).c_str()), { rowRect.X + colKeyW + 4.0f, curY + 2.0f, colTypeW - 8.0f, 22.0f }, bb.TypeName.c_str(), theme))
			{
				if (bb.TypeName == "Float") bb.TypeName = "Vector3";
				else if (bb.TypeName == "Vector3") bb.TypeName = "Entity";
				else if (bb.TypeName == "Entity") bb.TypeName = "Bool";
				else if (bb.TypeName == "Bool") bb.TypeName = "String";
				else bb.TypeName = "Float";
				m_Dirty = true;
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

		std::string entInfo = "已绑定实体: #" + std::to_string(static_cast<uint32_t>(static_cast<entt::entity>(entity)));
		if (entity.HasComponent<TagComponent>())
		{
			entInfo += " (" + StringPool::Get().NameOf(entity.GetComponent<TagComponent>().Tag) + ")";
		}
		Wui::Label(ctx, { startX, curY }, entInfo.c_str(), theme.Accent, theme.FontSizeSmall);

		const char* statusStr = "[Inactive] 休眠";
		Wui::WuiColor stColor = theme.TextMuted;
		switch (bt.Status)
		{
		case BehaviorStatus::Running: statusStr = "[Running] 运行中"; stColor = theme.Warning; break;
		case BehaviorStatus::Success: statusStr = "[Success] 成功"; stColor = theme.Success; break;
		case BehaviorStatus::Failure: statusStr = "[Failure] 失败"; stColor = theme.Danger; break;
		default: break;
		}

		const Wui::WuiRect stBadge { startX + 220.0f, curY - 2.0f, 110.0f, 20.0f };
		Wui::PanelBackground(ctx, stBadge, Wui::WuiColor { stColor.R * 0.25f, stColor.G * 0.25f, stColor.B * 0.25f, 0.9f }, theme.Radius);
		Wui::HighlightOutline(ctx, stBadge, stColor, theme.Radius, 1.0f);
		Wui::Label(ctx, { stBadge.X + 8.0f, stBadge.Y + 3.0f }, statusStr, stColor, theme.FontSizeSmall);

		std::string nodeName = "活跃节点: #" + std::to_string(bt.CurrentNodeIndex);
		BTNodeDesc* activeNode = FindNode(bt.CurrentNodeIndex);
		if (activeNode) nodeName += " (" + activeNode->Title + ")";
		Wui::Label(ctx, { startX + 345.0f, curY }, nodeName.c_str(), theme.Text, theme.FontSizeSmall);

		const char* fsmNames[] = { "0-Patrol", "1-SeekSO", "2-Interact", "3-Cooldown" };
		const char* fsmStr = (bt.StateMachineState < 4) ? fsmNames[bt.StateMachineState] : "Unknown";
		std::string fsmInfo = "FSM: " + std::string(fsmStr);
		Wui::Label(ctx, { startX + 620.0f, curY }, fsmInfo.c_str(), theme.Warning, theme.FontSizeSmall);

		curY += 26.0f;

		std::ostringstream ssBb;
		ssBb << "TargetPos: (" << bt.BlackboardTargetPos.x << ", " << bt.BlackboardTargetPos.y << ", " << bt.BlackboardTargetPos.z << ")  |  ";
		ssBb << "TargetEntity: #" << bt.TargetEntity << "  |  ";
		ssBb << "Timer: " << bt.ExecutionTimer << "s  |  ";
		ssBb << "BBValue (生命/数值): " << bt.BlackboardValue;

		const Wui::WuiRect bbSnapRect { startX, curY, rect.W - 24.0f, 24.0f };
		Wui::PanelBackground(ctx, bbSnapRect, theme.CardBg, theme.Radius);
		Wui::HighlightOutline(ctx, bbSnapRect, theme.Border, theme.Radius, 1.0f);
		Wui::Label(ctx, { bbSnapRect.X + 8.0f, bbSnapRect.Y + 4.0f }, ssBb.str().c_str(), theme.Text, theme.FontSizeSmall);

		curY += 30.0f;

		float dbgBtnW = 140.0f;
		float dbgX = startX;

		if (Wui::Button(ctx, Wui::HashId("bt.dbg.step"), { dbgX, curY, dbgBtnW, 24.0f }, "单步推进 (Step)", theme))
		{
			bt.ExecutionTimer += 0.1f;
			host.MarkDocumentDirty();
			m_StatusFeedback = "已触发单步步进";
		}
		dbgX += dbgBtnW + 8.0f;

		Wui::WuiTheme dmgTheme = theme;
		dmgTheme.ButtonBg = Wui::WuiColor { 0.50f, 0.15f, 0.15f, 0.95f };
		dmgTheme.Border = Wui::WuiColor { 0.90f, 0.30f, 0.30f, 1.0f };
		if (Wui::Button(ctx, Wui::HashId("bt.dbg.damage"), { dbgX, curY, 160.0f, 24.0f }, "施加 50 伤害 (Trigger Heal)", dmgTheme))
		{
			if (entity.HasComponent<HealthComponent>())
			{
				auto& hc = entity.GetComponent<HealthComponent>();
				hc.CurrentHealth = std::max(0.0f, hc.CurrentHealth - 50.0f);
				bt.BlackboardValue = hc.CurrentHealth;
				m_StatusFeedback = "已对实体施加 50 伤害，当前生命: " + std::to_string(static_cast<int>(hc.CurrentHealth));
			}
			else
			{
				bt.BlackboardValue = std::max(0.0f, bt.BlackboardValue - 50.0f);
				m_StatusFeedback = "扣减 BlackboardValue 50 点";
			}
			host.MarkDocumentDirty();
		}
		dbgX += 168.0f;

		Wui::WuiTheme healTheme = theme;
		healTheme.ButtonBg = Wui::WuiColor { 0.15f, 0.45f, 0.20f, 0.95f };
		healTheme.Border = Wui::WuiColor { 0.30f, 0.85f, 0.40f, 1.0f };
		if (Wui::Button(ctx, Wui::HashId("bt.dbg.heal"), { dbgX, curY, 140.0f, 24.0f }, "重置满血 (Reset 100)", healTheme))
		{
			if (entity.HasComponent<HealthComponent>())
			{
				auto& hc = entity.GetComponent<HealthComponent>();
				hc.CurrentHealth = hc.MaxHealth > 0.0f ? hc.MaxHealth : 100.0f;
				bt.BlackboardValue = hc.CurrentHealth;
				m_StatusFeedback = "已恢复实体生命值至满血";
			}
			else
			{
				bt.BlackboardValue = 100.0f;
				m_StatusFeedback = "重置 BlackboardValue 为 100.0";
			}
			host.MarkDocumentDirty();
		}
	}
}
