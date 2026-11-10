#include "wldpch.h"
#include "World/Renderer/HLODManager.h"

namespace World
{
	void HLODManager::EvaluateCluster(
		float distanceToCamera,
		float transitionDistance,
		float cullDistance,
		bool& outDrawProxy,
		bool& outDrawChildren)
	{
		if (distanceToCamera > cullDistance)
		{
			outDrawProxy = false;
			outDrawChildren = false;
			return;
		}

		if (distanceToCamera >= transitionDistance)
		{
			// 远距离：激活聚合代理网格，隐藏内部碎细子实体
			outDrawProxy = true;
			outDrawChildren = false;
		}
		else
		{
			// 近距离：展示子实体细节，隐藏代理网格
			outDrawProxy = false;
			outDrawChildren = true;
		}
	}
}
