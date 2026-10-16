#pragma once

#include "World/Core/Core.h"
#include "World/Scene/Entity.h"
#include "World/Scene/Components.h"

#include <string>
#include <string_view>
#include <memory>
#include <vector>
#include <deque>
#include <cstddef>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

namespace World
{
	// 抽象编辑器命令接口 (IEditorCommand)
	class WLD_API IEditorCommand
	{
	public:
		virtual ~IEditorCommand() = default;
		virtual void Execute() = 0;
		virtual void Undo() = 0;
		virtual void Redo() { Execute(); }
		virtual std::string_view GetName() const = 0;
		virtual size_t GetMemorySize() const { return sizeof(*this); }
	};

	// 批处理复合命令 (CompoundCommand)
	// 支持按正向执行/重做与逆向撤销
	class WLD_API CompoundCommand : public IEditorCommand
	{
	public:
		explicit CompoundCommand(std::string_view name = "Compound Command");
		~CompoundCommand() override = default;

		CompoundCommand(const CompoundCommand&) = delete;
		CompoundCommand& operator=(const CompoundCommand&) = delete;
		CompoundCommand(CompoundCommand&&) noexcept = default;
		CompoundCommand& operator=(CompoundCommand&&) noexcept = default;

		void AddCommand(std::unique_ptr<IEditorCommand> cmd);
		size_t CommandCount() const;
		const IEditorCommand* GetCommand(size_t index) const;

		void Execute() override;
		void Undo() override;
		void Redo() override;
		std::string_view GetName() const override;
		size_t GetMemorySize() const override;

	private:
		std::string m_Name;
		std::vector<std::unique_ptr<IEditorCommand>> m_Commands;
	};

	// 实体变换命令 (EntityTransformCommand)
	// 记录实体变换 (Location, Rotation, Scale) 变更前后状态，Undo 时原子回滚
	class WLD_API EntityTransformCommand : public IEditorCommand
	{
	public:
		EntityTransformCommand(Entity entity,
			const glm::vec3& oldPos, const glm::quat& oldRot, const glm::vec3& oldScale,
			const glm::vec3& newPos, const glm::quat& newRot, const glm::vec3& newScale,
			std::string_view name = "Transform Entity");

		EntityTransformCommand(Entity entity,
			const TransformComponent& oldTransform,
			const TransformComponent& newTransform,
			std::string_view name = "Transform Entity");

		~EntityTransformCommand() override = default;

		void Execute() override;
		void Undo() override;
		void Redo() override;
		std::string_view GetName() const override;
		size_t GetMemorySize() const override;

		Entity GetEntity() const { return m_Entity; }
		const glm::vec3& GetOldPosition() const { return m_OldPosition; }
		const glm::quat& GetOldRotation() const { return m_OldRotation; }
		const glm::vec3& GetOldScale() const { return m_OldScale; }
		const glm::vec3& GetNewPosition() const { return m_NewPosition; }
		const glm::quat& GetNewRotation() const { return m_NewRotation; }
		const glm::vec3& GetNewScale() const { return m_NewScale; }

	private:
		Entity m_Entity;
		glm::vec3 m_OldPosition { 0.0f };
		glm::quat m_OldRotation { 1.0f, 0.0f, 0.0f, 0.0f };
		glm::vec3 m_OldScale { 1.0f };
		glm::vec3 m_NewPosition { 0.0f };
		glm::quat m_NewRotation { 1.0f, 0.0f, 0.0f, 0.0f };
		glm::vec3 m_NewScale { 1.0f };
		std::string m_Name;
	};

	// 编辑器撤销重做历史栈 (CommandHistory)
	class WLD_API CommandHistory
	{
	public:
		explicit CommandHistory(size_t maxDepth = 100);
		~CommandHistory() = default;

		CommandHistory(const CommandHistory&) = delete;
		CommandHistory& operator=(const CommandHistory&) = delete;
		CommandHistory(CommandHistory&&) noexcept = default;
		CommandHistory& operator=(CommandHistory&&) noexcept = default;

		void PushAndExecute(std::unique_ptr<IEditorCommand> cmd);
		void Push(std::unique_ptr<IEditorCommand> cmd);

		bool Undo();
		bool Redo();

		bool CanUndo() const;
		bool CanRedo() const;

		size_t UndoCount() const;
		size_t RedoCount() const;

		void Clear();
		void SetMaxDepth(size_t depth);
		size_t GetMaxDepth() const { return m_MaxDepth; }

		std::string_view GetUndoName() const;
		std::string_view GetRedoName() const;

		void BeginCompound(std::string_view name = "Compound");
		void EndCompound();
		bool IsInCompound() const;

		size_t GetTotalMemoryUsage() const;

	private:
		std::deque<std::unique_ptr<IEditorCommand>> m_UndoStack;
		std::vector<std::unique_ptr<IEditorCommand>> m_RedoStack;
		size_t m_MaxDepth = 100;
		std::unique_ptr<CompoundCommand> m_ActiveCompound;
		int m_CompoundNestingLevel = 0;
	};
}
