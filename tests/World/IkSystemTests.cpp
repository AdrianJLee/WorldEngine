// 通用两骨 IK 回归：可达目标 / 超距钳制 / 权重 / 无效链 / 脚步示例 / 组件代理。
// headless 纯 CPU：直接构造 WModelData、Scene、TerrainData。
#include "wldpch.h"

#include "World/Animation/IkSystem.h"
#include "World/Core/StringPool.h"
#include "World/Core/UUID.h"
#include "World/Core/WorldContext.h"
#include "World/Scene/Components.h"
#include "World/Scene/Scene.h"
#include "World/Terrain/Terrain.h"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{
	using namespace World;
	namespace fs = std::filesystem;

	void Check(bool condition, const char* expression, int line)
	{
		if (!condition)
			throw std::runtime_error(std::string("line ") + std::to_string(line) + ": " + expression);
	}
#define CHECK(expression) Check(static_cast<bool>(expression), #expression, __LINE__)

	bool Nearly(float left, float right, float epsilon)
	{
		return std::fabs(left - right) <= epsilon;
	}

	Asset::WModelNode MakeNode(const char* name, int32_t parent, const glm::vec3& translation)
	{
		Asset::WModelNode node;
		node.Name = name;
		node.Parent = parent;
		node.Translation = translation;
		return node;
	}

	// 0 MeshNode, 1 Root(0,1,0), 2 Mid(0,0.5,0), 3 End(0,0,0)；两段各 0.5 m。
	Asset::WModelData MakeChainModel(bool directParentChain = true)
	{
		Asset::WModelData model;
		model.Meta.Valid = true;
		model.Meta.ImporterVersion = 1;
		model.Meta.Scale = 1.0f;
		model.Nodes = {
			MakeNode("MeshNode", -1, glm::vec3(0.0f)),
			MakeNode("Root", 0, glm::vec3(0.0f, 1.0f, 0.0f)),
			MakeNode("Mid", 1, glm::vec3(0.0f, -0.5f, 0.0f)),
			MakeNode("End", directParentChain ? 2 : 1, glm::vec3(0.0f, -0.5f, 0.0f)),
		};
		return model;
	}

	// 0 MeshNode, 1 Root, 2 Hips, 3/4/5 Thigh.L/Shin.L/Foot.L, 6/7/8 Thigh.R/Shin.R/Foot.R
	// 绑定姿态脚踝 y = 0.9 - 0.45 - 0.35 = 0.10。
	Asset::WModelData MakeLegModel()
	{
		Asset::WModelData model;
		model.Meta.Valid = true;
		model.Meta.ImporterVersion = 1;
		model.Meta.Scale = 1.0f;
		model.Nodes = {
			MakeNode("MeshNode", -1, glm::vec3(0.0f)),
			MakeNode("Root", 0, glm::vec3(0.0f)),
			MakeNode("Hips", 1, glm::vec3(0.0f, 0.9f, 0.0f)),
			MakeNode("Thigh.L", 2, glm::vec3(0.1f, 0.0f, 0.0f)),
			MakeNode("Shin.L", 3, glm::vec3(0.0f, -0.45f, 0.0f)),
			MakeNode("Foot.L", 4, glm::vec3(0.0f, -0.35f, 0.0f)),
			MakeNode("Thigh.R", 2, glm::vec3(-0.1f, 0.0f, 0.0f)),
			MakeNode("Shin.R", 6, glm::vec3(0.0f, -0.45f, 0.0f)),
			MakeNode("Foot.R", 7, glm::vec3(0.0f, -0.35f, 0.0f)),
		};
		return model;
	}

	AnimPose BindPose(const Asset::WModelData& model)
	{
		AnimPose pose;
		pose.Joints.resize(model.Nodes.size());
		for (size_t i = 0; i < model.Nodes.size(); ++i)
		{
			pose.Joints[i].Translation = model.Nodes[i].Translation;
			pose.Joints[i].Rotation = model.Nodes[i].Rotation;
			pose.Joints[i].Scale = model.Nodes[i].Scale;
		}
		return pose;
	}

	std::vector<glm::mat4> NodeWorld(const Asset::WModelData& model, const AnimPose& pose)
	{
		std::vector<glm::mat4> world(model.Nodes.size(), glm::mat4(1.0f));
		for (size_t i = 0; i < model.Nodes.size(); ++i)
		{
			const JointTransform& joint = pose.Joints[i];
			const glm::mat4 local = glm::translate(glm::mat4(1.0f), joint.Translation)
				* glm::mat4_cast(joint.Rotation)
				* glm::scale(glm::mat4(1.0f), joint.Scale);
			const int32_t parent = model.Nodes[i].Parent;
			world[i] = (parent >= 0 && static_cast<size_t>(parent) < i) ? world[static_cast<size_t>(parent)] * local : local;
		}
		return world;
	}

	glm::vec3 EndPosition(const Asset::WModelData& model, const AnimPose& pose, int32_t node)
	{
		return glm::vec3(NodeWorld(model, pose)[static_cast<size_t>(node)][3]);
	}

	void RunSolveReachableAndClamp()
	{
		const Asset::WModelData model = MakeChainModel();
		AnimPose pose = BindPose(model);

		IkChainRequest request;
		request.Root = 1;
		request.Mid = 2;
		request.End = 3;
		request.TargetModel = glm::vec3(0.0f, 0.75f, 0.25f);
		request.PoleDirectionModel = glm::vec3(0.0f, 0.0f, 1.0f);
		request.Weight = 1.0f;

		CHECK(IkSystem::SolveTwoBone(model, pose, request));
		const glm::vec3 reached = EndPosition(model, pose, 3);
		CHECK(glm::distance(reached, request.TargetModel) <= 1e-3f);

		// 超距目标 → 钳制到最大伸展（0.5 + 0.5），保持有限值。
		pose = BindPose(model);
		request.TargetModel = glm::vec3(0.0f, 1.0f, 4.0f);
		CHECK(IkSystem::SolveTwoBone(model, pose, request));
		const glm::mat4 rootWorld = NodeWorld(model, pose)[1];
		const glm::vec3 rootPos = glm::vec3(rootWorld[3]);
		const glm::vec3 clamped = EndPosition(model, pose, 3);
		const float length = glm::length(clamped - rootPos);
		CHECK(std::isfinite(length));
		CHECK(length <= 1.0f + 1e-3f);
		CHECK(length >= 0.99f);
	}

	void RunWeightBlend()
	{
		const Asset::WModelData model = MakeChainModel();
		const AnimPose bind = BindPose(model);

		IkChainRequest request;
		request.Root = 1;
		request.Mid = 2;
		request.End = 3;
		request.TargetModel = glm::vec3(0.0f, 0.75f, 0.3f);
		request.PoleDirectionModel = glm::vec3(0.0f, 0.0f, 1.0f);

		AnimPose zero = bind;
		request.Weight = 0.0f;
		CHECK(!IkSystem::SolveTwoBone(model, zero, request));   // 0 权重不求解
		CHECK(glm::distance(EndPosition(model, zero, 3), EndPosition(model, bind, 3)) <= 1e-6f);

		AnimPose full = bind;
		request.Weight = 1.0f;
		CHECK(IkSystem::SolveTwoBone(model, full, request));
		const glm::vec3 bindEnd = EndPosition(model, bind, 3);
		const glm::vec3 fullEnd = EndPosition(model, full, 3);

		AnimPose half = bind;
		request.Weight = 0.5f;
		CHECK(IkSystem::SolveTwoBone(model, half, request));
		const glm::vec3 halfEnd = EndPosition(model, half, 3);
		const float bindToTarget = glm::distance(bindEnd, request.TargetModel);
		const float halfToTarget = glm::distance(halfEnd, request.TargetModel);
		CHECK(glm::distance(halfEnd, bindEnd) > 1e-3f);
		CHECK(halfToTarget > 1e-3f);
		CHECK(halfToTarget < bindToTarget - 1e-3f);
		CHECK(std::isfinite(halfEnd.x) && std::isfinite(halfEnd.y) && std::isfinite(halfEnd.z));
		CHECK(glm::distance(fullEnd, request.TargetModel) <= 1e-3f);
	}

	void RunInvalidChain()
	{
		const Asset::WModelData model = MakeChainModel(false);   // End 直接挂 Root，不是 Root→Mid→End
		AnimPose pose = BindPose(model);
		const std::vector<JointTransform> before = pose.Joints;

		IkChainRequest request;
		request.Root = 1;
		request.Mid = 2;
		request.End = 3;
		request.TargetModel = glm::vec3(0.0f, 0.75f, 0.25f);
		CHECK(!IkSystem::SolveTwoBone(model, pose, request));
		CHECK(pose.Joints.size() == before.size());
		for (size_t i = 0; i < before.size(); ++i)
			CHECK(glm::distance(pose.Joints[i].Translation, before[i].Translation) <= 1e-6f);

		// 越界下标同样只返回 false。
		request.End = 99;
		CHECK(!IkSystem::SolveTwoBone(model, pose, request));
	}

	void RunFootScenario(const fs::path& directory, const char* fileName, bool slope, bool useExampleComponent)
	{
		TerrainData terrain(64, 64, 1.0f, 1.0f);
		for (uint32_t z = 0; z < 64; ++z)
			for (uint32_t x = 0; x < 64; ++x)
				terrain.SetHeight(x, z, slope ? (0.4f + 0.1f * static_cast<float>(x)) : 0.5f);

		const fs::path terrainPath = directory / fileName;
		CHECK(terrain.SaveToFile(terrainPath.string()));

		WorldContext context;
		Scene scene(context);

		const glm::vec3 terrainOffset(10.0f, 0.0f, 10.0f);
		const entt::entity terrainEntity = scene.GetRegistry().create();
		TransformComponent terrainTransform;
		terrainTransform.Location = terrainOffset;
		scene.GetRegistry().emplace<TransformComponent>(terrainEntity, terrainTransform);
		TerrainComponent terrainComponent;
		terrainComponent.TerrainAsset.Path = StringPool::Get().InternPath(terrainPath.string());
		scene.GetRegistry().emplace<TerrainComponent>(terrainEntity, terrainComponent);

		const entt::entity character = scene.GetRegistry().create();
		TransformComponent characterTransform;
		characterTransform.Location = terrainOffset;
		scene.GetRegistry().emplace<TransformComponent>(character, characterTransform);
		UUIDComponent identity(UUID(0x1001));
		scene.GetRegistry().emplace<UUIDComponent>(character, identity);
		AnimGraphComponent graph;
		graph.EnableFootIk = useExampleComponent ? 0 : 1;
		graph.FootIkWeight = 1.0f;
		scene.GetRegistry().emplace<AnimGraphComponent>(character, graph);
		if (useExampleComponent)
		{
			FootIkComponent foot;
			foot.Weight = 1.0f;
			foot.FootOffset = 0.0f;
			foot.MaxPelvisDrop = 0.35f;
			foot.Enabled = 1;
			scene.GetRegistry().emplace<FootIkComponent>(character, foot);
		}

		const Asset::WModelData model = MakeLegModel();
		AnimPose pose = BindPose(model);
		const glm::mat4 characterWorld = scene.GetRegistry().get<TransformComponent>(character).GetLocalMatrix();
		const glm::mat4 terrainWorld = scene.GetRegistry().get<TransformComponent>(terrainEntity).GetLocalMatrix();

		IkSystem::BeginFrame(scene);
		IkSystem::ApplyToPose(scene, character, model, characterWorld, pose, true);
		IkSystem::EndFrame();

		const glm::mat4 inverseTerrain = glm::inverse(terrainWorld);
		const std::vector<glm::mat4> world = NodeWorld(model, pose);
		for (const int32_t foot : { 5, 8 })
		{
			const glm::vec3 footWorld = glm::vec3(characterWorld * glm::vec4(glm::vec3(world[static_cast<size_t>(foot)][3]), 1.0f));
			const glm::vec3 footLocal = glm::vec3(inverseTerrain * glm::vec4(footWorld, 1.0f));
			const float expected = terrainWorld[3].y + terrain.SampleHeight(footLocal.x, footLocal.z) + 0.10f;
			CHECK(std::isfinite(footWorld.y));
			CHECK(Nearly(footWorld.y, expected, 5e-2f));
		}
	}

	void RunFootIkExamples(const fs::path& directory)
	{
		RunFootScenario(directory, "ik_flat_legacy.terrain", false, false);
		RunFootScenario(directory, "ik_slope_legacy.terrain", true, false);
		RunFootScenario(directory, "ik_flat_component.terrain", false, true);
		RunFootScenario(directory, "ik_slope_component.terrain", true, true);
	}

	entt::entity AddIdentityEntity(Scene& scene, uint64_t uuid, const glm::vec3& location)
	{
		const entt::entity entity = scene.GetRegistry().create();
		TransformComponent transform;
		transform.Location = location;
		scene.GetRegistry().emplace<TransformComponent>(entity, transform);
		scene.GetRegistry().emplace<UUIDComponent>(entity, UUIDComponent(UUID(uuid)));
		return entity;
	}

	void RunGenericChainViaProxy()
	{
		WorldContext context;
		Scene scene(context);
		const entt::entity character = AddIdentityEntity(scene, 0x2001, glm::vec3(0.0f));
		const entt::entity proxy = AddIdentityEntity(scene, 0x2002, glm::vec3(3.0f));

		TwoBoneIkComponent ik;
		ik.OwnerUuid = 0x2001;
		ik.RootBone = StringPool::Get().InternName("Root");
		ik.MidBone = StringPool::Get().InternName("Mid");
		ik.EndBone = StringPool::Get().InternName("End");
		ik.TargetPosition = glm::vec3(0.0f, 0.75f, 0.25f);
		ik.Weight = 1.0f;
		ik.Enabled = 1;
		scene.GetRegistry().emplace<TwoBoneIkComponent>(proxy, ik);

		const Asset::WModelData model = MakeChainModel();
		AnimPose pose = BindPose(model);
		const glm::mat4 characterWorld = scene.GetRegistry().get<TransformComponent>(character).GetLocalMatrix();

		IkSystem::BeginFrame(scene);
		IkSystem::ApplyToPose(scene, character, model, characterWorld, pose, true);
		IkSystem::EndFrame();
		CHECK(glm::distance(EndPosition(model, pose, 3), ik.TargetPosition) <= 1e-3f);

		// TargetUuid 优先于 TargetPosition。
		const entt::entity target = AddIdentityEntity(scene, 0x2003, glm::vec3(0.1f, 0.7f, 0.2f));
		TwoBoneIkComponent& stored = scene.GetRegistry().get<TwoBoneIkComponent>(proxy);
		stored.TargetUuid = 0x2003;
		stored.TargetPosition = glm::vec3(9.0f);
		pose = BindPose(model);
		IkSystem::BeginFrame(scene);
		IkSystem::ApplyToPose(scene, character, model, characterWorld, pose, true);
		IkSystem::EndFrame();
		CHECK(glm::distance(EndPosition(model, pose, 3), glm::vec3(0.1f, 0.7f, 0.2f)) <= 1e-3f);

		// OwnerUuid == 0 → 组件所在实体自身就是 owner。
		TwoBoneIkComponent selfIk = ik;
		selfIk.OwnerUuid = 0;
		selfIk.TargetUuid = 0;
		selfIk.TargetPosition = glm::vec3(0.0f, 0.8f, 0.15f);
		scene.GetRegistry().emplace<TwoBoneIkComponent>(character, selfIk);
		scene.GetRegistry().remove<TwoBoneIkComponent>(proxy);
		pose = BindPose(model);
		IkSystem::BeginFrame(scene);
		IkSystem::ApplyToPose(scene, character, model, characterWorld, pose, true);
		IkSystem::EndFrame();
		CHECK(glm::distance(EndPosition(model, pose, 3), selfIk.TargetPosition) <= 1e-3f);

		(void)target;
	}
}

int main()
{
	const fs::path root = fs::temp_directory_path() / "we_ik_system";
	std::error_code error;
	fs::create_directories(root, error);

	struct Case
	{
		const char* Name;
		std::function<void()> Run;
	};
	const Case cases[] = {
		{ "Two-bone chain reaches target and clamps over-reach", RunSolveReachableAndClamp },
		{ "Weight 0 keeps pose; 0.5 blends between", RunWeightBlend },
		{ "Invalid chain returns false without modifying pose", RunInvalidChain },
		{ "Foot IK example on flat and slope (legacy + component)", [&]() { RunFootIkExamples(root); } },
		{ "Generic chain via TwoBoneIkComponent proxy", RunGenericChainViaProxy },
	};

	int failures = 0;
	for (const Case& test : cases)
	{
		try
		{
			test.Run();
			std::printf("[PASS] %s\n", test.Name);
		}
		catch (const std::exception& failure)
		{
			++failures;
			std::printf("[FAIL] %s — %s\n", test.Name, failure.what());
		}
	}

	if (failures != 0)
	{
		std::printf("%d IK system test(s) failed\n", failures);
		return 1;
	}
	std::printf("All World.IkSystem tests passed!\n");
	return 0;
}
