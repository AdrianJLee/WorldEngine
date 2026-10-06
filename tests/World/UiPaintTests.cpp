// World.UiPaint — GameUI(M2)drawing + accessibility registration, headless.
//
// 覆盖(派工单验收):
//   ① 六件内置类型(Panel/Label/Button/Image/ProgressBar/Toggle)各自至少画出一条命令;
//   ② 未知 Type → 可读错误 + 跳过该节点(不静默画空气),同批其它节点继续画;
//   ③ 无障碍节点数与 Role/Path/Label/Value/Rect 正确,且矩形 = 设计矩形经同一视口映射。
//
// 说明:本文件不构建、不渲染;只断言"命令进了 WuiContext、节点进了无障碍树"。

#include "World/UI/UiPainter.h"
#include "World/UI/UiNodeRegistry.h"
#include "World/UI/UiScreen.h"
#include "World/UI/UiTypes.h"
#include "World/WUI/WuiAccessibility.h"
#include "World/WUI/WuiContext.h"

#include <cmath>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <glm/glm.hpp>

namespace
{
	using namespace World;
	using namespace World::UI;

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

	void CheckRect(const Wui::WuiRect& rect, float x, float y, float w, float h, const char* expression, int line)
	{
		const bool ok = std::fabs(rect.X - x) <= 0.01f && std::fabs(rect.Y - y) <= 0.01f &&
			std::fabs(rect.W - w) <= 0.01f && std::fabs(rect.H - h) <= 0.01f;
		if (!ok)
		{
			throw std::runtime_error(std::string("line ") + std::to_string(line) + ": " + expression +
				" (actual [" + std::to_string(rect.X) + ", " + std::to_string(rect.Y) + ", " +
				std::to_string(rect.W) + ", " + std::to_string(rect.H) + "] expected [" +
				std::to_string(x) + ", " + std::to_string(y) + ", " + std::to_string(w) + ", " +
				std::to_string(h) + "])");
		}
	}
#define CHECK_RECT(rect, x, y, w, h) CheckRect((rect), (x), (y), (w), (h), #rect, __LINE__)

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

