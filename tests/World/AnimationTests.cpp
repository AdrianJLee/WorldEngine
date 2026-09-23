// D5c-4a:骨骼动画系统(AnimationSystem)headless 回归 —— 纯 CPU,无设备/窗口依赖。
//
// 覆盖:
//   1. ComputePalette:节点父链 + 动画采样 + JointNodes 间接寻址 + meshWorld 的逆;
//   2. AdvanceTime:Speed 倍率 / Loop 回绕 / 非 Loop 夹到 duration / Playing=false 不推进;
//   3. 边界:无 skin / 越界 skin → 空调色板;空 clip(无通道)→ 绑定姿态(不崩);
//   4. Update:真实 .wmodel 读盘 + Time 写回 + 按实体缓存调色板 + 坏路径/未知 clip 不崩。
#include "wldpch.h"

#include "World/Renderer/AnimationSystem.h"

#include "World/Core/Asset/WModelIO.h"
#include "World/Core/WorldContext.h"
#include "World/Scene/Components.h"
#include "World/Scene/Scene.h"

#include <glm/gtc/quaternion.hpp>

#include <cmath>
#include <cstdio>
#include <filesystem>
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

	bool Nearly(float left, float right, float epsilon = 1e-5f)
	{
		return std::fabs(left - right) <= epsilon;
	}

	bool NearlyVec3(const glm::vec3& left, const glm::vec3& right, float epsilon = 1e-5f)
	{
		return Nearly(left.x, right.x, epsilon) && Nearly(left.y, right.y, epsilon)
			&& Nearly(left.z, right.z, epsilon);
	}

	bool NearlyMat4(const glm::mat4& left, const glm::mat4& right, float epsilon = 1e-5f)
	{
		for (int column = 0; column < 4; ++column)
			for (int row = 0; row < 4; ++row)
				if (!Nearly(left[column][row], right[column][row], epsilon))
					return false;
		return true;
	}

	Asset::WModelAnimationChannel MakeChannel(uint32_t node,
		Asset::WModelIO::WModelAnimationPath path,
		std::vector<Asset::WModelAnimationKey> keys)
	{
		Asset::WModelAnimationChannel channel;
		channel.TargetNode = node;
		channel.Path = path;
		channel.Keys = std::move(keys);
		return channel;
	}

	// 夹具:节点 0 = 网格节点(平移 (0,3,0),MeshIndex 0);节点 1/2 = 两个关节(都挂节点 0,绑定 TRS 单位)。
	// skin.JointNodes = {2,1} —— 关节 0 → 节点 2(有动画通道),关节 1 → 节点 1(无通道):
	// 节点下标与关节下标故意错开,间接寻址写错会立刻暴露(动画 TargetNode 用节点下标)。
	Asset::WModelData MakeSkinnedModel()
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
			{ { 1.0f, 0.0f, 0.0f, 0.0f }, { 1.0f, 0.0f, 0.0f, 0.0f } },
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
		meshNode.Parent = -1;
		meshNode.MeshIndex = 0;
		meshNode.Translation = { 0.0f, 3.0f, 0.0f };
		meshNode.Name = "SkinnedMeshNode";
		Asset::WModelNode jointA;
		jointA.Parent = 0;
		jointA.Name = "JointA";
		Asset::WModelNode jointB;
		jointB.Parent = 0;
		jointB.Name = "JointB";
		model.Nodes = { meshNode, jointA, jointB };

		Asset::WModelSkin skin;
		skin.Name = "TwoJointSkin";
		skin.JointNames = { "JointB", "JointA" };
		skin.JointNodes = { 2u, 1u };
		skin.JointParents = { -1, -1 };
		skin.InverseBindMatrices = { glm::mat4(1.0f), glm::mat4(1.0f) };
		skin.BindTranslations = { glm::vec3(0.0f), glm::vec3(0.0f) };
		skin.BindRotations = { glm::quat(1.0f, 0.0f, 0.0f, 0.0f), glm::quat(1.0f, 0.0f, 0.0f, 0.0f) };
		skin.BindScales = { glm::vec3(1.0f), glm::vec3(1.0f) };
		model.Skins = { skin };

		Asset::WModelAnimation move;
		move.Name = "Move";
		move.Duration = 1.0f;
		move.Channels = {
			MakeChannel(2u, Asset::WModelIO::WModelAnimationPath::Translation, {
				{ 0.0f, { 0.0f, 0.0f, 0.0f, 0.0f } },
				{ 1.0f, { 0.0f, 1.0f, 0.0f, 0.0f } },
			}),
		};
		model.Animations = { move };
		return model;
	}

	// 1.ComputePalette:节点父链 + 动画采样 + JointNodes 间接寻址 + meshWorld 的逆。
	void ComputePaletteFollowsNodeTreeAndJointIndirection()
	{
		const Asset::WModelData model = MakeSkinnedModel();
		const Asset::WModelAnimation& move = model.Animations.front();

		// t=0:动画回到绑定姿态;网格节点平移 (0,3,0) 被 inverse(meshWorld) 抵消 → 两个关节都是单位阵。
		// 这条同时钉住"meshWorld = 网格节点的世界矩阵,不是单位阵"(取错时平移列会是 (0,3,0))。
		const std::vector<glm::mat4> atStart = AnimationSystem::ComputePalette(model, 0u, move, 0.0f);
		CHECK(atStart.size() == 2u);
		CHECK(NearlyMat4(atStart[0], glm::mat4(1.0f)));
		CHECK(NearlyMat4(atStart[1], glm::mat4(1.0f)));

		// t=0.5:通道只作用在**节点 2**(= skin 关节 0,经 JointNodes 间接寻址)→ 关节 0 平移 (0,0.5,0),
		// 关节 1(节点 1,没有通道)保持单位阵。
		const std::vector<glm::mat4> atHalf = AnimationSystem::ComputePalette(model, 0u, move, 0.5f);
		CHECK(atHalf.size() == 2u);
		CHECK(NearlyVec3(glm::vec3(atHalf[0][3]), glm::vec3(0.0f, 0.5f, 0.0f)));
		CHECK(NearlyMat4(atHalf[1], glm::mat4(1.0f)));

		// t=1(整段平移;duration 之外由 SampleAnimation clamp)。
		const std::vector<glm::mat4> atEnd = AnimationSystem::ComputePalette(model, 0u, move, 1.0f);
		CHECK(atEnd.size() == 2u);
		CHECK(NearlyVec3(glm::vec3(atEnd[0][3]), glm::vec3(0.0f, 1.0f, 0.0f)));
		CHECK(NearlyMat4(atEnd[1], glm::mat4(1.0f)));
	}

	// 2.AdvanceTime:Speed 倍率 / Loop 回绕 / 非 Loop 停在 duration / Playing=false 不推进。
	void AdvanceTimeHonorsSpeedLoopAndPlaying()
	{
		// Speed=2 推进 1s → 2s(duration 4s 不触发回绕)。
		CHECK(Nearly(AnimationSystem::AdvanceTime(0.0f, 1.0f, 2.0f, true, true, 4.0f), 2.0f));
		// Loop=true 且 clip 时长 1s:0.5s × Speed 2 = 1s → 回绕到 0。
		CHECK(Nearly(AnimationSystem::AdvanceTime(0.0f, 0.5f, 2.0f, true, true, 1.0f), 0.0f));
		// 回绕后继续在 [0, duration) 内累加(0.6s × 2 = 1.2 → 0.2)。
		CHECK(Nearly(AnimationSystem::AdvanceTime(0.0f, 0.6f, 2.0f, true, true, 1.0f), 0.2f));
		// Loop=false:夹在 duration(0.9 + 0.5 → 1.0)。
		CHECK(Nearly(AnimationSystem::AdvanceTime(0.9f, 0.5f, 1.0f, true, false, 1.0f), 1.0f));
		// Playing=false:原值返回。
		CHECK(Nearly(AnimationSystem::AdvanceTime(0.35f, 1.0f, 1.0f, false, true, 1.0f), 0.35f));
		// duration=0(无 clip / 零时长):不回绕,直接累加。
		CHECK(Nearly(AnimationSystem::AdvanceTime(0.0f, 2.5f, 1.0f, true, true, 0.0f), 2.5f));
		// 非有限输入不写进组件:NaN 的 dt 不改动;NaN 的当前时间归一为 0。
		CHECK(Nearly(AnimationSystem::AdvanceTime(0.25f, std::nanf(""), 1.0f, true, true, 1.0f), 0.25f));
		CHECK(Nearly(AnimationSystem::AdvanceTime(std::nanf(""), 1.0f, 1.0f, true, true, 1.0f), 0.0f));
	}

	// 3.边界:无 skin / 越界 skin → 空调色板;空 clip(无通道)→ 绑定姿态,不崩。
	void ComputePaletteHandlesMissingSkinAndEmptyClips()
	{
		const Asset::WModelData model = MakeSkinnedModel();
		const Asset::WModelAnimation& move = model.Animations.front();

		// skinIndex 越界 → 空。
		CHECK(AnimationSystem::ComputePalette(model, 1u, move, 0.5f).empty());
		// 没有骨架的静态模型 → 空。
		Asset::WModelData staticModel = model;
		staticModel.Skins.clear();
		CHECK(AnimationSystem::ComputePalette(staticModel, 0u, move, 0.5f).empty());
		// 骨架存在但关节集合为空(容器不变量之外的手工数据)→ 空,不越界。
		Asset::WModelData noJoints = model;
		noJoints.Skins[0].JointNodes.clear();
		noJoints.Skins[0].InverseBindMatrices.clear();
		CHECK(AnimationSystem::ComputePalette(noJoints, 0u, move, 0.5f).empty());
		// 空 clip(无通道)= 绑定姿态:调色板非空且为单位阵 —— 没有 clip 的蒙皮模型仍然能被画出来。
		const Asset::WModelAnimation emptyClip;
		const std::vector<glm::mat4> bindPose = AnimationSystem::ComputePalette(model, 0u, emptyClip, 0.0f);
		CHECK(bindPose.size() == 2u);
		CHECK(NearlyMat4(bindPose[0], glm::mat4(1.0f)));
		CHECK(NearlyMat4(bindPose[1], glm::mat4(1.0f)));
	}

	// 4.Update:真实 .wmodel 读盘 + Time 写回组件 + 按实体缓存调色板;坏路径/未知 clip 不崩。
	void UpdateAdvancesTimeAndCachesPalette()
	{
		const fs::path root = fs::temp_directory_path() / "we-animation-tests";
		fs::remove_all(root);
		fs::create_directories(root);
		const fs::path modelFile = root / "skinned.wmodel";
		std::string error;
		CHECK(Asset::WModelIO::WriteFile(modelFile.string(), MakeSkinnedModel(), &error));

		WorldContext context;
		Scene scene(context);
		AnimationSystem::ClearCache();

		const entt::entity entity = scene.GetRegistry().create();
		SkinnedMeshRendererComponent component;
		component.MeshPath = modelFile.string();   // 绝对路径:ReadFile 的候选路径都会命中它
		component.AnimationClip = "Move";
		component.Speed = 2.0f;
		scene.GetRegistry().emplace<SkinnedMeshRendererComponent>(entity, component);

		// 0.25s × Speed 2 = +0.5s → Time=0.5,调色板 = t=0.5(关节 0 平移半程)。
		AnimationSystem::Update(scene, 0.25f);
		CHECK(Nearly(scene.GetRegistry().get<SkinnedMeshRendererComponent>(entity).Time, 0.5f));
		const std::vector<glm::mat4>* palette = AnimationSystem::GetPalette(entity);
		CHECK(palette != nullptr);
		if (palette)
		{
			CHECK(palette->size() == 2u);
			CHECK(NearlyVec3(glm::vec3((*palette)[0][3]), glm::vec3(0.0f, 0.5f, 0.0f)));
		}

		// 再 +0.5s(clip 时长 1s,Loop=true)→ 回绕到 0。
		AnimationSystem::Update(scene, 0.25f);
		CHECK(Nearly(scene.GetRegistry().get<SkinnedMeshRendererComponent>(entity).Time, 0.0f));
		CHECK(AnimationSystem::GetPalette(entity) != nullptr);

		// Playing=false:Time 不再推进;没有该组件的实体取不到调色板。
		scene.GetRegistry().get<SkinnedMeshRendererComponent>(entity).Playing = false;
		AnimationSystem::Update(scene, 1.0f);
		CHECK(Nearly(scene.GetRegistry().get<SkinnedMeshRendererComponent>(entity).Time, 0.0f));
		const entt::entity plain = scene.GetRegistry().create();
		AnimationSystem::Update(scene, 0.1f);
		CHECK(AnimationSystem::GetPalette(plain) == nullptr);

		// 未知 clip:不推 Time、按绑定姿态出调色板(非空),不崩。
		scene.GetRegistry().get<SkinnedMeshRendererComponent>(entity).Playing = true;
		scene.GetRegistry().get<SkinnedMeshRendererComponent>(entity).AnimationClip = "NoSuchClip";
		AnimationSystem::Update(scene, 0.5f);
		CHECK(Nearly(scene.GetRegistry().get<SkinnedMeshRendererComponent>(entity).Time, 0.0f));
		CHECK(AnimationSystem::GetPalette(entity) != nullptr);

		// 坏路径:读失败 → 跳过该实体(不崩、Time 不动、没有调色板);失败路径不逐帧重试。
		const entt::entity broken = scene.GetRegistry().create();
		SkinnedMeshRendererComponent brokenComponent;
		brokenComponent.MeshPath = (root / "missing.wmodel").string();
		brokenComponent.Time = 0.25f;
		scene.GetRegistry().emplace<SkinnedMeshRendererComponent>(broken, brokenComponent);
		AnimationSystem::Update(scene, 0.5f);
		CHECK(AnimationSystem::GetPalette(broken) == nullptr);
		CHECK(Nearly(scene.GetRegistry().get<SkinnedMeshRendererComponent>(broken).Time, 0.25f));

		AnimationSystem::ClearCache();
		fs::remove_all(root);
	}
}

