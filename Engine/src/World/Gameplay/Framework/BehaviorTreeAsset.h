#pragma once

#include "World/Core/Export.h"
#include "World/Gameplay/Framework/BehaviorTree.h"
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
		Action = 3
	};

	struct BehaviorTreeNodeDesc
	{
		uint32_t Id = 0;
		uint32_t ParentId = 0; // 0 表示根节点
		BehaviorTreeNodeKind Kind = BehaviorTreeNodeKind::Selector;
		std::string Title;
		std::string Description;
		std::string ConditionType; // 如 "HealthBelow", "HasTarget"
		std::string ActionType;    // 如 "SeekSmartObject", "UseSmartObject", "Patrol", "Wait"
		float FloatParam = 0.0f;
		std::string StringParam;
	};

	struct BehaviorBlackboardKeyDesc
	{
		std::string KeyName;
		std::string TypeName; // "Vector3", "Float", "Entity", "Bool"
		std::string DefaultValue;
	};

	class WLD_API BehaviorTreeAsset
	{
	public:
		BehaviorTreeAsset() = default;
		~BehaviorTreeAsset() = default;

		void Clear();
		void SetDefaultTemplate(); // 初始化一套默认 Root Selector -> Sequence(Heal) / Action(Patrol)

		bool LoadFromFile(const std::string& path, std::string* error = nullptr);
		bool SaveToFile(const std::string& path, std::string* error = nullptr) const;

		std::shared_ptr<BehaviorNode> InstantiateTree() const;

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
