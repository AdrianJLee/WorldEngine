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
#include "World/Level/AI/EQS/EQSAsset.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <sstream>

namespace World
{
	namespace
	{
		Gameplay::BlackboardKeyType ParseKeyType(const std::string& typeStr)
		{
			if (typeStr == "Bool") return Gameplay::BlackboardKeyType::Bool;
			if (typeStr == "Int" || typeStr == "Int32") return Gameplay::BlackboardKeyType::Int32;
			if (typeStr == "Float") return Gameplay::BlackboardKeyType::Float;
			if (typeStr == "Double") return Gameplay::BlackboardKeyType::Double;
			if (typeStr == "Vector2" || typeStr == "Vec2") return Gameplay::BlackboardKeyType::Vector2;
			if (typeStr == "Vector3" || typeStr == "Vec3") return Gameplay::BlackboardKeyType::Vector3;
			if (typeStr == "Vector4" || typeStr == "Vec4" || typeStr == "Color" || typeStr == "Quat") return Gameplay::BlackboardKeyType::Vector4;
			if (typeStr == "Entity") return Gameplay::BlackboardKeyType::Entity;
			if (typeStr == "String") return Gameplay::BlackboardKeyType::String;
			if (typeStr.rfind("ArrayFloat", 0) == 0 || typeStr == "FloatArray" || typeStr == "Float[]") return Gameplay::BlackboardKeyType::ArrayFloat;
			if (typeStr.rfind("ArrayInt", 0) == 0 || typeStr == "IntArray" || typeStr == "Int[]") return Gameplay::BlackboardKeyType::ArrayInt32;
			if (typeStr.rfind("ArrayVector3", 0) == 0 || typeStr == "Vector3Array" || typeStr == "Vec3[]" || typeStr == "Waypoints") return Gameplay::BlackboardKeyType::ArrayVector3;
			if (typeStr.rfind("ArrayEntity", 0) == 0 || typeStr == "EntityArray" || typeStr == "Entity[]" || typeStr == "PerceivedThreats") return Gameplay::BlackboardKeyType::ArrayEntity;
			if (typeStr.rfind("ArrayString", 0) == 0 || typeStr == "StringArray" || typeStr == "String[]") return Gameplay::BlackboardKeyType::ArrayString;
			return Gameplay::BlackboardKeyType::Float;
		}

		Gameplay::BlackboardValue ParseKeyValue(Gameplay::BlackboardKeyType type, const std::string& valStr)
		{
			switch (type)
			{
			case Gameplay::BlackboardKeyType::Bool:
				return (valStr == "true" || valStr == "1");
			case Gameplay::BlackboardKeyType::Int32:
				try { return static_cast<int32_t>(std::stoi(valStr)); } catch (...) { return int32_t(0); }
			case Gameplay::BlackboardKeyType::Float:
				try { return std::stof(valStr); } catch (...) { return 0.0f; }
			case Gameplay::BlackboardKeyType::Double:
				try { return std::stod(valStr); } catch (...) { return 0.0; }
			case Gameplay::BlackboardKeyType::Vector2:
			{
				float x = 0.0f, y = 0.0f;
				if (std::sscanf(valStr.c_str(), "[%f, %f]", &x, &y) == 2 ||
				    std::sscanf(valStr.c_str(), "%f,%f", &x, &y) == 2)
					return glm::vec2(x, y);
				return glm::vec2(0.0f);
			}
			case Gameplay::BlackboardKeyType::Vector3:
			{
				float x = 0.0f, y = 0.0f, z = 0.0f;
				if (std::sscanf(valStr.c_str(), "[%f, %f, %f]", &x, &y, &z) == 3 ||
				    std::sscanf(valStr.c_str(), "%f,%f,%f", &x, &y, &z) == 3 ||
				    std::sscanf(valStr.c_str(), "%f %f %f", &x, &y, &z) == 3)
				{
					return glm::vec3(x, y, z);
				}
				return glm::vec3(0.0f);
			}
			case Gameplay::BlackboardKeyType::Vector4:
			{
				float x = 0.0f, y = 0.0f, z = 0.0f, w = 0.0f;
				if (std::sscanf(valStr.c_str(), "[%f, %f, %f, %f]", &x, &y, &z, &w) == 4 ||
				    std::sscanf(valStr.c_str(), "%f,%f,%f,%f", &x, &y, &z, &w) == 4)
					return glm::vec4(x, y, z, w);
				return glm::vec4(0.0f);
			}
			case Gameplay::BlackboardKeyType::Entity:
				try { return static_cast<uint32_t>(std::stoul(valStr)); } catch (...) { return uint32_t(0); }
			case Gameplay::BlackboardKeyType::String:
				return valStr;
			case Gameplay::BlackboardKeyType::ArrayFloat:
			{
				std::vector<float> vec;
				std::stringstream ss(valStr);
				float f;
				while (ss >> f) { vec.push_back(f); if (ss.peek() == ',' || ss.peek() == ' ') ss.ignore(); }
				return vec;
			}
			case Gameplay::BlackboardKeyType::ArrayInt32:
			{
				std::vector<int32_t> vec;
				std::stringstream ss(valStr);
				int32_t i;
				while (ss >> i) { vec.push_back(i); if (ss.peek() == ',' || ss.peek() == ' ') ss.ignore(); }
				return vec;
			}
			case Gameplay::BlackboardKeyType::ArrayEntity:
			{
				std::vector<uint32_t> vec;
				std::stringstream ss(valStr);
				uint32_t e;
				while (ss >> e) { vec.push_back(e); if (ss.peek() == ',' || ss.peek() == ' ') ss.ignore(); }
				return vec;
			}
			case Gameplay::BlackboardKeyType::ArrayString:
			{
				std::vector<std::string> vec;
				std::stringstream ss(valStr);
				std::string s;
				while (std::getline(ss, s, ',')) { if (!s.empty()) vec.push_back(s); }
				return vec;
			}
			case Gameplay::BlackboardKeyType::ArrayVector3:
			{
				std::vector<glm::vec3> vec;
				const char* p = valStr.c_str();
				float x, y, z;
				while ((p = std::strchr(p, '[')) != nullptr)
				{
					if (std::sscanf(p, "[%f, %f, %f]", &x, &y, &z) == 3)
						vec.push_back(glm::vec3(x, y, z));
					p++;
				}
				return vec;
			}
			default:
				return 0.0f;
			}
		}
	}
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

		m_LogicalPath = "assets/ai/new_behavior_tree.wbt";
		m_AssetName = "new_behavior_tree";
		m_Dirty = false;

		// 纯净极简的初始根节点
		m_Nodes.push_back({ 1, 0, BTNodeKind::Selector, "Root Selector", "行为树根选择器", "", "", 0.0f, "" });

		m_NextNodeId = 2;
		m_SelectedNodeId = 1;
		m_StatusFeedback = "已初始化空白行为树 (Clean Behavior Tree Ready)";
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
		if (m_Mode == EditorMode::Blackboard)
		{
			m_BlackboardAsset.Clear();
			m_BlackboardKeys = {
				{ "TargetPos", "Vector3", "[0.0, 0.0, 0.0]" },
				{ "TargetEntity", "Entity", "0" },
				{ "HasThreat", "Bool", "false" },
				{ "Value", "Float", "0.0" }
			};
			m_LogicalPath = "assets/ai/new_blackboard.wbb";
			m_AssetName = "new_blackboard";
			m_Dirty = true;
			m_StatusFeedback = "新建独立黑板资产 (New Blackboard Asset Created)";
			return;
		}
		if (m_Mode == EditorMode::EQSQuery)
		{
			m_LogicalPath = "assets/ai/new_eqs_query.weqs";
			m_AssetName = "new_eqs_query";
			m_EQSAsset = EQS::EQSAsset();
			m_EQSAsset.Name = "new_eqs_query";
			m_Dirty = true;
			m_StatusFeedback = "新建独立 EQS 空间决策资产 (New EQS Asset Created)";
			return;
		}

		m_Nodes.clear();
		m_BlackboardKeys.clear();
		m_BoundBlackboardPath.clear();
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

		const bool isWeqs = (normalized.rfind(".weqs") != std::string::npos || normalized.rfind(".weqsquery") != std::string::npos);
		if (isWeqs)
		{
			m_Mode = EditorMode::EQSQuery;
			m_LogicalPath = normalized;
			m_AssetName = physical.stem().string();

			std::string error;
			if (!m_EQSAsset.LoadFromFile(physical.string(), &error))
			{
				m_EQSAsset.Name = m_AssetName;
				m_EQSAsset.GeneratorDesc.Type = "ProjectedGrid";
				m_EQSAsset.GeneratorDesc.Radius = 20.0f;
				m_EQSAsset.GeneratorDesc.Spacing = 2.0f;
				m_Dirty = true;
				m_StatusFeedback = "新建独立 EQS 空间决策资产: " + normalized;
			}
			else
			{
				m_Dirty = false;
				m_StatusFeedback = "已载入独立 EQS 资产 (.weqs): " + m_LogicalPath;
			}

			g_EQSPreviewState.PipelineTests.clear();
			for (const auto& td : m_EQSAsset.Tests)
			{
				EQSTestItem item;
				item.Name = td.Name;
				item.Weight = td.Weight;
				item.IdealValue = td.IdealValue;
				item.FilterMin = td.FilterMin;
				item.FilterMax = td.FilterMax;
				item.GaussianSigma = td.GaussianSigma;
				item.Curve = static_cast<uint8_t>(td.Curve);
				item.Purpose = static_cast<uint8_t>(td.Purpose);
				item.Enabled = td.Enabled;
				if (td.TestType == "Distance") item.Type = 0;
				else if (td.TestType == "Trace" || td.TestType == "TraceLOS") item.Type = 1;
				else if (td.TestType == "Dot" || td.TestType == "DotAngle") item.Type = 2;
				else if (td.TestType == "WallProximity") item.Type = 3;
				else if (td.TestType == "Height") item.Type = 4;
				else if (td.TestType == "Overlap") item.Type = 5;
				else if (td.TestType == "FriendlyPenalty") item.Type = 6;
				else if (td.TestType == "CoverReservation") item.Type = 7;
				else if (td.TestType == "Scripted") item.Type = 8;
				g_EQSPreviewState.PipelineTests.push_back(item);
			}
			g_EQSPreviewState.GeneratorMode = (m_EQSAsset.GeneratorDesc.Type == "Circle") ? 1
				: ((m_EQSAsset.GeneratorDesc.Type == "AroundObstacles") ? 2 : 0);
			g_EQSPreviewState.QueryRadius = m_EQSAsset.GeneratorDesc.Radius;
			g_EQSPreviewState.GridSpacing = m_EQSAsset.GeneratorDesc.Spacing;
			g_EQSPreviewState.BoundAssetPath = m_LogicalPath;

			return true;
		}

		const bool isWbb = (normalized.rfind(".wbb") != std::string::npos || normalized.rfind(".wblackboard") != std::string::npos);

