#include "wldpch.h"
#include "World/WUI/WuiTextBuffer.h"

#include <algorithm>

namespace World::Wui
{
	namespace
	{
		size_t CodepointLength(unsigned char lead)
		{
			if (lead >= 0xF0)
				return 4;
			if (lead >= 0xE0)
				return 3;
			if (lead >= 0xC0)
				return 2;
			return 1;
		}

		uint32_t DecodeCodepoint(const std::string& text, size_t offset, size_t& length)
		{
			const unsigned char lead = static_cast<unsigned char>(text[offset]);
			length = CodepointLength(lead);
			if (offset + length > text.size())
			{
				length = 1;
				return 0xFFFD;
			}
			uint32_t codepoint = 0;
			switch (length)
			{
				case 1: codepoint = lead; break;
				case 2: codepoint = lead & 0x1Fu; break;
				case 3: codepoint = lead & 0x0Fu; break;
				default: codepoint = lead & 0x07u; break;
			}
			for (size_t i = 1; i < length; ++i)
				codepoint = (codepoint << 6) | (static_cast<unsigned char>(text[offset + i]) & 0x3Fu);
			return codepoint;
		}

		// 词边界:CJK 与其他非 ASCII 一律算词内字符(与 Ctrl+←/→ 的常见行为一致)。
		bool IsWordCodepoint(uint32_t codepoint)
		{
			return codepoint >= 0x80
				|| (codepoint >= '0' && codepoint <= '9')
				|| (codepoint >= 'A' && codepoint <= 'Z')
				|| (codepoint >= 'a' && codepoint <= 'z')
				|| codepoint == '_';
		}
	}

	size_t WuiTextBuffer::PrevCodepointBoundary(const std::string& text, size_t offset)
	{
		if (offset == 0)
			return 0;
		if (offset > text.size())
			offset = text.size();
		--offset;
		while (offset > 0 && (static_cast<unsigned char>(text[offset]) & 0xC0) == 0x80)
			--offset;
		return offset;
	}

	size_t WuiTextBuffer::NextCodepointBoundary(const std::string& text, size_t offset)
	{
		if (offset >= text.size())
			return text.size();
		size_t length = CodepointLength(static_cast<unsigned char>(text[offset]));
		if (offset + length > text.size())
			length = 1;
		return offset + length;
	}

	void WuiTextBuffer::SetText(std::string text)
	{
		m_Text = std::move(text);
		m_Caret = 0;
		m_Anchor = 0;
		m_DesiredColumn = -1;
		m_Undo.clear();
		m_Redo.clear();
		m_UndoBytes = 0;
		m_UndoOpen = false;
		RebuildLines();
		++m_Revision;
		m_CurrentState = ++m_NextState;
		m_SavedState = m_CurrentState;
	}

	void WuiTextBuffer::RebuildLines()
	{
		m_LineStarts.clear();
		m_LineStarts.push_back(0);
		for (size_t i = 0; i < m_Text.size(); ++i)
			if (m_Text[i] == '\n')
				m_LineStarts.push_back(i + 1);
	}

	size_t WuiTextBuffer::ClampOffset(size_t offset) const
	{
		if (offset >= m_Text.size())
			return m_Text.size();
		while (offset > 0 && (static_cast<unsigned char>(m_Text[offset]) & 0xC0) == 0x80)
			--offset;
		return offset;
	}

	std::pair<size_t, size_t> WuiTextBuffer::LineRange(int line) const
	{
		if (m_LineStarts.empty())
			return { 0, 0 };
		line = std::max(0, std::min(line, LineCount() - 1));
		const size_t start = m_LineStarts[static_cast<size_t>(line)];
		size_t end = (line + 1 < LineCount()) ? m_LineStarts[static_cast<size_t>(line) + 1] : m_Text.size();
		if (end > start && m_Text[end - 1] == '\n')
			--end;
		return { start, end };
	}

	int WuiTextBuffer::LineOfOffset(size_t offset) const
	{
		offset = std::min(offset, m_Text.size());
		const auto it = std::upper_bound(m_LineStarts.begin(), m_LineStarts.end(), offset);
		const ptrdiff_t index = it - m_LineStarts.begin() - 1;
		return static_cast<int>(std::max<ptrdiff_t>(0, index));
	}

