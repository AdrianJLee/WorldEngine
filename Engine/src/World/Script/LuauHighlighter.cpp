#include "wldpch.h"
#include "World/Script/LuauHighlighter.h"

#include <cstring>

namespace World
{
	namespace
	{
		using Wui::WuiCodeToken;
		using Wui::WuiCodeTokenKind;

		constexpr size_t kNone = std::string_view::npos;

		bool IsAsciiDigit(char c) { return c >= '0' && c <= '9'; }
		bool IsHexDigit(char c)
		{
			return IsAsciiDigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
		}
		bool IsIdentStart(char c)
		{
			return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
		}
		bool IsIdentPart(char c) { return IsIdentStart(c) || IsAsciiDigit(c); }
		bool IsOperatorChar(char c)
		{
			switch (c)
			{
				case '+': case '-': case '*': case '/': case '%': case '^': case '#':
				case '=': case '<': case '>': case ';': case ':': case ',': case '.':
				case '(': case ')': case '{': case '}': case '[': case ']':
				case '&': case '|': case '~': case '?': case '@': case '$':
					return true;
				default:
					return false;
			}
		}

		// Luau 关键字(含 Luau 追加的 continue/export/type/typeof)。
		bool IsKeyword(std::string_view text)
		{
			static constexpr std::string_view kKeywords[] = {
				"and", "break", "continue", "do", "else", "elseif", "end", "export", "false",
				"for", "function", "if", "in", "local", "nil", "not", "or", "repeat", "return",
				"then", "true", "type", "typeof", "until", "while",
			};
			for (const std::string_view keyword : kKeywords)
				if (text == keyword)
					return true;
			return false;
		}

		// 引擎已知全局名(脚本层暴露给用户的入口;未列出的标识符按 Default 处理)。
		bool IsKnownGlobal(std::string_view text)
		{
			static constexpr std::string_view kGlobals[] = {
				"WorldScript", "ui", "Input", "Save", "Level", "events", "timers", "world",
				"game", "scene", "require", "print", "warn", "task", "Scene", "Entity",
				"Vec2", "Vec3", "Color", "math", "string", "table", "debug", "coroutine",
				"os", "tostring", "tonumber", "select", "pairs", "ipairs", "next", "unpack",
				"assert", "error", "pcall", "xpcall", "setmetatable", "getmetatable", "rawget",
				"rawset", "rawequal", "type", "typeof", "_G",
			};
			for (const std::string_view global : kGlobals)
				if (text == global)
					return true;
			return false;
		}

		// 长括号起始:'[' + n 个 '=' + '['(n >= 0)。命中时 outEquals = n,outLength = 起始长度。
		bool TryLongBracketOpen(std::string_view text, size_t pos, int& outEquals, size_t& outLength)
		{
			if (pos >= text.size() || text[pos] != '[')
				return false;
			size_t cursor = pos + 1;
			int equals = 0;
			while (cursor < text.size() && text[cursor] == '=')
			{
				++equals;
				++cursor;
			}
			if (cursor >= text.size() || text[cursor] != '[')
				return false;
			outEquals = equals;
			outLength = cursor + 1 - pos;
			return true;
		}

		// 匹配的 ']' + equals 个 '=' + ']';返回闭合括号**之后**的位置,找不到返回 kNone。
		size_t FindLongBracketClose(std::string_view text, size_t from, int equals)
		{
			for (size_t i = from; i < text.size(); ++i)
			{
				if (text[i] != ']')
					continue;
				size_t cursor = i + 1;
				int seen = 0;
				while (seen < equals && cursor < text.size() && text[cursor] == '=')
				{
					++seen;
					++cursor;
				}
				if (seen == equals && cursor < text.size() && text[cursor] == ']')
					return cursor + 1;
			}
			return kNone;
		}

		// 单/双引号字符串:支持反斜杠转义;未闭合(行尾截断)时高亮到行尾。
		size_t ScanQuoted(std::string_view line, size_t start)
		{
			const char quote = line[start];
			size_t i = start + 1;
			while (i < line.size())
			{
				const char c = line[i];
				if (c == '\\')
				{
					i += 2; // 转义的下一个字节(多字节串的续字节 >= 0x80,不会误判为转义)
					continue;
				}
				if (c == quote)
					return i + 1;
				++i;
			}
			return line.size();
		}

