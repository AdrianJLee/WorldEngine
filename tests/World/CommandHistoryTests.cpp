#include "World/Command/CommandHistory.h"
#include "World/Scene/Components.h"
#include "World/Scene/Scene.h"
#include "World/Scene/Entity.h"
#include "World/Scene/Hierarchy.h"
#include "World/Core/WorldContext.h"
#include "World/Core/StringPool.h"

#include <cassert>
#include <iostream>
#include <string>

using namespace World;

class MockValueCommand : public IEditorCommand
{
public:
	MockValueCommand(int& target, int delta, std::string_view name = "MockValueCommand")
		: m_Target(target)
		, m_Delta(delta)
		, m_Name(name)
	{
	}

	void Execute() override
	{
		m_Target += m_Delta;
		m_ExecuteCount++;
	}

	void Undo() override
	{
		m_Target -= m_Delta;
		m_UndoCount++;
	}

	std::string_view GetName() const override
	{
		return m_Name;
	}

	size_t GetMemorySize() const override
	{
		return sizeof(*this);
	}

	int GetExecuteCount() const { return m_ExecuteCount; }
	int GetUndoCount() const { return m_UndoCount; }

private:
	int& m_Target;
	int m_Delta;
	std::string m_Name;
	int m_ExecuteCount = 0;
	int m_UndoCount = 0;
};

static void TestPushUndoRedo()
{
	CommandHistory history(10);
	int value = 0;

	assert(!history.CanUndo());
	assert(!history.CanRedo());
	assert(history.UndoCount() == 0);
	assert(history.RedoCount() == 0);

	history.PushAndExecute(std::make_unique<MockValueCommand>(value, 10, "Add10"));
	assert(value == 10);
	assert(history.CanUndo());
	assert(!history.CanRedo());
	assert(history.UndoCount() == 1);
	assert(history.RedoCount() == 0);
	assert(history.GetUndoName() == "Add10");

	history.PushAndExecute(std::make_unique<MockValueCommand>(value, 20, "Add20"));
	assert(value == 30);
	assert(history.UndoCount() == 2);
	assert(history.GetUndoName() == "Add20");

	// Undo Add20
	bool undone = history.Undo();
	assert(undone);
	assert(value == 10);
	assert(history.UndoCount() == 1);
	assert(history.RedoCount() == 1);
	assert(history.CanRedo());
	assert(history.GetRedoName() == "Add20");

	// Undo Add10
	undone = history.Undo();
	assert(undone);
	assert(value == 0);
	assert(history.UndoCount() == 0);
	assert(history.RedoCount() == 2);
	assert(!history.CanUndo());

	// Redo Add10
	bool redone = history.Redo();
	assert(redone);
	assert(value == 10);
	assert(history.UndoCount() == 1);
	assert(history.RedoCount() == 1);

	// Pushing a new command clears redo stack
	history.PushAndExecute(std::make_unique<MockValueCommand>(value, 5, "Add5"));
	assert(value == 15);
	assert(history.UndoCount() == 2);
	assert(history.RedoCount() == 0);
	assert(!history.CanRedo());

	std::cout << "[PASS] TestPushUndoRedo\n";
}

static void TestMaxDepthLimit()
{
	CommandHistory history(3);
	int value = 0;

	for (int i = 1; i <= 5; ++i)
	{
		history.PushAndExecute(std::make_unique<MockValueCommand>(value, 1));
	}

	assert(value == 5);
	assert(history.UndoCount() == 3);

	// Can only undo 3 times
	assert(history.Undo());
	assert(history.Undo());
	assert(history.Undo());
	assert(!history.CanUndo());
	assert(value == 2);

	// Dynamically reduce max depth
	history.Redo();
	history.Redo();
	history.Redo();
	assert(history.UndoCount() == 3);

	history.SetMaxDepth(1);
	assert(history.GetMaxDepth() == 1);
	assert(history.UndoCount() == 1);

	std::cout << "[PASS] TestMaxDepthLimit\n";
}

