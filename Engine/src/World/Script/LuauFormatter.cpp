#include "wldpch.h"
#include "World/Script/LuauFormatter.h"

#include "World/Script/LuauHighlighter.h"

#include <string>
#include <vector>

namespace World
{
	namespace
	{
		bool IsTrimChar(char c)
		{
			return c == ' ' || c == '\t' || c == '\r';
		}

		std::string_view TrimView(std::string_view text)
		{
			while (!text.empty() && IsTrimChar(text.front()))
				text.remove_prefix(1);
			while (!text.empty() && IsTrimChar(text.back()))
				text.remove_suffix(1);
			return text;
		}

		bool TokenTextIs(const Wui::WuiCodeToken& token, std::string_view line, std::string_view text)
		{
			if (token.Kind != Wui::WuiCodeTokenKind::Keyword)
				return false;
			const std::size_t start = static_cast<std::size_t>(token.StartByte);
			const std::size_t end = static_cast<std::size_t>(token.EndByte);
			if (end > line.size() || start > end)
				return false;
			return line.substr(start, end - start) == text;
		}

		bool StartsClosed(const std::vector<Wui::WuiCodeToken>& tokens, std::string_view line)
		{
			for (const Wui::WuiCodeToken& token : tokens)
			{
				if (token.Kind != Wui::WuiCodeTokenKind::Keyword)
					continue;
				return TokenTextIs(token, line, "end") || TokenTextIs(token, line, "else")
					|| TokenTextIs(token, line, "elseif") || TokenTextIs(token, line, "until");
			}
			return false;
		}

		int BlockDelta(const std::vector<Wui::WuiCodeToken>& tokens, std::string_view line)
		{
			int delta = 0;
			for (const Wui::WuiCodeToken& token : tokens)
			{
				if (TokenTextIs(token, line, "function") || TokenTextIs(token, line, "then")
					|| TokenTextIs(token, line, "do") || TokenTextIs(token, line, "repeat"))
					++delta;
				else if (TokenTextIs(token, line, "end") || TokenTextIs(token, line, "until"))
					--delta;
			}
			return delta;
		}

		// 行内空白折叠:token(串/注释/算子等)原样保留,只归一化 token 之间的空隙 ——
		// 连续空白折成一个空格;括号/方括号/花括号内侧、逗号分号前后的空隙直接去掉。
		std::string NormalizeInnerSpacing(std::string_view line,
			const std::vector<Wui::WuiCodeToken>& tokens)
		{
			std::string out;
			out.reserve(line.size());
			std::size_t pos = 0;
			std::size_t tokenIndex = 0;
			while (pos < line.size())
			{
				if (tokenIndex < tokens.size()
					&& static_cast<std::size_t>(tokens[tokenIndex].StartByte) <= pos)
				{
					const Wui::WuiCodeToken& token = tokens[tokenIndex];
					const std::size_t end = std::min<std::size_t>(token.EndByte, line.size());
					if (end > pos)
					{
						out.append(line.substr(pos, end - pos));
						pos = end;
					}
					++tokenIndex;
					continue;
				}
				const std::size_t nextTokenStart = tokenIndex < tokens.size()
					? std::min<std::size_t>(tokens[tokenIndex].StartByte, line.size())
					: line.size();
				const std::string_view gap = line.substr(pos, nextTokenStart - pos);
				// 逐段处理:空白段与非空白段分开(例如 "   a " 里的 'a' 两侧各是一段空白,
				// 不能合并成一次折叠,否则 `a =` 会变成 `a=`——用户实测)。
				std::size_t i = 0;
				while (i < gap.size())
				{
					const char c = gap[i];
					if (c != ' ' && c != '\t')
					{
						out.push_back(c);
						++i;
						continue;
					}
					std::size_t j = i;
					while (j < gap.size() && (gap[j] == ' ' || gap[j] == '\t'))
						++j;
					const char prev = out.empty() ? '\0' : out.back();
					const char next = j < gap.size() ? gap[j]
						: (nextTokenStart < line.size() ? line[nextTokenStart] : '\0');
					const bool drop = out.empty() || prev == '(' || prev == '[' || prev == '{'
						|| next == ')' || next == ']' || next == '}' || next == ',' || next == ';';
					if (!drop)
						out.push_back(' ');
					i = j;
				}
				pos = nextTokenStart;
			}
			return out;
		}
	}

	std::string FormatLuauSource(std::string_view source, int indentSize)
	{
		if (indentSize <= 0)
			indentSize = 4;
		std::string out;
		out.reserve(source.size() + source.size() / 8);

		int depth = 0;
		LuauHighlightState state;
		std::size_t pos = 0;
		while (pos <= source.size())
		{
			const std::size_t end = source.find('\n', pos);
			const bool lastLine = end == std::string_view::npos;
			std::string_view raw = source.substr(pos, (lastLine ? source.size() : end) - pos);
			// 行尾符(CRLF 保留 CRLF,LF 保留 LF)。
			const bool crlf = !raw.empty() && raw.back() == '\r';
			std::string_view line = raw;
			if (crlf)
				line.remove_suffix(1);

			const bool insideContinuation = state.BlockCommentLevel >= 0 || state.LongStringLevel >= 0;
			const std::string_view trimmed = TrimView(line);
			std::vector<Wui::WuiCodeToken> tokens;
			LuauHighlightState scanning = state;
			LuauHighlighter::HighlightLine(line, scanning, tokens);
			if (insideContinuation)
			{
				// 块注释/长字符串内部:原样保留(改缩进/去空白会改内容)。
				out.append(line);
			}
			else if (!trimmed.empty())
			{
				const int lineDepth = StartsClosed(tokens, line) ? std::max(0, depth - 1) : depth;
				out.append(static_cast<std::size_t>(lineDepth * indentSize), ' ');
				const std::string normalized = NormalizeInnerSpacing(line, tokens);
				out.append(TrimView(normalized));
				depth = std::max(0, depth + BlockDelta(tokens, line));
			}
			state = scanning;
			if (crlf)
				out.push_back('\r');
			if (lastLine)
				break;
			out.push_back('\n');
			pos = end + 1;
		}
		return out;
	}
}
