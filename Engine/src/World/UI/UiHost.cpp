#include "wldpch.h"
#include "World/UI/UiHost.h"

#include "World/Core/Log.h"
#include "World/Core/Application.h"
#include "World/Core/Input.h"
#include "World/Core/Window.h"
#include "World/Gameplay/Runtime/GameApp.h"
#include "World/UI/UiDocument.h"
#include "World/UI/UiNodeRegistry.h"
#include "World/UI/UiPainter.h"
#include "World/UI/UiTypes.h"
#include "World/UI/UiWorldProjector.h"
#include "World/Utils/Paths.h"
#include "World/WUI/WuiAccessibility.h"
#include "World/WUI/WuiScriptedInput.h"

#include <GLFW/glfw3.h>

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <system_error>
#include <vector>

namespace World
{
	namespace
	{
		// ---- M38:加载期节点类型校验(宿主策略)----
		//
		// 契约要求"未知 Type = 可读错误"(`contract.ui-document-format` §6)。但 `ValidateUiDocument`
		// 是**文档层**校验,不该依赖节点注册表(那是 UI 运行时的宿主策略,不是文档格式合法性)。
		// 所以这条校验放在 `UiHost` 的加载路径上,`ValidateUiDocument` 的语义保持不变。
		//
		// 逐个节点的 `Type` 必须能被 `UI::UiNodeRegistry::Find` 命中(规范名 / 别名 / 组件 id /
		// 组件结构体名,见其 `MatchesName`)。有未命中 ⇒ **整份拒绝**(Initialize 不启用;
		// Reload 保留上一份可用版本)。错误列出**前 3 个**未登记的 Type 与它们的节点路径
		// (路径 = 文档遍历序的 "父.子" 稳定 Id 路径)。绘制期 `UiPainter` 的未知类型兜底仍保留
		// (双保险):这里拦的是"整份半可用",那里兜的是第三方运行期注册表被清空等极端情形。
		bool ValidateNodeTypes(const UI::UiDocument& document, std::string* error)
		{
			std::vector<std::pair<std::string, std::string>> unknown;   // {Type, 节点路径}
			document.ForEachNode([&unknown](const UI::UiNode& node, const std::string& path)
			{
				if (UI::UiNodeRegistry::Find(node.Type) == nullptr)
					unknown.emplace_back(node.Type, path);
			});
			if (unknown.empty())
				return true;

			if (error)
			{
				std::string message = "unknown UI node type(s) (not registered in UiNodeRegistry): ";
				const std::size_t shown = std::min<std::size_t>(unknown.size(), 3);
				for (std::size_t index = 0; index < shown; ++index)
				{
					if (index > 0)
						message += "; ";
					message += "'" + unknown[index].first + "' at '" + unknown[index].second + "'";
				}
				if (unknown.size() > shown)
					message += " (+" + std::to_string(unknown.size() - shown) + " more)";
				*error = std::move(message);
			}
			return false;
		}
	}

	std::filesystem::path UiHost::FirstUiDocument(const std::filesystem::path& candidate)
	{
		std::error_code error;
		if (std::filesystem::is_regular_file(candidate, error))
			return candidate.extension() == UI::kUiDocumentExtension ? candidate : std::filesystem::path {};
		if (!std::filesystem::is_directory(candidate, error))
			return {};

		// 目录取字典序第一个 `.wui`(确定性:同一目录内容不变则选中项不变)。
		std::vector<std::filesystem::path> documents;
		std::filesystem::directory_iterator iterator(candidate, error);
		const std::filesystem::directory_iterator end;
		for (; !error && iterator != end; iterator.increment(error))
		{
			const std::filesystem::directory_entry& entry = *iterator;
			std::error_code entryError;
			if (entry.path().extension() == UI::kUiDocumentExtension && entry.is_regular_file(entryError))
				documents.push_back(entry.path());
		}
		if (documents.empty())
			return {};
		std::sort(documents.begin(), documents.end());
		return documents.front();
	}

	std::filesystem::path UiHost::ResolveDocumentPath() const
	{
		// 1) 开发开关 WLD_UI_DOC:绝对路径,或相对**内容根**;指向目录时取其中第一个 `.wui`。
		if (const char* fromEnvironment = std::getenv("WLD_UI_DOC"))
		{
			if (fromEnvironment[0] != '\0')
			{
				std::filesystem::path candidate(fromEnvironment);
				if (candidate.is_relative())
					candidate = m_ContentRoot / candidate;
				if (const std::filesystem::path explicitDocument = FirstUiDocument(candidate);
					!explicitDocument.empty())
					return explicitDocument;
				// 显式开关指向但解析不到:一条可读警告(开发开关,不是发布路径的刷屏),再按优先级回退。
				WLD_CORE_WARN("[ui] WLD_UI_DOC='{0}' resolved to no .wui document ('{1}'); falling back to the content root",
					fromEnvironment, candidate.string());
			}
		}

		// 2) 内容根 `assets/ui/*.wui`(字典序第一个)。都不存在 = 返回空 = 静默关闭。
		return FirstUiDocument(m_ContentRoot / "ui");
	}

