#include "wldpch.h"
#include "World/Level/AI/EQSCoverSystem.h"
#include "World/Level/AI/EQS/EQSTypes.h"
#include "World/Level/AI/EQS/EQSContext.h"
#include "World/Level/AI/EQS/EQSGenerator.h"
#include "World/Level/AI/EQS/EQSTest.h"
#include "World/Level/AI/EQS/EQSQuery.h"
#include "World/Terrain/Terrain.h"
#include "World/Scene/Scene.h"
#include "World/Scene/Components.h"
#include "World/Core/StringPool.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <cctype>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

namespace World
{
	CoverBox CoverBox::FromOBB(const glm::vec3& center, const glm::vec3& halfExtents, const glm::vec3& eulerDegrees)
	{
		CoverBox b;
		b.Center = center;
		b.HalfExtents = glm::max(glm::vec3(0.001f), halfExtents);
		glm::quat q = glm::quat(glm::radians(eulerDegrees));
		b.Rotation = glm::mat3_cast(q);
		b.IsOriented = true;

		glm::vec3 extent(0.0f);
		for (int i = 0; i < 3; ++i)
		{
			extent.x += std::abs(b.Rotation[i].x) * b.HalfExtents[i];
			extent.y += std::abs(b.Rotation[i].y) * b.HalfExtents[i];
			extent.z += std::abs(b.Rotation[i].z) * b.HalfExtents[i];
		}
		b.Min = center - extent;
		b.Max = center + extent;
		return b;
	}

	CoverBox CoverBox::FromOBB(const glm::vec3& center, const glm::vec3& halfExtents, const glm::quat& orientation)
	{
		CoverBox b;
		b.Center = center;
		b.HalfExtents = glm::max(glm::vec3(0.001f), halfExtents);
		b.Rotation = glm::mat3_cast(orientation);
		b.IsOriented = true;

		glm::vec3 extent(0.0f);
		for (int i = 0; i < 3; ++i)
		{
			extent.x += std::abs(b.Rotation[i].x) * b.HalfExtents[i];
			extent.y += std::abs(b.Rotation[i].y) * b.HalfExtents[i];
			extent.z += std::abs(b.Rotation[i].z) * b.HalfExtents[i];
		}
		b.Min = center - extent;
		b.Max = center + extent;
		return b;
	}

	bool CoverBox::ContainsPoint(const glm::vec3& worldPt, float expandRadius) const
	{
		if (!IsOriented)
		{
			return (worldPt.x >= Min.x - expandRadius && worldPt.x <= Max.x + expandRadius &&
			        worldPt.y >= Min.y && worldPt.y <= Max.y &&
			        worldPt.z >= Min.z - expandRadius && worldPt.z <= Max.z + expandRadius);
		}

		const glm::vec3 delta = worldPt - Center;
		const glm::vec3 localPt = glm::transpose(Rotation) * delta;
		return (std::abs(localPt.x) <= HalfExtents.x + expandRadius &&
		        std::abs(localPt.y) <= HalfExtents.y &&
		        std::abs(localPt.z) <= HalfExtents.z + expandRadius);
	}

	bool CoverBox::IntersectRay(const glm::vec3& rayOrigin, const glm::vec3& rayDir, float maxDist, float& outHitDist) const
	{
		glm::vec3 localOrigin;
		glm::vec3 localDir;
		glm::vec3 boxMin;
		glm::vec3 boxMax;

		if (IsOriented)
		{
			const glm::mat3 invRot = glm::transpose(Rotation);
			localOrigin = invRot * (rayOrigin - Center);
			localDir = invRot * rayDir;
			boxMin = -HalfExtents;
			boxMax = HalfExtents;
		}
		else
		{
			localOrigin = rayOrigin;
			localDir = rayDir;
			boxMin = Min;
			boxMax = Max;
		}

		float tmin = 0.0f;
		float tmax = maxDist;

		for (int i = 0; i < 3; ++i)
		{
			if (std::abs(localDir[i]) < 1e-6f)
			{
				if (localOrigin[i] < boxMin[i] || localOrigin[i] > boxMax[i])
					return false;
			}
			else
			{
				const float invD = 1.0f / localDir[i];
				float t0 = (boxMin[i] - localOrigin[i]) * invD;
				float t1 = (boxMax[i] - localOrigin[i]) * invD;
				if (t0 > t1)
					std::swap(t0, t1);
				tmin = std::max(tmin, t0);
				tmax = std::min(tmax, t1);
				if (tmax < tmin)
					return false;
			}
		}

		outHitDist = tmin;
		return true;
	}

