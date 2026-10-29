#include "wldpch.h"
#include "World/Gameplay/Framework/BehaviorTreeSystem.h"
#include "World/Gameplay/Framework/BehaviorTreeAsset.h"
#include "World/Gameplay/Framework/SmartObjectSystem.h"
#include "World/Gameplay/Framework/DamageSystem.h"
#include "World/Scene/Scene.h"
#include "World/Scene/Entity.h"
#include "World/Scene/Components.h"
#include <mutex>
#include <unordered_map>
#include <cmath>

namespace World::Gameplay
{
	namespace
	{
		std::mutex s_TreeRegistryMutex;
		std::unordered_map<uint32_t, std::shared_ptr<BehaviorNode>> s_TreeRegistry;
		std::unordered_map<std::string, std::shared_ptr<BehaviorNode>> s_PathTreeRegistry;

		std::shared_ptr<BehaviorNode> BuildDefaultDemoTree()
		{
			BehaviorTreeAsset asset;
			asset.SetDefaultTemplate();
			return asset.InstantiateTree();
		}
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

		if (assetId == 1)
		{
			auto tree = BuildDefaultDemoTree();
			s_TreeRegistry[1] = tree;
			return tree;
		}

		return nullptr;
	}

	void BehaviorTreeSystem::ClearTrees()
	{
		std::lock_guard<std::mutex> lock(s_TreeRegistryMutex);
		s_TreeRegistry.clear();
		s_PathTreeRegistry.clear();
	}

	std::shared_ptr<BehaviorNode> BehaviorTreeSystem::GetOrCreateTree(const std::string& assetPath)
	{
		if (assetPath.empty())
			return nullptr;

		std::lock_guard<std::mutex> lock(s_TreeRegistryMutex);
		auto it = s_PathTreeRegistry.find(assetPath);
		if (it != s_PathTreeRegistry.end())
			return it->second;

		if (assetPath == "1" || assetPath == "default")
		{
			auto itId = s_TreeRegistry.find(1);
			if (itId != s_TreeRegistry.end())
			{
				s_PathTreeRegistry[assetPath] = itId->second;
				return itId->second;
			}
			auto tree = BuildDefaultDemoTree();
			s_TreeRegistry[1] = tree;
			s_PathTreeRegistry[assetPath] = tree;
			return tree;
		}

		BehaviorTreeAsset asset;
		std::string error;
		if (asset.LoadFromFile(assetPath, &error))
		{
			auto tree = asset.InstantiateTree();
			if (tree)
			{
				s_PathTreeRegistry[assetPath] = tree;
				uint32_t hashId = static_cast<uint32_t>(std::hash<std::string>{}(assetPath));
				if (hashId == 0) hashId = 1;
				s_TreeRegistry[hashId] = tree;
				return tree;
			}
		}

		return nullptr;
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
