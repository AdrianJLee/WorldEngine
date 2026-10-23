#include "World/Animation/AnimGraph.h"
#include "World/Animation/AnimGraphCondition.h"
#include "World/Terrain/Terrain.h"
#include "World/Scene/Components.h"

#include <glm/gtc/quaternion.hpp>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <iterator>
#include <string>

using namespace World;

namespace
{
	Asset::WModelAnimationChannel MakeChannel(uint32_t node, Asset::WModelIO::WModelAnimationPath path,
		std::vector<Asset::WModelAnimationKey> keys)
	{
		Asset::WModelAnimationChannel channel;
		channel.TargetNode = node;
		channel.Path = path;
		channel.Keys = std::move(keys);
		return channel;
	}

	// 3 节点夹具：0 = Root（有位移通道），1 = Spine，2 = Head。
	Asset::WModelData MakePoseModel()
	{
		Asset::WModelData model;
		model.Meta.Valid = true;
		Asset::WModelNode root;
		root.Name = "Root";
		root.Parent = -1;
		Asset::WModelNode spine;
		spine.Name = "Spine";
		spine.Parent = 0;
		spine.Translation = { 0.0f, 1.0f, 0.0f };
		Asset::WModelNode head;
		head.Name = "Head";
		head.Parent = 1;
		head.Translation = { 0.0f, 0.5f, 0.0f };
		model.Nodes = { root, spine, head };

		Asset::WModelAnimation a;
		a.Name = "A";
		a.Duration = 1.0f;
		a.Channels = { MakeChannel(0, Asset::WModelIO::WModelAnimationPath::Translation,
			{ { 0.0f, { 0.0f, 0.0f, 0.0f, 0.0f } }, { 1.0f, { 0.0f, 0.0f, 0.0f, 0.0f } } }) };
		Asset::WModelAnimation b;
		b.Name = "B";
		b.Duration = 1.0f;
		b.Channels = { MakeChannel(0, Asset::WModelIO::WModelAnimationPath::Translation,
			{ { 0.0f, { 2.0f, 0.0f, 0.0f, 0.0f } }, { 1.0f, { 2.0f, 0.0f, 0.0f, 0.0f } } }) };
		// 上层遮罩夹具：Root 大幅位移（必须被遮罩挡住），Spine 位移 0.5（必须生效）。
		Asset::WModelAnimation aim;
		aim.Name = "Aim";
		aim.Duration = 1.0f;
		aim.Channels = {
			MakeChannel(0, Asset::WModelIO::WModelAnimationPath::Translation,
				{ { 0.0f, { 9.0f, 0.0f, 0.0f, 0.0f } }, { 1.0f, { 9.0f, 0.0f, 0.0f, 0.0f } } }),
			MakeChannel(1, Asset::WModelIO::WModelAnimationPath::Translation,
				{ { 0.0f, { 0.5f, 1.0f, 0.0f, 0.0f } }, { 1.0f, { 0.5f, 1.0f, 0.0f, 0.0f } } }),
		};
		model.Animations = { a, b, aim };
		return model;
	}
}

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
	glm::vec3 target(0.0f, 2.0f, 1.0f);
	glm::vec3 pole(0.0f, 5.0f, 5.0f);

	glm::quat rootRot, midRot;
	bool ok = TwoBoneIK::Solve(root, mid, end, target, pole, rootRot, midRot);
	assert(ok);
	assert(std::abs(glm::length(rootRot) - 1.0f) < 1e-3f);
	assert(std::abs(glm::length(midRot) - 1.0f) < 1e-3f);

	std::cout << "[PASS] TestTwoBoneIK\n";
}

