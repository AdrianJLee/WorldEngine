#pragma once

#include "World/Core/Export.h"
#include <functional>
#include <memory>
#include <vector>

namespace World::Gameplay
{
	enum class NodeStatus : uint8_t
	{
		Success = 0,
		Failure,
		Running
	};

	class WLD_API BehaviorNode
	{
	public:
		virtual ~BehaviorNode() = default;
		virtual NodeStatus Tick(float dt) = 0;
		virtual void Reset() {}
	};

	class WLD_API SequenceNode : public BehaviorNode
	{
	public:
		void AddChild(std::shared_ptr<BehaviorNode> child) { m_Children.push_back(std::move(child)); }
		NodeStatus Tick(float dt) override;
		void Reset() override;

	private:
		std::vector<std::shared_ptr<BehaviorNode>> m_Children;
		std::size_t m_CurrentIndex = 0;
	};

	class WLD_API SelectorNode : public BehaviorNode
	{
	public:
		void AddChild(std::shared_ptr<BehaviorNode> child) { m_Children.push_back(std::move(child)); }
		NodeStatus Tick(float dt) override;
		void Reset() override;

	private:
		std::vector<std::shared_ptr<BehaviorNode>> m_Children;
		std::size_t m_CurrentIndex = 0;
	};

	class WLD_API ConditionNode : public BehaviorNode
	{
	public:
		explicit ConditionNode(std::function<bool()> predicate) : m_Predicate(std::move(predicate)) {}
		NodeStatus Tick(float dt) override { return (m_Predicate && m_Predicate()) ? NodeStatus::Success : NodeStatus::Failure; }

	private:
		std::function<bool()> m_Predicate;
	};

	class WLD_API ActionNode : public BehaviorNode
	{
	public:
		explicit ActionNode(std::function<NodeStatus(float dt)> action) : m_Action(std::move(action)) {}
		NodeStatus Tick(float dt) override { return m_Action ? m_Action(dt) : NodeStatus::Failure; }

	private:
		std::function<NodeStatus(float dt)> m_Action;
	};

	class WLD_API BehaviorTreeRunner
	{
	public:
		explicit BehaviorTreeRunner(std::shared_ptr<BehaviorNode> root = nullptr) : m_Root(std::move(root)) {}
		void SetRoot(std::shared_ptr<BehaviorNode> root) { m_Root = std::move(root); }
		NodeStatus Tick(float dt) { return m_Root ? m_Root->Tick(dt) : NodeStatus::Failure; }
		void Reset() { if (m_Root) m_Root->Reset(); }

	private:
		std::shared_ptr<BehaviorNode> m_Root;
	};
}