	void UiHost::Initialize(const std::filesystem::path& contentRoot)
	{
		Shutdown();
		m_ContentRoot = contentRoot.empty() ? Paths::AssetRoot() : contentRoot;

		// GameUI(M26):绑定解析器注册(宿主 Initialize 时一次;幂等)。`ecs:` 无外部依赖;
		// `service:` 的只读数据源 = 会话 GameApp —— 编辑器 Play 与 Runtime 都在本调用前建好会话
		// (`GameHost::Init` / `SetSceneState`),所以这里能拿到;拿不到也注册解析器,
		// 求值时给"需要活跃会话"的可读 error,而不是含混的"no resolver registered"。
		UI::RegisterBuiltinBindingSources(Gameplay::GameApp::TryGet());

		const std::filesystem::path documentPath = ResolveDocumentPath();
		if (documentPath.empty())
			return;   // 没有 `.wui` = 今天的行为:不加载、不开通道、不推命令。

		UI::UiDocument document;
		std::string error;
		if (!UI::UiDocumentIO::LoadFile(documentPath, document, &error))
		{
			WLD_CORE_WARN("[ui] game UI disabled: cannot load '{0}': {1}", documentPath.string(), error);
			return;
		}

		// M38:加载期节点类型校验(宿主策略;见 ValidateNodeTypes 注释)。未知 Type = 整份拒绝,
		// 不再"加载成功但少画东西"。校验在 `Build` 之前 ⇒ `m_Screen` 不会被半加载的文档污染。
		std::string typeError;
		if (!ValidateNodeTypes(document, &typeError))
		{
			WLD_CORE_WARN("[ui] game UI disabled: {0} (document '{1}')",
				typeError, documentPath.string());
			return;
		}

		// Build 会再跑一次 ValidateUiDocument,并在 `Source` 指针上持有本对象内的文档副本。
		std::string buildError;
		if (!m_Screen.Build(document, &buildError))
		{
			WLD_CORE_WARN("[ui] game UI disabled: {0} (document '{1}')",
				buildError, documentPath.string());
			return;
		}

		m_DocumentPath = documentPath.string();
		// GameUI(M26):把文档的全部 `Bind:` 编成扁平表(重复 Initialize = 从头重建)。
		// 只存节点稳定 Id + 目标属性 + 已解析源,不求值(DrawFrame 按 tick 求值)。
		m_Bindings.Attach(m_Screen);
		m_Overrides.Clear();
		m_BindingProblemReported = false;
		if (const char* dump = std::getenv("WLD_UI_A11Y_DUMP"))
			if (dump[0] != '\0')
				m_A11yDumpPath = dump;

		// 无障碍开关与加载条件一致:只有真的有 `.wui` 才(在自持通道口径下)开通道。
		// SharedChannel = 通道归宿主,这里不动开关(编辑器靠 --ai-control 自己开)。
		if (m_A11yMode == UiHostAccessibilityMode::OwnChannel)
			Wui::WuiAccessibility::Get().SetEnabled(true);
		m_Enabled = true;
		// M34:热重载的基线文件戳(首次登记不产生"变化")。
		m_DocumentStamp = ReadDocumentStamp(documentPath);
		m_LastReloadError.clear();

		WLD_CORE_INFO("[ui] game UI loaded from '{0}' (screen '{1}', {2} nodes, accessibility {3})",
			m_DocumentPath, m_Screen.Document().Screen, m_Screen.Count(),
			m_A11yMode == UiHostAccessibilityMode::OwnChannel ? "on" : "host-managed");

		// M39:开发开关注入页面栈(`WLD_UI_PAGE` / `WLD_UI_MODAL`;未设置 = 零操作)。
		SeedPagesFromEnvironment();
	}

	void UiHost::Shutdown()
	{
		if (m_Enabled)
		{
			if (m_A11yMode == UiHostAccessibilityMode::OwnChannel)
				Wui::WuiAccessibility::Get().SetEnabled(false);
			else
			{
				// 共享通道:只清掉**本片游戏 UI 的面板节点**,不关宿主的无障碍通道。
				// 不能用 `ClearWindow`:那会把同窗口("main")的编辑器节点一起清掉,退出 Play 后
				// `ui.tree` 会读到空树。面板 id 与 DrawFrame 登记时同一表达式(空 = 文档 Screen 名)。
				const std::string panel = m_PanelId.empty() ? m_Screen.Document().Screen : m_PanelId;
				Wui::WuiAccessibility::Get().ClearPanel(m_WindowKey, panel);
			}
		}
		// M39:页面栈随文档一起清(逐层 OnExit + 释放宿主自持页面屏幕;空栈 = 纯 no-op)。
		ClearPages();
		m_Screen = UI::UiScreen {};
		m_DocumentPath.clear();
		m_A11yDumpPath.clear();
		m_HasSurface = false;
		m_Origin = glm::vec2 { 0.0f, 0.0f };
		m_A11yDumpWritten = false;
		m_PaintProblemReported = false;
		m_WorldProblemReported = false;
		// M26:绑定表/覆盖表随文档一起清;运行时数据源失效(下次 Initialize 再 Attach)。
		m_Bindings.Reset();
		m_Overrides.Clear();
		m_BindingRuntime = UI::UiBindingContext {};
		m_BindingVersion = 0;
		m_HasBindingRuntime = false;
		m_BindingProblemReported = false;
		// M34:热重载基线随文档一起清(下次 Initialize 再建)。
		m_DocumentStamp = DocumentStamp {};
		m_LastReloadError.clear();
		m_Enabled = false;
	}

