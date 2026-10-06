#include "UiHost.h"

#include "World/Core/Log.h"
#include "World/UI/UiDocument.h"
#include "World/UI/UiPainter.h"
#include "World/UI/UiTypes.h"
#include "World/Utils/Paths.h"
#include "World/WUI/WuiAccessibility.h"

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

		// 无障碍开关与加载条件一致:只有真的有 `.wui` 才开通道(没有 = 零开销,
		// 且 HUD / 脚本 UI 的登记行为与引入本类之前逐字节一致)。
		Wui::WuiAccessibility::Get().SetEnabled(true);
		m_Enabled = true;

		WLD_CORE_INFO("[ui] game UI loaded from '{0}' (screen '{1}', {2} nodes, accessibility on)",
			m_DocumentPath, m_Screen.Document().Screen, m_Screen.Count());
	}

	void UiHost::Shutdown()
	{
		if (m_Enabled)
			Wui::WuiAccessibility::Get().SetEnabled(false);
		m_Screen = UI::UiScreen {};
		m_DocumentPath.clear();
		m_A11yDumpPath.clear();
		m_A11yDumpWritten = false;
		m_PaintProblemReported = false;
		m_Enabled = false;
	}

	void UiHost::DrawFrame(Wui::WuiContext& ctx, const Wui::WuiInputState& input)
	{
		if (!m_Enabled)
			return;

		// `input.ViewportSize` 已是 WUI 的设计单位视口(物理像素 / 平台内容缩放,见
		// WuiRhiBackend::BeginFrame)。`.wui` 的物理面就取它、DPI 系数保持 1.0:平台缩放
		// 已由 WUI 层的 UiScale() 施加过一次,这里再乘会变成双重缩放。
		UI::UiSurface surface;
		surface.PhysicalSize = input.ViewportSize;
		surface.DpiScale = 1.0f;

		const UI::UiDocument& document = m_Screen.Document();
		const UI::UiViewport viewport = UI::ComputeUiViewport(document.Design, document.SafeArea, surface);
		if (!m_Screen.Layout(viewport))
			return;

		// 每帧重建本窗口的无障碍节点:绘制前清上一帧,绘制后节点与画面同源。
		Wui::WuiAccessibility::Get().BeginFrame(m_WindowKey, viewport.PhysicalSize);

		UI::UiPaintOptions options;
		options.WindowKey = m_WindowKey;
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
}
