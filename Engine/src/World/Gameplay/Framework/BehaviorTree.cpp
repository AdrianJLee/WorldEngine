#include "wldpch.h"
#include "World/Gameplay/Framework/BehaviorTree.h"

namespace World::Gameplay
{
	void CompositeNode::AddChild(std::shared_ptr<BehaviorNode> child)
	{
		if (child)
		{
			child->SetChildIndexInParent(static_cast<uint16_t>(m_Children.size()));
			child->SetParentComposite(this);
			m_Children.push_back(std::move(child));
		}
	}

	void CompositeNode::Init(BehaviorContext& ctx)
	{
		SetDepth(ctx.CurrentDepth);
		for (size_t i = 0; i < m_Children.size(); ++i)
		{
			if (m_Children[i])
			{
				m_Children[i]->SetChildIndexInParent(static_cast<uint16_t>(i));
				m_Children[i]->SetParentComposite(this);
				m_Children[i]->SetDepth(ctx.CurrentDepth + 1);
				BehaviorContext childCtx = ctx;
				childCtx.CurrentDepth = ctx.CurrentDepth + 1;
				m_Children[i]->Init(childCtx);
			}
		}
	}

	void CompositeNode::Shutdown(BehaviorContext& ctx)
	{
		for (auto& child : m_Children)
		{
			if (child)
				child->Shutdown(ctx);
		}
	}

	void CompositeNode::OnAbort(BehaviorContext& ctx)
	{
		for (auto& child : m_Children)
		{
			if (child)
				child->OnAbort(ctx);
		}
	}

	void CompositeNode::Reset(BehaviorContext& ctx)
	{
		ctx.Component.SetPath(ctx.CurrentDepth, 0);
		ctx.Component.CurrentNodeIndex = 0;
		for (auto& child : m_Children)
		{
			if (child)
				child->Reset(ctx);
		}
	}

		void SequenceNode::OnAbort(BehaviorContext& ctx)
	{
		uint8_t depth = ctx.CurrentDepth;
		uint16_t curChild = ctx.Component.GetPath(depth);
		if (curChild < m_Children.size() && m_Children[curChild])
		{
			BehaviorContext childCtx = ctx;
			childCtx.CurrentDepth = depth + 1;
			m_Children[curChild]->OnAbort(childCtx);
		}
	}

	void SelectorNode::OnAbort(BehaviorContext& ctx)
	{
		uint8_t depth = ctx.CurrentDepth;
		uint16_t curChild = ctx.Component.GetPath(depth);
		if (curChild < m_Children.size() && m_Children[curChild])
		{
			BehaviorContext childCtx = ctx;
			childCtx.CurrentDepth = depth + 1;
			m_Children[curChild]->OnAbort(childCtx);
		}
	}

	NodeStatus SequenceNode::Tick(BehaviorContext& ctx)
	{
		if (m_Children.empty())
		{
			ctx.Component.SetPath(ctx.CurrentDepth, 0);
			ctx.Component.Status = BehaviorStatus::Success;
			return NodeStatus::Success;
		}

		uint8_t depth = ctx.CurrentDepth;
		uint16_t curChild = ctx.Component.GetPath(depth);
		if (curChild >= m_Children.size())
		{
			curChild = 0;
			ctx.Component.SetPath(depth, 0);
		}

		while (curChild < m_Children.size())
		{
			ctx.Component.SetPath(depth, curChild);
			ctx.Component.CurrentNodeIndex = curChild;

			auto& child = m_Children[curChild];
			BehaviorContext childCtx = ctx;
			childCtx.CurrentDepth = depth + 1;

			NodeStatus status = child ? child->Tick(childCtx) : NodeStatus::Failure;
			if (status == NodeStatus::Running)
			{
				ctx.Component.Status = BehaviorStatus::Running;
				return NodeStatus::Running;
			}
			if (status == NodeStatus::Failure)
			{
				ctx.Component.SetPath(depth, 0);
				ctx.Component.CurrentNodeIndex = 0;
				ctx.Component.Status = BehaviorStatus::Failure;
				Reset(ctx);
				return NodeStatus::Failure;
			}
			++curChild;
		}

		ctx.Component.SetPath(depth, 0);
		ctx.Component.CurrentNodeIndex = 0;
		ctx.Component.Status = BehaviorStatus::Success;
		Reset(ctx);
		return NodeStatus::Success;
	}

