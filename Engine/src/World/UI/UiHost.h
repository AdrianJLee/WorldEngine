#pragma once

// 游戏 UI 框架(GameUI)— 宿主无关的 `.wui` 运行时接线(工作包 M4,提升到引擎)。
//
// 职责(一个小类,让宿主 UI 帧只调两三行):
//   ① 加载:环境变量 `WLD_UI_DOC`(绝对路径或内容根相对)→ 内容根 `assets/ui/*.wui`
//      (字典序第一个);都不存在 = 静默关闭(发布路径默认零开销);找到但解析 / 构建失败
//      = 一条可读警告,不崩。M38 起还做**加载期节点类型校验**:任一 `Type` 不在
//      `UiNodeRegistry` ⇒ 整份拒绝(Initialize 不启用 / Reload 保留上一份可用版本),
//      错误列出前 3 个未登记的 Type 与节点路径 —— 见 `UiHost.cpp` 的 ValidateNodeTypes。
//   ② 每帧:组 `UiSurface` → `ComputeUiViewport` → `UiScreen::Layout` → `UiPainter::Paint`。
//      Runtime 口径 = 整窗(`input.ViewportSize`)+ 原点 (0,0);宿主也可用
//      `SetSurface` / `SetOrigin` 把 UI 画进物理面的**子矩形**(编辑器 Play 用它把游戏 UI
//      落进视口面板的场景矩形)。
//   ③ 无障碍:两种接入方式(见 `UiHostAccessibilityMode`)——
//      * `OwnChannel`(默认,Runtime):UiHost 开/关通道,DrawFrame 前 `BeginFrame(窗口 key)`;
//      * `SharedChannel`(编辑器 Play):通道与每帧 `BeginFrame("main")` 由宿主负责,
//        本类只登记节点(退出 Play 时 `Shutdown` 只清本窗口登记,不关通道)。
//      `WLD_UI_A11Y_DUMP` 是只读验证钩子(第一帧 `EndFrame` 之后写一次树;仅 `OwnChannel` 宿主会调)。
//   ④ 页面栈 / 模态(M39):持有 `UiNavigator`,按层序绘制各层页面;**没有页面时仍只画 `m_Screen`**
//      (与引入导航前逐字节一致);`.wui` 热重载迁移页面栈。入口见 `Navigator()` 与 .cpp 的开发开关
//      `WLD_UI_PAGE` / `WLD_UI_MODAL`(宿主自持页面屏幕:每帧布局、重载就地重读)。
//
// 边界:不改 `Engine/**` 之外的东西;`.wui` 未启用时 `DrawFrame` / `EndFrame` 是纯空操作,
// 宿主行为与引入本类之前逐字节一致。

#include "World/Core/Export.h"
#include "World/UI/UiInputRouter.h"
#include "World/UI/UiBinding.h"
#include "World/UI/UiBindingSources.h"
#include "World/UI/UiNavigator.h"
#include "World/UI/UiPainter.h"
#include "World/UI/UiScreen.h"
#include "World/UI/UiTypes.h"
#include "World/UI/UiWorldProjector.h"
#include "World/WUI/WuiContext.h"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <glm/glm.hpp>

namespace World
{
	// 谁拥有 `WuiAccessibility` 通道 / 谁负责每帧重置本窗口的节点。
	enum class UiHostAccessibilityMode : uint8_t
	{
		// 默认(Runtime M4 口径):UiHost 拥有无障碍通道 —— Initialize 时 SetEnabled(true),
		// DrawFrame 每帧 BeginFrame(窗口 key) 重置本窗口节点,Shutdown 时 SetEnabled(false)。
		OwnChannel = 0,
		// 共享通道(编辑器 Play/Simulate):通道由宿主(编辑器 AI 控制)管理,编辑器 shell 每帧
		// 已经 `BeginFrame("main", …)`。UiHost 不改开关、不重置窗口,只把游戏 UI 节点登记进
		// 同一份树(window 键仍由 `m_WindowKey` 决定);Shutdown 只 `ClearPanel(窗口 key, 本片面板)`
		// 清掉本片登记,不碰同窗口的编辑器节点(M9)。
		SharedChannel,
	};

