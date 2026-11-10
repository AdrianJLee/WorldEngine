// World.UiScroll — GameUI(M10)滚动容器 + 商业控件面,headless。
//
// 覆盖(派工单验收):
//   ① 滚动钳位:内容 > 容器 ⇒ 钳到 max(0, 内容 − 容器);内容 ≤ 容器 ⇒ 偏移恒 0;
//   ② 裁剪一致:容器外的子节点既不被画/登记,也不被 `HitTest` 命中;容器内正常命中;
//   ③ 虚拟化:1000 行 List/Grid 的命令数与"可见行数"同阶(给出实际数字);
//   ④ 新类型 Slider/Checkbox/TextField/List/Grid:每件至少一条命令 + 正确 Role/Kind;
//   ⑤ 未知类型仍可读报错(既有行为不回退);
//   ⑥ 裁剪命令成对(ClipPush == ClipPop),且滚动容器判定与 `UiInputRouter` 同口径。
//
// 说明:本文件不构建、不渲染;只断言命令流、无障碍树与命中结果。

#include "World/UI/UiInputRouter.h"
#include "World/UI/UiNodeRegistry.h"
#include "World/UI/UiPainter.h"
#include "World/UI/UiScreen.h"
#include "World/UI/UiTypes.h"
#include "World/WUI/WuiAccessibility.h"
#include "World/WUI/WuiContext.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
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

	// 设计 = 物理 = 1920x1080(ConstantPixelSize)⇒ Scale = 1,命中坐标 = 设计坐标,便于手算。
	constexpr float kW = 1920.0f;
	constexpr float kH = 1080.0f;

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
		doc.Screen = "Scroll";
		doc.Design.Resolution = glm::vec2 { kW, kH };
		doc.Design.ScaleMode = UiScaleMode::ConstantPixelSize;
		doc.Nodes = std::move(roots);
		return doc;
	}

	UiViewport MakeViewport()
	{
		UiDesign design;
		design.Resolution = { kW, kH };
		design.ScaleMode = UiScaleMode::ConstantPixelSize;
		return ComputeUiViewport(design, UiSafeArea {}, UiSurface { { kW, kH }, 1.0f });
	}

	UiScreen BuildAndLayout(const UiDocument& doc)
	{
		UiScreen screen;
		std::string error;
		Check(screen.Build(doc, &error), ("screen build: " + error).c_str(), __LINE__);
		Check(screen.Layout(MakeViewport()), "screen layout", __LINE__);
		return screen;
	}

	void BeginContext(Wui::WuiContext& ctx)
	{
		Wui::WuiInputState input;
		input.ViewportSize = glm::vec2 { kW, kH };
		ctx.BeginFrame(input);
	}

	std::size_t CountKind(Wui::WuiContext& ctx, Wui::WuiDrawKind kind)
	{
		std::size_t count = 0;
		for (const Wui::WuiDrawCommand& command : ctx.Commands())
		{
			if (command.Kind == kind)
				++count;
		}
		return count;
	}

	// 含 `scrollable: true` 的滚动容器 + 一个不溢出的滚动容器 + 一个非滚动容器。
	UiDocument ScrollDocument()
	{
		UiNode root = MakeNode("root", "Panel");
		root.Anchor = FullStretch();

		UiNode scroller = MakeNode("scroller", "Panel");
		scroller.Anchor = PointAnchor(0.0f, 0.0f, 200.0f, 200.0f);
		SetProp(scroller, "scrollable", "true");

		UiNode above = MakeNode("above", "Label");        // 初始 y=-40;滚 150 后完全滚出容器上沿
		above.Anchor = PointAnchor(0.0f, -40.0f, 100.0f, 50.0f);
		SetProp(above, "text", "Above");
		scroller.Children.push_back(std::move(above));

		UiNode item = MakeNode("item", "Label");          // 初始 y=300 ⇒ 内容高 350 > 容器 200
		item.Anchor = PointAnchor(0.0f, 300.0f, 100.0f, 50.0f);
		SetProp(item, "text", "Item");
		scroller.Children.push_back(std::move(item));
		root.Children.push_back(std::move(scroller));

		UiNode small = MakeNode("small", "Panel");        // 无子 ⇒ 内容 = 容器 ⇒ 偏移恒 0
		small.Anchor = PointAnchor(300.0f, 0.0f, 300.0f, 300.0f);
		SetProp(small, "scrollable", "true");
		root.Children.push_back(std::move(small));

		return BaseDocument({ std::move(root) });
	}

	// ---- ① 滚动钳位 ----

	void TestScrollClamp()
	{
		const UiDocument doc = ScrollDocument();
		UiScreen screen = BuildAndLayout(doc);

		CHECK(screen.IsScrollContainer("scroller"));
		CHECK(screen.IsScrollContainer("small"));
		CHECK(!screen.IsScrollContainer("root"));     // 未声明 scrollable/overflow = 非滚动容器
		CHECK(!screen.IsScrollContainer("item"));

		const glm::vec2 content = screen.ScrollContentSize("scroller");
		CHECK_NEAR(content.x, 200.0f, 0.01f);
		CHECK_NEAR(content.y, 350.0f, 0.01f);         // 子的包围盒(设计空间)
		CHECK_NEAR(screen.MaxScrollOffset("scroller", 1), 150.0f, 0.01f);
		CHECK_NEAR(screen.MaxScrollOffset("scroller", 0), 0.0f, 0.01f);

		// 内容 > 容器:钳到 max(0, 内容 − 容器) = 150。
		screen.SetScrollOffset("scroller", glm::vec2 { 0.0f, 9999.0f });
		CHECK_NEAR(screen.ScrollOffset("scroller").y, 150.0f, 0.01f);
		CHECK_NEAR(screen.ScrollOffset("scroller").x, 0.0f, 0.01f);   // 横向不溢出 ⇒ 0
		screen.SetScrollOffset("scroller", glm::vec2 { 50.0f, 10.0f });
		CHECK_NEAR(screen.ScrollOffset("scroller").x, 0.0f, 0.01f);   // 请求横向 50 ⇒ 钳回 0
		CHECK_NEAR(screen.ScrollOffset("scroller").y, 10.0f, 0.01f);

		// 负偏移 = 0(不产生反向滚动)。
		screen.SetScrollOffset("scroller", glm::vec2 { -100.0f, -100.0f });
		CHECK_NEAR(screen.ScrollOffset("scroller").x, 0.0f, 0.01f);
		CHECK_NEAR(screen.ScrollOffset("scroller").y, 0.0f, 0.01f);

		// 内容 ≤ 容器(无子)⇒ 偏移恒 0。
		screen.SetScrollOffset("small", glm::vec2 { 0.0f, 120.0f });
		CHECK_NEAR(screen.ScrollOffset("small").y, 0.0f, 0.01f);

		// 非滚动容器 = 忽略(不新增运行态)。
		screen.SetScrollOffset("root", glm::vec2 { 0.0f, 100.0f });
		CHECK_NEAR(screen.ScrollOffset("root").y, 0.0f, 0.01f);

		// 偏移立即作用到子节点矩阵(绘制与命中同一映射)。
		screen.SetScrollOffset("scroller", glm::vec2 { 0.0f, 150.0f });
		const UiNodeInstance* item = screen.Find("item");
		CHECK(item != nullptr);
		CHECK_NEAR(item->Rect.Y, 150.0f, 0.01f);      // 300 − 150

		// 重新 Layout 后偏移仍生效(运行态跨帧存活)。
		CHECK(screen.Layout(MakeViewport()));
		CHECK_NEAR(screen.ScrollOffset("scroller").y, 150.0f, 0.01f);
		CHECK_NEAR(screen.Find("item")->Rect.Y, 150.0f, 0.01f);
	}

	// ---- ② 裁剪:绘制/无障碍/命中同一口径 ----

	void TestScrollClipConsistency()
	{
		Wui::WuiAccessibility& accessibility = Wui::WuiAccessibility::Get();
		accessibility.SetEnabled(true);
		accessibility.BeginFrame("main", glm::vec2 { kW, kH });

		const UiDocument doc = ScrollDocument();
		UiScreen screen = BuildAndLayout(doc);
		screen.SetScrollOffset("scroller", glm::vec2 { 0.0f, 150.0f });

		Wui::WuiContext ctx;
		BeginContext(ctx);
		const UiPaintResult result = UiPainter::Paint(ctx, screen);

		CHECK(result.Ok());
		CHECK(result.Warnings.empty());                // `scrollable` 是声明属性,不是未知属性
		// `above` 初始 y=-40,滚 150 后 y=-190 —— 完全在裁剪外:不画、不登记。
		CHECK(result.ClippedNodes == 1);
		CHECK(result.DrawnNodes == 4);                 // root / scroller / item / small
		CHECK(accessibility.Find(Wui::HashId("above")) == nullptr);
		CHECK(accessibility.Find(Wui::HashId("item")) != nullptr);

		// 裁剪命令成对(渲染命令与裁剪栈同进同出)。
		CHECK(CountKind(ctx, Wui::WuiDrawKind::ClipPush) == 2);
		CHECK(CountKind(ctx, Wui::WuiDrawKind::ClipPop) == 2);
		CHECK(ctx.Commands().size() == result.Commands);

		// 命中:容器外的子节点不可命中(点落在 `above` 矩形内,但容器之外)。
		CHECK(screen.HitTest(glm::vec2 { 50.0f, -165.0f }) == nullptr);
		// 容器内:命中最上层子节点;容器空白处命中容器自身。
		const UiNodeInstance* hitItem = screen.HitTest(glm::vec2 { 50.0f, 175.0f });
		CHECK(hitItem != nullptr);
		CHECK(hitItem->Id == "item");
		const UiNodeInstance* hitScroller = screen.HitTest(glm::vec2 { 150.0f, 50.0f });
		CHECK(hitScroller != nullptr);
		CHECK(hitScroller->Id == "scroller");

		// 未滚动的容器内正常命中(对照:裁剪只在容器外生效)。
		const UiNodeInstance* hitSmall = screen.HitTest(glm::vec2 { 400.0f, 100.0f });
		CHECK(hitSmall != nullptr);
		CHECK(hitSmall->Id == "small");

		accessibility.SetEnabled(false);
	}

	// ---- ③ 虚拟化:1000 行只画可见行 ----

	UiDocument ListDocument(int rows)
	{
		UiNode root = MakeNode("root", "Panel");
		root.Anchor = FullStretch();

		UiNode list = MakeNode("list", "List");
		list.Anchor = PointAnchor(0.0f, 0.0f, 300.0f, 100.0f);
		SetProp(list, "scrollable", "true");
		SetProp(list, "rowCount", std::to_string(rows));
		SetProp(list, "rowHeight", "24");
		root.Children.push_back(std::move(list));

		return BaseDocument({ std::move(root) });
	}

	std::size_t PaintListCommands(int rows, float scrollY, std::size_t* drawnOut = nullptr)
	{
		UiScreen screen = BuildAndLayout(ListDocument(rows));
		if (scrollY > 0.0f)
			screen.SetScrollOffset("list", glm::vec2 { 0.0f, scrollY });
		Wui::WuiContext ctx;
		BeginContext(ctx);
		const UiPaintResult result = UiPainter::Paint(ctx, screen);
		CHECK(result.Ok());
		CHECK(result.DrawnNodes == 2);                 // root + list
		if (drawnOut != nullptr)
			*drawnOut = ctx.Commands().size();
		return ctx.Commands().size();
	}

	void TestVirtualizedList()
	{
		const std::size_t commands1000 = PaintListCommands(1000, 480.0f);
		const std::size_t commands10 = PaintListCommands(10, 0.0f);
		CHECK(commands1000 > 10);
		CHECK(commands1000 < 60);                      // 1000 行不产生 1000 条命令
		CHECK(commands10 < 60);
		// 命令数与"可见行数"同阶,与总行数无关。
		const long long difference = static_cast<long long>(commands1000) - static_cast<long long>(commands10);
		CHECK(std::llabs(difference) <= 8);

		// 内容尺寸/钳位由类型自报(行不是文档子节点)。
		UiScreen screen = BuildAndLayout(ListDocument(1000));
		CHECK_NEAR(screen.ScrollContentSize("list").y, 24000.0f, 0.01f);
		screen.SetScrollOffset("list", glm::vec2 { 0.0f, 1.0e9f });
		CHECK_NEAR(screen.ScrollOffset("list").y, 23900.0f, 0.01f);   // 24000 − 100

		// `EnsureScrollVisible`:滚到底部时再要求可见 = 无改动。
		CHECK(!screen.EnsureScrollVisible("list", "list"));           // 自身不算后代
	}

	UiDocument GridDocument(int rows, int columns)
	{
		UiNode root = MakeNode("root", "Panel");
		root.Anchor = FullStretch();

		UiNode grid = MakeNode("grid", "Grid");
		grid.Anchor = PointAnchor(0.0f, 0.0f, 300.0f, 100.0f);
		SetProp(grid, "scrollable", "true");
		SetProp(grid, "rowCount", std::to_string(rows));
		SetProp(grid, "columns", std::to_string(columns));
		SetProp(grid, "cellW", "75");
		SetProp(grid, "cellH", "24");
		root.Children.push_back(std::move(grid));

		return BaseDocument({ std::move(root) });
	}

	void TestVirtualizedGrid()
	{
		UiScreen screen = BuildAndLayout(GridDocument(1000, 4));
		CHECK_NEAR(screen.ScrollContentSize("grid").y, 24000.0f, 0.01f);
		CHECK_NEAR(screen.ScrollContentSize("grid").x, 300.0f, 0.01f);
		screen.SetScrollOffset("grid", glm::vec2 { 0.0f, 480.0f });

		Wui::WuiContext ctx;
		BeginContext(ctx);
		const UiPaintResult result = UiPainter::Paint(ctx, screen);
		CHECK(result.Ok());
		const std::size_t commands = ctx.Commands().size();
		CHECK(commands > 20);
		CHECK(commands < 200);                         // 4000 格全画会是 ~8000 条
	}

	// ---- ④ 五种新类型:真实绘制 + Role/Kind ----

	struct TypeExpectation
	{
		const char* Type;
		const char* Role;
		const char* ComponentId;
	};

	const TypeExpectation kNewTypes[5] = {
		{ "Slider", "slider", "slider.float" },
		{ "Checkbox", "checkbox", "checkbox" },
		{ "TextField", "text-field", "textfield" },
		{ "List", "list", "listview" },
		{ "Grid", "grid", "listview" },
	};

	UiNode MakeNewTypeNode(const std::string& type)
	{
		UiNode node = MakeNode("only", type);
		node.Anchor = PointAnchor(0.0f, 0.0f, 200.0f, 60.0f);
		if (type == "Slider")
		{
			SetProp(node, "value", "0.25");
			SetProp(node, "min", "0");
			SetProp(node, "max", "1");
			SetProp(node, "label", "Volume");
		}
		else if (type == "Checkbox")
		{
			SetProp(node, "checked", "true");
			SetProp(node, "label", "Enabled");
		}
		else if (type == "TextField")
		{
			SetProp(node, "value", "Player");
			SetProp(node, "placeholder", "Name");
		}
		else if (type == "List")
		{
			SetProp(node, "scrollable", "true");
			SetProp(node, "rowCount", "12");
			SetProp(node, "rowHeight", "24");
		}
		else if (type == "Grid")
		{
			SetProp(node, "scrollable", "true");
			SetProp(node, "rowCount", "12");
			SetProp(node, "columns", "3");
			SetProp(node, "cellW", "60");
			SetProp(node, "cellH", "24");
		}
		return node;
	}

	void TestNewNodeTypesPaintAndAccessibility()
	{
		Wui::WuiAccessibility& accessibility = Wui::WuiAccessibility::Get();

		for (const TypeExpectation& expected : kNewTypes)
		{
			const UiNodeTypeDesc* desc = UiNodeRegistry::Find(expected.Type);
			CHECK(desc != nullptr);
			CHECK(desc->Role == expected.Role);
			CHECK(desc->ComponentId == expected.ComponentId);
			CHECK(desc->Interactive);                              // 都能成为输入/命令目标
			CHECK(desc->Paint != nullptr);
			// ComponentId 必须命中组件登记表(不另起命名)。
			const Wui::WuiComponentDesc* component = UiNodeRegistry::Component(expected.Type);
			CHECK(component != nullptr);
			CHECK(component->Id == expected.ComponentId);

			accessibility.SetEnabled(true);
			accessibility.BeginFrame("main", glm::vec2 { kW, kH });

			const UiDocument doc = BaseDocument({ MakeNewTypeNode(expected.Type) });
			const UiScreen screen = BuildAndLayout(doc);
			Wui::WuiContext ctx;
			BeginContext(ctx);
			const UiPaintResult result = UiPainter::Paint(ctx, screen);

			CHECK(result.Ok());
			CHECK(result.Warnings.empty());
			CHECK(result.SkippedNodes == 0);
			CHECK(result.DrawnNodes == 1);
			CHECK(result.Commands > 0);

			const Wui::WuiAccessNode* node = accessibility.Find(Wui::HashId("only"));
			CHECK(node != nullptr);
			CHECK(node->Kind == expected.ComponentId);
			CHECK(node->Role == expected.Role);
			CHECK(node->Interactive);
			CHECK(node->Actions == "click");
			CHECK(node->Rect.W > 0.0f);

			accessibility.SetEnabled(false);
		}

		// 逐件的行为断言(值/状态来自属性,可被 AI 读到)。
		accessibility.SetEnabled(true);
		accessibility.BeginFrame("main", glm::vec2 { kW, kH });
		{
			const UiDocument doc = BaseDocument({ MakeNewTypeNode("Slider") });
			const UiScreen screen = BuildAndLayout(doc);
			Wui::WuiContext ctx;
			BeginContext(ctx);
			CHECK(UiPainter::Paint(ctx, screen).Ok());
			const Wui::WuiAccessNode* slider = accessibility.Find(Wui::HashId("only"));
			CHECK(slider != nullptr);
			CHECK(slider->Value == "0.250");
			CHECK(slider->Label == "Volume");
		}
		accessibility.BeginFrame("main", glm::vec2 { kW, kH });
		{
			const UiDocument doc = BaseDocument({ MakeNewTypeNode("Checkbox") });
			const UiScreen screen = BuildAndLayout(doc);
			Wui::WuiContext ctx;
			BeginContext(ctx);
			CHECK(UiPainter::Paint(ctx, screen).Ok());
			const Wui::WuiAccessNode* checkbox = accessibility.Find(Wui::HashId("only"));
			CHECK(checkbox != nullptr);
			CHECK(checkbox->States == "checked");
			CHECK(checkbox->Value == "true");
		}
		accessibility.BeginFrame("main", glm::vec2 { kW, kH });
		{
			const UiDocument doc = BaseDocument({ MakeNewTypeNode("List") });
			const UiScreen screen = BuildAndLayout(doc);
			Wui::WuiContext ctx;
			BeginContext(ctx);
			CHECK(UiPainter::Paint(ctx, screen).Ok());
			const Wui::WuiAccessNode* list = accessibility.Find(Wui::HashId("only"));
			CHECK(list != nullptr);
			CHECK(list->Value.find("items=12") != std::string::npos);
		}
		accessibility.SetEnabled(false);
	}

	// ---- ⑤ 未知类型仍可读报错 ----

	void TestUnknownTypeStillReadable()
	{
		UiNode root = MakeNode("root", "Panel");
		root.Anchor = FullStretch();
		UiNode ghost = MakeNode("ghost", "TotallyUnknown");
		ghost.Anchor = PointAnchor(10.0f, 10.0f, 50.0f, 20.0f);
		root.Children.push_back(std::move(ghost));

		const UiScreen screen = BuildAndLayout(BaseDocument({ std::move(root) }));
		Wui::WuiContext ctx;
		BeginContext(ctx);
		const UiPaintResult result = UiPainter::Paint(ctx, screen);

		CHECK(!result.Ok());
		CHECK(result.Errors.size() == 1);
		CHECK(result.Errors[0].Type == "TotallyUnknown");
		CHECK(result.Errors[0].Message.find("unknown UI node type") != std::string::npos);
		CHECK(result.SkippedNodes == 1);
		CHECK(result.DrawnNodes == 1);
	}

	// ---- ⑥ 判定口径与 `UiInputRouter::IsScrollContainer` 一致 ----

	void TestScrollContainerParityWithRouter()
	{
		struct Case
		{
			const char* Prop;
			const char* Value;
			bool Expected;
		};
		const Case cases[8] = {
			{ "scrollable", "true", true },
			{ "scrollable", "0", false },
			{ "scroll", "1", true },
			{ "overflow", "scroll", true },
			{ "overflow", "auto", true },
			{ "overflow", "Scroll", true },
			{ "overflow", "hidden", false },
			{ "overflow", "clip", false },
		};

		UiNode root = MakeNode("root", "Panel");
		root.Anchor = FullStretch();
		std::vector<std::string> ids;
		for (std::size_t i = 0; i < 8; ++i)
		{
			const std::string id = "c" + std::to_string(i);
			UiNode node = MakeNode(id, "Panel");
			node.Anchor = PointAnchor(static_cast<float>(i) * 10.0f, 0.0f, 50.0f, 50.0f);
			SetProp(node, cases[i].Prop, cases[i].Value);
			ids.push_back(id);
			root.Children.push_back(std::move(node));
		}

		const UiScreen screen = BuildAndLayout(BaseDocument({ std::move(root) }));
		for (std::size_t i = 0; i < ids.size(); ++i)
		{
			const UiNodeInstance* node = screen.Find(ids[i]);
			CHECK(node != nullptr);
			CHECK(screen.IsScrollContainer(*node) == cases[i].Expected);
			// 同一份口径:UiScreen(布局/绘制/命中用)与 UiInputRouter(输入用)必须一致。
			CHECK(UiInputRouter::IsScrollContainer(*node) == cases[i].Expected);
		}

		// `scrollable` 与 `overflow` 同时给出时,`scrollable` 真值优先(两边同序判断)。
		UiNode both = MakeNode("both", "Panel");
		both.Anchor = PointAnchor(0.0f, 0.0f, 50.0f, 50.0f);
		SetProp(both, "scrollable", "true");
		SetProp(both, "overflow", "hidden");
		const UiScreen bothScreen = BuildAndLayout(BaseDocument({ std::move(both) }));
		const UiNodeInstance* bothNode = bothScreen.Find("both");
		CHECK(bothNode != nullptr);
		CHECK(bothScreen.IsScrollContainer(*bothNode));
		CHECK(UiInputRouter::IsScrollContainer(*bothNode));
	}

	// ---- ⑦ 滚动容器的命中裁剪也适用于子容器(嵌套)----

	void TestEnsureScrollVisible()
	{
		UiScreen screen = BuildAndLayout(ScrollDocument());
		// `item` 在内容底部(300..350),容器 200 高 ⇒ 滚到 150 才可见。
		CHECK(screen.EnsureScrollVisible("scroller", "item"));
		CHECK_NEAR(screen.ScrollOffset("scroller").y, 150.0f, 0.01f);
		// 已经可见 = 无改动。
		CHECK(!screen.EnsureScrollVisible("scroller", "item"));
		// 非滚动容器 / 非后代 = false。
		CHECK(!screen.EnsureScrollVisible("root", "item"));
		CHECK(!screen.EnsureScrollVisible("scroller", "small"));
	}
}

int main()
{
	try
	{
		TestScrollClamp();
		TestScrollClipConsistency();
		TestVirtualizedList();
		TestVirtualizedGrid();
		TestNewNodeTypesPaintAndAccessibility();
		TestUnknownTypeStillReadable();
		TestScrollContainerParityWithRouter();
		TestEnsureScrollVisible();
	}
	catch (const std::exception& e)
	{
		std::fprintf(stderr, "World.UiScroll FAILED: %s\n", e.what());
		return 1;
	}
	std::printf("World.UiScroll OK\n");
	return 0;
}
