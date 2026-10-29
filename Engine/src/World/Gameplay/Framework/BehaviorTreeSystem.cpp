#include "wldpch.h"
#include "World/Gameplay/Framework/BehaviorTreeSystem.h"
#include "World/Gameplay/Framework/SmartObjectSystem.h"
#include "World/Gameplay/Framework/DamageSystem.h"
#include "World/Scene/Scene.h"
#include "World/Scene/Entity.h"
#include "World/Scene/Components.h"
#include <mutex>
#include <unordered_map>
#include <cmath>

namespace World::Gameplay
{
	namespace
	{
		std::mutex s_TreeRegistryMutex;
		std::unordered_map<uint32_t, std::shared_ptr<BehaviorNode>> s_TreeRegistry;

		std::shared_ptr<BehaviorNode> BuildDefaultDemoTree()
		{
			auto rootSelector = std::make_shared<SelectorNode>();

			// 分支 A: 紧急低血量医疗充能 (Sequence)
			auto healSequence = std::make_shared<SequenceNode>();

			// 节点 1: 条件判定 - 检查生命值是否低于 60
			healSequence->AddChild(std::make_shared<ConditionNode>([](BehaviorContext& ctx) {
				if (!ctx.Self.HasComponent<HealthComponent>()) {
					return false;
				}
				float hp = ctx.Self.GetComponent<HealthComponent>().CurrentHealth;
				return hp < 60.0f;
			}));

			// 节点 2: 动作 - 检索并预约 (Claim) 医疗智能对象
			healSequence->AddChild(std::make_shared<ActionNode>([](BehaviorContext& ctx) {
				if (ctx.Component.StateMachineState == 1 /* Seek */ || ctx.Component.StateMachineState == 2 /* Use */)
					return NodeStatus::Success;

				auto& tr = ctx.Self.GetComponent<TransformComponent>();
				entt::entity targetSo = SmartObjectSystem::FindNearestAvailableSlot(ctx.CurrentScene, tr.Location, 1, 50.0f);
				if (targetSo == entt::null) {
					targetSo = SmartObjectSystem::FindNearestAvailableSlot(ctx.CurrentScene, tr.Location, 0, 50.0f);
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
			}));

			// 节点 3: 动作 - 移动接近并使用 (Use) 智能对象
			healSequence->AddChild(std::make_shared<ActionNode>([](BehaviorContext& ctx) {
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
				DamageSystem::Heal(health, 60.0f);
				SmartObjectSystem::ReleaseSlot(ctx.CurrentScene, targetSo);
				ctx.Component.StateMachineState = 3; // 3 = CooldownExit
				ctx.Component.BlackboardValue += 1.0f;
				ctx.Component.TargetEntity = static_cast<uint32_t>(entt::null);
				ctx.Component.ExecutionTimer = 0.0f;
				return NodeStatus::Success;
			}));

			// 分支 B: 常规日常游荡巡逻动作 (Action)
			auto patrolAction = std::make_shared<ActionNode>([](BehaviorContext& ctx) {
				ctx.Component.StateMachineState = 0; // 0 = Patrol
				if (ctx.Self.HasComponent<TransformComponent>())
				{
					auto& tr = ctx.Self.GetComponent<TransformComponent>();
					float dir = (ctx.Component.BlackboardTag == 1) ? -1.0f : 1.0f;
					float speed = 2.0f;
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

			rootSelector->AddChild(healSequence);
			rootSelector->AddChild(patrolAction);
			return rootSelector;
		}
	}

	void BehaviorTreeSystem::RegisterTree(uint32_t assetId, std::shared_ptr<BehaviorNode> root)
	{
		std::lock_guard<std::mutex> lock(s_TreeRegistryMutex);
		s_TreeRegistry[assetId] = std::move(root);
	}

	void BehaviorTreeSystem::UnregisterTree(uint32_t assetId)
	{
		std::lock_guard<std::mutex> lock(s_TreeRegistryMutex);
		s_TreeRegistry.erase(assetId);
	}

	std::shared_ptr<BehaviorNode> BehaviorTreeSystem::GetTree(uint32_t assetId)
	{
		std::lock_guard<std::mutex> lock(s_TreeRegistryMutex);
		auto it = s_TreeRegistry.find(assetId);
		if (it != s_TreeRegistry.end())
			return it->second;

		if (assetId == 1)
		{
			auto tree = BuildDefaultDemoTree();
			s_TreeRegistry[1] = tree;
			return tree;
		}

		return nullptr;
	}

	void BehaviorTreeSystem::ClearTrees()
	{
		std::lock_guard<std::mutex> lock(s_TreeRegistryMutex);
		s_TreeRegistry.clear();
	}

	void BehaviorTreeSystem::Update(Scene& scene, float dt)
	{
		auto& registry = const_cast<entt::registry&>(std::as_const(scene).GetRegistry());
		auto view = registry.view<TransformComponent, BehaviorTreeComponent>();
		for (auto entityHandle : view)
		{
			auto& bt = view.get<BehaviorTreeComponent>(entityHandle);
			if (!bt.Enabled)
				continue;

			auto root = GetTree(bt.TreeAssetId);
			if (!root)
				continue;

			Entity entity(&scene, entityHandle);
			BehaviorContext ctx{ scene, entity, bt, dt };
			NodeStatus status = root->Tick(ctx);
			if (status == NodeStatus::Running)
				bt.Status = BehaviorStatus::Running;
			else if (status == NodeStatus::Success)
				bt.Status = BehaviorStatus::Success;
			else if (status == NodeStatus::Failure)
				bt.Status = BehaviorStatus::Failure;
		}
	}
}
