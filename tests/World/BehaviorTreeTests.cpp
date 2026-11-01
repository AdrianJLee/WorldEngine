#include "World/Gameplay/Framework/BehaviorTree.h"
#include "World/Gameplay/Framework/BehaviorTreeAsset.h"
#include "World/Gameplay/Framework/BehaviorTreeSystem.h"
#include "World/Gameplay/Framework/Blackboard.h"
#include "World/Level/AI/EQS/EnvQueryManager.h"
#include "World/Level/AI/EQSCoverSystem.h"
#include "World/Scene/Scene.h"
#include "World/Scene/Entity.h"
#include "World/Scene/Components.h"
#include "World/Core/Log.h"
#include "World/Core/WorldContext.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <memory>

using namespace World;
using namespace World::Gameplay;

static void TestSequenceAndSelectorBasic()
{
	WorldContext context;
	Scene scene(context);

	Entity agent = Entity::CreateEntity(&scene, "Agent");
	agent.AddComponent<TransformComponent>();
	auto& bt = agent.AddComponent<BehaviorTreeComponent>();

	BehaviorContext ctx{ scene, agent, bt, 0.016f };

	// 1. Sequence 成功测试
	auto seq = std::make_shared<SequenceNode>();
	int actionCount = 0;

	seq->AddChild(std::make_shared<ConditionNode>([](BehaviorContext&) { return true; }));
	seq->AddChild(std::make_shared<ActionNode>([&](BehaviorContext& c) {
		++actionCount;
		c.Component.BlackboardValue = 42.0f;
		return NodeStatus::Success;
	}));

	BehaviorTreeRunner runner(seq);
	NodeStatus s1 = runner.Tick(ctx);
	assert(s1 == NodeStatus::Success);
	assert(actionCount == 1);
	assert(bt.Status == BehaviorStatus::Success);
	assert(bt.BlackboardValue == 42.0f);
	assert(bt.CurrentNodeIndex == 0);

	// 2. Sequence 失败测试 (条件失败中断后续动作)
	auto seqFail = std::make_shared<SequenceNode>();
	seqFail->AddChild(std::make_shared<ConditionNode>([](BehaviorContext&) { return false; }));
	seqFail->AddChild(std::make_shared<ActionNode>([&](BehaviorContext&) {
		++actionCount;
		return NodeStatus::Success;
	}));

	runner.SetRoot(seqFail);
	NodeStatus s2 = runner.Tick(ctx);
	assert(s2 == NodeStatus::Failure);
	assert(actionCount == 1);
	assert(bt.Status == BehaviorStatus::Failure);
	assert(bt.CurrentNodeIndex == 0);

	// 3. Selector 成功测试 (第一个条件失败，回退执行第二个动作)
	auto sel = std::make_shared<SelectorNode>();
	sel->AddChild(std::make_shared<ConditionNode>([](BehaviorContext&) { return false; }));
	sel->AddChild(std::make_shared<ActionNode>([&](BehaviorContext& c) {
		++actionCount;
		c.Component.BlackboardValue = 99.0f;
		return NodeStatus::Success;
	}));

	runner.SetRoot(sel);
	NodeStatus s3 = runner.Tick(ctx);
	assert(s3 == NodeStatus::Success);
	assert(actionCount == 2);
	assert(bt.Status == BehaviorStatus::Success);
	assert(bt.BlackboardValue == 99.0f);
	assert(bt.CurrentNodeIndex == 0);

	// 4. Selector 全失败测试
	auto selFail = std::make_shared<SelectorNode>();
	selFail->AddChild(std::make_shared<ConditionNode>([](BehaviorContext&) { return false; }));
	selFail->AddChild(std::make_shared<ConditionNode>([](BehaviorContext&) { return false; }));

	runner.SetRoot(selFail);
	NodeStatus s4 = runner.Tick(ctx);
	assert(s4 == NodeStatus::Failure);
	assert(bt.Status == BehaviorStatus::Failure);
	assert(bt.CurrentNodeIndex == 0);

	std::cout << "[PASS] TestSequenceAndSelectorBasic\n";
}

static void TestSequenceRunningAndResume()
{
	WorldContext context;
	Scene scene(context);

	Entity agent = Entity::CreateEntity(&scene, "AgentSequenceResume");
	agent.AddComponent<TransformComponent>();
	auto& bt = agent.AddComponent<BehaviorTreeComponent>();

	int step1Runs = 0;
	int step3Runs = 0;

	auto seq = std::make_shared<SequenceNode>();
	// 节点 0: 步骤 1 (立即成功)
	seq->AddChild(std::make_shared<ActionNode>([&](BehaviorContext& c) {
		++step1Runs;
		c.Component.BlackboardValue = 100.0f;
		return NodeStatus::Success;
	}));
	// 节点 1: 步骤 2 (基于 ExecutionTimer 持续 Running，达到 0.05 秒后 Success)
	seq->AddChild(std::make_shared<ActionNode>([](BehaviorContext& c) {
		c.Component.ExecutionTimer += c.Dt;
		if (c.Component.ExecutionTimer < 0.05f)
		{
			return NodeStatus::Running;
		}
		c.Component.BlackboardValue = 200.0f;
		return NodeStatus::Success;
	}));
	// 节点 2: 步骤 3 (立即成功)
	seq->AddChild(std::make_shared<ActionNode>([&](BehaviorContext& c) {
		++step3Runs;
		c.Component.BlackboardValue = 300.0f;
		return NodeStatus::Success;
	}));

	BehaviorTreeRunner runner(seq);
	BehaviorContext ctx{ scene, agent, bt, 0.02f };

	// 帧 1 (dt = 0.02s):
	// 节点 0 成功 -> 节点 1 计时 0.02s (<0.05s) 返回 Running
	NodeStatus s1 = runner.Tick(ctx);
	assert(s1 == NodeStatus::Running);
	assert(bt.Status == BehaviorStatus::Running);
	assert(bt.CurrentNodeIndex == 1);
	assert(step1Runs == 1);
	assert(step3Runs == 0);
	assert(bt.ExecutionTimer == 0.02f);
	assert(bt.BlackboardValue == 100.0f);

	// 帧 2 (dt = 0.02s):
	// 无状态断点续跑：直接从节点 1 恢复执行，节点 0 绝不重复执行！
	NodeStatus s2 = runner.Tick(ctx);
	assert(s2 == NodeStatus::Running);
	assert(bt.Status == BehaviorStatus::Running);
	assert(bt.CurrentNodeIndex == 1);
	assert(step1Runs == 1); // 证明节点 0 未被重复执行
	assert(step3Runs == 0);
	assert(bt.ExecutionTimer == 0.04f);

	// 帧 3 (dt = 0.02s):
	// 节点 1 恢复执行，计时达 0.06s (>=0.05s) 返回 Success -> 推进到节点 2 执行成功 -> 序列全完成
	NodeStatus s3 = runner.Tick(ctx);
	assert(s3 == NodeStatus::Success);
	assert(bt.Status == BehaviorStatus::Success);
	assert(bt.CurrentNodeIndex == 0); // 完成后重置为 0
	assert(step1Runs == 1);
	assert(step3Runs == 1);
	assert(bt.BlackboardValue == 300.0f);

	std::cout << "[PASS] TestSequenceRunningAndResume\n";
}

static void TestSelectorRunningAndResume()
{
	WorldContext context;
	Scene scene(context);

	Entity agent = Entity::CreateEntity(&scene, "AgentSelectorResume");
	agent.AddComponent<TransformComponent>();
	auto& bt = agent.AddComponent<BehaviorTreeComponent>();

	int condRuns = 0;
	auto sel = std::make_shared<SelectorNode>();
	// 节点 0: 条件失败
	sel->AddChild(std::make_shared<ConditionNode>([&](BehaviorContext&) {
		++condRuns;
		return false;
	}));
	// 节点 1: 异步运行动作
	sel->AddChild(std::make_shared<ActionNode>([](BehaviorContext& c) {
		c.Component.ExecutionTimer += c.Dt;
		if (c.Component.ExecutionTimer < 0.03f)
		{
			return NodeStatus::Running;
		}
		c.Component.StateMachineState = 7;
		return NodeStatus::Success;
	}));

	BehaviorTreeRunner runner(sel);
	BehaviorContext ctx{ scene, agent, bt, 0.02f };

	// 帧 1:
	// 节点 0 失败 -> 节点 1 返回 Running
	NodeStatus s1 = runner.Tick(ctx);
	assert(s1 == NodeStatus::Running);
	assert(bt.Status == BehaviorStatus::Running);
	assert(bt.CurrentNodeIndex == 1);
	assert(condRuns == 1);

	// 帧 2:
	// 直接续跑节点 1，节点 0 不再重新评估
	NodeStatus s2 = runner.Tick(ctx);
	assert(s2 == NodeStatus::Success);
	assert(bt.Status == BehaviorStatus::Success);
	assert(bt.CurrentNodeIndex == 0);
	assert(condRuns == 1); // 未重新评估节点 0
	assert(bt.StateMachineState == 7);

	std::cout << "[PASS] TestSelectorRunningAndResume\n";
}