	float CoverBox::DistanceToSurfaceXZ(const glm::vec3& worldPt) const
	{
		if (!IsOriented)
		{
			const float dx = std::max({ Min.x - worldPt.x, 0.0f, worldPt.x - Max.x });
			const float dz = std::max({ Min.z - worldPt.z, 0.0f, worldPt.z - Max.z });
			return std::sqrt(dx * dx + dz * dz);
		}

		const glm::vec3 delta = worldPt - Center;
		const glm::vec3 localPt = glm::transpose(Rotation) * delta;
		const float dx = std::max(0.0f, std::abs(localPt.x) - HalfExtents.x);
		const float dz = std::max(0.0f, std::abs(localPt.z) - HalfExtents.z);
		return std::sqrt(dx * dx + dz * dz);
	}

	bool EQSCoverSystem::FindBestCover(
		const glm::vec3& querierPos,
		const glm::vec3& threatPos,
		float queryRadius,
		float gridSpacing,
		const std::vector<CoverBox>& obstacles,
		glm::vec3& outBestCover,
		float& outBestScore)
	{
		return ExecuteQuery(EQSQueryMode::Cover, querierPos, threatPos, queryRadius, gridSpacing, obstacles, outBestCover, outBestScore);
	}

	bool EQSCoverSystem::ExecuteQuery(
		EQSQueryMode mode,
		const glm::vec3& querierPos,
		const glm::vec3& threatPos,
		float queryRadius,
		float gridSpacing,
		const std::vector<CoverBox>& obstacles,
		glm::vec3& outBestPos,
		float& outBestScore)
	{
		std::vector<EQSCoverCandidate> dummy;
		return EvaluateQueryCandidates(mode, querierPos, threatPos, queryRadius, gridSpacing, obstacles, dummy, outBestPos, outBestScore, nullptr, glm::mat4(1.0f), 0);
	}

	std::shared_ptr<EQS::EQSQuery> EQSCoverSystem::BuildPresetCoverQuery(uint8_t generatorMode, const EQSScoringWeights& weights)
	{
		using namespace World::EQS;
		std::shared_ptr<IEQSGenerator> gen;
		if (generatorMode == 1)
			gen = std::make_shared<EQSGenerator_Circle>();
		else if (generatorMode == 2)
			gen = std::make_shared<EQSGenerator_AroundObstacles>();
		else
			gen = std::make_shared<EQSGenerator_ProjectedGrid>();

		auto overlapTest = std::make_shared<EQSTest_Overlap>();

		auto traceTest = std::make_shared<EQSTest_Trace>();
		traceTest->InvertOcclusion = false;
		traceTest->Weight = weights.OcclusionWeight;

		auto wallTest = std::make_shared<EQSTest_WallProximity>();
		wallTest->RequireOcclusion = true;
		wallTest->Weight = weights.WallProximityWeight;

		auto distTest = std::make_shared<EQSTest_Distance>();
		distTest->Curve = ScoringCurve::Inverse;
		distTest->Weight = weights.DistanceWeight;

		auto heightTest = std::make_shared<EQSTest_Height>();
		heightTest->Weight = weights.ElevationWeight;

		return EQSQuery::Builder()
			.Named("Preset_CoverQuery")
			.UsingGenerator(gen)
			.AddTest(overlapTest)
			.AddTest(traceTest)
			.AddTest(wallTest)
			.AddTest(distTest)
			.AddTest(heightTest)
			.Build();
	}

