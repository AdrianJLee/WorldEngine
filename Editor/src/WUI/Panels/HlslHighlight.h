#pragma once

// M4-S2 / Slang-B1:材质着色器(`.slang`,legacy `.hlsl`)的逐行语法高亮。
//
// 与 LuauHighlighter(Engine/src/World/Script/LuauHighlighter.h)同一套口径,便于复用
// Wui::CodeEditor 的内核(行号、选区、滚动、诊断行、Ctrl+S 全在核心里,这里只提供 token):
//   - 逐行接口 HighlightLine:行首状态进、行尾状态出 —— 跨行的 /* */ 块注释靠它在行间延续
//     (编辑器只画可见行,不能靠在回调里从头 token 化);
//   - 输出 Wui::WuiCodeToken(StartByte/EndByte 相对行首,升序、互不重叠),空隙 = Default;
//   - 纯 ASCII 判定边界(引号/注释/运算符都是 ASCII),中文只可能落在注释/字符串 token 内部;
//   - **只在编辑器侧**:引擎内核不依赖它,所以是 header-only(inline),不新增 Editor 源文件,
//     不需要 CMake reconfigure。

#include "World/WUI/WuiCodeEditor.h"
#include "World/WUI/WuiTextBuffer.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace World
{
// 行间延续状态:着色器源码里只有块注释会跨行(字符串不跨行)。
	struct HlslHighlightState
	{
		bool BlockComment = false;

		bool operator==(const HlslHighlightState& other) const
		{
			return BlockComment == other.BlockComment;
		}
		bool operator!=(const HlslHighlightState& other) const { return !(*this == other); }
	};

	class HlslHighlighter
	{
	public:
		// 高亮一行:state 为行首状态,返回后为该行行尾状态(交给下一行)。
		// line 不含换行符(允许带行尾 '\r',视为空白)。
		static void HighlightLine(std::string_view line, HlslHighlightState& state,
			std::vector<Wui::WuiCodeToken>& out)
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
				// ---- 预处理指令(#include "x.hlsli" / #define)----
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
					if (IsKeyword(word))
						emit(index, end, Wui::WuiCodeTokenKind::Keyword);
					else if (IsIntrinsic(word))
						emit(index, end, Wui::WuiCodeTokenKind::Global);
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
		// 关键字 + 内建类型/结构名(HLSL/Slang 的类型与关键字同一组着色)。
		// Slang-B1:补上 Slang 源里会真的出现的名字 —— 组合采样器(Sampler2D 等,
		// 严格子集里贴图参数就是组合采样器)与 Slang 的模块/泛型关键字。
		static bool IsKeyword(std::string_view word)
		{
			static constexpr std::string_view kWords[] = {
				"bool", "int", "uint", "dword", "half", "float", "double",
				"int2", "int3", "int4", "uint2", "uint3", "uint4",
				"float2", "float3", "float4", "float2x2", "float3x3", "float4x4",
				"min16float", "matrix", "void", "struct", "cbuffer", "tbuffer", "register",
				"Texture1D", "Texture2D", "Texture3D", "TextureCube", "SamplerState",
				"SamplerComparisonState", "RWTexture2D", "ByteAddressBuffer",
				"Texture2DArray", "TextureCubeArray", "Sampler1D", "Sampler2D", "Sampler3D",
				"SamplerCube", "Sampler2DArray", "SamplerCubeArray",
				"static", "const", "inline", "uniform", "in", "out", "inout", "volatile",
				"true", "false", "if", "else", "for", "while", "do", "switch", "case",
				"default", "break", "continue", "return", "discard", "namespace", "typedef",
				"nointerpolation", "noperspective", "linear", "centroid", "sample",
				"packoffset", "row_major", "column_major", "groupshared", "snorm", "unorm",
				// Slang 语言层(模块/泛型/接口)—— 高亮它们让"这是 Slang 源"读得出来。
				"import", "module", "interface", "associatedtype", "enum",
				"where", "each", "expand", "func",
			};
			for (const std::string_view candidate : kWords)
				if (candidate == word)
					return true;
			return false;
		}
		// 内建函数(着色与类型区分开,与脚本编辑器对全局符号的处理同口径)。
		static bool IsIntrinsic(std::string_view word)
		{
			static constexpr std::string_view kWords[] = {
				"abs", "acos", "all", "any", "asin", "atan", "atan2", "ceil", "clamp",
				"cos", "cosh", "cross", "ddx", "ddy", "ddx_coarse", "ddy_coarse",
				"degrees", "determinant", "distance", "dot", "exp", "exp2", "faceforward",
				"floor", "fmod", "frac", "length", "lerp", "log", "log2", "mad", "max",
				"min", "modf", "mul", "normalize", "pow", "radians", "reflect", "refract",
				"round", "rsqrt", "saturate", "sign", "sin", "sincos", "sinh", "smoothstep",
				"sqrt", "step", "tan", "tanh", "transpose", "trunc", "Sample", "SampleLevel",
				"SampleCmp", "GetDimensions", "WeLinearizeColor",
			};
			for (const std::string_view candidate : kWords)
				if (candidate == word)
					return true;
			return false;
		}
	};

	// 按行 token 缓存(与 LuauHighlightCache 同一口径):只有"内容或行首延续状态变了"的行
	// 才重新 token 化;Find 用行文本指针 + 长度定位,编辑导致缓冲区重分配时自然失配。
	class HlslHighlightCache
	{
	public:
		void Update(const Wui::WuiTextBuffer& buffer)
		{
			if (m_Revision == buffer.Revision() && m_LineCount == buffer.LineCount())
				return;
			m_Revision = buffer.Revision();
			m_LineCount = buffer.LineCount();
			m_Lines.clear();
			m_ByPointer.clear();
			m_Lines.reserve(static_cast<size_t>(std::max(0, m_LineCount)));
			HlslHighlightState state;
			for (int line = 0; line < m_LineCount; ++line)
			{
				const std::pair<size_t, size_t> range = buffer.LineRange(line);
				const char* text = buffer.Text().data() + range.first;
				const size_t size = range.second - range.first;
				Entry entry;
				entry.Start = state;
				HlslHighlighter::HighlightLine(std::string_view(text, size), state, entry.Tokens);
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
			HlslHighlightState Start;
			HlslHighlightState End;
			const char* Text = nullptr;
			size_t TextSize = 0;
			std::vector<Wui::WuiCodeToken> Tokens;
		};

		std::vector<Entry> m_Lines;
		std::unordered_map<const char*, size_t> m_ByPointer;
		uint64_t m_Revision = ~0ull;
		int m_LineCount = -1;
	};
}