static void TestBehaviorTreeSystemBatchExecution()
{
	WorldContext context;
	Scene scene(context);

	Entity agentA = Entity::CreateEntity(&scene, "AgentA");
	agentA.AddComponent<TransformComponent>();
	auto& btA = agentA.AddComponent<BehaviorTreeComponent>();
	btA.TreeAssetId = 42;
	btA.Enabled = 1;

	Entity agentB = Entity::CreateEntity(&scene, "AgentB");
	agentB.AddComponent<TransformComponent>();
	auto& btB = agentB.AddComponent<BehaviorTreeComponent>();
	btB.TreeAssetId = 42;
	btB.Enabled = 1;

	Entity agentC = Entity::CreateEntity(&scene, "AgentC");
	agentC.AddComponent<TransformComponent>();
	auto& btC = agentC.AddComponent<BehaviorTreeComponent>();
	btC.TreeAssetId = 42;
	btC.Enabled = 0; // 禁用

	auto tree = std::make_shared<ActionNode>([](BehaviorContext& c) {
		c.Component.BlackboardValue += 1.0f;
		return NodeStatus::Success;
	});

	BehaviorTreeSystem::RegisterTree(42, tree);
	BehaviorTreeSystem::Update(scene, 0.016f);

	assert(btA.Status == BehaviorStatus::Success);
	assert(btA.BlackboardValue == 1.0f);
	assert(btB.Status == BehaviorStatus::Success);
	assert(btB.BlackboardValue == 1.0f);
	assert(btC.Status == BehaviorStatus::Inactive);
	assert(btC.BlackboardValue == 0.0f);

	BehaviorTreeSystem::ClearTrees();

	std::cout << "[PASS] TestBehaviorTreeSystemBatchExecution\n";
}

static void TestBehaviorTreeAssetSerializationAndInstantiation()
{
	// 1. 创建行为树资产并配置黑板与节点
	BehaviorTreeAsset asset;
	asset.BlackboardKeys().push_back({ "TargetPos", "Vector3", "0,0,0" });
	asset.BlackboardKeys().push_back({ "ExecutionTimer", "Float", "0.0" });

	BehaviorTreeNodeDesc seqNode;
	seqNode.Id = 1;
	seqNode.ParentId = 0;
	seqNode.Kind = BehaviorTreeNodeKind::Sequence;
	seqNode.Title = "Root Sequence";
	seqNode.Description = "Execute timer check and value setters";

	BehaviorTreeNodeDesc condNode;
	condNode.Id = 2;
	condNode.ParentId = 1;
	condNode.Kind = BehaviorTreeNodeKind::Condition;
	condNode.Title = "Check Timer";
	condNode.ConditionType = "TimerElapsed";
	condNode.FloatParam = 0.0f;

	BehaviorTreeNodeDesc setValNode;
	setValNode.Id = 3;
	setValNode.ParentId = 1;
	setValNode.Kind = BehaviorTreeNodeKind::Action;
	setValNode.Title = "Set Blackboard";
	setValNode.ActionType = "SetBlackboardValue";
	setValNode.FloatParam = 88.0f;

	BehaviorTreeNodeDesc incValNode;
	incValNode.Id = 4;
	incValNode.ParentId = 1;
	incValNode.Kind = BehaviorTreeNodeKind::Action;
	incValNode.Title = "Increment Blackboard";
	incValNode.ActionType = "IncrementBlackboardValue";
	incValNode.FloatParam = 12.0f;

	asset.Nodes().push_back(seqNode);
	asset.Nodes().push_back(condNode);
	asset.Nodes().push_back(setValNode);
	asset.Nodes().push_back(incValNode);

	// 2. 序列化保存到磁盘
	const std::string tempPath = "build_test_bt_asset.wbt";
	std::string saveErr;
	bool saved = asset.SaveToFile(tempPath, &saveErr);
	assert(saved);

	// 3. 反序列化从磁盘重新读取
	BehaviorTreeAsset loadedAsset;
	std::string loadErr;
	bool loaded = loadedAsset.LoadFromFile(tempPath, &loadErr);
	assert(loaded);
	assert(loadedAsset.BlackboardKeys().size() == 2);
	assert(loadedAsset.Nodes().size() == 4);
	assert(loadedAsset.Nodes()[0].Kind == BehaviorTreeNodeKind::Sequence);
	assert(loadedAsset.Nodes()[1].ConditionType == "TimerElapsed");

	// 4. 资产实例化与 Tick 执行验证
	auto root = loadedAsset.InstantiateTree();
	assert(root != nullptr);

	WorldContext context;
	Scene scene(context);
	Entity agent = Entity::CreateEntity(&scene, "AssetAgent");
	agent.AddComponent<TransformComponent>();
	auto& bt = agent.AddComponent<BehaviorTreeComponent>();
	BehaviorContext ctx{ scene, agent, bt, 0.016f };

	BehaviorTreeRunner runner(root);
	NodeStatus status = runner.Tick(ctx);
	assert(status == NodeStatus::Success);
	assert(bt.Status == BehaviorStatus::Success);
	assert(bt.BlackboardValue == 100.0f); // 88.0 + 12.0

	// 5. 验证 BehaviorTreeSystem::GetOrCreateTree 缓存驱动接口
	auto sysTree = BehaviorTreeSystem::GetOrCreateTree(tempPath);
	assert(sysTree != nullptr);
	auto cachedTree = BehaviorTreeSystem::GetOrCreateTree(tempPath);
	assert(cachedTree == sysTree);

	// 6. 验证默认模板 SetDefaultTemplate
	BehaviorTreeAsset defaultAsset;
	defaultAsset.SetDefaultTemplate();
	assert(defaultAsset.Nodes().size() == 6);
	assert(defaultAsset.BlackboardKeys().size() == 4);
	auto defaultRoot = defaultAsset.InstantiateTree();
	assert(defaultRoot != nullptr);

	std::error_code ec;
	std::filesystem::remove(tempPath, ec);

	std::remove(tempPath.c_str());
	std::cout << "[PASS] TestBehaviorTreeAssetSerializationAndInstantiation\n";
}

