#include "wldpch.h"
#include "World/WUI/WuiCodeEditor.h"

#include "World/Core/KeyCodes.h"
#include "World/WUI/WuiAccessibility.h"
#include "World/WUI/WuiWidgets.h"   // DrawFocusRing / CurrentTheme(焦点环与其它控件同一套 token)

#include <algorithm>
#include <cmath>

namespace World::Wui
{
	namespace
	{
		constexpr float kScrollbarWidth = 10.0f;
		constexpr float kGutterPaddingLeft = 6.0f;
		constexpr float kGutterPaddingRight = 8.0f;
		constexpr float kThumbMinHeight = 24.0f;

		const WuiColor kBackground { 0.10f, 0.105f, 0.11f, 1.0f };
		const WuiColor kGutterBackground { 0.13f, 0.135f, 0.14f, 1.0f };
		const WuiColor kGutterText { 0.44f, 0.46f, 0.50f, 1.0f };
		const WuiColor kGutterTextCurrent { 0.78f, 0.81f, 0.85f, 1.0f };
		const WuiColor kCurrentLine { 1.0f, 1.0f, 1.0f, 0.05f };
		const WuiColor kCaretColor { 0.90f, 0.92f, 0.95f, 1.0f };
		const WuiColor kScrollbarTrack { 0.15f, 0.155f, 0.16f, 1.0f };
		const WuiColor kScrollbarThumb { 0.33f, 0.35f, 0.38f, 1.0f };
		const WuiColor kSuggestBackground { 0.145f, 0.152f, 0.165f, 0.985f };
		const WuiColor kSuggestBorder { 0.30f, 0.32f, 0.36f, 1.0f };
		const WuiColor kSuggestSelected { 0.20f, 0.30f, 0.46f, 1.0f };
		const WuiColor kSuggestName { 0.86f, 0.89f, 0.93f, 1.0f };
		// WUI-MAT-INTEL4:类型列用"类型色"(teal)而不是灰 —— 与名字(近白)一眼分开。
		const WuiColor kSuggestType { 0.31f, 0.79f, 0.69f, 1.0f };
		const WuiColor kSuggestDoc { 0.66f, 0.69f, 0.74f, 1.0f };
		// ---- MAT-UI6a:查找/替换条 + 同词高亮的配色(与正文同一条暗色系,不引入主题依赖)----
		const WuiColor kFindBarBackground { 0.135f, 0.142f, 0.152f, 0.995f };
		const WuiColor kFindBarButton { 0.19f, 0.20f, 0.22f, 1.0f };
		const WuiColor kFindBarButtonHover { 0.26f, 0.28f, 0.31f, 1.0f };
		const WuiColor kFindBarButtonActive { 0.20f, 0.34f, 0.52f, 1.0f };
		const WuiColor kFindBarText { 0.80f, 0.83f, 0.87f, 1.0f };
		const WuiColor kFindBarTextActive { 0.92f, 0.95f, 0.99f, 1.0f };
		const WuiColor kFindBarNoMatch { 0.92f, 0.55f, 0.50f, 1.0f };
		constexpr float kFindBarRowHeight = 22.0f;
		constexpr float kFindBarPad = 4.0f;
		constexpr float kFindBarGap = 3.0f;
		constexpr float kFindBarFontSize = 13.0f;
		// 同词高亮:当前词一档更强(可辨的两级底色)。
		const WuiColor kOccurrenceStrong { 0.26f, 0.42f, 0.62f, 0.65f };
		const WuiColor kOccurrenceWeak { 0.34f, 0.37f, 0.42f, 0.32f };
		// WUI-MAT-INTEL4 配色(VS Code Dark+ 风格):名字(Default)保持中性;类型=青绿、函数=暖黄、
		// 字段/全局=浅蓝、注解关键字=紫 —— 与关键字(蓝)/字符串/数字/注释/运算符分开。
		const WuiColor kTokenColors[11] = {
			{ 0.83f, 0.83f, 0.83f, 1.0f },   // Default(变量/名字/标点)
			{ 0.34f, 0.61f, 0.84f, 1.0f },   // Keyword
			{ 0.81f, 0.57f, 0.47f, 1.0f },   // String
			{ 0.42f, 0.60f, 0.33f, 1.0f },   // Comment
			{ 0.71f, 0.81f, 0.66f, 1.0f },   // Number
			{ 0.61f, 0.86f, 1.00f, 1.0f },   // Global
			{ 0.83f, 0.83f, 0.83f, 1.0f },   // Operator
			{ 0.31f, 0.79f, 0.69f, 1.0f },   // Type(类型标识:teal)
			{ 0.86f, 0.86f, 0.67f, 1.0f },   // Function(函数:暖黄)
			{ 0.61f, 0.86f, 1.00f, 1.0f },   // Field(成员/字段:浅蓝)
			{ 0.77f, 0.53f, 0.75f, 1.0f },   // Annotation(注解关键字:紫)
		};

		// W9.5 补全浮层:最多 12 行可滚动 + 底部一行 Doc。
		constexpr int kSuggestMaxRows = 12;
		constexpr float kSuggestRowHeight = 22.0f;
		constexpr float kSuggestWidth = 320.0f;
		constexpr float kSuggestFontSize = 13.0f;
		constexpr float kSuggestDocFontSize = 12.0f;

		// MAT-UI6a:缩放的唯一夹取口径(NaN / 非正数一律当 1.0,避免把字号画成 NaN)。
		float ClampZoom(float zoom)
		{
			if (!(zoom > 0.0f))
				return 1.0f;
			return std::max(kCodeEditorZoomMin, std::min(kCodeEditorZoomMax, zoom));
		}

		// MAT-UI6a:查找条高度(单行 = 查找;替换模式 = 查找 + 替换两行)。0 = 未打开。
		float FindBarHeight(bool visible, bool replaceMode, bool readOnly)
		{
			if (!visible)
				return 0.0f;
			const float rows = (replaceMode && !readOnly) ? 2.0f : 1.0f;
			return kFindBarPad * 2.0f + kFindBarRowHeight * rows + (rows > 1.0f ? kFindBarGap : 0.0f);
		}

		struct WuiCodeEditorState
		{
			float ScrollY = 0.0f;
			bool MouseSelecting = false;
			bool DraggingThumb = false;
			float ThumbGrabOffset = 0.0f;
			bool FollowCaret = true;
			// ---- W9.5 补全浮层(状态必须跨帧:打开/选中/滚动)----
			bool PopupVisible = false;
			std::size_t ReplaceStart = 0;   // 前缀起点(buffer 字节偏移)
			std::size_t CaretAtOpen = 0;    // 弹出时的 caret(buffer 字节偏移)
			std::vector<World::LuauCompletionItem> PopupItems;
			std::string PopupLinePrefix;    // 打开浮层时的 linePrefix(无障碍状态用)
			float PopupAnchorX = 0.0f;      // 浮层锚点(caret 左上角),打开时固定避免逐帧漂移
			float PopupAnchorY = 0.0f;
			int PopupSelected = 0;
			int PopupScroll = 0;
			// Enter 是否算"接受":点号/冒号、Ctrl+Space 或手动 Up/Down 之后为 true;
			// 普通打字自动弹出的浮层里 Enter 仍是换行(避免吞换行/把 Enter 当接受)。
			bool PopupAcceptEnter = false;
			bool PopupVisibleLastFrame = false;
			WuiRect PopupBounds {};
			// ---- W9.6 悬停提示(名称/类型/文档) ----
			bool HoverVisible = false;
			uint64_t HoverSinceFrame = 0;
			std::size_t HoverWordStart = 0;   // buffer 字节偏移
			std::size_t HoverWordEnd = 0;
			glm::vec2 HoverMousePos { 0, 0 };
			std::string HoverName;
			std::string HoverType;
			std::string HoverDoc;
			// W9.8:补全函数参数占位(交替 start/end buffer 偏移)与当前选中的参数序号。
			std::vector<std::size_t> SnippetRanges;
			std::size_t SnippetIndex = 0;
			// ---- MAT-UI6a:会话缩放(内核持有;初值取 options.UiZoom,"只有第一次"生效)----
			float UiZoom = 1.0f;
			bool ZoomSeeded = false;
			// ---- MAT-UI6a:查找/替换条 ----
			bool FindVisible = false;
			bool FindReplaceMode = false;
			std::string FindQuery;         // 查找输入框(TextField 直接写这里,跨帧保留)
			std::string FindReplaceQuery;  // 替换输入框
			// 命中缓存:只在"查询/开关/文本版本"变化时重扫(节流;大文件不逐帧全扫)。
			std::vector<WuiCodeFindMatch> FindMatches;
			std::string FindScannedQuery;
			bool FindScannedCase = false;
			bool FindScannedWord = false;
			uint64_t FindScannedRevision = 0;
			bool FindScanned = false;
			int FindCurrent = -1;      // 当前命中下标(FindMatches 内;-1 = 无)
			size_t FindAnchor = 0;     // 打开/上次导航落点:重扫后从这里往后挑当前命中
			// 同词高亮缓存:键 = (词, 文本版本, 大小写口径)。
			std::vector<WuiCodeFindMatch> OccurrenceMatches;
			std::string OccurrenceWord;
			size_t OccurrenceWordStart = 0;
			size_t OccurrenceWordEnd = 0;
			std::string OccurrenceScannedWord;
			uint64_t OccurrenceScannedRevision = 0;
			bool OccurrenceScannedCase = false;
			bool OccurrenceScanned = false;
		};

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

		uint32_t DecodeCodepoint(std::string_view text, size_t offset, size_t& length)
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

		void EncodeUtf8(std::string& out, uint32_t codepoint)
		{
			if (codepoint < 0x80)
				out.push_back(static_cast<char>(codepoint));
			else if (codepoint < 0x800)
			{
				out.push_back(static_cast<char>(0xC0 | (codepoint >> 6)));
				out.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
			}
			else if (codepoint < 0x10000)
			{
				out.push_back(static_cast<char>(0xE0 | (codepoint >> 12)));
				out.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
				out.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
			}
			else
			{
				out.push_back(static_cast<char>(0xF0 | (codepoint >> 18)));
				out.push_back(static_cast<char>(0x80 | ((codepoint >> 12) & 0x3F)));
				out.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
				out.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
			}
		}

		bool IsWordCodepoint(uint32_t codepoint)
		{
			return codepoint >= 0x80
				|| (codepoint >= '0' && codepoint <= '9')
				|| (codepoint >= 'A' && codepoint <= 'Z')
				|| (codepoint >= 'a' && codepoint <= 'z')
				|| codepoint == '_';
		}

		// 补全前缀标识符字符(与 Luau 标识符一致的 ASCII 子集)。
		bool IsIdentByte(char c)
		{
			return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z')
				|| (c >= 'a' && c <= 'z') || c == '_';
		}

		// 接受补全时的插入形态:方法/函数插 name() 且 caret 落括号内;其余插 name。
		bool InsertAsCall(const World::LuauCompletionItem& item)
		{
			if (item.Kind == World::LuauCompletionItem::KindType::Method)
				return true;
			return item.Type.find("fun") != std::string::npos
				|| item.Name.find('(') != std::string::npos;
		}

		// 浮层里的名字去掉 "name(...)" 写法,只留标识符(插入时按需补 "()")。
		std::string CompletionInsertName(std::string name)
		{
			const std::size_t paren = name.find('(');
			if (paren != std::string::npos)
				name.erase(paren);
			return name;
		}

		// 前缀不足时截断到字节预算(UTF-8 边界;不做精确像素裁剪)。
		std::string TruncateBytes(const std::string& text, std::size_t maxBytes)
		{
			if (text.size() <= maxBytes)
				return text;
			std::size_t cut = maxBytes;
			while (cut > 0 && (static_cast<unsigned char>(text[cut]) & 0xC0) == 0x80)
				--cut;
			return text.substr(0, cut) + "…";
		}

