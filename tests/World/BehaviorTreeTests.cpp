#include "World/Gameplay/Framework/BehaviorTree.h"
#include "World/Gameplay/Framework/BehaviorTreeAsset.h"
#include "World/Gameplay/Framework/BehaviorTreeSystem.h"
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

int main()
{
	Log::Init();
	std::cout << "Running WorldBehaviorTreeTests...\n";

	TestSequenceAndSelectorBasic();
	TestSequenceRunningAndResume();
	TestSelectorRunningAndResume();
	TestBehaviorTreeSystemBatchExecution();
	TestBehaviorTreeAssetSerializationAndInstantiation();
	TestGenericActionsAndConditions();

	std::cout << "All WorldBehaviorTreeTests passed successfully.\n";
	return 0;
}
