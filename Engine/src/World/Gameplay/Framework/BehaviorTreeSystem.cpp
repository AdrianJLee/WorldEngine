#include "wldpch.h"
#include "World/Gameplay/Framework/BehaviorTreeSystem.h"
#include "World/Gameplay/Framework/BehaviorTreeAsset.h"
#include "World/Gameplay/Framework/Blackboard.h"
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

		struct EntityRunnerRecord
		{
			uint32_t TreeAssetId = 0;
			std::shared_ptr<BehaviorTreeRunner> Runner;
		};
		std::unordered_map<uint32_t, EntityRunnerRecord> s_EntityRunners;

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
		s_EntityRunners.clear();
	}

	void BehaviorTreeSystem::ClearEntityRunners()
	{
		std::lock_guard<std::mutex> lock(s_TreeRegistryMutex);
		s_EntityRunners.clear();
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

			Entity entity(&scene, entityHandle);
			uint32_t entityRaw = static_cast<uint32_t>((entt::entity)entityHandle);

			auto board = BlackboardRegistry::Get().GetOrCreateBlackboard(entityRaw, bt.BlackboardAssetId);
			if (board)
			{
				if (!board->HasKey("TargetPos"))
					board->RegisterKey("TargetPos", BlackboardKeyType::Vector3, bt.BlackboardTargetPos);
				if (!board->HasKey("TargetEntity"))
					board->RegisterKey("TargetEntity", BlackboardKeyType::Entity, bt.TargetEntity);
				if (!board->HasKey("Value"))
					board->RegisterKey("Value", BlackboardKeyType::Float, bt.BlackboardValue);
				if (!board->HasKey("Tag"))
					board->RegisterKey("Tag", BlackboardKeyType::Int32, static_cast<int32_t>(bt.BlackboardTag));
			}

			// 获取或创建实体专属的 BehaviorTreeRunner 实例（彻底隔离多实体运行期状态与观察者绑定）
			std::shared_ptr<BehaviorTreeRunner> runner;
			{
				std::lock_guard<std::mutex> lock(s_TreeRegistryMutex);
				auto it = s_EntityRunners.find(entityRaw);
				if (it == s_EntityRunners.end() || it->second.TreeAssetId != bt.TreeAssetId || !it->second.Runner)
				{
					if (it != s_EntityRunners.end() && it->second.Runner)
					{
						BehaviorContext oldCtx{ scene, entity, bt, *board, dt, 0 };
						it->second.Runner->Shutdown(oldCtx);
					}

					std::shared_ptr<BehaviorNode> rootNode;
					if (bt.TreeAssetId == 1)
					{
						rootNode = BuildDefaultDemoTree();
					}
					else
					{
						auto itTree = s_TreeRegistry.find(bt.TreeAssetId);
						if (itTree != s_TreeRegistry.end())
							rootNode = itTree->second;
					}

					if (rootNode)
					{
						runner = std::make_shared<BehaviorTreeRunner>(rootNode);
						s_EntityRunners[entityRaw] = { bt.TreeAssetId, runner };
						BehaviorContext initCtx{ scene, entity, bt, *board, dt, 0 };
						runner->Init(initCtx);
					}
				}
				else
				{
					runner = it->second.Runner;
				}
			}

			if (!runner)
				continue;

			float prevVal = bt.BlackboardValue;
			glm::vec3 prevPos = bt.BlackboardTargetPos;
			uint32_t prevTag = bt.BlackboardTag;

			BehaviorContext ctx{ scene, entity, bt, *board, dt, 0 };
			runner->Tick(ctx);

			// 将黑板标准状态反向镜像回组件缓存
			if (bt.BlackboardValue != prevVal)
				board->Set("Value", bt.BlackboardValue);
			else if (board->HasKey("Value"))
				bt.BlackboardValue = board->Get<float>("Value", bt.BlackboardValue);

			if (bt.BlackboardTargetPos != prevPos)
				board->Set("TargetPos", bt.BlackboardTargetPos);
			else if (board->HasKey("TargetPos"))
				bt.BlackboardTargetPos = board->Get<glm::vec3>("TargetPos", bt.BlackboardTargetPos);

			if (bt.BlackboardTag != prevTag)
				board->Set("Tag", static_cast<int32_t>(bt.BlackboardTag));
			else if (board->HasKey("Tag"))
				bt.BlackboardTag = static_cast<uint32_t>(board->Get<int32_t>("Tag", static_cast<int32_t>(bt.BlackboardTag)));

			if (board->HasKey("TargetEntity"))
				bt.TargetEntity = board->Get<uint32_t>("TargetEntity", bt.TargetEntity);
		}
	}
}