	std::shared_ptr<EQS::EQSQuery> EQSCoverSystem::BuildPresetFlankQuery(uint8_t generatorMode, const EQSScoringWeights& weights)
	{
		using namespace World::EQS;
		std::shared_ptr<IEQSGenerator> gen;
		if (generatorMode == 1)
			gen = std::make_shared<EQSGenerator_Circle>();
		else if (generatorMode == 2)
			gen = std::make_shared<EQSGenerator_AroundObstacles>();
		else
			gen = std::make_shared<EQSGenerator_ProjectedGrid>();

		auto overlapTest = std::make_shared<EQSTest_Overlap>();

		auto traceTest = std::make_shared<EQSTest_Trace>();
		traceTest->InvertOcclusion = true; // 直瞄开火线
		traceTest->Weight = std::abs(weights.OcclusionWeight);

		auto dotTest = std::make_shared<EQSTest_Dot>();
		dotTest->PeakAtPerpendicular = true; // 90° 侧翼夹角
		dotTest->Weight = weights.AngleWeight;

		auto distTest = std::make_shared<EQSTest_Distance>();
		distTest->Curve = ScoringCurve::Inverse;
		distTest->Weight = weights.DistanceWeight;

		auto heightTest = std::make_shared<EQSTest_Height>();
		heightTest->Weight = weights.ElevationWeight;

		return EQSQuery::Builder()
			.Named("Preset_FlankQuery")
			.UsingGenerator(gen)
			.AddTest(overlapTest)
			.AddTest(traceTest)
			.AddTest(dotTest)
			.AddTest(distTest)
			.AddTest(heightTest)
			.Build();
	}

	std::shared_ptr<EQS::EQSQuery> EQSCoverSystem::BuildPresetRetreatQuery(uint8_t generatorMode, const EQSScoringWeights& weights)
	{
		using namespace World::EQS;
		std::shared_ptr<IEQSGenerator> gen;
		if (generatorMode == 1)
			gen = std::make_shared<EQSGenerator_Circle>();
		else if (generatorMode == 2)
			gen = std::make_shared<EQSGenerator_AroundObstacles>();
		else
			gen = std::make_shared<EQSGenerator_ProjectedGrid>();

		auto overlapTest = std::make_shared<EQSTest_Overlap>();

		auto traceTest = std::make_shared<EQSTest_Trace>();
		traceTest->InvertOcclusion = false;
		traceTest->Weight = weights.OcclusionWeight;

		auto retreatDistTest = std::make_shared<EQSTest_Distance>();
		retreatDistTest->Context = std::make_shared<EQSContext_CustomLocation>();
		retreatDistTest->Curve = ScoringCurve::Linear;
		retreatDistTest->Weight = weights.AngleWeight;

		auto querierDistTest = std::make_shared<EQSTest_Distance>();
		querierDistTest->Curve = ScoringCurve::Inverse;
		querierDistTest->Weight = weights.DistanceWeight;

		auto heightTest = std::make_shared<EQSTest_Height>();
		heightTest->Weight = weights.ElevationWeight;

		return EQSQuery::Builder()
			.Named("Preset_RetreatQuery")
			.UsingGenerator(gen)
			.AddTest(overlapTest)
			.AddTest(traceTest)
			.AddTest(retreatDistTest)
			.AddTest(querierDistTest)
			.AddTest(heightTest)
			.Build();
	}

