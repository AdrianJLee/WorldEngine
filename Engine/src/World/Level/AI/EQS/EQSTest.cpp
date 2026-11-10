#include "wldpch.h"
#include "World/Level/AI/EQS/EQSTest.h"
#include "World/Level/AI/EQSCoverSystem.h"
#include "World/Level/AI/EQS/EnvQueryManager.h"
#include "World/Scene/Scene.h"
#include "World/Scene/Components.h"
#include "World/Terrain/Terrain.h"
#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>

namespace World::EQS
{
	float IEQSTest::NormalizeScore(float rawValue) const
	{
		float norm = 0.0f;
		switch (Curve)
		{
		case ScoringCurve::Linear:
			if (FilterMax > FilterMin)
				norm = std::clamp((rawValue - FilterMin) / (FilterMax - FilterMin), 0.0f, 1.0f);
			else
				norm = (rawValue >= FilterMin) ? 1.0f : 0.0f;
			break;
		case ScoringCurve::Inverse:
			if (FilterMax > FilterMin)
				norm = 1.0f - std::clamp((rawValue - FilterMin) / (FilterMax - FilterMin), 0.0f, 1.0f);
			else
				norm = (rawValue <= FilterMin) ? 1.0f : 0.0f;
			break;
		case ScoringCurve::ClampedGaussian:
		{
			const float sigma = (GaussianSigma > 0.001f) ? GaussianSigma : 1.25f;
			const float delta = std::abs(rawValue - IdealValue);
			norm = std::exp(-(delta * delta) / (2.0f * sigma * sigma));
			break;
		}
		case ScoringCurve::StepThreshold:
			norm = (rawValue >= IdealValue) ? 1.0f : 0.0f;
			break;
		}
		return norm;
	}

	EQSTest_Distance::EQSTest_Distance()
	{
		Purpose = TestPurpose::ScoreOnly;
		Curve = ScoringCurve::Inverse;
		Weight = 0.7f;
		FilterMin = 0.0f;
		FilterMax = 20.0f;
	}

	void EQSTest_Distance::RunTest(const EQSQueryContext& ctx, std::vector<EQSCandidateItem>& inOutItems)
	{
		glm::vec3 refPos = ctx.QuerierPos;
		if (Context)
		{
			EQSContextData cdata;
			if (Context->Resolve(ctx.ScenePtr, ctx.QuerierEntityRaw, ctx.QuerierPos, cdata) && !cdata.Positions.empty())
				refPos = cdata.Positions.front();
		}

		const float maxRange = (FilterMax > FilterMin) ? FilterMax : ((ctx.QueryRadius > 0.0f) ? ctx.QueryRadius : 20.0f);

		for (auto& item : inOutItems)
		{
			if (item.FilteredOut)
			{
				item.TestScores.push_back(0.0f);
				continue;
			}

			const float dist = Use2DDistance
				? glm::distance(glm::vec2(item.Position.x, item.Position.z), glm::vec2(refPos.x, refPos.z))
				: glm::distance(item.Position, refPos);

			if (Purpose == TestPurpose::FilterOnly || Purpose == TestPurpose::FilterAndScore)
			{
				if (dist < FilterMin || dist > maxRange)
				{
					item.FilteredOut = true;
					item.TestScores.push_back(0.0f);
					continue;
				}
			}

			if (Purpose != TestPurpose::FilterOnly)
			{
				float norm = 0.0f;
				if (Curve == ScoringCurve::Inverse)
					norm = 1.0f - std::clamp(dist / maxRange, 0.0f, 1.0f);
				else if (Curve == ScoringCurve::Linear)
					norm = std::clamp(dist / maxRange, 0.0f, 1.0f);
				else
					norm = NormalizeScore(dist);

				const float score = norm * Weight;
				item.TotalScore += score;
				item.TestScores.push_back(score);
			}
			else
			{
				item.TestScores.push_back(Weight);
			}
		}
	}

	EQSTest_Trace::EQSTest_Trace()
	{
		Purpose = TestPurpose::ScoreOnly;
		Curve = ScoringCurve::Linear;
		Weight = 1.8f;
		FilterMin = 0.0f;
		FilterMax = 1.0f;
	}

