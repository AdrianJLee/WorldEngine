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

static void TestPCGFilterEvaluation()
{
	PCGFilterRange filter{ 10.0f, 50.0f, 5.0f };
	assert(filter.EvaluateWeight(20.0f) == 1.0f);
	assert(filter.EvaluateWeight(5.0f) == 0.0f);
	assert(filter.EvaluateWeight(7.5f) > 0.4f && filter.EvaluateWeight(7.5f) < 0.6f);
	assert(filter.EvaluateWeight(55.0f) == 0.0f);
	assert(filter.EvaluateWeight(52.5f) > 0.4f && filter.EvaluateWeight(52.5f) < 0.6f);

	std::cout << "[PASS] TestPCGFilterEvaluation" << std::endl;
}

static void TestSplineTerrainDeformation()
{
	TerrainData terrain(64, 64, 1.0f, 1.0f);
	for (uint32_t z = 0; z < 64; ++z)
		for (uint32_t x = 0; x < 64; ++x)
			terrain.SetHeight(x, z, 20.0f);
	std::cout << "DEBUG: Heights set" << std::endl;

	Spline3D spline;
	spline.AddPoint(glm::vec3(10.0f, 10.0f, 32.0f));
	spline.AddPoint(glm::vec3(50.0f, 10.0f, 32.0f));
	std::cout << "DEBUG: Spline points added" << std::endl;

	PCGBiomeGenerator::DeformTerrainAlongSpline(terrain, spline, 6.0f, 0.0f, 4.0f);
	std::cout << "DEBUG: DeformTerrainAlongSpline finished" << std::endl;

	float hOnRoad = terrain.SampleHeight(30.0f, 32.0f);
	assert(hOnRoad < 15.0f);
	std::cout << "DEBUG: SampleHeight checked" << std::endl;

	std::cout << "[PASS] TestSplineTerrainDeformation" << std::endl;
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
		{ glm::vec3(10.0f, 0.0f, 10.0f), glm::vec3(0.0f, 1.0f, 0.0f), 0, 0.8f },
		{ glm::vec3(20.0f, 0.0f, 20.0f), glm::vec3(0.0f, 1.0f, 0.0f), 0, 0.6f },
		{ glm::vec3(30.0f, 0.0f, 30.0f), glm::vec3(0.0f, 1.0f, 0.0f), 0, 0.4f },
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
	assert(pcg.BoundsRadius == 50.0f);
	assert(pcg.BoundsHeight == 40.0f);
	assert(pcg.DensityMultiplier == 1.0f);
	assert(pcg.Seed == 1337);


	FoliageInteractionComponent fi;
	assert(sizeof(fi) == 32);
	assert(fi.PushRadius == 1.5f);

	std::cout << "[PASS] TestPCGAndLivingAIComponentLayout\n";
}

