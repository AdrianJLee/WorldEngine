#pragma once

// M4-S2 / Slang-B1:材质着色器(`.slang`)的逐行语法高亮 —— 高亮的是 Slang 源,
// HLSL 语法是它的子集,所以关键字表按这一族语言给。
//
// MAT-INTEL2:关键字 / 内建 / 引擎契约符号的事实源挪到 `SlangKeywords.h`(唯一一张表,补全
// 与高亮共用 —— 旧版两张表漂移:补全只有 11 条关键字)。本文件只留"怎么 token 化":
//   - 标识符分类(见 ClassifyIdentifier):唯一表 → Keyword/Global;引擎契约字段
//     (MaterialSurfaceContract 的 X-macro)→ Global;本文件 `//! param` 声明的参数名 → Global;
//     点号后的成员(`surface.BaseColor` / `input.UV` / `Tint.rgb` / `map.Sample`)→ Global;
//   - 没被任何规则命中的标识符(局部变量名等)保持默认色 —— 不为了"好看"发明新的引擎 token kind。
//
// 与 LuauHighlighter(Engine/src/World/Script/LuauHighlighter.h)同一套口径,便于复用
// Wui::CodeEditor 的内核(行号、选区、滚动、诊断行、Ctrl+S 全在核心里,这里只提供 token):
//   - 逐行接口 HighlightLine:行首状态进、行尾状态出 —— 跨行的 /* */ 块注释靠它在行间延续
//     (编辑器只画可见行,不能靠在回调里从头 token 化);
//   - 输出 Wui::WuiCodeToken(StartByte/EndByte 相对行首,升序、互不重叠),空隙 = Default;
//   - 纯 ASCII 判定边界(引号/注释/运算符都是 ASCII),中文只可能落在注释/字符串 token 内部;
//   - **只在编辑器侧**:引擎内核不依赖它,所以是 header-only(inline),不新增 Editor 源文件,
//     不需要 CMake reconfigure。

#include "SlangKeywords.h"