	NodeStatus SelectorNode::Tick(BehaviorContext& ctx)
	{
		if (m_Children.empty())
		{
			ctx.Component.SetPath(ctx.CurrentDepth, 0);
			ctx.Component.Status = BehaviorStatus::Failure;
			return NodeStatus::Failure;
		}

		uint8_t depth = ctx.CurrentDepth;
		uint16_t curChild = ctx.Component.GetPath(depth);
		if (curChild >= m_Children.size())
		{
			curChild = 0;
			ctx.Component.SetPath(depth, 0);
		}

		while (curChild < m_Children.size())
		{
			ctx.Component.SetPath(depth, curChild);
			ctx.Component.CurrentNodeIndex = curChild;

			auto& child = m_Children[curChild];
			BehaviorContext childCtx = ctx;
			childCtx.CurrentDepth = depth + 1;

			NodeStatus status = child ? child->Tick(childCtx) : NodeStatus::Failure;
			if (status == NodeStatus::Running)
			{
				ctx.Component.Status = BehaviorStatus::Running;
				return NodeStatus::Running;
			}
			if (status == NodeStatus::Success)
			{
				ctx.Component.SetPath(depth, 0);
				ctx.Component.CurrentNodeIndex = 0;
				ctx.Component.Status = BehaviorStatus::Success;
				Reset(ctx);
				return NodeStatus::Success;
			}
			++curChild;
		}

		ctx.Component.SetPath(depth, 0);
		ctx.Component.CurrentNodeIndex = 0;
		ctx.Component.Status = BehaviorStatus::Failure;
		Reset(ctx);
		return NodeStatus::Failure;
	}

	NodeStatus ParallelNode::Tick(BehaviorContext& ctx)
	{
		if (m_Children.empty())
			return NodeStatus::Success;

		size_t successCount = 0;
		size_t failureCount = 0;
		bool anyRunning = false;

		for (size_t i = 0; i < m_Children.size(); ++i)
		{
			auto& child = m_Children[i];
			if (!child)
				continue;

			BehaviorContext childCtx = ctx;
			childCtx.CurrentDepth = ctx.CurrentDepth + 1;
			NodeStatus s = child->Tick(childCtx);
			if (s == NodeStatus::Running)
				anyRunning = true;
			else if (s == NodeStatus::Success)
				++successCount;
			else if (s == NodeStatus::Failure)
				++failureCount;
		}

		if (FinishPolicy == Policy::RequireOneSuccess && successCount > 0)
		{
			OnAbort(ctx);
			return NodeStatus::Success;
		}

		if (failureCount > 0 && FinishPolicy == Policy::RequireAllSuccess)
		{
			OnAbort(ctx);
			return NodeStatus::Failure;
		}

		if (anyRunning)
		{
			ctx.Component.Status = BehaviorStatus::Running;
			return NodeStatus::Running;
		}

		return (successCount == m_Children.size()) ? NodeStatus::Success : NodeStatus::Failure;
	}

	void BehaviorDecorator::SetChild(std::shared_ptr<BehaviorNode> child)
	{
		m_Child = std::move(child);
		if (m_Child)
		{
			m_Child->SetChildIndexInParent(GetChildIndexInParent());
			m_Child->SetParentComposite(GetParentComposite());
		}
	}

	void BehaviorDecorator::Init(BehaviorContext& ctx)
	{
		if (m_Child)
		{
			m_Child->SetChildIndexInParent(GetChildIndexInParent());
			m_Child->SetParentComposite(GetParentComposite());
			BehaviorContext childCtx = ctx;
			childCtx.CurrentDepth = ctx.CurrentDepth + 1;
			m_Child->Init(childCtx);
		}
	}

	void BehaviorDecorator::Shutdown(BehaviorContext& ctx)
	{
		if (m_Child)
		{
			BehaviorContext childCtx = ctx;
			childCtx.CurrentDepth = ctx.CurrentDepth + 1;
			m_Child->Shutdown(childCtx);
		}
	}

