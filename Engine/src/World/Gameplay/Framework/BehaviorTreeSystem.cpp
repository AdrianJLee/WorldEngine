#include "wldpch.h"
#include "World/Gameplay/Framework/BehaviorTreeSystem.h"
#include "World/Scene/Scene.h"
#include "World/Scene/Entity.h"
#include "World/Scene/Components.h"
#include <mutex>
#include <unordered_map>

namespace World::Gameplay
{
	namespace
	{
		std::mutex s_TreeRegistryMutex;
		std::unordered_map<uint32_t, std::shared_ptr<BehaviorNode>> s_TreeRegistry;
	}

	void BehaviorTreeSystem::RegisterTree(uint32_t assetId, std::shared_ptr<BehaviorNode> root)
	{
		std::lock_guard<std::mutex> lock(s_TreeRegistryMutex);
		s_TreeRegistry[assetId] = std::move(root);
	}

	void BehaviorTreeSystem::UnregisterTree(uint32_t assetId)
	{
		std::lock_guard<std::mutex> lock(s_TreeRegistryMutex);
		s_TreeRegistry.erase(assetId);
	}

	std::shared_ptr<BehaviorNode> BehaviorTreeSystem::GetTree(uint32_t assetId)
	{
		std::lock_guard<std::mutex> lock(s_TreeRegistryMutex);
		auto it = s_TreeRegistry.find(assetId);
		if (it != s_TreeRegistry.end())
			return it->second;
		return nullptr;
	}

	void BehaviorTreeSystem::ClearTrees()
	{
		std::lock_guard<std::mutex> lock(s_TreeRegistryMutex);
		s_TreeRegistry.clear();
	}

	void BehaviorTreeSystem::Update(Scene& scene, float dt)
	{
		auto& registry = const_cast<entt::registry&>(std::as_const(scene).GetRegistry());
		auto view = registry.view<TransformComponent, BehaviorTreeComponent>();
		for (auto entityHandle : view)
		{
			auto& bt = view.get<BehaviorTreeComponent>(entityHandle);
			if (!bt.Enabled)
				continue;

			auto root = GetTree(bt.TreeAssetId);
			if (!root)
				continue;

			Entity entity(&scene, entityHandle);
			BehaviorContext ctx{ scene, entity, bt, dt };
			NodeStatus status = root->Tick(ctx);
			if (status == NodeStatus::Running)
				bt.Status = BehaviorStatus::Running;
			else if (status == NodeStatus::Success)
				bt.Status = BehaviorStatus::Success;
			else if (status == NodeStatus::Failure)
				bt.Status = BehaviorStatus::Failure;
		}
	}
}