		// 真实度量的像素 → 行内字节偏移(命中测试,替代旧的 CursorAtX 启发式)。
		size_t OffsetAtX(const WuiContext& ctx, std::string_view line, float x, float fontSize)
		{
			float pen = 0.0f;
			size_t offset = 0;
			while (offset < line.size())
			{
				const size_t length = std::min(CodepointLength(static_cast<unsigned char>(line[offset])), line.size() - offset);
				const float width = ctx.MeasureTextWidth(line.substr(offset, length), fontSize, WuiFontFamily::Monospace);
				if (width > 0.0f && x < pen + width * 0.5f)
					return offset;
				pen += width;
				offset += length;
			}
			return line.size();
		}

		void SelectWordAt(WuiTextBuffer& buffer, const std::string& text, size_t offset)
		{
			offset = std::min(offset, text.size());
			size_t start = offset;
			while (start > 0)
			{
				const size_t prev = WuiTextBuffer::PrevCodepointBoundary(text, start);
				size_t length = 1;
				const uint32_t codepoint = DecodeCodepoint(text, prev, length);
				if (!IsWordCodepoint(codepoint))
					break;
				start = prev;
			}
			size_t end = offset;
			while (end < text.size())
			{
				size_t length = 1;
				const uint32_t codepoint = DecodeCodepoint(text, end, length);
				if (!IsWordCodepoint(codepoint))
					break;
				end += length;
			}
			if (end <= start)
				return;
			buffer.SetCaret(start, false);
			buffer.SetCaret(end, true);
		}

		std::string_view LineView(const WuiTextBuffer& buffer, int line, size_t& lineStart)
		{
			const auto [start, end] = buffer.LineRange(line);
			lineStart = start;
			std::string_view view(buffer.Text().data() + start, end - start);
			if (!view.empty() && view.back() == '\r')
				view.remove_suffix(1);
			return view;
		}

		// 前缀起点:caret 往回走连续标识符字符(不含 '.'/':');紧邻非标识符时返回 caret
		// (空前缀,如刚敲下 '.')。
		std::size_t CompletionReplaceStart(std::string_view text, std::size_t caret)
		{
			caret = std::min(caret, text.size());
			std::size_t start = caret;
			while (start > 0 && IsIdentByte(text[start - 1]))
				--start;
			return start;
		}

		// 上一个码点边界(string_view 版:悬停取词用,避免为每帧扫描构造 std::string)。
		std::size_t PrevBoundary(std::string_view text, std::size_t offset)
		{
			if (offset == 0)
				return 0;
			std::size_t i = offset - 1;
			while (i > 0 && (static_cast<unsigned char>(text[i]) & 0xC0u) == 0x80u)
				--i;
			return i;
		}

		// W9.6 悬停/文档:按像素宽度截断(超宽补 '…')。
		std::string EllipsizeToWidth(const WuiContext& ctx, std::string_view text, float maxWidth,
			float fontSize)
		{
			if (text.empty() || maxWidth <= 0.0f)
				return std::string(text);
			if (ctx.MeasureTextWidth(text, fontSize, WuiFontFamily::Ui) <= maxWidth)
				return std::string(text);
			std::string out;
			size_t i = 0;
			while (i < text.size())
			{
				const size_t begin = i;
				size_t length = 1;
				DecodeCodepoint(text, i, length);
				i += length;
				std::string candidate = out;
				candidate.append(text.substr(begin, i - begin));
				if (ctx.MeasureTextWidth(candidate, fontSize, WuiFontFamily::Ui) + 12.0f > maxWidth)
					break;
				out = std::move(candidate);
			}
			out += "…";
			return out;
		}

		// W9.6 悬停/文档:按像素宽度折行(最多 maxLines 行,最后一行超长时省略)。
		std::vector<std::string> WrapToWidth(const WuiContext& ctx, std::string_view text,
			float maxWidth, float fontSize, size_t maxLines)
		{
			std::vector<std::string> lines;
			if (text.empty() || maxLines == 0)
				return lines;
			std::string current;
			size_t i = 0;
			while (i < text.size())
			{
				const size_t begin = i;
				size_t length = 1;
				DecodeCodepoint(text, i, length);
				i += length;
				const std::string_view piece = text.substr(begin, i - begin);
				if (piece[0] == '\n')
				{
					lines.push_back(std::move(current));
					current.clear();
					if (lines.size() >= maxLines)
						break;
					continue;
				}
				std::string candidate = current;
				candidate.append(piece);
				if (ctx.MeasureTextWidth(candidate, fontSize, WuiFontFamily::Ui) > maxWidth
					&& !current.empty())
				{
					lines.push_back(std::move(current));
					current.assign(piece);
					if (lines.size() >= maxLines)
						break;
				}
				else
					current = std::move(candidate);
			}
			if (lines.size() < maxLines && !current.empty())
				lines.push_back(std::move(current));
			if (lines.size() == maxLines && i < text.size() && !lines.empty())
				lines.back() = EllipsizeToWidth(ctx, lines.back(), maxWidth, fontSize);
			return lines;
		}

		// ---- MAT-UI6a 匹配器内部件 ----
		// ASCII 大小写折叠。UTF-8 里 0x41-0x5A 只可能是 ASCII 字符本身(续字节恒 >=0x80),
		// 所以按字节折叠不会把多字节序列折坏。
		char FoldAsciiChar(char c)
		{
			return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
		}