#include "World/WUI/WuiCodeEditor.h"
#include "World/WUI/WuiTextBuffer.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace World
{
// 行间延续状态:着色器源码里只有块注释会跨行(字符串不跨行)。
	struct SlangHighlightState
	{
		bool BlockComment = false;

		bool operator==(const SlangHighlightState& other) const
		{
			return BlockComment == other.BlockComment;
		}
		bool operator!=(const SlangHighlightState& other) const { return !(*this == other); }
	};

	// 文件级符号(本文件 `//! param` 声明的参数名):高亮与补全读同一份扫描结果
	// (SlangAnnotations::CollectDeclaredNames)。指针为空 = 只用内置表 + 契约字段。
	struct SlangHighlightSymbols
	{
		const std::vector<std::string>* FileNames = nullptr;
	};

	// token kind 的稳定名字:探针 dump(面板的 WLD_SLANG_TOKEN_DUMP / scratch 的独立 dumper)
	// 与日志共用一份,避免两处 switch 漂移。
	namespace SlangTokens
	{
		inline const char* KindName(Wui::WuiCodeTokenKind kind)
		{
			switch (kind)
			{
				case Wui::WuiCodeTokenKind::Default: return "default";
				case Wui::WuiCodeTokenKind::Keyword: return "keyword";
				case Wui::WuiCodeTokenKind::String: return "string";
				case Wui::WuiCodeTokenKind::Comment: return "comment";
				case Wui::WuiCodeTokenKind::Number: return "number";
				case Wui::WuiCodeTokenKind::Global: return "global";
				case Wui::WuiCodeTokenKind::Operator: return "operator";
			}
			return "?";
		}
	}

	class SlangHighlighter
	{
	public:
		// 高亮一行:state 为行首状态,返回后为该行行尾状态(交给下一行)。
		// line 不含换行符(允许带行尾 '\r',视为空白)。
		static void HighlightLine(std::string_view line, SlangHighlightState& state,
			std::vector<Wui::WuiCodeToken>& out, const SlangHighlightSymbols* symbols = nullptr)
		{
			out.clear();
			const size_t size = line.size();
			size_t index = 0;
			auto emit = [&](size_t start, size_t end, Wui::WuiCodeTokenKind kind)
			{
				if (end > start)
					out.push_back(Wui::WuiCodeToken { static_cast<uint32_t>(start),
						static_cast<uint32_t>(end), kind });
			};

			// 上一行结束在块注释里:先吃掉注释,直到 */ 或行尾。
			if (state.BlockComment)
			{
				const size_t start = 0;
				size_t close = index;
				bool closed = false;
				while (close + 1 < size)
				{
					if (line[close] == '*' && line[close + 1] == '/')
					{
						close += 2;
						closed = true;
						break;
					}
					++close;
				}
				if (!closed)
				{
					emit(start, size, Wui::WuiCodeTokenKind::Comment);
					return;
				}
				emit(start, close, Wui::WuiCodeTokenKind::Comment);
				state.BlockComment = false;
				index = close;
			}

			while (index < size)
			{
				const char c = line[index];
				// ---- 注释 ----
				if (c == '/' && index + 1 < size && line[index + 1] == '/')
				{
					emit(index, size, Wui::WuiCodeTokenKind::Comment);
					return;
				}
				if (c == '/' && index + 1 < size && line[index + 1] == '*')
				{
					size_t close = index + 2;
					bool closed = false;
					while (close + 1 < size)
					{
						if (line[close] == '*' && line[close + 1] == '/')
						{
							close += 2;
							closed = true;
							break;
						}
						++close;
					}
					if (!closed)
					{
						emit(index, size, Wui::WuiCodeTokenKind::Comment);
						state.BlockComment = true;
						return;
					}
					emit(index, close, Wui::WuiCodeTokenKind::Comment);
					index = close;
					continue;
				}
				// ---- 预处理指令(#include "surface_utils.slang" / #define)----
				if (c == '#' && index == 0)
				{
					size_t directive = index + 1;
					while (directive < size && (line[directive] == ' ' || line[directive] == '\t'))
						++directive;
					size_t wordEnd = directive;
					while (wordEnd < size && IsIdentChar(line[wordEnd]))
						++wordEnd;
					emit(index, wordEnd, Wui::WuiCodeTokenKind::Keyword);
					index = wordEnd;
					continue;
				}
				// ---- 字符串 ----
				if (c == '"')
				{
					size_t end = index + 1;
					while (end < size)
					{
						if (line[end] == '\\' && end + 1 < size)
						{
							end += 2;
							continue;
						}
						if (line[end] == '"')
						{
							++end;
							break;
						}
						++end;
					}
					emit(index, end, Wui::WuiCodeTokenKind::String);
					index = end;
					continue;
				}
				// ---- 数字(1 / 1.0 / .5f / 0x1F / 1e-3)----
				if (std::isdigit(static_cast<unsigned char>(c))
					|| (c == '.' && index + 1 < size
						&& std::isdigit(static_cast<unsigned char>(line[index + 1]))))
				{
					size_t end = index;
					while (end < size)
					{
						const char d = line[end];
						if (std::isalnum(static_cast<unsigned char>(d)) || d == '.')
						{
							// 指数里的符号属于数字的一部分(1e-3),其它 ± 不是。
							++end;
							continue;
						}
						if ((d == '+' || d == '-')
							&& end > index
							&& (line[end - 1] == 'e' || line[end - 1] == 'E'))
						{
							++end;
							continue;
						}
						break;
					}
					emit(index, end, Wui::WuiCodeTokenKind::Number);
					index = end;
					continue;
				}
				// ---- 标识符 / 关键字 ----
				if (IsIdentStart(c))
				{
					size_t end = index;
					while (end < size && IsIdentChar(line[end]))
						++end;
					const std::string_view word = line.substr(index, end - index);
					Wui::WuiCodeTokenKind kind = Wui::WuiCodeTokenKind::Default;
					if (ClassifyIdentifier(word, line, index, symbols, kind))
						emit(index, end, kind);
					index = end;
					continue;
				}
				// ---- 运算符/标点 ----
				if (IsOperator(c))
					emit(index, index + 1, Wui::WuiCodeTokenKind::Operator);
				++index;
			}
		}

	private:
		static bool IsIdentStart(char c)
		{
			return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
		}
		static bool IsIdentChar(char c)
		{
			return IsIdentStart(c) || (c >= '0' && c <= '9');
		}
		static bool IsOperator(char c)
		{
			switch (c)
			{
				case '+': case '-': case '*': case '/': case '%':
				case '=': case '<': case '>': case '!': case '&': case '|':
				case '^': case '~': case '?': case ':': case ';': case ',':
				case '(': case ')': case '{': case '}': case '[': case ']':
				case '.': case '@':
					return true;
				default:
					return false;
			}
		}

		// 标识符分类(着色顺序无关,先命中的优先):
		//   ① 唯一符号表(SlangKeywords.h):关键字/类型 → Keyword;引擎类型/内建/引擎函数 → Global;
		//   ② 引擎契约字段名(MaterialSurfaceContract 的 X-macro)→ Global;
		//   ③ 本文件 `//! param` 声明的参数名 → Global;
		//   ④ 点号后的成员访问 → Global(点号字段:`surface.BaseColor` / `input.UV` / `Tint.rgb`);
		//   ⑤ 其余标识符不着色(保持默认色)。
		static bool ClassifyIdentifier(std::string_view word, std::string_view line, size_t start,
			const SlangHighlightSymbols* symbols, Wui::WuiCodeTokenKind& out)
		{
			if (SlangSymbols::HighlightKind(word, out))
				return true;
			if (SlangSymbols::IsContractField(word))
			{
				out = Wui::WuiCodeTokenKind::Global;
				return true;
			}
			if (symbols != nullptr && symbols->FileNames != nullptr)
			{
				for (const std::string& name : *symbols->FileNames)
				{
					if (name == word)
					{
						out = Wui::WuiCodeTokenKind::Global;
						return true;
					}
				}
			}
			if (IsMemberAccess(line, start))
			{
				out = Wui::WuiCodeTokenKind::Global;
				return true;
			}
			return false;
		}

		// 标识符前面(跳过空格/Tab)是不是 '.'。
		static bool IsMemberAccess(std::string_view line, size_t start)
		{
			size_t probe = start;
			while (probe > 0 && (line[probe - 1] == ' ' || line[probe - 1] == '\t'))
				--probe;
			return probe > 0 && line[probe - 1] == '.';
		}
	};

	// `//!` 注解行的逐行 token(偏移平移到整行):前导空白 + `//!`(含其后的一个空格)是 Comment,
	// 注解体按 Slang 语法着色 —— 于是 `param` / 类型 / `group(...)` / 参数名各有颜色,
	// 而 `label("…")` 这类字符串仍是 String 色。
	//
	// 为什么必须分开着色:WuiCodeEditor 只在"光标不在 String/Comment token 里"时查询补全
	// provider(见 WuiCodeEditor.cpp 的 caretInStringOrComment),注解体若整行 Comment,
	// `//! param …` 上永远弹不出补全。
	namespace SlangAnnotations
	{
		inline void HighlightLineWithAnnotations(std::string_view line, SlangHighlightState& state,
			std::vector<Wui::WuiCodeToken>& out, const SlangHighlightSymbols* symbols = nullptr)
		{
			out.clear();
			if (!IsAnnotationLine(line))
			{
				SlangHighlighter::HighlightLine(line, state, out, symbols);
				return;
			}
			const std::size_t body = BodyStart(line);
			if (body > 0)
			{
				out.push_back(Wui::WuiCodeToken { 0, static_cast<uint32_t>(body),
					Wui::WuiCodeTokenKind::Comment });
			}
			// 注解行不可能接在块注释里(行首就是 `//!`),所以注解体从零状态起。
			SlangHighlightState bodyState;
			std::vector<Wui::WuiCodeToken> bodyTokens;
			SlangHighlighter::HighlightLine(line.substr(body), bodyState, bodyTokens, symbols);
			for (const Wui::WuiCodeToken& token : bodyTokens)
			{
				out.push_back(Wui::WuiCodeToken {
					token.StartByte + static_cast<uint32_t>(body),
					token.EndByte + static_cast<uint32_t>(body), token.Kind });
			}
			state = bodyState;
		}
	}

	// 按行 token 缓存(与 LuauHighlightCache 同一口径):只有"内容或行首延续状态变了"的行
	// 才重新 token 化;Find 用行文本指针 + 长度定位,编辑导致缓冲区重分配时自然失配。
	class SlangHighlightCache
	{
	public:
		// fileNames = 本文件 `//! param` 声明的名字(见 SlangAnnotations::CollectDeclaredNames):
		// 参数名参与着色,所以名字表变了也要重算(表很小,直接按值比较)。
		void Update(const Wui::WuiTextBuffer& buffer, const std::vector<std::string>& fileNames)
		{
			if (m_Revision == buffer.Revision() && m_LineCount == buffer.LineCount()
				&& m_FileNames == fileNames)
				return;
			m_Revision = buffer.Revision();
			m_LineCount = buffer.LineCount();
			m_FileNames = fileNames;
			m_Lines.clear();
			m_ByPointer.clear();
			m_Lines.reserve(static_cast<size_t>(std::max(0, m_LineCount)));
			SlangHighlightSymbols symbols;
			symbols.FileNames = &m_FileNames;
			SlangHighlightState state;
			for (int line = 0; line < m_LineCount; ++line)
			{
				const std::pair<size_t, size_t> range = buffer.LineRange(line);
				const char* text = buffer.Text().data() + range.first;
				const size_t size = range.second - range.first;
				const std::string_view content(text, size);
				Entry entry;
				entry.Start = state;
				// 与面板回调同一条口径:`//!` 注解行交给 HighlightLineWithAnnotations。
				if (SlangAnnotations::IsAnnotationLine(content))
					SlangAnnotations::HighlightLineWithAnnotations(content, state, entry.Tokens, &symbols);
				else
					SlangHighlighter::HighlightLine(content, state, entry.Tokens, &symbols);
				entry.End = state;
				entry.Text = text;
				entry.TextSize = size;
				m_ByPointer[text] = m_Lines.size();
				m_Lines.push_back(std::move(entry));
			}
		}

		void Clear()
		{
			m_Lines.clear();
			m_ByPointer.clear();
			m_Revision = ~0ull;
			m_LineCount = -1;
			m_FileNames.clear();
		}

		const std::vector<Wui::WuiCodeToken>& Tokens(int line) const
		{
			static const std::vector<Wui::WuiCodeToken> empty;
			if (line < 0 || line >= static_cast<int>(m_Lines.size()))
				return empty;
			return m_Lines[static_cast<size_t>(line)].Tokens;
		}

		const std::vector<Wui::WuiCodeToken>* Find(std::string_view line) const
		{
			const auto found = m_ByPointer.find(line.data());
			if (found == m_ByPointer.end())
				return nullptr;
			const Entry& entry = m_Lines[found->second];
			if (entry.TextSize != line.size() || entry.Text != line.data())
				return nullptr;
			return &entry.Tokens;
		}

		int LineCount() const { return m_LineCount; }

	private:
		struct Entry
		{
			SlangHighlightState Start;
			SlangHighlightState End;
			const char* Text = nullptr;
			size_t TextSize = 0;
			std::vector<Wui::WuiCodeToken> Tokens;
		};

		std::vector<Entry> m_Lines;
		std::unordered_map<const char*, size_t> m_ByPointer;
		std::vector<std::string> m_FileNames;
		uint64_t m_Revision = ~0ull;
		int m_LineCount = -1;
	};
}
