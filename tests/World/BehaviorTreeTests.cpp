#include "World/Gameplay/Framework/BehaviorTree.h"
#include "World/Core/Log.h"

#include <cassert>
#include <iostream>

using namespace World;
using namespace World::Gameplay;

static void TestSequenceAndSelector()
{
	auto seq = std::make_shared<SequenceNode>();
	int actionCount = 0;

	seq->AddChild(std::make_shared<ConditionNode>([]() { return true; }));
	seq->AddChild(std::make_shared<ActionNode>([&](float dt) {
		++actionCount;
		return NodeStatus::Success;
	}));

	BehaviorTreeRunner runner(seq);
	NodeStatus s1 = runner.Tick(0.016f);
	assert(s1 == NodeStatus::Success);
	assert(actionCount == 1);

	auto seqFail = std::make_shared<SequenceNode>();
	seqFail->AddChild(std::make_shared<ConditionNode>([]() { return false; }));
	seqFail->AddChild(std::make_shared<ActionNode>([&](float dt) {
		++actionCount;
		return NodeStatus::Success;
	}));

	runner.SetRoot(seqFail);
	NodeStatus s2 = runner.Tick(0.016f);
	assert(s2 == NodeStatus::Failure);
	assert(actionCount == 1);

	auto sel = std::make_shared<SelectorNode>();
	sel->AddChild(std::make_shared<ConditionNode>([]() { return false; }));
	sel->AddChild(std::make_shared<ActionNode>([&](float dt) {
		++actionCount;
		return NodeStatus::Success;
	}));

	runner.SetRoot(sel);
	NodeStatus s3 = runner.Tick(0.016f);
	assert(s3 == NodeStatus::Success);
	assert(actionCount == 2);

	std::cout << "[PASS] TestSequenceAndSelector\n";
}

int main()
{
	Log::Init();
	std::cout << "Running WorldBehaviorTreeTests...\n";

	TestSequenceAndSelector();

	std::cout << "All WorldBehaviorTreeTests passed successfully.\n";
	return 0;
}