static void TestCompoundCommand()
{
	CommandHistory history(10);
	int valA = 0;
	int valB = 0;

	history.BeginCompound("BatchTransform");
	assert(history.IsInCompound());

	history.PushAndExecute(std::make_unique<MockValueCommand>(valA, 10, "AddA"));
	history.PushAndExecute(std::make_unique<MockValueCommand>(valB, 20, "AddB"));
	history.PushAndExecute(std::make_unique<MockValueCommand>(valA, 5, "AddA2"));

	assert(valA == 15);
	assert(valB == 20);

	history.EndCompound();
	assert(!history.IsInCompound());

	// Compound acts as single atomic undoable unit
	assert(history.UndoCount() == 1);
	assert(history.GetUndoName() == "BatchTransform");

	bool undone = history.Undo();
	assert(undone);
	assert(valA == 0);
	assert(valB == 0);
	assert(history.CanRedo());

	bool redone = history.Redo();
	assert(redone);
	assert(valA == 15);
	assert(valB == 20);

	std::cout << "[PASS] TestCompoundCommand\n";
}

static void TestEntityTransformCommand()
{
	WorldContext context;
	Scene scene(context);

	Entity entity = Entity::CreateEntity(&scene, "Actor");
	auto& tc = entity.AddComponent<TransformComponent>();
	tc.Location = glm::vec3(0.0f, 0.0f, 0.0f);
	tc.Rotation = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
	tc.Scale = glm::vec3(1.0f, 1.0f, 1.0f);
	tc.ClearDirty();

	CommandHistory history(10);

	glm::vec3 oldPos(0.0f, 0.0f, 0.0f);
	glm::quat oldRot(1.0f, 0.0f, 0.0f, 0.0f);
	glm::vec3 oldScale(1.0f, 1.0f, 1.0f);

	glm::vec3 newPos(12.0f, -4.0f, 8.5f);
	glm::quat newRot(0.7071f, 0.0f, 0.7071f, 0.0f);
	glm::vec3 newScale(2.5f, 2.5f, 2.5f);

	auto cmd = std::make_unique<EntityTransformCommand>(
		entity, oldPos, oldRot, oldScale, newPos, newRot, newScale, "Move Actor");

	history.PushAndExecute(std::move(cmd));

	assert(tc.Location.x == 12.0f);
	assert(tc.Location.y == -4.0f);
	assert(tc.Location.z == 8.5f);
	assert(tc.Scale.x == 2.5f);
	assert(tc.IsDirty());

	// Clear dirty flags before undo to verify Undo sets them again
	tc.ClearDirty();
	assert(!tc.IsDirty());

	// Undo revert
	bool undone = history.Undo();
	assert(undone);
	assert(tc.Location.x == 0.0f);
	assert(tc.Location.y == 0.0f);
	assert(tc.Location.z == 0.0f);
	assert(tc.Scale.x == 1.0f);
	assert(tc.IsDirty());

	// Redo apply
	tc.ClearDirty();
	bool redone = history.Redo();
	assert(redone);
	assert(tc.Location.x == 12.0f);
	assert(tc.Location.y == -4.0f);
	assert(tc.Location.z == 8.5f);
	assert(tc.IsDirty());

	std::cout << "[PASS] TestEntityTransformCommand\n";
}