	void UiHost::SetSurface(const UI::UiSurface& surface)
	{
		m_Surface = surface;
		m_HasSurface = true;
	}

	void UiHost::SetOrigin(glm::vec2 origin)
	{
		m_Origin = origin;
	}

	void UiHost::ClearSurface()
	{
		m_HasSurface = false;
		m_Origin = glm::vec2 { 0.0f, 0.0f };
	}

	void UiHost::DrawFrame(Wui::WuiContext& ctx, const Wui::WuiInputState& input)
	{
		if (!m_Enabled)
			return;

		// 默认(Runtime):`input.ViewportSize` 已是 WUI 的设计单位视口(物理像素 / 平台内容缩放,
		// 见 WuiRhiBackend::BeginFrame);`.wui` 的物理面取它,DPI 系数填**真实平台内容缩放**
		// (GLFW `glfwGetWindowContentScale`,`UiHost::PlatformContentScale()`;无窗口 ⇒ 1.0)。
		// M9 已知交互(留给主 agent 复核):WUI 后端把设计单位命令放大到物理像素时还会再乘一次
		// `Wui::UiScale()`,故高内容缩放屏上 `viewport.Scale` 与 `UiScale()` 会叠加;本任务按
		// 派工单只做"填真实 DPI",不改 `UiScale()` 的既有语义,也不擅自用 PhysicalSize 抵消。
		// 宿主显式 SetSurface 时(编辑器 Play)改用它给的子矩形尺寸(同一坐标系单位)。
		UI::UiSurface surface;
		if (m_HasSurface)
			surface = m_Surface;
		else
		{
			surface.PhysicalSize = input.ViewportSize;
			surface.DpiScale = PlatformContentScale();
		}

		const UI::UiDocument& document = m_Screen.Document();
		UI::UiViewport viewport = UI::ComputeUiViewport(document.Design, document.SafeArea, surface);
		// 画进子矩形时,内容原点 = 子矩形左上角(默认 (0,0) ⇒ 与 Runtime 逐字节一致)。
		viewport.PhysicalOrigin = m_Origin;
		if (!m_Screen.Layout(viewport))
			return;

		// M8:世界空间锚点 —— 把带 `World` 的节点重新定位到"目标投影点"。
		// 纯屏幕空间的文档(没有 `World` 块)在这里是零成本:没有任何节点命中,结果全为零。
		if (m_WorldSpaceEnabled)
		{
			// 坐标口径(硬):`UiWorldCamera::ScreenSize` 必须是**渲染面物理像素**。
			// 世界锚点要落在与布局/命令/无障碍同一空间(即视口单位 = 物理/UiScale),
			// 所以这里唯一一次换算成视口单位;宿主**不要**自己先除。
			// (M15 实测:漏掉这一步 ⇒ 落点被内容缩放多乘一次,UiScale=1.30 时偏 222px。)
			UI::UiWorldCamera worldCamera = m_WorldCamera;
			const float uiScale = Wui::UiScale() > 0.0f ? Wui::UiScale() : 1.0f;
			worldCamera.ScreenSize /= uiScale;
			const UI::UiWorldResult world = UI::ApplyWorldAnchors(
				m_Screen, viewport, worldCamera, m_WorldResolver);
			if (!world.Warnings.empty() && !m_WorldProblemReported)
			{
				m_WorldProblemReported = true;
				WLD_CORE_WARN("[ui] world-space anchor: {0}", world.Warnings.front());
			}
		}

		// GameUI(M26):绑定求值 —— `Layout` 之后、`Paint` 之前。版本号 = 宿主喂的场景 tick
		// (同 tick 不重复求值);求值结果覆盖进运行态属性表,`UiPainter` 读取属性时优先取覆盖值。
		// 只影响绘制命令 / 无障碍文本,不改文档、不改布局(与 `UiBinding` 的只读边界一致)。
		if (m_Bindings.Count() > 0 && m_HasBindingRuntime)
		{
			m_Bindings.Refresh(UI::UiBindingDataSource { m_BindingVersion, &m_BindingRuntime });
			m_Overrides.Clear();
			for (const UI::UiBinding& binding : m_Bindings.Entries())
			{
				std::string value;
				if (m_Bindings.Value(binding.NodeId, binding.Target, value))
					m_Overrides.Set(binding.NodeId, binding.Target, std::move(value));
			}
			if (!m_Bindings.Warnings().empty() && !m_BindingProblemReported)
			{
				// 解析器缺失 / 实体或字段缺失:一条可读警告(只报一次,不每帧刷屏)。
				m_BindingProblemReported = true;
				const UI::UiBindingWarning& first = m_Bindings.Warnings().front();
				WLD_CORE_WARN("[ui] binding '{0}' (node '{1}', target '{2}') not resolved: {3}",
					first.Source, first.NodeId, first.Target, first.Message);
			}
		}

		// OwnChannel(Runtime):每帧重建本窗口的无障碍节点:绘制前清上一帧,绘制后节点与画面同源。
		// SharedChannel(编辑器):宿主的 shell 每帧已 BeginFrame("main"),这里**不能**再清,
		// 否则会抹掉同窗口的编辑器节点;只登记游戏 UI 节点即可。
		if (m_A11yMode == UiHostAccessibilityMode::OwnChannel)
			Wui::WuiAccessibility::Get().BeginFrame(m_WindowKey, viewport.PhysicalSize);

		UI::UiPaintOptions options;
		options.WindowKey = m_WindowKey;
		// 空 = 文档 Screen 名(与 Runtime 的既有结果一致);编辑器显式给 Screen 名,
		// 避免落到 shell 当前面板 id。
		options.PanelId = m_PanelId.empty() ? document.Screen : m_PanelId;
		options.RegisterAccessibility = true;
		// M26:绑定求值结果 ⇒ 绘制期覆盖(空表 = 无覆盖,行为与引入绑定前一致)。
		options.Overrides = &m_Overrides;

		// M39:页面栈非空 ⇒ 按层序绘制各页(Page 底→顶 < Modal < Overlay < Debug),并给每页填
		// `UiPaintOptions::Page`/`Layer`(无障碍按页/层归属);**没有页面时落到下面的单屏路径**,
		// 与引入导航前逐字节一致(同样的 `Layout`/`Paint` 调用序列、同样的选项)。
		const std::vector<UI::UiDrawItem> layers = m_Navigator.Layers();
		if (!layers.empty())
		{
			// 宿主自持的页面(开发开关注入)按同一视口布局;外部页面由压栈方布局
			// (`UiPage::Screen` 是 const,本类不改他方屏幕 —— 见头文件)。
			for (const HostedPage& hosted : m_Pages)
			{
				if (hosted.Screen != nullptr)
					hosted.Screen->Layout(viewport);
			}
			for (const UI::UiDrawItem& item : layers)
			{
				if (item.Page == nullptr || item.Page->Screen == nullptr)
					continue;   // 层表由 `UiNavigator` 生成 ⇒ 构造上不可达;防御:不崩
				UI::UiPaintOptions pageOptions = options;
				pageOptions.Page = item.Page->Name;
				pageOptions.Layer = static_cast<int>(item.Layer);
				const UI::UiPaintResult pageResult =
					UI::UiPainter::Paint(ctx, *item.Page->Screen, pageOptions);
				if (!pageResult.Ok() && !m_PaintProblemReported)
				{
					m_PaintProblemReported = true;
					const std::string first = pageResult.Errors.empty() ? std::string("(unknown)")
						: pageResult.Errors.front().Message;
					WLD_CORE_WARN("[ui] {0} node(s) skipped while painting page '{1}': {2}",
						pageResult.SkippedNodes, item.Page->Name, first);
				}
			}
			return;
		}

		const UI::UiPaintResult result = UI::UiPainter::Paint(ctx, m_Screen, options);
		if (!result.Ok() && !m_PaintProblemReported)
		{
			// 未知类型是可读错误(绝不静默画空气);只报一次,不每帧刷屏。
			m_PaintProblemReported = true;
			const std::string first = result.Errors.empty() ? std::string("(unknown)")
				: result.Errors.front().Message;
			WLD_CORE_WARN("[ui] {0} node(s) skipped while painting '{1}': {2}",
				result.SkippedNodes, m_DocumentPath, first);
		}
	}

