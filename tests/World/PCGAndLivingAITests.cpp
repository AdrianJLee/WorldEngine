#include "World/Foliage/Foliage.h"
#include "World/Environment/PCGBiome.h"
#include "World/Level/AI/EQS/EQSTypes.h"
#include "World/Level/AI/EQS/EQSContext.h"
#include "World/Level/AI/EQS/EQSGenerator.h"
#include "World/Level/AI/EQS/EQSTest.h"
#include "World/Level/AI/EQS/EQSQuery.h"
#include "World/Level/AI/EQSCoverSystem.h"
#include "World/Foliage/FoliageInteraction.h"
#include "World/Terrain/Terrain.h"
#include "World/Level/Spline3D.h"
#include "World/Scene/Components.h"
#include "World/Scene/Scene.h"
#include "World/Scene/Entity.h"
#include "World/Core/WorldContext.h"

#include <glm/gtc/matrix_transform.hpp>
#include <cassert>
#include <cmath>
#include <iostream>
#include <vector>

using namespace World;

static void TestPCGBiomeClassification()
{
	assert(PCGBiomeGenerator::ClassifyBiome(200.0f, 10.0f, 0.5f) == BiomeType::SnowPeak);
	assert(PCGBiomeGenerator::ClassifyBiome(50.0f, 45.0f, 0.5f) == BiomeType::RockyMountain);
	assert(PCGBiomeGenerator::ClassifyBiome(30.0f, 10.0f, 0.1f) == BiomeType::Desert);
	assert(PCGBiomeGenerator::ClassifyBiome(20.0f, 10.0f, 0.8f) == BiomeType::Forest);
	assert(PCGBiomeGenerator::ClassifyBiome(20.0f, 10.0f, 0.35f) == BiomeType::Meadow);

	std::cout << "[PASS] TestPCGBiomeClassification\n";
}

static void TestSplineTerrainDeformation()
{
	TerrainData terrain(64, 64, 1.0f, 1.0f);
	// 制作一个高地 (高度 20m)
	for (uint32_t z = 0; z < 64; ++z)
		for (uint32_t x = 0; x < 64; ++x)
			terrain.SetHeight(x, z, 20.0f);

	Spline3D spline;
	spline.AddPoint(glm::vec3(10.0f, 10.0f, 32.0f));
	spline.AddPoint(glm::vec3(50.0f, 10.0f, 32.0f));

	// 沿样条线开槽，挖深到 10m
	PCGBiomeGenerator::DeformTerrainAlongSpline(terrain, spline, 6.0f, 0.0f, 4.0f);

	// 验证样条线中心位置的高度已被平整削切至 10m 附近
	float hOnRoad = terrain.SampleHeight(30.0f, 32.0f);
	assert(hOnRoad < 15.0f);

	std::cout << "[PASS] TestSplineTerrainDeformation\n";
}

static void TestEQSCoverSystem()
{
	// 威胁者位于原点 (0, 0, 0)
	const glm::vec3 threatPos(0.0f, 0.0f, 0.0f);
	// 询问者位于 (20, 0, 0)
	const glm::vec3 querierPos(20.0f, 0.0f, 0.0f);

	// 在两者之间放置一堵高墙障碍物 (X: 8~12, Y: 0~4, Z: -5~5)
	std::vector<CoverBox> obstacles = {
		{ { 8.0f, 0.0f, -5.0f }, { 12.0f, 4.0f, 5.0f } }
	};

	glm::vec3 bestCover;
	float bestScore = 0.0f;
	bool found = EQSCoverSystem::FindBestCover(querierPos, threatPos, 15.0f, 1.0f, obstacles, bestCover, bestScore);

	assert(found);
	assert(bestScore > 2.0f); // 视线完全遮蔽 (> 2.0)
	// 优质掩体应该位于墙壁后方 (X > 12.0)
	assert(bestCover.x >= 12.0f);

	std::cout << "[PASS] TestEQSCoverSystem (Best Cover: "
		<< bestCover.x << ", " << bestCover.y << ", " << bestCover.z << ", Score: " << bestScore << ")\n";
}

