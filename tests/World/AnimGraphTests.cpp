#include "World/Animation/AnimGraph.h"
#include "World/Terrain/Terrain.h"
#include "World/Scene/Components.h"

#include <cassert>
#include <cmath>
#include <iostream>

using namespace World;

static void TestJointTransformAndBlend()
{
	JointTransform a;
	a.Translation = glm::vec3(0.0f, 0.0f, 0.0f);
	a.Rotation = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
	a.Scale = glm::vec3(1.0f);

	JointTransform b;
	b.Translation = glm::vec3(10.0f, 20.0f, 30.0f);
	b.Rotation = glm::angleAxis(glm::radians(90.0f), glm::vec3(0.0f, 1.0f, 0.0f));
	b.Scale = glm::vec3(2.0f);

	JointTransform mid = JointTransform::Lerp(a, b, 0.5f);
	assert(std::abs(mid.Translation.x - 5.0f) < 1e-4f);
	assert(std::abs(mid.Translation.y - 10.0f) < 1e-4f);
	assert(std::abs(mid.Translation.z - 15.0f) < 1e-4f);
	assert(std::abs(mid.Scale.x - 1.5f) < 1e-4f);

	glm::mat4 m = mid.ToMatrix();
	assert(m[3][0] == 5.0f && m[3][1] == 10.0f && m[3][2] == 15.0f);

	std::cout << "[PASS] TestJointTransformAndBlend\n";
}

static void TestTwoBoneIK()
{
	glm::vec3 root(0.0f, 10.0f, 0.0f);
	glm::vec3 mid(0.0f, 5.0f, 0.0f);
	glm::vec3 end(0.0f, 0.0f, 0.0f);
	glm::vec3 target(0.0f, 2.0f, 1.0f); // 目标点在下方微前
	glm::vec3 pole(0.0f, 5.0f, 5.0f);  // 膝盖朝向前方

	glm::quat rootRot, midRot;
	bool ok = TwoBoneIK::Solve(root, mid, end, target, pole, rootRot, midRot);
	assert(ok);

	// 旋转四元数模长必须为 1
	assert(std::abs(glm::length(rootRot) - 1.0f) < 1e-3f);
	assert(std::abs(glm::length(midRot) - 1.0f) < 1e-3f);

	std::cout << "[PASS] TestTwoBoneIK\n";
}

static void TestFootIKTerrainPlacement()
{
	TerrainData terrain(32, 32, 1.0f, 1.0f);
	// 斜坡：X 坐标越高，高度越高
	for (uint32_t z = 0; z < 32; ++z)
	{
		for (uint32_t x = 0; x < 32; ++x)
		{
			terrain.SetHeight(x, z, static_cast<float>(x) * 0.2f);
		}
	}

	glm::vec3 charPos(16.0f, 3.2f, 16.0f);
	glm::vec3 charForward(0.0f, 0.0f, 1.0f); // 朝向 Z
	float footSpacing = 0.6f;                // 左右脚横跨 X 方向

	float pelvisOffset = 0.0f;
	float leftOffset = 0.0f;
	float rightOffset = 0.0f;
	glm::vec3 leftNorm, rightNorm;

	FootIK::SolveTerrainPlacement(terrain, charPos, charForward, footSpacing,
		pelvisOffset, leftOffset, rightOffset, leftNorm, rightNorm);

	// 左右脚高度应该有高低差
	assert(leftOffset != rightOffset);
	assert(leftNorm.y > 0.5f);
	assert(rightNorm.y > 0.5f);

	std::cout << "[PASS] TestFootIKTerrainPlacement\n";
}

