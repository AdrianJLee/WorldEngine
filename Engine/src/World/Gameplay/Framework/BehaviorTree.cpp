#include "wldpch.h"
#include "World/Gameplay/Framework/BehaviorTree.h"

namespace World::Gameplay
{
	NodeStatus SequenceNode::Tick(BehaviorContext& ctx)
	{
		if (m_Children.empty())
		{
			ctx.Component.CurrentNodeIndex = 0;
			ctx.Component.Status = BehaviorStatus::Success;
			return NodeStatus::Success;
		}

		if (ctx.Component.CurrentNodeIndex >= m_Children.size())
		{
			ctx.Component.CurrentNodeIndex = 0;
		}

		while (ctx.Component.CurrentNodeIndex < m_Children.size())
		{
			auto& child = m_Children[ctx.Component.CurrentNodeIndex];
			NodeStatus status = child ? child->Tick(ctx) : NodeStatus::Failure;
			if (status == NodeStatus::Running)
			{
				ctx.Component.Status = BehaviorStatus::Running;
				return NodeStatus::Running;
			}
			if (status == NodeStatus::Failure)
			{
				Reset(ctx);
				ctx.Component.CurrentNodeIndex = 0;
				ctx.Component.Status = BehaviorStatus::Failure;
				return NodeStatus::Failure;
			}
			++ctx.Component.CurrentNodeIndex;
		}

		Reset(ctx);
		ctx.Component.CurrentNodeIndex = 0;
		ctx.Component.Status = BehaviorStatus::Success;
		return NodeStatus::Success;
	}

	void SequenceNode::Reset(BehaviorContext& ctx)
	{
		ctx.Component.CurrentNodeIndex = 0;
		for (auto& child : m_Children)
		{
			if (child)
				child->Reset(ctx);
		}
	}

	NodeStatus SelectorNode::Tick(BehaviorContext& ctx)
	{
		if (m_Children.empty())
		{
			ctx.Component.CurrentNodeIndex = 0;
			ctx.Component.Status = BehaviorStatus::Failure;
			return NodeStatus::Failure;
		}

		if (ctx.Component.CurrentNodeIndex >= m_Children.size())
		{
			ctx.Component.CurrentNodeIndex = 0;
		}

		while (ctx.Component.CurrentNodeIndex < m_Children.size())
		{
			auto& child = m_Children[ctx.Component.CurrentNodeIndex];
			NodeStatus status = child ? child->Tick(ctx) : NodeStatus::Failure;
			if (status == NodeStatus::Running)
			{
				ctx.Component.Status = BehaviorStatus::Running;
				return NodeStatus::Running;
			}
			if (status == NodeStatus::Success)
			{
				Reset(ctx);
				ctx.Component.CurrentNodeIndex = 0;
				ctx.Component.Status = BehaviorStatus::Success;
				return NodeStatus::Success;
			}
			++ctx.Component.CurrentNodeIndex;
		}

		Reset(ctx);
		ctx.Component.CurrentNodeIndex = 0;
		ctx.Component.Status = BehaviorStatus::Failure;
		return NodeStatus::Failure;
	}

	void SelectorNode::Reset(BehaviorContext& ctx)
	{
		ctx.Component.CurrentNodeIndex = 0;
		for (auto& child : m_Children)
		{
			if (child)
				child->Reset(ctx);
		}
	}
}