	bool EQSCoverSystem::EvaluateQueryCandidates(
		EQSQueryMode mode,
		const glm::vec3& querierPos,
		const glm::vec3& threatPos,
		float queryRadius,
		float gridSpacing,
		const std::vector<CoverBox>& obstacles,
		std::vector<EQSCoverCandidate>& outCandidates,
		glm::vec3& outBestPos,
		float& outBestScore,
		const TerrainData* terrain,
		const glm::mat4& terrainWorld,
		uint8_t generatorMode)
	{
		outBestScore = -1.0f;
		outCandidates.clear();
		if (queryRadius <= 0.0f || gridSpacing <= 0.05f)
			return false;

		const EQSScoringWeights& weights = (mode == EQSQueryMode::Cover)
			? GetMutableCustomWeights()
			: EQSScoringWeights::ForMode(mode);

		std::shared_ptr<EQS::EQSQuery> query;
		if (mode == EQSQueryMode::Cover)
			query = BuildPresetCoverQuery(generatorMode, weights);
		else if (mode == EQSQueryMode::Flank)
			query = BuildPresetFlankQuery(generatorMode, weights);
		else
			query = BuildPresetRetreatQuery(generatorMode, weights);

		EQS::EQSQueryContext ctx;
		ctx.QuerierPos = querierPos;
		ctx.ThreatPos = threatPos;
		ctx.QueryRadius = queryRadius;
		ctx.GridSpacing = gridSpacing;
		ctx.Obstacles = obstacles;
		ctx.Terrain = terrain;
		ctx.TerrainWorldTransform = terrainWorld;

		if (mode == EQSQueryMode::Retreat && query->Tests.size() > 2)
		{
			query->Tests[2]->Context = std::make_shared<EQS::EQSContext_CustomLocation>(threatPos);
		}

		EQS::EQSQueryResult result;
		bool ok = query->Execute(ctx, result);
		if (!ok || result.Items.empty())
			return false;

		for (const auto& item : result.Items)
		{
			if (item.FilteredOut)
				continue;

			EQSCoverCandidate cand;
			cand.Position = item.Position;
			cand.TotalScore = item.TotalScore;

			if (mode == EQSQueryMode::Cover)
			{
				cand.LineOfSightOcclusion = (item.TestScores.size() > 1 && weights.OcclusionWeight > 0.0f)
					? (item.TestScores[1] / weights.OcclusionWeight) : 0.0f;
				cand.WallProximityScore = (item.TestScores.size() > 2 && weights.WallProximityWeight > 0.0f)
					? (item.TestScores[2] / weights.WallProximityWeight) : 0.0f;
				cand.DistanceScore = (item.TestScores.size() > 3 && weights.DistanceWeight > 0.0f)
					? (item.TestScores[3] / weights.DistanceWeight) : 0.0f;
				cand.ElevationScore = (item.TestScores.size() > 4 && weights.ElevationWeight > 0.0f)
					? (item.TestScores[4] / weights.ElevationWeight) : 0.0f;
			}
			else if (mode == EQSQueryMode::Flank)
			{
				cand.LineOfSightOcclusion = (item.TestScores.size() > 1 && std::abs(weights.OcclusionWeight) > 0.0f)
					? (1.0f - (item.TestScores[1] / std::abs(weights.OcclusionWeight))) : 0.0f;
				cand.AngleScore = (item.TestScores.size() > 2 && weights.AngleWeight > 0.0f)
					? (item.TestScores[2] / weights.AngleWeight) : 0.0f;
				cand.DistanceScore = (item.TestScores.size() > 3 && weights.DistanceWeight > 0.0f)
					? (item.TestScores[3] / weights.DistanceWeight) : 0.0f;
				cand.ElevationScore = (item.TestScores.size() > 4 && weights.ElevationWeight > 0.0f)
					? (item.TestScores[4] / weights.ElevationWeight) : 0.0f;
			}
			else if (mode == EQSQueryMode::Retreat)
			{
				cand.LineOfSightOcclusion = (item.TestScores.size() > 1 && weights.OcclusionWeight > 0.0f)
					? (item.TestScores[1] / weights.OcclusionWeight) : 0.0f;
				cand.DistanceScore = (item.TestScores.size() > 3 && weights.DistanceWeight > 0.0f)
					? (item.TestScores[3] / weights.DistanceWeight) : 0.0f;
				cand.ElevationScore = (item.TestScores.size() > 4 && weights.ElevationWeight > 0.0f)
					? (item.TestScores[4] / weights.ElevationWeight) : 0.0f;
			}

			outCandidates.push_back(cand);
		}

		if (outCandidates.empty())
			return false;

		std::sort(outCandidates.begin(), outCandidates.end(),
			[](const EQSCoverCandidate& a, const EQSCoverCandidate& b) {
				return a.TotalScore > b.TotalScore;
			});

		outBestPos = outCandidates.front().Position;
		outBestScore = outCandidates.front().TotalScore;
		return true;
	}

