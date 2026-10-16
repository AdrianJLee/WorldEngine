#include "World/Command/CommandHistory.h"
#include "World/Scene/Components.h"
#include "World/Scene/Scene.h"
#include "World/Scene/Entity.h"
#include "World/Core/WorldContext.h"

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

	// Undo revert
	bool undone = history.Undo();
	assert(undone);
	assert(tc.Location.x == 0.0f);
	assert(tc.Location.y == 0.0f);
	assert(tc.Location.z == 0.0f);
	assert(tc.Scale.x == 1.0f);

	// Redo apply
	bool redone = history.Redo();
	assert(redone);
	assert(tc.Location.x == 12.0f);
	assert(tc.Location.y == -4.0f);
	assert(tc.Location.z == 8.5f);

	std::cout << "[PASS] TestEntityTransformCommand\n";
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
	TestClearAndMemory();
	std::cout << "=== All CommandHistoryTests PASSED ===" << std::endl;
	return 0;
}
