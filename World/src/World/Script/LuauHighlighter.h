#pragma once

// P2 W9-2:Luau 逐行语法高亮(token 级,不做语义着色)。
//
// 口径:
//   - 纯函数式逐行接口 HighlightLine:输入行首状态、输出行尾状态。跨行的**块注释**
//     (--[[ .. ]] / --[==[ .. ]==])与**长括号字符串**([[ .. ]] / [=[ .. ]=])靠这个
//     状态在行间延续 —— 编辑器只画可见行,不能靠"从头 token 化到这一行"。
//   - 输出 Wui::WuiCodeToken(StartByte/EndByte 相对该行起点,升序、互不重叠);
//     空隙表示 Default(WuiCodeEditor 会按 Default 补齐)。
//   - 逐字节扫描但不在多字节 UTF-8 序列内断开:引号/转义/注释标记都是 ASCII,
//     中文字符只可能落在字符串/注释 token 内部(中文串边界用例)。

#include "World/Core/Export.h"
#include "World/WUI/WuiCodeEditor.h"
#include "World/WUI/WuiTextBuffer.h"

#include <cstdint>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace World
{
	// 行间延续状态:-1 = 不在块注释/长字符串中;>=0 = 该结构的 '=' 个数(0 = [[ / --[[)。
	struct LuauHighlightState
	{
		int BlockCommentLevel = -1;
		int LongStringLevel = -1;

		bool operator==(const LuauHighlightState& other) const
		{
			return BlockCommentLevel == other.BlockCommentLevel
				&& LongStringLevel == other.LongStringLevel;
		}
		bool operator!=(const LuauHighlightState& other) const { return !(*this == other); }
	};

	class WLD_API LuauHighlighter
	{
	public:
		// 高亮一行:state 为行首状态,返回后 state 为该行行尾状态(交给下一行)。
		// line 不含换行符(允许带行尾 '\r',会被当作空白)。
		static void HighlightLine(std::string_view line, LuauHighlightState& state,
			std::vector<Wui::WuiCodeToken>& out);
	};

	// 按行 token 缓存(面板每帧调用 Update;WuiCodeEditor 的 Highlight 回调按行取用):
	//   - 只有"内容或行首延续状态变了"的行才重新 token 化,其余行直接复用上一帧结果;
	//   - Find 用行文本指针 + 长度 + 内容指纹定位(编辑导致缓冲区重分配时自然失配,
	//     调用方 Update 后重试即可)。
	class WLD_API LuauHighlightCache
	{
	public:
		// 更新到 buffer 的当前内容(buffer.Revision() 与行数都没变时零成本返回)。
		void Update(const Wui::WuiTextBuffer& buffer);
		void Clear();

		// 该行的 token(行号越界返回空表)。
		const std::vector<Wui::WuiCodeToken>& Tokens(int line) const;
		// 按行文本查找(未命中返回 nullptr:调用方 Update 后重试或现场按行首状态兜底)。
		const std::vector<Wui::WuiCodeToken>* Find(std::string_view line) const;
		int LineCount() const { return m_LineCount; }

	private:
		struct Entry
		{
			uint64_t Hash = 0;
			LuauHighlightState Start;
			LuauHighlightState End;
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
