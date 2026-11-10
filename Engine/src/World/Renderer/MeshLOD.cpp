#include "wldpch.h"
#include "World/Renderer/MeshLOD.h"
#include "World/Scene/Components.h"

#include <algorithm>

namespace World
{
	uint8_t MeshLODEvaluator::EvaluateLOD(
		float distanceToCamera,
		float lod1Distance,
		float lod2Distance,
		float cullDistance,
		uint8_t currentLOD,
		float hysteresis)
	{
		const float dists[2] = { lod1Distance, lod2Distance };
		return EvaluateDynamicLOD(distanceToCamera, dists, 2, cullDistance, currentLOD, hysteresis);
	}

	uint8_t MeshLODEvaluator::EvaluateDynamicLOD(
		float distanceToCamera,
		const float* lodDistances,
		uint8_t numLevels,
		float cullDistance,
		uint8_t currentLOD,
		float hysteresis)
	{
		const float h = std::max(0.0f, hysteresis);

		if (cullDistance > 0.0f)
		{
			if (currentLOD == kCulled)
			{
				if (distanceToCamera > (cullDistance - h))
					return kCulled;
			}
			else
			{
				if (distanceToCamera > (cullDistance + h))
					return kCulled;
			}
		}

		if (!lodDistances || numLevels == 0)
			return 0;

		const uint8_t maxLOD = std::min<uint8_t>(numLevels, 7);

		float lowerBound = 0.0f;
		float upperBound = 1e9f;

		if (currentLOD > 0 && currentLOD <= maxLOD)
		{
			lowerBound = lodDistances[currentLOD - 1] - h;
		}
		if (currentLOD < maxLOD)
		{
			upperBound = lodDistances[currentLOD] + h;
		}

		if (currentLOD != kCulled && distanceToCamera >= lowerBound && distanceToCamera <= upperBound)
		{
			return currentLOD;
		}

		for (int i = static_cast<int>(maxLOD) - 1; i >= 0; --i)
		{
			float threshold = lodDistances[i];
			if (currentLOD == static_cast<uint8_t>(i + 1))
				threshold -= h;
			else if (currentLOD == static_cast<uint8_t>(i))
				threshold += h;

			if (distanceToCamera > threshold)
			{
				return static_cast<uint8_t>(i + 1);
			}
		}

		return 0;
	}

	MeshLODManager& MeshLODManager::Get()
	{
		static MeshLODManager s_Instance;
		return s_Instance;
	}

	uint8_t MeshLODManager::EvaluateLOD(float distance, MeshLODComponent& comp, uint8_t currentLOD)
	{
		comp.CurrentDistance = distance;

		// 1. 实体级强制档位锁定优先，其次为全局强制档位
		if (comp.ForcedLOD >= 0)
			return static_cast<uint8_t>(std::clamp<int>(comp.ForcedLOD, 0, 7));
		if (m_ForcedLOD >= 0)
			return static_cast<uint8_t>(std::clamp<int>(m_ForcedLOD, 0, 7));

		// 2. 结合实例偏置与全局缩放
		const float globalScale = m_GlobalDistanceScale > 0.0f ? m_GlobalDistanceScale : 1.0f;
		const float bias = comp.LODBias > 0.0f ? comp.LODBias : 1.0f;
		// bias 越大代表物体更重要、更晚降级 (相当于有效距离被除以 bias)
		const float effectiveDist = (distance * globalScale) / bias;

		float h = comp.Hysteresis;
		if (h <= 0.0f)
		{
			h = effectiveDist * 0.05f;
		}

		const float cullDist = comp.CullDistance > 0.0f ? comp.CullDistance : 1e9f;
		const float dists[7] = {
			comp.LOD1Distance, comp.LOD2Distance, comp.LOD3Distance, comp.LOD4Distance,
			comp.LOD5Distance, comp.LOD6Distance, comp.LOD7Distance
		};

		const uint8_t numLevels = std::clamp<uint8_t>(comp.MaxLODLevel, 1, 7);
		uint8_t target = MeshLODEvaluator::EvaluateDynamicLOD(effectiveDist, dists, numLevels, cullDist, currentLOD, h);
		if (target != MeshLODEvaluator::kCulled && target > comp.MaxLODLevel)
		{
			target = comp.MaxLODLevel;
		}
		return target;
	}

	void MeshLODManager::RecordLODResult(uint8_t lod)
	{
		++m_Stats.TotalLODEntities;
		if (lod == MeshLODEvaluator::kCulled)
		{
			++m_Stats.CulledCount;
		}
		else if (lod < 8)
		{
			++m_Stats.LODLevelCounts[lod];
			if (lod == 0) ++m_Stats.LOD0Count;
			else if (lod == 1) ++m_Stats.LOD1Count;
			else if (lod == 2) ++m_Stats.LOD2Count;
		}
	}

	glm::vec4 MeshLODManager::GetLODColor(uint8_t lod) const
	{
		switch (lod)
		{
		case 0: return glm::vec4(0.2f, 0.9f, 0.2f, 1.0f);   // 鲜绿: LOD0 原模
		case 1: return glm::vec4(0.95f, 0.85f, 0.1f, 1.0f); // 金黄: LOD1
		case 2: return glm::vec4(0.95f, 0.45f, 0.1f, 1.0f); // 橙色: LOD2
		case 3: return glm::vec4(0.2f, 0.6f, 1.0f, 1.0f);   // 天蓝: LOD3
		case 4: return glm::vec4(0.3f, 0.35f, 0.95f, 1.0f); // 深蓝: LOD4
		case 5: return glm::vec4(0.7f, 0.2f, 0.9f, 1.0f);   // 浅紫: LOD5
		case 6: return glm::vec4(0.5f, 0.1f, 0.7f, 1.0f);   // 深紫: LOD6
		case 7: return glm::vec4(0.4f, 0.2f, 0.1f, 1.0f);   // 棕褐: LOD7
		case MeshLODEvaluator::kCulled:
		default:
			return glm::vec4(0.8f, 0.15f, 0.15f, 1.0f);     // 绯红: 视距剔除
		}
	}
}
