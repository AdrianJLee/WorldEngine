#pragma once

#include "World/Core/Export.h"
#include "World/Scene/Components.h"

namespace World
{
	class WLD_API HLODManager
	{
	public:
		// 评估 HLOD 聚合簇的可见性状态
		static void EvaluateCluster(
			float distanceToCamera,
			float transitionDistance,
			float cullDistance,
			bool& outDrawProxy,
			bool& outDrawChildren);
	};
}