		bool MatchAt(std::string_view text, size_t at, std::string_view needle, bool caseSensitive)
		{
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
		bool WholeWordAt(std::string_view text, size_t start, size_t end)
		{
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
		void WordAtOffset(std::string_view text, size_t offset, size_t& wordStart, size_t& wordEnd)
		{
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
		bool IsIdentifierRange(std::string_view text, size_t start, size_t end)
		{
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

	std::vector<WuiCodeFindMatch> FindCodeMatches(std::string_view text, std::string_view needle,
		const WuiCodeFindOptions& options)
	{
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

	int NextCodeMatchIndex(const std::vector<WuiCodeFindMatch>& matches, size_t fromOffset)
	{
		if (matches.empty())
			return -1;
		for (size_t i = 0; i < matches.size(); ++i)
			if (matches[i].Start >= fromOffset)
				return static_cast<int>(i);
		return 0;   // 回绕到第一个
	}

	int PrevCodeMatchIndex(const std::vector<WuiCodeFindMatch>& matches, size_t fromOffset)
	{
		if (matches.empty())
			return -1;
		for (size_t i = matches.size(); i > 0; --i)
			if (matches[i - 1].Start < fromOffset)
				return static_cast<int>(i - 1);
		return static_cast<int>(matches.size()) - 1;   // 回绕到最后一个
	}

	WuiCodeEditorResult CodeEditor(WuiContext& ctx, WuiId id, const WuiRect& rect,
		WuiTextBuffer& buffer, const WuiCodeEditorOptions& options)
	{
		WuiCodeEditorResult result;
		if (rect.W <= 0.0f || rect.H <= 0.0f)
			return result;

		WuiCodeEditorState& state = ctx.Persist<WuiCodeEditorState>(id, {});
		// MAT-UI6a:会话缩放(生效字号 = FontSize × UiZoom,行高同倍)。只在本实例第一次出现时读
		// options.UiZoom 播种,之后以内核状态为准;宿主要持久化就回写 result.UiZoom。
		if (!state.ZoomSeeded)
		{
			state.UiZoom = ClampZoom(options.UiZoom);
			state.ZoomSeeded = true;
		}
		state.UiZoom = ClampZoom(state.UiZoom);
		const float zoom = state.UiZoom;
		const float fontSize = (options.FontSize > 0.0f ? options.FontSize : 14.0f) * zoom;
		const float lineHeight = (options.LineHeight > 0.0f ? options.LineHeight : 20.0f) * zoom;
		result.UiZoom = zoom;
		const uint64_t revisionAtStart = buffer.Revision();
		const int lineCount = std::max(1, buffer.LineCount());
		const float digitWidth = ctx.MeasureTextWidth("0", fontSize, WuiFontFamily::Monospace);
		const float gutterWidth = options.GutterWidth > 0.0f
			? options.GutterWidth
			: kGutterPaddingLeft + digitWidth * static_cast<float>(std::to_string(lineCount).size()) + kGutterPaddingRight;
		const float scrollbarWidth = rect.W > gutterWidth + kScrollbarWidth ? kScrollbarWidth : 0.0f;
		const WuiRect textRect { rect.X + gutterWidth, rect.Y,
			std::max(0.0f, rect.W - gutterWidth - scrollbarWidth), rect.H };
		const WuiRect track { rect.X + rect.W - kScrollbarWidth, rect.Y, kScrollbarWidth, rect.H };
		const float contentHeight = static_cast<float>(lineCount) * lineHeight;
		const float maxScroll = std::max(0.0f, contentHeight - rect.H);
		const bool needScrollbar = scrollbarWidth > 0.0f && maxScroll > 0.0f;
		bool focused = ctx.Focus() == id;
		const bool readOnly = options.ReadOnly;
		// MAT-UI6a:查找条的固定 a11y id(共享内核:两个宿主自动同一条口径,探针可驱动)。
		const WuiId findFieldId = HashId("code-editor.find");
		const WuiId findReplaceFieldId = HashId("code-editor.replace");
		// "Aa"/"ab" 两个开关:与查找条同生命周期地跨帧保留(内核状态,不写偏好文件)。
		bool& findCaseSetting = ctx.Persist<bool>(HashId("code-editor.find.case"), false);
		bool& findWordSetting = ctx.Persist<bool>(HashId("code-editor.find.word"), false);
		const WuiInputState& input = ctx.Input();
		// 本帧开始时的浮层可见性(帧内会被"打字关闭/点外关闭"改写,刷新判断要用这个)。
		const bool popupVisibleAtFrameStart = state.PopupVisible;
		// P1c-a:编辑器本体的 a11y 节点(以前只有补全浮层/悬停时才登记节点)。点它 = 聚焦并把
		// caret 放到该点(真实鼠标路径),之后的 ui.type/ui.key 都落在这个焦点上;只读模式仍然
		// 可聚焦/选择,但 enabled=false 让人一眼看出"不能改内容"。
		if (WuiAccessibility::Get().Enabled() && id != 0)
		{
			WuiAccessNode node;
			node.Window = WuiAccessibility::Get().CurrentWindow();
			node.Panel = WuiAccessibility::Get().CurrentPanel();
			node.Id = id;
			node.Kind = "code-editor";
			node.Label = options.CompletionIdPrefix;
			node.Value = std::to_string(lineCount) + (readOnly ? " lines, read-only" : " lines");
			node.Rect = rect;
			node.Enabled = !readOnly;
			node.Interactive = true;
			node.Focused = focused;
			WuiAccessibility::Get().Register(node);
		}

		// 命中:行 = 鼠标 y 对应的行,列 = 该行内真实度量得到的字节偏移。
		const auto HitOffset = [&](float mouseY, float mouseX)
		{
			int line = static_cast<int>(std::floor((mouseY - textRect.Y + state.ScrollY) / lineHeight));
			line = std::max(0, std::min(line, lineCount - 1));
			size_t lineStart = 0;
			const std::string_view view = LineView(buffer, line, lineStart);
			const float x = std::max(0.0f, mouseX - textRect.X);
			return lineStart + OffsetAtX(ctx, view, x, fontSize);
		};

		// ---- 鼠标:点击定位 / 拖拽选择 / 双击选词 ----
		if (input.MouseClicked[0] && ctx.IsHovered(textRect))
		{
			ctx.SetFocus(id);
			focused = true;
			const size_t offset = HitOffset(input.MousePos.y, input.MousePos.x);
			if (input.MouseDoubleClicked[0])
			{
				SelectWordAt(buffer, buffer.Text(), offset);
				state.MouseSelecting = false;
			}
			else
			{
				buffer.SetCaret(offset, input.Shift);
				state.MouseSelecting = true;
			}
			state.FollowCaret = true;
		}
		if (state.MouseSelecting && input.MouseDown[0])
		{
			buffer.SetCaret(HitOffset(input.MousePos.y, input.MousePos.x), true);
			state.FollowCaret = true;
		}
		if (state.MouseSelecting && input.MouseReleased[0])
			state.MouseSelecting = false;
		// 浮层打开时点击编辑区空白(浮层矩形之外)= 关闭浮层;点击候选行稍后处理。
		if (state.PopupVisible && input.MouseClicked[0]
			&& !state.PopupBounds.Contains(input.MousePos))
			state.PopupVisible = false;
		// "点到编辑器之外就失焦"必须用**原始**矩形判定:浮层(登记过遮挡)盖在编辑器上时,
		// IsHovered(rect) 会因为遮挡区返回 false,把"点浮层候选行"误判成"点了外面"而失焦关浮层。
		if (focused && input.MouseClicked[0] && !ctx.HitTestRaw(rect, input.MousePos))
		{
			ctx.SetFocus(0);
			ctx.SetTextInputActive(false);
			focused = false;
			state.PopupVisible = false;
		}

		// ---- 滚轮(悬停文本区):只滚动,不动 caret ----
		// MAT-UI6a:Ctrl+滚轮 = 会话缩放(每格 0.05,夹到 [0.5, 3.0]);普通滚轮仍只滚动。
		// 浮层内滚轮由下面的浮层逻辑消费(滚动列表,不滚编辑器)。
		const bool wheelOverPopup = state.PopupVisible
			&& state.PopupBounds.Contains(input.MousePos);
		if (ctx.IsHovered(textRect) && input.Wheel != 0.0f && !wheelOverPopup)
		{
			if (input.Ctrl)
			{
				const float next = ClampZoom(state.UiZoom + input.Wheel * kCodeEditorZoomStep);
				if (std::fabs(next - state.UiZoom) > 1e-6f)
				{
					state.UiZoom = next;
					result.ZoomChanged = true;
					result.UiZoom = next;
				}
			}
			else
				state.ScrollY -= input.Wheel * lineHeight * 3.0f;
		}

		// ---- W9.5 补全触发:本帧插入可见字符、TextInput 含 '.'/':'、或 Ctrl+Space 沿 ----
		struct CompletionRequest
		{
			bool Wanted = false;
			bool CtrlSpace = false;
			bool Explicit = false;   // 点号/冒号或 Ctrl+Space:Enter 可以接受
		};
		CompletionRequest completionRequest;

		// ---- 键盘(仅聚焦时;浮层可见时下列键优先被浮层消费,caret 不动)----
		if (focused)
		{
			ctx.SetTextInputActive(true);
			if (ctx.IsHovered(textRect))
				ctx.SetCursor(WuiCursor::IBeam);

			// Esc 的双重语义:浮层可见时只关浮层、保持焦点;浮层不可见时才失焦。
			const bool escapeTriggered = ctx.WasKeyTriggered(KeyCodes::Escape);
			bool escapeHandledByPopup = false;
			bool popupConsumedKey = false;
			if (state.PopupVisible)
			{
				const int count = static_cast<int>(state.PopupItems.size());
				// 浮层可见时普通字符仍然照常输入(输入后重新过滤候选);这里只消费
				// 导航/接受/关闭键。Enter/Tab 的按键分支在下面统一处理。
				if (escapeTriggered)
				{
					// Esc 只关浮层:保持文本焦点(浮层不可见时 Esc 仍是失焦)。
					state.PopupVisible = false;
					escapeHandledByPopup = true;
				}
				else if (ctx.WasKeyTriggered(KeyCodes::Up))
				{
					popupConsumedKey = true;
					state.PopupAcceptEnter = true;   // 手动选过之后 Enter 才可以接受
					if (count > 0)
						state.PopupSelected = (state.PopupSelected - 1 + count) % count;
					state.PopupScroll = std::max(0, std::min(
						std::max(0, count - kSuggestMaxRows), state.PopupSelected - kSuggestMaxRows / 2));
				}
				else if (ctx.WasKeyTriggered(KeyCodes::Down))
				{
					popupConsumedKey = true;
					state.PopupAcceptEnter = true;
					if (count > 0)
						state.PopupSelected = (state.PopupSelected + 1) % count;
					state.PopupScroll = std::max(0, std::min(
						std::max(0, count - kSuggestMaxRows), state.PopupSelected - kSuggestMaxRows / 2));
				}
				else if (ctx.WasKeyTriggered(KeyCodes::PageUp))
				{
					popupConsumedKey = true;
					state.PopupSelected = std::max(0, state.PopupSelected - kSuggestMaxRows);
					state.PopupScroll = std::max(0, std::min(
						std::max(0, count - kSuggestMaxRows), state.PopupSelected - kSuggestMaxRows / 2));
				}
				else if (ctx.WasKeyTriggered(KeyCodes::PageDown))
				{
					popupConsumedKey = true;
					if (count > 0)
						state.PopupSelected = std::min(count - 1, state.PopupSelected + kSuggestMaxRows);
					state.PopupScroll = std::max(0, std::min(
						std::max(0, count - kSuggestMaxRows), state.PopupSelected - kSuggestMaxRows / 2));
				}
			}
			// 接受:浮层可见时 Enter/Tab 优先给补全(不换行/不缩进)。
			// Tab 永远接受;Enter 只在"用户明确召唤过浮层"时接受(点号/冒号、Ctrl+Space 或
			// Up/Down 选过)——普通打字自动弹出的浮层里 Enter 保持换行语义。
			if (state.PopupVisible && ctx.WasKeyTriggered(KeyCodes::Enter) && !state.PopupAcceptEnter)
				state.PopupVisible = false;
			bool acceptedCompletion = false;
			if (state.PopupVisible
				&& (ctx.WasKeyTriggered(KeyCodes::Tab)
					|| (state.PopupAcceptEnter && ctx.WasKeyTriggered(KeyCodes::Enter))))
			{
				acceptedCompletion = true;
				const std::string& text = buffer.Text();
				const std::size_t caretNow = std::min(buffer.Caret(), text.size());
				const std::size_t replaceStart = std::min(state.ReplaceStart, caretNow);
				const int count = static_cast<int>(state.PopupItems.size());
				if (state.PopupSelected >= 0 && state.PopupSelected < count)
				{
					const World::LuauCompletionItem& item =
						state.PopupItems[static_cast<std::size_t>(state.PopupSelected)];
					const std::string insertName = CompletionInsertName(item.Name);
					std::string inserted = insertName;
					const bool call = InsertAsCall(item);
					// 光标后同行若已经是 '('(在已有调用里补名字)就只插名字;否则补完整的一对
					// "name()" 并把 caret 放进括号。修复:此前只在整个**文件末尾**才补 '(',
					// 文件中间接受函数会得到 "Name)"(用户实测 `self.OnCreate)`)。
					bool hasCallParens = false;
					for (std::size_t i = caretNow; i < text.size(); ++i)
					{
						if (text[i] == ' ' || text[i] == '\t')
							continue;
						hasCallParens = (text[i] == '(');
						break;
					}
					if (call && !hasCallParens)
					{
						inserted += "(";
						// W9.8:参数占位(---@param 顺序),接受后自动选中第一个参数。
						for (std::size_t i = 0; i < item.Params.size(); ++i)
						{
							if (i > 0)
								inserted += ", ";
							inserted += item.Params[i];
						}
					}
					buffer.SetCaret(replaceStart, false);
					buffer.SetCaret(caretNow, true);
					buffer.InsertAtCaret(inserted);
					if (call && !hasCallParens)
					{
						const std::size_t caretAfterName = buffer.Caret();
						buffer.InsertAtCaret(")");
						// 参数区起点 = replaceStart + name + '(';逐个记录 [start,end)。
						state.SnippetRanges.clear();
						state.SnippetIndex = 0;
						std::size_t paramCursor = replaceStart + insertName.size() + 1;
						for (std::size_t i = 0; i < item.Params.size(); ++i)
						{
							state.SnippetRanges.push_back(paramCursor);
							state.SnippetRanges.push_back(paramCursor + item.Params[i].size());
							paramCursor += item.Params[i].size() + (i + 1 < item.Params.size() ? 2 : 0);
						}
						if (state.SnippetRanges.size() >= 2)
						{
							buffer.SetCaret(state.SnippetRanges[0], false);
							buffer.SetCaret(state.SnippetRanges[1], true);   // 选中第一个参数
						}
						else
							buffer.SetCaret(caretAfterName, false);          // 无参数:caret 在括号内
					}
				}
				state.PopupVisible = false;
				state.FollowCaret = true;
			}
			// W9.8:参数占位导航 —— 仍选中参数时 Tab 跳到下一个参数;输入/点击/滚动/Esc 取消。
			if (!state.SnippetRanges.empty() && !acceptedCompletion)
			{
				if (ctx.WasKeyTriggered(KeyCodes::Tab))
				{
					popupConsumedKey = true;   // 不落到编辑分支当缩进
					const std::size_t count = state.SnippetRanges.size() / 2;
					++state.SnippetIndex;
					if (state.SnippetIndex < count)
					{
						buffer.SetCaret(state.SnippetRanges[state.SnippetIndex * 2], false);
						buffer.SetCaret(state.SnippetRanges[state.SnippetIndex * 2 + 1], true);
					}
					else
					{
						buffer.SetCaret(state.SnippetRanges.back(), false);
						state.SnippetRanges.clear();
					}
				}
				else if (!input.TextInput.empty() || escapeTriggered
					|| input.MouseClicked[0] || input.Wheel != 0.0f)
					state.SnippetRanges.clear();
			}
			// MAT-UI6a:查找条打开时 Esc = 关条并**保持代码区焦点**(再按一次才走原来的"失焦")。
			bool escapeHandledByFind = false;
			if (escapeTriggered && !escapeHandledByPopup && state.FindVisible)
			{
				state.FindVisible = false;
				state.FindReplaceMode = false;
				escapeHandledByFind = true;
			}
			if (escapeTriggered && !escapeHandledByPopup && !escapeHandledByFind && focused)
			{
				// Escape 失焦:后续输入回到引擎全局快捷键。
				ctx.SetFocus(0);
				ctx.SetTextInputActive(false);
				focused = false;
			}
			// 可见字符:始终输入(浮层打开时先关闭,随后由触发逻辑重新查询)。
			// 这保证普通打字永远不会被浮层吞掉(实测:多行注入含 '.' 时曾丢字符)。
			const bool popupConsuming = popupConsumedKey || acceptedCompletion;
			bool caretAction = false;
			// 普通编辑/导航键;被浮层消费的键(Up/Down/PageUp/PageDown/接受)不再落到 buffer。
			if (!popupConsuming)
			{
				const bool ctrl = input.Ctrl;
				const bool shift = input.Shift;
				const int pageLines = std::max(1, static_cast<int>(rect.H / lineHeight) - 1);
				if (ctrl)
				{
					if (ctx.WasKeyTriggered(KeyCodes::S) && !readOnly)
						result.SaveRequested = true;
					// MAT-UI6a:Ctrl+F / Ctrl+H 打开查找条(顶部覆盖条),焦点进查找输入框。
					// Ctrl+H(替换)只读模式不开;Ctrl+Shift+F 留给宿主(两个宿主都拿它当"格式化"),
					// 所以这里**不带 Shift** 才算查找。打开时关掉补全浮层,避免两层浮层叠着。
					if (!input.Shift && ctx.WasKeyTriggered(KeyCodes::F))
					{
						state.FindVisible = true;
						state.FindAnchor = std::min(buffer.Caret(), buffer.Text().size());
						state.PopupVisible = false;
						ctx.SetFocus(findFieldId);
					}
					if (!input.Shift && ctx.WasKeyTriggered(KeyCodes::H) && !readOnly)
					{
						state.FindVisible = true;
						state.FindReplaceMode = true;
						state.FindAnchor = std::min(buffer.Caret(), buffer.Text().size());
						state.PopupVisible = false;
						ctx.SetFocus(findFieldId);
					}
					// Ctrl+0:会话缩放复位到 1.0(= 基础字号回到偏好值;**偏好文件一律不写**)。
					if (ctx.WasKeyTriggered(KeyCodes::D0) && std::fabs(state.UiZoom - 1.0f) > 1e-6f)
					{
						state.UiZoom = 1.0f;
						result.ZoomChanged = true;
						result.UiZoom = 1.0f;
					}
					if (ctx.WasKeyTriggered(KeyCodes::A))
						buffer.SelectAll();
					if (ctx.WasKeyTriggered(KeyCodes::C) && options.GetClipboard)
					{
						std::string selection;
						if (buffer.Copy(selection) && options.SetClipboard)
							options.SetClipboard(selection);
					}
					if (!readOnly && ctx.WasKeyTriggered(KeyCodes::X))
					{
						std::string selection;
						if (buffer.Cut(selection) && options.SetClipboard)
							options.SetClipboard(selection);
						caretAction = true;
					}
					if (!readOnly && ctx.WasKeyTriggered(KeyCodes::V) && options.GetClipboard)
					{
						std::string clip;
						if (options.GetClipboard(clip))
							buffer.Paste(clip);
						caretAction = true;
					}
					if (!readOnly && ctx.WasKeyTriggered(KeyCodes::Z))
					{
						shift ? buffer.Redo() : buffer.Undo();
						caretAction = true;
					}
					if (!readOnly && ctx.WasKeyTriggered(KeyCodes::Y))
					{
						buffer.Redo();
						caretAction = true;
					}
				}
				// 编辑键;ReadOnly 只导航/选择/复制。
				if (!readOnly && !ctrl)
				{
					if (ctx.WasKeyTriggered(KeyCodes::Enter))
					{
						buffer.InsertNewline();
						caretAction = true;
					}
					if (ctx.WasKeyTriggered(KeyCodes::Backspace))
					{
						buffer.Backspace();
						caretAction = true;
					}
					if (ctx.WasKeyTriggered(KeyCodes::Delete))
					{
						buffer.DeleteForward();
						caretAction = true;
					}
					if (ctx.WasKeyTriggered(KeyCodes::Tab))
					{
						buffer.IndentSelection(shift);
						caretAction = true;
					}
				}
				// 导航键(ReadOnly 同样可用)。
				if (ctx.WasKeyTriggered(KeyCodes::Left))
				{
					buffer.MoveCaret(ctrl ? WuiTextBuffer::Motion::WordLeft : WuiTextBuffer::Motion::Left, shift);
					caretAction = true;
				}
				if (ctx.WasKeyTriggered(KeyCodes::Right))
				{
					buffer.MoveCaret(ctrl ? WuiTextBuffer::Motion::WordRight : WuiTextBuffer::Motion::Right, shift);
					caretAction = true;
				}
				if (ctx.WasKeyTriggered(KeyCodes::Up))
				{
					buffer.MoveCaret(WuiTextBuffer::Motion::Up, shift);
					caretAction = true;
				}
				if (ctx.WasKeyTriggered(KeyCodes::Down))
				{
					buffer.MoveCaret(WuiTextBuffer::Motion::Down, shift);
					caretAction = true;
				}
				if (ctx.WasKeyTriggered(KeyCodes::Home))
				{
					buffer.MoveCaret(WuiTextBuffer::Motion::LineStart, shift);
					caretAction = true;
				}
				if (ctx.WasKeyTriggered(KeyCodes::End))
				{
					buffer.MoveCaret(WuiTextBuffer::Motion::LineEnd, shift);
					caretAction = true;
				}
				if (ctx.WasKeyTriggered(KeyCodes::PageUp))
				{
					buffer.MoveCaret(WuiTextBuffer::Motion::PageUp, shift, pageLines);
					caretAction = true;
				}
				if (ctx.WasKeyTriggered(KeyCodes::PageDown))
				{
					buffer.MoveCaret(WuiTextBuffer::Motion::PageDown, shift, pageLines);
					caretAction = true;
				}
			}
			if (caretAction)
				state.FollowCaret = true;
		}
		else if (ctx.IsHovered(textRect))
		{
			ctx.SetCursor(WuiCursor::IBeam);
		}

		// ---- MAT-UI6a:查找/替换条(键路由 + 命中重扫;绘制在函数末尾的覆盖层)----
		// 焦点语义:打开时焦点落在查找输入框;Esc 关条并把焦点还给代码区;点条上的按钮不抢输入焦点。
		const bool findFieldFocused = ctx.Focus() == findFieldId;
		const bool findReplaceFocused = ctx.Focus() == findReplaceFieldId;
		const bool barFocused = findFieldFocused || findReplaceFocused;
		const auto SelectFindMatch = [&](int index)
		{
			if (index < 0 || index >= static_cast<int>(state.FindMatches.size()))
				return false;
			state.FindCurrent = index;
			const WuiCodeFindMatch& match = state.FindMatches[static_cast<size_t>(index)];
			buffer.SetCaret(match.Start, false);
			buffer.SetCaret(match.End, true);
			state.FindAnchor = match.Start;
			state.FollowCaret = true;
			return true;
		};
		// 命中重扫(节流):只在 查询文本 / Aa / ab / 文本版本 变化时重算 —— 大文件不逐帧全扫。
		const auto RefreshFindMatches = [&]()
		{
			if (!state.FindVisible)
				return;
			const std::string& text = buffer.Text();
			const uint64_t revision = buffer.Revision();
			const bool queryChanged = !state.FindScanned
				|| state.FindScannedQuery != state.FindQuery
				|| state.FindScannedCase != findCaseSetting
				|| state.FindScannedWord != findWordSetting;
			if (!queryChanged && state.FindScannedRevision == revision)
				return;
			// 重扫前的"当前命中起点"= 重扫后的搜索起点(查询变了就落到它之后的第一个命中)。
			size_t from = state.FindAnchor;
			if (state.FindCurrent >= 0 && state.FindCurrent < static_cast<int>(state.FindMatches.size()))
				from = state.FindMatches[static_cast<size_t>(state.FindCurrent)].Start;
			state.FindMatches = FindCodeMatches(text, state.FindQuery,
				WuiCodeFindOptions { findCaseSetting, findWordSetting });
			state.FindScanned = true;
			state.FindScannedQuery = state.FindQuery;
			state.FindScannedCase = findCaseSetting;
			state.FindScannedWord = findWordSetting;
			state.FindScannedRevision = revision;
			if (!queryChanged)
			{
				// 只是正文被编辑/撤销:不拽走 caret,只把当前下标夹回范围。
				state.FindCurrent = state.FindMatches.empty() ? -1 : std::max(0, std::min(
					state.FindCurrent, static_cast<int>(state.FindMatches.size()) - 1));
				return;
			}
			SelectFindMatch(NextCodeMatchIndex(state.FindMatches, from));
		};
		// 替换当前:走 WuiTextBuffer 的区间替换(单次撤销步)。替换后跳到替换点之后的第一个命中。
		const auto ReplaceCurrentMatch = [&]()
		{
			if (readOnly)
				return;
			RefreshFindMatches();
			if (state.FindCurrent < 0 || state.FindCurrent >= static_cast<int>(state.FindMatches.size()))
				return;
			const WuiCodeFindMatch match = state.FindMatches[static_cast<size_t>(state.FindCurrent)];
			if (!buffer.ReplaceRange(match.Start, match.End, state.FindReplaceQuery))
				return;
			state.FindScanned = false;   // 文本已变 → 强制重扫
			state.FindCurrent = -1;      // 从替换点重新挑当前命中
			state.FindAnchor = match.Start;
			RefreshFindMatches();
		};
		// 全部替换:一次区间替换 = **一步撤销**(Ctrl+Z 直接回到替换前)。
		const auto ReplaceAllMatches = [&]()
		{
			if (readOnly)
				return;
			RefreshFindMatches();
			if (state.FindMatches.empty())
				return;
			const std::string text = buffer.Text();
			const size_t spanStart = state.FindMatches.front().Start;
			const size_t spanEnd = state.FindMatches.back().End;
			std::string rewritten;
			rewritten.reserve(text.size());
			size_t cursor = spanStart;
			for (const WuiCodeFindMatch& match : state.FindMatches)
			{
				if (match.Start > cursor)
					rewritten.append(text, cursor, match.Start - cursor);
				rewritten.append(state.FindReplaceQuery);
				cursor = match.End;
			}
			if (cursor < spanEnd)
				rewritten.append(text, cursor, spanEnd - cursor);
			if (!buffer.ReplaceRange(spanStart, spanEnd, rewritten))
				return;
			state.FindScanned = false;
			state.FindCurrent = -1;
			state.FindAnchor = spanStart;
			RefreshFindMatches();
		};
		if (state.FindVisible)
		{
			// 条内再按 Ctrl+F / Ctrl+H:焦点回到查找输入框(不改查询、不关条)。
			if (input.Ctrl && !input.Shift && (ctx.WasKeyTriggered(KeyCodes::F)
				|| (ctx.WasKeyTriggered(KeyCodes::H) && !readOnly)))
				ctx.SetFocus(findFieldId);
			// 条内 Ctrl+Z/Ctrl+Y:撤销/重做**代码缓冲**(替换完不用先 Esc 再撤销;文本焦点在条里
			// 时宿主的场景撤销本来就让位,不会双重撤销)。
			if (!readOnly && barFocused && input.Ctrl && ctx.WasKeyTriggered(KeyCodes::Z))
			{
				if (input.Shift)
					buffer.Redo();
				else
					buffer.Undo();
				state.FollowCaret = true;
			}
			if (!readOnly && barFocused && input.Ctrl && ctx.WasKeyTriggered(KeyCodes::Y))
			{
				buffer.Redo();
				state.FollowCaret = true;
			}
			if (barFocused || focused)
			{
				const bool shift = input.Shift;
				bool navNext = false;
				bool navPrev = false;
				if (ctx.WasKeyTriggered(KeyCodes::F3))
					(shift ? navPrev : navNext) = true;
				else if (findFieldFocused && ctx.WasKeyTriggered(KeyCodes::Enter))
					(shift ? navPrev : navNext) = true;
				if (navNext || navPrev)
				{
					RefreshFindMatches();
					const auto [selStart, selEnd] = buffer.Selection();
					SelectFindMatch(navNext ? NextCodeMatchIndex(state.FindMatches, selEnd)
						: PrevCodeMatchIndex(state.FindMatches, selStart));
				}
			}
		}
		// 命中表在"依赖它的东西"(同词高亮 / 计数 / 绘制)之前刷新一次:查询/开关/文本版本没变
		// 时是纯命中缓存的 no-op。这样"查找选中的命中"与"同词高亮算的词"在同一帧内一致。
		RefreshFindMatches();

		// ---- MAT-UI6a:同词高亮(光标/选区落在标识符上 → 全文档同词;只在变化时重扫)----
		{
			const std::string& text = buffer.Text();
			size_t wordStart = 0;
			size_t wordEnd = 0;
			const auto [selStart, selEnd] = buffer.Selection();
			if (selEnd > selStart && IsIdentifierRange(text, selStart, selEnd))
			{
				wordStart = selStart;
				wordEnd = selEnd;
			}
			else
				WordAtOffset(text, buffer.Caret(), wordStart, wordEnd);
			if (wordEnd > wordStart)
			{
				const std::string word = text.substr(wordStart, wordEnd - wordStart);
				if (!state.OccurrenceScanned || state.OccurrenceScannedRevision != buffer.Revision()
					|| state.OccurrenceScannedWord != word || state.OccurrenceScannedCase != findCaseSetting)
				{
					// 与查找共用同一套匹配器;当前词本身一定整体是一个标识符 ⇒ WholeWord = true。
					state.OccurrenceMatches = FindCodeMatches(text, word,
						WuiCodeFindOptions { findCaseSetting, /*WholeWord=*/true });
					state.OccurrenceScanned = true;
					state.OccurrenceScannedRevision = buffer.Revision();
					state.OccurrenceScannedWord = word;
					state.OccurrenceScannedCase = findCaseSetting;
				}
				state.OccurrenceWord = word;
				state.OccurrenceWordStart = wordStart;
				state.OccurrenceWordEnd = wordEnd;
			}
			else
			{
				state.OccurrenceWord.clear();
				state.OccurrenceMatches.clear();
				state.OccurrenceScanned = false;
			}
			WuiAccessibility& accessibility = WuiAccessibility::Get();
			if (accessibility.Enabled() && !state.OccurrenceWord.empty())
			{
				WuiAccessNode node;
				node.Id = HashId("code-editor.occurrences");
				node.Window = accessibility.CurrentWindow();
				node.Panel = accessibility.CurrentPanel();
				node.Kind = "status";
				node.Label = "occurrences";
				node.Value = std::to_string(state.OccurrenceMatches.size()) + " occurrences: "
					+ state.OccurrenceWord;
				node.Rect = rect;
				node.Interactive = false;
				accessibility.Register(node);
			}
		}

		// ---- 可见字符:始终插入(浮层打开时先关闭,随后按新前缀重新查询)----
		// 注意顺序:必须在补全查询**之前**插入并刷新 caret,否则 linePrefix 是插入前的行内容。
		bool keepPopupOpen = false;
		bool textHadSeparator = false;
		bool typedVisible = false;
		if (focused && !readOnly && !input.Ctrl)
		{
			for (uint32_t codepoint : input.TextInput)
			{
				// Enter/Tab/Escape 的 char 事件走按键分支,这里只收可见字符。
				if (codepoint < 0x20 || codepoint == 0x7F)
					continue;
				std::string encoded;
				EncodeUtf8(encoded, codepoint);
				buffer.InsertAtCaret(encoded);
				typedVisible = true;
				if (codepoint == '.' || codepoint == ':')
					textHadSeparator = true;
			}
			if (typedVisible)
			{
				// 输入可见字符:浮层原本打开 → 按新前缀刷新;原本关闭 → 由下面的查询决定
				// 是否弹出(输入 ≥1 字符自动提示,无候选不弹——W9.5 方案 A)。
				keepPopupOpen = popupVisibleAtFrameStart;
				state.PopupVisible = false;
				state.FollowCaret = true;
			}
		}
		// 补全触发:Ctrl+Space 手动;'.'/':' 自动;普通可见字符也自动(方案 A)。
		if (focused && input.Ctrl && ctx.WasKeyTriggered(KeyCodes::Space))
			completionRequest = { true, true, true };
		else if (focused && (textHadSeparator || typedVisible || keepPopupOpen))
			completionRequest = { true, false, textHadSeparator };
		else if (focused && popupVisibleAtFrameStart
			&& (ctx.WasKeyTriggered(KeyCodes::Backspace) || ctx.WasKeyTriggered(KeyCodes::Delete)))
		{
			// 删除同样要刷新候选:否则删掉一个字母后列表还停在旧前缀的候选
			// (用户实测"输入一个字母后删除,提示还是原来的")。前缀变空/不再有意义时,
			// 下面的 meaningfulPrefix 检查会让列表自动关闭。
			completionRequest = { true, false, false };
		}

		// ---- W9.5 补全查询:字符串/注释内不弹(用高亮 token 判定,不数引号)----
		const int caretLine = buffer.LineOfOffset(buffer.Caret());
		size_t caretLineStart = 0;
		const std::string_view caretLineText = LineView(buffer, caretLine, caretLineStart);
		std::vector<WuiCodeToken> caretTokens;
		if (options.Highlight)
			options.Highlight(caretLineText, caretTokens);
		bool caretInStringOrComment = false;
		{
			const size_t localCaret = std::min(buffer.Caret(), caretLineStart + caretLineText.size())
				- caretLineStart;
			for (const WuiCodeToken& token : caretTokens)
			{
				if (token.Kind != WuiCodeTokenKind::String && token.Kind != WuiCodeTokenKind::Comment)
					continue;
				const size_t start = std::min<size_t>(token.StartByte, caretLineText.size());
				const size_t end = std::max(start, std::min<size_t>(token.EndByte, caretLineText.size()));
				if (localCaret > start && localCaret <= end)
				{
					caretInStringOrComment = true;
					break;
				}
			}
		}
		// 注解上下文:`---@` 之后没有空白——注释行里也要给 `---@class/@field/...` 标签候选。
		bool annotationContext = false;
		{
			const size_t localCaret = std::min(buffer.Caret(), caretLineStart + caretLineText.size())
				- caretLineStart;
			const std::string_view caretLinePrefixBytes = caretLineText.substr(0, localCaret);
			const std::size_t at = caretLinePrefixBytes.rfind("---@");
			if (at != std::string_view::npos)
			{
				annotationContext = true;
				for (const char c : caretLinePrefixBytes.substr(at + 4))
					if (!IsIdentByte(c))
						annotationContext = false;
			}
		}
		if (completionRequest.Wanted && focused && options.Completion && !readOnly
			&& (!caretInStringOrComment || annotationContext))
		{
			const std::size_t caretNow = std::min(buffer.Caret(), buffer.Text().size());
			const std::size_t replaceStart = CompletionReplaceStart(buffer.Text(), caretNow);
			std::string linePrefix;
			if (caretNow >= caretLineStart)
				linePrefix.assign(buffer.Text(), caretLineStart, caretNow - caretLineStart);
			std::vector<World::LuauCompletionItem> items;
			// 行首到现在的纯空白(空行/自动缩进)不查询:全量候选既没有信息量,又会让
			// 随后的 Enter 被当成"接受候选"(实测:注入多行文本时插入了 assert)。
			const bool meaningfulPrefix = !linePrefix.empty()
				&& (IsIdentByte(linePrefix.back()) || linePrefix.back() == '.' || linePrefix.back() == ':'
					|| annotationContext);
			if (meaningfulPrefix)
				options.Completion(linePrefix, items);
			if (!items.empty())
			{
				if (!state.PopupVisible)
				{
					// 打开/刷新时决定 Enter 语义:显式召唤(点号/冒号/Ctrl+Space)才接受;自动提示不吞 Enter。
					// 刷新(上一帧本已打开)时保留已经升级过的语义(例如打完 "." 再打字母)。
					state.PopupAcceptEnter = completionRequest.Explicit
						|| (popupVisibleAtFrameStart && state.PopupAcceptEnter);
					WLD_CORE_INFO("[wui] completion popup: {0} items, prefix '{1}'", items.size(), linePrefix);
				}
				state.PopupVisible = true;
				state.ReplaceStart = replaceStart;
				state.CaretAtOpen = caretNow;
				state.PopupLinePrefix = linePrefix;
				state.PopupItems = std::move(items);
				state.PopupSelected = 0;
				state.PopupScroll = 0;
				// 锚点固定在打开/刷新时的 caret 位置:避免浮层逐帧漂移导致鼠标命中错位。
				{
					size_t anchorLineStart = 0;
					const std::string_view anchorLineText = LineView(buffer, caretLine, anchorLineStart);
					const std::size_t anchorLocal = std::min(caretNow,
						anchorLineStart + anchorLineText.size()) - anchorLineStart;
					state.PopupAnchorX = textRect.X + ctx.MeasureTextWidth(
						anchorLineText.substr(0, anchorLocal), fontSize, WuiFontFamily::Monospace);
					state.PopupAnchorY = textRect.Y + static_cast<float>(caretLine) * lineHeight
						- state.ScrollY;
				}
			}
			else
			{
				state.PopupVisible = false;   // 空候选不弹
				state.PopupLinePrefix.clear();
			}
		}
		// ReadOnly / 未聚焦 / 无 provider / 光标落在字符串·注释内:一律不弹。
		if (state.PopupVisible && (!focused || readOnly || !options.Completion
			|| (caretInStringOrComment && !annotationContext)))
			state.PopupVisible = false;

		// ---- W9.6 悬停提示:同一标识符静止停留 ~0.4s → 名称/类型/文档 ----
		{
			constexpr uint64_t kHoverDelayFrames = 24;
			const bool inputQuiet = input.TextInput.empty() && input.KeyPressed.empty()
				&& input.KeyRepeated.empty() && input.Wheel == 0.0f;
			const bool candidate = options.Hover && ctx.IsHovered(textRect) && !state.PopupVisible
				&& inputQuiet && !state.MouseSelecting;
			size_t wordStart = 0;
			size_t wordEnd = 0;
			std::string linePrefix;
			std::string word;
			if (candidate)
			{
				const size_t hoverOffset = HitOffset(input.MousePos.y, input.MousePos.x);
				const int hoverLine = buffer.LineOfOffset(hoverOffset);
				size_t hoverLineStart = 0;
				const std::string_view hoverLineText = LineView(buffer, hoverLine, hoverLineStart);
				const size_t local = std::min(hoverOffset - hoverLineStart, hoverLineText.size());
				size_t start = local;
				while (start > 0)
				{
					const size_t prev = PrevBoundary(hoverLineText, start);
					size_t length = 1;
					if (!IsWordCodepoint(DecodeCodepoint(hoverLineText, prev, length)))
						break;
					start = prev;
				}
				size_t end = start;
				while (end < hoverLineText.size())
				{
					size_t length = 1;
					if (!IsWordCodepoint(DecodeCodepoint(hoverLineText, end, length)))
						break;
					end += length;
				}
				if (end > start && local >= start && local <= end)
				{
					wordStart = hoverLineStart + start;
					wordEnd = hoverLineStart + end;
					linePrefix.assign(buffer.Text(), hoverLineStart, start);
					word.assign(hoverLineText, start, end - start);
				}
			}
			if (!word.empty())
			{
				const bool sameWord = state.HoverWordStart == wordStart && state.HoverWordEnd == wordEnd;
				const bool samePos = std::fabs(input.MousePos.x - state.HoverMousePos.x) <= 4.0f
					&& std::fabs(input.MousePos.y - state.HoverMousePos.y) <= 4.0f;
				if (!sameWord || !samePos)
				{
					state.HoverSinceFrame = ctx.Frame();
					state.HoverWordStart = wordStart;
					state.HoverWordEnd = wordEnd;
					state.HoverMousePos = input.MousePos;
					state.HoverVisible = false;
				}
				else if (ctx.Frame() >= state.HoverSinceFrame + kHoverDelayFrames)
				{
					World::LuauCompletionItem item;
					if (options.Hover(linePrefix, word, item) && !item.Name.empty())
					{
						state.HoverName = item.Name;
						state.HoverType = item.Type;
						state.HoverDoc = item.Doc;
						state.HoverVisible = true;
					}
					else
						state.HoverVisible = false;
				}
			}
			else
			{
				state.HoverWordStart = state.HoverWordEnd = 0;
				state.HoverVisible = false;
			}
			if (!candidate)
				state.HoverVisible = false;
		}

		// ---- caret 跟随:滚动到刚移动/编辑的 caret 行 ----
		if (state.FollowCaret && maxScroll > 0.0f)
		{
			const float caretTop = static_cast<float>(buffer.LineOfOffset(buffer.Caret())) * lineHeight;
			// MAT-UI6a:查找条盖住顶部时,跳转到的命中不要在条下面藏着(关条时 barHeight = 0,
			// 与原逻辑逐字段相同)。
			const float barHeight = FindBarHeight(state.FindVisible, state.FindReplaceMode, readOnly);
			if (caretTop < state.ScrollY + barHeight)
				state.ScrollY = std::max(0.0f, caretTop - barHeight);
			else if (caretTop + lineHeight > state.ScrollY + rect.H)
				state.ScrollY = caretTop + lineHeight - rect.H;
		}
		state.FollowCaret = false;
		state.ScrollY = std::max(0.0f, std::min(state.ScrollY, maxScroll));

		// ---- W9.5 补全浮层:位置(空间不足翻到上方)/鼠标 hover·点击/绘制/无障碍 ----
		{
			const int itemCount = static_cast<int>(state.PopupItems.size());
			if (state.PopupVisible && itemCount > 0)
			{
				// 锚点:打开/刷新时固定的 caret 位置(不是当前 caret,浮层不逐帧漂移)。
				const float caretX = state.PopupAnchorX;
				const float caretTop = state.PopupAnchorY;
				const bool showDoc = state.PopupSelected >= 0 && state.PopupSelected < itemCount
					&& !state.PopupItems[static_cast<std::size_t>(state.PopupSelected)].Doc.empty();
				const int rows = std::min(itemCount, kSuggestMaxRows);
				const float width = std::max(180.0f, std::min(kSuggestWidth, textRect.W));
				const float height = static_cast<float>(rows) * kSuggestRowHeight
					+ (showDoc ? 24.0f : 6.0f) + 6.0f;
				float x = std::min(caretX, textRect.X + textRect.W - width);
				x = std::max(textRect.X, x);
				float y = caretTop + lineHeight;   // caret 下方
				if (y + height > rect.Y + rect.H)
					y = caretTop - height;         // 空间不足 → 翻到上方
				if (y < rect.Y)
					y = std::max(rect.Y, rect.Y + rect.H - height);
				state.PopupBounds = { x, y, width, height };

				// 滚轮在浮层内滚动列表(不滚编辑器)。
				// 浮层自己的命中一律走 HitTestRaw:浮层矩形是登记过的覆盖层,
				// 普通 IsHovered 在下一帧会被自己的遮挡区挡掉(见下面的绘制块)。
				if (ctx.HitTestRaw(state.PopupBounds, input.MousePos) && input.Wheel != 0.0f)
				{
					const int maxFirst = std::max(0, itemCount - kSuggestMaxRows);
					state.PopupSelected = std::max(0, std::min(itemCount - 1,
						state.PopupSelected - (input.Wheel > 0.0f ? 1 : -1)));
					state.PopupScroll = std::max(0, std::min(maxFirst,
						state.PopupSelected - kSuggestMaxRows / 2));
				}
				if (state.PopupScroll > itemCount - rows)
					state.PopupScroll = std::max(0, itemCount - rows);
				if (state.PopupScroll < 0)
					state.PopupScroll = 0;

				const float firstRowY = state.PopupBounds.Y + 3.0f;
				int hoveredItem = -1;
				for (int row = 0; row < rows; ++row)
				{
					const int itemIndex = state.PopupScroll + row;
					if (itemIndex >= itemCount)
						break;
					const WuiRect rowRect { state.PopupBounds.X,
						firstRowY + static_cast<float>(row) * kSuggestRowHeight,
						state.PopupBounds.W, kSuggestRowHeight };
					if (ctx.HitTestRaw(rowRect, input.MousePos))
						hoveredItem = itemIndex;
				}
				if (hoveredItem >= 0)
				{
					state.PopupSelected = hoveredItem;
					ctx.SetCursor(WuiCursor::Hand);
					if (input.MouseClicked[0])
					{
						// 点击接受(替换前缀,caret 落插入内容之后)。用浮层打开时记录的区间,
						// 而不是当前 caret:点击可能先被文本命中测试重新定位 caret
						// (自动化 ui.invoke 点击节点中心时实测会发生)。
						const World::LuauCompletionItem& item =
							state.PopupItems[static_cast<std::size_t>(state.PopupSelected)];
						const std::string& text = buffer.Text();
						const std::size_t caretNow = std::min(state.CaretAtOpen, text.size());
						const std::size_t replaceStart = std::min(state.ReplaceStart, caretNow);
						std::string inserted = CompletionInsertName(item.Name);
						const bool call = InsertAsCall(item);
						if (call && caretNow >= text.size())
							inserted += "(";
						buffer.SetCaret(replaceStart, false);
						buffer.SetCaret(caretNow, true);
						buffer.InsertAtCaret(inserted);
						if (call)
						{
							const std::size_t afterName = replaceStart + inserted.size();
							if (afterName > text.size() || text[afterName] != ')')
							{
								const std::size_t caretAfterName = buffer.Caret();
								buffer.InsertAtCaret(")");
								buffer.SetCaret(caretAfterName, false);
							}
						}
						state.PopupVisible = false;
						state.FollowCaret = true;
					}
				}
				if (state.PopupVisible)
				{
					// 绘制放在编辑区主体之后(函数末尾):否则会被正文背景/文本盖住
					// (用户实测:能 Tab 补全但看不到候选列表)。
					// 无障碍:仅浮层可见期间登记 <前缀>.<i>(Interactive,点击=接受)
					// 与 <前缀>.status(只读)。
					WuiAccessibility& accessibility = WuiAccessibility::Get();
					if (accessibility.Enabled())
					{
						const std::string prefix = options.CompletionIdPrefix.empty()
							? std::string("editor.suggest") : options.CompletionIdPrefix;
						for (int row = 0; row < rows; ++row)
						{
							const int itemIndex = state.PopupScroll + row;
							if (itemIndex >= itemCount)
								break;
							const World::LuauCompletionItem& item =
								state.PopupItems[static_cast<std::size_t>(itemIndex)];
							WuiAccessNode node;
							node.Id = HashId((prefix + "." + std::to_string(itemIndex)).c_str());
							node.Window = accessibility.CurrentWindow();
							node.Panel = accessibility.CurrentPanel();
							node.Kind = "suggest";
							node.Label = item.Name;
							node.Value = item.Type;
							node.Rect = { state.PopupBounds.X + 3.0f,
								firstRowY + static_cast<float>(row) * kSuggestRowHeight,
								state.PopupBounds.W - 6.0f, kSuggestRowHeight };
							node.Focused = itemIndex == state.PopupSelected;
							node.Interactive = true;
							accessibility.Register(node);
						}
						WuiAccessNode statusNode;
						statusNode.Id = HashId((prefix + ".status").c_str());
						statusNode.Window = accessibility.CurrentWindow();
						statusNode.Panel = accessibility.CurrentPanel();
						statusNode.Kind = "suggest";
						statusNode.Label = "completion status";
						statusNode.Value = "prefix '" + state.PopupLinePrefix + "', "
							+ std::to_string(itemCount) + " items, selected "
							+ (state.PopupSelected >= 0 && state.PopupSelected < itemCount
								? state.PopupItems[static_cast<std::size_t>(state.PopupSelected)].Name
								: std::string());
						statusNode.Rect = { state.PopupBounds.X, state.PopupBounds.Y,
							state.PopupBounds.W, state.PopupBounds.H };
						statusNode.Interactive = false;
						accessibility.Register(statusNode);
					}
				}
			}
			else
			{
				state.PopupVisible = false;
			}
			state.PopupVisibleLastFrame = state.PopupVisible;
		}

		// ---- 竖向滚动条:thumb 可拖拽,点击轨道翻页 ----
		float thumbHeight = 0.0f;
		float thumbY = 0.0f;
		if (needScrollbar)
		{
			thumbHeight = std::min(rect.H, std::max(kThumbMinHeight, rect.H * (rect.H / contentHeight)));
			float travel = std::max(1.0f, rect.H - thumbHeight);
			thumbY = rect.Y + (maxScroll > 0.0f ? state.ScrollY / maxScroll : 0.0f) * travel;
			const WuiRect thumb { track.X + 2.0f, thumbY, track.W - 4.0f, thumbHeight };
			if (input.MouseClicked[0] && ctx.IsHovered(track))
			{
				if (ctx.IsHovered(thumb))
				{
					state.DraggingThumb = true;
					state.ThumbGrabOffset = input.MousePos.y - thumbY;
				}
				else
				{
					state.ScrollY += (input.MousePos.y < thumbY ? -1.0f : 1.0f) * rect.H;
				}
			}
			if (state.DraggingThumb && input.MouseDown[0])
			{
				const float travelNow = std::max(1.0f, rect.H - thumbHeight);
				state.ScrollY = (input.MousePos.y - state.ThumbGrabOffset - rect.Y) / travelNow * maxScroll;
			}
			if (state.DraggingThumb && input.MouseReleased[0])
				state.DraggingThumb = false;
			state.ScrollY = std::max(0.0f, std::min(state.ScrollY, maxScroll));
			travel = std::max(1.0f, rect.H - thumbHeight);
			thumbY = rect.Y + (maxScroll > 0.0f ? state.ScrollY / maxScroll : 0.0f) * travel;
		}

		// ---- 绘制(只画可见行) ----
		const auto pushText = [&](float x, float y, const WuiColor& color, std::string text,
			int selStart, int selEnd, int caretByte)
		{
			WuiDrawCommand command;
			command.Kind = WuiDrawKind::Text;
			command.Rect = { x, y, 0.0f, 0.0f };
			command.Color = color;
			command.Text = std::move(text);
			command.FontSize = fontSize;
			command.Family = WuiFontFamily::Monospace;
			command.TextSelStart = selStart;
			command.TextSelEnd = selEnd;
			command.TextCaretByte = caretByte;
			ctx.Commands().push_back(std::move(command));
		};

		ctx.Commands().push_back({ WuiDrawKind::Rect, rect, kBackground, 0.0f });
		ctx.Commands().push_back({ WuiDrawKind::Rect, { rect.X, rect.Y, gutterWidth, rect.H }, kGutterBackground, 0.0f });

		const int drawCaretLine = focused ? buffer.LineOfOffset(buffer.Caret()) : -1;
		const int firstVisible = std::max(0, static_cast<int>(std::floor(state.ScrollY / lineHeight)));
		const int lastVisible = std::min(lineCount - 1,
			static_cast<int>(std::floor((state.ScrollY + rect.H) / lineHeight)) + 1);
		const float textPadY = (lineHeight - fontSize) * 0.5f;

		ctx.Commands().push_back({ WuiDrawKind::ClipPush, textRect, kBackground });
		std::vector<WuiCodeToken> tokens;
		const auto [selStart, selEnd] = buffer.Selection();
		const size_t caretOffset = buffer.Caret();
		for (int line = firstVisible; line <= lastVisible; ++line)
		{
			const float lineY = textRect.Y + static_cast<float>(line) * lineHeight - state.ScrollY;
			size_t lineStart = 0;
			const std::string_view lineView = LineView(buffer, line, lineStart);
			if (line == drawCaretLine)
				ctx.Commands().push_back({ WuiDrawKind::Rect, { textRect.X, lineY, textRect.W, lineHeight }, kCurrentLine, 0.0f });
			// W9.7 语法错误行:红色底 + 下划线(与当前行高亮叠加,顺序在后更醒目)。
			if (line == options.ErrorLine)
			{
				ctx.Commands().push_back({ WuiDrawKind::Rect, { textRect.X, lineY, textRect.W, lineHeight },
					{ 0.55f, 0.16f, 0.16f, 0.22f }, 0.0f });
				ctx.Commands().push_back({ WuiDrawKind::Rect,
					{ textRect.X, lineY + lineHeight - 2.0f, textRect.W, 2.0f },
					{ 0.85f, 0.25f, 0.25f, 0.9f }, 0.0f });
			}
			// MAT-UI6a:同词高亮 —— 当前词强一档、其余弱一档,画在正文之前(文字/选区仍然压在上面)。
			// 命中表按 Start 升序 + 已缓存,这里只挑与本可见行相交的那些(二分定位),不逐行扫全表。
			if (!state.OccurrenceMatches.empty())
			{
				const size_t lineAbsStart = lineStart;
				const size_t lineAbsEnd = lineStart + lineView.size();
				auto hit = std::lower_bound(state.OccurrenceMatches.begin(), state.OccurrenceMatches.end(),
					lineAbsStart,
					[](const WuiCodeFindMatch& match, size_t offset) { return match.End <= offset; });
				for (; hit != state.OccurrenceMatches.end() && hit->Start < lineAbsEnd; ++hit)
				{
					const size_t start = std::max(hit->Start, lineAbsStart);
					const size_t end = std::min(hit->End, lineAbsEnd);
					if (end <= start)
						continue;
					const float x1 = textRect.X + ctx.MeasureTextWidth(
						lineView.substr(0, start - lineAbsStart), fontSize, WuiFontFamily::Monospace);
					const float x2 = textRect.X + ctx.MeasureTextWidth(
						lineView.substr(0, end - lineAbsStart), fontSize, WuiFontFamily::Monospace);
					const bool current = hit->Start == state.OccurrenceWordStart
						&& hit->End == state.OccurrenceWordEnd;
					ctx.Commands().push_back({ WuiDrawKind::Rect, { x1, lineY, x2 - x1, lineHeight },
						current ? kOccurrenceStrong : kOccurrenceWeak, 0.0f });
				}
			}

			// 行号栏
			const std::string number = std::to_string(line + 1);
			const float numberWidth = ctx.MeasureTextWidth(number, fontSize, WuiFontFamily::Monospace);
			pushText(rect.X + gutterWidth - kGutterPaddingRight - numberWidth, lineY + textPadY,
				line == drawCaretLine ? kGutterTextCurrent : kGutterText, number, -1, -1, -1);

			// 语法分段:token 之间按 Default 补齐(不丢字符,也不打乱像素推进)。
			tokens.clear();
			if (options.Highlight)
				options.Highlight(lineView, tokens);
			std::sort(tokens.begin(), tokens.end(),
				[](const WuiCodeToken& a, const WuiCodeToken& b) { return a.StartByte < b.StartByte; });
			float pen = 0.0f;
			bool caretDrawn = false;
			const auto drawSegment = [&](size_t start, size_t end, WuiCodeTokenKind kind)
			{
				if (end <= start)
					return;
				const std::string_view segment = lineView.substr(start, end - start);
				const size_t absStart = lineStart + start;
				const size_t absEnd = lineStart + end;
				int localSelStart = -1;
				int localSelEnd = -1;
				if (selEnd > selStart && selStart < absEnd && selEnd > absStart)
				{
					const size_t s = std::max(selStart, absStart) - absStart;
					const size_t e = std::min(selEnd, absEnd) - absStart;
					if (e > s)
					{
						localSelStart = static_cast<int>(s);
						localSelEnd = static_cast<int>(e);
					}
				}
				int caretByte = -1;
				if (focused && line == drawCaretLine && !caretDrawn && caretOffset >= absStart && caretOffset <= absEnd)
				{
					caretByte = static_cast<int>(caretOffset - absStart);
					caretDrawn = true;
				}
				pushText(textRect.X + pen, lineY + textPadY, kTokenColors[static_cast<size_t>(kind)],
					std::string(segment), localSelStart, localSelEnd, caretByte);
				pen += ctx.MeasureTextWidth(segment, fontSize, WuiFontFamily::Monospace);
			};
			size_t cursor = 0;
			for (const WuiCodeToken& token : tokens)
			{
				const size_t tokenStart = std::min<size_t>(token.StartByte, lineView.size());
				const size_t tokenEnd = std::max(tokenStart, std::min<size_t>(token.EndByte, lineView.size()));
				// W9 review:重叠/乱序 token 只画尚未消费的部分,避免同一段重复绘制。
				const size_t start = std::max(tokenStart, cursor);
				if (start > cursor)
					drawSegment(cursor, start, WuiCodeTokenKind::Default);
				if (start < tokenEnd)
					drawSegment(start, tokenEnd, token.Kind);
				cursor = std::max(cursor, tokenEnd);
			}
			drawSegment(cursor, lineView.size(), WuiCodeTokenKind::Default);
			// 空行 / caret 在行尾:补一个空文本命令,让后端按真实度量画 caret。
			if (focused && line == drawCaretLine && !caretDrawn)
				pushText(textRect.X + pen, lineY + textPadY, kCaretColor, std::string(), -1, -1, 0);
			// MAT-UI45(纯追加):逐可见行装饰回调 —— 在正文之后、浮层(补全/Hover)之前,
			// 仍在文本区裁剪内。pen = 本行文本的度量右端(相对文本区左缘)。
			if (options.LineDecorator)
				options.LineDecorator(ctx, line, { textRect.X, lineY, textRect.W, lineHeight },
					textRect.X + pen);
		}
		ctx.Commands().push_back({ WuiDrawKind::ClipPop });

		if (needScrollbar)
		{
			ctx.Commands().push_back({ WuiDrawKind::Rect, track, kScrollbarTrack, 0.0f });
			ctx.Commands().push_back({ WuiDrawKind::Rect,
				{ track.X + 2.0f, thumbY, track.W - 4.0f, thumbHeight }, kScrollbarThumb, 2.0f });
		}

		// ---- W9.5 补全浮层绘制:必须在编辑区主体之后(顺序 = 绘制顺序,先画会被正文盖掉) ----
		if (state.PopupVisible && !state.PopupItems.empty())
		{
			const int itemCount = static_cast<int>(state.PopupItems.size());
			const int rows = std::min(itemCount, kSuggestMaxRows);
			const bool showDoc = state.PopupSelected >= 0 && state.PopupSelected < itemCount
				&& !state.PopupItems[static_cast<std::size_t>(state.PopupSelected)].Doc.empty();
			const float firstRowY = state.PopupBounds.Y + 3.0f;
			ctx.Commands().push_back({ WuiDrawKind::Rect, state.PopupBounds, kSuggestBackground, 4.0f });
			ctx.Commands().push_back({ WuiDrawKind::RectOutline, state.PopupBounds, kSuggestBorder, 4.0f, 1.0f });
			// P4-U7:补全浮层是画在正文之上的覆盖层 —— 登记矩形,下一帧落在浮层上的
			// 点击不再同时落到底下的把文本区(实测:点候选行会同时移动 caret)。
			ctx.RegisterOverlayRect(state.PopupBounds);
			for (int row = 0; row < rows; ++row)
			{
				const int itemIndex = state.PopupScroll + row;
				if (itemIndex >= itemCount)
					break;
				const World::LuauCompletionItem& item =
					state.PopupItems[static_cast<std::size_t>(itemIndex)];
				const WuiRect rowRect { state.PopupBounds.X + 3.0f,
					firstRowY + static_cast<float>(row) * kSuggestRowHeight,
					state.PopupBounds.W - 6.0f, kSuggestRowHeight };
				if (itemIndex == state.PopupSelected)
					ctx.Commands().push_back({ WuiDrawKind::Rect, rowRect, kSuggestSelected, 3.0f });
				const float textY = rowRect.Y + (kSuggestRowHeight - kSuggestFontSize) * 0.5f;
				{
					WuiDrawCommand nameCommand;
					nameCommand.Kind = WuiDrawKind::Text;
					nameCommand.Rect = { rowRect.X + 8.0f, textY, 0.0f, 0.0f };
					nameCommand.Color = kSuggestName;
					nameCommand.Text = TruncateBytes(item.Name, 34);
					nameCommand.FontSize = kSuggestFontSize;
					nameCommand.Family = WuiFontFamily::Monospace;
					ctx.Commands().push_back(std::move(nameCommand));
				}
				if (!item.Type.empty())
				{
					const float typeWidth = ctx.MeasureTextWidth(item.Type, kSuggestFontSize,
						WuiFontFamily::Monospace);
					WuiDrawCommand typeCommand;
					typeCommand.Kind = WuiDrawKind::Text;
					typeCommand.Rect = { rowRect.X + rowRect.W - 8.0f - typeWidth, textY, 0.0f, 0.0f };
					typeCommand.Color = kSuggestType;
					typeCommand.Text = TruncateBytes(item.Type, 24);
					typeCommand.FontSize = kSuggestFontSize;
					typeCommand.Family = WuiFontFamily::Monospace;
					ctx.Commands().push_back(std::move(typeCommand));
				}
			}
			if (showDoc)
			{
				const World::LuauCompletionItem& item =
					state.PopupItems[static_cast<std::size_t>(state.PopupSelected)];
				ctx.Commands().push_back({ WuiDrawKind::Rect,
					{ state.PopupBounds.X + 1.0f,
						state.PopupBounds.Y + state.PopupBounds.H - 25.0f,
						state.PopupBounds.W - 2.0f, 24.0f },
					{ 0.10f, 0.11f, 0.12f, 0.98f }, 0.0f });
				WuiDrawCommand docCommand;
				docCommand.Kind = WuiDrawKind::Text;
				docCommand.Rect = { state.PopupBounds.X + 8.0f,
					state.PopupBounds.Y + state.PopupBounds.H - 21.0f, 0.0f, 0.0f };
				docCommand.Color = kSuggestDoc;
				docCommand.Text = EllipsizeToWidth(ctx, item.Doc, state.PopupBounds.W - 16.0f,
					kSuggestDocFontSize);
				docCommand.FontSize = kSuggestDocFontSize;
				docCommand.Family = WuiFontFamily::Ui;
				ctx.Commands().push_back(std::move(docCommand));
			}
		}

		// ---- W9.6 悬停提示绘制(同样必须在正文之后) ----
		if (state.HoverVisible && !state.HoverName.empty())
		{
			constexpr float kHoverFontSize = 13.0f;
			constexpr float kHoverTitleSize = 14.0f;
			constexpr float kHoverLineH = 16.0f;
			const float maxTextWidth = std::min(400.0f, std::max(160.0f, rect.W - 24.0f));
			const std::vector<std::string> docLines = state.HoverDoc.empty()
				? std::vector<std::string> {}
				: WrapToWidth(ctx, state.HoverDoc, maxTextWidth, kHoverFontSize, 4);
			const float width = std::min(maxTextWidth + 16.0f, rect.W);
			const float height = 8.0f + kHoverLineH
				+ (state.HoverType.empty() ? 0.0f : kHoverLineH)
				+ static_cast<float>(docLines.size()) * kHoverLineH + 4.0f;
			float x = input.MousePos.x + 14.0f;
			float y = input.MousePos.y + 18.0f;
			if (x + width > rect.X + rect.W)
				x = input.MousePos.x - width - 8.0f;
			if (y + height > rect.Y + rect.H)
				y = input.MousePos.y - height - 8.0f;
			x = std::max(rect.X, std::min(x, rect.X + rect.W - width));
			y = std::max(rect.Y, std::min(y, rect.Y + rect.H - height));
			const WuiRect tooltip { x, y, width, height };
			ctx.Commands().push_back({ WuiDrawKind::Rect, tooltip, { 0.10f, 0.11f, 0.13f, 0.98f }, 3.0f });
			ctx.Commands().push_back({ WuiDrawKind::RectOutline, tooltip, kSuggestBorder, 3.0f, 1.0f });
			float lineY = tooltip.Y + 4.0f;
			{
				WuiDrawCommand title;
				title.Kind = WuiDrawKind::Text;
				title.Rect = { tooltip.X + 8.0f, lineY, 0.0f, 0.0f };
				title.Color = kSuggestName;
				title.Text = EllipsizeToWidth(ctx, state.HoverName, maxTextWidth, kHoverTitleSize);
				title.FontSize = kHoverTitleSize;
				title.Family = WuiFontFamily::Monospace;
				ctx.Commands().push_back(std::move(title));
			}
			lineY += kHoverLineH;
			if (!state.HoverType.empty())
			{
				WuiDrawCommand type;
				type.Kind = WuiDrawKind::Text;
				type.Rect = { tooltip.X + 8.0f, lineY, 0.0f, 0.0f };
				type.Color = kSuggestType;
				type.Text = EllipsizeToWidth(ctx, state.HoverType, maxTextWidth, kHoverFontSize);
				type.FontSize = kHoverFontSize;
				type.Family = WuiFontFamily::Monospace;
				ctx.Commands().push_back(std::move(type));
				lineY += kHoverLineH;
			}
			for (const std::string& docLine : docLines)
			{
				WuiDrawCommand doc;
				doc.Kind = WuiDrawKind::Text;
				doc.Rect = { tooltip.X + 8.0f, lineY, 0.0f, 0.0f };
				doc.Color = kSuggestDoc;
				doc.Text = docLine;
				doc.FontSize = kHoverFontSize;
				doc.Family = WuiFontFamily::Ui;
				ctx.Commands().push_back(std::move(doc));
				lineY += kHoverLineH;
			}
			WuiAccessibility& accessibility = WuiAccessibility::Get();
			if (accessibility.Enabled())
			{
				const std::string prefix = options.CompletionIdPrefix.empty()
					? std::string("editor.suggest") : options.CompletionIdPrefix;
				WuiAccessNode node;
				node.Id = HashId((prefix + ".hover").c_str());
				node.Window = accessibility.CurrentWindow();
				node.Panel = accessibility.CurrentPanel();
				node.Kind = "hover";
				node.Label = state.HoverName;
				node.Value = state.HoverType.empty() ? state.HoverDoc
					: state.HoverType + (state.HoverDoc.empty() ? "" : " | " + state.HoverDoc);
				node.Rect = tooltip;
				node.Interactive = false;
				accessibility.Register(node);
			}
		}

		// ---- MAT-UI6a:查找/替换条绘制(顶部覆盖条,画在正文/浮层之后)----
		// 打开时登记遮挡矩形:条上的点击不再穿透到正文(改 caret),条内控件自己不受影响
		// (PushOverlay 后登记的遮挡深度 = 1,覆盖层内部的命中不被自己的遮挡区挡掉)。
		if (state.FindVisible)
		{
			RefreshFindMatches();   // 绘制前再刷一次:本帧画出来的计数/命中就是这一帧的事实
			const bool replaceRow = state.FindReplaceMode && !readOnly;
			const float barHeight = FindBarHeight(true, state.FindReplaceMode, readOnly);
			const WuiRect barRect { rect.X, rect.Y, rect.W, std::min(barHeight, rect.H) };
			const WuiTheme& theme = CurrentTheme();
			// 行内布局:查找框 + 计数 + Aa/ab + 上/下跳 + 关闭;替换模式多一行(替换框 + Replace + All)。
			const float fixedWidth = 44.0f + 24.0f + 24.0f + 22.0f + 22.0f + 20.0f + kFindBarGap * 5.0f;
			const float fieldWidth = std::max(48.0f, std::min(180.0f,
				rect.W - fixedWidth - kFindBarPad * 2.0f - kFindBarGap * 2.0f));
			const float rowY = barRect.Y + kFindBarPad;
			float x = barRect.X + kFindBarPad;
			const WuiRect findFieldRect { x, rowY, fieldWidth, kFindBarRowHeight };
			x += fieldWidth + kFindBarGap;
			const WuiRect statusRect { x, rowY, 44.0f, kFindBarRowHeight };
			x += 44.0f + kFindBarGap;
			const WuiRect caseRect { x, rowY, 24.0f, kFindBarRowHeight };
			x += 24.0f + kFindBarGap;
			const WuiRect wordRect { x, rowY, 24.0f, kFindBarRowHeight };
			x += 24.0f + kFindBarGap;
			const WuiRect prevRect { x, rowY, 22.0f, kFindBarRowHeight };
			x += 22.0f + kFindBarGap;
			const WuiRect nextRect { x, rowY, 22.0f, kFindBarRowHeight };
			x += 22.0f + kFindBarGap;
			const WuiRect closeRect { x, rowY, 20.0f, kFindBarRowHeight };
			const float row2Y = rowY + kFindBarRowHeight + kFindBarGap;
			const WuiRect replaceFieldRect { barRect.X + kFindBarPad, row2Y, fieldWidth, kFindBarRowHeight };
			const WuiRect replaceRect { replaceFieldRect.X + fieldWidth + kFindBarGap, row2Y, 62.0f, kFindBarRowHeight };
			const WuiRect replaceAllRect { replaceRect.X + 62.0f + kFindBarGap, row2Y, 34.0f, kFindBarRowHeight };

			// 先算按钮点击:开关本帧就生效(绘制用新状态),且都不抢输入焦点。
			const auto barHit = [&](const WuiRect& r)
			{
				return ctx.HitTestRaw(r, input.MousePos) && input.MouseClicked[0];
			};
			const bool caseClicked = barHit(caseRect);
			const bool wordClicked = barHit(wordRect);
			const bool prevClicked = barHit(prevRect);
			const bool nextClicked = barHit(nextRect);
			const bool closeClicked = barHit(closeRect);
			const bool replaceClicked = replaceRow && barHit(replaceRect);
			const bool replaceAllClicked = replaceRow && barHit(replaceAllRect);
			if (caseClicked)
				findCaseSetting = !findCaseSetting;
			if (wordClicked)
				findWordSetting = !findWordSetting;
			if (closeClicked)
			{
				state.FindVisible = false;
				state.FindReplaceMode = false;
				ctx.SetFocus(id);
			}
			if (state.FindVisible)
			{
				ctx.PushOverlay();
				ctx.RegisterOverlayRect(barRect);
				// 条本身也要裁剪在编辑器矩形内(窄面板下按钮行不会溢出到相邻面板)。
				ctx.Commands().push_back({ WuiDrawKind::ClipPush, rect, kFindBarBackground });
				ctx.Commands().push_back({ WuiDrawKind::Rect, barRect, kFindBarBackground, 0.0f });
				ctx.Commands().push_back({ WuiDrawKind::Rect,
					{ barRect.X, barRect.Y + barRect.H - 1.0f, barRect.W, 1.0f }, kSuggestBorder, 0.0f });

				const TextFieldA11y findA11y { "Find", "Search in this code" };
				bool findCancelled = false;
				const bool findSubmitted = TextField(ctx, findFieldId, findFieldRect, state.FindQuery,
					theme, &findCancelled, &findA11y);
				bool replaceSubmitted = false;
				bool replaceCancelled = false;
				if (replaceRow)
				{
					const TextFieldA11y replaceA11y { "Replace", "Replace with" };
					replaceSubmitted = TextField(ctx, findReplaceFieldId, replaceFieldRect,
						state.FindReplaceQuery, theme, &replaceCancelled, &replaceA11y);
				}
				// 计数 n/m(查询非空但零命中时用另一档颜色提示"没找到")。
				const int matchCount = static_cast<int>(state.FindMatches.size());
				const int currentOrdinal = (state.FindCurrent >= 0 && state.FindCurrent < matchCount)
					? state.FindCurrent + 1 : 0;
				const std::string statusText = std::to_string(currentOrdinal) + "/"
					+ std::to_string(matchCount);
				{
					WuiDrawCommand status;
					status.Kind = WuiDrawKind::Text;
					const float width = ctx.MeasureTextWidth(statusText, kFindBarFontSize, WuiFontFamily::Ui);
					status.Rect = { statusRect.X + statusRect.W - width - 2.0f,
						statusRect.Y + (statusRect.H - kFindBarFontSize) * 0.5f, 0.0f, 0.0f };
					status.Color = (!state.FindQuery.empty() && matchCount == 0)
						? kFindBarNoMatch : kFindBarText;
					status.Text = statusText;
					status.FontSize = kFindBarFontSize;
					ctx.Commands().push_back(std::move(status));
				}
				WuiAccessibility& accessibility = WuiAccessibility::Get();
				if (accessibility.Enabled())
				{
					WuiAccessNode node;
					node.Id = HashId("code-editor.find.status");
					node.Window = accessibility.CurrentWindow();
					node.Panel = accessibility.CurrentPanel();
					node.Kind = "status";
					node.Label = "find status";
					node.Value = statusText;
					node.Rect = statusRect;
					node.Interactive = false;
					accessibility.Register(node);
				}
				const auto barButton = [&](const WuiRect& r, const std::string& label, WuiId buttonId,
					const std::string& kind, const std::string& value, bool active)
				{
					const bool hovered = ctx.HitTestRaw(r, input.MousePos);
					ctx.Commands().push_back({ WuiDrawKind::Rect, r,
						active ? kFindBarButtonActive : (hovered ? kFindBarButtonHover : kFindBarButton), 3.0f });
					ctx.Commands().push_back({ WuiDrawKind::RectOutline, r, kSuggestBorder, 3.0f, 1.0f });
					WuiDrawCommand text;
					text.Kind = WuiDrawKind::Text;
					const float width = ctx.MeasureTextWidth(label, kFindBarFontSize, WuiFontFamily::Ui);
					text.Rect = { r.X + (r.W - width) * 0.5f,
						r.Y + (r.H - kFindBarFontSize) * 0.5f, 0.0f, 0.0f };
					text.Color = active ? kFindBarTextActive : kFindBarText;
					text.Text = label;
					text.FontSize = kFindBarFontSize;
					ctx.Commands().push_back(std::move(text));
					if (hovered)
						ctx.SetCursor(WuiCursor::Hand);
					if (accessibility.Enabled())
					{
						WuiAccessNode node;
						node.Id = buttonId;
						node.Window = accessibility.CurrentWindow();
						node.Panel = accessibility.CurrentPanel();
						node.Kind = kind;
						node.Label = label;
						node.Value = value;
						node.Rect = r;
						node.Interactive = true;
						accessibility.Register(node);
					}
				};
				barButton(caseRect, "Aa", HashId("code-editor.find.case"), "toggle",
					findCaseSetting ? "on" : "off", findCaseSetting);
				barButton(wordRect, "ab", HashId("code-editor.find.word"), "toggle",
					findWordSetting ? "on" : "off", findWordSetting);
				barButton(prevRect, "<", HashId("code-editor.find.prev"), "button", std::string(), false);
				barButton(nextRect, ">", HashId("code-editor.find.next"), "button", std::string(), false);
				barButton(closeRect, "X", HashId("code-editor.find.close"), "button", std::string(), false);
				if (replaceRow)
				{
					barButton(replaceRect, "Replace", HashId("code-editor.find.replace"), "button",
						std::string(), false);
					barButton(replaceAllRect, "All", HashId("code-editor.find.replace_all"), "button",
						std::string(), false);
				}
				ctx.Commands().push_back({ WuiDrawKind::ClipPop });
				ctx.PopOverlay();
				// 焦点收尾:TextField 的"点外失焦"会把"点条上按钮/点回代码区"的焦点置 0,这里纠正回来。
				if (findSubmitted)
					ctx.SetFocus(findFieldId);          // 回车留在查找框里(可以连按找下一个)
				if (findCancelled || replaceCancelled)
				{
					state.FindVisible = false;
					state.FindReplaceMode = false;
					ctx.SetFocus(id);                   // Esc:关条 + 焦点还给代码区
				}
				else if (input.MouseClicked[0])
				{
					if (ctx.HitTestRaw(barRect, input.MousePos))
					{
						if (ctx.Focus() == 0)
							ctx.SetFocus(findFieldId);
					}
					else if (id != 0 && ctx.Focus() == 0 && ctx.HitTestRaw(rect, input.MousePos))
						ctx.SetFocus(id);
				}
				// 按钮动作(与键盘同一条函数)。
				if (nextClicked || prevClicked)
				{
					RefreshFindMatches();
					const auto [selStart, selEnd] = buffer.Selection();
					SelectFindMatch(nextClicked ? NextCodeMatchIndex(state.FindMatches, selEnd)
						: PrevCodeMatchIndex(state.FindMatches, selStart));
				}
				if (replaceClicked || replaceSubmitted)
					ReplaceCurrentMatch();
				if (replaceAllClicked)
					ReplaceAllMatches();
				if (replaceSubmitted)
					ctx.SetFocus(findReplaceFieldId);
			}
		}

		// P1c-E4:编辑器本体进焦点表 —— 旧实现只有"点进去"(鼠标)才 SetFocus,键盘/AI 到不了它
		// (a11y 节点一直存在、focused 也一直报,但焦点表里没有入口 → Tab 永远扫不到)。
		// 焦点环沿用全局 token(overlay 层);文本焦点持焦后 Tab 仍是编辑器自己的缩进(见 U2e 口径),
		// 退出用 Escape。
		if (id != 0)
		{
			ctx.RegisterFocusable(id, rect);
			DrawFocusRing(ctx, rect, id, CurrentTheme());
		}
		result.Changed = buffer.Revision() != revisionAtStart;
		return result;
	}
}
