#include "WuiCodeEditor_Internal.h"

namespace World::Wui
{

using namespace WuiCodeEditorDetail;

namespace WuiCodeEditorDetail
{

		// ---- MAT-UI6a 匹配器内部件 ----
		// ASCII 大小写折叠。UTF-8 里 0x41-0x5A 只可能是 ASCII 字符本身(续字节恒 >=0x80),
		// 所以按字节折叠不会把多字节序列折坏。
char FoldAsciiChar(char c){
			return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
		}


bool MatchAt(std::string_view text, size_t at, std::string_view needle, bool caseSensitive){
			if (at + needle.size() > text.size())
				return false;
			for (size_t i = 0; i < needle.size(); ++i)
			{
				const char a = text[at + i];
				const char b = needle[i];
				if (a == b)
					continue;
				if (caseSensitive || FoldAsciiChar(a) != FoldAsciiChar(b))
					return false;
			}
			return true;
		}


		// 命中两侧都不能是标识符码点(口径与 IsWordCodepoint 同一份判定)。
bool WholeWordAt(std::string_view text, size_t start, size_t end){
			if (start > 0)
			{
				size_t i = start - 1;
				while (i > 0 && (static_cast<unsigned char>(text[i]) & 0xC0u) == 0x80u)
					--i;
				size_t length = 1;
				if (IsWordCodepoint(DecodeCodepoint(text, i, length)))
					return false;
			}
			if (end < text.size())
			{
				size_t length = 1;
				if (IsWordCodepoint(DecodeCodepoint(text, end, length)))
					return false;
			}
			return true;
		}


		// caret 处的标识符(同词高亮的"当前词"):先看 caret 左边那个码点(caret 停在词尾也算"在词上"),
		// 再看 caret 处的码点;两者都不是词内字符 → 无词。
void WordAtOffset(std::string_view text, size_t offset, size_t& wordStart, size_t& wordEnd){
			wordStart = wordEnd = 0;
			offset = std::min(offset, text.size());
			size_t anchor = offset;
			bool found = false;
			if (offset > 0)
			{
				const size_t prev = PrevBoundary(text, offset);
				size_t length = 1;
				if (IsWordCodepoint(DecodeCodepoint(text, prev, length)))
				{
					anchor = prev;
					found = true;
				}
			}
			if (!found && offset < text.size())
			{
				size_t length = 1;
				if (IsWordCodepoint(DecodeCodepoint(text, offset, length)))
				{
					anchor = offset;
					found = true;
				}
			}
			if (!found)
				return;
			size_t start = anchor;
			while (start > 0)
			{
				const size_t prev = PrevBoundary(text, start);
				size_t length = 1;
				if (!IsWordCodepoint(DecodeCodepoint(text, prev, length)))
					break;
				start = prev;
			}
			size_t end = anchor;
			while (end < text.size())
			{
				size_t length = 1;
				if (!IsWordCodepoint(DecodeCodepoint(text, end, length)))
					break;
				end += length;
			}
			if (end > start)
			{
				wordStart = start;
				wordEnd = end;
			}
		}