	void UiHost::EndFrame()
	{
		if (!m_Enabled || m_A11yDumpWritten || m_A11yDumpPath.empty())
			return;

		// 只试一次:写盘失败也只留一条警告,不每帧打扰运行(验证脚本按"没有文件 = 失败"判定)。
		m_A11yDumpWritten = true;
		const std::string json = Wui::WuiAccessibility::Get().Serialize();
		std::ofstream file(m_A11yDumpPath, std::ios::binary | std::ios::trunc);
		if (!file)
		{
			WLD_CORE_WARN("[ui] failed to open accessibility dump '{0}'", m_A11yDumpPath);
			return;
		}
		file << json;
		if (!file.good())
		{
			WLD_CORE_WARN("[ui] failed to write accessibility dump '{0}'", m_A11yDumpPath);
			return;
		}
		WLD_CORE_INFO("[ui] accessibility dump written ({0} bytes): {1}", json.size(), m_A11yDumpPath);
	}

	// ---- M34:`.wui` 热重载 ----

	// 文件戳 = 大小 + 最后写入时间(与 `AssetHotReload` 读不到内容时的 size|mtime 兜底同口径)。
	// 读不到(不存在 / 无权限)= Exists false ⇒ 调用方**不判脏**:界面保留当前这一份,
	// 绝不因为"文件短暂读不到"把已经画着的界面变成空的。
	UiHost::DocumentStamp UiHost::ReadDocumentStamp(const std::filesystem::path& path)
	{
		std::error_code error;
		const uintmax_t size = std::filesystem::file_size(path, error);
		if (error)
			return DocumentStamp {};
		const std::filesystem::file_time_type writeTime = std::filesystem::last_write_time(path, error);
		if (error)
			return DocumentStamp {};

		DocumentStamp stamp;
		stamp.Exists = true;
		stamp.Size = size;
		stamp.Modified = static_cast<long long>(writeTime.time_since_epoch().count());
		return stamp;
	}