		// 数字:十进制(小数/指数)、0x 十六进制、0b 二进制。'..' 连接运算符不算小数点。
		size_t ScanNumber(std::string_view line, size_t start)
		{
			size_t i = start;
			if (i + 1 < line.size() && line[i] == '0' && (line[i + 1] == 'x' || line[i + 1] == 'X'))
			{
				i += 2;
				while (i < line.size() && (IsHexDigit(line[i]) || line[i] == '.' || line[i] == '_'))
					++i;
				if (i < line.size() && (line[i] == 'p' || line[i] == 'P'))
				{
					size_t j = i + 1;
					if (j < line.size() && (line[j] == '+' || line[j] == '-'))
						++j;
					if (j < line.size() && IsAsciiDigit(line[j]))
					{
						i = j;
						while (i < line.size() && IsAsciiDigit(line[i]))
							++i;
					}
				}
				return i;
			}
			if (i + 1 < line.size() && line[i] == '0' && (line[i + 1] == 'b' || line[i + 1] == 'B'))
			{
				i += 2;
				while (i < line.size() && (line[i] == '0' || line[i] == '1' || line[i] == '_'))
					++i;
				return i;
			}
			while (i < line.size())
			{
				const char c = line[i];
				if (IsAsciiDigit(c) || c == '_')
				{
					++i;
					continue;
				}
				if (c == '.' && !(i + 1 < line.size() && line[i + 1] == '.'))
				{
					++i;
					continue;
				}
				break;
			}
			if (i < line.size() && (line[i] == 'e' || line[i] == 'E'))
			{
				size_t j = i + 1;
				if (j < line.size() && (line[j] == '+' || line[j] == '-'))
					++j;
				if (j < line.size() && IsAsciiDigit(line[j]))
				{
					i = j;
					while (i < line.size() && IsAsciiDigit(line[i]))
						++i;
				}
			}
			return i;
		}

		// 运算符连续段;遇到 `--`(注释)或长括号起始就让位给更高优先级的规则。
		size_t ScanOperator(std::string_view line, size_t start)
		{
			size_t i = start;
			while (i < line.size() && IsOperatorChar(line[i]))
			{
				if (line[i] == '-' && i + 1 < line.size() && line[i + 1] == '-')
					break;
				int equals = 0;
				size_t openLength = 0;
				if (line[i] == '[' && TryLongBracketOpen(line, i, equals, openLength))
					break;
				++i;
			}
			return i > start ? i : start + 1;
		}

		uint64_t HashLine(std::string_view text)
		{
			uint64_t hash = 1469598103934665603ull;
			for (const char c : text)
			{
				hash ^= static_cast<uint8_t>(c);
				hash *= 1099511628211ull;
			}
			return hash;
		}
	}

	void LuauHighlighter::HighlightLine(std::string_view line, LuauHighlightState& state,
		std::vector<Wui::WuiCodeToken>& out)
	{
		out.clear();
		const size_t size = line.size();
		const auto push = [&out](size_t start, size_t end, WuiCodeTokenKind kind)
		{
			if (end > start)
				out.push_back({ static_cast<uint32_t>(start), static_cast<uint32_t>(end), kind });
		};

		size_t i = 0;
		// 行首延续:上一行开启的块注释/长字符串先吃掉本行的接续段。
		if (state.BlockCommentLevel >= 0)
		{
			const size_t close = FindLongBracketClose(line, 0, state.BlockCommentLevel);
			if (close == kNone)
			{
				push(0, size, WuiCodeTokenKind::Comment);
				return;
			}
			push(0, close, WuiCodeTokenKind::Comment);
			i = close;
			state.BlockCommentLevel = -1;
		}
		else if (state.LongStringLevel >= 0)
		{
			const size_t close = FindLongBracketClose(line, 0, state.LongStringLevel);
			if (close == kNone)
			{
				push(0, size, WuiCodeTokenKind::String);
				return;
			}
			push(0, close, WuiCodeTokenKind::String);
			i = close;
			state.LongStringLevel = -1;
		}

		while (i < size)
		{
			const char c = line[i];
			if (c == ' ' || c == '\t' || c == '\r')
			{
				++i;
				continue;
			}
			// 注释:`--` 行注释,或 `--[==[` 块注释(块注释内不再嵌套,与 Lua/Luau 一致)。
			if (c == '-' && i + 1 < size && line[i + 1] == '-')
			{
				size_t cursor = i + 2;
				int equals = 0;
				size_t openLength = 0;
				if (TryLongBracketOpen(line, cursor, equals, openLength))
				{
					const size_t close = FindLongBracketClose(line, cursor + openLength, equals);
					if (close == kNone)
					{
						push(i, size, WuiCodeTokenKind::Comment);
						state.BlockCommentLevel = equals;
						return;
					}
					push(i, close, WuiCodeTokenKind::Comment);
					i = close;
					continue;
				}
				push(i, size, WuiCodeTokenKind::Comment);
				return;
			}
			// 长括号字符串:[[ .. ]] / [=[ .. ]=]。
			if (c == '[')
			{
				int equals = 0;
				size_t openLength = 0;
				if (TryLongBracketOpen(line, i, equals, openLength))
				{
					const size_t close = FindLongBracketClose(line, i + openLength, equals);
					if (close == kNone)
					{
						push(i, size, WuiCodeTokenKind::String);
						state.LongStringLevel = equals;
						return;
					}
					push(i, close, WuiCodeTokenKind::String);
					i = close;
					continue;
				}
			}
			// 单/双引号字符串。
			if (c == '"' || c == '\'')
			{
				const size_t end = ScanQuoted(line, i);
				push(i, end, WuiCodeTokenKind::String);
				i = end;
				continue;
			}
			// 数字(允许 .5 形式)。
			if (IsAsciiDigit(c) || (c == '.' && i + 1 < size && IsAsciiDigit(line[i + 1])))
			{
				const size_t end = ScanNumber(line, i);
				push(i, end, WuiCodeTokenKind::Number);
				i = end;
				continue;
			}
			// 标识符:关键字 / 已知全局名 / 普通标识符(普通标识符不产 token = Default)。
			if (IsIdentStart(c))
			{
				size_t end = i + 1;
				while (end < size && IsIdentPart(line[end]))
					++end;
				const std::string_view word = line.substr(i, end - i);
				if (IsKeyword(word))
					push(i, end, WuiCodeTokenKind::Keyword);
				else if (IsKnownGlobal(word))
					push(i, end, WuiCodeTokenKind::Global);
				i = end;
				continue;
			}
			// 其余非 ASCII 字节(如未加引号的中文)按 Default 跳过,保证不切碎多字节序列。
			if (static_cast<unsigned char>(c) >= 0x80)
			{
				++i;
				continue;
			}
			// 运算符(连续段一个 token)。
			if (IsOperatorChar(c))
			{
				const size_t end = ScanOperator(line, i);
				push(i, end, WuiCodeTokenKind::Operator);
				i = end;
				continue;
			}
			++i;
		}
	}

