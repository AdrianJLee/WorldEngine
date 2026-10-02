#include "wldpch.h"
#include "ScriptsPanel.h"

#include "World/Scene/Components.h"
#include "World/Utils/Paths.h"
#include "World/WUI/WuiAccessibility.h"
#include "World/WUI/WuiLocalization.h"
#include "World/WUI/Widgets/WuiChrome.h"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <string>

namespace World
{
	namespace
	{
		// 行的动作按钮(Reload/Open 共用)与"底部状态行"的尺寸约定。
		constexpr float kRowButtonHeight = 22.0f;
		constexpr float kDiskRowHeight = 24.0f;
		constexpr float kProjectRowHeight = 24.0f;
		constexpr float kDiskHeaderHeight = 24.0f;
		constexpr float kSectionGap = 6.0f;
		constexpr float kStatusReserve = 26.0f;
		// PROJ-11/T1:有 C++ 源码时"项目源码"段至少留 3 行 —— 用户反馈新项目里
		// 这段被上面的场景脚本段挤到看不见(2026-09-29)。
		constexpr std::size_t kProjectMinRows = 3;

		double NowSeconds()
		{
			return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
		}

		// 按 UTF-8 码点边界截断(直接按字节切会切碎中文,文本渲染拿到半个序列)。
		std::string TruncateUtf8(const std::string& text, std::size_t maxBytes)
		{
			if (text.size() <= maxBytes)
				return text;
			std::size_t cut = maxBytes;
			while (cut > 0 && (static_cast<unsigned char>(text[cut]) & 0xC0) == 0x80)
				--cut;
			return text.substr(0, cut) + "…";
		}



		// 登记一个"只读"的无障碍节点(行/状态行):AI 能读到,但 ui.invoke 不会去点它。
		void RegisterReadonlyNode(Wui::WuiId id, const char* kind, const std::string& label,
			const std::string& value, const Wui::WuiRect& rect)
		{
			Wui::WuiAccessNode node;
			node.Id = id;
			node.Window = Wui::WuiAccessibility::Get().CurrentWindow();
			node.Panel = Wui::WuiAccessibility::Get().CurrentPanel();
			node.Kind = kind;
			node.Label = label;
			node.Value = value;
			node.Rect = rect;
			node.Interactive = false;
			Wui::WuiAccessibility::Get().Register(node);
		}
	}

	void ScriptsPanel::SetStatus(std::string text, bool error)
	{
		m_Status = std::move(text);
		m_StatusIsError = error;
	}

	void ScriptsPanel::FocusProjectSources()
	{
		// 面板可能当前没渲染(独立窗口关着/在其它页签);这里只记请求,展开与高亮
		// 都在下一次 OnRender 做 —— 与 EditorShell 的"帧边界再做窗口动作"同一纪律。
		// CPPSRC-1:项目 C++ 的展示面已搬到内容浏览器;OnRender 里这条请求会转成
		// host.FocusContentBrowserProjectSources(),宿主不可用时才回退到"展开段头"的老行为。
		m_FocusProjectSourcesRequested = true;
		m_ProjectSourcesCollapsed = false;
		m_NextProjectScanSeconds = 0.0;
	}

