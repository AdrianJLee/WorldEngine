#include "wldpch.h"
#include "PluginsPanel.h"

#include "../EditorShell.h"

#include "World/Plugins/PluginManager.h"
#include "World/WUI/WuiAccessibility.h"
#include "World/WUI/WuiLocalization.h"
#include "World/WUI/WuiWidgets.h"
#include "World/WUI/Widgets/WuiChrome.h"

#include <algorithm>
#include <string>
#include <vector>

namespace World
{
	namespace
	{
		constexpr float kHeaderHeight = 48.0f;
		constexpr float kRowHeight = 24.0f;
		constexpr float kTableHeaderHeight = 22.0f;
		constexpr float kDetailMinWidth = 320.0f;
		constexpr float kButtonHeight = 24.0f;

		std::string JoinList(const std::vector<std::string>& items, const std::string& empty)
		{
			if (items.empty())
				return empty;
			std::string text;
			for (size_t index = 0; index < items.size(); ++index)
			{
				if (index > 0)
					text += ", ";
				text += items[index];
			}
			return text;
		}

		const char* ScopeText(Plugins::PluginScope scope)
		{
			return scope == Plugins::PluginScope::Project
				? "panel.plugins.scope.project" : "panel.plugins.scope.engine";
		}

		// ABI 列/详情:已加载的插件用自报 ABI;未加载(发现/禁用/加载失败)时用清单声明值 ——
		// 否则还没加载的行永远显示 0,读不出插件要求的 ABI(诊断上重要)。
		uint32_t EffectiveAbi(const Plugins::PluginEntry& entry)
		{
			return entry.PluginAbi != 0 ? entry.PluginAbi : entry.Manifest.Abi;
		}

		// 状态文案 = 条目状态 × 本机禁用 / 本次启动加载失败 / 需重启 三类覆盖。
		// (PluginManager 的 Rejected 诊断与编辑器侧"跳过/失败"记录是两条独立事实,不互相掩盖。)
		std::string PluginStateText(const Plugins::PluginEntry& entry, bool disabled,
			bool restartPending, const std::string& loadError)
		{
			if (entry.State == Plugins::PluginState::Rejected)
				return Wui::Tr("panel.plugins.state.rejected", "Rejected");
			if (!loadError.empty())
				return Wui::Tr("panel.plugins.state.load_failed", "Load failed");
			if (disabled)
				return restartPending
					? Wui::Tr("panel.plugins.state.disabled_pending", "Disabled (restart pending)")
					: Wui::Tr("panel.plugins.state.disabled", "Disabled");
			switch (entry.State)
			{
				case Plugins::PluginState::Loaded:
					return restartPending
						? Wui::Tr("panel.plugins.state.loaded_pending", "Loaded (restart pending)")
						: Wui::Tr("panel.plugins.state.loaded", "Loaded");
				case Plugins::PluginState::Unloaded:
					return Wui::Tr("panel.plugins.state.unloaded", "Unloaded");
				default:
					return Wui::Tr("panel.plugins.state.discovered", "Discovered");
			}
		}

		// 详情里的一行"标签: 值";值超宽按 UTF-8 中间省略(路径这类长串不挤坏布局)。
		void DetailLine(Wui::WuiContext& ctx, const Wui::WuiRect& pane, float y, const std::string& label,
			const std::string& value, const Wui::WuiTheme& theme)
		{
			const float labelWidth = 74.0f;
			if (!label.empty())
				Wui::Label(ctx, { pane.X + theme.Pad, y }, label, theme.TextMuted, theme.FontSizeSmall);
			const std::string text = Wui::EllipsizeMiddleToWidth(ctx, value,
				std::max(40.0f, pane.W - labelWidth - theme.Pad * 2.0f), theme.FontSizeSmall);
			if (!text.empty())
				Wui::Label(ctx, { pane.X + labelWidth, y }, text, theme.Text, theme.FontSizeSmall);
		}

