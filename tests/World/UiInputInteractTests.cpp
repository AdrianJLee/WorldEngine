// World.UiInputInteract — GameUI(M13)交互输入状态机,headless。
//
// 覆盖(派工单验收):
//   A. Slider 拖拽:按下 → 拖到中点(值 ≈ (min+max)/2)→ 抬起;拖拽期间指针消费;
//      跨帧(第三帧仍按住)不丢状态;产生 Change/Commit 命令(Command 从 `On.*` 取)。
//   B. Slider step 量化:有 `step` 时按 step 取整并钳到 [min,max]。
//   C. TextField 编辑:点聚焦 → 进入编辑态;`TextInput` 逐字符追加;Backspace 退格;
//      Enter 提交(值 = 编辑文本);Escape 放弃(回到原值,无 Commit);
//      编辑态下方向键不导航。
//   D. 滚轮:命中滚动容器 ⇒ 消费 + `UiScreen::ScrollOffset` 真的变了(步长 `scrollStep`,默认 40);
//      空白 ⇒ 不消费且偏移不变。
//   E. 宿主预置初值 `UiInputRouter::SetEditingText` 优先于节点属性。
//
// 说明:本文件不构建、不渲染;设计 = 物理 = 1920x1080(ConstantPixelSize)⇒ 命中坐标 = 设计坐标。

#include "World/Core/KeyCodes.h"
#include "World/UI/UiInputRouter.h"
#include "World/UI/UiNodeRegistry.h"
#include "World/UI/UiScreen.h"
#include "World/UI/UiTypes.h"
#include "World/WUI/WuiContext.h"

#include <cmath>
#include <cstdint>
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

	void CheckStr(const std::string& actual, const std::string& expected, const char* expression, int line)
	{
		if (actual != expected)
		{
			throw std::runtime_error(std::string("line ") + std::to_string(line) + ": " + expression +
				" (actual '" + actual + "', expected '" + expected + "')");
		}
	}