static void TestGenericActionsAndConditions()
{
	WorldContext context;
	Scene scene(context);

	Entity agent = Entity::CreateEntity(&scene, "GenericAgent");
	agent.AddComponent<TransformComponent>(glm::vec3(0.0f, 0.0f, 0.0f));
	agent.AddComponent<HealthComponent>(75.0f, 100.0f);
	auto& bt = agent.AddComponent<BehaviorTreeComponent>();

	BehaviorContext ctx{ scene, agent, bt, 0.016f };

	// 1. MoveToTarget Action 测试
	{
		bt.BlackboardTargetPos = glm::vec3(0.0f, 0.0f, 10.0f);
		BehaviorTreeNodeDesc moveDesc;
		moveDesc.Id = 1;
		moveDesc.ParentId = 0;
		moveDesc.Kind = BehaviorTreeNodeKind::Action;
		moveDesc.ActionType = "MoveToTarget";
		moveDesc.FloatParam = 4.0f; // 速度 4 m/s

		BehaviorTreeAsset asset;
		asset.Nodes().push_back(moveDesc);

		auto moveNode = asset.InstantiateTree();
		assert(moveNode != nullptr);

		// 第 1 步：Dt = 1.0s, step = 4.0m < 10.0m -> Running
		ctx.Dt = 1.0f;
		NodeStatus s1 = moveNode->Tick(ctx);
		assert(s1 == NodeStatus::Running);
		assert(bt.StateMachineState == 1);
		assert(std::abs(agent.GetComponent<TransformComponent>().Location.z - 4.0f) < 0.001f);

		// 第 2 步：Dt = 2.0s, step = 8.0m >= remaining 6.0m -> Success
		ctx.Dt = 2.0f;
		NodeStatus s2 = moveNode->Tick(ctx);
		assert(s2 == NodeStatus::Success);
		assert(std::abs(agent.GetComponent<TransformComponent>().Location.z - 10.0f) < 0.001f);
	}

	// 2. TargetInRange Condition 测试 (坐标与 TargetEntity 双支持)
	{
		BehaviorTreeNodeDesc condDesc;
		condDesc.Id = 1;
		condDesc.ParentId = 0;
		condDesc.Kind = BehaviorTreeNodeKind::Condition;
		condDesc.ConditionType = "TargetInRange";
		condDesc.FloatParam = 5.0f; // 范围 5m

		BehaviorTreeAsset asset;
		asset.Nodes().push_back(condDesc);
		auto condNode = asset.InstantiateTree();
		assert(condNode != nullptr);

		// 当前 Agent 位于 (0, 0, 10)
		// 目标点位于 (0, 0, 20) -> 距离 10m > 5m -> Failure
		bt.BlackboardTargetPos = glm::vec3(0.0f, 0.0f, 20.0f);
		bt.TargetEntity = 0;
		NodeStatus sFar = condNode->Tick(ctx);
		assert(sFar == NodeStatus::Failure);

		// 目标点位于 (0, 0, 12) -> 距离 2m <= 5m -> Success
		bt.BlackboardTargetPos = glm::vec3(0.0f, 0.0f, 12.0f);
		NodeStatus sNear = condNode->Tick(ctx);
		assert(sNear == NodeStatus::Success);

		// 通过 TargetEntity 检查
		Entity targetEnt = Entity::CreateEntity(&scene, "TargetDummy");
		targetEnt.AddComponent<TransformComponent>(glm::vec3(0.0f, 0.0f, 13.0f)); // 距离 3m
		bt.TargetEntity = static_cast<uint32_t>((entt::entity)targetEnt);

		NodeStatus sEntityNear = condNode->Tick(ctx);
		assert(sEntityNear == NodeStatus::Success);

		// 移动 TargetEntity 到远离处 (0, 0, 50) -> 距离 40m > 5m -> Failure
		targetEnt.GetComponent<TransformComponent>().Location = glm::vec3(0.0f, 0.0f, 50.0f);
		NodeStatus sEntityFar = condNode->Tick(ctx);
		assert(sEntityFar == NodeStatus::Failure);
	}

	// 3. SetBlackboardTag 与 BlackboardTagEquals 测试
	{
		BehaviorTreeAsset asset;
		BehaviorTreeNodeDesc seqDesc;
		seqDesc.Id = 1;
		seqDesc.ParentId = 0;
		seqDesc.Kind = BehaviorTreeNodeKind::Sequence;

		BehaviorTreeNodeDesc setTagDesc;
		setTagDesc.Id = 2;
		setTagDesc.ParentId = 1;
		setTagDesc.Kind = BehaviorTreeNodeKind::Action;
		setTagDesc.ActionType = "SetBlackboardTag";
		setTagDesc.FloatParam = 99.0f;

		BehaviorTreeNodeDesc checkTagDesc;
		checkTagDesc.Id = 3;
		checkTagDesc.ParentId = 1;
		checkTagDesc.Kind = BehaviorTreeNodeKind::Condition;
		checkTagDesc.ConditionType = "BlackboardTagEquals";
		checkTagDesc.FloatParam = 99.0f;

		asset.Nodes().push_back(seqDesc);
		asset.Nodes().push_back(setTagDesc);
		asset.Nodes().push_back(checkTagDesc);

		auto tree = asset.InstantiateTree();
		assert(tree != nullptr);

		bt.BlackboardTag = 0;
		NodeStatus s = tree->Tick(ctx);
		assert(s == NodeStatus::Success);
		assert(bt.BlackboardTag == 99);
	}

	// 4. HealthAbove 与 HealthBelow 测试
	{
		BehaviorTreeAsset asset;
		BehaviorTreeNodeDesc healthAboveDesc;
		healthAboveDesc.Id = 1;
		healthAboveDesc.ParentId = 0;
		healthAboveDesc.Kind = BehaviorTreeNodeKind::Condition;
		healthAboveDesc.ConditionType = "HealthAbove";
		healthAboveDesc.FloatParam = 70.0f; // Agent HP = 75.0 > 70.0 -> Success
		asset.Nodes().push_back(healthAboveDesc);

		auto tree = asset.InstantiateTree();
		assert(tree != nullptr);
		assert(tree->Tick(ctx) == NodeStatus::Success);

		// 阈值设为 80.0 -> 75.0 < 80.0 -> Failure
		asset.Nodes()[0].FloatParam = 80.0f;
		auto treeFail = asset.InstantiateTree();
		assert(treeFail->Tick(ctx) == NodeStatus::Failure);
	}

	// 5. ScriptAction 与 ScriptCondition 插槽测试
	{
		BehaviorTreeAsset asset;
		BehaviorTreeNodeDesc seqDesc;
		seqDesc.Id = 1;
		seqDesc.ParentId = 0;
		seqDesc.Kind = BehaviorTreeNodeKind::Sequence;

		BehaviorTreeNodeDesc scriptCondDesc;
		scriptCondDesc.Id = 2;
		scriptCondDesc.ParentId = 1;
		scriptCondDesc.Kind = BehaviorTreeNodeKind::Condition;
		scriptCondDesc.ConditionType = "ScriptCondition";
		scriptCondDesc.StringParam = "assets/scripts/ai/conditions/CustomCheck.luau";

		BehaviorTreeNodeDesc scriptActDesc;
		scriptActDesc.Id = 3;
		scriptActDesc.ParentId = 1;
		scriptActDesc.Kind = BehaviorTreeNodeKind::Action;
		scriptActDesc.ActionType = "ScriptAction";
		scriptActDesc.StringParam = "assets/scripts/ai/actions/CustomAction.luau";

		asset.Nodes().push_back(seqDesc);
		asset.Nodes().push_back(scriptCondDesc);
		asset.Nodes().push_back(scriptActDesc);

		auto tree = asset.InstantiateTree();
		assert(tree != nullptr);
		assert(tree->Tick(ctx) == NodeStatus::Success);
	}

	// 6. 标准类型字典查询静态方法测试
	{
		const auto& condTypes = BehaviorTreeAsset::GetStandardConditionTypes();
		assert(!condTypes.empty());
		assert(std::find(condTypes.begin(), condTypes.end(), "MoveToTarget") == condTypes.end());
		assert(std::find(condTypes.begin(), condTypes.end(), "TargetInRange") != condTypes.end());
		assert(std::find(condTypes.begin(), condTypes.end(), "ScriptCondition") != condTypes.end());

		const auto& actTypes = BehaviorTreeAsset::GetStandardActionTypes();
		assert(!actTypes.empty());
		assert(std::find(actTypes.begin(), actTypes.end(), "MoveToTarget") != actTypes.end());
		assert(std::find(actTypes.begin(), actTypes.end(), "SetBlackboardTag") != actTypes.end());
		assert(std::find(actTypes.begin(), actTypes.end(), "ScriptAction") != actTypes.end());

		const auto& bbTypes = BehaviorTreeAsset::GetStandardBlackboardTypes();
		assert(!bbTypes.empty());
		assert(std::find(bbTypes.begin(), bbTypes.end(), "Float") != bbTypes.end());
		assert(std::find(bbTypes.begin(), bbTypes.end(), "Vector3") != bbTypes.end());
	}

	std::cout << "[PASS] TestGenericActionsAndConditions\n";
}