static void TestFootIKTerrainPlacement()
{
	TerrainData terrain(32, 32, 1.0f, 1.0f);
	for (uint32_t z = 0; z < 32; ++z)
		for (uint32_t x = 0; x < 32; ++x)
			terrain.SetHeight(x, z, static_cast<float>(x) * 0.2f);

	glm::vec3 charPos(16.0f, 3.2f, 16.0f);
	glm::vec3 charForward(0.0f, 0.0f, 1.0f);
	float pelvisOffset = 0.0f;
	float leftOffset = 0.0f;
	float rightOffset = 0.0f;
	glm::vec3 leftNorm, rightNorm;

	FootIK::SolveTerrainPlacement(terrain, charPos, charForward, 0.6f,
		pelvisOffset, leftOffset, rightOffset, leftNorm, rightNorm);

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

	bs.Evaluate(1.0f, i0, i1, factor);
	assert(i0 == 0 && i1 == 1);
	assert(std::abs(factor - 0.5f) < 1e-4f);

	bs.Evaluate(4.0f, i0, i1, factor);
	assert(i0 == 1 && i1 == 2);
	assert(std::abs(factor - 0.5f) < 1e-4f);

	// 端点夹取：低于最小值 → 第一段，因子 0；高于最大值 → 最后一段，因子 0。
	bs.Evaluate(-5.0f, i0, i1, factor);
	assert(i0 == 0 && i1 == 0 && factor == 0.0f);
	bs.Evaluate(99.0f, i0, i1, factor);
	assert(i0 == 2 && i1 == 2 && factor == 0.0f);

	// 单样本：任何参数都命中自身。
	BlendSpace1D single;
	single.AddSample(1.5f, "Only");
	single.Evaluate(100.0f, i0, i1, factor);
	assert(i0 == 0 && i1 == 0 && factor == 0.0f);

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

	// 三角剖分必须真实生成（4 点 → 2 个三角形）。
	const auto& triangles = bs.GetTriangles();
	assert(triangles.size() >= 2);

	// 内部点：权重和 = 1，最多 3 项（重心坐标，不是反距离权重）。
	bs.Evaluate(glm::vec2(1.5f, 0.0f), weights);
	assert(!weights.empty());
	assert(weights.size() <= 3);
	float sum = 0.0f;
	for (const auto& w : weights)
	{
		assert(std::isfinite(w.second));
		assert(w.second >= 0.0f);
		sum += w.second;
	}
	assert(std::abs(sum - 1.0f) < 1e-4f);

	// 顶点精确命中：该顶点权重 1。
	bs.Evaluate(glm::vec2(2.0f, 90.0f), weights);
	assert(weights.size() == 1);
	assert(weights[0].first == 3);
	assert(std::abs(weights[0].second - 1.0f) < 1e-4f);

	// 凸包外：投影到最近边，权重和 = 1，无 NaN。
	bs.Evaluate(glm::vec2(10.0f, 200.0f), weights);
	assert(!weights.empty());
	sum = 0.0f;
	for (const auto& w : weights)
	{
		assert(std::isfinite(w.second));
		assert(w.second >= 0.0f);
		sum += w.second;
	}
	assert(std::abs(sum - 1.0f) < 1e-4f);

	// 退化：<3 点，按最近点返回。
	BlendSpace2D two;
	two.AddSample(glm::vec2(0.0f, 0.0f), "A");
	two.AddSample(glm::vec2(1.0f, 0.0f), "B");
	two.Evaluate(glm::vec2(0.75f, 5.0f), weights);
	assert(!weights.empty());
	sum = 0.0f;
	for (const auto& w : weights)
		sum += w.second;
	assert(std::abs(sum - 1.0f) < 1e-4f);

	// 退化：三点共线，不得 NaN。
	BlendSpace2D line;
	line.AddSample(glm::vec2(0.0f, 0.0f), "A");
	line.AddSample(glm::vec2(1.0f, 0.0f), "B");
	line.AddSample(glm::vec2(2.0f, 0.0f), "C");
	line.Evaluate(glm::vec2(5.0f, 0.0f), weights);
	sum = 0.0f;
	for (const auto& w : weights)
	{
		assert(std::isfinite(w.second));
		sum += w.second;
	}
	assert(std::abs(sum - 1.0f) < 1e-4f);

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

	graph.SetFloat("Speed", 0.0f);
	graph.Update(0.1f);
	assert(graph.GetCurrentStateName() == "Idle");
	assert(!graph.IsInTransition());

	graph.SetFloat("Speed", 2.5f);
	graph.Update(0.05f);
	assert(graph.IsInTransition());
	assert(graph.GetTransitionProgress() > 0.0f && graph.GetTransitionProgress() < 1.0f);

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
	assert(comp.EnableRootMotion == 1);
	assert(comp.EnableFootIk == 1);

	std::cout << "[PASS] TestAnimGraphComponentLayout\n";
}