	NodeStatus BehaviorDecorator::Tick(BehaviorContext& ctx)
	{
		if (!EvaluateCondition(ctx))
		{
			if (ctx.Component.Status == BehaviorStatus::Running)
			{
				OnAbort(ctx);
			}
			return NodeStatus::Failure;
		}

		if (!m_Child)
			return NodeStatus::Success;

		BehaviorContext childCtx = ctx;
		childCtx.CurrentDepth = ctx.CurrentDepth + 1;
		return m_Child->Tick(childCtx);
	}

	void BehaviorDecorator::OnAbort(BehaviorContext& ctx)
	{
		if (m_Child)
		{
			BehaviorContext childCtx = ctx;
			childCtx.CurrentDepth = ctx.CurrentDepth + 1;
			m_Child->OnAbort(childCtx);
		}
	}

	void BehaviorDecorator::Reset(BehaviorContext& ctx)
	{
		if (m_Child)
		{
			BehaviorContext childCtx = ctx;
			childCtx.CurrentDepth = ctx.CurrentDepth + 1;
			m_Child->Reset(childCtx);
		}
	}

	void BlackboardConditionDecorator::Init(BehaviorContext& ctx)
	{
		m_BoundComponent = &ctx.Component;
		m_ObservedBoard = &ctx.Board;
		SetDepth(ctx.CurrentDepth);

		BehaviorDecorator::Init(ctx);

		if (m_AbortMode == FlowAbortMode::None || KeyName.empty())
			return;

		uint32_t keyId = ctx.Board.GetKeyId(KeyName);
		if (keyId == UINT32_MAX)
		{
			keyId = ctx.Board.RegisterKey(KeyName, BlackboardKeyType::Float);
		}

		if (m_ObserverId != 0)
		{
			ctx.Board.RemoveObserver(keyId, m_ObserverId);
			m_ObserverId = 0;
		}

		uint16_t myChildIdx = GetChildIndexInParent();
		uint8_t parentDepth = (GetDepth() > 0) ? static_cast<uint8_t>(GetDepth() - 1) : 0;
		FlowAbortMode abortMode = m_AbortMode;

		m_ObserverId = ctx.Board.AddObserver(keyId, [this, myChildIdx, parentDepth, abortMode](uint32_t, const BlackboardValue&) {
			if (!m_ObservedBoard || !m_BoundComponent)
				return;

			bool condPassed = EvaluateConditionOnBoard(*m_ObservedBoard);

			if (abortMode == FlowAbortMode::Self || abortMode == FlowAbortMode::Both)
			{
				if (!condPassed && m_BoundComponent->Status == BehaviorStatus::Running)
				{
					if (parentDepth < 8 && m_BoundComponent->GetPath(parentDepth) == myChildIdx)
					{
						m_BoundComponent->AbortRequested = 1;
					}
				}
			}

			if (abortMode == FlowAbortMode::LowerPriority || abortMode == FlowAbortMode::Both)
			{
				if (condPassed && m_BoundComponent->Status == BehaviorStatus::Running)
				{
					if (parentDepth < 8 && m_BoundComponent->GetPath(parentDepth) > myChildIdx)
					{
						m_BoundComponent->AbortRequested = 1;
					}
				}
			}
		});
	}

	void BlackboardConditionDecorator::Shutdown(BehaviorContext& ctx)
	{
		if (m_ObserverId != 0 && m_ObservedBoard)
		{
			uint32_t keyId = m_ObservedBoard->GetKeyId(KeyName);
			if (keyId != UINT32_MAX)
			{
				m_ObservedBoard->RemoveObserver(keyId, m_ObserverId);
			}
			m_ObserverId = 0;
			m_ObservedBoard = nullptr;
		}
		BehaviorDecorator::Shutdown(ctx);
	}

	bool BlackboardConditionDecorator::EvaluateCondition(BehaviorContext& ctx)
	{
		return EvaluateConditionOnBoard(ctx.Board);
	}

