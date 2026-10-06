#include "wldpch.h"
#include "World/UI/UiHost.h"

#include "World/Core/Log.h"
#include "World/Core/Application.h"
#include "World/Core/Input.h"
#include "World/Core/Window.h"
#include "World/UI/UiDocument.h"
#include "World/UI/UiPainter.h"
#include "World/UI/UiTypes.h"
#include "World/UI/UiWorldProjector.h"
#include "World/Utils/Paths.h"
#include "World/WUI/WuiAccessibility.h"

#include <GLFW/glfw3.h>

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <system_error>
#include <vector>

namespace World
{
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

		// Build 会再跑一次 ValidateUiDocument,并在 `Source` 指针上持有本对象内的文档副本。
		std::string buildError;
		if (!m_Screen.Build(document, &buildError))
		{
			WLD_CORE_WARN("[ui] game UI disabled: {0} (document '{1}')",
				buildError, documentPath.string());
			return;
		}

		m_DocumentPath = documentPath.string();
		if (const char* dump = std::getenv("WLD_UI_A11Y_DUMP"))
			if (dump[0] != '\0')
				m_A11yDumpPath = dump;

		// 无障碍开关与加载条件一致:只有真的有 `.wui` 才(在自持通道口径下)开通道。
		// SharedChannel = 通道归宿主,这里不动开关(编辑器靠 --ai-control 自己开)。
		if (m_A11yMode == UiHostAccessibilityMode::OwnChannel)
			Wui::WuiAccessibility::Get().SetEnabled(true);
		m_Enabled = true;

		WLD_CORE_INFO("[ui] game UI loaded from '{0}' (screen '{1}', {2} nodes, accessibility {3})",
			m_DocumentPath, m_Screen.Document().Screen, m_Screen.Count(),
			m_A11yMode == UiHostAccessibilityMode::OwnChannel ? "on" : "host-managed");
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
		m_Screen = UI::UiScreen {};
		m_DocumentPath.clear();
		m_A11yDumpPath.clear();
		m_HasSurface = false;
		m_Origin = glm::vec2 { 0.0f, 0.0f };
		m_A11yDumpWritten = false;
		m_PaintProblemReported = false;
		m_WorldProblemReported = false;
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
			// M15 修正:宿主喂的相机是**渲染面物理像素**口径,而世界锚点求出的必须是
			// WUI 的**视口坐标**(后端绘制时再乘 `Wui::UiScale()` 才是物理像素)。
			// 少这一步换算,世界锚点会被内容缩放多放大一次
			// (实测 UiScale=1.30 ⇒ 落点偏 1.30 倍,222px)。
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

	Wui::WuiInputState UiPlatformInputSampler::Sample(glm::vec2 viewportSize)
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
		return state;
	}

	bool UiHost::RouteInput(const Wui::WuiInputState& input)
	{
		if (!m_Enabled)
			return false;   // 没有 `.wui` = 零副作用:不推命令、不改焦点、不吞输入。

		// 命中用上一帧 `DrawFrame` 里 `Layout` 出来的矩形(见头文件说明)。
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
}
