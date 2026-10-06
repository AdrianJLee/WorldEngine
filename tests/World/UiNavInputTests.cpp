// World.UiNavInput — GameUI(M3)页导航 + 输入消费/焦点导航,headless。
//
// 覆盖(派工单验收):
//   ① 页面栈 Push/Pop/Replace/Reset 后层序与当前页正确;Back() 优先关模态;
//   ② 模态打开时命中只落在模态层(下层页面不可点);
//   ③ 输入消费**双向**:命中可交互节点 = 消费 + 命令进队列;空白 = 不消费 + 无命令;
//   ④ 禁用 / 不可见(矩形在内容矩形外)节点不可点;
//   ⑤ Tab/Shift+Tab 环绕;方向键按几何规则移动(规则见 UiInputRouter::MoveFocusGeometric);
//   ⑥ 滚轮命中滚动容器时消费;转场注册表 None/Fade + 归一化进度采样。
//
// 说明:本文件不构建、不渲染;坐标取 1920x1080 设计 = 1920x1080 物理 ⇒ Scale=1,
// 因此物理坐标与设计坐标相同,断言直接读设计值。

#include "World/Core/KeyCodes.h"
#include "World/UI/UiInputRouter.h"
#include "World/UI/UiNavigator.h"
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

	constexpr float kDesignW = 1920.0f;
	constexpr float kDesignH = 1080.0f;

	UiAnchor FullStretch()
	{
		UiAnchor anchor;
		anchor.Min = { 0.0f, 0.0f };
		anchor.Max = { 1.0f, 1.0f };
		anchor.Pivot = { 0.5f, 0.5f };
		return anchor;
	}

	// 点锚定(左上角定位):rect = Offset .. Offset+Size。
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

	void SetOnClick(UiNode& node, std::string command)
	{
		node.On.push_back(UiCommandDecl { "Click", std::move(command) });
	}

	UiNode RootPanel()
	{
		UiNode root = MakeNode("root", "Panel");
		root.Anchor = FullStretch();
		return root;
	}

	UiDocument MakeDoc(std::string screen, std::vector<UiNode> roots)
	{
		UiDocument doc;
		doc.FormatVersion = kUiDocumentFormatVersion;
		doc.Screen = std::move(screen);
		doc.Design.Resolution = glm::vec2 { kDesignW, kDesignH };
		doc.Design.ScaleMode = UiScaleMode::ScaleWithScreenSize;
		doc.Design.Match = 0.5f;
		doc.Nodes = std::move(roots);
		return doc;
	}

	// 设计 1920x1080 → 物理 1920x1080:Scale = 1 ⇒ 物理坐标 == 设计坐标。
	void BuildScreen(UiScreen& screen, const UiDocument& doc)
	{
		std::string error;
		const bool built = screen.Build(doc, &error);
		Check(built, ("screen build: " + error).c_str(), __LINE__);
		UiSurface surface;
		surface.PhysicalSize = glm::vec2 { kDesignW, kDesignH };
		surface.DpiScale = 1.0f;
		Check(screen.Layout(ComputeUiViewport(doc.Design, doc.SafeArea, surface)), "screen layout", __LINE__);
	}

	Wui::WuiInputState MouseInput(glm::vec2 point)
	{
		Wui::WuiInputState input;
		input.MousePos = point;
		return input;
	}

	Wui::WuiInputState KeyInput(uint32_t key, bool shift = false)
	{
		Wui::WuiInputState input;
		input.Shift = shift;
		input.KeyPressed.push_back(key);
		return input;
	}

	void ClickLeft(Wui::WuiInputState& input)
	{
		input.MouseDown[0] = true;
		input.MouseClicked[0] = true;
	}

	// ---- ① 页面栈 / 层序 ----

	void TestStackAndLayers()
	{
		UiDocument docA = MakeDoc("A", { RootPanel() });
		UiDocument docB = MakeDoc("B", { RootPanel() });
		UiDocument docM = MakeDoc("ModalM", { RootPanel() });
		UiDocument docO = MakeDoc("OverlayO", { RootPanel() });
		UiDocument docD = MakeDoc("DebugD", { RootPanel() });
		UiScreen screenA;
		UiScreen screenB;
		UiScreen screenM;
		UiScreen screenO;
		UiScreen screenD;
		BuildScreen(screenA, docA);
		BuildScreen(screenB, docB);
		BuildScreen(screenM, docM);
		BuildScreen(screenO, docO);
		BuildScreen(screenD, docD);

		UiNavigator nav;
		CHECK(nav.Empty());
		CHECK(nav.Top() == nullptr);
		CHECK(nav.TopInteractiveScreen() == nullptr);
		CHECK(!nav.Push(UiPage {}));                       // 无屏幕 = false
		CHECK(nav.Push(UiPage { std::string(), &screenA }));   // Name 自动取 document.Screen
		CHECK(nav.Count() == 1);
		CHECK(nav.Top() != nullptr);
		CHECK_STR(nav.Top()->Name, "A");
		CHECK(nav.Push(MakeUiPage(screenB)));
		CHECK(nav.Count() == 2);
		CHECK_STR(nav.Top()->Name, "B");
		CHECK(nav.Pop());
		CHECK(nav.Count() == 1);
		CHECK_STR(nav.Top()->Name, "A");
		CHECK(nav.Replace(MakeUiPage(screenB)));
		CHECK(nav.Count() == 1);
		CHECK_STR(nav.Top()->Name, "B");
		CHECK(nav.Push(MakeUiPage(screenA)));
		CHECK(nav.Count() == 2);

		CHECK(nav.PushModal(MakeUiPage(screenM)));
		CHECK(nav.ModalCount() == 1);
		CHECK(nav.Count() == 2);                            // 模态不占页面栈
		CHECK(nav.PushLayer(UiLayer::Overlay, MakeUiPage(screenO)));
		CHECK(nav.PushLayer(UiLayer::Debug, MakeUiPage(screenD)));
		CHECK(!nav.PushLayer(UiLayer::Page, MakeUiPage(screenA)));    // 专用入口
		CHECK(!nav.PushLayer(UiLayer::Modal, MakeUiPage(screenM)));   // 专用入口

		CHECK_STR(UiLayerName(UiLayer::Page), "Page");
		CHECK_STR(UiLayerName(UiLayer::Modal), "Modal");
		CHECK(nav.TopLayer() == UiLayer::Debug);
		CHECK(nav.TopInteractiveScreen() == &screenD);

		const std::vector<UiDrawItem> layers = nav.Layers();
		CHECK(layers.size() == 5);
		CHECK(layers[0].Layer == UiLayer::Page && layers[0].Index == 0 &&
			layers[0].Page != nullptr && layers[0].Page->Name == "B");
		CHECK(layers[1].Layer == UiLayer::Page && layers[1].Index == 1 &&
			layers[1].Page != nullptr && layers[1].Page->Name == "A");
		CHECK(layers[2].Layer == UiLayer::Modal && layers[2].Page->Name == "ModalM");
		CHECK(layers[3].Layer == UiLayer::Overlay && layers[3].Page->Name == "OverlayO");
		CHECK(layers[4].Layer == UiLayer::Debug && layers[4].Page->Name == "DebugD");

		nav.Reset();
		CHECK(nav.Empty());
		CHECK(nav.ModalCount() == 0);
		CHECK(nav.Layers().empty());
		CHECK(nav.TopInteractiveScreen() == nullptr);
		CHECK(nav.TopLayer() == UiLayer::Page);
		CHECK(nav.Reset(MakeUiPage(screenA)));
		CHECK(nav.Count() == 1);
		CHECK_STR(nav.Top()->Name, "A");
	}

	// ---- ① 生命周期 + Back 路由 ----

	void TestLifecycleCallbacksAndBack()
	{
		UiDocument docA = MakeDoc("A", { RootPanel() });
		UiDocument docB = MakeDoc("B", { RootPanel() });
		UiDocument docM1 = MakeDoc("M1", { RootPanel() });
		UiDocument docM2 = MakeDoc("M2", { RootPanel() });
		UiScreen screenA;
		UiScreen screenB;
		UiScreen screenM1;
		UiScreen screenM2;
		BuildScreen(screenA, docA);
		BuildScreen(screenB, docB);
		BuildScreen(screenM1, docM1);
		BuildScreen(screenM2, docM2);

		UiNavigator nav;
		std::vector<std::string> log;
		UiPageCallbacks callbacks;
		callbacks.OnEnter = [&log](const UiPage& page) { log.push_back("enter:" + page.Name); };
		callbacks.OnExit = [&log](const UiPage& page) { log.push_back("exit:" + page.Name); };
		callbacks.OnPause = [&log](const UiPage& page) { log.push_back("pause:" + page.Name); };
		callbacks.OnResume = [&log](const UiPage& page) { log.push_back("resume:" + page.Name); };
		nav.SetPageCallbacks(std::move(callbacks));

		nav.Push(MakeUiPage(screenA));
		nav.Push(MakeUiPage(screenB));
		CHECK(log.size() == 3);
		CHECK_STR(log[0], "enter:A");
		CHECK_STR(log[1], "pause:A");
		CHECK_STR(log[2], "enter:B");

		nav.PushModal(MakeUiPage(screenM1));
		nav.PushModal(MakeUiPage(screenM2));
		CHECK(log.size() == 5);
		CHECK_STR(log[3], "enter:M1");
		CHECK_STR(log[4], "enter:M2");

		CHECK(nav.Back());                                  // 有模态先关模态
		CHECK(nav.ModalCount() == 1);
		CHECK(nav.Count() == 2);                            // 页面栈不动
		CHECK(log.size() == 6);
		CHECK_STR(log[5], "exit:M2");

		CHECK(nav.Back());
		CHECK(nav.ModalCount() == 0);
		CHECK(nav.Count() == 2);
		CHECK(log.size() == 7);
		CHECK_STR(log[6], "exit:M1");

		CHECK(nav.Back());                                  // 无模态才出栈
		CHECK(nav.Count() == 1);
		CHECK_STR(nav.Top()->Name, "A");
		CHECK(log.size() == 9);
		CHECK_STR(log[7], "exit:B");
		CHECK_STR(log[8], "resume:A");

		nav.Replace(MakeUiPage(screenM1));
		CHECK(log.size() == 11);
		CHECK_STR(log[9], "exit:A");
		CHECK_STR(log[10], "enter:M1");
	}

	// ---- ⑥ 转场注册表 ----

	void TestTransitionRegistry()
	{
		CHECK(UiNavigator::FindTransition("None") != nullptr);
		CHECK(UiNavigator::FindTransition("Fade") != nullptr);
		CHECK(UiNavigator::FindTransition("NoSuchTransition") == nullptr);
		CHECK(!UiNavigator::RegisterTransition("", UiNavigator::FindTransition("None")));
		CHECK(!UiNavigator::RegisterTransition("Broken", nullptr));
		CHECK(UiNavigator::RegisterTransition("Sharp", [](float progress, UiTransitionSample& out)
		{
			out.FromAlpha = progress < 1.0f ? 1.0f : 0.0f;
			out.ToAlpha = progress < 1.0f ? 0.0f : 1.0f;
		}));
		CHECK(UiNavigator::FindTransition("Sharp") != nullptr);

		UiDocument docA = MakeDoc("A", { RootPanel() });
		UiDocument docB = MakeDoc("B", { RootPanel() });
		UiScreen screenA;
		UiScreen screenB;
		BuildScreen(screenA, docA);
		BuildScreen(screenB, docB);
		UiNavigator nav;
		CHECK(nav.SetTransition("Fade"));
		CHECK_STR(nav.TransitionName(), "Fade");
		CHECK(!nav.SetTransition("NoSuchTransition"));
		CHECK_STR(nav.TransitionName(), "Fade");

		nav.Push(MakeUiPage(screenA));
		CHECK(!nav.TransitionActive());                     // 首次入栈没有出场页
		nav.Push(MakeUiPage(screenB));
		CHECK(nav.TransitionActive());
		CHECK_STR(nav.TransitionFromName(), "A");
		CHECK_NEAR(nav.TransitionProgress(), 0.0f, 1e-4f);

		UiTransitionSample sample = nav.SampleTransition();
		CHECK_NEAR(sample.FromAlpha, 1.0f, 1e-4f);
		CHECK_NEAR(sample.ToAlpha, 0.0f, 1e-4f);

		nav.SetTransitionProgress(0.25f);                   // 时间由调用方驱动
		sample = nav.SampleTransition();
		CHECK_NEAR(sample.FromAlpha, 0.75f, 1e-4f);
		CHECK_NEAR(sample.ToAlpha, 0.25f, 1e-4f);

		nav.SetTransitionProgress(1.0f);
		CHECK(!nav.TransitionActive());
		sample = nav.SampleTransition();
		CHECK_NEAR(sample.FromAlpha, 0.0f, 1e-4f);
		CHECK_NEAR(sample.ToAlpha, 1.0f, 1e-4f);

		CHECK(nav.SetTransition("None"));
		nav.Push(MakeUiPage(screenA));
		CHECK(!nav.TransitionActive());                     // None = 硬切换
	}

	// ---- ③ 输入消费双向 ----

	void TestPointerConsumptionBidirectional()
	{
		UiNode root = RootPanel();
		UiNode button = MakeNode("close", "Button");
		button.Anchor = PointAnchor(100.0f, 100.0f, 200.0f, 60.0f);
		SetOnClick(button, "ui.close");
		root.Children.push_back(std::move(button));
		UiDocument doc = MakeDoc("HUD", { std::move(root) });
		UiScreen screen;
		BuildScreen(screen, doc);

		CHECK(UiNodeRegistry::Find("Button") != nullptr);
		CHECK(UiNodeRegistry::Find("Button")->Interactive);

		UiCommandQueue queue;
		UiInputRouter router;
		router.SetCommandQueue(&queue);

		Wui::WuiInputState hitInput = MouseInput(glm::vec2 { 200.0f, 130.0f });
		ClickLeft(hitInput);
		CHECK(router.Update(screen, hitInput));             // 命中 = 消费
		CHECK_STR(router.LastFrame().HitId, "close");
		CHECK(queue.Size() == 1);
		CHECK_STR(queue.Commands()[0].NodeId, "close");
		CHECK_STR(queue.Commands()[0].Event, "Click");
		CHECK_STR(queue.Commands()[0].Command, "ui.close");

		queue.Clear();
		Wui::WuiInputState blankInput = MouseInput(glm::vec2 { 1600.0f, 200.0f });
		ClickLeft(blankInput);
		CHECK(!router.Update(screen, blankInput));          // 空白 = 不消费
		CHECK(queue.Empty());                               // 且无命令
		CHECK(router.LastFrame().HitId.empty());
		CHECK(!router.LastFrame().Focus.Changed);           // 焦点仍停在 close

		// 便捷重载:临时队列 + Update。
		UiCommandQueue otherQueue;
		Wui::WuiInputState again = MouseInput(glm::vec2 { 200.0f, 130.0f });
		ClickLeft(again);
		CHECK(router.Update(screen, again, otherQueue));
		CHECK(otherQueue.Size() == 1);
	}

	// ---- ④ 禁用 / 不可见不可点 ----

	void TestDisabledAndInvisibleNotClickable()
	{
		UiNode root = RootPanel();
		UiNode disabledButton = MakeNode("disabledBtn", "Button");
		disabledButton.Anchor = PointAnchor(100.0f, 100.0f, 200.0f, 60.0f);
		SetProp(disabledButton, "disabled", "true");
		SetOnClick(disabledButton, "ui.disabled");
		UiNode enabledButton = MakeNode("enabledBtn", "Button");
		enabledButton.Anchor = PointAnchor(100.0f, 300.0f, 200.0f, 60.0f);
		SetOnClick(enabledButton, "ui.enabled");
		UiNode offscreenButton = MakeNode("offscreenBtn", "Button");
		offscreenButton.Anchor = PointAnchor(2400.0f, 100.0f, 200.0f, 60.0f);   // 内容矩形外
		SetOnClick(offscreenButton, "ui.offscreen");
		root.Children.push_back(std::move(disabledButton));
		root.Children.push_back(std::move(enabledButton));
		root.Children.push_back(std::move(offscreenButton));
		UiDocument doc = MakeDoc("HUD", { std::move(root) });
		UiScreen screen;
		BuildScreen(screen, doc);

		UiCommandQueue queue;
		UiInputRouter router;
		router.SetCommandQueue(&queue);

		Wui::WuiInputState input = MouseInput(glm::vec2 { 200.0f, 130.0f });
		ClickLeft(input);
		CHECK(!router.Update(screen, input));               // 禁用 = 不可点
		CHECK(queue.Empty());
		CHECK(router.LastFrame().HitId.empty());

		input = MouseInput(glm::vec2 { 2500.0f, 130.0f });  // 按钮矩形在内容矩形外
		ClickLeft(input);
		CHECK(!router.Update(screen, input));               // 不可见 = 不可点
		CHECK(queue.Empty());

		const std::vector<std::string> order = router.FocusOrder(screen);
		CHECK(order.size() == 1);
		CHECK_STR(order[0], "enabledBtn");

		input = MouseInput(glm::vec2 { 200.0f, 330.0f });   // 反向对照:可点的仍可点
		ClickLeft(input);
		CHECK(router.Update(screen, input));
		CHECK(queue.Size() == 1);
		CHECK_STR(queue.Commands()[0].NodeId, "enabledBtn");
	}

	// ---- ② 模态只路由最高层 ----

	void TestModalOnlyRoutesTopLayer()
	{
		UiNode pageRoot = RootPanel();
		UiNode pageButton = MakeNode("pageBtn", "Button");
		pageButton.Anchor = PointAnchor(100.0f, 100.0f, 200.0f, 60.0f);
		SetOnClick(pageButton, "ui.page");
		pageRoot.Children.push_back(std::move(pageButton));
		UiDocument pageDoc = MakeDoc("Page", { std::move(pageRoot) });
		UiScreen pageScreen;
		BuildScreen(pageScreen, pageDoc);

		UiNode modalRoot = RootPanel();
		UiNode modalButton = MakeNode("modalBtn", "Button");
		modalButton.Anchor = PointAnchor(600.0f, 600.0f, 200.0f, 60.0f);
		SetOnClick(modalButton, "ui.modal");
		modalRoot.Children.push_back(std::move(modalButton));
		UiDocument modalDoc = MakeDoc("Modal", { std::move(modalRoot) });
		UiScreen modalScreen;
		BuildScreen(modalScreen, modalDoc);

		UiCommandQueue queue;
		UiInputRouter router;
		router.SetCommandQueue(&queue);
		UiNavigator nav;
		nav.Push(MakeUiPage(pageScreen));

		Wui::WuiInputState input = MouseInput(glm::vec2 { 200.0f, 130.0f });
		ClickLeft(input);
		CHECK(router.Update(nav, input));                   // 无模态:页面按钮可点
		CHECK(queue.Size() == 1);
		CHECK_STR(queue.Commands()[0].NodeId, "pageBtn");

		nav.PushModal(MakeUiPage(modalScreen));
		queue.Clear();
		input = MouseInput(glm::vec2 { 200.0f, 130.0f });   // 正好是页面按钮的位置
		ClickLeft(input);
		CHECK(router.Update(nav, input));                   // 模态屏障吃掉,不穿透玩法
		CHECK(router.LastFrame().ModalBarrier);
		CHECK(queue.Empty());                               // 关键:下层页面按钮不可点

		queue.Clear();
		input = MouseInput(glm::vec2 { 700.0f, 630.0f });
		ClickLeft(input);
		CHECK(router.Update(nav, input));
		CHECK(!router.LastFrame().ModalBarrier);
		CHECK(queue.Size() == 1);
		CHECK_STR(queue.Commands()[0].NodeId, "modalBtn");

		nav.PopModal();
		queue.Clear();
		input = MouseInput(glm::vec2 { 200.0f, 130.0f });
		ClickLeft(input);
		CHECK(router.Update(nav, input));
		CHECK(queue.Size() == 1);
		CHECK_STR(queue.Commands()[0].NodeId, "pageBtn");
	}

	// ---- ⑤ Tab 焦点环绕 ----

	void TestTabFocusWraps()
	{
		UiNode root = RootPanel();
		for (int i = 0; i < 3; ++i)
		{
			UiNode button = MakeNode("b" + std::to_string(i), "Button");
			button.Anchor = PointAnchor(100.0f + static_cast<float>(i) * 300.0f, 100.0f, 200.0f, 60.0f);
			root.Children.push_back(std::move(button));
		}
		UiDocument doc = MakeDoc("HUD", { std::move(root) });
		UiScreen screen;
		BuildScreen(screen, doc);

		UiInputRouter router;
		CHECK(router.FocusedId().empty());

		const Wui::WuiInputState tab = KeyInput(World::Tab);
		CHECK(!router.Update(screen, tab));
		CHECK(router.LastFrame().KeyboardConsumed);
		CHECK_STR(router.FocusedId(), "b0");
		CHECK(router.LastFrame().Focus.Changed);

		router.Update(screen, tab);
		CHECK_STR(router.FocusedId(), "b1");
		router.Update(screen, tab);
		CHECK_STR(router.FocusedId(), "b2");
		router.Update(screen, tab);
		CHECK_STR(router.FocusedId(), "b0");                // 环绕

		const Wui::WuiInputState shiftTab = KeyInput(World::Tab, true);
		router.Update(screen, shiftTab);
		CHECK_STR(router.FocusedId(), "b2");                // 反向环绕
		CHECK(router.LastFrame().Focus.Changed);
	}

	// ---- ⑤ 方向键几何规则 ----

	void TestArrowFocusGeometric()
	{
		UiNode root = RootPanel();
		const char* const ids[4] = { "b00", "b10", "b01", "b11" };
		const float xs[4] = { 100.0f, 500.0f, 100.0f, 500.0f };
		const float ys[4] = { 100.0f, 100.0f, 400.0f, 400.0f };
		for (int i = 0; i < 4; ++i)
		{
			UiNode button = MakeNode(ids[i], "Button");
			button.Anchor = PointAnchor(xs[i], ys[i], 200.0f, 60.0f);
			root.Children.push_back(std::move(button));
		}
		UiDocument doc = MakeDoc("HUD", { std::move(root) });
		UiScreen screen;
		BuildScreen(screen, doc);

		UiInputRouter router;
		CHECK(router.SetFocus(screen, "b00"));

		const Wui::WuiInputState right = KeyInput(World::Right);
		CHECK(!router.Update(screen, right));
		CHECK(router.LastFrame().KeyboardConsumed);
		CHECK_STR(router.FocusedId(), "b10");

		const Wui::WuiInputState down = KeyInput(World::Down);
		router.Update(screen, down);
		CHECK_STR(router.FocusedId(), "b11");

		const Wui::WuiInputState left = KeyInput(World::Left);
		router.Update(screen, left);
		CHECK_STR(router.FocusedId(), "b01");

		const Wui::WuiInputState up = KeyInput(World::Up);
		router.Update(screen, up);
		CHECK_STR(router.FocusedId(), "b00");

		// 边界:最右再按右 = 无候选,焦点不变。
		CHECK(router.SetFocus(screen, "b10"));
		router.Update(screen, right);
		CHECK_STR(router.FocusedId(), "b10");
	}

	// ---- ⑥ 滚轮 ----

	void TestWheelScrollContainer()
	{
		UiNode scrollRoot = RootPanel();
		SetProp(scrollRoot, "scrollable", "true");
		UiDocument scrollDoc = MakeDoc("Scroll", { std::move(scrollRoot) });
		UiScreen scrollScreen;
		BuildScreen(scrollScreen, scrollDoc);

		UiInputRouter router;
		Wui::WuiInputState wheel = MouseInput(glm::vec2 { 900.0f, 500.0f });
		wheel.Wheel = -1.0f;
		CHECK(!router.Update(scrollScreen, wheel));         // 滚轮不是"指针按下"
		CHECK(router.LastFrame().WheelConsumed);
		CHECK_STR(router.LastFrame().WheelTargetId, "root");

		UiDocument plainDoc = MakeDoc("Plain", { RootPanel() });
		UiScreen plainScreen;
		BuildScreen(plainScreen, plainDoc);
		wheel.Wheel = -1.0f;
		CHECK(!router.Update(plainScreen, wheel));
		CHECK(!router.LastFrame().WheelConsumed);

		wheel.Wheel = 0.0f;
		CHECK(!router.Update(plainScreen, wheel));
		CHECK(!router.LastFrame().WheelConsumed);
	}

	// ---- 文本焦点域(独立域,最小实现)----

	void TestTextFocusDomain()
	{
		UiNode root = RootPanel();
		UiNode first = MakeNode("b0", "Button");
		first.Anchor = PointAnchor(100.0f, 100.0f, 200.0f, 60.0f);
		UiNode second = MakeNode("b1", "Button");
		second.Anchor = PointAnchor(400.0f, 100.0f, 200.0f, 60.0f);
		root.Children.push_back(std::move(first));
		root.Children.push_back(std::move(second));
		UiDocument doc = MakeDoc("HUD", { std::move(root) });
		UiScreen screen;
		BuildScreen(screen, doc);

		UiInputRouter router;
		CHECK(router.SetFocus(screen, "b0", UiFocusDomain::Text));
		CHECK(router.FocusDomain() == UiFocusDomain::Text);

		Wui::WuiInputState text;
		text.TextInput.push_back(static_cast<uint32_t>('a'));
		CHECK(!router.Update(screen, text));
		CHECK(router.LastFrame().KeyboardConsumed);
		CHECK_STR(router.FocusedId(), "b0");
		CHECK(router.FocusDomain() == UiFocusDomain::Text);

		const Wui::WuiInputState arrow = KeyInput(World::Right);
		router.Update(screen, arrow);
		CHECK(!router.LastFrame().KeyboardConsumed);        // 文本域:方向键归文本编辑器
		CHECK_STR(router.FocusedId(), "b0");

		const Wui::WuiInputState tab = KeyInput(World::Tab);
		router.Update(screen, tab);
		CHECK(router.LastFrame().KeyboardConsumed);
		CHECK(router.FocusDomain() == UiFocusDomain::Navigation);
		CHECK_STR(router.FocusedId(), "b1");                // Tab 退出文本域并前进
	}

	// ---- 键盘激活 ----

	void TestFocusActivationEmitsCommand()
	{
		UiNode root = RootPanel();
		UiNode button = MakeNode("ok", "Button");
		button.Anchor = PointAnchor(100.0f, 100.0f, 200.0f, 60.0f);
		SetOnClick(button, "ui.ok");
		root.Children.push_back(std::move(button));
		UiDocument doc = MakeDoc("HUD", { std::move(root) });
		UiScreen screen;
		BuildScreen(screen, doc);

		UiCommandQueue queue;
		UiInputRouter router;
		router.SetCommandQueue(&queue);
		CHECK(router.SetFocus(screen, "ok"));

		const Wui::WuiInputState enter = KeyInput(World::Enter);
		CHECK(!router.Update(screen, enter));
		CHECK(router.LastFrame().KeyboardConsumed);
		CHECK(queue.Size() == 1);
		CHECK_STR(queue.Commands()[0].NodeId, "ok");
		CHECK_STR(queue.Commands()[0].Command, "ui.ok");
	}
}

int main()
{
	try
	{
		TestStackAndLayers();
		TestLifecycleCallbacksAndBack();
		TestTransitionRegistry();
		TestPointerConsumptionBidirectional();
		TestDisabledAndInvisibleNotClickable();
		TestModalOnlyRoutesTopLayer();
		TestTabFocusWraps();
		TestArrowFocusGeometric();
		TestWheelScrollContainer();
		TestTextFocusDomain();
		TestFocusActivationEmitsCommand();
	}
	catch (const std::exception& e)
	{
		std::fprintf(stderr, "World.UiNavInput FAILED: %s\n", e.what());
		return 1;
	}
	std::printf("World.UiNavInput OK\n");
	return 0;
}