static void TestNestedBehaviorTreeExecution()
{
	WorldContext context;
	Scene scene(context);

	Entity agent = Entity::CreateEntity(&scene, "AgentNested");
	agent.AddComponent<TransformComponent>();
	auto& bt = agent.AddComponent<BehaviorTreeComponent>();

	Blackboard board;
	BehaviorContext ctx{ scene, agent, bt, board, 0.02f };

	// 构造 3 层深度的树:
	// Root Selector:
	//   Child 0: Sequence 1:
	//     Step 0: Action A (Success)
	//     Step 1: Action B (Running for 2 ticks, then Success)
	//     Step 2: Action C (Success)
	//   Child 1: Sequence 2:
	//     Step 0: Action D (Fallback)
	auto rootSel = std::make_shared<SelectorNode>();
	auto seq1 = std::make_shared<SequenceNode>();
	auto seq2 = std::make_shared<SequenceNode>();

	int actionARuns = 0;
	int actionBRuns = 0;
	int actionCRuns = 0;
	int actionDRuns = 0;

	seq1->AddChild(std::make_shared<ActionNode>([&](BehaviorContext&) {
		++actionARuns;
		return NodeStatus::Success;
	}));

	float bTimer = 0.0f;
	seq1->AddChild(std::make_shared<ActionNode>([&](BehaviorContext& c) {
		++actionBRuns;
		bTimer += c.Dt;
		if (bTimer < 0.03f)
			return NodeStatus::Running;
		return NodeStatus::Success;
	}));

	seq1->AddChild(std::make_shared<ActionNode>([&](BehaviorContext&) {
		++actionCRuns;
		return NodeStatus::Success;
	}));

	seq2->AddChild(std::make_shared<ActionNode>([&](BehaviorContext&) {
		++actionDRuns;
		return NodeStatus::Success;
	}));

	rootSel->AddChild(seq1);
	rootSel->AddChild(seq2);

	BehaviorTreeRunner runner(rootSel);

	// Tick 1 (dt = 0.02s):
	// Root Sel -> Seq 1 -> Action A 成功 -> Action B (bTimer=0.02s < 0.03s) 返回 Running!
	NodeStatus s1 = runner.Tick(ctx);
	assert(s1 == NodeStatus::Running);
	assert(actionARuns == 1);
	assert(actionBRuns == 1);
	assert(actionCRuns == 0);
	assert(actionDRuns == 0);
	// 验证关键深度隔离：Root 处于深度 0，游标为 0；Seq1 处于深度 1，游标为 1！互不覆盖！
	assert(bt.GetPath(0) == 0);
	assert(bt.GetPath(1) == 1);

	// Tick 2 (dt = 0.02s):
	// 断点续跑：Root Sel 从 Seq 1 恢复，Seq 1 从 Action B 恢复执行 (bTimer=0.04s >= 0.03s) -> Success -> Action C 成功 -> Seq 1 成功 -> Root 成功！
	NodeStatus s2 = runner.Tick(ctx);
	assert(s2 == NodeStatus::Success);
	assert(actionARuns == 1); // Action A 绝对没有被重复执行！
	assert(actionBRuns == 2);
	assert(actionCRuns == 1);
	assert(actionDRuns == 0); // Seq 2 绝不执行！

	std::cout << "[PASS] TestNestedBehaviorTreeExecution (Depth > 1 verified)\n";
}

static void TestBlackboardDynamicTypesAndObserver()
{
	Blackboard board;

	uint32_t kThreat = board.RegisterKey("HasThreat", BlackboardKeyType::Bool, false);
	uint32_t kTargetPos = board.RegisterKey("TargetPos", BlackboardKeyType::Vector3, glm::vec3(0.0f));
	uint32_t kHealth = board.RegisterKey("Health", BlackboardKeyType::Float, 100.0f);

	assert(board.Get<bool>(kThreat) == false);
	assert(board.Get<float>(kHealth) == 100.0f);

	// 观察者注册测试
	int threatChangeCount = 0;
	bool observedThreat = false;
	uint32_t obsId = board.AddObserver(kThreat, [&](uint32_t, const BlackboardValue& newVal) {
		++threatChangeCount;
		observedThreat = std::get<bool>(newVal);
	});

	// 设置相同的值不触发观察者
	board.Set(kThreat, false);
	assert(threatChangeCount == 0);

	// 修改数值触发观察者
	board.Set(kThreat, true);
	assert(threatChangeCount == 1);
	assert(observedThreat == true);
	assert(board.IsKeyDirty(kThreat));

	// 字符串键名访问
	board.Set("TargetPos", glm::vec3(12.0f, 1.5f, -8.0f));
	glm::vec3 readPos = board.Get<glm::vec3>("TargetPos");
	assert(glm::distance(readPos, glm::vec3(12.0f, 1.5f, -8.0f)) < 0.001f);

	board.RemoveObserver(kThreat, obsId);
	board.Set(kThreat, false);
	assert(threatChangeCount == 1); // 已注销不再接收通知

	std::cout << "[PASS] TestBlackboardDynamicTypesAndObserver\n";
}

static void TestDecoratorFlowAbortSelf()
{
	WorldContext context;
	Scene scene(context);

	Entity agent = Entity::CreateEntity(&scene, "AgentDecorator");
	agent.AddComponent<TransformComponent>();
	auto& bt = agent.AddComponent<BehaviorTreeComponent>();

	Blackboard board;
	board.RegisterKey("HasThreat", BlackboardKeyType::Bool, true);

	BehaviorContext ctx{ scene, agent, bt, board, 0.016f };

	int actionTicks = 0;
	int abortCalls = 0;

	auto longAction = std::make_shared<ActionNode>(
		[&](BehaviorContext&) {
			++actionTicks;
			return NodeStatus::Running; // 模拟持续动作
		},
		[&](BehaviorContext&) {
			++abortCalls;
		}
	);

	auto decorator = std::make_shared<BlackboardConditionDecorator>(
		"HasThreat",
		ConditionOperator::Equal,
		true,
		longAction,
		FlowAbortMode::Self
	);

	BehaviorTreeRunner runner(decorator);

	// Frame 1: HasThreat == true，装饰器放行，Action 运行并返回 Running
	NodeStatus s1 = runner.Tick(ctx);
	assert(s1 == NodeStatus::Running);
	assert(actionTicks == 1);
	assert(abortCalls == 0);

	// 外部事件突变：黑板威胁解除
	board.Set("HasThreat", false);

	// Frame 2: 装饰器在条件不满足时立即打断子动作 (OnAbort) 并返回 Failure
	NodeStatus s2 = runner.Tick(ctx);
	assert(s2 == NodeStatus::Failure);
	assert(abortCalls == 1);

	std::cout << "[PASS] TestDecoratorFlowAbortSelf\n";
}

static void TestParallelNodeExecution()
{
	WorldContext context;
	Scene scene(context);

	Entity agent = Entity::CreateEntity(&scene, "AgentParallel");
	agent.AddComponent<TransformComponent>();
	auto& bt = agent.AddComponent<BehaviorTreeComponent>();

	BehaviorContext ctx{ scene, agent, bt, 0.016f };

	auto par = std::make_shared<ParallelNode>();
	int action1Runs = 0;
	int action2Runs = 0;

	par->AddChild(std::make_shared<ActionNode>([&](BehaviorContext&) {
		++action1Runs;
		return NodeStatus::Success;
	}));
	par->AddChild(std::make_shared<ActionNode>([&](BehaviorContext&) {
		++action2Runs;
		return NodeStatus::Success;
	}));

	BehaviorTreeRunner runner(par);
	NodeStatus s = runner.Tick(ctx);
	assert(s == NodeStatus::Success);
	assert(action1Runs == 1);
	assert(action2Runs == 1);

	std::cout << "[PASS] TestParallelNodeExecution\n";
}

static void TestAsyncEnvQueryManagerAndReservation()
{
	using namespace World::EQS;

	auto& mgr = EnvQueryManager::Get();
	mgr.Clear();

	// 1. 掩体预定池测试
	const glm::vec3 coverPos(10.0f, 0.0f, 5.0f);
	uint32_t ai1 = 101;
	uint32_t ai2 = 102;

	mgr.GetReservationPool().ReserveCover(coverPos, ai1, 3.0f);
	// 验证已经被 ai1 锁定，ai2 无法重复选择
	assert(mgr.GetReservationPool().IsCoverReserved(coverPos, ai2, 1.5f) == true);
	// 自身查询不受限制
	assert(mgr.GetReservationPool().IsCoverReserved(coverPos, ai1, 1.5f) == false);

	// 释放掩体
	mgr.GetReservationPool().ReleaseCover(coverPos, ai1);
	assert(mgr.GetReservationPool().IsCoverReserved(coverPos, ai2, 1.5f) == false);

	// 2. 异步时间切片管理器测试
	std::vector<CoverBox> obstacles = {
		{ { 8.0f, 0.0f, -5.0f }, { 12.0f, 4.0f, 5.0f } }
	};
	auto query = EQSCoverSystem::BuildPresetCoverQuery();

	EQSQueryContext ctx;
	ctx.QuerierPos = glm::vec3(20.0f, 0.0f, 0.0f);
	ctx.ThreatPos = glm::vec3(0.0f, 0.0f, 0.0f);
	ctx.QueryRadius = 15.0f;
	ctx.GridSpacing = 1.0f;
	ctx.Obstacles = obstacles;

	bool queryInvoked = false;
	glm::vec3 asyncBestPos(0.0f);
	float asyncBestScore = -1.0f;

	uint32_t reqId = mgr.RequestQuery(query, ctx, [&](const EQSQueryResult& res) {
		queryInvoked = true;
		asyncBestPos = res.BestPosition;
		asyncBestScore = res.BestScore;
	});

	assert(reqId > 0);
	assert(mgr.GetPendingCount() == 1);
	assert(!queryInvoked);

	// 推进单帧切片调度 (给定 10ms 充足预算)
	mgr.Update(0.016f, 10.0f);

	assert(queryInvoked);
	assert(mgr.GetPendingCount() == 0);
	assert(asyncBestScore > 1.5f);
	assert(asyncBestPos.x >= 12.0f); // 掩体位于墙后

	std::cout << "[PASS] TestAsyncEnvQueryManagerAndReservation\n";
}

