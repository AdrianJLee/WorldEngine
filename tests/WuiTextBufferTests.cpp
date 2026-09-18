// W9-1:WuiTextBuffer(编辑内核)+ WuiCodeEditor(控件)headless 回归。
// 覆盖:UTF-8 码点边界、CRLF 行索引、编辑/撤销分组/上限、导航软列、真实度量命中的
// 假度量注入(含 CJK 2× 宽度)、绘制命令(行号/选区/TextCaretByte)、10k 行性能。

#include "World/WUI/WuiTextBuffer.h"
#include "World/WUI/WuiCodeEditor.h"
#include "World/WUI/WuiContext.h"
#include "World/Core/KeyCodes.h"

#include <chrono>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace
{
	using namespace World::Wui;

	void Check(bool condition, const char* expression, int line)
	{
		if (!condition)
			throw std::runtime_error(std::string("line ") + std::to_string(line) + ": " + expression);
	}
#define CHECK(expression) Check(static_cast<bool>(expression), #expression, __LINE__)

	int g_MeasureOwner = 0;

	// 假度量:ASCII 0.5em,CJK(非 ASCII)1.0em —— 即 CJK 是 ASCII 的 2 倍宽。
	void InstallFakeMeasure()
	{
		SetTextMeasureHook(&g_MeasureOwner, [](std::string_view text, float fontSize, WuiFontFamily)
		{
			float width = 0.0f;
			size_t i = 0;
			while (i < text.size())
			{
				const unsigned char lead = static_cast<unsigned char>(text[i]);
				size_t length = 1;
				if (lead >= 0xF0) length = 4;
				else if (lead >= 0xE0) length = 3;
				else if (lead >= 0xC0) length = 2;
				if (i + length > text.size()) length = 1;
				width += (lead < 0x80 ? 0.5f : 1.0f) * fontSize;
				i += length;
			}
			return width;
		});
	}

	void ClearFakeMeasure()
	{
		ClearTextMeasureHook(&g_MeasureOwner);
	}

	WuiCodeEditorResult RunEditorFrame(WuiContext& ctx, WuiTextBuffer& buffer, const WuiRect& rect,
		const WuiCodeEditorOptions& options, WuiInputState input)
	{
		ctx.BeginFrame(input);
		const WuiCodeEditorResult result = CodeEditor(ctx, HashId("test.editor"), rect, buffer, options);
		ctx.EndFrame();
		return result;
	}

	void ClickAt(WuiContext& ctx, WuiTextBuffer& buffer, const WuiRect& rect,
		const WuiCodeEditorOptions& options, float x, float y)
	{
		WuiInputState input;
		input.MousePos = { x, y };
		input.MouseClicked[0] = true;
		RunEditorFrame(ctx, buffer, rect, options, input);
	}

	void PressKeys(WuiContext& ctx, WuiTextBuffer& buffer, const WuiRect& rect,
		const WuiCodeEditorOptions& options, const std::vector<uint32_t>& keys,
		const std::vector<uint32_t>& chars = {}, bool ctrl = false, bool shift = false)
	{
		WuiInputState input;
		input.KeyDown = keys;
		input.TextInput = chars;
		input.Ctrl = ctrl;
		input.Shift = shift;
		RunEditorFrame(ctx, buffer, rect, options, input);
	}

	const WuiDrawCommand* FindTextCommand(const std::vector<WuiDrawCommand>& commands, const std::string& text)
	{
		for (const WuiDrawCommand& command : commands)
			if (command.Kind == WuiDrawKind::Text && command.Text == text)
				return &command;
		return nullptr;
	}
}