	EQSScoringWeights EQSScoringWeights::ForMode(EQSQueryMode mode, float optimalDist)
	{
		EQSScoringWeights w;
		w.OptimalDistance = optimalDist;
		switch (mode)
		{
		case EQSQueryMode::Cover:
			w.OcclusionWeight = 1.8f;
			w.WallProximityWeight = 1.2f;
			w.DistanceWeight = 0.7f;
			w.AngleWeight = 0.0f;
			w.ElevationWeight = 0.3f;
			break;
		case EQSQueryMode::Flank:
			w.OcclusionWeight = -1.5f;
			w.WallProximityWeight = 0.0f;
			w.DistanceWeight = 0.5f;
			w.AngleWeight = 2.0f;
			w.ElevationWeight = 0.3f;
			break;
		case EQSQueryMode::Retreat:
			w.OcclusionWeight = 2.0f;
			w.WallProximityWeight = 0.0f;
			w.DistanceWeight = 0.7f;
			w.AngleWeight = 2.0f;
			w.ElevationWeight = 0.3f;
			break;
		}
		return w;
	}

	EQSScoringWeights& EQSCoverSystem::GetMutableCustomWeights()
	{
		static EQSScoringWeights s_CustomWeights = EQSScoringWeights::ForMode(EQSQueryMode::Cover);
		return s_CustomWeights;
	}

	bool EQSCoverSystem::ResolveSceneThreatPosition(
		Scene& scene,
		uint32_t querierEntityRaw,
		uint32_t threatEntityHint,
		const glm::vec3& querierPos,
		glm::vec3& outThreatPos,
		uint32_t* outResolvedThreatEntityRaw)
	{
		if (outResolvedThreatEntityRaw)
			*outResolvedThreatEntityRaw = 0;

		auto& reg = scene.GetRegistry();

		auto matchEntity = [&](entt::entity e, uint32_t hint) -> bool {
			if (hint == 0) return false;
			if (static_cast<uint32_t>(e) == hint) return true;
			if (reg.all_of<UUIDComponent>(e))
			{
				uint64_t fullId = static_cast<uint64_t>(reg.get<UUIDComponent>(e).ID);
				if (static_cast<uint32_t>(fullId & 0xFFFFFFFF) == hint)
					return true;
			}
			return false;
		};

		// 1. 若提供了显式威胁实体提示 (ThreatEntityHint)
		if (threatEntityHint != 0)
		{
			auto view = reg.view<TransformComponent>();
			for (auto entity : view)
			{
				if (static_cast<uint32_t>(entity) == querierEntityRaw)
					continue;
				if (matchEntity(entity, threatEntityHint))
				{
					outThreatPos = view.get<TransformComponent>(entity).Location;
					if (outResolvedThreatEntityRaw)
						*outResolvedThreatEntityRaw = static_cast<uint32_t>(entity);
					return true;
				}
			}
		}

		// 2. 若未匹配到，检查查询者自身的 BehaviorTreeComponent::TargetEntity
		entt::entity querierEnt = static_cast<entt::entity>(querierEntityRaw);
		if (reg.valid(querierEnt) && reg.all_of<BehaviorTreeComponent>(querierEnt))
		{
			uint32_t btTarget = reg.get<BehaviorTreeComponent>(querierEnt).TargetEntity;
			if (btTarget != 0 && btTarget != static_cast<uint32_t>(entt::null))
			{
				auto view = reg.view<TransformComponent>();
				for (auto entity : view)
				{
					if (static_cast<uint32_t>(entity) == querierEntityRaw)
						continue;
					if (matchEntity(entity, btTarget))
					{
						outThreatPos = view.get<TransformComponent>(entity).Location;
						if (outResolvedThreatEntityRaw)
							*outResolvedThreatEntityRaw = static_cast<uint32_t>(entity);
						return true;
					}
				}
			}
		}

		// 3. 自动在场景中检索带有 TagComponent 且名称包含 Threat / Sniper / Enemy / Hostile 的实体
		float closestDistSq = 1e18f;
		entt::entity bestThreatEnt = entt::null;
		glm::vec3 bestThreatPos(0.0f);

		auto tagView = reg.view<TransformComponent, TagComponent>();
		for (auto entity : tagView)
		{
			if (static_cast<uint32_t>(entity) == querierEntityRaw)
				continue;

			const std::string& tag = StringPool::Get().NameOf(tagView.get<TagComponent>(entity).Tag);
			auto containsSub = [](const std::string& str, const char* sub) {
				auto it = std::search(str.begin(), str.end(), sub, sub + std::strlen(sub),
					[](char c1, char c2) { return std::tolower(static_cast<unsigned char>(c1)) == std::tolower(static_cast<unsigned char>(c2)); });
				return it != str.end();
			};

			if (containsSub(tag, "threat") || containsSub(tag, "sniper") || containsSub(tag, "enemy") || containsSub(tag, "hostile"))
			{
				const auto& tc = tagView.get<TransformComponent>(entity);
				glm::vec3 diff = tc.Location - querierPos;
				float dSq = glm::dot(diff, diff);
				if (dSq < closestDistSq)
				{
					closestDistSq = dSq;
					bestThreatEnt = entity;
					bestThreatPos = tc.Location;
				}
			}
		}

		if (bestThreatEnt != entt::null)
		{
			outThreatPos = bestThreatPos;
			if (outResolvedThreatEntityRaw)
				*outResolvedThreatEntityRaw = static_cast<uint32_t>(bestThreatEnt);
			return true;
		}

		// 4. 保底回退：querierPos + (-15, 0, 0)
		outThreatPos = querierPos + glm::vec3(-15.0f, 0.0f, 0.0f);
		return false;
	}

