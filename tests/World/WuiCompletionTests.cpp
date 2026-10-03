// W9.5-2:WuiCodeEditor 补全浮层 headless 回归。
// 覆盖:provider linePrefix/候选、ui. 触发与无障碍登记、Up/Down 选择、Enter/Tab 接受与 caret 位置、
// 方法/Method 插入 name() 且 caret 落括号内、字符串/注释内不弹、Esc 只关浮层保持焦点、
// 空候选不弹、ReadOnly 不弹、Ctrl+Space 沿触发、鼠标 hover/点击接受、滚轮只滚浮层。

#include "World/WUI/WuiAccessibility.h"
#include "World/WUI/WuiCodeEditor.h"
#include "World/WUI/WuiContext.h"
#include "World/WUI/WuiTextBuffer.h"
#include "World/Core/KeyCodes.h"
#include "World/Script/Tooling/LuauHighlighter.h"
#include "World/Script/Tooling/LuauCompletion.h"

#include <chrono>
#include <cstdio>
#include <filesystem>
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
		std::vector<std::vector<World::LuauCompletionItem>> Results;
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
			probe.Results.push_back(out);
		};
		return options;
	}

	std::string StubPath()
	{
		// PROJ-7/T3:入库存根样本在仓库测试夹具里(WLD_TEST_ASSETPATH),不再依赖默认项目。
		const std::filesystem::path assets(WLD_TEST_ASSETPATH);
		return (assets / "scripts" / "intermediate" / "WorldEngineAPI.luau").string();
	}

	bool HasName(const std::vector<World::LuauCompletionItem>& items, std::string_view name)
	{
		for (const World::LuauCompletionItem& item : items)
			if (item.Name == name)
				return true;
		return false;
	}

	int NameIndex(const std::vector<World::LuauCompletionItem>& items, std::string_view name)
	{
		for (std::size_t i = 0; i < items.size(); ++i)
			if (items[i].Name == name)
				return static_cast<int>(i);
		return -1;
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

		// ---- 1b. 函数/方法用 Tab 接受:插入 name() 且 caret 在括号内 ----
		{
			WuiContext ctx;
			WuiTextBuffer buffer;
			SetTextAtEnd(buffer, "entity:Get");
			ProviderProbe probe;
			probe.Items.push_back(MakeItem("GetComponent", "fun(componentType: string)",
				World::LuauCompletionItem::KindType::Method));
			WuiCodeEditorOptions options = OptionsWith(probe);
			ctx.SetFocus(HashId("test.editor"));
			TypeChars(ctx, buffer, editorRect, options, { 'x' });   // 触发查询并打开浮层
			CHECK(FindSuggest(HashId("test.suggest.status")) != nullptr);
			PressKey(ctx, buffer, editorRect, options, World::KeyCodes::Tab);
			std::printf("[wui] tab accept function: '%s' caret=%zu\n", buffer.Text().c_str(), buffer.Caret());
			CHECK(buffer.Text() == "entity:GetComponent()");
			CHECK(buffer.Caret() == std::string("entity:GetComponent(").size());   // caret 在括号内
		}

		// ---- 1b-2. 带参数的函数:插入 name(p1, p2) 并自动选中第一个参数,Tab 跳下一个 ----
		{
			WuiContext ctx;
			WuiTextBuffer buffer;
			SetTextAtEnd(buffer, "entity:Get");
			ProviderProbe probe;
			probe.Items.push_back(MakeItem("GetComponent", "fun(componentType: string)",
				World::LuauCompletionItem::KindType::Method));
			probe.Items[0].Params = { "componentType" };
			WuiCodeEditorOptions options = OptionsWith(probe);
			ctx.SetFocus(HashId("test.editor"));
			TypeChars(ctx, buffer, editorRect, options, { 'x' });
			CHECK(FindSuggest(HashId("test.suggest.status")) != nullptr);
			PressKey(ctx, buffer, editorRect, options, World::KeyCodes::Tab);
			CHECK(buffer.Text() == "entity:GetComponent(componentType)");
			const auto [selStart, selEnd] = buffer.Selection();
			CHECK(selStart == std::string("entity:GetComponent(").size());
			CHECK(buffer.Text().substr(selStart, selEnd - selStart) == "componentType");

			// 两个参数:Tab 从第一个跳到第二个。
			WuiTextBuffer two;
			SetTextAtEnd(two, "body:Move");
			ProviderProbe twoProbe;
			twoProbe.Items.push_back(MakeItem("MoveTo", "fun(x: number, y: number)",
				World::LuauCompletionItem::KindType::Method));
			twoProbe.Items[0].Params = { "x", "y" };
			WuiCodeEditorOptions twoOptions = OptionsWith(twoProbe);
			TypeChars(ctx, two, editorRect, twoOptions, { 'x' });
			CHECK(FindSuggest(HashId("test.suggest.status")) != nullptr);
			PressKey(ctx, two, editorRect, twoOptions, World::KeyCodes::Tab);
			CHECK(two.Text() == "body:MoveTo(x, y)");
			auto [twoStart, twoEnd] = two.Selection();
			CHECK(two.Text().substr(twoStart, twoEnd - twoStart) == "x");
			PressKey(ctx, two, editorRect, twoOptions, World::KeyCodes::Tab);
			auto [secondStart, secondEnd] = two.Selection();
			CHECK(two.Text().substr(secondStart, secondEnd - secondStart) == "y");
		}

		// ---- 1c. 文件中间接受函数:补完整的一对括号(修复 `self.OnCreate)`) ----
		{
			WuiContext ctx;
			WuiTextBuffer buffer;
			SetTextAtEnd(buffer, "local a = 1\nself.OnC\nprint(a)\n");
			buffer.SetCaret(std::string("local a = 1\nself.OnC").size(), false);   // 光标在第二行行尾
			ProviderProbe probe;
			probe.Items.push_back(MakeItem("OnCreate", "fun(self: WorldScript)",
				World::LuauCompletionItem::KindType::Field));
			WuiCodeEditorOptions options = OptionsWith(probe);
			ctx.SetFocus(HashId("test.editor"));
			TypeChars(ctx, buffer, editorRect, options, { 'x' });   // 触发查询(前缀 OnCx)
			CHECK(FindSuggest(HashId("test.suggest.status")) != nullptr);
			PressKey(ctx, buffer, editorRect, options, World::KeyCodes::Tab);
			CHECK(buffer.Text() == "local a = 1\nself.OnCreate()\nprint(a)\n");
			CHECK(buffer.Caret() == std::string("local a = 1\nself.OnCreate(").size());
		}

		// ---- 1d. 已有调用括号:只补名字,不重复插括号 ----
		{
			WuiContext ctx;
			WuiTextBuffer buffer;
			SetTextAtEnd(buffer, "ui.pan(1)");
			buffer.SetCaret(std::string("ui.pan").size(), false);   // 光标在 '(' 前
			ProviderProbe probe;
			probe.Items.push_back(MakeItem("panel", "fun(x: number)",
				World::LuauCompletionItem::KindType::Method));
			WuiCodeEditorOptions options = OptionsWith(probe);
			ctx.SetFocus(HashId("test.editor"));
			TypeChars(ctx, buffer, editorRect, options, { 'x' });   // 触发查询(前缀 panx)
			CHECK(FindSuggest(HashId("test.suggest.status")) != nullptr);
			PressKey(ctx, buffer, editorRect, options, World::KeyCodes::Tab);
			CHECK(buffer.Text() == "ui.panel(1)");
		}

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

			// 注解行例外:`---@fie` 是注释,但要给 `---@field` 之类的标签候选。
			WuiTextBuffer annotation;
			SetTextAtEnd(annotation, "");
			TypeChars(ctx, annotation, editorRect, options, { '-', '-', '-', '@', 'f', 'i', 'e' });
			CHECK(probe.Prefixes.back() == "---@fie");
			CHECK(FindSuggest(HashId("test.suggest.status")) != nullptr);
		}
		WuiAccessibility::Get().Clear();

		// ---- 4a. 注解上下文(真实索引):标签位/类型位弹,字段名位与说明文字位不弹 ----
		{
			World::LuauCompletionIndex index;
			std::string loadError = "stale";
			CHECK(index.LoadStubFile(StubPath(), &loadError));
			CHECK(loadError.empty());

			WuiContext ctx;
			ctx.SetFocus(HashId("test.editor"));
			ProviderProbe probe;
			WuiCodeEditorOptions options;
			options.FontSize = 14.0f;
			options.LineHeight = 20.0f;
			options.CompletionIdPrefix = "test.suggest";
			options.Highlight = [](std::string_view line, std::vector<WuiCodeToken>& out)
			{
				World::LuauHighlightState state;
				World::LuauHighlighter::HighlightLine(line, state, out);
			};
			options.Completion = [&](std::string_view linePrefix,
				std::vector<World::LuauCompletionItem>& out)
			{
				probe.Prefixes.emplace_back(linePrefix);
				index.Query(linePrefix, 50, out);
				probe.Results.push_back(out);
			};

			// ① `---@field Speed ` 打完最后一个空格就该弹;前 13 条含 7 个引擎类型。
			WuiTextBuffer typeSlot;
			SetTextAtEnd(typeSlot, "---@field Speed");
			TypeChars(ctx, typeSlot, editorRect, options, { ' ' });
			CHECK(typeSlot.Text() == "---@field Speed ");
			CHECK(probe.Prefixes.size() == 1);
			CHECK(probe.Prefixes.back() == "---@field Speed ");
			CHECK(FindSuggest(HashId("test.suggest.status")) != nullptr);
			const std::vector<World::LuauCompletionItem>& typeItems = probe.Results.back();
			CHECK(typeItems.size() >= 13);
			for (const char* name : { "vec2", "vec3", "vec4", "mat3", "mat4", "Entity", "WorldScript" })
			{
				const int at = NameIndex(typeItems, name);
				CHECK(at >= 0 && at < 13);
			}
			std::string firstNames;
			for (std::size_t i = 0; i < typeItems.size() && i < 13; ++i)
			{
				if (!firstNames.empty())
					firstNames += ", ";
				firstNames += typeItems[i].Name;
			}
			std::printf("[wui] annotation '%s' -> %zu items; first13: %s\n",
				probe.Prefixes.back().c_str(), typeItems.size(), firstNames.c_str());

			// 自动弹出(Explicit=false)时 Enter 不能被候选吃掉:关浮层 + 正常换行。
			PressKey(ctx, typeSlot, editorRect, options, World::KeyCodes::Enter);
			CHECK(typeSlot.Text() == "---@field Speed \n");
			CHECK(FindSuggest(HashId("test.suggest.status")) == nullptr);

			// ② `---@field Speed vec` 过滤出 vec2/vec3/vec4(不混入其它引擎类型)。
			WuiTextBuffer filtered;
			SetTextAtEnd(filtered, "---@field Speed ve");
			TypeChars(ctx, filtered, editorRect, options, { 'c' });
			CHECK(probe.Prefixes.back() == "---@field Speed vec");
			CHECK(HasName(probe.Results.back(), "vec2"));
			CHECK(HasName(probe.Results.back(), "vec3"));
			CHECK(HasName(probe.Results.back(), "vec4"));
			CHECK(!HasName(probe.Results.back(), "mat3"));
			CHECK(!HasName(probe.Results.back(), "mat4"));
			CHECK(!HasName(probe.Results.back(), "Entity"));
			CHECK(!HasName(probe.Results.back(), "WorldScript"));
			CHECK(!HasName(probe.Results.back(), "number"));

			// ②b 派工列出的其它类型位形态同样算注解上下文(候选由索引给)。
			for (const char* context : { "---@type ", "---@param dt ", "---@class Player : " })
			{
				WuiTextBuffer contextBuffer;
				SetTextAtEnd(contextBuffer, context);
				const std::size_t queriesBeforeContext = probe.Prefixes.size();
				OpenPopupAtEnd(ctx, contextBuffer, editorRect, options);
				CHECK(probe.Prefixes.size() == queriesBeforeContext + 1);
				CHECK(probe.Prefixes.back() == context);
				CHECK(HasName(probe.Results.back(), "vec2"));
				CHECK(FindSuggest(HashId("test.suggest.status")) != nullptr);
			}

			// ③ 第 3 段说明文字位:即使 Ctrl+Space 强制召唤也不查询、不弹。
			WuiTextBuffer description;
			SetTextAtEnd(description, "---@field Speed number 移动");
			const std::size_t queriesBeforeDescription = probe.Prefixes.size();
			OpenPopupAtEnd(ctx, description, editorRect, options);
			CHECK(probe.Prefixes.size() == queriesBeforeDescription);
			CHECK(FindSuggest(HashId("test.suggest.status")) == nullptr);

			// ④ `---@cl` 仍给标签候选(标签位行为不回归)。
			WuiTextBuffer tag;
			SetTextAtEnd(tag, "---@c");
			TypeChars(ctx, tag, editorRect, options, { 'l' });
			CHECK(probe.Prefixes.back() == "---@cl");
			CHECK(HasName(probe.Results.back(), "class"));
			CHECK(FindSuggest(HashId("test.suggest.status")) != nullptr);

			// ⑤ 普通散文注释(没有 `---@`)不弹,即使 Ctrl+Space。
			WuiTextBuffer prose;
			SetTextAtEnd(prose, "--- 只是注释");
			const std::size_t queriesBeforeProse = probe.Prefixes.size();
			OpenPopupAtEnd(ctx, prose, editorRect, options);
			CHECK(probe.Prefixes.size() == queriesBeforeProse);
			CHECK(FindSuggest(HashId("test.suggest.status")) == nullptr);
		}
		WuiAccessibility::Get().Clear();

		// ---- 4b. 悬停提示:停留 ~0.4s → 名称/类型/文档;移开消失 ----
		{
			WuiContext ctx;
			WuiTextBuffer buffer;
			SetTextAtEnd(buffer, "local ui = {}\nui.panel");
			ProviderProbe probe;
			probe.Items.push_back(MakeItem("panel", "fun(x: number)",
				World::LuauCompletionItem::KindType::Method, "Draw a panel."));
			WuiCodeEditorOptions options = OptionsWith(probe);
			options.GutterWidth = 40.0f;
			std::vector<std::string> hoverWords;
			std::vector<std::string> hoverPrefixes;
			options.Hover = [&](std::string_view linePrefix, std::string_view word,
				World::LuauCompletionItem& out)
			{
				hoverPrefixes.emplace_back(linePrefix);
				hoverWords.emplace_back(word);
				out = probe.Items[0];
				return true;
			};
			ctx.SetFocus(HashId("test.editor"));
			WuiInputState hoverInput;
			hoverInput.MousePos = { editorRect.X + 40.0f + 24.0f, editorRect.Y + 35.0f };
			for (int frame = 0; frame < 30; ++frame)
				RunFrame(ctx, buffer, editorRect, options, hoverInput);
			CHECK(!hoverWords.empty() && hoverWords.back() == "panel");
			CHECK(hoverPrefixes.back() == "ui.");
			const WuiAccessNode* hoverNode = FindSuggest(HashId("test.suggest.hover"));
			CHECK(hoverNode != nullptr);
			CHECK(hoverNode->Label == "panel");
			CHECK(hoverNode->Value.find("Draw a panel.") != std::string::npos);

			// 鼠标移开 → 提示消失
			WuiInputState away;
			away.MousePos = { editorRect.X + 5.0f, editorRect.Y + 5.0f };
			RunFrame(ctx, buffer, editorRect, options, away);
			CHECK(FindSuggest(HashId("test.suggest.hover")) == nullptr);
		}

		// ---- 4c. 绘制分段无缝隙:拼接 == 行文本,相邻命令 x 差 == 前一段测宽 ----
		{
			WuiContext ctx;
			WuiTextBuffer buffer;
			const std::string line = "local PlayerScript = { Speed = 5.0, Name = \"Player\" }";
			SetTextAtEnd(buffer, line);
			WuiCodeEditorOptions options;
			options.GutterWidth = 40.0f;
			options.Highlight = [](std::string_view text, std::vector<WuiCodeToken>& out)
			{
				World::LuauHighlightState state;
				World::LuauHighlighter::HighlightLine(text, state, out);
			};
			WuiInputState idle;
			RunFrame(ctx, buffer, editorRect, options, idle);
			std::string joined;
			float prevX = 0.0f;
			float prevWidth = 0.0f;
			bool first = true;
			bool consistent = true;
			for (const WuiDrawCommand& command : ctx.Commands())
			{
				if (command.Kind != WuiDrawKind::Text)
					continue;
				if (command.Rect.X < editorRect.X + 40.0f)   // 行号(gutter 内)
					continue;
				if (command.Rect.Y < editorRect.Y || command.Rect.Y > editorRect.Y + 20.0f)   // 只看第一行
					continue;
				if (!first && std::fabs(command.Rect.X - (prevX + prevWidth)) > 0.51f)
					consistent = false;
				joined += command.Text;
				prevX = command.Rect.X;
				prevWidth = ctx.MeasureTextWidth(command.Text, command.FontSize, command.Family);
				first = false;
			}
			CHECK(joined == line);
			CHECK(consistent);
		}

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
