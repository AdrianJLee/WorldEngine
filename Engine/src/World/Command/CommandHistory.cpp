#include "wldpch.h"
#include "World/Command/CommandHistory.h"
#include "World/Scene/Components.h"

namespace World
{
	// =========================================================================
	// CompoundCommand
	// =========================================================================

	CompoundCommand::CompoundCommand(std::string_view name)
		: m_Name(name)
	{
	}

	void CompoundCommand::AddCommand(std::unique_ptr<IEditorCommand> cmd)
	{
		if (cmd)
		{
			m_Commands.push_back(std::move(cmd));
		}
	}

	size_t CompoundCommand::CommandCount() const
	{
		return m_Commands.size();
	}

	const IEditorCommand* CompoundCommand::GetCommand(size_t index) const
	{
		if (index < m_Commands.size())
		{
			return m_Commands[index].get();
		}
		return nullptr;
	}

	void CompoundCommand::Execute()
	{
		for (auto& cmd : m_Commands)
		{
			if (cmd)
			{
				cmd->Execute();
			}
		}
	}

	void CompoundCommand::Undo()
	{
		// 批处理撤销按严格逆序执行
		for (auto it = m_Commands.rbegin(); it != m_Commands.rend(); ++it)
		{
			if (*it)
			{
				(*it)->Undo();
			}
		}
	}

	void CompoundCommand::Redo()
	{
		// 批处理重做按正序执行
		for (auto& cmd : m_Commands)
		{
			if (cmd)
			{
				cmd->Redo();
			}
		}
	}

	std::string_view CompoundCommand::GetName() const
	{
		return m_Name;
	}

	size_t CompoundCommand::GetMemorySize() const
	{
		size_t total = sizeof(*this) + m_Name.capacity();
		for (const auto& cmd : m_Commands)
		{
			if (cmd)
			{
				total += cmd->GetMemorySize();
			}
		}
		return total;
	}

	// =========================================================================
	// EntityTransformCommand
	// =========================================================================

	EntityTransformCommand::EntityTransformCommand(Entity entity,
		const glm::vec3& oldPos, const glm::quat& oldRot, const glm::vec3& oldScale,
		const glm::vec3& newPos, const glm::quat& newRot, const glm::vec3& newScale,
		std::string_view name)
		: m_Entity(entity)
		, m_OldPosition(oldPos)
		, m_OldRotation(oldRot)
		, m_OldScale(oldScale)
		, m_NewPosition(newPos)
		, m_NewRotation(newRot)
		, m_NewScale(newScale)
		, m_Name(name)
	{
	}

	EntityTransformCommand::EntityTransformCommand(Entity entity,
		const TransformComponent& oldTransform,
		const TransformComponent& newTransform,
		std::string_view name)
		: m_Entity(entity)
		, m_OldPosition(oldTransform.Location)
		, m_OldRotation(oldTransform.Rotation)
		, m_OldScale(oldTransform.Scale)
		, m_NewPosition(newTransform.Location)
		, m_NewRotation(newTransform.Rotation)
		, m_NewScale(newTransform.Scale)
		, m_Name(name)
	{
	}

	void EntityTransformCommand::Execute()
	{
		if (m_Entity.IsValid() && m_Entity.HasComponent<TransformComponent>())
		{
			auto& tc = m_Entity.GetComponent<TransformComponent>();
			tc.Location = m_NewPosition;
			tc.Rotation = m_NewRotation;
			tc.Scale = m_NewScale;
			tc.Flags |= (TransformFlags::DirtyLocal | TransformFlags::DirtyWorld);
		}
	}

	void EntityTransformCommand::Undo()
	{
		if (m_Entity.IsValid() && m_Entity.HasComponent<TransformComponent>())
		{
			auto& tc = m_Entity.GetComponent<TransformComponent>();
			tc.Location = m_OldPosition;
			tc.Rotation = m_OldRotation;
			tc.Scale = m_OldScale;
			tc.Flags |= (TransformFlags::DirtyLocal | TransformFlags::DirtyWorld);
		}
	}

	void EntityTransformCommand::Redo()
	{
		Execute();
	}

	std::string_view EntityTransformCommand::GetName() const
	{
		return m_Name;
	}

	size_t EntityTransformCommand::GetMemorySize() const
	{
		return sizeof(*this) + m_Name.capacity();
	}

	// =========================================================================
	// CommandHistory
	// =========================================================================

	CommandHistory::CommandHistory(size_t maxDepth)
		: m_MaxDepth(maxDepth)
	{
	}

