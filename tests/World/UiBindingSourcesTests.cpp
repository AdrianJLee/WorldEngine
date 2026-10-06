// World.UiBindingSources — GameUI(M26):绑定解析器(ecs:/service:)+ 运行态属性覆盖 +
// 命令 router,headless。
//
// 覆盖(派工单验收):
//   ① 注册 `ecs:` 解析器后,`ecs:<Entity>/<Component>/<Field>` 与 `ecs:<Component>/<Field>`
//      取到**真实组件字段**的文本值(字段读法走 SchemaRegistry 反射);
//   ② 缺失实体 / 组件 / 字段 / 无上下文 ⇒ Refresh 不求值 + 可读 warning(不猜、不崩);
//   ③ 变更检测 = 版本号(引擎两个宿主用场景 tick):同版本不重复求值,版本变化才更新;
//   ④ 运行态属性覆盖表:同键覆盖、空键拒绝;`UiPainter` 读取属性时**优先**取覆盖值
//      (绘制命令文本 = 绑定值,文档属性不被改写);
//   ⑤ 命令 router:每条命令 → `EventBus` 的 `UiCommandEvent`(订阅者收到、字段正确、超长截断);
//      `ui.close` 有 `UiNavigator` 时先关模态再出栈(`ui.back` 同 `UiNavigator::Back`);
//   ⑥ `service:` 只读查询(会话级:input/level/save)与"无会话"可读报错;
//   ⑦ `UiHost` 全链路:`.wui` 文档 `Bind:` → Attach → 按 tick Refresh → 覆盖 → 画出绑定值。
//
// 本文件不构建、不渲染、不开窗口;只跑 headless 逻辑 + 命令流断言。

#include "World/Core/Log.h"
#include "World/Core/WorldContext.h"
#include "World/Gameplay/Framework/EventBus.h"
#include "World/Gameplay/Runtime/GameApp.h"
#include "World/Scene/Components.h"
#include "World/Scene/Entity.h"
#include "World/Scene/Scene.h"
#include "World/UI/UiBinding.h"
#include "World/UI/UiBindingSources.h"
#include "World/UI/UiCommandRouter.h"
#include "World/UI/UiDocument.h"
#include "World/UI/UiHost.h"
#include "World/UI/UiNavigator.h"
#include "World/UI/UiPainter.h"
#include "World/UI/UiScreen.h"
#include "World/WUI/WuiContext.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
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

	void CheckContains(const std::string& text, const std::string& needle, const char* expression, int line)
	{
		if (text.find(needle) == std::string::npos)
		{
			throw std::runtime_error(std::string("line ") + std::to_string(line) + ": " + expression +
				" ('" + text + "' does not contain '" + needle + "')");
		}
	}