int main()
{
	try
	{
		const std::pair<const char*, void(*)()> tests[] = {
			{ "ComputePalette follows the node tree + JointNodes indirection",
				ComputePaletteFollowsNodeTreeAndJointIndirection },
			{ "AdvanceTime honors speed / loop / playing", AdvanceTimeHonorsSpeedLoopAndPlaying },
			{ "ComputePalette handles missing skins + empty clips",
				ComputePaletteHandlesMissingSkinAndEmptyClips },
			{ "Update advances Time and caches palettes per entity", UpdateAdvancesTimeAndCachesPalette },
		};
		int failures = 0;
		for (const auto& [name, test] : tests)
		{
			try { test(); std::printf("[PASS] %s\n", name); }
			catch (const std::exception& error)
			{
				++failures;
				std::fprintf(stderr, "[FAIL] %s: %s\n", name, error.what());
			}
			catch (...)
			{
				++failures;
				std::fprintf(stderr, "[FAIL] %s: unknown exception\n", name);
			}
		}
		if (failures == 0)
			std::printf("World.Animation: all checks passed\n");
		else
			std::fprintf(stderr, "World.Animation: %d group(s) failed\n", failures);
		return failures == 0 ? 0 : 1;
	}
	catch (const std::exception& error)
	{
		std::fprintf(stderr, "World.Animation: fatal: %s\n", error.what());
		return 1;
	}
}