		if (isWbb)
		{
			m_Mode = EditorMode::Blackboard;
			m_ActiveTab = ViewTab::Blackboard;
			m_LogicalPath = normalized;
			m_AssetName = physical.stem().string();

			std::string error;
			if (!m_BlackboardAsset.LoadFromFile(physical.string(), &error))
			{
				m_BlackboardAsset.Clear();
				m_BlackboardKeys = {
					{ "TargetPos", "Vector3", "[0.0, 0.0, 0.0]" },
					{ "TargetEntity", "Entity", "0" },
					{ "HasThreat", "Bool", "false" },
					{ "Value", "Float", "0.0" }
				};
				m_Dirty = true;
				m_StatusFeedback = "新建独立黑板资产: " + normalized;
				return true;
			}

			m_BlackboardKeys.clear();
			for (const auto& k : m_BlackboardAsset.GetKeys())
			{
				std::string valStr = "0.0";
				if (std::holds_alternative<bool>(k.DefaultValue)) valStr = std::get<bool>(k.DefaultValue) ? "true" : "false";
				else if (std::holds_alternative<int32_t>(k.DefaultValue)) valStr = std::to_string(std::get<int32_t>(k.DefaultValue));
				else if (std::holds_alternative<float>(k.DefaultValue)) valStr = std::to_string(std::get<float>(k.DefaultValue));
				else if (std::holds_alternative<glm::vec3>(k.DefaultValue))
				{
					const auto& v = std::get<glm::vec3>(k.DefaultValue);
					valStr = "[" + std::to_string(v.x) + ", " + std::to_string(v.y) + ", " + std::to_string(v.z) + "]";
				}
				else if (std::holds_alternative<uint32_t>(k.DefaultValue)) valStr = std::to_string(std::get<uint32_t>(k.DefaultValue));
				else if (std::holds_alternative<std::string>(k.DefaultValue)) valStr = std::get<std::string>(k.DefaultValue);

				std::string typeStr = "Float";
				switch (k.Type)
				{
				case Gameplay::BlackboardKeyType::Bool: typeStr = "Bool"; break;
				case Gameplay::BlackboardKeyType::Int32: typeStr = "Int"; break;
				case Gameplay::BlackboardKeyType::Float: typeStr = "Float"; break;
				case Gameplay::BlackboardKeyType::Vector3: typeStr = "Vector3"; break;
				case Gameplay::BlackboardKeyType::Entity: typeStr = "Entity"; break;
				case Gameplay::BlackboardKeyType::String: typeStr = "String"; break;
				}
				m_BlackboardKeys.push_back({ k.Name, typeStr, valStr });
			}

			m_Dirty = false;
			m_StatusFeedback = "已载入独立黑板资产 (.wbb): " + m_LogicalPath;
			return true;
		}

		// 行为树资产 (.wbt) 载入流程
		m_Mode = EditorMode::BehaviorTree;
		m_ActiveTab = ViewTab::TreeOutline;

		std::string error;
		if (!m_Asset.LoadFromFile(physical.string(), &error))
		{
			SetDefaultTemplate();
			m_LogicalPath = normalized;
			m_AssetName = physical.stem().string();
			m_Dirty = true;
			m_StatusFeedback = "资产未就绪或无法读取: " + normalized + " (已切换为默认模板)";
			return true;
		}

		m_Nodes = m_Asset.Nodes();
		m_BoundBlackboardPath = m_Asset.GetBlackboardAssetPath();
		m_LogicalPath = normalized;
		m_AssetName = physical.stem().string();

		// 若行为树关联了外部独立 .wbb 黑板资产，优先从该资产同步变量定义
		if (!m_BoundBlackboardPath.empty())
		{
			std::filesystem::path bbPhys = ResolvePhysicalPath(m_BoundBlackboardPath);
			std::string bbErr;
			if (m_BlackboardAsset.LoadFromFile(bbPhys.string(), &bbErr))
			{
				m_BlackboardKeys.clear();
				for (const auto& k : m_BlackboardAsset.GetKeys())
				{
					std::string valStr = "0.0";
					if (std::holds_alternative<bool>(k.DefaultValue)) valStr = std::get<bool>(k.DefaultValue) ? "true" : "false";
					else if (std::holds_alternative<int32_t>(k.DefaultValue)) valStr = std::to_string(std::get<int32_t>(k.DefaultValue));
					else if (std::holds_alternative<float>(k.DefaultValue)) valStr = std::to_string(std::get<float>(k.DefaultValue));
					else if (std::holds_alternative<glm::vec3>(k.DefaultValue))
					{
						const auto& v = std::get<glm::vec3>(k.DefaultValue);
						valStr = "[" + std::to_string(v.x) + ", " + std::to_string(v.y) + ", " + std::to_string(v.z) + "]";
					}
					else if (std::holds_alternative<uint32_t>(k.DefaultValue)) valStr = std::to_string(std::get<uint32_t>(k.DefaultValue));
					else if (std::holds_alternative<std::string>(k.DefaultValue)) valStr = std::get<std::string>(k.DefaultValue);

					std::string typeStr = "Float";
					switch (k.Type)
					{
					case Gameplay::BlackboardKeyType::Bool: typeStr = "Bool"; break;
					case Gameplay::BlackboardKeyType::Int32: typeStr = "Int"; break;
					case Gameplay::BlackboardKeyType::Float: typeStr = "Float"; break;
					case Gameplay::BlackboardKeyType::Vector3: typeStr = "Vector3"; break;
					case Gameplay::BlackboardKeyType::Entity: typeStr = "Entity"; break;
					case Gameplay::BlackboardKeyType::String: typeStr = "String"; break;
					}
					m_BlackboardKeys.push_back({ k.Name, typeStr, valStr });
				}
			}
			else
			{
				m_BlackboardKeys = m_Asset.BlackboardKeys();
			}
		}
		else
		{
			m_BlackboardKeys = m_Asset.BlackboardKeys();
		}

		m_Dirty = false;
		m_StatusFeedback = "已载入行为树资产 (.wbt): " + m_LogicalPath;

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

		if (m_Mode == EditorMode::Blackboard)
		{
			m_BlackboardAsset.Clear();
			for (const auto& k : m_BlackboardKeys)
			{
				Gameplay::BlackboardKeyDesc desc;
				desc.Name = k.KeyName;
				desc.Type = ParseKeyType(k.TypeName);
				desc.DefaultValue = ParseKeyValue(desc.Type, k.DefaultValue);
				m_BlackboardAsset.AddKey(desc);
			}

			std::string error;
			if (!m_BlackboardAsset.SaveToFile(physical.string(), &error))
			{
				m_StatusFeedback = "保存黑板资产失败: " + error;
				return false;
			}
			m_Dirty = false;
			m_StatusFeedback = "黑板资产保存成功 (.wbb): " + m_LogicalPath;
			return true;
		}
		if (m_Mode == EditorMode::EQSQuery)
		{
			std::filesystem::path physical = ResolvePhysicalPath(m_LogicalPath);
			m_EQSAsset.Name = m_AssetName;
			m_EQSAsset.GeneratorDesc.Radius = g_EQSPreviewState.QueryRadius;
			m_EQSAsset.GeneratorDesc.Spacing = g_EQSPreviewState.GridSpacing;
			m_EQSAsset.GeneratorDesc.Type = (g_EQSPreviewState.GeneratorMode == 1) ? "Circle"
				: ((g_EQSPreviewState.GeneratorMode == 2) ? "AroundObstacles" : "ProjectedGrid");

			m_EQSAsset.Tests.clear();
			for (const auto& t : g_EQSPreviewState.PipelineTests)
			{
				EQS::EQSTestDesc td;
				td.Name = t.Name;
				td.Weight = t.Weight;
				td.IdealValue = t.IdealValue;
				td.FilterMin = t.FilterMin;
				td.FilterMax = t.FilterMax;
				td.GaussianSigma = t.GaussianSigma;
				td.Curve = static_cast<EQS::ScoringCurve>(t.Curve);
				td.Purpose = static_cast<EQS::TestPurpose>(t.Purpose);
				td.Enabled = t.Enabled;
				const char* typeStrs[] = { "Distance", "Trace", "Dot", "WallProximity", "Height", "Overlap", "FriendlyPenalty", "CoverReservation", "Scripted" };
				if (t.Type < 9) td.TestType = typeStrs[t.Type];
				td.ContextType = (t.Context == 0) ? "Querier" : ((t.Context == 1) ? "Target" : "AllThreats");
				m_EQSAsset.Tests.push_back(td);
			}

			std::string error;
			if (!m_EQSAsset.SaveToFile(physical.string(), &error))
			{
				m_StatusFeedback = "保存 EQS 资产失败: " + error;
				return false;
			}
			m_Dirty = false;
			m_StatusFeedback = "EQS 空间决策资产保存成功 (.weqs): " + m_LogicalPath;
			return true;
		}

		// 行为树保存
		m_Asset.Nodes() = m_Nodes;
		m_Asset.SetBlackboardAssetPath(m_BoundBlackboardPath);
		if (m_BoundBlackboardPath.empty())
		{
			m_Asset.BlackboardKeys() = m_BlackboardKeys;
		}

		std::string error;
		if (!m_Asset.SaveToFile(physical.string(), &error))
		{
			m_StatusFeedback = "保存行为树失败: " + error;
			return false;
		}

		m_Dirty = false;
		m_StatusFeedback = "行为树保存成功 (.wbt): " + m_LogicalPath;
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

		if (m_Mode == EditorMode::EQSQuery)
		{
			RenderEQSQueryEditor(ctx, centerRect, host);
		}
		else if (m_ActiveTab == ViewTab::TreeOutline)
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

		if (m_Mode == EditorMode::Blackboard)
		{
			if (Wui::Button(ctx, Wui::HashId("bt.tb.new_bb"), { curX, curY, 66.0f, btnH }, "新建黑板", theme))
			{
				NewAsset();
			}
			curX += 72.0f;

			if (Wui::Button(ctx, Wui::HashId("bt.tb.open_bb"), { curX, curY, 66.0f, btnH }, "打开黑板", theme))
			{
				std::vector<std::string> bbs = Editor::AssetCatalog::PathsFor(EditorAssetKind::Blackboard);
				if (!bbs.empty())
					LoadAsset(bbs[0]);
			}
			curX += 72.0f;
		}
		else if (m_Mode == EditorMode::EQSQuery)
		{
			if (Wui::Button(ctx, Wui::HashId("bt.tb.new_eqs"), { curX, curY, 66.0f, btnH }, "新建EQS", theme))
			{
				NewAsset();
			}
			curX += 72.0f;

			if (Wui::Button(ctx, Wui::HashId("bt.tb.open_eqs"), { curX, curY, 66.0f, btnH }, "打开EQS", theme))
			{
				std::vector<std::string> eqss = Editor::AssetCatalog::PathsFor(EditorAssetKind::EQSQuery);
				if (!eqss.empty())
					LoadAsset(eqss[0]);
			}
			curX += 72.0f;
		}
		else
		{
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
		}

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