		void RegisterNode(Wui::WuiId id, const char* kind, const std::string& label,
			const std::string& value, const Wui::WuiRect& rect, bool interactive, bool focused,
			const std::string& tooltip)
		{
			Wui::WuiAccessNode node;
			node.Id = id;
			node.Window = Wui::WuiAccessibility::Get().CurrentWindow();
			node.Panel = Wui::WuiAccessibility::Get().CurrentPanel();
			node.Kind = kind;
			node.Label = label;
			node.Value = value;
			node.Tooltip = tooltip;
			node.Rect = rect;
			node.Enabled = true;
			node.Focused = focused;
			node.Interactive = interactive;
			node.Visible = true;
			Wui::WuiAccessibility::Get().Register(node);
		}
	}

	PluginsPanel::PluginsPanel(EditorShell& shell) : m_Shell(shell) {}

	void PluginsPanel::OnRender(Wui::WuiContext& ctx, const Wui::WuiRect& rect, PanelHost& host)
	{
		const Wui::WuiTheme& theme = host.Theme();
		Plugins::PluginManager* manager = m_Shell.GetPluginManager();
		if (!manager)
		{
			// 正常只在项目形态注册(E2);这一支是防御:宿主没接线时给出可读空态,不画假数据。
			Wui::EmptyState(ctx, rect, "!", Wui::Tr("panel.plugins.unavailable", "Plugins unavailable"),
				Wui::Tr("panel.plugins.unavailable.hint", "Open a project first."), std::string(), 0, theme);
			return;
		}

		const std::vector<Plugins::PluginEntry> entries = manager->Entries();

		// ---- 顶部摘要(已加载 N / 已拒绝 M + 引擎/项目/禁用计数)----
		size_t loaded = 0;
		size_t rejected = 0;
		size_t engineCount = 0;
		size_t projectCount = 0;
		size_t disabledCount = 0;
		for (const Plugins::PluginEntry& entry : entries)
		{
			if (entry.State == Plugins::PluginState::Loaded)
				++loaded;
			if (entry.State == Plugins::PluginState::Rejected)
				++rejected;
			if (entry.Manifest.Scope == Plugins::PluginScope::Engine)
				++engineCount;
			else
				++projectCount;
			if (m_Shell.IsPluginDisabled(entry.Manifest.Id))
				++disabledCount;
		}
		const std::string summary = Wui::Tr("panel.plugins.summary.loaded", "Loaded") + " "
			+ std::to_string(loaded) + " / " + Wui::Tr("panel.plugins.summary.rejected", "Rejected")
			+ " " + std::to_string(rejected);
		const std::string summaryDetail = Wui::Tr("panel.plugins.summary.engine", "Engine") + " "
			+ std::to_string(engineCount) + " · " + Wui::Tr("panel.plugins.summary.project", "Project")
			+ " " + std::to_string(projectCount) + " · "
			+ Wui::Tr("panel.plugins.summary.disabled", "Disabled") + " "
			+ std::to_string(disabledCount);
		Wui::Label(ctx, { rect.X + theme.Pad, rect.Y + theme.PadSmall }, summary, theme.Text,
			theme.FontSizeTitle);
		Wui::Label(ctx, { rect.X + theme.Pad, rect.Y + theme.PadSmall + 18.0f }, summaryDetail,
			theme.TextMuted, theme.FontSizeCaption);
		RegisterNode(Wui::HashId("plugins.summary"), "summary", summary,
			"loaded=" + std::to_string(loaded) + " rejected=" + std::to_string(rejected)
				+ " engine=" + std::to_string(engineCount) + " project=" + std::to_string(projectCount)
				+ " disabled=" + std::to_string(disabledCount),
			{ rect.X, rect.Y, rect.W, kHeaderHeight }, false, false, summaryDetail);

		// ---- PLUG-AUTH-1:面板级动作「新建插件…」----
		// 与 File ▸ New Plugin… / 启动器页的同名入口走**同一个向导与脚手架**
		// (面板不自己写一份落盘逻辑);稳定 a11y id plugins.action.new。
		{
			const float newButtonWidth = std::max(96.0f, std::min(140.0f, rect.W * 0.34f));
			const Wui::WuiRect newButton { rect.X + rect.W - theme.Pad - newButtonWidth,
				rect.Y + (kHeaderHeight - kButtonHeight) * 0.5f, newButtonWidth, kButtonHeight };
			if (Wui::ButtonEx(ctx, Wui::HashId("plugins.action.new"), newButton,
					Wui::Tr("panel.plugins.action.new", "New Plugin…"), theme, true, false,
					Wui::Tr("panel.plugins.action.new.tooltip",
						"Create a plugin package (manifest + source + CMake) under "
						"<project>/plugins or <engine>/plugins.")))
			{
				m_Shell.OpenNewPluginModal(ctx);
				ctx.RecordOp("plugins", "new-ask", "", "");
			}
		}

		// ---- 布局:宽 = 左右分栏;窄 = 上下堆叠(停靠面板默认宽度下也要能用)----
		const Wui::WuiRect body { rect.X, rect.Y + kHeaderHeight, rect.W,
			std::max(0.0f, rect.H - kHeaderHeight) };
		const bool sideBySide = body.W >= 720.0f;
		Wui::WuiRect listPane = body;
		Wui::WuiRect detailPane = body;
		if (sideBySide)
		{
			const float detailWidth = std::max(kDetailMinWidth, body.W * 0.38f);
			listPane.W = std::max(280.0f, body.W - detailWidth - 6.0f);
			detailPane.X = body.X + listPane.W + 6.0f;
			detailPane.W = body.W - listPane.W - 6.0f;
		}
		else
		{
			const float listHeight = std::max(120.0f, body.H * 0.52f);
			listPane.H = std::min(body.H, listHeight);
			detailPane.Y = body.Y + listPane.H + 4.0f;
			detailPane.H = std::max(0.0f, body.H - listPane.H - 4.0f);
		}

		// ---- 列表:表头(库件 TableHeader)+ 行(库件 HoverRow/Label)----
		const std::vector<std::string> columns {
			Wui::Tr("panel.plugins.column.id", "ID"),
			Wui::Tr("panel.plugins.column.name", "Name"),
			Wui::Tr("panel.plugins.column.source", "Source"),
			Wui::Tr("panel.plugins.column.version", "Version"),
			Wui::Tr("panel.plugins.column.abi", "ABI"),
			Wui::Tr("panel.plugins.column.state", "State"),
			Wui::Tr("panel.plugins.column.order", "Order"),
			Wui::Tr("panel.plugins.column.root", "Root"),
		};
		const std::vector<float> weights { 0.22f, 0.14f, 0.09f, 0.10f, 0.06f, 0.12f, 0.06f, 0.21f };
		const std::vector<float> columnWidths {
			listPane.W * weights[0], listPane.W * weights[1], listPane.W * weights[2],
			listPane.W * weights[3], listPane.W * weights[4], listPane.W * weights[5],
			listPane.W * weights[6], listPane.W * weights[7] };
		int& sortColumn = ctx.Persist<int>(Wui::HashId("plugins.list.sort"), -1);
		bool& sortAscending = ctx.Persist<bool>(Wui::HashId("plugins.list.sort.asc"), true);
		Wui::TableHeader(ctx, Wui::HashId("plugins.list.header"),
			{ listPane.X, listPane.Y, listPane.W, kTableHeaderHeight }, columns, columnWidths,
			sortColumn, sortAscending, theme);

		std::vector<const Plugins::PluginEntry*> rows;
		rows.reserve(entries.size());
		for (const Plugins::PluginEntry& entry : entries)
			rows.push_back(&entry);
		if (sortColumn >= 0 && rows.size() > 1)
		{
			std::stable_sort(rows.begin(), rows.end(),
				[&sortColumn, &sortAscending](const Plugins::PluginEntry* left,
					const Plugins::PluginEntry* right)
				{
					int order = 0;
					switch (sortColumn)
					{
						case 0: order = left->Manifest.Id.compare(right->Manifest.Id); break;
						case 1: order = left->Manifest.Name.compare(right->Manifest.Name); break;
						case 2: order = static_cast<int>(left->Manifest.Scope)
								- static_cast<int>(right->Manifest.Scope); break;
						case 3: order = left->Manifest.Version.compare(right->Manifest.Version); break;
						case 4: order = static_cast<int>(EffectiveAbi(*left)) - static_cast<int>(EffectiveAbi(*right)); break;
						case 5: order = static_cast<int>(left->State) - static_cast<int>(right->State); break;
						case 6: order = left->Order - right->Order; break;
						default:
							order = left->Manifest.Root.generic_string().compare(
								right->Manifest.Root.generic_string());
							break;
					}
					if (order == 0)
						order = left->Manifest.Id.compare(right->Manifest.Id);
					return sortAscending ? order < 0 : order > 0;
				});
		}

		// 默认选中第一行(用户没点过 = 详情立刻有内容;点过但条目消失 → 回落到第一行)。
		if (!rows.empty())
		{
			const bool selectionAlive = std::any_of(rows.begin(), rows.end(),
				[this](const Plugins::PluginEntry* entry) { return entry->Manifest.Id == m_SelectedId; });
			if (!selectionAlive)
				m_SelectedId = rows.front()->Manifest.Id;
		}
		else
		{
			m_SelectedId.clear();
		}

		const Wui::WuiRect listBody { listPane.X, listPane.Y + kTableHeaderHeight, listPane.W,
			std::max(0.0f, listPane.H - kTableHeaderHeight) };
		float& scroll = ctx.Persist<float>(Wui::HashId("plugins.list.scroll"), 0.0f);
		const float contentHeight = theme.PadSmall * 2.0f + kRowHeight * static_cast<float>(rows.size());
		Wui::BeginScrollArea(ctx, listBody, contentHeight, scroll, theme);
		for (size_t index = 0; index < rows.size(); ++index)
		{
			const Plugins::PluginEntry& entry = *rows[index];
			const Wui::WuiRect row { listBody.X,
				listBody.Y + theme.PadSmall + kRowHeight * static_cast<float>(index) - scroll,
				listBody.W, kRowHeight };
			if (row.Y + row.H < listBody.Y || row.Y > listBody.Y + listBody.H)
				continue;
			const bool selected = entry.Manifest.Id == m_SelectedId;
			const bool hovered = ctx.IsHovered(row);
			Wui::HoverRow(ctx, row, hovered, selected, theme, 0.0f);
			const std::string stateText = PluginStateText(entry,
				m_Shell.IsPluginDisabled(entry.Manifest.Id),
				m_Shell.PluginRestartPending(entry.Manifest.Id),
				m_Shell.PluginLoadError(entry.Manifest.Id));
			const std::string cells[] = {
				entry.Manifest.Id,
				entry.Manifest.Name,
				Wui::Tr(ScopeText(entry.Manifest.Scope),
					entry.Manifest.Scope == Plugins::PluginScope::Engine ? "Engine" : "Project"),
				entry.Manifest.Version,
				std::to_string(EffectiveAbi(entry)),
				stateText,
				entry.Order >= 0 ? std::to_string(entry.Order) : std::string("-"),
				entry.Manifest.Root.generic_string(),
			};
			for (size_t column = 0; column < columns.size(); ++column)
			{
				const Wui::WuiRect cell = Wui::TableCell(listPane, columnWidths, index, column, kRowHeight);
				const std::string text = Wui::EllipsizeMiddleToWidth(ctx, cells[column],
					std::max(10.0f, cell.W - theme.PadSmall * 2.0f), theme.FontSizeSmall);
				if (!text.empty())
					Wui::Label(ctx, { cell.X + theme.PadSmall, row.Y + (row.H - theme.FontSizeSmall) * 0.5f },
						text, selected ? theme.Text : theme.TextMuted, theme.FontSizeSmall);
			}
			RegisterNode(Wui::HashId(("plugins.row." + entry.Manifest.Id).c_str()), "plugin-row",
				entry.Manifest.Name.empty() ? entry.Manifest.Id : entry.Manifest.Name,
				"id=" + entry.Manifest.Id + " name=" + entry.Manifest.Name + " scope="
					+ Plugins::PluginScopeName(entry.Manifest.Scope) + " version=" + entry.Manifest.Version
					+ " abi=" + std::to_string(EffectiveAbi(entry)) + " state="
					+ Plugins::PluginStateName(entry.State) + " order=" + std::to_string(entry.Order)
					+ " root=" + entry.Manifest.Root.generic_string(),
				row, true, selected, entry.Diagnostic);
			if (ctx.IsClicked(row))
				m_SelectedId = entry.Manifest.Id;
		}
		Wui::EndScrollArea(ctx);

		// ---- 详情(选中插件)----
		const Plugins::PluginEntry* selected = nullptr;
		for (const Plugins::PluginEntry& entry : entries)
			if (entry.Manifest.Id == m_SelectedId)
				selected = &entry;
		if (!selected)
		{
			Wui::EmptyState(ctx, detailPane, "", Wui::Tr("panel.plugins.empty", "No plugins discovered"),
				Wui::Tr("panel.plugins.empty.hint",
					"Drop a plugin package under <project>/plugins (or <engine>/plugins) and restart."),
				std::string(), 0, theme);
			return;
		}

		const bool disabled = m_Shell.IsPluginDisabled(selected->Manifest.Id);
		const bool restartPending = m_Shell.PluginRestartPending(selected->Manifest.Id);
		const std::string loadError = m_Shell.PluginLoadError(selected->Manifest.Id);
		const std::string stateText = PluginStateText(*selected, disabled, restartPending, loadError);
		const std::string diagnostic = !selected->Diagnostic.empty() ? selected->Diagnostic : loadError;
		// PLUG-CLEAN-1:重载状态 + 宿主账本摘要(T6 的 pendingReload / ledgers 上屏)。
		const bool pendingReload = manager->HasPendingReload(selected->Manifest.Id);
		// HOTR-P3-T8:一键重载的构建进度(与 plugin.info 的 building/buildExitCode 同一数据源)。
		const Plugins::PluginManager::PluginBuildProgress buildProgress =
			manager->PluginBuildState(selected->Manifest.Id);
		const Plugins::PluginLedgerCounts ledgers = manager->LedgerCounts(selected->Manifest.Id);
		RegisterNode(Wui::HashId(("plugins.detail." + selected->Manifest.Id).c_str()), "plugin-detail",
			selected->Manifest.Id,
			"state=" + stateText + " raw=" + Plugins::PluginStateName(selected->State)
				+ " disabled=" + (disabled ? "1" : "0")
				+ " restart-pending=" + (restartPending ? "1" : "0")
				+ " pending-reload=" + (pendingReload ? "1" : "0")
				+ " ledgers-total=" + std::to_string(ledgers.Total())
				+ " scope=" + Plugins::PluginScopeName(selected->Manifest.Scope)
				+ " version=" + selected->Manifest.Version
				+ " abi=" + std::to_string(EffectiveAbi(*selected))
				+ " order=" + std::to_string(selected->Order)
				+ " root=" + selected->Manifest.Root.generic_string(),
			detailPane, false, false, diagnostic);

		const Wui::WuiRect detailBody { detailPane.X, detailPane.Y, detailPane.W,
			std::max(0.0f, detailPane.H - kButtonHeight - theme.Pad * 2.0f) };
		Wui::PanelBackground(ctx, detailBody, theme.ContentBg, theme.Radius);
		float y = detailBody.Y + theme.PadSmall;
		const float lineStep = theme.FontSizeSmall + 6.0f;
		DetailLine(ctx, detailBody, y, Wui::Tr("panel.plugins.detail.id", "ID"), selected->Manifest.Id, theme);
		y += lineStep;
		DetailLine(ctx, detailBody, y, Wui::Tr("panel.plugins.detail.name", "Name"),
			selected->Manifest.Name.empty() ? selected->Manifest.Id : selected->Manifest.Name, theme);
		y += lineStep;
		DetailLine(ctx, detailBody, y, Wui::Tr("panel.plugins.detail.source", "Source"),
			Wui::Tr(ScopeText(selected->Manifest.Scope),
				selected->Manifest.Scope == Plugins::PluginScope::Engine ? "Engine" : "Project"), theme);
		y += lineStep;
		DetailLine(ctx, detailBody, y, Wui::Tr("panel.plugins.detail.version", "Version"),
			selected->Manifest.Version, theme);
		y += lineStep;
		DetailLine(ctx, detailBody, y, Wui::Tr("panel.plugins.detail.abi", "ABI"),
			std::to_string(EffectiveAbi(*selected)), theme);
		y += lineStep;
		DetailLine(ctx, detailBody, y, Wui::Tr("panel.plugins.detail.state", "State"),
			stateText + (restartPending
				? ("  [" + Wui::Tr("panel.plugins.detail.restart", "restart pending") + "]") : std::string()),
			theme);
		y += lineStep;
		DetailLine(ctx, detailBody, y, Wui::Tr("panel.plugins.detail.order", "Order"),
			selected->Order >= 0 ? std::to_string(selected->Order) : std::string("-"), theme);
		y += lineStep;
		DetailLine(ctx, detailBody, y, Wui::Tr("panel.plugins.detail.root", "Root"),
			selected->Manifest.Root.generic_string(), theme);
		y += lineStep;
		DetailLine(ctx, detailBody, y, Wui::Tr("panel.plugins.detail.depends", "Depends"),
			JoinList(selected->Manifest.Depends, Wui::Tr("panel.plugins.none", "(none)")), theme);
		y += lineStep;
		DetailLine(ctx, detailBody, y, Wui::Tr("panel.plugins.detail.provides", "Provides"),
			JoinList(selected->Manifest.Provides, Wui::Tr("panel.plugins.none", "(none)")), theme);
		y += lineStep;
		// 导出名:PluginEntry::ExportNames 由加载成功时从 WePlugin::Exports 采集(2026-09-30 补的引擎 API);
		// 条目被拒绝/未加载时这里为空 —— 如实显示 (none),不猜。
		std::string exportText = JoinList(selected->ExportNames, Wui::Tr("panel.plugins.none", "(none)"));
		DetailLine(ctx, detailBody, y, Wui::Tr("panel.plugins.detail.exports", "Exports"), exportText, theme);
		y += lineStep;
		// PLUG-CLEAN-1:pendingReload 与账本计数(与 `plugin.info` 的 ledgers 同一数据源)。
		DetailLine(ctx, detailBody, y, Wui::Tr("panel.plugins.detail.pending_reload", "Pending Reload"),
			pendingReload
				? Wui::Tr("panel.plugins.detail.pending_reload.yes",
					"Yes — snapshot held in this session only; press Reload to rebuild and load")
				: Wui::Tr("panel.plugins.detail.pending_reload.no", "No"),
			theme);
		y += lineStep;
		// HOTR-P3-T8:构建进度行(未构建过 = (none);构建在飞/结束都直接可见)。
		std::string buildText;
		if (buildProgress.Running)
			buildText = "building (background CMake target)";
		else if (buildProgress.ExitCode >= 0)
			buildText = "exit " + std::to_string(buildProgress.ExitCode);
		else
			buildText = Wui::Tr("panel.plugins.none", "(none)");
		DetailLine(ctx, detailBody, y, "Build", buildText, theme);
		y += lineStep;
		// 注意:fallback 必须是**单个**字符串字面量 —— audit-localization 按
		// TrFormat 的第一个字符串字面量参数提取占位符集合,拼接的多个字面量会被截断。
		const std::string ledgerText = Wui::TrFormat("panel.plugins.detail.ledgers",
			"{total} registered (schema {schema}, assets {assets}, importers {importers}, commands {commands}, panels {panels}, script fns {script})",
			{ { "total", std::to_string(ledgers.Total()) },
				{ "schema", std::to_string(ledgers.Components) },
				{ "assets", std::to_string(ledgers.AssetTypes) },
				{ "importers", std::to_string(ledgers.Importers) },
				{ "commands", std::to_string(ledgers.EditorCommands) },
				{ "panels", std::to_string(ledgers.EditorPanels) },
				{ "script", std::to_string(ledgers.ScriptFunctions) } });
		DetailLine(ctx, detailBody, y, Wui::Tr("panel.plugins.detail.ledgers_label", "Ledgers"),
			ledgerText, theme);
		y += lineStep;
		DetailLine(ctx, detailBody, y, Wui::Tr("panel.plugins.detail.diagnostic", "Diagnostic"),
			diagnostic.empty() ? Wui::Tr("panel.plugins.none", "(none)") : diagnostic, theme);

		// ---- 操作按钮(稳定 a11y id:plugins.action.locate / copy-diagnostics / toggle)----
		const Wui::WuiRect buttons { detailPane.X, detailPane.Y + detailPane.H - kButtonHeight,
			detailPane.W, kButtonHeight };
		const float gap = theme.PadSmall;
		const float buttonW = std::max(56.0f, (buttons.W - gap * 3.0f) / 4.0f);
		const bool canLocate = selected->Manifest.Scope == Plugins::PluginScope::Project;
		const std::string locateTooltip = canLocate
			? Wui::Tr("panel.plugins.action.locate.tooltip",
				"Show this plugin's directory in the content browser (Project Plugins root).")
			: Wui::Tr("panel.plugins.action.locate.engine_tooltip",
				"Engine plugins live in <engine>/plugins — the content browser shows project plugins only.");
		if (Wui::ButtonEx(ctx, Wui::HashId("plugins.action.locate"),
				{ buttons.X, buttons.Y, buttonW, buttons.H },
				Wui::Tr("panel.plugins.action.locate", "Locate"), theme, canLocate, false, locateTooltip)
			&& canLocate)
		{
			std::string message;
			if (m_Shell.LocatePluginInContentBrowser(selected->Manifest.Id, &message))
				ctx.RecordOp("plugins", "locate", selected->Manifest.Id,
					selected->Manifest.Root.generic_string());
			else
				m_Shell.Notify(message);
		}
		const bool canCopy = !diagnostic.empty();
		if (Wui::ButtonEx(ctx, Wui::HashId("plugins.action.copy-diagnostics"),
				{ buttons.X + buttonW + gap, buttons.Y, buttonW, buttons.H },
				Wui::Tr("panel.plugins.action.copy_diagnostics", "Copy Diagnostics"), theme, canCopy, false,
				canCopy ? Wui::Tr("panel.plugins.action.copy_diagnostics.tooltip",
					"Copy this plugin's rejection/load diagnostics to the clipboard.")
					: Wui::Tr("panel.plugins.action.copy_diagnostics.empty",
						"This plugin has no diagnostics."))
			&& canCopy)
		{
			std::string text = "plugin " + selected->Manifest.Id + "\n";
			text += "scope: " + std::string(Plugins::PluginScopeName(selected->Manifest.Scope)) + "\n";
			text += "version: " + selected->Manifest.Version + "\n";
			text += "state: " + std::string(Plugins::PluginStateName(selected->State)) + "\n";
			text += "root: " + selected->Manifest.Root.generic_string() + "\n";
			text += "diagnostic: " + diagnostic + "\n";
			if (ctx.SetClipboard(text))
			{
				ctx.RecordOp("plugins", "copy-diagnostics", selected->Manifest.Id, diagnostic);
				m_Shell.Notify(Wui::Tr("panel.plugins.notice.diagnostics_copied",
					"Plugin diagnostics copied to the clipboard."));
			}
		}
		// HOTR-P3-T8:「重新加载」= 一键(稳定 a11y id plugins.action.reload)。
		// 面板只登记请求(帧边界由 EditorLayer 执行,面板绘制期间不卸载/装载 DLL):
		// 快照+卸载 → 后台构建该插件的 CMake 目标 → 成功后自动载入新 DLL(失败回滚,
		// 旧状态保留)。构建在飞时按钮置灰(理由 = tooltip;与 module.build_reload 共用
		// 同一个后台构建器)。AI 的两段式 `plugin.reload`(不带 build)保留不变。
		// 契约:快照只活在本会话,不跨编辑器重启(见 docs/dev/plugin-framework.md)。
		const bool buildRunning = manager->PluginBuildRunning();
		const std::string reloadTooltip = buildRunning
			? std::string("A plugin build is running — Reload re-enables when it finishes.")
			: Wui::Tr("panel.plugins.action.reload.tooltip",
				"One-click rebuild & reload: snapshot + unload → build this plugin's CMake target in the "
				"background → load the new DLL. On failure the plugin stays unloaded and the build output "
				"tail is in the log and plugin.info. The snapshot lives in this editor session only.");
		if (Wui::ButtonEx(ctx, Wui::HashId("plugins.action.reload"),
				{ buttons.X + (buttonW + gap) * 2.0f, buttons.Y, buttonW, buttons.H },
				Wui::Tr("panel.plugins.action.reload", "Reload"), theme, !buildRunning, false,
				reloadTooltip))
		{
			Plugins::PluginManager* reloadManager = m_Shell.GetPluginManager();
			if (reloadManager)
			{
				reloadManager->RequestReloadWithBuild(selected->Manifest.Id);
				ctx.RecordOp("plugins", "reload", selected->Manifest.Id,
					"one-click (snapshot+unload → build → load)");
				m_Shell.Notify(Wui::TrFormat("panel.plugins.notice.reload_requested",
					"Rebuild & reload requested for '{id}' — it runs at the next frame boundary.",
					{ { "id", selected->Manifest.Id } }));
			}
		}
		const bool enginePlugin = selected->Manifest.Scope == Plugins::PluginScope::Engine;
		const std::string toggleLabel = enginePlugin
			? (disabled ? Wui::Tr("panel.plugins.action.enable", "Enable")
				: Wui::Tr("panel.plugins.action.disable", "Disable"))
			: Wui::Tr("panel.plugins.action.ships_with_project", "With Project");
		const std::string toggleTooltip = enginePlugin
			? Wui::Tr("panel.plugins.action.toggle.tooltip",
				"Engine plugin: toggle in local/plugins.json (takes effect on the next editor start).")
			: Wui::Tr("panel.plugins.action.toggle.project_tooltip",
				"Project plugins are loaded with the project — manage them in the project manifest.");
		if (Wui::ButtonEx(ctx, Wui::HashId("plugins.action.toggle"),
				{ buttons.X + (buttonW + gap) * 3.0f, buttons.Y, buttonW, buttons.H }, toggleLabel, theme,
				enginePlugin, false, toggleTooltip)
			&& enginePlugin)
		{
			std::string message;
			if (m_Shell.SetPluginEnabled(selected->Manifest.Id, disabled, &message))
			{
				ctx.RecordOp("plugins", disabled ? "enable" : "disable", selected->Manifest.Id,
					"local/plugins.json");
				m_Shell.Notify(message);
			}
			else
			{
				m_Shell.Notify(message);
			}
		}
	}
}