static void TestEQSMultiModes()
{
	const glm::vec3 threatPos(0.0f, 0.0f, 0.0f);
	const glm::vec3 querierPos(20.0f, 0.0f, 0.0f);
	std::vector<CoverBox> obstacles = {
		{ { 8.0f, 0.0f, -5.0f }, { 12.0f, 4.0f, 5.0f } }
	};

	glm::vec3 bestFlank;
	float flankScore = 0.0f;
	bool foundFlank = EQSCoverSystem::ExecuteQuery(
		EQSQueryMode::Flank, querierPos, threatPos, 15.0f, 1.0f, obstacles, bestFlank, flankScore);
	assert(foundFlank);
	assert(std::abs(bestFlank.z) > 1.0f);

	glm::vec3 bestRetreat;
	float retreatScore = 0.0f;
	bool foundRetreat = EQSCoverSystem::ExecuteQuery(
		EQSQueryMode::Retreat, querierPos, threatPos, 15.0f, 1.0f, obstacles, bestRetreat, retreatScore);
	assert(foundRetreat);
	assert(glm::distance(bestRetreat, threatPos) >= glm::distance(querierPos, threatPos) - 1.0f);

	std::cout << "[PASS] TestEQSMultiModes\n";
}

static void TestFoliageInteractionPhysics()
{
	const glm::vec3 foliagePos(10.0f, 0.0f, 10.0f);
	const glm::vec3 pusherPos(10.5f, 0.0f, 10.0f); // 距离 0.5m
	const glm::vec3 pusherVel(2.0f, 0.0f, 0.0f);

	glm::vec3 displacement;
	FoliageInteractionSystem::ComputePushDisplacement(foliagePos, pusherPos, pusherVel, 1.5f, 1.0f, displacement);

	// 应该产生远离推挤者的位移向量
	assert(glm::length(displacement) > 0.0f);

	// 模拟弹性阻尼回弹
	glm::vec3 disp = displacement;
	for (int i = 0; i < 10; ++i)
	{
		FoliageInteractionSystem::UpdateSpringRecovery(disp, 5.0f, 0.1f);
	}
	assert(glm::length(disp) < glm::length(displacement) * 0.1f); // 显著回弹复位

	std::cout << "[PASS] TestFoliageInteractionPhysics\n";
}

static void TestPCGFoliageBaking()
{
	FoliageData foliage;
	std::vector<PCGSpawnPoint> pts = {
		{ glm::vec3(10.0f, 0.0f, 10.0f), glm::vec3(0.0f, 1.0f, 0.0f), BiomeType::Forest, 0.8f },
		{ glm::vec3(20.0f, 0.0f, 20.0f), glm::vec3(0.0f, 1.0f, 0.0f), BiomeType::Meadow, 0.6f },
		{ glm::vec3(30.0f, 0.0f, 30.0f), glm::vec3(0.0f, 1.0f, 0.0f), BiomeType::RockyMountain, 0.4f },
	};

	uint32_t count = PCGBiomeGenerator::PopulateFoliageFromBiomePoints(pts, foliage, 1.0f);
	assert(count == 3);
	assert(foliage.GetTotalInstanceCount() == 3);
	assert(!foliage.GetChunks().empty());

	Spline3D roadSpline;
	roadSpline.AddPoint(glm::vec3(5.0f, 0.0f, 10.0f));
	roadSpline.AddPoint(glm::vec3(15.0f, 0.0f, 10.0f));
	roadSpline.UpdateTangents();
	uint32_t erased = PCGBiomeGenerator::ClearFoliageAlongSpline(foliage, roadSpline, 6.0f);
	assert(erased >= 1);

	std::cout << "[PASS] TestPCGFoliageBaking\n";
}

static void TestPCGAndLivingAIComponentLayout()
{
	BiomePCGComponent pcg;
	assert(sizeof(pcg) == 48);
	assert(pcg.MaxSlope == 35.0f);

	EQSComponent eqs;
	assert(sizeof(eqs) == 32);
	assert(eqs.QueryRadius == 20.0f);
	assert(eqs.QueryMode == 0);
	assert(eqs.GeneratorMode == 0);
	assert(eqs.IncludeCrouchCover == 1);
	assert(eqs.OptimalDistance == 8.0f);

	FoliageInteractionComponent fi;
	assert(sizeof(fi) == 32);
	assert(fi.PushRadius == 1.5f);

	std::cout << "[PASS] TestPCGAndLivingAIComponentLayout\n";
}