	bool UiHost::Reload(std::string* error)
	{
		const auto fail = [error](const std::string& message) {
			if (error)
				*error = message;
			return false;
		};

		if (!m_Enabled || m_DocumentPath.empty())
			return fail("no .wui document is loaded");

		const std::filesystem::path documentPath(m_DocumentPath);
		const DocumentStamp stampBefore = ReadDocumentStamp(documentPath);

		// 运行态迁移快照(稳定 Id → 值),必须在 `Build` **之前**取:
		//   * 滚动偏移:唯独存在 `UiScreen` 里(`Build` 会清空),按节点 Id 枚举非零项;
		//   * 焦点:router 跨帧持有(FocusedId + 焦点域),不在文档里。
		// 两者都只读:快照失败/为空 ⇒ 迁移就是空操作,不影响"重载本身要成功"这件事。
		std::vector<std::pair<std::string, glm::vec2>> scrollOffsets;
		for (const UI::UiNodeInstance& node : m_Screen.Nodes())
		{
			const glm::vec2 offset = m_Screen.ScrollOffset(node.Id);
			if (offset.x != 0.0f || offset.y != 0.0f)
				scrollOffsets.emplace_back(node.Id, offset);
		}
		const std::string focusedId = m_Router.FocusedId();
		const UI::UiFocusDomain focusDomain = m_Router.FocusDomain();

		// M39:页面栈迁移快照(按层序:页名 + 屏幕指针)。与上面两个快照一样必须在 `Build`
		// **之前**取。空 = 宿主只有一屏(没有 push 过)⇒ 迁移是 no-op(绝不把单屏变成空栈)。
		struct PageRecord
		{
			UI::UiLayer Layer = UI::UiLayer::Page;
			std::string Name;
			const UI::UiScreen* Screen = nullptr;
		};
		std::vector<PageRecord> recordedPages;
		for (int layer = 0; layer < UI::kUiLayerCount; ++layer)
		{
			const UI::UiLayer current = static_cast<UI::UiLayer>(layer);
			for (const UI::UiPage& page : m_Navigator.LayerPages(current))
				recordedPages.push_back(PageRecord { current, page.Name, page.Screen });
		}

		UI::UiDocument document;
		std::string loadError;
		if (!UI::UiDocumentIO::LoadFile(documentPath, document, &loadError))
		{
			// 文件读不动 / 解析失败:旧文档、旧运行态**全部保留**,只记一条可读原因。
			m_DocumentStamp = stampBefore;   // 这份内容已看过 ⇒ 不每帧重试刷屏
			m_LastReloadError = loadError.empty()
				? "cannot read '" + m_DocumentPath + "'" : loadError;
			return fail(m_LastReloadError);
		}

		// M38:加载期节点类型校验(宿主策略;见 ValidateNodeTypes 注释)。未知 Type = 整份拒绝,
		// 旧文档、旧运行态**全部保留**(与下面 `Build` 失败同一口径)。
		std::string typeError;
		if (!ValidateNodeTypes(document, &typeError))
		{
			m_DocumentStamp = stampBefore;
			m_LastReloadError = typeError;
			return fail(m_LastReloadError);
		}

		// `Build` 先跑 `ValidateUiDocument` 再改状态:失败返回 false 时 `m_Screen` 逐字段不变
		// ⇒ 天然"保留上一份可用版本",不存在半加载的实例树。
		std::string buildError;
		if (!m_Screen.Build(document, &buildError))
		{
			m_DocumentStamp = stampBefore;
			m_LastReloadError = buildError.empty() ? std::string("cannot build UI screen") : buildError;
			return fail(m_LastReloadError);
		}

		// 成功:以**读取之后**的戳作新基线(文件在读取期间又被写 ⇒ 下次轮询立刻再重载一次,幂等)。
		m_DocumentStamp = ReadDocumentStamp(documentPath);
		m_LastReloadError.clear();

		// 文档整体换了 ⇒ 绑定表按新文档重建(与 `Initialize` 同一入口);
		// 绘制/世界空间的"只报一次"标志复位(新文档可能带来新的未知类型或失效锚点)。
		m_Bindings.Attach(m_Screen);
		m_Overrides.Clear();
		m_BindingProblemReported = false;
		m_PaintProblemReported = false;
		m_WorldProblemReported = false;

		// 先按**上一次的视口**重排一次:节点矩形与滚动钳位都依赖布局结果,否则迁移落在
		// 全零矩形上(滚动偏移会被钳成 0)。下一帧 `DrawFrame` 会用当时的物理面重算视口并覆盖
		// 这里的结果 —— 这一步只是给"运行态迁移"一个正确的基准。
		if (m_Screen.Viewport().Scale > 0.0f)
			m_Screen.Layout(m_Screen.Viewport());

		// 运行态迁移:按稳定 `Id` 回填。`SetScrollOffset` 是唯一钳位实现(内容变短 ⇒ 自动收窄);
		// 新文档里没有的 Id 自然丢弃。焦点同样按 Id 恢复,节点消失 ⇒ 明确清空(不留悬空焦点)。
		for (const auto& [id, offset] : scrollOffsets)
			m_Screen.SetScrollOffset(id, offset);
		if (!focusedId.empty() && !m_Router.SetFocus(m_Screen, focusedId, focusDomain))
			m_Router.ClearFocus();

		// M39:宿主自持的页面文档**就地**重读(屏幕地址不变 ⇒ 导航器里的 `UiPage::Screen` 仍有效)。
		// 读不到 / 类型校验或构建失败 ⇒ 保留上一份内容(绝不半加载),只记一条可读警告;
		// 戳无条件写回(同一份坏页面文件不每帧重试,与主文档同一口径)。
		std::size_t reloadedPages = 0;
		for (HostedPage& hosted : m_Pages)
		{
			if (hosted.Screen == nullptr || hosted.DocumentPath.empty())
				continue;
			UI::UiDocument pageDocument;
			std::string pageError;
			const bool pageLoaded = UI::UiDocumentIO::LoadFile(hosted.DocumentPath, pageDocument, &pageError)
				&& ValidateNodeTypes(pageDocument, &pageError)
				&& hosted.Screen->Build(pageDocument, &pageError);
			hosted.Stamp = ReadDocumentStamp(hosted.DocumentPath);
			if (!pageLoaded)
			{
				WLD_CORE_WARN("[ui] page document '{0}' kept the previous version: {1}",
					hosted.DocumentPath.string(), pageError);
				continue;
			}
			++reloadedPages;
		}

		// M39:按记录顺序重建页面栈(层内顺序 = 记录顺序 ⇒ 栈深与顶层不变;页名按记录值恢复,
		// 身份不变)。页"内部运行态"(页面自己的滚动/焦点/编辑态)不迁移 —— 页面屏幕归压栈方,
		// 本类只恢复栈的形状与页名;重载会重放页面生命周期(Reset 的 OnExit → Push 的 OnEnter)。
		if (!recordedPages.empty())
		{
			m_Navigator.Reset();
			std::size_t restoredPages = 0;
			for (int layer = 0; layer < UI::kUiLayerCount; ++layer)
			{
				const UI::UiLayer current = static_cast<UI::UiLayer>(layer);
				for (const PageRecord& record : recordedPages)
				{
					if (record.Layer != current || record.Screen == nullptr)
						continue;
					const UI::UiPage page = UI::MakeUiPage(*record.Screen, record.Name);
					const bool pushed = current == UI::UiLayer::Page ? m_Navigator.Push(page)
						: current == UI::UiLayer::Modal ? m_Navigator.PushModal(page)
						: m_Navigator.PushLayer(current, page);
					if (pushed)
						++restoredPages;
				}
			}
			WLD_CORE_INFO("[ui] page stack migrated across reload: {0}/{1} page(s) restored, "
				"{2} page document(s) re-read, {3} modal(s), top '{4}'",
				restoredPages, recordedPages.size(), reloadedPages, m_Navigator.ModalCount(),
				m_Navigator.Top() != nullptr ? m_Navigator.Top()->Name : std::string("(none)"));
		}

		WLD_CORE_INFO("[ui] document reloaded: '{0}' (screen '{1}', {2} nodes, {3} scroll offset(s) kept, focus '{4}')",
			m_DocumentPath, m_Screen.Document().Screen, m_Screen.Count(), scrollOffsets.size(),
			m_Router.FocusedId());
		return true;
	}