	void ScriptsPanel::RefreshDiskScripts(bool force)
	{
		const double now = NowSeconds();
		if (!force && now < m_NextScanSeconds)
			return;
		m_NextScanSeconds = now + 0.5;

		m_DiskScripts.clear();
		std::error_code error;
		const std::filesystem::path root = World::Paths::AssetRoot() / "scripts";
		if (!std::filesystem::is_directory(root, error))
			return;

		const std::filesystem::directory_options options = std::filesystem::directory_options::skip_permission_denied;
		for (std::filesystem::recursive_directory_iterator iterator(root, options, error), end;
			iterator != end; iterator.increment(error))
		{
			if (error)
			{
				// 单个条目失败(权限/竞态删除)跳过即可,不打断整个扫描。
				error.clear();
				continue;
			}
			const std::filesystem::directory_entry& entry = *iterator;
			std::error_code entryError;
			if (!entry.is_regular_file(entryError) || entryError)
				continue;
			const std::filesystem::path relative = entry.path().lexically_relative(root);
			if (relative.empty())
				continue;
			// 排除生成物目录:intermediate/ 只放引擎生成的存根,不是用户脚本。
			bool generated = false;
			for (const std::filesystem::path& part : relative)
			{
				if (part == "intermediate")
				{
					generated = true;
					break;
				}
			}
			if (generated)
				continue;
			std::string extension = entry.path().extension().string();
			std::transform(extension.begin(), extension.end(), extension.begin(),
				[](unsigned char character) { return static_cast<char>(std::tolower(character)); });
			if (extension != ".lua" && extension != ".luau")
				continue;

			DiskScript script;
			script.LogicalPath = "scripts/" + relative.generic_string();
			script.DiskPath = entry.path();
			m_DiskScripts.push_back(std::move(script));
		}
		std::sort(m_DiskScripts.begin(), m_DiskScripts.end(),
			[](const DiskScript& left, const DiskScript& right) { return left.LogicalPath < right.LogicalPath; });
	}

	// PROJ-8/T1:项目源码段的数据源 —— 当前项目 `<项目根>/src/**` 的 .h/.cpp。
	// 与"磁盘脚本"同一条纪律:0.5s 节流重扫(切片小、目录浅,不做常驻缓存失效的复杂度);
	// 项目切换 / 新建 / 删除后,`World::Paths::ProjectDir()` 一变,下一次节流窗口就换根重扫。
	void ScriptsPanel::RefreshProjectSources(bool force)
	{
		const double now = NowSeconds();
		if (!force && now < m_NextProjectScanSeconds)
			return;
		m_NextProjectScanSeconds = now + 0.5;

		m_ProjectSources.clear();
		std::error_code error;
		const std::filesystem::path projectRoot = World::Paths::ProjectDir();
		// "有当前项目" = 项目根里有清单(启动器的哨兵目录 / 引擎内空的 projects/ 都没有)。
		m_HasProject = !projectRoot.empty()
			&& std::filesystem::is_regular_file(projectRoot / "project.we.yaml", error);
		if (!m_HasProject)
			return;
		const std::filesystem::path sourceRoot = projectRoot / "src";
		if (!std::filesystem::is_directory(sourceRoot, error))
			return;

		const std::filesystem::directory_options options =
			std::filesystem::directory_options::skip_permission_denied;
		for (std::filesystem::recursive_directory_iterator iterator(sourceRoot, options, error), end;
			iterator != end; iterator.increment(error))
		{
			if (error)
			{
				// 单个条目失败(权限/竞态删除)跳过即可,不打断整个扫描(与磁盘脚本段同口径)。
				error.clear();
				continue;
			}
			const std::filesystem::directory_entry& entry = *iterator;
			// 跳过构建产物与隐藏目录(build/ 是项目自己的构建输出,不是源码)。
			std::error_code entryError;
			if (entry.is_directory(entryError) && !entryError)
			{
				const std::string name = entry.path().filename().string();
				if (name == "build" || (!name.empty() && name.front() == '.'))
					iterator.disable_recursion_pending();
				continue;
			}
			if (!entry.is_regular_file(entryError) || entryError)
				continue;
			std::string extension = entry.path().extension().string();
			std::transform(extension.begin(), extension.end(), extension.begin(),
				[](unsigned char character) { return static_cast<char>(std::tolower(character)); });
			if (extension != ".h" && extension != ".cpp")
				continue;
			const std::filesystem::path relative = entry.path().lexically_relative(projectRoot);
			if (relative.empty())
				continue;

			ProjectSource source;
			source.RelativePath = relative.generic_string();
			source.DiskPath = entry.path();
			m_ProjectSources.push_back(std::move(source));
		}
		std::sort(m_ProjectSources.begin(), m_ProjectSources.end(),
			[](const ProjectSource& left, const ProjectSource& right)
			{ return left.RelativePath < right.RelativePath; });
	}

