// AnimGraph 运行时集成回归：真实 .wmodel + .wanimgraph + TerrainComponent，headless 纯 CPU。
//
// 覆盖：
//   1. 组件挂载后调色板非空、参数驱动状态切换、CurrentStateHash 回写；
//   2. SetParameter 的 2 帧短时覆盖优先于 CharacterController 推导；
//   3. Foot IK 把 Thigh→Shin→Foot 送到“地面 + 踝高”，目标误差 ≤ 1e-4；
//   4. Root Motion 本帧增量按实体朝向旋转后写回 Transform。
#include "wldpch.h"

#include "World/Animation/AnimGraphSystem.h"
#include "World/Renderer/AnimationSystem.h"

#include "World/Asset/WModelIO.h"
#include "World/Core/StringPool.h"
#include "World/Core/WorldContext.h"
#include "World/Scene/Components.h"
#include "World/Scene/Scene.h"
#include "World/Terrain/Terrain.h"

#include <glm/gtc/quaternion.hpp>

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <utility>
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

	bool Nearly(float left, float right, float epsilon = 1e-4f)
	{
		return std::fabs(left - right) <= epsilon;
	}

	Asset::WModelAnimationChannel MakeChannel(uint32_t node, Asset::WModelIO::WModelAnimationPath path,
		std::vector<Asset::WModelAnimationKey> keys)
	{
		Asset::WModelAnimationChannel channel;
		channel.TargetNode = node;
		channel.Path = path;
		channel.Keys = std::move(keys);
		return channel;
	}

	uint32_t Fnv1a32(const std::string& text)
	{
		uint32_t hash = 2166136261u;
		for (const char c : text)
		{
			hash ^= static_cast<uint32_t>(static_cast<unsigned char>(c));
			hash *= 16777619u;
		}
		return hash;
	}

	// 骨架（节点）：0 MeshNode, 1 Root, 2 Hips, 3 Thigh.L, 4 Shin.L, 5 Foot.L,
	//                   6 Thigh.R, 7 Shin.R, 8 Foot.R
	// 绑定姿态：脚踝离地 0.10 m；腿长 0.45 + 0.35。
	Asset::WModelData MakeLegModel()
	{
		Asset::WModelData model;
		model.Meta.Valid = true;
		model.Meta.ImporterVersion = 1;
		model.Meta.Scale = 1.0f;
		model.VertexLayoutId = Asset::WModelIO::kVertexLayoutSkinned;
		model.Vertices = {
			{ { 0.0f, 0.0f, 0.0f }, { 0.0f, 0.0f, 1.0f }, { 0.0f, 0.0f } },
			{ { 1.0f, 0.0f, 0.0f }, { 0.0f, 0.0f, 1.0f }, { 1.0f, 0.0f } },
			{ { 0.0f, 1.0f, 0.0f }, { 0.0f, 0.0f, 1.0f }, { 0.5f, 1.0f } },
		};
		model.SkinVertices = {
			{ { 0.0f, 0.0f, 0.0f, 0.0f }, { 1.0f, 0.0f, 0.0f, 0.0f } },
			{ { 0.0f, 0.0f, 0.0f, 0.0f }, { 1.0f, 0.0f, 0.0f, 0.0f } },
			{ { 0.0f, 0.0f, 0.0f, 0.0f }, { 1.0f, 0.0f, 0.0f, 0.0f } },
		};
		model.Indices = { 0, 1, 2 };
		model.Bounds = { { 0.0f, 0.0f, 0.0f }, { 1.0f, 1.0f, 0.0f } };
		Asset::WModelSubmesh submesh;
		submesh.IndexOffset = 0;
		submesh.IndexCount = 3;
		submesh.MaterialSlot = -1;
		submesh.Bounds = { { 0.0f, 0.0f, 0.0f }, { 1.0f, 1.0f, 0.0f } };
		model.Submeshes = { submesh };
		Asset::WModelMeshRange range;
		range.FirstSubmesh = 0;
		range.SubmeshCount = 1;
		range.SkinIndex = 0;
		model.Meshes = { range };

		Asset::WModelNode meshNode;
		meshNode.Name = "MeshNode";
		meshNode.MeshIndex = 0;
		Asset::WModelNode root;
		root.Name = "Root";
		root.Parent = 0;
		Asset::WModelNode hips;
		hips.Name = "Hips";
		hips.Parent = 1;
		hips.Translation = { 0.0f, 0.9f, 0.0f };
		Asset::WModelNode thighL;
		thighL.Name = "Thigh.L";
		thighL.Parent = 2;
		thighL.Translation = { 0.1f, 0.0f, 0.0f };
		Asset::WModelNode shinL;
		shinL.Name = "Shin.L";
		shinL.Parent = 3;
		shinL.Translation = { 0.0f, -0.45f, 0.0f };
		Asset::WModelNode footL;
		footL.Name = "Foot.L";
		footL.Parent = 4;
		footL.Translation = { 0.0f, -0.35f, 0.0f };
		Asset::WModelNode thighR;
		thighR.Name = "Thigh.R";
		thighR.Parent = 2;
		thighR.Translation = { -0.1f, 0.0f, 0.0f };
		Asset::WModelNode shinR;
		shinR.Name = "Shin.R";
		shinR.Parent = 6;
		shinR.Translation = { 0.0f, -0.45f, 0.0f };
		Asset::WModelNode footR;
		footR.Name = "Foot.R";
		footR.Parent = 7;
		footR.Translation = { 0.0f, -0.35f, 0.0f };
		model.Nodes = { meshNode, root, hips, thighL, shinL, footL, thighR, shinR, footR };

		Asset::WModelSkin skin;
		skin.Name = "Legs";
		skin.JointNames = { "Root", "Hips", "Thigh.L", "Shin.L", "Foot.L", "Thigh.R", "Shin.R", "Foot.R" };
		skin.JointNodes = { 1u, 2u, 3u, 4u, 5u, 6u, 7u, 8u };
		skin.JointParents = { -1, 0, 1, 2, 3, 1, 5, 6 };
		skin.InverseBindMatrices.assign(skin.JointNodes.size(), glm::mat4(1.0f));
		skin.BindTranslations.assign(skin.JointNodes.size(), glm::vec3(0.0f));
		skin.BindRotations.assign(skin.JointNodes.size(), glm::quat(1.0f, 0.0f, 0.0f, 0.0f));
		skin.BindScales.assign(skin.JointNodes.size(), glm::vec3(1.0f));
		model.Skins = { skin };

		Asset::WModelAnimation idle;
		idle.Name = "Idle";
		idle.Duration = 1.0f;
		Asset::WModelAnimation rootMove;
		rootMove.Name = "RootMove";
		rootMove.Duration = 1.0f;
		rootMove.Channels = {
			MakeChannel(1u, Asset::WModelIO::WModelAnimationPath::Translation,
				{ { 0.0f, { 0.0f, 0.0f, 0.0f, 0.0f } }, { 1.0f, { 1.0f, 0.0f, 0.0f, 0.0f } } }),
		};
		model.Animations = { idle, rootMove };
		return model;
	}

	void WriteModel(const fs::path& path)
	{
		std::string error;
		if (!Asset::WModelIO::WriteFile(path.string(), MakeLegModel(), &error))
			throw std::runtime_error("WriteFile failed: " + error);
	}

	void WriteText(const fs::path& path, const std::string& text)
	{
		std::ofstream stream(path, std::ios::binary | std::ios::trunc);
		stream << text;
	}

	std::string MakeGraphText(bool rootMotionClip)
	{
		if (rootMotionClip)
		{
			return
				"version: 1\n"
				"name: Root Motion Fixture\n"
				"initial: Move\n"
				"\n"
				"state Move\n"
				"  kind: clip\n"
				"  clip: RootMove\n"
				"  loop: true\n"
				"  speed: 1.0\n";
		}
		return
			"version: 1\n"
			"name: Runtime Fixture\n"
			"initial: Idle\n"
			"parameter: Speed 0\n"
			"parameter: VerticalSpeed 0\n"
			"parameter: AimWeight 0\n"
			"\n"
			"state Idle\n"
			"  kind: clip\n"
			"  clip: Idle\n"
			"  loop: true\n"
			"  speed: 1.0\n"
			"  transition: Walk 0.1 Speed > 0.5\n"
			"\n"
			"state Walk\n"
			"  kind: clip\n"
			"  clip: Idle\n"
			"  loop: true\n"
			"  speed: 1.0\n"
			"  transition: Idle 0.1 Speed < 0.1\n"
			"\n"
			"layer UpperBody\n"
			"  clip: RootMove\n"
			"  weight: AimWeight\n"
			"  mask: Root 0\n";
	}

	entt::entity AddCharacter(Scene& scene, const std::string& modelPath, const std::string& graphPath,
		bool withController)
	{
		const entt::entity entity = scene.GetRegistry().create();
		scene.GetRegistry().emplace<TransformComponent>(entity);
		SkinnedMeshRendererComponent skinned;
		skinned.Mesh.Path = StringPool::Get().InternPath(modelPath);
		scene.GetRegistry().emplace<SkinnedMeshRendererComponent>(entity, skinned);
		AnimGraphComponent graph;
		graph.AnimGraphAsset.Path = StringPool::Get().InternPath(graphPath);
		scene.GetRegistry().emplace<AnimGraphComponent>(entity, graph);
		if (withController)
		{
			CharacterControllerComponent controller;
			controller.Grounded = true;
			scene.GetRegistry().emplace<CharacterControllerComponent>(entity, controller);
		}
		return entity;
	}

	void RunStateMachineIntegration(const fs::path& root)
	{
		const fs::path modelPath = root / "runtime_leg.wmodel";
		const fs::path graphPath = root / "runtime_graph.wanimgraph";
		WriteModel(modelPath);
		WriteText(graphPath, MakeGraphText(false));

		WorldContext context;
		Scene scene(context);
		AnimationSystem::ClearCache();
		AnimGraphSystem::ClearCache();

		const entt::entity entity = AddCharacter(scene, modelPath.string(), graphPath.string(), true);

		AnimGraphSystem::Update(scene, 0.016f);
		const std::vector<glm::mat4>* palette = AnimGraphSystem::GetPalette(entity);
		CHECK(palette != nullptr);
		if (palette)
			CHECK(palette->size() == 8u);
		const std::vector<glm::mat4>* mirrored = AnimationSystem::GetPalette(entity);
		CHECK(mirrored != nullptr);
		if (mirrored && palette)
			CHECK(mirrored->size() == palette->size());   // 覆盖槽已同步

		AnimGraphRuntimeInfo info;
		CHECK(AnimGraphSystem::GetRuntimeInfo(entity, info));
		CHECK(info.Loaded);
		CHECK(info.CurrentState == "Idle");

		// SetParameter 覆盖：2 帧内即使 Velocity 很大也保持 Idle。
		scene.GetRegistry().get<CharacterControllerComponent>(entity).Velocity = glm::vec3(2.0f, 0.0f, 0.0f);
		CHECK(AnimGraphSystem::SetParameter(entity, "Speed", 0.0f));
		AnimGraphSystem::Update(scene, 0.016f);
		CHECK(AnimGraphSystem::GetRuntimeInfo(entity, info));
		CHECK(info.CurrentState == "Idle");
		CHECK(!info.InTransition);
		AnimGraphSystem::Update(scene, 0.016f);
		CHECK(AnimGraphSystem::GetRuntimeInfo(entity, info));
		CHECK(info.CurrentState == "Idle");

		// 第 3 帧覆盖过期 → Speed 回到 controller 推导值 2.0 → CrossFade 后切到 Walk。
		AnimGraphSystem::Update(scene, 0.016f);
		CHECK(AnimGraphSystem::GetRuntimeInfo(entity, info));
		CHECK(info.Speed > 1.5f);
		for (int i = 0; i < 20 && info.CurrentState != "Walk"; ++i)
		{
			AnimGraphSystem::Update(scene, 0.016f);
			CHECK(AnimGraphSystem::GetRuntimeInfo(entity, info));
		}
		CHECK(info.CurrentState == "Walk");
		CHECK(scene.GetRegistry().get<AnimGraphComponent>(entity).CurrentStateHash == Fnv1a32("Walk"));

		// 参数回读：override 已过期，读的是图内实时参数。
		CHECK(Nearly(AnimGraphSystem::GetParameter(entity, "Speed"), 2.0f));

		// 停下 → 回到 Idle。
		scene.GetRegistry().get<CharacterControllerComponent>(entity).Velocity = glm::vec3(0.0f);
		for (int i = 0; i < 20; ++i)
			AnimGraphSystem::Update(scene, 0.016f);
		CHECK(AnimGraphSystem::GetRuntimeInfo(entity, info));
		CHECK(info.CurrentState == "Idle");
		CHECK(scene.GetRegistry().get<AnimGraphComponent>(entity).CurrentStateHash == Fnv1a32("Idle"));

		// 垂直速度推导（contract amendment）：Collision 未落地 → IsFalling=1 / VerticalSpeed 保留。
		CharacterControllerComponent& controller = scene.GetRegistry().get<CharacterControllerComponent>(entity);
		controller.Velocity = glm::vec3(0.0f, 3.0f, 0.0f);
		controller.Grounded = false;
		AnimGraphSystem::Update(scene, 0.016f);
		CHECK(AnimGraphSystem::GetRuntimeInfo(entity, info));
		CHECK(!info.Grounded);
		CHECK(Nearly(AnimGraphSystem::GetParameter(entity, "VerticalSpeed"), 3.0f));
		CHECK(Nearly(AnimGraphSystem::GetParameter(entity, "IsFalling"), 1.0f));

		// Direction 推导：世界速度 → 实体本地速度的 yaw（面向 +Z = 0，右 = +90）。
		controller.Grounded = true;
		TransformComponent& transform = scene.GetRegistry().get<TransformComponent>(entity);
		transform.Rotation = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
		controller.Velocity = glm::vec3(0.0f, 0.0f, 2.0f);       // 世界 +Z、朝向 +Z → 0°
		AnimGraphSystem::Update(scene, 0.016f);
		CHECK(Nearly(AnimGraphSystem::GetParameter(entity, "Direction"), 0.0f, 1e-3f));
		CHECK(Nearly(AnimGraphSystem::GetParameter(entity, "Speed"), 2.0f, 1e-3f));

		// 实体朝 -90°（前向 +Z 转向 -X）时，世界 +Z 就是实体本地 +X（角色左侧）→ -90°。
		transform.Rotation = glm::angleAxis(glm::radians(-90.0f), glm::vec3(0.0f, 1.0f, 0.0f));
		controller.Velocity = glm::vec3(0.0f, 0.0f, 2.0f);       // 世界 +Z 在实体本地是 +X → -90°
		AnimGraphSystem::Update(scene, 0.016f);
		CHECK(Nearly(AnimGraphSystem::GetParameter(entity, "Direction"), -90.0f, 1e-3f));

		// 面向 +Z、速度朝本地 -X（角色右侧）→ +90°。
		transform.Rotation = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
		controller.Velocity = glm::vec3(-2.0f, 0.0f, 0.0f);
		AnimGraphSystem::Update(scene, 0.016f);
		CHECK(Nearly(AnimGraphSystem::GetParameter(entity, "Direction"), 90.0f, 1e-3f));

		// 上层遮罩标志直接驱动层权重参数（示例脚本只切 flag，不写 AimWeight）。
		transform.Rotation = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
		controller.Velocity = glm::vec3(0.0f);
		scene.GetRegistry().get<AnimGraphComponent>(entity).EnableUpperBodyLayer = 0;
		AnimGraphSystem::Update(scene, 0.016f);
		CHECK(Nearly(AnimGraphSystem::GetParameter(entity, "AimWeight"), 0.0f, 1e-6f));
		scene.GetRegistry().get<AnimGraphComponent>(entity).EnableUpperBodyLayer = 1;
		AnimGraphSystem::Update(scene, 0.016f);
		CHECK(Nearly(AnimGraphSystem::GetParameter(entity, "AimWeight"), 1.0f, 1e-6f));

		// 显式 override 仍然优先（面板滑杆可以做 0..1 的平滑过渡）。
		CHECK(AnimGraphSystem::SetParameter(entity, "AimWeight", 0.35f));
		AnimGraphSystem::Update(scene, 0.016f);
		CHECK(Nearly(AnimGraphSystem::GetParameter(entity, "AimWeight"), 0.35f, 1e-6f));
	}

	void RunFootIk(const fs::path& root)
	{
		const fs::path modelPath = root / "foot_ik_leg.wmodel";
		const fs::path graphPath = root / "foot_ik_graph.wanimgraph";
		const fs::path terrainPath = root / "foot_ik.terrain";
		WriteModel(modelPath);
		WriteText(graphPath, MakeGraphText(false));

		TerrainData terrain(64, 64, 1.0f, 1.0f);
		for (uint32_t z = 0; z < 64; ++z)
			for (uint32_t x = 0; x < 64; ++x)
				terrain.SetHeight(x, z, 0.4f + 0.1f * static_cast<float>(x));
		CHECK(terrain.SaveToFile(terrainPath.string()));

		WorldContext context;
		Scene scene(context);
		AnimationSystem::ClearCache();
		AnimGraphSystem::ClearCache();

		const entt::entity entity = AddCharacter(scene, modelPath.string(), graphPath.string(), false);

		// 地形实体**带平移**：SampleHeight/SampleNormal 吃的是地形本地坐标，不换算就会采样到
		// 世界 x=10.1（高度 1.41）而不是本地 x=0.1（高度 0.41）—— 1 m 的判别性误差。
		const glm::vec3 terrainOffset(10.0f, 0.0f, 10.0f);
		const entt::entity terrainEntity = scene.GetRegistry().create();
		scene.GetRegistry().emplace<TransformComponent>(terrainEntity, terrainOffset);
		TerrainComponent terrainComponent;
		terrainComponent.TerrainAsset.Path = StringPool::Get().InternPath(terrainPath.string());
		scene.GetRegistry().emplace<TerrainComponent>(terrainEntity, terrainComponent);

		TransformComponent& characterTransform = scene.GetRegistry().get<TransformComponent>(entity);
		characterTransform.Location = terrainOffset;   // 角色站在地形本地原点附近

		AnimGraphSystem::Update(scene, 0.016f);

		const std::vector<glm::mat4>* palette = AnimGraphSystem::GetPalette(entity);
		CHECK(palette != nullptr);
		if (!palette || palette->size() < 8u)
			return;

		const glm::mat4 characterMatrix = characterTransform.GetLocalMatrix();
		const glm::mat4 terrainMatrix = scene.GetRegistry().get<TransformComponent>(terrainEntity).GetLocalMatrix();
		const glm::mat4 inverseTerrain = glm::inverse(terrainMatrix);
		const float restHeight = 0.10f;

		// skin.JointNodes = { Root, Hips, Thigh.L, Shin.L, Foot.L, Thigh.R, Shin.R, Foot.R }
		const auto worldFoot = [&](size_t jointIndex)
		{
			return glm::vec3(characterMatrix * glm::vec4(glm::vec3((*palette)[jointIndex][3]), 1.0f));
		};
		const glm::vec3 leftFoot = worldFoot(4);
		const glm::vec3 rightFoot = worldFoot(7);

		const glm::vec3 leftLocal = glm::vec3(inverseTerrain * glm::vec4(leftFoot, 1.0f));
		const glm::vec3 rightLocal = glm::vec3(inverseTerrain * glm::vec4(rightFoot, 1.0f));
		const float expectedLeftY = terrainMatrix[3].y + terrain.SampleHeight(leftLocal.x, leftLocal.z) + restHeight;
		const float expectedRightY = terrainMatrix[3].y + terrain.SampleHeight(rightLocal.x, rightLocal.z) + restHeight;

		CHECK(Nearly(leftFoot.y, expectedLeftY, 1e-4f));
		CHECK(Nearly(rightFoot.y, expectedRightY, 1e-4f));
		CHECK(Nearly(leftLocal.x, 0.1f, 1e-3f));    // 脚不横向滑动（地形本地空间）
		CHECK(Nearly(rightLocal.x, -0.1f, 1e-3f));
		CHECK(std::isfinite(leftFoot.y) && std::isfinite(rightFoot.y));

		// 关闭 Foot IK → 回到绑定姿态（脚踝 0.10，模型空间），证明 IK 真的生效过。
		scene.GetRegistry().get<AnimGraphComponent>(entity).EnableFootIk = 0;
		AnimGraphSystem::Update(scene, 0.016f);
		palette = AnimGraphSystem::GetPalette(entity);
		CHECK(palette != nullptr);
		if (palette && palette->size() >= 8u)
			CHECK(Nearly(glm::vec3((*palette)[4][3]).y, restHeight, 1e-3f));
	}

	void RunRootMotion(const fs::path& root)
	{
		const fs::path modelPath = root / "root_motion_leg.wmodel";
		const fs::path graphPath = root / "root_motion_graph.wanimgraph";
		WriteModel(modelPath);
		WriteText(graphPath, MakeGraphText(true));

		WorldContext context;
		Scene scene(context);
		AnimationSystem::ClearCache();
		AnimGraphSystem::ClearCache();

		const entt::entity entity = AddCharacter(scene, modelPath.string(), graphPath.string(), false);
		// 运行态禁止 GetRegistry()（结构写守卫）；组件指针在启动前取好，运行期只改字段。
		TransformComponent& transform = scene.GetRegistry().get<TransformComponent>(entity);
		AnimGraphComponent& animGraph = scene.GetRegistry().get<AnimGraphComponent>(entity);
		scene.OnRuntimeStart();
		transform.Rotation = glm::angleAxis(glm::radians(90.0f), glm::vec3(0.0f, 1.0f, 0.0f));

		AnimGraphSystem::Update(scene, 0.016f);   // 第一帧只记录根样本
		const glm::vec3 start = transform.Location;
		AnimGraphSystem::Update(scene, 0.1f);

		// 模型空间 +X 经实体 yaw 90° 后 → 世界 -Z。
		CHECK(Nearly(transform.Location.x, start.x, 1e-4f));
		CHECK(transform.Location.z < start.z - 1e-3f);

		// 权重 0 → 不再位移。
		animGraph.RootMotionWeight = 0.0f;
		const glm::vec3 before = transform.Location;
		AnimGraphSystem::Update(scene, 0.1f);
		CHECK(Nearly(glm::vec3(transform.Location).x, before.x, 1e-5f));
		CHECK(Nearly(glm::vec3(transform.Location).z, before.z, 1e-5f));

		scene.OnRuntimeStop();
	}
}

int main()
{
	const fs::path root = fs::temp_directory_path() / "we_animgraph_runtime";
	std::error_code ec;
	fs::create_directories(root, ec);

	struct Case
	{
		const char* Name;
		void (*Run)(const fs::path&);
	};
	const Case cases[] = {
		{ "Runtime state machine + CurrentStateHash + VerticalSpeed", RunStateMachineIntegration },
		{ "Foot IK reaches terrain target (<= 1e-4)", RunFootIk },
		{ "Root motion delta rotated by entity heading", RunRootMotion },
	};

	int failures = 0;
	for (const Case& test : cases)
	{
		try
		{
			test.Run(root);
			std::printf("[PASS] %s\n", test.Name);
		}
		catch (const std::exception& error)
		{
			++failures;
			std::printf("[FAIL] %s — %s\n", test.Name, error.what());
		}
	}

	if (failures != 0)
	{
		std::printf("%d AnimGraph runtime test(s) failed\n", failures);
		return 1;
	}
	std::printf("All World.AnimGraphRuntime tests passed!\n");
	return 0;
}