	void UiHost::PollDocumentChanges()
	{
		// 零成本守卫:没有 `.wui` / 没有路径时**不 stat**(宿主每帧调,发布路径默认零开销)。
		if (!m_Enabled || m_DocumentPath.empty())
			return;

		// 判脏口径(逐文件戳 = 大小 ⊕ 最后写入时间;读不到 = 不判脏,保留当前界面等它回来):
		//   * 主文档(M34);
		//   * M39:宿主自持的**页面文档** —— 页面文档改了同样走 `Reload`(全量重读主文档 + 各页)。
		bool dirty = false;
		const DocumentStamp current = ReadDocumentStamp(std::filesystem::path(m_DocumentPath));
		if (current.Exists && !(current.Exists == m_DocumentStamp.Exists
			&& current.Size == m_DocumentStamp.Size && current.Modified == m_DocumentStamp.Modified))
			dirty = true;
		for (const HostedPage& hosted : m_Pages)
		{
			if (dirty)
				break;
			if (hosted.Screen == nullptr || hosted.DocumentPath.empty())
				continue;
			const DocumentStamp stamp = ReadDocumentStamp(hosted.DocumentPath);
			if (stamp.Exists && !(stamp.Exists == hosted.Stamp.Exists && stamp.Size == hosted.Stamp.Size
				&& stamp.Modified == hosted.Stamp.Modified))
				dirty = true;
		}
		if (!dirty)
			return;

		// 戳变 ⇒ 试着重载。失败保留上一份可用版本:`Reload` 已把戳记为"已看过",
		// 所以同一份坏文件只在这里报一次(写回文件再次变化才重试)。
		std::string error;
		if (!Reload(&error))
			WLD_CORE_WARN("[ui] document reload failed, keeping the previous version: {0}", error);
	}