static void TestConditionExpressions()
{
	const AnimGraphCondition::ParameterMap params = {
		{ "Speed", 0.0f }, { "IsFalling", 0.0f }, { "VerticalSpeed", 2.0f }, { "AimWeight", 0.5f },
	};

	auto p1 = AnimGraphCondition::Parse("Speed > 0.10 && IsFalling < 0.50");
	assert(p1 && !p1(params));

	auto p2 = AnimGraphCondition::Parse("Speed > 0.10 || VerticalSpeed > 1.0");
	assert(p2 && p2(params));

	// && 优先级高于 ||：false && false || true == true。
	auto p3 = AnimGraphCondition::Parse("Speed > 0.1 && IsFalling > 0.5 || VerticalSpeed > 1.0");
	assert(p3 && p3(params));

	// 无空格的写法也要能解析。
	auto p4 = AnimGraphCondition::Parse("Speed>=0&&AimWeight==0.5");
	assert(p4 && p4(params));

	// 未知参数按 0 处理。
	auto p5 = AnimGraphCondition::Parse("Unknown > 0.1");
	assert(p5 && !p5(params));

	// 解析失败 → 空 function（永不触发），不抛异常。
	auto bad = AnimGraphCondition::Parse("Speed && > 3");
	assert(!bad);

	std::cout << "[PASS] TestConditionExpressions\n";
}

static void TestCrossFadeMonotonic()
{
	const Asset::WModelData model = MakePoseModel();
	AnimGraph graph;

	AnimState a;
	a.Name = "A";
	a.ClipName = "A";
	AnimTransition toB;
	toB.TargetState = "B";
	toB.Duration = 0.4f;
	toB.Condition = [](const std::unordered_map<std::string, float>& params) {
		auto it = params.find("Go");
		return it != params.end() && it->second > 0.5f;
	};
	a.Transitions.push_back(toB);
	AnimState b;
	b.Name = "B";
	b.ClipName = "B";

	graph.AddState(a);
	graph.AddState(b);
	graph.SetInitialState("A");

	AnimPose pose;
	assert(graph.EvaluatePose(model, pose));
	assert(std::abs(pose.Joints[0].Translation.x) < 1e-4f);

	graph.SetFloat("Go", 1.0f);
	graph.Update(0.0001f);
	assert(graph.IsInTransition());

	float previous = pose.Joints[0].Translation.x;
	// 0.4 s 过渡 + 每步 0.01 s ⇒ 至少 40 步；多跑 20 步确认收敛到目标状态。
	for (int step = 0; step < 60; ++step)
	{
		graph.Update(0.01f);
		AnimPose stepPose;
		assert(graph.EvaluatePose(model, stepPose));
		const float x = stepPose.Joints[0].Translation.x;
		assert(std::isfinite(x));
		assert(x >= previous - 1e-5f);       // CrossFade 单调推进
		assert(x <= 2.0f + 1e-4f);
		previous = x;
	}
	assert(!graph.IsInTransition());
	assert(graph.GetCurrentStateName() == "B");

	std::cout << "[PASS] TestCrossFadeMonotonic\n";
}

static void TestLayeredUpperBodyMask()
{
	const Asset::WModelData model = MakePoseModel();

	AnimPose base;
	base.Joints.resize(model.Nodes.size());
	base.Joints[0].Translation = { 0.0f, 0.0f, 0.0f };
	base.Joints[1].Translation = { 0.0f, 1.0f, 0.0f };
	base.Joints[2].Translation = { 0.0f, 1.5f, 0.0f };

	AnimPose layer;
	layer.Joints.resize(model.Nodes.size());
	layer.Joints[0].Translation = { 9.0f, 9.0f, 9.0f };
	layer.Joints[1].Translation = { 0.0f, 1.0f, 1.0f };
	layer.Joints[2].Translation = { 0.0f, 1.5f, 1.0f };

	std::vector<float> mask(model.Nodes.size(), 0.0f);
	mask[1] = 1.0f;
	mask[2] = 1.0f;

	const AnimPose blended = AnimPose::LayeredBlend(base, layer, mask, 1.0f);
	assert(std::abs(blended.Joints[0].Translation.x - 0.0f) < 1e-5f);   // 下半身不受影响
	assert(std::abs(blended.Joints[1].Translation.z - 1.0f) < 1e-5f);
	assert(std::abs(blended.Joints[2].Translation.z - 1.0f) < 1e-5f);

	// 通过 AnimGraph::EvaluatePose 走真实 layer 路径：只有 mask 里的骨骼变化。
	AnimGraph graph;
	AnimState idle;
	idle.Name = "Idle";
	idle.ClipName = "A";
	AnimLayer upper;
	upper.Name = "UpperBody";
	upper.ClipName = "Aim";
	upper.WeightParameter = "AimWeight";
	upper.BoneMask = { { "Spine", 1.0f }, { "Head", 1.0f } };
	graph.AddState(idle);
	graph.AddLayer(upper);
	graph.SetInitialState("Idle");
	graph.SetFloat("AimWeight", 1.0f);

	AnimPose posed;
	assert(graph.EvaluatePose(model, posed));
	assert(std::abs(posed.Joints[0].Translation.x) < 1e-4f);   // Root 不在遮罩
	assert(std::abs(posed.Joints[1].Translation.x - 0.5f) < 1e-3f);   // Spine 使用 layer clip Aim

	std::cout << "[PASS] TestLayeredUpperBodyMask\n";
}