	void EQSTest_Trace::RunTest(const EQSQueryContext& ctx, std::vector<EQSCandidateItem>& inOutItems)
	{
		glm::vec3 threatPos = ctx.ThreatPos;
		if (Context)
		{
			EQSContextData cdata;
			if (Context->Resolve(ctx.ScenePtr, ctx.QuerierEntityRaw, ctx.QuerierPos, cdata) && !cdata.Positions.empty())
				threatPos = cdata.Positions.front();
		}

		const glm::vec3 threatEye = threatPos + glm::vec3(0.0f, ctx.StandingEyeHeight, 0.0f);

		for (auto& item : inOutItems)
		{
			if (item.FilteredOut)
			{
				item.TestScores.push_back(0.0f);
				continue;
			}

			const glm::vec3 rayDirGround = item.Position - threatPos;
			const float rayLenGround = glm::length(rayDirGround);
			if (rayLenGround < 0.001f)
			{
				item.TestScores.push_back(0.0f);
				continue;
			}

			bool occludedGround = false;
			float hitDist = 0.0f;
			for (const auto& box : ctx.Obstacles)
			{
				if (box.IntersectRay(threatPos, rayDirGround / rayLenGround, rayLenGround, hitDist))
				{
					occludedGround = true;
					break;
				}
			}

			const glm::vec3 candEyeCrouch = item.Position + glm::vec3(0.0f, ctx.CrouchingEyeHeight, 0.0f);
			const glm::vec3 rayDirCrouch = candEyeCrouch - threatEye;
			const float rayLenCrouch = glm::length(rayDirCrouch);
			bool occludedCrouch = false;
			if (rayLenCrouch > 0.001f)
			{
				for (const auto& box : ctx.Obstacles)
				{
					if (box.IntersectRay(threatEye, rayDirCrouch / rayLenCrouch, rayLenCrouch, hitDist))
					{
						occludedCrouch = true;
						break;
					}
				}
			}

			const glm::vec3 candEyeStand = item.Position + glm::vec3(0.0f, ctx.StandingEyeHeight, 0.0f);
			const glm::vec3 rayDirStand = candEyeStand - threatEye;
			const float rayLenStand = glm::length(rayDirStand);
			bool occludedStand = false;
			if (rayLenStand > 0.001f)
			{
				for (const auto& box : ctx.Obstacles)
				{
					if (box.IntersectRay(threatEye, rayDirStand / rayLenStand, rayLenStand, hitDist))
					{
						occludedStand = true;
						break;
					}
				}
			}

			// 地形高度场遮蔽采样测试
			if (!occludedStand && ctx.Terrain)
			{
				const glm::mat4 invTerrain = glm::inverse(ctx.TerrainWorldTransform);
				for (int step = 1; step <= 3; ++step)
				{
					float t = static_cast<float>(step) / 4.0f;
					glm::vec3 samplePt = threatEye + rayDirStand * t;
					glm::vec3 localPt = glm::vec3(invTerrain * glm::vec4(samplePt, 1.0f));
					float th = ctx.Terrain->SampleHeight(localPt.x, localPt.z);
					if (th > localPt.y + 0.1f)
					{
						occludedStand = true;
						occludedCrouch = true;
						occludedGround = true;
						break;
					}
				}
			}

			float losOcclusion = 0.0f;
			if (occludedCrouch && occludedStand)
				losOcclusion = 1.0f;
			else if (occludedCrouch)
				losOcclusion = 0.65f;
			else if (occludedGround && !occludedCrouch)
				losOcclusion = 0.15f; // 地表低矮障碍仅微弱遮蔽足部，头胸躯干完全暴露在枪线下

			const float rawVal = InvertOcclusion ? (1.0f - losOcclusion) : losOcclusion;

			if (Purpose == TestPurpose::FilterOnly || Purpose == TestPurpose::FilterAndScore)
			{
				if (rawVal < FilterMin || rawVal > FilterMax)
				{
					item.FilteredOut = true;
					item.TestScores.push_back(0.0f);
					continue;
				}
			}

			if (Purpose != TestPurpose::FilterOnly)
			{
				const float score = rawVal * Weight;
				item.TotalScore += score;
				item.TestScores.push_back(score);
			}
			else
			{
				item.TestScores.push_back(Weight);
			}
		}
	}