	int WuiTextBuffer::AdvanceColumn(int column, uint32_t codepoint) const
	{
		if (codepoint == '\t')
			// W9 review:后端按"固定 4 空格宽"度量 tab(无列上下文),这里必须同口径,
			// 否则含 tab 的行列号/上下移动与渲染不一致。
			return column + 4;
		if (codepoint == '\r')
			return column;
		return column + 1;
	}

	int WuiTextBuffer::VisualColumn(size_t offset) const
	{
		const int line = LineOfOffset(offset);
		const auto [start, end] = LineRange(line);
		const size_t stop = std::min(offset, end);
		int column = 0;
		for (size_t i = start; i < stop;)
		{
			size_t length = 1;
			const uint32_t codepoint = DecodeCodepoint(m_Text, i, length);
			column = AdvanceColumn(column, codepoint);
			i += length;
		}
		return column;
	}

	int WuiTextBuffer::ColumnOfOffset(size_t offset) const
	{
		return VisualColumn(std::min(offset, m_Text.size()));
	}

	size_t WuiTextBuffer::OffsetAtColumn(int line, int column) const
	{
		const auto [start, end] = LineRange(line);
		int current = 0;
		size_t offset = start;
		while (offset < end)
		{
			size_t length = 1;
			const uint32_t codepoint = DecodeCodepoint(m_Text, offset, length);
			const int next = AdvanceColumn(current, codepoint);
			if (next > column)
				break;
			current = next;
			offset += length;
		}
		return offset;
	}

	std::pair<size_t, size_t> WuiTextBuffer::Selection() const
	{
		return m_Anchor <= m_Caret ? std::make_pair(m_Anchor, m_Caret) : std::make_pair(m_Caret, m_Anchor);
	}

	void WuiTextBuffer::SetCaret(size_t offset, bool keepAnchor)
	{
		m_Caret = ClampOffset(offset);
		if (!keepAnchor)
			m_Anchor = m_Caret;
		m_DesiredColumn = -1;
		BreakUndoGroup();
	}

	void WuiTextBuffer::SelectAll()
	{
		m_Anchor = 0;
		m_Caret = m_Text.size();
		m_DesiredColumn = -1;
		BreakUndoGroup();
	}

	bool WuiTextBuffer::Copy(std::string& out) const
	{
		if (!HasSelection())
			return false;
		const auto [start, end] = Selection();
		out.assign(m_Text, start, end - start);
		return true;
	}

	bool WuiTextBuffer::Cut(std::string& out)
	{
		if (!HasSelection())
			return false;
		const auto [start, end] = Selection();
		out.assign(m_Text, start, end - start);
		CommitEdit(EditKind::Replace, start, out, std::string(), start, start);
		return true;
	}

	bool WuiTextBuffer::Paste(std::string_view text)
	{
		if (text.empty())
			return false;
		const auto [selStart, selEnd] = Selection();
		const size_t offset = HasSelection() ? selStart : m_Caret;
		std::string erased;
		if (HasSelection())
			erased.assign(m_Text, selStart, selEnd - selStart);
		const size_t caretAfter = offset + text.size();
		CommitEdit(EditKind::Replace, offset, std::move(erased), std::string(text), caretAfter, caretAfter);
		return true;
	}

	void WuiTextBuffer::InsertAtCaret(std::string_view text)
	{
		if (text.empty())
			return;
		if (HasSelection())
		{
			const auto [selStart, selEnd] = Selection();
			const size_t caretAfter = selStart + text.size();
			CommitEdit(EditKind::Replace, selStart, m_Text.substr(selStart, selEnd - selStart), std::string(text),
				caretAfter, caretAfter);
			return;
		}
		const size_t caretAfter = m_Caret + text.size();
		CommitEdit(EditKind::Insert, m_Caret, std::string(), std::string(text), caretAfter, caretAfter);
	}

	void WuiTextBuffer::Backspace()
	{
		if (HasSelection())
		{
			const auto [selStart, selEnd] = Selection();
			CommitEdit(EditKind::Replace, selStart, m_Text.substr(selStart, selEnd - selStart), std::string(),
				selStart, selStart);
			return;
		}
		if (m_Caret == 0)
			return;
		const size_t start = PrevCodepointBoundary(m_Text, m_Caret);
		CommitEdit(EditKind::Backspace, start, m_Text.substr(start, m_Caret - start), std::string(), start, start);
	}

