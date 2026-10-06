// World.UiHotReload — GameUI(M34)`.wui` 热重载,headless。
//
// 背景(用户报障"改了 UI 的属性没变化,不起作用"):`UiHost` 只在 `Initialize` 读一次盘,
// 全仓没有 `.wui` 监听 ⇒ 运行时/Play 里改文件画面不变。
// 契约 `contract.ui-runtime` §9:`.wui` 变更 → 重载 → 按稳定 `Id` 迁移运行态;
// 失败保留上一份可用版本并报可读原因,**不允许半加载界面**。
//
// 覆盖(派工单验收):
//   ① 改盘上内容 → `PollDocumentChanges` 后新文本生效(文档 + 绘制命令双证);
//   ② 坏文件(解析失败 / 未登记 Type)→ 重载失败、**旧文档仍在**(节点数不变)、有可读 error、
//      `Enabled()` 不变、同一份坏文件不每帧重试,写回好文件能恢复;
//   ③ 重载后**滚动偏移**与**焦点**按稳定 Id 保住(且偏移只作用一次,不叠加);
//   ④ 判脏口径 = 文件戳(大小 + mtime):同一次内容不重复重载。
//
// 说明:本文件不构建、不渲染;设计分辨率 = 视口尺寸 = 1920x1080(ConstantPixelSize)
//       ⇒ Scale = 1,物理坐标 = 设计坐标,断言直接读设计值。每次改盘都断言文件"大小"真的变了,
//       避免依赖文件系统时间戳粒度(判据是大小 ⊕ mtime,任一变即脏)。

#include "World/Core/KeyCodes.h"
#include "World/UI/UiHost.h"
#include "World/UI/UiInputRouter.h"
#include "World/UI/UiScreen.h"
#include "World/UI/UiTypes.h"
#include "World/WUI/WuiContext.h"

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>

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

	// ---- 夹具文档(全文对照 contract.ui-document-format:v1)----
	//
	// root  :Panel,铺满内容矩形,`scrollable: true` ⇒ 唯一滚动容器;
	// label :Label,`text` 是 ①③ 的断言对象;
	// step  :Label,放在 y=1400 ⇒ 内容高 1460 > 容器 1080 ⇒ 纵向可滚 380(不是"内容不溢出"的假阳性);
	// action:Button,唯一可聚焦节点(y=200 在内容矩形内 = 可见可点)。
	const char* kDocAlpha = R"YAML(FormatVersion: 1
Screen: HUD
Design:
  Resolution: [1920, 1080]
  ScaleMode: ConstantPixelSize
Nodes:
  - Id: root
    Type: Panel
    Props:
      scrollable: "true"
    Anchor:
      Min: [0, 0]
      Max: [1, 1]
      Pivot: [0.5, 0.5]
      Offset: [0, 0]
      Size: [0, 0]
    Children:
      - Id: label
        Type: Label
        Props:
          text: Alpha
        Anchor:
          Min: [0, 0]
          Max: [0, 0]
          Pivot: [0, 0]
          Offset: [0, 0]
          Size: [200, 60]
      - Id: step
        Type: Label
        Props:
          text: Step
        Anchor:
          Min: [0, 0]
          Max: [0, 0]
          Pivot: [0, 0]
          Offset: [0, 1400]
          Size: [200, 60]
      - Id: action
        Type: Button
        Props:
          label: Go
        Anchor:
          Min: [0, 0]
          Max: [0, 0]
          Pivot: [0, 0]
          Offset: [0, 200]
          Size: [200, 60]
)YAML";

	// 与 kDocAlpha 同一形状,只把 label 文本换成更长的 "Bravo-Charlie"(文件大小必然不同)
	// 并追加一个节点(节点数 4 → 5),用来证明"重载真的读了盘、用了新文档"。
	const char* kDocBravo = R"YAML(FormatVersion: 1
Screen: HUD
Design:
  Resolution: [1920, 1080]
  ScaleMode: ConstantPixelSize