		if (m_Mode == EditorMode::Blackboard)
		{
			// 独立黑板模式徽章
			const Wui::WuiRect modeBadge { curX, curY, 200.0f, btnH };
			Wui::Badge(ctx, modeBadge, "BLACKBOARD ASSET (.WBB)", Wui::WuiColor { 0.12f, 0.35f, 0.45f, 0.4f }, Wui::WuiColor { 0.20f, 0.80f, 0.95f, 0.95f }, theme);
			curX += 208.0f;

			if (Wui::Button(ctx, Wui::HashId("bt.tb.switch_to_bt"), { curX, curY, 130.0f, btnH }, "[切换至行为树]", theme))
			{
				m_Mode = EditorMode::BehaviorTree;
				m_ActiveTab = ViewTab::TreeOutline;
				LoadAsset("assets/ai/SmartObjectDemo.wbt");
			}
			curX += 138.0f;
		}
		else if (m_Mode == EditorMode::EQSQuery)
		{
			const Wui::WuiRect modeBadge { curX, curY, 190.0f, btnH };
			Wui::Badge(ctx, modeBadge, "EQS QUERY ASSET (.WEQS)", Wui::WuiColor { 0.45f, 0.25f, 0.10f, 0.4f }, Wui::WuiColor { 0.95f, 0.65f, 0.20f, 0.95f }, theme);
			curX += 198.0f;

			if (Wui::Button(ctx, Wui::HashId("bt.tb.switch_to_bt2"), { curX, curY, 130.0f, btnH }, "[切换至行为树]", theme))
			{
				m_Mode = EditorMode::BehaviorTree;
				m_ActiveTab = ViewTab::TreeOutline;
				LoadAsset("assets/ai/SmartObjectDemo.wbt");
			}
			curX += 138.0f;
		}
		else
		{
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

			// 关联独立黑板资产快捷选择
			Wui::Label(ctx, { curX, curY + 4.0f }, "关联黑板 (.wbb):", theme.TextMuted, theme.FontSizeSmall);
			curX += 95.0f;
			std::vector<std::string> bbAssets = Editor::AssetCatalog::PathsFor(EditorAssetKind::Blackboard);
			if (bbAssets.empty())
				bbAssets.push_back("assets/ai/DefaultEnemyBlackboard.wbb");
			bbAssets.insert(bbAssets.begin(), "(无 / 内嵌保底黑板)");

			int currentBbIdx = 0;
			for (size_t b = 1; b < bbAssets.size(); ++b)
			{
				if (bbAssets[b] == m_BoundBlackboardPath) { currentBbIdx = static_cast<int>(b); break; }
			}

			if (Wui::Combo(ctx, Wui::HashId("bt.tb.bound_bb_combo"), { curX, curY, 180.0f, btnH }, "黑板资产", bbAssets, currentBbIdx, theme))
			{
				if (currentBbIdx == 0)
				{
					m_BoundBlackboardPath.clear();
					m_Dirty = true;
				}
				else if (currentBbIdx < static_cast<int>(bbAssets.size()))
				{
					m_BoundBlackboardPath = bbAssets[currentBbIdx];
					m_Dirty = true;
					// 立即从该独立黑板同步数据
					std::filesystem::path bbPhys = ResolvePhysicalPath(m_BoundBlackboardPath);
					m_BlackboardAsset.LoadFromFile(bbPhys.string());
				}
			}
			curX += 186.0f;
		}

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

		const float toolBtnW = (rect.W - 22.0f) / 3.0f;
		// 第一排：组合节点 (Selector, Sequence, Parallel)
		float toolY = rect.Y + 30.0f;
		float toolX = rect.X + 8.0f;

		Wui::WuiTheme selTheme = theme;
		selTheme.ButtonBg = NodeKindToColor(BTNodeKind::Selector);
		if (Wui::Button(ctx, Wui::HashId("bt.add_quick.sel"), { toolX, toolY, toolBtnW, 22.0f }, "+ 选择器 (Selector)", selTheme))
		{
			AddChildNode(m_SelectedNodeId != 0 ? m_SelectedNodeId : 1, BTNodeKind::Selector);
		}
		toolX += toolBtnW + 3.0f;

		Wui::WuiTheme seqTheme = theme;
		seqTheme.ButtonBg = NodeKindToColor(BTNodeKind::Sequence);
		if (Wui::Button(ctx, Wui::HashId("bt.add_quick.seq"), { toolX, toolY, toolBtnW, 22.0f }, "+ 顺序器 (Sequence)", seqTheme))
		{
			AddChildNode(m_SelectedNodeId != 0 ? m_SelectedNodeId : 1, BTNodeKind::Sequence);
		}
		toolX += toolBtnW + 3.0f;

		Wui::WuiTheme parTheme = theme;
		parTheme.ButtonBg = NodeKindToColor(BTNodeKind::Parallel);
		if (Wui::Button(ctx, Wui::HashId("bt.add_quick.par"), { toolX, toolY, toolBtnW, 22.0f }, "+ 并行器 (Parallel)", parTheme))
		{
			AddChildNode(m_SelectedNodeId != 0 ? m_SelectedNodeId : 1, BTNodeKind::Parallel);
		}

		// 第二排：控制与叶子节点 (Decorator, Condition, Action)
		toolY += 26.0f;
		toolX = rect.X + 8.0f;

		Wui::WuiTheme decTheme = theme;
		decTheme.ButtonBg = NodeKindToColor(BTNodeKind::Decorator);
		if (Wui::Button(ctx, Wui::HashId("bt.add_quick.dec"), { toolX, toolY, toolBtnW, 22.0f }, "+ 装饰器 (Decorator)", decTheme))
		{
			AddChildNode(m_SelectedNodeId != 0 ? m_SelectedNodeId : 1, BTNodeKind::Decorator);
		}
		toolX += toolBtnW + 3.0f;

		Wui::WuiTheme cndTheme = theme;
		cndTheme.ButtonBg = NodeKindToColor(BTNodeKind::Condition);
		if (Wui::Button(ctx, Wui::HashId("bt.add_quick.cnd"), { toolX, toolY, toolBtnW, 22.0f }, "+ 条件谓词 (Condition)", cndTheme))
		{
			AddChildNode(m_SelectedNodeId != 0 ? m_SelectedNodeId : 1, BTNodeKind::Condition);
		}
		toolX += toolBtnW + 3.0f;

		Wui::WuiTheme actTheme = theme;
		actTheme.ButtonBg = NodeKindToColor(BTNodeKind::Action);
		if (Wui::Button(ctx, Wui::HashId("bt.add_quick.act"), { toolX, toolY, toolBtnW, 22.0f }, "+ 执行动作 (Action)", actTheme))
		{
			AddChildNode(m_SelectedNodeId != 0 ? m_SelectedNodeId : 1, BTNodeKind::Action);
		}

