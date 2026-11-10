// World.UiTheme — GameUI(M14)主题令牌与样式继承(headless)。
//
// 覆盖(派工单验收):
//   ① `$tokenId` 解析:Tokens 优先于 Styles / 多级引用 / 环检测 / 未定义 = 可读错误 + 按"未设置"处理;
//   ② 令牌值参与绘制:命令颜色 = 令牌解析后的颜色,不是 `$primary` 字符串;
//   ③ 无障碍:节点 Label/Value 是解析后的文本,`$` 不出现;
//   ④ 旧文档(无 `Tokens`)行为与基线等价(序列化不含 Tokens 键、无令牌错误)。
//
// 说明:本文件不构建、不渲染;只断言"命令进了 WuiContext、节点进了无障碍树、错误可读"。

#include "World/UI/UiDocument.h"
#include "World/UI/UiNodeRegistry.h"
#include "World/UI/UiPainter.h"
#include "World/UI/UiScreen.h"
#include "World/UI/UiTypes.h"
#include "World/WUI/WuiAccessibility.h"
#include "World/WUI/WuiContext.h"
#include "World/WUI/WuiWidgets.h"

#include <cmath>
#include <cstdio>
#include <map>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <glm/glm.hpp>

namespace
{
	using namespace World;
	using namespace World::UI;

	// ---- 断言脚手架(与 tests/World/UiPaintTests.cpp 同形)----

	void Check(bool condition, const char* expression, int line)
	{
		if (!condition)
			throw std::runtime_error(std::string("line ") + std::to_string(line) + ": " + expression);
	}
#define CHECK(expression) Check(static_cast<bool>(expression), #expression, __LINE__)