	void EQSCoverSystem::GatherSceneObstacles(
		Scene& scene,
		uint32_t ignoreEntityRaw1,
		uint32_t ignoreEntityRaw2,
		std::vector<CoverBox>& outObstacles)
	{
		auto& reg = scene.GetRegistry();

		// 1. BoxCollider3DComponent
		auto boxView = reg.view<TransformComponent, BoxCollider3DComponent>();
		for (auto entity : boxView)
		{
			uint32_t raw = static_cast<uint32_t>(entity);
			if (raw == ignoreEntityRaw1 || raw == ignoreEntityRaw2)
				continue;
			const auto& tc = boxView.get<TransformComponent>(entity);
			const auto& col = boxView.get<BoxCollider3DComponent>(entity);
			glm::vec3 center = tc.Location + tc.Rotation * (col.Offset * tc.Scale);
			glm::vec3 halfExtents = glm::max(glm::vec3(0.01f), glm::abs(col.HalfExtents * tc.Scale));
			outObstacles.push_back(CoverBox::FromOBB(center, halfExtents, tc.Rotation));
		}

		// 2. WhiteboxComponent
		auto wbView = reg.view<TransformComponent, WhiteboxComponent>();
		for (auto entity : wbView)
		{
			uint32_t raw = static_cast<uint32_t>(entity);
			if (raw == ignoreEntityRaw1 || raw == ignoreEntityRaw2)
				continue;
			if (reg.all_of<BoxCollider3DComponent>(entity))
				continue;
			const auto& tc = wbView.get<TransformComponent>(entity);
			const auto& wb = wbView.get<WhiteboxComponent>(entity);
			glm::vec3 halfExtents = glm::max(glm::vec3(0.01f), glm::abs((wb.Dimensions * 0.5f) * tc.Scale));
			outObstacles.push_back(CoverBox::FromOBB(tc.Location, halfExtents, tc.Rotation));
		}

		// 3. NavMeshObstacleComponent
		auto obsView = reg.view<TransformComponent, NavMeshObstacleComponent>();
		for (auto entity : obsView)
		{
			uint32_t raw = static_cast<uint32_t>(entity);
			if (raw == ignoreEntityRaw1 || raw == ignoreEntityRaw2)
				continue;
			if (reg.all_of<BoxCollider3DComponent>(entity) || reg.all_of<WhiteboxComponent>(entity))
				continue;
			const auto& tc = obsView.get<TransformComponent>(entity);
			const auto& obs = obsView.get<NavMeshObstacleComponent>(entity);
			glm::vec3 halfExtents = glm::max(glm::vec3(0.01f), glm::abs(obs.Extents * tc.Scale));
			outObstacles.push_back(CoverBox::FromOBB(tc.Location, halfExtents, tc.Rotation));
		}
	}
}