	EQSTest_Dot::EQSTest_Dot()
	{
		Purpose = TestPurpose::ScoreOnly;
		Curve = ScoringCurve::Linear;
		Weight = 2.0f;
		FilterMin = 0.0f;
		FilterMax = 1.0f;
	}

	void EQSTest_Dot::RunTest(const EQSQueryContext& ctx, std::vector<EQSCandidateItem>& inOutItems)
	{
		glm::vec3 threatPos = ctx.ThreatPos;
		if (Context)
		{
			EQSContextData cdata;
			if (Context->Resolve(ctx.ScenePtr, ctx.QuerierEntityRaw, ctx.QuerierPos, cdata) && !cdata.Positions.empty())
				threatPos = cdata.Positions.front();
		}

		const float querierDistToThreat = glm::distance(ctx.QuerierPos, threatPos);
		const glm::vec3 threatToQuerier = (querierDistToThreat > 1e-4f)
			? glm::normalize(ctx.QuerierPos - threatPos)
			: glm::vec3(0.0f, 0.0f, 1.0f);

		for (auto& item : inOutItems)
		{
			if (item.FilteredOut)
			{
				item.TestScores.push_back(0.0f);
				continue;
			}

			const glm::vec3 diff = item.Position - threatPos;
			const float len = glm::length(diff);
			if (len < 0.001f)
			{
				item.TestScores.push_back(0.0f);
				continue;
			}

			const glm::vec3 threatToCand = diff / len;
			const float cosAngle = glm::dot(threatToQuerier, threatToCand);
			const float angleVal = PeakAtPerpendicular
				? (1.0f - std::abs(cosAngle))
				: ((cosAngle + 1.0f) * 0.5f);

			if (Purpose == TestPurpose::FilterOnly || Purpose == TestPurpose::FilterAndScore)
			{
				if (angleVal < FilterMin || angleVal > FilterMax)
				{
					item.FilteredOut = true;
					item.TestScores.push_back(0.0f);
					continue;
				}
			}

			if (Purpose != TestPurpose::FilterOnly)
			{
				const float score = angleVal * Weight;
				item.TotalScore += score;
				item.TestScores.push_back(score);
			}
			else
			{
				item.TestScores.push_back(Weight);
			}
		}
	}

	EQSTest_Overlap::EQSTest_Overlap()
	{
		Purpose = TestPurpose::FilterOnly;
		Weight = 1.0f;
		ExpandRadius = 0.15f;
	}

	void EQSTest_Overlap::RunTest(const EQSQueryContext& ctx, std::vector<EQSCandidateItem>& inOutItems)
	{
		for (auto& item : inOutItems)
		{
			if (item.FilteredOut)
			{
				item.TestScores.push_back(0.0f);
				continue;
			}

			bool insideObstacle = false;
			for (const auto& box : ctx.Obstacles)
			{
				if (box.ContainsPoint(item.Position, ExpandRadius))
				{
					insideObstacle = true;
					break;
				}
			}

			if (insideObstacle)
			{
				item.FilteredOut = true;
				item.TestScores.push_back(0.0f);
			}
			else
			{
				item.TestScores.push_back(Weight);
			}
		}
	}

	EQSTest_Height::EQSTest_Height()
	{
		Purpose = TestPurpose::ScoreOnly;
		Curve = ScoringCurve::Linear;
		Weight = 0.3f;
		FilterMin = 0.0f;
		FilterMax = 1.0f;
	}

	void EQSTest_Height::RunTest(const EQSQueryContext& ctx, std::vector<EQSCandidateItem>& inOutItems)
	{
		glm::vec3 threatPos = ctx.ThreatPos;
		if (Context)
		{
			EQSContextData cdata;
			if (Context->Resolve(ctx.ScenePtr, ctx.QuerierEntityRaw, ctx.QuerierPos, cdata) && !cdata.Positions.empty())
				threatPos = cdata.Positions.front();
		}

		for (auto& item : inOutItems)
		{
			if (item.FilteredOut)
			{
				item.TestScores.push_back(0.0f);
				continue;
			}

			const float elevScore = std::clamp((item.Position.y - threatPos.y) / 10.0f, 0.0f, 1.0f);

			if (Purpose != TestPurpose::FilterOnly)
			{
				const float score = elevScore * Weight;
				item.TotalScore += score;
				item.TestScores.push_back(score);
			}
			else
			{
				item.TestScores.push_back(Weight);
			}
		}
	}

