#include "wldpch.h"
#include "World/Gameplay/Framework/BehaviorTreeAsset.h"
#include "World/Gameplay/Framework/SmartObjectSystem.h"
#include "World/Gameplay/Framework/DamageSystem.h"
#include "World/Level/AI/EQSCoverSystem.h"
#include "World/NavMesh/NavMesh.h"
#include "World/Scene/Scene.h"
#include "World/Scene/Entity.h"
#include "World/Scene/Components.h"
#include "World/Script/Runtime/ScriptEngine.h"
#include "World/Utils/Paths.h"
#include "World/Core/Log.h"

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
			case BehaviorTreeNodeKind::Decorator: return "Decorator";
			case BehaviorTreeNodeKind::Parallel: return "Parallel";
			default: return "Selector";
			}
		}

		BehaviorTreeNodeKind StringToKind(const std::string& str)
		{
			if (str == "Sequence") return BehaviorTreeNodeKind::Sequence;
			if (str == "Condition") return BehaviorTreeNodeKind::Condition;
			if (str == "Action") return BehaviorTreeNodeKind::Action;
			if (str == "Decorator") return BehaviorTreeNodeKind::Decorator;
			if (str == "Parallel") return BehaviorTreeNodeKind::Parallel;
			return BehaviorTreeNodeKind::Selector;
		}

		std::string AbortModeToString(FlowAbortMode mode)
		{
			switch (mode)
			{
			case FlowAbortMode::None: return "None";
			case FlowAbortMode::Self: return "Self";
			case FlowAbortMode::LowerPriority: return "LowerPriority";
			case FlowAbortMode::Both: return "Both";
			default: return "None";
			}
		}

		FlowAbortMode StringToAbortMode(const std::string& str)
		{
			if (str == "Self") return FlowAbortMode::Self;
			if (str == "LowerPriority") return FlowAbortMode::LowerPriority;
			if (str == "Both") return FlowAbortMode::Both;
			return FlowAbortMode::None;
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
					if (ctx.Board.HasKey("TargetEntity"))
					{
						uint32_t ent = ctx.Board.Get<uint32_t>("TargetEntity", 0);
						if (ent != 0 && ent != static_cast<uint32_t>(entt::null))
							return true;
					}
					return ctx.Component.TargetEntity != 0 && ctx.Component.TargetEntity != static_cast<uint32_t>(entt::null);
				});
			}
			else if (type == "TargetInRange" || type == "IsEnemyInRange")
			{
				return std::make_shared<ConditionNode>([param](BehaviorContext& ctx) {
					if (!ctx.Self.HasComponent<TransformComponent>())
						return false;

					const auto& tr = ctx.Self.GetComponent<TransformComponent>();
					glm::vec3 selfPos = tr.Location;
					glm::vec3 targetPos = ctx.Board.Get<glm::vec3>("TargetPos", ctx.Component.BlackboardTargetPos);

					uint32_t targetId = ctx.Board.Get<uint32_t>("TargetEntity", ctx.Component.TargetEntity);
					if (targetId != 0 && targetId != static_cast<uint32_t>(entt::null))
					{
						Entity targetEntity(&ctx.CurrentScene, static_cast<entt::entity>(targetId));
						if (targetEntity.IsValid() && targetEntity.HasComponent<TransformComponent>())
						{
							targetPos = targetEntity.GetComponent<TransformComponent>().Location;
						}
					}

					float range = param > 0.0f ? param : 5.0f;
					float dist = glm::length(targetPos - selfPos);
					return dist <= range;
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
					uint32_t actualTag = ctx.Board.Get<uint32_t>("Tag", ctx.Component.BlackboardTag);
					return actualTag == tag;
				});
			}
			else if (type == "BlackboardValueAbove" || type == "BlackboardValueGreater")
			{
				return std::make_shared<ConditionNode>([param](BehaviorContext& ctx) {
					float actualVal = ctx.Board.Get<float>("Value", ctx.Component.BlackboardValue);
					return actualVal > param;
				});
			}
			else if (type == "BlackboardValueBelow" || type == "BlackboardValueLess")
			{
				return std::make_shared<ConditionNode>([param](BehaviorContext& ctx) {
					float actualVal = ctx.Board.Get<float>("Value", ctx.Component.BlackboardValue);
					return actualVal < param;
				});
			}
			else if (type == "ScriptCondition")
			{
				std::string scriptPath = desc.StringParam;
				return std::make_shared<ConditionNode>([scriptPath](BehaviorContext&) {
					if (scriptPath.empty())
						return true;
					if (World::ScriptEngine::IsInitialized())
					{
						std::string valStr;
						if (World::ScriptEngine::ReadScriptValue(scriptPath, "Condition", valStr))
							return valStr == "true";
					}
					return true;
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
					if (ctx.Component.StateMachineState == 1 || ctx.Component.StateMachineState == 2)
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
						ctx.Component.StateMachineState = 1;
						ctx.Component.TargetEntity = static_cast<uint32_t>(targetSo);
						glm::vec3 slotPos = SmartObjectSystem::GetSlotWorldPosition(ctx.CurrentScene, targetSo);
						ctx.Component.BlackboardTargetPos = slotPos;
						ctx.Component.ExecutionTimer = 0.0f;

						ctx.Board.Set("TargetEntity", ctx.Component.TargetEntity);
						ctx.Board.Set("TargetPos", slotPos);
						return NodeStatus::Success;
					}
					return NodeStatus::Failure;
				});
			}