	void WuiTextBuffer::DeleteForward()
	{
		if (HasSelection())
		{
			const auto [selStart, selEnd] = Selection();
			CommitEdit(EditKind::Replace, selStart, m_Text.substr(selStart, selEnd - selStart), std::string(),
				selStart, selStart);
			return;
		}
		if (m_Caret >= m_Text.size())
			return;
		const size_t end = NextCodepointBoundary(m_Text, m_Caret);
		CommitEdit(EditKind::Delete, m_Caret, m_Text.substr(m_Caret, end - m_Caret), std::string(), m_Caret, m_Caret);
	}

	std::string WuiTextBuffer::LineTerminatorAt(size_t offset) const
	{
		const int line = LineOfOffset(offset);
		const auto [start, end] = LineRange(line);
		const bool crlf = end > start && m_Text[end - 1] == '\r' && end < m_Text.size() && m_Text[end] == '\n';
		// caret 卡在 '\r' 与 '\n' 之间:只插 '\n',否则会造出 "\r\r\n"。
		if (crlf && offset == end)
			return "\n";
		return crlf ? "\r\n" : "\n";
	}

	void WuiTextBuffer::InsertNewline()
	{
		if (HasSelection())
		{
			const auto [selStart, selEnd] = Selection();
			std::string inserted = LineTerminatorAt(selStart);
			const size_t caretAfter = selStart + inserted.size();
			CommitEdit(EditKind::Replace, selStart, m_Text.substr(selStart, selEnd - selStart), std::move(inserted),
				caretAfter, caretAfter);
			return;
		}
		const int line = LineOfOffset(m_Caret);
		const auto [start, end] = LineRange(line);
		std::string inserted = LineTerminatorAt(m_Caret);
		// 自动缩进:复制当前行前导空白;caret 落在缩进中间时只复制到 caret。
		size_t indentEnd = start;
		while (indentEnd < end && (m_Text[indentEnd] == ' ' || m_Text[indentEnd] == '\t'))
			++indentEnd;
		if (indentEnd > m_Caret)
			indentEnd = m_Caret;
		inserted.append(m_Text, start, indentEnd - start);
		const size_t caretAfter = m_Caret + inserted.size();
		CommitEdit(EditKind::Replace, m_Caret, std::string(), std::move(inserted), caretAfter, caretAfter);
	}

	void WuiTextBuffer::IndentSelection(bool outdent)
	{
		const auto [selStart, selEnd] = Selection();
		const int firstLine = LineOfOffset(selStart);
		int lastLine = LineOfOffset(selEnd);
		const bool multiLine = selEnd > selStart && lastLine > firstLine;
		// 选区正好停在行首时,该行不参与缩进(常见编辑器行为)。
		if (multiLine && selEnd == LineRange(lastLine).first)
			--lastLine;

		if (!multiLine)
		{
			if (!outdent)
			{
				InsertAtCaret("    ");
				return;
			}
			const auto [start, end] = LineRange(firstLine);
			size_t removeEnd = start;
			int removed = 0;
			while (removeEnd < end && removed < 4 && m_Text[removeEnd] == ' ')
			{
				++removeEnd;
				++removed;
			}
			if (removed == 0 && removeEnd < end && m_Text[removeEnd] == '\t')
				++removeEnd;
			if (removeEnd == start)
				return;
			const size_t shift = removeEnd - start;
			const size_t caretAfter = m_Caret > removeEnd ? m_Caret - shift : start;
			const size_t anchorAfter = m_Anchor > removeEnd ? m_Anchor - shift : start;
			CommitEdit(EditKind::Replace, start, m_Text.substr(start, shift), std::string(), caretAfter, anchorAfter);
			return;
		}

		// 多行:整块替换成一步(单次撤销)。行内容含尾部 '\r',用 '\n' 重新拼接后
		// CRLF 原样保留。
		const size_t blockStart = LineRange(firstLine).first;
		const size_t blockEnd = LineRange(lastLine).second;
		std::string transformed;
		transformed.reserve(blockEnd - blockStart + static_cast<size_t>(lastLine - firstLine + 1) * 4);
		for (int line = firstLine; line <= lastLine; ++line)
		{
			const auto [lineStart, lineEnd] = LineRange(line);
			std::string_view content(m_Text.data() + lineStart, lineEnd - lineStart);
			if (outdent)
			{
				size_t remove = 0;
				while (remove < content.size() && remove < 4 && content[remove] == ' ')
					++remove;
				if (remove == 0 && !content.empty() && content[0] == '\t')
					remove = 1;
				transformed.append(content.substr(remove));
			}
			else
			{
				transformed.append("    ");
				transformed.append(content);
			}
			if (line < lastLine)
				transformed.push_back('\n');
		}
		const size_t caretAfter = blockStart + transformed.size();
		CommitEdit(EditKind::Replace, blockStart, m_Text.substr(blockStart, blockEnd - blockStart),
			std::move(transformed), caretAfter, blockStart);
	}