Nodes:
  - Id: root
    Type: Panel
    Props:
      scrollable: "true"
    Anchor:
      Min: [0, 0]
      Max: [1, 1]
      Pivot: [0.5, 0.5]
      Offset: [0, 0]
      Size: [0, 0]
    Children:
      - Id: label
        Type: Label
        Props:
          text: Bravo-Charlie
        Anchor:
          Min: [0, 0]
          Max: [0, 0]
          Pivot: [0, 0]
          Offset: [0, 0]
          Size: [200, 60]
      - Id: step
        Type: Label
        Props:
          text: Step
        Anchor:
          Min: [0, 0]
          Max: [0, 0]
          Pivot: [0, 0]
          Offset: [0, 1400]
          Size: [200, 60]
      - Id: action
        Type: Button
        Props:
          label: Go
        Anchor:
          Min: [0, 0]
          Max: [0, 0]
          Pivot: [0, 0]
          Offset: [0, 200]
          Size: [200, 60]
      - Id: extra
        Type: Label
        Props:
          text: Extra
        Anchor:
          Min: [0, 0]
          Max: [0, 0]
          Pivot: [0, 0]
          Offset: [0, 300]
          Size: [200, 60]
)YAML";

	// 坏文件(a):YAML 解析失败(未闭合的 flow 序列)。
	const char* kDocMalformed = "FormatVersion: 1\nScreen: HUD\nNodes: [oops\n";
	// 坏文件(b):YAML 语法没事,但**语义校验**失败 —— 版本门禁(契约:FormatVersion 缺失或 != 1
	// 必须拒绝加载,不猜、不迁移)。`UiDocumentIO::Parse` 会在返回前跑 `ValidateUiDocument`,
	// 所以这一类坏文件与 (a) 一样走"加载失败 ⇒ 保留上一份可用版本"这条路。
	const char* kDocBadVersion = R"YAML(FormatVersion: 2
Screen: HUD
Design:
  Resolution: [1920, 1080]
  ScaleMode: ConstantPixelSize
