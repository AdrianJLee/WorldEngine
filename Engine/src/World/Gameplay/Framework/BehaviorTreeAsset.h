#pragma once

#include "World/Core/Export.h"
#include "World/Gameplay/Framework/BehaviorTree.h"
#include "World/Gameplay/Framework/Blackboard.h"
#include <cstdint>
#include <string>
#include <vector>
#include <memory>

namespace World::Gameplay
{
	enum class BehaviorTreeNodeKind : uint8_t
	{
		Selector = 0,
		Sequence = 1,
		Condition = 2,
		Action = 3,
		Decorator = 4,
		Parallel = 5
	};

	struct BehaviorTreeNodeDesc
	{
		uint32_t Id = 0;
		uint32_t ParentId = 0; // 0 表示根节点
		BehaviorTreeNodeKind Kind = BehaviorTreeNodeKind::Selector;
		std::string Title;
		std::string Description;
		std::string ConditionType; // 如 "HealthBelow", "HasTarget", "BlackboardCondition"
		std::string ActionType;    // 如 "SeekSmartObject", "UseSmartObject", "Patrol", "Wait", "MoveToTarget"
		float FloatParam = 0.0f;
		std::string StringParam;
		FlowAbortMode AbortMode = FlowAbortMode::None;
		ConditionOperator CompOp = ConditionOperator::IsSet;
	};

	struct BehaviorBlackboardKeyDesc
	{
		std::string KeyName;
		std::string TypeName; // "Vector3", "Float", "Entity", "Bool", "Int", "String"
		std::string DefaultValue;
	};

	class WLD_API BehaviorTreeAsset
	{
	public:
		BehaviorTreeAsset() = default;
		~BehaviorTreeAsset() = default;

		void Clear();
		void SetDefaultTemplate();

		bool LoadFromFile(const std::string& path, std::string* error = nullptr);
		bool SaveToFile(const std::string& path, std::string* error = nullptr) const;

		std::shared_ptr<BehaviorNode> InstantiateTree() const;
		std::shared_ptr<Blackboard> InstantiateBlackboard() const;

		static const std::vector<std::string>& GetStandardConditionTypes();
		static const std::vector<std::string>& GetStandardActionTypes();
		static const std::vector<std::string>& GetStandardBlackboardTypes();

		std::vector<BehaviorTreeNodeDesc>& Nodes() { return m_Nodes; }
		const std::vector<BehaviorTreeNodeDesc>& Nodes() const { return m_Nodes; }

		std::vector<BehaviorBlackboardKeyDesc>& BlackboardKeys() { return m_BlackboardKeys; }
		const std::vector<BehaviorBlackboardKeyDesc>& BlackboardKeys() const { return m_BlackboardKeys; }

	private:
		std::vector<BehaviorTreeNodeDesc> m_Nodes;
		std::vector<BehaviorBlackboardKeyDesc> m_BlackboardKeys;
	};
}
