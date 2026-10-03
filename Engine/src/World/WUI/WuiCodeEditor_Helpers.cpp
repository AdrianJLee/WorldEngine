#include "WuiCodeEditor_Internal.h"

namespace World::Wui
{

using namespace WuiCodeEditorDetail;

namespace WuiCodeEditorDetail
{

		// MAT-UI6a:缩放的唯一夹取口径(NaN / 非正数一律当 1.0,避免把字号画成 NaN)。
float ClampZoom(float zoom){
			if (!(zoom > 0.0f))
				return 1.0f;
			return std::max(kCodeEditorZoomMin, std::min(kCodeEditorZoomMax, zoom));
		}


		// MAT-UI6a:查找条高度(单行 = 查找;替换模式 = 查找 + 替换两行)。0 = 未打开。
float FindBarHeight(bool visible, bool replaceMode, bool readOnly){
			if (!visible)
				return 0.0f;
			const float rows = (replaceMode && !readOnly) ? 2.0f : 1.0f;
			return kFindBarPad * 2.0f + kFindBarRowHeight * rows + (rows > 1.0f ? kFindBarGap : 0.0f);
		}


size_t CodepointLength(unsigned char lead){
			if (lead >= 0xF0)
				return 4;
			if (lead >= 0xE0)
				return 3;
			if (lead >= 0xC0)
				return 2;
			return 1;
		}


uint32_t DecodeCodepoint(std::string_view text, size_t offset, size_t& length){
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


void EncodeUtf8(std::string& out, uint32_t codepoint){
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


bool IsWordCodepoint(uint32_t codepoint){
			return codepoint >= 0x80
				|| (codepoint >= '0' && codepoint <= '9')
				|| (codepoint >= 'A' && codepoint <= 'Z')
				|| (codepoint >= 'a' && codepoint <= 'z')
				|| codepoint == '_';
		}


		// 补全前缀标识符字符(与 Luau 标识符一致的 ASCII 子集)。
bool IsIdentByte(char c){
			return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z')
				|| (c >= 'a' && c <= 'z') || c == '_';
		}


bool IsBlankByte(char c){
			return c == ' ' || c == '\t';
		}


		// 注解上下文:`---@` 之后光标仍在标签名或注解语法的"类型位"上。
		// 标签位 = `---@` / `---@fie`(候选是标签,由索引给)。
		// 类型位(与 LuauCompletion::IsAnnotationTypePosition 同一语法口径):
		//   `---@type <partial>`、`---@field <名字> <partial>`、`---@param <名字> <partial>`、
		//   `---@class <名字> : <partial>`(冒号连写 `名字: ` 也算)。
		// 字段名位/说明文字位不算:注释里不能冒出全局候选。
bool IsAnnotationContextBody(std::string_view body){
			std::size_t tokenStart = body.size();
			while (tokenStart > 0 && IsIdentByte(body[tokenStart - 1]))
				--tokenStart;
			if (tokenStart < body.size() && !IsIdentByte(body[tokenStart]))
				return false;   // 光标前既不是标识符也不是空白/行首(如 `---@field Speed.`)
			if (tokenStart == 0)
				return true;    // 标签名位置:`---@` / `---@fie`

			const std::string_view head = body.substr(0, tokenStart);
			std::vector<std::string_view> tokens;
			std::size_t cursor = 0;
			while (cursor < head.size())
			{
				while (cursor < head.size() && IsBlankByte(head[cursor]))
					++cursor;
				const std::size_t start = cursor;
				while (cursor < head.size() && !IsBlankByte(head[cursor]))
					++cursor;
				if (cursor > start)
					tokens.push_back(head.substr(start, cursor - start));
			}
			if (tokens.empty())
				return false;
			const std::string_view tag = tokens.front();
			if (tag == "class")
			{
				// `---@class Name : ` 或 `---@class Name: ` 之后才是继承类型位。
				if (tokens.size() == 2 && tokens[1].size() > 1 && tokens[1].back() == ':')
					return true;
				return tokens.size() == 3 && tokens[2] == ":";
			}
			if (tag != "type" && tag != "field" && tag != "param")
				return false;
			const std::size_t expectedTokens = (tag == "type") ? 1u : 2u;
			return tokens.size() == expectedTokens;
		}


		// 接受补全时的插入形态:方法/函数插 name() 且 caret 落括号内;其余插 name。
bool InsertAsCall(const World::LuauCompletionItem& item){
			if (item.Kind == World::LuauCompletionItem::KindType::Method)
				return true;
			return item.Type.find("fun") != std::string::npos
				|| item.Name.find('(') != std::string::npos;
		}


		// 浮层里的名字去掉 "name(...)" 写法,只留标识符(插入时按需补 "()")。
std::string CompletionInsertName(std::string name){
			const std::size_t paren = name.find('(');
			if (paren != std::string::npos)
				name.erase(paren);
			return name;
		}


		// 前缀不足时截断到字节预算(UTF-8 边界;不做精确像素裁剪)。
std::string TruncateBytes(const std::string& text, std::size_t maxBytes){
			if (text.size() <= maxBytes)
				return text;
			std::size_t cut = maxBytes;
			while (cut > 0 && (static_cast<unsigned char>(text[cut]) & 0xC0) == 0x80)
				--cut;
			return text.substr(0, cut) + "…";
		}


		// 真实度量的像素 → 行内字节偏移(命中测试,替代旧的 CursorAtX 启发式)。
size_t OffsetAtX(const WuiContext& ctx, std::string_view line, float x, float fontSize){
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


void SelectWordAt(WuiTextBuffer& buffer, const std::string& text, size_t offset){
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


std::string_view LineView(const WuiTextBuffer& buffer, int line, size_t& lineStart){
			const auto [start, end] = buffer.LineRange(line);
			lineStart = start;
			std::string_view view(buffer.Text().data() + start, end - start);
			if (!view.empty() && view.back() == '\r')
				view.remove_suffix(1);
			return view;
		}


		// 前缀起点:caret 往回走连续标识符字符(不含 '.'/':');紧邻非标识符时返回 caret
		// (空前缀,如刚敲下 '.')。
std::size_t CompletionReplaceStart(std::string_view text, std::size_t caret){
			caret = std::min(caret, text.size());
			std::size_t start = caret;
			while (start > 0 && IsIdentByte(text[start - 1]))
				--start;
			return start;
		}


		// 上一个码点边界(string_view 版:悬停取词用,避免为每帧扫描构造 std::string)。
std::size_t PrevBoundary(std::string_view text, std::size_t offset){
			if (offset == 0)
				return 0;
			std::size_t i = offset - 1;
			while (i > 0 && (static_cast<unsigned char>(text[i]) & 0xC0u) == 0x80u)
				--i;
			return i;
		}


		// W9.6 悬停/文档:按像素宽度截断(超宽补 '…')。
std::string EllipsizeToWidth(const WuiContext& ctx, std::string_view text, float maxWidth, float fontSize){
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
std::vector<std::string> WrapToWidth(const WuiContext& ctx, std::string_view text, float maxWidth, float fontSize, size_t maxLines){
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

}
}