static void TestDecoratorFlowAbortLowerPriority()
{
	WorldContext context;
	Scene scene(context);

	Entity agent = Entity::CreateEntity(&scene, "AgentLowerPriority");
	agent.AddComponent<TransformComponent>();
	auto& bt = agent.AddComponent<BehaviorTreeComponent>();

	Blackboard board;
	board.RegisterKey("HasThreat", BlackboardKeyType::Bool, false);

	BehaviorContext ctx{ scene, agent, bt, board, 0.016f };

	int combatTicks = 0;
	int combatAborts = 0;
	int patrolTicks = 0;
	int patrolAborts = 0;

	auto combatAction = std::make_shared<ActionNode>(
		[&](BehaviorContext&) {
			++combatTicks;
			return NodeStatus::Running;
		},
		[&](BehaviorContext&) {
			++combatAborts;
		}
	);

	auto combatDecorator = std::make_shared<BlackboardConditionDecorator>(
		"HasThreat",
		ConditionOperator::Equal,
		true,
		combatAction,
		FlowAbortMode::Both
	);

	auto patrolAction = std::make_shared<ActionNode>(
		[&](BehaviorContext&) {
			++patrolTicks;
			return NodeStatus::Running;
		},
		[&](BehaviorContext&) {
			++patrolAborts;
		}
	);

	auto rootSelector = std::make_shared<SelectorNode>();
	rootSelector->AddChild(combatDecorator); // Index 0 (High Priority)
	rootSelector->AddChild(patrolAction);   // Index 1 (Low Priority)

	BehaviorTreeRunner runner(rootSelector);

	// Frame 1: HasThreat == false，分支 0 失败，分支 1 (Patrol) 运行
	NodeStatus s1 = runner.Tick(ctx);
	assert(s1 == NodeStatus::Running);
	assert(patrolTicks == 1);
	assert(combatTicks == 0);
	assert(bt.GetPath(0) == 1); // 正在运行分支 1

	// 外部事件突变：黑板检测到威胁 -> 触发 LowerPriority 抢占打断！
	board.Set("HasThreat", true);
	assert(bt.AbortRequested == 1); // 观察者立即请求打断低优先级分支

	// Frame 2: 执行打断后，重新从 Root 开始评估，高优先级 Combat 分支抢占执行
	NodeStatus s2 = runner.Tick(ctx);
	assert(s2 == NodeStatus::Running);
	assert(patrolAborts == 1);   // Patrol 被即时打断
	assert(combatTicks == 1);   // Combat 开始运行
	assert(bt.GetPath(0) == 0); // 正在运行分支 0

	// 外部事件突变：威胁消除 -> 触发 Self 打断！
	board.Set("HasThreat", false);
	assert(bt.AbortRequested == 1);

	// Frame 3: 执行 Self 打断，重新回退到低优先级 Patrol 分支
	NodeStatus s3 = runner.Tick(ctx);
	assert(s3 == NodeStatus::Running);
	assert(combatAborts == 1);  // Combat 被即时打断
	assert(patrolTicks == 2);   // Patrol 恢复运行
	assert(bt.GetPath(0) == 1);

	std::cout << "[PASS] TestDecoratorFlowAbortLowerPriority\n";
}

static void TestLatentAsyncEQSAndLocomotion()
{
	using namespace World::EQS;

	WorldContext context;
	Scene scene(context);

	Entity agent = Entity::CreateEntity(&scene, "AgentAsyncEQS");
	agent.AddComponent<TransformComponent>(glm::vec3(0.0f, 0.0f, 0.0f));
	auto& nav = agent.AddComponent<NavMeshAgentComponent>();
	nav.Speed = 3.5f;
	auto& bt = agent.AddComponent<BehaviorTreeComponent>();

	Blackboard board;
	board.RegisterKey("CoverPos", BlackboardKeyType::Vector3, glm::vec3(0.0f));
	board.RegisterKey("TargetPos", BlackboardKeyType::Vector3, glm::vec3(0.0f));
	board.RegisterKey("EQSQueryPending", BlackboardKeyType::Bool, false);

	BehaviorContext ctx{ scene, agent, bt, board, 0.016f };

	// 构造环境障碍物与查询请求
	std::vector<CoverBox> obstacles = {
		{ { 8.0f, 0.0f, -5.0f }, { 12.0f, 4.0f, 5.0f } }
	};
	auto query = EQSCoverSystem::BuildPresetCoverQuery();
	EQSQueryContext queryCtx;
	queryCtx.QuerierPos = glm::vec3(20.0f, 0.0f, 0.0f);
	queryCtx.ThreatPos = glm::vec3(0.0f, 0.0f, 0.0f);
	queryCtx.QueryRadius = 15.0f;
	queryCtx.GridSpacing = 1.0f;
	queryCtx.Obstacles = obstacles;

	auto seq = std::make_shared<SequenceNode>();

	// 节点 0: 异步潜伏 EQS 任务
	uint32_t activeReq = 0;
	auto eqsTask = std::make_shared<ActionNode>(
		[&](BehaviorContext& c) {
			if (activeReq == 0)
			{
				activeReq = EnvQueryManager::Get().RequestQuery(
					query, queryCtx,
					[&](const EQSQueryResult& res) {
						if (res.Success)
						{
							board.Set("CoverPos", res.BestPosition);
							board.Set("TargetPos", res.BestPosition);
							board.Set("EQSQueryPending", false);
						}
					}
				);
				board.Set("EQSQueryPending", true);
				return NodeStatus::Running;
			}

			if (board.Get<bool>("EQSQueryPending", true))
			{
				return NodeStatus::Running; // 异步运算中，保持挂起
			}

			activeReq = 0;
			return NodeStatus::Success; // 运算完成，通知放行
		},
		[&](BehaviorContext&) {
			if (activeReq != 0)
			{
				EnvQueryManager::Get().CancelRequest(activeReq);
				activeReq = 0;
			}
		}
	);

	// 节点 1: 紧接着的 MoveToTarget 动作，纯粹驱动 NavMeshAgentComponent
	auto moveTask = std::make_shared<ActionNode>(
		[&](BehaviorContext& c) {
			glm::vec3 dest = c.Board.Get<glm::vec3>("CoverPos");
			nav.Destination = dest;
			nav.AutoRepath = 1;
			if (glm::distance(agent.GetComponent<TransformComponent>().Location, dest) < 0.1f)
			{
				return NodeStatus::Success;
			}
			return NodeStatus::Running;
		}
	);

	seq->AddChild(eqsTask);
	seq->AddChild(moveTask);

	BehaviorTreeRunner runner(seq);

	// Frame 1: EQS 任务发起异步请求，返回 Running
	NodeStatus s1 = runner.Tick(ctx);
	assert(s1 == NodeStatus::Running);
	assert(board.Get<bool>("EQSQueryPending") == true);
	assert(activeReq > 0);
	assert(bt.GetPath(0) == 0); // 游标停在节点 0

	// 模拟引擎主线程推进时间切片 (EnvQueryManager::Update)
	EnvQueryManager::Get().Update(0.016f, 5.0f);
	assert(board.Get<bool>("EQSQueryPending") == false);
	glm::vec3 foundCover = board.Get<glm::vec3>("CoverPos");
	assert(foundCover.x >= 12.0f); // 成功算出掩体位置

	// Frame 2: EQS 任务完成并退出，Sequence 推进到节点 1 (MoveTo)，NavMeshAgent 接收到目标点
	NodeStatus s2 = runner.Tick(ctx);
	assert(s2 == NodeStatus::Running);
	assert(bt.GetPath(0) == 1); // 游标推进至节点 1
	assert(glm::distance(nav.Destination, foundCover) < 0.001f); // 绝无硬修改 Location，走正规寻路目标

	std::cout << "[PASS] TestLatentAsyncEQSAndLocomotion\n";
}