	// ---- M9:平台输入组帧 + 输入路由 + DPI ----

	float UiHost::PlatformContentScale()
	{
		if (!Application::HasInstance())
			return 1.0f;   // headless(测试/工具):没有窗口,不做任何平台查询。
		void* nativeWindow = Application::Get().GetWindow().GetNativeWindow();
		if (nativeWindow == nullptr)
			return 1.0f;
		float xScale = 0.0f;
		float yScale = 0.0f;
		glfwGetWindowContentScale(static_cast<GLFWwindow*>(nativeWindow), &xScale, &yScale);
		// UI 用单一缩放系数:取 X 轴;异常配置下 Y 轴与 X 轴不一致时仍以 X 为准。
		(void)yScale;
		if (!(xScale > 0.0f))
			return 1.0f;
		return xScale;
	}

	Wui::WuiInputState UiPlatformInputSampler::Sample(glm::vec2 viewportSize, const std::string& windowKey,
		bool applyScriptedInput)
	{
		Wui::WuiInputState state;
		state.ViewportSize = viewportSize;
		// 无窗口宿主绝不轮询平台(与 GameHost::Tick 同一守卫):返回零状态、零边沿。
		if (!Application::HasInstance())
			return state;

		// 与 `WuiInputCollector::OnMouseMove` 同一换算:平台鼠标是物理像素,UI 布局是设计单位。
		const float scale = Wui::UiScale() > 0.0f ? Wui::UiScale() : 1.0f;
		const auto position = Input::GetMousePosition();
		state.MousePos = glm::vec2(position.first, position.second) / scale;

		for (int button = 0; button < 3; ++button)
		{
			const bool down = Input::IsMouseButtonPressed(button);
			state.MouseDown[button] = down;
			// 轮询没有"本帧新按下"的事件锁存 ⇒ 用跨帧比较取沿(口径与 collector 的并集项一致)。
			state.MouseClicked[button] = down && !m_PrevDown[button];
				m_PrevDown[button] = down;
		}

		// 滚轮:与本帧 UI 帧读的是同一份平台累积值(Application 在帧末 ResetScrollDelta)。
		state.Wheel = Input::GetScrollDelta().second;

		// 脚本/AI 注入(contract.ui-runtime §5):叠加在平台轮询**之上**(注入优先),位置与
		// 按钮边沿都由注入相位给出 —— `ui.invoke` / `WLD_UI_CLICK` 与真人鼠标走同一条输入路径。
		// 注入是破坏性的(推进相位)⇒ `applyScriptedInput` 为 false 的一方本帧不消费(见头注释);
		// 注入后的按钮状态回写 `m_PrevDown`,否则下一帧平台轮询会把"注入曾按下"误判成一次新按下。
		if (applyScriptedInput)
		{
			Wui::WuiScriptedInput::Get().Apply(windowKey, state);
			for (int button = 0; button < 3; ++button)
				m_PrevDown[button] = state.MouseDown[button];
		}
		return state;
	}

	bool UiHost::RouteInput(const Wui::WuiInputState& input)
	{
		if (!m_Enabled)
			return false;   // 没有 `.wui` = 零副作用:不推命令、不改焦点、不吞输入。

		// M39:页面栈非空 ⇒ 路由到导航器的**最高层页**(模态/覆盖/调试层打开时只路由最高层,
		// "模态外的指针不穿透"由 `UiInputRouter` 的 navigator 重载负责)。页面屏幕是 const ⇒ 该重载
		// 不落地滚动偏移(页面自己的滚动归压栈方);栈顶就是 `m_Screen` 时(项目把本对象的单屏压栈)
		// 走下面的可写路径,滚轮照旧落到 `UiScreen::SetScrollOffset`。
		// 没有页面 ⇒ 与引入导航前逐字节一致(命中用上一帧 `DrawFrame` 里 `Layout` 出来的矩形)。
		const UI::UiScreen* top = m_Navigator.TopInteractiveScreen();
		if (top != nullptr && top != &m_Screen)
		{
			m_Router.SetCommandQueue(&m_Commands);
			m_Router.Update(m_Navigator, input);
			// 与单屏路径同一口径:指针**或滚轮**被 UI 吃掉 ⇒ 玩法本帧不得再收到(contract §5)。
			const UI::UiInputFrame& pageFrame = m_Router.LastFrame();
			return pageFrame.PointerConsumed || pageFrame.WheelConsumed;
		}
		m_Router.Update(m_Screen, input, m_Commands);
		const UI::UiInputFrame& frame = m_Router.LastFrame();
		// 指针(含滚轮)被 UI 消费 ⇒ 玩法本帧不得再收到(contract.ui-runtime §5)。
		return frame.PointerConsumed || frame.WheelConsumed;
	}