static void TestAdaptiveFiveBiomeCoverage()
{
	TerrainData terrain(64, 64, 1.0f, 1.0f);
	// 制作 0~40m 高度起伏地形，包含平原、高山、陡坡
	for (uint32_t z = 0; z < 64; ++z)
	{
		for (uint32_t x = 0; x < 64; ++x)
		{
			// 基础高度 0 ~ 40m
			float h = (static_cast<float>(z) / 63.0f) * 40.0f;
			// 在中间制造陡坡 (x: 20~24) 使得 slopeDeg 超过 35度
			if (x >= 20 && x <= 24)
			{
				h += static_cast<float>(x - 20) * 4.0f;
			}
			terrain.SetHeight(x, z, h);
		}
	}

	BiomePCGComponent comp;
	comp.MinElevation = -100.0f;
	comp.MaxElevation = 200.0f;
	comp.MaxSlope = 35.0f;
	comp.MoistureThreshold = 0.3f;
	comp.Seed = 42;

	std::vector<PCGSpawnPoint> points;
	PCGBiomeGenerator::GenerateBiomePoints(terrain, comp, points);

	assert(!points.empty());

	bool hasForest = false;
	bool hasMeadow = false;
	bool hasRockyMountain = false;
	bool hasDesert = false;
	bool hasSnowPeak = false;

	for (const auto& pt : points)
	{
		if (pt.Biome == BiomeType::Forest) hasForest = true;
		if (pt.Biome == BiomeType::Meadow) hasMeadow = true;
		if (pt.Biome == BiomeType::RockyMountain) hasRockyMountain = true;
		if (pt.Biome == BiomeType::Desert) hasDesert = true;
		if (pt.Biome == BiomeType::SnowPeak) hasSnowPeak = true;
	}

	assert(hasForest);
	assert(hasMeadow);
	assert(hasRockyMountain);
	assert(hasDesert);
	assert(hasSnowPeak);

	std::cout << "[PASS] TestAdaptiveFiveBiomeCoverage (Generated " << points.size() << " points across all 5 biomes)\n";
}

static void TestSlopeSplineGrading()
{
	TerrainData terrain(64, 64, 1.0f, 1.0f);
	// 制作一个斜坡地形 (沿 X 轴从 5m 升高到 35m)
	for (uint32_t z = 0; z < 64; ++z)
		for (uint32_t x = 0; x < 64; ++x)
			terrain.SetHeight(x, z, 5.0f + static_cast<float>(x) * 0.5f);

	Spline3D spline;
	spline.AddPoint(glm::vec3(10.0f, 10.0f, 32.0f));
	spline.AddPoint(glm::vec3(50.0f, 30.0f, 32.0f));

	// 沿样条线连续横截面平整放坡: roadWidth=6m, cutDepth=0m, slopeTransition=4m
	PCGBiomeGenerator::DeformTerrainAlongSpline(terrain, spline, 6.0f, 0.0f, 4.0f);

	// 在 x=30m 处 (中间截面):
	// 中心点 z=32, 左侧 z=30 (距中心 2m, 在路基 halfWidth=3m 范围内), 右侧 z=34 (距中心 2m)
	const float hCenter = terrain.GetHeight(30, 32);
	const float hLeft = terrain.GetHeight(30, 30);
	const float hRight = terrain.GetHeight(30, 34);

	// 断言横截面平整度极高: 左右两侧与中心高度差 < 0.1m
	assert(std::abs(hLeft - hCenter) < 0.1f);
	assert(std::abs(hRight - hCenter) < 0.1f);
	assert(std::abs(hCenter - 20.0f) < 0.1f);

	// 断言纵向平滑过渡 (x=20 到 x=40 连续爬坡)
	const float hBefore = terrain.GetHeight(20, 32);
	const float hAfter = terrain.GetHeight(40, 32);
	assert(hBefore < hCenter && hCenter < hAfter);
	assert(std::abs(hBefore - 15.0f) < 0.2f);
	assert(std::abs(hAfter - 25.0f) < 0.2f);

	std::cout << "[PASS] TestSlopeSplineGrading\n";
}