	void CommandHistory::PushAndExecute(std::unique_ptr<IEditorCommand> cmd)
	{
		if (!cmd)
		{
			return;
		}

		if (IsInCompound())
		{
			cmd->Execute();
			m_ActiveCompound->AddCommand(std::move(cmd));
			return;
		}

		cmd->Execute();
		m_RedoStack.clear();
		m_UndoStack.push_back(std::move(cmd));
		while (m_UndoStack.size() > m_MaxDepth && !m_UndoStack.empty())
		{
			m_UndoStack.pop_front();
		}
	}

	void CommandHistory::Push(std::unique_ptr<IEditorCommand> cmd)
	{
		if (!cmd)
		{
			return;
		}

		if (IsInCompound())
		{
			m_ActiveCompound->AddCommand(std::move(cmd));
			return;
		}

		m_RedoStack.clear();
		m_UndoStack.push_back(std::move(cmd));
		while (m_UndoStack.size() > m_MaxDepth && !m_UndoStack.empty())
		{
			m_UndoStack.pop_front();
		}
	}

	bool CommandHistory::Undo()
	{
		if (!CanUndo())
		{
			return false;
		}

		auto cmd = std::move(m_UndoStack.back());
		m_UndoStack.pop_back();

		cmd->Undo();
		m_RedoStack.push_back(std::move(cmd));
		return true;
	}

	bool CommandHistory::Redo()
	{
		if (!CanRedo())
		{
			return false;
		}

		auto cmd = std::move(m_RedoStack.back());
		m_RedoStack.pop_back();

		cmd->Redo();
		m_UndoStack.push_back(std::move(cmd));
		while (m_UndoStack.size() > m_MaxDepth && !m_UndoStack.empty())
		{
			m_UndoStack.pop_front();
		}
		return true;
	}

	bool CommandHistory::CanUndo() const
	{
		return !m_UndoStack.empty() && !IsInCompound();
	}

	bool CommandHistory::CanRedo() const
	{
		return !m_RedoStack.empty() && !IsInCompound();
	}

	size_t CommandHistory::UndoCount() const
	{
		return m_UndoStack.size();
	}

	size_t CommandHistory::RedoCount() const
	{
		return m_RedoStack.size();
	}

	void CommandHistory::Clear()
	{
		m_UndoStack.clear();
		m_RedoStack.clear();
		m_ActiveCompound.reset();
		m_CompoundNestingLevel = 0;
	}

	void CommandHistory::SetMaxDepth(size_t depth)
	{
		m_MaxDepth = depth;
		while (m_UndoStack.size() > m_MaxDepth && !m_UndoStack.empty())
		{
			m_UndoStack.pop_front();
		}
	}

	std::string_view CommandHistory::GetUndoName() const
	{
		if (CanUndo())
		{
			return m_UndoStack.back()->GetName();
		}
		return {};
	}

	std::string_view CommandHistory::GetRedoName() const
	{
		if (CanRedo())
		{
			return m_RedoStack.back()->GetName();
		}
		return {};
	}

	void CommandHistory::BeginCompound(std::string_view name)
	{
		if (m_CompoundNestingLevel == 0)
		{
			m_ActiveCompound = std::make_unique<CompoundCommand>(name);
		}
		m_CompoundNestingLevel++;
	}

	void CommandHistory::EndCompound()
	{
		if (m_CompoundNestingLevel <= 0)
		{
			return;
		}

		m_CompoundNestingLevel--;
		if (m_CompoundNestingLevel == 0 && m_ActiveCompound)
		{
			if (m_ActiveCompound->CommandCount() > 0)
			{
				m_RedoStack.clear();
				m_UndoStack.push_back(std::move(m_ActiveCompound));
				while (m_UndoStack.size() > m_MaxDepth && !m_UndoStack.empty())
				{
					m_UndoStack.pop_front();
				}
			}
			m_ActiveCompound.reset();
		}
	}

	bool CommandHistory::IsInCompound() const
	{
		return m_CompoundNestingLevel > 0;
	}

	size_t CommandHistory::GetTotalMemoryUsage() const
	{
		size_t total = sizeof(*this);
		for (const auto& cmd : m_UndoStack)
		{
			if (cmd)
			{
				total += cmd->GetMemorySize();
			}
		}
		for (const auto& cmd : m_RedoStack)
		{
			if (cmd)
			{
				total += cmd->GetMemorySize();
			}
		}
		if (m_ActiveCompound)
		{
			total += m_ActiveCompound->GetMemorySize();
		}
		return total;
	}
}
