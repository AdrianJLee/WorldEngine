#include "wldpch.h"
#include "EditorShell.h"

#include <cstdio>
#include <fstream>
#include <shellapi.h>
#include "../EditorPreferences.h"
#include "../EditorLayer.h"
#include "../Project/ProjectLauncher.h"
#include "../Project/ProjectScaffolder.h"

#include "World/Core/Application.h"
#include "World/Core/Asset/ProjectManifest.h"
#include "World/Core/KeyCodes.h"
#include "World/Renderer/Renderer.h"
#include "World/Plugins/PluginManager.h"
#include "World/Utils/Paths.h"
#include "World/Utils/PlatformUtils.h"
#include "World/WUI/WuiLayoutStore.h"
#include "World/WUI/WuiWidgets.h"
#include "World/WUI/WuiAccessibility.h"
#include "World/WUI/Widgets/WuiChrome.h"
#include "World/WUI/Widgets/WuiModal.h"
#include "Panels/MaterialEditorPanel.h"
#include "Panels/ModelPreviewPanel.h"
#include "Panels/PrefabPanel.h"
#include "Panels/SettingsPanel.h"
#include "Panels/PreferencesPanel.h"
#include "World/WUI/WuiLocalization.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <map>
#include <tuple>

#include <chrono>

namespace World
{
	namespace
	{
		// P4-U13:prefab 编辑横幅的行高(激活时它占一行,停靠区整体下移同样高度)。
		constexpr float kPrefabBarHeight = 26.0f;

		// P4-UX10:状态栏提示的计时(悬停暂停/移出宽限/淡出都要秒级精度)。
		double ShellNowSeconds()
		{
			return std::chrono::duration<double>(
				std::chrono::steady_clock::now().time_since_epoch()).count();
		}

		// PROJ-1/T1:项目名输入框的规范化(两侧空白不进目录名/id)。
		std::string TrimProjectNameText(const std::string& text)
		{
			size_t begin = 0;
			while (begin < text.size() && std::isspace(static_cast<unsigned char>(text[begin])))
				++begin;
			size_t end = text.size();
			while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1])))
				--end;
			return text.substr(begin, end - begin);
		}

		// ---- PLUG-AUTH-1:插件向导的默认值 ----
		// 默认插件 id = <owner>.<短名>:owner 优先取当前项目清单的 id(com.<项目名>),
		// 没有项目(启动器形态)时 = "com"。与方案 §1 "默认 com.<owner>.<name>" 同口径。
		std::string PluginIdOwnerPrefix()
		{
			const std::filesystem::path projectRoot = World::Paths::ProjectDir();
			if (projectRoot.empty())
				return "com";
			World::Asset::ProjectManifest manifest;
			std::string error;
			if (World::Asset::ProjectManifest::Load(projectRoot / "project.we.yaml", &manifest, &error)
				&& !manifest.Id.empty()
				&& Editor::PluginScaffolder::ValidatePluginId(manifest.Id + ".probe").empty())
				return manifest.Id;
			return "com";
		}

		std::string PluginIdSuffixFromName(const std::string& name)
		{
			std::string slug;
			for (const char character : name)
			{
				const unsigned char c = static_cast<unsigned char>(character);
				if (std::isalnum(c) != 0)
					slug.push_back(static_cast<char>(std::tolower(c)));
				else if ((c == '-' || c == '_') && !slug.empty() && slug.back() != '-')
					slug.push_back('-');
			}
			while (!slug.empty() && slug.back() == '-')
				slug.pop_back();
			return slug.empty() ? std::string("plugin") : slug;
		}

		std::string DefaultPluginIdForName(const std::string& name)
		{
			return PluginIdOwnerPrefix() + "." + PluginIdSuffixFromName(name);
		}

		// template.json 的 requires → 用户可读的注册面名(T2b/T3b/T4);其它值原样返回。
		std::string PluginRequiresSurface(const std::string& requires)
		{
			if (requires == "t2b")
				return "T2b";
			if (requires == "t3b")
				return "T3b";
			if (requires == "t4")
				return "T4";
			return requires;
		}

		// PLUG-AUTH-1:向无障碍树登记一个"外壳"节点(菜单/模态内部件都挂 panel="shell")。
		void RegisterShellAccessNode(Wui::WuiId id, const char* kind, const std::string& label,
			const std::string& value, const Wui::WuiRect& rect, bool interactive, bool enabled,
			const std::string& tooltip)
		{
			Wui::WuiAccessNode node;
			node.Id = id;
			node.Window = Wui::WuiAccessibility::Get().CurrentWindow();
			node.Panel = "shell";
			node.Kind = kind;
			node.Label = label;
			node.Value = value;
			node.Tooltip = tooltip;
			node.Rect = rect;
			node.Enabled = enabled;
			node.Interactive = interactive;
			node.Visible = true;
			Wui::WuiAccessibility::Get().Register(node);
		}

		// PROJ-4/T1(P2):最近项目搜索的 ASCII 大小写折叠 —— "Alpha"/"alpha" 命中同一行;
		// 非 ASCII 字节原样保留(中文没有大小写,不做 locale 折叠也不会漏)。
		std::string AsciiLowerCopy(const std::string& text)
		{
			std::string lowered = text;
			for (char& character : lowered)
			{
				const unsigned char c = static_cast<unsigned char>(character);
				if (c >= 'A' && c <= 'Z')
					character = static_cast<char>(c - 'A' + 'a');
			}
			return lowered;
		}

		// needle 必须已经是 AsciiLowerCopy 的结果(调用方每帧只折叠一次)。
		bool ContainsCaseInsensitive(const std::string& haystack, const std::string& loweredNeedle)
		{
			if (loweredNeedle.empty())
				return true;
			return AsciiLowerCopy(haystack).find(loweredNeedle) != std::string::npos;
		}

		// PROJ-5R/T1(v2):启动器"删除…"一步确认模态的三种形态 —— 由**目标当前状态**决定
		// (绘制期只读探测;口径与 ProjectLauncher::DeleteProjectPermanently 的守卫一致):
		//   Delete  = 目录在 + 含项目清单 ⇒ 提供"永久删除"(危险色);
		//   Missing = 目录已不存在 ⇒ 提供"从列表移除"(只动 local/projects.json,不碰磁盘);
		//   Reject  = 目录在但不是项目(缺 project.we.yaml)⇒ 拒绝打开/删除 + 可读理由,
		//             但仍提供"从列表移除"(只动 local/projects.json,不碰磁盘)。
		enum class LauncherDeleteVariant { Delete, Missing, Reject };

		LauncherDeleteVariant ClassifyLauncherDeleteTarget(const std::string& path)
		{
			std::error_code error;
			const std::filesystem::path target = std::filesystem::u8path(path);
			if (path.empty() || !std::filesystem::is_directory(target, error))
				return LauncherDeleteVariant::Missing;
			if (!std::filesystem::is_regular_file(target / Editor::ProjectLauncher::kManifestFileName, error))
				return LauncherDeleteVariant::Reject;
			return LauncherDeleteVariant::Delete;
		}

		// PROJ-5/T1:危险动作(删除)的按钮主题 —— 只改两个既有的槽位,不新增控件类型:
		// Accent → Danger 让主按钮填充与悬停描边走危险色,Text → Danger 让非主按钮的文字
		// 也是危险色(禁用态的灰化仍由 ButtonEx 自己的 enabled=false 分支负责)。
		Wui::WuiTheme DangerButtonTheme(const Wui::WuiTheme& base)
		{
			Wui::WuiTheme theme = base;
			theme.Accent = base.Danger;
			theme.Text = base.Danger;
			return theme;
		}

		const char* ZoneName(Wui::DropZone zone)
		{
			switch (zone)
			{
				case Wui::DropZone::Center: return "center";
				case Wui::DropZone::Left: return "left";
				case Wui::DropZone::Right: return "right";
				case Wui::DropZone::Top: return "top";
				case Wui::DropZone::Bottom: return "bottom";
				default: return "?";
			}
		}

		// CPPT-7/PROJ-8:内置脚本编辑器只服务 Lua/Luau —— C++ 源码一律走外部 Visual Studio。
		// 判定只看扩展名(内容浏览器只对 .lua/.luau 走 OpenScriptEditor,不会误伤脚本)。
		bool IsCppSourcePath(const std::string& path)
		{
			std::string extension = std::filesystem::path(path).extension().string();
			std::transform(extension.begin(), extension.end(), extension.begin(),
				[](unsigned char character) { return static_cast<char>(std::tolower(character)); });
			return extension == ".h" || extension == ".hpp" || extension == ".hh"
				|| extension == ".hxx" || extension == ".cpp" || extension == ".cc"
				|| extension == ".cxx";
		}

		// P3-1②:挂靠标签(AttachTag)的无障碍登记 —— 稳定 id `shell.attach.<panel>`,
		// value 标明面板当前是"附加态(attached)"还是"浮动态(floating)",脚本据此断言
		// ui.detach / ui.attach 的结果,与 state.dump 的 "attach" 段同源(PanelStateLabel)。
		// Window 显式写 "main":登记发生在挂靠栏绘制时,但这一枚标签属于主窗口
		// (ui.invoke 的注入点击必须送到主窗口,而不是本轮最后一个渲染过的独立窗口)。
		void RegisterAttachNode(const std::string& panel, const Wui::WuiRect& rect, const char* state,
			std::string label, bool interactive)
		{
			Wui::WuiAccessNode node;
			node.Id = Wui::HashId(("shell.attach." + panel).c_str());
			node.Window = "main";
			node.Panel = panel;
			node.Kind = "attach-tag";
			node.Label = std::move(label);
			node.Value = state;
			node.Rect = rect;
			node.Enabled = true;
			node.Interactive = interactive;
			Wui::WuiAccessibility::Get().Register(node);
		}

		// 面板形态声明(单一事实源):Id 同时用于 Window 菜单、面板注册表与布局存档。
		// Independent = "独立窗口"(自带 OS 窗口 + 标签栏;只能挂靠到主窗口顶部挂靠栏)。
		struct PanelSpec
		{
			const char* Id;
			EditorShell::PanelForm Form;
			Wui::WuiRect DefaultFloatRect; // 独立形态:没有位置记忆时的默认屏幕矩形
		};

		const PanelSpec kPanelSpecs[] =
		{
			{ "hierarchy",       EditorShell::PanelForm::Docked, {} },
			{ "properties",      EditorShell::PanelForm::Docked, {} },
			{ "content_browser", EditorShell::PanelForm::Docked, {} },
			{ "view",            EditorShell::PanelForm::Docked, {} },
			{ "stats",           EditorShell::PanelForm::Docked, {} },
			{ "systems",         EditorShell::PanelForm::Docked, {} },
			// M4-TEX P4:纹理设置(Texture Settings)—— 编辑一个 `.wtex` 资产(设置 + source:)。
			// 形态与 Project Settings 同款(独立窗口形态,默认打开 = 附加到主窗口的标签):
			// 内容浏览器双击 `.wtex` / 右键 → 与 Window 菜单、AI `ui.activate` 落到同一条路径。
			{ "texture_settings", EditorShell::PanelForm::Independent, { 260.0f, 180.0f, 720.0f, 520.0f } },
			// 项目设置(用户 2026-09-20:应为独立窗口):声明为独立窗口形态 —— 默认打开仍走
			// TogglePanel → OpenPanelAttached(附加到主窗口的标签切换),可拖出为独立 OS 窗口。
			// 布局存档里的旧停靠记录由 StripIndependentPanelsFromTree 丢弃(不会再停靠回树)。
			{ "settings",        EditorShell::PanelForm::Independent, { 220.0f, 140.0f, 560.0f, 460.0f } },
			// P4-UX1:编辑器偏好(用户级,自动保存):语言/主题/缩放/术语对照。
			// P4-UX2:左侧分类 + 右侧内容,默认尺寸更大;位置由 FloatRectFor 居中。
			{ "prefs",           EditorShell::PanelForm::Independent, { 0.0f, 0.0f, 900.0f, 560.0f } },
			{ "memory",          EditorShell::PanelForm::Docked, {} },
			{ "operations",      EditorShell::PanelForm::Docked, {} },
			{ "save",            EditorShell::PanelForm::Docked, {} },
			{ "levels",          EditorShell::PanelForm::Docked, {} },
			// 独立窗口(用户指定):Widget Gallery、Input Map、Scripts(与 2026-09-20 起的
			// Project Settings)。
			{ "gallery",         EditorShell::PanelForm::Independent, { 120.0f, 120.0f, 520.0f, 400.0f } },
			{ "input",           EditorShell::PanelForm::Independent, { 660.0f, 120.0f, 440.0f, 340.0f } },
			// W8:Scripts 面板(独立窗口,诊断行/按钮在默认客户区内,便于无鼠标自动化)。
			{ "scripts",         EditorShell::PanelForm::Independent, { 120.0f, 160.0f, 760.0f, 470.0f } },
			// PLUG-T3:插件管理器(**独立窗口形态**,用户 2026-09-30 指定:"应该是独立窗口类型"
			// —— 与 Project Settings / Widget Gallery / Scripts 同款:默认打开走
			// TogglePanel → OpenPanelAttached(附加到主窗口的标签切换),可拖出为独立 OS 窗口,
			// 布局存档里的停靠记录由 StripIndependentPanelsFromTree 丢弃)。
			// E2 = 只在**项目形态**注册 —— 启动器形态下构造期跳过它:不进 m_Panels /
			// 不进注册表 / Window 菜单不出现。
			{ "plugins",         EditorShell::PanelForm::Independent, { 200.0f, 120.0f, 760.0f, 500.0f } },
			// 注:D3 材质编辑器是**动态面板**(每个材质一个 "material:<path>" 实例),
			// 不在这张静态声明表里,由 IsMaterialPanel/EditorShell::OpenMaterialEditor 处理。
		};

		// 动态材质面板 id 前缀:每个材质一个面板/独立窗口(用户 2026-09-16 要求)。
		constexpr const char* kMaterialPanelPrefix = "material:";
		// P1b D5:模型预览面板(每个 .wmodel 一个,打开=只读预览,不改场景)。
		constexpr const char* kModelPanelPrefix = "model:";
		// W9-2:动态脚本编辑器面板 id 前缀:每个脚本一个 "script:<逻辑路径>" 面板。
		constexpr const char* kScriptPanelPrefix = "script:";
		// P4-U13c:prefab 资产窗口(每个 .wprefab 一个 "prefab:<逻辑路径>" 面板)。
		constexpr const char* kPrefabPanelPrefix = "prefab:";
		// PLUG-T3b:插件贡献的面板(独立窗口形态;id 命名空间 = `plugin.panel.<pluginId>.<id>`)。
		constexpr const char* kPluginPanelPrefix = "plugin.panel.";
	}

	EditorShell::EditorShell(EditorLayer& editor, bool launcherMode)
		: m_Editor(editor), m_LauncherMode(launcherMode),
			m_LayoutPath(std::string(WLD_LOCAL_DIR) + "wui-layout.json")
	{
		// 面板列表与默认停靠布局都由形态声明生成:独立形态面板不进停靠树。
		// 注意:"windows"(Independent Windows 面板)与 "attach_slot" 仍未注册(等后续任务),
		// 因此也不在声明表里——布局存档里若残留它们的记录会被加载白名单丢弃。
		std::vector<Wui::PanelId> dockedPanels;
		for (const PanelSpec& spec : kPanelSpecs)
		{
			// PLUG-T3/E2:插件管理器只在项目形态注册(启动器形态:不注册面板、菜单不出现)。
			if (m_LauncherMode && std::strcmp(spec.Id, "plugins") == 0)
				continue;
			m_Panels.push_back(spec.Id);
			if (spec.Form == PanelForm::Docked)
				dockedPanels.push_back(spec.Id);
		}
		const Wui::DockLayout fallback = Wui::DockLayout::Default(dockedPanels);
		if (m_LauncherMode)
		{
			// PROJ-3/T1:启动器模式不读停靠布局存档 —— 启动器页不画面板,也不该因为
			// "启动一次启动器"而改写用户上一次的编辑器布局(整个启动器进程都不保存布局)。
			m_Layout = fallback;
		}
		else
		{
			std::string error;
			if (!Wui::WuiLayoutStore::Load(m_LayoutPath, fallback, &m_Layout, &error))
				WLD_CORE_WARN("Failed to load WUI layout, using default: {0}", error);

			// 加载白名单(按声明纠正存档,不尝试"修复"矛盾记录):
			//  - 未声明的历史面板、声明为独立窗口的面板:从停靠树/浮动记录里直接丢弃;
			//  - 声明为停靠的面板:临时拖出不跨会话(回停靠树)。
			StripIndependentPanelsFromTree(m_Layout);
			RestoreDockedPanelsFromFloat(m_Layout);
		}

		m_PanelRegistry.emplace("hierarchy", std::make_unique<HierarchyPanel>());
		// P4-UX1:主题来源单一化 —— 默认暗色,`WLD_UI_THEME=light|system` 可覆盖。
		m_Theme = Wui::CurrentTheme();
		m_ThemeGeneration = Wui::ThemeGeneration();
		m_PanelRegistry.emplace("properties", std::make_unique<PropertiesPanel>(*this));
		m_PanelRegistry.emplace("content_browser", std::make_unique<ContentBrowserPanel>(*this));
		m_PanelRegistry.emplace("view", std::make_unique<ViewportPanel>(*this));
		m_PanelRegistry.emplace("stats", std::make_unique<StatsPanel>());
		m_PanelRegistry.emplace("systems", std::make_unique<SystemsPanel>());
		// D8a2:项目渲染设置(引擎用户可配置)。用户 2026-09-20 指定为独立窗口形态:
		// 默认打开 = 附加到主窗口(见 PanelSpec),可拖出为独立 OS 窗口 / ui.detach。
		m_PanelRegistry.emplace("settings", std::make_unique<SettingsPanel>());
		m_PanelRegistry.emplace("prefs", std::make_unique<PreferencesPanel>());
		m_PanelRegistry.emplace("memory", std::make_unique<MemoryPanel>());
		m_PanelRegistry.emplace("operations", std::make_unique<OperationsPanel>());
		m_PanelRegistry.emplace("save", std::make_unique<SavePanel>());
		m_PanelRegistry.emplace("levels", std::make_unique<LevelPanel>());
		m_PanelRegistry.emplace("input", std::make_unique<InputMapPanel>());
		m_PanelRegistry.emplace("gallery", std::make_unique<WidgetGalleryPanel>());
		m_PanelRegistry.emplace("material", std::make_unique<MaterialEditorPanel>());
		m_PanelRegistry.emplace("texture_settings", std::make_unique<TextureSettingsPanel>());
		m_PanelRegistry.emplace("scripts", std::make_unique<ScriptsPanel>());
		// PLUG-T3:插件管理器面板(数据/动作都走 EditorShell → EditorLayer;项目形态才注册)。
		if (!m_LauncherMode)
			m_PanelRegistry.emplace("plugins", std::make_unique<PluginsPanel>(*this));
		// PLUG-T3b:插件扩展宿主(命令 / 面板注册表;由 EditorLayer 注入 PluginManager)。
		// 启动器形态不建它:E2 口径下插件扩展面整体缺席。
		if (!m_LauncherMode)
			m_PluginEditorHost = std::make_unique<PluginEditorHost>(*this);

		// P4-UX10:恢复"上次退出时开着"的独立窗口。
		// 存档里仍有浮动记录 = 上次开着(关掉的不会自动弹出);同时记了上次形态(挂靠 chip / 浮窗)。
		// 策略(EditorPreferences::RestoreWindowsMode):默认 **Ask 询问** —— 引擎不该替用户决定
		// 他此刻需不需要这些窗口;询问里可以"记住我的选择"。
		std::vector<PendingFloatRestore> restoreItems;
		std::map<std::tuple<int, int, int, int>, size_t> floatGroups;
		for (const Wui::DockFloat& entry : m_Layout.Floating)
		{
			if (!IsIndependentPanel(entry.Panel))
				continue;
			const auto key = std::make_tuple(
				static_cast<int>(entry.Rect.X), static_cast<int>(entry.Rect.Y),
				static_cast<int>(entry.Rect.W), static_cast<int>(entry.Rect.H));
			auto found = floatGroups.find(key);
			if (found == floatGroups.end())
			{
				PendingFloatRestore item;
				item.Rect = entry.Rect;
				item.Attached = entry.Attached;
				floatGroups.emplace(key, restoreItems.size());
				restoreItems.push_back(std::move(item));
				found = floatGroups.find(key);
			}
			restoreItems[found->second].Panels.push_back(entry.Panel);
			// 同一窗口里只要有一页是浮窗,整窗就按浮窗恢复(挂靠信息含混时取更保守的一方)。
			restoreItems[found->second].Attached = restoreItems[found->second].Attached && entry.Attached;
		}
		// 跨会话记忆:曾经作为独立窗口存在过的面板,其屏幕矩形用于下次打开。
		for (const Wui::DockFloat& entry : m_Layout.FloatMemory)
			m_LastFloatRects[entry.Panel] = entry.Rect;

		if (!restoreItems.empty() && !m_LauncherMode)
		{
			switch (Editor::EditorPreferences::Get().Data().RestoreWindows)
			{
				case Editor::RestoreWindowsMode::None:
					// "不恢复" = 以后也不再问:清掉待恢复记录(位置记忆仍在,可从 Window 菜单重开)。
					m_Layout.Floating.clear();
					SaveLayout();   // 立刻落盘,别把"待恢复"留在文件里等下次换策略又冒出来
					WLD_CORE_INFO("[float] restore skipped by preference ({0} windows forgotten)", restoreItems.size());
					break;
				case Editor::RestoreWindowsMode::Tabs:
					RestoreIndependentWindows(restoreItems, true);
					break;
				case Editor::RestoreWindowsMode::Layout:
					RestoreIndependentWindows(restoreItems, false);
					break;
				case Editor::RestoreWindowsMode::Ask:
				default:
					m_PendingFloatRestore = std::move(restoreItems);   // 首个 UI 帧弹询问
					WLD_CORE_INFO("[float] {0} windows from last session waiting for the restore prompt",
						m_PendingFloatRestore.size());
					break;
			}
		}
	}

	EditorShell::~EditorShell() = default;

	// ---- 面板形态(单一事实源,见 EditorShell.h / T03 方案)----

	EditorShell::PanelForm EditorShell::FormOf(const std::string& panel) const
	{
		// 动态材质面板:每个材质一个独立窗口(不在静态声明表里)。
		if (panel.compare(0, std::strlen(kMaterialPanelPrefix), kMaterialPanelPrefix) == 0)
			return PanelForm::Independent;
		if (panel.compare(0, std::strlen(kModelPanelPrefix), kModelPanelPrefix) == 0)
			return PanelForm::Independent;
		// W9-2:动态脚本编辑器面板同上(打开后默认附加到主窗口,仍属"独立窗口"形态:
		// 可拖出为 OS 窗口 / 再挂靠回主窗口)。
		if (panel.compare(0, std::strlen(kScriptPanelPrefix), kScriptPanelPrefix) == 0)
			return PanelForm::Independent;
		// P4-U13c:prefab 资产窗口同上(默认附加到主窗口,可拖出)。
		if (panel.compare(0, std::strlen(kPrefabPanelPrefix), kPrefabPanelPrefix) == 0)
			return PanelForm::Independent;
		// PLUG-T3b:插件贡献的面板 = 独立窗口形态(与插件管理器同款)。
		if (panel.compare(0, std::strlen(kPluginPanelPrefix), kPluginPanelPrefix) == 0)
			return PanelForm::Independent;
		for (const PanelSpec& spec : kPanelSpecs)
			if (panel == spec.Id)
				return spec.Form;
		return PanelForm::Docked; // 未声明面板按停靠处理,加载白名单会把它们丢掉
	}

	bool EditorShell::IsDeclaredPanel(const std::string& panel) const
	{
		// PLUG-T3/E2:启动器形态不注册插件管理器(菜单/脚本都看不到它)。
		if (m_LauncherMode && panel == "plugins")
			return false;
		// PLUG-T3b:插件面板只在注册表里存在时才算声明(卸载后立刻从菜单/脚本消失)。
		if (IsPluginPanelId(panel))
			return m_PluginEditorHost && m_PluginEditorHost->HasPanel(panel);
		if (panel.compare(0, std::strlen(kMaterialPanelPrefix), kMaterialPanelPrefix) == 0)
			return true;
		if (panel.compare(0, std::strlen(kModelPanelPrefix), kModelPanelPrefix) == 0)
			return true;
		if (panel.compare(0, std::strlen(kScriptPanelPrefix), kScriptPanelPrefix) == 0)
			return true;
		if (panel.compare(0, std::strlen(kPrefabPanelPrefix), kPrefabPanelPrefix) == 0)
			return true;
		for (const PanelSpec& spec : kPanelSpecs)
			if (panel == spec.Id)
				return true;
		return false;
	}

	bool EditorShell::IsPluginPanelId(const std::string& panelId)
	{
		return panelId.rfind(kPluginPanelPrefix, 0) == 0;
	}

	// 独立窗口只能以"独立窗口"存在:存档/撤销里的停靠树记录一律丢弃(不尝试修复)。
	// 同时清掉未声明的历史面板(如已下线的 "windows"),避免它们被保存路径写回停靠树。
	void EditorShell::StripIndependentPanelsFromTree(Wui::DockLayout& layout) const
	{
		std::vector<Wui::PanelId> docked;
		layout.AllPanels(&docked);
		for (const Wui::PanelId& panel : docked)
			if (!IsDeclaredPanel(panel) || IsIndependentPanel(panel))
				layout.RemoveTab(panel);

		const auto undeclared = [this](const Wui::DockFloat& entry) { return !IsDeclaredPanel(entry.Panel); };
		layout.Floating.erase(std::remove_if(layout.Floating.begin(), layout.Floating.end(), undeclared),
			layout.Floating.end());
		layout.FloatMemory.erase(std::remove_if(layout.FloatMemory.begin(), layout.FloatMemory.end(), undeclared),
			layout.FloatMemory.end());
	}

	// 停靠形态面板的"临时拖出"不跨会话:加载/保存时回停靠树(位置记忆仍保留在 FloatMemory)。
	void EditorShell::RestoreDockedPanelsFromFloat(Wui::DockLayout& layout) const
	{
		for (const PanelSpec& spec : kPanelSpecs)
		{
			if (spec.Form != PanelForm::Docked || !layout.IsFloating(spec.Id))
				continue;
			layout.Floating.erase(std::remove_if(layout.Floating.begin(), layout.Floating.end(),
				[&](const Wui::DockFloat& entry) { return entry.Panel == spec.Id; }), layout.Floating.end());
			if (layout.Contains(spec.Id))
				continue;
			const Wui::PanelId anchor = layout.FirstPanel();
			if (!anchor.empty())
			{
				layout.AddTab(spec.Id, anchor, Wui::DropZone::Center);
				continue;
			}
			// 停靠树为空(用户关掉了所有停靠面板):重建一个只含该面板的根 tab 组。
			Wui::DockLayout fresh;
			fresh.Floating = std::move(layout.Floating);
			fresh.FloatMemory = std::move(layout.FloatMemory);
			fresh.Root.Panels.push_back(spec.Id);
			fresh.Root.Active = 0;
			layout = std::move(fresh);
		}
	}

	Wui::DockLayout EditorShell::LayoutForSave() const
	{
		Wui::DockLayout out = m_Layout;
		StripIndependentPanelsFromTree(out);
		RestoreDockedPanelsFromFloat(out);
		// P4-UX10:记录每个独立窗口的**上次形态**(挂靠 chip / 浮窗)——下次启动据此恢复或询问。
		// 判定统一走 PanelStateLabel(单一事实源),覆盖顶栏挂靠、拖动脱出、菜单与 AI 命令各条路径,
		// 不需要在每个状态变更点手写标志位。
		for (Wui::DockFloat& entry : out.Floating)
			entry.Attached = std::strcmp(PanelStateLabel(entry.Panel), "attached") == 0;
		return out;
	}

	Wui::WuiRect EditorShell::FloatRectFor(const std::string& panel) const
	{
		if (const auto remembered = m_LastFloatRects.find(panel); remembered != m_LastFloatRects.end())
			return remembered->second;
		Wui::WuiRect rect {};
		for (const PanelSpec& spec : kPanelSpecs)
			if (panel == spec.Id && spec.Form == PanelForm::Independent)
			{
				rect = spec.DefaultFloatRect;
				break;
			}
		int windowX = 0, windowY = 0;
		float clientW = 0.0f, clientH = 0.0f;
		if (Application::HasInstance())
		{
			Application::Get().GetWindow().GetPosition(&windowX, &windowY);
			clientW = static_cast<float>(Application::Get().GetWindow().GetWidth());
			clientH = static_cast<float>(Application::Get().GetWindow().GetHeight());
		}
		if (rect.W <= 0.0f || rect.H <= 0.0f)
			rect = { 0.0f, 0.0f, 480.0f, 340.0f };
		// P4-UX2c:默认矩形是**设计单位**,窗口是物理像素:按内容缩放放大,
		// 否则 1.3 缩放下面板只有 77% 的设计空间(实测偏好面板右侧说明被裁掉)。
		const float uiScale = Wui::UiScale() > 0.0f ? Wui::UiScale() : 1.0f;
		rect.W *= uiScale;
		rect.H *= uiScale;
		// P4-UX2(用户反馈"全屏时位置太靠左"):没有位置记忆的面板默认落在**主窗口客户区正中**;
		// 用户拖过之后走 m_LastFloatRects / FloatMemory,不会再被居中覆盖。
		rect.X = static_cast<float>(windowX) + (clientW > rect.W ? (clientW - rect.W) * 0.5f : 40.0f);
		rect.Y = static_cast<float>(windowY) + (clientH > rect.H ? (clientH - rect.H) * 0.5f : 40.0f);
		return rect;
	}

	Gameplay::SaveService* EditorShell::GetSaveService()
	{
		return m_Editor.GetSaveService();
	}

	// W8:Scripts 面板的三个宿主能力 —— 只做转发,策略全部留在 EditorLayer(路径解析/唯一重载入口)。
	bool EditorShell::ScriptsReloadInstance(entt::entity handle, std::string* message)
	{
		return m_Editor.ScriptsReloadInstance(handle, message);
	}

	bool EditorShell::ScriptsOpenExternal(const std::string& logicalPath, std::string* message)
	{
		return m_Editor.ScriptsOpenExternal(logicalPath, message);
	}

	bool EditorShell::ScriptsCreateFromTemplate(std::string& outLogicalPath, std::string* message)
	{
		return m_Editor.ScriptsCreateFromTemplate(outLogicalPath, message);
	}

	bool EditorShell::IsReadOnlyMode() const
	{
		// Play/Simulate 期间面板只读查看:可选中/显示,但不改场景数据。
		return m_Editor.IsPlaying() || m_Editor.IsSimulating();
	}

	bool EditorShell::IsViewportCamera3D() const
	{
		return m_Editor.IsViewportCamera3D();
	}

	void EditorShell::ToggleViewportCamera3D()
	{
		m_Editor.ToggleViewportCamera3D();
	}

	Wui::GizmoCamera EditorShell::GetGizmoCamera() const
	{
		// gizmo 只依赖"投影 + 相机基向量 + 距离/FOV":2D 与 3D 视口各取一台相机。
		Wui::GizmoCamera camera;
		// Play(且未暂停):视口渲染走场景的**主相机实体**(见 EditorLayer::OnUpdate 的 Play 分支;
		// 暂停时该分支会切回编辑器相机 —— 覆盖层必须跟渲染保持同一条件,否则暂停后又错位),
		// 覆盖层(选中框/gizmo)必须跟着同一台相机,否则位置整体错位
		// (用户 2026-09-16:"Play 后点物体,选中框位置不对")。
		if (m_Editor.IsPlaying() && !m_Editor.IsPaused())
		{
			if (Ref<Scene> scene = m_Editor.GetActiveScene())
			{
				Entity cameraEntity = scene->GetPrimaryCameraEntity();
				if (cameraEntity.IsValid() && cameraEntity.HasComponent<CameraComponent>() &&
					cameraEntity.HasComponent<TransformComponent>())
				{
					glm::mat4 world = cameraEntity.GetComponent<TransformComponent>().Transform;
					if (cameraEntity.HasComponent<WorldTransformComponent>())
						world = cameraEntity.GetComponent<WorldTransformComponent>().Matrix;
					const Camera& playCamera = cameraEntity.GetComponent<CameraComponent>().Camera;
					camera.ViewProjection = playCamera.GetProjectionMatrix() * glm::inverse(world);
					camera.Position = glm::vec3(world[3]);
					// 与 EditorCamera3D 同约定:Forward = 看向场景内的方向(相机局部 -Z)。
					camera.Forward = -glm::normalize(glm::vec3(world[2]));
					camera.Right = glm::normalize(glm::vec3(world[0]));
					camera.Up = glm::normalize(glm::vec3(world[1]));
					// 透视投影反推竖直 FOV(gizmo 的"屏幕恒定尺寸"用);距离取相机到选中实体。
					const glm::mat4& projection = playCamera.GetProjectionMatrix();
					camera.FovDegrees = std::abs(projection[1][1]) > 1e-6f
						? glm::degrees(2.0f * std::atan(1.0f / std::abs(projection[1][1]))) : 45.0f;
					Entity selected = m_Editor.GetSelectedEntity();
					if (selected.IsValid() && selected.GetScene() == scene.get() &&
						selected.HasComponent<TransformComponent>())
					{
						glm::mat4 selectedWorld = selected.GetComponent<TransformComponent>().Transform;
						if (selected.HasComponent<WorldTransformComponent>())
							selectedWorld = selected.GetComponent<WorldTransformComponent>().Matrix;
						camera.Distance = std::max(0.1f, glm::length(camera.Position - glm::vec3(selectedWorld[3])));
					}
					else
						camera.Distance = 10.0f;
					return camera;
				}
			}
		}
		if (m_Editor.IsViewportCamera3D())
		{
			EditorCamera3D& source = m_Editor.GetEditorCamera3D();
			camera.ViewProjection = source.GetViewProjectionMatrix(/*vulkan=*/false);
			camera.Right = source.GetRight();
			camera.Up = source.GetUp();
			camera.Forward = source.GetForward();
			camera.Position = source.GetPosition();
			camera.Distance = source.GetDistance();
			camera.FovDegrees = source.GetFOV();
		}
		else
		{
			EditorCamera& source = m_Editor.GetEditorCamera();
			camera.ViewProjection = source.GetViewProjection();
			camera.Right = source.GetRightDirection();
			camera.Up = source.GetUpDirection();
			camera.Forward = source.GetForwardDirection();
			camera.Position = source.GetPosition();
			camera.Distance = source.GetDistance();
			camera.FovDegrees = source.GetFov();
		}
		return camera;
	}

	void EditorShell::ReleaseIndependentWindows()
	{
		// 退出时先显式销毁独立窗口:它们的 Vulkan 交换链/OS 窗口必须在
		// RHI 设备与主窗口销毁之前释放,否则会在关闭引擎时崩溃。
		for (const std::unique_ptr<FloatWindowHost>& host : m_FloatHosts)
			if (host)
				host->SetHidden(true); // 保持隐藏,直接进入销毁
		m_FloatHosts.clear();
		m_AttachedPanels.clear();
		m_ActiveWindowTag.clear();
	}

	void EditorShell::RecreateIndependentWindows()
	{
		for (const std::unique_ptr<FloatWindowHost>& host : m_FloatHosts)
			if (host && !host->IsHidden())
				host->RecreateWindow();
	}

	// ---- PanelHost ----

	Ref<Scene> EditorShell::GetActiveScene()
	{
		return m_Editor.GetActiveScene();
	}

	Entity EditorShell::GetSelectedEntity()
	{
		return m_Editor.GetSelectedEntity();
	}

	void EditorShell::SetSelectedEntity(Entity entity)
	{
		m_Editor.SetSelectedEntity(entity);
	}

	bool EditorShell::DebugClickHierarchyRow(size_t index)
	{
		const auto it = m_PanelRegistry.find("hierarchy");
		if (it == m_PanelRegistry.end() || !it->second)
			return false;
		auto* panel = dynamic_cast<HierarchyPanel*>(it->second.get());
		return panel && panel->DebugInvokeRowClick(index);
	}

	void EditorShell::MarkDocumentDirty()
	{
		m_Editor.MarkDocumentDirty();
	}

	void EditorShell::DuplicateSelectedEntity()
	{
		m_Editor.DuplicateSelectedEntity();
	}

	void EditorShell::OpenScene(const std::filesystem::path& path)
	{
		m_Editor.OpenScene(path);
	}

	void EditorShell::OpenPrefabEditor(const std::string& logicalPath)
	{
		// P4-U13e:同一个 prefab 的**资产窗口**里有未保存改动时,先进文档会话会把这些改动丢掉
		// (文档会话从磁盘读)。所以先问一次(丢弃 / 取消),用户确认丢弃后才真的切过去。
		std::string normalized = logicalPath;
		std::replace(normalized.begin(), normalized.end(), '\\', '/');
		if (InterceptPrefabUnsaved(std::string(kPrefabPanelPrefix) + normalized,
			PrefabPendingAction::OpenDocument, normalized))
			return;
		m_Editor.OpenPrefab(logicalPath);
	}

	bool EditorShell::InstantiatePrefabAsset(const std::string& logicalPath, std::string* message)
	{
		return m_Editor.InstantiatePrefabAsset(logicalPath, message);
	}

	// P4-U13d:创建预制体 —— 面板与 AI 通道共用 EditorLayer 的那一条内核(写盘/日志/选中/开窗口)。
	bool EditorShell::CreatePrefabFromSelection(Entity root, const std::string& logicalPath, bool overwrite,
		std::string* message)
	{
		return m_Editor.CreatePrefabFromSelection(root, logicalPath, overwrite, message);
	}

	// P4-U13d:内容浏览器选中一个刚创建/刚生成的资产(必要时先导航到它所在的目录)。
	bool EditorShell::SelectContentAsset(const std::string& logicalPath, const char* op)
	{
		const std::string panel = "content_browser";
		// 面板被关掉时先让它回到停靠树:创建完看不到"被选中的新资产"等于没回显
		// (与打开导入位置模态前同一条处理)。
		if (!m_Layout.Contains(panel))
			DockPanelBackToTree(panel);
		const auto found = m_PanelRegistry.find(panel);
		if (found == m_PanelRegistry.end())
			return false;
		auto* browser = dynamic_cast<ContentBrowserPanel*>(found->second.get());
		if (!browser)
			return false;
		return browser->SelectAsset(logicalPath, op);
	}

	// CPPSRC-1(用户 2026-09-29「c++脚本要像 asset 资产一样在编辑器里展示」):
	// 项目 C++ 的唯一展示面 = 内容浏览器的 `Project C++` 根。三条入口(File ▸ 项目源码…、
	// Scripts 面板的那行入口、AI/脚本)都收敛到这里:面板关着先拉回停靠树(否则"打开了但看不见"),
	// 再激活,最后切根并把结果显示成状态栏提示/警告。
	bool EditorShell::FocusContentBrowserProjectSources()
	{
		const std::string panel = "content_browser";
		if (!m_Layout.Contains(panel))
			DockPanelBackToTree(panel);
		AiActivatePanel(panel, nullptr);
		const auto found = m_PanelRegistry.find(panel);
		if (found == m_PanelRegistry.end())
			return false;
		auto* browser = dynamic_cast<ContentBrowserPanel*>(found->second.get());
		if (!browser)
			return false;
		if (!browser->SwitchRoot(ContentBrowserPanel::RootScope::ProjectSources))
		{
			// 没项目 / 没有 <项目根>/src:给可读理由(与"禁用项必须解释原因"同一口径),
			// 不静默失败 —— 用户点了菜单却什么都不发生是最糟的反馈。
			PushNotice(Wui::Tr("notice.project_sources.unavailable",
				"Project C++ lives in <project>/src — open or create a project first."));
			return false;
		}
		if (m_Ctx)
			m_Ctx->RecordOp("browser", "root-project-sources", CurrentProjectRoot().generic_string(),
				(CurrentProjectRoot() / "src").generic_string());
		return true;
	}

	// CPPSRC-1:内容浏览器在"项目 C++"根下提供的"新建 C++ 组件…"入口 —— 与 File ▸ 新建 C++ 组件…
	// 共用同一个向导(落点 `<项目根>/src/Components/*.h`,建完用外部 Visual Studio 打开)。
	bool EditorShell::RequestNewCppScript()
	{
		if (!m_Ctx || CurrentProjectRoot().empty())
			return false;   // 没有项目:向导弹"先打开项目",面板侧据此显示可读理由
		OpenNewCppScriptModal(*m_Ctx);
		return true;
	}

	// ---- PLUG-T3:插件管理器面板的数据与动作(面板只依赖 shell)----
	// 数据源 = EditorLayer 持有的 PluginManager(T3 里在挂载项目后实例化并 Discover+Load);
	// 面板/命令都走这几条入口,不直接抓 EditorLayer 内部状态。
	Plugins::PluginManager* EditorShell::GetPluginManager() const
	{
		return m_Editor.GetPluginManager();
	}

	bool EditorShell::PluginManagerAvailable() const
	{
		return GetPluginManager() != nullptr;
	}

	bool EditorShell::IsPluginDisabled(const std::string& id) const
	{
		return m_Editor.IsPluginDisabled(id);
	}

	bool EditorShell::PluginRestartPending(const std::string& id) const
	{
		return m_Editor.PluginRestartPending(id);
	}

	std::string EditorShell::PluginLoadError(const std::string& id) const
	{
		return m_Editor.PluginLoadError(id);
	}

	bool EditorShell::SetPluginEnabled(const std::string& id, bool enabled, std::string* message)
	{
		return m_Editor.SetPluginEnabled(id, enabled, message);
	}

	bool EditorShell::LocatePluginInContentBrowser(const std::string& id, std::string* message)
	{
		const auto fail = [message](const std::string& reason)
		{
			if (message)
				*message = reason;
			return false;
		};
		Plugins::PluginManager* plugins = GetPluginManager();
		if (!plugins)
			return fail(Wui::Tr("panel.plugins.notice.need_project", "Open a project first."));
		const Plugins::PluginEntry* entry = plugins->Find(id);
		if (!entry)
			return fail(Wui::TrFormat("panel.plugins.notice.not_found", "No plugin with id '{id}'.",
				{ { "id", id } }));
		if (entry->Manifest.Scope != Plugins::PluginScope::Project)
		{
			// 第三根只显示 `<项目根>/plugins`;引擎插件住在引擎仓库里,不在本根的范围内。
			return fail(Wui::Tr("panel.plugins.notice.engine_plugin_not_in_browser",
				"Engine plugins live in <engine>/plugins — the content browser shows project plugins only."));
		}
		if (!RevealPathInContentBrowserPanel(ContentBrowserPanel::RootScope::ProjectPlugins,
				entry->Manifest.Root, message))
			return false;
		if (m_Ctx)
			m_Ctx->RecordOp("plugins", "locate", id, entry->Manifest.Root.generic_string());
		return true;
	}

	// PLUG-AUTH-1:定位动作的通用实现(插件面板的「在内容浏览器中定位」与新建插件成功态的
	// 「在内容浏览器中定位」共用)。失败 = false + 可读原因。
	bool EditorShell::RevealPathInContentBrowserPanel(ContentBrowserPanel::RootScope scope,
		const std::filesystem::path& absolutePath, std::string* message)
	{
		const auto fail = [message](const std::string& reason)
		{
			if (message)
				*message = reason;
			return false;
		};
		const std::string panel = "content_browser";
		if (!m_Layout.Contains(panel))
			DockPanelBackToTree(panel);
		const auto found = m_PanelRegistry.find(panel);
		if (found == m_PanelRegistry.end())
			return fail("content browser panel is not registered");
		auto* browser = dynamic_cast<ContentBrowserPanel*>(found->second.get());
		if (!browser)
			return fail("content browser panel has an unexpected type");
		if (!browser->RevealPathInRoot(scope, absolutePath))
		{
			if (scope == ContentBrowserPanel::RootScope::ProjectPlugins)
				return fail(Wui::Tr("panel.plugins.notice.no_project_plugins_root",
					"Project plugins live in <project>/plugins — this project has no plugins/ directory."));
			return fail(Wui::TrFormat("notice.browser.reveal_failed",
				"Cannot show this folder in the content browser: {path}",
				{ { "path", absolutePath.u8string() } }));
		}
		AiActivatePanel(panel, nullptr);
		return true;
	}

	// ---- U25-M2:材质工作流(PanelHost 能力)----
	// 赋值/撤销都转发到 EditorLayer 的**唯一写入口**(与 AI 通道 scene.set Material 同一条字段);
	// 编辑器侧不做"面板自己改组件"的旁路,否则脏标记与只读规则会两套。
	bool EditorShell::AssignMaterialToSelection(const std::string& logicalPath, Entity* outEntity,
		std::string* outPreviousPath, std::string* message)
	{
		return m_Editor.AssignMaterialToSelection(logicalPath, outEntity, outPreviousPath, message);
	}

	bool EditorShell::SetEntityMaterialPath(Entity entity, const std::string& materialPath,
		std::string* message)
	{
		return m_Editor.AssignMaterialToEntity(entity, materialPath, message, nullptr);
	}

	// "新建材质"向导住在内容浏览器面板(它本来就有模态与目录/落点控件):
	// Window 菜单与材质面板的 Extract from Selection 都走这一条 —— 单点实现,不复制向导。
	bool EditorShell::OpenNewMaterialWizard(bool fromSelection, std::string* message)
	{
		const std::string panel = "content_browser";
		// 面板被关掉时先让它回到停靠树:向导开在一个看不见的面板里等于没做事
		// (与 SelectContentAsset / 打开导入位置模态同一条处理)。
		if (!m_Layout.Contains(panel))
			DockPanelBackToTree(panel);
		AiActivatePanel(panel, nullptr);
		const auto found = m_PanelRegistry.find(panel);
		if (found == m_PanelRegistry.end())
		{
			if (message)
				*message = "content browser panel is not registered";
			return false;
		}
		auto* browser = dynamic_cast<ContentBrowserPanel*>(found->second.get());
		if (!browser)
		{
			if (message)
				*message = "content browser panel has an unexpected type";
			return false;
		}
		return fromSelection ? browser->OpenNewMaterialFromSelection(message)
			: browser->OpenNewMaterialWizard(message);
	}

	// 面板所在窗口的客户区原点(屏幕物理像素):跨窗口拖放按屏幕坐标命中落点。
	// 主窗口 = GLFW 内容区原点;独立窗口 = FloatWindowHost::ScreenRect() 的原点。
	bool EditorShell::PanelWindowScreenOrigin(const Wui::WuiContext& ctx, float* outX, float* outY)
	{
		const std::string& windowKey = ctx.WindowKey();
		if (windowKey.empty() || windowKey == "main")
		{
			if (!Application::HasInstance())
				return false;
			int windowX = 0, windowY = 0;
			Application::Get().GetWindow().GetPosition(&windowX, &windowY);
			if (outX)
				*outX = static_cast<float>(windowX);
			if (outY)
				*outY = static_cast<float>(windowY);
			return true;
		}
		if (windowKey.rfind("float:", 0) == 0)
		{
			const std::string panel = windowKey.substr(6);
			if (const FloatWindowHost* host = FindFloatHost(panel))
			{
				const Wui::WuiRect screen = host->ScreenRect();
				if (outX)
					*outX = screen.X;
				if (outY)
					*outY = screen.Y;
				return true;
			}
		}
		return false;
	}

	bool EditorShell::PrefabInstanceInfo(Entity entity, std::string* sourcePath, size_t* overrideCount,
		Entity* root)
	{
		return m_Editor.PrefabInstanceInfo(entity, sourcePath, overrideCount, root);
	}

	bool EditorShell::PrefabInstanceRevert(Entity root, std::string* message)
	{
		return m_Editor.PrefabInstanceRevert(root, message);
	}

	bool EditorShell::PrefabInstanceApply(Entity root, std::string* message)
	{
		return m_Editor.PrefabInstanceApply(root, message);
	}

	bool EditorShell::PrefabInstanceUnpack(Entity root, std::string* message)
	{
		return m_Editor.PrefabInstanceUnpack(root, message);
	}

	bool EditorShell::IsEditingPrefabDocument() const
	{
		return m_Editor.IsEditingPrefab();
	}

	bool EditorShell::SavePrefabDocument(std::string* message)
	{
		if (!m_Editor.IsEditingPrefab())
		{
			if (message) *message = "not editing a prefab";
			return false;
		}
		const std::string logical = m_Editor.PrefabEditLogical();
		if (!m_Editor.SavePrefab())
		{
			if (message) *message = "failed to save " + logical;
			return false;
		}
		if (message) *message = "saved " + logical;
		return true;
	}

	bool EditorShell::ClosePrefabDocument()
	{
		if (!m_Editor.IsEditingPrefab())
			return false;
		m_Editor.ClosePrefab();
		return true;
	}

	// P4-U13:prefab 编辑横幅 —— 常驻一条,把"你正在改的是资产"写在最显眼处,
	// 保存与返回各一个按钮(破坏性/离开语义一眼可见)。
	void EditorShell::DrawPrefabBar(Wui::WuiContext& ctx, float y)
	{
		const glm::vec2 viewport = ctx.ViewportSize();
		const Wui::WuiRect bar { 0.0f, y, viewport.x, kPrefabBarHeight };
		// 底色比面板头略暖一档 + 左侧 3px 强调条:和普通工具栏区分开(状态,不是工具)。
		Wui::BarSurface(ctx, bar, m_Theme.PanelHeader, m_Theme.Border);
		ctx.Commands().push_back({ Wui::WuiDrawKind::Rect, { bar.X, bar.Y, 3.0f, bar.H },
			m_Theme.Accent, 0.0f });
		const std::string name = std::filesystem::path(m_Editor.PrefabEditLogical()).filename().string();
		Wui::Label(ctx, { 12.0f, bar.Y + 5.0f },
			Wui::Tr("shell.prefab.editing", "Editing Prefab: ") + name, m_Theme.Text, 13.0f);
		Wui::Label(ctx, { 12.0f + ctx.MeasureTextWidth(
			Wui::Tr("shell.prefab.editing", "Editing Prefab: ") + name, 13.0f) + 10.0f, bar.Y + 6.0f },
			Wui::Tr("shell.prefab.hint", "Saving writes the asset; Back returns to the scene."),
			m_Theme.TextMuted, 11.0f);

		const Wui::WuiRect back { viewport.x - 190.0f, bar.Y + 3.0f, 86.0f, bar.H - 6.0f };
		const Wui::WuiRect save { viewport.x - 98.0f, bar.Y + 3.0f, 90.0f, bar.H - 6.0f };
		if (Wui::Button(ctx, Wui::HashId("shell.prefab.back"), back,
			Wui::Tr("shell.prefab.back", "Back to Scene"), m_Theme))
			m_Editor.ClosePrefab();
		Wui::Tooltip(ctx, back, Wui::Tr("shell.prefab.back.tooltip",
			"Return to the scene you were editing before (unsaved prefab changes stay in the document)."));
		if (Wui::Button(ctx, Wui::HashId("shell.prefab.save"), save,
			Wui::Tr("shell.prefab.save", "Save Prefab"), m_Theme))
			m_Editor.SavePrefab();
		Wui::Tooltip(ctx, save, Wui::Tr("shell.prefab.save.tooltip",
			"Write the document back to this .wprefab (Ctrl+S does the same)."));
	}

	// ---- ViewportHost ----

	bool EditorShell::HasRenderedScene() const
	{
		return m_Editor.HasRenderedScene();
	}

	Ref<SceneRenderer>& EditorShell::GetSceneRenderer()
	{
		return m_Editor.GetSceneRenderer();
	}

	void EditorShell::SetViewportState(bool focused, bool hovered, glm::vec2 size, glm::vec2 bounds[2])
	{
		m_Editor.SetViewportState(focused, hovered, size, bounds);
	}

	void EditorShell::SetViewportRect(const Wui::WuiRect& rect)
	{
		m_ViewportRect = rect;
	}

	bool EditorShell::IsPlaying() const { return m_Editor.IsPlaying(); }
	bool EditorShell::IsSimulating() const { return m_Editor.IsSimulating(); }
	bool EditorShell::IsPaused() const { return m_Editor.IsPaused(); }
	void EditorShell::TogglePlay() { m_Editor.TogglePlay(); }
	void EditorShell::ToggleSimulate() { m_Editor.ToggleSimulate(); }
	void EditorShell::TogglePause() { m_Editor.TogglePause(); }

	Ref<Texture2D> EditorShell::GetIcon(int index) const
	{
		return m_Editor.GetIcon(index);
	}

	uint64_t EditorShell::GetIconId(int index) const
	{
		return m_Editor.GetIconId(index);
	}

	uint32_t EditorShell::TextureEpoch() const
	{
		return m_Editor.TextureEpoch();
	}

	uint64_t EditorShell::GetSceneTextureId() const
	{
		return m_Editor.GetSceneTextureId();
	}

	uint64_t EditorShell::GetCameraPreviewTextureId() const
	{
		return m_Editor.GetCameraPreviewTextureId();
	}

	bool EditorShell::IsCameraPreviewEnabled() const
	{
		return m_Editor.IsCameraPreviewEnabled();
	}

	void EditorShell::ToggleCameraPreview()
	{
		m_Editor.ToggleCameraPreview();
	}

	std::string EditorShell::CameraPreviewLabel() const
	{
		return m_Editor.CameraPreviewLabel();
	}

	Entity EditorShell::PickEntityAt(glm::vec2 viewportLocal)
	{
		return m_Editor.PickEntityAt(viewportLocal);
	}

	EditorCamera& EditorShell::GetEditorCamera()
	{
		return m_Editor.GetEditorCamera();
	}

	Wui::GizmoOperation EditorShell::GetGizmoOperation() const
	{
		return m_Editor.GetGizmoOperation();
	}

	std::string EditorShell::PanelTitle(const std::string& id) const
	{
		// 静态面板走本地化表;动态面板(材质/模型/脚本)用注册表里的动态标题。
		static const std::pair<const char*, const char*>* kTitles = nullptr;
		static const std::map<std::string, std::pair<const char*, const char*>> titles {
			{ "hierarchy",       { "panel.hierarchy", "Hierarchy" } },
			{ "properties",      { "panel.properties", "Properties" } },
			{ "content_browser", { "panel.content_browser", "Content Browser" } },
			{ "view",            { "panel.view", "View" } },
			{ "stats",           { "panel.stats", "Stats" } },
			{ "systems",         { "panel.systems", "Systems" } },
			{ "settings",        { "panel.settings", "Project Settings" } },
			{ "prefs",           { "panel.prefs", "Editor Preferences" } },
			{ "memory",          { "panel.memory", "Memory" } },
			{ "operations",      { "panel.operations", "Operations" } },
			{ "save",            { "panel.save", "Save" } },
			{ "levels",          { "panel.levels", "Levels" } },
			{ "gallery",         { "panel.gallery", "Widget Gallery" } },
			{ "input",           { "panel.input", "Input Map" } },
			{ "scripts",         { "panel.scripts", "Scripts" } },
			{ "plugins",         { "panel.plugins.title", "Plugins" } },
			{ "texture_settings", { "panel.texture_settings", "Texture Settings" } },
		};
		(void)kTitles;
		// PLUG-T3b:插件面板标题来自插件注册(不受本地化表约束;卸载后注册表为空,回退到 id)。
		if (IsPluginPanelId(id) && m_PluginEditorHost)
			if (const std::string pluginTitle = m_PluginEditorHost->PanelTitle(id); !pluginTitle.empty())
				return pluginTitle;
		if (const auto found = titles.find(id); found != titles.end())
			return Wui::Tr(found->second.first, found->second.second);
		const auto it = m_PanelRegistry.find(id);
		if (it == m_PanelRegistry.end())
			return id;
		std::string title = it->second->Title();
		// P4-U13e:prefab 资产窗口有未保存改动时标题带 `*`(窗口标签 / 挂靠标签 / Window 菜单
		// 共用这一份标题,所以"改了但没保存"到哪儿都看得见)。
		if (const auto* prefab = dynamic_cast<const PrefabPanel*>(it->second.get());
			prefab && prefab->HasUnsavedChanges())
			title += " *";
		return title;
	}

	void EditorShell::SaveLayout()
	{
		// PROJ-3/T1:启动器模式不碰用户的布局存档(既不读也不写,见构造函数)。
		if (m_LauncherMode)
			return;
		std::string error;
		// 落盘前按形态声明规范化:独立窗口不进停靠树;停靠面板不持久化"临时拖出"。
		if (!Wui::WuiLayoutStore::Save(m_LayoutPath, LayoutForSave(), &error))
			WLD_CORE_WARN("Failed to save WUI layout: {0}", error);
	}

	void EditorShell::RestoreLayout(const std::string& json)
	{
		std::string error;
		Wui::DockLayout restored;
		if (Wui::DockLayout::Deserialize(json, &restored, &error))
		{
			m_Layout = std::move(restored);
			SaveLayout();
		}
		else
			WLD_CORE_WARN("Failed to restore dock layout: {0}", error);
	}

	void EditorShell::RecordDockChange(Wui::WuiContext& ctx, const std::string& action, const std::string& target, const std::string& before)
	{
		const std::string after = m_Layout.Serialize();
		ctx.RecordOp("dock", action, target, "");
		ctx.History().Push("Dock " + action + " " + target,
			[this, before] { RestoreLayout(before); },
			[this, after] { RestoreLayout(after); });
		SaveLayout();
	}

	// ---- P3-1①:面板拖拽状态归零(唯一出口)----
	// "这次拖拽是谁":拖起手标签 / 跟随窗口 / 落点位置。松手那一帧也要清 ——
	// 残留的 m_TabDragPanel/m_DragPanel 会让面板一回到停靠树就被再次浮出(P3-1① 的 bug)。
	void EditorShell::ClearPanelDragIdentity()
	{
		m_DragPanel.clear();
		m_LastDragPos = { 0.0f, 0.0f };
		m_TabDragPanel.clear();
		m_MovingFloat.clear();
		m_FloatGrabOffset = { 0.0f, 0.0f };
	}

	// 完整归零:身份成员 + 落点成员 + 挂靠标签拖拽。
	// 落点成员(m_DropTargetPanel/m_EdgeDockActive/…)是"最后一帧武装的目标",WuiContext 要到
	// **下一帧**才交付 AcceptDrop —— 所以在"释放沿"那一帧只能清身份成员(见状态机里结束分支的
	// 说明),只有落点已消费或显式取消(Esc)时才走这个完整版本。
	void EditorShell::ClearPanelDragState()
	{
		ClearPanelDragIdentity();
		m_DropTargetPanel.clear();
		m_DropZone = Wui::DropZone::Center;
		m_EdgeDockActive = false;
		m_EdgeDropZone = Wui::DropZone::Center;
		m_LastDragTarget.clear();
		m_LastDragZone = Wui::DropZone::Center;
		m_DropPreviewActive = false;
		m_AttachTagPress.clear();
		m_AttachTagDrag.clear();
	}

	// 拖拽确定已经结束(drop 已消费 / 松手未落点 / Esc 取消 / 左键已抬起)时的收口。
	void EditorShell::EndPanelDrag(Wui::WuiContext& ctx)
	{
		ClearPanelDragState();
		// ctx 里的 dragging/pending/payload/drop 标记一起清:否则下一条命令/下一帧仍会看到
		// "payload 是 panel:xxx"的残留拖拽(正是"面板刚回到停靠树就再次浮出"的原料)。
		ctx.EndDrag();
	}

	// 面板当前形态标签(单一事实源):AiDetachPanel/AiAttachPanel 的幂等判定、state.dump 的
	// "attach" 段、AttachTag 无障碍节点的 value 都用它,避免三处各写一套判定。
	const char* EditorShell::PanelStateLabel(const std::string& panel) const
	{
		if (IsIndependentPanel(panel))
		{
			// 独立窗口形态只有三种状态:顶栏标签(attached)/ 真 OS 窗口(floating)/ 关闭。
			if (std::find(m_AttachedPanels.begin(), m_AttachedPanels.end(), panel) != m_AttachedPanels.end())
				return "attached";
			for (const std::unique_ptr<FloatWindowHost>& host : m_FloatHosts)
			{
				if (!host->Contains(panel))
					continue;
				if (!host->IsHidden())
					return "floating";
				// 隐藏宿主:窗口里只要有一个标签在附加列表里,本面板就属于那枚顶栏标签
				// (窗口被拆成多个标签页时,state.dump / 无障碍节点都要说真话)。
				for (const std::string& id : host->Panels())
					if (std::find(m_AttachedPanels.begin(), m_AttachedPanels.end(), id) != m_AttachedPanels.end())
						return "attached";
				break;
			}
			return "hidden";
		}
		// 停靠形态面板:docked(在停靠树里)/ floating(主窗口内临时浮动)/ hidden。
		if (m_Layout.Contains(panel))
			return "docked";
		if (m_Layout.IsFloating(panel))
			return "floating";
		return "hidden";
	}

	void EditorShell::TogglePanel(Wui::WuiContext& ctx, const std::string& panel)
	{
		const std::string before = m_Layout.Serialize();
		// 已附加到主窗口(标签切换状态):菜单点击 = 关闭该窗口,
		// 隐藏其面板并把标签移出栏(此前只调 HideFloatPanel,会残留附加标签)。
		const auto attached = std::find(m_AttachedPanels.begin(), m_AttachedPanels.end(), panel);
		if (attached != m_AttachedPanels.end())
		{
			// P4-U13e:prefab 面板有未保存改动 → 先弹确认;**不要**先把标签摘掉
			// (窗口还在,只是内容不画了 —— 那就是"静默丢"的另一种形态)。
			if (InterceptPrefabUnsaved(panel, PrefabPendingAction::Close))
				return;
			CloseFloatWindow(panel, true, &ctx);
			m_AttachedPanels.erase(attached);
			if (m_ActiveWindowTag == panel)
				m_ActiveWindowTag.clear();
			return;
		}
		// 独立窗口:开着 → 关闭(隐藏复用,不写回停靠树);关着 → 打开/复用同一窗口。
		if (IsIndependentPanel(panel))
		{
			if (m_Layout.IsFloating(panel))
			{
				HideFloatPanel(panel, &ctx);
				return;
			}
			// D10:默认打开 = **附加到主窗口**(用户 2026-09-19)。Window 菜单与 AI `ui.open`
			// 都走这条,所以这一处覆盖所有"声明为独立形态"的面板(gallery/input/scripts)。
			OpenPanelAttached(panel);
			RecordDockChange(ctx, "attach", panel, before);
			return;
		}
		// 停靠形态面板:临时浮动着 → 回停靠位(D3);已停靠 → 隐藏;两者都不是 → 回到停靠树。
		if (m_Layout.IsFloating(panel))
		{
			if (DockPanelBackToTree(panel))
				RecordDockChange(ctx, "dock", panel, before);
			return;
		}
		if (m_Layout.Contains(panel))
		{
			if (m_Layout.RemoveTab(panel))
				RecordDockChange(ctx, "hide", panel, before);
			return;
		}
		{
			// 优先回到该面板**上次所在的标签组**(RenderTabs 每帧刷新锚点表);
			// 该组已不存在时再退回 FirstPanel,树为空则重建根组。
			std::string anchor;
			if (const auto remembered = m_LastDockAnchors.find(panel); remembered != m_LastDockAnchors.end() &&
				m_Layout.Contains(remembered->second))
				anchor = remembered->second;
			else
				anchor = m_Layout.FirstPanel();
			if (anchor.empty())
			{
				Wui::DockLayout fresh;
				fresh.Root.Panels.push_back(panel);
				fresh.Root.Active = 0;
				m_Layout = std::move(fresh);
				RecordDockChange(ctx, "show", panel, before);
			}
			else if (m_Layout.AddTab(panel, anchor, Wui::DropZone::Center))
				RecordDockChange(ctx, "show", panel, before);
		}
	}

	// 停靠形态面板:从"临时拖出"回到停靠树(锚点 = 树里第一个面板;树为空则重建根 tab 组)。
	bool EditorShell::DockPanelBackToTree(const std::string& panel)
	{
		if (IsIndependentPanel(panel))
			return false;
		bool changed = false;
		if (m_Layout.IsFloating(panel))
		{
			m_Layout.Floating.erase(std::remove_if(m_Layout.Floating.begin(), m_Layout.Floating.end(),
				[&](const Wui::DockFloat& entry) { return entry.Panel == panel; }), m_Layout.Floating.end());
			changed = true;
		}
		if (m_Layout.Contains(panel))
			return changed;
		const Wui::PanelId anchor = m_Layout.FirstPanel();
		if (!anchor.empty())
			return m_Layout.AddTab(panel, anchor, Wui::DropZone::Center) || changed;
		const std::vector<Wui::DockFloat> floating = std::move(m_Layout.Floating);
		const std::vector<Wui::DockFloat> memory = std::move(m_Layout.FloatMemory);
		Wui::DockLayout fresh;
		fresh.Root.Panels.push_back(panel);
		fresh.Root.Active = 0;
		fresh.Floating = floating;
		fresh.FloatMemory = memory;
		m_Layout = std::move(fresh);
		return true;
	}

	// Window 菜单:打开独立形态面板 = 复用已隐藏的窗口(含它原有的其它标签)。
	void EditorShell::OpenIndependentPanel(const std::string& panel)
	{
		const Wui::WuiRect rect = FloatRectFor(panel);
		if (!m_Layout.IsFloating(panel))
			m_Layout.Floating.push_back({ panel, rect });
		m_LastFloatRects[panel] = rect;
		AddFloatWindow(panel, rect, "open");
	}

	void EditorShell::OpenPanelAttached(const std::string& panel)
	{
		// D10(用户 2026-09-19):**所有独立窗口默认附加到主窗口**。
		//  - 已经附加 → 只激活对应标签;
		//  - 用户此前把它拖成了独立窗口(布局里有浮动记录)→ 尊重现状,只前置焦点;
		//  - 否则先按独立窗口建出(复用已隐藏的窗口),随后立即挂靠到主窗口。
		// 显式分离(拖出/菜单)仍然可用 —— 这个函数只改"默认打开"的落点。
		if (std::find(m_AttachedPanels.begin(), m_AttachedPanels.end(), panel) != m_AttachedPanels.end())
		{
			m_ActiveWindowTag = panel;
			return;
		}
		if (m_Layout.IsFloating(panel))
		{
			FocusIndependentWindow(panel);
			return;
		}
		if (!FindFloatHost(panel))
			OpenIndependentPanel(panel);
		AttachIndependentWindowToSlot(panel);
	}

	void EditorShell::ResetLayout(Wui::WuiContext& ctx)
	{
		const std::string before = m_Layout.Serialize();
		// 重置也要按形态声明:独立窗口面板不进停靠树。
		std::vector<Wui::PanelId> dockedPanels;
		for (const PanelSpec& spec : kPanelSpecs)
			if (spec.Form == PanelForm::Docked)
				dockedPanels.push_back(spec.Id);
		m_Layout = Wui::DockLayout::Default(dockedPanels);
		RecordDockChange(ctx, "reset", "", before);
	}

	void EditorShell::OnRender(Wui::WuiContext& ctx)
	{
		m_Ctx = &ctx;
		// P4-UX1:主题模式(暗/浅/跟随系统)变化时刷新本地副本;独立窗口在下次创建/附加时
		// 取到新主题(它们的 callbacks.Theme 是创建期快照)。
		if (m_ThemeGeneration != Wui::ThemeGeneration())
		{
			m_Theme = Wui::CurrentTheme();
			m_ThemeGeneration = Wui::ThemeGeneration();
		}
		// W9-2 修复:上一帧被推迟的脚本编辑器打开请求,在帧边界统一执行(安全点)。
		if (!m_PendingScriptOpen.empty())
		{
			std::vector<std::string> pending;
			pending.swap(m_PendingScriptOpen);
			for (const std::string& path : pending)
				OpenScriptEditorNow(path);
		}
		// PROJ-1/T1:"New Project…" 的 Browse…(原生文件夹对话框)与 Create(写盘)同样是
		// **帧边界**动作:上一帧只置标记,这里统一执行(渲染中途弹 Win32 模态/写盘会踩坑,
		// 与 m_PendingScriptOpen 同一条纪律)。
		if (m_NewProjectBrowsePending)
		{
			m_NewProjectBrowsePending = false;
			RunNewProjectBrowse(ctx);
		}
		if (m_NewProjectCreatePending)
		{
			m_NewProjectCreatePending = false;
			RunNewProjectCreate(ctx);
		}
		// PLUG-AUTH-1:"New Plugin…" 的落盘同样只在帧边界执行(上一帧只置标记)。
		if (m_NewPluginCreatePending)
		{
			m_NewPluginCreatePending = false;
			RunNewPluginCreate(ctx);
		}
		// PROJ-2/T1:File ▸ Open Project… / 启动器 ▸ 打开项目… 的"选择目录"原生对话框
		// 同样只在帧边界弹(上一帧只置标记;与 New Project 向导同一条纪律)。
		if (m_OpenProjectBrowsePending)
		{
			m_OpenProjectBrowsePending = false;
			RunOpenProjectBrowse(ctx);
		}
		// PROJ-3/T1:纯启动器模式 —— 窗口只有一页启动器;菜单栏 / 挂靠栏 / 停靠区 /
		// 状态栏 / 独立窗口全部不画(无障碍树里也就只有启动器页的节点)。
		if (m_LauncherMode)
		{
			RenderLauncherPage(ctx);
			return;
		}
		// M4-TEX P4:内容浏览器(双击 `.wtex` / 右键 Texture Settings…/Reimport/Reset)在渲染期间
		// 只能写"打开纹理设置"的请求 —— 可见性/布局不能在面板渲染中途改。这里在帧边界统一执行,
		// 与 Window 菜单、AI 通道 `ui.open` 落到同一条开关路径(AiActivatePanel)。
		{
			std::string textureAsset;
			Editor::TextureSettingsRequests::Kind textureKind = Editor::TextureSettingsRequests::Kind::Open;
			while (Editor::TextureSettingsRequests::Get().Take(textureAsset, textureKind))
			{
				const auto found = m_PanelRegistry.find("texture_settings");
				if (found == m_PanelRegistry.end() || !found->second)
					continue;
				static_cast<TextureSettingsPanel*>(found->second.get())->RequestOpenAsset(textureAsset,
					textureKind == Editor::TextureSettingsRequests::Kind::ResetDefaults);
				AiActivatePanel("texture_settings", nullptr);
			}
		}
		// AI 无障碍树:主窗口这一帧的节点从这里开始重新登记(见 WuiAccessibility)。
		Wui::WuiAccessibility::Get().BeginFrame("main", ctx.ViewportSize());
		// U25-M2:跨窗口拖放桥的落点登记每帧重建(独立窗口在本帧稍后登记自己).
		Editor::AssetDropBridge::Get().BeginFrame(ctx.Frame());
		// W9 review:文本焦点登记用宿主显式身份,不依赖无障碍开关。
		ctx.SetWindowKey("main");
		m_ViewportRect = {};
		ctx.ClearDropTarget();
		m_DropPreviewActive = false;
		// W9-2:Ctrl+Z/Y 与脚本编辑器的 buffer 撤销/重做冲突 —— 文本焦点活跃(上一帧快照)
		// 或焦点面板是脚本编辑器时,场景撤销/重做让位(WuiCodeEditor 自己消费这组键)。
		EditorPanel* const focusPanelAtFrameStart = FocusedPanel();
		const bool scriptEditorFocused = focusPanelAtFrameStart
			&& std::strncmp(focusPanelAtFrameStart->Id(), kScriptPanelPrefix, std::strlen(kScriptPanelPrefix)) == 0;
		const bool sceneUndoAllowed = !m_TextFocusLatched && !scriptEditorFocused;
		const bool undoKey = sceneUndoAllowed && ctx.Input().Ctrl && !ctx.Input().Shift
			&& ctx.IsKeyPressed(KeyCodes::Z);
		const bool redoKey = sceneUndoAllowed && ctx.Input().Ctrl
			&& (ctx.IsKeyPressed(KeyCodes::Y) || (ctx.Input().Shift && ctx.IsKeyPressed(KeyCodes::Z)));
		if (undoKey)
		{
			if (ctx.History().Undo())
				ctx.RecordOp("undo", "undo", ctx.History().UndoName(), "");
		}
		else if (redoKey)
		{
			if (ctx.History().Redo())
				ctx.RecordOp("undo", "redo", ctx.History().RedoName(), "");
		}
		const glm::vec2 viewport = ctx.ViewportSize();
		// D10-11(用户 2026-09-19"这种窗口也该抽象出来"):导入位置是**窗口级模态** ——
		// 开帧用 WuiModal 的输入封锁把整个客户区登记成遮挡区,后面画的所有面板照常显示但
		// 收不到命中(点击/悬停/拖放都走 HitTest);画模态本体之前 EndModalInputBlock 解开。
		// D10-15:shell 的四个模态(未保存/错误/打包/项目设置)同样是真模态,同一口径挡输入。
		// P4-UX10:启动询问是浮在停靠区上的**非模态**通知 —— 帧初先登记它的遮挡,
		// 下面的面板才不会吃到落在提示上的点击(提示内控件在绘制时仍然可命中)。
		if (!m_PendingFloatRestore.empty())
		{
			const float statusBarHeight = 22.0f;
			const Wui::WuiRect statusBarRect { 0, viewport.y - statusBarHeight, viewport.x, statusBarHeight };
			ctx.PushHoverBlocker(RestorePromptRect(statusBarRect));
		}
		const bool shellModalOpen = m_ImportModalOpen || m_Editor.ShowUnsavedModal()
			|| m_Editor.ShowErrorModal() || m_Editor.ShowCookingProgress()
			// P4-U13e:prefab 未保存改动的确认(关窗 / 进文档会话)也是窗口级模态。
			|| m_PrefabPendingAction != PrefabPendingAction::None
			// CPPT-6-ED-NEWSCRIPT:"新建 C++ 组件"模态同样封锁下层命中。
			|| m_NewCppScriptOpen
			// PROJ-1/T1:"新建项目"模态(名称/位置/模板/落盘)同样封锁下层命中。
			|| m_NewProjectOpen
			// PLUG-AUTH-1:"新建插件"模态(目标/模板/名称/ID/落盘)同样封锁下层命中。
			|| m_NewPluginOpen
			// PROJ-2/T1:项目启动器(启动态窗口级模态)同样封锁下层命中。
			|| m_Editor.ShowProjectLauncher();
		// P4-U6b:面板级模态(属性面板的"添加组件"居中窗口)与 shell 模态同一条封锁路径;
		// 渲染该面板之前会解开(RenderTabs),画完再封回去。
		const bool panelModalOpen = !m_PanelModalOwner.empty();
		if (shellModalOpen || panelModalOpen)
			Wui::BeginModalInputBlock(ctx);
		// 编辑器级四边停靠区:拖拽面板进入窗口边缘条带时,生成横跨整个编辑器的
		// 停靠区(而不是只切分鼠标所在的面板组)。需在渲染面板前判定,以便
		// RenderTabs 跳过面板内的落区逻辑。
		// P4-U13:prefab 编辑会话期间多一行横幅(在菜单栏下面),停靠区整体让出这一行。
		const bool prefabEditActive = m_Editor.IsEditingPrefab();
		const float prefabBarHeight = prefabEditActive ? kPrefabBarHeight : 0.0f;
		const float editorTop = 26.0f + m_AttachBarHeight + prefabBarHeight;
		// P4-UX6:底部留出状态栏(场景/选择/后端/帧率)——面板不再压到它上面。
		constexpr float statusBarHeight = 22.0f;
		const Wui::WuiRect statusBar { 0, viewport.y - statusBarHeight, viewport.x, statusBarHeight };
		const Wui::WuiRect editorArea { 0, editorTop, viewport.x,
			viewport.y - editorTop - statusBarHeight };
		std::string edgePayload;
		const bool edgeDragActive = ctx.IsDragActive(&edgePayload) && edgePayload.rfind("panel:", 0) == 0;
		// 独立窗口不参与四边停靠:它只能挂靠到顶部挂靠栏。
		const bool edgeDragIndependent = edgeDragActive
			&& IsIndependentPanel(edgePayload.substr(6));
		if (edgeDragActive && !edgeDragIndependent)
		{
			m_EdgeDockActive = false;
			m_EdgeDropZone = Wui::DropZone::Center;
			if (ctx.IsHovered(editorArea))
			{
				// 四边停靠判定带(用户 2026-09-16:110px 太大,压到面板中部)。
				constexpr float edgeBand = 64.0f;
				const glm::vec2 mouse = ctx.Input().MousePos;
				if (mouse.x - editorArea.X <= edgeBand) m_EdgeDropZone = Wui::DropZone::Left;
				else if (editorArea.X + editorArea.W - mouse.x <= edgeBand) m_EdgeDropZone = Wui::DropZone::Right;
				else if (mouse.y - editorArea.Y <= edgeBand) m_EdgeDropZone = Wui::DropZone::Top;
				else if (editorArea.Y + editorArea.H - mouse.y <= edgeBand) m_EdgeDropZone = Wui::DropZone::Bottom;
				m_EdgeDockActive = m_EdgeDropZone != Wui::DropZone::Center;
				if (m_EdgeDockActive)
				{
					ctx.DropTarget(editorArea, "panel:");
					// 原面板落区状态让位,避免同时高亮/同时落位。
					m_DropTargetPanel.clear();
					m_DropZone = Wui::DropZone::Center;
				}
			}
		}
		// 拖拽结束的落位在下一帧消费,因此拖拽不活跃时保留上一帧的边缘落区状态。
		// 窗口内浮动面板(停靠形态)画在停靠区之上,命中也要优先:先登记它们的矩形为
		// 遮挡区,下面的面板就不会同时响应;正在拖动的那一个除外(它跟随光标,且必须
		// 让下方的停靠落点能被命中)。
		for (const Wui::DockFloat& entry : m_Layout.Floating)
			if (!IsIndependentPanel(entry.Panel) && entry.Panel != m_MovingFloat)
				ctx.PushHoverBlocker(entry.Rect);
		// 已附加独立窗口时,主窗口作为"切换容器":当前标签是它,就显示它的内容。
		if (!m_ActiveWindowTag.empty())
		{
			const auto attached = std::find(m_AttachedPanels.begin(), m_AttachedPanels.end(), m_ActiveWindowTag);
			if (attached != m_AttachedPanels.end())
			{
				// M4-TEX P4(2026-09-25 补,口径同 P4-U6b):面板级模态的拥有者**不管渲染在停靠组里还是
				// 附加视图里**都必须能命中自己的模态 —— 画它之前解整窗输入封锁,画完封回去。
				// 此前只有 RenderTabs(停靠路径)做了这件事:附加视图里的面板模态的按钮
				// 全部落在封锁区里,点不动(实测:纹理设置的"回到默认设置"确认框)。
				const bool ownsPanelModal =
					!m_PanelModalOwner.empty() && m_PanelModalOwner == m_ActiveWindowTag;
				if (ownsPanelModal)
					Wui::EndModalInputBlock(ctx);
				RenderPanelContent(ctx, m_ActiveWindowTag, editorArea);
				if (ownsPanelModal)
					Wui::BeginModalInputBlock(ctx);
			}
			else
				m_ActiveWindowTag.clear();
		}
		if (m_ActiveWindowTag.empty())
			RenderNode(ctx, m_Layout.Root, editorArea);

		// 边缘落位提示必须在面板之后绘制:面板背景是不透明矩形,
		// 先画会被完全遮住(表现就是"没有目标位置提示")。
		if (m_EdgeDockActive)
		{
			Wui::WuiRect zone = editorArea;
			if (m_EdgeDropZone == Wui::DropZone::Left) zone.W = editorArea.W * 0.25f;
			else if (m_EdgeDropZone == Wui::DropZone::Right) { zone.X = editorArea.X + editorArea.W * 0.75f; zone.W = editorArea.W * 0.25f; }
			else if (m_EdgeDropZone == Wui::DropZone::Top) zone.H = editorArea.H * 0.25f;
			else if (m_EdgeDropZone == Wui::DropZone::Bottom) { zone.Y = editorArea.Y + editorArea.H * 0.75f; zone.H = editorArea.H * 0.25f; }
			// 预览延迟到 RenderFloating 里画(浮动面板之上),否则会被拖动中的面板盖住。
			m_DropPreviewRect = zone;
			m_DropPreviewActive = true;
		}
		// 下层绘制结束:解除遮挡,浮动面板/菜单/弹窗仍按真实光标命中。
		if (shellModalOpen)
			Wui::EndModalInputBlock(ctx);   // 与上面的 BeginModalInputBlock 成对
		else
			ctx.ClearHoverBlockers();       // 无模态:清掉窗口内浮动面板的遮挡区(原行为)
		// 模态之下还有浮动面板与菜单栏要画,继续挡住它们的命中(它们照常显示,
		// 但点不到);到画模态前再解除(见 DrawModals 前后)。
		if (shellModalOpen)
			Wui::BeginModalInputBlock(ctx);

		std::string payload;
		bool dropConsumed = false;
		if (ctx.AcceptDrop(&payload, "panel:"))
		{
			dropConsumed = true;
			if (payload.rfind("panel:", 0) == 0)
			{
				const std::string panel = payload.substr(6);
				const bool floating = m_Layout.IsFloating(panel);
				// 独立窗口只能挂靠到顶部挂靠栏:落到停靠区/面板一律忽略,窗口留在原处。
				if (IsIndependentPanel(panel))
				{
					m_DropTargetPanel.clear();
					m_DropZone = Wui::DropZone::Center;
					m_EdgeDockActive = false;
					m_EdgeDropZone = Wui::DropZone::Center;
					m_MovingFloat.clear();
				}
				else if (m_EdgeDockActive && (floating || m_Layout.Contains(panel)))
				{
					const std::string before = m_Layout.Serialize();
					const bool docked = floating
						? m_Layout.DockFloatingToRoot(panel, m_EdgeDropZone)
						: m_Layout.DockToRoot(panel, m_EdgeDropZone);
					if (docked)
						RecordDockChange(ctx, "drop", panel + " -> editor/" + ZoneName(m_EdgeDropZone), before);
				}
				else if (!m_DropTargetPanel.empty() && (floating || m_Layout.Contains(panel)))
				{
					const std::string before = m_Layout.Serialize();
					const bool docked = floating
						? m_Layout.DockFloating(panel, m_DropTargetPanel, m_DropZone)
						: m_Layout.MoveTab(panel, m_DropTargetPanel, m_DropZone);
					if (docked)
						RecordDockChange(ctx, "drop", panel + " -> " + m_DropTargetPanel + "/" + ZoneName(m_DropZone), before);
				}
			}
			// 只在落位消费后清空,避免下一帧 AcceptDrop 读取时目标已被清。
			// P3-1①:落点是拖拽的**正常出口**,这里一次性把外壳与 WuiContext 的拖拽
			// 状态全部归零(payload 刚被 AcceptDrop 消费,清掉不会丢落点)。
			EndPanelDrag(ctx);
		}
		// 拖拽结束且本帧未消费落点时,清除四边高亮:
		// 否则四边预览框会残留,表现成"启动/平时自动出现一个框"。
		if (!ctx.IsDragActive(nullptr))
		{
			m_EdgeDockActive = false;
			m_EdgeDropZone = Wui::DropZone::Center;
		}

		// ---- 面板拖拽状态机:停靠面板一旦进入拖拽即"拖出"为浮动窗口 ----
		// 拖到落点上释放会重新停靠(DockFloating*),否则保持浮动并跟随鼠标。
		// P3-1①:状态机的四个出口(落点消费 / 松手未落点 / Esc 取消 / 左键已抬)都必须
		// 让拖拽状态归零 —— 否则残留的 m_TabDragPanel/m_DragPanel 会在面板**回到停靠树**
		// 的那一帧再次执行拖出分支(实测表现:浮窗刚被 ✕ 关掉又跳回来,第一次点 ✕ 被吃掉)。
		// 左键**物理**状态是不依赖事件送达的判据:释放落在别的窗口/窗口外时,本窗口的
		// 释放事件可能永远不到(与 WuiInputCollector::SyncButtonsWithSystem、
		// FloatWindowHost::Render 同一口径)。
		const bool leftButtonDown = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
		{
			// Esc = 取消(左键可能还按着):一次把 WuiContext 与外壳状态都归零,
			// 之后即使按键还按着,也不会因为"按住即报"的 DragStart 被重新武装(见 RenderTabs)。
			std::string escPayload;
			const bool panelDragInFlight = !m_DragPanel.empty()
				|| (ctx.IsDragActive(&escPayload) && escPayload.rfind("panel:", 0) == 0);
			if (panelDragInFlight && ctx.WasKeyPressed(KeyCodes::Escape))
				EndPanelDrag(ctx);
		}
		std::string activePayload;
		if (leftButtonDown && ctx.IsDragActive(&activePayload) && activePayload.rfind("panel:", 0) == 0)
		{
			m_DragPanel = activePayload.substr(6);
			m_LastDragPos = ctx.Input().MousePos;
			// 只有"本次拖拽确实起手于该面板的标签页"时才允许拖出为独立窗口;
			// 否则残留的拖拽状态会在挂靠后立刻把面板再次浮出(表现为多出一个窗口)。
			if (!dropConsumed && m_AttachCooldownFrames <= 0 && m_Layout.Contains(m_DragPanel)
				&& m_TabDragPanel == m_DragPanel)
			{
				// T03 修订(用户 2026-09-16):子面板(停靠形态)拖出只在**主窗口内浮动**,
				// 不创建 OS 窗口 —— 独立 OS 窗口只属于声明为 Independent 的面板
				// (Widget Gallery / Input Map)。因此这里全部用主窗口客户区坐标。
				Wui::WuiRect source { m_LastDragPos.x - 40.0f, m_LastDragPos.y - 12.0f, 480.0f, 320.0f };
				if (const auto remembered = m_LastFloatRects.find(m_DragPanel); remembered != m_LastFloatRects.end())
				{
					source.W = remembered->second.W;
					source.H = remembered->second.H;
				}
				else
				{
					// 首次拖出:沿用原停靠区的尺寸作为初始浮动尺寸。
					std::vector<std::pair<Wui::PanelId, Wui::WuiRect>> rects;
					m_Layout.ComputeRects({ 0, 26, viewport.x, viewport.y - 26 }, &rects);
					for (const auto& entry : rects)
					{
						if (entry.first != m_DragPanel)
							continue;
						source.W = std::max(360.0f, entry.second.W * 0.75f);
						source.H = std::max(260.0f, entry.second.H * 0.75f);
						break;
					}
				}
				// 初始位置夹在编辑区里,标题栏必须可见(与 RenderFloatWindow 的约束一致)。
				source.W = std::min(source.W, std::max(320.0f, viewport.x - 16.0f));
				source.H = std::min(source.H, std::max(240.0f, viewport.y - editorTop - 16.0f));
				source.X = std::max(8.0f, std::min(source.X, std::max(8.0f, viewport.x - source.W - 8.0f)));
				source.Y = std::max(editorTop, std::min(source.Y, std::max(editorTop, viewport.y - source.H - 8.0f)));
				const std::string before = m_Layout.Serialize();
				if (m_Layout.Float(m_DragPanel, source))
				{
					RecordDockChange(ctx, "float", m_DragPanel, before);
					// 拖动期间面板跟随光标:抓取点固定在标题栏左侧(贴近原标签位置)。
					m_MovingFloat = m_DragPanel;
					m_FloatGrabOffset = { 40.0f, 12.0f };
					m_LastFloatRects[m_DragPanel] = source;
					// P3-1①:拖出已经完成,消费掉"本次拖拽的起手标签"标记 —— 同一个拖拽里
					// 面板若又回到停靠树(例如点浮窗 ✕ 收回停靠位),不允许再浮出一次。
					m_TabDragPanel.clear();
				}
				else if (std::getenv("WLD_TRACE_UI"))
				{
					WLD_CORE_WARN("[float] drag-out float() rejected for '{0}'", m_DragPanel);
				}
			}
			else if (std::getenv("WLD_TRACE_UI"))
			{
				// 诊断:拖拽已激活但没有走拖出分支(供脚本/人工排查用,按面板去重打印)。
				static std::string lastSkipped;
				if (lastSkipped != m_DragPanel)
				{
					lastSkipped = m_DragPanel;
					WLD_CORE_INFO("[float] drag-out skipped: panel={0} consumed={1} cooldown={2} contained={3} tabDrag={4}",
						m_DragPanel, dropConsumed ? 1 : 0, m_AttachCooldownFrames,
						m_Layout.Contains(m_DragPanel) ? 1 : 0, m_TabDragPanel);
				}
			}
			else if (!m_DragPanel.empty() && m_Layout.Contains(m_DragPanel))
			{
				// 拖出条件未满足:保持原状(用于人工排查,不打印高频日志)。
			}
		}
		else if (leftButtonDown && ctx.IsDragActive(nullptr))
		{
			// 其他类型的拖拽(file: 等)不参与面板浮动。
			m_DragPanel.clear();
			m_TabDragPanel.clear();
		}
		// P3-1① 修订(用户 2026-09-20 报"子栏不能拖拽了"):出口收口只在**物理左键已经抬起**
		// 时执行。WuiContext 的拖拽是两段式:按下沿的 BeginDrag 只进入 pending(记下
		// m_DragPressPos),移动超过 4px 后(WuiContext::EndFrame / 下一次 BeginDrag)才置
		// m_Dragging —— 期间 IsDragActive() 恒为 false。而 m_TabDragPanel / m_MovingFloat 在
		// **按下沿当帧**就已经写入;若这时按"拖拽结束"收口,ClearPanelDragIdentity() 会清掉
		// 起手标记、ctx.EndDrag() 会直接撤销 pending —— 面板再也进不了拖拽(实测表现:按住
		// 标签拖动,面板完全不动)。pending 期间这些身份成员是拖拽必需的,所以收口条件加上
		// !leftButtonDown:松手 / 释放事件丢失(由 WuiInputCollector::SyncButtonsWithSystem
		// 补发释放)时左键已抬起,收口照常发生,P3-1 的"拖出后一次点 ✕ 就关闭"不受影响。
		else if (!leftButtonDown
			&& (!m_DragPanel.empty() || !m_MovingFloat.empty() || !m_TabDragPanel.empty()))
		{
			// 拖拽结束(松手 / 释放事件丢失):清"拖拽身份"成员 —— 拖拽起点标记残留会让
			// 已经收回停靠的面板立刻再次浮出。**落点成员必须留到下一帧**:WuiContext 下一帧
			// 才交付 AcceptDrop,落位用的就是这一帧武装好的目标;这里清掉会把落点整个吃掉。
			ClearPanelDragIdentity();
			// ctx 侧的拖拽结束由 WuiContext::EndFrame 负责:它要把"本帧已武装的落点"转成
			// 可消费的 m_DropAccepted(见 WuiContext.cpp 的 release 分支)。在"释放沿"这一帧
			// 调 EndDrag 会把这次落点整个吃掉(拖回停靠位失效),所以只在 ctx 已经不在拖拽时
			// 清残留 payload。
			if (!ctx.IsDragActive(nullptr))
				ctx.EndDrag();
		}

		// 浮动面板绘制在停靠区之上、菜单/模态之下。
		RenderFloating(ctx);

		// 菜单栏最后绘制:其弹出面板需要盖在所有停靠面板之上。
		DrawMenuBar(ctx);
		// 挂靠栏:横条形式(排在菜单栏下方),独立窗口可挂靠至此。
		DrawAttachBar(ctx);
		// P4-U13:prefab 编辑横幅(菜单栏之下、停靠区之上;激活时才存在这一行)。
		if (prefabEditActive)
			DrawPrefabBar(ctx, 26.0f + m_AttachBarHeight);

		// D10-11/D10-15:模态绘制前解除遮挡(对话框自身要能命中);画完再清一次,
		// 确保本帧结束时不留 blocker(下一帧开帧也会清,这里是双保险)。
		if (shellModalOpen)
			Wui::EndModalInputBlock(ctx);
		else
			ctx.ClearHoverBlockers();
		DrawModals(ctx);
		ctx.ClearHoverBlockers();

		std::string dragPayload;
		if (ctx.IsDragActive(&dragPayload))
		{
			std::string label = dragPayload;
			if (dragPayload.rfind("panel:", 0) == 0) label = "停靠面板: " + dragPayload.substr(6);
			else if (dragPayload.rfind("file:", 0) == 0) label = "移动文件: " + dragPayload.substr(5);
			ctx.PushOverlay();
			Label(ctx, ctx.Input().MousePos + glm::vec2 { 14, 14 }, label, m_Theme.Text, 13.0f);
			ctx.PopOverlay();
		}

		// 遍历结束后再执行标签关闭请求:这样"关闭组内最后一个标签"引起的塌缩
		// 不会打断正在进行的渲染遍历(修"关闭 Saves/Levels 等标签崩溃")。
		for (const Wui::PanelId& panel : m_PendingPanelCloses)
		{
			const std::string before = m_Layout.Serialize();
			if (m_Layout.RemoveTab(panel))
				RecordDockChange(ctx, "close", panel, before);
		}
		m_PendingPanelCloses.clear();
		// PLUG-T3b:插件卸载后把对应面板的窗口/标签/布局记录收干净(注册表已没有它)。
		ClosePluginPanelsNotInRegistry();

		// P3-1①:挂靠标签拖拽(不经过 WuiContext)的收口 —— 左键已经抬起,而
		// DrawAttachBar 这一帧没收到释放沿(释放落在别的窗口/窗口外)时,残留会让顶栏
		// 一直停在"正在拖标签"的状态。正常路径上 DrawAttachBar 已消费并清空,这里不会再触发;
		// 只收口"已经在拖"的状态:m_AttachTagPress 要留给 DrawAttachBar 的"单击=切换视图"
		// 释放沿消费(脚本注入的点击没有物理按键,不能在这里被判成残留)。
		if (!leftButtonDown && !m_AttachTagDrag.empty())
		{
			m_AttachTagPress.clear();
			m_AttachTagDrag.clear();
		}

		// W9-2:本帧(含所有独立窗口)结束时的文本焦点快照。UI 帧开始时各窗口的登记
		// 已被 BeginFrame 清空,所以帧内 Ctrl+Z/Y 判定必须用"上一帧结束"的这份状态。
		// P4-UX4:悬停提示最后画 —— 面板/模态都已登记完,这里统一画到 overlay 层。
		DrawStatusBar(ctx, statusBar);
		// P4-UX10:启动询问贴状态栏上方(非模态;不压暗、不挡操作)。
		DrawRestorePrompt(ctx, statusBar);
		// M4-S2:菜单栏下拉的延后批次在**模态之后**补画(它们与 U29 的下拉弹层同属"永远在最上层"
		// 的那一类);tooltip 仍在最后画,所以提示不会被菜单压住。
		FlushDeferredMenuDraws(ctx);
		Wui::DrawTooltip(ctx, m_Theme);
		// U26:独立 OS 窗口放在**最后**渲染 —— 它们会把无障碍树的"当前窗口"切到自己,
		// 放在前面会让菜单栏/状态栏等主窗口节点挂错窗口键(见 RenderIndependentWindows)。
		RenderIndependentWindows(ctx);
		m_TextFocusLatched = Wui::WuiTextFocus::Get().Active();
	}

	// PROJ-3/T1:纯启动器模式的整页渲染。与普通编辑器**共用同一批模态绘制函数**
	// (DrawModals:新建项目向导 / 项目启动器 / 错误框),但不画菜单栏、挂靠栏、停靠区、
	// 状态栏与独立窗口 —— 启动器页就是窗口的全部内容;停靠布局在构造期就没有装载,
	// 也不会有任何"顺手保存布局"的路径(见 SaveLayout 的启动器模式直接返回)。
	void EditorShell::RenderLauncherPage(Wui::WuiContext& ctx)
	{
		// 无障碍帧照常开始:启动器节点就是 a11y 树的全部内容(AI 通道 ui.tree 读的是它)。
		Wui::WuiAccessibility::Get().BeginFrame("main", ctx.ViewportSize());
		Editor::AssetDropBridge::Get().BeginFrame(ctx.Frame());
		ctx.SetWindowKey("main");
		ctx.ClearDropTarget();

		DrawModals(ctx);
		ctx.ClearHoverBlockers();

		// 状态提示(打开项目失败 / Runtime 缺失等)在启动器页也要看得见:用窗口底部
		// 一条当它的落点(与普通路径的状态栏同一份实现,只是这里没有真状态栏)。
		const glm::vec2 viewport = ctx.ViewportSize();
		DrawStatusNotice(ctx, { 0.0f, viewport.y - 22.0f, viewport.x, 22.0f });
		Wui::DrawTooltip(ctx, m_Theme);

		// 文本焦点快照与普通路径同源(下一个 UI 帧的输入判定要用)。
		m_TextFocusLatched = Wui::WuiTextFocus::Get().Active();
	}

	// P4-UX6:状态栏 —— 一眼看到"当前场景有没有改、选中了什么、跑在哪个后端、多少帧"。
	// 全部取自既有状态(文档/选择/Input().FPS),不做任何额外计算或分配。
	// M4-S2 遗留②:菜单栏下拉的延后收口。调用点在**所有面板与模态之后、tooltip 之前**
	// (OnUiFrame 的帧末)。没有菜单打开时批次为空,零成本。
	void EditorShell::FlushDeferredMenuDraws(Wui::WuiContext& ctx)
	{
		if (m_DeferredMenuCommands.empty())
			return;
		ctx.PushOverlay();
		std::vector<Wui::WuiDrawCommand>& target = ctx.Commands();
		for (const Wui::WuiDrawCommand& command : m_DeferredMenuCommands)
			target.push_back(command);
		ctx.PopOverlay();
		m_DeferredMenuCommands.clear();
	}

	void EditorShell::DrawStatusBar(Wui::WuiContext& ctx, const Wui::WuiRect& rect)
	{
		ctx.Commands().push_back({ Wui::WuiDrawKind::Rect, rect, m_Theme.PanelHeader, 0.0f });
		ctx.Commands().push_back({ Wui::WuiDrawKind::Rect, { rect.X, rect.Y, rect.W, 1.0f },
			m_Theme.Border, 0.0f });

		EditorDocument& document = m_Editor.GetDocument();
		const std::string sceneName = document.HasPath()
			? document.GetPath().filename().string() : std::string("Untitled");
		std::string left = std::string(Wui::Tr("status.scene", "Scene")) + ": " + sceneName;
		if (document.IsDirty())
			left += " *";
		Entity selected = GetSelectedEntity();
		left += "   |   " + std::string(Wui::Tr("status.selection", "Selection")) + ": ";
		if (selected && selected.HasComponent<TagComponent>())
			left += selected.GetComponent<TagComponent>().Tag;
		else
			left += Wui::Tr("status.selection.none", "none");
		if (!m_ActiveWindowTag.empty())
			left += "   |   " + std::string(Wui::Tr("status.view", "View")) + ": " + PanelTitle(m_ActiveWindowTag);

		const float fps = ctx.Input().FPS;
		char right[96] = {};
		std::snprintf(right, sizeof(right), "%s   %.1f FPS (%.1f ms)", Renderer::GetBackendName().c_str(),
			static_cast<double>(fps), fps > 0.0f ? 1000.0 / static_cast<double>(fps) : 0.0);
		const std::string rightText = right;
		const float rightWidth = ctx.MeasureTextWidth(rightText, 12.0f);
		const float textY = rect.Y + (rect.H - 14.0f) * 0.5f;
		Wui::Label(ctx, { rect.X + 10.0f, textY }, left, m_Theme.TextMuted, 12.0f);

		// ---- CPPT-3:Game 模块热重载状态(loaded / unloaded / reloading / rolled-back)----
		// 常驻显示:模块未加载窗口(启动失败 / module.unload / reloading)必须显式提示 ——
		// 该窗口里 Lua 存根生成被拒绝、C++ 组件下拉为空,用户要能一眼看出"是模块没加载"。
		// 无障碍:`cppmodule.status`(kind=status,value = 稳定字面量)供 AI/探针断言;
		// 发生过模块动作后追加 `cppmodule.result` 反馈节点(module.reload 的 ok/回滚/计数)。
		{
			const EditorLayer::CppModuleStatus& module = m_Editor.GetCppModuleStatus();
			const std::string moduleTitle = std::string(Wui::Tr("status.cppmodule", "C++ module")) + ": "
				+ CppModuleStateText();
			const std::string moduleHint = CppModuleHintText();
			std::string chipText = moduleHint.empty() ? moduleTitle : (moduleTitle + " — " + moduleHint);
			const float leftWidth = ctx.MeasureTextWidth(left, 12.0f);
			const float rightReserve = rightWidth + 24.0f;
			float chipX = rect.X + 10.0f + leftWidth + 12.0f;
			float chipWidth = ctx.MeasureTextWidth(chipText, 12.0f) + 12.0f;
			// 空间不足(窄窗口 + 长场景名)时按字节截断,不盖住右侧后端/FPS 文本。
			const float limit = rect.X + rect.W - rightReserve;
			if (chipX + chipWidth > limit)
			{
				std::string cut = chipText;
				while (cut.size() > 8 && ctx.MeasureTextWidth(cut + "…", 12.0f) + 12.0f > limit - chipX)
				{
					cut.pop_back();
					while (!cut.empty() && (static_cast<unsigned char>(cut.back()) & 0xC0u) == 0x80u)
						cut.pop_back();
				}
				chipText = cut.empty() ? std::string() : (cut + "…");
				chipWidth = std::max(0.0f, limit - chipX);
			}
			const bool moduleLoaded = m_Editor.IsCppModuleLoaded();
			const Wui::WuiRect chip { chipX, rect.Y + 2.0f, std::max(0.0f, chipWidth), rect.H - 4.0f };
			if (!chipText.empty() && chip.W > 4.0f)
				Wui::Label(ctx, { chip.X + 6.0f, textY }, chipText,
					moduleLoaded ? m_Theme.TextMuted : m_Theme.Warning, 12.0f);
			// a11y Tooltip = 本地化提示 + 引擎消息 + 最近一次诊断(按行截断,避免超长)。
			std::string moduleTooltip = CppModuleHintText();
			if (!module.Message.empty())
				moduleTooltip += (moduleTooltip.empty() ? "" : "\n") + module.Message;
			size_t shown = 0;
			for (const std::string& diagnostic : module.Diagnostics)
			{
				// CPPT-3-FIX1:迁移诊断(类型变化 / 字段删除 / 新增字段)逐条进 tooltip;
				// 上限 10 条避免无限长,超出只报剩余条数。
				if (shown >= 10)
					break;
				++shown;
				moduleTooltip += "\n" + diagnostic;
			}
			if (module.Diagnostics.size() > shown)
			{
				moduleTooltip += "\n" + std::to_string(module.Diagnostics.size() - shown) + " "
					+ Wui::Tr("status.cppmodule.diagnostics.more",
						"more migration diagnostics (see the AI channel module.status)");
			}
			Wui::WuiAccessNode moduleNode;
			moduleNode.Id = Wui::HashId("cppmodule.status");
			moduleNode.Window = "main";
			moduleNode.Panel = "shell";
			moduleNode.Kind = "status";
			moduleNode.Label = Wui::Tr("status.cppmodule.label", "C++ module status");
			moduleNode.Value = m_Editor.CppModuleStateName();
			moduleNode.Tooltip = moduleTooltip;
			moduleNode.Rect = chip;
			moduleNode.Enabled = false;
			moduleNode.Interactive = false;
			moduleNode.Visible = true;
			Wui::WuiAccessibility::Get().Register(moduleNode);
			// 反馈节点:只在发生过模块动作后出现(module.reload / module.unload 的最终结果)。
			if (module.Sequence > 0)
			{
				std::string summary = std::string("ok=") + (module.Ok ? "true" : "false")
					+ " rolledBack=" + (module.RolledBack ? "true" : "false")
					+ " abi=" + std::to_string(module.AbiVersion)
					+ " drained=" + std::to_string(module.InstancesDrained)
					+ " restored=" + std::to_string(module.InstancesRestored)
					+ " diagnostics=" + std::to_string(module.Diagnostics.size());
				Wui::WuiAccessNode resultNode;
				resultNode.Id = Wui::HashId("cppmodule.result");
				resultNode.Window = "main";
				resultNode.Panel = "shell";
				resultNode.Kind = "status";
				resultNode.Label = Wui::Tr("status.cppmodule.result", "C++ module reload result");
				resultNode.Value = summary;
				resultNode.Tooltip = moduleTooltip;
				resultNode.Rect = chip;
				resultNode.Enabled = false;
				resultNode.Interactive = false;
				resultNode.Visible = true;
				Wui::WuiAccessibility::Get().Register(resultNode);
			}
		}
		Wui::Label(ctx, { rect.X + rect.W - rightWidth - 10.0f, textY }, rightText,
			m_Theme.TextMuted, 12.0f);
		// 无障碍:脚本/AI 通道可直接读整条状态(不依赖像素)。
		Wui::WuiAccessNode node;
		node.Id = Wui::HashId("shell.status");
		node.Window = "main";
		node.Panel = "shell";
		node.Kind = "status";
		node.Label = "editor status bar";
		node.Value = left + "   |   " + rightText;
		node.Rect = rect;
		node.Enabled = false;
		node.Interactive = false;
		node.Visible = true;
		Wui::WuiAccessibility::Get().Register(node);
		DrawStatusNotice(ctx, rect);
	}

	// CPPT-3:Game 模块状态显示文案(稳定字面量 → 本地化显示;`rolled-back` 用连字符口径)。
	std::string EditorShell::CppModuleStateText() const
	{
		switch (m_Editor.GetCppModuleStatus().State)
		{
			case EditorLayer::CppModuleState::Loaded: return Wui::Tr("status.cppmodule.loaded", "loaded");
			case EditorLayer::CppModuleState::Unloaded: return Wui::Tr("status.cppmodule.unloaded", "unloaded");
			case EditorLayer::CppModuleState::Reloading: return Wui::Tr("status.cppmodule.reloading", "reloading");
			case EditorLayer::CppModuleState::RolledBack: return Wui::Tr("status.cppmodule.rolled_back", "rolled-back");
		}
		return Wui::Tr("status.cppmodule.unloaded", "unloaded");
	}

	// 未加载窗口的显式提示(unloaded / reloading 都提示"先重建、再重载");loaded = 无提示。
	std::string EditorShell::CppModuleHintText() const
	{
		switch (m_Editor.GetCppModuleStatus().State)
		{
			case EditorLayer::CppModuleState::Unloaded:
				return Wui::Tr("status.cppmodule.hint.unloaded",
					"Game.dll is not loaded — use File ▶ Build & Reload C++ Module");
			case EditorLayer::CppModuleState::Reloading:
				return Wui::Tr("status.cppmodule.hint.reloading",
					"Game.dll unloaded for rebuild — use File ▶ Build & Reload C++ Module to build and load it");
			case EditorLayer::CppModuleState::RolledBack:
				return Wui::Tr("status.cppmodule.hint.rolled_back",
					"the new build was rejected; the previous Game.dll is loaded again (see diagnostics)");
			case EditorLayer::CppModuleState::Loaded:
			{
				// CPPT-3-FIX1:加载成功但发生了属性迁移(类型变化 / 字段删除 / 新增字段)时,
				// 状态栏常驻显示诊断条数(逐条文本在 tooltip / AI module.status / 加载提示里)。
				const size_t diagnostics = m_Editor.GetCppModuleStatus().Diagnostics.size();
				if (diagnostics > 0)
					return std::to_string(diagnostics) + " " + Wui::Tr("status.cppmodule.hint.diagnostics",
						"field migration diagnostics — hover here for the details");
				break;
			}
		}
		return std::string();
	}

	// CPPT-3-FIX1:模块动作的可见反馈:菜单项与 AI `module.reload` 共用一条(此前只有菜单单独
	// Notify,AI 通道没有提示;诊断条数/首条文本要能被用户看到,不能只藏在 a11y 节点里)。
	void EditorShell::NotifyCppModuleResult()
	{
		const EditorLayer::CppModuleStatus& module = m_Editor.GetCppModuleStatus();
		std::string text = CppModuleStateText();
		if (!module.Message.empty())
			text += " — " + module.Message;
		if (!module.Diagnostics.empty())
		{
			// 诊断文本是引擎的英文 canonical 口径(含实体/字段名),不翻译;只翻译前缀。
			text += " — " + std::to_string(module.Diagnostics.size()) + " "
				+ Wui::Tr("notice.cppmodule.diagnostics", "field migration diagnostics") + ": "
				+ module.Diagnostics.front();
		}
		Notify(text);
	}

	void EditorShell::RenderNode(Wui::WuiContext& ctx, Wui::DockNode& node, const Wui::WuiRect& area)
	{
		if (node.IsTabs())
			RenderTabs(ctx, node, area);
		else
			RenderSplit(ctx, node, area);
	}

	// P4-UX10:状态栏提示(人类交互细节都是刻意的,别当装饰):
	//   · 鼠标停在提示上 = **暂停倒计时**,不会读到一半消失(用户明确要求);
	//   · 鼠标移开 = 再给 1.2s 宽限,然后 0.25s 淡出(即使悬停超过 4s 再移开也一样);
	//   · 悬停时右侧出现 ×,点提示或按 Esc 立即关闭(可撤销/可控);
	//   · 悬停期间光标变手型,告诉用户"这块是可以点的"。
	void EditorShell::DrawStatusNotice(Wui::WuiContext& ctx, const Wui::WuiRect& statusBar)
	{
		if (!m_Notice.Active || m_Notice.Text.empty())
			return;
		if (ctx.WasKeyPressed(KeyCodes::Escape))
		{
			m_Notice.Active = false;
			return;
		}
		const double now = ShellNowSeconds();
		const float textWidth = ctx.MeasureTextWidth(m_Notice.Text, 12.0f);
		const Wui::WuiRect chip { statusBar.X + statusBar.W - 210.0f - textWidth, statusBar.Y + 2.0f,
			textWidth + 30.0f, statusBar.H - 4.0f };
		const bool hovered = ctx.IsHovered(chip);
		if (hovered)
			ctx.SetCursor(Wui::WuiCursor::Hand);
		if (!m_Notice.Timing.Update(now, hovered, 4.0, 1.2, 0.25))
		{
			m_Notice.Active = false;
			return;
		}
		const float alpha = m_Notice.Timing.Alpha;

		const Wui::WuiColor background { m_Theme.ActiveBg.R, m_Theme.ActiveBg.G, m_Theme.ActiveBg.B,
			(hovered ? 0.95f : 0.75f) * alpha };
		ctx.Commands().push_back({ Wui::WuiDrawKind::Rect, chip, background, 3.0f });
		ctx.Commands().push_back({ Wui::WuiDrawKind::RectOutline, chip,
			{m_Theme.Accent.R, m_Theme.Accent.G, m_Theme.Accent.B, alpha}, 3.0f, 1.0f });
		ctx.Commands().push_back({ Wui::WuiDrawKind::Text, { chip.X + 10.0f, chip.Y + 3.0f, 0, 0 },
			{m_Theme.Text.R, m_Theme.Text.G, m_Theme.Text.B, alpha}, 0.0f, 1.0f, m_Notice.Text, 12.0f, false });
		if (hovered)
		{
			const Wui::WuiRect close { chip.X + chip.W - 18.0f, chip.Y + 2.0f, 14.0f, chip.H - 4.0f };
			ctx.Commands().push_back({ Wui::WuiDrawKind::Text, { close.X + 3.0f, close.Y + 2.0f, 0, 0 },
				m_Theme.TextMuted, 0.0f, 1.0f, "x", 12.0f, false });
			if (ctx.IsClicked(close) || ctx.IsClicked(chip))
			{
				m_Notice.Active = false;
				return;
			}
		}
		// 无障碍:脚本/读屏能读到这条提示,也能点它关掉。
		Wui::WuiAccessNode node;
		node.Id = Wui::HashId("shell.notice");
		node.Window = "main";
		node.Panel = "shell";
		node.Kind = "notice";
		node.Label = Wui::Tr("notice.label", "Status notice");
		node.Value = m_Notice.Text;
		node.Rect = chip;
		node.Enabled = true;
		node.Interactive = true;
		node.Visible = true;
		node.Tooltip = Wui::Tr("notice.tooltip", "Click to dismiss (Esc). It stays while the pointer is on it.");
		Wui::WuiAccessibility::Get().Register(node);
	}

	// P4-UX10:启动询问 —— **非模态通知**,贴在状态栏正上方(与状态栏同一条右边界)。
	// 之前这里做成了模态对话框 + 全屏遮罩,用户反馈"怎么有个选项霸屏"(2026-09-20):
	// 询问必须让人能一边干活一边决定,所以改成通知条 —— 不压暗、不挡面板操作,
	// 只挡自己那一小块(登记 hover blocker);不回答就一直留着,回答/关闭即消失。
	Wui::WuiRect EditorShell::RestorePromptRect(const Wui::WuiRect& statusBar) const
	{
		constexpr float width = 520.0f;
		constexpr float height = 84.0f;
		return { statusBar.X + statusBar.W - width - 12.0f, statusBar.Y - height - 8.0f, width, height };
	}

	void EditorShell::DrawRestorePrompt(Wui::WuiContext& ctx, const Wui::WuiRect& statusBar)
	{
		if (m_PendingFloatRestore.empty())
			return;

		const Wui::WuiRect panel = RestorePromptRect(statusBar);
		const float height = panel.H;
		// 询问条也按时间收(用户 2026-09-20:"这个提示不会随时间消失"):停留 15s(要读要选,
		// 比状态栏提示的 4s 长得多),悬停暂停、移开给宽限、最后整条淡出。
		const double now = ShellNowSeconds();
		if (m_RestorePromptTiming.ShownAt <= 0.0)
			m_RestorePromptTiming.ShownAt = now;   // 首帧才开始计时(0 会被当成"已经过了很久")
		const bool promptHovered = ctx.IsHovered(panel);
		if (!m_RestorePromptTiming.Update(now, promptHovered, 15.0, 1.5, 0.3))
		{
			// 超时 = 本次不恢复(和 × / Esc 同义):记录保留,下次启动还会问。
			m_PendingFloatRestore.clear();
			m_RestorePromptTiming = TimedNotice {};
			PushNotice(Wui::Tr("notice.restore.skipped", "Skipped restoring last session's windows"));
			WLD_CORE_INFO("[float] restore prompt timed out (windows kept in layout for next start)");
			return;
		}
		const float alpha = m_RestorePromptTiming.Alpha;
		// 淡出阶段连控件一起淡:复制一份主题,把颜色 alpha 缩掉再交给按钮/勾选框。
		Wui::WuiTheme promptTheme = m_Theme;
		if (alpha < 0.999f)
		{
			auto fade = [alpha](Wui::WuiColor& color) { color.A *= alpha; };
			fade(promptTheme.PanelBg); fade(promptTheme.PanelHeader); fade(promptTheme.Border);
			fade(promptTheme.Text); fade(promptTheme.TextMuted); fade(promptTheme.TextDisabled);
			fade(promptTheme.Accent); fade(promptTheme.ButtonBg); fade(promptTheme.ButtonHover);
			fade(promptTheme.ActiveBg);
		}
		ctx.PushOverlay();
		ctx.Commands().push_back({ Wui::WuiDrawKind::Rect, panel, promptTheme.PanelBg, 5.0f });
		ctx.Commands().push_back({ Wui::WuiDrawKind::RectOutline, panel, promptTheme.Border, 5.0f, 1.0f });
		ctx.Commands().push_back({ Wui::WuiDrawKind::Rect, { panel.X, panel.Y, panel.W, 3.0f },
			promptTheme.Accent, 2.0f });

		const size_t count = m_PendingFloatRestore.size();
		std::string names;
		for (size_t i = 0; i < count && i < 3; ++i)
			names += std::string(i == 0 ? "" : ", ") + std::string(PanelTitle(m_PendingFloatRestore[i].Panels.front()));
		if (count > 3)
			names += Wui::Tr("modal.restore.more", " …");
		Wui::Label(ctx, { panel.X + 12.0f, panel.Y + 10.0f },
			Wui::Tr("modal.restore.body", "Last session left these windows open:"), promptTheme.Text, 13.0f);
		Wui::Label(ctx, { panel.X + 12.0f, panel.Y + 28.0f }, names, promptTheme.TextMuted, 12.0f);

		// × 关闭 = 本次不恢复(下次再问;想彻底不问就勾"记住")。
		const Wui::WuiRect closeRect { panel.X + panel.W - 20.0f, panel.Y + 6.0f, 14.0f, 14.0f };
		const bool closeHovered = ctx.IsHovered(closeRect);
		if (closeHovered)
			ctx.Commands().push_back({ Wui::WuiDrawKind::Rect, closeRect, promptTheme.ButtonHover, 2.0f });
		ctx.Commands().push_back({ Wui::WuiDrawKind::Text, { closeRect.X + 3.0f, closeRect.Y - 1.0f, 0, 0 },
			closeHovered ? promptTheme.Text : promptTheme.TextMuted, 0.0f, 1.0f, "x", 12.0f, false });

		const float rowY = panel.Y + height - 28.0f;
		const bool tabsClicked = Wui::Button(ctx, Wui::HashId("restore.prompt.tabs"),
			{ panel.X + 12.0f, rowY, 132.0f, 22.0f },
			Wui::Tr("modal.restore.tabs", "Restore as Tabs"), promptTheme);
		const bool layoutClicked = Wui::Button(ctx, Wui::HashId("restore.prompt.layout"),
			{ panel.X + 150.0f, rowY, 140.0f, 22.0f },
			Wui::Tr("modal.restore.layout", "Restore Layout"), promptTheme);
		const bool noneClicked = Wui::Button(ctx, Wui::HashId("restore.prompt.none"),
			{ panel.X + 296.0f, rowY, 104.0f, 22.0f },
			Wui::Tr("modal.restore.none", "Don't Restore"), promptTheme);
		const Wui::LocalizedLabel remember = Wui::TrLabel("modal.restore.remember", "Remember");
		Wui::Checkbox(ctx, Wui::HashId("restore.prompt.remember"),
			{ panel.X + panel.W - 112.0f, rowY + 1.0f, 104.0f, 20.0f },
			remember.Text, remember.Term, m_RestoreAskRemember, promptTheme);

		const bool escape = ctx.WasKeyPressed(KeyCodes::Escape);
		const bool dismiss = closeHovered && ctx.Input().MouseClicked[0];
		ctx.PopOverlay();

		// 无障碍:整条询问登记成节点(动作按钮/勾选框由控件自己登记,脚本都能点)。
		Wui::WuiAccessNode node;
		node.Id = Wui::HashId("shell.restore.prompt");
		node.Window = "main";
		node.Panel = "shell";
		node.Kind = "notice";
		node.Label = Wui::Tr("modal.restore.title", "Restore Independent Windows");
		node.Value = names;
		node.Rect = panel;
		node.Enabled = true;
		node.Interactive = false;
		node.Visible = true;
		node.Tooltip = Wui::Tr("modal.restore.hint",
			"Top-bar tabs keep the desktop clean and never steal focus; floating windows come back without focus too.");
		Wui::WuiAccessibility::Get().Register(node);

		if (!(tabsClicked || layoutClicked || noneClicked || dismiss || escape))
			return;

		std::vector<PendingFloatRestore> pending = std::move(m_PendingFloatRestore);
		m_PendingFloatRestore.clear();
		Editor::EditorPreferences& preferences = Editor::EditorPreferences::Get();
		if (tabsClicked)
		{
			RestoreIndependentWindows(pending, true);
			if (m_RestoreAskRemember)
				preferences.SetRestoreWindows(Editor::RestoreWindowsMode::Tabs);
		}
		else if (layoutClicked)
		{
			RestoreIndependentWindows(pending, false);
			if (m_RestoreAskRemember)
				preferences.SetRestoreWindows(Editor::RestoreWindowsMode::Layout);
		}
		else
		{
			// 不恢复 / × / Esc:本次跳过;勾了"记住"才清掉记录并停止再问。
			if (m_RestoreAskRemember)
			{
				preferences.SetRestoreWindows(Editor::RestoreWindowsMode::None);
				m_Layout.Floating.clear();
				SaveLayout();
			}
			PushNotice(Wui::Tr("notice.restore.skipped", "Skipped restoring last session's windows"));
			WLD_CORE_INFO("[float] restore declined ({0} windows kept in layout)", pending.size());
		}
	}

	void EditorShell::RenderSplit(Wui::WuiContext& ctx, Wui::DockNode& node, const Wui::WuiRect& area)
	{
		const bool row = node.Direction == Wui::WuiDirection::Row;
		const size_t count = node.Children.size();
		if (count == 0)
			return;
		const float total = row ? area.W : area.H;
		float cursor = 0;
		for (size_t i = 0; i < count; ++i)
		{
			float size = 0;
			if (count == 1) size = total;
			else if (count == 2) size = i == 0 ? total * node.Ratio : total - total * node.Ratio;
			else size = total / static_cast<float>(count);

			Wui::WuiRect childArea = row
				? Wui::WuiRect { area.X + cursor, area.Y, size, area.H }
				: Wui::WuiRect { area.X, area.Y + cursor, area.W, size };
			RenderNode(ctx, node.Children[i], childArea);
			cursor += size;

			if (i + 1 < count)
			{
				// 分隔条走组件(Splitter):高亮/光标/命中统一。
				const Wui::WuiRect splitterArea = row
					? Wui::WuiRect { area.X + cursor - 2, area.Y, 4, area.H }
					: Wui::WuiRect { area.X, area.Y + cursor - 2, area.W, 4 };
				const Wui::SplitterResult split = Wui::Splitter(ctx, splitterArea, row, m_Theme, m_DragSplitNode == &node);
				if (split.Hovered && ctx.IsClicked(splitterArea))
				{
					m_SplitterDragging = true;
					m_DragSplitNode = &node;
					m_DragSplitRow = row;
					m_SplitterBeforeJson = m_Layout.Serialize();
				}
			}
		}

		if (m_DragSplitNode == &node && m_SplitterDragging)
		{
			const float mouse = row ? ctx.Input().MousePos.x - area.X : ctx.Input().MousePos.y - area.Y;
			node.Ratio = std::max(0.05f, std::min(0.95f, mouse / std::max(1.0f, total)));
			if (ctx.Input().MouseReleased[0])
			{
				m_SplitterDragging = false;
				m_DragSplitNode = nullptr;
				RecordDockChange(ctx, "resize", "", m_SplitterBeforeJson);
			}
		}
	}

	void EditorShell::RenderTabs(Wui::WuiContext& ctx, Wui::DockNode& node, const Wui::WuiRect& area)
	{
		const float tabH = 24;
		// 位置记忆:记下每个停靠标签所在组的首个面板(菜单重新打开该面板时回到这一组)。
		if (!node.Panels.empty())
			for (const std::string& panel : node.Panels)
				m_LastDockAnchors[panel] = node.Panels.front();
		// 停靠标签栏统一走组件(WuiChrome::DockTabBar),主窗口与独立窗口外观/交互一致。
		std::vector<Wui::DockTab> tabs;
		tabs.reserve(node.Panels.size());
		for (size_t i = 0; i < node.Panels.size(); ++i)
			tabs.push_back({ Wui::HashId(("tab." + node.Panels[i]).c_str()), PanelTitle(node.Panels[i]), i == node.Active });
		const Wui::DockTabBarResult tabResult = Wui::DockTabBar(ctx, { area.X, area.Y, area.W, tabH }, tabs, m_Theme);

		if (tabResult.Clicked >= 0 && static_cast<size_t>(tabResult.Clicked) < node.Panels.size())
		{
			m_Layout.Activate(node.Panels[tabResult.Clicked]);
			// 单纯点击标签不是拖拽:清掉可能残留的拖拽来源记录。
			m_TabDragPanel.clear();
		}
		if (tabResult.Closed >= 0 && static_cast<size_t>(tabResult.Closed) < node.Panels.size())
		{
			// 只登记请求:此刻正在遍历停靠树,直接删标签会让本组(乃至父级分栏)
			// 塌缩,RenderTabs/RenderSplit 手里的 node/children 引用立即失效。
			// 真正删除在 OnRender 末尾统一执行(见 m_PendingPanelCloses)。
			m_PendingPanelCloses.push_back(node.Panels[tabResult.Closed]);
		}
		if (tabResult.DragStart >= 0 && static_cast<size_t>(tabResult.DragStart) < node.Panels.size())
		{
			const std::string& panel = node.Panels[tabResult.DragStart];
			// P3-1①:拖拽只能在**按下沿**起手。DockTabBar 的 DragStart 是"按住即报",
			// 若照单全收,一次点击在"面板刚回到停靠树、标签正好画在光标下、按键还没抬"时
			// (典型场景:点浮窗 ✕ → 面板收回停靠位)会被当成新的拖拽,面板立刻被再次浮出 ——
			// 这正是"第一次点 ✕ 被'重新浮出'吃掉"的机制。独立窗口的标签早就是按下沿起手
			// (见 FloatWindowHost::RenderTabBar),这里补齐同一条规则。
			if (ctx.Input().MouseClicked[0])
			{
				ctx.BeginDrag(Wui::HashId(("tab." + panel).c_str()), "panel:" + panel);
				// 记录本次拖拽的真实来源:只在还没有来源时记一次。拖动过程中经过别的标签页时
				// DockTabBar 仍会报 DragStart,若覆盖会把真实来源记错 —— 拖出分支的
				// "m_TabDragPanel == m_DragPanel" 守卫随即拒绝,表现是面板拖不出来。
				if (m_TabDragPanel.empty())
					m_TabDragPanel = panel;
			}
		}

		const Wui::WuiRect content { area.X, area.Y + tabH, area.W, area.H - tabH };
		if (!node.Panels.empty())
		{
			// P4-U6b:面板级模态的拥有者自己需要能命中 —— 先解封锁,画完再封回去,
			// 这样它之后渲染的面板(以及本帧的其它交互)仍然被挡住。
			const std::string& active = node.Panels[node.Active];
			const bool ownsPanelModal = !m_PanelModalOwner.empty() && m_PanelModalOwner == active;
			if (ownsPanelModal)
				Wui::EndModalInputBlock(ctx);
			RenderPanelContent(ctx, active, content);
			if (ownsPanelModal)
				Wui::BeginModalInputBlock(ctx);
		}

		std::string dragPayload;
		// 独立窗口不能在停靠面板上落区(只能挂靠到顶部挂靠栏)。
		if (!m_EdgeDockActive && ctx.IsDragActive(&dragPayload) && dragPayload.rfind("panel:", 0) == 0
			&& !IsIndependentPanel(dragPayload.substr(6)) && ctx.IsHovered(area))
		{
			const glm::vec2 rel = ctx.Input().MousePos - glm::vec2 { area.X, area.Y };
			const float lx = area.W > 0 ? rel.x / area.W : 0;
			const float ly = area.H > 0 ? rel.y / area.H : 0;
			Wui::DropZone targetZone = Wui::DropZone::Center;
			if (lx < 0.25f) targetZone = Wui::DropZone::Left;
			else if (lx > 0.75f) targetZone = Wui::DropZone::Right;
			else if (ly < 0.25f) targetZone = Wui::DropZone::Top;
			else if (ly > 0.75f) targetZone = Wui::DropZone::Bottom;
			// 中心区(面板主体或标签栏)= 合并进该组的标签页(用户 2026-09-16:
			// 拖到另一个子面板上就应该变成同组标签,原来只认 24px 标签栏,很难命中)。
			ctx.DropTarget(area, "panel:"); // 武装落点:仅面板拖拽在此生效
			m_DropZone = targetZone;
			Wui::WuiRect zone = area;
			if (m_DropZone == Wui::DropZone::Left) zone.W = area.W * 0.25f;
			else if (m_DropZone == Wui::DropZone::Right) { zone.X = area.X + area.W * 0.75f; zone.W = area.W * 0.25f; }
			else if (m_DropZone == Wui::DropZone::Top) zone.H = area.H * 0.25f;
			else if (m_DropZone == Wui::DropZone::Bottom) { zone.Y = area.Y + area.H * 0.75f; zone.H = area.H * 0.25f; }
			else zone.H = tabH; // 中心落点:高亮该组标签栏(它会变成这里的一个标签页)
			// 落区预览延迟到 RenderFloating 里画(浮动面板之上),否则会被拖动中的面板盖住。
			m_DropPreviewRect = zone;
			m_DropPreviewActive = true;
			if (!node.Panels.empty())
			{
				const std::string target = node.Panels[node.Active];
				if (target != m_LastDragTarget || m_DropZone != m_LastDragZone)
					ctx.RecordOp("drag", "hover", target, ZoneName(m_DropZone));
				m_LastDragTarget = target;
				m_LastDragZone = m_DropZone;
				m_DropTargetPanel = node.Panels[node.Active];
			}
		}
	}

	void EditorShell::RenderPanelContent(Wui::WuiContext& ctx, const std::string& id, const Wui::WuiRect& rect)
	{
		Wui::PanelBackground(ctx, rect, m_Theme.PanelBg);
		// 无障碍树:此后登记的控件归属该面板(ui.tree/state.dump 靠它区分面板)。
		Wui::WuiAccessibility::Get().SetPanel(id);
		ctx.SetPanelId(id);
		const auto it = m_PanelRegistry.find(id);
		if (it != m_PanelRegistry.end())
			it->second->OnRender(ctx, rect, *this);
		else
			Label(ctx, { rect.X + 8, rect.Y + 8 }, PanelTitle(id), m_Theme.TextMuted, 14.0f);
	}

	// 挂靠栏:横跨主窗口的一条,按"窗口"列出一行(chip 显示活动标签名与标签数),
	// 独立窗口被拖到这条栏上(窗口中心进入栏内并停稳)或点 Attach 会整窗挂靠回主窗口。
	void EditorShell::DrawAttachBar(Wui::WuiContext& ctx)
	{
		const glm::vec2 viewport = ctx.ViewportSize();
		// 顶部第一行即挂靠栏:也是无边框主窗口的拖动/关闭区域。
		const Wui::WuiRect bar { 0, 0.0f, viewport.x, m_AttachBarHeight };
		const Wui::WuiColor fill = m_AttachSlotHighlight
			? Wui::WuiColor { 0.3f, 0.5f, 0.9f, 0.45f }
			: m_Theme.PanelHeader;
		Wui::BarSurface(ctx, bar, fill, m_Theme.Border);

		// 标准窗口控制(最小化/最大化/关闭)在最右侧。
		const Wui::WindowControl control = Wui::WindowControls(ctx,
			{ bar.X + bar.W - 102.0f, bar.Y, 102.0f, bar.H }, m_Theme,
			Application::HasInstance() && Application::Get().GetWindow().IsMaximized());
		if (control == Wui::WindowControl::Minimize)
		{
			if (Application::HasInstance())
				Application::Get().GetWindow().Minimize();
			return;
		}
		if (control == Wui::WindowControl::Maximize)
		{
			if (Application::HasInstance())
				Application::Get().GetWindow().MaximizeOrRestore();
			return;
		}
		if (control == Wui::WindowControl::Close)
		{
			m_Editor.CloseAction();
			return;
		}

		// 标签栏只列出"窗口":Main + 已附加到主窗口的独立窗口(切换关系)。
		float x = bar.X + 6.0f;
		{
			const Wui::WuiRect tab { x, bar.Y + 3.0f, 90.0f, bar.H - 6.0f };
			const bool active = m_ActiveWindowTag.empty();
			// 标签 chip 走组件:活动/悬停底色与关闭 x 的外观统一。
			if (Wui::AttachTag(ctx, tab, "Main", active, false, m_Theme).Clicked)
				m_ActiveWindowTag.clear();
			// P3-1②:Main 标签同样登记(稳定 id `shell.attach.main`)—— 脚本可以在
			// ui.attach 把视图切到某个附加窗口之后,再读/点这一枚标签切回主界面。
			RegisterAttachNode("main", tab, active ? "active" : "inactive", "Main", true);
			x += 96.0f;
		}
		x += 4.0f;

		// 已附加的独立窗口标签:点击切换;× 关闭该窗口;按住可拖出为独立窗口;
		// 多个附加窗口之间可左右拖动换位。
		std::string closeRequest;
		struct AttachTagHit { std::string Panel; Wui::WuiRect Rect; };
		std::vector<AttachTagHit> tagHits;
		for (const std::string& panel : m_AttachedPanels)
		{
			const bool active = m_ActiveWindowTag == panel;
			const Wui::WuiRect tab { x, bar.Y + 3.0f, 140.0f, bar.H - 6.0f };
			const Wui::AttachTagResult tag = Wui::AttachTag(ctx, tab, PanelTitle(panel), active, true, m_Theme);
			// P3-1②:同一 id 在浮动态由下面的浮窗分支登记(value=floating),这里登记附加态。
			RegisterAttachNode(panel, tab, "attached", PanelTitle(panel), true);
			if (tag.CloseClicked)
				closeRequest = panel;
			// 按下(非关闭键)记录起点;移动超过阈值进入拖动。
			if (ctx.Input().MouseDown[0] && tag.Hovered && !tag.CloseHovered
				&& m_AttachTagDrag.empty())
			{
				m_AttachTagPress = panel;
				m_AttachTagPressPos = ctx.Input().MousePos;
			}
			if (m_AttachTagPress == panel && m_AttachTagDrag.empty() && ctx.Input().MouseDown[0]
				&& glm::length(ctx.Input().MousePos - m_AttachTagPressPos) > 3.0f)
				m_AttachTagDrag = panel;
			if (m_AttachTagDrag == panel)
			{
				Wui::HighlightOutline(ctx, tab, m_Theme.Accent, 2.0f, 2.0f);
				// 脱出提示:跟随光标的标签名 + 栏外时提示将变为独立窗口。
				const bool outside = !ctx.IsHovered(bar);
				ctx.PushOverlay();
				Label(ctx, { ctx.Input().MousePos.x + 14.0f, ctx.Input().MousePos.y + 14.0f },
					outside ? std::string(PanelTitle(panel)) + "  →  独立窗口" : std::string(PanelTitle(panel)),
					m_Theme.Text, 13.0f);
				ctx.PopOverlay();
			}
			tagHits.push_back({ panel, tab });
			x += 144.0f;
		}

		// 同一窗口的其它标签页:顶栏只给窗口一枚 chip(拖出=整个窗口),但它们的形态
		// 也要能从无障碍树读到 —— 注册成只读节点(Interactive=false,共用同一 chip 矩形)。
		for (const AttachTagHit& hit : tagHits)
		{
			FloatWindowHost* host = FindFloatHost(hit.Panel);
			if (!host)
				continue;
			for (const std::string& panel : host->Panels())
			{
				if (panel == hit.Panel)
					continue;
				RegisterAttachNode(panel, hit.Rect, "attached-tab", PanelTitle(panel), false);
			}
		}

		// P3-1②:可见的独立窗口 = 浮动态。节点与顶栏标签共用 id(`shell.attach.<panel>`),
		// value=floating;rect 由窗口屏幕矩形换算到主窗口客户区,只用于"读状态",因此
		// Interactive=false(点它不应该落到主窗口上,也不参与 ui.invoke)。
		{
			int mainX = 0, mainY = 0;
			if (Application::HasInstance())
				Application::Get().GetWindow().GetPosition(&mainX, &mainY);
			for (const std::unique_ptr<FloatWindowHost>& host : m_FloatHosts)
			{
				if (host->IsHidden())
					continue;
				const Wui::WuiRect screen = host->ScreenRect();
				const Wui::WuiRect local { screen.X - static_cast<float>(mainX),
					screen.Y - static_cast<float>(mainY), screen.W, screen.H };
				for (const std::string& panel : host->Panels())
					RegisterAttachNode(panel, local, "floating", PanelTitle(panel), false);
			}
		}

		if (!closeRequest.empty())
		{
			// P4-U13e:prefab 面板有未保存改动 → 先弹确认;窗口先留着,标签也不能摘。
			if (InterceptPrefabUnsaved(closeRequest, PrefabPendingAction::Close))
				closeRequest.clear();
		}
		if (!closeRequest.empty())
		{
			// × = 关闭:隐藏该窗口的面板(可从 Window 菜单重新打开),并把标签移出栏。
			// 一窗口一枚标签:关掉的是整个窗口,该窗口所有标签页的记录一起摘掉。
			std::vector<std::string> closingPanels;
			if (FloatWindowHost* host = FindFloatHost(closeRequest))
				closingPanels = host->Panels();
			if (closingPanels.empty())
				closingPanels.push_back(closeRequest);
			CloseFloatWindow(closeRequest, true, &ctx);
			for (const std::string& id : closingPanels)
			{
				m_AttachedPanels.erase(std::remove(m_AttachedPanels.begin(), m_AttachedPanels.end(), id),
					m_AttachedPanels.end());
				if (m_ActiveWindowTag == id)
					m_ActiveWindowTag.clear();
			}
		}
		// 拖动结束:在栏内 → 换位;在栏外 → 拖出为独立窗口。
		if (m_AttachTagDrag.empty() && !m_AttachTagPress.empty() && ctx.Input().MouseReleased[0])
		{
			// 未拖动 = 单击:切换显示该窗口内容。
			m_ActiveWindowTag = m_AttachTagPress;
			m_AttachTagPress.clear();
		}
		if (!m_AttachTagDrag.empty() && ctx.Input().MouseReleased[0])
		{
			const std::string dragged = m_AttachTagDrag;
			if (ctx.IsHovered(bar))
			{
				size_t target = m_AttachedPanels.size();
				for (size_t i = 0; i < tagHits.size(); ++i)
					if (ctx.Input().MousePos.x < tagHits[i].Rect.X + tagHits[i].Rect.W * 0.5f)
					{
						target = i;
						break;
					}
				const auto current = std::find(m_AttachedPanels.begin(), m_AttachedPanels.end(), dragged);
				if (current != m_AttachedPanels.end())
				{
					const size_t from = static_cast<size_t>(current - m_AttachedPanels.begin());
					m_AttachedPanels.erase(current);
					if (target > from && target > 0)
						--target;
					m_AttachedPanels.insert(m_AttachedPanels.begin()
						+ static_cast<std::ptrdiff_t>(std::min(target, m_AttachedPanels.size())), dragged);
				}
			}
			else if (FloatWindowHost* host = FindFloatHost(dragged))
			{
				// 拖出:恢复为独立窗口,窗口放到光标附近(屏幕坐标)。
				const std::vector<std::string> hostPanels = host->Panels();
				int mainX = 0, mainY = 0;
				if (Application::HasInstance())
					Application::Get().GetWindow().GetPosition(&mainX, &mainY);
				host->SetScreenPosition(static_cast<float>(mainX) + ctx.Input().MousePos.x - 60.0f,
					static_cast<float>(mainY) + ctx.Input().MousePos.y - 12.0f);
				// 拖出来的是**这个窗口**,窗口显示用户拖的那一页(标签页可能被切过)。
				host->ActivatePanel(dragged);
				host->SetHidden(false);
				// 整窗离槽:把该窗口里所有标签页的附加记录一次性摘干净(不留下悬空 chip)。
				for (const std::string& id : hostPanels)
				{
					m_AttachedPanels.erase(std::remove(m_AttachedPanels.begin(), m_AttachedPanels.end(), id),
						m_AttachedPanels.end());
					if (m_ActiveWindowTag == id)
						m_ActiveWindowTag.clear();
				}
				ctx.RecordOp("float", "detach", dragged, "");
				SaveLayout();   // 形态变了(挂靠 → 浮窗),立刻落盘(P4-UX10)
			}
			m_AttachTagPress.clear();
			m_AttachTagDrag.clear();
		}
		if (!ctx.Input().MouseDown[0] && m_AttachTagDrag.empty())
			m_AttachTagPress.clear();
		if (ctx.Input().MouseDown[0] && ctx.IsHovered(bar) && !ctx.IsHovered({ 0, 0, x, bar.H })
			&& !ctx.IsHovered({ bar.X + bar.W - 102.0f, bar.Y, 102.0f, bar.H }))
			Application::Get().GetWindow().BeginSystemDrag();
	}

	// 独立窗口:每个宿主 = 一个 OS 窗口(可含多个标签面板),绘制在停靠区之上。
	void EditorShell::RenderFloating(Wui::WuiContext& ctx)
	{
		if (m_AttachCooldownFrames > 0)
			--m_AttachCooldownFrames;
		// 每帧复位挂靠栏高亮,只在独立窗口真正悬停其上时点亮,避免残留。
		m_AttachSlotHighlight = false;

		// 槽位屏幕矩形(客户区 -> 屏幕):用于判断独立窗口是否停到了槽位上。
		const glm::vec2 viewport = ctx.ViewportSize();
		// 挂靠栏的屏幕矩形(横条):客户区坐标 -> 屏幕坐标。
		// 注意:viewport 是**设计单位**(= 物理像素 / UiScale),而这里要和 GetCursorPos /
		// 窗口屏幕矩形(都是物理像素)比较 —— 必须乘回 UiScale,否则 UI 缩放 1.3 时
		// 命中区只有真实栏高的 77%,"拖到栏上"会时灵时不灵。
		const float uiScale = Wui::UiScale();
		m_AttachSlotScreenRect = { 0, 0.0f, viewport.x * uiScale, m_AttachBarHeight * uiScale };
		int windowX = 0, windowY = 0;
		if (Application::HasInstance())
			Application::Get().GetWindow().GetPosition(&windowX, &windowY);
		m_AttachSlotScreenRect.X += static_cast<float>(windowX);
		m_AttachSlotScreenRect.Y += static_cast<float>(windowY);

		// 独立 OS 窗口不在这里渲染 —— 见 RenderIndependentWindows(帧末调用,理由见该函数)。
		// 停靠形态的"临时浮动"面板:在主窗口内绘制(OS 窗口只属于 Independent 面板)。
		for (size_t i = 0; i < m_Layout.Floating.size(); ++i)
		{
			const std::string panel = m_Layout.Floating[i].Panel;
			if (IsIndependentPanel(panel))
				continue; // 独立面板有自己的 OS 窗口
			bool closed = false;
			RenderFloatWindow(ctx, m_Layout.Floating[i], &closed);
			if (closed)
			{
				// 停靠形态的浮动窗口关闭 = 回停靠位(D3),不是隐藏面板。
				HideFloatPanel(panel, &ctx);
				break; // 容器已改变,下一帧继续绘制其余窗口
			}
		}
		// 置顶在绘制结束后应用,避免遍历中修改容器。
		if (!m_BringFloatFront.empty())
		{
			m_Layout.BringFloatToFront(m_BringFloatFront);
			m_BringFloatFront.clear();
		}
		// 落点预览画在所有浮动面板之上:拖动中的面板正好盖在目标上。
		if (m_DropPreviewActive)
			Wui::DropZoneOverlay(ctx, m_DropPreviewRect, 0.30f, 3.0f);
	}

	// 独立窗口:每个宿主 = 一个 OS 窗口(可含多个标签面板)。
	//
	// **为什么放在主窗口 UI 的最后一步**(U26 修):每个窗口渲染前都会调
	// `WuiAccessibility::BeginFrame(windowKey)` 把无障碍树的"当前窗口"切到自己,而
	// 主窗口的菜单栏 / 状态栏 / 外壳模态都在停靠面板之后才绘制。若独立窗口在本帧中段
	// 渲染,之后登记的主窗口节点(菜单栏 File/Window、菜单项、状态栏…)就会挂着**浮窗的
	// window 键** —— `ui.invoke` 会把点击投进浮窗,菜单从此点不开(实测:U25 探针在
	// `window=main` 下找不到 menu.window,唯一那份指向材质浮窗,点它只打中浮窗的标签栏),
	// 跨窗口拖放的落点登记同样晚于源面板渲染(见 AssetDropBridge 的帧老化)。
	// 移到帧末后,主窗口 UI 全程处于 BeginFrame("main") 之后,独立窗口只影响自己的节点。
	void EditorShell::RenderIndependentWindows(Wui::WuiContext& ctx)
	{
		// 每个独立窗口渲染自己的 OS 窗口(含标签栏);窗口被关闭 = 隐藏其全部面板。
		// 标签栏 x 只登记关闭请求,统一在遍历结束后处理,避免边遍历边改 m_FloatHosts。
		std::vector<std::string> closeRequests;
		// 收集新发起的标签拖拽(跨窗口附加);同帧只接受一个。
		// 独立窗口标签被拖过阈值 → 交给系统移动循环(阻塞到松手,见 PerformIndependentWindowDrag)。
		for (const std::unique_ptr<FloatWindowHost>& candidate : m_FloatHosts)
		{
			if (const std::string drag = candidate->TakePendingTabDrag(); !drag.empty())
			{
				PerformIndependentWindowDrag(drag);
				break;
			}
		}
		for (size_t i = 0; i < m_FloatHosts.size(); )
		{
			FloatWindowHost& host = *m_FloatHosts[i];
			// 隐藏的宿主(复用中)不渲染。
			if (host.IsHidden())
			{
				++i;
				continue;
			}
			// 空宿主:隐藏保留(运行期销毁窗口在 Vulkan 下会崩),等待下次复用。
			if (host.Panels().empty())
			{
				host.SetHidden(true);
				++i;
				continue;
			}
			bool alive = true;
			try
			{
				alive = host.Render();
			}
			catch (const std::exception& error)
			{
				WLD_CORE_ERROR("[float] render failed for '{0}': {1}", host.Panel(), error.what());
				alive = false;
			}
			if (!alive)
			{
				const std::string panel = host.Panel();
				CloseFloatWindow(panel, true, &ctx);
				continue; // CloseFloatWindow 会移除该 host
			}
			if (const std::string closing = host.TakeCloseRequest(); !closing.empty())
				closeRequests.push_back(closing);

			// 位置/尺寸变化写回布局(供重启恢复):窗口内每个标签写同一屏幕矩形。
			const Wui::WuiRect rect = host.ScreenRect();
			for (const std::string& panel : host.Panels())
				if (Wui::DockFloat* entry = m_Layout.FindFloat(panel))
					entry->Rect = rect;

			// 位置记忆写回布局(拖动由系统移动循环负责,这里只记录最终矩形)。
			const std::string windowKey = host.Panels().front();
			m_LastFloatScreenRects[windowKey] = rect;
			++i;
		}
		for (const std::string& panel : closeRequests)
			HideFloatPanel(panel, &ctx);
		// 挂靠栏高亮不再在这里复位:本函数已经排到 DrawAttachBar 之后,复位会擦掉
		// 当帧刚算出的高亮;残留由下一帧 RenderFloating 开头的复位统一清掉。
		//
		// 独立窗口渲染会把当前 GL 上下文切到各自窗口,这里恢复主窗口上下文 ——
		// 否则主窗口后续的呈现/交换会作用在错误的上下文上(表现为主窗口不再刷新)。
		if (Application::HasInstance())
			Application::Get().GetWindow().MakeCurrent();
	}

	// 窗口内浮动面板(停靠形态面板拖出后的形态)的绘制与交互:
	// 标题栏拖动 = 移动(拖动载荷仍是 "panel:",拖到停靠落点上即回停靠);
	// 右下角 = 缩放;左上 × = 关闭浮动窗口(停靠形态 = 回停靠位,D3)。
	// 独立窗口(Independent)不走这条路径,它们由 FloatWindowHost 的 OS 窗口渲染。
	void EditorShell::RenderFloatWindow(Wui::WuiContext& ctx, Wui::DockFloat& window, bool* closed)
	{
		const float titleH = 24.0f;
		const glm::vec2 viewport = ctx.ViewportSize();
		const float topLimit = 26.0f + m_AttachBarHeight;
		Wui::WuiRect& rect = window.Rect;

		// 视口约束:窗口不能完全跑出编辑区,标题栏必须可见。
		rect.W = std::max(240.0f, std::min(rect.W, std::max(240.0f, viewport.x - 16.0f)));
		rect.H = std::max(160.0f, std::min(rect.H, std::max(160.0f, viewport.y - topLimit - 16.0f)));
		rect.X = std::max(8.0f, std::min(rect.X, std::max(8.0f, viewport.x - rect.W - 8.0f)));
		rect.Y = std::max(topLimit, std::min(rect.Y, std::max(topLimit, viewport.y - rect.H - 8.0f)));
		if (std::getenv("WLD_TRACE_UI"))
		{
			// 诊断/自动化:窗口内浮动面板的实际矩形(客户区坐标),供脚本点击标题栏与关闭按钮。
			static int traced = 0;
			if (traced < 60)
			{
				++traced;
				WLD_CORE_INFO("[float] in-window '{0}' rect=({1},{2},{3},{4})",
					window.Panel, rect.X, rect.Y, rect.W, rect.H);
			}
		}

		ctx.PushOverlay();
		ctx.Commands().push_back({ Wui::WuiDrawKind::Rect, rect, m_Theme.PanelBg, 5.0f });
		ctx.Commands().push_back({ Wui::WuiDrawKind::RectOutline, rect, m_Theme.Border, 5.0f, 1.0f });
		const Wui::WuiRect title { rect.X, rect.Y, rect.W, titleH };
		ctx.Commands().push_back({ Wui::WuiDrawKind::Rect, title, m_Theme.PanelHeader, 5.0f });
		Label(ctx, { title.X + 10.0f, title.Y + 4.0f }, PanelTitle(window.Panel), m_Theme.Text, 14.0f);

		// 关闭按钮
		const Wui::WuiRect close { title.X + title.W - 22.0f, title.Y + 5.0f, 14.0f, 14.0f };
		const bool overClose = ctx.IsHovered(close);
		if (overClose)
			ctx.Commands().push_back({ Wui::WuiDrawKind::Rect, close, m_Theme.ButtonHover, 2.0f });
		Label(ctx, { close.X + 3.0f, close.Y - 2.0f }, "x", m_Theme.TextMuted, 13.0f);
		const bool closeClicked = ctx.IsClicked(close);

		// 标题栏拖动:置顶 + 跟随鼠标;松手落在停靠落点上则由外壳的落位逻辑回停靠。
		if (!closeClicked && !overClose && ctx.IsClicked(title))
		{
			m_BringFloatFront = window.Panel;
			m_MovingFloat = window.Panel;
			m_FloatGrabOffset = ctx.Input().MousePos - glm::vec2 { rect.X, rect.Y };
			m_FloatChangeBefore = m_Layout.Serialize();
			ctx.BeginDrag(Wui::HashId(("float." + window.Panel).c_str()), "panel:" + window.Panel);
		}
		if (m_MovingFloat == window.Panel && ctx.IsDragActive(nullptr))
		{
			rect.X = std::max(8.0f, std::min(ctx.Input().MousePos.x - m_FloatGrabOffset.x,
				std::max(8.0f, viewport.x - rect.W - 8.0f)));
			rect.Y = std::max(topLimit, std::min(ctx.Input().MousePos.y - m_FloatGrabOffset.y,
				std::max(topLimit, viewport.y - rect.H - 8.0f)));
			ctx.SetCursor(Wui::WuiCursor::Hand);
		}

		// 右下角缩放
		const Wui::WuiRect grip { rect.X + rect.W - 16.0f, rect.Y + rect.H - 16.0f, 16.0f, 16.0f };
		if (ctx.IsHovered(grip))
		{
			ctx.Commands().push_back({ Wui::WuiDrawKind::Rect, grip, m_Theme.ButtonHover, 3.0f });
			ctx.SetCursor(Wui::WuiCursor::ResizeEW);
		}
		if (ctx.IsClicked(grip))
		{
			m_BringFloatFront = window.Panel;
			m_FloatResize = window.Panel;
			m_FloatResizeStart = ctx.Input().MousePos;
			m_FloatResizeRect = rect;
			m_FloatChangeBefore = m_Layout.Serialize();
		}
		if (m_FloatResize == window.Panel && ctx.Input().MouseDown[0])
		{
			const glm::vec2 delta = ctx.Input().MousePos - m_FloatResizeStart;
			rect.W = std::max(240.0f, m_FloatResizeRect.W + delta.x);
			rect.H = std::max(160.0f, m_FloatResizeRect.H + delta.y);
		}
		if (m_FloatResize == window.Panel && ctx.Input().MouseReleased[0])
		{
			m_FloatResize.clear();
			if (!m_FloatChangeBefore.empty())
			{
				RecordDockChange(ctx, "float-resize", window.Panel, m_FloatChangeBefore);
				m_FloatChangeBefore.clear();
			}
		}
		// 尺寸记忆:下次拖出沿用用户调好的大小(位置按当次拖拽点重新计算)。
		m_LastFloatRects[window.Panel] = { 0.0f, 0.0f, rect.W, rect.H };

		// 自动化钩子(开发验证):WLD_FLOAT_RECT_FILE=<路径> 时把浮动面板的客户区矩形
		// 写进该文件(矩形变化才写),供脚本点击标题栏/关闭按钮。
		if (const char* rectFile = std::getenv("WLD_FLOAT_RECT_FILE"))
		{
			char buffer[192];
			std::snprintf(buffer, sizeof(buffer), "panel=%s\nx=%.1f\ny=%.1f\nw=%.1f\nh=%.1f\n",
				window.Panel.c_str(), rect.X, rect.Y, rect.W, rect.H);
			static std::string lastWritten;
			if (lastWritten != buffer)
			{
				lastWritten = buffer;
				std::ofstream(rectFile, std::ios::trunc) << buffer;
			}
		}

		// 内容区(标题栏之下)
		const Wui::WuiRect body { rect.X + 1.0f, rect.Y + titleH, rect.W - 2.0f, rect.H - titleH - 1.0f };
		RenderPanelContent(ctx, window.Panel, body);
		ctx.PopOverlay();

		if (closeClicked && closed)
			*closed = true;
	}

	// P4-UX9:拖动独立窗口(在标签栏/空白区按下并越过阈值后进入)。
	//
	// 手感设计(用户 2026-09-20:"独立窗口拖拽手感差,鼠标滑动一快就会出现偏移"):
	//   ① 窗口移动交给**系统移动循环**(SC_MOVE):系统按输入频率移动窗口,与渲染帧率无关
	//      —— 本引擎 Debug 下一帧 50ms,"每帧轮询光标"必然发飘,再怎么调都追不上;
	//      它同时自带 Esc 取消与系统吸附,是 Windows 上拖标题栏的工业标准做法。
	//   ② 进入循环前把窗口"预置"到 光标 - 按下瞬间的抓取偏移:补偿"按下 → 识别到拖动"
	//      之间已经发生的位移。否则系统会把那段位移吸收进抓取偏移,快速甩动时窗口不跟手。
	//   ③ 拖动期间光标进入挂靠栏 → 窗口被压到栏下方(SetSystemDragParkZone):
	//      "窗口停在栏下"就是"松手即挂靠"的可见提示,目标不会被窗口自己挡住。
	//   ④ 松手按真实落点判定:挂靠栏上 → 整窗挂靠;Esc → 回到拖动前的位置(取消);
	//      其它位置 → 就停在那里(位置由 OnRender 里既有的写回逻辑进布局)。
	void EditorShell::PerformIndependentWindowDrag(const std::string& panel)
	{
		FloatWindowHost* host = FindFloatHost(panel);
		if (!host)
			return;
		Window* window = host->NativeWindow();
		if (!window)
			return;

		const glm::vec2 grab = host->TakePendingTabDragGrab();
		const Wui::WuiRect startRect = host->ScreenRect();
		POINT cursor { 0, 0 };
		GetCursorPos(&cursor);

		int mainX = 0, mainY = 0;
		float mainW = static_cast<float>(cursor.x + 1280);
		if (Application::HasInstance())
		{
			Application::Get().GetWindow().GetPosition(&mainX, &mainY);
			mainW = static_cast<float>(Application::Get().GetWindow().GetWidth());
		}
		const float barPixels = m_AttachBarHeight * Wui::UiScale();

		window->SetPosition(static_cast<int>(static_cast<float>(cursor.x) - grab.x),
			static_cast<int>(static_cast<float>(cursor.y) - grab.y));
		// 投放提示:光标进挂靠栏 → 栏上亮一层半透明提示(不改窗口位置,窗口全程跟手)。
		window->SetSystemDragDropHint(
			{ static_cast<float>(mainX), static_cast<float>(mainY), mainW, barPixels });
		host->SetTabDragActive(true);
		window->BeginSystemDrag();          // 阻塞:系统移动循环,回到这里就是松手
		window->SetSystemDragDropHint({ 0.0f, 0.0f, 0.0f, 0.0f });
		host->SetTabDragActive(false);

		if (GetAsyncKeyState(VK_ESCAPE) & 0x8000)
		{
			// Esc = 取消(系统只保证回到循环起点,也就是预置后的位置;这里再放回按下前的位置)。
			host->SetScreenPosition(startRect.X, startRect.Y);
			WLD_CORE_INFO("[float] drag cancelled by Esc: {0}", panel);
			return;
		}

		POINT released { 0, 0 };
		GetCursorPos(&released);
		const bool overAttachBar = IsIndependentPanel(panel)
			&& m_AttachSlotScreenRect.W > 0.0f
			&& static_cast<float>(released.x) >= m_AttachSlotScreenRect.X
			&& static_cast<float>(released.x) <= m_AttachSlotScreenRect.X + m_AttachSlotScreenRect.W
			&& static_cast<float>(released.y) >= m_AttachSlotScreenRect.Y
			&& static_cast<float>(released.y) <= m_AttachSlotScreenRect.Y + m_AttachSlotScreenRect.H;
		// 落点诊断(拖拽是模态循环,出问题时日志是唯一现场)。
		WLD_CORE_INFO("[float] drag end: panel={0} released=({1},{2}) slot=({3:.0f},{4:.0f},{5:.0f},{6:.0f}) attach={7}",
			panel, released.x, released.y, m_AttachSlotScreenRect.X, m_AttachSlotScreenRect.Y,
			m_AttachSlotScreenRect.W, m_AttachSlotScreenRect.H, overAttachBar ? 1 : 0);
		if (overAttachBar)
		{
			AttachIndependentWindowToSlot(panel);
			return;
		}
		if (m_Ctx)
			m_Ctx->RecordOp("float", "move", panel, "");
	}

	void EditorShell::AddFloatWindow(const std::string& panel, const Wui::WuiRect& screenRect, const char* origin,
		bool startHidden)
	{
		WLD_CORE_INFO("[float] AddFloatWindow panel={0} origin={1} rect=({2},{3},{4},{5})",
			panel, origin, screenRect.X, screenRect.Y, screenRect.W, screenRect.H);
		// 去重(T03):同一面板最多一个窗口——已存在的窗口(可见或隐藏)直接复用,
		// 避免"同一面板被创建两次"这类布局乱象。
		if (FloatWindowHost* existing = FindFloatHost(panel))
		{
			existing->SetScreenPosition(screenRect.X, screenRect.Y);
			existing->ActivatePanel(panel);
			existing->SetHidden(startHidden);
			WLD_CORE_INFO("[float] reused existing window for panel={0}", panel);
			return;
		}
		// 复用已隐藏的独立窗口:运行期销毁窗口在 Vulkan 下会崩,因此"关闭/挂靠"只隐藏。
		for (const std::unique_ptr<FloatWindowHost>& host : m_FloatHosts)
		{
			// P4-UX9:只复用**空窗**(面板全部关掉了的"壳")。以前会把新面板塞进一个还挂着
			// 别的面板的隐藏窗口里,于是"一个 OS 窗口 + 两枚顶栏标签",拖出左侧那枚会把整个
			// 窗口(含另一个面板)一起拔出来,另一枚标签的状态就悬空了(用户 2026-09-20 复现)。
			if (!host->IsHidden() || !host->Panels().empty())
				continue;
			host->SetScreenPosition(screenRect.X, screenRect.Y);
			host->AddPanel(panel, true);
			host->SetHidden(startHidden);
			WLD_CORE_INFO("[float] reused hidden window for panel={0}", panel);
			return;
		}
		// 独立窗口 = 容器 + 标签栏;标题与内容由面板注册表提供,容器不感知具体面板类型。
		FloatWindowHost::Callbacks callbacks;
		callbacks.Theme = m_Theme;
		callbacks.Title = [this](const std::string& id) { return std::string(PanelTitle(id)); };
		callbacks.Content = [this](Wui::WuiContext& ctx, const Wui::WuiRect& rect, const std::string& id)
		{
			RenderPanelContent(ctx, id, rect);
		};
		callbacks.TabDragStart = [](const std::string& panel)
		{
			WLD_CORE_INFO("[float] tag drag started: {0}", panel);
		};
		callbacks.DockToMain = [this](const std::string& id) { AttachIndependentWindowToSlot(id); };
		callbacks.CloseWindow = [this](const std::string& id) { CloseFloatWindow(id, false, m_Ctx); };
		callbacks.CanAttach = [this](const std::string& id) { return IsIndependentPanel(id); };
		try
		{
			m_FloatHosts.push_back(std::make_unique<FloatWindowHost>(panel, PanelTitle(panel), screenRect, std::move(callbacks)));
			if (startHidden)
				m_FloatHosts.back()->SetHidden(true);   // 恢复成顶栏标签:不闪窗口
		}
		catch (const std::exception& error)
		{
			WLD_CORE_ERROR("[float] create failed for '{0}': {1}", panel, error.what());
			m_Layout.CloseFloating(panel);
		}
	}

	// P4-UX10:按策略恢复上次的独立窗口。
	// forceTabs = 一律恢复成顶栏标签(不弹 OS 窗口、不抢焦点);否则按每项上次形态。
	void EditorShell::RestoreIndependentWindows(const std::vector<PendingFloatRestore>& items, bool forceTabs)
	{
		uint32_t restored = 0;
		uint32_t asTabs = 0;
		for (const PendingFloatRestore& item : items)
		{
			if (item.Panels.empty())
				continue;
			// 动态面板(材质/脚本/模型)按 id 补建实例。
			for (const std::string& panel : item.Panels)
			{
				EnsureMaterialPanelFromId(panel);
				EnsureScriptPanelFromId(panel);
				EnsureModelPanelFromId(panel);
				EnsurePrefabPanelFromId(panel);
				EnsurePluginPanelFromId(panel);   // PLUG-T3b:插件面板跨会话恢复
			}
			const bool attach = forceTabs || item.Attached;
			// 先隐藏着建出来:恢复成 chip 时不会"闪一下窗口",恢复成浮窗时下一步再显示。
			AddFloatWindow(item.Panels.front(), item.Rect, "restore", true);
			FloatWindowHost* host = m_FloatHosts.empty() ? nullptr : m_FloatHosts.back().get();
			if (!host)
				continue;
			for (size_t i = 1; i < item.Panels.size(); ++i)
				host->AddPanel(item.Panels[i], false);
			if (attach)
			{
				AttachIndependentWindowToSlot(item.Panels.front());
				++asTabs;
			}
			else
			{
				host->ShowWithoutActivation();   // 浮窗按上次位置回来,但不抢焦点
			}
			++restored;
		}
		if (restored > 0)
		{
			std::string text = Wui::Tr("notice.restore", "Restored last session's windows") + ": "
				+ std::to_string(restored);
			if (asTabs == restored)
				text += "  ·  " + Wui::Tr("notice.restore.tabs", "they are in the top bar");
			PushNotice(text);
		}
	}

	// 状态栏提示:给"刚刚发生了什么"一个不打断的表达。
	void EditorShell::PushNotice(const std::string& text)
	{
		m_Notice.Text = text;
		m_Notice.Active = true;
		m_Notice.Timing = TimedNotice {};
		m_Notice.Timing.ShownAt = ShellNowSeconds();
		WLD_CORE_INFO("[notice] {0}", text);
	}

	// 提示节拍(状态栏提示与恢复询问条共用):
	//   · 悬停 = 冻结倒计时(用户要读/要点,不能读一半消失);
	//   · 移开 = 再给宽限,然后淡出 —— 悬停超过停留时长再移开也是这个节奏;
	//   · 返回 false 表示"该收了",由调用方决定收起来后做什么。
	bool EditorShell::TimedNotice::Update(double now, bool hovered, double staySeconds, double graceSeconds,
		double fadeSeconds)
	{
		if (hovered)
		{
			ShownAt = now;
			LeaveAt = 0.0;
		}
		else if (Hovered)
		{
			LeaveAt = now;
		}
		Hovered = hovered;
		if (hovered)
		{
			Alpha = 1.0f;
			return true;
		}
		const bool leaving = LeaveAt > 0.0;
		const double elapsed = now - (leaving ? LeaveAt : ShownAt);
		const double limit = leaving ? graceSeconds : staySeconds;
		if (elapsed >= limit + fadeSeconds)
			return false;
		Alpha = elapsed <= limit ? 1.0f
			: 1.0f - static_cast<float>((elapsed - limit) / fadeSeconds);
		Alpha = std::clamp(Alpha, 0.0f, 1.0f);
		return true;
	}

	// ---- AI 控制通道 ----

	bool EditorShell::AiTogglePanel(const std::string& panel)
	{
		if (panel.empty() || !IsDeclaredPanel(panel))
			return false;
		// 材质面板是动态实例:先按 id 建出面板对象,再走菜单同一条开关路径。
		if (panel.rfind("material:", 0) == 0)
		{
			EnsureMaterialPanelFromId(panel);
			// 与 OpenMaterialEditor 同一条默认尺寸:材质面板的窄布局会把贴图下拉挤到窗口外
			// (实测 480x340 时 Albedo 组合框在 y=782 → 用户根本看不到)。
			if (!m_Layout.FindFloatMemory(panel, nullptr))
				m_Layout.FloatMemory.push_back({ panel, Wui::WuiRect { 200.0f, 170.0f, 760.0f, 470.0f } });
		}
		if (panel.rfind(kModelPanelPrefix, 0) == 0)
		{
			EnsureModelPanelFromId(panel);
			if (!m_Layout.FindFloatMemory(panel, nullptr))
				m_Layout.FloatMemory.push_back({ panel, Wui::WuiRect { 220.0f, 160.0f, 620.0f, 660.0f } });
		}
		// P4-U13c:动态 prefab 资产窗口 —— 未打开 → 走 OpenPrefabWindow(默认附加到主窗口);
		// 已打开(附加标签或可见独立窗口)→ 走菜单同一条开关路径关闭。
		if (panel.rfind(kPrefabPanelPrefix, 0) == 0)
		{
			EnsurePrefabPanelFromId(panel);
			if (!m_Ctx)
				return false;
			const auto attached = std::find(m_AttachedPanels.begin(), m_AttachedPanels.end(), panel);
			FloatWindowHost* host = FindFloatHost(panel);
			const bool visibleWindow = host && !host->IsHidden();
			if (attached != m_AttachedPanels.end() || visibleWindow)
				TogglePanel(*m_Ctx, panel);
			else
				OpenPrefabWindow(panel.substr(std::strlen(kPrefabPanelPrefix)));
			return true;
		}
		// W9-2:动态脚本面板 —— 未打开 → 走 OpenScriptEditor(默认附加到主窗口);
		// 已打开(附加标签或可见独立窗口)→ 走菜单同一条开关路径关闭。
		if (panel.rfind(kScriptPanelPrefix, 0) == 0)
		{
			EnsureScriptPanelFromId(panel);
			if (!m_Ctx)
				return false;
			const auto attached = std::find(m_AttachedPanels.begin(), m_AttachedPanels.end(), panel);
			FloatWindowHost* host = FindFloatHost(panel);
			const bool visibleWindow = host && !host->IsHidden();
			if (attached != m_AttachedPanels.end() || visibleWindow)
				TogglePanel(*m_Ctx, panel);
			else
				OpenScriptEditorNow(panel.substr(std::strlen(kScriptPanelPrefix)));
			return true;
		}
		// PLUG-T3b:插件面板 —— 未建实例先补建,未打开走默认"附加到主窗口",已打开走切换。
		if (IsPluginPanelId(panel))
		{
			EnsurePluginPanelFromId(panel);
			if (!m_Ctx || !IsDeclaredPanel(panel))
				return false;
			const auto attached = std::find(m_AttachedPanels.begin(), m_AttachedPanels.end(), panel);
			FloatWindowHost* host = FindFloatHost(panel);
			const bool visibleWindow = host && !host->IsHidden();
			if (attached != m_AttachedPanels.end() || visibleWindow)
				TogglePanel(*m_Ctx, panel);
			else
				OpenPanelAttached(panel);
			return true;
		}
		if (!m_Ctx)
			return false;
		TogglePanel(*m_Ctx, panel);
		return true;
	}

	// P3-1②:脚本化"分离 / 挂回"。与顶部挂靠栏拖拽、窗口菜单走同一对既有路径:
	//   分离 = OpenIndependentPanel(复用已隐藏的窗口 / 必要时新建),附加态先把顶栏标签摘掉;
	//   挂回 = AttachIndependentWindowToSlot(OS 窗口隐藏 + 顶栏出现切换标签)。
	// 已处于目标状态时幂等,可读结果写进 message(供脚本直接断言,不必解析布局 JSON)。
	bool EditorShell::AiDetachPanel(const std::string& panel, std::string* message)
	{
		auto fail = [message](const std::string& text)
		{
			if (message)
				*message = text;
			return false;
		};
		if (panel.empty() || !IsDeclaredPanel(panel))
			return fail("unknown panel '" + panel + "'");
		// 形态规则(T03):只有声明为独立窗口的面板才有"附加态/浮动态";停靠形态面板的
		// 拖出是主窗口内的临时浮动,不在这两个命令的语义里(用 ui.open/拖拽改它的位置)。
		if (!IsIndependentPanel(panel))
			return fail("panel '" + panel + "' is a docked panel; ui.detach/ui.attach only apply to independent windows");

		const char* const state = PanelStateLabel(panel);
		if (std::strcmp(state, "floating") == 0)
		{
			// 幂等:已经是目标状态(真 OS 窗口)时不做任何事,只回报现状。
			if (message)
				*message = "already floating: " + panel + " (independent OS window)";
			return true;
		}
		const bool wasAttached = std::strcmp(state, "attached") == 0;
		const std::string before = m_Layout.Serialize();
		OpenIndependentPanel(panel); // 复用已隐藏的窗口;从未打开过则新建(位置走 FloatRectFor 记忆)
		FloatWindowHost* host = FindFloatHost(panel);
		if (!host || host->IsHidden())
		{
			// 窗口没建起来:附加态/标签保持原样,命令可重试(不制造"既没标签也没窗口"的半状态)。
			return fail("cannot show independent window for '" + panel + "'");
		}
		if (wasAttached)
		{
			// 摘掉顶栏标签(与顶栏 ✕ 之后的清理同一份状态):窗口本体保持不变。
			m_AttachedPanels.erase(std::remove(m_AttachedPanels.begin(), m_AttachedPanels.end(), panel),
				m_AttachedPanels.end());
			if (m_ActiveWindowTag == panel)
				m_ActiveWindowTag.clear();
		}
		if (m_Ctx)
			RecordDockChange(*m_Ctx, "detach", panel, before);
		// 与顶栏拖出同一条操作记录(category/action/target 口径一致)。
		if (m_Ctx)
			m_Ctx->RecordOp("float", "detach", panel, wasAttached ? "attached" : "opened");
		SaveLayout();   // 形态变了(挂靠 → 浮窗),立刻落盘(P4-UX10)
		const Wui::WuiRect rect = host->ScreenRect();
		if (message)
		{
			std::ostringstream text;
			text << (wasAttached ? "detached " : "opened floating ") << panel
				<< " (independent window at " << static_cast<int>(rect.X) << "," << static_cast<int>(rect.Y)
				<< " " << static_cast<int>(rect.W) << "x" << static_cast<int>(rect.H) << ")";
			*message = text.str();
		}
		return true;
	}

	// P4-UX15:把停靠面板切到前台。以前只有 ui.focus(独立窗口),后台标签不渲染 →
	// 树里没有它的节点,内容浏览器这类停靠面板无法被脚本驱动。
	bool EditorShell::AiActivatePanel(const std::string& panel, std::string* message)
	{
		auto fail = [message](const std::string& text)
		{
			if (message) *message = text;
			return false;
		};
		if (panel.empty() || !IsDeclaredPanel(panel))
			return fail("unknown panel '" + panel + "'");
		if (IsIndependentPanel(panel))
		{
			if (std::find(m_AttachedPanels.begin(), m_AttachedPanels.end(), panel) != m_AttachedPanels.end())
			{
				m_ActiveWindowTag = panel;
				if (message) *message = "activated " + panel + " (top-bar tab)";
				return true;
			}
			if (FloatWindowHost* host = FindFloatHost(panel))
			{
				host->ActivatePanel(panel);
				host->Focus();
				if (message) *message = "activated " + panel + " (independent window)";
				return true;
			}
			OpenPanelAttached(panel);
			if (message) *message = "opened and activated " + panel;
			return true;
		}
		// 停靠面板:把它所在标签组的 Active 指到它(树里没有 = 先按 Window 菜单同一条路打开)。
		std::function<bool(Wui::DockNode&)> activateInTree = [&](Wui::DockNode& node) -> bool
		{
			if (node.IsTabs())
			{
				for (size_t i = 0; i < node.Panels.size(); ++i)
					if (node.Panels[i] == panel)
					{
						node.Active = i;
						return true;
					}
				return false;
			}
			for (Wui::DockNode& child : node.Children)
				if (activateInTree(child))
					return true;
			return false;
		};
		if (activateInTree(m_Layout.Root))
		{
			// P4-UX16:主窗口正被"已附加的独立窗口"占用时(顶栏标签模式,m_ActiveWindowTag 非空),
			// 停靠区根本不渲染 —— 此时只回 "activated (dock tab)" 就是假话:实测在内容浏览器里
			// 新建材质(顺带打开材质编辑器,它默认附加到主窗口)之后,ui.activate content_browser
			// 返回成功,但面板一个无障碍节点都不回来,后续 ui.invoke 全部落空。
			// 所以切停靠面板时顺手退回停靠视图(用户点顶栏标签的等效动作)。
			const bool leftAttachedView = !m_ActiveWindowTag.empty();
			m_ActiveWindowTag.clear();
			SaveLayout();
			if (message)
				*message = "activated " + panel + " (dock tab)"
					+ (leftAttachedView ? "; left the attached-window view" : "");
			return true;
		}
		if (m_Ctx)
			TogglePanel(*m_Ctx, panel);
		if (message) *message = "opened " + panel;
		return true;
	}

	bool EditorShell::AiAttachPanel(const std::string& panel, std::string* message)
	{
		auto fail = [message](const std::string& text)
		{
			if (message)
				*message = text;
			return false;
		};
		if (panel.empty() || !IsDeclaredPanel(panel))
			return fail("unknown panel '" + panel + "'");
		if (!IsIndependentPanel(panel))
			return fail("panel '" + panel + "' is a docked panel; ui.detach/ui.attach only apply to independent windows");

		if (std::strcmp(PanelStateLabel(panel), "attached") == 0)
		{
			// 幂等:已经是目标状态(顶栏标签 + 隐藏的 OS 窗口)时不做任何事,只回报现状。
			if (message)
				*message = "already attached: " + panel + " (top-bar tag active, OS window hidden)";
			return true;
		}
		const std::string before = m_Layout.Serialize();
		if (!FindFloatHost(panel))
			OpenIndependentPanel(panel); // 未打开过:先按独立窗口建出,再走同一条挂靠路径
		AttachIndependentWindowToSlot(panel);
		if (!FindFloatHost(panel) || std::strcmp(PanelStateLabel(panel), "attached") != 0)
			return fail("cannot attach panel '" + panel + "'");
		if (m_Ctx)
			RecordDockChange(*m_Ctx, "attach", panel, before);
		if (message)
			*message = std::string("attached ") + panel + " (independent window hidden; top-bar tag = "
				+ PanelTitle(panel) + ")";
		return true;
	}

	bool EditorShell::AiRequestFloatCapture(const std::string& panel, const std::string& path)
	{
		FloatWindowHost* host = FindFloatHost(panel);
		if (!host || host->IsHidden() || path.empty())
			return false;
		// 只登记整窗抓图请求:该窗口自己的帧循环会在"UI 提交之后、呈现之前"执行它。
		Renderer::RequestPresentCapture(host->GetPresentTarget(), path,
			static_cast<uint32_t>(host->ScreenRect().W), static_cast<uint32_t>(host->ScreenRect().H));
		return true;
	}

	bool EditorShell::AiRequestPreviewCapture(const std::string& panel, const std::string& path)
	{
		const auto found = m_PanelRegistry.find(panel);
		if (found == m_PanelRegistry.end() || path.empty())
			return false;
		if (auto* material = dynamic_cast<MaterialEditorPanel*>(found->second.get()))
		{
			material->RequestPreviewCapture(path);
			return true;
		}
		return false;
	}

	bool EditorShell::AiResizeWindow(const std::string& panel, float width, float height)
	{
		FloatWindowHost* host = FindFloatHost(panel);
		if (!host || host->IsHidden())
			return false;
		const uint32_t targetWidth = static_cast<uint32_t>(std::max(240.0f, width));
		const uint32_t targetHeight = static_cast<uint32_t>(std::max(160.0f, height));
		host->SetClientSize(targetWidth, targetHeight);
		m_LastFloatRects[panel] = Wui::WuiRect { host->ScreenRect().X, host->ScreenRect().Y,
			static_cast<float>(targetWidth), static_cast<float>(targetHeight) };
		return true;
	}

	std::string EditorShell::AiDescribeState() const
	{
		auto escape = [](const std::string& text)
		{
			std::string out;
			out.reserve(text.size() + 8);
			for (char c : text)
			{
				switch (c)
				{
					case '"': out += "\\\""; break;
					case '\\': out += "\\\\"; break;
					case '\n': out += "\\n"; break;
					case '\r': out += "\\r"; break;
					case '\t': out += "\\t"; break;
					default: out += c; break;
				}
			}
			return out;
		};
		std::ostringstream out;
		out << "{";
		out << "\"docked\":[";
		bool first = true;
		std::vector<Wui::PanelId> docked;
		m_Layout.AllPanels(&docked);
		for (const Wui::PanelId& id : docked)
		{
			if (!first)
				out << ",";
			first = false;
			out << "\"" << escape(id) << "\"";
		}
		out << "],\"floating\":[";
		first = true;
		for (const Wui::DockFloat& entry : m_Layout.Floating)
		{
			if (!first)
				out << ",";
			first = false;
			out << "{\"panel\":\"" << escape(entry.Panel) << "\",\"rect\":[" << entry.Rect.X << "," << entry.Rect.Y
				<< "," << entry.Rect.W << "," << entry.Rect.H << "]}";
		}
		out << "],\"independentWindows\":[";
		first = true;
		for (const std::unique_ptr<FloatWindowHost>& host : m_FloatHosts)
		{
			if (host->IsHidden())
				continue;
			if (!first)
				out << ",";
			first = false;
			const Wui::WuiRect rect = host->ScreenRect();
			out << "{\"panel\":\"" << escape(host->Panel()) << "\",\"rect\":[" << rect.X << "," << rect.Y
				<< "," << rect.W << "," << rect.H << "],\"tabCount\":" << host->Panels().size() << "}";
		}
		out << "],\"materials\":[";
		first = true;
		for (const auto& entry : m_PanelRegistry)
		{
			const auto* material = dynamic_cast<const MaterialEditorPanel*>(entry.second.get());
			if (!material)
				continue;
			const Ref<Material>& asset = material->GetMaterial();
			if (!first)
				out << ",";
			first = false;
			out << "{\"panel\":\"" << escape(entry.first) << "\",\"path\":\"" << escape(material->GetMaterialPath()) << "\"";
			if (asset)
			{
				const MaterialDesc& desc = asset->GetDesc();
				out << ",\"albedo\":\"" << escape(desc.AlbedoTexture) << "\""
					<< ",\"normal\":\"" << escape(desc.NormalTexture) << "\""
					<< ",\"revision\":" << asset->GetRevision()
					<< ",\"baseColor\":[" << desc.BaseColor.r << "," << desc.BaseColor.g << ","
					<< desc.BaseColor.b << "," << desc.BaseColor.a << "]"
					<< ",\"blendMode\":" << static_cast<int>(desc.BlendMode);
			}
			out << "}";
		}
		// P3-1②:"附加/浮动态"是第一手断言目标(ui.open → ui.detach → ui.attach 的验收),
		// 这里按面板 id 列出形态(attached/floating/hidden),与 AttachTag 无障碍节点的
		// value 走同一个 PanelStateLabel —— 脚本不必去解析 floating/independentWindows 记录。
		std::vector<std::string> attachPanels = m_Panels;
		for (const auto& entry : m_PanelRegistry)
			if (std::find(attachPanels.begin(), attachPanels.end(), entry.first) == attachPanels.end())
				attachPanels.push_back(entry.first);
		for (const std::string& id : m_AttachedPanels)
			if (std::find(attachPanels.begin(), attachPanels.end(), id) == attachPanels.end())
				attachPanels.push_back(id);
		out << "],\"attach\":[";
		first = true;
		for (const std::string& id : attachPanels)
		{
			// 只有独立窗口形态的面板才有 attached/floating 两态;停靠面板的浮动是主窗口内
			// 临时浮动,不属于本清单(形态规则见 T03 / PanelStateLabel)。
			if (!IsDeclaredPanel(id) || !IsIndependentPanel(id))
				continue;
			if (!first)
				out << ",";
			first = false;
			out << "{\"panel\":\"" << escape(id) << "\",\"state\":\"" << PanelStateLabel(id) << "\"}";
		}
		out << "]}";
		return out.str();
	}

	// U26:窗口几何(客户区屏幕原点 + 尺寸,均为物理像素),键 = 无障碍节点的 window。
	// 主窗口 = GLFW 内容区原点(与注入窗口一致);独立窗口 = FloatWindowHost::ScreenRect
	// (同一份数据也是跨窗口拖放命中用的原点)。隐藏态(附加到主窗口)照常列出,
	// 但 visible=false —— 脚本由此能判断"面板在主窗口里"(那时节点 window 键是 main)。
	std::string EditorShell::AiWindowRectsJson() const
	{
		auto escape = [](const std::string& text)
		{
			std::string out;
			out.reserve(text.size() + 8);
			for (char c : text)
			{
				switch (c)
				{
					case '"': out += "\\\""; break;
					case '\\': out += "\\\\"; break;
					case '\n': out += "\\n"; break;
					case '\r': out += "\\r"; break;
					case '\t': out += "\\t"; break;
					default: out += c; break;
				}
			}
			return out;
		};
		std::ostringstream out;
		out << "[";
		bool first = true;
		if (Application::HasInstance())
		{
			int x = 0, y = 0;
			Application::Get().GetWindow().GetPosition(&x, &y);
			out << "{\"window\":\"main\",\"x\":" << x << ",\"y\":" << y
				<< ",\"w\":" << Application::Get().GetWindow().GetWidth()
				<< ",\"h\":" << Application::Get().GetWindow().GetHeight()
				<< ",\"visible\":true,\"state\":\"main\",\"tabs\":1}";
			first = false;
		}
		for (const std::unique_ptr<FloatWindowHost>& host : m_FloatHosts)
		{
			if (!first)
				out << ",";
			first = false;
			const Wui::WuiRect rect = host->ScreenRect();
			out << "{\"window\":\"float:" << escape(host->Panel())
				<< "\",\"x\":" << rect.X << ",\"y\":" << rect.Y
				<< ",\"w\":" << rect.W << ",\"h\":" << rect.H
				<< ",\"visible\":" << (host->IsHidden() ? "false" : "true")
				<< ",\"state\":\"" << PanelStateLabel(host->Panel())
				<< "\",\"tabs\":" << host->Panels().size() << "}";
		}
		out << "]";
		return out.str();
	}

	std::string EditorShell::IndependentWindowPanel(size_t index) const
	{
		// 返回该窗口的代表面板(活动标签);调用方以它定位窗口(焦点/收回/挂靠)。
		if (index >= m_FloatHosts.size() || m_FloatHosts[index]->IsHidden())
			return std::string();
		return m_FloatHosts[index]->Panel();
	}

	size_t EditorShell::IndependentWindowCount() const
	{
		size_t count = 0;
		for (const std::unique_ptr<FloatWindowHost>& host : m_FloatHosts)
			if (!host->IsHidden())
				++count;
		return count;
	}

	std::string EditorShell::IndependentWindowLabel(size_t index) const
	{
		if (index >= m_FloatHosts.size())
			return std::string();
		std::string label;
		for (size_t i = 0; i < m_FloatHosts[index]->Panels().size(); ++i)
		{
			if (i != 0)
				label += " | ";
			label += PanelTitle(m_FloatHosts[index]->Panels()[i]);
		}
		return label;
	}

	FloatWindowHost* EditorShell::FindFloatHost(const std::string& panel)
	{
		for (const std::unique_ptr<FloatWindowHost>& host : m_FloatHosts)
			if (host->Contains(panel))
				return host.get();
		return nullptr;
	}

	void EditorShell::EraseFloatHost(FloatWindowHost* host)
	{
		if (!host)
			return;
		// 每窗口屏幕位置缓存以标签为键:宿主销毁时一并清理,避免重建后误判"停稳"。
		for (const std::string& panel : host->Panels())
			m_LastFloatScreenRects.erase(panel);
		m_FloatHosts.erase(std::remove_if(m_FloatHosts.begin(), m_FloatHosts.end(),
			[&](const std::unique_ptr<FloatWindowHost>& candidate) { return candidate.get() == host; }),
			m_FloatHosts.end());
	}

	void EditorShell::FocusIndependentWindow(const std::string& panel)
	{
		if (FloatWindowHost* host = FindFloatHost(panel))
			host->Focus();
	}

	void EditorShell::OpenMaterialEditor(const std::string& path)
	{
		// 每个材质一个面板 + 独立窗口:同一材质重复打开只是前置焦点(用户 2026-09-16)。
		const std::string panelId = std::string(kMaterialPanelPrefix) + MaterialLibrary::NormalizePath(path);
		if (m_PanelRegistry.find(panelId) == m_PanelRegistry.end())
		{
			auto panel = std::make_unique<MaterialEditorPanel>(path);
			panel->OpenMaterial(path);
			m_PanelRegistry.emplace(panelId, std::move(panel));
			m_Panels.push_back(panelId);
			m_Layout.FloatMemory.push_back({ panelId, Wui::WuiRect { 200.0f, 170.0f, 760.0f, 470.0f } });
		}
		// D10:默认附加到主窗口(用户 2026-09-19);已拖成独立窗口的按原样只前置焦点。
		OpenPanelAttached(panelId);
	}

	void EditorShell::EnsureMaterialPanelFromId(const std::string& panelId)
	{
		if (m_PanelRegistry.find(panelId) != m_PanelRegistry.end())
			return;
		if (panelId.compare(0, std::strlen(kMaterialPanelPrefix), kMaterialPanelPrefix) != 0)
			return;
		const std::string path = panelId.substr(std::strlen(kMaterialPanelPrefix));
		if (path == "(unsaved)")
			return;   // 未落盘的临时面板无法跨会话恢复
		auto panel = std::make_unique<MaterialEditorPanel>(path);
		panel->OpenMaterial(path);
		m_PanelRegistry.emplace(panelId, std::move(panel));
		if (std::find(m_Panels.begin(), m_Panels.end(), panelId) == m_Panels.end())
			m_Panels.push_back(panelId);
	}

	// ---- P1b D5:模型预览面板(动态实例,id = "model:<逻辑路径>")----

	void EditorShell::OpenModelPreview(const std::string& logicalPath)
	{
		// 每个模型一个面板 + 独立窗口:重复双击只是前置焦点(与材质编辑器同款;
		// 用户反馈"多次打开会多次叠加"—— 现在打开预览不改场景,放进场景在预览里显式点)。
		std::string path = logicalPath;
		std::replace(path.begin(), path.end(), '\\', '/');
		if (path.empty())
			return;
		const std::string panelId = std::string(kModelPanelPrefix) + path;
		if (m_PanelRegistry.find(panelId) == m_PanelRegistry.end())
		{
			auto panel = std::make_unique<ModelPreviewPanel>(path);
			m_PanelRegistry.emplace(panelId, std::move(panel));
			m_Panels.push_back(panelId);
			m_Layout.FloatMemory.push_back({ panelId, Wui::WuiRect { 220.0f, 160.0f, 620.0f, 660.0f } });
		}
		// D10:默认附加到主窗口(用户 2026-09-19);已拖成独立窗口的按原样只前置焦点。
		OpenPanelAttached(panelId);
	}

	// ---- P4-U13c:prefab 资产窗口(动态实例,id = "prefab:<逻辑路径>")----

	void EditorShell::OpenPrefabWindow(const std::string& logicalPath)
	{
		OpenPrefabWindowChecked(logicalPath, nullptr);
	}

	bool EditorShell::OpenPrefabWindowChecked(const std::string& logicalPath, std::string* message)
	{
		// 与 `.wmodel` 预览同款:每个 prefab 一个面板 + 默认附加到主窗口;
		// 重复打开 = 立刻重读一次 + 前置焦点(资产的"刷新"入口)。
		std::string path = logicalPath;
		std::replace(path.begin(), path.end(), '\\', '/');
		if (path.empty())
		{
			if (message)
				*message = "empty prefab path";
			return false;
		}
		const std::string panelId = std::string(kPrefabPanelPrefix) + path;
		PrefabPanel* panel = nullptr;
		if (const auto found = m_PanelRegistry.find(panelId); found != m_PanelRegistry.end())
		{
			panel = dynamic_cast<PrefabPanel*>(found->second.get());
		}
		else
		{
			auto created = std::make_unique<PrefabPanel>(path);
			panel = created.get();
			m_PanelRegistry.emplace(panelId, std::move(created));
			m_Panels.push_back(panelId);
			m_Layout.FloatMemory.push_back({ panelId, Wui::WuiRect { 220.0f, 160.0f, 620.0f, 560.0f } });
		}
		if (!panel)
		{
			if (message)
				*message = "panel id '" + panelId + "' is not a prefab window";
			return false;
		}
		// 打开/前置之前先把这份资产读一遍:失败也要开窗口 —— 状态行写可读原因,
		// 调用方(AI 通道)拿到 false + 同一条原因,不存在"静默成功"。
		const bool readable = panel->ReloadNow(m_Editor.GetActiveScene().get(), message);
		OpenPanelAttached(panelId);
		return readable;
	}

	// P4-U13e:脚本化写字段 —— 复用面板自己的写入口(不是旁路:同样走脏标记/状态行/资产警告)。
	bool EditorShell::SetPrefabPanelField(const std::string& panelId, const std::string& component,
		const std::string& field, const std::string& value, const std::string& axis, std::string* message)
	{
		PrefabPanel* prefab = PrefabPanelById(panelId);
		if (!prefab)
		{
			if (message)
				*message = "no prefab window for panel '" + panelId + "'";
			return false;
		}
		return prefab->SetEditableField(component, field, value, axis, message);
	}

	void EditorShell::RequestImportDestination(const std::string& sourcePath)
	{
		// D10-10(用户 2026-09-19):导入位置选择器改成**窗口级模态**(此前是内容浏览器面板
		// 内的一层覆盖:位置偏、挡不住后面的输入)。范围仍限定内容根内 —— 原生文件夹对话框
		// 能选到工作区外,那种位置场景/打包都引用不到。状态与目录树由 shell 持有。
		if (sourcePath.empty())
			return;
		m_ImportSourcePath = std::filesystem::path(sourcePath);
		m_ImportStatus.clear();
		// 内容根 = 运行期当前项目根(World::Paths):换项目(--project / WLD_PROJECT_DIR)后
		// 导入位置选择器跟着切,不再固定成编译期默认项目。
		m_ImportTreeRoot = World::Paths::AssetRoot();
		m_ImportDestDir = m_ImportTreeRoot;
		const std::string panel = "content_browser";
		if (!m_Layout.Contains(panel))
			DockPanelBackToTree(panel);   // 内容浏览器被关掉时先让它回到停靠树(导入完能直接看到新文件)
		const auto found = m_PanelRegistry.find(panel);
		if (found != m_PanelRegistry.end())
			if (auto* browser = dynamic_cast<ContentBrowserPanel*>(found->second.get()))
				m_ImportDestDir = browser->CurrentDirectory();   // 默认落点 = 当前文件夹
		// 打开时扫一次内容根 + 重置模态自己的树状态(根行默认展开)。
		ScanImportTree();
		m_ImportTreeOpen.clear();
		// 默认落点(内容浏览器当前文件夹)可能在深层:把它的祖先链一起展开,
		// 打开时就能看到"选中"的那一行(只展开,不改选中)。
		for (std::filesystem::path dir = m_ImportDestDir; dir != m_ImportTreeRoot && dir.has_relative_path();)
		{
			m_ImportTreeOpen.insert(dir);
			const std::filesystem::path parent = dir.parent_path();
			if (parent == dir)
				break;   // 防御:到达盘符根仍不等于内容根时停止
			dir = parent;
		}
		m_ImportTreeOpen.insert(m_ImportTreeRoot);
		m_ImportTreeScroll = 0.0f;
		m_ImportModalOpen = true;
		if (m_Ctx)
		{
			// 打开前清掉悬着的弹窗;清文本焦点,否则后面面板里已聚焦的输入框还会继续吃键盘输入。
			m_Ctx->CloseAllPopups();
			m_Ctx->SetFocus(0);
			m_Ctx->RecordOp("import", "dest-open", m_ImportSourcePath.filename().string(), "");
		}
		WLD_CORE_INFO("[import] 选择导入位置(窗口级模态): {0}", sourcePath);
	}

	void EditorShell::Notify(const std::string& message)
	{
		// P4-UX16:面板级短反馈统一进状态栏提示(4s 停留 / 悬停冻结 / 移开 2.6s 宽限后淡出)。
		// 走同一条 PushNotice = 同一条无障碍节点,脚本与读屏也能读到这句话。
		PushNotice(message);
	}

	// D10-10:扫内容根下的全部子目录(低频操作:只在打开导入模态时跑一次)。
	// 与内容浏览器左侧树同一套数据形态:按路径排序 + 根行在最前(Depth 0)。
	void EditorShell::ScanImportTree()
	{
		m_ImportTree.clear();
		std::error_code scanError;
		std::filesystem::recursive_directory_iterator scanIt(m_ImportTreeRoot,
			std::filesystem::directory_options::skip_permission_denied, scanError);
		const std::filesystem::recursive_directory_iterator scanEnd;
		for (; scanIt != scanEnd; scanIt.increment(scanError))
		{
			if (scanError)
				break;   // 权限错误等:已扫到的部分照常可用(不抛异常、不中断整个选择器)
			const std::filesystem::directory_entry& entry = *scanIt;
			std::error_code entryError;
			if (!entry.is_directory(entryError))
				continue;
			ImportTreeRow row;
			row.Path = entry.path();
			row.Depth = scanIt.depth() + 1;
			std::error_code childError;
			for (const std::filesystem::directory_entry& child : std::filesystem::directory_iterator(row.Path,
				std::filesystem::directory_options::skip_permission_denied, childError))
			{
				if (childError)
					break;
				std::error_code childDirError;
				if (child.is_directory(childDirError))
				{
					row.HasChildren = true;
					break;
				}
			}
			m_ImportTree.push_back(std::move(row));
		}
		std::sort(m_ImportTree.begin(), m_ImportTree.end(),
			[](const ImportTreeRow& a, const ImportTreeRow& b) { return a.Path < b.Path; });
		ImportTreeRow rootRow;
		rootRow.Path = m_ImportTreeRoot;
		rootRow.Depth = 0;
		rootRow.HasChildren = !m_ImportTree.empty();
		m_ImportTree.insert(m_ImportTree.begin(), std::move(rootRow));
	}

	// D10-11:窗口级"选择导入位置"模态 —— 居中/遮罩/标题栏/Esc/按钮条走 WuiModal 组件;
	// "挡住后面所有面板的命中"由 OnRender 的 BeginModalInputBlock/EndModalInputBlock 成对负责
	// (见那里的顺序说明)。状态行仍归 shell(它有稳定的无障碍 id import.dest.status)。
	void EditorShell::RenderImportDestinationModal(Wui::WuiContext& ctx)
	{
		const Wui::WuiId modalId = Wui::HashId("modal.importdest");
		if (m_ImportModalOpen)
			ctx.SetModal(modalId);
		else if (ctx.Modal() == modalId)
			ctx.ClearModal();

		const auto logicalText = [this](const std::filesystem::path& dir)
		{
			const std::string relative = dir.lexically_relative(m_ImportTreeRoot).generic_string();
			return (relative.empty() || relative == ".") ? std::string("(内容根)") : relative;
		};

		Wui::WuiRect panel;
		bool escapePressed = false;
		Wui::ModalFrameDesc frameDesc;
		frameDesc.Id = modalId;
		frameDesc.Title = "选择导入位置";
		frameDesc.Size = { 560.0f, 440.0f };
		if (!Wui::BeginModalFrame(ctx, frameDesc, &panel, &escapePressed, m_Theme))
			return;

		const float pad = 16.0f;
		Wui::Label(ctx, { panel.X + pad, panel.Y + 40.0f },
			"源文件: " + m_ImportSourcePath.filename().string()
				+ " · 导入到: " + logicalText(m_ImportDestDir),
			m_Theme.TextMuted, 13.0f);

		const float statusH = 20.0f;
		// 按钮条固定贴 frame 底部(ModalFooter 画),状态行排在它上方。
		const float statusY = panel.Y + panel.H - Wui::ModalFooterPadding - Wui::ModalFooterHeight - 6.0f - statusH;
		const Wui::WuiRect treeArea { panel.X + pad, panel.Y + 62.0f, panel.W - pad * 2.0f,
			std::max(40.0f, statusY - 8.0f - (panel.Y + 62.0f)) };
		Wui::PanelBackground(ctx, treeArea, { 0.09f, 0.095f, 0.10f, 1 });

		// 可见行:父行折叠 → 整棵子树不显示(m_ImportTree 是"父在子前"的预排序)。
		std::vector<const ImportTreeRow*> visibleRows;
		std::vector<Wui::TreeViewItem> treeItems;
		std::vector<Wui::WuiId> treeItemIds;
		std::vector<bool> openAtDepth;
		for (const ImportTreeRow& row : m_ImportTree)
		{
			if (row.Depth > 0)
			{
				if (row.Depth - 1 >= static_cast<int>(openAtDepth.size()))
					continue;   // 祖先行没显示 → 本行也不显示
				if (!openAtDepth[row.Depth - 1])
					continue;   // 直接父行折叠
			}
			if (static_cast<int>(openAtDepth.size()) > row.Depth)
				openAtDepth.resize(static_cast<size_t>(row.Depth));
			const bool expanded = m_ImportTreeOpen.find(row.Path) != m_ImportTreeOpen.end();
			openAtDepth.push_back(expanded);

			const std::filesystem::path rel = row.Path.lexically_relative(m_ImportTreeRoot);
			const std::string relText = (rel == ".") ? std::string() : rel.generic_string();
			Wui::TreeViewItem item;
			// 无障碍 id 约定(逐字,与 D10-9 一致):根行 = import.dest.tree.root,
			// 其它 = import.dest.tree.<相对路径>。
			item.Id = Wui::HashId(relText.empty() ? "import.dest.tree.root"
				: ("import.dest.tree." + relText).c_str());
			item.Label = row.Path.filename().string();   // 根行 = 内容根目录名
			item.Depth = row.Depth;
			item.HasChildren = row.HasChildren;
			item.Expanded = expanded;
			item.Selected = m_ImportDestDir == row.Path;
			visibleRows.push_back(&row);
			treeItemIds.push_back(item.Id);
			treeItems.push_back(std::move(item));
		}
		const Wui::TreeViewResult tree = Wui::TreeView(ctx, treeArea, treeItems, 20.0f, m_ImportTreeScroll, m_Theme);
		for (size_t i = 0; i < visibleRows.size(); ++i)
		{
			const ImportTreeRow& row = *visibleRows[i];
			if (tree.ClickedArrow == static_cast<int>(i))
			{
				if (treeItems[i].Expanded)
					m_ImportTreeOpen.erase(row.Path);
				else
					m_ImportTreeOpen.insert(row.Path);
			}
			else if (tree.Clicked == static_cast<int>(i))
			{
				m_ImportDestDir = row.Path;   // 单选:点行只改选中,不导航
			}
			// TreeView 自身不登记行节点(已知限制),按上面的 id 约定手动登记:
			// AI 可读可点;只登记落在树可视区内的行,避免点到看不见的行。
			if (i < tree.ItemRects.size() && treeItemIds[i] != 0)
			{
				const Wui::WuiRect& rowRect = tree.ItemRects[i];
				// 只登记"行中心确实落在树可视区内"的行:AI 注入的点击打在行中心,
				// 半滚出视口的行中心可能压到按钮行,点了会打错目标。
				const float rowCenterY = rowRect.Y + rowRect.H * 0.5f;
				if (rowRect.W > 0.0f && rowRect.H > 0.0f
					&& rowCenterY >= treeArea.Y && rowCenterY <= treeArea.Y + treeArea.H)
				{
					Wui::WuiAccessNode rowNode;
					rowNode.Id = treeItemIds[i];
					rowNode.Window = Wui::WuiAccessibility::Get().CurrentWindow();
					rowNode.Panel = "shell";
					rowNode.Kind = "tree-item";
					rowNode.Label = treeItems[i].Label;
					rowNode.Value = logicalText(row.Path);
					rowNode.Rect = rowRect;
					rowNode.Interactive = true;
					Wui::WuiAccessibility::Get().Register(rowNode);
				}
			}
		}

		bool closeRequested = false;
		const Wui::ModalResult footerResult = Wui::ModalFooter(ctx, panel, "导入到此文件夹", "取消",
			Wui::HashId("import.dest.ok"), Wui::HashId("import.dest.cancel"), true, m_Theme);
		if (footerResult == Wui::ModalResult::Confirm)
		{
			// 目的地 = 选中目录相对内容根的路径;**内容根本身传空串**(与内核/cook 约定一致)。
			const std::filesystem::path destRelative = m_ImportDestDir.lexically_relative(m_ImportTreeRoot);
			const std::string destRelativeText = destRelative.generic_string();
			const std::string destination =
				(destRelativeText.empty() || destRelativeText == ".") ? std::string() : destRelativeText;
			std::string message;
			std::string logicalModel;
			if (m_Editor.ImportModelFile(m_ImportSourcePath.string(), &message, &logicalModel, destination))
			{
				m_ImportStatus = message.empty() ? ("已导入到 " + logicalText(m_ImportDestDir)) : message;
				ctx.RecordOp("import", "dest-ok", m_ImportSourcePath.filename().string(), m_ImportStatus);
				WLD_CORE_INFO("[import] {0}", m_ImportStatus);
				// 内容浏览器刷新(它自己的公开入口)+ 模型预览。
				const auto browserFound = m_PanelRegistry.find("content_browser");
				if (browserFound != m_PanelRegistry.end())
					if (auto* browser = dynamic_cast<ContentBrowserPanel*>(browserFound->second.get()))
						browser->RefreshContents();
				if (!logicalModel.empty())
					OpenModelPreview(logicalModel);
				closeRequested = true;   // 成功后关闭;失败保持打开,用户可改选目录重试。
			}
			else
			{
				m_ImportStatus = message.empty() ? std::string("导入失败(宿主未给出原因)") : message;
				ctx.RecordOp("import", "dest-failed", m_ImportSourcePath.filename().string(), m_ImportStatus);
				WLD_CORE_WARN("[import] 导入 '{0}' 失败: {1}", m_ImportSourcePath.string(), m_ImportStatus);
			}
		}
		else if (footerResult == Wui::ModalResult::Cancel)
		{
			ctx.RecordOp("import", "dest-cancel", m_ImportSourcePath.filename().string(), "");
			closeRequested = true;
		}
		if (escapePressed)
			closeRequested = true;

		// 状态行:选中目录 + 上一次导入的可读结果(失败原因也写在这里)。
		const std::string statusText = "选中: " + logicalText(m_ImportDestDir)
			+ (m_ImportStatus.empty() ? std::string() : (" · " + m_ImportStatus));
		{
			Wui::WuiAccessNode statusNode;
			statusNode.Id = Wui::HashId("import.dest.status");
			statusNode.Window = Wui::WuiAccessibility::Get().CurrentWindow();
			statusNode.Panel = "shell";
			statusNode.Kind = "status";
			statusNode.Label = "import destination status";
			statusNode.Value = statusText;
			statusNode.Rect = { panel.X + pad, statusY, panel.W - pad * 2.0f, statusH };
			statusNode.Interactive = false;
			Wui::WuiAccessibility::Get().Register(statusNode);
		}
		Wui::Label(ctx, { panel.X + pad, statusY }, statusText,
			m_ImportStatus.empty() ? m_Theme.TextMuted : m_Theme.Text, 13.0f);

		if (closeRequested)
		{
			m_ImportModalOpen = false;
			m_ImportStatus.clear();
			ctx.ClearModal();
		}
		Wui::EndModalFrame(ctx);
	}

	// ---- CPPT-6-ED-NEWSCRIPT:File ▸ New C++ Component… ----
	//
	// 与内容浏览器的新建材质 / 新建着色器向导同一套交互骨架(名称 + 实时落点 + 行内错误 +
	// Enter 确认 / Esc 取消),差别只有两点:
	//   * 落点在**当前项目层**(`<项目根>/src/Components/<Name>.h`,PROJ-8/T1;不在内容根里);
	//   * 创建后**外部 Visual Studio** 打开(CPPT-7:内置脚本编辑器只服务 Lua/Luau)。
	// 模板是纯 ECS 组件(纯数据 + WE_FIELD 反射;逻辑归 src/Systems/ 的系统),
	// 语法与示例项目模板的 `src/Components/SampleDataComponent.h` 一致,随项目构建进 Game.dll。
	namespace
	{
		// 名称 → 文件名:去掉用户可能顺手输入的 `.h` 后缀与首尾空白(与内容浏览器同名口径)。
		std::string CppScriptBaseName(const std::string& text)
		{
			std::string name = text;
			while (!name.empty() && (name.front() == ' ' || name.front() == '\t'))
				name.erase(name.begin());
			while (!name.empty() && (name.back() == ' ' || name.back() == '\t'))
				name.pop_back();
			if (name.size() > 2 && name.compare(name.size() - 2, 2, ".h") == 0)
				name.erase(name.size() - 2);
			return name;
		}

		// 合法 C++ 标识符:[A-Za-z_][A-Za-z0-9_]*。
		bool IsValidCppIdentifier(const std::string& name)
		{
			if (name.empty())
				return false;
			const auto letter = [](unsigned char c)
			{
				return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
			};
			if (!letter(static_cast<unsigned char>(name[0])) && name[0] != '_')
				return false;
			for (const char character : name)
			{
				const unsigned char c = static_cast<unsigned char>(character);
				if (!letter(c) && !(c >= '0' && c <= '9') && c != '_')
					return false;
			}
			return true;
		}

		// 模板正文:头注释(组件 = 纯数据 / 逻辑写在 src/Systems/ / 构建后重载 C++ 模块)+ 标量
		// (Default/Range/Unit/Step/Doc)+ 枚举 + 命名 struct(Object, Of(...))+
		// Array/Map 容器 + `WE_SCHEMA_BODY(Game, <Name>, Component)`。
		// 每个类型名都带组件名前缀(`<Name>Mode` / `<Name>Data`):文件名唯一由校验保证,
		// 生成注册单元同时包含多个组件头时也不会重定义。
		std::string NewCppScriptTemplateSource(const std::string& name)
		{
			std::string source;
			source += "#pragma once\n";
			source += "#include \"World/Scene/Components.h\"\n\n";
			source += "#include <map>\n";
			source += "#include <string>\n";
			source += "#include <vector>\n\n";
			source += "namespace World\n";
			source += "{\n";
			source += "\t// ============================================================================\n";
			source += "\t// " + name + " — 由编辑器「文件 ▶ 新建 C++ 组件…」生成的纯 ECS 组件模板。\n";
			source += "\t//\n";
			source += "\t// ① 这是**组件 = 纯数据**:只声明字段与 WE_FIELD 反射元数据,不写逻辑;\n";
			source += "\t//    编辑器属性面板按 WE_FIELD 的声明自动画控件。\n";
			source += "\t// ② 逻辑写在 <项目根>/src/Systems/ 的系统里,并在 <项目根>/src/GameProject.cpp 里挂载:\n";
			source += "\t//    AttachProjectSystems 里 scene.RegisterSystem<...>();\n";
			source += "\t//    DetachProjectSystems 里 scene.UnregisterFrameSystem(\"...\")。\n";
			source += "\t//    系统只活在一次运行时内(Play 启停各一次)。\n";
			source += "\t// ③ 构建项目后回编辑器执行「文件 ▶ 重载 C++ 模块」加载新组件。\n";
			source += "\t//\n";
			source += "\t// 结构与示例项目模板 src/Components/SampleDataComponent.h 一致:标量 / 枚举 / 命名\n";
			source += "\t// struct / Array / Map 各留一行范例,不需要的字段整行删掉即可。只有被编辑过的值才写进\n";
			source += "\t// 场景(.wd),未编辑时用成员初始化里的默认值。\n";
			source += "\t// ============================================================================\n\n";
			source += "\t// 枚举:WE_ENUM_SCHEMA 注册后,`Enum, Of(...)` 在面板里是下拉框,存档写整数。\n";
			source += "\tenum class " + name + "Mode : int32_t\n";
			source += "\t{\n";
			source += "\t\tIdle = 0,\n";
			source += "\t\tActive = 1,\n";
			source += "\t};\n";
			source += "\tWE_ENUM_SCHEMA(Game, " + name + "Mode, Int32)\n";
			source += "\t\tWE_ENUM_VALUE(Idle);\n";
			source += "\t\tWE_ENUM_VALUE(Active);\n";
			source += "\tWE_ENUM_END\n\n";
			source += "\t// 命名 struct:字段模型只声明一次;标量字段与容器元素复用同一份(面板展开成子行)。\n";
			source += "\tstruct " + name + "Data\n";
			source += "\t{\n";
			source += "\t\tfloat Amount = 1.0f;\n";
			source += "\t\tint32_t Count = 0;\n\n";
			source += "\t\tWE_SCHEMA_BODY(Game, " + name + "Data, Struct)\n";
			source += "\t\t\tWE_FIELD(Amount, Float, Range(0.0f, 1000.0f),\n";
			source += "\t\t\t\tDoc(\"Nested struct sample: one editable number.\"));\n";
			source += "\t\t\tWE_FIELD(Count, Int32,\n";
			source += "\t\t\t\tDoc(\"Nested struct sample: one editable integer.\"));\n";
			source += "\t\tWE_SCHEMA_END\n";
			source += "\t};\n\n";
			source += "\t// 纯数据组件:不继承任何基类、不写生命周期方法(旧 ScriptableEntity 已删除)。\n";
			source += "\tstruct " + name + "\n";
			source += "\t{\n";
			source += "\t\t// ---- 标量:Default/Range/Unit/Step/Doc ----\n";
			source += "\t\tfloat Speed = 1.0f;\n";
			source += "\t\t" + name + "Mode Mode = " + name + "Mode::Idle;\n";
			source += "\t\t// ---- 命名 struct:Object, Of(...) ----\n";
			source += "\t\t" + name + "Data Data {};\n";
			source += "\t\t// ---- 容器:std::vector<元素> / std::map<std::string, 元素> ----\n";
			source += "\t\tstd::vector<float> Values {};\n";
			source += "\t\tstd::map<std::string, float> Weights {};\n\n";
			source += "\t\tWE_SCHEMA_BODY(Game, " + name + ", Component)\n";
			source += "\t\t\tWE_SCHEMA_META(Category(\"Project\"),\n";
			source += "\t\t\t\tDoc(\"C++ component template generated from the editor: scalar with edit metadata, enum, nested struct and Array/Map container samples.\"))\n";
			source += "\t\t\tWE_FIELD(Speed, Float, Default(1.0f), Range(0.0f, 100.0f), Unit(\"m/s\"), Step(0.1f),\n";
			source += "\t\t\t\tDoc(\"Scalar sample: Step(0.1) is the drag increment; Unit('m/s') is drawn after the value.\"));\n";
			source += "\t\t\tWE_FIELD(Mode, Enum, Of(" + name + "Mode),\n";
			source += "\t\t\t\tDoc(\"Enum sample: the dropdown stores the integer value in the scene.\"));\n";
			source += "\t\t\tWE_FIELD(Data, Object, Of(" + name + "Data),\n";
			source += "\t\t\t\tDoc(\"Nested struct sample: expandable child rows declared once by " + name + "Data.\"));\n";
			source += "\t\t\tWE_FIELD(Values, Array, Of(Float),\n";
			source += "\t\t\t\tDoc(\"Array<Float> sample: one row per element; '+' appends and '-' removes.\"));\n";
			source += "\t\t\tWE_FIELD(Weights, Map, Of(Float),\n";
			source += "\t\t\t\tDoc(\"Map<Float> sample: string key -> number; '+' asks for the key name first.\"));\n";
			source += "\t\tWE_SCHEMA_END\n";
			source += "\t};\n";
			source += "}\n";
			return source;
		}
	}

	std::filesystem::path EditorShell::NewCppScriptTargetPath() const
	{
		// PROJ-8/T1:落点在**当前项目层** —— `<项目根>/src/Components/<Name>.h`(项目根取运行期
		// World::Paths::ProjectDir(),与进程 CWD 无关)。引擎里的 `Game/src` 只剩模块骨架,
		// 用户的 C++ 组件源码属于项目:项目 CMake 的 schema 发现 glob 恰好是
		// `<项目根>/src/Components/*.h`,随项目构建进 Game.dll。
		// 没有当前项目时 CurrentProjectRoot() 为空 → 调用方先给可读提示,不会走到写盘。
		return CurrentProjectRoot() / "src" / "Components"
			/ (CppScriptBaseName(m_NewCppScriptName) + ".h");
	}

	std::string EditorShell::NewCppScriptNameError() const
	{
		// 没有当前项目(启动器 / 未打开项目):先给"先打开/新建项目"这条可读原因,
		// 不再去拼一个相对路径(那会按 CWD 误判"已存在")。
		if (CurrentProjectRoot().empty())
			return Wui::Tr("modal.newscript.no_project",
				"No project is open — open or create a project in the launcher first.");
		const std::string name = CppScriptBaseName(m_NewCppScriptName);
		if (name.empty())
			return Wui::Tr("modal.newscript.name.empty", "Name cannot be empty");
		if (!IsValidCppIdentifier(name))
			return Wui::Tr("modal.newscript.name.invalid",
				"Name must be a valid C++ identifier: start with a letter or '_' and use only "
				"letters, digits and '_'");
		std::error_code existsError;
		if (std::filesystem::exists(NewCppScriptTargetPath(), existsError))
		{
			// 落点回显 = 相对**项目根**(与项目源码视图的行标签同一口径)。
			const std::string relative = "src/Components/" + name + ".h";
			return Wui::TrFormat("modal.newscript.name.exists",
				"A file with this name already exists: {path}", { { "path", relative } });
		}
		return {};
	}

	void EditorShell::OpenNewCppScriptModal(Wui::WuiContext& ctx)
	{
		// PROJ-8/T1:没有当前项目时不打开一个写不了盘的模态 —— 给一条可读提示,并把这次
		// 意图记进操作日志(与真实菜单动作同口径,方便自动化断言"点过但被拦下")。
		if (CurrentProjectRoot().empty())
		{
			const std::string notice = Wui::Tr("notice.newscript.no_project",
				"No project is open — open or create a project in the launcher first, "
				"then create C++ components.");
			PushNotice(notice);
			ctx.RecordOp("script", "new-cpp-no-project", m_NewCppScriptName, "");
			WLD_CORE_WARN("[new-cpp-script] rejected: no current project");
			return;
		}
		m_NewCppScriptOpen = true;
		m_NewCppScriptOpenedFrame = static_cast<uint32_t>(ctx.Frame());
		m_NewCppScriptName = "MyComponent";
		m_NewCppScriptFailure.clear();
		m_NewCppScriptFailureFor.clear();
		ctx.SetModal(Wui::HashId("modal.newscript"));
		ctx.SetFocus(Wui::HashId("script.new.name"));
		ctx.RecordOp("script", "new-cpp-ask", m_NewCppScriptName, "src/Components");
	}

	bool EditorShell::CreateNewCppScript(Wui::WuiContext& ctx)
	{
		const std::string name = CppScriptBaseName(m_NewCppScriptName);
		const std::string nameError = NewCppScriptNameError();
		if (!nameError.empty())
		{
			// 模态里已经画过行内错误;这条分支只是拒绝"绕过按钮的第二次调用"。
			m_NewCppScriptFailure = nameError;
			m_NewCppScriptFailureFor = NewCppScriptTargetPath().string();
			return false;
		}

		const std::filesystem::path target = NewCppScriptTargetPath();
		const std::filesystem::path parent = target.parent_path();
		std::error_code folderError;
		if (!std::filesystem::is_directory(parent, folderError))
			std::filesystem::create_directories(parent, folderError);

		std::string error;
		bool wrote = false;
		if (folderError)
		{
			error = Wui::Tr("modal.newscript.folder_failed", "Could not create the folder: ")
				+ parent.string();
		}
		else
		{
			// 临时文件 + 同目录改名(与脚本编辑器保存/新建着色器同一套写法);失败不留半成品。
			const std::filesystem::path temporary = parent / (target.filename().string() + ".tmp-write");
			std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
			if (!out.is_open())
			{
				error = Wui::Tr("modal.newscript.write_failed", "Could not write the file: ")
					+ temporary.string();
			}
			else
			{
				out << NewCppScriptTemplateSource(name);
				out.close();
				std::error_code renameError;
				std::filesystem::rename(temporary, target, renameError);
				if (renameError)
				{
					std::error_code cleanupError;
					std::filesystem::remove(temporary, cleanupError);
					error = Wui::Tr("modal.newscript.write_failed", "Could not write the file: ")
						+ target.string() + " (" + renameError.message() + ")";
				}
				else
				{
					wrote = true;
				}
			}
		}
		if (!wrote)
		{
			m_NewCppScriptFailure = error;
			m_NewCppScriptFailureFor = target.string();
			WLD_CORE_WARN("[new-cpp-script] write failed: {0}", error);
			return false;
		}

		// manifest 是**双向类型账本**(schema-compiler 拒绝"已声明但未登记"的新类型):
		// 创建入口把模板声明的三个类型(组件 / <Name>Data / <Name>Mode)全部登记,
		// 用户重建 Game 时生成器即可直接通过。手写新组件的作者仍需自己补这些行
		// (见 docs/user/scripting/README.md)。
		//
		// PROJ-8/T1:账本位置口径不变(组件源码的 `../Generated/Game.manifest`),组件落到项目层后
		// 它自动变成 `<项目根>/src/Generated/Game.manifest`(与 T2 的子项目构建故事一致)。
		// 新增一条**存在性守卫**:账本还不存在(项目从未构建过)时**不新建** —— 凭空写一份
		// 只有三条目的账本会把项目的构建输入改坏;由项目首次构建产出账本,之后再创建组件即可登记。
		const std::filesystem::path manifest =
			target.parent_path().parent_path() / "Generated" / "Game.manifest";
		std::error_code manifestFileError;
		if (!std::filesystem::is_regular_file(manifest, manifestFileError))
		{
			WLD_CORE_WARN("[new-cpp-script] type ledger not found ({0}); "
				"skipping registration of {1}, {1}Data, {1}Mode", manifest.string(), name);
		}
		else
		{
			std::string manifestText;
			{
				std::ifstream in(manifest, std::ios::binary);
				std::string line;
				while (std::getline(in, line))
				{
					if (!line.empty() && line.back() == '\r')
						line.pop_back();
					manifestText += line;
					manifestText.push_back('\n');
				}
			}
			// 模板声明的**每个**类型都要登记(struct/enum 双向账本);漏一个重建 Game 就会被拒。
			const std::vector<std::string> entries = {
				"struct World::" + name,
				"struct World::" + name + "Data",
				"enum World::" + name + "Mode",
			};
			bool manifestChanged = false;
			for (const std::string& entry : entries)
			{
				if (manifestText.find(entry + "\n") != std::string::npos)
					continue;
				manifestText += entry;
				manifestText.push_back('\n');
				manifestChanged = true;
			}
			if (manifestChanged)
			{
				const std::filesystem::path temporary = manifest.string() + ".tmp-write";
				std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
				if (!out.is_open())
				{
					WLD_CORE_WARN("[new-cpp-script] could not update manifest '{0}'", manifest.string());
				}
				else
				{
					out << manifestText;
					out.close();
					std::error_code renameError;
					std::filesystem::rename(temporary, manifest, renameError);
					if (renameError)
					{
						std::error_code cleanupError;
						std::filesystem::remove(temporary, cleanupError);
						WLD_CORE_WARN("[new-cpp-script] could not update manifest '{0}': {1}",
							manifest.string(), renameError.message());
					}
				}
			}
		}

		// 落点/提示口径 = 相对**项目根**(项目源码视图的同一口径)。
		const std::string relative = "src/Components/" + name + ".h";
		// 操作日志:与内容浏览器的新建资产(browser.new-shader)同一条口径。
		ctx.RecordOp("script", "new-cpp", name, relative);
		WLD_CORE_INFO("[new-cpp-script] created '{0}'", target.string());
		// CPPT-7/PROJ-8:C++ 源码一律外部 Visual Studio —— 走既有的"帧边界延迟打开"安全路径
		// (与内容浏览器双击脚本相同的安全点;EditorShell::OpenScriptEditorNow 按扩展名分流),
		// 内置脚本编辑器只服务 Lua/Luau。
		OpenScriptEditor(target.generic_string());
		// 状态栏提示:编辑器不内置编译器 —— 显式告诉用户"用 VS 构建这个项目,再重载 C++ 模块"。
		PushNotice(Wui::TrFormat("notice.newscript.created",
			"Created {path} — build this project with Visual Studio (output under {project}/build), "
			"then use File ▶ Build & Reload C++ Module.",
			{ { "path", relative }, { "project", CurrentProjectRoot().generic_string() } }));
		return true;
	}

	void EditorShell::DrawNewCppScriptModal(Wui::WuiContext& ctx)
	{
		const Wui::WuiId modalId = Wui::HashId("modal.newscript");
		if (m_NewCppScriptOpen)
			ctx.SetModal(modalId);
		else if (ctx.Modal() == modalId)
			ctx.ClearModal();
		if (!m_NewCppScriptOpen)
			return;

		Wui::WuiRect frame;
		bool escapePressed = false;
		Wui::ModalFrameDesc frameDesc;
		frameDesc.Id = modalId;
		frameDesc.Title = Wui::Tr("modal.newscript.title", "New C++ Component");
		frameDesc.Size = { 560.0f, 236.0f };
		if (!Wui::BeginModalFrame(ctx, frameDesc, &frame, &escapePressed, m_Theme))
		{
			// 模态被别的路径接管/收口:同步清掉宿主状态,避免状态与真实模态脱节。
			m_NewCppScriptOpen = false;
			return;
		}

		const float labelX = frame.X + 16.0f;
		const float fieldX = frame.X + 130.0f;
		const float suffixW = 30.0f;
		const float fieldW = frame.W - 146.0f - suffixW - 16.0f;
		const Wui::WuiId nameId = Wui::HashId("script.new.name");
		const Wui::WuiId okId = Wui::HashId("script.new.ok");
		const Wui::WuiId cancelId = Wui::HashId("script.new.cancel");
		const bool justOpened = ctx.Frame() == m_NewCppScriptOpenedFrame;

		// ---- 名称(标识符校验 + 重名拒绝都走同一条行内错误)----
		float cursorY = frame.Y + 46.0f;
		const std::string nameLabel = Wui::Tr("modal.newscript.name", "Name");
		Wui::Label(ctx, { labelX, cursorY + 5.0f }, nameLabel, m_Theme.TextMuted, 13.0f);
		const Wui::WuiRect nameRect { fieldX, cursorY, fieldW, 24.0f };
		// 写盘失败原因只对"同一个落点"有效:名字一改就作废(与新建着色器向导同口径)。
		if (!m_NewCppScriptFailure.empty() && m_NewCppScriptFailureFor != NewCppScriptTargetPath().string())
		{
			m_NewCppScriptFailure.clear();
			m_NewCppScriptFailureFor.clear();
		}
		const std::string nameError = NewCppScriptNameError();
		const std::string inlineError = nameError.empty() ? m_NewCppScriptFailure : nameError;
		Wui::TextFieldA11y nameA11y;
		nameA11y.Label = nameLabel;
		nameA11y.Placeholder = Wui::Tr("modal.newscript.name.placeholder", "Component name (valid C++ identifier)");
		// Enter 提交判定必须在**控件绘制前**取焦点:TextFieldCore 在回车那一帧会 `SetFocus(0)`
		// (提交即交出焦点),画完再读 ctx.Focus() 已经不是本字段(实测:回车点了不建文件)。
		const bool nameFocused = ctx.Focus() == nameId;
		// TextFieldEx:错误就地画在输入框下方(描边 Danger),同时把 error 追加进无障碍节点 value。
		const bool submitted = Wui::TextFieldEx(ctx, nameId, nameRect, m_NewCppScriptName, m_Theme,
			inlineError, &nameA11y);
		Wui::Label(ctx, { nameRect.X + nameRect.W + 8.0f, cursorY + 6.0f }, ".h", m_Theme.TextMuted, 13.0f);
		cursorY += 46.0f;

		// ---- 实时落点回显(相对当前项目根;绝对路径进节点 Tooltip)----
		const std::string relative = "src/Components/" + CppScriptBaseName(m_NewCppScriptName) + ".h";
		const std::string targetLabel = Wui::Tr("modal.newscript.target", "Will create");
		Wui::Label(ctx, { labelX, cursorY + 3.0f }, targetLabel, m_Theme.TextMuted, 12.0f);
		Wui::Label(ctx, { fieldX, cursorY + 1.0f }, relative, m_Theme.Text, 13.0f);
		{
			Wui::WuiAccessNode node;
			node.Id = Wui::HashId("script.new.target");
			node.Window = Wui::WuiAccessibility::Get().CurrentWindow();
			node.Panel = "shell";
			node.Kind = "text";
			node.Label = targetLabel;
			node.Value = relative;
			node.Tooltip = NewCppScriptTargetPath().generic_string();
			node.Rect = { fieldX, cursorY - 3.0f, fieldW + suffixW, 20.0f };
			node.Enabled = true;
			node.Interactive = false;
			node.Visible = true;
			Wui::WuiAccessibility::Get().Register(node);
		}
		cursorY += 24.0f;

		// ---- 生效步骤提示(用 VS 构建项目 → 重载 C++ 模块;编辑器不内置编译器)----
		Wui::Label(ctx, { labelX, cursorY + 2.0f },
			Wui::Tr("modal.newscript.hint",
				"Build this project with Visual Studio (output under <project>/build), "
				"then File ▶ Build & Reload C++ Module."),
			m_Theme.TextMuted, 12.0f);

		// ---- 底部按钮(名称不合法/重名时创建按钮禁用并带原因)----
		const bool canCreate = nameError.empty();
		const Wui::ModalResult footerResult = Wui::ModalFooter(ctx, frame,
			Wui::Tr("modal.newscript.ok", "Create"),
			Wui::Tr("modal.newscript.cancel", "Cancel"),
			okId, cancelId, canCreate, m_Theme);

		bool closeRequested = false;
		bool created = false;
		if ((footerResult == Wui::ModalResult::Confirm || (submitted && nameFocused && !justOpened))
			&& canCreate)
		{
			created = CreateNewCppScript(ctx);
			closeRequested = created;
		}
		else if (footerResult == Wui::ModalResult::Cancel || escapePressed)
		{
			ctx.RecordOp("script", "new-cpp-cancel", CppScriptBaseName(m_NewCppScriptName), relative);
			closeRequested = true;
		}

		if (closeRequested)
		{
			m_NewCppScriptOpen = false;
			m_NewCppScriptFailure.clear();
			m_NewCppScriptFailureFor.clear();
			ctx.ClearModal();
		}
		Wui::EndModalFrame(ctx);
		// 创建成功:操作日志/提示/打开编辑器都在 CreateNewCppScript 里完成(单一出口)。
		(void)created;
	}

	// ---- PROJ-1/T1:File ▸ New Project…(任意位置新建标准项目)----
	//
	// 需求(用户 2026-09-28):在任意位置生成一个**标准、干净**的项目骨架(不含示例)。
	// 本模态只做三件事:收集 项目名 + 位置;实时回显落点与行内错误;把"浏览…/创建"
	// 标记成下一帧开头的任务(原生对话框与落盘都不在渲染中途做)。
	// 生成内核 = Editor::ProjectScaffolder(清单走引擎 writer、Main.wd 走引擎序列化、
	// 结构来自所选模板 templates/project-<id>/**);成功后模态换成两个动作:资源管理器打开 / 打开项目。
	// PROJ-2/T1 追加:可勾选"最小可运行场景"(默认勾选),成功态第三个动作 = 启动项目
	// (独立 Runtime 进程,不重启编辑器)。

	std::string EditorShell::NewProjectTrimmedName() const
	{
		return TrimProjectNameText(m_NewProjectName);
	}

	std::string EditorShell::NewProjectNameError() const
	{
		return Editor::ProjectScaffolder::ValidateProjectName(m_NewProjectName);
	}

	std::string EditorShell::NewProjectLocationError() const
	{
		return Editor::ProjectScaffolder::ValidateLocation(
			std::filesystem::u8path(m_NewProjectLocation), m_NewProjectName);
	}

	std::filesystem::path EditorShell::NewProjectTargetRoot() const
	{
		if (m_NewProjectLocation.empty())
			return {};
		const std::string name = NewProjectTrimmedName();
		if (name.empty())
			return {};
		// 输入框与原生对话框都是 UTF-8 文本 → 按 UTF-8 解释成路径(非 ASCII 位置不会乱码)。
		return (std::filesystem::u8path(m_NewProjectLocation) / std::filesystem::u8path(name)).lexically_normal();
	}

	// PROJ-7/T2:模板库刷新(模板 = 磁盘事实 templates/project-*/template.json)。打开模态时强制
	// 刷一次;模态开着时按 2s 节流 —— 目录扫描不进每帧路径。选中项按 id 保持;被删/首次打开时
	// 优先回退到 empty,再退第一个模板。
	void EditorShell::RefreshNewProjectTemplates(bool force)
	{
		const double now = std::chrono::duration<double>(
			std::chrono::steady_clock::now().time_since_epoch()).count();
		if (!force && m_NewProjectTemplatesScannedAt > 0.0 && now - m_NewProjectTemplatesScannedAt < 2.0)
			return;
		m_NewProjectTemplatesScannedAt = now;
		m_NewProjectTemplates = Editor::ProjectScaffolder::ListTemplates();
		const auto has = [this](const std::string& id)
		{
			return std::any_of(m_NewProjectTemplates.begin(), m_NewProjectTemplates.end(),
				[&id](const Editor::ProjectScaffolder::TemplateInfo& info) { return info.Id == id; });
		};
		if (!has(m_NewProjectTemplateId))
		{
			m_NewProjectTemplateId.clear();
			if (has("empty"))
				m_NewProjectTemplateId = "empty";
			else if (!m_NewProjectTemplates.empty())
				m_NewProjectTemplateId = m_NewProjectTemplates.front().Id;
			// 换了模板:上一次的落盘失败原因不再对应当前选择(与"名称/落点一改就作废"同口径)。
			m_NewProjectFailure.clear();
			m_NewProjectFailureFor.clear();
		}
	}

	const Editor::ProjectScaffolder::TemplateInfo* EditorShell::SelectedNewProjectTemplate() const
	{
		for (const Editor::ProjectScaffolder::TemplateInfo& info : m_NewProjectTemplates)
			if (info.Id == m_NewProjectTemplateId)
				return &info;
		return nullptr;
	}

	void EditorShell::OpenNewProjectModal(Wui::WuiContext& ctx, const std::string& preselectTemplateId)
	{
		// PROJ-17(用户 2026-09-29:"位置那里,删除按一下就把路径全删了"):文本框的编辑状态
		// (光标/选区)按控件 id 长期持久化 —— 上一次会话里若整段被选中(拖选或 Ctrl+A),
		// 下次打开对话框时选区仍在,第一次 Delete/Backspace 就会把**整段路径**一次删掉。
		// 每次打开对话框都清掉这两个输入框的持久状态:新状态 = 光标在末尾、无选区。
		ctx.ErasePersist(Wui::HashId("project.new.name"));
		ctx.ErasePersist(Wui::HashId("project.new.location"));
		m_NewProjectOpen = true;
		m_NewProjectOpenedFrame = static_cast<uint32_t>(ctx.Frame());
		m_NewProjectCreated = false;
		m_NewProjectCreatedStarterScene = true;
		m_NewProjectCreatedTemplateName.clear();
		m_NewProjectCreatedStartScene.clear();
		m_NewProjectEntryPoints.clear();
		m_NewProjectRoot.clear();
		m_NewProjectCreatedName.clear();
		m_NewProjectFailure.clear();
		m_NewProjectFailureFor.clear();
		m_NewProjectBrowsePending = false;
		m_NewProjectCreatePending = false;
		// PROJ-7/T3b:调用方可以预选模板(启动器"新建示例项目…" ⇒ example)。必须写在强制刷新
		// **之前** —— RefreshNewProjectTemplates 只在"选中 id 不在模板库里"时才回退,所以预选值
		// 存在就保留;模板缺失时自动降级到 empty/第一个模板(不额外报错)。
		if (!preselectTemplateId.empty())
			m_NewProjectTemplateId = preselectTemplateId;
		RefreshNewProjectTemplates(true);   // 模态打开这一帧就把模板列出来(磁盘事实)
		if (m_NewProjectName.empty())
			m_NewProjectName = "MyProject";
		if (m_NewProjectLocation.empty())
			m_NewProjectLocation = Editor::ProjectScaffolder::DefaultProjectLocation().u8string();
		ctx.SetModal(Wui::HashId("modal.newproject"));
		ctx.SetFocus(Wui::HashId("project.new.name"));
		ctx.RecordOp("project", "new-ask", NewProjectTrimmedName(), m_NewProjectLocation);
	}

	void EditorShell::RunNewProjectBrowse(Wui::WuiContext& ctx)
	{
		// 原生"选择文件夹"对话框:只在帧边界打开(取消返回空串 → 保持原值)。
		const std::string folder = FileDialogs::SelectFolder("选择项目位置");
		if (folder.empty())
			return;
		m_NewProjectLocation = folder;
		m_NewProjectFailure.clear();
		m_NewProjectFailureFor.clear();
		ctx.RecordOp("project", "browse", NewProjectTrimmedName(), folder);
	}

	void EditorShell::RunNewProjectCreate(Wui::WuiContext& ctx)
	{
		RefreshNewProjectTemplates(false);
		const Editor::ProjectScaffolder::TemplateInfo* selected = SelectedNewProjectTemplate();
		// 所选模板自带启动场景(声明 defaultScene)时不生成最小可运行场景:向导里那个勾选框
		// 此时是禁用态(见 DrawNewProjectModal),这里再夹一次,防止"绕过按钮的第二次调用"。
		const bool templateOwnsStartScene = selected != nullptr && !selected->DefaultScene.empty();
		const std::string nameError = NewProjectNameError();
		const std::string locationError = NewProjectLocationError();
		const std::string templateError = Editor::ProjectScaffolder::ValidateTemplate(m_NewProjectTemplateId);
		const std::string blocked = !nameError.empty() ? nameError
			: (!locationError.empty() ? locationError : templateError);
		if (!blocked.empty())
		{
			// 模态里已经画过行内错误;这条分支只是拒绝"绕过按钮的第二次调用"。
			m_NewProjectFailure = blocked;
			m_NewProjectFailureFor = NewProjectTargetRoot().u8string();
			return;
		}

		const std::string name = NewProjectTrimmedName();
		const Editor::ProjectScaffolder::Result result = Editor::ProjectScaffolder::Create(
			std::filesystem::u8path(m_NewProjectLocation), name, Application::Get().GetContext(),
			templateOwnsStartScene ? false : m_NewProjectStarterScene, m_NewProjectTemplateId);
		if (!result.Ok)
		{
			m_NewProjectFailure = result.Error;
			m_NewProjectFailureFor = NewProjectTargetRoot().u8string();
			WLD_CORE_WARN("[new-project] create failed: {0}", result.Error);
			return;
		}

		m_NewProjectCreated = true;
		m_NewProjectCreatedStarterScene = !templateOwnsStartScene && m_NewProjectStarterScene;
		m_NewProjectCreatedTemplateName = selected != nullptr ? selected->Name : m_NewProjectTemplateId;
		m_NewProjectCreatedStartScene = templateOwnsStartScene ? selected->DefaultScene : std::string();
		m_NewProjectEntryPoints = result.EntryPoints;
		m_NewProjectRoot = result.ProjectRoot;
		m_NewProjectCreatedName = name;
		m_NewProjectFailure.clear();
		m_NewProjectFailureFor.clear();
		// 操作日志与状态栏提示:与其它"新建资产"入口同一口径(project / new)。
		// 细节里带上模板 id:验证者/脚本从 ops.tail 就能确认"用了哪个模板"。
		ctx.RecordOp("project", "new", name,
			result.ProjectRoot.u8string() + " [template " + m_NewProjectTemplateId + "]");
		PushNotice(Wui::TrFormat("notice.newproject.created",
			"Created project {path} (template: {template})",
			{ { "path", result.ProjectRoot.u8string() }, { "template", m_NewProjectCreatedTemplateName } }));
		WLD_CORE_INFO("[new-project] created '{0}' ({1} files)", result.ProjectRoot.u8string(),
			result.Files.size());
	}

	void EditorShell::DrawNewProjectModal(Wui::WuiContext& ctx)
	{
		const Wui::WuiId modalId = Wui::HashId("modal.newproject");
		if (m_NewProjectOpen)
			ctx.SetModal(modalId);
		else if (ctx.Modal() == modalId)
			ctx.ClearModal();
		if (!m_NewProjectOpen)
			return;

		Wui::WuiRect frame;
		bool escapePressed = false;
		Wui::ModalFrameDesc frameDesc;
		frameDesc.Id = modalId;
		frameDesc.Title = Wui::Tr("modal.newproject.title", "New Project");
		// PROJ-7/T2:模板选择 + 说明/提示各占一行 ⇒ 比 PROJ-2 的 320 高 100px。
		frameDesc.Size = { 620.0f, 420.0f };
		if (!Wui::BeginModalFrame(ctx, frameDesc, &frame, &escapePressed, m_Theme))
		{
			// 模态被别的路径接管/收口:同步清掉宿主状态,避免状态与真实模态脱节。
			m_NewProjectOpen = false;
			return;
		}

		const float labelX = frame.X + 16.0f;
		const float fieldX = frame.X + 130.0f;
		const float fieldW = frame.W - 146.0f - 16.0f;

			// ---- 成功态:已生成 + 三个动作按钮(资源管理器打开 / 打开项目 / 启动项目)----
		if (m_NewProjectCreated)
		{
			float cursorY = frame.Y + 48.0f;
			Wui::Label(ctx, { labelX, cursorY }, Wui::TrFormat("modal.newproject.created",
				"Created the project (template: {template}):",
				{ { "template", m_NewProjectCreatedTemplateName } }), m_Theme.TextMuted, 12.0f);
			cursorY += 20.0f;
			Wui::Label(ctx, { labelX, cursorY }, m_NewProjectRoot.u8string(), m_Theme.Text, 13.0f);
			cursorY += 24.0f;
			// 成功态也把"实际用的模板 / 清单 start_scene"登记进无障碍树:验证者按 id 读,
			// 不用从提示文案里猜(ops.tail 里的 project/new 也带模板 id)。
			{
				Wui::WuiAccessNode templateNode;
				templateNode.Id = Wui::HashId("project.new.created.template");
				templateNode.Window = Wui::WuiAccessibility::Get().CurrentWindow();
				templateNode.Panel = "shell";
				templateNode.Kind = "text";
				templateNode.Label = Wui::Tr("modal.newproject.template", "Template");
				templateNode.Value = m_NewProjectCreatedTemplateName;
				templateNode.Rect = { labelX, frame.Y + 46.0f, frame.W - (labelX - frame.X) - 16.0f, 18.0f };
				templateNode.Enabled = true;
				templateNode.Interactive = false;
				templateNode.Visible = true;
				Wui::WuiAccessibility::Get().Register(templateNode);

				Wui::WuiAccessNode sceneNode;
				sceneNode.Id = Wui::HashId("project.new.created.scene");
				sceneNode.Window = Wui::WuiAccessibility::Get().CurrentWindow();
				sceneNode.Panel = "shell";
				sceneNode.Kind = "text";
				sceneNode.Label = Wui::Tr("modal.newproject.created.scene_label", "Start scene");
				sceneNode.Value = m_NewProjectCreatedStartScene.empty()
					? std::string("scenes/Main.wd") : m_NewProjectCreatedStartScene;
				sceneNode.Rect = { labelX, frame.Y + 66.0f, frame.W - (labelX - frame.X) - 16.0f, 18.0f };
				sceneNode.Enabled = true;
				sceneNode.Interactive = false;
				sceneNode.Visible = true;
				Wui::WuiAccessibility::Get().Register(sceneNode);
			}
			if (!m_NewProjectCreatedStartScene.empty())
			{
				// PROJ-7/T2:模板自带启动场景(example ⇒ scenes/3DTest.wd)—— 没有生成 Main.wd。
				Wui::Label(ctx, { labelX, cursorY },
					Wui::TrFormat("modal.newproject.created.template_scene",
						"Start scene: {scene} (ships with the template).",
						{ { "scene", m_NewProjectCreatedStartScene } }),
					m_Theme.TextMuted, 12.0f);
			}
			else if (m_NewProjectCreatedStarterScene)
			{
				Wui::Label(ctx, { labelX, cursorY },
					Wui::Tr("modal.newproject.created.starter",
						"assets/scenes/Main.wd has a minimal scene (camera + directional light) — "
						"Launch Project shows a picture right away."),
					m_Theme.TextMuted, 12.0f);
			}
			else
			{
				Wui::Label(ctx, { labelX, cursorY },
					Wui::Tr("modal.newproject.created.empty_scene",
						"The scene is empty — launching the project shows a blank picture. "
						"Tick the starter-scene option next time, or add content in the editor."),
					m_Theme.Danger, 12.0f);
			}
			cursorY += 22.0f;
			// PROJ-3/T1:启动器模式下没有"留在这里继续工作"这一选项 —— 默认动作是打开新项目。
			Wui::Label(ctx, { labelX, cursorY },
				m_LauncherMode
					? Wui::Tr("modal.newproject.created.hint.launcher",
						"In launcher mode: Open Project (the default action) restarts the editor into "
						"the new project; Launch Project runs it in a separate Runtime process.")
					: Wui::Tr("modal.newproject.created.hint",
						"Launch Project runs it in a separate Runtime process; Open Project restarts the "
						"editor into it; or keep working here."),
				m_Theme.TextMuted, 12.0f);
			cursorY += 22.0f;
			// PROJ-3/T1(P2b):项目根里的启动入口(exe 优先,缺失则 .cmd 兜底)。
			if (!m_NewProjectEntryPoints.empty())
			{
				std::string entryList;
				for (const std::string& entry : m_NewProjectEntryPoints)
				{
					if (!entryList.empty())
						entryList += ", ";
					entryList += entry;
				}
				Wui::Label(ctx, { labelX, cursorY },
					Wui::TrFormat("modal.newproject.created.entrypoints",
						"Project root: {files}; build with build.cmd, other entry files live in .we/ "
						"(engine root record + .cmd fallback), and Visual Studio lists one launch item "
						"(ProjectRun.exe — opens this project in the editor, from .vs/launch.vs.json).",
						{ { "files", entryList } }),
					m_Theme.TextMuted, 12.0f);
			}

			// PROJ-3/T1:启动器模式下"打开项目"是默认(最左)动作 —— 启动器进程要做的是把
			// 用户送进新项目;普通编辑器形态保持 PROJ-2 的按钮顺序。
			// None = 本帧没有任何按钮被按下(ModalButtons 返回 -1)—— 不能当成 Close,
			// 否则成功态会在画出来的同一帧把自己关掉。
			enum class CreateAction { None, Explorer, OpenProject, LaunchProject, Close };
			CreateAction actionOrder[4];
			Wui::ModalButtonDesc buttons[4];
			if (m_LauncherMode)
			{
				actionOrder[0] = CreateAction::OpenProject;
				buttons[0] = { Wui::Tr("modal.newproject.open_project", "Open Project"),
					Wui::HashId("project.new.open"), true };
				actionOrder[1] = CreateAction::LaunchProject;
				buttons[1] = { Wui::Tr("modal.newproject.launch", "Launch Project"),
					Wui::HashId("project.new.launch"), true };
				actionOrder[2] = CreateAction::Explorer;
				buttons[2] = { Wui::Tr("modal.newproject.open_explorer", "Open in Explorer"),
					Wui::HashId("project.new.explorer"), true };
				actionOrder[3] = CreateAction::Close;
				buttons[3] = { Wui::Tr("modal.newproject.close", "Close"),
					Wui::HashId("project.new.close"), true };
			}
			else
			{
				actionOrder[0] = CreateAction::Explorer;
				buttons[0] = { Wui::Tr("modal.newproject.open_explorer", "Open in Explorer"),
					Wui::HashId("project.new.explorer"), true };
				actionOrder[1] = CreateAction::OpenProject;
				buttons[1] = { Wui::Tr("modal.newproject.open_project", "Open Project"),
					Wui::HashId("project.new.open"), true };
				// PROJ-2/T1:第三个动作按钮 —— 独立 Runtime 进程直接跑刚建的项目(不重启编辑器)。
				actionOrder[2] = CreateAction::LaunchProject;
				buttons[2] = { Wui::Tr("modal.newproject.launch", "Launch Project"),
					Wui::HashId("project.new.launch"), true };
				actionOrder[3] = CreateAction::Close;
				buttons[3] = { Wui::Tr("modal.newproject.close", "Close"),
					Wui::HashId("project.new.close"), true };
			}
			const int clicked = Wui::ModalButtons(ctx, frame, buttons, 4, m_Theme);
			const CreateAction action = clicked >= 0 && clicked < 4 ? actionOrder[clicked] : CreateAction::None;
			if (action == CreateAction::Explorer)
			{
				// ShellExecuteW 打开项目目录(与"打开外部脚本"同一条系统关联路径)。
				const HINSTANCE shellResult = ShellExecuteW(nullptr, L"open",
					m_NewProjectRoot.wstring().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
				if (reinterpret_cast<INT_PTR>(shellResult) <= 32)
				{
					PushNotice(Wui::TrFormat("notice.newproject.explorer_failed",
						"Could not open the project folder: {path}",
						{ { "path", m_NewProjectRoot.u8string() } }));
					WLD_CORE_WARN("[new-project] ShellExecuteW failed ({0}) for '{1}'",
						static_cast<long long>(reinterpret_cast<INT_PTR>(shellResult)),
						m_NewProjectRoot.u8string());
				}
				else
				{
					ctx.RecordOp("project", "open-explorer", m_NewProjectCreatedName,
						m_NewProjectRoot.u8string());
				}
			}
			else if (action == CreateAction::OpenProject)
			{
				ctx.RecordOp("project", "open", m_NewProjectCreatedName, m_NewProjectRoot.u8string());
				// 重启编辑器到新项目(有未保存改动时先走既有的未保存确认模态)。
				m_Editor.RelaunchWithProject(m_NewProjectRoot);
				m_NewProjectOpen = false;
				ctx.ClearModal();
			}
			else if (action == CreateAction::LaunchProject)
			{
				// 一键启动:独立 Runtime 进程(定位/命令行/日志/操作记录都在 EditorLayer)。
				m_Editor.LaunchProjectRuntime(m_NewProjectRoot);
			}
			else if (action == CreateAction::Close || escapePressed)
			{
				m_NewProjectOpen = false;
				ctx.ClearModal();
			}
			Wui::EndModalFrame(ctx);
			return;
		}

		const Wui::WuiId nameId = Wui::HashId("project.new.name");
		const Wui::WuiId locationId = Wui::HashId("project.new.location");
		const Wui::WuiId browseId = Wui::HashId("project.new.browse");
		const Wui::WuiId okId = Wui::HashId("project.new.ok");
		const Wui::WuiId cancelId = Wui::HashId("project.new.cancel");
		const bool justOpened = ctx.Frame() == m_NewProjectOpenedFrame;
		// PROJ-7/T2:模板库按 2s 节流刷新(打开模态那一帧已经在 OpenNewProjectModal 里强制刷过)。
		RefreshNewProjectTemplates(false);
		const Editor::ProjectScaffolder::TemplateInfo* selectedTemplate = SelectedNewProjectTemplate();
		// 模板自带启动场景(声明 defaultScene)⇒ 那个"最小可运行场景"勾选框不可用(灰显 + 理由)。
		const bool templateOwnsStartScene = selectedTemplate != nullptr && !selectedTemplate->DefaultScene.empty();

		// 写盘失败原因只对"同一个落点"有效:名称/位置一改就作废(与新建脚本向导同口径)。
		if (!m_NewProjectFailure.empty() && m_NewProjectFailureFor != NewProjectTargetRoot().u8string())
		{
			m_NewProjectFailure.clear();
			m_NewProjectFailureFor.clear();
		}
		const std::string nameError = NewProjectNameError();
		const std::string locationError = NewProjectLocationError();
		const std::string templateError = Editor::ProjectScaffolder::ValidateTemplate(m_NewProjectTemplateId);

		// ---- 名称(= 目录名;非法/保留名就地报错)----
		float cursorY = frame.Y + 46.0f;
		const std::string nameLabel = Wui::Tr("modal.newproject.name", "Name");
		Wui::Label(ctx, { labelX, cursorY + 5.0f }, nameLabel, m_Theme.TextMuted, 13.0f);
		const Wui::WuiRect nameRect { fieldX, cursorY, fieldW, 24.0f };
		Wui::TextFieldA11y nameA11y;
		nameA11y.Label = nameLabel;
		nameA11y.Placeholder = Wui::Tr("modal.newproject.name.placeholder", "Project name (folder name)");
		const bool nameFocused = ctx.Focus() == nameId;
		const bool nameSubmitted = Wui::TextFieldEx(ctx, nameId, nameRect, m_NewProjectName, m_Theme,
			nameError, &nameA11y);
		cursorY += 46.0f;

		// ---- 位置 + Browse…(原生文件夹对话框;帧边界执行)----
		const float browseW = 84.0f;
		const float locationW = fieldW - browseW - 8.0f;
		const std::string locationLabel = Wui::Tr("modal.newproject.location", "Location");
		Wui::Label(ctx, { labelX, cursorY + 5.0f }, locationLabel, m_Theme.TextMuted, 13.0f);
		const Wui::WuiRect locationRect { fieldX, cursorY, locationW, 24.0f };
		Wui::TextFieldA11y locationA11y;
		locationA11y.Label = locationLabel;
		locationA11y.Placeholder = Wui::Tr("modal.newproject.location.placeholder",
			"Folder that will contain the project");
		const bool locationFocused = ctx.Focus() == locationId;
		const bool locationSubmitted = Wui::TextFieldEx(ctx, locationId, locationRect, m_NewProjectLocation,
			m_Theme, locationError, &locationA11y);
		const Wui::WuiRect browseRect { fieldX + locationW + 8.0f, cursorY, browseW, 24.0f };
		const bool browseClicked = Wui::Button(ctx, browseId, browseRect,
			Wui::Tr("modal.newproject.browse", "Browse…"), m_Theme);
		if (browseClicked)
			m_NewProjectBrowsePending = true;   // 下一帧开头弹原生对话框
		cursorY += 46.0f;

		// ---- 实时落点(<位置>/<名称>;绝对路径进节点 Tooltip)----
		const std::filesystem::path target = NewProjectTargetRoot();
		const std::string targetText = target.empty() ? std::string("—") : target.u8string();
		const std::string targetLabel = Wui::Tr("modal.newproject.target", "Will create");
		Wui::Label(ctx, { labelX, cursorY + 3.0f }, targetLabel, m_Theme.TextMuted, 12.0f);
		Wui::Label(ctx, { fieldX, cursorY + 1.0f }, targetText,
			target.empty() ? m_Theme.TextMuted : m_Theme.Text, 13.0f);
		{
			Wui::WuiAccessNode node;
			node.Id = Wui::HashId("project.new.target");
			node.Window = Wui::WuiAccessibility::Get().CurrentWindow();
			node.Panel = "shell";
			node.Kind = "text";
			node.Label = targetLabel;
			node.Value = targetText;
			node.Tooltip = target.u8string();
			node.Rect = { fieldX, cursorY - 3.0f, fieldW, 20.0f };
			node.Enabled = true;
			node.Interactive = false;
			node.Visible = true;
			Wui::WuiAccessibility::Get().Register(node);
		}
		cursorY += 24.0f;

		// ---- PROJ-7/T2:模板选择(分段按钮;每项一个稳定 a11y id project.new.template.<id>)----
		// 模板库 = templates/project-*/template.json(引擎自带);name 画在按钮上,description
		// 是悬停提示 + 选中行说明。坏模板(缺 template.json / 缺必需条目)也列出来 —— 选中它
		// 在下面给可读的行内错误,而不是从列表里静默消失。
		{
			const std::string templateLabel = Wui::Tr("modal.newproject.template", "Template");
			Wui::Label(ctx, { labelX, cursorY + 5.0f }, templateLabel, m_Theme.TextMuted, 13.0f);
			{
				Wui::WuiAccessNode node;
				node.Id = Wui::HashId("project.new.template");
				node.Window = Wui::WuiAccessibility::Get().CurrentWindow();
				node.Panel = "shell";
				node.Kind = "group";
				node.Label = templateLabel;
				node.Value = m_NewProjectTemplateId;   // 稳定 id(脚本按它断言,不按显示名)
				node.Tooltip = selectedTemplate != nullptr ? selectedTemplate->Description : std::string();
				node.Rect = { fieldX, cursorY - 3.0f, fieldW, 26.0f };
				node.Enabled = true;
				node.Interactive = false;
				node.Visible = true;
				Wui::WuiAccessibility::Get().Register(node);
			}
			float segmentX = fieldX;
			constexpr float segmentW = 168.0f;
			constexpr float segmentGap = 8.0f;
			for (const Editor::ProjectScaffolder::TemplateInfo& info : m_NewProjectTemplates)
			{
				// 放不下就换行(模板数量由磁盘决定,不假设只有两个)。
				if (segmentX > fieldX && segmentX + segmentW > fieldX + fieldW + 0.5f)
				{
					segmentX = fieldX;
					cursorY += 30.0f;
				}
				const std::string segmentLabel = info.Valid ? info.Name
					: Wui::TrFormat("modal.newproject.template.unavailable", "{name} (unavailable)",
						{ { "name", info.Name } });
				const Wui::WuiId segmentId = Wui::HashId(("project.new.template." + info.Id).c_str());
				if (Wui::ButtonEx(ctx, segmentId, { segmentX, cursorY, segmentW, 24.0f }, segmentLabel,
					m_Theme, true, info.Id == m_NewProjectTemplateId,
					info.Valid ? info.Description : info.Error))
				{
					m_NewProjectTemplateId = info.Id;
					m_NewProjectFailure.clear();
					m_NewProjectFailureFor.clear();
				}
				segmentX += segmentW + segmentGap;
			}
			cursorY += 30.0f;
			if (selectedTemplate != nullptr && !selectedTemplate->Description.empty())
			{
				Wui::Label(ctx, { labelX, cursorY }, selectedTemplate->Description, m_Theme.TextMuted, 12.0f);
				Wui::WuiAccessNode node;
				node.Id = Wui::HashId("project.new.template.description");
				node.Window = Wui::WuiAccessibility::Get().CurrentWindow();
				node.Panel = "shell";
				node.Kind = "text";
				node.Label = templateLabel;
				node.Value = selectedTemplate->Description;
				node.Tooltip = selectedTemplate->Id;
				node.Rect = { labelX, cursorY - 2.0f, frame.W - (labelX - frame.X) - 16.0f, 18.0f };
				node.Enabled = true;
				node.Interactive = false;
				node.Visible = true;
				Wui::WuiAccessibility::Get().Register(node);
			}
			cursorY += 20.0f;
			// 带示例内容的模板(assets/scripts/examples/)给一行"示例内容随模板走"的提示。
			if (selectedTemplate != nullptr && selectedTemplate->HasSamples)
			{
				const std::string hint = Wui::Tr("modal.newproject.template.samples",
					"Brings in the sample scenes, materials and scripts (sample content ships with the template).");
				Wui::Label(ctx, { labelX, cursorY }, hint, m_Theme.Warning, 12.0f);
				Wui::WuiAccessNode node;
				node.Id = Wui::HashId("project.new.template.hint");
				node.Window = Wui::WuiAccessibility::Get().CurrentWindow();
				node.Panel = "shell";
				node.Kind = "text";
				node.Label = templateLabel;
				node.Value = hint;
				node.Tooltip = selectedTemplate->Id;
				node.Rect = { labelX, cursorY - 2.0f, frame.W - (labelX - frame.X) - 16.0f, 18.0f };
				node.Enabled = true;
				node.Interactive = false;
				node.Visible = true;
				Wui::WuiAccessibility::Get().Register(node);
				cursorY += 20.0f;
			}
		}

		// ---- PROJ-2/T1 + PROJ-7/T2:最小可运行场景(相机 + 方向光;默认勾选)----
		// 勾选 = assets/scenes/Main.wd 由引擎序列化写入一台 Camera3D + 一盏方向光(不含示例);
		// 不勾 = 旧口径的空场景,成功态会给"启动后是空画面"的提示。
		// 所选模板自带启动场景(example ⇒ scenes/3DTest.wd)时该勾选框**禁用**(灰显 + 理由):
		// 清单的 start_scene 指向模板自带场景,不再生成 Main.wd —— 勾选没有意义。
		{
			const Wui::WuiId starterId = Wui::HashId("project.new.starter_scene");
			const Wui::WuiRect starterRect { labelX, cursorY, frame.W - (labelX - frame.X) - 16.0f, 22.0f };
			const std::string starterLabel = Wui::Tr("modal.newproject.starter",
				"Include a minimal runnable scene (camera + directional light)");
			if (templateOwnsStartScene)
			{
				const std::string lockedReason = Wui::TrFormat("modal.newproject.starter.locked",
					"The template ships its own start scene ({scene}); no minimal scene is generated.",
					{ { "scene", selectedTemplate->DefaultScene } });
				// 禁用态 = 与 Wui::Checkbox 同形状,但灰显且不响应点击(Engine 的 Checkbox 没有
				// 禁用参数,Engine/** 不在本任务白名单内 ⇒ 这里按 ButtonEx 的禁用口径自绘)。
				const Wui::WuiRect box { starterRect.X, starterRect.Y + (starterRect.H - 16.0f) * 0.5f, 16.0f, 16.0f };
				ctx.Commands().push_back({ Wui::WuiDrawKind::Rect, box, m_Theme.PanelBg, 3.0f });
				ctx.Commands().push_back({ Wui::WuiDrawKind::RectOutline, box, m_Theme.Border, 3.0f, 1.0f });
				ctx.Commands().push_back({ Wui::WuiDrawKind::Text,
					{ starterRect.X + 24.0f, starterRect.Y + (starterRect.H - 15.0f) * 0.5f, 0, 0 },
					m_Theme.TextDisabled, 0, 1.0f, starterLabel, 15.0f, false });
				Wui::WuiAccessNode node;
				node.Id = starterId;
				node.Window = Wui::WuiAccessibility::Get().CurrentWindow();
				node.Panel = "shell";
				node.Kind = "checkbox";
				node.Label = starterLabel;
				node.Value = lockedReason;   // 灰件不能没有理由(理由同时进悬停提示)
				node.Tooltip = lockedReason;
				node.Rect = starterRect;
				node.Enabled = false;
				node.Interactive = true;
				node.Visible = true;
				Wui::WuiAccessibility::Get().Register(node);
				Wui::Tooltip(ctx, starterRect, lockedReason);
			}
			else
			{
				Wui::Checkbox(ctx, starterId, starterRect, starterLabel, m_NewProjectStarterScene, m_Theme);
				Wui::Tooltip(ctx, starterRect,
					Wui::Tr("modal.newproject.starter.tooltip",
						"Writes assets/scenes/Main.wd with a Camera3D at [0, 1, 5] and a directional "
						"light, so Launch Project renders a picture. No sample assets, materials or "
						"scripts are copied."));
			}
		}
		cursorY += 30.0f;

		// ---- 模板/落盘的通用错误行(名称与位置各自画在自己的输入框下)----
		const std::string generalError = !templateError.empty() ? templateError : m_NewProjectFailure;
		if (!generalError.empty())
		{
			Wui::Label(ctx, { labelX, cursorY + 2.0f }, generalError, m_Theme.Danger, 12.0f);
			// 行内错误也进无障碍树(标签本身不登记节点):自动化/验证者用 project.new.error
			// 读"为什么建不了",不必截图猜文字。
			Wui::WuiAccessNode errorNode;
			errorNode.Id = Wui::HashId("project.new.error");
			errorNode.Window = Wui::WuiAccessibility::Get().CurrentWindow();
			errorNode.Panel = "shell";
			errorNode.Kind = "text";
			errorNode.Label = Wui::Tr("modal.newproject.error.label", "Cannot create");
			errorNode.Value = generalError;
			errorNode.Tooltip = generalError;
			errorNode.Rect = { labelX, cursorY, frame.W - (labelX - frame.X) - 16.0f, 18.0f };
			errorNode.Enabled = true;
			errorNode.Interactive = false;
			errorNode.Visible = true;
			Wui::WuiAccessibility::Get().Register(errorNode);
		}
		else
			Wui::Label(ctx, { labelX, cursorY + 2.0f },
				Wui::Tr("modal.newproject.hint",
					"Content comes from the selected template (templates/project-<id>/**, copied file by "
					"file); the manifest is written by the engine writer; engine C++ stays in Engine/**, "
					"project C++ goes to src/**."),
				m_Theme.TextMuted, 12.0f);

		// ---- 底部按钮(名称/位置/模板任一不通过时创建按钮禁用并带原因)----
		const bool canCreate = nameError.empty() && locationError.empty() && templateError.empty();
		const Wui::ModalResult footerResult = Wui::ModalFooter(ctx, frame,
			Wui::Tr("modal.newproject.ok", "Create"),
			Wui::Tr("modal.newproject.cancel", "Cancel"),
			okId, cancelId, canCreate, m_Theme);

		bool closeRequested = false;
		if ((footerResult == Wui::ModalResult::Confirm
				|| ((nameSubmitted && nameFocused) || (locationSubmitted && locationFocused)))
			&& !justOpened && canCreate)
		{
			// 落盘推迟到下一帧开头(与 Browse… 同一条帧边界纪律)。
			m_NewProjectCreatePending = true;
		}
		else if (footerResult == Wui::ModalResult::Cancel || escapePressed)
		{
			ctx.RecordOp("project", "new-cancel", NewProjectTrimmedName(), m_NewProjectLocation);
			closeRequested = true;
		}

		if (closeRequested)
		{
			m_NewProjectOpen = false;
			m_NewProjectFailure.clear();
			m_NewProjectFailureFor.clear();
			ctx.ClearModal();
		}
		Wui::EndModalFrame(ctx);
	}

	// ---- PLUG-AUTH-1:File ▸ New Plugin…(窗口级模态;成功态换成动作按钮)----
	//
	// 入口:File ▸ New Plugin…(menu.file.new_plugin)、插件管理器面板的「新建插件…」
	// (plugins.action.new)、启动器页的「新建插件…」(project.launcher.new_plugin)——
	// 三条路都进同一个 m_NewPlugin* 状态机,内核 = PluginScaffolder。
	//
	// 目标:引擎插件 = <repo>/plugins(启动器形态固定这一项,Project 分段禁用并给理由);
	// 项目插件 = <项目根>/plugins。模板 = templates/plugin-*(6 类);requires != none 的模板
	// 只生成"可编译骨架 + TODO",向导里显式提示需要哪个注册面。
	//
	// 落盘在**帧边界**执行(m_NewPluginCreatePending 由 OnRender 开头消费,与 New Project 同款)。

	std::string EditorShell::NewPluginTrimmedName() const
	{
		return TrimProjectNameText(m_NewPluginName);
	}

	std::string EditorShell::NewPluginTrimmedId() const
	{
		return TrimProjectNameText(m_NewPluginId);
	}

	std::filesystem::path EditorShell::NewPluginPluginsRoot() const
	{
		if (m_NewPluginTarget == Editor::PluginScaffolder::PluginTarget::Project)
		{
			const std::filesystem::path projectRoot = World::Paths::ProjectDir();
			if (projectRoot.empty())
				return {};
			return projectRoot / "plugins";
		}
		return std::filesystem::path(WLD_REPO_ROOT) / "plugins";
	}

	std::filesystem::path EditorShell::NewPluginTargetRoot() const
	{
		const std::filesystem::path root = NewPluginPluginsRoot();
		const std::string name = NewPluginTrimmedName();
		if (root.empty() || name.empty())
			return {};
		return (root / std::filesystem::u8path(name)).lexically_normal();
	}

	std::string EditorShell::NewPluginNameError() const
	{
		return Editor::PluginScaffolder::ValidatePluginName(m_NewPluginName);
	}

	std::string EditorShell::NewPluginIdError() const
	{
		return Editor::PluginScaffolder::ValidatePluginId(m_NewPluginId);
	}

	std::string EditorShell::NewPluginTemplateError() const
	{
		return Editor::PluginScaffolder::ValidateTemplate(m_NewPluginTemplateId, m_NewPluginTarget);
	}

	std::string EditorShell::NewPluginTargetError() const
	{
		if (m_NewPluginTarget == Editor::PluginScaffolder::PluginTarget::Project
			&& (m_LauncherMode || World::Paths::ProjectDir().empty()))
		{
			return Wui::Tr("modal.newplugin.error.no_project",
				"Open a project first — project plugins live in <project>/plugins.");
		}
		const std::filesystem::path root = NewPluginPluginsRoot();
		if (root.empty())
			return Wui::Tr("modal.newplugin.error.root_missing",
				"Cannot resolve the plugins directory for this target");
		// 名称/ID 的错误画在各自输入框下,这里不重复报(与 New Project 的模板错误同款)。
		if (!NewPluginNameError().empty() || !NewPluginIdError().empty())
			return {};
		return Editor::PluginScaffolder::ValidateTarget(root, m_NewPluginName, m_NewPluginId);
	}

	void EditorShell::EnsureNewPluginTemplateSelection()
	{
		// 只调整"当前选中的模板",**不碰** m_NewPluginTemplates —— 调用它的路径可能发生在
		// 一帧的中途(目标分段按钮的点击),而同一帧后面还持有指向该向量的指针
		// (PLUG-AUTH-1 实测:帧中途重建向量会让早先抓的 selectedTemplate 悬垂,
		//  读 Description 直接 std::length_error "string too long")。
		const auto allowed = [this](const Editor::PluginScaffolder::PluginTemplateInfo& info)
		{
			return m_NewPluginTarget == Editor::PluginScaffolder::PluginTarget::Engine
				? info.EngineOk : info.ProjectOk;
		};
		const auto contains = [this](const std::string& id)
		{
			for (const Editor::PluginScaffolder::PluginTemplateInfo& info : m_NewPluginTemplates)
				if (info.Id == id)
					return true;   // 坏模板也列出来(选中给行内错误),不静默消失
			return false;
		};
		if (contains(m_NewPluginTemplateId))
			return;
		m_NewPluginTemplateId.clear();
		for (const Editor::PluginScaffolder::PluginTemplateInfo& info : m_NewPluginTemplates)
		{
			if (info.Id == "empty" && allowed(info))
			{
				m_NewPluginTemplateId = info.Id;
				break;
			}
		}
		if (m_NewPluginTemplateId.empty())
		{
			for (const Editor::PluginScaffolder::PluginTemplateInfo& info : m_NewPluginTemplates)
			{
				if (allowed(info))
				{
					m_NewPluginTemplateId = info.Id;
					break;
				}
			}
		}
		if (m_NewPluginTemplateId.empty() && !m_NewPluginTemplates.empty())
			m_NewPluginTemplateId = m_NewPluginTemplates.front().Id;
		if (m_NewPluginTemplateId.empty())
			m_NewPluginTemplateId = "empty";
		m_NewPluginFailure.clear();
		m_NewPluginFailureFor.clear();
	}

	void EditorShell::RefreshNewPluginTemplates(bool force)
	{
		const double now = ShellNowSeconds();
		if (!force && m_NewPluginTemplatesScannedAt > 0.0
			&& now - m_NewPluginTemplatesScannedAt < 2.0)
			return;
		m_NewPluginTemplatesScannedAt = now;
		m_NewPluginTemplates = Editor::PluginScaffolder::ListTemplates();
		EnsureNewPluginTemplateSelection();
	}

	const Editor::PluginScaffolder::PluginTemplateInfo* EditorShell::SelectedNewPluginTemplate() const
	{
		for (const Editor::PluginScaffolder::PluginTemplateInfo& info : m_NewPluginTemplates)
			if (info.Id == m_NewPluginTemplateId)
				return &info;
		return nullptr;
	}

	void EditorShell::RunNewPluginCreate(Wui::WuiContext& ctx)
	{
		RefreshNewPluginTemplates(false);
		const Editor::PluginScaffolder::PluginTemplateInfo* selected = SelectedNewPluginTemplate();
		const std::string nameError = NewPluginNameError();
		const std::string idError = NewPluginIdError();
		const std::string templateError = NewPluginTemplateError();
		const std::string targetError = NewPluginTargetError();
		// Create 按钮在任一校验不过时是禁用态;这里再夹一次,防止"绕过按钮的第二次调用"。
		if (selected == nullptr || !nameError.empty() || !idError.empty()
			|| !templateError.empty() || !targetError.empty())
		{
			const std::string blocked = !templateError.empty() ? templateError
				: (!targetError.empty() ? targetError
					: (!nameError.empty() ? nameError : idError));
			m_NewPluginFailure = blocked.empty()
				? Wui::Tr("modal.newplugin.error.no_template",
					"No plugin template is available under <checkout>/templates/plugin-*/.")
				: blocked;
			m_NewPluginFailureFor = NewPluginTargetRoot().u8string();
			return;
		}

		Editor::PluginScaffolder::PluginScaffoldRequest request;
		request.Target = m_NewPluginTarget;
		request.TemplateId = m_NewPluginTemplateId;
		request.Name = NewPluginTrimmedName();
		request.PluginId = NewPluginTrimmedId();

		const Editor::PluginScaffolder::PluginScaffoldResult result =
			Editor::PluginScaffolder::Scaffold(NewPluginPluginsRoot(), request);
		if (!result.Ok)
		{
			m_NewPluginFailure = result.Error;
			m_NewPluginFailureFor = NewPluginTargetRoot().u8string();
			WLD_CORE_WARN("[new-plugin] scaffold failed: {0}", result.Error);
			return;
		}

		m_NewPluginCreated = true;
		m_NewPluginRoot = result.PluginRoot;
		m_NewPluginFiles = result.Files;
		m_NewPluginCreatedId = request.PluginId;
		m_NewPluginCreatedTarget = Editor::PluginScaffolder::TargetName(request.Target);
		m_NewPluginFailure.clear();
		m_NewPluginFailureFor.clear();
		ctx.RecordOp("plugin", "new", request.PluginId,
			result.PluginRoot.u8string() + " [template " + m_NewPluginTemplateId + " target "
				+ m_NewPluginCreatedTarget + " files " + std::to_string(result.Files.size()) + "]");
		PushNotice(Wui::TrFormat("notice.newplugin.created", "Created the plugin at {path}",
			{ { "path", result.PluginRoot.u8string() } }));
		WLD_CORE_INFO("[new-plugin] created '{0}' (template '{1}', target {2}) at '{3}': {4} files",
			request.PluginId, m_NewPluginTemplateId, m_NewPluginCreatedTarget,
			result.PluginRoot.u8string(), result.Files.size());
	}

	void EditorShell::OpenNewPluginModal(Wui::WuiContext& ctx)
	{
		m_NewPluginOpen = true;
		m_NewPluginOpenedFrame = static_cast<uint32_t>(ctx.Frame());
		m_NewPluginCreated = false;
		m_NewPluginRoot.clear();
		m_NewPluginFiles.clear();
		m_NewPluginCreatedId.clear();
		m_NewPluginCreatedTarget.clear();
		m_NewPluginFailure.clear();
		m_NewPluginFailureFor.clear();
		m_NewPluginCreatePending = false;

		// 目标默认:能建项目插件时 = 项目插件(编辑器/面板入口的常见场景);
		// 启动器形态(没有项目)= 引擎插件 —— Project 分段禁用并给理由(D1=允许引擎插件)。
		const bool projectAvailable = !m_LauncherMode && !World::Paths::ProjectDir().empty();
		m_NewPluginTarget = projectAvailable
			? Editor::PluginScaffolder::PluginTarget::Project
			: Editor::PluginScaffolder::PluginTarget::Engine;
		RefreshNewPluginTemplates(true);   // 打开这一帧就把模板列出来(磁盘事实)
		if (m_NewPluginName.empty())
			m_NewPluginName = "MyPlugin";
		if (m_NewPluginId.empty() || !m_NewPluginIdTouched)
			m_NewPluginId = DefaultPluginIdForName(NewPluginTrimmedName());
		ctx.SetModal(Wui::HashId("modal.newplugin"));
		ctx.RecordOp("plugin", "new-ask", NewPluginTrimmedName(),
			Editor::PluginScaffolder::TargetName(m_NewPluginTarget));
	}

	void EditorShell::DrawNewPluginModal(Wui::WuiContext& ctx)
	{
		const Wui::WuiId modalId = Wui::HashId("modal.newplugin");
		if (m_NewPluginOpen)
			ctx.SetModal(modalId);
		else if (ctx.Modal() == modalId)
			ctx.ClearModal();
		if (!m_NewPluginOpen)
			return;

		Wui::WuiRect frame;
		bool escapePressed = false;
		Wui::ModalFrameDesc frameDesc;
		frameDesc.Id = modalId;
		frameDesc.Title = Wui::Tr("modal.newplugin.title", "New Plugin");
		frameDesc.Size = { 680.0f, 470.0f };
		if (!Wui::BeginModalFrame(ctx, frameDesc, &frame, &escapePressed, m_Theme))
		{
			// 模态被别的路径接管/收口:同步清掉宿主状态,避免状态与真实模态脱节。
			m_NewPluginOpen = false;
			return;
		}

		const float labelX = frame.X + 16.0f;
		const float fieldX = frame.X + 130.0f;
		const float fieldW = frame.W - 146.0f - 16.0f;

		// ---- 成功态:落盘目录 + 文件清单 + 「在内容浏览器中定位 / 打开插件目录 / 关闭」----
		if (m_NewPluginCreated)
		{
			const Editor::PluginScaffolder::PluginTemplateInfo* templateInfo = SelectedNewPluginTemplate();
			const std::string templateName = templateInfo != nullptr ? templateInfo->Name
				: m_NewPluginTemplateId;
			float cursorY = frame.Y + 48.0f;
			Wui::Label(ctx, { labelX, cursorY }, Wui::TrFormat("modal.newplugin.created",
				"Created the plugin (template: {template}, target: {target}):",
				{ { "template", templateName }, { "target", m_NewPluginCreatedTarget } }),
				m_Theme.TextMuted, 12.0f);
			cursorY += 20.0f;
			Wui::Label(ctx, { labelX, cursorY }, m_NewPluginRoot.u8string(), m_Theme.Text, 13.0f);
			cursorY += 24.0f;
			{
				// plugin.new.done:验证者按 id 读"落盘目录"与 id/target/文件数,不用从提示文案里猜。
				std::string files;
				const size_t shown = std::min<size_t>(m_NewPluginFiles.size(), 8);
				for (size_t index = 0; index < shown; ++index)
				{
					if (index > 0)
						files += ", ";
					files += m_NewPluginFiles[index];
				}
				if (m_NewPluginFiles.size() > shown)
					files += ", …";
				RegisterShellAccessNode(Wui::HashId("plugin.new.done"), "text",
					Wui::Tr("modal.newplugin.created.label", "Created plugin"),
					"path=" + m_NewPluginRoot.u8string() + " id=" + m_NewPluginCreatedId
						+ " target=" + m_NewPluginCreatedTarget + " files="
						+ std::to_string(m_NewPluginFiles.size()),
					{ labelX, frame.Y + 46.0f, frame.W - (labelX - frame.X) - 16.0f, 40.0f },
					false, true, files);
			}
			if (!m_NewPluginFiles.empty())
			{
				std::string files;
				const size_t shown = std::min<size_t>(m_NewPluginFiles.size(), 8);
				for (size_t index = 0; index < shown; ++index)
				{
					if (index > 0)
						files += ", ";
					files += m_NewPluginFiles[index];
				}
				if (m_NewPluginFiles.size() > shown)
					files += ", …";
				Wui::Label(ctx, { labelX, cursorY }, Wui::Tr("modal.newplugin.created.files", "Files: ")
						+ files, m_Theme.TextMuted, 12.0f);
				cursorY += 20.0f;
			}
			Wui::Label(ctx, { labelX, cursorY }, Wui::Tr("modal.newplugin.created.hint",
				"Engine plugins: re-run the engine CMake configure and build ALL_BUILD, then restart "
				"the editor. Project plugins: build the project (build.cmd) — its plugin DLL lands in "
				"<project>/build/x64-<config>/bin/<config>/plugins/<config>/."),
				m_Theme.TextMuted, 12.0f);
			cursorY += 20.0f;
			// 引擎插件不在内容浏览器的第三根里(那儿只显示 <项目根>/plugins)⇒ locate 禁用,
			// 并把理由画成可读文字 + 只读 a11y 节点(禁用按钮本身没有 tooltip 槽位)。
			const bool canLocate = m_NewPluginCreatedTarget == std::string("project")
				&& !World::Paths::ProjectDir().empty();
			if (!canLocate)
			{
				const std::string reason = Wui::Tr("modal.newplugin.locate.reason",
					"Locate is for project plugins: engine plugins live in <engine>/plugins, and the "
					"content browser only shows <project>/plugins.");
				Wui::Label(ctx, { labelX, cursorY }, reason, m_Theme.TextMuted, 12.0f);
				RegisterShellAccessNode(Wui::HashId("plugin.new.locate.reason"), "text",
					Wui::Tr("modal.newplugin.locate", "Locate in Content Browser"), reason,
					{ labelX, cursorY - 2.0f, frame.W - (labelX - frame.X) - 16.0f, 18.0f },
					false, true, reason);
			}

			const Wui::ModalButtonDesc buttons[3] = {
				{ Wui::Tr("modal.newplugin.locate", "Locate in Content Browser"),
					Wui::HashId("plugin.new.locate"), canLocate },
				{ Wui::Tr("modal.newplugin.open", "Open Plugin Folder"),
					Wui::HashId("plugin.new.open"), true },
				{ Wui::Tr("modal.newplugin.close", "Close"),
					Wui::HashId("plugin.new.close"), true },
			};
			const int clicked = Wui::ModalButtons(ctx, frame, buttons, 3, m_Theme);
			if (clicked == 0 && canLocate)
			{
				std::string message;
				if (RevealPathInContentBrowserPanel(ContentBrowserPanel::RootScope::ProjectPlugins,
						m_NewPluginRoot, &message))
					ctx.RecordOp("plugin", "new-locate", m_NewPluginCreatedId,
						m_NewPluginRoot.generic_string());
				else
					PushNotice(message);
			}
			else if (clicked == 1)
			{
				const HINSTANCE shellResult = ShellExecuteW(nullptr, L"open",
					m_NewPluginRoot.wstring().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
				if (reinterpret_cast<INT_PTR>(shellResult) <= 32)
				{
					PushNotice(Wui::TrFormat("notice.newplugin.explorer_failed",
						"Could not open the plugin folder: {path}",
						{ { "path", m_NewPluginRoot.u8string() } }));
					WLD_CORE_WARN("[new-plugin] ShellExecuteW failed ({0}) for '{1}'",
						static_cast<long long>(reinterpret_cast<INT_PTR>(shellResult)),
						m_NewPluginRoot.u8string());
				}
				else
				{
					ctx.RecordOp("plugin", "new-open-explorer", m_NewPluginCreatedId,
						m_NewPluginRoot.u8string());
				}
			}
			else if (clicked == 2 || escapePressed)
			{
				m_NewPluginOpen = false;
				ctx.ClearModal();
			}
			Wui::EndModalFrame(ctx);
			return;
		}

		const Wui::WuiId nameId = Wui::HashId("plugin.new.name");
		const Wui::WuiId idId = Wui::HashId("plugin.new.id");
		const Wui::WuiId okId = Wui::HashId("plugin.new.create");
		const Wui::WuiId cancelId = Wui::HashId("plugin.new.cancel");
		const bool justOpened = ctx.Frame() == m_NewPluginOpenedFrame;
		RefreshNewPluginTemplates(false);
		const bool projectAvailable = !m_LauncherMode && !World::Paths::ProjectDir().empty();

		// 写盘失败原因只对"同一个落点"有效:名称/ID/目标一改就作废(与 New Project 同口径)。
		if (!m_NewPluginFailure.empty()
			&& m_NewPluginFailureFor != NewPluginTargetRoot().u8string())
		{
			m_NewPluginFailure.clear();
			m_NewPluginFailureFor.clear();
		}
		const std::string nameError = NewPluginNameError();
		const std::string idError = NewPluginIdError();
		const std::string templateError = NewPluginTemplateError();
		const std::string targetError = NewPluginTargetError();

		float cursorY = frame.Y + 46.0f;

		// ---- 目标(引擎 / 项目;启动器形态 Project 禁用并给理由)----
		{
			const std::string targetLabel = Wui::Tr("modal.newplugin.target", "Target");
			Wui::Label(ctx, { labelX, cursorY + 5.0f }, targetLabel, m_Theme.TextMuted, 13.0f);
			RegisterShellAccessNode(Wui::HashId("plugin.new.target"), "group", targetLabel,
				Editor::PluginScaffolder::TargetName(m_NewPluginTarget),
				{ fieldX, cursorY - 3.0f, fieldW, 26.0f }, false, true, std::string());
			const float segmentW = 168.0f;
			const float segmentGap = 8.0f;
			const bool engineActive =
				m_NewPluginTarget == Editor::PluginScaffolder::PluginTarget::Engine;
			const std::filesystem::path engineRoot =
				std::filesystem::path(WLD_REPO_ROOT) / "plugins";
			if (Wui::ButtonEx(ctx, Wui::HashId("plugin.new.scope.engine"),
					{ fieldX, cursorY, segmentW, 24.0f },
					Wui::Tr("modal.newplugin.scope.engine", "Engine Plugin"), m_Theme, true,
					engineActive, engineRoot.u8string()))
			{
				if (!engineActive)
				{
					m_NewPluginTarget = Editor::PluginScaffolder::PluginTarget::Engine;
					m_NewPluginFailure.clear();
					m_NewPluginFailureFor.clear();
					// 帧中途只重选模板,不重建模板向量(见 EnsureNewPluginTemplateSelection 注释)。
					EnsureNewPluginTemplateSelection();
					if (!m_NewPluginIdTouched)
						m_NewPluginId = DefaultPluginIdForName(NewPluginTrimmedName());
				}
			}
			const std::filesystem::path projectPluginsRoot =
				World::Paths::ProjectDir().empty() ? std::filesystem::path()
					: World::Paths::ProjectDir() / "plugins";
			const std::string projectReason = projectAvailable ? projectPluginsRoot.u8string()
				: Wui::Tr("modal.newplugin.scope.project.locked",
					"Open or create a project first — project plugins live in <project>/plugins.");
			const bool projectActive =
				m_NewPluginTarget == Editor::PluginScaffolder::PluginTarget::Project;
			if (Wui::ButtonEx(ctx, Wui::HashId("plugin.new.scope.project"),
					{ fieldX + segmentW + segmentGap, cursorY, segmentW, 24.0f },
					Wui::Tr("modal.newplugin.scope.project", "Project Plugin"), m_Theme,
					projectAvailable, projectActive, projectReason)
				&& projectAvailable && !projectActive)
			{
				m_NewPluginTarget = Editor::PluginScaffolder::PluginTarget::Project;
				m_NewPluginFailure.clear();
				m_NewPluginFailureFor.clear();
				EnsureNewPluginTemplateSelection();
				if (!m_NewPluginIdTouched)
					m_NewPluginId = DefaultPluginIdForName(NewPluginTrimmedName());
			}
			cursorY += 34.0f;
		}

		// 目标分段按钮的点击可能刚刚改过 m_NewPluginTarget(并且只重选了模板 id,不动向量):
		// 这里**重新取一次**选中模板指针 —— 指针必须晚于任何可能改动 m_NewPluginTemplates 的调用。
		const Editor::PluginScaffolder::PluginTemplateInfo* selectedTemplate = SelectedNewPluginTemplate();

		// ---- 模板(分段按钮;每项一个稳定 a11y id plugin.new.template.<id>)----
		{
			const std::string templateLabel = Wui::Tr("modal.newplugin.template", "Template");
			Wui::Label(ctx, { labelX, cursorY + 5.0f }, templateLabel, m_Theme.TextMuted, 13.0f);
			RegisterShellAccessNode(Wui::HashId("plugin.new.template"), "group", templateLabel,
				m_NewPluginTemplateId,
				{ fieldX, cursorY - 3.0f, fieldW, 26.0f }, false, true,
				selectedTemplate != nullptr ? selectedTemplate->Description : std::string());
			float segmentX = fieldX;
			const float segmentW = 168.0f;
			const float segmentGap = 8.0f;
			float rowY = cursorY;
			for (const Editor::PluginScaffolder::PluginTemplateInfo& info : m_NewPluginTemplates)
			{
				if (segmentX > fieldX && segmentX + segmentW > fieldX + fieldW + 0.5f)
				{
					segmentX = fieldX;
					rowY += 30.0f;
				}
				const std::string surface = PluginRequiresSurface(info.Requires);
				std::string tooltip = info.Valid ? info.Description : info.Error;
				if (info.Valid && !surface.empty() && info.Requires != "none")
				{
					tooltip += "\n" + Wui::TrFormat("modal.newplugin.template.requires",
						"Needs the {surface} registration surface (the generated skeleton compiles today).",
						{ { "surface", surface } });
				}
				const std::string segmentLabel = info.Valid ? info.Name
					: Wui::TrFormat("modal.newplugin.template.unavailable", "{name} (unavailable)",
						{ { "name", info.Name } });
				if (Wui::ButtonEx(ctx,
						Wui::HashId(("plugin.new.template." + info.Id).c_str()),
						{ segmentX, rowY, segmentW, 24.0f }, segmentLabel, m_Theme, true,
						info.Id == m_NewPluginTemplateId, tooltip))
				{
					m_NewPluginTemplateId = info.Id;
					m_NewPluginFailure.clear();
					m_NewPluginFailureFor.clear();
				}
				segmentX += segmentW + segmentGap;
			}
			cursorY = rowY + 30.0f;
			if (selectedTemplate != nullptr && !selectedTemplate->Description.empty())
			{
				Wui::Label(ctx, { labelX, cursorY }, selectedTemplate->Description,
					m_Theme.TextMuted, 12.0f);
				RegisterShellAccessNode(Wui::HashId("plugin.new.template.description"), "text",
					templateLabel, selectedTemplate->Description,
					{ labelX, cursorY - 2.0f, frame.W - (labelX - frame.X) - 16.0f, 18.0f },
					false, true, selectedTemplate->Id);
			}
			cursorY += 20.0f;
			if (selectedTemplate != nullptr && selectedTemplate->Valid
				&& selectedTemplate->Requires != "none")
			{
				const std::string requiresText = Wui::TrFormat("modal.newplugin.template.requires",
					"Needs the {surface} registration surface (the generated skeleton compiles today).",
					{ { "surface", PluginRequiresSurface(selectedTemplate->Requires) } });
				Wui::Label(ctx, { labelX, cursorY }, requiresText, m_Theme.Warning, 12.0f);
				RegisterShellAccessNode(Wui::HashId("plugin.new.template.requires"), "text",
					templateLabel, requiresText,
					{ labelX, cursorY - 2.0f, frame.W - (labelX - frame.X) - 16.0f, 18.0f },
					false, true, selectedTemplate->Requires);
			cursorY += 20.0f;
			}
		}

		// ---- 名称(= 目录名 + 默认显示名;非法就地报错)----
		{
			const std::string nameLabel = Wui::Tr("modal.newplugin.name", "Name");
			Wui::Label(ctx, { labelX, cursorY + 5.0f }, nameLabel, m_Theme.TextMuted, 13.0f);
			const Wui::WuiRect nameRect { fieldX, cursorY, fieldW, 24.0f };
			Wui::TextFieldA11y nameA11y;
			nameA11y.Label = nameLabel;
			nameA11y.Placeholder = Wui::Tr("modal.newplugin.name.placeholder",
				"Plugin name (folder name; letters, digits, '-' and '_')");
			const bool nameFocused = ctx.Focus() == nameId;
			const std::string nameBefore = m_NewPluginName;
			const bool nameSubmitted = Wui::TextFieldEx(ctx, nameId, nameRect, m_NewPluginName,
				m_Theme, nameError, &nameA11y);
			// 名称变化时同步默认 ID(用户手动改过 ID 之后不再改写)。
			if (m_NewPluginName != nameBefore && !m_NewPluginIdTouched)
				m_NewPluginId = DefaultPluginIdForName(NewPluginTrimmedName());
			cursorY += 46.0f;
			if (!nameError.empty())
				Wui::Label(ctx, { fieldX, cursorY - 20.0f }, nameError, m_Theme.Danger, 12.0f);

			// ---- 插件 ID(反域名;唯一性在 Create 前由 ValidateTarget 查)----
			const std::string idLabel = Wui::Tr("modal.newplugin.id", "Plugin ID");
			Wui::Label(ctx, { labelX, cursorY + 5.0f }, idLabel, m_Theme.TextMuted, 13.0f);
			const Wui::WuiRect idRect { fieldX, cursorY, fieldW, 24.0f };
			Wui::TextFieldA11y idA11y;
			idA11y.Label = idLabel;
			idA11y.Placeholder = Wui::Tr("modal.newplugin.id.placeholder",
				"Reverse-domain ID (e.g. com.studio.my-plugin)");
			const bool idFocused = ctx.Focus() == idId;
			const std::string idBefore = m_NewPluginId;
			const bool idSubmitted = Wui::TextFieldEx(ctx, idId, idRect, m_NewPluginId,
				m_Theme, idError, &idA11y);
			if (m_NewPluginId != idBefore)
				m_NewPluginIdTouched = true;
			cursorY += 46.0f;
			if (!idError.empty())
				Wui::Label(ctx, { fieldX, cursorY - 20.0f }, idError, m_Theme.Danger, 12.0f);

			// ---- 实时落点(只读;绝对路径进节点 Tooltip)----
			const std::filesystem::path target = NewPluginTargetRoot();
			const std::string targetText = target.empty() ? std::string("—") : target.u8string();
			const std::string locationLabel = Wui::Tr("modal.newplugin.location", "Will create");
			Wui::Label(ctx, { labelX, cursorY + 3.0f }, locationLabel, m_Theme.TextMuted, 12.0f);
			Wui::Label(ctx, { fieldX, cursorY + 1.0f }, targetText,
				target.empty() ? m_Theme.TextMuted : m_Theme.Text, 13.0f);
			RegisterShellAccessNode(Wui::HashId("plugin.new.location"), "text", locationLabel,
				targetText, { fieldX, cursorY - 3.0f, fieldW, 20.0f }, false, true,
				target.u8string());
			cursorY += 24.0f;

			// ---- 模板/落点的通用错误行 + 提示 ----
			const std::string generalError = !templateError.empty() ? templateError
				: (!targetError.empty() ? targetError : m_NewPluginFailure);
			if (!generalError.empty())
			{
				Wui::Label(ctx, { labelX, cursorY + 2.0f }, generalError, m_Theme.Danger, 12.0f);
				RegisterShellAccessNode(Wui::HashId("plugin.new.error"), "text",
					Wui::Tr("modal.newplugin.error.label", "Cannot create"), generalError,
					{ labelX, cursorY, frame.W - (labelX - frame.X) - 16.0f, 18.0f },
					false, true, generalError);
			}
			else
			{
				Wui::Label(ctx, { labelX, cursorY + 2.0f }, Wui::Tr("modal.newplugin.hint",
					"The generated plugin.we.yaml is validated by the plugin loader before anything "
					"lands; an existing directory is never overwritten. The plugin DLL is produced by "
					"the engine or project build, not by this wizard."), m_Theme.TextMuted, 12.0f);
			}

			// ---- 底部按钮(名称/ID/模板/落点任一不过时 Create 禁用并带原因)----
			const bool canCreate = selectedTemplate != nullptr && nameError.empty()
				&& idError.empty() && templateError.empty() && targetError.empty();
			const Wui::ModalResult footerResult = Wui::ModalFooter(ctx, frame,
				Wui::Tr("modal.newplugin.ok", "Create"),
				Wui::Tr("modal.newplugin.cancel", "Cancel"),
				okId, cancelId, canCreate, m_Theme);

			bool closeRequested = false;
			if ((footerResult == Wui::ModalResult::Confirm
					|| ((nameSubmitted && nameFocused) || (idSubmitted && idFocused)))
				&& !justOpened && canCreate)
			{
				// 落盘推迟到下一帧开头(与 New Project 同一条帧边界纪律)。
				m_NewPluginCreatePending = true;
			}
			else if (footerResult == Wui::ModalResult::Cancel || escapePressed)
			{
				ctx.RecordOp("plugin", "new-cancel", NewPluginTrimmedName(),
					Editor::PluginScaffolder::TargetName(m_NewPluginTarget));
				closeRequested = true;
			}
			if (closeRequested)
			{
				m_NewPluginOpen = false;
				m_NewPluginFailure.clear();
				m_NewPluginFailureFor.clear();
				ctx.ClearModal();
			}
		}
		Wui::EndModalFrame(ctx);
	}

	// ---- PROJ-2/T1:项目启动器 / File ▸ Open Project… / 运行 ▸ 启动项目(Runtime) ----
	//
	// 启动决策(显式项目 / 自动打开最近一次 / 显示启动器)在 EditorLayer;本文件只做:
	//   * 启动器模态的渲染与四个动作(新建项目向导 / 新建示例项目向导(预选 example 模板) /
	//     打开项目… / 关闭·退出);
	//   * 最近列表的缓存刷新与"移除失效项";
	//   * File ▸ Open Project… 的入口(与启动器共用同一条"选目录 → 校验 → 重启"路径);
	//   * 运行 ▸ 启动项目(Runtime)= EditorLayer::LaunchProjectRuntime(当前项目根)。
	// 原生目录对话框与重启都在**帧边界**发生:这里只置 m_OpenProjectBrowsePending。

	void EditorShell::RefreshRecentProjectsIfStale(bool force)
	{
		// 最近列表只在下拉/启动器可见时按 1 秒节流刷新(逐帧查盘 + 解析清单会白烧 IO)。
		const double now = ShellNowSeconds();
		if (!force && m_RecentProjectsLoadedAt > 0.0 && now - m_RecentProjectsLoadedAt < 1.0)
			return;
		m_RecentProjectsLoadedAt = now;
		m_RecentProjects = Editor::ProjectLauncher::LoadRecent();
	}

	const std::vector<Editor::RecentProjectEntry>& EditorShell::RecentProjects()
	{
		RefreshRecentProjectsIfStale();
		return m_RecentProjects;
	}

	void EditorShell::RequestOpenProjectBrowse(Wui::WuiContext& ctx)
	{
		if (m_OpenProjectBrowsePending)
			return;
		m_OpenProjectBrowsePending = true;   // 下一帧开头弹原生"选择项目目录"
		ctx.RecordOp("project", "open-ask", "", "");
	}

	void EditorShell::RunOpenProjectBrowse(Wui::WuiContext& ctx)
	{
		// 原生"选择文件夹"对话框:只在帧边界打开(取消返回空串 → 什么都不做)。
		const std::string folder = FileDialogs::SelectFolder("Open Project");
		if (folder.empty())
			return;
		const std::filesystem::path root = std::filesystem::u8path(folder);
		std::string reason;
		if (!Editor::ProjectLauncher::IsValidProjectRoot(root, &reason))
		{
			// 选中的目录不是项目(缺清单/清单解析失败):通知里给可读原因,不重启。
			ctx.RecordOp("project", "open-failed", folder, reason);
			WLD_CORE_WARN("[project] open failed: '{0}': {1}", folder, reason);
			PushNotice(Wui::TrFormat("notice.project.open_failed", "{reason}: {path}",
				{ { "reason", reason }, { "path", folder } }));
			return;
		}
		ctx.RecordOp("project", "open", Editor::ProjectLauncher::DisplayName(root), root.u8string());
		m_Editor.RelaunchWithProject(root);
	}

	void EditorShell::LaunchCurrentProjectRuntime()
	{
		// 目标项目 = 当前项目根(定位 Runtime / 命令行 / 日志 / 操作记录全在 EditorLayer)。
		m_Editor.LaunchProjectRuntime(World::Paths::ProjectDir());
	}

	void EditorShell::DrawProjectLauncherModal(Wui::WuiContext& ctx)
	{
		const Wui::WuiId modalId = Wui::HashId("modal.project_launcher");
		// 其它模态在前时让位(启动期最可能的来源是错误框);它们收口后启动器会回到前台。
		const bool blockedByOtherModal = m_NewProjectOpen || m_NewPluginOpen || m_NewCppScriptOpen
			|| m_ImportModalOpen ||
			m_PrefabPendingAction != PrefabPendingAction::None || m_Editor.ShowUnsavedModal() ||
			m_Editor.ShowErrorModal() || m_Editor.ShowCookingProgress();
		const bool visible = m_Editor.ShowProjectLauncher() && !blockedByOtherModal;
		// PROJ-7/T3b:刚回到前台的那一帧吞掉点击/Esc(理由见头文件 m_LauncherModalVisible 注释):
		// 向导的"取消"与启动器按钮在同一帧收口时,上一次输入的落点会命中启动器按钮。
		const bool justBecameVisible = visible && !m_LauncherModalVisible;
		m_LauncherModalVisible = visible;
		if (visible)
			ctx.SetModal(modalId);
		else if (ctx.Modal() == modalId)
			ctx.ClearModal();
		if (!visible)
		{
			// PROJ-5R/T1:模态收口/让位时"删除…"流程不悬空 —— 下次打开回到最近列表态。
			m_LauncherDeleteOpen = false;
			m_LauncherDeletePath.clear();
			m_LauncherDeleteName.clear();
			m_LauncherDeleteError.clear();
			return;
		}

		const LauncherDeleteVariant deleteVariant = ClassifyLauncherDeleteTarget(m_LauncherDeletePath);
		Wui::WuiRect frame;
		bool escapePressed = false;
		Wui::ModalFrameDesc frameDesc;
		frameDesc.Id = modalId;
		// PROJ-5R/T1:确认模态的标题按目标当前状态切换(模态 id 不变 —— 确认/失效都是同一个
		// 模态框;确认态下列表行不再绘制/登记,所以模态期间点不到其它项目行)。
		frameDesc.Title = !m_LauncherDeleteOpen
			? Wui::Tr("modal.launcher.title", "Choose a Project")
			: deleteVariant == LauncherDeleteVariant::Delete
				? Wui::Tr("modal.project_delete.title", "Permanently delete this project?")
				: deleteVariant == LauncherDeleteVariant::Missing
					? Wui::Tr("modal.project_delete.missing.title", "This folder no longer exists")
					: Wui::Tr("modal.project_delete.reject.title", "Cannot delete this folder");
		frameDesc.Size = { 660.0f, 430.0f };
		if (!Wui::BeginModalFrame(ctx, frameDesc, &frame, &escapePressed, m_Theme))
			return;
		if (m_LauncherDeleteOpen)
		{
			DrawLauncherDeleteFlow(ctx, frame, escapePressed);
			Wui::EndModalFrame(ctx);
			return;
		}

		const float pad = 16.0f;
		// 行高 24 + 2 间距;列表本体放在标题行与提示行之间,由滚动区裁剪
		// (上限 30 条,超出时滚轮/键盘滚动;行数少时不出现滚动)。
		const float rowHeight = 24.0f;
		const float rowWidth = frame.W - pad * 2.0f;
		float cursorY = frame.Y + 46.0f;
		Wui::Label(ctx, { frame.X + pad, cursorY }, Wui::Tr("modal.launcher.recent", "Recent projects"),
			m_Theme.Text, 13.0f);

		// PROJ-4/T1(P2):搜索框 + 清空按钮。占位提示是单独画的 Label,读屏/脚本读不到 →
		// 显式喂给 TextFieldA11y(与设置页同一口径);清空在空输入时禁用并给理由。
		const float clearWidth = 64.0f;
		const float searchWidth = 220.0f;
		const Wui::WuiRect searchRect { frame.X + frame.W - pad - clearWidth - 8.0f - searchWidth,
			cursorY - 3.0f, searchWidth, 24.0f };
		const Wui::WuiRect clearRect { frame.X + frame.W - pad - clearWidth, cursorY - 3.0f, clearWidth, 24.0f };
		Wui::TextFieldA11y searchA11y;
		searchA11y.Label = Wui::Tr("modal.launcher.search.a11y", "Search recent projects");
		searchA11y.Placeholder = Wui::Tr("modal.launcher.search.hint", "Search projects…");
		Wui::TextField(ctx, Wui::HashId("project.launcher.search"), searchRect, m_LauncherSearch,
			m_Theme, nullptr, &searchA11y);
		if (m_LauncherSearch.empty())
		{
			Wui::Label(ctx, { searchRect.X + 8.0f, searchRect.Y + 5.0f },
				Wui::Tr("modal.launcher.search.hint", "Search projects…"), m_Theme.TextDisabled, 12.0f);
		}
		if (Wui::ButtonEx(ctx, Wui::HashId("project.launcher.search.clear"), clearRect,
			Wui::Tr("modal.launcher.search.clear", "Clear"), m_Theme, !m_LauncherSearch.empty(), false,
			m_LauncherSearch.empty()
				? Wui::Tr("modal.launcher.search.clear_disabled",
					"Nothing to clear — the search box is empty")
				: std::string()))
		{
			m_LauncherSearch.clear();
			m_LauncherScrollY = 0.0f;
		}
		cursorY += 30.0f;

		RefreshRecentProjectsIfStale();
		std::string openPendingPath;
		std::string openPendingName;
		// PROJ-5/T1:点"删除…"⇒下一帧起进入两步确认(本帧先画列表,不改流程状态)。
		std::string deletePendingPath;
		std::string deletePendingName;
		// 过滤只决定"画哪些行";行 id、动作、打开目标一律用**原列表下标** ——
		// 过滤状态下点第一行打开的仍是它自己对应的项目,不会错位到筛选后的第一条。
		const std::string loweredNeedle = AsciiLowerCopy(m_LauncherSearch);
		std::vector<size_t> visibleRows;
		const size_t recentCount = std::min<size_t>(m_RecentProjects.size(), 30);
		for (size_t i = 0; i < recentCount; ++i)
		{
			const Editor::RecentProjectEntry& entry = m_RecentProjects[i];
			if (!ContainsCaseInsensitive(entry.Name, loweredNeedle) &&
				!ContainsCaseInsensitive(entry.Path, loweredNeedle))
				continue;
			visibleRows.push_back(i);
		}
		if (m_RecentProjects.empty())
		{
			Wui::Label(ctx, { frame.X + pad, cursorY + 4.0f },
				Wui::Tr("modal.launcher.empty",
					"No recent projects yet — create a new one, or open a folder that contains project.we.yaml."),
				m_Theme.TextMuted, 13.0f);
		}
		else if (visibleRows.empty())
		{
			// P2:无匹配 ⇒ 空态;文案登记成独立 a11y 节点,脚本按 modal.launcher.no_match 读它。
			const std::string noMatch = Wui::Tr("modal.launcher.no_match",
				"No matching projects — try Open Project… to pick a folder.");
			Wui::Label(ctx, { frame.X + pad, cursorY + 4.0f }, noMatch, m_Theme.TextMuted, 13.0f);
			Wui::WuiAccessNode node;
			node.Id = Wui::HashId("modal.launcher.no_match");
			node.Window = Wui::WuiAccessibility::Get().CurrentWindow();
			node.Panel = "shell";
			node.Kind = "text";
			node.Label = noMatch;
			node.Value = noMatch;
			node.Rect = { frame.X + pad, cursorY + 4.0f, rowWidth, 20.0f };
			node.Enabled = true;
			node.Interactive = false;
			node.Visible = true;
			Wui::WuiAccessibility::Get().Register(node);
		}
		else
		{
			const Wui::WuiRect listClip { frame.X + pad, cursorY, rowWidth,
				std::max(40.0f, frame.Y + frame.H - 82.0f - cursorY) };
			const float contentHeight = static_cast<float>(visibleRows.size()) * (rowHeight + 2.0f);
			Wui::BeginScrollArea(ctx, listClip, contentHeight, m_LauncherScrollY, m_Theme,
				Wui::HashId("project.launcher.recent.scroll"));
			float rowY = listClip.Y - m_LauncherScrollY;
			for (const size_t i : visibleRows)
			{
				const Editor::RecentProjectEntry& entry = m_RecentProjects[i];
				const std::string path = entry.Path;
				const Wui::WuiRect row { listClip.X, rowY, listClip.W, rowHeight };
				rowY += rowHeight + 2.0f;
				if (!ctx.ClipAllows(row))
					continue;   // 滚出视口的行不绘制也不登记(与属性面板滚动区同一口径)
				// PROJ-5R/T1(v2):行尾只剩"删除…"(危险色)——"移除"入口按用户二次指令去掉;
				// 只删列表项的动作改由"目录已不存在"确认模态里的"从列表移除"承接。
				const float deleteWidth = 96.0f;
				const Wui::WuiRect deleteRect { row.X + row.W - deleteWidth, row.Y, deleteWidth, rowHeight };
				const std::string name = entry.Name.empty() ? path : entry.Name;
				if (entry.Valid)
				{
					// 有效行:整行"打开"按钮,宽度让到"删除…"左侧。
					const std::string label = Wui::EllipsizeMiddleToWidth(ctx, name + "    " + entry.Path,
						deleteRect.X - row.X - 16.0f, 15.0f);
					const std::string tooltip = entry.LastOpened.empty()
						? entry.Path : entry.Path + "\n" + entry.LastOpened;
					if (Wui::ButtonEx(ctx,
						Wui::HashId(("project.launcher.recent." + std::to_string(i)).c_str()),
						{ row.X, row.Y, deleteRect.X - row.X - 6.0f, rowHeight }, label, m_Theme,
						true, false, tooltip))
					{
						openPendingPath = path;
						openPendingName = name;
					}
				}
				else
				{
					// 失效行:名称 + 路径 + 失效标记(红)在左(不可打开);右侧仍可"删除…"。
					const std::string text = name + "  —  " + entry.Path + "  ("
						+ Wui::Tr("modal.launcher.invalid", "unavailable") + ")";
					Wui::Label(ctx, { row.X + 2.0f, row.Y + 6.0f },
						Wui::EllipsizeMiddleToWidth(ctx, text, deleteRect.X - row.X - 10.0f, 12.0f),
						m_Theme.Danger, 12.0f);
				}
				// PROJ-5R/T1:"删除…"= 一步确认模态(完整路径 + 永久删除警告 + 确认/取消)。
				// 失效行同样可点开 —— 目录已不存在时模态换成"从列表移除"(只动最近列表);
				// 全部安全守卫仍在 API 里(危险目标会被逐条拒绝并给出可读理由)。
				if (Wui::ButtonEx(ctx,
					Wui::HashId(("project.launcher.recent.delete." + std::to_string(i)).c_str()),
					deleteRect, Wui::Tr("modal.launcher.delete", "Delete…"), DangerButtonTheme(m_Theme),
					true, false,
					Wui::Tr("modal.launcher.delete_hint",
						"Permanently delete this project folder (it will NOT go to the Recycle Bin)")))
				{
					deletePendingPath = path;
					deletePendingName = name;
				}
			}
			Wui::EndScrollArea(ctx);
		}

		// 打开/移除都在遍历之后执行(不能在持有 m_RecentProjects 引用时改表)。
		if (!openPendingPath.empty())
		{
			ctx.RecordOp("project", "open", openPendingName, openPendingPath);
			// PROJ-3/T1:启动器模式下没有"当前项目"可停留 —— 保留启动器页,
			// 子进程起来后本进程就退出(RelaunchWithProject 内部负责收尾)。
			if (!m_LauncherMode)
			{
				m_Editor.DismissProjectLauncher();
				ctx.ClearModal();
			}
			m_Editor.RelaunchWithProject(std::filesystem::u8path(openPendingPath));
			Wui::EndModalFrame(ctx);
			return;
		}
		if (!deletePendingPath.empty())
		{
			// PROJ-5R/T1:打开一步确认模态。这里**不**做任何删除 —— 真正的 remove_all 只在
			// "永久删除"按钮被点时发生,且守卫全在 ProjectLauncher 里。
			m_LauncherDeleteOpen = true;
			m_LauncherDeletePath = deletePendingPath;
			m_LauncherDeleteName = deletePendingName;
			m_LauncherDeleteError.clear();
			ctx.RecordOp("project", "delete-ask", deletePendingName, deletePendingPath);
		}

		// 动作按钮上方的说明(点项目 = 重启编辑器;"新建项目…"向导里可选空模板或示例模板)。
		Wui::Label(ctx, { frame.X + pad, frame.Y + frame.H - 76.0f },
			Wui::Tr("modal.launcher.hint",
				"Picking a project restarts the editor into it. New Project lets you choose a template "
				"(blank, or the example with scenes, materials and scripts); New Plugin creates an "
				"engine plugin package from one of the built-in plugin templates."),
			m_Theme.TextMuted, 12.0f);

		// PROJ-9:动作顺序 = 新建项目… / 打开项目… / 退出(编辑器形态=关闭)。
		// 单独的"新建示例项目…"已移除 —— 新建项目向导里本来就能选示例模板(templates/project-example/**),
		// 少一个入口也少一份维护面(用户 2026-09-29 口径)。
		// PLUG-AUTH-1:启动器形态没有菜单栏,所以「新建插件…」在这里也放一个入口(D1=允许
		// 无项目时新建引擎插件;Project 分段会在向导里禁用并给理由)。
		const Wui::ModalButtonDesc buttons[4] = {
			{ Wui::Tr("modal.launcher.new", "New Project…"), Wui::HashId("project.launcher.new"), true },
			{ Wui::Tr("modal.launcher.new_plugin", "New Plugin…"),
				Wui::HashId("project.launcher.new_plugin"), true },
			{ Wui::Tr("modal.launcher.open", "Open Project…"), Wui::HashId("project.launcher.open"), true },
			// PROJ-3/T1:启动器模式下第 4 个动作是"退出"(关掉整个启动器进程);
			// 普通编辑器形态下仍是 PROJ-2 的"关闭"(停在当前项目)。a11y id 两个形态共用。
			{ m_LauncherMode ? Wui::Tr("modal.launcher.quit", "Quit")
				: Wui::Tr("modal.launcher.close", "Close"),
				Wui::HashId("project.launcher.close"), true },
		};
		// 按钮照常绘制/登记(不能因为吞输入那一帧就少画),只丢掉这一帧的动作。
		const int clickedRaw = Wui::ModalButtons(ctx, frame, buttons, 4, m_Theme);
		const int clicked = justBecameVisible ? -1 : clickedRaw;
		const bool launcherEscape = justBecameVisible ? false : escapePressed;
		Wui::EndModalFrame(ctx);
		if (clicked == 0)
		{
			OpenNewProjectModal(ctx);   // 向导接管模态(启动器保持"待显示",向导收口后回来)
		}
		else if (clicked == 1)
		{
			OpenNewPluginModal(ctx);   // 启动器形态:目标固定引擎插件(向导里 Project 分段禁用)
		}
		else if (clicked == 2)
		{
			RequestOpenProjectBrowse(ctx);
		}
		else if (clicked == 3 || launcherEscape)
		{
			if (m_LauncherMode)
			{
				// "关窗 ⇒ 直接退出(不停留在空项目上)":日志 + 关闭进程都收在 EditorLayer。
				m_Editor.RequestLauncherExit();
			}
			else
			{
				m_Editor.DismissProjectLauncher();
				ctx.ClearModal();
				PushNotice(Wui::Tr("notice.project.launcher_closed",
					"Launcher closed — staying in the current project. Use File ▶ Open Project… to switch later."));
			}
		}
	}

	// PROJ-5R/T1(v2,用户二次指令):启动器"永久删除项目"的确认模态 —— 不再要求逐字输入目录名,
	// 只有"确认/取消"一步。同一个模态按**目标当前状态**(ClassifyLauncherDeleteTarget)分三种:
	//   ① 目录在 + 含项目清单 ⇒ 完整路径 + "永久删除、不进回收站、不可恢复"警告 +
	//      `永久删除`(危险色)+ `取消`(Esc = 取消);
	//   ② 目录已不存在 ⇒ 换成"该目录已不存在",主按钮变 `从列表移除` —— 只调 RemoveRecent
	//      (只动 local/projects.json,绝不碰磁盘);
	//   ③ 目录在但不是项目(缺 project.we.yaml)⇒ 拒绝打开/删除 + 可读理由,主按钮同样是
	//      `从列表移除`(只调 RemoveRecent),磁盘一动不动。
	// 用户可见的删除只发生在形态①的 `永久删除` 被点的那一帧,并且全部安全守卫都在
	// ProjectLauncher::DeleteProjectPermanently(先全查、再做唯一的写操作 remove_all);
	// 失败/被拒时列表项**不动**,原因就地显示 + 状态栏通知。
	void EditorShell::DrawLauncherDeleteFlow(Wui::WuiContext& ctx, const Wui::WuiRect& frame,
		bool escapePressed)
	{
		const float pad = 16.0f;
		const float contentWidth = frame.W - pad * 2.0f;
		const std::string path = m_LauncherDeletePath;
		const LauncherDeleteVariant variant = ClassifyLauncherDeleteTarget(path);
		const auto resetFlow = [this]
		{
			m_LauncherDeleteOpen = false;
			m_LauncherDeletePath.clear();
			m_LauncherDeleteName.clear();
			m_LauncherDeleteError.clear();
		};

		float cursorY = frame.Y + 54.0f;
		// ---- 目标是哪个目录:显示完整路径(读屏/脚本读的节点 Value 是未缩略的原串)----
		const std::string pathLabel = Wui::Tr("modal.project_delete.path", "Folder to delete");
		Wui::Label(ctx, { frame.X + pad, cursorY }, pathLabel, m_Theme.TextMuted, 12.0f);
		cursorY += 18.0f;
		const std::string shownPath = ctx.MeasureTextWidth(path, 13.0f) <= contentWidth
			? path : Wui::EllipsizeMiddleToWidth(ctx, path, contentWidth, 13.0f);
		Wui::Label(ctx, { frame.X + pad, cursorY }, shownPath, m_Theme.Text, 13.0f);
		{
			Wui::WuiAccessNode node;
			node.Id = Wui::HashId("modal.project_delete.path");
			node.Window = Wui::WuiAccessibility::Get().CurrentWindow();
			node.Panel = "shell";
			node.Kind = "text";
			node.Label = pathLabel;
			node.Value = path;             // 完整路径:绘制文本超宽才缩略,节点里永远是原串
			node.Tooltip = path;
			node.Rect = { frame.X + pad, cursorY - 2.0f, contentWidth, 20.0f };
			node.Enabled = true;
			node.Interactive = false;
			node.Visible = true;
			Wui::WuiAccessibility::Get().Register(node);
		}
		cursorY += 30.0f;

		// ---- 形态说明:三种形态互斥,各自的 a11y 节点 id 不同(脚本按 id 断言)----
		{
			Wui::WuiId bodyNodeId = Wui::HashId("modal.project_delete.reject");
			std::string bodyLabel = Wui::Tr("modal.project_delete.reject.title", "Cannot delete this folder");
			std::string bodyText = Wui::Tr("modal.project_delete.error.no_manifest",
				"Not a WorldEngine project: project.we.yaml is missing");
			std::string bodyHint;                 // 形态③第二行:可以只把记录从最近列表移除
			Wui::WuiColor bodyColor = m_Theme.Danger;
			if (variant == LauncherDeleteVariant::Delete)
			{
				bodyNodeId = Wui::HashId("modal.project_delete.warning");
				bodyLabel = Wui::Tr("modal.project_delete.warning.label", "Warning");
				bodyText = Wui::Tr("modal.project_delete.warning",
					"Everything inside this folder is deleted permanently — no Recycle Bin, no recovery.");
			}
			else if (variant == LauncherDeleteVariant::Missing)
			{
				bodyNodeId = Wui::HashId("modal.project_delete.missing");
				bodyLabel = Wui::Tr("modal.project_delete.missing.title", "This folder no longer exists");
				bodyText = Wui::Tr("modal.project_delete.missing",
					"This folder no longer exists — it cannot be deleted. You can remove this entry "
					"from the recent list.");
				bodyColor = m_Theme.Text;
			}
			else if (variant == LauncherDeleteVariant::Reject)
			{
				// 目录在、但没有 project.we.yaml:打不开也删不掉;出口是"只把这条记录从最近
				// 列表移除" —— 与形态②同一条 RemoveRecent 路径,磁盘一动不动。
				bodyHint = Wui::Tr("modal.project_delete.reject.remove_hint",
					"It cannot be opened or deleted. You can still remove this entry from the "
					"recent list — nothing on disk is touched.");
			}
			Wui::Label(ctx, { frame.X + pad, cursorY }, bodyText, bodyColor, 12.5f);
			Wui::WuiAccessNode node;
			node.Id = bodyNodeId;
			node.Window = Wui::WuiAccessibility::Get().CurrentWindow();
			node.Panel = "shell";
			node.Kind = "text";
			node.Label = bodyLabel;
			// 形态③:两行都给到同一个节点,读屏/脚本一次拿到完整说明。
			node.Value = bodyHint.empty() ? bodyText : bodyText + " " + bodyHint;
			node.Rect = { frame.X + pad, cursorY - 2.0f, contentWidth, 20.0f };
			node.Enabled = true;
			node.Interactive = false;
			node.Visible = true;
			Wui::WuiAccessibility::Get().Register(node);
			cursorY += 20.0f;
			if (!bodyHint.empty())
			{
				Wui::Label(ctx, { frame.X + pad, cursorY }, bodyHint, m_Theme.TextMuted, 12.0f);
				cursorY += 20.0f;
			}
		}
		cursorY += 14.0f;

		// 失败/被拒的原因就地显示(上一帧 API 的返回文本);状态栏通知里也有一份。
		if (!m_LauncherDeleteError.empty())
		{
			Wui::Label(ctx, { frame.X + pad, cursorY }, m_LauncherDeleteError, m_Theme.Danger, 12.0f);
			cursorY += 20.0f;
		}

		// 底部按钮与 ModalFooter 同一内边距/高度;这里手排是因为"永久删除"要用危险色主题
		// (ModalButtons 只吃一个主题),按钮宽度沿用 ModalFooter 的测量口径。
		const float buttonHeight = Wui::ModalFooterHeight;
		const float buttonY = frame.Y + frame.H - Wui::ModalFooterPadding - buttonHeight;
		const std::string cancelLabel = Wui::Tr("modal.project_delete.cancel", "Cancel");
		const std::string cancelTooltip = Wui::Tr("modal.project_delete.cancel.tooltip",
			"Cancel and go back to the project list (Esc)");
		const float cancelWidth = std::min(160.0f,
			std::max(90.0f, ctx.MeasureTextWidth(cancelLabel, 15.0f) + 30.0f));

		// 主按钮:形态① = `永久删除`(危险色);形态②/③ = `从列表移除`(安全动作,只动最近列表)。
		std::string primaryLabel;
		if (variant == LauncherDeleteVariant::Delete)
			primaryLabel = Wui::Tr("modal.project_delete.confirm", "Delete permanently");
		else
			primaryLabel = Wui::Tr("modal.project_delete.remove_from_list", "Remove from list");
		const float primaryWidth = primaryLabel.empty() ? 0.0f : std::min(200.0f,
			std::max(110.0f, ctx.MeasureTextWidth(primaryLabel, 15.0f) + 30.0f));
		const float rowX = frame.X + frame.W - pad
			- (primaryWidth > 0.0f ? primaryWidth + 8.0f : 0.0f) - cancelWidth;
		const Wui::WuiRect cancelRect { rowX + (primaryWidth > 0.0f ? primaryWidth + 8.0f : 0.0f),
			buttonY, cancelWidth, buttonHeight };

		bool primaryClicked = false;
		if (variant == LauncherDeleteVariant::Delete)
		{
			primaryClicked = Wui::ButtonEx(ctx, Wui::HashId("project.project_delete.confirm"),
				{ rowX, buttonY, primaryWidth, buttonHeight }, primaryLabel, DangerButtonTheme(m_Theme),
				true, true, std::string());
		}
		else
		{
			// 失效/非项目条目补偿(形态②目录已不存在;形态③目录在但不是项目):
			// 只动最近列表 —— tooltip 明确"磁盘上什么都不动"。
			primaryClicked = Wui::ButtonEx(ctx, Wui::HashId("project.project_delete.confirm"),
				{ rowX, buttonY, primaryWidth, buttonHeight }, primaryLabel, m_Theme, true, false,
				Wui::Tr("modal.project_delete.remove_from_list.tooltip",
					"Remove this entry from the recent list only — nothing on disk is touched"));
		}
		const bool cancelClicked = Wui::ButtonEx(ctx, Wui::HashId("project.project_delete.cancel"),
			cancelRect, cancelLabel, m_Theme, true, false, cancelTooltip);

		if (variant == LauncherDeleteVariant::Delete && primaryClicked)
		{
			// 唯一写操作:全部守卫 + remove_all 都在 API 里;返回空串 = 成功。
			const std::string reason = Editor::ProjectLauncher::DeleteProjectPermanently(
				std::filesystem::u8path(path));
			if (reason.empty())
			{
				ctx.RecordOp("project", "delete", m_LauncherDeleteName, path);
				RefreshRecentProjectsIfStale(/*force=*/true);   // 成功后条目已从最近列表移除
				PushNotice(Wui::TrFormat("notice.project_delete.done", "Permanently deleted: {path}",
					{ { "path", path } }));
				resetFlow();
			}
			else
			{
				// 失败/被拒 ⇒ 列表项不动(避免"看着删了其实还在");模态留在确认态可重试或取消。
				ctx.RecordOp("project", "delete-failed", m_LauncherDeleteName, reason);
				WLD_CORE_WARN("[project] permanent delete refused or failed: '{0}': {1}", path, reason);
				m_LauncherDeleteError = reason;
				PushNotice(Wui::TrFormat("notice.project_delete.failed", "Permanent delete failed: {reason}",
					{ { "reason", reason } }));
			}
		}
		else if (variant != LauncherDeleteVariant::Delete && primaryClicked)
		{
			// 失效/非项目条目补偿:只调 RemoveRecent(local/projects.json),磁盘一动不动。
			std::string error;
			if (Editor::ProjectLauncher::RemoveRecent(std::filesystem::u8path(path), &error))
			{
				ctx.RecordOp("project", "recent-remove", m_LauncherDeleteName, path);
				RefreshRecentProjectsIfStale(/*force=*/true);
				PushNotice(Wui::TrFormat("notice.project.recent_removed",
					"Removed from the recent list: {name} (the project files on disk were not touched)",
					{ { "name", m_LauncherDeleteName } }));
				resetFlow();
			}
			else
			{
				ctx.RecordOp("project", "recent-remove-failed", m_LauncherDeleteName, error);
				PushNotice(Wui::TrFormat("notice.project.recent_remove_failed",
					"Could not update the recent projects list: {reason}", { { "reason", error } }));
				m_LauncherDeleteError = error;
			}
		}
		else if (cancelClicked || escapePressed)
		{
			ctx.RecordOp("project", "delete-cancel", m_LauncherDeleteName, path);
			resetFlow();
		}
	}

	void EditorShell::EnsureModelPanelFromId(const std::string& panelId)
	{
		if (m_PanelRegistry.find(panelId) != m_PanelRegistry.end())
			return;
		if (panelId.compare(0, std::strlen(kModelPanelPrefix), kModelPanelPrefix) != 0)
			return;
		const std::string path = panelId.substr(std::strlen(kModelPanelPrefix));
		if (path.empty())
			return;
		auto panel = std::make_unique<ModelPreviewPanel>(path);
		m_PanelRegistry.emplace(panelId, std::move(panel));
		if (std::find(m_Panels.begin(), m_Panels.end(), panelId) == m_Panels.end())
			m_Panels.push_back(panelId);
	}

	// ---- PLUG-T3b:插件贡献的面板(动态实例,id = "plugin.panel.<pluginId>.<id>")----

	void EditorShell::EnsurePluginPanelsFromRegistry()
	{
		if (m_LauncherMode || !m_PluginEditorHost)
			return;
		// 注册表是插件加载完成后的权威清单(插件中途失败不会留半个注册)。
		for (const std::string& panelId : m_PluginEditorHost->PanelIds())
			EnsurePluginPanelFromId(panelId);
	}

	void EditorShell::EnsurePluginPanelFromId(const std::string& panelId)
	{
		if (m_LauncherMode || !m_PluginEditorHost)
			return;
		if (m_PanelRegistry.find(panelId) != m_PanelRegistry.end())
			return;
		if (!IsPluginPanelId(panelId) || !m_PluginEditorHost->HasPanel(panelId))
			return;
		auto panel = std::make_unique<PluginPanel>(*this, panelId);
		m_PanelRegistry.emplace(panelId, std::move(panel));
		if (std::find(m_Panels.begin(), m_Panels.end(), panelId) == m_Panels.end())
			m_Panels.push_back(panelId);
		// 默认尺寸:插件面板要装下几行控件与按钮(与插件管理器同量级)。
		if (m_LastFloatRects.find(panelId) == m_LastFloatRects.end())
			m_LastFloatRects[panelId] = Wui::WuiRect { 200.0f, 140.0f, 520.0f, 360.0f };
		if (!m_Layout.FindFloatMemory(panelId, nullptr))
			m_Layout.FloatMemory.push_back({ panelId, m_LastFloatRects[panelId] });
	}

	void EditorShell::ClosePluginPanelsNotInRegistry()
	{
		if (m_LauncherMode || !m_PluginEditorHost)
			return;
		// 插件卸载后:注册表里没有了 ⇒ 关掉已打开的窗口/标签/停靠记录(不留下空窗口)。
		const auto pluginPanelGone = [this](const std::string& panelId)
		{
			return IsPluginPanelId(panelId) && !m_PluginEditorHost->HasPanel(panelId);
		};
		const auto isGone = [&pluginPanelGone](const std::string& panelId)
		{
			return pluginPanelGone(panelId);
		};

		// 独立窗口(含附加态标签):先关窗口,再摘附加标签。
		std::vector<std::string> staleHosts;
		for (const std::unique_ptr<FloatWindowHost>& host : m_FloatHosts)
			for (const std::string& panelId : host->Panels())
				if (isGone(panelId) && std::find(staleHosts.begin(), staleHosts.end(), panelId)
					== staleHosts.end())
					staleHosts.push_back(panelId);
		for (const std::string& panelId : staleHosts)
			CloseFloatWindow(panelId, true, m_Ctx);
		m_AttachedPanels.erase(std::remove_if(m_AttachedPanels.begin(), m_AttachedPanels.end(),
			[&isGone](const std::string& panelId) { return isGone(panelId); }), m_AttachedPanels.end());
		if (isGone(m_ActiveWindowTag))
			m_ActiveWindowTag.clear();
		for (const Wui::DockFloat& entry : m_Layout.Floating)
			if (isGone(entry.Panel))
				m_Layout.CloseFloating(entry.Panel);
		m_Layout.FloatMemory.erase(std::remove_if(m_Layout.FloatMemory.begin(), m_Layout.FloatMemory.end(),
			[&isGone](const Wui::DockFloat& entry) { return isGone(entry.Panel); }), m_Layout.FloatMemory.end());
		m_Panels.erase(std::remove_if(m_Panels.begin(), m_Panels.end(),
			[&pluginPanelGone](const std::string& panelId) { return pluginPanelGone(panelId); }), m_Panels.end());
		std::vector<Wui::PanelId> dockedPanels;
		m_Layout.AllPanels(&dockedPanels);
		for (const Wui::PanelId& panelId : dockedPanels)
			if (isGone(panelId))
				m_Layout.RemoveTab(panelId);
		// unordered_map 不支持 remove_if 的整体删除(pair 不可赋值)⇒ 显式遍历 + erase(it)。
		for (auto it = m_PanelRegistry.begin(); it != m_PanelRegistry.end(); )
		{
			if (pluginPanelGone(it->first))
				it = m_PanelRegistry.erase(it);
			else
				++it;
		}
	}

	void EditorShell::DrawPluginPanel(Wui::WuiContext& ctx, const Wui::WuiRect& rect, const std::string& panelId)
	{
		Plugins::PluginManager* manager = GetPluginManager();
		if (!manager || !m_PluginEditorHost || !m_PluginEditorHost->HasPanel(panelId))
		{
			// 面板已经被注销(插件卸载/禁用)而窗口还没收掉:给一行可读空态而不是空白。
			Label(ctx, { rect.X + 8.0f, rect.Y + 8.0f },
				Wui::Tr("panel.plugin.unavailable", "This plugin panel is no longer available."),
				m_Theme.TextMuted, m_Theme.FontSizeBody);
			return;
		}
		m_CurrentPluginPanelRect = rect;
		if (manager->RenderEditorPanel(panelId) != 0)
			WLD_CORE_WARN("[plugin] editor panel '{0}' draw failed", panelId);
		m_CurrentPluginPanelRect = Wui::WuiRect { 0, 0, 0, 0 };
	}

	bool EditorShell::InvokePluginEditorCommand(const std::string& commandName, std::string* message)
	{
		if (!m_PluginEditorHost)
		{
			if (message) *message = Wui::Tr("plugin.command.unavailable", "No editor command surface.");
			return false;
		}
		std::string error;
		if (!m_PluginEditorHost->InvokeEditorCommand(commandName, &error))
		{
			if (message) *message = error;
			return false;
		}
		if (message) *message = "invoked " + commandName;
		return true;
	}

	// ---- P4-U13c:prefab 资产窗口(动态实例,id = "prefab:<逻辑路径>")----

	void EditorShell::EnsurePrefabPanelFromId(const std::string& panelId)
	{
		if (m_PanelRegistry.find(panelId) != m_PanelRegistry.end())
			return;
		if (panelId.compare(0, std::strlen(kPrefabPanelPrefix), kPrefabPanelPrefix) != 0)
			return;
		const std::string path = panelId.substr(std::strlen(kPrefabPanelPrefix));
		if (path.empty())
			return;
		auto panel = std::make_unique<PrefabPanel>(path);
		m_PanelRegistry.emplace(panelId, std::move(panel));
		if (std::find(m_Panels.begin(), m_Panels.end(), panelId) == m_Panels.end())
			m_Panels.push_back(panelId);
	}

	// ---- W9-2:脚本编辑器面板(动态实例,id = "script:<逻辑路径>")----

	void EditorShell::EnsureScriptPanelFromId(const std::string& panelId)
	{
		if (m_PanelRegistry.find(panelId) != m_PanelRegistry.end())
			return;
		if (panelId.compare(0, std::strlen(kScriptPanelPrefix), kScriptPanelPrefix) != 0)
			return;
		const std::string path = panelId.substr(std::strlen(kScriptPanelPrefix));
		if (path.empty())
			return;
		auto panel = std::make_unique<ScriptEditorPanel>(path);
		m_PanelRegistry.emplace(panelId, std::move(panel));
		if (std::find(m_Panels.begin(), m_Panels.end(), panelId) == m_Panels.end())
			m_Panels.push_back(panelId);
		// 默认窗口尺寸:代码编辑器需要足够宽高(自动 gutter + 状态行 + 工具栏都在默认客户区内)。
		if (m_LastFloatRects.find(panelId) == m_LastFloatRects.end())
			m_LastFloatRects[panelId] = Wui::WuiRect { 220.0f, 150.0f, 900.0f, 620.0f };
		if (!m_Layout.FindFloatMemory(panelId, nullptr))
			m_Layout.FloatMemory.push_back({ panelId, m_LastFloatRects[panelId] });
	}

	void EditorShell::OpenScriptEditor(const std::string& logicalPath)
	{
		// 面板渲染中途(内容浏览器双击 / Scripts 面板按钮)不能立刻"建新窗口(新 Vulkan
		// 交换链)+ 改附加标签":用户实测双击 .lua 会触发 vkQueueSubmit 设备丢失、界面黑屏。
		// 统一推迟到下一帧 OnRender 开头(与 AI 通道的帧首执行同为安全点)。
		std::string normalized = logicalPath;
		std::replace(normalized.begin(), normalized.end(), '\\', '/');
		if (normalized.empty())
			return;
		if (std::find(m_PendingScriptOpen.begin(), m_PendingScriptOpen.end(), normalized) == m_PendingScriptOpen.end())
		{
			m_PendingScriptOpen.push_back(std::move(normalized));
			WLD_CORE_INFO("[script-editor] open request deferred to frame boundary");
		}
	}

	std::filesystem::path EditorShell::CurrentProjectRoot() const
	{
		// 运行期项目根唯一入口 = World::Paths::ProjectDir();有清单才算"打开着项目"
		// (启动器的哨兵目录 / 引擎内空的 projects/ 容器都没有清单 → 空)。
		std::error_code error;
		const std::filesystem::path root = World::Paths::ProjectDir();
		if (root.empty() || !std::filesystem::is_regular_file(root / "project.we.yaml", error))
			return {};
		return root;
	}

	bool EditorShell::ResolveCppSourcePath(const std::string& path, std::filesystem::path& out,
		std::string* error) const
	{
		auto fail = [error](const std::string& text)
		{
			if (error)
				*error = text;
			return false;
		};
		std::string relative = path;
		// 旧逻辑路径前缀(`module:` + checkout 相对路径):老存档 / 老调用点仍可能带它,
		// 这里只当作"去掉前缀的相对路径",不再有独立的模块源码编辑通道。
		constexpr const char* kModuleSourcePrefix = "module:";
		if (relative.rfind(kModuleSourcePrefix, 0) == 0)
			relative.erase(0, std::strlen(kModuleSourcePrefix));
		if (relative.empty())
			return fail("empty C++ source path");

		std::error_code fileError;
		const std::filesystem::path candidate(relative);
		// 绝对路径(项目源码视图给的就是绝对路径)原样接受。
		if (candidate.is_absolute() || candidate.has_root_name())
		{
			if (std::filesystem::is_regular_file(candidate, fileError))
			{
				out = candidate;
				return true;
			}
			return fail("file not found: " + candidate.string());
		}

		// 相对路径依次按 项目根 → 仓库根 → 内容根 解析(第一条命中的算数)。
		const std::filesystem::path roots[] = {
			CurrentProjectRoot(),
			std::filesystem::path(WLD_REPO_ROOT),
			World::Paths::AssetRoot(),
		};
		std::string tried;
		for (const std::filesystem::path& root : roots)
		{
			if (root.empty())
				continue;
			const std::filesystem::path full = root / candidate;
			if (!tried.empty())
				tried += ", ";
			tried += full.string();
			if (std::filesystem::is_regular_file(full, fileError))
			{
				out = full;
				return true;
			}
		}
		return fail("C++ source not found: " + path + (tried.empty() ? "" : " (tried " + tried + ")"));
	}

	void EditorShell::OpenInVisualStudioNow(const std::filesystem::path& absolute)
	{
		std::string message;
		if (m_Editor.OpenInVisualStudio(absolute, &message))
			return;
		PushNotice(Wui::TrFormat("notice.vsopen.failed", "Could not open in Visual Studio: {reason}",
			{ { "reason", message.empty() ? absolute.string() : message } }));
		WLD_CORE_WARN("[vsopen] open failed for '{0}': {1}", absolute.string(), message);
	}

	void EditorShell::OpenScriptEditorNow(const std::string& logicalPath)
	{
		// 用户决定(2026-09-18 C):打开即**默认直接附加到主窗口** —— OS 窗口保持隐藏,
		// 主窗口顶栏出现切换标签(点击在主界面/脚本内容之间切换);用户仍可把它拖出成独立窗口。
		std::string normalized = logicalPath;
		std::replace(normalized.begin(), normalized.end(), '\\', '/');
		if (normalized.empty())
			return;
		// CPPT-7/PROJ-8:C++ 源码(含旧的 `module:` 逻辑路径)不进内置编辑器 —— 改走外部
		// Visual Studio(策略与固定日志见 EditorLayer::OpenInVisualStudio)。
		if (IsCppSourcePath(normalized))
		{
			std::filesystem::path resolved;
			std::string resolveError;
			if (ResolveCppSourcePath(normalized, resolved, &resolveError))
			{
				OpenInVisualStudioNow(resolved);
			}
			else
			{
				PushNotice(Wui::TrFormat("notice.vsopen.failed",
					"Could not open in Visual Studio: {reason}", { { "reason", resolveError } }));
				WLD_CORE_WARN("[vsopen] cannot resolve '{0}': {1}", normalized, resolveError);
			}
			return;
		}
		const std::string panelId = std::string(kScriptPanelPrefix) + normalized;
		EnsureScriptPanelFromId(panelId);
		if (std::find(m_AttachedPanels.begin(), m_AttachedPanels.end(), panelId) != m_AttachedPanels.end())
		{
			m_ActiveWindowTag = panelId; // 已打开:重复打开只是激活该标签
			return;
		}
		// 尚未附加:先按独立窗口建出(必要时复用已隐藏的窗口),随后立即挂靠到主窗口。
		if (!FindFloatHost(panelId))
			OpenIndependentPanel(panelId);
		AttachIndependentWindowToSlot(panelId);
	}

	void EditorShell::CloseEditorPanel(const std::string& panel)
	{
		if (!m_Ctx || panel.empty())
			return;
		// 与 Window 菜单/顶栏标签关闭同一条路径:已附加 → 摘标签并隐藏窗口;
		// 已浮动 → 隐藏复用;未打开 → 不做事(按钮只在面板可见时存在)。
		const auto attached = std::find(m_AttachedPanels.begin(), m_AttachedPanels.end(), panel);
		FloatWindowHost* host = FindFloatHost(panel);
		const bool visibleWindow = host && !host->IsHidden();
		if (attached == m_AttachedPanels.end() && !visibleWindow)
			return;
		TogglePanel(*m_Ctx, panel);
	}

	// ---- P4-U13e:prefab 资产窗口的"未保存改动"守卫 ----
	//
	// 关窗 / 进文档会话都会丢掉窗口里未落盘的编辑,所以先弹项目现成的确认模态问一次
	// (丢弃 / 取消),用户点"丢弃"后才执行被延迟的那个动作。守卫只认 prefab 面板,
	// 其它面板原样透传(不影响既有路径)。

	PrefabPanel* EditorShell::PrefabPanelById(const std::string& panelId) const
	{
		const auto found = m_PanelRegistry.find(panelId);
		if (found == m_PanelRegistry.end())
			return nullptr;
		return dynamic_cast<PrefabPanel*>(found->second.get());
	}

	bool EditorShell::InterceptPrefabUnsaved(const std::string& panelId, PrefabPendingAction action,
		const std::string& logicalPath)
	{
		if (m_PrefabGuardBypass || panelId.empty())
			return false;
		PrefabPanel* panel = PrefabPanelById(panelId);
		if (!panel || !panel->HasUnsavedChanges())
			return false;
		if (m_PrefabPendingAction == action && m_PrefabPendingPanel == panelId)
			return true;   // 已经在等用户回答:不要重复排队
		m_PrefabPendingPanel = panelId;
		m_PrefabPendingLogical = logicalPath;
		m_PrefabPendingAction = action;
		// 独立窗口的 × 会先把 OS 窗口标成 should-close(此后不再渲染,内容会冻在上一帧)。
		// 这里把那个待关闭状态撤销:用户点"取消"后窗口还能继续用(容器唯一会清 should-close
		// 的公开入口是"显示但不激活")。挂靠态的面板 OS 窗口本来就在隐藏复用,跳过。
		if (FloatWindowHost* host = FindFloatHost(panelId); host && !host->IsHidden())
			host->ShowWithoutActivation();
		WLD_CORE_INFO("[prefab] '{0}' has unsaved edits: waiting for the discard confirmation", panelId);
		return true;
	}

	void EditorShell::RunPendingPrefabAction(Wui::WuiContext& ctx)
	{
		const std::string panel = m_PrefabPendingPanel;
		const std::string logical = m_PrefabPendingLogical;
		const PrefabPendingAction action = m_PrefabPendingAction;
		m_PrefabPendingPanel.clear();
		m_PrefabPendingLogical.clear();
		m_PrefabPendingAction = PrefabPendingAction::None;
		// 丢弃面板内的编辑(下一次绘制会从磁盘重读),然后执行被拦下的动作。
		if (PrefabPanel* prefab = PrefabPanelById(panel))
			prefab->DiscardUnsavedChanges();
		m_PrefabGuardBypass = true;
		if (action == PrefabPendingAction::Close && !panel.empty())
			TogglePanel(ctx, panel);
		else if (action == PrefabPendingAction::OpenDocument && !logical.empty())
			m_Editor.OpenPrefab(logical);
		m_PrefabGuardBypass = false;
	}

	void EditorShell::DrawPrefabUnsavedModal(Wui::WuiContext& ctx)
	{
		const Wui::WuiId modalId = Wui::HashId("modal.prefab.unsaved");
		if (m_PrefabPendingAction != PrefabPendingAction::None)
			ctx.SetModal(modalId);
		else if (ctx.Modal() == modalId)
			ctx.ClearModal();

		Wui::WuiRect panel;
		bool escapePressed = false;
		Wui::ModalFrameDesc frameDesc;
		frameDesc.Id = modalId;
		frameDesc.Title = Wui::Tr("modal.prefab.unsaved.title", "Unsaved Prefab Changes");
		frameDesc.Size = { 500.0f, 160.0f };
		if (!Wui::BeginModalFrame(ctx, frameDesc, &panel, &escapePressed, m_Theme))
			return;
		const bool opening = m_PrefabPendingAction == PrefabPendingAction::OpenDocument;
		Label(ctx, { panel.X + 16.0f, panel.Y + 44.0f }, opening
				? Wui::Tr("modal.prefab.unsaved.line1_open", "The full editor loads this asset from disk.")
				: Wui::Tr("modal.prefab.unsaved.line1_close", "This window has unsaved prefab edits."),
			m_Theme.Text, 14.0f);
		Label(ctx, { panel.X + 16.0f, panel.Y + 64.0f }, opening
				? Wui::Tr("modal.prefab.unsaved.line2_open", "Continuing discards the edits made in this window.")
				: Wui::Tr("modal.prefab.unsaved.line2_close", "Closing discards them; Save writes them into the asset."),
			m_Theme.Text, 14.0f);
		Label(ctx, { panel.X + 16.0f, panel.Y + 86.0f }, m_PrefabPendingPanel, m_Theme.TextMuted, 12.0f);
		const std::string discardLabel = Wui::Tr("modal.prefab.unsaved.discard", "Discard");
		const std::string cancelLabel = Wui::Tr("modal.prefab.unsaved.cancel", "Cancel");
		const Wui::WuiId discardId = Wui::HashId("modal.prefab.unsaved.discard");
		const Wui::WuiId cancelId = Wui::HashId("modal.prefab.unsaved.cancel");
		// 按钮几何与 Wui::ModalButtons 的两按钮口径一致(等分、整行居中、gap 8;内边距/按钮高
		// 取 ModalFooter 的公开常量)。按钮自己画而不是走 ModalButtons,原因只有一个:
		// 本帧如果先渲染过可见的独立窗口,WuiAccessibility 的"当前窗口"会停在那个浮窗上
		// (每个窗口在自己的 BeginFrame 里设置它),而模态是画在主窗口里的 —— 跟着"当前窗口"
		// 登记的话,脚本 ui.invoke 会按浮窗去投递点击(实测:点不到)。这里显式写死 main。
		const float buttonGap = 8.0f;
		const float available = std::max(80.0f, panel.W - Wui::ModalFooterPadding * 2.0f);
		const float buttonWidth = std::clamp((available - buttonGap) * 0.5f, 48.0f, 240.0f);
		const float rowWidth = buttonWidth * 2.0f + buttonGap;
		const float buttonX = panel.X + (panel.W - rowWidth) * 0.5f;
		const float buttonY = panel.Y + panel.H - Wui::ModalFooterPadding - Wui::ModalFooterHeight;
		const Wui::WuiRect discardRect { buttonX, buttonY, buttonWidth, Wui::ModalFooterHeight };
		const Wui::WuiRect cancelRect { buttonX + buttonWidth + buttonGap, buttonY, buttonWidth,
			Wui::ModalFooterHeight };
		const auto registerMainButton = [](Wui::WuiId id, const Wui::WuiRect& rect, const std::string& label)
		{
			Wui::WuiAccessNode node;
			node.Id = id;
			node.Window = "main";
			node.Panel = "shell";
			node.Kind = "button";
			node.Label = label;
			node.Rect = rect;
			node.Enabled = true;
			node.Visible = true;
			node.Interactive = true;
			Wui::WuiAccessibility::Get().Register(node);
		};
		registerMainButton(discardId, discardRect, discardLabel);
		registerMainButton(cancelId, cancelRect, cancelLabel);
		const bool discardClicked = Wui::Button(ctx, discardId, discardRect, discardLabel, m_Theme);
		const bool cancelClicked = Wui::Button(ctx, cancelId, cancelRect, cancelLabel, m_Theme);
		if (discardClicked)
		{
			RunPendingPrefabAction(ctx);
		}
		else if (cancelClicked || escapePressed)
		{
			// 取消:窗口/资产原样不动,未保存改动保留。
			m_PrefabPendingPanel.clear();
			m_PrefabPendingLogical.clear();
			m_PrefabPendingAction = PrefabPendingAction::None;
		}
		Wui::EndModalFrame(ctx);
	}

	// W5-L1:文档场景外部改动提示(视口面板读;按钮触发重开,未保存时走确认模态)。
	bool EditorShell::ExternalSceneChanged() const
	{
		return m_Editor.ExternalSceneChanged();
	}

	void EditorShell::ReopenExternalScene()
	{
		m_Editor.ReopenExternalScene();
	}

	bool EditorShell::InstantiateModelFile(const std::string& logicalPath, std::string* message)
	{
		return m_Editor.InstantiateModelFile(logicalPath, message);
	}

	bool EditorShell::SaveProjectRenderSettings(const Asset::RenderingSettings& settings, std::string* message)
	{
		// 写盘口径与其它"回写清单"的地方一致:先 Load(保留 id/场景/包列表等字段),
		// 只覆盖 rendering,再 Validate + Save。
		std::filesystem::path manifestPath;
		if (!Asset::ProjectManifest::Locate(std::filesystem::current_path(), &manifestPath))
		{
			if (message) *message = "找不到 project.we.yaml(工作目录下没有清单)";
			return false;
		}
		Asset::ProjectManifest manifest;
		std::string error;
		if (!Asset::ProjectManifest::Load(manifestPath, &manifest, &error))
		{
			if (message) *message = "清单读取失败: " + error;
			return false;
		}
		manifest.Rendering = settings;
		if (!Asset::ProjectManifest::Save(manifestPath, manifest, &error))
		{
			if (message) *message = "清单写入失败: " + error;
			return false;
		}
		if (message)
			*message = "已保存渲染设置到 " + manifestPath.filename().string();
		return true;
	}

	bool EditorShell::SaveProjectPhysicsSettings(const Asset::PhysicsSettingsData& settings, std::string* message)
	{
		// 与渲染设置同一套"回写清单"口径:Load → 只覆盖 physics → Validate + Save。
		std::filesystem::path manifestPath;
		if (!Asset::ProjectManifest::Locate(std::filesystem::current_path(), &manifestPath))
		{
			if (message) *message = "找不到 project.we.yaml(工作目录下没有清单)";
			return false;
		}
		Asset::ProjectManifest manifest;
		std::string error;
		if (!Asset::ProjectManifest::Load(manifestPath, &manifest, &error))
		{
			if (message) *message = "清单读取失败: " + error;
			return false;
		}
		manifest.Physics = settings;
		if (!Asset::ProjectManifest::Save(manifestPath, manifest, &error))
		{
			if (message) *message = "清单写入失败: " + error;
			return false;
		}
		if (message)
			*message = "已保存物理设置到 " + manifestPath.filename().string();
		return true;
	}

	bool EditorShell::SaveProjectImportDefaults(const Asset::ModelImportSettings& settings,
		std::string* message)
	{
		// P4-U4:与渲染/物理同一套"回写清单"口径(Load → 只覆盖 imports → Save)。
		// 额外一步:写进进程级默认,让**本次会话之后的新导入**立刻按新默认走。
		std::filesystem::path manifestPath;
		if (!Asset::ProjectManifest::Locate(std::filesystem::current_path(), &manifestPath))
		{
			if (message) *message = "找不到 project.we.yaml(工作目录下没有清单)";
			return false;
		}
		Asset::ProjectManifest manifest;
		std::string error;
		if (!Asset::ProjectManifest::Load(manifestPath, &manifest, &error))
		{
			if (message) *message = "清单读取失败: " + error;
			return false;
		}
		manifest.ImportDefaults = settings;
		if (!Asset::ProjectManifest::Save(manifestPath, manifest, &error))
		{
			if (message) *message = "清单写入失败: " + error;
			return false;
		}
		Asset::ModelImportSettings::SetProjectDefaults(settings);
		if (message)
			*message = "已保存导入默认值到 " + manifestPath.filename().string();
		return true;
	}

	// P4-UX11:项目启动项(renderer / start_scene / content_root)。
	// 与渲染/物理同一套"回写清单"口径:Load(保留其它字段与注释风格) → 只覆盖这三项 → Save。
	bool EditorShell::SaveProjectStartupSettings(const std::string& renderer, const std::string& startScene,
		const std::string& contentRoot, std::string* message)
	{
		std::filesystem::path manifestPath;
		if (!Asset::ProjectManifest::Locate(std::filesystem::current_path(), &manifestPath))
		{
			if (message) *message = "找不到 project.we.yaml(工作目录下没有清单)";
			return false;
		}
		Asset::ProjectManifest manifest;
		std::string error;
		if (!Asset::ProjectManifest::Load(manifestPath, &manifest, &error))
		{
			if (message) *message = "清单读取失败: " + error;
			return false;
		}
		if (!renderer.empty())
			manifest.Renderer = renderer;
		manifest.StartScene = startScene;
		if (!contentRoot.empty())
			manifest.ContentRoot = contentRoot;
		if (!Asset::ProjectManifest::Save(manifestPath, manifest, &error))
		{
			if (message) *message = "清单写入失败: " + error;
			return false;
		}
		if (message)
			*message = "已保存启动项到 " + manifestPath.filename().string();
		return true;
	}

	std::vector<std::string> EditorShell::ListProjectScenes()
	{
		// 内容根下的 .wd 场景(逻辑路径,与 manifest 的 start_scene 同一口径:相对 content_root)。
		std::vector<std::string> scenes;
		std::filesystem::path manifestPath;
		if (!Asset::ProjectManifest::Locate(std::filesystem::current_path(), &manifestPath))
			return scenes;
		Asset::ProjectManifest manifest;
		std::string error;
		if (!Asset::ProjectManifest::Load(manifestPath, &manifest, &error))
			return scenes;
		const std::filesystem::path root = manifest.ResolveContentRoot(manifestPath);
		std::error_code code;
		for (const std::filesystem::directory_entry& entry :
			std::filesystem::recursive_directory_iterator(root, code))
		{
			if (code)
				break;
			if (!entry.is_regular_file(code) || entry.path().extension() != ".wd")
				continue;
			scenes.push_back(std::filesystem::relative(entry.path(), root, code).generic_string());
		}
		std::sort(scenes.begin(), scenes.end());
		return scenes;
	}

	void EditorShell::ApplyProjectRendererChange(const std::string& renderer)
	{
		// 运行中热切换会串资源(GL 名字/描述符集跨上下文复用) → EditorLayer 会写回请求并重启编辑器。
		m_Editor.ApplyRendererChange(renderer);
	}

	bool EditorShell::SaveProjectPackages(const std::vector<std::string>& packages, std::string* message)
	{
		std::filesystem::path manifestPath;
		if (!Asset::ProjectManifest::Locate(std::filesystem::current_path(), &manifestPath))
		{
			if (message) *message = "找不到 project.we.yaml(工作目录下没有清单)";
			return false;
		}
		Asset::ProjectManifest manifest;
		std::string error;
		if (!Asset::ProjectManifest::Load(manifestPath, &manifest, &error))
		{
			if (message) *message = "清单读取失败: " + error;
			return false;
		}
		manifest.Packages = packages;
		if (!Asset::ProjectManifest::Save(manifestPath, manifest, &error))
		{
			if (message) *message = "清单写入失败: " + error;
			return false;
		}
		if (message)
			*message = "已保存发行包列表(" + std::to_string(packages.size()) + " 项)";
		return true;
	}

	bool EditorShell::ImportModelFile(const std::string& sourcePath, std::string* message,
		std::string* outLogicalModel)
	{
		return m_Editor.ImportModelFile(sourcePath, message, outLogicalModel);
	}

	bool EditorShell::ImportModelFileTo(const std::string& sourcePath, const std::string& destinationLogicalDir,
		std::string* message, std::string* outLogicalModel)
	{
		return m_Editor.ImportModelFile(sourcePath, message, outLogicalModel, destinationLogicalDir);
	}

	EditorPanel* EditorShell::FocusedPanel()
	{
		// ① 主窗口处于"已附加面板"模式(顶栏标签):该面板就是焦点面板。
		if (!m_ActiveWindowTag.empty())
		{
			const auto found = m_PanelRegistry.find(m_ActiveWindowTag);
			if (found != m_PanelRegistry.end())
				return found->second.get();
		}
		// ② 独立窗口在前台:该窗口当前标签的面板。
		for (const std::unique_ptr<FloatWindowHost>& host : m_FloatHosts)
		{
			if (host->IsHidden() || !host->IsFocused())
				continue;
			const auto found = m_PanelRegistry.find(host->ActivePanel());
			if (found != m_PanelRegistry.end())
				return found->second.get();
		}
		// ③ 文本焦点所属面板(脚本编辑器 / 搜索框 / 重命名框等)。
		const std::string& textPanel = Wui::WuiTextFocus::Get().Panel();
		if (!textPanel.empty())
		{
			const auto found = m_PanelRegistry.find(textPanel);
			if (found != m_PanelRegistry.end())
				return found->second.get();
		}
		return nullptr;
	}

	void EditorShell::DockBackIndependentWindow(const std::string& panel)
	{
		// 收回整窗:记住用户调好的尺寸,销毁独立窗口并把它的全部标签挂回停靠树。
		FloatWindowHost* host = FindFloatHost(panel);
		if (!host)
			return;
		const std::vector<std::string> panels = host->Panels();
		const Wui::WuiRect rect = host->ScreenRect();
		for (const std::string& id : panels)
			m_LastFloatRects[id] = rect;
		for (const std::string& id : panels)
			host->RemovePanel(id);
		host->SetHidden(true); // 复用:隐藏而非销毁(运行期销毁窗口会崩)

		const Wui::PanelId anchor = m_Layout.FirstPanel();
		for (const std::string& id : panels)
		{
			// 停靠树为空时无法挂载:仅隐藏该面板(CloseFloating 会留下跨会话位置记忆)。
			if (!anchor.empty() && m_Layout.DockFloating(id, anchor, Wui::DropZone::Center))
				WLD_CORE_INFO("Independent window docked back: {0}", id);
			else
				m_Layout.CloseFloating(id);
		}
	}

	void EditorShell::AttachIndependentWindowToSlot(const std::string& panel)
	{
		// 挂靠整窗:窗口的全部面板作为槽位所在标签组的成员回到主窗口,OS 窗口销毁。
		FloatWindowHost* host = FindFloatHost(panel);
		if (!host)
			return;
		// 只有声明为"独立窗口"的面板能挂靠到顶部挂靠栏;停靠形态的临时浮动不允许挂靠。
		if (!IsIndependentPanel(panel))
		{
			WLD_CORE_INFO("[float] panel '{0}' is a docked panel; attach ignored", panel);
			return;
		}
		const std::vector<std::string> panels = host->Panels();
		const Wui::WuiRect rect = host->ScreenRect();
		for (const std::string& id : panels)
			m_LastFloatRects[id] = rect;
		// 附加 = 与主窗口建立"标签切换"关系:窗口与其面板保持不变,只隐藏 OS 窗口;
		// 主窗口顶栏出现该标签,点击即在 主界面 / 该窗口内容 之间切换。
		host->SetHidden(true);
		// 顶栏标签 = 一枚 OS 窗口(不是一枚面板):先把同一窗口里其它标签页的旧记录摘掉,
		// 再补上本次的面板 —— 否则会出现"一个窗口两枚 chip",拖出其中一枚会把整个窗口拔走。
		for (const std::string& id : panels)
			m_AttachedPanels.erase(std::remove(m_AttachedPanels.begin(), m_AttachedPanels.end(), id),
				m_AttachedPanels.end());
		m_AttachedPanels.push_back(panel);
		m_ActiveWindowTag = panel;
		WLD_CORE_INFO("Independent window attached as switch tab: {0} (still in dock tree: {1})",
			panel, m_Layout.Contains(panel) ? "yes" : "no");

		// 不再把面板并入停靠树(那是旧"挂靠"语义);附加只建立标签切换关系。
		m_AttachSlotHighlight = false;
		m_DragPanel.clear();
		m_MovingFloat.clear();
		m_TabDragPanel.clear();
		m_LastDragPos = { 0, 0 };
		m_AttachCooldownFrames = 45;
		WLD_CORE_INFO("Independent window attached to slot: {0} ({1} panels)", panel, panels.size());
		SaveLayout();   // 立刻落盘"这次是挂靠形态",下次启动才能按形态恢复/询问(P4-UX10)
	}

	// 隐藏单个面板(标签栏 x / Window 菜单):从所属窗口摘除,窗口为空则销毁。
	void EditorShell::HideFloatPanel(const std::string& panel, Wui::WuiContext* ctx)
	{
		// P4-U13e:prefab 面板有未保存改动时先问一次(丢弃 / 取消),不静默丢。
		if (InterceptPrefabUnsaved(panel, PrefabPendingAction::Close))
			return;
		const std::string before = m_Layout.Serialize();
		if (FloatWindowHost* host = FindFloatHost(panel))
		{
			// 写回最后位置:窗口内其余标签仍由它们自己的布局记录继续跟踪。
			const Wui::WuiRect rect = host->ScreenRect();
			if (Wui::DockFloat* entry = m_Layout.FindFloat(panel))
				entry->Rect = rect;
			m_LastFloatRects[panel] = rect;
			host->RemovePanel(panel);
			if (host->Empty())
				host->SetHidden(true); // 复用:隐藏而非销毁
		}
		m_LastFloatScreenRects.erase(panel);
		// 独立形态:隐藏即从浮动记录移除(Window 菜单可重开);
		// 停靠形态:临时拖出的标签关闭 = 回停靠位(D3)。
		const bool changed = IsIndependentPanel(panel)
			? m_Layout.CloseFloating(panel)
			: DockPanelBackToTree(panel);
		if (changed && ctx)
			RecordDockChange(*ctx, "hide", panel, before);
	}

	// 整窗关闭(用户关闭/渲染失败):窗口内全部面板隐藏,布局写回并记录操作。
	void EditorShell::CloseFloatWindow(const std::string& panel, bool recordChange, Wui::WuiContext* ctx)
	{
		// P4-U13e:同上(OS 窗口关闭 / 挂靠标签 × 都从这里过)。
		if (InterceptPrefabUnsaved(panel, PrefabPendingAction::Close))
			return;
		const std::string before = m_Layout.Serialize();
		FloatWindowHost* host = FindFloatHost(panel);
		if (!host)
		{
			// 宿主已不存在:仅清理浮动记录,避免残留"看不见的窗口"。
			if (m_Layout.CloseFloating(panel) && recordChange && ctx)
				RecordDockChange(*ctx, "hide", panel, before);
			// 停靠形态的"临时浮动"关闭后应回停靠位(独立形态保持隐藏)。
			if (!IsIndependentPanel(panel))
				DockPanelBackToTree(panel);
			return;
		}
		const std::vector<std::string> panels = host->Panels();
		const Wui::WuiRect rect = host->ScreenRect();
		for (const std::string& id : panels)
			host->RemovePanel(id);
		host->SetHidden(true); // 复用:隐藏而非销毁
		bool changed = false;
		for (const std::string& id : panels)
		{
			if (Wui::DockFloat* entry = m_Layout.FindFloat(id))
				entry->Rect = rect;
			// 独立形态:关闭 = 隐藏复用(移除浮动记录,便于 Window 菜单重开)。
			// 停靠形态:临时拖出不跨会话,直接回停靠树(D3)。
			if (IsIndependentPanel(id))
			{
				m_LastFloatRects[id] = rect;
				changed = m_Layout.CloseFloating(id) || changed;
			}
			else
			{
				changed = DockPanelBackToTree(id) || changed;
			}
		}
		if (changed && recordChange && ctx)
			RecordDockChange(*ctx, "hide", panel, before);
	}

	void EditorShell::DrawMenuBar(Wui::WuiContext& ctx)
	{
		const glm::vec2 viewport = ctx.ViewportSize();

		// 菜单栏属于"当前窗口":切到已附加的独立窗口内容时,显示它自己的菜单栏
		// (Widget 目前为空),而不是主窗口的 File/Window 菜单。
		if (!m_ActiveWindowTag.empty())
		{
			Wui::PanelBackground(ctx, { 0, 26, viewport.x, 26 }, m_Theme.PanelHeader);
			return;
		}

		// Header = 分组小标题行:只显示、不响应悬停/点击(菜单里区分"独立窗口/停靠面板")。
		// Tooltip 只给"看名字猜不到全部含义"的项(悬停解释 + 同步进无障碍节点的 Tooltip)。
		struct MenuEntry
		{
			std::string Label;
			bool Checked;
			std::function<void()> Action;
			bool Header = false;
			std::string Tooltip;
			// CPPT-3:显式无障碍 id(空 = 沿用"菜单名 + 本地化标签"的既有派生口径)。
			// 需要跨语言稳定 id 的项(如 menu.file.build_reload_cpp_module)必须显式给。
			Wui::WuiId ExplicitId = 0;
			// PROJ-2/T1:失效的"最近项目"行灰显(可读不可点);其余菜单项默认可用。
			bool Enabled = true;
		};

		const Wui::WuiId menuFile = Wui::HashId("menu.file");
		const Wui::WuiId menuWindow = Wui::HashId("menu.window");
		const Wui::WuiId menuRun = Wui::HashId("menu.run");
		if (!m_MenuBar)
		{
			m_MenuBar = std::make_shared<Wui::WuiBox>();
			m_MenuBar->Direction = Wui::WuiDirection::Row;
			m_MenuBar->Gap = 4;
			m_FileButton = std::make_shared<Wui::WuiButton>();
			// P4-U7:菜单栏按钮给稳定 id —— 对象式按钮按 id 进无障碍树,AI 通道
			// (`ui.invoke`)才能打开菜单;没有 id 的按钮只登记焦点、脚本点不到。
			m_FileButton->SetId(menuFile);
			m_FileButton->Label = Wui::Tr("menu.file", "File");
			m_FileButton->OnClick = [this, menuFile]
				{
					const bool opening = m_OpenMenu != menuFile;
					m_OpenMenu = opening ? menuFile : 0;
					if (opening)
					{
						m_Ctx->CloseAllPopups();
						m_MenuHeaderRect = m_FileButton->Rect();
						m_Ctx->OpenPopup(menuFile);
					}
					else
						m_Ctx->ClosePopup(menuFile);
				};
			m_MenuBar->Add(m_FileButton, { 60, 60, 0, 22, 0 });
			m_WindowButton = std::make_shared<Wui::WuiButton>();
			m_WindowButton->SetId(menuWindow);
			m_WindowButton->Label = Wui::Tr("menu.window", "Window");
			m_WindowButton->OnClick = [this, menuWindow]
				{
					const bool opening = m_OpenMenu != menuWindow;
					m_OpenMenu = opening ? menuWindow : 0;
					if (opening)
					{
						m_Ctx->CloseAllPopups();
						m_MenuHeaderRect = m_WindowButton->Rect();
						m_Ctx->OpenPopup(menuWindow);
					}
					else
						m_Ctx->ClosePopup(menuWindow);
				};
			m_MenuBar->Add(m_WindowButton, { 78, 78, 0, 22, 0 });
			// PROJ-2/T1:运行菜单 —— 独立 Runtime 进程的"启动项目"入口(不重启编辑器)。
			m_RunButton = std::make_shared<Wui::WuiButton>();
			m_RunButton->SetId(menuRun);
			m_RunButton->Label = Wui::Tr("menu.run", "Run");
			m_RunButton->OnClick = [this, menuRun]
				{
					const bool opening = m_OpenMenu != menuRun;
					m_OpenMenu = opening ? menuRun : 0;
					if (opening)
					{
						m_Ctx->CloseAllPopups();
						m_MenuHeaderRect = m_RunButton->Rect();
						m_Ctx->OpenPopup(menuRun);
					}
					else
						m_Ctx->ClosePopup(menuRun);
				};
			m_MenuBar->Add(m_RunButton, { 64, 64, 0, 22, 0 });
		}

		// 菜单栏下移一行:顶部第一行现在是挂靠栏。
		Wui::PanelBackground(ctx, { 0, 26, viewport.x, 26 }, m_Theme.PanelHeader);
		// P4-U8:菜单栏/挂靠栏/模态都属于**外壳**(不属于最后渲染的那个面板)。
		// 不显式切面板时,菜单项的无障碍节点会挂着上一个面板 id(实测:View 菜单项被标成
		// panel="properties"),脚本按面板过滤就找不到它。
		Wui::WuiAccessibility::Get().SetPanel("shell");
		Wui::LayoutWidgetTree(m_MenuBar, { 8, 28, viewport.x - 16, 22 });
		Wui::WuiPaintContext paint(ctx);
		m_MenuBar->Paint(paint);

		// M4-S2 遗留①(另一半):任何**不是**"点外面关闭"的收口路径(别的弹层打开时的
		// CloseAllPopups、面板里的 Esc、脚本 ui.close…)都会把弹层关掉而 m_OpenMenu 仍留着;
		// 那会让同一个按钮下一次点击被判成"已经在开" → 又是按两次。每帧按弹层真实状态对齐:
		// 开着的那个是唯一事实源,弹层没了就把归属清零。放在按钮绘制之后(刚打开那一帧
		// 弹层已 Open,不会被误清)。
		if (m_OpenMenu != 0 && !ctx.IsPopupOpen(m_OpenMenu))
			m_OpenMenu = 0;

		auto drawMenu = [&](const Wui::WuiId menuId, const std::string& menuIdName, const std::vector<MenuEntry>& entries)
		{
			if (ctx.IsPopupOpen(menuId))
			{
				ctx.PushOverlay();
				// M4-S2 遗留②:菜单栏下拉的**绘制命令**延后到帧末(模态画完之后),与 U29 给
				// Combo/SearchableCombo 的 `WuiDeferredPopupScope` 同一口径 —— 菜单栏在模态之前
				// 绘制,否则模态一开就把下拉盖住(同类 z-order 风险)。命中测试、Tooltip、
				// RegisterOverlayRect 与关闭判定全部留在原地当场生效(命令只是搬走像素)。
				std::vector<Wui::WuiDrawCommand>& menuOverlay = ctx.Commands();
				const size_t menuCommandMark = menuOverlay.size();
				const Wui::WuiRect panel { m_MenuHeaderRect.X, m_MenuHeaderRect.Y + m_MenuHeaderRect.H + 2, 240, static_cast<float>(entries.size() * 22 + 8) };
				DrawPanelSurface(ctx, panel, m_Theme);
				// P4-U7:菜单矩形登记为覆盖层 —— 下一帧面板内容不会吃掉落在菜单上的点击
				// (实测:菜单项那一下会同时穿透到下面的面板控件)。
				ctx.RegisterOverlayRect(panel);
				for (size_t i = 0; i < entries.size(); ++i)
				{
					const Wui::WuiRect item { panel.X + 4, panel.Y + 4 + i * 22, panel.W - 8, 22 };
					if (entries[i].Header)
					{
						// 分组标题:淡色小字,不可点击。
						Label(ctx, { item.X + 4.0f, item.Y + 4.0f }, entries[i].Label, m_Theme.TextMuted, 12.0f);
						// 分组标题也要进无障碍树 —— 可见但读不到,读屏/脚本就看不出菜单的分组结构。
						Wui::WuiAccessNode header;
						header.Id = Wui::HashId((menuIdName + ".group." + std::to_string(i)).c_str());
						header.Window = Wui::WuiAccessibility::Get().CurrentWindow();
						header.Panel = Wui::WuiAccessibility::Get().CurrentPanel();
						header.Kind = "text";
						header.Label = entries[i].Label;
						header.Rect = item;
						header.Interactive = false;
						Wui::WuiAccessibility::Get().Register(header);
						continue;
					}
					const Wui::WuiId itemId = entries[i].ExplicitId != 0
						? entries[i].ExplicitId
						: Wui::HashId((menuIdName + "." + entries[i].Label).c_str());
					// P4-U8:悬停解释(勾选项的"关掉是什么效果"这类信息读不出来,只能靠提示)。
					if (!entries[i].Tooltip.empty())
						Wui::Tooltip(ctx, item, entries[i].Tooltip);
					if (MenuItem(ctx, itemId, item, entries[i].Label, entries[i].Checked, entries[i].Enabled, m_Theme))
					{
						entries[i].Action();
						ctx.RecordOp("menu", "item", entries[i].Label, menuIdName);
						ctx.CloseAllPopups();
						m_OpenMenu = 0;
					}
					// 无障碍:MENU 项说明必须同时进节点 —— 读屏/脚本看不到悬停提示。
					if (!entries[i].Tooltip.empty())
					{
						Wui::WuiAccessNode node;
						node.Id = itemId;
						node.Window = Wui::WuiAccessibility::Get().CurrentWindow();
						node.Panel = Wui::WuiAccessibility::Get().CurrentPanel();
						node.Kind = "menu-item";
						node.Label = entries[i].Label;
						node.Value = entries[i].Checked ? "checked" : "unchecked";
						node.Tooltip = entries[i].Tooltip;
						node.Rect = item;
						node.Enabled = entries[i].Enabled;
						node.Interactive = entries[i].Enabled;
						Wui::WuiAccessibility::Get().Register(node);
					}
				}
				ctx.ClosePopupsOnOutsideClick({ menuId }, panel);
				// M4-S2 遗留①:点外面关闭后必须**同步清零 m_OpenMenu** —— 否则下一次按同一个
				// 菜单按钮时 `opening = (m_OpenMenu != menuFile)` 判成"已经开着" → 只发关闭、
				// 菜单不出现,用户得按两次(实测)。这里按**弹层真实状态**收口(不看返回值:
				// ClosePopupsOnOutsideClick 在"弹层是本帧刚打开"时会跳过关闭但同样返回 true)。
				if (m_OpenMenu == menuId && !ctx.IsPopupOpen(menuId))
					m_OpenMenu = 0;
				if (ctx.IsKeyPressed(KeyCodes::Escape))
				{
					ctx.ClosePopup(menuId);
					m_OpenMenu = 0;
				}
				// 只搬"这一帧仍然开着"的菜单:菜单项动作(打开模态等)会在本帧调 CloseAllPopups,
				// 那一帧的命令必须丢弃 —— 否则刚弹出的模态会被上一帧的下拉盖一帧。
				if (ctx.IsPopupOpen(menuId))
				{
					m_DeferredMenuCommands.insert(m_DeferredMenuCommands.end(),
						menuOverlay.begin() + static_cast<std::ptrdiff_t>(menuCommandMark), menuOverlay.end());
				}
				menuOverlay.resize(menuCommandMark);
				ctx.PopOverlay();
			}
		};

		std::vector<MenuEntry> fileEntries;
		fileEntries.push_back({ Wui::Tr("menu.file.new", "New"), false, [this] { m_Editor.NewScene(); } });
		fileEntries.push_back({ Wui::Tr("menu.file.open", "Open"), false, [this] { m_Editor.OpenScene(); } });
		fileEntries.push_back({ Wui::Tr("menu.file.save", "Save"), false, [this] { m_Editor.SaveScene(); } });
		// PROJ-2/T1:File ▸ Open Project…(选目录 → 校验 project.we.yaml → 重启到该项目)。
		// 原生对话框推迟到下一帧开头(见 OnRender 的 m_OpenProjectBrowsePending 分支)。
		fileEntries.push_back({ Wui::Tr("menu.file.open_project", "Open Project…"), false,
			[this]
			{
				if (m_Ctx)
					RequestOpenProjectBrowse(*m_Ctx);
			}, false,
			Wui::Tr("menu.file.open_project.tooltip",
				"Open a project from any folder: pick the project root (the folder that contains "
				"project.we.yaml), validate the manifest, then the editor restarts into it."),
			Wui::HashId("menu.file.open_project") });
		// PROJ-1/T1:任意位置新建**标准**项目(干净骨架,不含示例)。稳定 id
		// menu.file.new_project:AI 通道/自动化按 id 点它,不依赖标签语言。
		fileEntries.push_back({ Wui::Tr("menu.file.new_project", "New Project…"), false,
				[this]
				{
					if (m_Ctx)
						OpenNewProjectModal(*m_Ctx);
				}, false,
				Wui::Tr("menu.file.new_project.tooltip",
					"Create a standard project skeleton (no samples) at any location: project name + "
					"location → manifest, content root and a minimal runnable scene (camera + directional "
					"light, can be turned off); then open it in Explorer, restart the editor into it, or "
					"launch it."),
				Wui::HashId("menu.file.new_project") });
		// PLUG-AUTH-1:File ▸ New Plugin…(引擎插件 / 项目插件;6 类模板;同一个脚手架)。
		// 稳定 id menu.file.new_plugin:AI 通道/自动化按 id 点它,不依赖标签语言。
		fileEntries.push_back({ Wui::Tr("menu.file.new_plugin", "New Plugin…"), false,
				[this]
				{
					if (m_Ctx)
						OpenNewPluginModal(*m_Ctx);
				}, false,
				Wui::Tr("menu.file.new_plugin.tooltip",
					"Create a plugin package (manifest + source + CMake) from one of the built-in "
					"templates: engine plugins land in <engine>/plugins, project plugins in "
					"<project>/plugins. The manifest is validated by the plugin loader before the "
					"files land."),
				Wui::HashId("menu.file.new_plugin") });
		// PROJ-2/T1:Recent Projects 分区(最多 10 条;失效项灰显,仍带完整路径与原因提示)。
		// 只在有记录时出现;列表在弹出后节流刷新(见 RefreshRecentProjectsIfStale)。
		if (ctx.IsPopupOpen(menuFile))
		{
			RefreshRecentProjectsIfStale();
			const size_t recentCount = std::min<size_t>(m_RecentProjects.size(), 10);
			if (recentCount > 0)
			{
				MenuEntry header;
				header.Label = Wui::Tr("menu.file.recent", "Recent Projects");
				header.Checked = false;
				header.Header = true;
				fileEntries.push_back(std::move(header));
			}
			for (size_t i = 0; i < recentCount; ++i)
			{
				const Editor::RecentProjectEntry& entry = m_RecentProjects[i];
				MenuEntry item;
				item.Label = entry.Name.empty() ? entry.Path : entry.Name;
				item.Checked = false;
				item.Enabled = entry.Valid;
				const std::string path = entry.Path;
				item.Action = [this, path] { m_Editor.RelaunchWithProject(std::filesystem::u8path(path)); };
				item.Tooltip = entry.Valid ? path : path + " — " + entry.InvalidReason;
				item.ExplicitId = Wui::HashId(("menu.file.recent." + std::to_string(i)).c_str());
				fileEntries.push_back(std::move(item));
			}
		}
		fileEntries.push_back({ Wui::Tr("menu.file.import", "Import glTF..."), false,
			[this] { m_Editor.ImportModelDialog(); } });
		fileEntries.push_back({ Wui::Tr("menu.file.new_cpp_script", "New C++ Component…"), false,
				[this]
				{
					if (m_Ctx)
						OpenNewCppScriptModal(*m_Ctx);
				}, false,
				Wui::Tr("menu.file.new_cpp_script.tooltip",
					"Create a pure-ECS C++ component (data-only struct) under <project>/src/Components/ "
					"(scalar/enum/struct/Array/Map samples) and open it in Visual Studio. Put logic in "
					"<project>/src/Systems/ and register it in <project>/src/GameProject.cpp; build the "
					"project, then use File ▶ Build & Reload C++ Module to load it."),
				Wui::HashId("menu.file.new_cpp_script") });
		// CPPSRC-1(用户 2026-09-29「c++脚本要像 asset 资产一样在编辑器里展示」):
		// 项目 C++ 的唯一展示面 = 内容浏览器的 `Project C++` 根 —— 这一项不再打开 Scripts 面板
		// 的列表段,而是把内容浏览器切到源码根(同一套网格/列表/类型列/搜索)。
		fileEntries.push_back({ Wui::Tr("menu.file.project_sources", "Project Sources…"), false,
				[this]
				{
					if (!m_Ctx)
						return;
					FocusContentBrowserProjectSources();
				}, false,
				Wui::Tr("menu.file.project_sources.tooltip",
					"Show this project's C++ sources (<project>/src) in the Content Browser — same grid or "
					"list, type column and search as assets; double-click a file to open it in Visual Studio."),
				Wui::HashId("menu.file.project_sources") });
		// PROJ-11/T1 + PROJ-12/T2:已存在的项目(向导旧版本建出来的)同步
		// CMakeLists.txt / build.cmd / CMakePresets.json(旧版备份 .bak 后升级)、
		// 两个 exe 启动器、.we/engine-root.txt、.vs/launch.vs.json
		// (PROJ-15/T1:锚定项目自己的 ProjectRun,.vs/ProjectSettings.json 设成当前启动项;
		// 根级旧版 launch.vs.json 清掉)与 .gitignore。
		// 不碰 src 与 assets。
		fileEntries.push_back({ Wui::Tr("menu.file.generate_build_entry",
				"Generate Build Entry Points (CMake + build.cmd)"), false,
				[this]
				{
					if (!m_Ctx)
						return;
					const std::filesystem::path projectRoot = CurrentProjectRoot();
					if (projectRoot.empty())
					{
						PushNotice(Wui::Tr("notice.build_entry.no_project",
							"No project is open — open or create one first, then generate its build entry points."));
						return;
					}
					m_BuildEntryResult = Editor::ProjectScaffolder::EnsureBuildEntryPoints(projectRoot);
					m_BuildEntryOpen = true;
					m_Ctx->RecordOp("project", "build-entry", projectRoot.filename().u8string(),
						m_BuildEntryResult.Ok ? "ok" : "failed");
				}, false,
				Wui::Tr("menu.file.generate_build_entry.tooltip",
					"For the current project: sync CMakeLists.txt / build.cmd / CMakePresets.json from the "
					"engine template (an older version is backed up as <name>.bak-<timestamp> and existing "
					"backups are never overwritten; legacy root .cmd launchers move into .we/ as "
					"legacy-*.bak, or stay in place when there is no replacement launcher exe), refresh "
					"<project>-Edit.exe / <project>-Play.exe, write .we/engine-root.txt (plus a .cmd fallback "
					"in .we/ only when a launcher exe is missing) and .vs/launch.vs.json (one Visual Studio "
					"CMake launch item anchoring the project's own ProjectRun target, which opens the editor "
					"by default; .vs/ProjectSettings.json gets CurrentProjectSetting pointed at it while the "
					"keys Visual Studio already wrote are kept; a leftover root-level launch.vs.json from "
					"the older generator is removed), "
					"then top up .gitignore. Never touches src/ or assets/."),
				Wui::HashId("menu.file.generate_build_entry") });
		// HOTR-P3-T7:一步完成"卸载 + 构建 + 加载" —— 与 AI `module.build_reload` 是同一条
		// EditorLayer::BuildAndReloadCppModule 入口(用户在编辑器内不再需要切到 VS/CMake 构建)。
		// 2026-10-01 用户口径:旧的"分步 Reload C++ Module"菜单项已移除(AI 侧仍保留
		// `module.unload`/`module.reload` 两段式给自动化用)。
		fileEntries.push_back({ Wui::Tr("menu.file.build_reload_cpp_module", "Build & Reload C++ Module (Game.dll)"),
				false,
				[this] { m_Editor.BuildAndReloadCppModule(); }, false,
				Wui::Tr("menu.file.build_reload_cpp_module.tooltip",
					"Unload Game.dll, run this project's own build.cmd in the background, then load the "
					"new build (a build failure keeps the module unloaded and shows the build output in "
					"the log). One build at a time."),
				Wui::HashId("menu.file.build_reload_cpp_module") });
		fileEntries.push_back({ Wui::Tr("menu.file.project_settings", "Project Settings"), false, [this]
				{
					// P4-UX11:项目设置只有一处入口 = 独立窗口的 Settings 面板(渲染/物理/启动与内容)。
					// 旧的"只有渲染后端一项"的模态框已删除,避免两套界面互相打架。
					if (m_Ctx)
						TogglePanel(*m_Ctx, "settings");
				} });
		fileEntries.push_back({ Wui::Tr("menu.file.editor_settings", "Editor Preferences"), false, [this, &ctx]
				{
					if (!FindFloatHost("prefs") && !m_Layout.Contains("prefs"))
						OpenIndependentPanel("prefs");
					TogglePanel(ctx, "prefs");
				} });
		fileEntries.push_back({ Wui::Tr("menu.file.lua_stubs", "Generate Lua API Stubs"), false,
			[this] { m_Editor.GenerateLuaStubsAction(); } });
		fileEntries.push_back({ Wui::Tr("menu.file.cooking", "Cooking"), false,
			[this] { m_Editor.StartCookingAction(); } });
		fileEntries.push_back({ Wui::Tr("menu.file.export_ops", "Export Operation Log"), false,
			[this] { m_Editor.ExportOperationLog(); } });
		fileEntries.push_back({ Wui::Tr("menu.file.exit", "Exit"), false, [this] { m_Editor.CloseAction(); } });
		drawMenu(menuFile, "menu.file", fileEntries);

		// PROJ-2/T1:运行 ▸ 启动项目(Runtime)—— 独立 Runtime 进程跑当前项目的 start_scene,
		// 不重启编辑器、不需要先进 Play。
		drawMenu(menuRun, "menu.run", {
			{ Wui::Tr("menu.run.launch_project", "Launch Project (Runtime)"), false,
				[this] { LaunchCurrentProjectRuntime(); }, false,
				Wui::Tr("menu.run.launch_project.tooltip",
					"Start the current project in a separate Runtime process (manifest start_scene). "
					"The editor keeps running; no restart and no Play session needed."),
				Wui::HashId("menu.run.launch_project") },
		});

		// 菜单分组:独立窗口(自带 OS 窗口)与停靠面板分开列,并给出不可点击的分组标题 ——
		// 之前两类平铺在一起,用户看不出"Gallery/Input Map 是独立窗口,其余是停靠标签"。
		std::vector<MenuEntry> windowEntries;
		// U25-M2:写材质的入口 —— "从零新建一份材质"(模板 + 名称 + 目录 + 实时落点),
		// 与内容浏览器空白右键的 New ▸ Material… 共用同一个向导(实现全在内容浏览器面板)。
		windowEntries.push_back({ Wui::Tr("menu.window.new_material", "New Material…"), false,
			[this] {
				std::string message;
				if (!OpenNewMaterialWizard(false, &message))
					Notify(message.empty() ? std::string("could not open the new-material wizard") : message);
			}, false,
			Wui::Tr("menu.window.new_material.tooltip",
				"New Material…: pick a template, name it, choose a folder — then it opens in the "
				"material editor.") });
		const auto appendPanels = [this, &windowEntries, &ctx](bool independent)
		{
			for (const std::string& panel : m_Panels)
			{
				if (IsIndependentPanel(panel) != independent)
					continue;
				const bool visible = m_Layout.Contains(panel) || m_Layout.IsFloating(panel) ||
					std::find(m_AttachedPanels.begin(), m_AttachedPanels.end(), panel) != m_AttachedPanels.end();
				windowEntries.push_back({ PanelTitle(panel), visible, [this, panel, &ctx] { TogglePanel(ctx, panel); } });
			}
		};
		windowEntries.push_back({ Wui::Tr("menu.window.independent", "Independent Windows"), false, {}, true });
		appendPanels(/*independent=*/true);
		windowEntries.push_back({ Wui::Tr("menu.window.docked", "Docked Panels"), false, {}, true });
		appendPanels(/*independent=*/false);
		windowEntries.push_back({ Wui::Tr("menu.window.reset_layout", "Reset Layout"), false, [this, &ctx] { ResetLayout(ctx); } });
		drawMenu(menuWindow, "menu.window", windowEntries);

		// P4-U8a:菜单栏不再有 View 菜单 —— "看"的开关**全部搬进视口自己的悬浮 `视图 ▼`**
		// (用户 2026-09-21:「视图菜单栏的功能能否放到 view 里」;Unity 的 Scene 视图工具条/
		// Unreal 的 Show 菜单就是这个位置)。实现见 ViewportPanel::OnRender。
	}

	// ---- PROJ-11/T1:File ▸ 生成项目构建入口 的结果模态 ----
	// 落盘动作已在菜单回调里同步执行(纯文件复制,毫秒级);这里只显示结果 +
	// 两条可复制的指引(build.cmd → 双击 <项目名>-Play.exe / 运行 ▸ 启动项目)。
	void EditorShell::DrawBuildEntryModal(Wui::WuiContext& ctx)
	{
		const Wui::WuiId modalId = Wui::HashId("modal.build_entry");
		if (!m_BuildEntryOpen)
		{
			if (ctx.Modal() == modalId)
				ctx.ClearModal();
			return;
		}
		ctx.SetModal(modalId);

		Wui::WuiRect frame;
		bool escapePressed = false;
		Wui::ModalFrameDesc frameDesc;
		frameDesc.Id = modalId;
		frameDesc.Title = Wui::Tr("modal.build_entry.title", "Project Build Entry Points");
		// PROJ-12F/T2:多出"备份(时间戳)"与"旧 .cmd 迁移到 .we/"两行,框加高。
		frameDesc.Size = { 720.0f, 420.0f };
		if (!Wui::BeginModalFrame(ctx, frameDesc, &frame, &escapePressed, m_Theme))
		{
			// 模态被更高优先级的路径接管:同步清掉状态,避免下一帧再抢焦点。
			m_BuildEntryOpen = false;
			return;
		}

		const float labelX = frame.X + 16.0f;
		float cursorY = frame.Y + 44.0f;
		const auto join = [](const std::vector<std::string>& values)
		{
			std::string text;
			for (const std::string& value : values)
			{
				if (!text.empty())
					text += ", ";
				text += value;
			}
			return text;
		};

		const std::string projectName = m_BuildEntryResult.ProjectRoot.filename().u8string();
		const std::string projectRoot = m_BuildEntryResult.ProjectRoot.u8string();
		// 复制到剪贴板的完整指引(与下面画出来的两步同文)。
		const std::string guidance = Wui::TrFormat("modal.build_entry.guidance",
			"Project root ({root}): run build.cmd; double-click {name}-Edit.exe / {name}-Play.exe. "
			"Other entry files live in .we/ (engine-root.txt records the engine root; a .cmd fallback "
			"appears there only when a launcher exe is missing; legacy root .cmd launchers are moved "
			"there as legacy-*.bak, never deleted). Visual Studio has one launch item "
			"(.vs/launch.vs.json): ProjectRun.exe — it builds Game first, then opens the editor; add "
			"`--play` (or double-click {name}-Play.exe) to run the game instead.",
			{ { "root", projectRoot }, { "name", projectName } });

		if (!m_BuildEntryResult.Ok)
		{
			Wui::Label(ctx, { labelX, cursorY }, Wui::TrFormat("modal.build_entry.error",
				"Could not generate the build entry points: {reason}",
				{ { "reason", m_BuildEntryResult.Error } }), m_Theme.Accent, 13.0f);
			const Wui::ModalButtonDesc buttons[1] = {
				{ Wui::Tr("modal.build_entry.close", "Close"),
					Wui::HashId("modal.build_entry.close"), true },
			};
			const int clicked = Wui::ModalButtons(ctx, frame, buttons, 1, m_Theme);
			if (clicked == 0 || escapePressed)
			{
				m_BuildEntryOpen = false;
				ctx.ClearModal();
			}
			Wui::EndModalFrame(ctx);
			return;
		}

		if (!m_BuildEntryResult.Created.empty())
		{
			Wui::Label(ctx, { labelX, cursorY }, Wui::TrFormat("modal.build_entry.created",
				"Created: {files}", { { "files", join(m_BuildEntryResult.Created) } }),
				m_Theme.Text, 13.0f);
			cursorY += 20.0f;
		}
		if (!m_BuildEntryResult.Upgraded.empty())
		{
			Wui::Label(ctx, { labelX, cursorY }, Wui::TrFormat("modal.build_entry.upgraded",
				"Upgraded: {files}",
				{ { "files", join(m_BuildEntryResult.Upgraded) } }),
				m_Theme.Text, 13.0f);
			cursorY += 20.0f;
		}
		// PROJ-12F/T2:备份名带时间戳(绝不覆盖旧备份)—— 列出实际文件,便于用户找回。
		// 名字里带时间戳 + 项目前缀,一行一份(多份挤一行会被右边缘裁掉);超过 4 份折叠。
		if (!m_BuildEntryResult.Backups.empty())
		{
			Wui::Label(ctx, { labelX, cursorY }, Wui::Tr("modal.build_entry.backups",
				"Previous versions backed up (timestamped, never overwritten):"),
				m_Theme.TextMuted, 12.0f);
			cursorY += 18.0f;
			constexpr size_t kMaxListedBackups = 4;
			const size_t listedBackups = std::min(kMaxListedBackups, m_BuildEntryResult.Backups.size());
			for (size_t index = 0; index < listedBackups; ++index)
			{
				Wui::Label(ctx, { labelX + 16.0f, cursorY }, m_BuildEntryResult.Backups[index],
					m_Theme.TextMuted, 12.0f);
				cursorY += 16.0f;
			}
			if (listedBackups < m_BuildEntryResult.Backups.size())
			{
				Wui::Label(ctx, { labelX + 16.0f, cursorY }, Wui::TrFormat("modal.build_entry.backups_more",
					"… and {count} more (see the editor log)",
					{ { "count", std::to_string(m_BuildEntryResult.Backups.size() - listedBackups) } }),
					m_Theme.TextMuted, 12.0f);
				cursorY += 16.0f;
			}
		}
		// PROJ-12F/T2:根级旧 .cmd 是"先备份再迁走",不是静默删除。
		if (!m_BuildEntryResult.Migrated.empty())
		{
			Wui::Label(ctx, { labelX, cursorY }, Wui::TrFormat("modal.build_entry.migrated",
				"Legacy .cmd launchers moved into .we/ (backed up as legacy-*.bak): {files}",
				{ { "files", join(m_BuildEntryResult.Migrated) } }),
				m_Theme.TextMuted, 13.0f);
			cursorY += 20.0f;
		}
		if (!m_BuildEntryResult.Refreshed.empty())
		{
			Wui::Label(ctx, { labelX, cursorY }, Wui::TrFormat("modal.build_entry.refreshed",
				"Launchers/entry files written or refreshed: {files}",
				{ { "files", join(m_BuildEntryResult.Refreshed) } }), m_Theme.TextMuted, 13.0f);
			cursorY += 20.0f;
		}
		if (!m_BuildEntryResult.Removed.empty())
		{
			Wui::Label(ctx, { labelX, cursorY }, Wui::TrFormat("modal.build_entry.removed",
				"Cleaned up (generated files no longer needed): {files}",
				{ { "files", join(m_BuildEntryResult.Removed) } }), m_Theme.TextMuted, 13.0f);
			cursorY += 20.0f;
		}
		if (!m_BuildEntryResult.Skipped.empty())
		{
			Wui::Label(ctx, { labelX, cursorY }, Wui::TrFormat("modal.build_entry.skipped",
				"Already up to date (not rewritten): {files}",
				{ { "files", join(m_BuildEntryResult.Skipped) } }), m_Theme.TextMuted, 13.0f);
			cursorY += 20.0f;
		}
		for (const std::string& warning : m_BuildEntryResult.Warnings)
		{
			Wui::Label(ctx, { labelX, cursorY }, Wui::TrFormat("modal.build_entry.warning",
				"Warning: {text}", { { "text", warning } }), m_Theme.Accent, 12.0f);
			cursorY += 18.0f;
		}

		Wui::Label(ctx, { labelX, cursorY }, Wui::Tr("modal.build_entry.layout",
			"Layout: project root = 2 exe launchers + build.cmd + CMakePresets.json; .vs/ = Visual "
			"Studio launch item + current-item setting (launch.vs.json → ProjectRun.exe, "
			"ProjectSettings.json); .we/ = engine root (+ .cmd fallback)."),
			m_Theme.TextMuted, 12.0f);
		cursorY += 18.0f;
		Wui::Label(ctx, { labelX, cursorY }, Wui::Tr("modal.build_entry.next_steps", "Next steps"),
			m_Theme.Text, 13.0f);
		cursorY += 20.0f;
		Wui::Label(ctx, { labelX, cursorY }, Wui::Tr("modal.build_entry.step_build",
			"1) In the project root, run: build.cmd"), m_Theme.TextMuted, 12.0f);
		cursorY += 18.0f;
		Wui::Label(ctx, { labelX, cursorY }, Wui::TrFormat("modal.build_entry.step_launch",
			"2) Double-click {name}-Play.exe to play (or Run ▶ Launch Project); in Visual Studio pick "
			"ProjectRun.exe (it builds Game, then opens the editor; add `--play` to run the game).",
			{ { "name", projectName } }), m_Theme.TextMuted, 12.0f);

		const Wui::ModalButtonDesc buttons[3] = {
			{ Wui::Tr("modal.build_entry.copy", "Copy instructions"),
				Wui::HashId("modal.build_entry.copy"), true },
			{ Wui::Tr("modal.build_entry.explorer", "Open project folder"),
				Wui::HashId("modal.build_entry.explorer"), true },
			{ Wui::Tr("modal.build_entry.close", "Close"),
				Wui::HashId("modal.build_entry.close"), true },
		};
		const int clicked = Wui::ModalButtons(ctx, frame, buttons, 3, m_Theme);
		if (clicked == 0)
		{
			if (ctx.SetClipboard)
				ctx.SetClipboard(guidance);
			PushNotice(Wui::Tr("notice.build_entry.copied", "Build instructions copied to the clipboard"));
		}
		else if (clicked == 1)
		{
			const HINSTANCE shellResult = ShellExecuteW(nullptr, L"open",
				m_BuildEntryResult.ProjectRoot.wstring().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
			if (reinterpret_cast<INT_PTR>(shellResult) <= 32)
			{
				PushNotice(Wui::TrFormat("notice.build_entry.explorer_failed",
					"Could not open the project folder: {path}", { { "path", projectRoot } }));
				WLD_CORE_WARN("[build-entry] ShellExecuteW failed ({0}) for '{1}'",
					static_cast<long long>(reinterpret_cast<INT_PTR>(shellResult)), projectRoot);
			}
		}
		else if (clicked == 2 || escapePressed)
		{
			m_BuildEntryOpen = false;
			ctx.ClearModal();
		}
		Wui::EndModalFrame(ctx);
	}

	void EditorShell::DrawModals(Wui::WuiContext& ctx)
	{
		// ---- P4-U13e:prefab 窗口的未保存改动(关窗 / 进文档会话)----
		DrawPrefabUnsavedModal(ctx);

		// ---- D10-10:导入位置(窗口级模态) ----
		// 放在这里(其余模态之前)是故意的:导入失败时 EditorLayer 会弹它自己的 Error 模态,
		// 那份错误框必须画在选择器**之上**才看得见(与 D10-9 面板内选择器时期的行为一致);
		// 选择器保持打开并把失败原因写进 import.dest.status。
		RenderImportDestinationModal(ctx);

		// ---- CPPT-6-ED-NEWSCRIPT:新建 C++ 组件(窗口级模态) ----
		DrawNewCppScriptModal(ctx);

		// ---- PROJ-1/T1:新建项目(窗口级模态;成功态换成两个动作按钮) ----
		DrawNewProjectModal(ctx);

		// ---- PLUG-AUTH-1:新建插件(窗口级模态;成功态换成动作按钮) ----
		DrawNewPluginModal(ctx);

		// ---- PROJ-11/T1:生成项目构建入口的结果(动作已在菜单回调里执行完) ----
		DrawBuildEntryModal(ctx);

		// ---- PROJ-2/T1:项目启动器(启动态窗口级模态;其它模态在前时让位) ----
		DrawProjectLauncherModal(ctx);

		const Wui::WuiId unsaved = Wui::HashId("modal.unsaved");
		if (m_Editor.ShowUnsavedModal()) ctx.SetModal(unsaved);
		else if (ctx.Modal() == unsaved) ctx.ClearModal();
		{
			Wui::WuiRect panel;
			bool escapePressed = false;
			Wui::ModalFrameDesc frameDesc;
			frameDesc.Id = unsaved;
			frameDesc.Title = "Unsaved Changes";
			frameDesc.Size = { 460.0f, 150.0f };
			if (Wui::BeginModalFrame(ctx, frameDesc, &panel, &escapePressed, m_Theme))
			{
				Label(ctx, { panel.X + 16, panel.Y + 48 }, "The current scene has unsaved changes.", m_Theme.Text, 14.0f);
				// 三按钮:Save / Don't Save / Cancel(Esc = Cancel),文案与顺序逐字保留。
				const Wui::ModalButtonDesc buttons[3] = {
					{ "Save", Wui::HashId("modal.unsaved.save"), true },
					{ "Don't Save", Wui::HashId("modal.unsaved.nosave"), true },
					{ "Cancel", Wui::HashId("modal.unsaved.cancel"), true },
				};
				const int clicked = Wui::ModalButtons(ctx, panel, buttons, 3, m_Theme);
				if (clicked == 0)
				{
					m_Editor.ResolveUnsavedModal(true);
					ctx.ClearModal();
				}
				else if (clicked == 1)
				{
					m_Editor.ResolveUnsavedModal(false);
					ctx.ClearModal();
				}
				else if (clicked == 2 || escapePressed)
				{
					m_Editor.CancelUnsavedModal();
					ctx.ClearModal();
				}
				Wui::EndModalFrame(ctx);
			}
		}

		const Wui::WuiId error = Wui::HashId("modal.error");
		if (m_Editor.ShowErrorModal()) ctx.SetModal(error);
		else if (ctx.Modal() == error) ctx.ClearModal();
		{
			Wui::WuiRect panel;
			bool escapePressed = false;
			Wui::ModalFrameDesc frameDesc;
			frameDesc.Id = error;
			frameDesc.Title = "Error";
			frameDesc.Size = { 460.0f, 150.0f };
			if (Wui::BeginModalFrame(ctx, frameDesc, &panel, &escapePressed, m_Theme))
			{
				Label(ctx, { panel.X + 16, panel.Y + 48 }, m_Editor.ErrorText(), m_Theme.Text, 14.0f);
				const Wui::ModalButtonDesc buttons[1] = {
					{ "OK", Wui::HashId("modal.error.ok"), true },
				};
				const int clicked = Wui::ModalButtons(ctx, panel, buttons, 1, m_Theme);
				if (clicked == 0 || escapePressed)   // Esc = OK
				{
					m_Editor.ShowErrorModal() = false;
					m_Editor.ErrorText().clear();
					ctx.ClearModal();
				}
				Wui::EndModalFrame(ctx);
			}
		}

		const Wui::WuiId cooking = Wui::HashId("modal.cooking");
		if (m_Editor.ShowCookingProgress()) ctx.SetModal(cooking);
		else if (ctx.Modal() == cooking) ctx.ClearModal();
		{
			Wui::WuiRect panel;
			bool escapePressed = false;
			Wui::ModalFrameDesc frameDesc;
			frameDesc.Id = cooking;
			frameDesc.Title = "Packaging";
			frameDesc.Size = { 420.0f, 140.0f };
			if (Wui::BeginModalFrame(ctx, frameDesc, &panel, &escapePressed, m_Theme))
			{
				if (m_Editor.CookingFinished())
				{
					const std::string message = m_Editor.CookingSucceeded() ? "Packaging finished." : "Packaging failed: " + m_Editor.CookingError();
					Label(ctx, { panel.X + 16, panel.Y + 48 }, message, m_Theme.Text, 14.0f);
					const Wui::ModalButtonDesc buttons[1] = {
						{ "OK", Wui::HashId("modal.cooking.ok"), true },
					};
					const int clicked = Wui::ModalButtons(ctx, panel, buttons, 1, m_Theme);
					if (clicked == 0 || escapePressed)   // 完成态才关(Esc = OK)
					{
						m_Editor.ShowCookingProgress() = false;
						ctx.ClearModal();
					}
				}
				else
				{
					Label(ctx, { panel.X + 16, panel.Y + 52 }, "Packaging...", m_Theme.Text, 14.0f);
					// 进度中不允许 Esc:不消费 escapePressed。
				}
				Wui::EndModalFrame(ctx);
			}
		}

		// P4-UX11:旧的"项目设置"模态框已删除 —— 项目设置统一进独立窗口的 Settings 面板
		// (File ▸ Project Settings 改为打开该面板),避免两套界面互相打架。
	}

	// ---- U25-M2:跨窗口资产拖放桥(实现;契约见 Panels/EditorPanel.h)----
	//
	// 核心 WUI 的拖拽态(按下/负载/释放)是**每个窗口一份**,而内容浏览器(主窗口的停靠面板)
	// 与材质编辑器(独立 OS 窗口,或附加到主窗口的标签页)不可能同时渲染 —— 拖拽全程发生在
	// 源窗口,它看不到目标面板的控件矩形。桥的做法:
	//   · 目标面板每帧把自己的"可落点"登记进来(屏幕物理像素);
	//   · 源面板在**释放那一帧**用全局光标命中登记项,投递一次 drop;
	//   · 目标面板渲染时取走(恰好一次);过期未取走则丢弃。
	namespace Editor
	{
		AssetDropBridge& AssetDropBridge::Get()
		{
			static AssetDropBridge bridge;
			return bridge;
		}

		void AssetDropBridge::BeginFrame(uint64_t frame)
		{
			m_Frame = frame;
			// 只老化、不清空:目标窗口(独立 OS 窗口)在本帧的渲染发生在源面板之后,
			// 释放那一帧必须还能看到上一帧登记的落点矩形(见 EditorPanel.h 的契约注释)。
			m_Targets.erase(std::remove_if(m_Targets.begin(), m_Targets.end(),
				[frame](const Target& target)
				{
					return target.Frame + kTargetLifetimeFrames < frame;
				}), m_Targets.end());
			// 投递过的 drop 只给目标几帧时间取走:面板被关掉/不可见时不无限堆积。
			const uint64_t oldest = frame > 4 ? frame - 4 : 0;
			m_Pending.erase(std::remove_if(m_Pending.begin(), m_Pending.end(),
				[oldest](const Drop& drop) { return drop.Frame < oldest; }), m_Pending.end());
		}

		void AssetDropBridge::Register(const Target& target)
		{
			if (target.Owner.empty() || target.W <= 0.0f || target.H <= 0.0f)
				return;
			Target entry = target;
			entry.Frame = m_Frame;
			for (Target& existing : m_Targets)
			{
				if (existing.Owner == target.Owner && existing.Sink == target.Sink
					&& existing.Key == target.Key)
				{
					existing = entry;   // 每帧刷新:矩形会随布局/窗口移动变化
					return;
				}
			}
			m_Targets.push_back(std::move(entry));
		}

		bool AssetDropBridge::DeliverFromScreen(float screenX, float screenY, const std::string& payload)
		{
			for (const Target& target : m_Targets)
			{
				if (!target.PayloadPrefix.empty() && payload.rfind(target.PayloadPrefix, 0) != 0)
					continue;
				// 过期登记不参与命中(面板已隐藏/关闭后不能还吃 drop)。
				if (target.Frame + kTargetLifetimeFrames < m_Frame)
					continue;
				if (screenX < target.X || screenY < target.Y
					|| screenX > target.X + target.W || screenY > target.Y + target.H)
					continue;
				Drop drop;
				drop.Owner = target.Owner;
				drop.Sink = target.Sink;
				drop.Key = target.Key;
				drop.Payload = payload;
				drop.Frame = m_Frame;
				WLD_CORE_INFO("[wui-drop] bridge '{0}' -> panel '{1}' ({2}:{3})", payload, target.Owner,
					target.Sink, target.Key);
				m_Pending.push_back(std::move(drop));
				return true;
			}
			return false;
		}

		bool AssetDropBridge::TakeDrop(const std::string& owner, const std::string& sink,
			const std::string& key, Drop* out)
		{
			for (size_t index = 0; index < m_Pending.size(); ++index)
			{
				const Drop& drop = m_Pending[index];
				if (drop.Owner != owner || drop.Sink != sink)
					continue;
				if (!key.empty() && drop.Key != key)
					continue;
				if (out)
					*out = drop;
				m_Pending.erase(m_Pending.begin() + static_cast<std::ptrdiff_t>(index));
				return true;
			}
			return false;
		}
	}
}