static void TestEntityCreateCommand()
{
	WorldContext context;
	Scene scene(context);

	CommandHistory history(10);

	// Case A: Create an entity, modify it, push EntityCreateCommand
	Entity entity = Entity::CreateEntity(&scene, "Player");
	auto& tc = entity.AddComponent<TransformComponent>();
	tc.Location = glm::vec3(1.0f, 2.0f, 3.0f);
	UUID playerId = entity.GetComponent<UUIDComponent>().ID;

	auto cmd = std::make_unique<EntityCreateCommand>(entity, "Create Player");
	assert(cmd->GetEntityUUID() == playerId);
	assert(cmd->GetEntityName() == "Player");
	history.Push(std::move(cmd));

	assert(history.CanUndo());
	assert(history.UndoCount() == 1);

	// Undo should destroy the entity from the scene
	bool undone = history.Undo();
	assert(undone);
	assert(!entity.IsValid());

	// Verify entity is gone from registry
	bool found = false;
	for (const auto handle : scene.GetRegistry().view<UUIDComponent>())
	{
		if (scene.GetRegistry().get<UUIDComponent>(handle).ID == playerId)
		{
			found = true;
			break;
		}
	}
	assert(!found);

	// Redo should restore the entity with the exact same UUID and components
	bool redone = history.Redo();
	assert(redone);

	Entity restored;
	for (const auto handle : scene.GetRegistry().view<UUIDComponent>())
	{
		if (scene.GetRegistry().get<UUIDComponent>(handle).ID == playerId)
		{
			restored = Entity(&scene, handle);
			break;
		}
	}
	assert(restored.IsValid());
	assert(restored.GetComponent<UUIDComponent>().ID == playerId);
	assert(StringPool::Get().NameOf(restored.GetComponent<TagComponent>().Tag) == "Player");
	assert(restored.HasComponent<TransformComponent>());
	assert(restored.GetComponent<TransformComponent>().Location.x == 1.0f);
	assert(restored.GetComponent<TransformComponent>().Location.y == 2.0f);
	assert(restored.GetComponent<TransformComponent>().Location.z == 3.0f);

	// Case B: Create via PushAndExecute with constructor (scene, name)
	auto cmd2 = std::make_unique<EntityCreateCommand>(&scene, "Enemy", UUID(0), "Create Enemy");
	UUID enemyId = cmd2->GetEntityUUID();
	history.PushAndExecute(std::move(cmd2));

	Entity enemy;
	for (const auto handle : scene.GetRegistry().view<UUIDComponent>())
	{
		if (scene.GetRegistry().get<UUIDComponent>(handle).ID == enemyId)
		{
			enemy = Entity(&scene, handle);
			break;
		}
	}
	assert(enemy.IsValid());
	assert(StringPool::Get().NameOf(enemy.GetComponent<TagComponent>().Tag) == "Enemy");

	// Consecutive undo-redo cycles
	assert(history.Undo()); // Undo Enemy
	assert(!enemy.IsValid());
	assert(history.Undo()); // Undo Player
	assert(!restored.IsValid());
	assert(history.Redo()); // Redo Player
	assert(history.Redo()); // Redo Enemy

	std::cout << "[PASS] TestEntityCreateCommand\n";
}

static void TestEntityDestroyCommand()
{
	WorldContext context;
	Scene scene(context);

	CommandHistory history(10);

	Entity entity = Entity::CreateEntity(&scene, "Prop");
	auto& tc = entity.AddComponent<TransformComponent>();
	tc.Location = glm::vec3(4.0f, 5.0f, 6.0f);
	UUID propId = entity.GetComponent<UUIDComponent>().ID;

	auto cmd = std::make_unique<EntityDestroyCommand>(entity, "Delete Prop");
	assert(cmd->GetEntityUUID() == propId);
	assert(cmd->GetEntityName() == "Prop");

	// Execute via history
	history.PushAndExecute(std::move(cmd));
	assert(!entity.IsValid());
	assert(history.CanUndo());

	// Verify prop is gone from scene
	bool found = false;
	for (const auto handle : scene.GetRegistry().view<UUIDComponent>())
	{
		if (scene.GetRegistry().get<UUIDComponent>(handle).ID == propId)
		{
			found = true;
			break;
		}
	}
	assert(!found);

	// Undo should resurrect the entity with same UUID and components
	bool undone = history.Undo();
	assert(undone);

	Entity resurrected;
	for (const auto handle : scene.GetRegistry().view<UUIDComponent>())
	{
		if (scene.GetRegistry().get<UUIDComponent>(handle).ID == propId)
		{
			resurrected = Entity(&scene, handle);
			break;
		}
	}
	assert(resurrected.IsValid());
	assert(resurrected.GetComponent<UUIDComponent>().ID == propId);
	assert(StringPool::Get().NameOf(resurrected.GetComponent<TagComponent>().Tag) == "Prop");
	assert(resurrected.HasComponent<TransformComponent>());
	assert(resurrected.GetComponent<TransformComponent>().Location.x == 4.0f);
	assert(resurrected.GetComponent<TransformComponent>().Location.y == 5.0f);
	assert(resurrected.GetComponent<TransformComponent>().Location.z == 6.0f);

	// Redo should destroy again
	bool redone = history.Redo();
	assert(redone);
	assert(!resurrected.IsValid());

	// Undo again to ensure repeatability
	assert(history.Undo());

	std::cout << "[PASS] TestEntityDestroyCommand\n";
}