		// 选区是否"整体就是一个标识符"(否则不把选区当当前词:选中一行代码不该全篇高亮)。
bool IsIdentifierRange(std::string_view text, size_t start, size_t end){
			if (end <= start || end > text.size() || end - start > 128)
				return false;
			for (size_t i = start; i < end;)
			{
				size_t length = 1;
				if (!IsWordCodepoint(DecodeCodepoint(text, i, length)))
					return false;
				i += length;
			}
			return true;
		}

}

std::vector<WuiCodeFindMatch> FindCodeMatches(std::string_view text, std::string_view needle, const WuiCodeFindOptions& options){
		std::vector<WuiCodeFindMatch> matches;
		if (needle.empty() || text.empty() || needle.size() > text.size())
			return matches;
		size_t at = 0;
		while (at + needle.size() <= text.size())
		{
			if (MatchAt(text, at, needle, options.CaseSensitive))
			{
				const size_t end = at + needle.size();
				if (!options.WholeWord || WholeWordAt(text, at, end))
				{
					matches.push_back({ at, end });
					at = end;   // 不重叠:命中之后继续
					continue;
				}
			}
			++at;
		}
		return matches;
	}


int NextCodeMatchIndex(const std::vector<WuiCodeFindMatch>& matches, size_t fromOffset){
		if (matches.empty())
			return -1;
		for (size_t i = 0; i < matches.size(); ++i)
			if (matches[i].Start >= fromOffset)
				return static_cast<int>(i);
		return 0;   // 回绕到第一个
	}


int PrevCodeMatchIndex(const std::vector<WuiCodeFindMatch>& matches, size_t fromOffset){
		if (matches.empty())
			return -1;
		for (size_t i = matches.size(); i > 0; --i)
			if (matches[i - 1].Start < fromOffset)
				return static_cast<int>(i - 1);
		return static_cast<int>(matches.size()) - 1;   // 回绕到最后一个
	}

namespace WuiCodeEditorDetail
{
		// MAT-UI6c:"选中即搜索"的播种口径 —— 从当前选区取出可填进查找框的查询文本。
		// 只认**单行**选区:查找框是单行控件,多行选区取第一行只会拿到半行代码/半截标识符,
		// 所以多行直接**跳过**(口径见 MAT-UI6c 报告);首尾空格/制表裁掉后为空也跳过。
bool SeedFindQueryFromSelection(std::string_view text, size_t selStart, size_t selEnd, std::string& out){
			if (selEnd <= selStart || selEnd > text.size())
				return false;
			std::string_view selection = text.substr(selStart, selEnd - selStart);
			if (selection.find('\n') != std::string_view::npos
				|| selection.find('\r') != std::string_view::npos)
				return false;
			while (!selection.empty() && (selection.front() == ' ' || selection.front() == '\t'))
				selection.remove_prefix(1);
			while (!selection.empty() && (selection.back() == ' ' || selection.back() == '\t'))
				selection.remove_suffix(1);
			if (selection.empty())
				return false;
			out.assign(selection);
			return true;
		}


		// 选区是否正好就是"当前命中":连按 Ctrl+F(或上一次查找刚选中了它)时不该把输入框里
		// 的查询改写成命中文本(命中的大小写会跟着变),这种情况只把焦点送回查找框。
bool SelectionIsCurrentFindMatch(const WuiCodeEditorState& state, size_t selStart, size_t selEnd){
			if (state.FindCurrent < 0 || state.FindCurrent >= static_cast<int>(state.FindMatches.size()))
				return false;
			const WuiCodeFindMatch& match = state.FindMatches[static_cast<size_t>(state.FindCurrent)];
			return match.Start == selStart && match.End == selEnd;
		}


		// Ctrl+F 的播种步骤(只在**代码区持焦**那条路径调用;条内再按 Ctrl+F 不改查询,见下面)。
		// 返回 true = 查询已按选区刷新并置好重扫标记(首个命中 = 选区起点处/之后的第一个命中)。
		// 没有可用选区时返回 false(= 查询沿用上一次;光标所在单词**不**播种,这是 MAT-UI6c 定下
		// 的口径:空选区按 Ctrl+F 就是空查询,用户想搜词先双击/拖选,或从剪贴板粘贴)。
bool ApplyFindQueryFromSelection(WuiCodeEditorState& state, const WuiTextBuffer& buffer){
			const auto [selStart, selEnd] = buffer.Selection();
			if (SelectionIsCurrentFindMatch(state, selStart, selEnd))
				return false;
			std::string seed;
			if (!SeedFindQueryFromSelection(buffer.Text(), selStart, selEnd, seed))
				return false;
			state.FindQuery = std::move(seed);
			state.FindAnchor = selStart;
			state.FindScanned = false;   // 强制重扫:当帧就出计数并选中首个命中
			state.FindCurrent = -1;      // 重扫时从 FindAnchor 往后挑,不用旧的当前命中
			return true;
		}

}
}