	bool BlackboardConditionDecorator::EvaluateConditionOnBoard(const Blackboard& board) const
	{
		const BlackboardValue* valPtr = board.GetValue(KeyName);
		if (Op == ConditionOperator::IsSet)
		{
			if (!valPtr) return false;
			if (std::holds_alternative<bool>(*valPtr)) return std::get<bool>(*valPtr);
			if (std::holds_alternative<uint32_t>(*valPtr)) return std::get<uint32_t>(*valPtr) != 0;
			if (std::holds_alternative<float>(*valPtr)) return std::abs(std::get<float>(*valPtr)) > 1e-4f;
			if (std::holds_alternative<std::string>(*valPtr)) return !std::get<std::string>(*valPtr).empty();
			return true;
		}
		if (Op == ConditionOperator::IsNotSet)
		{
			if (!valPtr) return true;
			if (std::holds_alternative<bool>(*valPtr)) return !std::get<bool>(*valPtr);
			if (std::holds_alternative<uint32_t>(*valPtr)) return std::get<uint32_t>(*valPtr) == 0;
			return false;
		}

		if (!valPtr)
			return false;

		const BlackboardValue& actual = *valPtr;
		if (Op == ConditionOperator::Equal)
		{
			return actual == TargetValue;
		}
		if (Op == ConditionOperator::NotEqual)
		{
			return actual != TargetValue;
		}

		// 数值比较
		float aVal = 0.0f;
		float bVal = 0.0f;
		if (std::holds_alternative<float>(actual)) aVal = std::get<float>(actual);
		else if (std::holds_alternative<int32_t>(actual)) aVal = static_cast<float>(std::get<int32_t>(actual));
		else if (std::holds_alternative<uint32_t>(actual)) aVal = static_cast<float>(std::get<uint32_t>(actual));

		if (std::holds_alternative<float>(TargetValue)) bVal = std::get<float>(TargetValue);
		else if (std::holds_alternative<int32_t>(TargetValue)) bVal = static_cast<float>(std::get<int32_t>(TargetValue));
		else if (std::holds_alternative<uint32_t>(TargetValue)) bVal = static_cast<float>(std::get<uint32_t>(TargetValue));

		if (Op == ConditionOperator::Greater)
			return aVal > bVal;
		if (Op == ConditionOperator::Less)
			return aVal < bVal;

		return false;
	}

	void BehaviorTreeRunner::Init(BehaviorContext& ctx)
	{
		if (m_Root)
		{
			ctx.CurrentDepth = 0;
			m_Root->Init(ctx);
		}
		m_Initialized = true;
	}

	void BehaviorTreeRunner::Shutdown(BehaviorContext& ctx)
	{
		if (m_Root)
		{
			ctx.CurrentDepth = 0;
			m_Root->Shutdown(ctx);
		}
		m_Initialized = false;
	}

	NodeStatus BehaviorTreeRunner::Tick(BehaviorContext& ctx)
	{
		if (!m_Root || !ctx.Component.Enabled)
		{
			ctx.Component.Status = BehaviorStatus::Failure;
			return NodeStatus::Failure;
		}

		if (!m_Initialized)
		{
			Init(ctx);
		}

		// 响应式打断处理：若黑板观察者触发了打断请求，立刻中止当前运行路径并重置执行栈
		if (ctx.Component.AbortRequested != 0)
		{
			ctx.Component.AbortRequested = 0;
			Abort(ctx);
		}

		ctx.CurrentDepth = 0;
		NodeStatus status = m_Root->Tick(ctx);
		if (status == NodeStatus::Running)
		{
			ctx.Component.Status = BehaviorStatus::Running;
		}
		else if (status == NodeStatus::Success)
		{
			ctx.Component.Status = BehaviorStatus::Success;
			ctx.Component.ClearPath();
		}
		else if (status == NodeStatus::Failure)
		{
			ctx.Component.Status = BehaviorStatus::Failure;
			ctx.Component.ClearPath();
		}
		return status;
	}

	void BehaviorTreeRunner::Abort(BehaviorContext& ctx)
	{
		if (m_Root)
		{
			ctx.CurrentDepth = 0;
			m_Root->OnAbort(ctx);
			ctx.Component.ClearPath();
			ctx.Component.Status = BehaviorStatus::Inactive;
		}
	}

	void BehaviorTreeRunner::Reset(BehaviorContext& ctx)
	{
		if (m_Root)
		{
			ctx.CurrentDepth = 0;
			m_Root->Reset(ctx);
			ctx.Component.ClearPath();
			ctx.Component.Status = BehaviorStatus::Inactive;
		}
	}
}