static void TestMultiEntityIsolationAndReactiveAborts()
{
	WorldContext context;
	Scene scene(context);

	Entity agentA = Entity::CreateEntity(&scene, "AgentA");
	agentA.AddComponent<TransformComponent>(glm::vec3(0.0f, 0.0f, 0.0f));
	auto& btA = agentA.AddComponent<BehaviorTreeComponent>();
	btA.TreeAssetId = 99;
	btA.Enabled = 1;

	Entity agentB = Entity::CreateEntity(&scene, "AgentB");
	agentB.AddComponent<TransformComponent>(glm::vec3(10.0f, 0.0f, 0.0f));
	auto& btB = agentB.AddComponent<BehaviorTreeComponent>();
	btB.TreeAssetId = 99;
	btB.Enabled = 1;

	auto combatAction = std::make_shared<ActionNode>([](BehaviorContext& c) {
		c.Component.BlackboardValue = 100.0f;
		return NodeStatus::Running;
	});
	auto patrolAction = std::make_shared<ActionNode>([](BehaviorContext& c) {
		c.Component.BlackboardValue = 10.0f;
		return NodeStatus::Running;
	});

	auto combatDecorator = std::make_shared<BlackboardConditionDecorator>(
		"HasThreat",
		ConditionOperator::Equal,
		true,
		combatAction,
		FlowAbortMode::LowerPriority
	);

	auto rootSelector = std::make_shared<SelectorNode>();
	rootSelector->AddChild(combatDecorator); // Index 0 (High Priority: Combat)
	rootSelector->AddChild(patrolAction);   // Index 1 (Low Priority: Patrol)

	BehaviorTreeSystem::ClearTrees();
	BehaviorTreeSystem::ClearEntityRunners();
	BehaviorTreeSystem::RegisterTree(99, rootSelector);

	auto boardA = BlackboardRegistry::Get().GetOrCreateBlackboard(static_cast<uint32_t>((entt::entity)agentA));
	auto boardB = BlackboardRegistry::Get().GetOrCreateBlackboard(static_cast<uint32_t>((entt::entity)agentB));
	boardA->Set("HasThreat", false);
	boardB->Set("HasThreat", false);

	// Frame 1: 两者都应运行 Patrol (值 = 10.0f, 路径 = 1)
	BehaviorTreeSystem::Update(scene, 0.016f);
	assert(btA.Status == BehaviorStatus::Running);
	assert(btB.Status == BehaviorStatus::Running);
	assert(btA.GetPath(0) == 1);
	assert(btB.GetPath(0) == 1);
	assert(btA.BlackboardValue == 10.0f);
	assert(btB.BlackboardValue == 10.0f);

	// 外部突变：仅实体 A 遭遇威胁！
	boardA->Set("HasThreat", true);
	// 验证：仅实体 A 请求了打断，实体 B 毫发无损！
	assert(btA.AbortRequested == 1);
	assert(btB.AbortRequested == 0);

	// Frame 2: 实体 A 打断切入 Combat (值 = 100.0f, 路径 = 0)，实体 B 继续保持 Patrol！
	BehaviorTreeSystem::Update(scene, 0.016f);
	assert(btA.GetPath(0) == 0);
	assert(btA.BlackboardValue == 100.0f);
	assert(btB.GetPath(0) == 1);
	assert(btB.BlackboardValue == 10.0f);

	// 外部突变：实体 B 遭遇威胁！
	boardB->Set("HasThreat", true);
	assert(btB.AbortRequested == 1);

	// Frame 3: 实体 B 也切入 Combat，两者互不干扰！
	BehaviorTreeSystem::Update(scene, 0.016f);
	assert(btA.GetPath(0) == 0);
	assert(btB.GetPath(0) == 0);
	assert(btA.BlackboardValue == 100.0f);
	assert(btB.BlackboardValue == 100.0f);

	BehaviorTreeSystem::ClearTrees();
	BehaviorTreeSystem::ClearEntityRunners();
	std::cout << "[PASS] TestMultiEntityIsolationAndReactiveAborts\n";
}

static void TestConditionOperatorSerializationRoundTrip()
{
	BehaviorTreeAsset asset;
	asset.BlackboardKeys().push_back({ "ThreatCount", "Int", "0" });

	BehaviorTreeNodeDesc decNode;
	decNode.Id = 1;
	decNode.ParentId = 0;
	decNode.Kind = BehaviorTreeNodeKind::Decorator;
	decNode.Title = "Threat Greater Check";
	decNode.StringParam = "ThreatCount";
	decNode.CompOp = ConditionOperator::Greater;
	decNode.FloatParam = 3.0f;
	decNode.AbortMode = FlowAbortMode::LowerPriority;

	BehaviorTreeNodeDesc actNode;
	actNode.Id = 2;
	actNode.ParentId = 1;
	actNode.Kind = BehaviorTreeNodeKind::Action;
	actNode.Title = "Fallback Action";
	actNode.ActionType = "Wait";
	actNode.FloatParam = 1.0f;

	asset.Nodes().push_back(decNode);
	asset.Nodes().push_back(actNode);

	const std::string testFile = "build_test_comp_op.wbt";
	std::string err;
	bool saved = asset.SaveToFile(testFile, &err);
	assert(saved);

	BehaviorTreeAsset loaded;
	bool loadedOk = loaded.LoadFromFile(testFile, &err);
	assert(loadedOk);
	assert(loaded.Nodes().size() == 2);
	assert(loaded.Nodes()[0].CompOp == ConditionOperator::Greater);
	assert(loaded.Nodes()[0].AbortMode == FlowAbortMode::LowerPriority);
	assert(loaded.Nodes()[0].StringParam == "ThreatCount");

	std::remove(testFile.c_str());
	std::cout << "[PASS] TestConditionOperatorSerializationRoundTrip\n";
}

static void TestTypeAgnosticBlackboardConditionComparisons()
{
	Blackboard board;
	board.RegisterKey("IntKey", BlackboardKeyType::Int32, int32_t(10));
	board.RegisterKey("BoolKey", BlackboardKeyType::Bool, true);
	board.RegisterKey("FloatKey", BlackboardKeyType::Float, 5.5f);

	// 1. Equal 对比：int32(10) vs float(10.0f)
	BlackboardConditionDecorator decIntVsFloat("IntKey", ConditionOperator::Equal, 10.0f);
	assert(decIntVsFloat.EvaluateConditionOnBoard(board) == true);

	// 2. Greater 对比：int32(10) > float(5.0f)
	BlackboardConditionDecorator decGreater("IntKey", ConditionOperator::Greater, 5.0f);
	assert(decGreater.EvaluateConditionOnBoard(board) == true);

	// 3. Less 对比：int32(10) < float(20.0f)
	BlackboardConditionDecorator decLess("IntKey", ConditionOperator::Less, 20.0f);
	assert(decLess.EvaluateConditionOnBoard(board) == true);

	// 4. Bool 对比：bool(true) vs float(1.0f)
	BlackboardConditionDecorator decBool("BoolKey", ConditionOperator::Equal, 1.0f);
	assert(decBool.EvaluateConditionOnBoard(board) == true);

	// 5. NotEqual 对比：bool(true) != float(0.0f)
	BlackboardConditionDecorator decBoolNotEq("BoolKey", ConditionOperator::NotEqual, 0.0f);
	assert(decBoolNotEq.EvaluateConditionOnBoard(board) == true);

	std::cout << "[PASS] TestTypeAgnosticBlackboardConditionComparisons\n";
}