static void TestBlendSpace1D()
{
	BlendSpace1D bs;
	bs.AddSample(0.0f, "Idle");
	bs.AddSample(2.0f, "Walk");
	bs.AddSample(6.0f, "Run");

	assert(bs.GetSampleCount() == 3);

	size_t i0 = 0, i1 = 0;
	float factor = 0.0f;

	// 测试 1.0m/s (在 Idle 与 Walk 之间中点)
	bs.Evaluate(1.0f, i0, i1, factor);
	assert(i0 == 0 && i1 == 1);
	assert(std::abs(factor - 0.5f) < 1e-4f);

	// 测试 4.0m/s (在 Walk 与 Run 之间中点)
	bs.Evaluate(4.0f, i0, i1, factor);
	assert(i0 == 1 && i1 == 2);
	assert(std::abs(factor - 0.5f) < 1e-4f);

	std::cout << "[PASS] TestBlendSpace1D\n";
}

static void TestBlendSpace2D()
{
	BlendSpace2D bs;
	bs.AddSample(glm::vec2(0.0f, 0.0f), "Idle");
	bs.AddSample(glm::vec2(2.0f, 0.0f), "WalkForward");
	bs.AddSample(glm::vec2(2.0f, -90.0f), "StrafeLeft");
	bs.AddSample(glm::vec2(2.0f, 90.0f), "StrafeRight");

	std::vector<std::pair<size_t, float>> weights;
	bs.Evaluate(glm::vec2(1.5f, 0.0f), weights);
	assert(!weights.empty());

	float sum = 0.0f;
	for (const auto& w : weights)
		sum += w.second;
	assert(std::abs(sum - 1.0f) < 1e-3f);

	std::cout << "[PASS] TestBlendSpace2D\n";
}

static void TestAnimGraphStateMachineTransitions()
{
	AnimGraph graph;

	AnimState idleState;
	idleState.Name = "Idle";
	idleState.Speed = 1.0f;

	AnimTransition toMove;
	toMove.TargetState = "Locomotion";
	toMove.Duration = 0.2f;
	toMove.Condition = [](const std::unordered_map<std::string, float>& params) {
		auto it = params.find("Speed");
		return (it != params.end() && it->second > 0.1f);
	};
	idleState.Transitions.push_back(toMove);

	AnimState moveState;
	moveState.Name = "Locomotion";
	moveState.Speed = 1.0f;

	graph.AddState(idleState);
	graph.AddState(moveState);
	graph.SetInitialState("Idle");

	assert(graph.GetCurrentStateName() == "Idle");
	assert(!graph.IsInTransition());

	// Speed == 0 时，保持 Idle
	graph.SetFloat("Speed", 0.0f);
	graph.Update(0.1f);
	assert(graph.GetCurrentStateName() == "Idle");
	assert(!graph.IsInTransition());

	// Speed > 0.1 时，触发平滑过渡
	graph.SetFloat("Speed", 2.5f);
	graph.Update(0.05f);
	assert(graph.IsInTransition());
	assert(graph.GetTransitionProgress() > 0.0f && graph.GetTransitionProgress() < 1.0f);

	// 推进时间越过 0.2s 过渡时长，切换至 Locomotion
	graph.Update(0.2f);
	assert(!graph.IsInTransition());
	assert(graph.GetCurrentStateName() == "Locomotion");

	std::cout << "[PASS] TestAnimGraphStateMachineTransitions\n";
}

static void TestAnimGraphComponentLayout()
{
	AnimGraphComponent comp;
	assert(sizeof(comp) == 48);
	assert(comp.PlaybackSpeed == 1.0f);
	assert(comp.RootMotionWeight == 1.0f);
	assert(comp.FootIkWeight == 1.0f);

	std::cout << "[PASS] TestAnimGraphComponentLayout\n";
}

int main()
{
	std::cout << "Starting World.AnimGraph tests...\n";
	TestJointTransformAndBlend();
	TestTwoBoneIK();
	TestFootIKTerrainPlacement();
	TestBlendSpace1D();
	TestBlendSpace2D();
	TestAnimGraphStateMachineTransitions();
	TestAnimGraphComponentLayout();
	std::cout << "All World.AnimGraph tests passed successfully!\n";
	return 0;
}
