#include "wldpch.h"
#include "World/Gameplay/Framework/BehaviorTree.h"

namespace World::Gameplay
{
	NodeStatus SequenceNode::Tick(float dt)
	{
		while (m_CurrentIndex < m_Children.size())
		{
			NodeStatus status = m_Children[m_CurrentIndex]->Tick(dt);
			if (status == NodeStatus::Running)
				return NodeStatus::Running;
			if (status == NodeStatus::Failure)
			{
				Reset();
				return NodeStatus::Failure;
			}
			++m_CurrentIndex;
		}
		Reset();
		return NodeStatus::Success;
	}

	void SequenceNode::Reset()
	{
		m_CurrentIndex = 0;
		for (auto& child : m_Children)
		{
			if (child) child->Reset();
		}
	}

	NodeStatus SelectorNode::Tick(float dt)
	{
		while (m_CurrentIndex < m_Children.size())
		{
			NodeStatus status = m_Children[m_CurrentIndex]->Tick(dt);
			if (status == NodeStatus::Running)
				return NodeStatus::Running;
			if (status == NodeStatus::Success)
			{
				Reset();
				return NodeStatus::Success;
			}
			++m_CurrentIndex;
		}
		Reset();
		return NodeStatus::Failure;
	}

	void SelectorNode::Reset()
	{
		m_CurrentIndex = 0;
		for (auto& child : m_Children)
		{
			if (child) child->Reset();
		}
	}
}
