#include "wldpch.h"
#include "World/Gameplay/Framework/BehaviorTreeAsset.h"
#include "World/Gameplay/Framework/SmartObjectSystem.h"
#include "World/Gameplay/Framework/DamageSystem.h"
#include "World/Scene/Scene.h"
#include "World/Scene/Entity.h"
#include "World/Scene/Components.h"
#include "World/Utils/Paths.h"

#include <yaml-cpp/yaml.h>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <cmath>
#include <unordered_set>

namespace World::Gameplay
{
	namespace
	{
		std::string KindToString(BehaviorTreeNodeKind kind)
		{
			switch (kind)
			{
			case BehaviorTreeNodeKind::Selector: return "Selector";
			case BehaviorTreeNodeKind::Sequence: return "Sequence";
			case BehaviorTreeNodeKind::Condition: return "Condition";
			case BehaviorTreeNodeKind::Action: return "Action";
			default: return "Selector";
			}
		}

		BehaviorTreeNodeKind StringToKind(const std::string& str)
		{
			if (str == "Sequence") return BehaviorTreeNodeKind::Sequence;
			if (str == "Condition") return BehaviorTreeNodeKind::Condition;
			if (str == "Action") return BehaviorTreeNodeKind::Action;
			return BehaviorTreeNodeKind::Selector;
		}

		std::filesystem::path ResolveBtAssetPath(const std::string& path)
		{
			std::error_code ec;
			std::filesystem::path p(path);
			if (std::filesystem::exists(p, ec) && std::filesystem::is_regular_file(p, ec))
				return std::filesystem::absolute(p);

			const auto& assetRoot = World::Paths::AssetRoot();
			if (!assetRoot.empty())
			{
				std::filesystem::path fromAssetRoot = assetRoot / p;
				if (std::filesystem::exists(fromAssetRoot, ec) && std::filesystem::is_regular_file(fromAssetRoot, ec))
					return std::filesystem::absolute(fromAssetRoot);
			}

			std::string s = p.generic_string();
			if (s.rfind("assets/", 0) == 0)
			{
				std::string sub = s.substr(7);
				std::filesystem::path subPath = assetRoot / sub;
				if (std::filesystem::exists(subPath, ec) && std::filesystem::is_regular_file(subPath, ec))
					return std::filesystem::absolute(subPath);
			}

			return p;
		}

		std::shared_ptr<BehaviorNode> CreateConditionNode(const BehaviorTreeNodeDesc& desc)
		{
			std::string type = desc.ConditionType;
			float param = desc.FloatParam;

			if (type == "HealthBelow")
			{
				return std::make_shared<ConditionNode>([param](BehaviorContext& ctx) {
					if (!ctx.Self.HasComponent<HealthComponent>())
						return false;
					float hp = ctx.Self.GetComponent<HealthComponent>().CurrentHealth;
					float threshold = param > 0.0f ? param : 60.0f;
					return hp < threshold;
				});
			}
			else if (type == "HealthAbove")
			{
				return std::make_shared<ConditionNode>([param](BehaviorContext& ctx) {
					if (!ctx.Self.HasComponent<HealthComponent>())
						return false;
					float hp = ctx.Self.GetComponent<HealthComponent>().CurrentHealth;
					return hp > param;
				});
			}
			else if (type == "HasTarget")
			{
				return std::make_shared<ConditionNode>([](BehaviorContext& ctx) {
					return ctx.Component.TargetEntity != 0 && ctx.Component.TargetEntity != static_cast<uint32_t>(entt::null);
				});
			}
			else if (type == "TimerElapsed" || type == "TimerExceeded")
			{
				return std::make_shared<ConditionNode>([param](BehaviorContext& ctx) {
					return ctx.Component.ExecutionTimer >= param;
				});
			}
			else if (type == "BlackboardTagEquals")
			{
				uint32_t tag = static_cast<uint32_t>(param);
				return std::make_shared<ConditionNode>([tag](BehaviorContext& ctx) {
					return ctx.Component.BlackboardTag == tag;
				});
			}
			else if (type == "BlackboardValueAbove" || type == "BlackboardValueGreater")
			{
				return std::make_shared<ConditionNode>([param](BehaviorContext& ctx) {
					return ctx.Component.BlackboardValue > param;
				});
			}
			else if (type == "BlackboardValueBelow" || type == "BlackboardValueLess")
			{
				return std::make_shared<ConditionNode>([param](BehaviorContext& ctx) {
					return ctx.Component.BlackboardValue < param;
				});
			}
			else if (type == "AlwaysFalse")
			{
				return std::make_shared<ConditionNode>([](BehaviorContext&) { return false; });
			}
			else
			{
				return std::make_shared<ConditionNode>([](BehaviorContext&) { return true; });
			}
		}

