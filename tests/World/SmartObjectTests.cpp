#include "World/Gameplay/Framework/SmartObjectSystem.h"
#include "World/Gameplay/Framework/BehaviorTree.h"
#include "World/Gameplay/Framework/DamageSystem.h"
#include "World/Scene/Components.h"
#include "World/Scene/Scene.h"
#include "World/Scene/Entity.h"
#include "World/Core/Log.h"
#include "World/Core/WorldContext.h"

#include <cassert>
#include <iostream>
#include <memory>

using namespace World;
using namespace World::Gameplay;

static void TestSmartObjectClaimUseRelease()
{
	WorldContext context;
	Scene scene(context);

	Entity bench = Entity::CreateEntity(&scene, "ParkBench");
	auto& so = bench.AddComponent<SmartObjectComponent>();
	so.SlotOffset = glm::vec3(0.0f, 0.5f, 0.0f);
	so.CooldownDuration = 2.0f;

	Entity npc1 = Entity::CreateEntity(&scene, "NPC1");
	Entity npc2 = Entity::CreateEntity(&scene, "NPC2");

	bool claimed1 = SmartObjectSystem::ClaimSlot(scene, (entt::entity)bench, (entt::entity)npc1);
	assert(claimed1);
	assert(so.SlotState == SmartObjectSlotState::Claimed);
	assert(so.ClaimedByEntity == (uint32_t)(entt::entity)npc1);

	bool claimed2 = SmartObjectSystem::ClaimSlot(scene, (entt::entity)bench, (entt::entity)npc2);
	assert(!claimed2);

	bool used1 = SmartObjectSystem::UseSlot(scene, (entt::entity)bench, (entt::entity)npc1);
	assert(used1);
	assert(so.SlotState == SmartObjectSlotState::InUse);

	bool released = SmartObjectSystem::ReleaseSlot(scene, (entt::entity)bench);
	assert(released);
	assert(so.SlotState == SmartObjectSlotState::Cooldown);
	assert(so.CooldownTimer == 2.0f);

	SmartObjectSystem::Update(scene, 2.1f);
	assert(so.SlotState == SmartObjectSlotState::Available);
	assert(so.CooldownTimer == 0.0f);

	std::cout << "[PASS] TestSmartObjectClaimUseRelease\n";
}