	size_t WuiTextBuffer::PrevWordBoundary(size_t offset) const
	{
		size_t i = ClampOffset(offset);
		while (i > 0)
		{
			const size_t prev = PrevCodepointBoundary(m_Text, i);
			size_t length = 1;
			const uint32_t codepoint = DecodeCodepoint(m_Text, prev, length);
			if (IsWordCodepoint(codepoint))
				break;
			i = prev;
		}
		while (i > 0)
		{
			const size_t prev = PrevCodepointBoundary(m_Text, i);
			size_t length = 1;
			const uint32_t codepoint = DecodeCodepoint(m_Text, prev, length);
			if (!IsWordCodepoint(codepoint))
				break;
			i = prev;
		}
		return i;
	}

	size_t WuiTextBuffer::NextWordBoundary(size_t offset) const
	{
		size_t i = ClampOffset(offset);
		while (i < m_Text.size())
		{
			size_t length = 1;
			const uint32_t codepoint = DecodeCodepoint(m_Text, i, length);
			if (!IsWordCodepoint(codepoint))
				break;
			i += length;
		}
		while (i < m_Text.size())
		{
			size_t length = 1;
			const uint32_t codepoint = DecodeCodepoint(m_Text, i, length);
			if (IsWordCodepoint(codepoint))
				break;
			i += length;
		}
		return i;
	}

	void WuiTextBuffer::ApplyCaret(size_t offset, bool extend)
	{
		const size_t target = ClampOffset(offset);
		if (extend)
		{
			m_Caret = target;
		}
		else
		{
			m_Caret = target;
			m_Anchor = target;
		}
		m_DesiredColumn = -1;
		BreakUndoGroup();
	}

	void WuiTextBuffer::MoveVertical(int deltaLines, bool extend)
	{
		if (m_DesiredColumn < 0)
			m_DesiredColumn = VisualColumn(m_Caret);
		const int line = LineOfOffset(m_Caret);
		const int targetLine = std::max(0, std::min(LineCount() - 1, line + deltaLines));
		const size_t target = OffsetAtColumn(targetLine, m_DesiredColumn);
		if (extend)
			m_Caret = target;
		else
		{
			m_Caret = target;
			m_Anchor = target;
		}
		BreakUndoGroup();
	}

	void WuiTextBuffer::MoveCaret(Motion motion, bool extend, int pageLines)
	{
		switch (motion)
		{
			case Motion::Up:
				MoveVertical(-1, extend);
				return;
			case Motion::Down:
				MoveVertical(1, extend);
				return;
			case Motion::PageUp:
				MoveVertical(-std::max(1, pageLines), extend);
				return;
			case Motion::PageDown:
				MoveVertical(std::max(1, pageLines), extend);
				return;
			default:
				break;
		}

		size_t target = m_Caret;
		switch (motion)
		{
			case Motion::Left: target = PrevCodepointBoundary(m_Text, m_Caret); break;
			case Motion::Right: target = NextCodepointBoundary(m_Text, m_Caret); break;
			case Motion::WordLeft: target = PrevWordBoundary(m_Caret); break;
			case Motion::WordRight: target = NextWordBoundary(m_Caret); break;
			case Motion::LineStart: target = LineRange(LineOfOffset(m_Caret)).first; break;
			case Motion::LineEnd: target = LineRange(LineOfOffset(m_Caret)).second; break;
			case Motion::DocStart: target = 0; break;
			case Motion::DocEnd: target = m_Text.size(); break;
			default: break;
		}
		ApplyCaret(target, extend);
	}

