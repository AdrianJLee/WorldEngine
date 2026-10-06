#include "World/UI/UiDocument.h"
#include "World/UI/UiScreen.h"
#include "World/UI/UiTypes.h"
#include "World/UI/UiWorldProjector.h"

#include <cmath>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <vector>

#include <glm/gtc/matrix_transform.hpp>

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
#define CHECK_NEAR(actual, expected, tolerance) CheckNear(static_cast<float>(actual), static_cast<float>(expected), static_cast<float>(tolerance), #actual " ~= " #expected, __LINE__)

	constexpr float kW = 1920.0f;
	constexpr float kH = 1080.0f;

	UiWorldCamera MakeCamera()
	{
		UiWorldCamera camera;
		camera.ViewProjection = glm::ortho(-1.0f, 1.0f, -1.0f, 1.0f, -1.0f, 1.0f);
		camera.ScreenSize = { kW, kH };
		return camera;
	}

	UiViewport MakeViewport()
	{
		UiDesign design;
		design.Resolution = { kW, kH };
		design.ScaleMode = UiScaleMode::ConstantPixelSize;
		return ComputeUiViewport(design, UiSafeArea {}, UiSurface { { kW, kH }, 1.0f });
	}

	const char* kWorldDocument = R"YAML(FormatVersion: 1