	void CheckNear(float actual, float expected, float tolerance, const char* expression, int line)
	{
		if (std::fabs(actual - expected) > tolerance)
		{
			throw std::runtime_error(std::string("line ") + std::to_string(line) + ": " + expression +
				" (actual " + std::to_string(actual) + ", expected " + std::to_string(expected) + ")");
		}
	}
#define CHECK_NEAR(actual, expected, tolerance) \
	CheckNear(static_cast<float>(actual), static_cast<float>(expected), static_cast<float>(tolerance), #actual " ~= " #expected, __LINE__)

	void CheckContains(const std::string& text, const char* needle, const char* expression, int line)
	{
		if (text.find(needle) == std::string::npos)
		{
			throw std::runtime_error(std::string("line ") + std::to_string(line) + ": " + expression +
				" (missing '" + needle + "'; actual: " + text + ")");
		}
	}
#define CHECK_CONTAINS(text, needle) CheckContains((text), (needle), #text " contains " #needle, __LINE__)

	bool ColorEq(const Wui::WuiColor& a, const Wui::WuiColor& b)
	{
		return std::fabs(a.R - b.R) <= 1.0e-4f && std::fabs(a.G - b.G) <= 1.0e-4f &&
			std::fabs(a.B - b.B) <= 1.0e-4f && std::fabs(a.A - b.A) <= 1.0e-4f;
	}

	Wui::WuiColor ParsedColor(const char* text)
	{
		Wui::WuiColor color;
		Check(Wui::ParseComponentColor(text, color), text, __LINE__);
		return color;
	}

	constexpr float kDesignW = 1920.0f;
	constexpr float kDesignH = 1080.0f;
	constexpr float kPhysicalW = 1280.0f;
	constexpr float kPhysicalH = 720.0f;
	// 设计 1920x1080 → 物理 1280x720:宽高比相同 ⇒ 任何 Match 下 Scale 都是 2/3。
	constexpr float kScale = 2.0f / 3.0f;

	UiAnchor FullStretch()
	{
		UiAnchor anchor;
		anchor.Min = { 0.0f, 0.0f };
		anchor.Max = { 1.0f, 1.0f };
		anchor.Pivot = { 0.5f, 0.5f };
		return anchor;
	}

	UiAnchor PointAnchor(float x, float y, float w, float h)
	{
		UiAnchor anchor;
		anchor.Min = { 0.0f, 0.0f };
		anchor.Max = { 0.0f, 0.0f };
		anchor.Pivot = { 0.0f, 0.0f };
		anchor.Offset = { x, y };
		anchor.Size = { w, h };
		return anchor;
	}

	UiNode MakeNode(std::string id, std::string type)
	{
		UiNode node;
		node.Id = std::move(id);
		node.Type = std::move(type);
		return node;
	}

	void SetProp(UiNode& node, std::string name, std::string value)
	{
		node.Props.push_back(UiProp { std::move(name), std::move(value) });
	}

	UiDocument BaseDocument(std::vector<UiNode> roots)
	{
		UiDocument doc;
		doc.FormatVersion = kUiDocumentFormatVersion;
		doc.Screen = "HUD";
		doc.Design.Resolution = glm::vec2 { kDesignW, kDesignH };
		doc.Design.ScaleMode = UiScaleMode::ScaleWithScreenSize;
		doc.Design.Match = 0.5f;
		doc.Nodes = std::move(roots);
		return doc;
	}

	UiScreen BuildAndLayout(const UiDocument& doc)
	{
		UiScreen screen;
		std::string error;
		Check(screen.Build(doc, &error), ("screen build: " + error).c_str(), __LINE__);
		UiSurface surface;
		surface.PhysicalSize = glm::vec2 { kPhysicalW, kPhysicalH };
		surface.DpiScale = 1.0f;
		Check(screen.Layout(ComputeUiViewport(doc.Design, doc.SafeArea, surface)), "screen layout", __LINE__);
		return screen;
	}

	// 每帧开头宿主会 `BeginFrame`(命令清零 + 输入快照);绘制前必须已经这样开帧。
	void BeginContext(Wui::WuiContext& ctx)
	{
		Wui::WuiInputState input;
		input.ViewportSize = glm::vec2 { kPhysicalW, kPhysicalH };
		ctx.BeginFrame(input);
	}

	UiStyleDef TokenDef(std::string id, std::string propName, std::string value)
	{
		UiStyleDef def;
		def.Id = std::move(id);
		def.Props.push_back(UiProp { std::move(propName), std::move(value) });
		return def;
	}

	UiStyleDef SingleValueToken(std::string id, std::string value)
	{
		return TokenDef(std::move(id), "value", std::move(value));
	}

	// 单值令牌表查询器(`UiTokenFinder` 只在调用语句内有效:闭包持有 map 的副本)。
	UiTokenFinder SingleValueFinder(const std::map<std::string, std::string>& values)
	{
		return [values](std::string_view id, std::string_view) -> const std::string*
		{
			const auto it = values.find(std::string(id));
			return it != values.end() ? &it->second : nullptr;
		};
	}

	// 命令流查询(注意 `WuiContext::Commands()` 无 const 重载)。
	Wui::WuiDrawCommand* FindCommand(Wui::WuiContext& ctx, Wui::WuiDrawKind kind)
	{
		for (Wui::WuiDrawCommand& command : ctx.Commands())
		{
			if (command.Kind == kind)
				return &command;
		}
		return nullptr;
	}

	bool AnyCommandColorEquals(Wui::WuiContext& ctx, const Wui::WuiColor& color)
	{
		for (const Wui::WuiDrawCommand& command : ctx.Commands())
		{
			if (ColorEq(command.Color, color))
				return true;
		}
		return false;
	}

	bool AnyCommandTextContains(Wui::WuiContext& ctx, const char* needle)
	{
		for (const Wui::WuiDrawCommand& command : ctx.Commands())
		{
			if (command.Text.find(needle) != std::string::npos)
				return true;
		}
		return false;
	}

	// ---- 用例 ----

	void TestTokenReferenceSyntax()
	{
		CHECK(!IsUiTokenRef(""));
		CHECK(!IsUiTokenRef("$"));
		CHECK(!IsUiTokenRef("$   "));
		CHECK(!IsUiTokenRef("primary"));
		CHECK(!IsUiTokenRef("@ui.hud.title"));
		CHECK(!IsUiTokenRef("#RRGGBB"));
		CHECK(IsUiTokenRef("$primary"));
		CHECK(IsUiTokenRef("$ surface "));

		CHECK(UiTokenRefId("$primary") == "primary");
		CHECK(UiTokenRefId("$ surface ") == "surface");
		CHECK(UiTokenRefId("primary").empty());
		CHECK(UiTokenRefId("$").empty());

		const std::vector<std::string> chain { "a", "b" };
		CHECK(FormatUiTokenChain(chain) == "$a -> $b");
		CHECK(FormatUiTokenChain({}).empty());
	}

	void TestResolveLiteralChainsAndMissing()
	{
		const std::map<std::string, std::string> table {
			{ "primary", "#FF0000FF" },
			{ "a", "$b" },
			{ "b", "$c" },
			{ "c", "#123456FF" },
		};
		const UiTokenFinder find = SingleValueFinder(table);

		// 非令牌值 = 原样(旧文档行为不变)。
		const UiTokenResult literal = ResolveUiTokenValue("#AABBCC", "bg", find);
		CHECK(literal.Ok);
		CHECK(!literal.WasToken);
		CHECK(literal.Value == "#AABBCC");
		CHECK(literal.Error.empty());

		// 单级引用。
		const UiTokenResult one = ResolveUiTokenValue("$primary", "bg", find);
		CHECK(one.Ok);
		CHECK(one.WasToken);
		CHECK(one.Value == "#FF0000FF");
		CHECK(one.Chain == "$primary");

		// 多级引用。
		const UiTokenResult many = ResolveUiTokenValue("$a", "color", find);
		CHECK(many.Ok);
		CHECK(many.Value == "#123456FF");
		CHECK(many.Chain == "$a -> $b -> $c");

		// 未定义 = 可读错误 + 空值(调用方按"未设置"处理)。
		const UiTokenResult missing = ResolveUiTokenValue("$missing", "bg", find);
		CHECK(!missing.Ok);
		CHECK(missing.Value.empty());
		CHECK_CONTAINS(missing.Error, "undefined UI token '$missing'");
		CHECK_CONTAINS(missing.Error, "property 'bg'");
	}

	void TestResolveCyclesAndDepthGuard()
	{
		const std::map<std::string, std::string> two { { "a", "$b" }, { "b", "$a" } };
		const UiTokenResult cycle = ResolveUiTokenValue("$a", "value", SingleValueFinder(two));
		CHECK(!cycle.Ok);
		CHECK(cycle.Value.empty());
		CHECK(cycle.Error == "token cycle: $a -> $b -> $a");

		const std::map<std::string, std::string> self { { "a", "$a" } };
		const UiTokenResult selfCycle = ResolveUiTokenValue("$a", "value", SingleValueFinder(self));
		CHECK(!selfCycle.Ok);
		CHECK(selfCycle.Error == "token cycle: $a -> $a");

		// 深度防御:超长链报错,不无限循环。
		std::map<std::string, std::string> deep;
		for (int i = 0; i < 40; ++i)
			deep["t" + std::to_string(i)] = "$t" + std::to_string(i + 1);
		deep["t40"] = "#000000FF";
		const UiTokenResult tooDeep = ResolveUiTokenValue("$t0", "value", SingleValueFinder(deep));
		CHECK(!tooDeep.Ok);
		CHECK_CONTAINS(tooDeep.Error, "too deep");
	}

	void TestDocumentTokenLookup()
	{
		UiDocument doc = BaseDocument({});
		UiStyleDef surface = TokenDef("surface", "bg", "#111111FF");
		surface.Props.push_back(UiProp { "border", "#222222FF" });
		doc.Tokens.push_back(std::move(surface));
		doc.Tokens.push_back(SingleValueToken("primary", "#FF0000FF"));
		doc.Styles.push_back(SingleValueToken("primary", "#00FF00FF"));
		doc.Styles.push_back(SingleValueToken("legacy", "#0000FFFF"));

		// 属性名精确命中(样式继承):同一个令牌在不同属性上给不同值。
		const std::string* bg = FindUiToken(doc, "surface", "bg");
		const std::string* border = FindUiToken(doc, "surface", "border");
		CHECK(bg != nullptr && *bg == "#111111FF");
		CHECK(border != nullptr && *border == "#222222FF");
		// 多属性令牌没有该属性名 = 未定义(不跨属性猜)。
		CHECK(FindUiToken(doc, "surface", "radius") == nullptr);
		// Tokens 优先于 Styles(同名 primary)。
		const std::string* primary = FindUiToken(doc, "primary", "bg");
		CHECK(primary != nullptr && *primary == "#FF0000FF");
		// Styles 作为兼容回退仍可解析(旧的局部令牌写法)。
		const std::string* legacy = FindUiToken(doc, "legacy", "bg");
		CHECK(legacy != nullptr && *legacy == "#0000FFFF");
		CHECK(FindUiToken(doc, "nope", "bg") == nullptr);

		CHECK(ResolveUiToken(doc, "bg", "$surface").Value == "#111111FF");
		CHECK(ResolveUiToken(doc, "bg", "$primary").Value == "#FF0000FF");
		CHECK(ResolveUiToken(doc, "bg", "$legacy").Value == "#0000FFFF");
		CHECK(!ResolveUiToken(doc, "bg", "$nope").Ok);
		CHECK(ValidateUiTokens(doc, nullptr));
	}

	void TestTokensDrivePaintAndAccessibility()
	{
		Wui::WuiAccessibility& accessibility = Wui::WuiAccessibility::Get();
		accessibility.SetEnabled(true);
		accessibility.BeginFrame("main", glm::vec2 { kPhysicalW, kPhysicalH });

		UiStyleDef surface;
		surface.Id = "surface";
		surface.Props.push_back(UiProp { "bg", "#204060FF" });
		surface.Props.push_back(UiProp { "border", "#808890FF" });

		UiDocument doc = BaseDocument({});
		doc.Tokens.push_back(std::move(surface));
		doc.Tokens.push_back(SingleValueToken("accent", "#00FF00FF"));
		doc.Tokens.push_back(SingleValueToken("playText", "Play"));
		// 同名局部令牌:必须被 Tokens 覆盖(证明"Tokens 优先")。
		UiStyleDef localSurface;
		localSurface.Id = "surface";
		localSurface.Props.push_back(UiProp { "bg", "#FF0000FF" });
		localSurface.Props.push_back(UiProp { "border", "#FF0000FF" });
		doc.Styles.push_back(std::move(localSurface));

		UiNode root = MakeNode("root", "Panel");
		root.Anchor = FullStretch();
		SetProp(root, "bg", "$surface");
		SetProp(root, "border", "$surface");

		UiNode play = MakeNode("play", "Button");
		play.Anchor = PointAnchor(32.0f, 32.0f, 160.0f, 40.0f);
		SetProp(play, "label", "$playText");
		SetProp(play, "bg", "$accent");
		root.Children.push_back(std::move(play));
		doc.Nodes.push_back(std::move(root));

		const UiScreen screen = BuildAndLayout(doc);
		CHECK(screen.Count() == 2);

		Wui::WuiContext ctx;
		BeginContext(ctx);
		const UiPaintResult result = UiPainter::Paint(ctx, screen);

		CHECK(result.Ok());
		CHECK(result.Errors.empty());
		CHECK(result.Warnings.empty());
		CHECK(result.DrawnNodes == 2);
		CHECK(result.AccessNodes == 2);

		// Panel:命令 0 = bg 填充,命令 1 = border 描边(都是令牌解析后的颜色,
		// 且证明"同名属性精确命中"而不是字符串 `$surface`)。
		const Wui::WuiColor bg = ParsedColor("#204060FF");
		const Wui::WuiColor border = ParsedColor("#808890FF");
		const Wui::WuiColor accent = ParsedColor("#00FF00FF");
		CHECK(ctx.Commands().size() >= 2);
		CHECK(ColorEq(ctx.Commands()[0].Color, bg));
		CHECK(ColorEq(ctx.Commands()[1].Color, border));
		// Button 的底 = $accent;文本 = $playText 解析后的字面值。
		CHECK(AnyCommandColorEquals(ctx, accent));
		Wui::WuiDrawCommand* text = FindCommand(ctx, Wui::WuiDrawKind::Text);
		CHECK(text != nullptr);
		CHECK(text->Text == "Play");
		CHECK(!AnyCommandTextContains(ctx, "$"));

		// 无障碍读到的是解析后的文本,`$` 不泄漏给 AI/读屏。
		const Wui::WuiAccessNode* playNode = accessibility.Find(Wui::HashId("play"));
		const Wui::WuiAccessNode* rootNode = accessibility.Find(Wui::HashId("root"));
		CHECK(playNode != nullptr);
		CHECK(rootNode != nullptr);
		CHECK(playNode->Label == "Play");
		CHECK(playNode->Label.find('$') == std::string::npos);
		CHECK(rootNode->Label.find('$') == std::string::npos);

		accessibility.SetEnabled(false);
	}

	void TestUndefinedTokenIsReadableErrorAndUnset()
	{
		UiNode title = MakeNode("title", "Label");
		title.Anchor = PointAnchor(16.0f, 16.0f, 300.0f, 32.0f);
		SetProp(title, "text", "Health");
		SetProp(title, "color", "$missingColor");
		SetProp(title, "fontSize", "$missingSize");
		const UiDocument doc = BaseDocument({ std::move(title) });

		const UiScreen screen = BuildAndLayout(doc);
		Wui::WuiContext ctx;
		BeginContext(ctx);
		const UiPaintResult result = UiPainter::Paint(ctx, screen);

		// 可读错误(不是静默);节点照画,两个属性都回退到默认值(按"未设置"处理)。
		CHECK(!result.Ok());
		CHECK(result.DrawnNodes == 1);      // 未解析的令牌不跳过节点
		CHECK(result.Errors.size() == 2);
		CHECK(result.Errors[0].Path == "title");
		CHECK(result.Errors[0].Type == "Label");
		std::string joined;
		for (const UiPaintError& error : result.Errors)
			joined += error.Message + "\n";
		CHECK_CONTAINS(joined, "undefined UI token '$missingColor'");
		CHECK_CONTAINS(joined, "undefined UI token '$missingSize'");
		CHECK_CONTAINS(joined, "treated as unset");

		Wui::WuiDrawCommand* text = FindCommand(ctx, Wui::WuiDrawKind::Text);
		CHECK(text != nullptr);
		CHECK(text->Text == "Health");
		CHECK(ColorEq(text->Color, Wui::CurrentTheme().Text));   // 回退到主题默认,不是黑/0
		CHECK_NEAR(text->FontSize, 15.0f * kScale, 0.01f);       // 回退到字号默认,不是 0
	}

	void TestCycleIsReportedAtPaintAndByValidation()
	{
		UiDocument doc = BaseDocument({});
		doc.Tokens.push_back(SingleValueToken("a", "$b"));
		doc.Tokens.push_back(SingleValueToken("b", "$a"));

		UiNode title = MakeNode("title", "Label");
		title.Anchor = PointAnchor(16.0f, 16.0f, 300.0f, 32.0f);
		SetProp(title, "text", "Health");
		SetProp(title, "color", "$a");
		doc.Nodes.push_back(std::move(title));

		// 令牌环不影响加载/构建(M14 是兼容追加;环在解析期给可读错误并"不崩")。
		const UiScreen screen = BuildAndLayout(doc);

		Wui::WuiContext ctx;
		BeginContext(ctx);
		const UiPaintResult result = UiPainter::Paint(ctx, screen);
		CHECK(!result.Ok());
		CHECK(result.DrawnNodes == 1);
		CHECK(result.Errors.size() == 1);
		CHECK(result.Errors[0].Path == "title");
		CHECK_CONTAINS(result.Errors[0].Message, "token cycle: $a -> $b -> $a");
		CHECK_CONTAINS(result.Errors[0].Message, "treated as unset");

		Wui::WuiDrawCommand* text = FindCommand(ctx, Wui::WuiDrawKind::Text);
		CHECK(text != nullptr);
		CHECK(ColorEq(text->Color, Wui::CurrentTheme().Text));

		// 结构校验也独立报出环(不依赖绘制)。
		std::vector<UiValidationIssue> issues;
		CHECK(!ValidateUiTokens(doc, &issues));
		CHECK(!issues.empty());
		CHECK_CONTAINS(issues[0].Message, "token cycle");
	}

	void TestLegacyDocumentWithoutTokensIsUnchanged()
	{
		UiNode title = MakeNode("title", "Label");
		title.Anchor = PointAnchor(16.0f, 16.0f, 300.0f, 32.0f);
		SetProp(title, "text", "Health");
		const UiDocument doc = BaseDocument({ std::move(title) });

		// 追加键:空令牌表不写盘(旧文档序列化逐字节不变)。
		const std::string text = UiDocumentIO::Serialize(doc);
		CHECK(text.find("Tokens") == std::string::npos);

		UiDocument parsed;
		std::string error;
		CHECK(UiDocumentIO::Parse(text, parsed, &error));
		CHECK(error.empty());
		CHECK(UiDocumentsEquivalent(doc, parsed));

		const UiScreen screen = BuildAndLayout(parsed);
		Wui::WuiContext ctx;
		BeginContext(ctx);
		const UiPaintResult result = UiPainter::Paint(ctx, screen);
		CHECK(result.Ok());                 // 没有令牌引用 ⇒ 没有令牌错误
		CHECK(result.Errors.empty());
		CHECK(result.Warnings.empty());
		CHECK(result.DrawnNodes == 1);
	}

	const char* const kTokenDocument = R"YAML(FormatVersion: 1
Screen: HUD
Design:
  Resolution: [1920, 1080]
  ScaleMode: ScaleWithScreenSize
  Match: 0.5
Tokens:
  - Id: primary
    Props:
      value: "$base"
  - Id: base
    Props:
      value: "#112233FF"
Styles:
  - Id: primary
    Props:
      bg: "#FFFFFFFF"
Nodes:
  - Id: root
    Type: Panel
    Props:
      bg: "$primary"
    Anchor:
      Min: [0, 0]
      Max: [1, 1]
      Pivot: [0.5, 0.5]
      Offset: [0, 0]
      Size: [0, 0]
)YAML";