static void TestOBBCoverWallProximity()
{
	const glm::vec3 threatPos(0.0f, 0.0f, 0.0f);
	const glm::vec3 querierPos(20.0f, 0.0f, 0.0f);

	// 构造旋转 25 度的 OBB 掩体沙袋
	// 中心位于 (10, 1, 0), 半长宽 (1.5, 1.0, 4.0), 绕 Y 轴旋转 25 度
	CoverBox obb = CoverBox::FromOBB(glm::vec3(10.0f, 1.0f, 0.0f), glm::vec3(1.5f, 1.0f, 4.0f), glm::vec3(0.0f, 25.0f, 0.0f));
	assert(obb.IsOriented);

	std::vector<CoverBox> obstacles = { obb };
	std::vector<EQSCoverCandidate> candidates;
	glm::vec3 bestCover(0.0f);
	float bestScore = -1.0f;

	bool found = EQSCoverSystem::EvaluateQueryCandidates(
		EQSQueryMode::Cover, querierPos, threatPos, 15.0f, 0.75f,
		obstacles, candidates, bestCover, bestScore, nullptr, glm::mat4(1.0f), 2);

	assert(found);
	assert(bestScore > 2.0f);
	// 验证最佳掩体点不穿入 OBB 内部
	assert(!obb.ContainsPoint(bestCover, 0.0f));
	// 验证最佳掩体紧贴在掩体背弹面 (距离掩体墙面 < 2.0m)
	const float wallDist = obb.DistanceToSurfaceXZ(bestCover);
	assert(wallDist < 2.0f);
	// 验证在掩体后方 (X > 10.0)
	assert(bestCover.x > 10.0f);

	std::cout << "[PASS] TestOBBCoverWallProximity (Best Pos: "
		<< bestCover.x << ", " << bestCover.y << ", " << bestCover.z << ", WallDist: " << wallDist << ", Score: " << bestScore << ")\n";
}

static void TestStatefulFoliageInteractionField()
{
	FoliageInteractionSystem::ClearState();
	assert(FoliageInteractionSystem::GetActiveCellCount() == 0);

	FoliageInteractorSource source;
	source.SourceId = 1001;
	source.Position = glm::vec3(10.0f, 0.0f, 10.0f);
	source.Velocity = glm::vec3(2.0f, 0.0f, 0.0f);
	source.PushRadius = 2.0f;
	source.PushStrength = 1.5f;
	source.RecoverySpeed = 4.0f;
	source.LeaveTracks = true;

	FoliageInteractionSystem::BeginFrame(0.016f);
	FoliageInteractionSystem::SubmitInteractor(source);

	assert(FoliageInteractionSystem::GetActiveCellCount() > 0);

	// 采样靠近推挤者处的位移
	const glm::vec3 foliagePos(10.5f, 0.0f, 10.0f);
	const glm::vec3 disp = FoliageInteractionSystem::SampleFieldDisplacement(foliagePos);
	assert(glm::length(disp) > 0.1f);

	// 验证 ApplyPivotBendTransform 保长支点弯曲
	const glm::mat4 baseTransform = glm::translate(glm::mat4(1.0f), foliagePos);
	const glm::mat4 bentTransform = FoliageInteractionSystem::ApplyPivotBendTransform(baseTransform, disp, 0.35f);

	// 根部坐标 Transform[3] 保持不变
	const glm::vec3 newRoot = glm::vec3(bentTransform[3]);
	assert(glm::distance(newRoot, foliagePos) < 1e-4f);

	// 顶部发生外倾偏移 (+X 方向)
	const glm::vec4 topLocal(0.0f, 1.0f, 0.0f, 1.0f);
	const glm::vec3 topWorld = glm::vec3(bentTransform * topLocal);
	assert(topWorld.x > foliagePos.x);

	// 模拟交互源离开，连续多帧模拟衰减
	FoliageInteractionSystem::BeginFrame(0.1f);
	for (int i = 0; i < 40; ++i)
	{
		FoliageInteractionSystem::StepSimulation(0.1f);
	}

	const glm::vec3 recoveredDisp = FoliageInteractionSystem::SampleFieldDisplacement(foliagePos);
	assert(glm::length(recoveredDisp) < 0.05f);

	std::cout << "[PASS] TestStatefulFoliageInteractionField\n";
}