	EQSTest_WallProximity::EQSTest_WallProximity()
	{
		Purpose = TestPurpose::ScoreOnly;
		Curve = ScoringCurve::ClampedGaussian;
		Weight = 1.2f;
		IdealValue = 0.85f;
		GaussianSigma = 1.25f;
	}

	void EQSTest_WallProximity::RunTest(const EQSQueryContext& ctx, std::vector<EQSCandidateItem>& inOutItems)
	{
		glm::vec3 threatPos = ctx.ThreatPos;
		if (Context)
		{
			EQSContextData cdata;
			if (Context->Resolve(ctx.ScenePtr, ctx.QuerierEntityRaw, ctx.QuerierPos, cdata) && !cdata.Positions.empty())
				threatPos = cdata.Positions.front();
		}

		for (auto& item : inOutItems)
		{
			if (item.FilteredOut)
			{
				item.TestScores.push_back(0.0f);
				continue;
			}

			bool isOccluded = false;
			if (RequireOcclusion)
			{
				const glm::vec3 ray = item.Position - threatPos;
				const float len = glm::length(ray);
				if (len > 0.001f)
				{
					float hitDist = 0.0f;
					for (const auto& box : ctx.Obstacles)
					{
						if (box.IntersectRay(threatPos, ray / len, len, hitDist))
						{
							isOccluded = true;
							break;
						}
					}
				}
			}

			float wallScore = 0.0f;
			if (!RequireOcclusion || isOccluded)
			{
				float minWallDist = 1e9f;
				for (const auto& box : ctx.Obstacles)
				{
					minWallDist = std::min(minWallDist, box.DistanceToSurfaceXZ(item.Position));
				}

				if (minWallDist < 1e8f)
				{
					const float deltaD = minWallDist - IdealValue;
					const float sigma = (GaussianSigma > 0.001f) ? GaussianSigma : 1.25f;
					wallScore = std::exp(-(deltaD * deltaD) / (2.0f * sigma * sigma));
				}
			}

			if (Purpose != TestPurpose::FilterOnly)
			{
				const float score = wallScore * Weight;
				item.TotalScore += score;
				item.TestScores.push_back(score);
			}
			else
			{
				item.TestScores.push_back(Weight);
			}
		}
	}

	EQSTest_FriendlyPenalty::EQSTest_FriendlyPenalty()
	{
		Purpose = TestPurpose::ScoreOnly;
		Curve = ScoringCurve::Linear;
		Weight = 1.0f;
		AvoidanceRadius = 3.0f;
	}

	void EQSTest_FriendlyPenalty::RunTest(const EQSQueryContext& ctx, std::vector<EQSCandidateItem>& inOutItems)
	{
		std::vector<glm::vec3> friendPositions;
		if (ctx.ScenePtr)
		{
			auto& reg = ctx.ScenePtr->GetRegistry();
			for (auto entity : reg.view<TransformComponent, BehaviorTreeComponent>())
			{
				if (static_cast<uint32_t>(entity) == ctx.QuerierEntityRaw || static_cast<uint32_t>(entity) == ctx.ThreatEntityHint)
					continue;
				const auto& tc = reg.get<TransformComponent>(entity);
				friendPositions.push_back(tc.Location);
			}
		}

		for (auto& item : inOutItems)
		{
			if (item.FilteredOut)
			{
				item.TestScores.push_back(0.0f);
				continue;
			}

			float penalty = 0.0f;
			for (const auto& friendPos : friendPositions)
			{
				float dist = glm::distance(glm::vec2(item.Position.x, item.Position.z), glm::vec2(friendPos.x, friendPos.z));
				if (dist < AvoidanceRadius && dist > 0.01f)
				{
					penalty += (1.0f - (dist / AvoidanceRadius));
				}
			}

			float penaltyScore = -std::min(penalty, 2.0f) * Weight;
			item.TotalScore += penaltyScore;
			item.TestScores.push_back(penaltyScore);
		}
	}

	EQSTest_CoverReservation::EQSTest_CoverReservation()
	{
		Purpose = TestPurpose::FilterOnly;
		Weight = 1.0f;
		ReservationRadius = 1.5f;
	}