static void TestAssetRoundTrip()
{
	const std::filesystem::path root = std::filesystem::temp_directory_path() / "we_animgraph_roundtrip";
	std::error_code ec;
	std::filesystem::create_directories(root, ec);
	const std::filesystem::path first = root / "roundtrip_a.wanimgraph";
	const std::filesystem::path second = root / "roundtrip_b.wanimgraph";

	AnimGraph graph;
	graph.SetGraphName("Round Trip");
	graph.SetModelHint("engine/models/characters/mannequin.wmodel");
	graph.SetParameterDefault("Speed", 0.0f);
	graph.SetParameterDefault("AimWeight", 0.5f);

	AnimState idle;
	idle.Name = "Idle";
	idle.ClipName = "Idle";
	idle.Loop = true;
	idle.Speed = 1.0f;
	AnimTransition toLoco;
	toLoco.TargetState = "Locomotion";
	toLoco.Duration = 0.2f;
	toLoco.ConditionExpression = "Speed > 0.10";
	idle.Transitions.push_back(toLoco);

	AnimState loco;
	loco.Name = "Locomotion";
	loco.Kind = AnimStateKind::BlendSpace2D;
	loco.ParameterX = "Speed";
	loco.ParameterY = "Direction";
	loco.Blend2D.AddSample(glm::vec2(0.0f, 0.0f), "Idle");
	loco.Blend2D.AddSample(glm::vec2(1.6f, 0.0f), "Walk");
	loco.Blend2D.AddSample(glm::vec2(1.6f, 90.0f), "StrafeRight");

	graph.AddState(idle);
	graph.AddState(loco);
	graph.SetInitialState("Idle");

	AnimLayer layer;
	layer.Name = "UpperBody";
	layer.ClipName = "Aim";
	layer.WeightParameter = "AimWeight";
	layer.BoneMask = { { "Chest", 1.0f }, { "Spine", 0.25f } };
	graph.AddLayer(layer);

	std::string error;
	assert(graph.SaveToFile(first.string(), &error));

	AnimGraph loaded;
	assert(loaded.LoadFromFile(first.string(), &error));
	assert(loaded.GetGraphName() == "Round Trip");
	assert(loaded.GetModelHint() == "engine/models/characters/mannequin.wmodel");
	assert(loaded.GetLayers().size() == 1);
	assert(loaded.GetLayers()[0].BoneMask.size() == 2);
	const AnimState* loadedLoco = loaded.FindState("Locomotion");
	assert(loadedLoco);
	assert(loadedLoco->Kind == AnimStateKind::BlendSpace2D);
	assert(loadedLoco->Blend2D.GetSampleCount() == 3);
	assert(loaded.FindState("Idle")->Transitions.size() == 1);
	assert(loaded.FindState("Idle")->Transitions[0].ConditionExpression == "Speed > 0.10");

	// 条件表达式在加载后被编译成可调用谓词。
	loaded.SetFloat("Speed", 0.5f);
	loaded.Update(0.001f);
	assert(loaded.IsInTransition());

	assert(loaded.SaveToFile(second.string(), &error));

	const auto readAll = [](const std::filesystem::path& path)
	{
		std::ifstream stream(path, std::ios::binary);
		return std::string((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
	};
	const std::string bytesA = readAll(first);
	const std::string bytesB = readAll(second);
	assert(!bytesA.empty());
	assert(bytesA == bytesB);   // 保存→加载→再保存 字节一致

	std::cout << "[PASS] TestAssetRoundTrip\n";
}

static void TestShippedMannequinGraph()
{
	namespace fs = std::filesystem;
	fs::path root = fs::current_path();
	fs::path graphPath;
	for (int depth = 0; depth < 6 && graphPath.empty(); ++depth)
	{
		const fs::path candidate = root / "Engine" / "content" / "animation" / "mannequin_locomotion.wanimgraph";
		std::error_code ec;
		if (fs::exists(candidate, ec))
		{
			graphPath = candidate;
			break;
		}
		root = root.parent_path();
	}
	assert(!graphPath.empty() && "shipped mannequin_locomotion.wanimgraph not found");

	AnimGraph graph;
	std::string error;
	assert(graph.LoadFromFile(graphPath.string(), &error));
	assert(error.empty());
	assert(graph.GetStates().size() == 4);
	assert(graph.FindState("Idle") != nullptr);
	assert(graph.FindState("Locomotion") != nullptr);
	assert(graph.FindState("Jump") != nullptr);
	assert(graph.FindState("Fall") != nullptr);
	assert(graph.GetCurrentStateName() == "Idle");
	assert(graph.GetLayers().size() == 1);
	assert(graph.GetLayers().front().ClipName == "Aim");

	const AnimState* locomotion = graph.FindState("Locomotion");
	assert(locomotion != nullptr);
	assert(locomotion->Kind == AnimStateKind::BlendSpace2D);
	assert(locomotion->Blend2D.GetSampleCount() == 9);
	assert(locomotion->Blend2D.GetTriangles().size() >= 8);

	std::vector<std::pair<size_t, float>> weights;
	locomotion->Blend2D.Evaluate(glm::vec2(2.4f, 12.0f), weights);
	assert(!weights.empty());
	float sum = 0.0f;
	for (const auto& entry : weights)
	{
		assert(entry.first < locomotion->Blend2D.GetSamples().size());
		assert(std::isfinite(entry.second) && entry.second >= 0.0f);
		sum += entry.second;
	}
	assert(std::abs(sum - 1.0f) < 1e-3f);

	// 分层遮罩:同一个资产把 AimWeight 从 0 拉到 1,UpperArm.L 必须明显抬起。
	fs::path modelPath;
	for (fs::path probe = fs::current_path(); !probe.empty(); probe = probe.parent_path())
	{
		const fs::path candidate = probe / "Engine" / "content" / "models" / "characters" / "mannequin.wmodel";
		std::error_code ec;
		if (fs::exists(candidate, ec))
		{
			modelPath = candidate;
			break;
		}
		if (probe == probe.root_path()) break;
	}
	assert(!modelPath.empty() && "shipped mannequin.wmodel not found");
	Asset::WModelData model;
	assert(Asset::WModelIO::ReadFile(modelPath.string(), model, &error));
	assert(!model.Animations.empty());

	graph.SetFloat("Speed", 0.0f);
	graph.SetFloat("Direction", 0.0f);
	graph.SetFloat("AimWeight", 0.0f);
	graph.Update(0.016f);
	AnimPose withoutAim;
	assert(graph.EvaluatePose(model, withoutAim));

	graph.SetFloat("AimWeight", 1.0f);
	graph.Update(0.016f);
	AnimPose withAim;
	assert(graph.EvaluatePose(model, withAim));
	assert(withoutAim.Joints.size() == withAim.Joints.size());

	size_t upperArm = model.Nodes.size();
	for (size_t i = 0; i < model.Nodes.size(); ++i)
		if (model.Nodes[i].Name == "UpperArm.L") { upperArm = i; break; }
	assert(upperArm < withAim.Joints.size());
	const float dot = std::min(1.0f, std::fabs(glm::dot(withoutAim.Joints[upperArm].Rotation, withAim.Joints[upperArm].Rotation)));
	const float angle = 2.0f * std::acos(dot);
	assert(angle > 0.35f);

	// 文本资产的 round-trip 必须字节稳定（编辑器保存后再次加载的结果不变）。
	const fs::path roundTripPath = fs::temp_directory_path() / "we_shipped_animgraph_roundtrip.wanimgraph";
	assert(graph.SaveToFile(roundTripPath.string(), &error));

	std::ifstream first(roundTripPath, std::ios::binary);
	std::stringstream firstText;
	firstText << first.rdbuf();
	first.close();

	AnimGraph reloaded;
	assert(reloaded.LoadFromFile(roundTripPath.string(), &error));
	assert(reloaded.SaveToFile(roundTripPath.string(), &error));
	std::ifstream second(roundTripPath, std::ios::binary);
	std::stringstream secondText;
	secondText << second.rdbuf();
	second.close();
	assert(firstText.str() == secondText.str());
	fs::remove(roundTripPath);

	std::cout << "[PASS] TestShippedMannequinGraph\n";
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
	TestShippedMannequinGraph();
	TestConditionExpressions();
	TestCrossFadeMonotonic();
	TestLayeredUpperBodyMask();
	TestAssetRoundTrip();
	std::cout << "All World.AnimGraph tests passed successfully!\n";
	return 0;
}