else if (type == "UseSmartObject")
			{
				return std::make_shared<ActionNode>([param](BehaviorContext& ctx) {
					uint32_t targetRaw = ctx.Board.Get<uint32_t>("TargetEntity", ctx.Component.TargetEntity);
					if (ctx.Component.StateMachineState == 0 || targetRaw == static_cast<uint32_t>(entt::null))
						return NodeStatus::Failure;

					entt::entity targetSo = static_cast<entt::entity>(targetRaw);
					if (!ctx.Self.HasComponent<TransformComponent>() || !ctx.Self.HasComponent<HealthComponent>())
						return NodeStatus::Failure;

					auto& tr = ctx.Self.GetComponent<TransformComponent>();
					auto& health = ctx.Self.GetComponent<HealthComponent>();

					glm::vec3 slotPos = ctx.Board.Get<glm::vec3>("TargetPos", ctx.Component.BlackboardTargetPos);
					glm::vec3 diff = slotPos - tr.Location;
					float distSq = diff.x * diff.x + diff.z * diff.z;

					// 距离大于 0.4m 时向插槽移动（走 NavMesh 优先，无则平滑趋近）
					if (distSq > 0.16f)
					{
						ctx.Component.StateMachineState = 1; // Seek
						if (ctx.Self.HasComponent<NavMeshAgentComponent>())
						{
							auto& agent = ctx.Self.GetComponent<NavMeshAgentComponent>();
							agent.Destination = slotPos;
							agent.Speed = 3.0f;
							agent.AutoRepath = 1;
						}
						else
						{
							float dist = std::sqrt(distSq);
							float speed = 3.0f;
							float dt = ctx.Dt > 0.0f ? ctx.Dt : 0.016f;
							float step = std::min(dist, speed * dt);
							tr.Location.x += (diff.x / dist) * step;
							tr.Location.z += (diff.z / dist) * step;
						}
						return NodeStatus::Running;
					}

					// 到达插槽位置，开始使用
					if (ctx.Component.StateMachineState != 2)
					{
						bool used = SmartObjectSystem::UseSlot(ctx.CurrentScene, targetSo, (entt::entity)ctx.Self);
						if (used)
						{
							ctx.Component.StateMachineState = 2; // Use
							ctx.Component.ExecutionTimer = 0.0f;
						}
						else
						{
							ctx.Component.StateMachineState = 0;
							ctx.Component.TargetEntity = static_cast<uint32_t>(entt::null);
							ctx.Board.Set("TargetEntity", uint32_t(0));
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
					ctx.Component.StateMachineState = 3;
					ctx.Component.BlackboardValue += 1.0f;
					ctx.Component.TargetEntity = static_cast<uint32_t>(entt::null);
					ctx.Component.ExecutionTimer = 0.0f;

					ctx.Board.Set("TargetEntity", uint32_t(0));
					ctx.Board.Set("Value", ctx.Component.BlackboardValue);
					return NodeStatus::Success;
				});
			}
			else if (type == "Patrol")
			{
				return std::make_shared<ActionNode>([param](BehaviorContext& ctx) {
					ctx.Component.StateMachineState = 0;
					if (!ctx.Self.HasComponent<TransformComponent>())
						return NodeStatus::Failure;

					auto& tr = ctx.Self.GetComponent<TransformComponent>();
					float radius = param > 0.0f ? param : 10.0f;

					// 工业级马达集成：优先使用 NavMeshAgentComponent 驱动寻路漫游
					if (ctx.Self.HasComponent<NavMeshAgentComponent>())
					{
						auto& agent = ctx.Self.GetComponent<NavMeshAgentComponent>();
						if (agent.ReachedDestination || agent.HasPath == 0)
						{
							const auto& navMesh = ctx.CurrentScene.GetNavMesh();
							glm::vec3 nextPatrolPos = tr.Location;
							if (navMesh && navMesh->GetPolygonCount() > 0)
							{
								navMesh->GetRandomPointAround(tr.Location, radius, nextPatrolPos);
							}
							else
							{
								// 平面随机点
								float angle = static_cast<float>(rand() % 360) * 0.0174533f;
								nextPatrolPos = tr.Location + glm::vec3(std::cos(angle) * radius * 0.5f, 0.0f, std::sin(angle) * radius * 0.5f);
							}
							agent.Destination = nextPatrolPos;
							agent.AutoRepath = 1;
							agent.ReachedDestination = 0;
							ctx.Board.Set("TargetPos", nextPatrolPos);
							ctx.Component.BlackboardTargetPos = nextPatrolPos;
						}
						return NodeStatus::Running;
					}

					// 基础线性摆动兜底（仅在无 NavMeshAgentComponent 时）
					float dir = (ctx.Component.BlackboardTag == 1) ? -1.0f : 1.0f;
					float speed = param > 0.0f ? param : 2.0f;
					float dt = ctx.Dt > 0.0f ? ctx.Dt : 0.016f;
					tr.Location.x += dir * speed * dt;
					if (tr.Location.x >= 3.0f) { tr.Location.x = 3.0f; ctx.Component.BlackboardTag = 1; }
					else if (tr.Location.x <= -3.0f) { tr.Location.x = -3.0f; ctx.Component.BlackboardTag = 0; }
					ctx.Board.Set("Tag", ctx.Component.BlackboardTag);
					return NodeStatus::Success;
				});
			}
			else if (type == "MoveToTarget" || type == "MoveTo")
			{
				return std::make_shared<ActionNode>([param](BehaviorContext& ctx) {
					if (!ctx.Self.HasComponent<TransformComponent>())
						return NodeStatus::Failure;

					auto& tr = ctx.Self.GetComponent<TransformComponent>();
					glm::vec3 targetPos = ctx.Board.Get<glm::vec3>("TargetPos", ctx.Component.BlackboardTargetPos);

					uint32_t targetId = ctx.Board.Get<uint32_t>("TargetEntity", ctx.Component.TargetEntity);
					if (targetId != 0 && targetId != static_cast<uint32_t>(entt::null))
					{
						Entity targetEntity(&ctx.CurrentScene, static_cast<entt::entity>(targetId));
						if (targetEntity.IsValid() && targetEntity.HasComponent<TransformComponent>())
						{
							targetPos = targetEntity.GetComponent<TransformComponent>().Location;
							ctx.Board.Set("TargetPos", targetPos);
							ctx.Component.BlackboardTargetPos = targetPos;
						}
					}

					ctx.Component.StateMachineState = 1; // Moving

					// 工业级标准：委托给 NavMeshAgentComponent 处理 A* 寻路与避障
					if (ctx.Self.HasComponent<NavMeshAgentComponent>())
					{
						auto& agent = ctx.Self.GetComponent<NavMeshAgentComponent>();
						agent.Destination = targetPos;
						if (param > 0.0f) agent.Speed = param;
						agent.AutoRepath = 1;

						if (agent.ReachedDestination != 0)
						{
							return NodeStatus::Success;
						}
						return NodeStatus::Running;
					}

					// 若实体未挂载 NavMeshAgent，执行平滑趋近并到达判定
					glm::vec3 diff = targetPos - tr.Location;
					float dist = glm::length(diff);
					float speed = param > 0.0f ? param : 3.0f;
					float dt = ctx.Dt > 0.0f ? ctx.Dt : 0.016f;
					float step = speed * dt;

					if (dist <= 0.05f || step >= dist)
					{
						tr.Location = targetPos;
						return NodeStatus::Success;
					}

					tr.Location += (diff / dist) * step;
					return NodeStatus::Running;
				});
			}
			else if (type == "FindTacticalCover" || type == "RunEQSQuery")
			{
				std::string modeStr = "Cover";
				if (!desc.StringParam.empty())
					modeStr = desc.StringParam;

				return std::make_shared<ActionNode>([param, modeStr](BehaviorContext& ctx) {
					if (!ctx.Self.HasComponent<TransformComponent>())
						return NodeStatus::Failure;

					auto& tr = ctx.Self.GetComponent<TransformComponent>();
					uint32_t querierRaw = static_cast<uint32_t>((entt::entity)ctx.Self);
					uint32_t threatHint = 0;
					float radius = param > 0.0f ? param : 25.0f;
					float spacing = 1.5f;
					EQSQueryMode queryMode = EQSQueryMode::Cover;
					if (modeStr == "Flank") queryMode = EQSQueryMode::Flank;
					else if (modeStr == "Retreat") queryMode = EQSQueryMode::Retreat;

					uint8_t genMode = 0;
					if (ctx.Self.HasComponent<EQSComponent>())
					{
						const auto& eqsc = ctx.Self.GetComponent<EQSComponent>();
						threatHint = eqsc.ThreatEntityHint;
						if (eqsc.QueryRadius > 0.1f) radius = eqsc.QueryRadius;
						if (eqsc.GridSpacing > 0.1f) spacing = eqsc.GridSpacing;
						if (eqsc.QueryMode <= 2) queryMode = static_cast<EQSQueryMode>(eqsc.QueryMode);
						genMode = eqsc.GeneratorMode;
					}

					glm::vec3 threatPos(0.0f);
					uint32_t resolvedThreatRaw = 0;
					EQSCoverSystem::ResolveSceneThreatPosition(ctx.CurrentScene, querierRaw, threatHint, tr.Location, threatPos, &resolvedThreatRaw);

					std::vector<CoverBox> obstacles;
					EQSCoverSystem::GatherSceneObstacles(ctx.CurrentScene, querierRaw, resolvedThreatRaw, obstacles);

					glm::vec3 bestPos(0.0f);
					float bestScore = -1.0f;
					std::vector<EQSCoverCandidate> dummyCandidates;
					if (EQSCoverSystem::EvaluateQueryCandidates(queryMode, tr.Location, threatPos, radius, spacing, obstacles, dummyCandidates, bestPos, bestScore, nullptr, glm::mat4(1.0f), genMode))
					{
						ctx.Board.Set("TargetPos", bestPos);
						ctx.Board.Set("Value", bestScore);
						ctx.Board.Set("Tag", uint32_t(88));

						ctx.Component.BlackboardTargetPos = bestPos;
						ctx.Component.BlackboardValue = bestScore;
						ctx.Component.BlackboardTag = 88;
						if (ctx.Self.HasComponent<EQSComponent>())
						{
							auto& eqsc = ctx.Self.GetComponent<EQSComponent>();
							eqsc.QueryCenter = tr.Location;
							eqsc.BestCoverFound = 1;
						}
						return NodeStatus::Success;
					}
					return NodeStatus::Failure;
				});
			}
			else if (type == "Wait")
			{
				return std::make_shared<ActionNode>([param](BehaviorContext& ctx) {
					ctx.Component.ExecutionTimer += (ctx.Dt > 0.0f ? ctx.Dt : 0.016f);
					if (ctx.Component.ExecutionTimer >= param)
					{
						ctx.Component.ExecutionTimer = 0.0f;
						return NodeStatus::Success;
					}
					return NodeStatus::Running;
				});
			}
			else if (type == "SetBlackboardValue")
			{
				return std::make_shared<ActionNode>([param, desc](BehaviorContext& ctx) {
					std::string keyName = desc.StringParam.empty() ? "Value" : desc.StringParam;
					ctx.Board.Set(keyName, param);
					ctx.Component.BlackboardValue = param;
					return NodeStatus::Success;
				});
			}
			else if (type == "IncrementBlackboardValue")
			{
				return std::make_shared<ActionNode>([param, desc](BehaviorContext& ctx) {
					std::string keyName = desc.StringParam.empty() ? "Value" : desc.StringParam;
					float cur = ctx.Board.Get<float>(keyName, ctx.Component.BlackboardValue);
					cur += (param != 0.0f ? param : 1.0f);
					ctx.Board.Set(keyName, cur);
					ctx.Component.BlackboardValue = cur;
					return NodeStatus::Success;
				});
			}
			else if (type == "SetBlackboardTag")
			{
				return std::make_shared<ActionNode>([param, desc](BehaviorContext& ctx) {
					uint32_t tag = static_cast<uint32_t>(param);
					std::string keyName = desc.StringParam.empty() ? "Tag" : desc.StringParam;
					ctx.Board.Set(keyName, tag);
					ctx.Component.BlackboardTag = tag;
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
			else if (type == "ScriptAction")
			{
				std::string scriptPath = desc.StringParam;
				return std::make_shared<ActionNode>([scriptPath](BehaviorContext&) {
					if (scriptPath.empty())
						return NodeStatus::Success;
					if (World::ScriptEngine::IsInitialized())
					{
						std::string valStr;
						if (World::ScriptEngine::ReadScriptValue(scriptPath, "Status", valStr))
						{
							if (valStr == "Running") return NodeStatus::Running;
							if (valStr == "Failure") return NodeStatus::Failure;
						}
					}
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

		std::shared_ptr<BehaviorNode> BuildSubtree(
			uint32_t nodeId,
			const std::vector<BehaviorTreeNodeDesc>& nodes,
			std::unordered_set<uint32_t>& visited)
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
				auto sel = std::make_shared<SelectorNode>();
				sel->SetNodeId(static_cast<uint16_t>(desc.Id));
				sel->SetName(desc.Title);
				for (const auto& n : nodes)
				{
					if (n.ParentId == desc.Id)
					{
						auto child = BuildSubtree(n.Id, nodes, visited);
						if (child) sel->AddChild(child);
					}
				}
				return sel;
			}
			case BehaviorTreeNodeKind::Sequence:
			{
				auto seq = std::make_shared<SequenceNode>();
				seq->SetNodeId(static_cast<uint16_t>(desc.Id));
				seq->SetName(desc.Title);
				for (const auto& n : nodes)
				{
					if (n.ParentId == desc.Id)
					{
						auto child = BuildSubtree(n.Id, nodes, visited);
						if (child) seq->AddChild(child);
					}
				}
				return seq;
			}
			case BehaviorTreeNodeKind::Parallel:
			{
				auto par = std::make_shared<ParallelNode>();
				par->SetNodeId(static_cast<uint16_t>(desc.Id));
				par->SetName(desc.Title);
				for (const auto& n : nodes)
				{
					if (n.ParentId == desc.Id)
					{
						auto child = BuildSubtree(n.Id, nodes, visited);
						if (child) par->AddChild(child);
					}
				}
				return par;
			}
			case BehaviorTreeNodeKind::Decorator:
			{
				std::shared_ptr<BehaviorNode> childNode = nullptr;
				for (const auto& n : nodes)
				{
					if (n.ParentId == desc.Id)
					{
						childNode = BuildSubtree(n.Id, nodes, visited);
						break;
					}
				}
				auto dec = std::make_shared<BlackboardConditionDecorator>(
					desc.StringParam.empty() ? "TargetPos" : desc.StringParam,
					desc.CompOp,
					desc.FloatParam,
					childNode,
					desc.AbortMode);
				dec->SetNodeId(static_cast<uint16_t>(desc.Id));
				dec->SetName(desc.Title);
				return dec;
			}
			case BehaviorTreeNodeKind::Condition:
			{
				auto cond = CreateConditionNode(desc);
				if (cond)
				{
					cond->SetNodeId(static_cast<uint16_t>(desc.Id));
					cond->SetName(desc.Title);
				}
				return cond;
			}
			case BehaviorTreeNodeKind::Action:
			{
				auto act = CreateActionNode(desc);
				if (act)
				{
					act->SetNodeId(static_cast<uint16_t>(desc.Id));
					act->SetName(desc.Title);
				}
				return act;
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
		patrolAction.Description = "NavMesh roaming patrol";
		patrolAction.ActionType = "Patrol";
		patrolAction.FloatParam = 10.0f;

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
			YAML::Node root = YAML::LoadFile(path);
			if (!root || !root.IsMap())
			{
				if (error) *error = "Failed to parse YAML file: " + path;
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
					if (nNode["AbortMode"]) nodeDesc.AbortMode = StringToAbortMode(nNode["AbortMode"].as<std::string>());
					m_Nodes.push_back(std::move(nodeDesc));
				}
			}

			return true;
		}
		catch (const std::exception& e)
		{
			if (error) *error = e.what();
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
				out << YAML::Key << "AbortMode" << YAML::Value << AbortModeToString(node.AbortMode);
				out << YAML::EndMap;
			}
			out << YAML::EndSeq;
			out << YAML::EndMap;

			std::ofstream fout(path);
			if (!fout.is_open())
			{
				if (error) *error = "Failed to open file for writing: " + path;
				return false;
			}
			fout << out.c_str() << "\n";
			return true;
		}
		catch (const std::exception& e)
		{
			if (error) *error = e.what();
			return false;
		}
	}

	std::shared_ptr<BehaviorNode> BehaviorTreeAsset::InstantiateTree() const
	{
		if (m_Nodes.empty())
			return nullptr;

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

	std::shared_ptr<Blackboard> BehaviorTreeAsset::InstantiateBlackboard() const
	{
		auto bb = std::make_shared<Blackboard>();
		for (const auto& key : m_BlackboardKeys)
		{
			BlackboardKeyType ktype = BlackboardKeyType::Float;
			if (key.TypeName == "Bool") ktype = BlackboardKeyType::Bool;
			else if (key.TypeName == "Int" || key.TypeName == "Int32") ktype = BlackboardKeyType::Int32;
			else if (key.TypeName == "Vector3" || key.TypeName == "Vec3") ktype = BlackboardKeyType::Vector3;
			else if (key.TypeName == "Entity") ktype = BlackboardKeyType::Entity;
			else if (key.TypeName == "String") ktype = BlackboardKeyType::String;

			bb->RegisterKey(key.KeyName, ktype);
		}
		return bb;
	}

	const std::vector<std::string>& BehaviorTreeAsset::GetStandardConditionTypes()
	{
		static const std::vector<std::string> s_Types = {
			"HasTarget",
			"TargetInRange",
			"HealthBelow",
			"HealthAbove",
			"TimerElapsed",
			"BlackboardTagEquals",
			"BlackboardValueGreater",
			"BlackboardValueLess",
			"ScriptCondition",
			"AlwaysTrue",
			"AlwaysFalse",
			"Custom"
		};
		return s_Types;
	}

	const std::vector<std::string>& BehaviorTreeAsset::GetStandardActionTypes()
	{
		static const std::vector<std::string> s_Types = {
			"Wait",
			"Patrol",
			"MoveToTarget",
			"SeekSmartObject",
			"UseSmartObject",
			"FindTacticalCover",
			"SetBlackboardValue",
			"IncrementBlackboardValue",
			"SetBlackboardTag",
			"ResetTimer",
			"ScriptAction",
			"Custom"
		};
		return s_Types;
	}

	const std::vector<std::string>& BehaviorTreeAsset::GetStandardBlackboardTypes()
	{
		static const std::vector<std::string> s_Types = {
			"Float",
			"Vector3",
			"Entity",
			"Bool",
			"String",
			"Int"
		};
		return s_Types;
	}
}