	void LuauHighlightCache::Clear()
	{
		m_Lines.clear();
		m_ByPointer.clear();
		m_Revision = ~0ull;
		m_LineCount = -1;
	}

	void LuauHighlightCache::Update(const Wui::WuiTextBuffer& buffer)
	{
		const int count = std::max(1, buffer.LineCount());
		if (m_Revision == buffer.Revision() && m_LineCount == count)
			return;
		m_Revision = buffer.Revision();
		m_LineCount = count;

		const std::string& text = buffer.Text();
		std::vector<Entry> lines(static_cast<size_t>(count));
		LuauHighlightState state;
		for (int line = 0; line < count; ++line)
		{
			const std::pair<size_t, size_t> range = buffer.LineRange(line);
			const std::string_view view(text.data() + range.first, range.second - range.first);
			Entry& entry = lines[static_cast<size_t>(line)];
			entry.Hash = HashLine(view);
			entry.Start = state;
			entry.Text = view.data();
			entry.TextSize = view.size();
			const Entry* previous = static_cast<size_t>(line) < m_Lines.size()
				? &m_Lines[static_cast<size_t>(line)] : nullptr;
			if (previous && previous->Hash == entry.Hash && previous->Start == entry.Start)
			{
				// 内容与行首延续状态都没变 → 直接复用 token,不重复 token 化。
				entry.End = previous->End;
				entry.Tokens = previous->Tokens;
			}
			else
			{
				LuauHighlightState scanning = entry.Start;
				LuauHighlighter::HighlightLine(view, scanning, entry.Tokens);
				entry.End = scanning;
			}
			state = entry.End;
		}
		m_Lines = std::move(lines);
		m_ByPointer.clear();
		for (int line = 0; line < count; ++line)
			m_ByPointer[m_Lines[static_cast<size_t>(line)].Text] = static_cast<size_t>(line);
	}

	const std::vector<Wui::WuiCodeToken>& LuauHighlightCache::Tokens(int line) const
	{
		static const std::vector<Wui::WuiCodeToken> empty;
		if (line < 0 || line >= m_LineCount)
			return empty;
		return m_Lines[static_cast<size_t>(line)].Tokens;
	}

	const std::vector<Wui::WuiCodeToken>* LuauHighlightCache::Find(std::string_view line) const
	{
		const auto found = m_ByPointer.find(line.data());
		if (found == m_ByPointer.end())
			return nullptr;
		const Entry& entry = m_Lines[found->second];
		if (entry.TextSize != line.size() || entry.Hash != HashLine(line))
			return nullptr;
		if (entry.TextSize != 0 && std::memcmp(entry.Text, line.data(), line.size()) != 0)
			return nullptr;
		return &entry.Tokens;
	}
}