static void TestEntityReparentCommand()
{
	WorldContext context;
	Scene scene(context);

	CommandHistory history(10);

	Entity rootA = Entity::CreateEntity(&scene, "RootA");
	rootA.AddComponent<TransformComponent>(glm::vec3(10.0f, 0.0f, 0.0f));
	UUID rootAId = rootA.GetComponent<UUIDComponent>().ID;

	Entity rootB = Entity::CreateEntity(&scene, "RootB");
	rootB.AddComponent<TransformComponent>(glm::vec3(20.0f, 0.0f, 0.0f));
	UUID rootBId = rootB.GetComponent<UUIDComponent>().ID;

	Entity child = Entity::CreateEntity(&scene, "Child");
	child.AddComponent<TransformComponent>(glm::vec3(1.0f, 0.0f, 0.0f));
	UUID childId = child.GetComponent<UUIDComponent>().ID;

	// Initially link Child -> RootA
	bool initialParentOk = Hierarchy::SetParent(scene.GetRegistry(),
		static_cast<entt::entity>(child), static_cast<entt::entity>(rootA));
	assert(initialParentOk);
	Hierarchy::UpdateWorldTransforms(scene.GetRegistry());
	assert(Hierarchy::ChildrenOf(scene.GetRegistry(), static_cast<entt::entity>(rootA)).size() == 1);

	// Reparent Child to RootB
	auto cmd = std::make_unique<EntityReparentCommand>(child, rootB, "Reparent Child to RootB");
	assert(cmd->GetEntityUUID() == childId);
	assert(cmd->GetOldParentUUID() == rootAId);
	assert(cmd->GetNewParentUUID() == rootBId);

	history.PushAndExecute(std::move(cmd));

	// Verify Child is now under RootB
	entt::entity currentParent = scene.GetRegistry().get<HierarchyComponent>(static_cast<entt::entity>(child)).Parent;
	assert(currentParent == static_cast<entt::entity>(rootB));
	assert(Hierarchy::ChildrenOf(scene.GetRegistry(), static_cast<entt::entity>(rootA)).empty());
	assert(Hierarchy::ChildrenOf(scene.GetRegistry(), static_cast<entt::entity>(rootB)).size() == 1);

	// Undo reparent -> back to RootA
	bool undone = history.Undo();
	assert(undone);
	currentParent = scene.GetRegistry().get<HierarchyComponent>(static_cast<entt::entity>(child)).Parent;
	assert(currentParent == static_cast<entt::entity>(rootA));
	assert(Hierarchy::ChildrenOf(scene.GetRegistry(), static_cast<entt::entity>(rootA)).size() == 1);
	assert(Hierarchy::ChildrenOf(scene.GetRegistry(), static_cast<entt::entity>(rootB)).empty());

	// Redo reparent -> back to RootB
	bool redone = history.Redo();
	assert(redone);
	currentParent = scene.GetRegistry().get<HierarchyComponent>(static_cast<entt::entity>(child)).Parent;
	assert(currentParent == static_cast<entt::entity>(rootB));

	// Reparent to root (detach)
	auto detachCmd = std::make_unique<EntityReparentCommand>(child, Entity(), "Detach Child");
	assert(detachCmd->GetOldParentUUID() == rootBId);
	assert(static_cast<uint64_t>(detachCmd->GetNewParentUUID()) == 0);

	history.PushAndExecute(std::move(detachCmd));
	currentParent = scene.GetRegistry().get<HierarchyComponent>(static_cast<entt::entity>(child)).Parent;
	assert(currentParent == entt::null);
	assert(Hierarchy::ChildrenOf(scene.GetRegistry(), static_cast<entt::entity>(rootB)).empty());

	// Undo detach -> back under RootB
	assert(history.Undo());
	currentParent = scene.GetRegistry().get<HierarchyComponent>(static_cast<entt::entity>(child)).Parent;
	assert(currentParent == static_cast<entt::entity>(rootB));

	std::cout << "[PASS] TestEntityReparentCommand\n";
}