	void UiHost::SetEditingText(std::string_view nodeId, std::string text)
	{
		// 编辑态/初值都在常驻 `m_Router` 里(不随帧重建),这里只转发。
		m_Router.SetEditingText(nodeId, std::move(text));
	}

	void UiHost::SetBindingRuntime(const UI::UiBindingContext& runtime, uint64_t version)
	{
		// 只记数据源与版本:真正的求值在 `DrawFrame` 的 Layout → Paint 之间(见那里的注释)。
		// `runtime` 里的场景 / 会话指针由宿主保证在本次绘制期间有效(引擎两个宿主每帧重喂)。
		m_BindingRuntime = runtime;
		m_BindingVersion = version;
		m_HasBindingRuntime = true;
	}

	// ---- M39:页面栈 / 模态 ----

	// 加载一份 `.wui` 并压入导航器的指定层:屏幕归本对象持有(地址稳定),页名取文档 `Screen`。
	bool UiHost::LoadPageDocument(UI::UiLayer layer, const std::filesystem::path& documentPath, std::string* error)
	{
		const auto fail = [error](const std::string& message) {
			if (error)
				*error = message;
			return false;
		};

		// 路径口径与 `Initialize` 的 `WLD_UI_DOC` 一致:绝对路径,或相对内容根;目录取第一个 `.wui`。
		std::filesystem::path candidate = documentPath;
		if (candidate.is_relative())
			candidate = m_ContentRoot / candidate;
		const std::filesystem::path resolved = FirstUiDocument(candidate);
		if (resolved.empty())
			return fail("no .wui document at '" + candidate.string() + "'");

		UI::UiDocument document;
		std::string loadError;
		if (!UI::UiDocumentIO::LoadFile(resolved, document, &loadError))
			return fail(loadError.empty() ? "cannot read '" + resolved.string() + "'" : loadError);
		// 与主文档同一门禁(M38):未知 Type = 整份拒绝,不半加载。
		if (!ValidateNodeTypes(document, &loadError))
			return fail(loadError);

		auto screen = std::make_unique<UI::UiScreen>();
		std::string buildError;
		if (!screen->Build(document, &buildError))
			return fail(buildError.empty() ? std::string("cannot build UI screen") : buildError);

		const UI::UiPage page = UI::MakeUiPage(*screen);
		const bool pushed = layer == UI::UiLayer::Page ? m_Navigator.Push(page)
			: layer == UI::UiLayer::Modal ? m_Navigator.PushModal(page)
			: m_Navigator.PushLayer(layer, page);
		if (!pushed)
			return fail("navigator rejected the page");
		// 先压栈再搬进 `m_Pages`:`UiScreen` 是堆上独立对象 ⇒ 地址不变,`UiPage::Screen` 保持有效。
		m_Pages.push_back(HostedPage { layer, resolved, ReadDocumentStamp(resolved), std::move(screen) });
		WLD_CORE_INFO("[ui] page loaded from '{0}' (screen '{1}', layer {2}, {3} nodes)",
			resolved.string(), page.Name, UI::UiLayerName(layer), m_Pages.back().Screen->Count());
		return true;
	}

	void UiHost::SeedPagesFromEnvironment()
	{
		if (!m_Enabled)
			return;   // 没有主文档 = 整个 UI 关闭(与 `DrawFrame` / `RouteInput` 同一守卫)

		const auto seed = [this](const char* variable, UI::UiLayer layer) {
			const char* raw = std::getenv(variable);
			if (raw == nullptr || raw[0] == '\0')
				return;
			const std::string value(raw);
			std::size_t start = 0;
			for (;;)
			{
				const std::size_t separator = value.find(';', start);
				const std::string item = separator == std::string::npos
					? value.substr(start) : value.substr(start, separator - start);
				if (!item.empty())
				{
					std::string error;
					if (!LoadPageDocument(layer, item, &error))
						WLD_CORE_WARN("[ui] {0}='{1}' ignored: {2}", variable, item, error);
				}
				if (separator == std::string::npos)
					break;
				start = separator + 1;
			}
		};
		seed("WLD_UI_PAGE", UI::UiLayer::Page);
		seed("WLD_UI_MODAL", UI::UiLayer::Modal);
	}

	void UiHost::ClearPages()
	{
		// 逐层 OnExit(顶→底)后清空;再释放宿主自持的页面屏幕。空栈 = 纯 no-op。
		m_Navigator.Reset();
		m_Pages.clear();
	}
}
