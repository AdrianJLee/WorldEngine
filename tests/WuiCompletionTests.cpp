// W9.5-2:WuiCodeEditor 补全浮层 headless 回归。
// 覆盖:provider linePrefix/候选、ui. 触发与无障碍登记、Up/Down 选择、Enter/Tab 接受与 caret 位置、
// 方法/Method 插入 name() 且 caret 落括号内、字符串/注释内不弹、Esc 只关浮层保持焦点、
// 空候选不弹、ReadOnly 不弹、Ctrl+Space 沿触发、鼠标 hover/点击接受、滚轮只滚浮层。

#include "World/WUI/WuiAccessibility.h"
#include "World/WUI/WuiCodeEditor.h"
#include "World/WUI/WuiContext.h"
#include "World/WUI/WuiTextBuffer.h"
#include "World/Core/KeyCodes.h"
#include "World/Script/LuauHighlighter.h"

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

	// 假度量:ASCII 0.5em,CJK 1.0em(与 WuiTextBufferTests 同款,行内像素位置可预测)。
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

	World::LuauCompletionItem MakeItem(const char* name, const char* type,
		World::LuauCompletionItem::KindType kind, const char* doc = "")
	{
		World::LuauCompletionItem item;
		item.Name = name;
		item.Type = type;
		item.Kind = kind;
		item.Doc = doc;
		return item;
	}

	// 静态度量:提供候选表(替代真实索引),并记录最近一次收到的 linePrefix。
	struct ProviderProbe
	{
		std::vector<World::LuauCompletionItem> Items;
		std::vector<std::string> Prefixes;
	};

	WuiCodeEditorOptions OptionsWith(ProviderProbe& probe)
	{
		WuiCodeEditorOptions options;
		options.FontSize = 14.0f;
		options.LineHeight = 20.0f;
		options.CompletionIdPrefix = "test.suggest";
		options.Highlight = [](std::string_view line, std::vector<WuiCodeToken>& out)
		{
			// 注释行:-- 之后全部是 Comment(模拟 LuauHighlighter 的注释 token)。
			const size_t comment = line.find("--");
			if (comment != std::string_view::npos)
			{
				out.push_back({ static_cast<uint32_t>(comment), static_cast<uint32_t>(line.size()),
					WuiCodeTokenKind::Comment });
			}
		};
		options.Completion = [&probe](std::string_view linePrefix,
			std::vector<World::LuauCompletionItem>& out)
		{
			probe.Prefixes.emplace_back(linePrefix);
			out = probe.Items;
		};
		return options;
	}

	WuiCodeEditorResult RunFrame(WuiContext& ctx, WuiTextBuffer& buffer, const WuiRect& rect,
		const WuiCodeEditorOptions& options, WuiInputState input)
	{
		// 与真实宿主一致:每帧重建本窗口的无障碍树(否则上一帧节点会残留)。
		WuiAccessibility::Get().BeginFrame("test");
		ctx.BeginFrame(input);
		const WuiCodeEditorResult result = CodeEditor(ctx, HashId("test.editor"), rect, buffer, options);
		ctx.EndFrame();
		return result;
	}

	void TypeChars(WuiContext& ctx, WuiTextBuffer& buffer, const WuiRect& rect,
		const WuiCodeEditorOptions& options, const std::vector<uint32_t>& chars)
	{
		WuiInputState input;
		input.TextInput = chars;
		input.MousePos = { rect.X + 40.0f, rect.Y + 40.0f };   // 真实鼠标在编辑区内(避免点击失焦路径)
		RunFrame(ctx, buffer, rect, options, input);
	}

	void PressKey(WuiContext& ctx, WuiTextBuffer& buffer, const WuiRect& rect,
		const WuiCodeEditorOptions& options, uint32_t key, bool ctrl = false)
	{
		WuiInputState input;
		input.KeyDown = { key };
		input.KeyPressed = { key };
		input.Ctrl = ctrl;
		RunFrame(ctx, buffer, rect, options, input);
	}

	// 在文本末尾按 Ctrl+Space 打开浮层(候选由 provider 决定)。
	void OpenPopupAtEnd(WuiContext& ctx, WuiTextBuffer& buffer, const WuiRect& rect,
		const WuiCodeEditorOptions& options)
	{
		WuiInputState input;
		input.KeyDown = { World::KeyCodes::End, World::KeyCodes::Space };
		input.KeyPressed = { World::KeyCodes::End, World::KeyCodes::Space };
		input.Ctrl = true;
		RunFrame(ctx, buffer, rect, options, input);
	}

	const WuiAccessNode* FindSuggest(WuiId id)
	{
		return WuiAccessibility::Get().Find(id);
	}

	// SetText 把 caret 归零;测试统一从文本末尾开始(caret 跟随输入)。
	void SetTextAtEnd(WuiTextBuffer& buffer, const std::string& text)
	{
		buffer.SetText(text);
		buffer.SetCaret(buffer.Text().size(), false);
	}
}