static void TestPCGPreservesHandPaintedFoliage()
{
	FoliageData foliage;
	assert(foliage.GetTypeCount() > 0);

	// 1. 先用 foliage.PaintFoliage 手绘若干颗植物（断言 GetHandPaintedInstanceCount() > 0）
	uint32_t painted = foliage.PaintFoliage(glm::vec3(0.0f, 0.0f, 0.0f), 10.0f, 1.0f, 0);
	assert(painted > 0);
	size_t handPaintedCountBefore = foliage.GetHandPaintedInstanceCount();
	assert(handPaintedCountBefore > 0);
	assert(foliage.GetPCGInstanceCount() == 0);

	// 2. 再调用 PCGBiomeGenerator::PopulateFoliageFromBiomePoints 烘焙 20+ 个 PCG 点
	std::vector<PCGSpawnPoint> pts;
	for (int i = 0; i < 25; ++i)
	{
		PCGSpawnPoint p;
		p.Position = glm::vec3(50.0f + static_cast<float>(i) * 2.0f, 0.0f, 50.0f);
		p.Normal = glm::vec3(0.0f, 1.0f, 0.0f);
		p.Biome = BiomeType::Meadow;
		p.Weight = 1.0f;
		pts.push_back(p);
	}

	uint32_t pcgSpawned = PCGBiomeGenerator::PopulateFoliageFromBiomePoints(pts, foliage, 1.0f, true);
	assert(pcgSpawned == 25);
	// 断言手绘实例数 GetHandPaintedInstanceCount() 100% 完好无损
	assert(foliage.GetHandPaintedInstanceCount() == handPaintedCountBefore);
	assert(foliage.GetPCGInstanceCount() == 25);
	assert(foliage.GetTotalInstanceCount() == handPaintedCountBefore + 25);

	// 3. 断言 ClearPCGInstances() 仅清除 PCG 实例而不删手绘实例！
	uint32_t cleared = foliage.ClearPCGInstances();
	assert(cleared == 25);
	assert(foliage.GetPCGInstanceCount() == 0);
	assert(foliage.GetHandPaintedInstanceCount() == handPaintedCountBefore);
	assert(foliage.GetTotalInstanceCount() == handPaintedCountBefore);

	std::cout << "[PASS] TestPCGPreservesHandPaintedFoliage\n";
}

static void TestEQSAndBehaviorTreeSynergy()
{
	WorldContext context;
	Scene scene(context);

	// 创建一个 "Threat Sniper" 实体
	Entity threat = Entity::CreateEntity(&scene, "Threat Sniper");
	auto& threatTr = threat.AddComponent<TransformComponent>();
	threatTr.Location = glm::vec3(0.0f, 0.0f, 0.0f);

	// 创建一个旋转 OBB 掩体墙实体 (位于 10, 0, 0)
	Entity wall = Entity::CreateEntity(&scene, "Cover Wall");
	auto& wallTr = wall.AddComponent<TransformComponent>();
	wallTr.Location = glm::vec3(10.0f, 0.0f, 0.0f);
	wallTr.Rotation = glm::quat(glm::radians(glm::vec3(0.0f, 25.0f, 0.0f)));
	auto& col = wall.AddComponent<BoxCollider3DComponent>();
	col.HalfExtents = glm::vec3(1.5f, 2.0f, 4.0f);

	// 创建一个挂载 TransformComponent + EQSComponent + BehaviorTreeComponent 的 AI 实体 (位于 20, 0, 0)
	Entity ai = Entity::CreateEntity(&scene, "AI Agent");
	auto& aiTr = ai.AddComponent<TransformComponent>();
	aiTr.Location = glm::vec3(20.0f, 0.0f, 0.0f);
	auto& eqs = ai.AddComponent<EQSComponent>();
	eqs.QueryRadius = 15.0f;
	eqs.GridSpacing = 1.0f;
	eqs.QueryMode = 0; // Cover
	auto& bt = ai.AddComponent<BehaviorTreeComponent>();
	bt.Enabled = 1;
	bt.TargetEntity = static_cast<uint32_t>((entt::entity)threat); // 指向威胁源

	// 验证 EQSCoverSystem::ResolveSceneThreatPosition 自动锁定威胁源
	glm::vec3 resolvedThreatPos(0.0f);
	uint32_t resolvedThreatRaw = 0;
	bool resolved = EQSCoverSystem::ResolveSceneThreatPosition(
		scene, static_cast<uint32_t>((entt::entity)ai), eqs.ThreatEntityHint, aiTr.Location, resolvedThreatPos, &resolvedThreatRaw);
	assert(resolved);
	assert(glm::distance(resolvedThreatPos, threatTr.Location) < 0.001f);
	assert(resolvedThreatRaw == static_cast<uint32_t>((entt::entity)threat));

	// 更新帧: eqs-tactical-system 自动执行评估并同步至 BehaviorTreeComponent
	scene.OnUpdateRuntime(Timestep(0.016f));

	// 验证更新帧后 BlackboardTargetPos 精准落在掩体后方而非被威胁源坐标覆盖！
	assert(eqs.BestCoverFound == 1);
	assert(bt.BlackboardTag == 88);
	assert(bt.BlackboardTargetPos.x > 10.0f);
	assert(glm::distance(bt.BlackboardTargetPos, threatTr.Location) > 5.0f);

	std::cout << "[PASS] TestEQSAndBehaviorTreeSynergy (Threat: " << threatTr.Location.x
		<< ", TargetCover: " << bt.BlackboardTargetPos.x << ", " << bt.BlackboardTargetPos.y << ", " << bt.BlackboardTargetPos.z
		<< ", Score: " << bt.BlackboardValue << ")\n";
}

