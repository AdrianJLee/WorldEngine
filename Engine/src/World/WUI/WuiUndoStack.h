#pragma once

#include <cstddef>
#include <functional>
#include <string>
#include <vector>

namespace World::Wui
{
	// 通用撤销栈:每个条目携带 Undo/Redo 闭包,新 Push 截断重做分支。
	class WuiUndoStack
	{
	public:
		static constexpr size_t MaxDepth = 64;

		struct Entry
		{
			std::string Name;
			std::function<void()> Undo;
			std::function<void()> Redo;
		};

		void Push(std::string name, std::function<void()> undo, std::function<void()> redo);
		bool CanUndo() const { return m_Index > 0; }
		bool CanRedo() const { return m_Index < m_Entries.size(); }
		const std::string& UndoName() const { return m_Entries[m_Index - 1].Name; }
		const std::string& RedoName() const { return m_Entries[m_Index].Name; }
		bool Undo();
		bool Redo();
		void Clear();
		size_t Depth() const { return m_Entries.size(); }

	private:
		std::vector<Entry> m_Entries;
		size_t m_Index = 0;
	};
}