		std::shared_ptr<BehaviorNode> CreateActionNode(const BehaviorTreeNodeDesc& desc)
		{
			std::string type = desc.ActionType;
			float param = desc.FloatParam;

			if (type == "SeekSmartObject")
			{
				return std::make_shared<ActionNode>([param](BehaviorContext& ctx) {
					if (ctx.Component.StateMachineState == 1 /* Seek */ || ctx.Component.StateMachineState == 2 /* Use */)
						return NodeStatus::Success;

					if (!ctx.Self.HasComponent<TransformComponent>())
						return NodeStatus::Failure;

					auto& tr = ctx.Self.GetComponent<TransformComponent>();
					float radius = param > 0.0f ? param : 50.0f;
					entt::entity targetSo = SmartObjectSystem::FindNearestAvailableSlot(ctx.CurrentScene, tr.Location, 1, radius);
					if (targetSo == entt::null) {
						targetSo = SmartObjectSystem::FindNearestAvailableSlot(ctx.CurrentScene, tr.Location, 0, radius);
					}
					if (targetSo == entt::null)
						return NodeStatus::Failure;

					bool claimed = SmartObjectSystem::ClaimSlot(ctx.CurrentScene, targetSo, (entt::entity)ctx.Self);
					if (claimed)
					{
						ctx.Component.StateMachineState = 1; // 1 = Seek
						ctx.Component.TargetEntity = static_cast<uint32_t>(targetSo);
						ctx.Component.BlackboardTargetPos = SmartObjectSystem::GetSlotWorldPosition(ctx.CurrentScene, targetSo);
						ctx.Component.ExecutionTimer = 0.0f;
						return NodeStatus::Success;
					}
					return NodeStatus::Failure;
				});
			}
			else if (type == "UseSmartObject")
			{
				return std::make_shared<ActionNode>([param](BehaviorContext& ctx) {
					if (ctx.Component.StateMachineState == 0 || static_cast<entt::entity>(ctx.Component.TargetEntity) == entt::null)
						return NodeStatus::Failure;

					entt::entity targetSo = static_cast<entt::entity>(ctx.Component.TargetEntity);
					if (!ctx.Self.HasComponent<TransformComponent>() || !ctx.Self.HasComponent<HealthComponent>())
						return NodeStatus::Failure;

					auto& tr = ctx.Self.GetComponent<TransformComponent>();
					auto& health = ctx.Self.GetComponent<HealthComponent>();

					glm::vec3 diff = ctx.Component.BlackboardTargetPos - tr.Location;
					float distSq = diff.x * diff.x + diff.z * diff.z;
					if (distSq > 0.16f) // 距离大于 0.4m 时向插槽移动
					{
						ctx.Component.StateMachineState = 1; // 1 = Seek
						float dist = std::sqrt(distSq);
						float speed = 3.0f;
						float dt = ctx.Dt > 0.0f ? ctx.Dt : 0.016f;
						float step = speed * dt;
						if (step > dist) step = dist;
						tr.Location.x += (diff.x / dist) * step;
						tr.Location.z += (diff.z / dist) * step;
						return NodeStatus::Running;
					}

					// 到达插槽位置，开始使用
					if (ctx.Component.StateMachineState != 2 /* Use */)
					{
						bool used = SmartObjectSystem::UseSlot(ctx.CurrentScene, targetSo, (entt::entity)ctx.Self);
						if (used)
						{
							ctx.Component.StateMachineState = 2; // 2 = Use
							ctx.Component.ExecutionTimer = 0.0f;
						}
						else
						{
							ctx.Component.StateMachineState = 0;
							ctx.Component.TargetEntity = static_cast<uint32_t>(entt::null);
							return NodeStatus::Failure;
						}
					}

					ctx.Component.ExecutionTimer += (ctx.Dt > 0.0f ? ctx.Dt : 0.016f);
					if (ctx.Component.ExecutionTimer < 1.0f)
					{
						return NodeStatus::Running;
					}

					// 使用完成: 治疗回血，释放插槽
					float healAmount = param > 0.0f ? param : 60.0f;
					DamageSystem::Heal(health, healAmount);
					SmartObjectSystem::ReleaseSlot(ctx.CurrentScene, targetSo);
					ctx.Component.StateMachineState = 3; // 3 = CooldownExit
					ctx.Component.BlackboardValue += 1.0f;
					ctx.Component.TargetEntity = static_cast<uint32_t>(entt::null);
					ctx.Component.ExecutionTimer = 0.0f;
					return NodeStatus::Success;
				});
			}
			else if (type == "Patrol")
			{
				return std::make_shared<ActionNode>([param](BehaviorContext& ctx) {
					ctx.Component.StateMachineState = 0; // 0 = Patrol
					if (ctx.Self.HasComponent<TransformComponent>())
					{
						auto& tr = ctx.Self.GetComponent<TransformComponent>();
						float dir = (ctx.Component.BlackboardTag == 1) ? -1.0f : 1.0f;
						float speed = param > 0.0f ? param : 2.0f;
						float dt = ctx.Dt > 0.0f ? ctx.Dt : 0.016f;
						tr.Location.x += dir * speed * dt;

						if (tr.Location.x >= 3.0f)
						{
							tr.Location.x = 3.0f;
							ctx.Component.BlackboardTag = 1;
						}
						else if (tr.Location.x <= -3.0f)
						{
							tr.Location.x = -3.0f;
							ctx.Component.BlackboardTag = 0;
						}
					}
					return NodeStatus::Success;
				});
			}
			else if (type == "Wait")
			{
				return std::make_shared<ActionNode>([param](BehaviorContext& ctx) {
					ctx.Component.ExecutionTimer += (ctx.Dt > 0.0f ? ctx.Dt : 0.016f);
					float duration = param > 0.0f ? param : 1.0f;
					if (ctx.Component.ExecutionTimer < duration)
					{
						return NodeStatus::Running;
					}
					ctx.Component.ExecutionTimer = 0.0f;
					return NodeStatus::Success;
				});
			}
			else if (type == "SetBlackboardValue")
			{
				return std::make_shared<ActionNode>([param](BehaviorContext& ctx) {
					ctx.Component.BlackboardValue = param;
					return NodeStatus::Success;
				});
			}
			else if (type == "IncrementBlackboardValue")
			{
				return std::make_shared<ActionNode>([param](BehaviorContext& ctx) {
					ctx.Component.BlackboardValue += (param != 0.0f ? param : 1.0f);
					return NodeStatus::Success;
				});
			}
			else if (type == "ResetTimer")
			{
				return std::make_shared<ActionNode>([](BehaviorContext& ctx) {
					ctx.Component.ExecutionTimer = 0.0f;
					return NodeStatus::Success;
				});
			}
			else
			{
				return std::make_shared<ActionNode>([](BehaviorContext&) {
					return NodeStatus::Success;
				});
			}
		}

