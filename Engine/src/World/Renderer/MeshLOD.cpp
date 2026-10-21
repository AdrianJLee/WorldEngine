#include "wldpch.h"
#include "World/Renderer/MeshLOD.h"
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
}