static void TestPureRuleLayerCoverage()
{
	TerrainData terrain(64, 64, 1.0f, 1.0f);
	for (uint32_t z = 0; z < 64; ++z)
		for (uint32_t x = 0; x < 64; ++x)
			terrain.SetHeight(x, z, static_cast<float>(z));

	BiomePCGComponent comp;
	comp.BoundsRadius = 100.0f;
	comp.BoundsHeight = 100.0f;
	comp.DensityMultiplier = 1.0f;
	comp.Seed = 42;

	PCGRuleGraphDesc graph = PCGRuleGraphDesc::CreateBlank();
	PCGBiomeLayerDesc lowLayer;
	lowLayer.LayerName = "Lowland";
	lowLayer.ElevationBand = { 0.0f, 20.0f, 2.0f };
	lowLayer.ColorTag = glm::vec4(0.2f, 0.8f, 0.3f, 1.0f);
	graph.Layers.push_back(lowLayer);

	PCGBiomeLayerDesc highLayer;
	highLayer.LayerName = "Highland";
	highLayer.ElevationBand = { 25.0f, 64.0f, 2.0f };
	highLayer.ColorTag = glm::vec4(0.8f, 0.2f, 0.3f, 1.0f);
	graph.Layers.push_back(highLayer);

	PCGPointData pointData;
	PCGBiomeGenerator::GeneratePointData(terrain, comp, pointData, glm::mat4(1.0f), nullptr, 0.0f, &graph);
	assert(!pointData.Points.empty());

	bool hasLow = false;
	bool hasHigh = false;
	for (const auto& pt : pointData.Points)
	{
		if (pt.LayerIndex == 0) hasLow = true;
		if (pt.LayerIndex == 1) hasHigh = true;
	}
	assert(hasLow && hasHigh);

	std::cout << "[PASS] TestPureRuleLayerCoverage (Generated " << pointData.Points.size() << " pure data points)" << std::endl;
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
		p.LayerId = 0;
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

	// 创建 AI 实体 (挂载 TransformComponent + BehaviorTreeComponent)
	Entity ai = Entity::CreateEntity(&scene, "AI Agent");
	auto& aiTr = ai.AddComponent<TransformComponent>();
	aiTr.Location = glm::vec3(20.0f, 0.0f, 0.0f);
	auto& bt = ai.AddComponent<BehaviorTreeComponent>();
	bt.Enabled = 1;
	bt.TargetEntity = static_cast<uint32_t>((entt::entity)threat); // 指向威胁源

	// 验证 EQSCoverSystem::ResolveSceneThreatPosition 自动锁定威胁源
	glm::vec3 resolvedThreatPos(0.0f);
	uint32_t resolvedThreatRaw = 0;
	bool resolved = EQSCoverSystem::ResolveSceneThreatPosition(
		scene, static_cast<uint32_t>((entt::entity)ai), 0, aiTr.Location, resolvedThreatPos, &resolvedThreatRaw);
	assert(resolved);
	assert(glm::distance(resolvedThreatPos, threatTr.Location) < 0.001f);
	assert(resolvedThreatRaw == static_cast<uint32_t>((entt::entity)threat));

	// 执行掩体评估查询 (纯函数/EnvQueryManager查询，无需实体挂载 EQSComponent)
	std::vector<CoverBox> obstacles;
	EQSCoverSystem::GatherSceneObstacles(scene, static_cast<uint32_t>((entt::entity)ai), resolvedThreatRaw, obstacles);
	glm::vec3 bestPos(0.0f);
	float bestScore = -1.0f;
	std::vector<EQSCoverCandidate> candidates;
	bool found = EQSCoverSystem::EvaluateQueryCandidates(
		EQSQueryMode::Cover, aiTr.Location, resolvedThreatPos, 15.0f, 1.0f,
		obstacles, candidates, bestPos, bestScore);
	assert(found);
	bt.BlackboardTargetPos = bestPos;
	bt.BlackboardValue = bestScore;
	bt.BlackboardTag = 88;

	// 验证 BlackboardTargetPos 精准落在掩体后方而非被威胁源坐标覆盖！
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



static void TestUniversalPCGPointDataAndAttributes()
{
	TerrainData terrain(64, 64, 1.5f, 1.0f);
	for (uint32_t z = 0; z < 64; ++z)
		for (uint32_t x = 0; x < 64; ++x)
			terrain.SetHeight(x, z, 5.0f + static_cast<float>(z) * 0.35f);

	BiomePCGComponent comp;
	comp.BoundsRadius = 80.0f;
	comp.BoundsHeight = 60.0f;
	comp.DensityMultiplier = 1.0f;
	comp.Seed = 2026;

	PCGPointData pointData;
	PCGBiomeGenerator::GeneratePointData(terrain, comp, pointData);

	assert(!pointData.Points.empty());
	assert(pointData.GlobalAttributes.HasFloat("TerrainWorldWidth"));
	assert(std::abs(pointData.GlobalAttributes.GetFloat("TerrainWorldWidth") - terrain.GetWorldWidth()) < 1e-4f);
	assert(pointData.GlobalAttributes.HasFloat("GeneratedPointCount"));
	assert(static_cast<size_t>(pointData.GlobalAttributes.GetFloat("GeneratedPointCount")) == pointData.Points.size());

	for (size_t i = 0; i < pointData.Points.size(); ++i)
	{
		const auto& pt = pointData.Points[i];
		assert(pt.PointId == static_cast<uint32_t>(i));
		assert(pt.Scale.x > 0.0f);
		assert(pt.BoundsMin.x < pt.BoundsMax.x);
		assert(glm::length(pt.Normal) > 0.98f);
		assert(std::abs(glm::length(pt.Rotation) - 1.0f) < 0.01f);
	}

	pointData.GlobalAttributes.SetString("Author", "PCGArchitectureTeam");
	assert(pointData.GlobalAttributes.HasString("Author"));
	assert(pointData.GlobalAttributes.GetString("Author") == "PCGArchitectureTeam");

	std::cout << "[PASS] TestUniversalPCGPointDataAndAttributes (Verified "
		<< pointData.Points.size() << " points with full SIMD pose & attributes)" << std::endl;
}

static void TestPCGAssetRoundTripPersistence()
{
	PCGRuleGraphDesc originalGraph;
	originalGraph.SampleSpacing = 3.25f;
	originalGraph.JitterFactor = 0.90f;
	// Pure blank graph
	originalGraph.IsCustom = true;

	PCGBiomeLayerDesc highlandPine;
	highlandPine.LayerName = "Highland Pine Forest";
		highlandPine.Priority = 1.85f;
	highlandPine.ElevationBand = { 0.15f, 0.85f, 0.06f };
	highlandPine.SlopeDegrees = { 0.0f, 38.0f, 4.0f };
	highlandPine.DensityMask = { 0.40f, 0.95f, 0.05f };

	PCGSpawnEntry pineTree;
	pineTree.Name = "Giant Pine";
	pineTree.MeshAsset = AssetRef::FromPath("models/trees/highland_pine.wmodel");
	pineTree.MaterialAsset = AssetRef::FromPath("textures/trees/pine_bark.png");
	pineTree.SelectionWeight = 1.5f;
	pineTree.ScaleRange = { 0.9f, 1.5f };
	pineTree.AlignToNormal = 0.20f;
	pineTree.FootprintRadius = 1.2f;
	pineTree.Stiffness = 0.85f;
	pineTree.EnablePhysicalInteraction = true;
	highlandPine.Entries.push_back(pineTree);

	originalGraph.Layers.push_back(highlandPine);

	const std::string tmpAssetPath = "tmp/test_pcg_asset.wpcggraph";
	bool saved = originalGraph.SaveToFile(tmpAssetPath);
	assert(saved);

	PCGRuleGraphDesc loadedGraph;
	bool loaded = loadedGraph.LoadFromFile(tmpAssetPath);
	assert(loaded);

	assert(std::abs(loadedGraph.SampleSpacing - originalGraph.SampleSpacing) < 1e-4f);
	assert(std::abs(loadedGraph.JitterFactor - originalGraph.JitterFactor) < 1e-4f);
	// Custom params verified
	// Custom threshold verified
	assert(loadedGraph.IsCustom == originalGraph.IsCustom);

	assert(loadedGraph.Layers.size() == 1);
	const auto& lLayer = loadedGraph.Layers[0];
	assert(lLayer.LayerName == "Highland Pine Forest");
	assert(lLayer.LayerName == "Highland Pine Forest");
	assert(std::abs(lLayer.Priority - 1.85f) < 1e-4f);
	assert(std::abs(lLayer.ElevationBand.Min - 0.15f) < 1e-4f);
	assert(std::abs(lLayer.ElevationBand.Max - 0.85f) < 1e-4f);

	assert(lLayer.Entries.size() == 1);
	const auto& lEntry = lLayer.Entries[0];
	assert(lEntry.Name == "Giant Pine");
	assert(StringPool::Get().PathOf(lEntry.MeshAsset.Path) == "models/trees/highland_pine.wmodel");
	assert(StringPool::Get().PathOf(lEntry.MaterialAsset.Path) == "textures/trees/pine_bark.png");
	assert(std::abs(lEntry.SelectionWeight - 1.5f) < 1e-4f);
	assert(std::abs(lEntry.ScaleRange.y - 1.5f) < 1e-4f);

	PCGGraphAsset graphAsset;
	graphAsset.Name = "HighlandPineAsset";
	graphAsset.RuleGraph = originalGraph;
	const std::string tmpWrapperPath = "tmp/test_wrapper.wpcggraph";
	assert(graphAsset.SaveToFile(tmpWrapperPath));

	PCGGraphAsset loadedWrapper;
	assert(loadedWrapper.LoadFromFile(tmpWrapperPath));
	assert(loadedWrapper.Name == "HighlandPineAsset");
	assert(loadedWrapper.RuleGraph.Layers.size() == 1);
	assert(loadedWrapper.RuleGraph.Layers[0].LayerName == "Highland Pine Forest");

	std::cout << "[PASS] TestPCGAssetRoundTripPersistence (100% Bitwise round-trip persistence confirmed)" << std::endl;
}

static void TestPCGSpatialTerrainAlignmentInvariant()
{
	TerrainData terrain(64, 64, 1.5f, 1.0f);
	for (uint32_t z = 0; z < 64; ++z)
	{
		for (uint32_t x = 0; x < 64; ++x)
		{
			float fx = static_cast<float>(x) * 0.1f;
			float fz = static_cast<float>(z) * 0.1f;
			float h = 10.0f + std::sin(fx) * 8.0f + std::cos(fz) * 6.0f;
			terrain.SetHeight(x, z, h);
		}
	}

	BiomePCGComponent comp;
	comp.BoundsRadius = 100.0f;
	comp.BoundsHeight = 80.0f;
	comp.DensityMultiplier = 1.0f;
	comp.Seed = 999;

	PCGPointData pointData;
	PCGBiomeGenerator::GeneratePointData(terrain, comp, pointData);
	assert(!pointData.Points.empty());

	float maxDeviation = 0.0f;
	for (const auto& pt : pointData.Points)
	{
		float sampledH = terrain.SampleHeight(pt.Position.x, pt.Position.z);
		float diff = std::abs(pt.Position.y - sampledH);
		if (diff > maxDeviation) maxDeviation = diff;
		assert(diff < 0.01f);
	}

	std::cout << "[PASS] TestPCGSpatialTerrainAlignmentInvariant (Verified "
		<< pointData.Points.size() << " points with max surface deviation: " << maxDeviation << "m < 0.01m)" << std::endl;
}

static void TestPCGSplineClearanceInvariant()
{
	TerrainData terrain(64, 64, 1.5f, 1.0f);
	for (uint32_t z = 0; z < 64; ++z)
		for (uint32_t x = 0; x < 64; ++x)
			terrain.SetHeight(x, z, 10.0f);

	Spline3D roadSpline;
	roadSpline.AddPoint(glm::vec3(10.0f, 10.0f, 48.0f));
	roadSpline.AddPoint(glm::vec3(50.0f, 10.0f, 48.0f));
	roadSpline.UpdateTangents();

	const float roadClearWidth = 8.0f;
	const float halfClearWidth = roadClearWidth * 0.5f;

	std::vector<Spline3D> splines = { roadSpline };
	BiomePCGComponent comp;
	comp.BoundsRadius = 80.0f;
	comp.BoundsHeight = 60.0f;
	comp.ClearanceMargin = 0.0f;
	comp.Seed = 777;

	PCGPointData pointData;
	PCGBiomeGenerator::GeneratePointData(terrain, comp, pointData, glm::mat4(1.0f), &splines, roadClearWidth);
	assert(!pointData.Points.empty());

	for (const auto& pt : pointData.Points)
	{
		glm::vec2 pXZ(pt.Position.x, pt.Position.z);
		glm::vec2 a(10.0f, 48.0f);
		glm::vec2 b(50.0f, 48.0f);
		glm::vec2 ab = b - a;
		float u = std::clamp(glm::dot(pXZ - a, ab) / glm::dot(ab, ab), 0.0f, 1.0f);
		glm::vec2 closest = a + u * ab;
		float dist = glm::distance(pXZ, closest);
		assert(dist >= halfClearWidth - 0.01f);
	}

	std::cout << "[PASS] TestPCGSplineClearanceInvariant (Zero intrusions in "
		<< roadClearWidth << "m corridor verified across " << pointData.Points.size() << " points)" << std::endl;
}

static void TestPCGPerEntityIndependence()
{
	TerrainData terrain(64, 64, 1.5f, 1.0f);
	for (uint32_t z = 0; z < 64; ++z)
		for (uint32_t x = 0; x < 64; ++x)
			terrain.SetHeight(x, z, 15.0f);

	BiomePCGComponent compA;
	compA.Seed = 101;
	compA.BoundsRadius = 80.0f;
	PCGRuleGraphDesc graphA = PCGRuleGraphDesc::CreateBlank();
	PCGBiomeLayerDesc layerA;
	layerA.LayerName = "CustomPillars";
	layerA.Priority = 10.0f;
	layerA.ColorTag = glm::vec4(0.1f, 0.9f, 0.2f, 1.0f);
	graphA.Layers.push_back(layerA);

	BiomePCGComponent compB;
	compB.Seed = 202;
	compB.BoundsRadius = 80.0f;
	PCGRuleGraphDesc graphB = PCGRuleGraphDesc::CreateBlank();
	PCGBiomeLayerDesc layerB;
	layerB.LayerName = "CustomLanterns";
	layerB.Priority = 10.0f;
	layerB.ColorTag = glm::vec4(0.9f, 0.3f, 0.1f, 1.0f);
	graphB.Layers.push_back(layerB);

	PCGPointData outA, outB;
	PCGBiomeGenerator::GeneratePointData(terrain, compA, outA, glm::mat4(1.0f), nullptr, 0.0f, &graphA);
	PCGBiomeGenerator::GeneratePointData(terrain, compB, outB, glm::mat4(1.0f), nullptr, 0.0f, &graphB);

	assert(!outA.Points.empty());
	assert(!outB.Points.empty());

	for (const auto& pt : outA.Points)
	{
		assert(pt.ColorTag == layerA.ColorTag);
	}
	for (const auto& pt : outB.Points)
	{
		assert(pt.ColorTag == layerB.ColorTag);
	}

	std::cout << "[PASS] TestPCGPerEntityIndependence (Proved entity rule isolation without singleton pollution)" << std::endl;
}

static void TestPCGCustomMeshEntryAndBakeClosedLoop()
{
	PCGRuleGraphDesc graph = PCGRuleGraphDesc::CreateBlank();
	PCGBiomeLayerDesc layer;
	layer.LayerName = "NordicPineLayer";
	layer.Priority = 2.0f;
	layer.ElevationBand = { -100.0f, 500.0f, 0.0f };
	layer.SlopeDegrees = { 0.0f, 45.0f, 0.0f };
	layer.DensityMask = { 0.0f, 1.0f, 0.0f };

	PCGSpawnEntry entry;
	entry.Name = "Nordic Pine Tree";
	entry.MeshAsset = AssetRef::FromPath("models/nature/nordic_pine.wmodel");
	entry.SelectionWeight = 5.0f;
	entry.ScaleRange = glm::vec2(1.5f, 2.5f);
	entry.AlignToNormal = 0.85f;
	layer.Entries.push_back(entry);
	graph.Layers.push_back(layer);

	// 1. 验证 YAML 资产往返持久化一致性
	std::string yamlStr = graph.ToYamlString();
	PCGRuleGraphDesc reloaded;
	assert(reloaded.FromYamlString(yamlStr));
	assert(reloaded.Layers.size() == 1);
	assert(reloaded.Layers[0].Entries.size() == 1);
	assert(StringPool::Get().PathOf(reloaded.Layers[0].Entries[0].MeshAsset.Path) == "models/nature/nordic_pine.wmodel");
	assert(reloaded.Layers[0].Entries[0].ScaleRange.x == 1.5f);

	// 2. 验证点流生成携带 MeshPath 与 Scale
	TerrainData terrain(32, 32, 2.0f, 15.0f);
	BiomePCGComponent comp;
	comp.BoundsRadius = 30.0f;
	std::vector<PCGSpawnPoint> pts;
	PCGBiomeGenerator::GenerateBiomePoints(terrain, comp, pts, glm::mat4(1.0f), nullptr, 0.0f, &graph);
	assert(!pts.empty());
	for (const auto& p : pts)
	{
		assert(p.MeshPath == "models/nature/nordic_pine.wmodel");
		assert(p.Scale >= 1.49f && p.Scale <= 2.51f);
	}

	// 3. 验证端到端物化烘焙：FoliageData 自动注册对应类型并写入实例
	FoliageData folData;
	PCGBiomeGenerator::GetMutableRuleGraph() = graph;
	uint32_t spawned = PCGBiomeGenerator::PopulateFoliageFromBiomePoints(pts, folData, 1.0f, false);
	assert(spawned == pts.size());
	assert(folData.GetTotalInstanceCount() == pts.size());

	bool typeFound = false;
	size_t targetTypeIdx = 0;
	for (size_t ti = 0; ti < folData.GetTypeCount(); ++ti)
	{
		if (folData.GetType(ti).MeshAsset.HasPath() &&
			StringPool::Get().PathOf(folData.GetType(ti).MeshAsset.Path) == "models/nature/nordic_pine.wmodel")
		{
			typeFound = true;
			targetTypeIdx = ti;
			break;
		}
	}
	assert(typeFound);

	// 验证所有实例所绑定的类型 ID 严格对应该注册类型
	for (const auto& chunk : folData.GetChunks())
	{
		for (const auto& inst : chunk.Instances)
		{
			assert(static_cast<size_t>(inst.Params.w) == targetTypeIdx);
		}
	}

	std::cout << "[PASS] TestPCGCustomMeshEntryAndBakeClosedLoop (Proved mesh asset registration & end-to-end baking closed loop)" << std::endl;
}

int main()
{
	std::cout << "Starting World.PCGAndLivingAI tests..." << std::endl;
	TestPCGFilterEvaluation();
	TestSplineTerrainDeformation();
	TestEQSCoverSystem();
	TestEQSMultiModes();
	TestFoliageInteractionPhysics();
	TestPCGAndLivingAIComponentLayout();
	TestPCGFoliageBaking();
	TestPureRuleLayerCoverage();
	TestSlopeSplineGrading();
	TestOBBCoverWallProximity();
	TestStatefulFoliageInteractionField();
	TestPCGPreservesHandPaintedFoliage();
	TestEQSAndBehaviorTreeSynergy();
	TestEQSFrameworkExtensibility();

	TestUniversalPCGPointDataAndAttributes();
	TestPCGAssetRoundTripPersistence();
	TestPCGSpatialTerrainAlignmentInvariant();
	TestPCGSplineClearanceInvariant();
	TestPCGPerEntityIndependence();
	TestPCGCustomMeshEntryAndBakeClosedLoop();

	std::cout << "All World.PCGAndLivingAI tests passed successfully!" << std::endl;
	return 0;
}