static void TestCoverReservationPoolNoTheft()
{
	using namespace World::EQS;
	auto& pool = EnvQueryManager::Get().GetReservationPool();
	pool.Clear();

	const glm::vec3 coverSpot(15.0f, 0.0f, 15.0f);
	uint32_t agentA = 101;
	uint32_t agentB = 102;

	// Agent A 成功预定掩体
	bool okA = pool.ReserveCover(coverSpot, agentA, 5.0f);
	assert(okA == true);
	assert(pool.IsCoverReserved(coverSpot, agentA) == false); // 对 A 自身不排斥
	assert(pool.IsCoverReserved(coverSpot, agentB) == true);  // 对 B 产生遮蔽排斥！

	// Agent B 试图抢占同一个掩体点位（距离 0.5m 内）-> 必须被拒绝（防抢占机制生效）！
	const glm::vec3 adjacentSpot(15.3f, 0.0f, 15.2f);
	bool okB = pool.ReserveCover(adjacentSpot, agentB, 5.0f);
	assert(okB == false);

	// Agent A 可以正常刷新自己的租约
	bool okA2 = pool.ReserveCover(coverSpot, agentA, 8.0f);
	assert(okA2 == true);

	// Agent B 可以在相距 3.0m 以外的合法位置预定自己的掩体
	const glm::vec3 distinctSpot(18.5f, 0.0f, 15.0f);
	bool okB2 = pool.ReserveCover(distinctSpot, agentB, 5.0f);
	assert(okB2 == true);

	pool.Clear();
	std::cout << "[PASS] TestCoverReservationPoolNoTheft\n";
}

static void TestAsyncRunEQSQueryAndTacticalActions()
{
	using namespace World::EQS;
	WorldContext context;
	Scene scene(context);

	Entity wall = Entity::CreateEntity(&scene, "CoverWall");
	auto& wTr = wall.AddComponent<TransformComponent>(glm::vec3(10.0f, 0.0f, 0.0f));
	auto& col = wall.AddComponent<BoxCollider3DComponent>();
	col.HalfExtents = glm::vec3(1.0f, 2.0f, 3.0f);

	Entity threat = Entity::CreateEntity(&scene, "Threat");
	threat.AddComponent<TransformComponent>(glm::vec3(0.0f, 0.0f, 0.0f));

	Entity agent = Entity::CreateEntity(&scene, "TacticalAgent");
	agent.AddComponent<TransformComponent>(glm::vec3(20.0f, 0.0f, 0.0f));
	auto& nav = agent.AddComponent<NavMeshAgentComponent>();
	nav.Speed = 4.0f;
	auto& bt = agent.AddComponent<BehaviorTreeComponent>();

	auto board = BlackboardRegistry::Get().GetOrCreateBlackboard(static_cast<uint32_t>((entt::entity)agent));
	BehaviorContext ctx{ scene, agent, bt, *board, 0.016f };

	BehaviorTreeAsset asset;
	BehaviorTreeNodeDesc rootSeq;
	rootSeq.Id = 1; rootSeq.ParentId = 0; rootSeq.Kind = BehaviorTreeNodeKind::Sequence; rootSeq.Title = "Tactical Sequence";

	BehaviorTreeNodeDesc eqsAct;
	eqsAct.Id = 2; eqsAct.ParentId = 1; eqsAct.Kind = BehaviorTreeNodeKind::Action; eqsAct.Title = "Query Cover";
	eqsAct.ActionType = "RunEQSQuery"; eqsAct.StringParam = "Cover"; eqsAct.FloatParam = 20.0f;

	BehaviorTreeNodeDesc moveAct;
	moveAct.Id = 3; moveAct.ParentId = 1; moveAct.Kind = BehaviorTreeNodeKind::Action; moveAct.Title = "Move Cover";
	moveAct.ActionType = "MoveToEQSPosition"; moveAct.FloatParam = 4.0f;

	asset.Nodes().push_back(rootSeq);
	asset.Nodes().push_back(eqsAct);
	asset.Nodes().push_back(moveAct);

	auto tree = asset.InstantiateTree();
	assert(tree != nullptr);

	BehaviorTreeRunner runner(tree);

	// Frame 1: 发起异步 EQS 查询，动作挂起返回 Running
	NodeStatus s1 = runner.Tick(ctx);
	assert(s1 == NodeStatus::Running);
	assert(board->Get<bool>("EQSQueryPending") == true);
	assert(bt.GetPath(0) == 0);

	// 推进异步调度器时间片
	EnvQueryManager::Get().Update(0.016f, 5.0f);
	assert(board->Get<bool>("EQSQueryPending") == false);
	assert(board->Get<bool>("EQSQueryCompleted") == true);

	// Frame 2: EQS 查询完成并返回 Success，Sequence 推进至节点 1 (MoveToEQSPosition)
	NodeStatus s2 = runner.Tick(ctx);
	assert(s2 == NodeStatus::Running);
	assert(bt.GetPath(0) == 1);
	assert(board->HasKey("CoverPos") == true);
	glm::vec3 coverPos = board->Get<glm::vec3>("CoverPos");
	assert(coverPos.x >= 10.0f);
	assert(glm::distance(nav.Destination, coverPos) < 0.001f);

	std::cout << "[PASS] TestAsyncRunEQSQueryAndTacticalActions\n";
}

static void TestDecoupledBlackboardAssetLinkage()
{
	// 1. 创建独立黑板资产 (.wbb)
	BlackboardAsset bbAsset;
	bbAsset.AddKey({ "SharedAmmo", BlackboardKeyType::Int32, int32_t(30) });
	bbAsset.AddKey({ "CombatAlert", BlackboardKeyType::Bool, true });

	const std::string bbPath = "build_test_shared.wbb";
	std::string err;
	bool savedBb = bbAsset.SaveToFile(bbPath, &err);
	assert(savedBb);

	// 2. 创建行为树资产 (.wbt) 并引用该独立黑板
	BehaviorTreeAsset btAsset;
	btAsset.SetBlackboardAssetPath(bbPath);

	BehaviorTreeNodeDesc rootNode;
	rootNode.Id = 1; rootNode.ParentId = 0; rootNode.Kind = BehaviorTreeNodeKind::Selector; rootNode.Title = "Root";
	btAsset.Nodes().push_back(rootNode);

	const std::string btPath = "build_test_linked.wbt";
	bool savedBt = btAsset.SaveToFile(btPath, &err);
	assert(savedBt);

	// 3. 反序列化行为树资产并验证黑板引用与动态实例化
	BehaviorTreeAsset loadedBt;
	bool loaded = loadedBt.LoadFromFile(btPath, &err);
	assert(loaded);
	assert(loadedBt.GetBlackboardAssetPath() == bbPath);

	auto boardInstance = loadedBt.InstantiateBlackboard();
	assert(boardInstance != nullptr);
	assert(boardInstance->HasKey("SharedAmmo") == true);
	assert(boardInstance->Get<int32_t>("SharedAmmo") == 30);
	assert(boardInstance->HasKey("CombatAlert") == true);
	assert(boardInstance->Get<bool>("CombatAlert") == true);

	std::remove(bbPath.c_str());
	std::remove(btPath.c_str());
	std::cout << "[PASS] TestDecoupledBlackboardAssetLinkage\n";
}

