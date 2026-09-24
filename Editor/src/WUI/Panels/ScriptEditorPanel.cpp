#include "wldpch.h"
#include "ScriptEditorPanel.h"

#include "../../EditorPreferences.h"
#include "World/Core/Application.h"
#include "World/Core/KeyCodes.h"
#include "World/Scene/Components.h"
#include "World/Script/HotReload.h"
#include "World/Script/LuauFormatter.h"
#include "World/Script/LuauSyntax.h"
#include "World/WUI/WuiAccessibility.h"
#include "World/WUI/WuiLocalization.h"
#include "World/WUI/Widgets/WuiChrome.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <utility>

namespace World
{
	namespace
	{
		constexpr float kToolbarButtonHeight = 22.0f;
		constexpr float kToolbarHeight = 34.0f;
		constexpr float kStatusHeight = 28.0f;

		double NowSeconds()
		{
			return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
		}

		// FNV-1a64:内容指纹(与 mtime 无关;外部改动的判定只看内容)。
		uint64_t FingerprintText(const std::string& text)
		{
			uint64_t hash = 1469598103934665603ull;
			for (const char c : text)
			{
				hash ^= static_cast<uint8_t>(c);
				hash *= 1099511628211ull;
			}
			return hash;
		}

		// 逻辑路径统一成分隔符 '/' 的形式(面板 id、路径比较、日志都吃这一份)。
		std::string NormalizeLogicalPath(std::string path)
		{
			std::replace(path.begin(), path.end(), '\\', '/');
			while (path.size() > 2 && path.compare(0, 2, "./") == 0)
				path.erase(0, 2);
			return path;
		}

		bool SameLogicalPath(const std::string& left, const std::string& right)
		{
			return NormalizeLogicalPath(left) == NormalizeLogicalPath(right);
		}

		bool ReadDiskFile(const std::filesystem::path& path, std::string& out, std::string* error)
		{
			std::ifstream input(path, std::ios::binary);
			if (!input)
			{
				if (error)
					*error = Wui::Tr("panel.script.error.open_failed", "Cannot read script file: ") + path.string();
				return false;
			}
			std::ostringstream buffer;
			buffer << input.rdbuf();
			if (input.bad())
			{
				if (error)
					*error = Wui::Tr("panel.script.error.read_failed", "Failed to read script file: ") + path.string();
				return false;
			}
			out = buffer.str();
			return true;
		}