		std::shared_ptr<BehaviorNode> BuildSubtree(uint32_t nodeId, const std::vector<BehaviorTreeNodeDesc>& nodes, std::unordered_set<uint32_t>& visited)
		{
			if (visited.count(nodeId) > 0)
				return nullptr;
			visited.insert(nodeId);

			const BehaviorTreeNodeDesc* pDesc = nullptr;
			for (const auto& n : nodes)
			{
				if (n.Id == nodeId)
				{
					pDesc = &n;
					break;
				}
			}
			if (!pDesc)
				return nullptr;

			const auto& desc = *pDesc;
			switch (desc.Kind)
			{
			case BehaviorTreeNodeKind::Selector:
			{
				auto selector = std::make_shared<SelectorNode>();
				for (const auto& childDesc : nodes)
				{
					if (childDesc.ParentId == nodeId)
					{
						auto childNode = BuildSubtree(childDesc.Id, nodes, visited);
						if (childNode)
							selector->AddChild(childNode);
					}
				}
				return selector;
			}
			case BehaviorTreeNodeKind::Sequence:
			{
				auto sequence = std::make_shared<SequenceNode>();
				for (const auto& childDesc : nodes)
				{
					if (childDesc.ParentId == nodeId)
					{
						auto childNode = BuildSubtree(childDesc.Id, nodes, visited);
						if (childNode)
							sequence->AddChild(childNode);
					}
				}
				return sequence;
			}
			case BehaviorTreeNodeKind::Condition:
			{
				return CreateConditionNode(desc);
			}
			case BehaviorTreeNodeKind::Action:
			{
				return CreateActionNode(desc);
			}
			default:
				return nullptr;
			}
		}
	}

