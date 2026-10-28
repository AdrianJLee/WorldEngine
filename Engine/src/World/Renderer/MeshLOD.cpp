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
		if (distanceToCamera > cullDistance)
			return kCulled;

		const float h = std::max(0.0f, hysteresis);

		if (currentLOD == 0)
		{
			if (distanceToCamera > (lod2Distance + h))
				return 2;
			if (distanceToCamera > (lod1Distance + h))
				return 1;
			return 0;
		}
		else if (currentLOD == 1)
		{
			if (distanceToCamera < (lod1Distance - h))
				return 0;
			if (distanceToCamera > (lod2Distance + h))
				return 2;
			return 1;
		}
		else // currentLOD == 2
		{
			if (distanceToCamera < (lod1Distance - h))
				return 0;
			if (distanceToCamera < (lod2Distance - h))
				return 1;
			return 2;
		}
	}

	MeshLODManager& MeshLODManager::Get()
	{
		static MeshLODManager s_Instance;
		return s_Instance;
	}

	uint8_t MeshLODManager::EvaluateLOD(float distance, const MeshLODComponent& comp, uint8_t currentLOD)
	{
		if (m_ForcedLOD >= 0)
			return static_cast<uint8_t>(m_ForcedLOD);

		const float scale = m_GlobalDistanceScale > 0.0f ? m_GlobalDistanceScale : 1.0f;
		const float scaledDist = distance * scale;

		float h = comp.Hysteresis;
		if (h <= 0.0f)
		{
			h = scaledDist * 0.05f;
		}

		const float cullDist = comp.CullDistance > 0.0f ? comp.CullDistance : 1e9f;
		return MeshLODEvaluator::EvaluateLOD(
			scaledDist,
			comp.LOD1Distance,
			comp.LOD2Distance,
			cullDist,
			currentLOD,
			h);
	}

	void MeshLODManager::RecordLODResult(uint8_t lod)
	{
		++m_Stats.TotalLODEntities;
		if (lod == 0)
			++m_Stats.LOD0Count;
		else if (lod == 1)
			++m_Stats.LOD1Count;
		else if (lod == 2)
			++m_Stats.LOD2Count;
		else if (lod == MeshLODEvaluator::kCulled)
			++m_Stats.CulledCount;
	}

	glm::vec4 MeshLODManager::GetLODColor(uint8_t lod) const
	{
		if (lod == 0)
			return glm::vec4(0.2f, 0.9f, 0.2f, 1.0f);
		if (lod == 1)
			return glm::vec4(0.95f, 0.85f, 0.1f, 1.0f);
		if (lod == 2)
			return glm::vec4(0.95f, 0.45f, 0.1f, 1.0f);
		if (lod == MeshLODEvaluator::kCulled)
			return glm::vec4(0.8f, 0.15f, 0.15f, 1.0f);
		return glm::vec4(1.0f);
	}
}