static void TestEQSFrameworkExtensibility()
{
	using namespace World::EQS;

	// 1. 自定义生成器：在 X 轴上生成等距候选点 (CustomLineGenerator)
	class CustomLineGenerator : public IEQSGenerator
	{
	public:
		std::string GetName() const override { return "CustomLineGenerator"; }
		void GenerateItems(const EQSQueryContext& ctx, std::vector<EQSCandidateItem>& outItems) override
		{
			for (int i = 0; i < 10; ++i)
			{
				EQSCandidateItem item;
				item.Position = ctx.QuerierPos + glm::vec3(static_cast<float>(i) * 2.0f, 0.0f, 0.0f);
				item.TotalScore = 0.0f;
				item.FilteredOut = false;
				outItems.push_back(item);
			}
		}
	};

	// 2. 自定义测试项：偏好靠近 X = 8.0 的位置 (CustomTargetXTest)
	class CustomTargetXTest : public IEQSTest
	{
	public:
		float TargetX { 8.0f };
		std::string GetName() const override { return "CustomTargetXTest"; }
		void RunTest(const EQSQueryContext& ctx, std::vector<EQSCandidateItem>& inOutItems) override
		{
			for (auto& item : inOutItems)
			{
				if (item.FilteredOut)
				{
					item.TestScores.push_back(0.0f);
					continue;
				}
				float diff = std::abs(item.Position.x - TargetX);
				float score = std::max(0.0f, 10.0f - diff);
				item.TotalScore += score * Weight;
				item.TestScores.push_back(score);
			}
		}
	};

	// 3. 注册到全局工厂单例中，证明工厂扩展性
	EQSGeneratorRegistry::Get().RegisterGenerator<CustomLineGenerator>("CustomLine");
	EQSTestRegistry::Get().RegisterTest<CustomTargetXTest>("CustomTargetX");

	assert(EQSGeneratorRegistry::Get().Find("CustomLine") != nullptr);
	assert(EQSTestRegistry::Get().Find("CustomTargetX") != nullptr);

	// 4. 使用 Builder 装配通用 EQSQuery
	auto customGen = EQSGeneratorRegistry::Get().Find("CustomLine");
	auto customTest = std::dynamic_pointer_cast<CustomTargetXTest>(EQSTestRegistry::Get().Find("CustomTargetX"));
	assert(customTest != nullptr);
	customTest->TargetX = 8.0f;
	customTest->Weight = 2.0f;

	auto query = EQSQuery::Builder()
		.Named("ExtensibilityTestQuery")
		.UsingGenerator(customGen)
		.AddTest(customTest)
		.Build();

	assert(query != nullptr);

	EQSQueryContext ctx;
	ctx.QuerierPos = glm::vec3(0.0f, 0.0f, 0.0f);

	EQSQueryResult result;
	bool success = query->Execute(ctx, result);

	assert(success);
	assert(result.Items.size() == 10);
	assert(std::abs(result.BestPosition.x - 8.0f) < 0.001f);
	assert(result.BestScore == 20.0f);

	std::cout << "[PASS] TestEQSFrameworkExtensibility (Best Pos: "
		<< result.BestPosition.x << ", " << result.BestPosition.y << ", " << result.BestPosition.z
		<< ", Score: " << result.BestScore << ")\n";
}

int main()
{
	std::cout << "Starting World.PCGAndLivingAI tests...\n";
	TestPCGBiomeClassification();
	TestSplineTerrainDeformation();
	TestEQSCoverSystem();
	TestEQSMultiModes();
	TestFoliageInteractionPhysics();
	TestPCGAndLivingAIComponentLayout();
	TestPCGFoliageBaking();
	TestAdaptiveFiveBiomeCoverage();
	TestSlopeSplineGrading();
	TestOBBCoverWallProximity();
	TestStatefulFoliageInteractionField();
	TestPCGPreservesHandPaintedFoliage();
	TestEQSAndBehaviorTreeSynergy();
	TestEQSFrameworkExtensibility();
	std::cout << "All World.PCGAndLivingAI tests passed successfully!\n";
	return 0;
}