	void BehaviorTreeAsset::Clear()
	{
		m_Nodes.clear();
		m_BlackboardKeys.clear();
	}

	void BehaviorTreeAsset::SetDefaultTemplate()
	{
		Clear();

		m_BlackboardKeys = {
			{ "TargetPos", "Vector3", "0,0,0" },
			{ "ExecutionTimer", "Float", "0.0" },
			{ "TargetEntity", "Entity", "0" },
			{ "PatrolSpeed", "Float", "2.0" }
		};

		BehaviorTreeNodeDesc root;
		root.Id = 1;
		root.ParentId = 0;
		root.Kind = BehaviorTreeNodeKind::Selector;
		root.Title = "Root Selector";
		root.Description = "Root selector evaluating heal and patrol branches";

		BehaviorTreeNodeDesc healSeq;
		healSeq.Id = 2;
		healSeq.ParentId = 1;
		healSeq.Kind = BehaviorTreeNodeKind::Sequence;
		healSeq.Title = "Emergency Heal";
		healSeq.Description = "Seek and use health smart object when low HP";

		BehaviorTreeNodeDesc lowHpCond;
		lowHpCond.Id = 3;
		lowHpCond.ParentId = 2;
		lowHpCond.Kind = BehaviorTreeNodeKind::Condition;
		lowHpCond.Title = "Low Health Check";
		lowHpCond.Description = "Check if CurrentHealth < 60";
		lowHpCond.ConditionType = "HealthBelow";
		lowHpCond.FloatParam = 60.0f;

		BehaviorTreeNodeDesc seekAction;
		seekAction.Id = 4;
		seekAction.ParentId = 2;
		seekAction.Kind = BehaviorTreeNodeKind::Action;
		seekAction.Title = "Seek Smart Object";
		seekAction.Description = "Find and claim nearest medical station";
		seekAction.ActionType = "SeekSmartObject";
		seekAction.FloatParam = 50.0f;

		BehaviorTreeNodeDesc useAction;
		useAction.Id = 5;
		useAction.ParentId = 2;
		useAction.Kind = BehaviorTreeNodeKind::Action;
		useAction.Title = "Use Smart Object";
		useAction.Description = "Approach and use medical station to heal";
		useAction.ActionType = "UseSmartObject";
		useAction.FloatParam = 60.0f;

		BehaviorTreeNodeDesc patrolAction;
		patrolAction.Id = 6;
		patrolAction.ParentId = 1;
		patrolAction.Kind = BehaviorTreeNodeKind::Action;
		patrolAction.Title = "Patrol Area";
		patrolAction.Description = "Standard patrol roaming along X-axis";
		patrolAction.ActionType = "Patrol";
		patrolAction.FloatParam = 2.0f;

		m_Nodes.push_back(std::move(root));
		m_Nodes.push_back(std::move(healSeq));
		m_Nodes.push_back(std::move(lowHpCond));
		m_Nodes.push_back(std::move(seekAction));
		m_Nodes.push_back(std::move(useAction));
		m_Nodes.push_back(std::move(patrolAction));
	}