	// GameUI(M9):宿主在 `OnUpdate`(玩法 `Tick` 之前)组一帧**最小** `WuiInputState` 的采样器。
	//
	// 为什么需要它:UI 帧拿到的是"事件锁存"后的输入(`WuiInputCollector`),而宿主 `OnUpdate`
	// 只有 GLFW 轮询(按下/抬起/滚轮累积、鼠标位置)。轮询没有"本帧新按下"的沿,所以这里跨帧
	// 记住上一帧按键状态,产出 `MouseClicked`(口径与 `WuiInputCollector::BeginFrame` 一致:
	// 两处读的是同一帧的同一平台状态,只是这里在 UI 帧**之前**读)。
	//
	// 坐标:平台鼠标是物理像素,而 UI 布局是设计单位 ⇒ 与 `WuiInputCollector::OnMouseMove`
	// 一样除以 `Wui::UiScale()`。无窗口(headless)⇒ 返回零状态、零边沿(绝不轮询不存在的窗口)。
	//
	// **脚本/AI 注入(contract.ui-runtime §5)**:采样末尾按 `windowKey` 叠加 `WuiScriptedInput`
	// —— `ui.invoke` / `WLD_UI_CLICK` 走的因此是"和真人鼠标同一条输入路径",声明式游戏 UI
	// 能被 AI 真的点到(不是测试分支)。注入是**破坏性**的(推进注入相位),所以同一帧里
	// **只能有一个消费者**:宿主在游戏 UI 路由的那一帧把 `ApplyScriptedInput` 传 true,
	// WUI 面板帧就不再消费同一份注入(见 RuntimeLayer / EditorLayer 的 gate;游戏 UI 没启用
	// / 不在 Play 时行为与引入本参数前逐字节一致)。
	class WLD_API UiPlatformInputSampler
	{
	public:
		Wui::WuiInputState Sample(glm::vec2 viewportSize, const std::string& windowKey = "main",
			bool applyScriptedInput = true);

	private:
		bool m_PrevDown[3] = { false, false, false };
	};

	class WLD_API UiHost
	{
	public:
		// `UiHost` 自持**自引用**状态:导航器里的 `UiPage::Screen` 指向本对象的 `m_Screen` 与
		// `m_Pages[].Screen`。拷贝/搬移会让这些指针悬空或错位 ⇒ 明确不可拷贝、不可搬移
		// (宿主一律按成员持有;需要转移时转移持有者本身,例如 `std::unique_ptr<UiHost>`)。
		UiHost() = default;
		UiHost(const UiHost&) = delete;
		UiHost& operator=(const UiHost&) = delete;
		UiHost(UiHost&&) = delete;
		UiHost& operator=(UiHost&&) = delete;

		// 解析并加载游戏 UI 文档。contentRoot 为空时回退 `World::Paths::AssetRoot()`。
		void Initialize(const std::filesystem::path& contentRoot);
		// 停止绘制并释放本对象持有的 UI 文档(幂等)。
		//   OwnChannel  :关闭无障碍通道(SetEnabled(false))。
		//   SharedChannel:只 ClearWindow(窗口 key) 清掉本片登记,不关宿主通道。
		void Shutdown();

		bool Enabled() const { return m_Enabled; }
		const std::string& DocumentPath() const { return m_DocumentPath; }

		// ---- 页面栈 / 模态(M39)----
		//
		// 宿主持有导航器(四层:Page < Modal < Overlay < Debug):项目代码经本访问器 Push/Pop/查询;
		// `DrawFrame` 按层序绘制各层页面(**没有页面时仍只画 `m_Screen`**,与引入导航前逐字节一致);
		// `Reload` 迁移页面栈。两个宿主把 `&Navigator()` 传给 `UiCommandRouter::Dispatch`
		// (`ui.close` = 有模态先关模态、否则出栈;`ui.back` = `UiNavigator::Back()` 同一口径)。
		//
		// 页面屏幕归压栈方持有(`UiPage::Screen` 是 const,本类不改他方屏幕):压栈方负责 `Build`
		// 与按视口 `Layout`(`ComputeUiViewport(document.Design, document.SafeArea, surface)`,
		// surface 口径与 `SetSurface` 相同);本类只按层序绘制 + 登记无障碍。
		// 宿主自持的页面(开发开关 `WLD_UI_PAGE` / `WLD_UI_MODAL`)由本类每帧布局、重载时就地重读。
		// 已知未接(见 M39 报告):页面的世界锚点与 `Bind:` 求值仍只作用于 `m_Screen`。
		UI::UiNavigator& Navigator() { return m_Navigator; }
		const UI::UiNavigator& Navigator() const { return m_Navigator; }

