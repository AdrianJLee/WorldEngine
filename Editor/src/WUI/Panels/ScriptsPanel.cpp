#include "wldpch.h"
#include "ScriptsPanel.h"

#include "World/Scene/Components.h"
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
		constexpr float kSceneRowHeight = 46.0f;
		constexpr float kDiskRowHeight = 24.0f;
		constexpr float kDiskHeaderHeight = 24.0f;
		constexpr float kStatusReserve = 26.0f;

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

		const char* ScriptStateText(ScriptInstanceState state)
		{
			switch (state)
			{
				case ScriptInstanceState::Pending: return "Pending";
				case ScriptInstanceState::Creating: return "Creating";
				case ScriptInstanceState::Running: return "Running";
				case ScriptInstanceState::Destroying: return "Destroying";
				case ScriptInstanceState::Stopped: return "Stopped";
				case ScriptInstanceState::Faulted: return "Faulted";
				default: return "?";
			}
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

	void ScriptsPanel::RefreshDiskScripts(bool force)
	{
		const double now = NowSeconds();
		if (!force && now < m_NextScanSeconds)
			return;
		m_NextScanSeconds = now + 0.5;

		m_DiskScripts.clear();
		std::error_code error;
		const std::filesystem::path root = std::filesystem::path(WLD_ASSETPATH) / "scripts";
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

	void ScriptsPanel::OnRender(Wui::WuiContext& ctx, const Wui::WuiRect& rect, PanelHost& host)
	{
		const Wui::WuiTheme& theme = host.Theme();
		RefreshDiskScripts(false);

		Wui::PanelBackground(ctx, rect, { 0.10f, 0.105f, 0.115f, 1.0f });

		// ---- 顶部:New(名字自动生成 scripts/script_<n>.lua,不依赖文本输入)----
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
			"scene + disk scripts; New copies templates/WorldScript.lua", theme.TextMuted, 12.0f);
		y += 30.0f;

		// ---- 场景脚本段 ----
		struct SceneRow
		{
			entt::entity Handle {};
			std::string Tag;
			std::string Path;
			std::string State;
			std::string Diagnostic;
		};
		std::vector<SceneRow> sceneRows;
		if (Ref<Scene> scene = host.GetActiveScene())
		{
			// 只读枚举必须走 const Scene::GetRegistry():Play/Simulate 下活动场景的非 const
			// 入口会触发"结构写禁令"断言。
			const Scene& sceneRef = *scene;
			const entt::registry& registry = sceneRef.GetRegistry();
			for (const entt::entity handle : registry.view<LuaScriptComponent>())
			{
				const LuaScriptComponent& script = registry.get<LuaScriptComponent>(handle);
				SceneRow row;
				row.Handle = handle;
				if (const TagComponent* tag = registry.try_get<TagComponent>(handle))
					row.Tag = tag->Tag;
				if (row.Tag.empty())
					row.Tag = "Entity " + std::to_string(static_cast<uint32_t>(handle));
				row.Path = script.ScriptFilePath.empty() ? std::string("(no script path)") : script.ScriptFilePath;
				row.State = ScriptStateText(script.State);
				row.Diagnostic = !script.ReloadDiagnostic.empty() ? script.ReloadDiagnostic : script.LastError;
				sceneRows.push_back(std::move(row));
			}
			std::sort(sceneRows.begin(), sceneRows.end(),
				[](const SceneRow& left, const SceneRow& right) { return left.Tag < right.Tag; });
		}

		// ---- 底部状态行(也是无障碍节点,便于 AI 断言动作结果);空状态分支与正常分支共用 ----
		const auto drawStatusLine = [&]()
		{
			const Wui::WuiRect statusRect { rect.X + 8.0f, rect.Y + rect.H - 22.0f, rect.W - 16.0f, 20.0f };
			const std::string status = m_Status.empty() ? std::string("idle") : m_Status;
			RegisterReadonlyNode(Wui::HashId("scripts.status"), "status", "scripts status", status, statusRect);
			Wui::Label(ctx, { statusRect.X + 2.0f, statusRect.Y + 3.0f },
				TruncateUtf8(status, 150), m_StatusIsError ? theme.Accent : theme.TextMuted, 12.0f);
		};

		// ---- U2d:场景里没有脚本、磁盘上也没有脚本 → 统一空状态 ----
		// 工具栏与底部状态行保持原样;有任一脚本时下面两段与改动前逐帧一致。
		if (sceneRows.empty() && m_DiskScripts.empty())
		{
			const Wui::WuiRect emptyRect { rect.X + 8.0f, y, std::max(0.0f, rect.W - 16.0f),
				std::max(0.0f, rect.Y + rect.H - kStatusReserve - y - 8.0f) };
			(void)Wui::EmptyState(ctx, emptyRect, std::string(),
				Wui::Tr("panel.scripts.empty.title", "No scripts yet"),
				Wui::Tr("panel.scripts.empty.hint",
					"Create a .luau script in the Content Browser, then open it here."),
				std::string(), 0, theme);
			drawStatusLine();
			return;
		}

		const std::string sceneHeader = "Scene Scripts (" + std::to_string(sceneRows.size()) + ")";
		Wui::SectionHeader(ctx, { rect.X + 8.0f, y, rect.W - 16.0f, 20.0f }, sceneHeader, theme.Accent, theme);
		y += 22.0f;

		const float listBottom = rect.Y + rect.H - kStatusReserve;
		const float listHeight = std::max(0.0f, listBottom - y - kDiskHeaderHeight);
		// 两段共享剩余高度:场景行先占 45%,至少 1 行;720×470 默认窗口下两段都有行。
		const std::size_t maxSceneRows = std::max<std::size_t>(1,
			static_cast<std::size_t>((listHeight * 0.45f) / kSceneRowHeight));
		const std::size_t visibleSceneRows = std::min(sceneRows.size(), maxSceneRows);
		for (std::size_t index = 0; index < visibleSceneRows; ++index)
		{
			const SceneRow& row = sceneRows[index];
			const Wui::WuiRect rowRect { rect.X + 8.0f, y, rect.W - 16.0f, kSceneRowHeight - 4.0f };
			Wui::PanelBackground(ctx, rowRect, theme.PanelHeader);

			std::string detail = row.State;
			if (!row.Diagnostic.empty())
				detail += " | " + row.Diagnostic;
			RegisterReadonlyNode(Wui::HashId(("scripts.scene." + std::to_string(index)).c_str()),
				"list-item", row.Tag, detail, rowRect);

			Wui::Label(ctx, { rowRect.X + 8.0f, rowRect.Y + 4.0f },
				row.Tag + "  [" + row.State + "]", theme.Text, 13.0f);
			std::string line = row.Path;
			if (!row.Diagnostic.empty())
				line += "  -  " + row.Diagnostic;
			Wui::Label(ctx, { rowRect.X + 8.0f, rowRect.Y + 23.0f },
				TruncateUtf8(line, 92), theme.TextMuted, 12.0f);

			const Wui::WuiRect reloadRect {
				rowRect.X + rowRect.W - 72.0f, rowRect.Y + 6.0f, 66.0f, kRowButtonHeight };
			if (Wui::Button(ctx, Wui::HashId(("scripts.reload." + std::to_string(index)).c_str()),
				reloadRect, "Reload", theme))
			{
				std::string message;
				const bool ok = host.ScriptsReloadInstance(row.Handle, &message);
				SetStatus("reload: " + (message.empty() ? (ok ? std::string("queued") : std::string("failed")) : message), !ok);
			}
			y += kSceneRowHeight;
		}
		if (sceneRows.size() > visibleSceneRows)
		{
			Wui::Label(ctx, { rect.X + 12.0f, y }, "+" + std::to_string(sceneRows.size() - visibleSceneRows)
				+ " more scene script(s); enlarge the window to see them", theme.TextMuted, 12.0f);
			y += 16.0f;
		}
		if (sceneRows.empty())
		{
			Wui::Label(ctx, { rect.X + 12.0f, y + 2.0f },
				"current scene has no LuaScriptComponent", theme.TextMuted, 12.0f);
			y += 18.0f;
		}

		// ---- 磁盘脚本段 ----
		y += 6.0f;
		const std::string diskHeader = "Disk Scripts (" + std::to_string(m_DiskScripts.size()) + ")";
		Wui::SectionHeader(ctx, { rect.X + 8.0f, y, rect.W - 16.0f, 20.0f }, diskHeader, theme.Accent, theme);
		y += kDiskHeaderHeight;

		const std::size_t maxDiskRows = std::max<std::size_t>(1,
			static_cast<std::size_t>(std::max(0.0f, listBottom - y) / kDiskRowHeight));
		const std::size_t visibleDiskRows = std::min(m_DiskScripts.size(), maxDiskRows);
		for (std::size_t index = 0; index < visibleDiskRows; ++index)
		{
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
		}
		if (m_DiskScripts.size() > visibleDiskRows)
		{
			Wui::Label(ctx, { rect.X + 12.0f, y }, "+" + std::to_string(m_DiskScripts.size() - visibleDiskRows)
				+ " more disk script(s); enlarge the window to see them", theme.TextMuted, 12.0f);
			y += 16.0f;
		}

		drawStatusLine();
	}
}