	bool BehaviorTreeAsset::LoadFromFile(const std::string& path, std::string* error)
	{
		try
		{
			std::filesystem::path resolvedPath = ResolveBtAssetPath(path);
			std::error_code ec;
			if (!std::filesystem::exists(resolvedPath, ec) || !std::filesystem::is_regular_file(resolvedPath, ec))
			{
				if (error)
					*error = "File does not exist: " + path;
				return false;
			}

			YAML::Node root = YAML::LoadFile(resolvedPath.string());
			if (!root.IsDefined() || !root.IsMap())
			{
				if (error)
					*error = "Invalid YAML root in: " + path;
				return false;
			}

			Clear();

			if (root["Blackboard"] && root["Blackboard"].IsSequence())
			{
				for (const auto& bbNode : root["Blackboard"])
				{
					BehaviorBlackboardKeyDesc keyDesc;
					if (bbNode["KeyName"]) keyDesc.KeyName = bbNode["KeyName"].as<std::string>();
					if (bbNode["TypeName"]) keyDesc.TypeName = bbNode["TypeName"].as<std::string>();
					if (bbNode["DefaultValue"])
					{
						try {
							keyDesc.DefaultValue = bbNode["DefaultValue"].as<std::string>();
						} catch (...) {
							std::stringstream ss;
							ss << bbNode["DefaultValue"];
							keyDesc.DefaultValue = ss.str();
						}
					}
					m_BlackboardKeys.push_back(std::move(keyDesc));
				}
			}

			if (root["Nodes"] && root["Nodes"].IsSequence())
			{
				for (const auto& nNode : root["Nodes"])
				{
					BehaviorTreeNodeDesc nodeDesc;
					if (nNode["Id"]) nodeDesc.Id = nNode["Id"].as<uint32_t>();
					if (nNode["ParentId"]) nodeDesc.ParentId = nNode["ParentId"].as<uint32_t>();
					if (nNode["Kind"]) nodeDesc.Kind = StringToKind(nNode["Kind"].as<std::string>());
					if (nNode["Title"]) nodeDesc.Title = nNode["Title"].as<std::string>();
					if (nNode["Description"]) nodeDesc.Description = nNode["Description"].as<std::string>();
					if (nNode["ConditionType"]) nodeDesc.ConditionType = nNode["ConditionType"].as<std::string>();
					if (nNode["ActionType"]) nodeDesc.ActionType = nNode["ActionType"].as<std::string>();
					if (nNode["FloatParam"]) nodeDesc.FloatParam = nNode["FloatParam"].as<float>();
					if (nNode["StringParam"]) nodeDesc.StringParam = nNode["StringParam"].as<std::string>();
					m_Nodes.push_back(std::move(nodeDesc));
				}
			}

			return true;
		}
		catch (const std::exception& e)
		{
			if (error)
				*error = e.what();
			return false;
		}
	}

	bool BehaviorTreeAsset::SaveToFile(const std::string& path, std::string* error) const
	{
		try
		{
			std::filesystem::path fsPath(path);
			if (fsPath.has_parent_path())
			{
				std::error_code ec;
				std::filesystem::create_directories(fsPath.parent_path(), ec);
			}

			YAML::Emitter out;
			out << YAML::BeginMap;
			out << YAML::Key << "FormatVersion" << YAML::Value << 1;
			out << YAML::Key << "AssetType" << YAML::Value << "BehaviorTree";

			out << YAML::Key << "Blackboard" << YAML::Value << YAML::BeginSeq;
			for (const auto& key : m_BlackboardKeys)
			{
				out << YAML::BeginMap;
				out << YAML::Key << "KeyName" << YAML::Value << key.KeyName;
				out << YAML::Key << "TypeName" << YAML::Value << key.TypeName;
				out << YAML::Key << "DefaultValue" << YAML::Value << key.DefaultValue;
				out << YAML::EndMap;
			}
			out << YAML::EndSeq;

			out << YAML::Key << "Nodes" << YAML::Value << YAML::BeginSeq;
			for (const auto& node : m_Nodes)
			{
				out << YAML::BeginMap;
				out << YAML::Key << "Id" << YAML::Value << node.Id;
				out << YAML::Key << "ParentId" << YAML::Value << node.ParentId;
				out << YAML::Key << "Kind" << YAML::Value << KindToString(node.Kind);
				out << YAML::Key << "Title" << YAML::Value << node.Title;
				out << YAML::Key << "Description" << YAML::Value << node.Description;
				out << YAML::Key << "ConditionType" << YAML::Value << node.ConditionType;
				out << YAML::Key << "ActionType" << YAML::Value << node.ActionType;
				out << YAML::Key << "FloatParam" << YAML::Value << node.FloatParam;
				out << YAML::Key << "StringParam" << YAML::Value << node.StringParam;
				out << YAML::EndMap;
			}
			out << YAML::EndSeq;

			out << YAML::EndMap;

			std::ofstream fout(path);
			if (!fout.is_open())
			{
				if (error)
					*error = "Failed to open file for writing: " + path;
				return false;
			}
			fout << out.c_str() << "\n";
			return true;
		}
		catch (const std::exception& e)
		{
			if (error)
				*error = e.what();
			return false;
		}
	}

	std::shared_ptr<BehaviorNode> BehaviorTreeAsset::InstantiateTree() const
	{
		if (m_Nodes.empty())
			return nullptr;

		// 寻找根节点 (ParentId == 0)
		uint32_t rootId = m_Nodes.front().Id;
		for (const auto& node : m_Nodes)
		{
			if (node.ParentId == 0)
			{
				rootId = node.Id;
				break;
			}
		}

		std::unordered_set<uint32_t> visited;
		return BuildSubtree(rootId, m_Nodes, visited);
	}
}