		// 无障碍窗口键(默认 `main`)。宿主把它交给 `UiPlatformInputSampler` ⇒ 脚本/AI 注入
		// (`ui.invoke` / `WLD_UI_CLICK`)落在**同一个窗口**上,不靠调用点各自硬编码字符串。
		const std::string& WindowKey() const { return m_WindowKey; }

		// ---- 无障碍接入方式(持久设置,Shutdown 不改;默认 OwnChannel)----
		void SetAccessibilityMode(UiHostAccessibilityMode mode) { m_A11yMode = mode; }
		UiHostAccessibilityMode AccessibilityMode() const { return m_A11yMode; }
		// 无障碍节点归属面板 id;空 = 用文档 `Screen` 名(Runtime 的既有口径)。
		void SetAccessibilityPanel(std::string panelId) { m_PanelId = std::move(panelId); }

		// ---- 绘制位置(默认 = 整面 `input.ViewportSize`、原点 (0,0),即 Runtime 口径)----
		// 显式指定物理面(尺寸 + DPI)。宿主用它把 UI 落进一个子矩形。
		void SetSurface(const UI::UiSurface& surface);
		// 物理面左上角在宿主坐标系中的位置;填进 `UiViewport::PhysicalOrigin`。
		void SetOrigin(glm::vec2 origin);
		// 回到"整面 = input.ViewportSize、原点 (0,0)"的 Runtime 口径。
		void ClearSurface();

		// ---- 世界空间 UI(M8;默认关闭)----
		// 开启后,`Layout` 之后会对带 `World` 锚点的节点调 `ApplyWorldAnchors`:
		// 位置 = 目标世界坐标经 `camera` 投影到屏幕,再按节点 Pivot 摆放。纯屏幕空间的节点不受影响。
		// 相机每帧喂(`SetWorldCamera`),目标位置由宿主经 `SetWorldPositionResolver` 提供。
		void SetWorldSpace(bool enabled) { m_WorldSpaceEnabled = enabled; }
		bool WorldSpaceEnabled() const { return m_WorldSpaceEnabled; }
		void SetWorldCamera(const UI::UiWorldCamera& camera) { m_WorldCamera = camera; }
		void SetWorldPositionResolver(UI::UiWorldPositionResolver resolver)
		{
			m_WorldResolver = std::move(resolver);
		}

		// 在宿主 `ctx.BeginFrame(input)` 之后、宿主覆盖层之前调用;未启用时零操作。
		void DrawFrame(Wui::WuiContext& ctx, const Wui::WuiInputState& input);
		// 在 `ctx.EndFrame()` 之后调用:第一帧落一次无障碍树(`WLD_UI_A11Y_DUMP`);未启用时零操作。
		void EndFrame();

		// ---- 热重载(M34)----
		//
		// 契约 `contract.ui-runtime` §9:`.wui` 变更 → 重载 → 按稳定 `Id` **迁移运行态**
		// (栈/焦点/滚动/动画);失败保留上一份可用版本并报可读原因,**不允许半加载界面**。
		// 本类此前只在 `Initialize` 读一次盘(改文件画面不变 ⇒ 实测报障),这两个入口补上。
		//
		// `Reload` 重新读 `m_DocumentPath` → `UiScreen::Build`;`m_Surface` / `m_Origin` /
		// 世界空间设置不变。解析或构建失败 ⇒ 返回 false + 可读 error,且**不动**现有界面
		// (`Build` 先校验再改状态,失败时实例树/文档/运行态逐字段保留)。
		// 成功时按稳定 `Id` 迁移运行态:滚动偏移(`UiScreen::SetScrollOffset` 是唯一钳位实现)
		// 与 router 焦点(`FocusedId` + 焦点域);`Id` 在新文档里消失的项自然丢弃。
		bool Reload(std::string* error = nullptr);
		// 按**文件戳**(大小 + 最后写入时间,与 `AssetHotReload` 的 size|mtime 兜底同口径)
		// 判脏:变了就 `Reload`。宿主每帧调一次;`m_Enabled == false` 或路径为空时零成本
		// (**不 stat**)。同一份坏文件只报一次警告(失败也把戳记为已见,文件再次变化才重试)。
		void PollDocumentChanges();
		// 最近一次热重载失败的**可读**原因(成功 / 尚未失败 = 空;诊断与 headless 验证用)。
		const std::string& LastReloadError() const { return m_LastReloadError; }