		float listY = rect.Y + 84.0f;

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
				"RunEQSQuery",              // 执行空间决策查询 (Run EQS Query)
				"MoveToEQSPosition",        // 移动至战术点 (Move To EQS Position)
				"ExecuteEQSTacticalAction", // 一站式战术机动 (Execute EQS Tactical Action)
				"SetBlackboardValue",       // 写入黑板数值 (Set Blackboard Value)
				"IncrementBlackboardValue", // 累加黑板数值 (Increment Blackboard Value)
				"SetBlackboardTag",         // 写入黑板标签 (Set Blackboard Tag)
				"ResetTimer",               // 重置计时器 (Reset Timer)
				"ScriptAction",             // 执行自定义脚本动作 (Execute Luau Script Action)
				"Custom"                    // 自定义动作名 (Custom Named Action)
			};

			if (node->ActionType == "FindTacticalCover")
			{
				node->ActionType = "RunEQSQuery";
				m_Dirty = true;
			}
			const bool isEqsAction = (node->ActionType == "RunEQSQuery" ||
			                          node->ActionType == "MoveToEQSPosition" ||
			                          node->ActionType == "ExecuteEQSTacticalAction");
			const bool hasExtraRow = (node->ActionType == "SeekSmartObject" || node->ActionType == "Custom");
			const float cardH = isEqsAction
				? (g_EQSPreviewState.ConfigTab == 0 ? 385.0f : (365.0f + std::min(6, (int)g_EQSPreviewState.PipelineTests.size()) * 26.0f))
				: (hasExtraRow ? 175.0f : 125.0f);
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
			cardY += 26.0f;

			// 自适应动作参数配置
			if (isEqsAction)
			{
				const char* actRoleHint = "";
				if (node->ActionType == "RunEQSQuery")
					actRoleHint = "定位: 空间决策计算 (只算不走) —— 异步算好掩体点并写回黑板，AI保持原地";

				else if (node->ActionType == "MoveToEQSPosition")
					actRoleHint = "定位: 机动导航执行 (只走不算) —— 从黑板读取 TargetPos 驱动寻路机动";
				else if (node->ActionType == "ExecuteEQSTacticalAction")
					actRoleHint = "定位: 一站式复合动作 (边算边走) —— 异步算点并在完成后直接导航到达";

				if (actRoleHint[0] != '\\0')
				{
					Wui::Label(ctx, { startX + labelW, cardY }, actRoleHint, theme.Warning, theme.FontSizeSmall);
					cardY += 22.0f;
				}

				Wui::Label(ctx, { startX + 8.0f, cardY + 2.0f }, "架构约定:", theme.Accent, theme.FontSizeSmall);
				Wui::Label(ctx, { startX + labelW, cardY + 2.0f }, "由行为树通过异步 EnvQueryManager 全权调度，实体无需挂载 EQSComponent", theme.TextMuted, theme.FontSizeSmall);
				cardY += 24.0f;

				// 核心工作台模式: 0: 关联独立资产 (.weqs), 1: 空间决策准则 (Tactical Rules)
				const std::vector<std::string> eqsConfigTabs = {
					"关联独立资产 (.weqs)", "空间决策规则 (Tactical Rules)"
				};
				int tabIdx = std::clamp(static_cast<int>(g_EQSPreviewState.ConfigTab), 0, 1);
				Wui::Label(ctx, { startX + 8.0f, cardY + 2.0f }, "配置模式:", theme.Accent, theme.FontSizeSmall);
				if (Wui::Segmented(ctx, Wui::HashId((std::string("bt.act.eqs_tabs.") + std::to_string(node->Id)).c_str()),
					{ startX + labelW, cardY, inputW - 16.0f, 22.0f }, eqsConfigTabs, tabIdx, theme))
				{
					g_EQSPreviewState.ConfigTab = static_cast<uint8_t>(tabIdx);
				}
				cardY += 26.0f;

				if (g_EQSPreviewState.ConfigTab == 0) // 模式 0: 关联独立 .weqs 资产
				{
					std::vector<std::string> availableWeqs = Editor::AssetCatalog::PathsFor(EditorAssetKind::EQSQuery);
					if (availableWeqs.empty())
					{
						availableWeqs = {
							"assets/ai/DefaultTacticalCover.weqs",
							"assets/ai/DefaultFlank.weqs",
							"assets/ai/DefaultRetreat.weqs",
							"assets/ai/DefaultSniper.weqs"
						};
					}
					int currentWeqsIdx = -1;
					for (size_t wi = 0; wi < availableWeqs.size(); ++wi)
					{
						if (availableWeqs[wi] == node->StringParam)
						{
							currentWeqsIdx = static_cast<int>(wi);
							break;
						}
					}
					if (currentWeqsIdx < 0)
					{
						if (!node->StringParam.empty() && node->StringParam.rfind(".weqs") != std::string::npos)
						{
							availableWeqs.insert(availableWeqs.begin(), node->StringParam);
							currentWeqsIdx = 0;
						}
						else
						{
							currentWeqsIdx = 0;
							node->StringParam = availableWeqs[0];
						}
					}

					Wui::Label(ctx, { startX + 8.0f, cardY + 2.0f }, "资产选择 (.weqs):", theme.Accent, theme.FontSizeSmall);
					int chosenWeqs = currentWeqsIdx;
					const float pickComboW = inputW - 16.0f - 110.0f;
					if (Wui::Combo(ctx, Wui::HashId((std::string("bt.act.eqs_pick_combo.") + std::to_string(node->Id)).c_str()),
						{ startX + labelW, cardY, pickComboW, 22.0f }, "选择 .weqs 资产", availableWeqs, chosenWeqs, theme))
					{
						if (chosenWeqs >= 0 && chosenWeqs < static_cast<int>(availableWeqs.size()))
						{
							node->StringParam = availableWeqs[chosenWeqs];
							m_Dirty = true;
						}
					}
					std::string weqsPath = node->StringParam;

					if (Wui::Button(ctx, Wui::HashId((std::string("bt.act.btn_open_weqs_edit.") + std::to_string(node->Id)).c_str()),
						{ startX + labelW + pickComboW + 6.0f, cardY, 104.0f, 22.0f }, "[在工作台打开]", theme))
					{
						LoadAsset(weqsPath);
					}
					cardY += 28.0f;

					const float opBtnW = (inputW - 16.0f - 8.0f) * 0.5f;
					if (Wui::Button(ctx, Wui::HashId((std::string("bt.act.eqs_btn_export.") + std::to_string(node->Id)).c_str()),
						{ startX + labelW, cardY, opBtnW, 22.0f }, "导出当前为 .weqs", theme))
					{
						node->StringParam = weqsPath;
						std::filesystem::path phys = ResolvePhysicalPath(weqsPath);
						EQS::EQSAsset exportAsset;
						exportAsset.Name = m_AssetName + "_Query";
						exportAsset.GeneratorDesc.Radius = (node->FloatParam > 0.0f) ? node->FloatParam : 20.0f;
						exportAsset.GeneratorDesc.Spacing = 2.0f;
						exportAsset.GeneratorDesc.Type = (g_EQSPreviewState.GeneratorMode == 1) ? "Circle"
							: ((g_EQSPreviewState.GeneratorMode == 2) ? "AroundObstacles" : "ProjectedGrid");

						if (g_EQSPreviewState.PipelineTests.empty())
						{
							LoadEQSPreset(0, nullptr, g_EQSPreviewState);
						}
						for (const auto& t : g_EQSPreviewState.PipelineTests)
						{
							EQS::EQSTestDesc td;
							td.Name = t.Name;
							td.Weight = t.Weight;
							td.IdealValue = t.IdealValue;
							td.FilterMin = t.FilterMin;
							td.FilterMax = t.FilterMax;
							td.Curve = static_cast<EQS::ScoringCurve>(t.Curve);
							td.Purpose = static_cast<EQS::TestPurpose>(t.Purpose);
							td.Enabled = t.Enabled;
							const char* typeStrs[] = { "Distance", "Trace", "Dot", "WallProximity", "Height", "Overlap", "FriendlyPenalty", "CoverReservation", "Scripted" };
							if (t.Type < 9) td.TestType = typeStrs[t.Type];
							td.ContextType = (t.Context == 0) ? "Querier" : ((t.Context == 1) ? "Target" : "AllThreats");
							exportAsset.Tests.push_back(td);
						}
						exportAsset.SaveToFile(phys.string());
						m_Dirty = true;
						m_StatusFeedback = "已成功导出独立 .weqs 资产: " + weqsPath;
					}

					if (Wui::Button(ctx, Wui::HashId((std::string("bt.act.eqs_btn_load.") + std::to_string(node->Id)).c_str()),
						{ startX + labelW + opBtnW + 8.0f, cardY, opBtnW, 22.0f }, "从 .weqs 载入", theme))
					{
						node->StringParam = weqsPath;
						std::filesystem::path phys = ResolvePhysicalPath(weqsPath);
						EQS::EQSAsset importAsset;
						if (importAsset.LoadFromFile(phys.string()))
						{
							g_EQSPreviewState.PipelineTests.clear();
							for (const auto& td : importAsset.Tests)
							{
								EQSTestItem item;
								item.Name = td.Name;
								item.Weight = td.Weight;
								item.IdealValue = td.IdealValue;
								item.FilterMin = td.FilterMin;
								item.FilterMax = td.FilterMax;
								item.Curve = static_cast<uint8_t>(td.Curve);
								item.Purpose = static_cast<uint8_t>(td.Purpose);
								item.Enabled = td.Enabled;
								if (td.TestType == "Distance") item.Type = 0;
								else if (td.TestType == "Trace" || td.TestType == "TraceLOS") item.Type = 1;
								else if (td.TestType == "Dot" || td.TestType == "DotAngle") item.Type = 2;
								else if (td.TestType == "WallProximity") item.Type = 3;
								else if (td.TestType == "Height") item.Type = 4;
								else if (td.TestType == "Overlap") item.Type = 5;
								else if (td.TestType == "FriendlyPenalty") item.Type = 6;
								else if (td.TestType == "CoverReservation") item.Type = 7;
								else if (td.TestType == "Scripted") item.Type = 8;
								g_EQSPreviewState.PipelineTests.push_back(item);
							}
							g_EQSPreviewState.QueryRadius = importAsset.GeneratorDesc.Radius;
							node->FloatParam = importAsset.GeneratorDesc.Radius;
							m_StatusFeedback = "已成功从 .weqs 资产载入配置: " + weqsPath;
						}
					}
					cardY += 28.0f;
					Wui::Label(ctx, { startX + labelW, cardY }, "提示: 运行时直接读取 .weqs 资产，支持在多个 AI 行为树间共享相同空间决策", theme.TextMuted, theme.FontSizeSmall);
					cardY += 24.0f;
				}
				else // 模式 2: 流水线堆栈管理 (Pipeline Stack)
				{
					if (g_EQSPreviewState.PipelineTests.empty())
					{
						LoadEQSPreset(0, nullptr, g_EQSPreviewState);
					}

					Wui::Label(ctx, { startX + 8.0f, cardY + 2.0f }, "测试项栈 (Tests):", theme.Accent, theme.FontSizeSmall);
					Wui::Label(ctx, { startX + labelW, cardY + 2.0f }, "顺序即流水线执行顺序，先硬淘汰(Filter)后加权(Score)", theme.TextMuted, theme.FontSizeSmall);
					cardY += 24.0f;

					int toDeleteTest = -1;
					int toMoveUpTest = -1;
					for (size_t t = 0; t < g_EQSPreviewState.PipelineTests.size(); ++t)
					{
						auto& item = g_EQSPreviewState.PipelineTests[t];
						const float rowW = inputW - 16.0f;
						if (Wui::Checkbox(ctx, Wui::HashId((std::string("bt.eqs.test_en.") + std::to_string(t)).c_str()),
							{ startX + labelW, cardY, 18.0f, 20.0f }, "", item.Enabled, theme))
						{
							m_Dirty = true;
						}
						char itemLabel[96];
						std::snprintf(itemLabel, sizeof(itemLabel), "[%s] W:%.1f Ideal:%.1f",
							item.Name.c_str(), item.Weight, item.IdealValue);
						Wui::Label(ctx, { startX + labelW + 24.0f, cardY + 2.0f }, itemLabel,
							item.Enabled ? theme.Text : theme.TextMuted, theme.FontSizeSmall);

						if (t > 0)
						{
							if (Wui::Button(ctx, Wui::HashId((std::string("bt.eqs.t_up.") + std::to_string(t)).c_str()),
								{ startX + labelW + rowW - 60.0f, cardY, 26.0f, 20.0f }, "^", theme))
							{
								toMoveUpTest = static_cast<int>(t);
							}
						}
						if (Wui::Button(ctx, Wui::HashId((std::string("bt.eqs.t_del.") + std::to_string(t)).c_str()),
							{ startX + labelW + rowW - 28.0f, cardY, 26.0f, 20.0f }, "x", theme))
						{
							toDeleteTest = static_cast<int>(t);
						}

						cardY += 24.0f;
					}

					if (toMoveUpTest > 0)
					{
						std::swap(g_EQSPreviewState.PipelineTests[toMoveUpTest], g_EQSPreviewState.PipelineTests[toMoveUpTest - 1]);
						m_Dirty = true;
					}
					if (toDeleteTest >= 0 && toDeleteTest < static_cast<int>(g_EQSPreviewState.PipelineTests.size()))
					{
						g_EQSPreviewState.PipelineTests.erase(g_EQSPreviewState.PipelineTests.begin() + toDeleteTest);
						m_Dirty = true;
					}

					static const std::vector<std::string> kAddTestTypes = {
						"+ Distance (距离加权/范围过滤)",
						"+ Trace (视线射线遮蔽/开阔度)",
						"+ Dot (朝向夹角/侧翼角)",
						"+ WallProximity (掩体贴墙度)",
						"+ Height (高程制高点)",
						"+ Overlap (障碍物穿模硬淘汰)",
						"+ FriendlyPenalty (友军防扎堆排斥)",
						"+ CoverReservation (掩体防抢占排他预定)",
						"+ Scripted (Luau 脚本/自定义测试)"
					};
					int addChoice = -1;
					if (Wui::Combo(ctx, Wui::HashId((std::string("bt.eqs.add_test_combo.") + std::to_string(node->Id)).c_str()),
						{ startX + labelW, cardY, inputW - 16.0f, 22.0f }, "+ 添加测试项至流水线", kAddTestTypes, addChoice, theme))
					{
						if (addChoice >= 0 && addChoice < static_cast<int>(kAddTestTypes.size()))
						{
							EQSTestItem newItem;
							newItem.Type = static_cast<uint8_t>(addChoice);
							newItem.Enabled = true;
							const char* defaultNames[] = {
								"Distance", "Trace LOS", "Dot Angle", "Wall Proximity", "Height", "Overlap", "FriendlyPenalty", "CoverReservation", "Scripted"
							};
							newItem.Name = defaultNames[addChoice];
							if (addChoice == 5) { newItem.Purpose = 1; newItem.Weight = 0.0f; } // Overlap: FilterOnly
							else if (addChoice == 7) { newItem.Purpose = 1; newItem.IdealValue = 1.5f; } // Reservation: FilterOnly
							else if (addChoice == 6) { newItem.Weight = 1.2f; newItem.IdealValue = 3.0f; } // Friendly: Avoidance
							else { newItem.Purpose = 2; newItem.Weight = 1.0f; }
							g_EQSPreviewState.PipelineTests.push_back(newItem);
							m_Dirty = true;
						}
					}
					cardY += 26.0f;
				}

				if (g_EQSPreviewState.PipelineTests.empty())
				{
					LoadEQSPreset(0, nullptr, g_EQSPreviewState);
				}
				std::string testsSummary;
				for (size_t t = 0; t < g_EQSPreviewState.PipelineTests.size(); ++t)
				{
					if (t > 0) testsSummary += " + ";
					testsSummary += g_EQSPreviewState.PipelineTests[t].Name;
				}
				Wui::Label(ctx, { startX + 8.0f, cardY + 2.0f }, "流水线栈:", theme.Accent, theme.FontSizeSmall);
				Wui::Label(ctx, { startX + labelW, cardY + 2.0f }, testsSummary.c_str(), theme.TextMuted, theme.FontSizeSmall);
				cardY += 24.0f;

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
					{ startX + 8.0f, cardY, fieldW - 16.0f, 26.0f }, "模拟此节点空间推理 (Simulate Node EQS)", theme))
				{
					if (node->ActionType != "MoveToEQSPosition" && node->FloatParam > 0.0f)
					{
						g_EQSPreviewState.QueryRadius = node->FloatParam;
					}
					if (g_EQSPreviewState.PipelineTests.empty())
					{
						LoadEQSPreset(0, nullptr, g_EQSPreviewState);
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
		else if (node->Kind == BTNodeKind::Decorator)
		{
			const float cardH = 220.0f;
			const Wui::WuiRect decCard { startX, curY, fieldW, cardH };
			Wui::PanelBackground(ctx, decCard, Wui::WuiColor { 0.10f, 0.24f, 0.28f, 0.85f }, theme.Radius);
			Wui::HighlightOutline(ctx, decCard, Wui::WuiColor { 0.25f, 0.65f, 0.85f, 1.0f }, theme.Radius, 1.0f);

			float cardY = curY + 8.0f;
			Wui::Label(ctx, { startX + 8.0f, cardY }, "[黑板条件装饰器] (Blackboard Observer & Flow Aborts)", theme.Accent, theme.FontSizeSmall);
			cardY += 24.0f;

			// 1. 观察黑板键 KeyName
			Wui::Label(ctx, { startX + 8.0f, cardY + 2.0f }, "观察黑板键 (Key):", theme.Text, theme.FontSizeSmall);
			std::vector<std::string> bbKeyOptions;
			for (const auto& k : m_BlackboardKeys)
				bbKeyOptions.push_back(k.KeyName);
			if (bbKeyOptions.empty())
				bbKeyOptions = { "HasThreat", "TargetPos", "TargetEntity", "Value", "Tag" };

			int chosenKeyIdx = -1;
			for (size_t k = 0; k < bbKeyOptions.size(); ++k)
			{
				if (bbKeyOptions[k] == node->StringParam) { chosenKeyIdx = static_cast<int>(k); break; }
			}
			if (chosenKeyIdx < 0)
			{
				if (!node->StringParam.empty())
				{
					bbKeyOptions.insert(bbKeyOptions.begin(), node->StringParam);
					chosenKeyIdx = 0;
				}
				else
				{
					chosenKeyIdx = 0;
					node->StringParam = bbKeyOptions[0];
				}
			}

			if (Wui::Combo(ctx, Wui::HashId((std::string("bt.dec.key_combo.") + std::to_string(node->Id)).c_str()),
				{ startX + labelW, cardY, inputW - 16.0f, 22.0f }, "黑板键", bbKeyOptions, chosenKeyIdx, theme))
			{
				if (chosenKeyIdx >= 0 && chosenKeyIdx < static_cast<int>(bbKeyOptions.size()))
				{
					node->StringParam = bbKeyOptions[chosenKeyIdx];
					m_Dirty = true;
				}
			}
			cardY += 28.0f;

			// 2. 比较操作符 CompOp
			Wui::Label(ctx, { startX + 8.0f, cardY + 2.0f }, "比较条件 (Op):", theme.Text, theme.FontSizeSmall);
			const std::vector<std::string> opOptions = {
				"0: IsSet (有效/非零/非空)",
				"1: IsNotSet (无效/为零/为空)",
				"2: Equal (== 等于目标值)",
				"3: NotEqual (!= 不等于目标值)",
				"4: Greater (> 大于目标值)",
				"5: Less (< 小于目标值)"
			};
			int chosenOp = std::clamp(static_cast<int>(node->CompOp), 0, 5);
			if (Wui::Combo(ctx, Wui::HashId((std::string("bt.dec.op_combo.") + std::to_string(node->Id)).c_str()),
				{ startX + labelW, cardY, inputW - 16.0f, 22.0f }, "比较符", opOptions, chosenOp, theme))
			{
				node->CompOp = static_cast<Gameplay::ConditionOperator>(chosenOp);
				m_Dirty = true;
			}
			cardY += 28.0f;

			// 3. 目标比较数值 TargetValue (FloatParam)
			Wui::Label(ctx, { startX + 8.0f, cardY + 2.0f }, "目标值 (Target):", theme.Text, theme.FontSizeSmall);
			if (Wui::DragFloat(ctx, Wui::HashId((std::string("bt.dec.val_drag.") + std::to_string(node->Id)).c_str()),
				{ startX + labelW, cardY, inputW - 16.0f, 22.0f }, node->FloatParam, 1.0f, -99999.0f, 99999.0f, theme))
			{
				m_Dirty = true;
			}
			cardY += 28.0f;

			// 4. 响应式打断模式 AbortMode
			Wui::Label(ctx, { startX + 8.0f, cardY + 2.0f }, "打断模式 (Abort):", theme.Text, theme.FontSizeSmall);
			const std::vector<std::string> abortOptions = {
				"0: None (不主动打断)",
				"1: Self (条件失效打断自身运行中子树)",
				"2: LowerPriority (条件成立抢占打断低优先级分支)",
				"3: Both (同时具备 Self 与 LowerPriority 响应式打断)"
			};
			int chosenAbort = std::clamp(static_cast<int>(node->AbortMode), 0, 3);
			if (Wui::Combo(ctx, Wui::HashId((std::string("bt.dec.abort_combo.") + std::to_string(node->Id)).c_str()),
				{ startX + labelW, cardY, inputW - 16.0f, 22.0f }, "打断策略", abortOptions, chosenAbort, theme))
			{
				node->AbortMode = static_cast<Gameplay::FlowAbortMode>(chosenAbort);
				m_Dirty = true;
			}
			cardY += 26.0f;
			Wui::Label(ctx, { startX + labelW, cardY }, "提示: LowerPriority 用于当高优先级条件满足时立刻中止并抢占低优先级漫游/巡逻分支", theme.Warning, theme.FontSizeSmall);

			curY += cardH + 12.0f;
		}
		else if (node->Kind == BTNodeKind::Parallel)
		{
			const float cardH = 95.0f;
			const Wui::WuiRect parCard { startX, curY, fieldW, cardH };
			Wui::PanelBackground(ctx, parCard, Wui::WuiColor { 0.24f, 0.12f, 0.22f, 0.85f }, theme.Radius);
			Wui::HighlightOutline(ctx, parCard, Wui::WuiColor { 0.75f, 0.25f, 0.65f, 1.0f }, theme.Radius, 1.0f);

			float cardY = curY + 8.0f;
			Wui::Label(ctx, { startX + 8.0f, cardY }, "[并行执行器策略] (Parallel Policy)", theme.Accent, theme.FontSizeSmall);
			cardY += 26.0f;

			Wui::Label(ctx, { startX + 8.0f, cardY + 2.0f }, "成功条件 (Policy):", theme.Text, theme.FontSizeSmall);
			const std::vector<std::string> parPolicies = {
				"0: RequireOneSuccess (任一子节点成功即成功)",
				"1: RequireAllSuccess (全部子节点成功才算成功)"
			};
			int chosenPol = (node->FloatParam > 0.5f) ? 1 : 0;
			if (Wui::Combo(ctx, Wui::HashId((std::string("bt.par.pol_combo.") + std::to_string(node->Id)).c_str()),
				{ startX + labelW, cardY, inputW - 16.0f, 22.0f }, "完成策略", parPolicies, chosenPol, theme))
			{
				node->FloatParam = static_cast<float>(chosenPol);
				m_Dirty = true;
			}
			cardY += 26.0f;
			Wui::Label(ctx, { startX + labelW, cardY }, "提示: 并行执行器会同时 Tick 所有子节点，适用于边移动边观察或边攻击边播放语音", theme.TextMuted, theme.FontSizeSmall);

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
		curY += 36.0f;
	}

void BehaviorTreeEditorPanel::RenderBlackboard(Wui::WuiContext& ctx, const Wui::WuiRect& rect)
	{
		const Wui::WuiTheme& theme = Wui::CurrentTheme();
		Wui::PanelBackground(ctx, rect, theme.PanelBg, 0.0f);
		Wui::HighlightOutline(ctx, rect, theme.Border, 0.0f, 1.0f);

		const Wui::WuiRect headRect { rect.X, rect.Y, rect.W, 28.0f };
		Wui::PanelBackground(ctx, headRect, theme.CardBg, 0.0f);
		Wui::HighlightOutline(ctx, headRect, theme.Border, 0.0f, 1.0f);

		if (m_Mode == EditorMode::Blackboard)
		{
			Wui::Label(ctx, { rect.X + 8.0f, rect.Y + 6.0f }, ("[独立黑板资产] " + m_LogicalPath + " (独立数据契约，可供多个行为树共享)").c_str(), theme.Warning, theme.FontSizeSmall);
			if (Wui::Button(ctx, Wui::HashId("bt.bb.btn_create_bt"), { rect.X + rect.W - 190.0f, rect.Y + 3.0f, 180.0f, 22.0f }, "+ 创建使用此黑板的行为树", theme))
			{
				m_Mode = EditorMode::BehaviorTree;
				m_ActiveTab = ViewTab::TreeOutline;
				m_BoundBlackboardPath = m_LogicalPath;
				m_LogicalPath = "assets/ai/new_tree_using_" + m_AssetName + ".wbt";
				m_AssetName = "new_tree_using_" + m_AssetName;
				m_Dirty = true;
			}
		}
		else
		{
			if (!m_BoundBlackboardPath.empty())
			{
				Wui::Label(ctx, { rect.X + 8.0f, rect.Y + 6.0f }, ("[关联独立黑板资产: " + m_BoundBlackboardPath + "]").c_str(), theme.Success, theme.FontSizeSmall);
				if (Wui::Button(ctx, Wui::HashId("bt.bb.btn_open_indep"), { rect.X + rect.W - 190.0f, rect.Y + 3.0f, 180.0f, 22.0f }, "在黑板编辑器中独立打开", theme))
				{
					LoadAsset(m_BoundBlackboardPath);
				}
			}
			else
			{
				Wui::Label(ctx, { rect.X + 8.0f, rect.Y + 6.0f }, "行为树内嵌保底黑板 (Standalone) —— 建议导出为独立 .wbb 资产以复用", theme.TextMuted, theme.FontSizeSmall);
				if (Wui::Button(ctx, Wui::HashId("bt.bb.btn_export_wbb"), { rect.X + rect.W - 180.0f, rect.Y + 3.0f, 170.0f, 22.0f }, "导出为独立 .wbb 资产", theme))
				{
					std::string newWbbPath = "assets/ai/" + m_AssetName + "_Blackboard.wbb";
					std::filesystem::path phys = ResolvePhysicalPath(newWbbPath);
					Gameplay::BlackboardAsset exportAsset;
					for (const auto& k : m_BlackboardKeys)
					{
						Gameplay::BlackboardKeyDesc desc;
						desc.Name = k.KeyName;
						desc.Type = ParseKeyType(k.TypeName);
						desc.DefaultValue = ParseKeyValue(desc.Type, k.DefaultValue);
						exportAsset.AddKey(desc);
					}
					exportAsset.SaveToFile(phys.string());
					m_BoundBlackboardPath = newWbbPath;
					m_Dirty = true;
					m_StatusFeedback = "已成功导出独立黑板资产: " + newWbbPath;
				}
			}
		}

		float curX = rect.X + 16.0f;
		float curY = rect.Y + 36.0f;

		if (Wui::Button(ctx, Wui::HashId("bt.bb.add"), { curX, curY, 120.0f, 24.0f }, "+ 添加黑板键", theme))
		{
			m_BlackboardKeys.push_back({ "NewKey_" + std::to_string(m_BlackboardKeys.size()), "Float", "0.0" });
			m_Dirty = true;
		}


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
			"Int",
			"Bool",
			"String",
			"Vector3",
			"Vector2",
			"Vector4",
			"Entity",
			"Double",
			"ArrayVector3",
			"ArrayEntity",
			"ArrayFloat",
			"ArrayInt",
			"ArrayString"
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

	void BehaviorTreeEditorPanel::RenderEQSQueryEditor(Wui::WuiContext& ctx, const Wui::WuiRect& rect, PanelHost& host)
	{
		const Wui::WuiTheme& theme = Wui::CurrentTheme();
		Wui::PanelBackground(ctx, rect, theme.PanelBg, 0.0f);
		Wui::HighlightOutline(ctx, rect, theme.Border, 0.0f, 1.0f);

		// 顶部标题栏
		const Wui::WuiRect headRect { rect.X, rect.Y, rect.W, 28.0f };
		Wui::PanelBackground(ctx, headRect, theme.CardBg, 0.0f);
		Wui::HighlightOutline(ctx, headRect, theme.Border, 0.0f, 1.0f);

		Wui::Label(ctx, { rect.X + 8.0f, rect.Y + 6.0f }, ("[独立 EQS 空间决策资产] " + m_LogicalPath + " (可供多个行为树与智能体共享复用)").c_str(), theme.Warning, theme.FontSizeSmall);

		if (Wui::Button(ctx, Wui::HashId("bt.eqs.btn_create_bt"), { rect.X + rect.W - 200.0f, rect.Y + 3.0f, 190.0f, 22.0f }, "+ 创建使用此 EQS 的行为树", theme))
		{
			m_Mode = EditorMode::BehaviorTree;
			m_ActiveTab = ViewTab::TreeOutline;
			std::string newBtPath = "assets/ai/tree_using_" + m_AssetName + ".wbt";
			m_LogicalPath = newBtPath;
			m_AssetName = "tree_using_" + m_AssetName;
			SetDefaultTemplate();
			for (auto& n : m_Nodes)
			{
				if (n.Kind == BTNodeKind::Action)
				{
					n.ActionType = "RunEQSQuery";
					n.StringParam = m_LogicalPath;
					break;
				}
			}
			m_Dirty = true;
		}

		// 双栏工业级布局计算
		const bool isWide = rect.W >= 860.0f;
		const float leftW = isWide ? std::floor(rect.W * 0.56f) : (rect.W - 32.0f);
		const float rightW = isWide ? (rect.W - leftW - 36.0f) : (rect.W - 32.0f);
		const float leftStartX = rect.X + 16.0f;
		const float rightStartX = isWide ? (rect.X + leftW + 24.0f) : leftStartX;

		float curY = rect.Y + 34.0f;
		const float labelW = 110.0f;

		// =========================================================================
		// 左栏: 空间采样生成器与战术决策规则管线 (Pipeline Authoring Workspace)
		// =========================================================================


		// 1. 空间采样生成器卡片 (Spatial Item Generator)
		const Wui::WuiRect genCard { leftStartX, curY, leftW, 115.0f };
		Wui::PanelBackground(ctx, genCard, Wui::WuiColor { 0.12f, 0.18f, 0.24f, 0.85f }, theme.Radius);
		Wui::HighlightOutline(ctx, genCard, Wui::WuiColor { 0.20f, 0.65f, 0.85f, 0.95f }, theme.Radius, 1.0f);

		float gy = curY + 6.0f;
		Wui::Label(ctx, { leftStartX + 8.0f, gy }, "[1. 空间采样生成器 (Spatial Item Generator)]", theme.Accent, theme.FontSizeSmall);
		gy += 22.0f;

		const std::vector<std::string> genOptions = {
			"贴地网格 (Projected Grid)",
			"同心环带 (Concentric Circle)",
			"掩体边缘 (Around Obstacles)",
			"扇形视锥 (Directional Cone)"
		};
		int genIdx = std::clamp(static_cast<int>(g_EQSPreviewState.GeneratorMode), 0, 3);
		Wui::Label(ctx, { leftStartX + 8.0f, gy + 2.0f }, "生成模式 (Type):", theme.Text, theme.FontSizeSmall);
		if (Wui::Combo(ctx, Wui::HashId("bt.eqs.gen_mode_combo"), { leftStartX + labelW, gy, leftW - labelW - 12.0f, 22.0f }, "", genOptions, genIdx, theme))
		{
			g_EQSPreviewState.GeneratorMode = static_cast<uint8_t>(genIdx);
			m_Dirty = true;
		}
		gy += 26.0f;

		const float halfW = (leftW - labelW - 20.0f) * 0.5f;
		Wui::Label(ctx, { leftStartX + 8.0f, gy + 2.0f }, "搜索半径/间距:", theme.Text, theme.FontSizeSmall);
		if (Wui::DragFloat(ctx, Wui::HashId("bt.eqs.gen_radius"), { leftStartX + labelW, gy, halfW, 22.0f }, g_EQSPreviewState.QueryRadius, 0.5f, 1.0f, 100.0f, theme))
		{
			m_Dirty = true;
		}
		Wui::Tooltip(ctx, { leftStartX + labelW, gy, halfW, 22.0f }, "空间查询半径 (米)");

		if (Wui::DragFloat(ctx, Wui::HashId("bt.eqs.gen_spacing"), { leftStartX + labelW + halfW + 8.0f, gy, halfW, 22.0f }, g_EQSPreviewState.GridSpacing, 0.1f, 0.2f, 10.0f, theme))
		{
			m_Dirty = true;
		}
		Wui::Tooltip(ctx, { leftStartX + labelW + halfW + 8.0f, gy, halfW, 22.0f }, "采样点间距 (米)");
		gy += 26.0f;

		if (genIdx == 3) // Cone
		{
			Wui::Label(ctx, { leftStartX + 8.0f, gy + 2.0f }, "视锥扇区夹角:", theme.TextMuted, theme.FontSizeSmall);
			if (Wui::DragFloat(ctx, Wui::HashId("bt.eqs.gen_cone_ang"), { leftStartX + labelW, gy, halfW, 22.0f }, g_EQSPreviewState.ConeAngle, 1.0f, 10.0f, 180.0f, theme))
			{
				m_Dirty = true;
			}
			Wui::Tooltip(ctx, { leftStartX + labelW, gy, halfW, 22.0f }, "扇形视锥夹角 (度)");
		}
		else if (genIdx == 2) // AroundObstacles
		{
			Wui::Label(ctx, { leftStartX + 8.0f, gy + 2.0f }, "掩体贴墙距离:", theme.TextMuted, theme.FontSizeSmall);
			if (Wui::DragFloat(ctx, Wui::HashId("bt.eqs.gen_wall_off"), { leftStartX + labelW, gy, halfW, 22.0f }, g_EQSPreviewState.WallOffset, 0.05f, 0.1f, 5.0f, theme))
			{
				m_Dirty = true;
			}
			Wui::Tooltip(ctx, { leftStartX + labelW, gy, halfW, 22.0f }, "掩体表面外扩贴靠距离 (米)");
		}
		else
		{
			Wui::Label(ctx, { leftStartX + 8.0f, gy + 2.0f }, "地形贴合吸附:", theme.TextMuted, theme.FontSizeSmall);
			Wui::Label(ctx, { leftStartX + labelW, gy + 2.0f }, "自动吸附到当前关卡物理碰撞与高度地形", theme.Success, theme.FontSizeSmall);
		}

		curY += 125.0f;

		// 2. 战术评估规则库 (Tactical Criteria Stack)
		if (g_EQSPreviewState.PipelineTests.empty())
		{
			LoadEQSPreset(0, nullptr, g_EQSPreviewState);
		}

		const size_t numRules = g_EQSPreviewState.PipelineTests.size();
		const float stackCardH = 40.0f + std::min(7, (int)numRules) * 26.0f + 32.0f;
		const Wui::WuiRect stackCard { leftStartX, curY, leftW, stackCardH };
		Wui::PanelBackground(ctx, stackCard, Wui::WuiColor { 0.22f, 0.16f, 0.10f, 0.85f }, theme.Radius);
		Wui::HighlightOutline(ctx, stackCard, Wui::WuiColor { 0.85f, 0.55f, 0.15f, 1.0f }, theme.Radius, 1.0f);

		float sy = curY + 6.0f;
		char stackTitle[128];
		std::snprintf(stackTitle, sizeof(stackTitle), "[2. 战术评估规则库 (Tactical Criteria)] - 活跃规则: %zu 条", numRules);
		Wui::Label(ctx, { leftStartX + 8.0f, sy }, stackTitle, theme.Warning, theme.FontSizeSmall);
		sy += 24.0f;

		int toMoveUp = -1;
		int toDelete = -1;
		for (size_t t = 0; t < g_EQSPreviewState.PipelineTests.size(); ++t)
		{
			auto& item = g_EQSPreviewState.PipelineTests[t];
			const bool isSelected = (m_SelectedRuleIdx == static_cast<int>(t));

			// 启用勾选框
			Wui::Checkbox(ctx, Wui::HashId((std::string("bt.eqs.st_en.") + std::to_string(t)).c_str()),
				{ leftStartX + 8.0f, sy + 1.0f, 18.0f, 20.0f }, "", item.Enabled, theme);

			// 规则名选择按钮 (点击选中在下方详细编辑)
			const float selBtnW = 150.0f;
			Wui::WuiTheme itemBtnTheme = theme;
			if (isSelected)
			{
				itemBtnTheme.ButtonBg = Wui::WuiColor { 0.35f, 0.45f, 0.70f, 0.95f };
				itemBtnTheme.Border = Wui::WuiColor { 0.55f, 0.75f, 0.95f, 1.0f };
			}
			else
			{
				itemBtnTheme.ButtonBg = Wui::WuiColor { 0.18f, 0.22f, 0.26f, 0.85f };
			}
			std::string btnText = "#" + std::to_string(t + 1) + " " + item.Name;
			if (Wui::Button(ctx, Wui::HashId((std::string("bt.eqs.st_sel.") + std::to_string(t)).c_str()),
				{ leftStartX + 28.0f, sy, selBtnW, 22.0f }, btnText.c_str(), itemBtnTheme))
			{
				m_SelectedRuleIdx = static_cast<int>(t);
			}

			// 模式简述
			const char* purpBadge = (item.Purpose == 1) ? "[硬淘汰]" : ((item.Purpose == 2) ? "[淘汰+打分]" : "[仅加权]");
			Wui::Label(ctx, { leftStartX + 28.0f + selBtnW + 6.0f, sy + 2.0f }, purpBadge,
				(item.Purpose == 1) ? theme.Danger : ((item.Purpose == 2) ? theme.Warning : theme.Success), theme.FontSizeSmall);

			// 权重滑块
			float xCursor = leftStartX + 28.0f + selBtnW + 82.0f;
			Wui::Label(ctx, { xCursor, sy + 2.0f }, "权重:", theme.TextMuted, theme.FontSizeSmall);
			xCursor += 32.0f;
			if (Wui::DragFloat(ctx, Wui::HashId((std::string("bt.eqs.st_wt.") + std::to_string(t)).c_str()),
				{ xCursor, sy, 55.0f, 22.0f }, item.Weight, 0.05f, -10.0f, 10.0f, theme))
			{
				m_Dirty = true;
			}
			xCursor += 60.0f;

			// 理想值滑块
			Wui::Label(ctx, { xCursor, sy + 2.0f }, "理想:", theme.TextMuted, theme.FontSizeSmall);
			xCursor += 32.0f;
			if (Wui::DragFloat(ctx, Wui::HashId((std::string("bt.eqs.st_idl.") + std::to_string(t)).c_str()),
				{ xCursor, sy, 55.0f, 22.0f }, item.IdealValue, 0.5f, 0.0f, 180.0f, theme))
			{
				m_Dirty = true;
			}

			// 调序与删除按钮
			if (t > 0)
			{
				if (Wui::Button(ctx, Wui::HashId((std::string("bt.eqs.st_up.") + std::to_string(t)).c_str()),
					{ leftStartX + leftW - 56.0f, sy, 24.0f, 22.0f }, "^", theme))
				{
					toMoveUp = static_cast<int>(t);
				}
			}
			if (Wui::Button(ctx, Wui::HashId((std::string("bt.eqs.st_del.") + std::to_string(t)).c_str()),
				{ leftStartX + leftW - 28.0f, sy, 24.0f, 22.0f }, "x", theme))
			{
				toDelete = static_cast<int>(t);
			}

			sy += 26.0f;
		}

		if (toMoveUp > 0)
		{
			std::swap(g_EQSPreviewState.PipelineTests[toMoveUp], g_EQSPreviewState.PipelineTests[toMoveUp - 1]);
			m_SelectedRuleIdx = toMoveUp - 1;
			m_Dirty = true;
		}
		if (toDelete >= 0 && toDelete < static_cast<int>(g_EQSPreviewState.PipelineTests.size()))
		{
			g_EQSPreviewState.PipelineTests.erase(g_EQSPreviewState.PipelineTests.begin() + toDelete);
			if (m_SelectedRuleIdx >= static_cast<int>(g_EQSPreviewState.PipelineTests.size()))
				m_SelectedRuleIdx = static_cast<int>(g_EQSPreviewState.PipelineTests.size()) - 1;
			m_Dirty = true;
		}

		// 追加规则下拉菜单
		static const std::vector<std::string> kAddRuleOptions = {
			"+ 视线射击死角遮蔽 (Line of Sight Occlusion)",
			"+ 战术交战距离 (Combat Distance)",
			"+ 侧翼包抄夹角 (Flank Attack Angle)",
			"+ 掩体紧贴贴墙度 (Wall Proximity)",
			"+ 制高点高程优势 (Elevation Advantage)",
			"+ 障碍物穿模硬排除 (Obstacle Overlap Guard)",
			"+ 友军防扎堆间距排斥 (Friendly Spread Avoidance)",
			"+ 掩体独占预定抢占保护 (Cover Reservation Guard)",
			"+ 路径连通与寻路代价 (Pathfinding & Reachability)",
			"+ 敌军视野锥体规避 (Field of View Guard)",
			"+ 自定义脚本规则 (Custom Scripted Rule)"
		};
		int addRuleIdx = -1;
		if (Wui::Combo(ctx, Wui::HashId("bt.eqs.add_rule_combo"), { leftStartX + 8.0f, sy, leftW - 16.0f, 22.0f }, "+ 添加战术评估准则 (Add Tactical Criterion)", kAddRuleOptions, addRuleIdx, theme))
		{
			if (addRuleIdx >= 0 && addRuleIdx < static_cast<int>(kAddRuleOptions.size()))
			{
				EQSTestItem newItem;
				newItem.Enabled = true;
				const char* defaultNames[] = {
					"Trace LOS", "Distance", "Dot Angle", "Wall Proximity", "Height", "Overlap Guard", "Friendly Spread", "Cover Reservation", "Pathfinding", "Field of View", "Custom Script"
				};
				newItem.Name = defaultNames[addRuleIdx];
				if (addRuleIdx == 0) { newItem.Type = 1; newItem.Purpose = 2; newItem.Weight = 1.8f; newItem.IdealValue = 1.0f; newItem.Context = 1; }
				else if (addRuleIdx == 1) { newItem.Type = 0; newItem.Purpose = 0; newItem.Weight = 1.0f; newItem.IdealValue = 10.0f; newItem.Curve = 1; }
				else if (addRuleIdx == 2) { newItem.Type = 2; newItem.Purpose = 0; newItem.Weight = 2.0f; newItem.IdealValue = 90.0f; newItem.Curve = 2; newItem.Context = 1; }
				else if (addRuleIdx == 3) { newItem.Type = 3; newItem.Purpose = 0; newItem.Weight = 1.2f; newItem.IdealValue = 0.85f; newItem.Curve = 2; }
				else if (addRuleIdx == 4) { newItem.Type = 4; newItem.Purpose = 0; newItem.Weight = 0.8f; newItem.IdealValue = 5.0f; }
				else if (addRuleIdx == 5) { newItem.Type = 5; newItem.Purpose = 1; newItem.Weight = 0.0f; }
				else if (addRuleIdx == 6) { newItem.Type = 6; newItem.Purpose = 0; newItem.Weight = 1.2f; newItem.IdealValue = 3.0f; newItem.Context = 3; }
				else if (addRuleIdx == 7) { newItem.Type = 7; newItem.Purpose = 1; newItem.Weight = 0.0f; newItem.IdealValue = 1.5f; }
				else if (addRuleIdx == 8) { newItem.Type = 9; newItem.Purpose = 2; newItem.Weight = 1.0f; newItem.IdealValue = 20.0f; newItem.FilterMax = 50.0f; }
				else if (addRuleIdx == 9) { newItem.Type = 10; newItem.Purpose = 0; newItem.Weight = 1.5f; newItem.IdealValue = 90.0f; newItem.Context = 1; }
				else if (addRuleIdx == 10) { newItem.Type = 8; newItem.Purpose = 2; newItem.Weight = 1.0f; newItem.ScriptPath = "assets/scripts/ai/eqs/CustomRule.luau"; }

				g_EQSPreviewState.PipelineTests.push_back(newItem);
				m_SelectedRuleIdx = static_cast<int>(g_EQSPreviewState.PipelineTests.size()) - 1;
				m_Dirty = true;
			}
		}
		curY += stackCardH + 10.0f;

		// 3. 选中规则详细属性检查器卡片 (Selected Rule Details Inspector)
		if (m_SelectedRuleIdx >= 0 && m_SelectedRuleIdx < static_cast<int>(g_EQSPreviewState.PipelineTests.size()))
		{
			auto& curRule = g_EQSPreviewState.PipelineTests[m_SelectedRuleIdx];
			const float ruleCardH = 140.0f;
			const Wui::WuiRect ruleCard { leftStartX, curY, leftW, ruleCardH };
			Wui::PanelBackground(ctx, ruleCard, Wui::WuiColor { 0.16f, 0.22f, 0.28f, 0.85f }, theme.Radius);
			Wui::HighlightOutline(ctx, ruleCard, Wui::WuiColor { 0.40f, 0.70f, 0.95f, 1.0f }, theme.Radius, 1.0f);

			float ry = curY + 6.0f;
			char ruleHead[96];
			std::snprintf(ruleHead, sizeof(ruleHead), "[3. 选中规则详细属性: #%d %s]", m_SelectedRuleIdx + 1, curRule.Name.c_str());
			Wui::Label(ctx, { leftStartX + 8.0f, ry }, ruleHead, theme.Accent, theme.FontSizeSmall);
			ry += 22.0f;

			// Row 1: 规则名称与评估目标
			const float colHalfW = (leftW - labelW * 2.0f - 20.0f) * 0.5f;
			Wui::Label(ctx, { leftStartX + 8.0f, ry + 2.0f }, "规则名称:", theme.Text, theme.FontSizeSmall);
			if (Wui::TextField(ctx, Wui::HashId("bt.eqs.rule_name"), { leftStartX + labelW, ry, colHalfW, 22.0f }, curRule.Name, theme))
			{
				m_Dirty = true;
			}

			const float xCol2 = leftStartX + labelW + colHalfW + 12.0f;
			Wui::Label(ctx, { xCol2, ry + 2.0f }, "相对目标 (Context):", theme.Text, theme.FontSizeSmall);
			const std::vector<std::string> contextOptions = {
				"自身 (Querier)", "敌方威胁 (Threat)", "全体敌军 (AllThreats)", "全体友军 (AllAllies)"
			};
			int ctxIdx = std::clamp(static_cast<int>(curRule.Context), 0, 3);
			if (Wui::Combo(ctx, Wui::HashId("bt.eqs.rule_ctx_combo"), { xCol2 + labelW, ry, colHalfW, 22.0f }, "", contextOptions, ctxIdx, theme))
			{
				curRule.Context = static_cast<uint8_t>(ctxIdx);
				m_Dirty = true;
			}
			ry += 26.0f;

			// Row 2: 作用模式与评分曲线
			Wui::Label(ctx, { leftStartX + 8.0f, ry + 2.0f }, "作用模式 (Purpose):", theme.Text, theme.FontSizeSmall);
			const std::vector<std::string> purposeOptions = {
				"仅加权 (Score)", "硬淘汰 (Filter)", "淘汰+加权 (Filter&Score)"
			};
			int purpIdx = std::clamp(static_cast<int>(curRule.Purpose), 0, 2);
			if (Wui::Segmented(ctx, Wui::HashId("bt.eqs.rule_purp_seg"), { leftStartX + labelW, ry, colHalfW, 22.0f }, purposeOptions, purpIdx, theme))
			{
				curRule.Purpose = static_cast<uint8_t>(purpIdx);
				m_Dirty = true;
			}

			Wui::Label(ctx, { xCol2, ry + 2.0f }, "评分曲线 (Curve):", theme.Text, theme.FontSizeSmall);
			const std::vector<std::string> curveOptions = {
				"线性递增 (Linear)", "反比衰减 (Inverse)", "高斯钟形 (Gaussian)", "阶跃阈值 (Step)"
			};
			int curveIdx = std::clamp(static_cast<int>(curRule.Curve), 0, 3);
			if (Wui::Combo(ctx, Wui::HashId("bt.eqs.rule_curve_combo"), { xCol2 + labelW, ry, colHalfW, 22.0f }, "", curveOptions, curveIdx, theme))
			{
				curRule.Curve = static_cast<uint8_t>(curveIdx);
				m_Dirty = true;
			}
			ry += 26.0f;

			// Row 3: 权重与理想中心值
			const float triW = (leftW - labelW - 28.0f) / 3.0f;
			Wui::Label(ctx, { leftStartX + 8.0f, ry + 2.0f }, "数值与参数:", theme.Text, theme.FontSizeSmall);
			if (Wui::DragFloat(ctx, Wui::HashId("bt.eqs.rule_det_wt"), { leftStartX + labelW, ry, triW, 22.0f }, curRule.Weight, 0.05f, -10.0f, 10.0f, theme))
			{
				m_Dirty = true;
			}
			Wui::Tooltip(ctx, { leftStartX + labelW, ry, triW, 22.0f }, "评分权重 (Weight)");

			if (Wui::DragFloat(ctx, Wui::HashId("bt.eqs.rule_det_ideal"), { leftStartX + labelW + triW + 6.0f, ry, triW, 22.0f }, curRule.IdealValue, 0.5f, 0.0f, 180.0f, theme))
			{
				m_Dirty = true;
			}
			Wui::Tooltip(ctx, { leftStartX + labelW + triW + 6.0f, ry, triW, 22.0f }, "最佳理想中心值 (Ideal Value)");

			if (Wui::DragFloat(ctx, Wui::HashId("bt.eqs.rule_det_sigma"), { leftStartX + labelW + (triW + 6.0f) * 2.0f, ry, triW, 22.0f }, curRule.GaussianSigma, 0.1f, 0.1f, 50.0f, theme))
			{
				m_Dirty = true;
			}
			Wui::Tooltip(ctx, { leftStartX + labelW + (triW + 6.0f) * 2.0f, ry, triW, 22.0f }, "高斯钟形曲线方差宽度 (Gaussian Sigma)");
			ry += 26.0f;

			// Row 4: 硬淘汰过滤范围 [Min, Max]
			const float rangeHalfW = (leftW - labelW - 16.0f) * 0.5f;
			Wui::Label(ctx, { leftStartX + 8.0f, ry + 2.0f }, "淘汰区间 [Min,Max]:", theme.TextMuted, theme.FontSizeSmall);
			if (Wui::DragFloat(ctx, Wui::HashId("bt.eqs.rule_fmin"), { leftStartX + labelW, ry, rangeHalfW - 6.0f, 22.0f }, curRule.FilterMin, 0.1f, -1000.0f, 1000.0f, theme))
			{
				m_Dirty = true;
			}
			if (Wui::DragFloat(ctx, Wui::HashId("bt.eqs.rule_fmax"), { leftStartX + labelW + rangeHalfW, ry, rangeHalfW - 6.0f, 22.0f }, curRule.FilterMax, 0.1f, -1000.0f, 1000.0f, theme))
			{
				m_Dirty = true;
			}
			Wui::Tooltip(ctx, { leftStartX + labelW, ry, leftW - labelW, 22.0f }, "硬淘汰区间：超出 [Min, Max] 的候选点直接被剔除淘汰");
		}

		// =========================================================================
		// 右栏: 实时空间推理诊断与候选点分析器 (Live 3D Diagnostics & Breakdown)
		// =========================================================================
		float rightY = isWide ? (rect.Y + 34.0f) : (curY + 16.0f);

		// 1. 实时空间决策推理控制台 (Live Simulation Control)
		Wui::WuiTheme simTheme = theme;
		simTheme.ButtonBg = Wui::WuiColor { 0.16f, 0.45f, 0.35f, 0.95f };
		simTheme.Border = Wui::WuiColor { 0.25f, 0.85f, 0.55f, 1.0f };
		if (Wui::Button(ctx, Wui::HashId("bt.eqs.btn_sim_editor"),
			{ rightStartX, rightY, rightW, 30.0f }, "⚡ 运行空间决策推理并在视口投影 (Run Simulation)", simTheme))
		{
			Scene* scenePtr = host.GetActiveScene().get();
			Entity simEnt = host.GetSelectedEntity();
			EQSComponent* compPtr = (simEnt.IsValid() && simEnt.HasComponent<EQSComponent>())
				? &simEnt.GetComponent<EQSComponent>() : nullptr;

			ExecuteEQSPipeline(scenePtr, simEnt, compPtr, g_EQSPreviewState);
			m_StatusFeedback = g_EQSPreviewState.LastStatus;
			host.MarkDocumentDirty();
		}
		rightY += 36.0f;

		// 2. 指标看板卡片 (Metrics Overview Board)
		const Wui::WuiRect metricsCard { rightStartX, rightY, rightW, 76.0f };
		Wui::PanelBackground(ctx, metricsCard, Wui::WuiColor { 0.08f, 0.14f, 0.18f, 0.95f }, 4.0f);
		Wui::HighlightOutline(ctx, metricsCard, Wui::WuiColor { 0.20f, 0.85f, 0.95f, 0.65f }, 4.0f, 1.0f);

		float my = rightY + 6.0f;
		Wui::Label(ctx, { rightStartX + 8.0f, my }, "[空间推理诊断看板 (Diagnostics Telemetry)]", theme.Accent, theme.FontSizeSmall);
		my += 20.0f;

		char mLine1[160], mLine2[160];
		if (g_EQSPreviewState.Evaluated)
		{
			std::snprintf(mLine1, sizeof(mLine1), "总采样: %zu 点 | 合格: %zu 点 | 硬淘汰: %zu 点 (耗时 %.2f ms)",
				g_EQSPreviewState.TotalSampleCount, g_EQSPreviewState.QualifiedCount,
				g_EQSPreviewState.HardFilteredCount, g_EQSPreviewState.ExecutionTimeMs);
			std::snprintf(mLine2, sizeof(mLine2), "最高战术得分: %.2f 分 | 最优站位: (%.1f, %.1f, %.1f)",
				g_EQSPreviewState.BestScore,
				g_EQSPreviewState.BestCoverPos.x, g_EQSPreviewState.BestCoverPos.y, g_EQSPreviewState.BestCoverPos.z);
		}
		else
		{
			std::snprintf(mLine1, sizeof(mLine1), "状态: 待推理 (点击上方绿色按钮在 3D 视口投射热力图)");
			std::snprintf(mLine2, sizeof(mLine2), "支持在视口中点击任意候选点，实时查看各项规则得分拆解");
		}
		Wui::Label(ctx, { rightStartX + 8.0f, my }, mLine1, g_EQSPreviewState.Evaluated ? theme.Text : theme.TextMuted, theme.FontSizeSmall);
		my += 20.0f;
		Wui::Label(ctx, { rightStartX + 8.0f, my }, mLine2, g_EQSPreviewState.Evaluated ? theme.Success : theme.TextMuted, theme.FontSizeSmall);
		rightY += 86.0f;

		// 3. Top 候选站位排名清单 (Top Ranked Positions Table)
		const float tableH = 155.0f;
		const Wui::WuiRect tableCard { rightStartX, rightY, rightW, tableH };
		Wui::PanelBackground(ctx, tableCard, Wui::WuiColor { 0.10f, 0.14f, 0.18f, 0.85f }, theme.Radius);
		Wui::HighlightOutline(ctx, tableCard, Wui::WuiColor { 0.25f, 0.45f, 0.65f, 0.95f }, theme.Radius, 1.0f);

		float ty = rightY + 6.0f;
		Wui::Label(ctx, { rightStartX + 8.0f, ty }, "[Top 候选站位排名清单 (Top Ranked Positions)]", theme.Accent, theme.FontSizeSmall);
		ty += 22.0f;

		if (g_EQSPreviewState.Candidates.empty())
		{
			Wui::Label(ctx, { rightStartX + 8.0f, ty + 10.0f }, "暂无候选点数据，请先执行推理", theme.TextMuted, theme.FontSizeSmall);
		}
		else
		{
			// 提取合格点中得分最高的几个
			std::vector<std::pair<size_t, float>> validCandidates;
			for (size_t ci = 0; ci < g_EQSPreviewState.Candidates.size(); ++ci)
			{
				if (!g_EQSPreviewState.Candidates[ci].FilteredOut)
				{
					validCandidates.push_back({ ci, g_EQSPreviewState.Candidates[ci].TotalScore });
				}
			}
			std::sort(validCandidates.begin(), validCandidates.end(), [](const auto& a, const auto& b) {
				return a.second > b.second;
			});

			const size_t showCount = std::min((size_t)5, validCandidates.size());
			for (size_t vi = 0; vi < showCount; ++vi)
			{
				const size_t candIdx = validCandidates[vi].first;
				const auto& cand = g_EQSPreviewState.Candidates[candIdx];
				const bool isSelected = (g_EQSPreviewState.SelectedCandidateIdx == static_cast<int>(candIdx));

				Wui::WuiTheme rowTheme = theme;
				if (vi == 0)
				{
					rowTheme.ButtonBg = isSelected ? Wui::WuiColor { 0.60f, 0.45f, 0.15f, 0.95f } : Wui::WuiColor { 0.35f, 0.28f, 0.10f, 0.85f };
				}
				else
				{
					rowTheme.ButtonBg = isSelected ? Wui::WuiColor { 0.25f, 0.35f, 0.50f, 0.95f } : Wui::WuiColor { 0.14f, 0.18f, 0.22f, 0.75f };
				}

				char rowLabel[128];
				std::snprintf(rowLabel, sizeof(rowLabel), "#%zu %s (%.1f, %.1f, %.1f) - 得分: %.2f",
					vi + 1, (vi == 0 ? "[★ 最优]" : "[备选]"), cand.Position.x, cand.Position.y, cand.Position.z, cand.TotalScore);

				if (Wui::Button(ctx, Wui::HashId((std::string("bt.eqs.top_c_") + std::to_string(vi)).c_str()),
					{ rightStartX + 8.0f, ty, rightW - 16.0f, 22.0f }, rowLabel, rowTheme))
				{
					g_EQSPreviewState.SelectedCandidateIdx = static_cast<int>(candIdx);
				}
				ty += 24.0f;
			}
		}
		rightY += tableH + 10.0f;

		// 4. 选中候选点单项得分雷达拆解 (Score Breakdown Inspector)
		const float breakdownH = 145.0f;
		const Wui::WuiRect breakCard { rightStartX, rightY, rightW, breakdownH };
		Wui::PanelBackground(ctx, breakCard, Wui::WuiColor { 0.12f, 0.16f, 0.20f, 0.85f }, theme.Radius);
		Wui::HighlightOutline(ctx, breakCard, Wui::WuiColor { 0.35f, 0.65f, 0.45f, 0.95f }, theme.Radius, 1.0f);

		float by = rightY + 6.0f;
		Wui::Label(ctx, { rightStartX + 8.0f, by }, "[各准则单项得分拆解 (Score Breakdown Inspector)]", theme.Success, theme.FontSizeSmall);
		by += 22.0f;

		int activeInspectIdx = g_EQSPreviewState.SelectedCandidateIdx;
		if (activeInspectIdx < 0 && !g_EQSPreviewState.Candidates.empty())
		{
			// 默认展示最高得分点
			for (size_t ci = 0; ci < g_EQSPreviewState.Candidates.size(); ++ci)
			{
				if (!g_EQSPreviewState.Candidates[ci].FilteredOut)
				{
					activeInspectIdx = static_cast<int>(ci);
					break;
				}
			}
		}

		if (activeInspectIdx >= 0 && activeInspectIdx < static_cast<int>(g_EQSPreviewState.Candidates.size()))
		{
			const auto& inspectCand = g_EQSPreviewState.Candidates[activeInspectIdx];
			char ptTitle[96];
			std::snprintf(ptTitle, sizeof(ptTitle), "站位 (%.1f, %.1f, %.1f) - 综合得分: %.2f",
				inspectCand.Position.x, inspectCand.Position.y, inspectCand.Position.z, inspectCand.TotalScore);
			Wui::Label(ctx, { rightStartX + 8.0f, by }, ptTitle, theme.Accent, theme.FontSizeSmall);
			by += 20.0f;

			for (size_t ri = 0; ri < g_EQSPreviewState.PipelineTests.size(); ++ri)
			{
				const auto& rule = g_EQSPreviewState.PipelineTests[ri];
				float scoreVal = (ri < inspectCand.TestScores.size()) ? inspectCand.TestScores[ri] : 0.0f;
				float rawVal = (ri < inspectCand.TestRawValues.size()) ? inspectCand.TestRawValues[ri] : 0.0f;

				char ruleBreakdown[128];
				std::snprintf(ruleBreakdown, sizeof(ruleBreakdown), "• %s: +%.2f 分 (读数: %.1f)",
					rule.Name.c_str(), scoreVal, rawVal);
				Wui::Label(ctx, { rightStartX + 12.0f, by }, ruleBreakdown, theme.Text, theme.FontSizeSmall);
				by += 18.0f;
			}
		}
		else
		{
			Wui::Label(ctx, { rightStartX + 8.0f, by + 10.0f }, "请先运行空间决策推理，以检视站位得分拆解", theme.TextMuted, theme.FontSizeSmall);
		}
	}

}
