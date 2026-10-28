#pragma once

#include "World/Core/Export.h"
#include "World/Scene/Entity.h"
#include "World/Scene/Components.h"
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

namespace World
{
	class Scene;
}

namespace World::Gameplay
{
	enum class NodeStatus : uint8_t
	{
		Success = 0,
		Failure,
		Running
	};

	struct BehaviorContext
	{
		Scene& CurrentScene;
		Entity Self;
		BehaviorTreeComponent& Component;
		float Dt = 0.0f;
	};

	class WLD_API BehaviorNode
	{
	public:
		virtual ~BehaviorNode() = default;
		virtual NodeStatus Tick(BehaviorContext& ctx) = 0;
		virtual void Reset(BehaviorContext& ctx) { (void)ctx; }
	};

	class WLD_API SequenceNode : public BehaviorNode
	{
	public:
		void AddChild(std::shared_ptr<BehaviorNode> child) { m_Children.push_back(std::move(child)); }
		NodeStatus Tick(BehaviorContext& ctx) override;
		void Reset(BehaviorContext& ctx) override;

	private:
		std::vector<std::shared_ptr<BehaviorNode>> m_Children;
	};

	class WLD_API SelectorNode : public BehaviorNode
	{
	public:
		void AddChild(std::shared_ptr<BehaviorNode> child) { m_Children.push_back(std::move(child)); }
		NodeStatus Tick(BehaviorContext& ctx) override;
		void Reset(BehaviorContext& ctx) override;

	private:
		std::vector<std::shared_ptr<BehaviorNode>> m_Children;
	};

	class WLD_API ConditionNode : public BehaviorNode
	{
	public:
		explicit ConditionNode(std::function<bool(BehaviorContext& ctx)> predicate) : m_Predicate(std::move(predicate)) {}
		explicit ConditionNode(std::function<bool()> predicate)
			: m_Predicate([p = std::move(predicate)](BehaviorContext&) { return p ? p() : false; }) {}

		NodeStatus Tick(BehaviorContext& ctx) override
		{
			return (m_Predicate && m_Predicate(ctx)) ? NodeStatus::Success : NodeStatus::Failure;
		}

	private:
		std::function<bool(BehaviorContext& ctx)> m_Predicate;
	};

	class WLD_API ActionNode : public BehaviorNode
	{
	public:
		explicit ActionNode(std::function<NodeStatus(BehaviorContext& ctx)> action) : m_Action(std::move(action)) {}
		explicit ActionNode(std::function<NodeStatus(float dt)> action)
			: m_Action([a = std::move(action)](BehaviorContext& ctx) { return a ? a(ctx.Dt) : NodeStatus::Failure; }) {}
		explicit ActionNode(std::function<NodeStatus()> action)
			: m_Action([a = std::move(action)](BehaviorContext& ctx) { return a ? a() : NodeStatus::Failure; }) {}

		NodeStatus Tick(BehaviorContext& ctx) override
		{
			return m_Action ? m_Action(ctx) : NodeStatus::Failure;
		}

	private:
		std::function<NodeStatus(BehaviorContext& ctx)> m_Action;
	};

	class WLD_API BehaviorTreeRunner
	{
	public:
		explicit BehaviorTreeRunner(std::shared_ptr<BehaviorNode> root = nullptr) : m_Root(std::move(root)) {}
		void SetRoot(std::shared_ptr<BehaviorNode> root) { m_Root = std::move(root); }
		std::shared_ptr<BehaviorNode> GetRoot() const { return m_Root; }

		NodeStatus Tick(BehaviorContext& ctx)
		{
			if (!m_Root)
			{
				ctx.Component.Status = BehaviorStatus::Failure;
				return NodeStatus::Failure;
			}
			NodeStatus status = m_Root->Tick(ctx);
			if (status == NodeStatus::Running)
				ctx.Component.Status = BehaviorStatus::Running;
			else if (status == NodeStatus::Success)
				ctx.Component.Status = BehaviorStatus::Success;
			else if (status == NodeStatus::Failure)
				ctx.Component.Status = BehaviorStatus::Failure;
			return status;
		}

		void Reset(BehaviorContext& ctx)
		{
			if (m_Root)
				m_Root->Reset(ctx);
		}

	private:
		std::shared_ptr<BehaviorNode> m_Root;
	};
}
