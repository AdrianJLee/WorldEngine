#pragma once

#include "World/Core/Export.h"
#include <cstdint>

namespace World
{
	class WLD_API MeshLODEvaluator
	{
	public:
		constexpr static uint8_t kCulled = 255;

		// 带有滞后防抖带的 LOD 级别计算 (0=LOD0, 1=LOD1, 2=LOD2, 255=裁剪剔除)
		static uint8_t EvaluateLOD(
			float distanceToCamera,
			float lod1Distance,
			float lod2Distance,
			float cullDistance,
			uint8_t currentLOD,
			float hysteresis = 2.0f);
	};
}
