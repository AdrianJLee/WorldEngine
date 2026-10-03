#pragma once

// MAT-INTEL(用户 2026-09-24:「接下来做材质编辑器代码的智能提示,格式化以及完善知识库和资料」):
// 材质着色器(`.slang`)的**保守**格式化 —— 编辑器侧 header-only(与 SlangHighlight.h 同一口径)。
// 与脚本编辑器的 LuauFormatter 同一思路:只动空白,不重排运算符、不拆合语句、不改标识符。
//
// 规则(逐条可核对;探针 ⑥⑦⑧ 用逐字节断言钉住):
//   1) 行尾空白(空格 / Tab)清除;
//   2) Tab → 4 空格(可调 `indentSize`);**双引号字符串里的 Tab 保留**(改了会改字符串值);
//   3) 缩进 = 4 × `{}` 层级:第一个代码字符是 `}` 的行先减一层;行内 `{`/`}` 计入该行的净增量;
//      字符串与注释里的括号不计数;`#` 开头的指令行保持列 0,且不计括号;
//   4) 连续 ≥3 个空行压成 1 个空行(1–2 个空行原样保留);
//   5) 文件末尾恰好一个换行(多个压缩,缺了补上;空输入仍是空);
//   6) `//!` 后规范化成一个空格(`//!param` → `//! param`;只剩 `//!` 的行不加);
//   7) 行尾符按输入的**第一个**换行判定(LF 输入出 LF,CRLF 输入出 CRLF)。
//
// 幂等:同一份输入格式化两次逐字节相同(format(format(x)) == format(x))—— 规则 3/4/5/6 的
// 输出都落在自己的不动点上。

#include <algorithm>
#include <cstddef>
#include <string>
#include <string_view>

namespace World
{
	namespace SlangFormatDetail
	{
		struct LineScan
		{
			std::string Text;            // Tab 展开 + 行尾空白清除后的行(不含换行)
			std::string_view Trimmed;    // Text 去掉前导空白(指向 Text)
			std::size_t IndentBytes = 0; // Text 的前导空白字节数
			bool Blank = true;
			bool LeadingClose = false;   // 第一个代码字符是 '}'
			bool Directive = false;      // 第一个代码字符是 '#'
			int Open = 0;
			int Close = 0;
			bool BlockComment = false;   // 行尾是否仍在块注释里(交给下一行)
		};

		inline bool IsSpace(char c)
		{
			return c == ' ' || c == '\t' || c == '\r';
		}

