#pragma once

#include "World/Core/Export.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace World::Wui
{
	// 多行代码编辑模型:UTF-8 字节存储 + 行索引 + 光标/选区 + 编辑命令 + 撤销/重做。
	// 纯模型,无渲染依赖(度量/绘制由 WuiCodeEditor 负责)。
	//
	// 单位与策略(冻结口径,见 W9 方案 §11):
	//   - 所有 offset(光标/选区/行范围)都是 UTF-8 字节偏移,并始终落在码点边界上;
	//   - 行按 '\n' 切分;'\r\n' 原样保留 —— 行内容包含尾部 '\r',保存时写回仍是 CRLF,
	//     不把用户的 CRLF 改成 LF;列/软列计算把尾部 '\r' 视为不可见;
	//   - Tab:文件里已有的 '\t' 原样保留,列宽按 4 空格(列计算与渲染一致);
	//     Tab 键(无多行选区时)插入 4 空格,Shift+Tab 反缩进当前行(最多 4 列空白);
	//   - 撤销:插入/退格/删除按"连续同类编辑"合并成一步,其余编辑(回车/粘贴/缩进/替换)
	//     各自成步;历史上限 200 步或 2MB(先到者淘汰最老一步)。
	class WLD_API WuiTextBuffer
	{
	public:
		enum class Motion
		{
			Left,
			Right,
			Up,
			Down,
			WordLeft,
			WordRight,
			LineStart,
			LineEnd,
			DocStart,
			DocEnd,
			PageUp,
			PageDown,
		};

		static constexpr int kUndoStepLimit = 200;
		static constexpr size_t kUndoByteLimit = 2u * 1024u * 1024u;

		// ---- 文本与版本 ----
		// 载入文本(视为已保存状态):清空历史、光标归零、Dirty=false。
		void SetText(std::string text);
		const std::string& Text() const { return m_Text; }
		// 与上次 MarkSaved 的内容状态是否不同(撤销回保存点会重新变干净)。
		bool Dirty() const { return m_CurrentState != m_SavedState; }
		void MarkSaved() { m_SavedState = m_CurrentState; }
		// 单调的内容变更计数:每次编辑/撤销/重做 +1(用于渲染缓存失效与 Changed 判定)。
		uint64_t Revision() const { return m_Revision; }

		// ---- 行索引 ----
		int LineCount() const { return static_cast<int>(m_LineStarts.size()); }
		// 行号 line 的内容字节范围 [start,end):不含行尾 '\n',但保留 '\r'。
		std::pair<size_t, size_t> LineRange(int line) const;
		// offset 所在行号(0 基,越界自动夹紧)。
		int LineOfOffset(size_t offset) const;
		// offset 在行内的显示列(0 基):Tab 推进到下一个 4 的倍数,其余码点 +1,
		// 尾部 '\r' 不计列。
		int ColumnOfOffset(size_t offset) const;

		// ---- 光标与选区 ----
		size_t Caret() const { return m_Caret; }
		size_t Anchor() const { return m_Anchor; }
		void SetCaret(size_t offset, bool keepAnchor = false);
		// 有序选区 [start,end);空选区 start == end。
		std::pair<size_t, size_t> Selection() const;
		bool HasSelection() const { return m_Anchor != m_Caret; }
		void SelectAll();

		// ---- 剪贴板(文本由宿主注入/取走,模型不依赖系统剪贴板) ----
		bool Copy(std::string& out) const;
		bool Cut(std::string& out);

		// ---- 编辑 ----
		void InsertAtCaret(std::string_view text);
		void Backspace();
		void DeleteForward();
		void InsertNewline();
		// Tab / Shift+Tab:多行选区按行缩进/反缩进 4 空格;否则 Tab 插入 4 空格,
		// Shift+Tab 反缩进当前行。
		void IndentSelection(bool outdent);
		bool Paste(std::string_view text);
		// MAT-UI6a:把 [start,end) 换成 text(单次撤销步:替换全部也只算一步,Ctrl+Z 一次回到替换前)。
		// 两端会被夹到 [0,size] 并对齐码点边界;text 为空 = 纯删除。结果与原文相同 → 不改动、返回 false。
		// caret/anchor 落到插入内容之后。
		bool ReplaceRange(size_t start, size_t end, std::string_view text);

		// ---- 导航 ----
		// pageLines:PageUp/PageDown 一次翻过的行数(宿主按视口高度传入,至少 1)。
		void MoveCaret(Motion motion, bool extend, int pageLines = 1);

		// ---- 撤销/重做 ----
		bool Undo();
		bool Redo();
		// 关闭当前合并组:下一次编辑另起一步。
		void BreakUndoGroup() { m_UndoOpen = false; }
		// 整篇替换(格式化等批量改写):只产生**一次**撤销步;返回是否有变化。
		bool ReplaceAll(std::string text);

		// 码点边界(offset 必须落在边界上;不在边界上时向前对齐)。
		static size_t PrevCodepointBoundary(const std::string& text, size_t offset);
		static size_t NextCodepointBoundary(const std::string& text, size_t offset);

	private:
		enum class EditKind : uint8_t
		{
			Insert,
			Backspace,
			Delete,
			Replace,
		};

		struct Edit
		{
			EditKind Kind = EditKind::Replace;
			size_t Offset = 0;
			std::string Erased;
			std::string Inserted;
			size_t CaretBefore = 0, AnchorBefore = 0;
			size_t CaretAfter = 0, AnchorAfter = 0;
			uint64_t StateBefore = 0, StateAfter = 0;
		};

		void CommitEdit(EditKind kind, size_t offset, std::string erased, std::string inserted,
			size_t caretAfter, size_t anchorAfter);
		void RecordUndo(Edit entry);
		void TrimHistory();
		void RebuildLines();
		size_t ClampOffset(size_t offset) const;
		int AdvanceColumn(int column, uint32_t codepoint) const;
		int VisualColumn(size_t offset) const;
		size_t OffsetAtColumn(int line, int column) const;
		void MoveVertical(int deltaLines, bool extend);
		void ApplyCaret(size_t offset, bool extend);
		size_t PrevWordBoundary(size_t offset) const;
		size_t NextWordBoundary(size_t offset) const;
		std::string LineTerminatorAt(size_t offset) const;

		std::string m_Text;
		std::vector<size_t> m_LineStarts { 0 };
		size_t m_Caret = 0;
		size_t m_Anchor = 0;
		int m_DesiredColumn = -1;
		std::vector<Edit> m_Undo;
		std::vector<Edit> m_Redo;
		size_t m_UndoBytes = 0;
		bool m_UndoOpen = false;
		uint64_t m_Revision = 0;
		uint64_t m_CurrentState = 0;
		uint64_t m_SavedState = 0;
		uint64_t m_NextState = 0;
	};
}