	// 横向拉伸:左边距 x、上边距 y、左右共内缩 inset、固定高 height。
	UiAnchor StretchRow(float x, float y, float inset, float height)
	{
		UiAnchor anchor;
		anchor.Min = { 0.0f, 0.0f };
		anchor.Max = { 1.0f, 0.0f };
		anchor.Pivot = { 0.0f, 0.0f };
		anchor.Offset = { x, y };
		anchor.Size = { -inset, height };
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

	// 派工单要求的六件内置类型 + 各自的属性写法(键必须在该类型登记属性表内)。
	const char* const kBuiltinTypes[6] = { "Panel", "Label", "Button", "Image", "ProgressBar", "Toggle" };

	UiNode MakeBuiltinNode(const std::string& type)
	{
		UiNode node = MakeNode("only", type);
		if (type == "Panel")
		{
			node.Anchor = FullStretch();
			SetProp(node, "title", "@ui.paint-test.missing-key");
		}
		else if (type == "Label")
		{
			node.Anchor = PointAnchor(16.0f, 16.0f, 200.0f, 32.0f);
			SetProp(node, "text", "Health");
		}
		else if (type == "Button")
		{
			node.Anchor = PointAnchor(16.0f, 16.0f, 160.0f, 40.0f);
			SetProp(node, "label", "Close");
		}
		else if (type == "Image")
		{
			node.Anchor = PointAnchor(16.0f, 16.0f, 96.0f, 96.0f);
			SetProp(node, "textureId", "0");
		}
		else if (type == "ProgressBar")
		{
			node.Anchor = PointAnchor(16.0f, 16.0f, 400.0f, 24.0f);
			SetProp(node, "value", "0.5");
		}
		else if (type == "Toggle")
		{
			node.Anchor = PointAnchor(16.0f, 16.0f, 200.0f, 32.0f);
			SetProp(node, "label", "Enabled");
			SetProp(node, "value", "true");
		}
		return node;
	}

	// 六件类型齐全的一份文档(供无障碍断言)。
	UiDocument SampleDocument()
	{
		UiNode root = MakeNode("root", "Panel");
		root.Anchor = FullStretch();
		SetProp(root, "title", "@ui.paint-test.missing-key");

		UiNode title = MakeNode("title", "Label");
		title.Anchor = PointAnchor(32.0f, 32.0f, 400.0f, 32.0f);
		SetProp(title, "text", "Health");

		UiNode close = MakeNode("close", "Button");
		close.Anchor = PointAnchor(32.0f, 96.0f, 160.0f, 40.0f);
		SetProp(close, "label", "Close");

		UiNode icon = MakeNode("icon", "Image");
		icon.Anchor = PointAnchor(32.0f, 160.0f, 96.0f, 96.0f);
		SetProp(icon, "textureId", "0");

		UiNode hp = MakeNode("hp", "ProgressBar");
		hp.Anchor = StretchRow(32.0f, 288.0f, 64.0f, 24.0f);
		SetProp(hp, "value", "0.5");

		UiNode opt = MakeNode("opt", "Toggle");
		opt.Anchor = PointAnchor(32.0f, 336.0f, 200.0f, 32.0f);
		SetProp(opt, "label", "Enabled");
		SetProp(opt, "value", "true");

		root.Children.push_back(std::move(title));
		root.Children.push_back(std::move(close));
		root.Children.push_back(std::move(icon));
		root.Children.push_back(std::move(hp));
		root.Children.push_back(std::move(opt));
		return BaseDocument({ std::move(root) });
	}

	void TestBuiltinTypesPaintCommands()
	{
		for (const char* type : kBuiltinTypes)
		{
			const UiNodeTypeDesc* desc = UiNodeRegistry::Find(type);
			CHECK(desc != nullptr);
			CHECK(!desc->Role.empty());
			CHECK(desc->Paint != nullptr);
			const Wui::WuiComponentDesc* component = UiNodeRegistry::Component(type);
			CHECK(component != nullptr);
			CHECK(component->Id == desc->ComponentId);

			const UiDocument doc = BaseDocument({ MakeBuiltinNode(type) });
			const UiScreen screen = BuildAndLayout(doc);
			Wui::WuiContext ctx;
			BeginContext(ctx);

			const UiPaintResult result = UiPainter::Paint(ctx, screen);
			CHECK(result.Ok());
			CHECK(result.Errors.empty());
			CHECK(result.DrawnNodes == 1);
			CHECK(result.SkippedNodes == 0);
			CHECK(result.Commands > 0);
			CHECK(ctx.Commands().size() == result.Commands);
		}
	}

	void TestAliasesResolveToSameRegistryEntry()
	{
		// `UiNodeRegistry` 复用组件登记口径:规范名/组件 id/结构体名指向同一件。
		CHECK(UiNodeRegistry::Find("Button") == UiNodeRegistry::Find("button"));
		CHECK(UiNodeRegistry::Find("ProgressBar") == UiNodeRegistry::Find("progress"));
		CHECK(UiNodeRegistry::Find("Label") == UiNodeRegistry::Find("WuiLabel"));
		CHECK(UiNodeRegistry::Find("NoSuchType") == nullptr);
	}

	void TestUnknownTypeIsReadableErrorAndSkipped()
	{
		UiNode root = MakeNode("root", "Panel");
		root.Anchor = FullStretch();
		UiNode ghost = MakeNode("ghost", "TotallyUnknown");
		ghost.Anchor = PointAnchor(10.0f, 10.0f, 50.0f, 20.0f);
		root.Children.push_back(std::move(ghost));

		const UiDocument doc = BaseDocument({ std::move(root) });
		const UiScreen screen = BuildAndLayout(doc);
		CHECK(screen.Count() == 2);

		Wui::WuiContext ctx;
		BeginContext(ctx);
		const UiPaintResult result = UiPainter::Paint(ctx, screen);

		CHECK(!result.Ok());
		CHECK(result.Errors.size() == 1);
		CHECK(result.Errors[0].Type == "TotallyUnknown");
		CHECK(result.Errors[0].Path == "root.ghost");
		CHECK(result.Errors[0].Message.find("unknown UI node type 'TotallyUnknown'") != std::string::npos);
		CHECK(result.Errors[0].Message.find("root.ghost") != std::string::npos);
		CHECK(result.SkippedNodes == 1);
		CHECK(result.DrawnNodes == 1);      // 未知节点不影响同批其它节点
		CHECK(result.Commands > 0);
	}

	void TestAccessibilityNodesRegistered()
	{
		Wui::WuiAccessibility& accessibility = Wui::WuiAccessibility::Get();
		accessibility.SetEnabled(true);
		accessibility.BeginFrame("main", glm::vec2 { kPhysicalW, kPhysicalH });

		const UiDocument doc = SampleDocument();
		const UiScreen screen = BuildAndLayout(doc);
		CHECK(screen.Count() == 6);

		Wui::WuiContext ctx;
		BeginContext(ctx);
		const UiPaintResult result = UiPainter::Paint(ctx, screen);

		CHECK(result.Ok());
		CHECK(result.Warnings.empty());     // 示例文档的属性全在登记属性表 ∪ ExtraProps 内
		CHECK(result.DrawnNodes == 6);
		CHECK(result.AccessNodes == 6);
		CHECK(accessibility.Nodes().size() == 6);

		const Wui::WuiAccessNode* root = accessibility.Find(Wui::HashId("root"));
		CHECK(root != nullptr);
		CHECK(root->Role == "group");
		CHECK(root->Kind == "box");          // 复用组件登记 id(Panel → box)
		CHECK(root->Path == "root");
		CHECK(root->Page == "HUD");
		CHECK(root->Layer == 0);
		CHECK(root->Label == "ui.paint-test.missing-key");   // 缺 key → 回退去掉 '@' 的字面量
		CHECK(root->Interactive == false);
		CHECK(root->Visible);

		const Wui::WuiAccessNode* hp = accessibility.Find(Wui::HashId("hp"));
		CHECK(hp != nullptr);
		CHECK(hp->Role == "progressbar");
		CHECK(hp->Kind == "progress");
		CHECK(hp->Path == "root.hp");
		CHECK(hp->Value == "0.500");
		CHECK(hp->Interactive == false);
		CHECK(hp->Visible);
		// 窗口坐标 = 物理坐标:设计 {32,288,1856,24} × 2/3。
		CHECK_RECT(hp->Rect, 32.0f * kScale, 288.0f * kScale, 1856.0f * kScale, 24.0f * kScale);

		const Wui::WuiAccessNode* close = accessibility.Find(Wui::HashId("close"));
		CHECK(close != nullptr);
		CHECK(close->Role == "button");
		CHECK(close->Kind == "button");
		CHECK(close->Label == "Close");
		CHECK(close->Actions == "click");
		CHECK(close->Interactive);
		CHECK(close->States.empty());

		const Wui::WuiAccessNode* opt = accessibility.Find(Wui::HashId("opt"));
		CHECK(opt != nullptr);
		CHECK(opt->Role == "checkbox");
		CHECK(opt->Value == "on");
		CHECK(opt->States == "checked");
		CHECK(opt->Interactive);

		const Wui::WuiAccessNode* title = accessibility.Find(Wui::HashId("title"));
		CHECK(title != nullptr);
		CHECK(title->Role == "text");
		CHECK(title->Label == "Health");

		const Wui::WuiAccessNode* icon = accessibility.Find(Wui::HashId("icon"));
		CHECK(icon != nullptr);
		CHECK(icon->Role == "img");
		CHECK(icon->Value.empty());          // textureId=0 = 空图槽位(仍画可见占位框)
		CHECK(icon->Rect.W > 0.0f);

		// 绘制坐标与命中坐标是同一映射:用 a11y 矩形中心反查命中,必须命中同一节点。
		const UiNodeInstance* hit = screen.HitTest(glm::vec2 { hp->Rect.X + hp->Rect.W * 0.5f,
			hp->Rect.Y + hp->Rect.H * 0.5f });
		CHECK(hit != nullptr);
		CHECK(hit->Id == "hp");

		accessibility.SetEnabled(false);
	}

	void TestAccessibilityOffRegistersNothing()
	{
		Wui::WuiAccessibility& accessibility = Wui::WuiAccessibility::Get();
		accessibility.SetEnabled(false);
		accessibility.Clear();

		const UiDocument doc = SampleDocument();
		const UiScreen screen = BuildAndLayout(doc);
		Wui::WuiContext ctx;
		BeginContext(ctx);
		const UiPaintResult result = UiPainter::Paint(ctx, screen);

		CHECK(result.Ok());
		CHECK(result.AccessNodes == 0);
		CHECK(accessibility.Nodes().empty());
		CHECK(result.Commands > 0);     // 控制通道关闭不影响绘制
	}
}

int main()
{
	try
	{
		TestBuiltinTypesPaintCommands();
		TestAliasesResolveToSameRegistryEntry();
		TestUnknownTypeIsReadableErrorAndSkipped();
		TestAccessibilityNodesRegistered();
		TestAccessibilityOffRegistersNothing();
	}
	catch (const std::exception& e)
	{
		std::fprintf(stderr, "World.UiPaint FAILED: %s\n", e.what());
		return 1;
	}
	std::printf("World.UiPaint OK\n");
	return 0;
}