		// Tab 展开(字符串内保留)+ 代码括号计数 + 行尾块注释状态。
		// inBlockComment = 行首状态;返回的行尾状态写回同一个变量。
		inline LineScan ScanLine(std::string_view raw, int indentSize, bool& inBlockComment)
		{
			LineScan scan;
			scan.Text.reserve(raw.size() + 8);
			std::size_t index = 0;
			bool inString = false;
			bool inLineComment = false;
			bool firstCode = true;
			for (; index < raw.size(); ++index)
			{
				const char c = raw[index];
				if (inLineComment)
				{
					scan.Text.push_back(c);
					continue;
				}
				if (inBlockComment)
				{
					scan.Text.push_back(c);
					if (c == '*' && index + 1 < raw.size() && raw[index + 1] == '/')
					{
						scan.Text.push_back('/');
						++index;
						inBlockComment = false;
					}
					continue;
				}
				if (inString)
				{
					scan.Text.push_back(c);
					if (c == '\\' && index + 1 < raw.size())
					{
						scan.Text.push_back(raw[index + 1]);
						++index;
						continue;
					}
					if (c == '"')
						inString = false;
					continue;
				}
				if (c == '/' && index + 1 < raw.size() && raw[index + 1] == '/')
				{
					inLineComment = true;
					scan.Text.append("//");
					++index;
					continue;
				}
				if (c == '/' && index + 1 < raw.size() && raw[index + 1] == '*')
				{
					inBlockComment = true;
					scan.Text.append("/*");
					++index;
					continue;
				}
				if (c == '"')
					inString = true;
				if (c != ' ' && c != '\t')
				{
					if (firstCode)
					{
						firstCode = false;
						scan.LeadingClose = (c == '}');
						scan.Directive = (c == '#');
					}
					if (c == '{')
						++scan.Open;
					else if (c == '}')
						++scan.Close;
				}
				if (c == '\t')
				{
					// Tab:字符串里保留,其它位置展开成 indentSize 个空格。
					scan.Text.append(static_cast<std::size_t>(std::max(1, indentSize)), ' ');
					continue;
				}
				scan.Text.push_back(c);
			}
			scan.BlockComment = inBlockComment;
			// 行尾空白清除(不含字符串内的 Tab —— 已在上面保留)。
			while (!scan.Text.empty() && IsSpace(scan.Text.back()))
				scan.Text.pop_back();
			std::size_t first = 0;
			while (first < scan.Text.size() && (scan.Text[first] == ' ' || scan.Text[first] == '\t'))
				++first;
			scan.IndentBytes = first;
			scan.Trimmed = std::string_view(scan.Text).substr(first);
			scan.Blank = scan.Trimmed.empty();
			// `//!` 注解后规范化一个空格(只剩 `//!` 的行不动)。
			if (scan.Trimmed.size() > 3 && scan.Trimmed[0] == '/' && scan.Trimmed[1] == '/'
				&& scan.Trimmed[2] == '!')
			{
				std::size_t body = 3;
				while (body < scan.Trimmed.size() && (scan.Trimmed[body] == ' ' || scan.Trimmed[body] == '\t'))
					++body;
				if (body < scan.Trimmed.size())
				{
					const std::string normalized = std::string("//! ") + std::string(scan.Trimmed.substr(body));
					scan.Text = scan.Text.substr(0, scan.IndentBytes) + normalized;
					scan.Trimmed = std::string_view(scan.Text).substr(scan.IndentBytes);
				}
			}
			return scan;
		}
	}

	// 格式化整段 Slang 源码(见文件头的规则表)。
	inline std::string FormatSlangSource(std::string_view source, int indentSize = 4)
	{
		if (indentSize <= 0)
			indentSize = 4;
		if (source.empty())
			return std::string();
		// 行尾符 = 输入第一个换行符的形态(LF 输入出 LF,CRLF 输入出 CRLF)。
		const std::size_t firstNewline = source.find('\n');
		const bool crlf = firstNewline != std::string_view::npos && firstNewline > 0
			&& source[firstNewline - 1] == '\r';
		const std::string_view newline = crlf ? std::string_view("\r\n") : std::string_view("\n");

		std::string out;
		out.reserve(source.size() + 16);
		int level = 0;
		int blankRun = 0;
		bool inBlockComment = false;
		std::size_t start = 0;
		for (;;)
		{
			const std::size_t end = source.find('\n', start);
			const bool last = end == std::string_view::npos;
			std::string_view raw = source.substr(start, (last ? source.size() : end) - start);
			if (!raw.empty() && raw.back() == '\r')
				raw.remove_suffix(1);
			const SlangFormatDetail::LineScan scan =
				SlangFormatDetail::ScanLine(raw, indentSize, inBlockComment);
			if (scan.Blank)
			{
				// 空行不缩进;连续 ≥3 个压成 1 个(1–2 个原样)—— 数量在遇到下一行时结算。
				++blankRun;
			}
			else
			{
				const int keep = blankRun <= 2 ? blankRun : 1;
				for (int blank = 0; blank < keep; ++blank)
					out += newline;
				blankRun = 0;
				const int indent = scan.Directive
					? 0 : std::max(0, level - (scan.LeadingClose ? 1 : 0));
				out.append(static_cast<std::size_t>(indent * indentSize), ' ');
				out.append(scan.Text.substr(scan.IndentBytes));
				out += newline;
				if (!scan.Directive)
					level = std::max(0, level + scan.Open - scan.Close);
			}
			if (last)
				break;
			start = end + 1;
		}
		// 末尾恰好一个换行(去掉多余的空行/换行,再补一个)。
		while (!out.empty() && (out.back() == '\n' || out.back() == '\r'))
			out.pop_back();
		out += newline;
		return out;
	}
}