#define CHECK_STR(actual, expected) CheckStr((actual), (expected), #actual " == " #expected, __LINE__)

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

	void SetOn(UiNode& node, std::string event, std::string command)
	{
		node.On.push_back(UiCommandDecl { std::move(event), std::move(command) });
	}

	UiDocument MakeDoc(std::vector<UiNode> roots)
	{
		UiDocument doc;
		doc.FormatVersion = kUiDocumentFormatVersion;
		doc.Screen = "Interact";
		doc.Design.Resolution = glm::vec2 { kW, kH };
		doc.Design.ScaleMode = UiScaleMode::ConstantPixelSize;
		doc.Nodes = std::move(roots);
		return doc;
	}

	UiScreen BuildAndLayout(const UiDocument& doc)
	{
		UiScreen screen;
		std::string error;
		Check(screen.Build(doc, &error), ("screen build: " + error).c_str(), __LINE__);
		UiDesign design;
		design.Resolution = { kW, kH };
		design.ScaleMode = UiScaleMode::ConstantPixelSize;
		Check(screen.Layout(ComputeUiViewport(design, UiSafeArea {}, UiSurface { { kW, kH }, 1.0f })),
			"screen layout", __LINE__);
		return screen;
	}

	Wui::WuiInputState MouseInput(glm::vec2 point)
	{
		Wui::WuiInputState input;
		input.MousePos = point;
		input.ViewportSize = { kW, kH };
		return input;
	}

	void Press(Wui::WuiInputState& input)
	{
		input.MouseDown[0] = true;
		input.MouseClicked[0] = true;
	}

	Wui::WuiInputState KeyInput(uint32_t key)
	{
		Wui::WuiInputState input;
		input.KeyPressed.push_back(key);
		return input;
	}

	// ---- A. Slider 拖拽(跨帧)----

	void TestSliderDragCrossFrame()
	{
		UiNode root = MakeNode("root", "Panel");
		root.Anchor = FullStretch();
		UiNode slider = MakeNode("volume", "Slider");
		slider.Anchor = PointAnchor(100.0f, 100.0f, 200.0f, 40.0f);   // x: 100..300
		SetProp(slider, "min", "0");
		SetProp(slider, "max", "1");
		SetOn(slider, "Change", "ui.volume.change");
		SetOn(slider, "Commit", "ui.volume.commit");
		root.Children.push_back(std::move(slider));

		UiScreen screen = BuildAndLayout(MakeDoc({ std::move(root) }));
		UiCommandQueue queue;
		UiInputRouter router;

		// 帧 1:按下在滑条中点(x=200 ⇒ 0.5)。
		Wui::WuiInputState press = MouseInput(glm::vec2 { 200.0f, 120.0f });
		Press(press);
		CHECK(router.Update(screen, press, queue));
		CHECK(router.LastFrame().PointerConsumed);
		CHECK(router.IsDragging());
		CHECK_STR(router.DraggingId(), "volume");
		CHECK(queue.Size() == 1);
		CHECK_STR(queue.Commands()[0].Event, "Change");
		CHECK_STR(queue.Commands()[0].Command, "ui.volume.change");
		CHECK_NEAR(std::stof(queue.Commands()[0].Value), 0.5f, 0.001f);

		// 帧 2:仍按住,拖到 x=250 ⇒ 0.75(跨帧状态未丢)。
		queue.Clear();
		Wui::WuiInputState held1 = MouseInput(glm::vec2 { 250.0f, 120.0f });
		held1.MouseDown[0] = true;
		CHECK(router.Update(screen, held1, queue));
		CHECK(router.IsDragging());
		CHECK_NEAR(std::stof(queue.Commands().back().Value), 0.75f, 0.001f);

		// 帧 3:第三帧仍按住(x=260 ⇒ 0.8)。
		queue.Clear();
		Wui::WuiInputState held2 = MouseInput(glm::vec2 { 260.0f, 120.0f });
		held2.MouseDown[0] = true;
		CHECK(router.Update(screen, held2, queue));
		CHECK(router.IsDragging());
		CHECK(queue.Size() == 1);
		CHECK_STR(queue.Commands()[0].Event, "Change");
		CHECK_NEAR(std::stof(queue.Commands()[0].Value), 0.8f, 0.001f);

		// 帧 4:抬起(x=200 ⇒ Commit 0.5)。
		queue.Clear();
		Wui::WuiInputState release = MouseInput(glm::vec2 { 200.0f, 120.0f });
		release.MouseReleased[0] = true;
		CHECK(router.Update(screen, release, queue));
		CHECK(router.LastFrame().PointerConsumed);
		CHECK(!router.IsDragging());
		CHECK(queue.Size() == 1);
		CHECK_STR(queue.Commands()[0].Event, "Commit");
		CHECK_STR(queue.Commands()[0].Command, "ui.volume.commit");
		CHECK_NEAR(std::stof(queue.Commands()[0].Value), 0.5f, 0.001f);
	}

	// ---- B. Slider step 量化 ----

	void TestSliderStepQuantization()
	{
		UiNode root = MakeNode("root", "Panel");
		root.Anchor = FullStretch();
		UiNode slider = MakeNode("s", "Slider");
		slider.Anchor = PointAnchor(100.0f, 100.0f, 200.0f, 40.0f);
		SetProp(slider, "min", "0");
		SetProp(slider, "max", "10");
		SetProp(slider, "step", "1");
		root.Children.push_back(std::move(slider));

		UiScreen screen = BuildAndLayout(MakeDoc({ std::move(root) }));
		UiCommandQueue queue;
		UiInputRouter router;

		// x=175 ⇒ fraction 0.375 ⇒ 3.75 ⇒ 量化到 4。
		Wui::WuiInputState press = MouseInput(glm::vec2 { 175.0f, 120.0f });
		Press(press);
		CHECK(router.Update(screen, press, queue));
		CHECK_NEAR(std::stof(queue.Commands().back().Value), 4.0f, 0.001f);

		// x=150 ⇒ 2.5 ⇒ round-half-away ⇒ 3。
		queue.Clear();
		Wui::WuiInputState held = MouseInput(glm::vec2 { 150.0f, 120.0f });
		held.MouseDown[0] = true;
		CHECK(router.Update(screen, held, queue));
		CHECK_NEAR(std::stof(queue.Commands().back().Value), 3.0f, 0.001f);

		// 抬起:同样量化。
		queue.Clear();
		Wui::WuiInputState release = MouseInput(glm::vec2 { 175.0f, 120.0f });
		release.MouseReleased[0] = true;
		CHECK(router.Update(screen, release, queue));
		CHECK_STR(queue.Commands().back().Event, "Commit");
		CHECK_NEAR(std::stof(queue.Commands().back().Value), 4.0f, 0.001f);
	}

	// ---- C. TextField 键入 / 退格 / Enter 提交 ----

	void TestTextFieldTypingCommit()
	{
		UiNode root = MakeNode("root", "Panel");
		root.Anchor = FullStretch();
		UiNode field = MakeNode("input", "TextField");
		field.Anchor = PointAnchor(100.0f, 100.0f, 200.0f, 40.0f);
		SetOn(field, "Commit", "ui.input.commit");
		root.Children.push_back(std::move(field));

		UiScreen screen = BuildAndLayout(MakeDoc({ std::move(root) }));
		UiCommandQueue queue;
		UiInputRouter router;

		// 帧 1:点击聚焦 ⇒ 编辑态(不推 Click)。
		Wui::WuiInputState click = MouseInput(glm::vec2 { 150.0f, 120.0f });
		Press(click);
		CHECK(router.Update(screen, click, queue));
		CHECK(queue.Empty());
		CHECK(router.IsEditing());
		CHECK_STR(router.EditingNodeId(), "input");
		CHECK_STR(router.FocusedId(), "input");
		CHECK(router.FocusDomain() == UiFocusDomain::Text);

		// 帧 2:键入 "abc" ⇒ 逐字符追加。
		Wui::WuiInputState typing = MouseInput(glm::vec2 { 150.0f, 120.0f });
		typing.TextInput = { 'a', 'b', 'c' };
		CHECK(!router.Update(screen, typing, queue));
		CHECK(router.LastFrame().KeyboardConsumed);
		CHECK_STR(router.EditingText(), "abc");
		CHECK(queue.Empty());

		// 帧 3:退格 ⇒ "ab"。
		const Wui::WuiInputState backspace = KeyInput(World::Backspace);
		router.Update(screen, backspace, queue);
		CHECK_STR(router.EditingText(), "ab");

		// 帧 4:Enter ⇒ Commit,值 = 编辑文本;退出编辑态。
		const Wui::WuiInputState enter = KeyInput(World::Enter);
		router.Update(screen, enter, queue);
		CHECK(router.LastFrame().KeyboardConsumed);
		CHECK(!router.IsEditing());
		CHECK(router.FocusDomain() == UiFocusDomain::Navigation);
		CHECK(queue.Size() == 1);
		CHECK_STR(queue.Commands()[0].Event, "Commit");
		CHECK_STR(queue.Commands()[0].NodeId, "input");
		CHECK_STR(queue.Commands()[0].Command, "ui.input.commit");
		CHECK_STR(queue.Commands()[0].Value, "ab");
	}

	// ---- C2. Escape 放弃(回到原值,无 Commit)----

	void TestTextFieldEscapeRevert()
	{
		UiNode root = MakeNode("root", "Panel");
		root.Anchor = FullStretch();
		UiNode field = MakeNode("input", "TextField");
		field.Anchor = PointAnchor(100.0f, 100.0f, 200.0f, 40.0f);
		SetProp(field, "value", "orig");
		SetOn(field, "Commit", "ui.input.commit");
		root.Children.push_back(std::move(field));

		UiScreen screen = BuildAndLayout(MakeDoc({ std::move(root) }));
		UiCommandQueue queue;
		UiInputRouter router;

		Wui::WuiInputState click = MouseInput(glm::vec2 { 150.0f, 120.0f });
		Press(click);
		router.Update(screen, click, queue);
		CHECK_STR(router.EditingText(), "orig");   // 初值来自节点属性 `value`

		Wui::WuiInputState typing = MouseInput(glm::vec2 { 150.0f, 120.0f });
		typing.TextInput = { 'z' };
		router.Update(screen, typing, queue);
		CHECK_STR(router.EditingText(), "origz");

		const Wui::WuiInputState escape = KeyInput(World::Escape);
		router.Update(screen, escape, queue);
		CHECK(router.LastFrame().KeyboardConsumed);
		CHECK(!router.IsEditing());
		CHECK_STR(router.EditingText(), "orig");   // 回到原值
		CHECK(queue.Empty());                       // 放弃 = 无 Commit
	}

	// ---- C3. 编辑态下方向键不导航 ----

	void TestEditingArrowDoesNotNavigate()
	{
		UiNode root = MakeNode("root", "Panel");
		root.Anchor = FullStretch();
		UiNode first = MakeNode("tf0", "TextField");
		first.Anchor = PointAnchor(100.0f, 100.0f, 200.0f, 40.0f);
		UiNode second = MakeNode("tf1", "TextField");
		second.Anchor = PointAnchor(500.0f, 100.0f, 200.0f, 40.0f);
		root.Children.push_back(std::move(first));
		root.Children.push_back(std::move(second));

		UiScreen screen = BuildAndLayout(MakeDoc({ std::move(root) }));
		UiCommandQueue queue;
		UiInputRouter router;

		Wui::WuiInputState click = MouseInput(glm::vec2 { 150.0f, 120.0f });
		Press(click);
		router.Update(screen, click, queue);
		CHECK(router.IsEditing());
		CHECK_STR(router.FocusedId(), "tf0");

		const Wui::WuiInputState right = KeyInput(World::Right);
		router.Update(screen, right, queue);
		CHECK_STR(router.FocusedId(), "tf0");       // 导航焦点未移动
		CHECK(router.IsEditing());                  // 编辑态保持
		CHECK(!router.LastFrame().KeyboardConsumed); // 文本域语义:方向键不归 UI
	}

	// ---- D. 滚轮命中滚动容器 ⇒ 消费 + 偏移落地 ----

	void TestWheelScrollsContainer()
	{
		UiNode root = MakeNode("root", "Panel");
		root.Anchor = FullStretch();
		UiNode scroller = MakeNode("scroller", "Panel");
		scroller.Anchor = PointAnchor(0.0f, 0.0f, 200.0f, 200.0f);
		SetProp(scroller, "scrollable", "true");
		UiNode tall = MakeNode("tall", "Panel");
		tall.Anchor = PointAnchor(0.0f, 300.0f, 50.0f, 50.0f);   // 内容高 350 > 容器 200
		scroller.Children.push_back(std::move(tall));
		root.Children.push_back(std::move(scroller));

		UiScreen screen = BuildAndLayout(MakeDoc({ std::move(root) }));
		UiInputRouter router;

		CHECK_NEAR(screen.ScrollOffset("scroller").y, 0.0f, 0.001f);

		Wui::WuiInputState wheel = MouseInput(glm::vec2 { 100.0f, 100.0f });
		wheel.Wheel = -1.0f;   // 向下滚 ⇒ 偏移增(默认步长 40)
		CHECK(!router.Update(screen, wheel));         // 滚轮不是"指针按下"
		CHECK(router.LastFrame().WheelConsumed);
		CHECK_STR(router.LastFrame().WheelTargetId, "scroller");
		CHECK_NEAR(screen.ScrollOffset("scroller").y, 40.0f, 0.001f);   // 偏移真的落地

		// 空白位置:不消费,偏移不变。
		Wui::WuiInputState blank = MouseInput(glm::vec2 { 600.0f, 600.0f });
		blank.Wheel = -1.0f;
		CHECK(!router.Update(screen, blank));
		CHECK(!router.LastFrame().WheelConsumed);
		CHECK_NEAR(screen.ScrollOffset("scroller").y, 40.0f, 0.001f);
	}

	// ---- D2. 滚轮步长从 `scrollStep` 读 ----

	void TestWheelScrollStep()
	{
		UiNode root = MakeNode("root", "Panel");
		root.Anchor = FullStretch();
		UiNode scroller = MakeNode("scroller", "Panel");
		scroller.Anchor = PointAnchor(0.0f, 0.0f, 200.0f, 200.0f);
		SetProp(scroller, "scrollable", "true");
		SetProp(scroller, "scrollStep", "10");
		UiNode tall = MakeNode("tall", "Panel");
		tall.Anchor = PointAnchor(0.0f, 300.0f, 50.0f, 50.0f);
		scroller.Children.push_back(std::move(tall));
		root.Children.push_back(std::move(scroller));

		UiScreen screen = BuildAndLayout(MakeDoc({ std::move(root) }));
		UiInputRouter router;

		Wui::WuiInputState down1 = MouseInput(glm::vec2 { 100.0f, 100.0f });
		down1.Wheel = -1.0f;
		router.Update(screen, down1);
		CHECK_NEAR(screen.ScrollOffset("scroller").y, 10.0f, 0.001f);

		Wui::WuiInputState down2 = MouseInput(glm::vec2 { 100.0f, 100.0f });
		down2.Wheel = -1.0f;
		router.Update(screen, down2);
		CHECK_NEAR(screen.ScrollOffset("scroller").y, 20.0f, 0.001f);

		// 向上滚过头:由 `SetScrollOffset` 钳到 0(router 不自算钳位)。
		Wui::WuiInputState up = MouseInput(glm::vec2 { 100.0f, 100.0f });
		up.Wheel = 5.0f;
		router.Update(screen, up);
		CHECK_NEAR(screen.ScrollOffset("scroller").y, 0.0f, 0.001f);
	}

	// ---- C4. 退格按 UTF-8 码点删(多字节字符)----

	void TestTextFieldUnicodeBackspace()
	{
		UiNode root = MakeNode("root", "Panel");
		root.Anchor = FullStretch();
		UiNode field = MakeNode("input", "TextField");
		field.Anchor = PointAnchor(100.0f, 100.0f, 200.0f, 40.0f);
		root.Children.push_back(std::move(field));

		UiScreen screen = BuildAndLayout(MakeDoc({ std::move(root) }));
		UiCommandQueue queue;
		UiInputRouter router;

		Wui::WuiInputState click = MouseInput(glm::vec2 { 150.0f, 120.0f });
		Press(click);
		router.Update(screen, click, queue);

		// 'a' + U+00E9('é'):1 + 2 字节。
		Wui::WuiInputState typing = MouseInput(glm::vec2 { 150.0f, 120.0f });
		typing.TextInput = { 'a', 0x00E9u };
		router.Update(screen, typing, queue);
		CHECK(router.EditingText().size() == 3);

		const Wui::WuiInputState backspace = KeyInput(World::Backspace);
		router.Update(screen, backspace, queue);
		CHECK_STR(router.EditingText(), "a");   // 整个 'é' 被删,不留半个 UTF-8 序列
	}

	// ---- E. 宿主预置初值优先于节点属性 ----

	void TestSetEditingTextPreset()
	{
		UiNode root = MakeNode("root", "Panel");
		root.Anchor = FullStretch();
		UiNode field = MakeNode("input", "TextField");
		field.Anchor = PointAnchor(100.0f, 100.0f, 200.0f, 40.0f);
		SetProp(field, "value", "propval");
		root.Children.push_back(std::move(field));

		UiScreen screen = BuildAndLayout(MakeDoc({ std::move(root) }));

		{
			UiCommandQueue queue;
			UiInputRouter router;
			router.SetEditingText("input", "preset");   // 进入编辑前预置
			Wui::WuiInputState click = MouseInput(glm::vec2 { 150.0f, 120.0f });
			Press(click);
			router.Update(screen, click, queue);
			CHECK_STR(router.EditingText(), "preset");  // 预置优先于属性
		}
		{
			UiCommandQueue queue;
			UiInputRouter router;
			Wui::WuiInputState click = MouseInput(glm::vec2 { 150.0f, 120.0f });
			Press(click);
			router.Update(screen, click, queue);
			CHECK_STR(router.EditingText(), "propval"); // 无预置 ⇒ 读属性
			router.SetEditingText("input", "live");     // 编辑中调用立即生效
			CHECK_STR(router.EditingText(), "live");
		}
	}
}

int main()
{
	try
	{
		TestSliderDragCrossFrame();
		TestSliderStepQuantization();
		TestTextFieldTypingCommit();
		TestTextFieldEscapeRevert();
		TestEditingArrowDoesNotNavigate();
		TestWheelScrollsContainer();
		TestWheelScrollStep();
		TestTextFieldUnicodeBackspace();
		TestSetEditingTextPreset();
	}
	catch (const std::exception& error)
	{
		std::fprintf(stderr, "World.UiInputInteract FAILED: %s\n", error.what());
		return 1;
	}
	std::printf("World.UiInputInteract OK\n");
	return 0;
}