int main()
{
	try
	{
		// ---- 1. UTF-8 码点边界:移动/删除都按码点 ----
		{
			WuiTextBuffer buffer;
			buffer.SetText(u8"aé世b");
			CHECK(buffer.Text().size() == 7);
			CHECK(buffer.LineCount() == 1);
			buffer.MoveCaret(WuiTextBuffer::Motion::Right, false);
			CHECK(buffer.Caret() == 1);
			buffer.MoveCaret(WuiTextBuffer::Motion::Right, false);
			CHECK(buffer.Caret() == 3);
			buffer.MoveCaret(WuiTextBuffer::Motion::Right, false);
			CHECK(buffer.Caret() == 6);
			buffer.MoveCaret(WuiTextBuffer::Motion::Right, false);
			CHECK(buffer.Caret() == 7);
			buffer.MoveCaret(WuiTextBuffer::Motion::Right, false);
			CHECK(buffer.Caret() == 7); // 行尾夹紧
			buffer.MoveCaret(WuiTextBuffer::Motion::Left, false);
			CHECK(buffer.Caret() == 6);

			buffer.Backspace();
			CHECK(buffer.Text() == u8"aéb"); // 删除一整个 3 字节码点
			CHECK(buffer.Caret() == 3);
			buffer.Backspace();
			CHECK(buffer.Text() == u8"ab");
			buffer.SetCaret(0, false);
			buffer.DeleteForward();
			CHECK(buffer.Text() == u8"b");
			buffer.DeleteForward();
			CHECK(buffer.Text().empty());
			CHECK(buffer.ColumnOfOffset(0) == 0);
		}

		// ---- 2. 行索引与 CRLF 原样保留 ----
		{
			WuiTextBuffer buffer;
			buffer.SetText(u8"a\r\nb\r\n");
			CHECK(buffer.LineCount() == 3);
			const auto line0 = buffer.LineRange(0);
			const auto line1 = buffer.LineRange(1);
			const auto line2 = buffer.LineRange(2);
			CHECK(line0.first == 0 && line0.second == 2); // 行内容含尾部 '\r'
			CHECK(line1.first == 3 && line1.second == 5);
			CHECK(line2.first == 6 && line2.second == 6);
			CHECK(buffer.LineOfOffset(0) == 0);
			CHECK(buffer.LineOfOffset(2) == 0);
			CHECK(buffer.LineOfOffset(3) == 1);
			CHECK(buffer.LineOfOffset(6) == 2);
			CHECK(buffer.ColumnOfOffset(2) == 1); // '\r' 不计列

			// 回车:CRLF 文件里插入的行终止符也是 CRLF,不把用户文件改成 LF。
			buffer.SetCaret(1, false);
			buffer.InsertNewline();
			CHECK(buffer.Text() == u8"a\r\n\r\nb\r\n");

			// caret 卡在 '\r' 与 '\n' 之间时只插 '\n'(否则会造出 "\r\r\n")。
			buffer.SetText(u8"a\r\nb\r\n");
			buffer.SetCaret(2, false);
			buffer.InsertNewline();
			CHECK(buffer.Text() == u8"a\r\n\nb\r\n");
		}

		// ---- 3. 编辑命令 ----
		{
			// 选区替换 + 回车自动缩进
			WuiTextBuffer buffer;
			buffer.SetText(u8"hello world");
			buffer.SetCaret(6, false);
			buffer.SetCaret(11, true);
			CHECK(buffer.HasSelection());
			buffer.InsertAtCaret(u8"luau");
			CHECK(buffer.Text() == u8"hello luau");
			CHECK(buffer.Caret() == 10);

			buffer.SetText(u8"    x = 1");
			buffer.SetCaret(buffer.Text().size(), false);
			buffer.InsertNewline();
			CHECK(buffer.Text() == u8"    x = 1\n    ");
			CHECK(buffer.Caret() == 14);

			// Tab / Shift+Tab:多行选区缩进与反缩进
			buffer.SetText(u8"a\nb\nc");
			buffer.SetCaret(0, false);
			buffer.SetCaret(buffer.Text().size(), true);
			buffer.IndentSelection(false);
			CHECK(buffer.Text() == u8"    a\n    b\n    c");
			buffer.IndentSelection(true);
			CHECK(buffer.Text() == u8"a\nb\nc");

			// CRLF 文件缩进:行尾 '\r' 原样保留,不被改成 LF
			buffer.SetText(u8"a\r\nb\r\n");
			buffer.SetCaret(0, false);
			buffer.SetCaret(buffer.Text().size(), true);
			buffer.IndentSelection(false);
			CHECK(buffer.Text() == u8"    a\r\n    b\r\n");
			buffer.IndentSelection(true);
			CHECK(buffer.Text() == u8"a\r\nb\r\n");

			// 单行 Tab = 插入 4 空格;Shift+Tab = 反缩进当前行
			buffer.SetText(u8"x");
			buffer.SetCaret(0, false);
			buffer.IndentSelection(false);
			CHECK(buffer.Text() == u8"    x");
			CHECK(buffer.Caret() == 4);
			buffer.IndentSelection(true);
			CHECK(buffer.Text() == u8"x");
			CHECK(buffer.Caret() == 0);

			// SelectAll / Copy / Cut / Paste
			buffer.SetText(u8"one two");
			buffer.SelectAll();
			std::string clip;
			CHECK(buffer.Copy(clip) && clip == u8"one two");
			CHECK(buffer.Cut(clip) && clip == u8"one two");
			CHECK(buffer.Text().empty());
			CHECK(buffer.Paste(u8"three"));
			CHECK(buffer.Text() == u8"three");
			CHECK(buffer.Caret() == 5);
			CHECK(!buffer.Paste(std::string_view()));

			// Backspace/Delete 跨选区删除
			buffer.SetText(u8"abcdef");
			buffer.SetCaret(1, false);
			buffer.SetCaret(5, true);
			buffer.Backspace();
			CHECK(buffer.Text() == u8"af");
			buffer.SetCaret(1, false);
			buffer.DeleteForward();
			CHECK(buffer.Text() == u8"a");
		}

		// ---- 4. 撤销/重做:分组、上限、Dirty ----
		{
			WuiTextBuffer buffer;
			buffer.SetText(u8"");
			CHECK(!buffer.Dirty());
			const uint64_t revisionBefore = buffer.Revision();
			buffer.InsertAtCaret(u8"a");
			buffer.InsertAtCaret(u8"b");
			buffer.InsertAtCaret(u8"c");
			CHECK(buffer.Text() == u8"abc");
			CHECK(buffer.Dirty());
			CHECK(buffer.Revision() > revisionBefore);
			CHECK(buffer.Undo());
			CHECK(buffer.Text().empty()); // 连续插入合并为一步
			CHECK(!buffer.Dirty());       // 撤销回保存点重新变干净
			CHECK(buffer.Redo());
			CHECK(buffer.Text() == u8"abc");
			CHECK(buffer.Dirty());
			buffer.MarkSaved();
			CHECK(!buffer.Dirty());

			// 连续退格合并为一步
			buffer.SetText(u8"abc");
			buffer.SetCaret(3, false);
			buffer.Backspace();
			buffer.Backspace();
			CHECK(buffer.Text() == u8"a");
			CHECK(buffer.Undo());
			CHECK(buffer.Text() == u8"abc");

			// 导航打断合并组
			buffer.SetText(u8"");
			buffer.InsertAtCaret(u8"a");
			buffer.MoveCaret(WuiTextBuffer::Motion::Left, false);
			buffer.InsertAtCaret(u8"b");
			CHECK(buffer.Text() == u8"ba");
			CHECK(buffer.Undo());
			CHECK(buffer.Text() == u8"a");
			CHECK(buffer.Undo());
			CHECK(buffer.Text().empty());
			CHECK(buffer.Redo() && buffer.Redo());
			CHECK(buffer.Text() == u8"ba");

			// 200 步上限:第 201 步之前的编辑被淘汰
			WuiTextBuffer capped;
			capped.SetText(u8"");
			for (int i = 0; i < 250; ++i)
			{
				capped.InsertAtCaret(u8"x");
				capped.BreakUndoGroup();
			}
			CHECK(capped.Text().size() == 250);
			int undos = 0;
			while (capped.Undo())
				++undos;
			CHECK(undos == 200);
			CHECK(capped.Text().size() == 50);

			// 2MB 字节上限:超限时淘汰更老的一步
			WuiTextBuffer byteCapped;
			byteCapped.SetText(u8"");
			const std::string huge(2300 * 1024, 'h');
			const std::string small(300 * 1024, 's');
			CHECK(byteCapped.Paste(huge));
			CHECK(byteCapped.Paste(small));
			CHECK(byteCapped.Undo());
			CHECK(byteCapped.Text().size() == huge.size());
			CHECK(!byteCapped.Undo());
		}

		// ---- 5. 导航:软列 / Home/End / 按词 / 翻页 / Shift 扩展 ----
		{
			WuiTextBuffer buffer;
			buffer.SetText(u8"abcdef\nab\nabcdef");
			buffer.SetCaret(5, false);
			buffer.MoveCaret(WuiTextBuffer::Motion::Down, false);
			CHECK(buffer.Caret() == 9); // 第 1 行只有 "ab",停到行尾但软列仍是 5
			buffer.MoveCaret(WuiTextBuffer::Motion::Down, false);
			CHECK(buffer.Caret() == 15); // 回到软列 5(不是上一行的行尾)
			CHECK(buffer.LineOfOffset(buffer.Caret()) == 2);
			buffer.MoveCaret(WuiTextBuffer::Motion::LineStart, false);
			CHECK(buffer.Caret() == 10);
			buffer.MoveCaret(WuiTextBuffer::Motion::LineEnd, false);
			CHECK(buffer.Caret() == 16);
			buffer.MoveCaret(WuiTextBuffer::Motion::DocStart, false);
			CHECK(buffer.Caret() == 0);
			buffer.MoveCaret(WuiTextBuffer::Motion::DocEnd, false);
			CHECK(buffer.Caret() == buffer.Text().size());

			// Ctrl+←/→ 按词(非 ASCII 算词内字符)
			WuiTextBuffer words;
			words.SetText(u8"one two 三");
			words.MoveCaret(WuiTextBuffer::Motion::WordRight, false);
			CHECK(words.Caret() == 4);
			words.MoveCaret(WuiTextBuffer::Motion::WordRight, false);
			CHECK(words.Caret() == 8);
			words.MoveCaret(WuiTextBuffer::Motion::WordRight, false);
			CHECK(words.Caret() == 11);
			words.MoveCaret(WuiTextBuffer::Motion::WordLeft, false);
			CHECK(words.Caret() == 8);
			words.MoveCaret(WuiTextBuffer::Motion::WordLeft, false);
			CHECK(words.Caret() == 4);

			// PageUp/PageDown:页步数由参数传入
			WuiTextBuffer pages;
			std::string text;
			for (int i = 0; i < 10; ++i)
				text += "L" + std::to_string(i) + "\n";
			pages.SetText(text);
			pages.SetCaret(8, false); // 第 3 行(L2)行尾
			pages.MoveCaret(WuiTextBuffer::Motion::PageDown, false, 3);
			CHECK(pages.LineOfOffset(pages.Caret()) == 5);
			pages.MoveCaret(WuiTextBuffer::Motion::PageUp, false, 3);
			CHECK(pages.LineOfOffset(pages.Caret()) == 2);

			// Shift 扩展选区:anchor 不动,caret 走
			WuiTextBuffer selection;
			selection.SetText(u8"abcdef");
			selection.SetCaret(2, false);
			selection.MoveCaret(WuiTextBuffer::Motion::Right, true);
			selection.MoveCaret(WuiTextBuffer::Motion::Right, true);
			CHECK(selection.Anchor() == 2);
			CHECK(selection.Caret() == 4);
			const auto [selStart, selEnd] = selection.Selection();
			CHECK(selStart == 2 && selEnd == 4);
		}

		// ---- 6. 假度量注入:真实测量的鼠标命中(含 CJK 2× 宽度) ----
		InstallFakeMeasure();
		const WuiRect editorRect { 0.0f, 0.0f, 400.0f, 200.0f };
		WuiCodeEditorOptions editorOptions;
		editorOptions.FontSize = 10.0f;
		editorOptions.LineHeight = 20.0f;
		editorOptions.GutterWidth = 30.0f;
		{
			WuiContext ctx;
			WuiTextBuffer buffer;
			buffer.SetText(u8"a世界b"); // 'a' = 1 字节,'世' = 3,'界' = 3,'b' = 1
			// 'a' 宽 5px:x = 30 + 7 落在 '世' 的左半边 → caret 到 '世' 起点。
			ClickAt(ctx, buffer, editorRect, editorOptions, 37.0f, 5.0f);
			CHECK(buffer.Caret() == 1);
			CHECK(ctx.Focus() == HashId("test.editor"));
			// '世' 宽 10px:x = 30 + 16 落在 '界' 的左半边 → caret 到 '界' 起点。
			ClickAt(ctx, buffer, editorRect, editorOptions, 46.0f, 5.0f);
			CHECK(buffer.Caret() == 4);
			// 点到行尾右侧 → caret 到行尾。
			ClickAt(ctx, buffer, editorRect, editorOptions, 200.0f, 5.0f);
			CHECK(buffer.Caret() == 8);
		}

		// ---- 7. 绘制命令:行号 / 选区 / TextCaretByte / 10k 行只画可见行 ----
		{
			WuiContext ctx;
			WuiTextBuffer buffer;
			buffer.SetText(u8"local x = 1\nprint(x)\n");
			WuiCodeEditorOptions options = editorOptions;
			options.Highlight = [](std::string_view line, std::vector<WuiCodeToken>& out)
			{
				const size_t at = line.find(u8"local");
				if (at != std::string_view::npos)
					out.push_back({ static_cast<uint32_t>(at), static_cast<uint32_t>(at + 5), WuiCodeTokenKind::Keyword });
			};
			ClickAt(ctx, buffer, editorRect, options, 31.0f, 5.0f);
			buffer.SetCaret(0, false);
			buffer.SetCaret(5, true);
			const WuiCodeEditorResult result = RunEditorFrame(ctx, buffer, editorRect, options, WuiInputState {});
			CHECK(!result.Changed);
			const std::vector<WuiDrawCommand>& commands = ctx.Commands();
			CHECK(FindTextCommand(commands, "1") != nullptr); // 行号栏
			bool selectionFound = false;
			bool caretFound = false;
			for (const WuiDrawCommand& command : commands)
			{
				if (command.Kind != WuiDrawKind::Text)
					continue;
				if (command.TextSelStart == 0 && command.TextSelEnd == 5)
					selectionFound = true;
				if (command.TextCaretByte >= 0 && command.Family == WuiFontFamily::Monospace)
					caretFound = true;
			}
			CHECK(selectionFound);
			CHECK(caretFound);

			// 10k 行:命令数与可见行同阶,不随文件行数增长。
			WuiContext bigCtx;
			WuiTextBuffer bigBuffer;
			std::string big;
			for (int i = 0; i < 10000; ++i)
				big += "line " + std::to_string(i) + "\n";
			bigBuffer.SetText(big);
			CHECK(bigBuffer.LineCount() == 10001); // 末尾换行后还有一个空行
			WuiInputState click;
			click.MousePos = { 45.0f, 5.0f };
			click.MouseClicked[0] = true;
			RunEditorFrame(bigCtx, bigBuffer, editorRect, options, click);
			const int visibleLines = static_cast<int>(editorRect.H / options.LineHeight) + 2;
			const size_t commandCount = bigCtx.Commands().size();
			CHECK(commandCount < static_cast<size_t>(visibleLines) * 8 + 32);
			CHECK(commandCount < 200);
			std::printf("World.WuiTextBuffer: 10k-line editor frame commands=%zu (visible lines~%d)\n",
				commandCount, visibleLines);
		}

		// ---- 8. 键盘路由:SaveRequested / ReadOnly / 剪贴板 / 双击选词 / Escape ----
		{
			WuiContext ctx;
			WuiTextBuffer buffer;
			buffer.SetText(u8"alpha beta");
			ClickAt(ctx, buffer, editorRect, editorOptions, 31.0f, 5.0f);
			CHECK(WuiTextFocus::Get().Active()); // 文本焦点登记(AI/快捷键路由用)

			WuiInputState saveInput;
			saveInput.KeyDown = { World::KeyCodes::S };
			saveInput.Ctrl = true;
			const WuiCodeEditorResult saveResult = RunEditorFrame(ctx, buffer, editorRect, editorOptions, saveInput);
			CHECK(saveResult.SaveRequested);
			CHECK(!saveResult.Changed);

			// ReadOnly:不响应输入/编辑键,只导航/选择/复制。
			WuiCodeEditorOptions readOnly = editorOptions;
			readOnly.ReadOnly = true;
			WuiInputState editInput;
			editInput.KeyDown = { World::KeyCodes::Backspace, World::KeyCodes::Enter, World::KeyCodes::Tab };
			editInput.TextInput = { 'x' };
			const WuiCodeEditorResult readOnlyResult = RunEditorFrame(ctx, buffer, editorRect, readOnly, editInput);
			CHECK(buffer.Text() == u8"alpha beta");
			CHECK(!readOnlyResult.Changed);
			CHECK(!readOnlyResult.SaveRequested);

			// 剪贴板注入:Ctrl+C / Ctrl+V / Ctrl+Z
			std::string clip;
			WuiCodeEditorOptions clipboardOptions = editorOptions;
			clipboardOptions.SetClipboard = [&clip](std::string_view text) { clip.assign(text); return true; };
			clipboardOptions.GetClipboard = [&clip](std::string& out) { out = clip; return true; };
			buffer.SetCaret(0, false);
			buffer.SetCaret(5, true);
			WuiInputState copyInput;
			copyInput.KeyDown = { World::KeyCodes::C };
			copyInput.Ctrl = true;
			RunEditorFrame(ctx, buffer, editorRect, clipboardOptions, copyInput);
			CHECK(clip == u8"alpha");
			buffer.SetCaret(buffer.Text().size(), false);
			WuiInputState pasteInput;
			pasteInput.KeyDown = { World::KeyCodes::V };
			pasteInput.Ctrl = true;
			RunEditorFrame(ctx, buffer, editorRect, clipboardOptions, pasteInput);
			CHECK(buffer.Text() == u8"alpha betaalpha");
			WuiInputState undoInput;
			undoInput.KeyDown = { World::KeyCodes::Z };
			undoInput.Ctrl = true;
			RunEditorFrame(ctx, buffer, editorRect, clipboardOptions, undoInput);
			CHECK(buffer.Text() == u8"alpha beta");

			// 双击选词(合成 MouseClicked + MouseDoubleClicked 同帧)
			WuiInputState doubleClick;
			doubleClick.MousePos = { 31.0f + 15.0f, 5.0f };
			doubleClick.MouseClicked[0] = true;
			doubleClick.MouseDoubleClicked[0] = true;
			RunEditorFrame(ctx, buffer, editorRect, editorOptions, doubleClick);
			const auto [wordStart, wordEnd] = buffer.Selection();
			CHECK(wordStart == 0 && wordEnd == 5);

			// Escape 失焦:文本焦点登记随之清空。
			WuiInputState escapeInput;
			escapeInput.KeyDown = { World::KeyCodes::Escape };
			RunEditorFrame(ctx, buffer, editorRect, editorOptions, escapeInput);
			CHECK(ctx.Focus() == 0);
			CHECK(!WuiTextFocus::Get().Active());
		}

		// ---- 9. 滚轮滚动:只画滚动后的可见行 ----
		{
			WuiContext ctx;
			WuiTextBuffer buffer;
			std::string many;
			for (int i = 0; i < 100; ++i)
				many += "row " + std::to_string(i) + "\n";
			buffer.SetText(many);
			ClickAt(ctx, buffer, editorRect, editorOptions, 31.0f, 5.0f);
			WuiInputState wheel;
			wheel.MousePos = { 100.0f, 100.0f };
			wheel.Wheel = -3.0f; // 向下滚 180px = 9 行
			RunEditorFrame(ctx, buffer, editorRect, editorOptions, wheel);
			const std::vector<WuiDrawCommand>& commands = ctx.Commands();
			CHECK(FindTextCommand(commands, "1") == nullptr);
			CHECK(FindTextCommand(commands, "10") != nullptr);
		}

		// ---- 10. 10k 行性能实测(数字进报告,不设硬门禁) ----
		{
			using Clock = std::chrono::steady_clock;
			std::string big;
			for (int i = 0; i < 10000; ++i)
				big += "line " + std::to_string(i) + " = " + std::to_string(i * 7) + "\n";
			WuiTextBuffer buffer;
			const auto t0 = Clock::now();
			buffer.SetText(big);
			const auto t1 = Clock::now();
			size_t sink = 0;
			for (int i = 0; i < 10000; ++i)
				sink += static_cast<size_t>(buffer.LineOfOffset(static_cast<size_t>(i) * 17 % big.size()));
			const auto t2 = Clock::now();
			buffer.SetCaret(buffer.Text().size() / 2, false);
			buffer.InsertAtCaret(u8"x");
			const auto t3 = Clock::now();
			buffer.Undo();
			const auto t4 = Clock::now();
			const auto ms = [](Clock::time_point a, Clock::time_point b)
			{
				return std::chrono::duration<double, std::milli>(b - a).count();
			};
			std::printf("World.WuiTextBuffer: 10k lines (%zu bytes): SetText=%.3f ms, 10000x LineOfOffset=%.3f ms, insert=%.3f ms, undo=%.3f ms (sink=%zu)\n",
				big.size(), ms(t0, t1), ms(t1, t2), ms(t2, t3), ms(t3, t4), sink);
			CHECK(buffer.Text() == big);
		}

		ClearFakeMeasure();
		std::printf("World.WuiTextBuffer: all checks passed\n");
		return 0;
	}
	catch (const std::exception& error)
	{
		std::fprintf(stderr, "World.WuiTextBuffer: FAILED: %s\n", error.what());
		return 1;
	}
}