#define CHECK_CONTAINS(text, needle) CheckContains((text), (needle), #text " contains " #needle, __LINE__)

	// ---- 夹具 ----

	UiNode MakeNode(std::string id, std::string type)
	{
		UiNode node;
		node.Id = std::move(id);
		node.Type = std::move(type);
		return node;
	}

	UiDocument MakeDoc(std::vector<UiNode> roots, std::string screen = "HUD")
	{
		UiDocument doc;
		doc.Screen = std::move(screen);
		doc.Nodes = std::move(roots);
		return doc;
	}

	UiScreen BuildScreen(const UiDocument& doc)
	{
		UiScreen screen;
		std::string error;
		Check(screen.Build(doc, &error), ("screen build: " + error).c_str(), __LINE__);
		return screen;
	}

	// 单条绑定源 → 求值结果(失败 = ok=false + 第一条 warning 文本)。
	std::string ResolveSingle(const std::string& source, UiBindingContext* context, bool& ok)
	{
		UiNode node = MakeNode("n", "Label");
		node.Bind.push_back(UiBindingDecl { "text", source });
		const UiScreen screen = BuildScreen(MakeDoc({ std::move(node) }));

		UiBindingTable table;
		table.Attach(screen);
		table.Refresh(UiBindingDataSource { 1, context });

		std::string value;
		ok = table.Value("n", "text", value);
		if (ok)
			return value;
		return table.Warnings().empty() ? std::string("(no warning)") : table.Warnings().front().Message;
	}

	// ---- ① / ② ecs: 解析器 ----

	void TestEcsResolverReadsRealComponentFields()
	{
		WorldContext context;
		Scene scene(context);
		Entity player = Entity::CreateEntity(&scene, "Player");
		player.AddComponent<TransformComponent>();
		player.GetComponent<TransformComponent>().Location = glm::vec3 { 1.5f, -2.0f, 3.0f };

		RegisterEcsBindingResolver();
		UiBindingContext bindingContext { &scene, nullptr };

		// 3 段(实体名)+ 全名 / 短名组件;2 段(首个带该组件的实体);Kind::Name 字段也走同一反射。
		bool ok = false;
		CHECK_STR(ResolveSingle("ecs:Player/World::TransformComponent/Location", &bindingContext, ok), "1.5,-2,3");
		CHECK(ok);
		CHECK_STR(ResolveSingle("ecs:Player/TransformComponent/Location", &bindingContext, ok), "1.5,-2,3");
		CHECK(ok);
		CHECK_STR(ResolveSingle("ecs:World::TransformComponent/Location", &bindingContext, ok), "1.5,-2,3");
		CHECK(ok);
		CHECK_STR(ResolveSingle("ecs:Player/World::TagComponent/Tag", &bindingContext, ok), "Player");
		CHECK(ok);
		// 十进制句柄与名字是同一个实体(与编辑器 AI 通道 `scene.get handle=` 同一口径)。
		const std::string handleSource =
			"ecs:" + std::to_string(static_cast<uint32_t>(player)) + "/World::TransformComponent/Location";
		CHECK_STR(ResolveSingle(handleSource, &bindingContext, ok), "1.5,-2,3");
		CHECK(ok);
	}

	void TestEcsResolverFailuresAreReadable()
	{
		WorldContext context;
		Scene scene(context);
		Entity player = Entity::CreateEntity(&scene, "Player");
		player.AddComponent<TransformComponent>();
		Entity::CreateEntity(&scene, "NoTransform");   // 有 Tag、没有 Transform 的实体

		RegisterEcsBindingResolver();
		UiBindingContext bindingContext { &scene, nullptr };

		bool ok = true;
		std::string message;

		message = ResolveSingle("ecs:Missing/World::TransformComponent/Location", &bindingContext, ok);
		CHECK(!ok);
		CHECK_CONTAINS(message, "no entity named 'Missing'");

		message = ResolveSingle("ecs:Player/NopeComponent/Location", &bindingContext, ok);
		CHECK(!ok);
		CHECK_CONTAINS(message, "no component type 'NopeComponent'");

		message = ResolveSingle("ecs:Player/World::TransformComponent/Nope", &bindingContext, ok);
		CHECK(!ok);
		CHECK_CONTAINS(message, "has no field 'Nope'");

		message = ResolveSingle("ecs:NoTransform/World::TransformComponent/Location", &bindingContext, ok);
		CHECK(!ok);
		CHECK_CONTAINS(message, "has no component 'World::TransformComponent'");

		// 2 段但没有任何实体带该组件。
		message = ResolveSingle("ecs:World::MeshRendererComponent/MeshIndex", &bindingContext, ok);
		CHECK(!ok);
		CHECK_CONTAINS(message, "no entity has component");

		// 没有上下文(宿主没喂运行时数据源)= 可读报错,不是崩溃或空值。
		message = ResolveSingle("ecs:Player/World::TransformComponent/Location", nullptr, ok);
		CHECK(!ok);
		CHECK_CONTAINS(message, "needs a scene");
	}

	void TestChangeDetectionUsesVersion()
	{
		WorldContext context;
		Scene scene(context);
		Entity player = Entity::CreateEntity(&scene, "Player");
		player.AddComponent<TransformComponent>();
		player.GetComponent<TransformComponent>().Location = glm::vec3 { 1.0f, 0.0f, 0.0f };

		UiNode node = MakeNode("hp", "Label");
		node.Bind.push_back(UiBindingDecl { "text", "ecs:Player/World::TransformComponent/Location" });
		const UiScreen screen = BuildScreen(MakeDoc({ std::move(node) }));

		UiBindingTable table;
		table.Attach(screen);
		UiBindingContext bindingContext { &scene, nullptr };

		CHECK(table.Refresh(UiBindingDataSource { 7, &bindingContext }) == 1);
		std::string value;
		CHECK(table.Value("hp", "text", value));
		CHECK_STR(value, "1,0,0");

		// 同 tick ⇒ 解析器零调用(返回 0),值保留。
		CHECK(table.Refresh(UiBindingDataSource { 7, &bindingContext }) == 0);

		// 场景变了但 tick 没变(宿主口径 = 每帧喂 tick)⇒ 不重算,仍是上一值。
		player.GetComponent<TransformComponent>().Location = glm::vec3 { 9.0f, 0.0f, 0.0f };
		CHECK(table.Refresh(UiBindingDataSource { 7, &bindingContext }) == 0);
		CHECK(table.Value("hp", "text", value));
		CHECK_STR(value, "1,0,0");

		// tick 前进 ⇒ 重算并取到新值。
		CHECK(table.Refresh(UiBindingDataSource { 8, &bindingContext }) == 1);
		CHECK(table.Value("hp", "text", value));
		CHECK_STR(value, "9,0,0");
	}

	// ---- ⑥ service: 解析器 ----

	void TestServiceResolverWithoutSessionIsReadable()
	{
		Gameplay::GameApp::Shutdown();
		RegisterBuiltinBindingSources(nullptr);   // 清掉上次登记的回退源,确定"无会话"路径

		bool ok = true;
		const std::string message = ResolveSingle("service:level.state", nullptr, ok);
		CHECK(!ok);
		CHECK_CONTAINS(message, "needs an active GameApp session");
	}

	void TestServiceResolverReadsSession()
	{
		Gameplay::GameApp::Shutdown();
		Gameplay::GameAppDesc desc;
		desc.ProjectId = "we-ui-binding-sources-test";
		Gameplay::GameApp::Create(desc);
		Gameplay::GameApp& app = Gameplay::GameApp::Get();

		// 宿主 Initialize 口径:一次注册(此处显式传会话;解析时也优先用 Context.App)。
		RegisterBuiltinBindingSources(&app);
		UiBindingContext bindingContext { nullptr, &app };

		bool ok = false;
		CHECK_STR(ResolveSingle("service:level.state", &bindingContext, ok), "Idle");
		CHECK(ok);
		CHECK_STR(ResolveSingle("service:level.loading", &bindingContext, ok), "0");
		CHECK(ok);
		CHECK_STR(ResolveSingle("service:input.players", &bindingContext, ok), "0");
		CHECK(ok);
		CHECK_STR(ResolveSingle("service:input.jump", &bindingContext, ok), "0");   // 未按下
		CHECK(ok);
		CHECK_STR(ResolveSingle("service:save.exists", &bindingContext, ok), "0");  // 未注入 SaveService
		CHECK(ok);

		// 未接 / 拼错的查询 = 可读报错(不发明语义、不静默给空)。
		const std::string message = ResolveSingle("service:nope.query", &bindingContext, ok);
		CHECK(!ok);
		CHECK_CONTAINS(message, "unknown service query 'service:nope.query'");

		// 只有回退源(上下文没给 App)时也能求值。
		UiBindingContext fallbackOnly { nullptr, nullptr };
		CHECK_STR(ResolveSingle("service:level.primary", &fallbackOnly, ok), "");
		CHECK(ok);

		Gameplay::GameApp::Shutdown();
		RegisterBuiltinBindingSources(nullptr);
	}

	// ---- ④ 覆盖表 + 绘制读取路径 ----

	void TestOverrideTableSemantics()
	{
		UiPropertyOverrideTable overrides;
		CHECK(overrides.Empty());
		CHECK(overrides.Find("hp", "text") == nullptr);

		CHECK(overrides.Find("", "") == nullptr);   // 空键不查
		overrides.Set("", "text", "x");
		overrides.Set("hp", "", "x");
		CHECK(overrides.Count() == 0);              // 空节点 Id / 空属性名 = 拒绝写入

		overrides.Set("hp", "text", "50");
		CHECK(overrides.Count() == 1);
		const std::string* value = overrides.Find("hp", "text");
		CHECK(value != nullptr && *value == "50");

		overrides.Set("hp", "text", "60");          // 同键 = 覆盖,不追加
		CHECK(overrides.Count() == 1);
		value = overrides.Find("hp", "text");
		CHECK(value != nullptr && *value == "60");

		overrides.Set("hp", "color", "#FF0000");
		CHECK(overrides.Count() == 2);
		CHECK(overrides.Find("hp", "text") != nullptr);
		CHECK(overrides.Find("hp", "color") != nullptr);

		overrides.Clear();
		CHECK(overrides.Empty());
		CHECK(overrides.Find("hp", "text") == nullptr);
	}

	void TestPainterPrefersOverrideValue()
	{
		UiNode node = MakeNode("lbl", "Label");
		node.Props.push_back(UiProp { "text", "document" });
		UiDocument doc = MakeDoc({ std::move(node) });
		UiScreen screen = BuildScreen(doc);

		UiSurface surface;
		surface.PhysicalSize = glm::vec2 { 1280.0f, 720.0f };
		surface.DpiScale = 1.0f;
		CHECK(screen.Layout(ComputeUiViewport(doc.Design, doc.SafeArea, surface)));

		Wui::WuiInputState input;
		input.ViewportSize = surface.PhysicalSize;

		// 无覆盖 = 文档属性(基线)。
		{
			Wui::WuiContext ctx;
			ctx.BeginFrame(input);
			const UiPaintResult result = UiPainter::Paint(ctx, screen);
			CHECK(result.Ok());
			bool sawDocument = false;
			for (const Wui::WuiDrawCommand& command : ctx.Commands())
				if (command.Kind == Wui::WuiDrawKind::Text && command.Text == "document")
					sawDocument = true;
			CHECK(sawDocument);
		}

		// 有覆盖 = 覆盖值(绑定求值结果),文档属性不变。
		{
			UiPropertyOverrideTable overrides;
			overrides.Set("lbl", "text", "bound");
			UiPaintOptions options;
			options.RegisterAccessibility = false;
			options.Overrides = &overrides;

			Wui::WuiContext ctx;
			ctx.BeginFrame(input);
			const UiPaintResult result = UiPainter::Paint(ctx, screen, options);
			CHECK(result.Ok());
			bool sawOverride = false;
			bool sawDocument = false;
			for (const Wui::WuiDrawCommand& command : ctx.Commands())
			{
				if (command.Kind != Wui::WuiDrawKind::Text)
					continue;
				if (command.Text == "bound")
					sawOverride = true;
				if (command.Text == "document")
					sawDocument = true;
			}
			CHECK(sawOverride);
			CHECK(!sawDocument);
			// 文档本身没被改写(覆盖只活在运行态表里)。
			const UiProp* prop = screen.Document().FindNode("lbl")->FindProp("text");
			CHECK(prop != nullptr && prop->Value == "document");
		}
	}

	// ---- ⑤ 命令 router ----

	void TestCommandRouterEmitsEvents()
	{
		Gameplay::EventBus events;
		std::vector<UiCommandEvent> received;
		events.Subscribe<UiCommandEvent>([&received](const UiCommandEvent& event)
		{
			received.push_back(event);
		});

		UiCommandQueue queue;
		queue.Push(UiInputCommand { "btn.start", "Click", "game.start", std::string() });
		queue.Push(UiInputCommand { "hp.bar", "Change", std::string(), std::string() });

		const UiCommandDispatchResult result = UiCommandRouter::Dispatch(queue, events);
		CHECK(result.Dispatched == 2);
		CHECK(result.Navigations == 0);
		// EmitDeferred = 入队,不立即派发(与会话帧末 `DispatchPending` 同一路径)。
		CHECK(events.GetPendingCount() == 2);
		CHECK(received.empty());
		CHECK(events.DispatchPending() == 2);
		CHECK(received.size() == 2);
		CHECK_STR(std::string(received[0].NodeId), "btn.start");
		CHECK_STR(std::string(received[0].Event), "Click");
		CHECK_STR(std::string(received[0].Command), "game.start");
		CHECK_STR(std::string(received[1].Command), "");   // 只声明事件、没声明命令也要送出去

		// 超长字段按字节截断(定长载荷,不越界)。
		const std::string longId(200, 'n');
		UiCommandQueue truncating;
		truncating.Push(UiInputCommand { longId, "Click", longId, std::string() });
		CHECK(UiCommandRouter::Dispatch(truncating, events).Dispatched == 1);
		CHECK(events.GetPendingCount() == 1);
		CHECK(events.DispatchPending() == 1);
		CHECK(received.size() == 3);
		CHECK(received[2].NodeId[kUiCommandEventNodeIdCapacity - 1] == '\0');
		CHECK_STR(std::string(received[2].NodeId), std::string(kUiCommandEventNodeIdCapacity - 1, 'n'));
		CHECK(received[2].Command[kUiCommandEventCommandCapacity - 1] == '\0');
		CHECK_STR(std::string(received[2].Command), std::string(kUiCommandEventCommandCapacity - 1, 'n'));
	}

	void TestCommandRouterNavigation()
	{
		const UiScreen pageA = BuildScreen(MakeDoc({ MakeNode("a", "Panel") }, "PageA"));
		const UiScreen pageB = BuildScreen(MakeDoc({ MakeNode("b", "Panel") }, "PageB"));

		UiNavigator navigator;
		CHECK(navigator.Push(MakeUiPage(pageA)));
		CHECK(navigator.PushModal(MakeUiPage(pageB)));
		CHECK(navigator.ModalCount() == 1);

		Gameplay::EventBus events;
		std::size_t received = 0;
		events.Subscribe<UiCommandEvent>([&received](const UiCommandEvent&) { ++received; });

		// ui.close:有模态先关模态(栈不动),并且照常发事件(UI → 逻辑的出口无损)。
		UiCommandQueue close;
		close.Push(UiInputCommand { "pause.close", "Click", " ui.close ", std::string() });
		const UiCommandDispatchResult closed = UiCommandRouter::Dispatch(close, events, &navigator);
		CHECK(closed.Dispatched == 1);
		CHECK(closed.Navigations == 1);
		CHECK(navigator.ModalCount() == 0);
		CHECK(navigator.Count() == 1);

		// ui.back:与 `UiNavigator::Back` 同口径(模态已关 ⇒ 出栈)。
		UiCommandQueue back;
		back.Push(UiInputCommand { "hud.back", "Click", "ui.back", std::string() });
		const UiCommandDispatchResult wentBack = UiCommandRouter::Dispatch(back, events, &navigator);
		CHECK(wentBack.Navigations == 1);
		CHECK(navigator.Count() == 0);

		CHECK(events.GetPendingCount() == 2);
		CHECK(events.DispatchPending() == 2);
		CHECK(received == 2);

		// 没给 navigator = 只发事件,不导航(引擎当前两个宿主的口径)。
		CHECK(navigator.Push(MakeUiPage(pageA)));
		UiCommandQueue noNavigator;
		noNavigator.Push(UiInputCommand { "hud.close", "Click", "ui.close", std::string() });
		const UiCommandDispatchResult passive = UiCommandRouter::Dispatch(noNavigator, events, nullptr);
		CHECK(passive.Dispatched == 1);
		CHECK(passive.Navigations == 0);
		CHECK(navigator.Count() == 1);
		CHECK(events.DispatchPending() == 1);
	}

	// ---- ⑦ UiHost 全链路(文档 Bind: → 覆盖 → 画出绑定值)----

	void TestUiHostBindingPipeline()
	{
		WorldContext context;
		Scene scene(context);
		Entity player = Entity::CreateEntity(&scene, "Player");
		player.AddComponent<TransformComponent>();
		player.GetComponent<TransformComponent>().Location = glm::vec3 { 12.5f, 0.0f, 0.0f };

		const std::string document =
			"FormatVersion: 1\n"
			"Screen: BindHost\n"
			"Design:\n"
			"  Resolution: [1920, 1080]\n"
			"  ScaleMode: ScaleWithScreenSize\n"
			"  Match: 0.5\n"
			"Nodes:\n"
			"  - Id: hp\n"
			"    Type: Label\n"
			"    Bind:\n"
			"      text: \"ecs:Player/World::TransformComponent/Location\"\n"
			"    Anchor: { Min: [0, 0], Max: [0, 0], Pivot: [0, 0], Offset: [0, 0], Size: [200, 20] }\n";

		const std::filesystem::path path =
			std::filesystem::temp_directory_path() / "we-ui-binding-sources-test.wui";
		{
			std::ofstream file(path, std::ios::binary | std::ios::trunc);
			file << document;
			CHECK(file.good());
		}

		_putenv_s("WLD_UI_DOC", path.string().c_str());
		UiHost host;
		host.SetAccessibilityMode(UiHostAccessibilityMode::SharedChannel);   // 不碰全局无障碍通道
		host.Initialize(std::filesystem::path {});
		CHECK(host.Enabled());
		CHECK(host.Bindings().Count() == 1);
		CHECK(host.PropertyOverrides().Empty());

		host.SetBindingRuntime(UiBindingContext { &scene, Gameplay::GameApp::TryGet() },
			scene.CurrentWorldTick());

		Wui::WuiContext ctx;
		Wui::WuiInputState input;
		input.ViewportSize = glm::vec2 { 1280.0f, 720.0f };
		ctx.BeginFrame(input);
		host.DrawFrame(ctx, input);

		std::string value;
		CHECK(host.Bindings().Value("hp", "text", value));
		CHECK_STR(value, "12.5,0,0");
		const std::string* override = host.PropertyOverrides().Find("hp", "text");
		CHECK(override != nullptr && *override == "12.5,0,0");

		bool sawBoundText = false;
		for (const Wui::WuiDrawCommand& command : ctx.Commands())
			if (command.Kind == Wui::WuiDrawKind::Text && command.Text == "12.5,0,0")
				sawBoundText = true;
		CHECK(sawBoundText);

		host.Shutdown();
		_putenv_s("WLD_UI_DOC", "");
		std::filesystem::remove(path);
	}
}

int main()
{
	try
	{
		World::Log::Init();

		TestEcsResolverReadsRealComponentFields();
		TestEcsResolverFailuresAreReadable();
		TestChangeDetectionUsesVersion();
		TestServiceResolverWithoutSessionIsReadable();
		TestServiceResolverReadsSession();
		TestOverrideTableSemantics();
		TestPainterPrefersOverrideValue();
		TestCommandRouterEmitsEvents();
		TestCommandRouterNavigation();
		TestUiHostBindingPipeline();
	}
	catch (const std::exception& e)
	{
		std::fprintf(stderr, "World.UiBindingSources FAILED: %s\n", e.what());
		return 1;
	}
	std::printf("World.UiBindingSources OK\n");
	return 0;
}
