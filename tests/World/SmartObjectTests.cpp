#include "World/Gameplay/Framework/SmartObjectSystem.h"
#include "World/Gameplay/Framework/BehaviorTree.h"
#include "World/Gameplay/Framework/BehaviorTreeSystem.h"
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

	// 1. 创建场景中的 AI Agent 并挂载全要素组件
	Entity agent = Entity::CreateEntity(&scene, "NpcDoctor");
	auto& agentTr = agent.AddComponent<TransformComponent>();
	agentTr.Location = glm::vec3(0.0f, 0.0f, 0.0f);
	auto& agentHealth = agent.AddComponent<HealthComponent>();
	agentHealth.CurrentHealth = 100.0f;
	agentHealth.MaxHealth = 100.0f;
	agentHealth.InvulnerableDuration = 0.0f;

	auto& agentBt = agent.AddComponent<BehaviorTreeComponent>();
	agentBt.TreeAssetId = 1;
	agentBt.Enabled = 1;
	agentBt.StateMachineState = 0; // 0 = Patrol
	agentBt.TargetEntity = 0;

	// 2. 创建场景中的智能交互对象: 医疗充能站 (HealthStation)
	Entity station = Entity::CreateEntity(&scene, "HealthStation");
	auto& stationTr = station.AddComponent<TransformComponent>();
	stationTr.Location = glm::vec3(10.0f, 0.0f, 0.0f);
	auto& stationSo = station.AddComponent<SmartObjectComponent>();
	stationSo.SlotOffset = glm::vec3(0.0f, 0.0f, 1.0f); // 交互点在基站前方 1m
	stationSo.CooldownDuration = 2.0f;
	stationSo.ActivityTag = 1; // 1 = Medical/Healing

	// 3. 构建通用行为树 (Behavior Tree)
	// 架构:
	//           [Selector (Root)]
	//              /        \
	//   [Sequence (Emergency)]  [Action (Patrol)]
	//     /       |       \
	// [Cond]   [Action]  [Action]
	// (HP<50) (Claim)   (Use&Heal)

	auto rootSelector = std::make_shared<SelectorNode>();

	// 分支 A: 紧急低血量搜寻智能对象充能 (Sequence)
	auto healSequence = std::make_shared<SequenceNode>();
	// 条件节点: 检查生命值是否 < 50
	healSequence->AddChild(std::make_shared<ConditionNode>([](BehaviorContext& ctx) {
		return ctx.Self.GetComponent<HealthComponent>().CurrentHealth < 50.0f;
	}));
	// 动作节点 1: 检索最近的治疗智能对象并预约插槽 (Claim)
	healSequence->AddChild(std::make_shared<ActionNode>([](BehaviorContext& ctx) {
		auto& tr = ctx.Self.GetComponent<TransformComponent>();
		entt::entity targetSo = SmartObjectSystem::FindNearestAvailableSlot(ctx.CurrentScene, tr.Location, 1, 50.0f);
		if (targetSo == entt::null)
			return NodeStatus::Failure;

		bool claimed = SmartObjectSystem::ClaimSlot(ctx.CurrentScene, targetSo, (entt::entity)ctx.Self);
		if (claimed)
		{
			ctx.Component.StateMachineState = 1; // 1 = Seek
			ctx.Component.TargetEntity = static_cast<uint32_t>(targetSo);
			ctx.Component.BlackboardTargetPos = SmartObjectSystem::GetSlotWorldPosition(ctx.CurrentScene, targetSo);
			return NodeStatus::Success;
		}
		return NodeStatus::Failure;
	}));
	// 动作节点 2: 寻路对齐至插槽位置并使用智能对象 (Use)
	healSequence->AddChild(std::make_shared<ActionNode>([](BehaviorContext& ctx) {
		if (ctx.Component.TargetEntity == 0)
			return NodeStatus::Failure;

		entt::entity targetSo = static_cast<entt::entity>(ctx.Component.TargetEntity);
		auto& tr = ctx.Self.GetComponent<TransformComponent>();
		auto& health = ctx.Self.GetComponent<HealthComponent>();

		// 移动对齐到插槽世界坐标 (通过黑板中记录的位置)
		tr.Location = ctx.Component.BlackboardTargetPos;

		bool used = SmartObjectSystem::UseSlot(ctx.CurrentScene, targetSo, (entt::entity)ctx.Self);
		if (used)
		{
			ctx.Component.StateMachineState = 2; // 2 = Use
			DamageSystem::Heal(health, 60.0f); // 治疗充能
			ctx.Component.BlackboardValue += 1.0f; // 累加治疗计数至黑板标量

			// 使用完成后释放插槽进入冷却
			SmartObjectSystem::ReleaseSlot(ctx.CurrentScene, targetSo);
			ctx.Component.StateMachineState = 3; // 3 = CooldownExit
			ctx.Component.TargetEntity = 0;
			return NodeStatus::Success;
		}
		return NodeStatus::Failure;
	}));

	// 分支 B: 常规游荡巡逻动作 (Action)
	auto patrolAction = std::make_shared<ActionNode>([](BehaviorContext& ctx) {
		ctx.Component.StateMachineState = 0; // 0 = Patrol
		auto& tr = ctx.Self.GetComponent<TransformComponent>();
		tr.Location += glm::vec3(0.5f, 0.0f, 0.0f);
		return NodeStatus::Success;
	});

	rootSelector->AddChild(healSequence);
	rootSelector->AddChild(patrolAction);

	BehaviorTreeRunner aiRunner(rootSelector);
	BehaviorContext ctx{ scene, agent, agent.GetComponent<BehaviorTreeComponent>(), 0.016f };

	// ---- 运行走查验证 (所有断言 100% 检查实体组件数据) ----
	// 阶段 1: Agent 处于满血状态 (HP = 100) -> 行为树选择常规巡逻 (Patrol)
	aiRunner.Tick(ctx);
	assert(agent.GetComponent<BehaviorTreeComponent>().StateMachineState == 0); // 0 = Patrol
	assert(agent.GetComponent<BehaviorTreeComponent>().Status == BehaviorStatus::Success);
	assert(stationSo.SlotState == SmartObjectSlotState::Available);
	assert(agentTr.Location.x == 0.5f);

	// 阶段 2: Agent 遭受 70 点重创 (HP 降为 30 < 50)
	DamageEvent dmg;
	dmg.Target = (entt::entity)agent;
	dmg.Amount = 70.0f;
	dmg.Flags = DamageFlags::IgnoreArmor;
	DamageSystem::ApplyDamage(scene, dmg);
	assert(agentHealth.CurrentHealth == 30.0f);

	// 阶段 3: 行为树 Tick -> 条件 (HP<50) 满足 -> 触发分支 A!
	// 步骤 A1: 找到 HealthStation 并预约锁定 (Claim)，写入 TargetEntity 与 BlackboardTargetPos，状态置为 Seek (1)
	// 步骤 A2: 移动至 (10, 0, 1), 使用智能对象并回血至 90, 释放插槽进入 Cooldown，状态置为 CooldownExit (3)
	aiRunner.Tick(ctx);
	assert(agentHealth.CurrentHealth == 90.0f);
	assert(agentTr.Location.x == 10.0f && agentTr.Location.z == 1.0f);
	assert(stationSo.SlotState == SmartObjectSlotState::Cooldown);
	assert(agent.GetComponent<BehaviorTreeComponent>().StateMachineState == 3); // 3 = CooldownExit
	assert(agent.GetComponent<BehaviorTreeComponent>().TargetEntity == 0);
	assert(agent.GetComponent<BehaviorTreeComponent>().Status == BehaviorStatus::Success);
	assert(agent.GetComponent<BehaviorTreeComponent>().BlackboardValue == 1.0f);

	// 阶段 4: 下一帧 Agent 已经恢复健康 (HP = 90 >= 50) -> 行为树回归常规巡逻分支
	aiRunner.Tick(ctx);
	assert(agent.GetComponent<BehaviorTreeComponent>().StateMachineState == 0); // 0 = Patrol
	assert(agentTr.Location.x == 10.5f);
	assert(agent.GetComponent<BehaviorTreeComponent>().Status == BehaviorStatus::Success);

	// 阶段 5: 另一个受伤 NPC 试图在此期间抢占该 HealthStation -> 失败 (因为处于 Cooldown 冷却中)
	Entity anotherNpc = Entity::CreateEntity(&scene, "AnotherNpc");
	bool claimWhileCooldown = SmartObjectSystem::ClaimSlot(scene, (entt::entity)station, (entt::entity)anotherNpc);
	assert(!claimWhileCooldown);

	// 阶段 6: 随着时间流逝 2.1 秒后, 智能对象自动冷却完毕解冻
	SmartObjectSystem::Update(scene, 2.1f);
	assert(stationSo.SlotState == SmartObjectSlotState::Available);

	std::cout << "[PASS] TestSmartObjectWithBehaviorTreeAndFSM\n";
}