	void EQSTest_CoverReservation::RunTest(const EQSQueryContext& ctx, std::vector<EQSCandidateItem>& inOutItems)
	{
		const auto& pool = EnvQueryManager::Get().GetReservationPool();
		for (auto& item : inOutItems)
		{
			if (item.FilteredOut)
			{
				item.TestScores.push_back(0.0f);
				continue;
			}

			if (pool.IsCoverReserved(item.Position, ctx.QuerierEntityRaw, ReservationRadius))
			{
				item.FilteredOut = true;
				item.TestScores.push_back(0.0f);
			}
			else
			{
				item.TestScores.push_back(Weight);
			}
		}
	}

	
	EQSTest_Scripted::EQSTest_Scripted()
	{
		Purpose = TestPurpose::FilterAndScore;
		Curve = ScoringCurve::Linear;
		Weight = 1.0f;
		FilterMin = 0.0f;
		FilterMax = 1.0f;
	}

	EQSTest_Scripted::EQSTest_Scripted(const std::string& scriptPath)
		: ScriptPath(scriptPath)
	{
		Purpose = TestPurpose::FilterAndScore;
		Curve = ScoringCurve::Linear;
		Weight = 1.0f;
		FilterMin = 0.0f;
		FilterMax = 1.0f;
	}

	EQSTest_Scripted::EQSTest_Scripted(std::function<std::pair<float, bool>(const glm::vec3&, const EQSQueryContext&)> evaluator)
		: CustomEvaluator(std::move(evaluator))
	{
		Purpose = TestPurpose::FilterAndScore;
		Curve = ScoringCurve::Linear;
		Weight = 1.0f;
		FilterMin = 0.0f;
		FilterMax = 1.0f;
	}

	void EQSTest_Scripted::RunTest(const EQSQueryContext& ctx, std::vector<EQSCandidateItem>& inOutItems)
	{
		for (auto& item : inOutItems)
		{
			if (item.FilteredOut)
			{
				item.TestScores.push_back(0.0f);
				continue;
			}

			float rawScore = 1.0f;
			bool pass = true;

			if (CustomEvaluator)
			{
				auto res = CustomEvaluator(item.Position, ctx);
				rawScore = res.first;
				pass = res.second;
			}

			if (Purpose == TestPurpose::FilterOnly || Purpose == TestPurpose::FilterAndScore)
			{
				if (!pass || rawScore < FilterMin || rawScore > FilterMax)
				{
					item.FilteredOut = true;
					item.TestScores.push_back(0.0f);
					continue;
				}
			}

			if (Purpose != TestPurpose::FilterOnly)
			{
				const float norm = NormalizeScore(rawScore);
				const float score = norm * Weight;
				item.TotalScore += score;
				item.TestScores.push_back(score);
			}
			else
			{
				item.TestScores.push_back(Weight);
			}
		}
	}

	
	EQSTest_Pathfinding::EQSTest_Pathfinding()
	{
		Purpose = TestPurpose::FilterAndScore;
		Curve = ScoringCurve::Inverse;
		Weight = 1.0f;
		FilterMin = 0.5f;
		FilterMax = 50.0f;
	}

	void EQSTest_Pathfinding::RunTest(const EQSQueryContext& ctx, std::vector<EQSCandidateItem>& inOutItems)
	{
		const glm::vec3 startPos = ctx.QuerierPos;
		for (size_t idx = 0; idx < inOutItems.size(); ++idx)
		{
			auto& item = inOutItems[idx];
			if (item.FilteredOut)
			{
				item.TestScores.push_back(0.0f);
				item.TestRawValues.push_back(0.0f);
				continue;
			}

			float dist = glm::distance(item.Position, startPos);
			bool reachable = true;

			for (const auto& box : ctx.Obstacles)
			{
				float hitDist = 0.0f;
				glm::vec3 rayDir = item.Position - startPos;
				float rayLen = glm::length(rayDir);
				if (rayLen > 0.001f)
				{
					if (box.IntersectRay(startPos, rayDir / rayLen, rayLen, hitDist))
					{
						dist += 4.0f; // 障碍物绕行阻断成本惩罚
					}
				}
			}

			item.TestRawValues.push_back(dist);

			if (Purpose == TestPurpose::FilterOnly || Purpose == TestPurpose::FilterAndScore)
			{
				if (!reachable || dist > FilterMax || dist < FilterMin)
				{
					item.FilteredOut = true;
					item.FilteredByTest = 9;
					item.TestScores.push_back(0.0f);
					continue;
				}
			}

			if (Purpose != TestPurpose::FilterOnly)
			{
				float norm = NormalizeScore(dist);
				float score = norm * Weight;
				item.TotalScore += score;
				item.TestScores.push_back(score);
			}
			else
			{
				item.TestScores.push_back(Weight);
			}
		}
	}

