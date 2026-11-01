#include "wldpch.h"
#include "World/Level/AI/EQSCoverSystem.h"
#include "World/Terrain/Terrain.h"
#include "World/Scene/Scene.h"
#include "World/Scene/Components.h"
#include "World/Core/StringPool.h"

#include <algorithm>
#include <cmath>
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

		std::vector<glm::vec3> samplePoints;
		const glm::mat4 invTerrain = terrain ? glm::inverse(terrainWorld) : glm::mat4(1.0f);

		if (generatorMode == 1) // Concentric Rings
		{
			samplePoints.push_back(querierPos);
			const float ringStep = gridSpacing;
			const int ringCount = static_cast<int>(queryRadius / ringStep);
			for (int r = 1; r <= ringCount; ++r)
			{
				const float radius = r * ringStep;
				const float circumference = 2.0f * 3.14159265f * radius;
				const int pointCount = std::max(6, static_cast<int>(circumference / gridSpacing));
				for (int i = 0; i < pointCount; ++i)
				{
					const float angle = (static_cast<float>(i) / static_cast<float>(pointCount)) * 2.0f * 3.14159265f;
					glm::vec3 pt = querierPos + glm::vec3(std::cos(angle) * radius, 0.0f, std::sin(angle) * radius);
					samplePoints.push_back(pt);
				}
			}
		}
		else if (generatorMode == 2) // Hybrid Obstacle Edge
		{
			// 基础网格
			const int steps = static_cast<int>(queryRadius / gridSpacing);
			for (int z = -steps; z <= steps; ++z)
			{
				for (int x = -steps; x <= steps; ++x)
				{
					const glm::vec3 pt = querierPos + glm::vec3(x * gridSpacing, 0.0f, z * gridSpacing);
					const float distToQuerier = glm::distance(glm::vec2(pt.x, pt.z), glm::vec2(querierPos.x, querierPos.z));
					if (distToQuerier <= queryRadius)
						samplePoints.push_back(pt);
				}
			}

			// 障碍物四周紧贴外扩战术点 (0.75m 贴墙掩护位)
			for (const auto& box : obstacles)
			{
				const float offset = 0.75f;
				const float ex = box.HalfExtents.x + offset;
				const float ez = box.HalfExtents.z + offset;

				const std::vector<glm::vec3> localOffsets = {
					{ +ex, 0.0f,  0.0f },
					{ -ex, 0.0f,  0.0f },
					{  0.0f, 0.0f, +ez },
					{  0.0f, 0.0f, -ez },
					{ +ex, 0.0f, +ez * 0.5f },
					{ +ex, 0.0f, -ez * 0.5f },
					{ -ex, 0.0f, +ez * 0.5f },
					{ -ex, 0.0f, -ez * 0.5f },
					{ +ex * 0.5f, 0.0f, +ez },
					{ -ex * 0.5f, 0.0f, +ez },
					{ +ex * 0.5f, 0.0f, -ez },
					{ -ex * 0.5f, 0.0f, -ez }
				};

				for (const auto& lOff : localOffsets)
				{
					glm::vec3 worldPt = box.Center + box.Rotation * lOff;
					worldPt.y = querierPos.y;
					const float dist = glm::distance(glm::vec2(worldPt.x, worldPt.z), glm::vec2(querierPos.x, querierPos.z));
					if (dist <= queryRadius)
						samplePoints.push_back(worldPt);
				}
			}
		}
		else // 0: ProjectedGrid
		{
			const int steps = static_cast<int>(queryRadius / gridSpacing);
			for (int z = -steps; z <= steps; ++z)
			{
				for (int x = -steps; x <= steps; ++x)
				{
					const glm::vec3 pt = querierPos + glm::vec3(x * gridSpacing, 0.0f, z * gridSpacing);
					const float distToQuerier = glm::distance(glm::vec2(pt.x, pt.z), glm::vec2(querierPos.x, querierPos.z));
					if (distToQuerier <= queryRadius)
						samplePoints.push_back(pt);
				}
			}
		}

		// 贴合地形高程
		if (terrain)
		{
			for (auto& pt : samplePoints)
			{
				glm::vec3 localPt = glm::vec3(invTerrain * glm::vec4(pt, 1.0f));
				float h = terrain->SampleHeight(localPt.x, localPt.z);
				glm::vec3 worldGround = glm::vec3(terrainWorld * glm::vec4(localPt.x, h, localPt.z, 1.0f));
				pt.y = worldGround.y;
			}
		}

		const float querierDistToThreat = glm::distance(querierPos, threatPos);
		const glm::vec3 threatToQuerier = (querierDistToThreat > 1e-4f)
			? glm::normalize(querierPos - threatPos) : glm::vec3(0.0f, 0.0f, 1.0f);

		const glm::vec3 threatEye = threatPos + glm::vec3(0.0f, 1.6f, 0.0f);

		for (const auto& pt : samplePoints)
		{
			const float distToQuerier = glm::distance(glm::vec2(pt.x, pt.z), glm::vec2(querierPos.x, querierPos.z));
			if (distToQuerier > queryRadius)
				continue;

			// 检查点是否在障碍物内部 (不可站立穿模)
			bool insideObstacle = false;
			for (const auto& box : obstacles)
			{
				if (box.ContainsPoint(pt, 0.15f))
				{
					insideObstacle = true;
					break;
				}
			}
			if (insideObstacle)
				continue;

			// 视线遮蔽射线检测 (兼顾地表枪线与人眼高度视线)
			const glm::vec3 rayDirGround = pt - threatPos;
			const float rayLenGround = glm::length(rayDirGround);
			if (rayLenGround < 0.001f)
				continue;

			bool occludedGround = false;
			float hitDist = 0.0f;
			for (const auto& box : obstacles)
			{
				if (box.IntersectRay(threatPos, rayDirGround / rayLenGround, rayLenGround, hitDist))
				{
					occludedGround = true;
					break;
				}
			}

			// 蹲伏眼高 (0.9m) 与站立眼高 (1.6m) 检测
			const glm::vec3 candEyeCrouch = pt + glm::vec3(0.0f, 0.9f, 0.0f);
			const glm::vec3 rayDirCrouch = candEyeCrouch - threatEye;
			const float rayLenCrouch = glm::length(rayDirCrouch);
			bool occludedCrouch = false;
			if (rayLenCrouch > 0.001f)
			{
				for (const auto& box : obstacles)
				{
					if (box.IntersectRay(threatEye, rayDirCrouch / rayLenCrouch, rayLenCrouch, hitDist))
					{
						occludedCrouch = true;
						break;
					}
				}
			}

			const glm::vec3 candEyeStand = pt + glm::vec3(0.0f, 1.6f, 0.0f);
			const glm::vec3 rayDirStand = candEyeStand - threatEye;
			const float rayLenStand = glm::length(rayDirStand);
			bool occludedStand = false;
			if (rayLenStand > 0.001f)
			{
				for (const auto& box : obstacles)
				{
					if (box.IntersectRay(threatEye, rayDirStand / rayLenStand, rayLenStand, hitDist))
					{
						occludedStand = true;
						break;
					}
				}
			}

			float losOcclusion = 0.0f;
			if (occludedGround || (occludedCrouch && occludedStand))
				losOcclusion = 1.0f;
			else if (occludedCrouch)
				losOcclusion = 0.65f;

			// 计算掩体最近表面水平距离
			float minWallDist = 1e9f;
			for (const auto& box : obstacles)
			{
				minWallDist = std::min(minWallDist, box.DistanceToSurfaceXZ(pt));
			}

			// 掩体贴靠评分 WallProximityScore: 贴靠在掩体背面 (0.3m ~ 1.5m) 达到 1.0 满分
			float wallScore = 0.0f;
			if (losOcclusion > 0.0f && minWallDist < 1e8f)
			{
				const float deltaD = std::max(0.0f, minWallDist - 0.85f);
				wallScore = std::exp(-(deltaD * deltaD) / (2.0f * 1.25f * 1.25f));
			}

			// 地形高差优势评分 ElevationScore
			float elevScore = std::clamp((pt.y - threatPos.y) / 10.0f, 0.0f, 1.0f);

			EQSCoverCandidate cand;
			cand.Position = pt;
			cand.LineOfSightOcclusion = losOcclusion;
			cand.DistanceScore = 1.0f - (distToQuerier / queryRadius);
			cand.WallProximityScore = wallScore;
			cand.ElevationScore = elevScore;

			const EQSScoringWeights& w = (mode == EQSQueryMode::Cover) ? GetMutableCustomWeights() : EQSScoringWeights::ForMode(mode);

			if (mode == EQSQueryMode::Cover)
			{
				// 防守掩体：视线完全遮蔽为第一优先，贴靠掩体墙面加分显著，彻底消除原地停留缺陷
				cand.TotalScore = cand.LineOfSightOcclusion * (w.OcclusionWeight + w.WallProximityWeight * cand.WallProximityScore)
					+ cand.DistanceScore * w.DistanceWeight + cand.ElevationScore * w.ElevationWeight;

			}
			else if (mode == EQSQueryMode::Flank)
			{
				// 侧翼包抄：与威胁形成侧向 60~90 度夹角，需要具备对威胁的直视射击线 (Occlusion == 0)
				glm::vec3 threatToCand = glm::normalize(pt - threatPos);
				float cosAngle = glm::dot(threatToQuerier, threatToCand);
				cand.AngleScore = 1.0f - std::abs(cosAngle);
				cand.TotalScore = cand.AngleScore * w.AngleWeight + (1.0f - cand.LineOfSightOcclusion) * std::abs(w.OcclusionWeight)
					+ cand.DistanceScore * w.DistanceWeight + cand.ElevationScore * w.ElevationWeight;

			}
			else if (mode == EQSQueryMode::Retreat)
			{
				// 安全撤退：点位到威胁的距离大于查询者自身到威胁的距离，且视线处于遮蔽状态
				const float candDistToThreat = rayLenGround;
				float retreatGain = (candDistToThreat - querierDistToThreat) / queryRadius;
				cand.TotalScore = cand.LineOfSightOcclusion * w.OcclusionWeight + retreatGain * w.AngleWeight
					+ cand.DistanceScore * w.DistanceWeight + cand.ElevationScore * w.ElevationWeight;

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
				uint64_t uuidVal = static_cast<uint64_t>(reg.get<UUIDComponent>(e).ID);
				if (static_cast<uint32_t>(uuidVal & 0xFFFFFFFFULL) == hint)
					return true;
				if (static_cast<uint32_t>(uuidVal % 1000000000ULL) == hint)
					return true;
			}
			return false;
		};

		// 1. 若 threatEntityHint != 0：遍历场景 TransformComponent 实体匹配
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

		// 3. 若仍未匹配到，自动在场景中检索带有 TagComponent 且名称包含 Threat / Sniper / Enemy / Hostile 的实体
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
