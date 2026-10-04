#include "EditorShell_Internal.h"
#include "World/Asset/AssetCatalog.h"

namespace World
{

using namespace EditorShellDetail;

namespace EditorShellDetail
{

		// P4-UX10:状态栏提示的计时(悬停暂停/移出宽限/淡出都要秒级精度)。
double ShellNowSeconds(){
			return std::chrono::duration<double>(
				std::chrono::steady_clock::now().time_since_epoch()).count();
		}


		// PROJ-1/T1:项目名输入框的规范化(两侧空白不进目录名/id)。
std::string TrimProjectNameText(const std::string& text){
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
std::string PluginIdOwnerPrefix(){
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


std::string PluginIdSuffixFromName(const std::string& name){
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


std::string DefaultPluginIdForName(const std::string& name){
			return PluginIdOwnerPrefix() + "." + PluginIdSuffixFromName(name);
		}


		// template.json 的 requires → 用户可读的注册面名(T2b/T3b/T4);其它值原样返回。
std::string PluginRequiresSurface(const std::string& requires){
			if (requires == "t2b")
				return "T2b";
			if (requires == "t3b")
				return "T3b";
			if (requires == "t4")
				return "T4";
			return requires;
		}


		// PLUG-AUTH-1:向无障碍树登记一个"外壳"节点(菜单/模态内部件都挂 panel="shell")。
void RegisterShellAccessNode(Wui::WuiId id, const char* kind, const std::string& label, const std::string& value, const Wui::WuiRect& rect, bool interactive, bool enabled, const std::string& tooltip){
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
std::string AsciiLowerCopy(const std::string& text){
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
bool ContainsCaseInsensitive(const std::string& haystack, const std::string& loweredNeedle){
			if (loweredNeedle.empty())
				return true;
			return AsciiLowerCopy(haystack).find(loweredNeedle) != std::string::npos;
		}


LauncherDeleteVariant ClassifyLauncherDeleteTarget(const std::string& path){
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
Wui::WuiTheme DangerButtonTheme(const Wui::WuiTheme& base){
			Wui::WuiTheme theme = base;
			theme.Accent = base.Danger;
			theme.Text = base.Danger;
			return theme;
		}


const char* ZoneName(Wui::DropZone zone){
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
bool IsCppSourcePath(const std::string& path){
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
void RegisterAttachNode(const std::string& panel, const Wui::WuiRect& rect, const char* state, std::string label, bool interactive){
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

}

EditorShell::EditorShell(EditorLayer& editor, bool launcherMode) : m_Editor(editor), m_LauncherMode(launcherMode), m_LayoutPath(std::string(WLD_LOCAL_DIR) + "wui-layout.json"){
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


EditorShell::~EditorShell()= default;


	// ---- 面板形态(单一事实源,见 EditorShell.h / T03 方案)----

EditorShell::PanelForm EditorShell::FormOf(const std::string& panel) const{
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


bool EditorShell::IsDeclaredPanel(const std::string& panel) const{
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


bool EditorShell::IsPluginPanelId(const std::string& panelId){
		return panelId.rfind(kPluginPanelPrefix, 0) == 0;
	}


	// 独立窗口只能以"独立窗口"存在:存档/撤销里的停靠树记录一律丢弃(不尝试修复)。
	// 同时清掉未声明的历史面板(如已下线的 "windows"),避免它们被保存路径写回停靠树。
void EditorShell::StripIndependentPanelsFromTree(Wui::DockLayout& layout) const{
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
void EditorShell::RestoreDockedPanelsFromFloat(Wui::DockLayout& layout) const{
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


Wui::DockLayout EditorShell::LayoutForSave() const{
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


Wui::WuiRect EditorShell::FloatRectFor(const std::string& panel) const{
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


Gameplay::SaveService* EditorShell::GetSaveService(){
		return m_Editor.GetSaveService();
	}


	// W8:Scripts 面板的三个宿主能力 —— 只做转发,策略全部留在 EditorLayer(路径解析/唯一重载入口)。
bool EditorShell::ScriptsReloadInstance(entt::entity handle, std::string* message){
		return m_Editor.ScriptsReloadInstance(handle, message);
	}


bool EditorShell::ScriptsOpenExternal(const std::string& logicalPath, std::string* message){
		return m_Editor.ScriptsOpenExternal(logicalPath, message);
	}


bool EditorShell::ScriptsCreateFromTemplate(std::string& outLogicalPath, std::string* message){
		// 与 ScriptsReloadInstance / ScriptsOpenExternal 同一口径:**只做转发**,策略(落点/命名/
		// 模板解析)全部留在 EditorLayer。PECS-T11 一度在这里复制了一份实现只为改扩展名 —— 会把
		// “同一件事两处实现”变成新的漂移源,已归并回 EditorLayer。
		return m_Editor.ScriptsCreateFromTemplate(outLogicalPath, message);
	}


bool EditorShell::IsReadOnlyMode() const{
		// Play/Simulate 期间面板只读查看:可选中/显示,但不改场景数据。
		return m_Editor.IsPlaying() || m_Editor.IsSimulating();
	}


bool EditorShell::IsViewportCamera3D() const{
		return m_Editor.IsViewportCamera3D();
	}


void EditorShell::ToggleViewportCamera3D(){
		m_Editor.ToggleViewportCamera3D();
	}


Wui::GizmoCamera EditorShell::GetGizmoCamera() const{
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
					glm::mat4 world = cameraEntity.GetComponent<TransformComponent>().GetLocalMatrix();
					if (cameraEntity.HasComponent<WorldTransformComponent>())
						world = cameraEntity.GetComponent<WorldTransformComponent>().Matrix;
					const Camera& playCamera = scene->GetCameraView(static_cast<entt::entity>(cameraEntity));
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
						glm::mat4 selectedWorld = selected.GetComponent<TransformComponent>().GetLocalMatrix();
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


void EditorShell::ReleaseIndependentWindows(){
		// 退出时先显式销毁独立窗口:它们的 Vulkan 交换链/OS 窗口必须在
		// RHI 设备与主窗口销毁之前释放,否则会在关闭引擎时崩溃。
		for (const std::unique_ptr<FloatWindowHost>& host : m_FloatHosts)
			if (host)
				host->SetHidden(true); // 保持隐藏,直接进入销毁
		m_FloatHosts.clear();
		m_AttachedPanels.clear();
		m_ActiveWindowTag.clear();
	}


void EditorShell::RecreateIndependentWindows(){
		for (const std::unique_ptr<FloatWindowHost>& host : m_FloatHosts)
			if (host && !host->IsHidden())
				host->RecreateWindow();
	}


	// ---- PanelHost ----

void EditorShell::RefreshAssetCatalog(const std::string& logicalPath)
{
	if (logicalPath.empty())
		return;
	if (Ref<Scene> scene = m_Editor.GetActiveScene())
		::World::RefreshAssetCatalog(scene->GetContext(), logicalPath);
}

Ref<Scene> EditorShell::GetActiveScene(){
		return m_Editor.GetActiveScene();
	}


Entity EditorShell::GetSelectedEntity(){
		return m_Editor.GetSelectedEntity();
	}


void EditorShell::SetSelectedEntity(Entity entity){
		m_Editor.SetSelectedEntity(entity);
	}


bool EditorShell::DebugClickHierarchyRow(size_t index){
		const auto it = m_PanelRegistry.find("hierarchy");
		if (it == m_PanelRegistry.end() || !it->second)
			return false;
		auto* panel = dynamic_cast<HierarchyPanel*>(it->second.get());
		return panel && panel->DebugInvokeRowClick(index);
	}


void EditorShell::MarkDocumentDirty(){
		m_Editor.MarkDocumentDirty();
	}


void EditorShell::DuplicateSelectedEntity(){
		m_Editor.DuplicateSelectedEntity();
	}


void EditorShell::OpenScene(const std::filesystem::path& path){
		m_Editor.OpenScene(path);
	}


void EditorShell::OpenPrefabEditor(const std::string& logicalPath){
		// P4-U13e:同一个 prefab 的**资产窗口**里有未保存改动时,先进文档会话会把这些改动丢掉
		// (文档会话从磁盘读)。所以先问一次(丢弃 / 取消),用户确认丢弃后才真的切过去。
		std::string normalized = logicalPath;
		std::replace(normalized.begin(), normalized.end(), '\\', '/');
		if (InterceptPrefabUnsaved(std::string(kPrefabPanelPrefix) + normalized,
			PrefabPendingAction::OpenDocument, normalized))
			return;
		m_Editor.OpenPrefab(logicalPath);
	}


bool EditorShell::InstantiatePrefabAsset(const std::string& logicalPath, std::string* message){
		return m_Editor.InstantiatePrefabAsset(logicalPath, message);
	}


	// P4-U13d:创建预制体 —— 面板与 AI 通道共用 EditorLayer 的那一条内核(写盘/日志/选中/开窗口)。
bool EditorShell::CreatePrefabFromSelection(Entity root, const std::string& logicalPath, bool overwrite, std::string* message){
		return m_Editor.CreatePrefabFromSelection(root, logicalPath, overwrite, message);
	}


	// P4-U13d:内容浏览器选中一个刚创建/刚生成的资产(必要时先导航到它所在的目录)。
bool EditorShell::SelectContentAsset(const std::string& logicalPath, const char* op){
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
bool EditorShell::FocusContentBrowserProjectSources(){
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
bool EditorShell::RequestNewCppScript(){
		if (!m_Ctx || CurrentProjectRoot().empty())
			return false;   // 没有项目:向导弹"先打开项目",面板侧据此显示可读理由
		OpenNewCppScriptModal(*m_Ctx);
		return true;
	}


	// ---- PLUG-T3:插件管理器面板的数据与动作(面板只依赖 shell)----
	// 数据源 = EditorLayer 持有的 PluginManager(T3 里在挂载项目后实例化并 Discover+Load);
	// 面板/命令都走这几条入口,不直接抓 EditorLayer 内部状态。
Plugins::PluginManager* EditorShell::GetPluginManager() const{
		return m_Editor.GetPluginManager();
	}


bool EditorShell::PluginManagerAvailable() const{
		return GetPluginManager() != nullptr;
	}


bool EditorShell::IsPluginDisabled(const std::string& id) const{
		return m_Editor.IsPluginDisabled(id);
	}


bool EditorShell::PluginRestartPending(const std::string& id) const{
		return m_Editor.PluginRestartPending(id);
	}


std::string EditorShell::PluginLoadError(const std::string& id) const{
		return m_Editor.PluginLoadError(id);
	}


bool EditorShell::SetPluginEnabled(const std::string& id, bool enabled, std::string* message){
		return m_Editor.SetPluginEnabled(id, enabled, message);
	}


bool EditorShell::LocatePluginInContentBrowser(const std::string& id, std::string* message){
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
	// PECS-T11:内容浏览器面板实例(shell 的面板注册表;未注册 / 类型不符 = nullptr)。
ContentBrowserPanel* EditorShell::ContentBrowserPanelInstance() const{
		const auto found = m_PanelRegistry.find("content_browser");
		if (found == m_PanelRegistry.end())
			return nullptr;
		return dynamic_cast<ContentBrowserPanel*>(found->second.get());
	}


bool EditorShell::RevealPathInContentBrowserPanel(ContentBrowserPanel::RootScope scope, const std::filesystem::path& absolutePath, std::string* message){
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
bool EditorShell::AssignMaterialToSelection(const std::string& logicalPath, Entity* outEntity, std::string* outPreviousPath, std::string* message){
		return m_Editor.AssignMaterialToSelection(logicalPath, outEntity, outPreviousPath, message);
	}


bool EditorShell::SetEntityMaterialPath(Entity entity, const std::string& materialPath, std::string* message){
		return m_Editor.AssignMaterialToEntity(entity, materialPath, message, nullptr);
	}


	// "新建材质"向导住在内容浏览器面板(它本来就有模态与目录/落点控件):
	// Window 菜单与材质面板的 Extract from Selection 都走这一条 —— 单点实现,不复制向导。
bool EditorShell::OpenNewMaterialWizard(bool fromSelection, std::string* message){
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
bool EditorShell::PanelWindowScreenOrigin(const Wui::WuiContext& ctx, float* outX, float* outY){
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


bool EditorShell::PrefabInstanceInfo(Entity entity, std::string* sourcePath, size_t* overrideCount, Entity* root){
		return m_Editor.PrefabInstanceInfo(entity, sourcePath, overrideCount, root);
	}


bool EditorShell::PrefabInstanceRevert(Entity root, std::string* message){
		return m_Editor.PrefabInstanceRevert(root, message);
	}


bool EditorShell::PrefabInstanceApply(Entity root, std::string* message){
		return m_Editor.PrefabInstanceApply(root, message);
	}


bool EditorShell::PrefabInstanceUnpack(Entity root, std::string* message){
		return m_Editor.PrefabInstanceUnpack(root, message);
	}


bool EditorShell::IsEditingPrefabDocument() const{
		return m_Editor.IsEditingPrefab();
	}


bool EditorShell::SavePrefabDocument(std::string* message){
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


bool EditorShell::ClosePrefabDocument(){
		if (!m_Editor.IsEditingPrefab())
			return false;
		m_Editor.ClosePrefab();
		return true;
	}


	// P4-U13:prefab 编辑横幅 —— 常驻一条,把"你正在改的是资产"写在最显眼处,
	// 保存与返回各一个按钮(破坏性/离开语义一眼可见)。
void EditorShell::DrawPrefabBar(Wui::WuiContext& ctx, float y){
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

bool EditorShell::HasRenderedScene() const{
		return m_Editor.HasRenderedScene();
	}


Ref<SceneRenderer>& EditorShell::GetSceneRenderer(){
		return m_Editor.GetSceneRenderer();
	}


void EditorShell::SetViewportState(bool focused, bool hovered, glm::vec2 size, glm::vec2 bounds[2]){
		m_Editor.SetViewportState(focused, hovered, size, bounds);
	}


void EditorShell::SetViewportRect(const Wui::WuiRect& rect){
		m_ViewportRect = rect;
	}


bool EditorShell::IsPlaying() const{ return m_Editor.IsPlaying(); }

bool EditorShell::IsSimulating() const{ return m_Editor.IsSimulating(); }

bool EditorShell::IsPaused() const{ return m_Editor.IsPaused(); }

void EditorShell::TogglePlay(){ m_Editor.TogglePlay(); }

void EditorShell::ToggleSimulate(){ m_Editor.ToggleSimulate(); }

void EditorShell::TogglePause(){ m_Editor.TogglePause(); }

uint64_t EditorShell::GetIconId(int index) const{
		return m_Editor.GetIconId(index);
	}


uint32_t EditorShell::TextureEpoch() const{
		return m_Editor.TextureEpoch();
	}


uint64_t EditorShell::GetSceneTextureId() const{
		return m_Editor.GetSceneTextureId();
	}


uint64_t EditorShell::GetCameraPreviewTextureId() const{
		return m_Editor.GetCameraPreviewTextureId();
	}


bool EditorShell::IsCameraPreviewEnabled() const{
		return m_Editor.IsCameraPreviewEnabled();
	}


void EditorShell::ToggleCameraPreview(){
		m_Editor.ToggleCameraPreview();
	}


std::string EditorShell::CameraPreviewLabel() const{
		return m_Editor.CameraPreviewLabel();
	}


Entity EditorShell::PickEntityAt(glm::vec2 viewportLocal){
		return m_Editor.PickEntityAt(viewportLocal);
	}


EditorCamera& EditorShell::GetEditorCamera(){
		return m_Editor.GetEditorCamera();
	}


Wui::GizmoOperation EditorShell::GetGizmoOperation() const{
		return m_Editor.GetGizmoOperation();
	}


std::string EditorShell::PanelTitle(const std::string& id) const{
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


void EditorShell::SaveLayout(){
		// PROJ-3/T1:启动器模式不碰用户的布局存档(既不读也不写,见构造函数)。
		if (m_LauncherMode)
			return;
		std::string error;
		// 落盘前按形态声明规范化:独立窗口不进停靠树;停靠面板不持久化"临时拖出"。
		if (!Wui::WuiLayoutStore::Save(m_LayoutPath, LayoutForSave(), &error))
			WLD_CORE_WARN("Failed to save WUI layout: {0}", error);
	}


void EditorShell::RestoreLayout(const std::string& json){
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


void EditorShell::RecordDockChange(Wui::WuiContext& ctx, const std::string& action, const std::string& target, const std::string& before){
		const std::string after = m_Layout.Serialize();
		ctx.RecordOp("dock", action, target, "");
		ctx.History().Push("Dock " + action + " " + target,
			[this, before] { RestoreLayout(before); },
			[this, after] { RestoreLayout(after); });
		SaveLayout();
	}

}
