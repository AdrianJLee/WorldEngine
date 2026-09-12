#include "wldpch.h"
#include "World/WUI/WuiUndoStack.h"

namespace World::Wui
{
	void WuiUndoStack::Push(std::string name, std::function<void()> undo, std::function<void()> redo)
	{
		if (m_Index < m_Entries.size())
			m_Entries.resize(m_Index); // 截断重做分支
		m_Entries.push_back({ std::move(name), std::move(undo), std::move(redo) });
		++m_Index;
		if (m_Entries.size() > MaxDepth)
		{
			m_Entries.erase(m_Entries.begin());
			--m_Index;
		}
	}

	bool WuiUndoStack::Undo()
	{
		if (!CanUndo())
			return false;
		m_Entries[m_Index - 1].Undo();
		--m_Index;
		return true;
	}

	bool WuiUndoStack::Redo()
	{
		if (!CanRedo())
			return false;
		m_Entries[m_Index].Redo();
		++m_Index;
		return true;
	}

	void WuiUndoStack::Clear()
	{
		m_Entries.clear();
		m_Index = 0;
	}
}