Screen: WorldProbe
Design: { Resolution: [1920, 1080], ScaleMode: ConstantPixelSize }
Nodes:
  - Id: root
    Type: Panel
    Anchor: { Min: [0, 0], Max: [1, 1], Offset: [0, 0], Size: [0, 0] }
    Children:
      - Id: nameplate
        Type: Label
        Props:
          text: "Boss"
        World:
          Target: boss
          Offset: [0, 0.5, 0]
        Anchor: { Min: [0.5, 0.5], Max: [0.5, 0.5], Pivot: [0.5, 0.5], Size: [200, 40] }
      - Id: fixed
        Type: Label
        Props:
          text: "Fixed"
        Anchor: { Min: [0, 0], Max: [0, 0], Pivot: [0, 0], Offset: [10, 10], Size: [100, 20] }
)YAML";

	// ---- 1. 投影数学 ----

	void TestProjection()
	{
		const UiWorldCamera camera = MakeCamera();

		glm::vec2 screen(0.0f);
		CHECK(ProjectWorldToScreen(camera, glm::vec3 { 0.0f, 0.0f, 0.0f }, screen));
		CHECK_NEAR(screen.x, kW * 0.5f, 0.01f);
		CHECK_NEAR(screen.y, kH * 0.5f, 0.01f);

		// 正交 [-1,1] 的右边缘 → 屏幕最右;NDC +Y 向上 ⇒ 屏幕 y 翻转。
		CHECK(ProjectWorldToScreen(camera, glm::vec3 { 1.0f, 0.0f, 0.0f }, screen));
		CHECK_NEAR(screen.x, kW, 0.01f);
		CHECK_NEAR(screen.y, kH * 0.5f, 0.01f);

		CHECK(ProjectWorldToScreen(camera, glm::vec3 { 0.0f, 1.0f, 0.0f }, screen));
		CHECK_NEAR(screen.x, kW * 0.5f, 0.01f);
		CHECK_NEAR(screen.y, 0.0f, 0.01f);

		// 相机背后(w <= 0)必须拒绝,而不是画在 (0,0)。
		UiWorldCamera behind;
		behind.ViewProjection = glm::mat4(1.0f);
		behind.ViewProjection[3][3] = 0.0f;   // clip.w = 0
		behind.ScreenSize = { kW, kH };
		CHECK(!ProjectWorldToScreen(behind, glm::vec3 { 0.0f, 0.0f, 0.0f }, screen));

		glm::mat4 flipped(1.0f);
		flipped[3][3] = -1.0f;               // clip.w = -1
		behind.ViewProjection = flipped;
		CHECK(!ProjectWorldToScreen(behind, glm::vec3 { 1.0f, 2.0f, 3.0f }, screen));
	}

	// ---- 2. 锚定:命中同一映射 ----

	void TestAnchorPlacement()
	{
		UiDocument doc;
		std::string error;
		CHECK(UiDocumentIO::Parse(kWorldDocument, doc, &error));
		CHECK(error.empty());
		CHECK(doc.Nodes[0].Children[0].World.Enabled);
		CHECK(doc.Nodes[0].Children[0].World.Target == "boss");
		CHECK(!doc.Nodes[0].Children[1].World.Enabled);

		UiScreen screen;
		CHECK(screen.Build(doc, &error));
		const UiViewport viewport = MakeViewport();
		CHECK(screen.Layout(viewport));

		const UiWorldCamera camera = MakeCamera();
		const UiWorldResult result = ApplyWorldAnchors(screen, viewport, camera,
			[](std::string_view target, glm::vec3& out)
			{
				if (target != "boss")
					return false;
				out = glm::vec3 { 0.0f, 0.0f, 0.0f };
				return true;
			});

		CHECK(result.Anchored == 1);
		CHECK(result.Hidden == 0);

		// 世界 (0,0,0) + Offset (0,0.5,0) → NDC y = 0.5 → 屏幕 y = 0.25H;x = 0.5W。
		// Pivot 居中 ⇒ 节点中心落在该点。
		const Wui::WuiRect plate = screen.RectOf("nameplate");
		CHECK_NEAR(plate.W, 200.0f, 0.01f);
		CHECK_NEAR(plate.H, 40.0f, 0.01f);
		CHECK_NEAR(plate.X + plate.W * 0.5f, kW * 0.5f, 0.01f);
		CHECK_NEAR(plate.Y + plate.H * 0.5f, kH * 0.25f, 0.01f);

		// 纯屏幕空间的兄弟节点不受影响。
		const Wui::WuiRect fixed = screen.RectOf("fixed");
		CHECK_NEAR(fixed.X, 10.0f, 0.01f);
		CHECK_NEAR(fixed.Y, 10.0f, 0.01f);

		// 命中:锚定后的矩形参与命中,且用的是同一映射(物理坐标 → 设计)。
		const glm::vec2 centerPhysical = viewport.DesignToPhysical(
			glm::vec2 { plate.X + plate.W * 0.5f, plate.Y + plate.H * 0.5f });
		const UiNodeInstance* hit = screen.HitTest(centerPhysical);
		CHECK(hit != nullptr);
		CHECK(hit->Id == "nameplate");
	}

	// ---- 3. 目标缺失 / 相机背后 ⇒ 隐藏(不可见也不可点) ----

	void TestHiddenWhenUnresolvable()
	{
		UiDocument doc;
		std::string error;
		CHECK(UiDocumentIO::Parse(kWorldDocument, doc, &error));

		UiScreen screen;
		CHECK(screen.Build(doc, &error));
		const UiViewport viewport = MakeViewport();
		CHECK(screen.Layout(viewport));
		const UiWorldCamera camera = MakeCamera();

		// 解析不到目标。
		const UiWorldResult missing = ApplyWorldAnchors(screen, viewport, camera,
			[](std::string_view, glm::vec3&) { return false; });
		CHECK(missing.Anchored == 0);
		CHECK(missing.Hidden == 1);
		CHECK(!missing.Warnings.empty());
		const Wui::WuiRect hidden = screen.RectOf("nameplate");
		CHECK(!viewport.ContentRect.Contains(glm::vec2 { hidden.X + hidden.W * 0.5f, hidden.Y + hidden.H * 0.5f }));
		// 隐藏的节点不能被点到。
		CHECK(screen.HitTest(glm::vec2 { kW * 0.5f, kH * 0.25f }) == nullptr ||
			screen.HitTest(glm::vec2 { kW * 0.5f, kH * 0.25f })->Id != "nameplate");

		// 没有解析器。
		const UiWorldResult noResolver = ApplyWorldAnchors(screen, viewport, camera, nullptr);
		CHECK(noResolver.Hidden == 1);
	}

	// ---- 4. round-trip:World 块逐字节稳定 ----

	void TestRoundTrip()
	{
		UiDocument parsed;
		std::string error;
		CHECK(UiDocumentIO::Parse(kWorldDocument, parsed, &error));
		const std::string text = UiDocumentIO::Serialize(parsed);
		UiDocument again;
		CHECK(UiDocumentIO::Parse(text, again, &error));
		CHECK(UiDocumentsEquivalent(parsed, again));
		CHECK(UiDocumentIO::Serialize(again) == text);

		// 缺 Target = 可读报错(不能静默当成没有世界锚点)。
		UiDocument bad;
		error.clear();
		CHECK(!UiDocumentIO::Parse(
			"FormatVersion: 1\nScreen: X\nNodes:\n  - Type: Label\n    World:\n      Offset: [0, 0, 0]\n",
			bad, &error));
		CHECK(error.find("Target") != std::string::npos);
	}
}

int main()
{
	try
	{
		TestProjection();
		TestAnchorPlacement();
		TestHiddenWhenUnresolvable();
		TestRoundTrip();
	}
	catch (const std::exception& e)
	{
		std::fprintf(stderr, "World.UiWorld FAILED: %s\n", e.what());
		return 1;
	}
	std::printf("World.UiWorld OK\n");
	return 0;
}