		// 只读观察口(headless 验证/诊断:实例树、滚动偏移、焦点)。不改任何运行态。
		const UI::UiScreen& Screen() const { return m_Screen; }
		const UI::UiInputRouter& InputRouter() const { return m_Router; }

		// ---- 输入路由(M9;宿主在 `OnUpdate`、`m_Host.Tick(...)` 之前调用)----
		// 命中可交互节点 ⇒ true(宿主据此 `GameHost::SetPointerCaptured(true)`:玩法本帧收不到指针);
		// 空白 / 不可见 / 禁用 / 未启用(没有 `.wui`)⇒ false,且**零副作用**(不推命令、不改焦点)。
		// 命中用的是**上一帧 UI 帧**已经 `Layout` 过的矩形 —— UiScreen 的矩形在 `DrawFrame` 里由
		// `Layout` 写入,而宿主每帧顺序是 RouteInput → Tick → DrawFrame,因此本帧路由读到的是上一帧
		// 的布局。这是"UI 帧之后才有布局"的现实口径,不是缺陷:首帧没有布局 ⇒ 不命中、不吞输入。
		// 返回值 = UI 是否消费了指针(**含滚轮**:滚轮被滚动容器吃掉时同样为 true)。
		bool RouteInput(const Wui::WuiInputState& input);
		// 本帧 UI 命令(router 在 `RouteInput` 里 append)。宿主处理完自行 `ClearCommands()`。
		const UI::UiCommandQueue& Commands() const { return m_Commands; }
		void ClearCommands() { m_Commands.Clear(); }

		// ---- 绑定运行时(M26)----
		//
		// 宿主每帧在 `DrawFrame` 之前喂一次:`runtime.Scene` = 当前活动场景(Play/运行态场景,
		// 不是编辑文档),`version` 通常 = `Scene::CurrentWorldTick()`。`DrawFrame` 在 `Layout`
		// 之后、`Paint` 之前按该版本求值文档的全部 `Bind:` 条目(同 version 不重复求值),
		// 求值结果覆盖进运行态属性表再画 —— **不改** `.wui` 文档、不改布局口径。
		// 未调用 / `.wui` 未启用 ⇒ 不求值,绘制与引入绑定前逐字节一致。
		void SetBindingRuntime(const UI::UiBindingContext& runtime, uint64_t version);
		// 文档绑定表(`Bind:` 条目 + 最近一次求值结果;诊断/测试用)。
		const UI::UiBindingTable& Bindings() const { return m_Bindings; }
		// 本帧运行态属性覆盖表(绑定求值结果 → 绘制覆盖;诊断/测试用)。
		const UI::UiPropertyOverrideTable& PropertyOverrides() const { return m_Overrides; }

		// M13:宿主预置文本框初值(如从绑定来的当前值)。转发给常驻的 `UiInputRouter`
		// (`m_Router` 跨帧持有编辑态);编辑中调用立即替换编辑缓冲,否则作为下次进入编辑的初值。
		void SetEditingText(std::string_view nodeId, std::string text);

		// 平台内容缩放(GLFW `glfwGetWindowContentScale` 取 X 轴);无窗口 / 取值失败 ⇒ 1.0。
		// M9:填进 `UiSurface.DpiScale`(Runtime 由 `DrawFrame` 的默认面填,编辑器 Play 显式填)。
		static float PlatformContentScale();

	private:
		std::filesystem::path ResolveDocumentPath() const;
		// candidate 既可为 `.wui` 文件,也可为目录(取其中字典序第一个 `.wui`);无 → 空路径。
		static std::filesystem::path FirstUiDocument(const std::filesystem::path& candidate);

		// M34:热重载判脏用的**文件戳**(大小 + 最后写入时间;读不到 = Exists false = 不判脏)。
		struct DocumentStamp
		{
			bool Exists = false;
			uintmax_t Size = 0;
			long long Modified = 0;
		};
		static DocumentStamp ReadDocumentStamp(const std::filesystem::path& path);