static void TestSmartObjectDefaultBehaviorTreeAndFrameSystem()
{
	WorldContext context;
	Scene scene(context);

	// 1. 验证 GetTree(1) 自动注册 Demo 树
	auto tree = BehaviorTreeSystem::GetTree(1);
	assert(tree != nullptr);

	// 2. 创建场景中的智能交互对象: 医疗充能站 (HealthStation)
	Entity station = Entity::CreateEntity(&scene, "HealthStation");
	auto& stationTr = station.AddComponent<TransformComponent>();
	stationTr.Location = glm::vec3(0.0f, 0.0f, 0.0f);
	auto& stationSo = station.AddComponent<SmartObjectComponent>();
	stationSo.SlotOffset = glm::vec3(0.0f, 0.0f, 0.0f);
	stationSo.CooldownDuration = 2.0f;
	stationSo.ActivityTag = 1;

	// 3. 创建场景中的 AI Agent
	Entity agent = Entity::CreateEntity(&scene, "AIAgent");
	auto& agentTr = agent.AddComponent<TransformComponent>();
	agentTr.Location = glm::vec3(0.0f, 0.0f, 0.0f);
	auto& agentHealth = agent.AddComponent<HealthComponent>();
	agentHealth.CurrentHealth = 30.0f; // 低血量 (< 60)
	agentHealth.MaxHealth = 100.0f;

	auto& agentBt = agent.AddComponent<BehaviorTreeComponent>();
	agentBt.TreeAssetId = 1;
	agentBt.Enabled = 1;
	agentBt.StateMachineState = 0;

	// 4. 执行一帧 BehaviorTreeSystem::Update
	BehaviorTreeSystem::Update(scene, 0.016f);

	// 验证已自动预约 (Claim) 并进入使用/移动流程
	
assert(agentBt.StateMachineState >= 1);

	assert(stationSo.SlotState == SmartObjectSlotState::Claimed || stationSo.SlotState == SmartObjectSlotState::InUse);

	// 5. 持续步进直到治疗充能完成 (交互计时器 1.0s)
	for (int i = 0; i < 70; ++i)
	{
		BehaviorTreeSystem::Update(scene, 0.016f);
	}

	// 验证血量已恢复 (30 + 60 = 90) 且插槽进入 Cooldown
	assert(agentHealth.CurrentHealth >= 90.0f);
	assert(stationSo.SlotState == SmartObjectSlotState::Cooldown);

	// 6. 验证 smart-object-system 在 Scene 默认帧管线中正常推进倒计时解冻
	scene.EnsureDefaultFrameSystems();
	scene.OnFixedUpdate(2.1f);
	assert(stationSo.SlotState == SmartObjectSlotState::Available);

	std::cout << "[PASS] TestSmartObjectDefaultBehaviorTreeAndFrameSystem\n";
}

int main()
{
	Log::Init();
	std::cout << "Running WorldSmartObjectTests...\n";

	TestSmartObjectClaimUseRelease();
	TestSmartObjectWithBehaviorTreeAndFSM();
	TestSmartObjectDefaultBehaviorTreeAndFrameSystem();

	std::cout << "All WorldSmartObjectTests passed successfully.\n";
	return 0;
}