	EQSTest_FieldOfView::EQSTest_FieldOfView()
	{
		Purpose = TestPurpose::ScoreOnly;
		Curve = ScoringCurve::Linear;
		Weight = 1.5f;
		IdealValue = 90.0f;
		FilterMin = 0.0f;
		FilterMax = 180.0f;
	}

	void EQSTest_FieldOfView::RunTest(const EQSQueryContext& ctx, std::vector<EQSCandidateItem>& inOutItems)
	{
		glm::vec3 threatPos = ctx.ThreatPos;
		glm::vec3 threatFwd = ctx.ThreatForward;
		if (glm::length(glm::vec2(threatFwd.x, threatFwd.z)) < 0.001f)
		{
			threatFwd = glm::vec3(0.0f, 0.0f, -1.0f);
		}
		threatFwd = glm::normalize(glm::vec3(threatFwd.x, 0.0f, threatFwd.z));

		for (size_t idx = 0; idx < inOutItems.size(); ++idx)
		{
			auto& item = inOutItems[idx];
			if (item.FilteredOut)
			{
				item.TestScores.push_back(0.0f);
				item.TestRawValues.push_back(0.0f);
				continue;
			}

			glm::vec3 toItem = item.Position - threatPos;
			toItem.y = 0.0f;
			float len = glm::length(toItem);
			float angleDeg = 180.0f;
			if (len > 0.001f)
			{
				float dotVal = glm::clamp(glm::dot(threatFwd, toItem / len), -1.0f, 1.0f);
				angleDeg = glm::degrees(std::acos(dotVal));
			}

			item.TestRawValues.push_back(angleDeg);

			float evalAngle = InvertFOV ? angleDeg : (180.0f - angleDeg);

			if (Purpose == TestPurpose::FilterOnly || Purpose == TestPurpose::FilterAndScore)
			{
				if (angleDeg < FovAngleDegrees * 0.5f && InvertFOV)
				{
					item.FilteredOut = true;
					item.FilteredByTest = 10;
					item.TestScores.push_back(0.0f);
					continue;
				}
			}

			if (Purpose != TestPurpose::FilterOnly)
			{
				float norm = std::clamp(evalAngle / 180.0f, 0.0f, 1.0f);
				float score = norm * Weight;
				item.TotalScore += score;
				item.TestScores.push_back(score);
			}
			else
			{
				item.TestScores.push_back(Weight);
			}
		}
	}

	EQSTestRegistry::EQSTestRegistry()
	{
		Register("Distance", std::make_shared<EQSTest_Distance>());
		Register("Trace", std::make_shared<EQSTest_Trace>());
		Register("Dot", std::make_shared<EQSTest_Dot>());
		Register("Overlap", std::make_shared<EQSTest_Overlap>());
		Register("Height", std::make_shared<EQSTest_Height>());
		Register("WallProximity", std::make_shared<EQSTest_WallProximity>());
		Register("FriendlyPenalty", std::make_shared<EQSTest_FriendlyPenalty>());
		Register("CoverReservation", std::make_shared<EQSTest_CoverReservation>());
		Register("Scripted", std::make_shared<EQSTest_Scripted>());
		Register("Pathfinding", std::make_shared<EQSTest_Pathfinding>());
		Register("FieldOfView", std::make_shared<EQSTest_FieldOfView>());
	}

	EQSTestRegistry& EQSTestRegistry::Get()
	{
		static EQSTestRegistry s_Instance;
		return s_Instance;
	}

	void EQSTestRegistry::Register(const std::string& name, std::shared_ptr<IEQSTest> test)
	{
		m_Tests[name] = std::move(test);
	}

	std::shared_ptr<IEQSTest> EQSTestRegistry::Find(const std::string& name) const
	{
		auto it = m_Tests.find(name);
		if (it != m_Tests.end())
			return it->second;
		return nullptr;
	}
}