		// 同目录原子替换:临时文件与目标同卷,MoveFileEx(REPLACE_EXISTING) 是原子的。
		bool ReplaceFileAtomically(const std::filesystem::path& tempPath, const std::filesystem::path& target,
			std::string* error)
		{
#ifdef _WIN32
			if (MoveFileExW(tempPath.wstring().c_str(), target.wstring().c_str(),
				MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
				return true;
			if (error)
				*error = Wui::Tr("panel.script.error.replace_failed",
					"Save failed (could not replace target; the file may be in use): ") + target.string()
					+ " (Win32 " + std::to_string(static_cast<unsigned long>(GetLastError())) + ")";
			return false;
#else
			std::error_code ec;
			std::filesystem::rename(tempPath, target, ec);
			if (ec)
			{
				if (error)
					*error = Wui::Tr("panel.script.error.save_failed", "Save failed: ") + ec.message();
				return false;
			}
			return true;
#endif
		}

		// 按 UTF-8 码点边界截断(状态行/标签直接按字节切会切碎中文)。
		std::string TruncateUtf8(const std::string& text, std::size_t maxBytes)
		{
			if (text.size() <= maxBytes)
				return text;
			std::size_t cut = maxBytes;
			while (cut > 0 && (static_cast<unsigned char>(text[cut]) & 0xC0) == 0x80)
				--cut;
			return text.substr(0, cut) + "…";
		}

		// 只读节点(status / 列表项):AI 能读到,ui.invoke 不会去点它。
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

		// 禁用按钮:Wui::Button 没有 disabled 参数(WuiWidgets.h),只读时用它画出同样的外观,
		// 同时登记 enabled=false / interactive=false —— ui.tree 能断言"确实禁用了"。
		void DrawDisabledButton(Wui::WuiContext& ctx, Wui::WuiId id, const Wui::WuiRect& rect,
			const std::string& label, const Wui::WuiTheme& theme)
		{
			Wui::WuiAccessNode node;
			node.Id = id;
			node.Window = Wui::WuiAccessibility::Get().CurrentWindow();
			node.Panel = Wui::WuiAccessibility::Get().CurrentPanel();
			node.Kind = "button";
			node.Label = label;
			node.Rect = rect;
			node.Enabled = false;
			node.Interactive = false;
			Wui::WuiAccessibility::Get().Register(node);
			// WUI-P1c-W3.7:只读工具条的禁用画法(底色/描边/文字)改走库件,命令逐字段等价;
			// a11y 节点(Enabled=false / Interactive=false)与外观口径(PanelHeader + TextMuted)
			// 保持在面板侧 —— 与 `Wui::ButtonEx` 的禁用配色是两套口径,不在此片换件。
			Wui::PanelBackground(ctx, rect, theme.PanelHeader, 3.0f);
			Wui::HighlightOutline(ctx, rect, theme.Border, 3.0f, 1.0f);
			Wui::Label(ctx, { rect.X + 8.0f, rect.Y + (rect.H - 15.0f) * 0.5f }, label,
				theme.TextMuted, 15.0f);
		}
	}

	void ScriptEditorPanel::EnsureCompletionReady()
	{
		if (m_CompletionStubReady || m_CompletionStubFailed)
			return;
		const std::string stubPath =
			std::string(WLD_ASSETPATH) + "/scripts/intermediate/WorldEngineAPI.luau";
		std::string error;
		if (!m_Completion.LoadStubFile(stubPath, &error))
		{
			m_CompletionStubFailed = true;
			SetStatus(Wui::Tr("panel.script.completion.unavailable", "Completion unavailable: ")
				+ (error.empty() ? Wui::Tr("panel.script.completion.missing_stub", "missing ") + stubPath : error), true);
			WLD_CORE_WARN("[script-editor] completion index unavailable: {0}",
				error.empty() ? stubPath : error);
			return;
		}
		m_CompletionStubReady = true;
		WLD_CORE_INFO("[script-editor] completion index loaded ({0} symbols)", m_Completion.SymbolCount());
	}

	ScriptEditorPanel::ScriptEditorPanel(std::string logicalPath)
		: m_LogicalPath(NormalizeLogicalPath(logicalPath))
	{
		// 面板 id 必须与 EditorShell 的注册 key 逐字节一致(调用方传入什么后缀就用什么):
		// 只用规范化后的 m_LogicalPath 做"路径解析/比较/状态行",不再二次改写 id,
		// 否则 "./scripts/x.lua" 这类写法会让注册 key 与 Id() 不一致(Close 会错开成"新开面板")。
		m_PanelId = "script:" + logicalPath;
		const std::filesystem::path path(m_LogicalPath);
		const std::string name = path.filename().empty() ? m_LogicalPath : path.filename().string();
		m_PanelTitle = name;
		LoadFromDisk();
	}

	void ScriptEditorPanel::SetStatus(std::string text, bool error)
	{
		m_Status = std::move(text);
		m_StatusIsError = error;
		// 任何显式状态都接管状态行:语法检查的"通过"提示只用于收尾它自己先前的错误,
		// 不得覆盖保存/重载/冲突等用户动作反馈(实测:自动重载提示 0.3s 后被
		// "语法检查通过"吃掉,用户看不到文件已被外部改动重新载入)。
		m_StatusIsSyntax = false;
	}

	void ScriptEditorPanel::LoadFromDisk()
	{
		m_DiskPath.clear();
		m_DiskBacked = false;
		m_ExternalConflict = false;
		m_IgnoredFingerprint = 0;
		m_ConflictFingerprint = 0;

		std::filesystem::path resolved;
		std::string resolveError;
		if (!ResolveScriptDiskPath(m_LogicalPath, resolved, &resolveError))
		{
			// 包内/非法/不存在:面板仍显示,只读 + 状态行错误文本(不伪造占位内容)。
			m_Buffer.SetText(std::string());
			SetStatus(resolveError.empty()
				? Wui::Tr("panel.script.not_editable", "Script is not editable: ") + m_LogicalPath
				: resolveError, true);
			WLD_CORE_INFO("[script-editor] '{0}' opened read-only: {1}", m_LogicalPath,
				resolveError.empty() ? std::string("not a disk script") : resolveError);
			return;
		}
		std::string text;
		std::string readError;
		if (!ReadDiskFile(resolved, text, &readError))
		{
			m_Buffer.SetText(std::string());
			SetStatus(readError, true);
			return;
		}
		m_DiskPath = resolved;
		m_DiskBacked = true;
		m_Buffer.SetText(std::move(text));
		m_DiskFingerprint = FingerprintText(m_Buffer.Text());
		SetStatus(Wui::Tr("panel.script.status.loaded", "Loaded from disk ") + m_LogicalPath, false);
		WLD_CORE_INFO("[script-editor] loaded '{0}' from {1} ({2} bytes)", m_LogicalPath,
			m_DiskPath.string(), m_Buffer.Text().size());
	}

	void ScriptEditorPanel::PollExternalChange()
	{
		if (!m_DiskBacked)
			return;
		const double now = NowSeconds();
		if (now < m_NextDiskCheck)
			return;
		m_NextDiskCheck = now + 0.5;

		std::string diskText;
		std::string error;
		if (!ReadDiskFile(m_DiskPath, diskText, &error))
			return; // 文件被删/暂时不可读:保留当前内容,不猜测、不刷屏
		const uint64_t hash = FingerprintText(diskText);
		if (hash == m_DiskFingerprint)
		{
			m_ExternalConflict = false;
			return;
		}
		if (hash == m_IgnoredFingerprint)
			return; // Keep:同一份磁盘版本不重复提示,直到它再次变化
		if (!m_Buffer.Dirty())
		{
			// buffer 干净:静默跟随磁盘(用户的编辑器里没有会丢的改动)。
			m_Buffer.SetText(std::move(diskText));
			m_DiskFingerprint = hash;
			m_IgnoredFingerprint = 0;
			m_ExternalConflict = false;
			SetStatus(Wui::Tr("panel.script.status.auto_reloaded",
				"File changed on disk: reloaded automatically"), false);
			return;
		}
		m_ExternalConflict = true;
		m_ConflictFingerprint = hash;
		SetStatus(Wui::Tr("panel.script.status.external_conflict",
			"File changed on disk: Reload = overwrite with the disk version, Keep = continue editing"), true);
	}

	void ScriptEditorPanel::ApplySave(PanelHost& host)
	{
		if (host.IsReadOnlyMode())
		{
			SetStatus(Wui::Tr("panel.script.status.readonly_save",
				"Read-only (Play/Simulate): the script cannot be saved"), true);
			return;
		}
		if (!m_DiskBacked)
		{
			SetStatus(Wui::Tr("panel.script.status.not_disk_backed", "Not a disk script, cannot save: ")
				+ m_LogicalPath, true);
			return;
		}
		if (!m_Buffer.Dirty())
		{
			SetStatus(Wui::Tr("panel.script.status.no_changes", "No unsaved changes"), false);
			return;
		}

		const std::string& text = m_Buffer.Text();
		const std::filesystem::path tempPath = m_DiskPath.parent_path()
			/ (m_DiskPath.filename().string() + ".tmp-"
				+ std::to_string(static_cast<unsigned long>(GetCurrentProcessId())));
		{
			std::ofstream output(tempPath, std::ios::binary | std::ios::trunc);
			if (!output)
			{
				SetStatus(Wui::Tr("panel.script.error.temp_create", "Cannot create temporary file: ")
					+ tempPath.string(), true);
				return;
			}
			output.write(text.data(), static_cast<std::streamsize>(text.size()));
			output.flush();
			if (!output.good())
			{
				output.close();
				std::error_code cleanup;
				std::filesystem::remove(tempPath, cleanup);
				SetStatus(Wui::Tr("panel.script.error.temp_write",
					"Failed to write temporary file (editor content preserved): ") + tempPath.string(), true);
				return;
			}
		}
		std::string replaceError;
		if (!ReplaceFileAtomically(tempPath, m_DiskPath, &replaceError))
		{
			std::error_code cleanup;
			std::filesystem::remove(tempPath, cleanup);
			SetStatus(replaceError, true); // 失败:保留 buffer,不丢用户输入
			return;
		}

		m_Buffer.MarkSaved();
		m_DiskFingerprint = FingerprintText(m_Buffer.Text());
		m_IgnoredFingerprint = 0;
		m_ConflictFingerprint = 0;
		m_ExternalConflict = false;
		SetStatus(Wui::Tr("panel.script.status.saved", "Saved ") + m_LogicalPath, false);
		WLD_CORE_INFO("[script-editor] saved '{0}' ({1} bytes)", m_LogicalPath, m_Buffer.Text().size());
		ReloadSceneInstances(host);
	}

	void ScriptEditorPanel::ApplyFormat()
	{
		if (!m_DiskBacked)
			return;
		const std::size_t beforeBytes = m_Buffer.Text().size();
		const std::string formatted = FormatLuauSource(m_Buffer.Text());
		if (m_Buffer.ReplaceAll(formatted))
		{
			SetStatus(Wui::Tr("panel.script.status.formatted",
				"Formatted (4-space indent + trailing whitespace removed)"), false);
			WLD_CORE_INFO("[script-editor] formatted '{0}' ({1} -> {2} bytes)",
				m_LogicalPath, beforeBytes, m_Buffer.Text().size());
		}
		else
			SetStatus(Wui::Tr("panel.script.status.format_unchanged", "No formatting changes"), false);
	}

	void ScriptEditorPanel::ApplyReloadFromDisk(PanelHost& host)
	{
		if (!m_DiskBacked)
		{
			// 非磁盘来源:重新解析一次 —— "文件还没建出来 → 建好 → Ctrl+R" 这条路要能走通。
			LoadFromDisk();
			return;
		}
		if (host.IsReadOnlyMode())
		{
			SetStatus(Wui::Tr("panel.script.status.readonly_reload",
				"Read-only (Play/Simulate): cannot reload from disk"), true);
			return;
		}
		std::string text;
		std::string error;
		if (!ReadDiskFile(m_DiskPath, text, &error))
		{
			SetStatus(error, true);
			return;
		}
		m_Buffer.SetText(std::move(text)); // SetText:重置撤销历史 + 光标归零 + Dirty=false
		m_DiskFingerprint = FingerprintText(m_Buffer.Text());
		m_IgnoredFingerprint = 0;
		m_ConflictFingerprint = 0;
		m_ExternalConflict = false;
		SetStatus(Wui::Tr("panel.script.status.reloaded",
			"Reloaded from disk (undo history reset)"), false);
	}

	void ScriptEditorPanel::ReloadSceneInstances(PanelHost& host)
	{
		if (host.IsReadOnlyMode())
			return; // Play/Simulate 不触发
		Ref<Scene> scene = host.GetActiveScene();
		if (!scene)
			return;
		// 只读枚举必须走 const Scene::GetRegistry():Play/Simulate 下活动场景的非 const 入口会断言。
		const Scene& sceneRef = *scene;
		const entt::registry& registry = sceneRef.GetRegistry();
		int matched = 0;
		int failed = 0;
		std::string lastError;
		for (const entt::entity handle : registry.view<LuaScriptComponent>())
		{
			const LuaScriptComponent& script = registry.get<LuaScriptComponent>(handle);
			if (script.ScriptFilePath.empty() || !SameLogicalPath(script.ScriptFilePath, m_LogicalPath))
				continue;
			++matched;
			std::string message;
			if (!host.ScriptsReloadInstance(handle, &message))
			{
				++failed;
				lastError = message;
			}
		}
		if (matched == 0)
		{
			SetStatus(Wui::Tr("panel.script.status.saved_no_instances", "Saved ") + m_LogicalPath
				+ Wui::Tr("panel.script.status.no_instances_suffix",
					" (the current scene has no instances using this script)"), false);
			return;
		}
		if (failed > 0)
		{
			SetStatus(Wui::Tr("panel.script.status.saved_reload_failed", "Saved, but ")
				+ std::to_string(failed) + "/" + std::to_string(matched)
				+ Wui::Tr("panel.script.status.reload_failed_suffix", " instances failed to reload: ")
				+ lastError, true);
			return;
		}
		SetStatus(Wui::Tr("panel.script.status.saved_hot_reloaded", "Saved and hot-reloaded ")
			+ std::to_string(matched)
			+ Wui::Tr("panel.script.status.instances_suffix", " scene instance(s)"), false);
	}

	bool ScriptEditorPanel::OnShortcut(uint32_t keyCode, bool ctrl, bool shift, bool alt)
	{
		if (!ctrl || alt)
			return false;
		if (keyCode == KeyCodes::S && !shift)
		{
			m_PendingSave = true; // 事件派发期只置位:文档修改统一在 UI 帧内(OnRender)执行
			return true;
		}
		if (keyCode == KeyCodes::R && !shift)
		{
			m_PendingReload = true;
			return true;
		}
		if (keyCode == KeyCodes::F && shift)
		{
			m_PendingFormat = true;   // Ctrl+Shift+F:轻量格式化
			return true;
		}
		return false;
	}

	void ScriptEditorPanel::OnRender(Wui::WuiContext& ctx, const Wui::WuiRect& rect, PanelHost& host)
	{
		const Wui::WuiTheme& theme = host.Theme();
		const bool readOnly = host.IsReadOnlyMode() || !m_DiskBacked;

		// P4-UX7:字号来自编辑器偏好(用户明确要过"希望能自由调整代码字体大小")。
		// 只在偏好值本身变化时套用 —— 否则改主题/缩放这类无关偏好会把 Ctrl+滚轮的临时缩放顶掉。
		{
			const float preferenceFontSize = Editor::EditorPreferences::Get().Data().ScriptFontSize;
			if (std::abs(preferenceFontSize - m_PreferenceFontSize) > 0.01f)
			{
				m_PreferenceFontSize = preferenceFontSize;
				m_FontSize = std::max(10.0f, std::min(32.0f, preferenceFontSize));
			}
		}

		// OnShortcut 只是置位(事件派发发生在 UI 帧之外);这里与工具栏按钮同一条路径消费。
		if (m_PendingReload)
		{
			m_PendingReload = false;
			ApplyReloadFromDisk(host);
		}
		if (m_PendingSave)
		{
			m_PendingSave = false;
			ApplySave(host);
		}
		if (m_PendingFormat)
		{
			m_PendingFormat = false;
			ApplyFormat();
		}
		PollExternalChange();

		// W9.7 编辑防抖语法检查(≈300ms @60fps):状态行给出第一条错误,出错行由编辑器标红。
		if (!readOnly && m_Buffer.Revision() != m_SyntaxCheckedRevision)
		{
			m_SyntaxCheckedRevision = m_Buffer.Revision();
			m_SyntaxDueFrame = ctx.Frame() + 18;
			m_SyntaxScheduled = true;
		}
		if (m_SyntaxScheduled && ctx.Frame() >= m_SyntaxDueFrame)
		{
			m_SyntaxScheduled = false;
			LuauSyntaxError syntaxError;
			if (CheckLuauSyntax(m_Buffer.Text(), m_LogicalPath.c_str(), &syntaxError))
			{
				m_ErrorLine = 0;
				if (m_StatusIsSyntax)
					SetStatus(Wui::Tr("panel.script.syntax.ok", "Syntax check passed"), false);
			}
			else
			{
				// Luau 的 "got <eof>" 会报到尾换行后的空行:按缓冲行数夹取,保证标记行存在。
				m_ErrorLine = syntaxError.Line > 0
					? std::max(1, std::min(syntaxError.Line, static_cast<int>(m_Buffer.LineCount())))
					: 1;
				std::string message = Wui::Tr("panel.script.syntax.error", "Syntax error");
				if (syntaxError.Line > 0)
					message += " L" + std::to_string(syntaxError.Line);
				if (!syntaxError.Message.empty())
					message += ": " + syntaxError.Message;
				SetStatus(std::move(message), true);
				m_StatusIsSyntax = true;
			}
		}

		Wui::PanelBackground(ctx, rect, { 0.09f, 0.095f, 0.105f, 1.0f });

		// ---- 工具栏:Save / Revert / Close(只读时禁用 Save/Revert)----
		float x = rect.X + 10.0f;
		const float toolbarY = rect.Y + 6.0f;
		const auto button = [&](const char* id, float width, const char* label, bool enabled) -> bool
		{
			const Wui::WuiRect bounds { x, toolbarY, width, kToolbarButtonHeight };
			x += width + 8.0f;
			if (enabled)
				return Wui::Button(ctx, Wui::HashId(id), bounds, label, theme);
			DrawDisabledButton(ctx, Wui::HashId(id), bounds, label, theme);
			return false;
		};
		if (button("script.save", 74.0f, "Save", !readOnly))
			ApplySave(host);
		if (button("script.revert", 80.0f, "Revert", !readOnly && m_DiskBacked))
			ApplyReloadFromDisk(host);
		if (button("script.format", 82.0f, "Format", !readOnly))
			ApplyFormat();
		if (button("script.close", 74.0f, "Close", true))
			host.CloseEditorPanel(m_PanelId); // 与 Window 菜单同一条开关路径(附加中 → 关闭并摘标签)

		std::string toolbarHint = m_LogicalPath;
		if (readOnly)
			toolbarHint += Wui::Tr("panel.script.readonly_tag", "   [read-only]");
		if (m_Buffer.Dirty())
			toolbarHint += "   *";
		Wui::Label(ctx, { x + 4.0f, toolbarY + 4.0f }, TruncateUtf8(toolbarHint, 96),
			readOnly ? theme.TextMuted : theme.Text, 12.0f);

		// ---- 状态行:逻辑路径 + line:col + 脏标记 + 错误/提示 ----
		const Wui::WuiRect statusRect { rect.X + 8.0f, rect.Y + rect.H - kStatusHeight + 4.0f,
			rect.W - 16.0f, kStatusHeight - 8.0f };
		const int line = m_Buffer.LineOfOffset(m_Buffer.Caret());
		const int column = m_Buffer.ColumnOfOffset(m_Buffer.Caret());
		std::string status = m_LogicalPath + "  Ln " + std::to_string(line + 1) + ", Col "
			+ std::to_string(column + 1);
		if (m_Buffer.Dirty())
			status += "  *";
		if (readOnly)
			status += Wui::Tr("panel.script.readonly_tag_short", "  [read-only]");
		status += "  " + std::to_string(static_cast<int>(std::round(m_FontSize))) + "px";
		if (!m_Status.empty())
			status += "   |   " + m_Status;
		RegisterReadonlyNode(Wui::HashId("script.status"), "status", "script status", status, statusRect);
		const float statusTextWidth = m_ExternalConflict ? statusRect.W - 136.0f : statusRect.W;
		const std::size_t statusBudget = static_cast<std::size_t>(std::max(32.0f, statusTextWidth / 12.0f));
		Wui::Label(ctx, { statusRect.X + 2.0f, statusRect.Y + 3.0f },
			TruncateUtf8(status, statusBudget),
			m_StatusIsError ? theme.Accent : theme.TextMuted, 12.0f);

		// 外部改动 + 本地脏:两个动作(Reload = 覆盖,Keep = 忽略到指纹再次变化)。
		if (m_ExternalConflict)
		{
			const Wui::WuiRect keepRect { statusRect.X + statusRect.W - 60.0f, statusRect.Y, 56.0f, statusRect.H };
			const Wui::WuiRect reloadRect { keepRect.X - 66.0f, statusRect.Y, 62.0f, statusRect.H };
			if (Wui::Button(ctx, Wui::HashId("script.reload_external"), reloadRect, "Reload", theme))
				ApplyReloadFromDisk(host);
			if (Wui::Button(ctx, Wui::HashId("script.keep_external"), keepRect, "Keep", theme))
			{
				m_IgnoredFingerprint = m_ConflictFingerprint;
				m_ExternalConflict = false;
				SetStatus(Wui::Tr("panel.script.status.keep_local",
					"Kept your edits (ignoring this version on disk)"), false);
			}
		}

		// ---- 正文:代码编辑器(语法高亮 / 行号 / 选区 / 撤销 / 滚动条)----
		const Wui::WuiRect editorRect { rect.X + 8.0f, rect.Y + kToolbarHeight, rect.W - 16.0f,
			std::max(0.0f, rect.H - kToolbarHeight - kStatusHeight) };
		// ---- 字号缩放(W9 追加):Ctrl+滚轮 / Ctrl+= / Ctrl+- / Ctrl+0 复位 ----
		if (ctx.IsHovered(editorRect) && ctx.Input().Ctrl && ctx.Input().Wheel != 0.0f)
			m_FontSize = std::max(10.0f, std::min(32.0f, m_FontSize + ctx.Input().Wheel * 1.0f));
		if (ctx.Input().Ctrl && ctx.WasKeyTriggered(World::KeyCodes::Equal))
			m_FontSize = std::min(32.0f, m_FontSize + 1.0f);
		if (ctx.Input().Ctrl && ctx.WasKeyTriggered(World::KeyCodes::Minus))
			m_FontSize = std::max(10.0f, m_FontSize - 1.0f);
		if (ctx.Input().Ctrl && ctx.WasKeyTriggered(World::KeyCodes::D0))
			m_FontSize = Editor::EditorPreferences::Get().Data().ScriptFontSize;
		auto& accessibility = Wui::WuiAccessibility::Get();
		Wui::WuiAccessNode editorNode;
		editorNode.Id = Wui::HashId("script.editor");
		editorNode.Window = accessibility.CurrentWindow();
		editorNode.Panel = accessibility.CurrentPanel();
		editorNode.Kind = "editor";
		editorNode.Label = "script editor";
		editorNode.Value = m_LogicalPath;
		editorNode.Rect = editorRect;
		editorNode.Enabled = !readOnly;
		editorNode.Interactive = true; // 点击进入编辑区(只读时仍可选中/复制)
		accessibility.Register(editorNode);

		m_Highlight.Update(m_Buffer);
		// W9.5:懒加载补全索引 + 同步当前脚本文件符号(Revision 变化才重建)。
		EnsureCompletionReady();
		if (m_CompletionStubReady && m_CompletionFileRevision != m_Buffer.Revision())
		{
			m_CompletionFileRevision = m_Buffer.Revision();
			m_Completion.SetFileSource(m_Buffer.Text());
		}
		Wui::WuiCodeEditorOptions options;
		options.FontSize = m_FontSize;
		options.LineHeight = std::round(m_FontSize * (20.0f / 14.0f));
		options.ErrorLine = (m_ErrorLine > 0) ? m_ErrorLine - 1 : -1;
		options.ReadOnly = readOnly;
		options.Highlight = [this](std::string_view text, std::vector<Wui::WuiCodeToken>& out)
		{
			if (const std::vector<Wui::WuiCodeToken>* cached = m_Highlight.Find(text))
			{
				out = *cached;
				return;
			}
			// 本帧编辑器改过文本(缓冲区重分配)→ 用当前 buffer 重建缓存后重试;
			// 仍未命中就现场按"行首无延续状态"兜底(下一帧必然命中缓存)。
			m_Highlight.Update(m_Buffer);
			if (const std::vector<Wui::WuiCodeToken>* refreshed = m_Highlight.Find(text))
			{
				out = *refreshed;
				return;
			}
			LuauHighlightState state;
			LuauHighlighter::HighlightLine(text, state, out);
		};
		options.GetClipboard = [](std::string& out)
		{
			if (!Application::HasInstance())
				return false;
			out = Application::Get().GetWindow().GetClipboardText();
			return !out.empty();
		};
		options.SetClipboard = [](std::string_view text)
		{
			if (!Application::HasInstance())
				return false;
			Application::Get().GetWindow().SetClipboardText(std::string(text));
			return true;
		};
		// W9.5:补全 provider(入库 API 存根 + 当前文件符号);候选/过滤由索引负责。
		if (m_CompletionStubReady)
		{
			options.CompletionIdPrefix = "script.suggest";
			options.Completion = [this](std::string_view linePrefix,
				std::vector<World::LuauCompletionItem>& out)
			{
				m_Completion.Query(linePrefix, 50, out);
			};
			// W9.6 悬停提示:同一符号表,按上下文给出 名称/类型/文档。
			options.Hover = [this](std::string_view linePrefix, std::string_view word,
				World::LuauCompletionItem& out)
			{
				return m_Completion.Describe(linePrefix, word, out);
			};
		}
		const Wui::WuiCodeEditorResult result =
			Wui::CodeEditor(ctx, Wui::HashId("script.editor"), editorRect, m_Buffer, options);
		if (result.SaveRequested)
			ApplySave(host);
	}
}
