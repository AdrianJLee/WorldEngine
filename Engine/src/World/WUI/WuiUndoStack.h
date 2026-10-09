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
		// 名字只在 CanUndo()/CanRedo() 为真时有意义;越界时返回同一份空名,而**不是**
		// `m_Entries[SIZE_MAX]` —— 取名字的 getter 没有让进程弹断言框的权力。
		// 2026-10-09:编辑器 shell 曾在 `Undo()` **之后**才取名字,退到栈底时命中越界,
		// Debug 下弹 "vector subscript out of range"(用户口径的"Ctrl+Z 崩溃")。
		const std::string& UndoName() const { return CanUndo() ? m_Entries[m_Index - 1].Name : EmptyName(); }
		const std::string& RedoName() const { return CanRedo() ? m_Entries[m_Index].Name : EmptyName(); }
		bool Undo();
		bool Redo();
		void Clear();
		size_t Depth() const { return m_Entries.size(); }

	private:
		static const std::string& EmptyName();
		std::vector<Entry> m_Entries;
		size_t m_Index = 0;
	};
}