	void ScriptsPanel::OnRender(Wui::WuiContext& ctx, const Wui::WuiRect& rect, PanelHost& host)
	{
		const Wui::WuiTheme& theme = host.Theme();
		if (m_FocusProjectSourcesRequested)
		{
			m_FocusProjectSourcesRequested = false;
			m_ProjectSourcesCollapsed = false;
			RefreshProjectSources(true);
			const std::filesystem::path projectRoot = World::Paths::ProjectDir();
			const std::string sourceRoot = projectRoot.empty()
				? std::string("<project>/src") : (projectRoot / "src").generic_u8string();
			// CPPSRC-1:项目 C++ 的展示面 = 内容浏览器「项目 C++」根。这条请求先走宿主入口;
			// 宿主不可用(没项目 / 没有 src)时才回退到"展开段头 + 高亮"的老提示。
			if (host.FocusContentBrowserProjectSources())
			{
				SetStatus(Wui::TrFormat("panel.scripts.project_sources.focus_status",
					"content browser switched to Project C++ ({count}) under {path}",
					{ { "count", std::to_string(m_ProjectSources.size()) }, { "path", sourceRoot } }), false);
			}
			else
			{
				m_ProjectSourcesHighlightUntil = NowSeconds() + 2.5;
				SetStatus(Wui::TrFormat("panel.scripts.project_sources.focus_status",
					"project sources ({count}) under {path}",
					{ { "count", std::to_string(m_ProjectSources.size()) }, { "path", sourceRoot } }), true);
			}
		}
		RefreshDiskScripts(false);
		RefreshProjectSources(false);

		Wui::PanelBackground(ctx, rect, { 0.10f, 0.105f, 0.115f, 1.0f });

		// ---- 顶部:New(名字自动生成 scripts/systems/script_<n>.lua,不依赖文本输入)----
		float y = rect.Y + 6.0f;
		const Wui::WuiRect newRect { rect.X + 10.0f, y, 116.0f, 24.0f };
		if (Wui::Button(ctx, Wui::HashId("scripts.new"), newRect, "New Script", theme))
		{
			std::string created;
			std::string message;
			if (host.ScriptsCreateFromTemplate(created, &message))
			{
				m_SelectedDisk = created;
				SetStatus(message.empty() ? ("created " + created) : message, false);
				RefreshDiskScripts(true);
			}
			else
			{
				SetStatus(message.empty() ? std::string("create failed") : message, true);
			}
		}
		Wui::Label(ctx, { rect.X + 136.0f, y + 4.0f },
			"disk scripts; New creates one under scripts/systems/ (auto-loaded on play)", theme.TextMuted, 12.0f);
		y += 30.0f;

		// ---- 底部状态行(也是无障碍节点,便于 AI 断言动作结果);空状态分支与正常分支共用 ----
		const auto drawStatusLine = [&]()
		{
			const Wui::WuiRect statusRect { rect.X + 8.0f, rect.Y + rect.H - 22.0f, rect.W - 16.0f, 20.0f };
			const std::string status = m_Status.empty() ? std::string("idle") : m_Status;
			RegisterReadonlyNode(Wui::HashId("scripts.status"), "status", "scripts status", status, statusRect);
			Wui::Label(ctx, { statusRect.X + 2.0f, statusRect.Y + 3.0f },
				TruncateUtf8(status, 150), m_StatusIsError ? theme.Accent : theme.TextMuted, 12.0f);
		};

		// ---- PROJ-8/T1:没有当前项目(启动器/未打开项目)时整页给一条可读提示 ----
		if (!m_HasProject)
		{
			const Wui::WuiRect emptyRect { rect.X + 8.0f, y, std::max(0.0f, rect.W - 16.0f),
				std::max(0.0f, rect.Y + rect.H - kStatusReserve - y - 8.0f) };
			(void)Wui::EmptyState(ctx, emptyRect, std::string(),
				Wui::Tr("panel.scripts.project_sources.no_project.title", "No project open"),
				Wui::Tr("panel.scripts.project_sources.no_project",
					"No project is open — open or create one in the launcher; its C++ sources show up here."),
				std::string(), 0, theme);
			drawStatusLine();
			return;
		}

		// ---- U2d:项目源码与磁盘脚本都空 → 统一空状态 ----
		if (m_ProjectSources.empty() && m_DiskScripts.empty())
		{
			const Wui::WuiRect emptyRect { rect.X + 8.0f, y, std::max(0.0f, rect.W - 16.0f),
				std::max(0.0f, rect.Y + rect.H - kStatusReserve - y - 8.0f) };
			(void)Wui::EmptyState(ctx, emptyRect, std::string(),
				Wui::Tr("panel.scripts.empty.title", "No scripts yet"),
				Wui::Tr("panel.scripts.empty.hint",
					"Create a .luau script in the Content Browser, or use File ▶ New C++ Component… "
					"to add C++ sources under <project>/src/."),
				std::string(), 0, theme);
			drawStatusLine();
			return;
		}

		const float listBottom = rect.Y + rect.H - kStatusReserve;

		// ---- 项目源码段(PROJ-8/T1:当前项目 `<项目根>/src/**` 的 .h/.cpp;
		// PROJ-11/T1:段头可折叠(默认展开)+ 有源码时保证 ≥3 行可见 + File ▸ 项目源码… 高亮)----
		y += kSectionGap;
		const std::string projectHeader = Wui::TrFormat("panel.scripts.project_sources.header",
			"C++ Sources ({count})", { { "count", std::to_string(m_ProjectSources.size()) } });
		const Wui::WuiRect projectHeaderRect { rect.X + 8.0f, y, rect.W - 16.0f, 20.0f };
		bool projectOpen = !m_ProjectSourcesCollapsed;
		if (Wui::CollapsibleHeader(ctx, Wui::HashId("panel.scripts.project_sources.header"),
			projectHeaderRect, projectHeader, projectOpen, theme, std::string(), std::string(),
			Wui::Tr("panel.scripts.project_sources.fold_tooltip",
				"Collapse/expand this project's C++ sources; Open in VS always uses the external Visual Studio.")))
		{
			m_ProjectSourcesCollapsed = !projectOpen;
		}
		if (NowSeconds() < m_ProjectSourcesHighlightUntil)
			Wui::HighlightOutline(ctx, projectHeaderRect, theme.Accent, 3.0f, 2.0f);
		y += kDiskHeaderHeight;

		if (!m_ProjectSourcesCollapsed)
		{
			// CPPSRC-1(用户 2026-09-29「c++脚本要像 asset 资产一样在编辑器里展示」):这里不再重复
			// 画 C++ 文件列表(那是内容浏览器「项目 C++」根的职责) —— 只留一行入口,
			// 点一下就把内容浏览器切到源码根(与 File ▸ 项目源码… 同一条 PanelHost 路径)。
			const std::filesystem::path projectRoot = World::Paths::ProjectDir();
			const std::string sourceRoot = projectRoot.empty()
				? std::string("<project>/src") : (projectRoot / "src").generic_u8string();
			const Wui::WuiRect entryRect { rect.X + 8.0f, y, rect.W - 16.0f, kProjectRowHeight + 4.0f };
			const std::string entryLabel = Wui::Tr("panel.scripts.project_sources.open_in_browser",
				"Open Project C++ in Content Browser");
			if (Wui::Button(ctx, Wui::HashId("scripts.project_sources.open_in_browser"), entryRect,
					entryLabel, theme))
			{
				if (!host.FocusContentBrowserProjectSources())
					SetStatus(Wui::Tr("panel.scripts.project_sources.open_in_browser.unavailable",
						"Open or create a project first — project C++ lives in <project>/src."), true);
				else
					SetStatus(Wui::Tr("panel.scripts.project_sources.open_in_browser.done",
						"Content Browser switched to Project C++: ") + sourceRoot, false);
			}
			y += kProjectRowHeight + 6.0f;
			if (m_ProjectSources.empty())
			{
				// 空态给出下一步 + 解析后的 <项目>/src 绝对路径(提示"文件放哪儿")。
				Wui::Label(ctx, { rect.X + 12.0f, y + 1.0f },
					TruncateUtf8(Wui::Tr("panel.scripts.project_sources.empty",
						"No C++ sources yet — use File ▶ New C++ Component… or drop files into <project>/src/."), 110),
					theme.TextMuted, 12.0f);
				Wui::Label(ctx, { rect.X + 12.0f, y + kProjectRowHeight + 1.0f },
					TruncateUtf8(sourceRoot, 110), theme.TextMuted, 11.0f);
				y += kProjectRowHeight * 2.0f;
			}
			else
			{
				Wui::Label(ctx, { rect.X + 12.0f, y + 1.0f },
					TruncateUtf8(sourceRoot, 110), theme.TextMuted, 11.0f);
				y += 16.0f;
			}
		}

		// ---- 磁盘脚本段 ----
		y += kSectionGap;
		const std::string diskHeader = "Disk Scripts (" + std::to_string(m_DiskScripts.size()) + ")";
		Wui::SectionHeader(ctx, { rect.X + 8.0f, y, rect.W - 16.0f, 20.0f }, diskHeader, theme.Accent, theme);
		y += kDiskHeaderHeight;

		const std::size_t maxDiskRows = std::max<std::size_t>(1,
			static_cast<std::size_t>(std::max(0.0f, listBottom - y) / kDiskRowHeight));
		const std::size_t visibleDiskRows = std::min(m_DiskScripts.size(), maxDiskRows);
		std::size_t drawnDiskRows = 0;
		for (std::size_t index = 0; index < visibleDiskRows; ++index)
		{
			if (y + kDiskRowHeight > listBottom + 0.5f)
				break;
			const DiskScript& script = m_DiskScripts[index];
			const Wui::WuiRect rowRect { rect.X + 8.0f, y, rect.W - 16.0f, kDiskRowHeight - 2.0f };
			const bool selected = script.LogicalPath == m_SelectedDisk;
			Wui::PanelBackground(ctx, rowRect, selected ? theme.ButtonHover : theme.PanelHeader);
			RegisterReadonlyNode(Wui::HashId(("scripts.disk." + std::to_string(index)).c_str()),
				"list-item", script.LogicalPath, script.DiskPath.string(), rowRect);
			Wui::Label(ctx, { rowRect.X + 8.0f, rowRect.Y + 3.0f },
				TruncateUtf8(script.LogicalPath, 76), selected ? theme.Accent : theme.Text, 12.0f);

			// W9-2:主按钮 = 在引擎内打开(脚本编辑器面板,默认附加到主窗口);
			// 次按钮 = External(系统默认程序,原 Open 语义)。
			const Wui::WuiRect externalRect {
				rowRect.X + rowRect.W - 70.0f, rowRect.Y + 1.0f, 66.0f, 20.0f };
			const Wui::WuiRect openRect {
				externalRect.X - 102.0f, rowRect.Y + 1.0f, 96.0f, 20.0f };
			if (Wui::Button(ctx, Wui::HashId(("scripts.open." + std::to_string(index)).c_str()),
				openRect, Wui::Tr("panel.scripts.open_in_engine", "Open in Editor"), theme))
			{
				host.OpenScriptEditor(script.LogicalPath);
				SetStatus("open in editor: " + script.LogicalPath, false);
			}
			if (Wui::Button(ctx, Wui::HashId(("scripts.external." + std::to_string(index)).c_str()),
				externalRect, "External", theme))
			{
				std::string message;
				const bool ok = host.ScriptsOpenExternal(script.LogicalPath, &message);
				SetStatus("external: " + (message.empty() ? (ok ? std::string("ok") : std::string("failed")) : message), !ok);
			}
			y += kDiskRowHeight;
			++drawnDiskRows;
		}
		if (m_DiskScripts.size() > drawnDiskRows)
		{
			Wui::Label(ctx, { rect.X + 12.0f, y }, "+" + std::to_string(m_DiskScripts.size() - drawnDiskRows)
				+ " more disk script(s); enlarge the window to see them", theme.TextMuted, 12.0f);
			y += 16.0f;
		}

		drawStatusLine();
	}
}
