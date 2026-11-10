#include "World/UI/UiDocument.h"
#include "World/UI/UiScreen.h"
#include "World/UI/UiTypes.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{
	using namespace World::UI;
	using namespace World;

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
#define CHECK_NEAR(actual, expected, tolerance) CheckNear(static_cast<float>(actual), static_cast<float>(expected), static_cast<float>(tolerance), #actual " ~= " #expected, __LINE__)

	void CheckRect(const Wui::WuiRect& rect, float x, float y, float w, float h, const char* expression, int line)
	{
		const bool ok = std::fabs(rect.X - x) <= 0.001f && std::fabs(rect.Y - y) <= 0.001f &&
			std::fabs(rect.W - w) <= 0.001f && std::fabs(rect.H - h) <= 0.001f;
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

	const char* kSampleDocument = R"YAML(FormatVersion: 1
Screen: HUD
Design:
  Resolution: [1920, 1080]
  ScaleMode: ScaleWithScreenSize
  Match: 0.5
SafeArea:
  Enabled: true
  Insets: [0, 44, 0, 21]
Theme: ui/default
Parameters:
  - Name: Title
    Type: Text
    Default: "@ui.hud.title"
Nodes:
  - Id: root
    Type: Panel
    Props:
      title: "@ui.hud.title"
    Anchor:
      Min: [0, 0]
      Max: [1, 1]
      Pivot: [0.5, 0.5]
      Offset: [0, 0]
      Size: [0, 0]
    Layout:
      Kind: Column
      Gap: 8
      Padding: [12, 12, 12, 12]
    Children:
      - Id: hp
        Type: ProgressBar
        Bind:
          Value: player.health
          Max: player.healthMax
        Anchor:
          Min: [0, 0]
          Max: [1, 0]
          Offset: [0, 0]
          Size: [0, 28]
      - Id: close
        Type: Button
        Props:
          label: "@ui.common.close"
        On:
          Click: ui.close
)YAML";

	UiDocument MakeDocument()
	{
		UiDocument doc;
		doc.Screen = "HUD";
		doc.Theme = "ui/default";
		doc.Design.Resolution = { 1920.0f, 1080.0f };
		doc.Design.ScaleMode = UiScaleMode::ScaleWithScreenSize;
		doc.Design.Match = 0.5f;
		doc.SafeArea.Enabled = true;
		doc.SafeArea.Top = 44.0f;
		doc.SafeArea.Bottom = 21.0f;

		UiNode root;
		root.Id = "root";
		root.Type = "Panel";
		root.Props.push_back({ "title", "@ui.hud.title" });
		root.Anchor.Min = { 0.0f, 0.0f };
		root.Anchor.Max = { 1.0f, 1.0f };
		root.Layout.Kind = UiLayoutKind::Column;
		root.Layout.Gap = 8.0f;
		root.Layout.Padding[0] = root.Layout.Padding[1] = root.Layout.Padding[2] = root.Layout.Padding[3] = 12.0f;

		UiNode hp;
		hp.Id = "hp";
		hp.Type = "ProgressBar";
		hp.Anchor.Min = { 0.0f, 0.0f };
		hp.Anchor.Max = { 1.0f, 0.0f };
		hp.Anchor.Size = { 0.0f, 28.0f };
		hp.Bind.push_back({ "Value", "player.health" });
		hp.Bind.push_back({ "Max", "player.healthMax" });
		root.Children.push_back(hp);

		UiNode close;
		close.Id = "close";
		close.Type = "Button";
		close.Props.push_back({ "label", "@ui.common.close" });
		close.On.push_back({ "Click", "ui.close" });
		root.Children.push_back(close);

		doc.Nodes.push_back(root);
		return doc;
	}

	// ---- 1. round-trip:语义等价 + 序列化逐字节确定 ----

	void TestRoundTripDeterministic()
	{
		const UiDocument original = MakeDocument();
		const std::string text = UiDocumentIO::Serialize(original);
		CHECK(!text.empty());

		UiDocument parsed;
		std::string error;
		CHECK(UiDocumentIO::Parse(text, parsed, &error));
		CHECK(error.empty());
		CHECK(UiDocumentsEquivalent(original, parsed));

		const std::string again = UiDocumentIO::Serialize(parsed);
		CHECK(text == again);

		CHECK(parsed.FindNode("hp") != nullptr);
		CHECK(parsed.FindNode("close") != nullptr);
		CHECK(parsed.FindNode("missing") == nullptr);
		CHECK(parsed.NodeCount() == 3);
	}

	// ---- 2. 版本门禁 ----

	void TestVersionGate()
	{
		UiDocument doc;
		std::string error;

		CHECK(!UiDocumentIO::Parse("Screen: HUD\nNodes: []\n", doc, &error));
		CHECK(error.find("version") != std::string::npos);

		error.clear();
		CHECK(!UiDocumentIO::Parse("FormatVersion: 2\nScreen: HUD\nNodes: []\n", doc, &error));
		CHECK(error.find("version") != std::string::npos);

		error.clear();
		CHECK(!UiDocumentIO::Parse("FormatVersion: 1\nNodes: []\n", doc, &error));
		CHECK(error.find("Screen") != std::string::npos);

		error.clear();
		CHECK(!UiDocumentIO::Parse("FormatVersion: 1\nScreen: HUD\n", doc, &error));
		CHECK(error.find("Nodes") != std::string::npos);

		error.clear();
		CHECK(!UiDocumentIO::Parse("- not a map\n", doc, &error));
	}

	// ---- 3. 未知/非法结构必须报错(不静默丢参数)----

	void TestInvalidStructuresRejected()
	{
		UiDocument doc;
		std::string error;

		// 属性必须是标量:嵌套 map 会让设计师/AI 以为改生效了 —— 必须报错。
		CHECK(!UiDocumentIO::Parse(
			"FormatVersion: 1\nScreen: X\nNodes:\n  - Type: Panel\n    Props:\n      bad: { nested: 1 }\n",
			doc, &error));
		CHECK(error.find("scalar") != std::string::npos);

		error.clear();
		CHECK(!UiDocumentIO::Parse(
			"FormatVersion: 1\nScreen: X\nNodes:\n  - Type: Panel\n    Layout:\n      Kind: Diagonal\n",
			doc, &error));
		CHECK(error.find("Kind") != std::string::npos);

		error.clear();
		CHECK(!UiDocumentIO::Parse(
			"FormatVersion: 1\nScreen: X\nNodes:\n  - Type: Panel\n    Anchor:\n      Min: [0, 0, 0]\n",
			doc, &error));

		error.clear();
		CHECK(!UiDocumentIO::Parse(
			"FormatVersion: 1\nScreen: X\nNodes:\n  - Id: Panel\n    Type: Panel\n  - Id: Panel\n    Type: Panel\n",
			doc, &error));
		CHECK(error.find("Panel") != std::string::npos);
	}

	// ---- 4. 稳定 Id 生成 ----

	void TestStableIdGeneration()
	{
		UiDocument doc;
		std::string error;
		CHECK(UiDocumentIO::Parse(
			"FormatVersion: 1\nScreen: X\nNodes:\n  - Type: Panel\n    Children:\n      - Type: Label\n",
			doc, &error));
		CHECK(error.empty());
		CHECK(doc.Nodes.size() == 1);
		CHECK(doc.Nodes[0].Id == "Panel#0");
		CHECK(doc.Nodes[0].IdWasGenerated);
		CHECK(doc.Nodes[0].Children.size() == 1);
		CHECK(doc.Nodes[0].Children[0].Id == "Panel#0.Label#0");
		CHECK(IsValidUiNodeId(doc.Nodes[0].Children[0].Id));

		CHECK(!IsValidUiNodeId(""));
		CHECK(!IsValidUiNodeId("a/b"));
		CHECK(!IsValidUiNodeId("a b"));
		CHECK(IsValidUiNodeId("hud.hp#0"));
	}

	// ---- 5. 锚点求解 ----

	void TestAnchorResolution()
	{
		const Wui::WuiRect parent { 100.0f, 50.0f, 200.0f, 400.0f };

		// 点锚定 + Pivot(0,0) = Offset 即左上角位置。
		UiAnchor fixed;
		fixed.Min = { 0.0f, 0.0f };
		fixed.Max = { 0.0f, 0.0f };
		fixed.Pivot = { 0.0f, 0.0f };
		fixed.Offset = { 8.0f, 12.0f };
		fixed.Size = { 120.0f, 24.0f };
		CHECK_RECT(ResolveAnchor(fixed, parent), 108.0f, 62.0f, 120.0f, 24.0f);

		// 点锚定 + 居中 Pivot = 以锚点为中心。
		UiAnchor centered;
		centered.Min = { 0.5f, 0.5f };
		centered.Max = { 0.5f, 0.5f };
		centered.Offset = { 0.0f, 0.0f };
		centered.Size = { 100.0f, 40.0f };
		CHECK_RECT(ResolveAnchor(centered, parent), 150.0f, 230.0f, 100.0f, 40.0f);

		// 四角锚定(右下角 + Pivot(1,1) + 16 边距)。
		UiAnchor bottomRight;
		bottomRight.Min = { 1.0f, 1.0f };
		bottomRight.Max = { 1.0f, 1.0f };
		bottomRight.Pivot = { 1.0f, 1.0f };
		bottomRight.Offset = { -16.0f, -16.0f };
		bottomRight.Size = { 100.0f, 30.0f };
		CHECK_RECT(ResolveAnchor(bottomRight, parent), 184.0f, 404.0f, 100.0f, 30.0f);

		// 拉伸:铺满 + 负 Size 内缩。
		UiAnchor stretch;
		stretch.Min = { 0.0f, 0.0f };
		stretch.Max = { 1.0f, 1.0f };
		stretch.Size = { -20.0f, -10.0f };
		CHECK_RECT(ResolveAnchor(stretch, parent), 100.0f, 50.0f, 180.0f, 390.0f);

		// 水平拉伸 + 固定高(Pivot(0,0) = 高度从锚点向下长)。
		UiAnchor topBar;
		topBar.Min = { 0.0f, 0.0f };
		topBar.Max = { 1.0f, 0.0f };
		topBar.Pivot = { 0.0f, 0.0f };
		topBar.Size = { 0.0f, 28.0f };
		CHECK_RECT(ResolveAnchor(topBar, parent), 100.0f, 50.0f, 200.0f, 28.0f);

		// 负尺寸钳到 0(不产生负宽)。
		UiAnchor degenerate;
		degenerate.Min = { 0.5f, 0.5f };
		degenerate.Max = { 0.5f, 0.5f };
		degenerate.Size = { -40.0f, -40.0f };
		CHECK_RECT(ResolveAnchor(degenerate, parent), 200.0f, 250.0f, 0.0f, 0.0f);

		// 退化父矩形(0 尺寸)+ 负增量 → 钳到零尺寸,不产生负矩形。
		CHECK_RECT(ResolveAnchor(stretch, Wui::WuiRect { 0, 0, 0, 0 }), 0.0f, 0.0f, 0.0f, 0.0f);
	}

	// ---- 6. 视口缩放策略 ----

	void TestViewportScaleModes()
	{
		UiDesign design;
		design.Resolution = { 1920.0f, 1080.0f };
		const UiSurface surface { { 1920.0f, 1080.0f }, 1.0f };
		const UiSafeArea noSafe;

		design.ScaleMode = UiScaleMode::ConstantPixelSize;
		CHECK_NEAR(ComputeUiViewport(design, noSafe, surface).Scale, 1.0f, 0.0001f);

		// 小屏:1920→960 宽,1080→540 高 → 比值都是 0.5。
		const UiSurface half { { 960.0f, 540.0f }, 1.0f };
		design.ScaleMode = UiScaleMode::ScaleWithScreenSize;
		design.Match = 0.0f;
		CHECK_NEAR(ComputeUiViewport(design, noSafe, half).Scale, 0.5f, 0.0001f);
		design.Match = 1.0f;
		CHECK_NEAR(ComputeUiViewport(design, noSafe, half).Scale, 0.5f, 0.0001f);
		design.Match = 0.5f;
		CHECK_NEAR(ComputeUiViewport(design, noSafe, half).Scale, 0.5f, 0.0001f);

		// 非等比:1280x1080 → rw = 2/3, rh = 1。Match=0 → rw,Match=1 → rh,Match=0.5 → sqrt(rw*rh)。
		const UiSurface odd { { 1280.0f, 1080.0f }, 1.0f };
		design.Match = 0.0f;
		CHECK_NEAR(ComputeUiViewport(design, noSafe, odd).Scale, 2.0f / 3.0f, 0.0005f);
		design.Match = 1.0f;
		CHECK_NEAR(ComputeUiViewport(design, noSafe, odd).Scale, 1.0f, 0.0005f);
		design.Match = 0.5f;
		CHECK_NEAR(ComputeUiViewport(design, noSafe, odd).Scale, std::sqrt(2.0f / 3.0f), 0.0005f);

		design.ScaleMode = UiScaleMode::Expand;
		CHECK_NEAR(ComputeUiViewport(design, noSafe, odd).Scale, 1.0f, 0.0005f);
		design.ScaleMode = UiScaleMode::Shrink;
		CHECK_NEAR(ComputeUiViewport(design, noSafe, odd).Scale, 2.0f / 3.0f, 0.0005f);

		// DPI 系数乘在策略缩放之上。
		const UiSurface hidpi { { 1920.0f, 1080.0f }, 2.0f };
		design.ScaleMode = UiScaleMode::ConstantPixelSize;
		CHECK_NEAR(ComputeUiViewport(design, noSafe, hidpi).Scale, 2.0f, 0.0001f);

		// 退化的物理尺寸不得产生 0/NaN 缩放。
		const UiSurface degenerate { { 0.0f, 0.0f }, 1.0f };
		const UiViewport guard = ComputeUiViewport(design, noSafe, degenerate);
		CHECK(guard.Scale > 0.0f);
		CHECK(std::isfinite(guard.Scale));
	}

	// ---- 7. 安全区 ----

	void TestSafeAreaContentRect()
	{
		UiDesign design;
		design.Resolution = { 1920.0f, 1080.0f };
		design.ScaleMode = UiScaleMode::ConstantPixelSize;
		UiSafeArea safe;
		safe.Enabled = true;
		safe.Top = 44.0f;
		safe.Bottom = 21.0f;
		safe.Left = 10.0f;
		safe.Right = 6.0f;

		const UiSurface surface { { 1920.0f, 1080.0f }, 1.0f };
		const UiViewport viewport = ComputeUiViewport(design, safe, surface);
		CHECK_NEAR(viewport.Scale, 1.0f, 0.0001f);
		CHECK_RECT(viewport.ContentRect, 10.0f, 44.0f, 1904.0f, 1015.0f);

		// 缩放 0.5 时,物理内缩换算成设计单位 = 内缩 / 缩放。
		design.ScaleMode = UiScaleMode::ScaleWithScreenSize;
		design.Match = 0.5f;
		const UiSurface half { { 960.0f, 540.0f }, 1.0f };
		const UiViewport scaled = ComputeUiViewport(design, safe, half);
		CHECK_NEAR(scaled.Scale, 0.5f, 0.0001f);
		CHECK_RECT(scaled.ContentRect, 20.0f, 88.0f, 1920.0f - 20.0f - 12.0f, 1080.0f - 88.0f - 42.0f);

		// 关闭安全区 → 内容矩形 = 整块设计矩形。
		safe.Enabled = false;
		const UiViewport plain = ComputeUiViewport(design, safe, half);
		CHECK_RECT(plain.ContentRect, 0.0f, 0.0f, 1920.0f, 1080.0f);

		// 内缩大于屏幕 → 钳到 0,不产生负矩形。
		UiSafeArea huge;
		huge.Enabled = true;
		huge.Top = huge.Bottom = huge.Left = huge.Right = 5000.0f;
		const UiViewport clamped = ComputeUiViewport(design, huge, half);
		CHECK(clamped.ContentRect.W >= 0.0f);
		CHECK(clamped.ContentRect.H >= 0.0f);
	}

	// ---- 8. 命中:设计 ↔ 物理 反算恒等 ----

	void TestHitTestRoundTrip()
	{
		UiDesign design;
		design.Resolution = { 1920.0f, 1080.0f };
		design.ScaleMode = UiScaleMode::ScaleWithScreenSize;
		design.Match = 0.0f;
		const UiSurface surface { { 1280.0f, 720.0f }, 1.0f };
		const UiViewport viewport = ComputeUiViewport(design, UiSafeArea {}, surface);

		const Wui::WuiRect rect { 100.0f, 100.0f, 200.0f, 50.0f };
		const glm::vec2 centerDesign { 200.0f, 125.0f };
		const glm::vec2 centerPhysical = viewport.DesignToPhysical(centerDesign);
		CHECK(HitTestDesign(rect, viewport, centerPhysical));
		CHECK_NEAR(viewport.PhysicalToDesign(centerPhysical).x, centerDesign.x, 0.001f);

		const glm::vec2 outsidePhysical = viewport.DesignToPhysical(glm::vec2 { 400.0f, 125.0f });
		CHECK(!HitTestDesign(rect, viewport, outsidePhysical));
	}

	// ---- 9. 容器布局 ----

	void TestLayoutContainers()
	{
		UiDocument doc;
		std::string error;
		CHECK(UiDocumentIO::Parse(R"YAML(FormatVersion: 1
Screen: Layout
Design: { Resolution: [400, 300] }
Nodes:
  - Id: root
    Type: Panel
    Anchor: { Min: [0, 0], Max: [1, 1], Size: [0, 0] }
    Layout: { Kind: Column, Gap: 10, Padding: [10, 10, 10, 10] }
    Children:
      - Id: a
        Type: Panel
      - Id: b
        Type: Panel
  - Id: overlayRoot
    Type: Overlay
    Anchor: { Min: [0, 0], Max: [1, 1], Size: [0, 0] }
    Layout: { Kind: Overlay }
    Children:
      - Id: toast
        Type: Label
)YAML", doc, &error));
		CHECK(error.empty());

		UiScreen screen;
		CHECK(screen.Build(doc, &error));
		CHECK(error.empty());
		CHECK(screen.Count() == 5);
		CHECK(screen.Find("root") != nullptr);
		CHECK(screen.Find("a") != nullptr);
		CHECK(screen.Find("root")->Children.size() == 2);

		UiDesign design;
		design.Resolution = { 400.0f, 300.0f };
		design.ScaleMode = UiScaleMode::ConstantPixelSize;
		const UiViewport viewport = ComputeUiViewport(design, UiSafeArea {}, UiSurface { { 400.0f, 300.0f }, 1.0f });
		CHECK(screen.Layout(viewport));

		CHECK_RECT(screen.RectOf("root"), 0.0f, 0.0f, 400.0f, 300.0f);

		// Column:内缩 10,总高 280,两项 + 10 间距 → 每项 135。
		CHECK_RECT(screen.RectOf("a"), 10.0f, 10.0f, 380.0f, 135.0f);
		CHECK_RECT(screen.RectOf("b"), 10.0f, 155.0f, 380.0f, 135.0f);

		// Overlay:子节点铺满父矩形。
		CHECK_RECT(screen.RectOf("toast"), 0.0f, 0.0f, 400.0f, 300.0f);

		// 命中:分层后最后画的最上层赢。
		const UiNodeInstance* hit = screen.HitTest(glm::vec2 { 200.0f, 200.0f });
		CHECK(hit != nullptr);
		CHECK(hit->Id == "toast");
	}

	// ---- 10. 夹具文本解析 + 尺寸设计分辨率 ----

	void TestParseSampleDocument()
	{
		UiDocument doc;
		std::string error;
		CHECK(UiDocumentIO::Parse(kSampleDocument, doc, &error));
		CHECK(error.empty());
		CHECK(doc.Screen == "HUD");
		CHECK(doc.Theme == "ui/default");
		CHECK(doc.SafeArea.Enabled);
		CHECK_NEAR(doc.SafeArea.Top, 44.0f, 0.001f);
		CHECK(doc.Parameters.size() == 1);
		CHECK(doc.Nodes.size() == 1);
		CHECK(doc.Nodes[0].Layout.Kind == UiLayoutKind::Column);
		CHECK(doc.Nodes[0].Children.size() == 2);
		CHECK(doc.Nodes[0].Children[0].Bind.size() == 2);
		CHECK(doc.Nodes[0].Children[1].On.size() == 1);
		CHECK(doc.Nodes[0].Children[1].On[0].Command == "ui.close");

		UiScreen screen;
		CHECK(screen.Build(doc, &error));
		CHECK(screen.Count() == 3);

		UiDesign design = doc.Design;
		design.ScaleMode = UiScaleMode::ConstantPixelSize;
		const UiViewport viewport = ComputeUiViewport(design, doc.SafeArea, UiSurface { { 1920.0f, 1080.0f }, 1.0f });
		CHECK(screen.Layout(viewport));
		// 根铺满内容矩形(已扣安全区)。
		CHECK_RECT(screen.RectOf("root"), 0.0f, 44.0f, 1920.0f, 1080.0f - 44.0f - 21.0f);
	}

	// ---- 11. 文件读写 ----

	void TestFileSaveLoad()
	{
		std::error_code ec;
		const uint64_t unique = static_cast<uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
		const std::filesystem::path dir = std::filesystem::temp_directory_path(ec) /
			("we-ui-" + std::to_string(unique));
		std::filesystem::create_directories(dir, ec);
		const std::filesystem::path file = dir / "HUD.wui";

		const UiDocument doc = MakeDocument();
		std::string error;
		CHECK(UiDocumentIO::SaveFile(file, doc, &error));
		CHECK(error.empty());
		CHECK(std::filesystem::exists(file));

		UiDocument loaded;
		CHECK(UiDocumentIO::LoadFile(file, loaded, &error));
		CHECK(error.empty());
		CHECK(UiDocumentsEquivalent(doc, loaded));

		// 再次保存 → 逐字节稳定(覆盖已有文件也要走原子路径)。
		CHECK(UiDocumentIO::SaveFile(file, loaded, &error));
		UiDocument reloaded;
		CHECK(UiDocumentIO::LoadFile(file, reloaded, &error));
		CHECK(UiDocumentIO::Serialize(loaded) == UiDocumentIO::Serialize(reloaded));

		CHECK(!UiDocumentIO::LoadFile(dir / "missing.wui", loaded, &error));
		CHECK(!error.empty());

		std::filesystem::remove_all(dir, ec);
	}
}

int main()
{
	try
	{
		TestRoundTripDeterministic();
		TestVersionGate();
		TestInvalidStructuresRejected();
		TestStableIdGeneration();
		TestAnchorResolution();
		TestViewportScaleModes();
		TestSafeAreaContentRect();
		TestHitTestRoundTrip();
		TestLayoutContainers();
		TestParseSampleDocument();
		TestFileSaveLoad();
	}
	catch (const std::exception& e)
	{
		std::fprintf(stderr, "World.UiDocument FAILED: %s\n", e.what());
		return 1;
	}
	std::printf("World.UiDocument OK\n");
	return 0;
}