		// ---- M39:页面栈 / 模态 ----
		//
		// 宿主自持的一页(开发开关注入的 `.wui`)。屏幕归本对象持有 ⇒ 地址稳定,
		// `Reload` 可就地重读文档,导航器里的 `UiPage::Screen` 保持有效。
		struct HostedPage
		{
			UI::UiLayer Layer = UI::UiLayer::Page;
			std::filesystem::path DocumentPath;
			DocumentStamp Stamp;                  // 页面文档判脏戳(与主文档同一口径)
			std::unique_ptr<UI::UiScreen> Screen;

			// 屏幕归本记录**独占**(唯一所有权 = 地址稳定,导航器里的 `UiPage::Screen` 才不会悬空)
			// ⇒ 只可搬移、不可拷贝。显式声明:否则编译器会按成员隐式删除拷贝赋值,
			// 报错点落在 `std::vector` 深处,看不出真正的原因。
			HostedPage() = default;
			HostedPage(const HostedPage&) = delete;
			HostedPage& operator=(const HostedPage&) = delete;
			HostedPage(HostedPage&&) = default;
			HostedPage& operator=(HostedPage&&) = default;
		};
		// 加载一份 `.wui` 并压入导航器的指定层。路径口径同 `Initialize` 的 `WLD_UI_DOC`:
		// 绝对路径,或相对内容根(指向目录时取其中字典序第一个 `.wui`)。失败 = false + 可读 error,栈不变。
		bool LoadPageDocument(UI::UiLayer layer, const std::filesystem::path& documentPath, std::string* error);
		// 开发开关(与 `WLD_UI_DOC` 同族):`WLD_UI_PAGE` = 分号分隔的 `.wui` 依次压页面栈,
		// `WLD_UI_MODAL` = 压模态层。用途 = 让页面栈在 Runtime / 编辑器 Play 里真的可跑
		// (实机验证 "ui.close 先关模态、再出栈");未设置 = 零操作,失败只一条可读警告。
		void SeedPagesFromEnvironment();
		// 清空导航器各层(逐层 OnExit)+ 释放宿主自持页面屏幕(`Shutdown` 与重新 `Initialize`)。
		void ClearPages();

		UI::UiScreen m_Screen;
		// M39:导航器(页面栈/模态/覆盖/调试)+ 宿主自持页面屏幕(开发开关注入)。
		UI::UiNavigator m_Navigator;
		std::vector<HostedPage> m_Pages;
		std::filesystem::path m_ContentRoot;
		std::string m_DocumentPath;
		std::string m_A11yDumpPath;
		std::string m_WindowKey = "main";
		// 无障碍节点归属面板覆盖;空 = 文档 Screen 名。
		std::string m_PanelId;
		// 显式物理面(编辑器 Play);未设置 = 沿用 input.ViewportSize。
		UI::UiSurface m_Surface;
		glm::vec2 m_Origin { 0.0f, 0.0f };
		bool m_HasSurface = false;
		UiHostAccessibilityMode m_A11yMode = UiHostAccessibilityMode::OwnChannel;
		bool m_WorldSpaceEnabled = false;
		UI::UiWorldCamera m_WorldCamera;
		UI::UiWorldPositionResolver m_WorldResolver;
		bool m_Enabled = false;
		bool m_A11yDumpWritten = false;
		// 绘制期未知类型 / 提示只报一次,避免每帧刷屏。
		bool m_PaintProblemReported = false;
		// 世界空间锚点解析失败只报一次(目标缺失是每帧都会发生的事)。
		bool m_WorldProblemReported = false;
		// M9:输入路由(焦点状态跨帧持有)+ 本帧命令队列(宿主每帧 ClearCommands)。
		UI::UiInputRouter m_Router;
		UI::UiCommandQueue m_Commands;
		// M26:文档绑定表(Initialize 时 Attach)+ 运行态覆盖表(每帧 Refresh 后重建)
		// + 宿主每帧喂的运行时数据源与变更检测版本。
		UI::UiBindingTable m_Bindings;
		UI::UiPropertyOverrideTable m_Overrides;
		UI::UiBindingContext m_BindingRuntime;
		uint64_t m_BindingVersion = 0;
		bool m_HasBindingRuntime = false;
		// 绑定求值失败只报一次(避免每帧刷屏;Reset 时复位)。
		bool m_BindingProblemReported = false;
		// M34:最近一次看到并已处理的文件戳(失败也写回 ⇒ 同一份坏文件不每帧重试/刷屏)
		// + 最近一次重载失败的可读原因。
		DocumentStamp m_DocumentStamp;
		std::string m_LastReloadError;
	};
}