	void TestDocumentIORoundTripWithTokens()
	{
		UiDocument doc;
		std::string error;
		CHECK(UiDocumentIO::Parse(kTokenDocument, doc, &error));
		CHECK(error.empty());
		CHECK(doc.Tokens.size() == 2);
		CHECK(doc.Styles.size() == 1);

		// Tokens 优先:同名 `primary` 在 Styles 里是 bg=#FFFFFFFF,但解析走 Tokens 的多级链。
		const UiTokenResult resolved = ResolveUiToken(doc, "bg", "$primary");
		CHECK(resolved.Ok);
		CHECK(resolved.Value == "#112233FF");
		CHECK(resolved.Chain == "$primary -> $base");
		CHECK(ValidateUiTokens(doc, nullptr));

		// 令牌值参与绘制:命令颜色 = 解析后的颜色。
		const UiScreen screen = BuildAndLayout(doc);
		Wui::WuiContext ctx;
		BeginContext(ctx);
		const UiPaintResult result = UiPainter::Paint(ctx, screen);
		CHECK(result.Ok());
		CHECK(!ctx.Commands().empty());
		CHECK(ColorEq(ctx.Commands()[0].Color, ParsedColor("#112233FF")));

		// round-trip:序列化含 Tokens,再解析回来行为等价。
		const std::string text = UiDocumentIO::Serialize(doc);
		CHECK(text.find("Tokens") != std::string::npos);
		UiDocument reparsed;
		CHECK(UiDocumentIO::Parse(text, reparsed, &error));
		CHECK(error.empty());
		CHECK(UiDocumentsEquivalent(doc, reparsed));
	}