Nodes:
  - Id: root
    Type: Panel
    Anchor:
      Min: [0, 0]
      Max: [1, 1]
      Pivot: [0.5, 0.5]
      Offset: [0, 0]
      Size: [0, 0]
    Children:
      - Id: label
        Type: Label
        Props:
          text: ShouldNeverAppear
        Anchor:
          Min: [0, 0]
          Max: [0, 0]
          Pivot: [0, 0]
          Offset: [0, 0]
          Size: [200, 60]
)YAML";

	// ---- 临时内容根:一个 `ui/HUD.wui`(`ResolveDocumentPath` 的兜底路径 = `<内容根>/ui/*.wui`)----

	class TempUiProject
	{
	public:
		explicit TempUiProject(const std::string& document)
		{
			std::error_code error;
			const unsigned long long unique = static_cast<unsigned long long>(
				std::chrono::steady_clock::now().time_since_epoch().count());
			m_Root = std::filesystem::temp_directory_path(error) /
				("we-ui-hotreload-" + std::to_string(unique));
			std::filesystem::create_directories(m_Root / "ui", error);
			Write(document, __LINE__);
		}

		~TempUiProject()
		{
			std::error_code error;
			std::filesystem::remove_all(m_Root, error);
		}

		TempUiProject(const TempUiProject&) = delete;
		TempUiProject& operator=(const TempUiProject&) = delete;

		const std::filesystem::path& Root() const { return m_Root; }
		std::filesystem::path Document() const { return m_Root / "ui" / "HUD.wui"; }

		// 写盘,并断言**文件戳真的变了**(大小必须不同):判据是 大小 ⊕ mtime,
		// 只写同尺寸内容会依赖文件系统时间戳粒度 ⇒ 这里从根上避免 flaky。
		void Write(const std::string& text, int line)
		{
			std::error_code error;
			const std::uintmax_t sizeBefore = std::filesystem::file_size(Document(), error);
			// error 非空 = 文件还不存在(首次写),sizeBefore 记 0,不影响下面的"必须变大"断言。
			{
				std::ofstream file(Document(), std::ios::binary | std::ios::trunc);
				file << text;
			}
			const std::uintmax_t sizeAfter = std::filesystem::file_size(Document(), error);
			Check(!error && sizeAfter == text.size() && sizeAfter != sizeBefore,
				"hot-reload fixture write must change the file stamp (size)", line);
		}

	private:
		std::filesystem::path m_Root;
	};

	// ---- 帧 / 输入工具(headless:WuiContext 不需要后端)----

	void DrawOneFrame(UiHost& host, Wui::WuiContext& ctx)
	{
		Wui::WuiInputState input;
		input.ViewportSize = glm::vec2 { kDesignW, kDesignH };
		ctx.BeginFrame(input);
		host.DrawFrame(ctx, input);
	}

	Wui::WuiInputState KeyInput(uint32_t key)
	{
		Wui::WuiInputState input;
		input.KeyDown.push_back(key);
		input.KeyPressed.push_back(key);
		return input;
	}

	Wui::WuiInputState WheelInput(glm::vec2 point, float delta)
	{
		Wui::WuiInputState input;
		input.MousePos = point;
		input.Wheel = delta;
		return input;
	}

	// `Commands()` 是非 const 访问器(它会按 overlay 深度选通道)⇒ 这里收非 const 引用。
	bool HasTextCommand(Wui::WuiContext& ctx, const std::string& text)
	{
		for (const Wui::WuiDrawCommand& command : ctx.Commands())
		{
			if (command.Text == text)
				return true;
		}
		return false;
	}

	std::string PropValue(const UiHost& host, std::string_view nodeId, std::string_view propName)
	{
		const UiNodeInstance* node = host.Screen().Find(nodeId);
		if (node == nullptr || node->Source == nullptr)
			return "<no node>";
		const UiProp* prop = node->Source->FindProp(propName);
		return prop != nullptr ? prop->Value : std::string("<no prop>");
	}

	// ---- ① 改盘 → 重载生效 ----

	void TestDiskChangeReloads()
	{
		TempUiProject project(kDocAlpha);
		UiHost host;
		host.Initialize(project.Root());
		CHECK(host.Enabled());                                       // 有 `.wui` 才启用
		CHECK_STR(host.LastReloadError(), "");
		CHECK(host.Screen().Count() == 4);
		CHECK_STR(PropValue(host, "label", "text"), "Alpha");

		Wui::WuiContext ctx;
		DrawOneFrame(host, ctx);

		// 改盘:文本变长 + 追加一个节点(大小与 mtime 都变)。
		project.Write(kDocBravo, __LINE__);
		host.PollDocumentChanges();

		CHECK_STR(host.LastReloadError(), "");                        // 成功无错误
		CHECK(host.Screen().Count() == 5);                            // 新节点进来了
		CHECK(host.Screen().Find("extra") != nullptr);
		CHECK_STR(PropValue(host, "label", "text"), "Bravo-Charlie");  // 节点文本变了

		// 画面同源:同一帧的绘制命令里能读到新文本(不只是文档字段变了)。
		DrawOneFrame(host, ctx);
		CHECK(HasTextCommand(ctx, "Bravo-Charlie"));
		CHECK(!HasTextCommand(ctx, "Alpha"));

		// 没有新变化时再轮询:不重复重载(戳未变 ⇒ 直接返回),状态保持一致。
		host.PollDocumentChanges();
		CHECK(host.Screen().Count() == 5);
		CHECK_STR(host.LastReloadError(), "");
		std::printf("  [ok] disk change -> reload (nodes 4 -> 5, text 'Alpha' -> 'Bravo-Charlie')\n");
	}

	// ---- ② 坏文件 → 保留上一份可用版本 ----

	void TestBadFileKeepsPreviousVersion()
	{
		TempUiProject project(kDocAlpha);
		UiHost host;
		host.Initialize(project.Root());
		CHECK(host.Enabled());

		Wui::WuiContext ctx;
		DrawOneFrame(host, ctx);
		const std::size_t nodesBefore = host.Screen().Count();
		CHECK(nodesBefore == 4);

		// (a) 解析失败:旧文档逐字段保留,错误可读。
		project.Write(kDocMalformed, __LINE__);
		host.PollDocumentChanges();
		CHECK(!host.LastReloadError().empty());
		std::printf("  [ok] malformed reload error: %s\n", host.LastReloadError().c_str());
		CHECK(host.Enabled());                                     // 不因坏文件关闭 UI
		CHECK(host.Screen().Count() == nodesBefore);               // 节点数不变 = 没有半加载界面
		CHECK_STR(PropValue(host, "label", "text"), "Alpha");      // 旧内容仍在

		// 同一份坏文件再轮询:不重试、不刷屏(结论与上次同一份内容)。
		const std::string malformedError = host.LastReloadError();
		host.PollDocumentChanges();
		CHECK_STR(host.LastReloadError(), malformedError);
		CHECK(host.Screen().Count() == nodesBefore);

		// (b) 语法没事、语义校验失败(版本门禁):同样保留旧文档。
		project.Write(kDocBadVersion, __LINE__);
		host.PollDocumentChanges();
		CHECK(!host.LastReloadError().empty());
		std::printf("  [ok] validation-gate reload error: %s\n", host.LastReloadError().c_str());
		CHECK(host.Screen().Count() == nodesBefore);
		CHECK_STR(PropValue(host, "label", "text"), "Alpha");
		CHECK(host.Screen().Find("label") != nullptr);             // 旧节点还在(不是被新文档替换)

		// (c) 写回好文件:失败不粘住,下一次变化照常重载。
		project.Write(kDocBravo, __LINE__);
		host.PollDocumentChanges();
		CHECK_STR(host.LastReloadError(), "");
		CHECK(host.Screen().Count() == nodesBefore + 1);
		CHECK_STR(PropValue(host, "label", "text"), "Bravo-Charlie");
		std::printf("  [ok] bad file kept the previous version; good file reloads again\n");
	}

	// ---- ③ 运行态按稳定 Id 迁移(滚动偏移 + 焦点)----

	void TestRuntimeStateMigratesById()
	{
		TempUiProject project(kDocAlpha);
		UiHost host;
		host.Initialize(project.Root());
		CHECK(host.Enabled());

		Wui::WuiContext ctx;
		DrawOneFrame(host, ctx);                                   // 先有布局,命中/滚动才有意义

		// 焦点:Tab 落在唯一可聚焦节点(绘制顺序里第一个可聚焦项)。
		host.RouteInput(KeyInput(World::Tab));
		CHECK_STR(host.InputRouter().FocusedId(), "action");

		// 滚动:滚轮向下两次(step 默认 40)⇒ 偏移 y = 80。
		host.RouteInput(WheelInput(glm::vec2 { 100.0f, 100.0f }, -1.0f));
		host.RouteInput(WheelInput(glm::vec2 { 100.0f, 100.0f }, -1.0f));
		const float scrolledBefore = host.Screen().ScrollOffset("root").y;
		CHECK(scrolledBefore > 0.0f);
		// 偏移确实作用到后代矩形(label 从 y=0 上移到 y=-80)。
		CHECK_NEAR(host.Screen().RectOf("label").Y, -scrolledBefore, 0.01f);

		// 改盘 + 重载(文本同时变 ⇒ 证明这份状态迁移发生在"新文档"上)。
		project.Write(kDocBravo, __LINE__);
		host.PollDocumentChanges();
		CHECK_STR(host.LastReloadError(), "");
		CHECK(host.Screen().Count() == 5);
		CHECK_STR(PropValue(host, "label", "text"), "Bravo-Charlie");

		// 按稳定 Id 迁移:滚动偏移与焦点都还在,且**没有叠加**(仍是 -80,不是 -160)。
		CHECK_NEAR(host.Screen().ScrollOffset("root").y, scrolledBefore, 0.01f);
		CHECK_NEAR(host.Screen().RectOf("label").Y, -scrolledBefore, 0.01f);
		CHECK_STR(host.InputRouter().FocusedId(), "action");

		// 再画一帧(新文档整体重排 + 偏移重放)后仍然一致 —— 迁移不是"只写在表里"。
		DrawOneFrame(host, ctx);
		CHECK_NEAR(host.Screen().ScrollOffset("root").y, scrolledBefore, 0.01f);
		CHECK_NEAR(host.Screen().RectOf("label").Y, -scrolledBefore, 0.01f);
		CHECK_STR(host.InputRouter().FocusedId(), "action");
		std::printf("  [ok] reload kept scroll offset %.1f and focus 'action' by stable Id\n",
			static_cast<double>(scrolledBefore));
	}

	// ---- ④ 零成本守卫 / 失败语义 ----

	void TestDisabledHostIsInert()
	{
		// 空内容根(没有 `.wui`):`Initialize` 静默关闭 ⇒ 轮询/重载/绘制都不做事。
		std::error_code error;
		const std::filesystem::path emptyRoot = std::filesystem::temp_directory_path(error) /
			("we-ui-hotreload-empty-" + std::to_string(static_cast<unsigned long long>(
				std::chrono::steady_clock::now().time_since_epoch().count())));
		std::filesystem::create_directories(emptyRoot, error);

		UiHost host;
		host.Initialize(emptyRoot);
		CHECK(!host.Enabled());
		CHECK(host.DocumentPath().empty());          // 没有路径 ⇒ 轮询连 stat 都不做
		host.PollDocumentChanges();                  // 必须零操作、不崩

		std::string reloadError;
		CHECK(!host.Reload(&reloadError));           // 没有文档 = 明确失败 + 可读原因
		CHECK(!reloadError.empty());
		std::printf("  [ok] disabled host: Reload() -> false ('%s')\n", reloadError.c_str());

		std::filesystem::remove_all(emptyRoot, error);
	}
}

int main()
{
#if defined(_WIN32)
	// 夹具走"内容根 `ui/*.wui`"这条发布路径:清掉开发开关,避免机器上恰好设了 WLD_UI_DOC
	// 时测试指向别的文件(空值 = 未设置,`ResolveDocumentPath` 会回退内容根)。
	_putenv_s("WLD_UI_DOC", "");
#endif

	try
	{
		TestDiskChangeReloads();
		TestBadFileKeepsPreviousVersion();
		TestRuntimeStateMigratesById();
		TestDisabledHostIsInert();
	}
	catch (const std::exception& e)
	{
		std::fprintf(stderr, "World.UiHotReload FAILED: %s\n", e.what());
		return 1;
	}
	std::printf("World.UiHotReload OK\n");
	return 0;
}
