#include "wldpch.h"
#include "World/Command/CommandHistory.h"
#include "World/Scene/Components.h"
#include "World/Scene/Scene.h"
#include "World/Scene/SceneSerializer.h"
#include "World/Scene/Hierarchy.h"
#include "World/Core/StringPool.h"

#include <algorithm>

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
		: m_Scene(entity.IsValid() ? entity.GetScene() : nullptr)
		, m_Entity(entity)
		, m_OldPosition(oldPos)
		, m_OldRotation(oldRot)
		, m_OldScale(oldScale)
		, m_NewPosition(newPos)
		, m_NewRotation(newRot)
		, m_NewScale(newScale)
		, m_Name(name)
	{
		if (entity.IsValid() && entity.HasComponent<UUIDComponent>())
		{
			m_EntityUUID = entity.GetComponent<UUIDComponent>().ID;
		}
	}

	EntityTransformCommand::EntityTransformCommand(Entity entity,
		const TransformComponent& oldTransform,
		const TransformComponent& newTransform,
		std::string_view name)
		: m_Scene(entity.IsValid() ? entity.GetScene() : nullptr)
		, m_Entity(entity)
		, m_OldPosition(oldTransform.Location)
		, m_OldRotation(oldTransform.Rotation)
		, m_OldScale(oldTransform.Scale)
		, m_NewPosition(newTransform.Location)
		, m_NewRotation(newTransform.Rotation)
		, m_NewScale(newTransform.Scale)
		, m_Name(name)
	{
		if (entity.IsValid() && entity.HasComponent<UUIDComponent>())
		{
			m_EntityUUID = entity.GetComponent<UUIDComponent>().ID;
		}
	}

	Entity EntityTransformCommand::ResolveEntity() const
	{
		if (m_Entity.IsValid())
		{
			return m_Entity;
		}
		if (m_Scene && static_cast<uint64_t>(m_EntityUUID) != 0)
		{
			for (const auto handle : m_Scene->GetRegistry().view<UUIDComponent>())
			{
				if (m_Scene->GetRegistry().get<UUIDComponent>(handle).ID == m_EntityUUID)
				{
					m_Entity = Entity(m_Scene, handle);
					return m_Entity;
				}
			}
		}
		return Entity();
	}

	Entity EntityTransformCommand::GetEntity() const
	{
		return ResolveEntity();
	}

	void EntityTransformCommand::Execute()
	{
		Entity entity = ResolveEntity();
		if (entity.IsValid() && entity.HasComponent<TransformComponent>())
		{
			auto& tc = entity.GetComponent<TransformComponent>();
			tc.Location = m_NewPosition;
			tc.Rotation = m_NewRotation;
			tc.Scale = m_NewScale;
			tc.Flags |= (TransformFlags::DirtyLocal | TransformFlags::DirtyWorld);
			m_Entity = entity;
		}
	}

	void EntityTransformCommand::Undo()
	{
		Entity entity = ResolveEntity();
		if (entity.IsValid() && entity.HasComponent<TransformComponent>())
		{
			auto& tc = entity.GetComponent<TransformComponent>();
			tc.Location = m_OldPosition;
			tc.Rotation = m_OldRotation;
			tc.Scale = m_OldScale;
			tc.Flags |= (TransformFlags::DirtyLocal | TransformFlags::DirtyWorld);
			m_Entity = entity;
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
	// EntityCreateCommand
	// =========================================================================

	EntityCreateCommand::EntityCreateCommand(Entity entity, std::string_view name)
		: m_Entity(entity)
		, m_Name(name)
	{
		if (entity.IsValid())
		{
			m_Scene = entity.GetScene();
			if (entity.HasComponent<UUIDComponent>())
			{
				m_EntityUUID = entity.GetComponent<UUIDComponent>().ID;
			}
			if (entity.HasComponent<TagComponent>())
			{
				m_EntityName = StringPool::Get().NameOf(entity.GetComponent<TagComponent>().Tag);
			}
			if (entity.HasComponent<HierarchyComponent>())
			{
				entt::entity parentHandle = entity.GetComponent<HierarchyComponent>().Parent;
				if (parentHandle != entt::null && m_Scene && m_Scene->GetRegistry().valid(parentHandle))
				{
					if (m_Scene->GetRegistry().all_of<UUIDComponent>(parentHandle))
					{
						m_ParentUUID = m_Scene->GetRegistry().get<UUIDComponent>(parentHandle).ID;
					}
				}
			}
			SceneSerializer::SerializeEntityDocumentToYaml(entity, &m_SnapshotYaml);
		}
	}

	EntityCreateCommand::EntityCreateCommand(Scene* scene, const std::string& entityName,
		const UUID& parentUuid, std::string_view name)
		: m_Scene(scene)
		, m_EntityUUID(UUID())
		, m_ParentUUID(parentUuid)
		, m_EntityName(entityName)
		, m_Name(name)
	{
	}

	Entity EntityCreateCommand::ResolveEntity() const
	{
		if (m_Entity.IsValid())
		{
			return m_Entity;
		}
		return ResolveEntityByUUID(m_EntityUUID);
	}

	Entity EntityCreateCommand::ResolveEntityByUUID(const UUID& uuid) const
	{
		if (!m_Scene || static_cast<uint64_t>(uuid) == 0)
		{
			return Entity();
		}
		for (const auto handle : m_Scene->GetRegistry().view<UUIDComponent>())
		{
			if (m_Scene->GetRegistry().get<UUIDComponent>(handle).ID == uuid)
			{
				return Entity(m_Scene, handle);
			}
		}
		return Entity();
	}

	Entity EntityCreateCommand::GetEntity() const
	{
		return ResolveEntity();
	}

	void EntityCreateCommand::Execute()
	{
		if (!m_Scene)
		{
			return;
		}

		Entity live = ResolveEntity();
		if (live.IsValid())
		{
			if (m_SnapshotYaml.empty())
			{
				SceneSerializer::SerializeEntityDocumentToYaml(live, &m_SnapshotYaml);
			}
			m_Entity = live;
			return;
		}

		if (!m_SnapshotYaml.empty())
		{
			std::string error;
			size_t count = SceneSerializer::AppendEntityYaml(*m_Scene, m_SnapshotYaml, &error);
			if (count > 0)
			{
				if (static_cast<uint64_t>(m_ParentUUID) != 0)
				{
					Entity parent = ResolveEntityByUUID(m_ParentUUID);
					Entity child = ResolveEntityByUUID(m_EntityUUID);
					if (parent.IsValid() && child.IsValid())
					{
						Hierarchy::SetParent(m_Scene->GetRegistry(),
							static_cast<entt::entity>(child),
							static_cast<entt::entity>(parent));
					}
				}
				SceneSerializer::FinalizeEntityImport(*m_Scene);
				m_Entity = ResolveEntityByUUID(m_EntityUUID);
			}
		}
		else
		{
			Entity created = Entity::CreateEntity(m_Scene, m_EntityName, m_EntityUUID);
			m_Entity = created;
			if (static_cast<uint64_t>(m_ParentUUID) != 0)
			{
				Entity parent = ResolveEntityByUUID(m_ParentUUID);
				if (parent.IsValid())
				{
					Hierarchy::SetParent(m_Scene->GetRegistry(),
						static_cast<entt::entity>(created),
						static_cast<entt::entity>(parent));
					Hierarchy::UpdateWorldTransforms(m_Scene->GetRegistry());
				}
			}
			SceneSerializer::SerializeEntityDocumentToYaml(created, &m_SnapshotYaml);
		}
	}

	void EntityCreateCommand::Undo()
	{
		if (!m_Scene)
		{
			return;
		}

		Entity live = ResolveEntity();
		if (live.IsValid())
		{
			SceneSerializer::SerializeEntityDocumentToYaml(live, &m_SnapshotYaml);
			Entity::DestroyEntity(m_Scene, live);
			m_Scene->FlushStructuralChanges();
			m_Entity = Entity();
		}
	}

	void EntityCreateCommand::Redo()
	{
		Execute();
	}

	std::string_view EntityCreateCommand::GetName() const
	{
		return m_Name;
	}

	size_t EntityCreateCommand::GetMemorySize() const
	{
		return sizeof(*this) + m_Name.capacity() + m_EntityName.capacity() + m_SnapshotYaml.capacity();
	}

	// =========================================================================
	// EntityDestroyCommand
	// =========================================================================

	EntityDestroyCommand::EntityDestroyCommand(Entity entity, std::string_view name)
		: m_Entity(entity)
		, m_Name(name)
	{
		if (entity.IsValid())
		{
			m_Scene = entity.GetScene();
			if (entity.HasComponent<UUIDComponent>())
			{
				m_EntityUUID = entity.GetComponent<UUIDComponent>().ID;
			}
			if (entity.HasComponent<TagComponent>())
			{
				m_EntityName = StringPool::Get().NameOf(entity.GetComponent<TagComponent>().Tag);
			}
			if (entity.HasComponent<HierarchyComponent>())
			{
				entt::entity parentHandle = entity.GetComponent<HierarchyComponent>().Parent;
				if (parentHandle != entt::null && m_Scene && m_Scene->GetRegistry().valid(parentHandle))
				{
					if (m_Scene->GetRegistry().all_of<UUIDComponent>(parentHandle))
					{
						m_ParentUUID = m_Scene->GetRegistry().get<UUIDComponent>(parentHandle).ID;
					}
				}
			}
			SceneSerializer::SerializeEntityDocumentToYaml(entity, &m_SnapshotYaml);
		}
	}

	Entity EntityDestroyCommand::ResolveEntity() const
	{
		if (m_Entity.IsValid())
		{
			return m_Entity;
		}
		return ResolveEntityByUUID(m_EntityUUID);
	}

	Entity EntityDestroyCommand::ResolveEntityByUUID(const UUID& uuid) const
	{
		if (!m_Scene || static_cast<uint64_t>(uuid) == 0)
		{
			return Entity();
		}
		for (const auto handle : m_Scene->GetRegistry().view<UUIDComponent>())
		{
			if (m_Scene->GetRegistry().get<UUIDComponent>(handle).ID == uuid)
			{
				return Entity(m_Scene, handle);
			}
		}
		return Entity();
	}

	Entity EntityDestroyCommand::GetEntity() const
	{
		return ResolveEntity();
	}

	void EntityDestroyCommand::Execute()
	{
		if (!m_Scene)
		{
			return;
		}

		Entity live = ResolveEntity();
		if (live.IsValid())
		{
			if (m_SnapshotYaml.empty())
			{
				SceneSerializer::SerializeEntityDocumentToYaml(live, &m_SnapshotYaml);
			}
			Entity::DestroyEntity(m_Scene, live);
			m_Scene->FlushStructuralChanges();
			m_Entity = Entity();
		}
	}

	void EntityDestroyCommand::Undo()
	{
		if (!m_Scene || m_SnapshotYaml.empty())
		{
			return;
		}

		Entity existing = ResolveEntityByUUID(m_EntityUUID);
		if (existing.IsValid())
		{
			m_Entity = existing;
			return;
		}

		std::string error;
		size_t count = SceneSerializer::AppendEntityYaml(*m_Scene, m_SnapshotYaml, &error);
		if (count > 0)
		{
			if (static_cast<uint64_t>(m_ParentUUID) != 0)
			{
				Entity parent = ResolveEntityByUUID(m_ParentUUID);
				Entity child = ResolveEntityByUUID(m_EntityUUID);
				if (parent.IsValid() && child.IsValid())
				{
					Hierarchy::SetParent(m_Scene->GetRegistry(),
						static_cast<entt::entity>(child),
						static_cast<entt::entity>(parent));
				}
			}
			SceneSerializer::FinalizeEntityImport(*m_Scene);
			m_Entity = ResolveEntityByUUID(m_EntityUUID);
		}
	}

	void EntityDestroyCommand::Redo()
	{
		Execute();
	}

	std::string_view EntityDestroyCommand::GetName() const
	{
		return m_Name;
	}

	size_t EntityDestroyCommand::GetMemorySize() const
	{
		return sizeof(*this) + m_Name.capacity() + m_EntityName.capacity() + m_SnapshotYaml.capacity();
	}

	// =========================================================================
	// EntityReparentCommand
	// =========================================================================

	EntityReparentCommand::EntityReparentCommand(Entity entity, Entity newParent, std::string_view name)
		: m_Entity(entity)
		, m_Name(name)
	{
		if (entity.IsValid())
		{
			m_Scene = entity.GetScene();
			if (entity.HasComponent<UUIDComponent>())
			{
				m_EntityUUID = entity.GetComponent<UUIDComponent>().ID;
			}
			if (entity.HasComponent<HierarchyComponent>())
			{
				entt::entity oldParentHandle = entity.GetComponent<HierarchyComponent>().Parent;
				if (oldParentHandle != entt::null && m_Scene && m_Scene->GetRegistry().valid(oldParentHandle))
				{
					if (m_Scene->GetRegistry().all_of<UUIDComponent>(oldParentHandle))
					{
						m_OldParentUUID = m_Scene->GetRegistry().get<UUIDComponent>(oldParentHandle).ID;
					}
				}
			}
		}

		if (newParent.IsValid())
		{
			if (!m_Scene)
			{
				m_Scene = newParent.GetScene();
			}
			if (newParent.HasComponent<UUIDComponent>())
			{
				m_NewParentUUID = newParent.GetComponent<UUIDComponent>().ID;
			}
		}
	}

	EntityReparentCommand::EntityReparentCommand(Entity entity, const UUID& newParentUuid, std::string_view name)
		: m_Entity(entity)
		, m_NewParentUUID(newParentUuid)
		, m_Name(name)
	{
		if (entity.IsValid())
		{
			m_Scene = entity.GetScene();
			if (entity.HasComponent<UUIDComponent>())
			{
				m_EntityUUID = entity.GetComponent<UUIDComponent>().ID;
			}
			if (entity.HasComponent<HierarchyComponent>())
			{
				entt::entity oldParentHandle = entity.GetComponent<HierarchyComponent>().Parent;
				if (oldParentHandle != entt::null && m_Scene && m_Scene->GetRegistry().valid(oldParentHandle))
				{
					if (m_Scene->GetRegistry().all_of<UUIDComponent>(oldParentHandle))
					{
						m_OldParentUUID = m_Scene->GetRegistry().get<UUIDComponent>(oldParentHandle).ID;
					}
				}
			}
		}
	}

	EntityReparentCommand::EntityReparentCommand(Scene* scene, const UUID& entityUuid, const UUID& newParentUuid, std::string_view name)
		: m_Scene(scene)
		, m_EntityUUID(entityUuid)
		, m_NewParentUUID(newParentUuid)
		, m_Name(name)
	{
		if (m_Scene && static_cast<uint64_t>(m_EntityUUID) != 0)
		{
			Entity ent = ResolveEntityByUUID(m_EntityUUID);
			if (ent.IsValid() && ent.HasComponent<HierarchyComponent>())
			{
				entt::entity oldParentHandle = ent.GetComponent<HierarchyComponent>().Parent;
				if (oldParentHandle != entt::null && m_Scene && m_Scene->GetRegistry().valid(oldParentHandle))
				{
					if (m_Scene->GetRegistry().all_of<UUIDComponent>(oldParentHandle))
					{
						m_OldParentUUID = m_Scene->GetRegistry().get<UUIDComponent>(oldParentHandle).ID;
					}
				}
			}
		}
	}

	Entity EntityReparentCommand::ResolveEntity() const
	{
		if (m_Entity.IsValid())
		{
			return m_Entity;
		}
		return ResolveEntityByUUID(m_EntityUUID);
	}

	Entity EntityReparentCommand::ResolveEntityByUUID(const UUID& uuid) const
	{
		if (!m_Scene || static_cast<uint64_t>(uuid) == 0)
		{
			return Entity();
		}
		for (const auto handle : m_Scene->GetRegistry().view<UUIDComponent>())
		{
			if (m_Scene->GetRegistry().get<UUIDComponent>(handle).ID == uuid)
			{
				return Entity(m_Scene, handle);
			}
		}
		return Entity();
	}

	Entity EntityReparentCommand::GetEntity() const
	{
		return ResolveEntity();
	}

	bool EntityReparentCommand::ApplyParent(const UUID& parentUuid)
	{
		Entity child = ResolveEntity();
		if (!child.IsValid() || !m_Scene)
		{
			return false;
		}

		entt::registry& registry = m_Scene->GetRegistry();
		entt::entity childHandle = static_cast<entt::entity>(child);

		if (static_cast<uint64_t>(parentUuid) == 0)
		{
			Hierarchy::ClearParent(registry, childHandle);
			Hierarchy::UpdateWorldTransforms(registry);
			if (child.HasComponent<TransformComponent>())
			{
				child.GetComponent<TransformComponent>().SetDirty();
			}
			m_Entity = child;
			return true;
		}

		Entity parent = ResolveEntityByUUID(parentUuid);
		if (!parent.IsValid())
		{
			return false;
		}

		entt::entity parentHandle = static_cast<entt::entity>(parent);
		bool ok = Hierarchy::SetParent(registry, childHandle, parentHandle);
		if (ok)
		{
			Hierarchy::UpdateWorldTransforms(registry);
			if (child.HasComponent<TransformComponent>())
			{
				child.GetComponent<TransformComponent>().SetDirty();
			}
			m_Entity = child;
		}
		return ok;
	}

	void EntityReparentCommand::Execute()
	{
		ApplyParent(m_NewParentUUID);
	}

	void EntityReparentCommand::Undo()
	{
		ApplyParent(m_OldParentUUID);
	}

	void EntityReparentCommand::Redo()
	{
		Execute();
	}

	std::string_view EntityReparentCommand::GetName() const
	{
		return m_Name;
	}

	size_t EntityReparentCommand::GetMemorySize() const
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