static void TestCompoundHierarchyUndoRedo()
{
	WorldContext context;
	Scene scene(context);

	CommandHistory history(10);

	history.BeginCompound("Batch Spawn & Reparent");

	Entity p1 = Entity::CreateEntity(&scene, "P1");
	p1.AddComponent<TransformComponent>(glm::vec3(0.0f));
	UUID p1Id = p1.GetComponent<UUIDComponent>().ID;

	Entity c1 = Entity::CreateEntity(&scene, "C1");
	c1.AddComponent<TransformComponent>(glm::vec3(2.0f, 0.0f, 0.0f));
	UUID c1Id = c1.GetComponent<UUIDComponent>().ID;

	history.PushAndExecute(std::make_unique<EntityCreateCommand>(p1));
	history.PushAndExecute(std::make_unique<EntityCreateCommand>(c1));
	history.PushAndExecute(std::make_unique<EntityReparentCommand>(c1, p1));

	history.EndCompound();

	assert(history.UndoCount() == 1);
	assert(Hierarchy::ChildrenOf(scene.GetRegistry(), static_cast<entt::entity>(p1)).size() == 1);

	// Undo compound: reverses in order: reparent undone -> c1 undone -> p1 undone
	bool undone = history.Undo();
	assert(undone);
	assert(!p1.IsValid());
	assert(!c1.IsValid());

	// Redo compound: p1 restored -> c1 restored -> reparented
	bool redone = history.Redo();
	assert(redone);

	Entity restoredP1;
	Entity restoredC1;
	for (const auto handle : scene.GetRegistry().view<UUIDComponent>())
	{
		const UUID& id = scene.GetRegistry().get<UUIDComponent>(handle).ID;
		if (id == p1Id) restoredP1 = Entity(&scene, handle);
		else if (id == c1Id) restoredC1 = Entity(&scene, handle);
	}
	assert(restoredP1.IsValid());
	assert(restoredC1.IsValid());
	assert(scene.GetRegistry().get<HierarchyComponent>(static_cast<entt::entity>(restoredC1)).Parent ==
		static_cast<entt::entity>(restoredP1));

	std::cout << "[PASS] TestCompoundHierarchyUndoRedo\n";
}

static void TestMemoryAndDepthWithEntityCommands()
{
	WorldContext context;
	Scene scene(context);

	CommandHistory history(3);

	for (int i = 0; i < 5; ++i)
	{
		Entity e = Entity::CreateEntity(&scene, "Item" + std::to_string(i));
		e.AddComponent<TransformComponent>(glm::vec3((float)i));
		history.PushAndExecute(std::make_unique<EntityCreateCommand>(e));
	}

	assert(history.UndoCount() == 3);
	assert(history.GetTotalMemoryUsage() > sizeof(CommandHistory));

	// Test Undo drops
	assert(history.Undo());
	assert(history.Undo());
	assert(history.Undo());
	assert(!history.CanUndo());

	// Test Clear
	history.Clear();
	assert(history.UndoCount() == 0);
	assert(history.RedoCount() == 0);

	std::cout << "[PASS] TestMemoryAndDepthWithEntityCommands\n";
}

static void TestClearAndMemory()
{
	CommandHistory history(10);
	int value = 0;

	history.PushAndExecute(std::make_unique<MockValueCommand>(value, 100));
	assert(history.UndoCount() == 1);
	assert(history.GetTotalMemoryUsage() >= sizeof(CommandHistory));

	history.Clear();
	assert(history.UndoCount() == 0);
	assert(history.RedoCount() == 0);
	assert(!history.CanUndo());
	assert(!history.CanRedo());

	std::cout << "[PASS] TestClearAndMemory\n";
}

int main()
{
	std::cout << "=== Running CommandHistoryTests ===" << std::endl;
	TestPushUndoRedo();
	TestMaxDepthLimit();
	TestCompoundCommand();
	TestEntityTransformCommand();
	TestEntityCreateCommand();
	TestEntityDestroyCommand();
	TestEntityReparentCommand();
	TestCompoundHierarchyUndoRedo();
	TestMemoryAndDepthWithEntityCommands();
	TestClearAndMemory();
	std::cout << "=== All CommandHistoryTests PASSED ===" << std::endl;
	return 0;
}