int main()
{
	try
	{
		InstallFakeMeasure();
		WuiAccessibility::Get().Clear();
		WuiAccessibility::Get().SetEnabled(true);
		const WuiRect editorRect { 8.0f, 8.0f, 600.0f, 300.0f };

		// ---- 1. 触发 + provider linePrefix + 无障碍节点登记 ----
		{
			WuiContext ctx;
			WuiTextBuffer buffer;
			SetTextAtEnd(buffer, "local ui = {}\n");
			ProviderProbe probe;
			probe.Items.push_back(MakeItem("panel", "Panel", World::LuauCompletionItem::KindType::Field, "UI panel."));
			probe.Items.push_back(MakeItem("button", "fun(text: string)", World::LuauCompletionItem::KindType::Method));
			WuiCodeEditorOptions options = OptionsWith(probe);
			ctx.SetFocus(HashId("test.editor"));

			TypeChars(ctx, buffer, editorRect, options, { 'u', 'i', '.' });
			CHECK(buffer.Text() == "local ui = {}\nui.");
			CHECK(!probe.Prefixes.empty());
			CHECK(probe.Prefixes.back() == "ui.");
			CHECK(FindSuggest(HashId("test.suggest.0")) != nullptr);
			CHECK(FindSuggest(HashId("test.suggest.1")) != nullptr);
			const WuiAccessNode* status = FindSuggest(HashId("test.suggest.status"));
			CHECK(status != nullptr);
			// 浮层必须真的发出绘制命令(用户实测:节点有、画面无;这条断言防止再次回归)。
			bool popupBackgroundDrawn = false;
			int popupIndex = -1;
			int editorBodyIndex = -1;
			int commandIndex = 0;
			for (const WuiDrawCommand& command : ctx.Commands())
			{
				if (command.Kind == WuiDrawKind::Rect
					&& std::fabs(command.Rect.X - status->Rect.X) < 0.5f
					&& std::fabs(command.Rect.W - status->Rect.W) < 0.5f
					&& std::fabs(command.Rect.H - status->Rect.H) < 0.5f)
				{
					popupBackgroundDrawn = true;
					popupIndex = commandIndex;
				}
				// 编辑区主体背景:与 editorRect 同尺寸的实心矩形。
				if (command.Kind == WuiDrawKind::Rect
					&& std::fabs(command.Rect.X - editorRect.X) < 0.5f
					&& std::fabs(command.Rect.W - editorRect.W) < 0.5f
					&& std::fabs(command.Rect.H - editorRect.H) < 0.5f)
					editorBodyIndex = commandIndex;
				++commandIndex;
			}
			CHECK(popupBackgroundDrawn);
			CHECK(editorBodyIndex >= 0 && popupIndex > editorBodyIndex);   // 顺序 = 绘制顺序:浮层必须在正文之后
			CHECK(status->Value.find("2 items") != std::string::npos);
			CHECK(status->Value.find("panel") != std::string::npos);
			const WuiAccessNode* first = FindSuggest(HashId("test.suggest.0"));
			CHECK(first->Label == "panel" && first->Value == "Panel" && first->Interactive);

			// 再按一个字母:provider 收到带前缀的 linePrefix,浮层仍在。
			buffer.SetCaret(buffer.Text().size(), false);
			TypeChars(ctx, buffer, editorRect, options, { 'b' });
			CHECK(probe.Prefixes.back() == "ui.b");
			CHECK(FindSuggest(HashId("test.suggest.0")) != nullptr);

			// 删除同样要刷新候选(用户实测:输入一个字母后删除,列表还停在旧前缀)。
			PressKey(ctx, buffer, editorRect, options, World::KeyCodes::Backspace);
			CHECK(buffer.Text() == "local ui = {}\nui.");
			CHECK(probe.Prefixes.back() == "ui.");
			CHECK(FindSuggest(HashId("test.suggest.0")) != nullptr);
			// 删到空前缀 → 列表关闭(空行/无意义前缀不查询)。
			PressKey(ctx, buffer, editorRect, options, World::KeyCodes::Backspace);   // 删 '.'
			PressKey(ctx, buffer, editorRect, options, World::KeyCodes::Backspace);   // 删 'i'
			PressKey(ctx, buffer, editorRect, options, World::KeyCodes::Backspace);   // 删 'u'
			CHECK(buffer.Text() == "local ui = {}\n");
			CHECK(FindSuggest(HashId("test.suggest.status")) == nullptr);
		}
		WuiAccessibility::Get().Clear();

		// ---- 2. Up/Down 移动选择(不改 caret),Enter 接受插入名字且 caret 位置正确 ----
		{
			WuiContext ctx;
			WuiTextBuffer buffer;
			SetTextAtEnd(buffer, "ui.");
			ProviderProbe probe;
			probe.Items.push_back(MakeItem("panel", "Panel", World::LuauCompletionItem::KindType::Field));
			probe.Items.push_back(MakeItem("button", "Button", World::LuauCompletionItem::KindType::Field));
			WuiCodeEditorOptions options = OptionsWith(probe);
			ctx.SetFocus(HashId("test.editor"));

			OpenPopupAtEnd(ctx, buffer, editorRect, options);
			CHECK(FindSuggest(HashId("test.suggest.0")) != nullptr);
			const WuiAccessNode* selectedFirst = FindSuggest(HashId("test.suggest.status"));
			CHECK(selectedFirst->Value.find("panel") != std::string::npos);

			const size_t caretBefore = buffer.Caret();
			PressKey(ctx, buffer, editorRect, options, World::KeyCodes::Down);
			CHECK(buffer.Caret() == caretBefore);   // 移动选择时 caret 不动
			CHECK(FindSuggest(HashId("test.suggest.status"))->Value.find("button") != std::string::npos);

			PressKey(ctx, buffer, editorRect, options, World::KeyCodes::Enter);
			CHECK(buffer.Text() == "ui.button");
			CHECK(buffer.Caret() == buffer.Text().size());
			CHECK(FindSuggest(HashId("test.suggest.status")) == nullptr);   // 接受后关闭
		}
		WuiAccessibility::Get().Clear();

		// ---- 3. 方法候选插入 name() 且 caret 落括号内 ----
		{
			WuiContext ctx;
			WuiTextBuffer buffer;
			SetTextAtEnd(buffer, "entity:Get");
			ProviderProbe probe;
			probe.Items.push_back(MakeItem("GetComponent", "fun(self, type: string)",
				World::LuauCompletionItem::KindType::Method));
			WuiCodeEditorOptions options = OptionsWith(probe);
			ctx.SetFocus(HashId("test.editor"));

			OpenPopupAtEnd(ctx, buffer, editorRect, options);
			CHECK(FindSuggest(HashId("test.suggest.0")) != nullptr);
			PressKey(ctx, buffer, editorRect, options, World::KeyCodes::Enter);
			CHECK(buffer.Text() == "entity:GetComponent()");
			CHECK(buffer.Caret() == std::string("entity:GetComponent(").size());
		}
		WuiAccessibility::Get().Clear();

		// ---- 4. 字符串/注释里不弹(LuauHighlighter token 判定) ----
		{
			WuiContext ctx;
			WuiTextBuffer buffer;
			SetTextAtEnd(buffer, "local s = \"ui.\"\n");
			ProviderProbe probe;
			probe.Items.push_back(MakeItem("panel", "Panel", World::LuauCompletionItem::KindType::Field));
			WuiCodeEditorOptions options = OptionsWith(probe);
			options.Highlight = [](std::string_view line, std::vector<WuiCodeToken>& out)
			{
				World::LuauHighlightState state;
				World::LuauHighlighter::HighlightLine(line, state, out);
			};
			ctx.SetFocus(HashId("test.editor"));

			PressKey(ctx, buffer, editorRect, options, World::KeyCodes::End);
			// 光标移到字符串内容里(引号之间)。
			PressKey(ctx, buffer, editorRect, options, World::KeyCodes::Left);
			PressKey(ctx, buffer, editorRect, options, World::KeyCodes::Left);
			TypeChars(ctx, buffer, editorRect, options, { 'x' });
			CHECK(FindSuggest(HashId("test.suggest.status")) == nullptr);

			// 注释行:-- ui.
			WuiTextBuffer comment;
			SetTextAtEnd(comment, "-- ui.");
			PressKey(ctx, comment, editorRect, options, World::KeyCodes::End);
			TypeChars(ctx, comment, editorRect, options, { 'x' });
			CHECK(FindSuggest(HashId("test.suggest.status")) == nullptr);
		}
		WuiAccessibility::Get().Clear();

		// ---- 5. Esc 只关浮层、保持焦点;空候选不弹;ReadOnly 不弹 ----
		{
			WuiContext ctx;
			WuiTextBuffer buffer;
			SetTextAtEnd(buffer, "ui.");
			ProviderProbe probe;
			probe.Items.push_back(MakeItem("panel", "Panel", World::LuauCompletionItem::KindType::Field));
			WuiCodeEditorOptions options = OptionsWith(probe);
			ctx.SetFocus(HashId("test.editor"));
			OpenPopupAtEnd(ctx, buffer, editorRect, options);
			CHECK(FindSuggest(HashId("test.suggest.status")) != nullptr);

			PressKey(ctx, buffer, editorRect, options, World::KeyCodes::Escape);
			CHECK(FindSuggest(HashId("test.suggest.status")) == nullptr);
			CHECK(ctx.Focus() == HashId("test.editor"));   // Esc 只关浮层:焦点保留
			TypeChars(ctx, buffer, editorRect, options, { 'y' });   // 仍能输入 = 未失焦
			CHECK(buffer.Text() == "ui.y");

			// 空候选:provider 返回空 → 不弹(即使 Ctrl+Space)。
			WuiContext emptyCtx;
			WuiTextBuffer emptyBuffer;
			SetTextAtEnd(emptyBuffer, "ui.");
			ProviderProbe emptyProbe;
			WuiCodeEditorOptions emptyOptions = OptionsWith(emptyProbe);
			emptyCtx.SetFocus(HashId("test.editor"));
			OpenPopupAtEnd(emptyCtx, emptyBuffer, editorRect, emptyOptions);
			CHECK(FindSuggest(HashId("test.suggest.status")) == nullptr);
			PressKey(emptyCtx, emptyBuffer, editorRect, emptyOptions, World::KeyCodes::Space, true);
			CHECK(FindSuggest(HashId("test.suggest.status")) == nullptr);

			// Ctrl+Space(沿)在无候选时不弹、有候选时弹。
			WuiContext ctrlCtx;
			WuiTextBuffer ctrlBuffer;
			SetTextAtEnd(ctrlBuffer, "ui.");
			ProviderProbe ctrlProbe;
			ctrlProbe.Items.push_back(MakeItem("panel", "Panel", World::LuauCompletionItem::KindType::Field));
			WuiCodeEditorOptions ctrlOptions = OptionsWith(ctrlProbe);
			ctrlCtx.SetFocus(HashId("test.editor"));
			PressKey(ctrlCtx, ctrlBuffer, editorRect, ctrlOptions, World::KeyCodes::End);
			CHECK(FindSuggest(HashId("test.suggest.status")) == nullptr);
			PressKey(ctrlCtx, ctrlBuffer, editorRect, ctrlOptions, World::KeyCodes::Space, true);
			CHECK(FindSuggest(HashId("test.suggest.status")) != nullptr);
			CHECK(!ctrlProbe.Prefixes.empty() && ctrlProbe.Prefixes.back() == "ui.");

			// ReadOnly:即使 Ctrl+Space 也不弹,且 buffer 不被改。
			WuiContext roCtx;
			WuiTextBuffer roBuffer;
			SetTextAtEnd(roBuffer, "ui.");
			ProviderProbe roProbe;
			roProbe.Items.push_back(MakeItem("panel", "Panel", World::LuauCompletionItem::KindType::Field));
			WuiCodeEditorOptions roOptions = OptionsWith(roProbe);
			roOptions.ReadOnly = true;
			roCtx.SetFocus(HashId("test.editor"));
			PressKey(roCtx, roBuffer, editorRect, roOptions, World::KeyCodes::End);
			PressKey(roCtx, roBuffer, editorRect, roOptions, World::KeyCodes::Space, true);
			CHECK(FindSuggest(HashId("test.suggest.status")) == nullptr);
			CHECK(roBuffer.Text() == "ui.");
		}
		WuiAccessibility::Get().Clear();

		// ---- 6. 鼠标:hover 选中第二行、点击接受;滚轮在浮层内不滚编辑器 ----
		{
			WuiContext ctx;
			WuiTextBuffer buffer;
			SetTextAtEnd(buffer, "ui.");
			ProviderProbe probe;
			probe.Items.push_back(MakeItem("panel", "Panel", World::LuauCompletionItem::KindType::Field));
			probe.Items.push_back(MakeItem("button", "Button", World::LuauCompletionItem::KindType::Field));
			WuiCodeEditorOptions options = OptionsWith(probe);
			ctx.SetFocus(HashId("test.editor"));
			OpenPopupAtEnd(ctx, buffer, editorRect, options);
			const WuiAccessNode* second = FindSuggest(HashId("test.suggest.1"));
			CHECK(second != nullptr);
			const float secondCenterY = second->Rect.Y + second->Rect.H * 0.5f;
			const float secondCenterX = second->Rect.X + second->Rect.W * 0.5f;

			WuiInputState hover;
			hover.MousePos = { secondCenterX, secondCenterY };
			RunFrame(ctx, buffer, editorRect, options, hover);
			CHECK(FindSuggest(HashId("test.suggest.status"))->Value.find("button") != std::string::npos);

			// 滚轮在浮层内:选中项移动但编辑器不滚动(文本第一行仍在绘制命令里)。
			WuiInputState wheel;
			wheel.MousePos = { secondCenterX, secondCenterY };
			wheel.Wheel = 1.0f;
			RunFrame(ctx, buffer, editorRect, options, wheel);
			CHECK(buffer.Text() == "ui.");

			// 点击第二行接受。
			WuiInputState click;
			click.MousePos = { secondCenterX, secondCenterY };
			click.MouseClicked[0] = true;
			RunFrame(ctx, buffer, editorRect, options, click);
			CHECK(buffer.Text() == "ui.button");
			CHECK(FindSuggest(HashId("test.suggest.status")) == nullptr);
		}
		WuiAccessibility::Get().Clear();

		// ---- 7. 多行注入回归:Enter 后输入普通字符不得重开浮层(Enter 不能被当成"接受") ----
		{
			WuiContext ctx;
			WuiTextBuffer buffer;
			SetTextAtEnd(buffer, "end");
			ProviderProbe probe;
			probe.Items.push_back(MakeItem("panel", "Panel", World::LuauCompletionItem::KindType::Field));
			probe.Items.push_back(MakeItem("assert", "fun(v)", World::LuauCompletionItem::KindType::Global));
			WuiCodeEditorOptions options = OptionsWith(probe);
			ctx.SetFocus(HashId("test.editor"));

			// 第 1 帧:注入换行(与注入器"第 2 段起先 Enter 再写该行"一致)。
			WuiInputState enterFrame;
			enterFrame.KeyDown = { World::KeyCodes::Enter };
			enterFrame.KeyPressed = { World::KeyCodes::Enter };
			RunFrame(ctx, buffer, editorRect, options, enterFrame);
			CHECK(buffer.Text() == "end\n");
			CHECK(FindSuggest(HashId("test.suggest.status")) == nullptr);   // 空行不弹浮层

			// 第 2 帧:输入字母 → 自动弹出候选(W9.5 方案 A:输入 ≥1 字符)。
			TypeChars(ctx, buffer, editorRect, options, { 'l', 'o', 'c', 'a', 'l' });
			CHECK(buffer.Text() == "end\nlocal");
			CHECK(FindSuggest(HashId("test.suggest.status")) != nullptr);
			CHECK(probe.Prefixes.back() == "local");

			// 第 3 帧:'.' 触发 → 浮层打开。
			TypeChars(ctx, buffer, editorRect, options, { '.' });
			CHECK(FindSuggest(HashId("test.suggest.status")) != nullptr);
			CHECK(buffer.Text() == "end\nlocal.");

			// 第 4 帧:浮层打开时输入字母 → 刷新前缀,不吞字符。
			TypeChars(ctx, buffer, editorRect, options, { 'x' });
			CHECK(buffer.Text() == "end\nlocal.x");
			CHECK(probe.Prefixes.back() == "local.x");
			const WuiAccessNode* status = FindSuggest(HashId("test.suggest.status"));
			CHECK(status != nullptr);
			CHECK(status->Value.find("prefix 'local.x'") != std::string::npos);

			// 第 5 帧:Enter = 接受候选(替换 x),不能插入换行。
			PressKey(ctx, buffer, editorRect, options, World::KeyCodes::Enter);
			CHECK(buffer.Text() == "end\nlocal.panel");
			CHECK(buffer.Text().find('\n') == 3 && buffer.Text().size() == 15);
		}

		// ---- 7b. 自动提示不吞 Enter:普通打字弹出的浮层里 Enter=换行,Tab=接受 ----
		{
			WuiContext ctx;
			WuiTextBuffer buffer;
			SetTextAtEnd(buffer, "en");
			ProviderProbe probe;
			probe.Items.push_back(MakeItem("panel", "Panel", World::LuauCompletionItem::KindType::Field));
			probe.Items.push_back(MakeItem("enabled", "boolean", World::LuauCompletionItem::KindType::Field));
			WuiCodeEditorOptions options = OptionsWith(probe);
			ctx.SetFocus(HashId("test.editor"));
			TypeChars(ctx, buffer, editorRect, options, { 'a', 'b' });
			CHECK(buffer.Text() == "enab");
			CHECK(FindSuggest(HashId("test.suggest.status")) != nullptr);

			// 自动弹出的浮层:Enter 必须换行(不把 Enter 当接受候选)。
			PressKey(ctx, buffer, editorRect, options, World::KeyCodes::Enter);
			CHECK(buffer.Text() == "enab\n");
			CHECK(FindSuggest(HashId("test.suggest.status")) == nullptr);

			// Tab 仍然接受候选(替换前缀)。
			TypeChars(ctx, buffer, editorRect, options, { 'p', 'a' });
			CHECK(FindSuggest(HashId("test.suggest.status")) != nullptr);
			PressKey(ctx, buffer, editorRect, options, World::KeyCodes::Tab);
			CHECK(buffer.Text() == "enab\npanel");
			CHECK(FindSuggest(HashId("test.suggest.status")) == nullptr);
		}
		WuiAccessibility::Get().Clear();

		// ---- 8. 性能:query + 过滤 + 绘制命令 1000 次实测(数字进报告,不设硬门禁) ----
		{
			using Clock = std::chrono::steady_clock;
			WuiAccessibility::Get().SetEnabled(false);   // 性能口径不含无障碍登记
			WuiContext ctx;
			WuiTextBuffer buffer;
			SetTextAtEnd(buffer, "ui.");
			ProviderProbe probe;
			for (int i = 0; i < 50; ++i)
			{
				probe.Items.push_back(MakeItem(("member" + std::to_string(i)).c_str(), "fun(...)",
					World::LuauCompletionItem::KindType::Method, "doc"));
			}
			WuiCodeEditorOptions options = OptionsWith(probe);
			ctx.SetFocus(HashId("test.editor"));
			const auto t0 = Clock::now();
			for (int i = 0; i < 1000; ++i)
			{
				WuiInputState input;
				input.TextInput = { '.' };   // 每帧重新触发一次查询(与真实逐键补全一致)
				RunFrame(ctx, buffer, editorRect, options, input);
			}
			const auto t1 = Clock::now();
			const double totalMs = std::chrono::duration<double, std::milli>(t1 - t0).count();
			std::printf("World.WuiCompletion: 1000 frames with 50 candidates: %.3f ms (%.1f us/frame)\n",
				totalMs, totalMs * 1000.0 / 1000.0);
			CHECK(probe.Prefixes.size() >= 1000);
			WuiAccessibility::Get().SetEnabled(true);
		}

		WuiAccessibility::Get().SetEnabled(false);
		WuiAccessibility::Get().Clear();
		ClearTextMeasureHook(&g_MeasureOwner);
		std::printf("World.WuiCompletion: ALL CHECKS PASSED\n");
		return 0;
	}
	catch (const std::exception& error)
	{
		std::fprintf(stderr, "World.WuiCompletion: FAILED: %s\n", error.what());
		return 1;
	}
}