	void WuiTextBuffer::CommitEdit(EditKind kind, size_t offset, std::string erased, std::string inserted,
		size_t caretAfter, size_t anchorAfter)
	{
		Edit entry;
		entry.Kind = kind;
		entry.Offset = offset;
		entry.Erased = std::move(erased);
		entry.Inserted = std::move(inserted);
		entry.CaretBefore = m_Caret;
		entry.AnchorBefore = m_Anchor;
		entry.CaretAfter = caretAfter;
		entry.AnchorAfter = anchorAfter;
		entry.StateBefore = m_CurrentState;

		m_Text.replace(offset, entry.Erased.size(), entry.Inserted);
		RebuildLines();
		m_Caret = ClampOffset(caretAfter);
		m_Anchor = ClampOffset(anchorAfter);
		m_DesiredColumn = -1;
		++m_Revision;
		m_CurrentState = ++m_NextState;
		entry.StateAfter = m_CurrentState;
		RecordUndo(std::move(entry));
	}

	void WuiTextBuffer::RecordUndo(Edit entry)
	{
		const EditKind editKind = entry.Kind;
		bool merged = false;
		if (m_UndoOpen && !m_Undo.empty() && m_Undo.back().Kind == entry.Kind)
		{
			Edit& last = m_Undo.back();
			switch (entry.Kind)
			{
				case EditKind::Insert:
					if (entry.Offset == last.Offset + last.Inserted.size())
					{
						last.Inserted += entry.Inserted;
						merged = true;
					}
					break;
				case EditKind::Backspace:
					if (entry.Offset + entry.Erased.size() == last.Offset)
					{
						last.Offset = entry.Offset;
						last.Erased.insert(0, entry.Erased);
						merged = true;
					}
					break;
				case EditKind::Delete:
					if (entry.Offset == last.Offset)
					{
						last.Erased += entry.Erased;
						merged = true;
					}
					break;
				default:
					break;
			}
			if (merged)
			{
				last.CaretAfter = entry.CaretAfter;
				last.AnchorAfter = entry.AnchorAfter;
				last.StateAfter = entry.StateAfter;
			}
		}
		m_UndoBytes += entry.Erased.size() + entry.Inserted.size();
		if (!merged)
			m_Undo.push_back(std::move(entry));
		m_Redo.clear();
		m_UndoOpen = editKind == EditKind::Insert || editKind == EditKind::Backspace || editKind == EditKind::Delete;
		TrimHistory();
	}

	void WuiTextBuffer::TrimHistory()
	{
		while (m_Undo.size() > static_cast<size_t>(kUndoStepLimit)
			|| (m_UndoBytes > kUndoByteLimit && m_Undo.size() > 1))
		{
			m_UndoBytes -= m_Undo.front().Erased.size() + m_Undo.front().Inserted.size();
			m_Undo.erase(m_Undo.begin());
		}
	}

	bool WuiTextBuffer::Undo()
	{
		if (m_Undo.empty())
			return false;
		Edit entry = std::move(m_Undo.back());
		m_Undo.pop_back();
		m_UndoBytes -= entry.Erased.size() + entry.Inserted.size();
		m_Text.replace(entry.Offset, entry.Inserted.size(), entry.Erased);
		RebuildLines();
		m_Caret = ClampOffset(entry.CaretBefore);
		m_Anchor = ClampOffset(entry.AnchorBefore);
		m_CurrentState = entry.StateBefore;
		m_DesiredColumn = -1;
		m_UndoOpen = false;
		++m_Revision;
		m_Redo.push_back(std::move(entry));
		return true;
	}

	bool WuiTextBuffer::Redo()
	{
		if (m_Redo.empty())
			return false;
		Edit entry = std::move(m_Redo.back());
		m_Redo.pop_back();
		m_Text.replace(entry.Offset, entry.Erased.size(), entry.Inserted);
		RebuildLines();
		m_Caret = ClampOffset(entry.CaretAfter);
		m_Anchor = ClampOffset(entry.AnchorAfter);
		m_CurrentState = entry.StateAfter;
		m_DesiredColumn = -1;
		m_UndoOpen = false;
		++m_Revision;
		m_UndoBytes += entry.Erased.size() + entry.Inserted.size();
		m_Undo.push_back(std::move(entry));
		TrimHistory();
		return true;
	}
}