static void TestSmartObjectWithBehaviorTreeAndFSM()
{
	WorldContext context;
	Scene scene(context);

	// 1. 创建场景中的智能交互对象: 医疗充能站 (HealthStation)
	Entity station = Entity::CreateEntity(&scene, "HealthStation");
	auto& stationTr = station.AddComponent<TransformComponent>();
	stationTr.Location = glm::vec3(10.0f, 0.0f, 0.0f);
	auto& stationSo = station.AddComponent<SmartObjectComponent>();
	stationSo.SlotOffset = glm::vec3(0.0f, 0.0f, 1.0f); // 交互点在基站前方 1m
	stationSo.CooldownDuration = 2.0f;
	stationSo.ActivityTag = 1; // 1 = Medical/Healing

	// 2. 创建场景中的 AI Agent
	Entity agent = Entity::CreateEntity(&scene, "NpcDoctor");
	auto& agentTr = agent.AddComponent<TransformComponent>();
	agentTr.Location = glm::vec3(0.0f, 0.0f, 0.0f);
	auto& agentHealth = agent.AddComponent<HealthComponent>();
	agentHealth.CurrentHealth = 100.0f;
	agentHealth.MaxHealth = 100.0f;
	agentHealth.InvulnerableDuration = 0.0f;

	// 3. 定义有限状态机 (FSM)
	enum class AgentState
	{
		Patrol,
		SeekSmartObject,
		UseSmartObject,
		CooldownExit
	};
	AgentState currentState = AgentState::Patrol;

	// 4. 构建通用行为树 (Behavior Tree)
	// 架构:
	//           [Selector (Root)]
	//              /        \
	//   [Sequence (Emergency)]  [Action (Patrol)]
	//     /       |       \
	// [Cond]   [Action]  [Action]
	// (HP<50) (Claim)   (Use&Heal)
	
	entt::entity targetSmartObject = entt::null;
	int patrolCount = 0;
	int healCount = 0;

	auto rootSelector = std::make_shared<SelectorNode>();

	// 分支 A: 紧急低血量搜寻智能对象充能 (Sequence)
	auto healSequence = std::make_shared<SequenceNode>();
	// 条件节点: 检查生命值是否 < 50
	healSequence->AddChild(std::make_shared<ConditionNode>([&]() {
		return agentHealth.CurrentHealth < 50.0f;
	}));
	// 动作节点 1: 检索最近的治疗智能对象并预约插槽 (Claim)
	healSequence->AddChild(std::make_shared<ActionNode>([&](float dt) {
		targetSmartObject = SmartObjectSystem::FindNearestAvailableSlot(scene, agentTr.Location, 1, 50.0f);
		if (targetSmartObject == entt::null)
			return NodeStatus::Failure;

		bool claimed = SmartObjectSystem::ClaimSlot(scene, targetSmartObject, (entt::entity)agent);
		if (claimed)
		{
			currentState = AgentState::SeekSmartObject;
			return NodeStatus::Success;
		}
		return NodeStatus::Failure;
	}));
	// 动作节点 2: 寻路对齐至插槽位置并使用智能对象 (Use)
	healSequence->AddChild(std::make_shared<ActionNode>([&](float dt) {
		if (targetSmartObject == entt::null)
			return NodeStatus::Failure;

		// 移动对齐到插槽世界坐标
		glm::vec3 slotPos = SmartObjectSystem::GetSlotWorldPosition(scene, targetSmartObject);
		agentTr.Location = slotPos;

		bool used = SmartObjectSystem::UseSlot(scene, targetSmartObject, (entt::entity)agent);
		if (used)
		{
			currentState = AgentState::UseSmartObject;
			DamageSystem::Heal(agentHealth, 60.0f); // 治疗充能
			++healCount;

			// 使用完成后释放插槽进入冷却
			SmartObjectSystem::ReleaseSlot(scene, targetSmartObject);
			currentState = AgentState::CooldownExit;
			targetSmartObject = entt::null;
			return NodeStatus::Success;
		}
		return NodeStatus::Failure;
	}));

	// 分支 B: 常规游荡巡逻动作 (Action)
	auto patrolAction = std::make_shared<ActionNode>([&](float dt) {
		currentState = AgentState::Patrol;
		++patrolCount;
		agentTr.Location += glm::vec3(0.5f, 0.0f, 0.0f);
		return NodeStatus::Success;
	});

	rootSelector->AddChild(healSequence);
	rootSelector->AddChild(patrolAction);

	BehaviorTreeRunner aiRunner(rootSelector);

	// ---- 运行走查验证 ----
	// 阶段 1: Agent 处于满血状态 (HP = 100) -> 行为树选择分支 B (巡逻)
	aiRunner.Tick(0.016f);
	assert(currentState == AgentState::Patrol);
	assert(patrolCount == 1);
	assert(healCount == 0);
	assert(stationSo.SlotState == SmartObjectSlotState::Available);

	// 阶段 2: Agent 遭受 70 点重创 (HP 降为 30 < 50)
	DamageEvent dmg;
	dmg.Target = (entt::entity)agent;
	dmg.Amount = 70.0f;
	dmg.Flags = DamageFlags::IgnoreArmor;
	DamageSystem::ApplyDamage(scene, dmg);
	assert(agentHealth.CurrentHealth == 30.0f);

	// 阶段 3: 行为树 Tick -> 条件 (HP<50) 满足 -> 触发分支 A!
	// 步骤 A1: 找到 HealthStation 并预约锁定 (Claim)
	// 步骤 A2: 移动至 (10, 0, 1), 使用智能对象并回血至 90, 释放插槽进入 Cooldown!
	aiRunner.Tick(0.016f);
	assert(healCount == 1);
	assert(agentHealth.CurrentHealth == 90.0f);
	assert(agentTr.Location.x == 10.0f && agentTr.Location.z == 1.0f);
	assert(stationSo.SlotState == SmartObjectSlotState::Cooldown);
	assert(currentState == AgentState::CooldownExit);

	// 阶段 4: 下一帧 Agent 已经恢复健康 (HP = 90 >= 50) -> 行为树回归常规巡逻分支
	aiRunner.Tick(0.016f);
	assert(currentState == AgentState::Patrol);
	assert(patrolCount == 2);

	// 阶段 5: 另一个受伤 NPC 试图在此期间抢占该 HealthStation -> 失败 (因为处于 Cooldown 冷却中)
	Entity anotherNpc = Entity::CreateEntity(&scene, "AnotherNpc");
	bool claimWhileCooldown = SmartObjectSystem::ClaimSlot(scene, (entt::entity)station, (entt::entity)anotherNpc);
	assert(!claimWhileCooldown);

	// 阶段 6: 随着时间流逝 2.1 秒后, 智能对象自动冷却完毕解冻
	SmartObjectSystem::Update(scene, 2.1f);
	assert(stationSo.SlotState == SmartObjectSlotState::Available);

	std::cout << "[PASS] TestSmartObjectWithBehaviorTreeAndFSM\n";
}

int main()
{
	Log::Init();
	std::cout << "Running WorldSmartObjectTests...\n";

	TestSmartObjectClaimUseRelease();
	TestSmartObjectWithBehaviorTreeAndFSM();

	std::cout << "All WorldSmartObjectTests passed successfully.\n";
	return 0;
}