static void TestBlackboardExtendedTypesAndArrays()
{
	Blackboard board;

	// 1. 测试扩展标量类型 (Double, Vector2, Vector4)
	board.RegisterKey("PrecisionTime", BlackboardKeyType::Double, 123456.789012);
	board.RegisterKey("ScreenCoord", BlackboardKeyType::Vector2, glm::vec2(1920.0f, 1080.0f));
	board.RegisterKey("FacingQuat", BlackboardKeyType::Vector4, glm::vec4(0.0f, 1.0f, 0.0f, 0.0f));

	assert(std::abs(board.Get<double>("PrecisionTime") - 123456.789012) < 1e-5);
	assert(board.Get<glm::vec2>("ScreenCoord") == glm::vec2(1920.0f, 1080.0f));
	assert(board.Get<glm::vec4>("FacingQuat") == glm::vec4(0.0f, 1.0f, 0.0f, 0.0f));

	// 2. 测试 5 种核心数组类型 (ArrayVector3, ArrayEntity, ArrayFloat, ArrayInt32, ArrayString)
	board.RegisterKey("Waypoints", BlackboardKeyType::ArrayVector3, std::vector<glm::vec3>{ { 1.0f, 0.0f, 1.0f }, { 5.0f, 0.0f, 5.0f } });
	board.RegisterKey("PerceivedThreats", BlackboardKeyType::ArrayEntity, std::vector<uint32_t>{ 101, 102, 103 });
	board.RegisterKey("DamageHistory", BlackboardKeyType::ArrayFloat, std::vector<float>{ 12.5f, 25.0f });
	board.RegisterKey("ItemIds", BlackboardKeyType::ArrayInt32, std::vector<int32_t>{ 7, 8, 9 });
	board.RegisterKey("Tags", BlackboardKeyType::ArrayString, std::vector<std::string>{ "Patrol", "Alert" });

	assert(board.IsArrayKey("Waypoints") == true);
	assert(board.IsArrayKey("PerceivedThreats") == true);
	assert(board.IsArrayKey("PrecisionTime") == false);

	assert(board.GetArraySize("Waypoints") == 2);
	assert(board.GetArraySize("PerceivedThreats") == 3);
	assert(board.GetArraySize("DamageHistory") == 2);
	assert(board.GetArraySize("ItemIds") == 3);
	assert(board.GetArraySize("Tags") == 2);

	// 3. 测试数组追加元素 (PushArrayElement) 与观察者通知
	int observerCalls = 0;
	uint32_t wpKeyId = board.GetKeyId("Waypoints");
	board.AddObserver(wpKeyId, [&](uint32_t, const BlackboardValue&) {
		++observerCalls;
	});

	bool pushed = board.PushArrayElement<glm::vec3>("Waypoints", glm::vec3(10.0f, 0.0f, 10.0f));
	assert(pushed == true);
	assert(board.GetArraySize("Waypoints") == 3);
	assert(observerCalls == 1);
	auto wpVec = board.Get<std::vector<glm::vec3>>("Waypoints");
	assert(wpVec.size() == 3);
	assert(wpVec[2] == glm::vec3(10.0f, 0.0f, 10.0f));

	// 4. 测试清空数组 (ClearArray)
	bool cleared = board.ClearArray("Waypoints");
	assert(cleared == true);
	assert(board.GetArraySize("Waypoints") == 0);
	assert(observerCalls == 2);

	// 5. 测试 BlackboardAsset 资产序列化与反序列化完整往返
	BlackboardAsset asset;
	asset.AddKey({ "PrecisionTime", BlackboardKeyType::Double, 999.888 });
	asset.AddKey({ "ScreenPos", BlackboardKeyType::Vector2, glm::vec2(256.0f, 512.0f) });
	asset.AddKey({ "Quat", BlackboardKeyType::Vector4, glm::vec4(0.0f, 0.707f, 0.0f, 0.707f) });
	asset.AddKey({ "Waypoints", BlackboardKeyType::ArrayVector3, std::vector<glm::vec3>{ { 2.0f, 0.0f, 2.0f }, { 4.0f, 0.0f, 4.0f } } });
	asset.AddKey({ "Threats", BlackboardKeyType::ArrayEntity, std::vector<uint32_t>{ 888, 999 } });
	asset.AddKey({ "Dialogues", BlackboardKeyType::ArrayString, std::vector<std::string>{ "Stop!", "Freeze!" } });

	const std::string bbTestFile = "build_test_arrays.wbb";
	std::string err;
	bool saved = asset.SaveToFile(bbTestFile, &err);
	assert(saved);

	BlackboardAsset loadedAsset;
	bool loaded = loadedAsset.LoadFromFile(bbTestFile, &err);
	assert(loaded);
	assert(loadedAsset.GetKeys().size() == 6);

	auto instantiatedBoard = loadedAsset.Instantiate();
	assert(instantiatedBoard != nullptr);
	assert(instantiatedBoard->GetArraySize("Waypoints") == 2);
	assert(instantiatedBoard->GetArraySize("Threats") == 2);
	assert(instantiatedBoard->GetArraySize("Dialogues") == 2);
	assert(instantiatedBoard->Get<glm::vec2>("ScreenPos") == glm::vec2(256.0f, 512.0f));

	std::remove(bbTestFile.c_str());
	std::cout << "[PASS] TestBlackboardExtendedTypesAndArrays\n";
}

static void TestBehaviorTreeConditionOnArrays()
{
	Blackboard board;
	board.RegisterKey("PerceivedThreats", BlackboardKeyType::ArrayEntity, std::vector<uint32_t>{});

	// 1. 当数组为空时，IsNotSet 判定成立，IsSet 判定失败
	BlackboardConditionDecorator decEmptyIsNotSet("PerceivedThreats", ConditionOperator::IsNotSet);
	assert(decEmptyIsNotSet.EvaluateConditionOnBoard(board) == true);

	BlackboardConditionDecorator decEmptyIsSet("PerceivedThreats", ConditionOperator::IsSet);
	assert(decEmptyIsSet.EvaluateConditionOnBoard(board) == false);

	// 2. 注入 3 个威胁实体
	board.Set("PerceivedThreats", std::vector<uint32_t>{ 1001, 1002, 1003 });

	// 3. 数组非空判定成立
	BlackboardConditionDecorator decThreatsIsSet("PerceivedThreats", ConditionOperator::IsSet);
	assert(decThreatsIsSet.EvaluateConditionOnBoard(board) == true);

	// 4. 数组元素个数对比：Greater 2 (3 > 2 判定成立)
	BlackboardConditionDecorator decThreatsGreater("PerceivedThreats", ConditionOperator::Greater, 2.0f);
	assert(decThreatsGreater.EvaluateConditionOnBoard(board) == true);

	// 5. 数组元素个数对比：Equal 3 (3 == 3 判定成立)
	BlackboardConditionDecorator decThreatsEqual("PerceivedThreats", ConditionOperator::Equal, 3.0f);
	assert(decThreatsEqual.EvaluateConditionOnBoard(board) == true);

	// 6. 数组元素个数对比：Less 2 (3 < 2 判定失败)
	BlackboardConditionDecorator decThreatsLess("PerceivedThreats", ConditionOperator::Less, 2.0f);
	assert(decThreatsLess.EvaluateConditionOnBoard(board) == false);

	std::cout << "[PASS] TestBehaviorTreeConditionOnArrays\n";
}

int main()
{
	Log::Init();
	std::cout << "Running WorldBehaviorTreeTests..." << std::endl;

	std::cout << "--- Running 1 ---" << std::endl; TestSequenceAndSelectorBasic();
	std::cout << "--- Running 2 ---" << std::endl; TestSequenceRunningAndResume();
	std::cout << "--- Running 3 ---" << std::endl; TestSelectorRunningAndResume();
	std::cout << "--- Running 4 ---" << std::endl; TestBehaviorTreeSystemBatchExecution();
	std::cout << "--- Running 5 ---" << std::endl; TestBehaviorTreeAssetSerializationAndInstantiation();
	std::cout << "--- Running 6 ---" << std::endl; TestGenericActionsAndConditions();
	std::cout << "--- Running 7 ---" << std::endl; TestNestedBehaviorTreeExecution();
	std::cout << "--- Running 8 ---" << std::endl; TestBlackboardDynamicTypesAndObserver();
	std::cout << "--- Running 9 ---" << std::endl; TestDecoratorFlowAbortSelf();
	std::cout << "--- Running 10 ---" << std::endl; TestDecoratorFlowAbortLowerPriority();
	std::cout << "--- Running 11 ---" << std::endl; TestLatentAsyncEQSAndLocomotion();
	std::cout << "--- Running 12 ---" << std::endl; TestParallelNodeExecution();
	std::cout << "--- Running 13 ---" << std::endl; TestAsyncEnvQueryManagerAndReservation();
	std::cout << "--- Running 14 ---" << std::endl; TestMultiEntityIsolationAndReactiveAborts();
	std::cout << "--- Running 15 ---" << std::endl; TestConditionOperatorSerializationRoundTrip();
	std::cout << "--- Running 16 ---" << std::endl; TestTypeAgnosticBlackboardConditionComparisons();
	std::cout << "--- Running 17 ---" << std::endl; TestCoverReservationPoolNoTheft();
	std::cout << "--- Running 18 ---" << std::endl; TestAsyncRunEQSQueryAndTacticalActions();
	std::cout << "--- Running 19 ---" << std::endl; TestDecoupledBlackboardAssetLinkage();
	std::cout << "--- Running 20 ---" << std::endl; TestBlackboardExtendedTypesAndArrays();
	std::cout << "--- Running 21 ---" << std::endl; TestBehaviorTreeConditionOnArrays();
	std::cout << "All passed!" << std::endl;
	return 0;
}