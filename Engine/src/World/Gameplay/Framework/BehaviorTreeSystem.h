#pragma once

#include "World/Core/Export.h"
#include "World/Gameplay/Framework/BehaviorTree.h"
#include <cstdint>
#include <memory>

namespace World
{
	class Scene;
}

namespace World::Gameplay
{
	class WLD_API BehaviorTreeSystem
	{
	public:
		static void RegisterTree(uint32_t assetId, std::shared_ptr<BehaviorNode> root);
		static void UnregisterTree(uint32_t assetId);
		static std::shared_ptr<BehaviorNode> GetTree(uint32_t assetId);
		static void ClearTrees();

		static void Update(Scene& scene, float dt);
	};
}