	void TestApplyTokenOverrides()
	{
		UiDocument doc = BaseDocument({});
		UiStyleDef primary;
		primary.Id = "primary";
		primary.Props.push_back(UiProp { "bg", "#FF0000FF" });
		primary.Props.push_back(UiProp { "border", "#FF0000FF" });
		doc.Tokens.push_back(std::move(primary));

		doc.ApplyTokenOverrides({ { "primary", "#00FF00FF" }, { "accent", "#0000FFFF" } });

		CHECK(doc.Tokens.size() == 2);   // 命中 = 改写;未命中 = 追加
		const std::string* primaryBg = FindUiToken(doc, "primary", "bg");
		const std::string* primaryBorder = FindUiToken(doc, "primary", "border");
		const std::string* accent = FindUiToken(doc, "accent", "bg");
		CHECK(primaryBg != nullptr && *primaryBg == "#00FF00FF");
		CHECK(primaryBorder != nullptr && *primaryBorder == "#00FF00FF");
		CHECK(accent != nullptr && *accent == "#0000FFFF");
		CHECK(ResolveUiToken(doc, "bg", "$accent").Value == "#0000FFFF");
	}
}

int main()
{
	try
	{
		TestTokenReferenceSyntax();
		TestResolveLiteralChainsAndMissing();
		TestResolveCyclesAndDepthGuard();
		TestDocumentTokenLookup();
		TestTokensDrivePaintAndAccessibility();
		TestUndefinedTokenIsReadableErrorAndUnset();
		TestCycleIsReportedAtPaintAndByValidation();
		TestLegacyDocumentWithoutTokensIsUnchanged();
		TestDocumentIORoundTripWithTokens();
		TestApplyTokenOverrides();
	}
	catch (const std::exception& e)
	{
		std::fprintf(stderr, "World.UiTheme FAILED: %s\n", e.what());
		return 1;
	}
	std::printf("World.UiTheme OK\n");
	return 0;
}
