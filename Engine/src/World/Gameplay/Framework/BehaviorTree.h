#pragma once

#include "World/Core/Export.h"
#include "World/Scene/Entity.h"
#include "World/Scene/Components.h"
#include "World/Gameplay/Framework/Blackboard.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
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
		Failure = 1,
		Running = 2
	};

	enum class FlowAbortMode : uint8_t
	{
		None = 0,           // 不主动打断
		Self = 1,           // 当自身条件不满足时，打断自身正在运行的子树
		LowerPriority = 2,  // 当自身条件满足时，打断正在运行的低优先级兄弟子树
		Both = 3            // 同时具备 Self 与 LowerPriority 抢占打断能力
	};

	struct BehaviorContext
	{
		Scene& CurrentScene;
		Entity Self;
		BehaviorTreeComponent& Component;
		Blackboard& Board;
		float Dt = 0.0f;
		uint8_t CurrentDepth = 0;

		static Blackboard& GetFallbackBoard()
		{
			static Blackboard s_Fallback;
			return s_Fallback;
		}

		BehaviorContext(Scene& scene, Entity self, BehaviorTreeComponent& comp, Blackboard& board, float dt = 0.0f, uint8_t depth = 0)
			: CurrentScene(scene), Self(self), Component(comp), Board(board), Dt(dt), CurrentDepth(depth)
		{
		}

		BehaviorContext(Scene& scene, Entity self, BehaviorTreeComponent& comp, float dt = 0.0f, uint8_t depth = 0)
			: CurrentScene(scene), Self(self), Component(comp), Board(GetFallbackBoard()), Dt(dt), CurrentDepth(depth)
		{
		}
	};

	class CompositeNode;

	class WLD_API BehaviorNode
	{
	public:
		virtual ~BehaviorNode() = default;
		virtual void Init(BehaviorContext& ctx) { (void)ctx; }
		virtual void Shutdown(BehaviorContext& ctx) { (void)ctx; }
		virtual NodeStatus Tick(BehaviorContext& ctx) = 0;
		virtual void OnAbort(BehaviorContext& ctx) { (void)ctx; }
		virtual void Reset(BehaviorContext& ctx) { (void)ctx; }

		uint16_t GetNodeId() const { return m_NodeId; }
		void SetNodeId(uint16_t id) { m_NodeId = id; }
		const std::string& GetName() const { return m_Name; }
		void SetName(const std::string& name) { m_Name = name; }

		uint16_t GetChildIndexInParent() const { return m_ChildIndex; }
		void SetChildIndexInParent(uint16_t idx) { m_ChildIndex = idx; }
		uint8_t GetDepth() const { return m_Depth; }
		void SetDepth(uint8_t d) { m_Depth = d; }
		CompositeNode* GetParentComposite() const { return m_Parent; }
		void SetParentComposite(CompositeNode* p) { m_Parent = p; }

	private:
		uint16_t m_NodeId = 0;
		uint16_t m_ChildIndex = 0;
		uint8_t m_Depth = 0;
		CompositeNode* m_Parent = nullptr;
		std::string m_Name;
	};

	class WLD_API CompositeNode : public BehaviorNode
	{
	public:
		void AddChild(std::shared_ptr<BehaviorNode> child);
		const std::vector<std::shared_ptr<BehaviorNode>>& GetChildren() const { return m_Children; }
		size_t GetChildCount() const { return m_Children.size(); }

		void Init(BehaviorContext& ctx) override;
		void Shutdown(BehaviorContext& ctx) override;
		void OnAbort(BehaviorContext& ctx) override;
		void Reset(BehaviorContext& ctx) override;

	protected:
		std::vector<std::shared_ptr<BehaviorNode>> m_Children;
	};

	class WLD_API SequenceNode : public CompositeNode
	{
	public:
		NodeStatus Tick(BehaviorContext& ctx) override;
		void OnAbort(BehaviorContext& ctx) override;
	};

	class WLD_API SelectorNode : public CompositeNode
	{
	public:
		NodeStatus Tick(BehaviorContext& ctx) override;
		void OnAbort(BehaviorContext& ctx) override;
	};

	class WLD_API ParallelNode : public CompositeNode
	{
	public:
		enum class Policy : uint8_t
		{
			RequireOneSuccess = 0,
			RequireAllSuccess = 1
		};
		Policy FinishPolicy = Policy::RequireAllSuccess;

		NodeStatus Tick(BehaviorContext& ctx) override;
	};

	class WLD_API BehaviorDecorator : public BehaviorNode
	{
	public:
		explicit BehaviorDecorator(std::shared_ptr<BehaviorNode> child = nullptr, FlowAbortMode abortMode = FlowAbortMode::None)
			: m_Child(std::move(child)), m_AbortMode(abortMode) {}

		void SetChild(std::shared_ptr<BehaviorNode> child);
		std::shared_ptr<BehaviorNode> GetChild() const { return m_Child; }

		FlowAbortMode GetAbortMode() const { return m_AbortMode; }
		void SetAbortMode(FlowAbortMode mode) { m_AbortMode = mode; }

		virtual bool EvaluateCondition(BehaviorContext& ctx) = 0;

		void Init(BehaviorContext& ctx) override;
		void Shutdown(BehaviorContext& ctx) override;
		NodeStatus Tick(BehaviorContext& ctx) override;
		void OnAbort(BehaviorContext& ctx) override;
		void Reset(BehaviorContext& ctx) override;

	protected:
		std::shared_ptr<BehaviorNode> m_Child;
		FlowAbortMode m_AbortMode = FlowAbortMode::None;
	};

	enum class ConditionOperator : uint8_t
	{
		IsSet = 0,
		IsNotSet = 1,
		Equal = 2,
		NotEqual = 3,
		Greater = 4,
		Less = 5
	};

	class WLD_API BlackboardConditionDecorator : public BehaviorDecorator
	{
	public:
		std::string KeyName;
		ConditionOperator Op = ConditionOperator::IsSet;
		BlackboardValue TargetValue;

		BlackboardConditionDecorator(
			const std::string& keyName,
			ConditionOperator op,
			const BlackboardValue& targetVal = {},
			std::shared_ptr<BehaviorNode> child = nullptr,
			FlowAbortMode abortMode = FlowAbortMode::None)
			: BehaviorDecorator(std::move(child), abortMode)
			, KeyName(keyName)
			, Op(op)
			, TargetValue(targetVal)
		{
		}

		void Init(BehaviorContext& ctx) override;
		void Shutdown(BehaviorContext& ctx) override;
		bool EvaluateCondition(BehaviorContext& ctx) override;
		bool EvaluateConditionOnBoard(const Blackboard& board) const;

	private:
		uint32_t m_ObserverId = 0;
		Blackboard* m_ObservedBoard = nullptr;
		BehaviorTreeComponent* m_BoundComponent = nullptr;
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
		explicit ActionNode(
			std::function<NodeStatus(BehaviorContext& ctx)> action,
			std::function<void(BehaviorContext& ctx)> onAbort = nullptr)
			: m_Action(std::move(action)), m_OnAbort(std::move(onAbort)) {}

		explicit ActionNode(std::function<NodeStatus(float dt)> action)
			: m_Action([a = std::move(action)](BehaviorContext& ctx) { return a ? a(ctx.Dt) : NodeStatus::Failure; }) {}

		explicit ActionNode(std::function<NodeStatus()> action)
			: m_Action([a = std::move(action)](BehaviorContext& ctx) { return a ? a() : NodeStatus::Failure; }) {}

		NodeStatus Tick(BehaviorContext& ctx) override
		{
			return m_Action ? m_Action(ctx) : NodeStatus::Failure;
		}

		void OnAbort(BehaviorContext& ctx) override
		{
			if (m_OnAbort)
				m_OnAbort(ctx);
		}

	private:
		std::function<NodeStatus(BehaviorContext& ctx)> m_Action;
		std::function<void(BehaviorContext& ctx)> m_OnAbort;
	};

	class WLD_API BehaviorTreeRunner
	{
	public:
		explicit BehaviorTreeRunner(std::shared_ptr<BehaviorNode> root = nullptr) : m_Root(std::move(root)) {}
		void SetRoot(std::shared_ptr<BehaviorNode> root) { m_Root = std::move(root); }
		std::shared_ptr<BehaviorNode> GetRoot() const { return m_Root; }

		void Init(BehaviorContext& ctx);
		void Shutdown(BehaviorContext& ctx);
		NodeStatus Tick(BehaviorContext& ctx);
		void Abort(BehaviorContext& ctx);
		void Reset(BehaviorContext& ctx);

	private:
		std::shared_ptr<BehaviorNode> m_Root;
		bool m_Initialized = false;
	};
}