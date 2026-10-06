#include "wldpch.h"
#include "WUI/Shell/EditorShell.h"

#include <cstdio>
#include <fstream>
#include <shellapi.h>
#include "Core/EditorPreferences.h"
#include "App/EditorLayer.h"
#include "Project/ProjectLauncher.h"
#include "Project/ProjectScaffolder.h"
#include "Project/GameProjectSource.h"

#include "World/Core/Application.h"
#include "World/Asset/ProjectManifest.h"
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
#include "WUI/Panels/MaterialEditorPanel.h"
#include "WUI/Panels/ModelPreviewPanel.h"
#include "WUI/Panels/PrefabPanel.h"
#include "WUI/Panels/SettingsPanel.h"
#include "WUI/Panels/PreferencesPanel.h"
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
namespace EditorShellDetail {}   // 前置声明:下面的 using 必须先见到这个名字
using namespace EditorShellDetail;   // 等价于拆分前的文件内匿名命名空间可见性
	namespace EditorShellDetail
	{
		// P4-U13:prefab 编辑横幅的行高(激活时它占一行,停靠区整体下移同样高度)。
		constexpr float kPrefabBarHeight = 26.0f;
double ShellNowSeconds();

std::string TrimProjectNameText(const std::string& text);

std::string PluginIdOwnerPrefix();

std::string PluginIdSuffixFromName(const std::string& name);

std::string DefaultPluginIdForName(const std::string& name);

std::string PluginRequiresSurface(const std::string& requires);

void RegisterShellAccessNode(Wui::WuiId id, const char* kind, const std::string& label, const std::string& value, const Wui::WuiRect& rect, bool interactive, bool enabled, const std::string& tooltip);

std::string AsciiLowerCopy(const std::string& text);

bool ContainsCaseInsensitive(const std::string& haystack, const std::string& loweredNeedle);


		// PROJ-5R/T1(v2):启动器"删除…"一步确认模态的三种形态 —— 由**目标当前状态**决定
		// (绘制期只读探测;口径与 ProjectLauncher::DeleteProjectPermanently 的守卫一致):
		//   Delete  = 目录在 + 含项目清单 ⇒ 提供"永久删除"(危险色);
		//   Missing = 目录已不存在 ⇒ 提供"从列表移除"(只动 local/projects.json,不碰磁盘);
		//   Reject  = 目录在但不是项目(缺 project.we.yaml)⇒ 拒绝打开/删除 + 可读理由,
		//             但仍提供"从列表移除"(只动 local/projects.json,不碰磁盘)。
		enum class LauncherDeleteVariant { Delete, Missing, Reject };
LauncherDeleteVariant ClassifyLauncherDeleteTarget(const std::string& path);

Wui::WuiTheme DangerButtonTheme(const Wui::WuiTheme& base);

const char* ZoneName(Wui::DropZone zone);

bool IsCppSourcePath(const std::string& path);

void RegisterAttachNode(const std::string& panel, const Wui::WuiRect& rect, const char* state, std::string label, bool interactive);


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

			// M3:性能剖析(帧时间曲线 + 分位数 + 掉帧 + 内存概览;数据与 prof.stats 同源)。

			{ "profiler",        EditorShell::PanelForm::Docked, {} },
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

	// ---- CPPT-6-ED-NEWSCRIPT + PECS-T8/T11:File ▸ New C++ …(组件 / 系统 / 空 同一个向导)----
	//
	// 与内容浏览器的新建材质 / 新建着色器向导同一套交互骨架(类型 + 名称 + 实时落点 + 行内错误 +
	// Enter 确认 / Esc 取消),差别有四点:
	//   * 第一步选**类型**(组件默认 / 系统 / 空),落点随类型变:
	//     组件 = `<项目根>/src/Components/<Name>.h`(PROJ-8/T1),系统 = `<项目根>/src/Systems/<Name>.h`,
	//     空 = `<项目根>/src/<Name>.h`(只有 `#pragma once` + 说明注释;不进 schema 发现、不登记);
	//   * 系统类型会把 include + Attach/Detach 登记自动写进 `<项目根>/src/GameProject.cpp`
	//     (GameProjectSource.cpp 的纯文本工具;幂等、认不出结构就一个字节都不写);
	//   * 名称校验除"文件已存在"外,还按纯文本探测"已登记的系统名 / src 下已声明的同名类型"(PECS-T11);
	//   * 创建后**外部 Visual Studio** 打开(CPPT-7:内置脚本编辑器只服务 Lua/Luau)。
	// 组件模板是纯 ECS 组件(纯数据 + WE_FIELD 反射),语法与示例项目模板的
	// `src/Components/SampleDataComponent.h` 一致,随项目构建进 Game.dll;系统模板见
	// GameProjectSource.cpp 的 GameProjectSystemHeaderSource。
	namespace EditorShellDetail
	{
std::string CppScriptBaseName(const std::string& text);

bool IsValidCppIdentifier(const std::string& name);


		// PECS-T8/PECS-T11:「新建 C++ …」向导的类型(Combo 的选项串就是它的 a11y value)。
		constexpr int kCppScriptKindComponent = 0;
		constexpr int kCppScriptKindSystem = 1;
		// PECS-T11(任务 A):第三个类型 = 空 —— 只写 `#pragma once` + 说明注释,
		// 不参与 schema 发现(不在 Components/)、不自动登记进 GameProject.cpp。
		constexpr int kCppScriptKindEmpty = 2;
		const char* const kCppScriptKindComponentTerm = "Component";
		const char* const kCppScriptKindSystemTerm = "System";
		const char* const kCppScriptKindEmptyTerm = "Empty";
const char* CppScriptKindTerm(int kind);

const char* CppScriptKindLabelKey(int kind);

std::string CppScriptRelativePath(int kind, const std::string& name);

std::string CppScriptRelativeDir(int kind);

std::string LuaSystemBaseName(const std::string& text);

bool IsValidLuaScriptFileName(const std::string& name);

std::string NewLuaSystemTemplateSource();


		// ---- PECS-T11(任务 C):「新建 Lua …」向导的三类(与「新建 C++ …」同构)----
		// 落点全部相对**内容根**:
		//   系统   → scripts/systems/<Name>.luau(唯一会被 Play 自动装载的目录)
		//   脚本库 → scripts/lib/<Name>.luau(不自动加载)
		//   空     → scripts/<Name>.luau(不自动加载;只有 systems/ 下的才会被自动执行)
		constexpr int kLuaScriptKindSystem = 0;
		constexpr int kLuaScriptKindLibrary = 1;
		constexpr int kLuaScriptKindEmpty = 2;
		const char* const kLuaScriptKindSystemTerm = "System";
		const char* const kLuaScriptKindLibraryTerm = "Library";
		const char* const kLuaScriptKindEmptyTerm = "Empty";
const char* LuaScriptKindTerm(int kind);

const char* LuaScriptKindLabelKey(int kind);

std::string LuaScriptRelativePath(int kind, const std::string& name);

std::string LuaScriptRelativeDir(int kind);

std::string NewLuaLibraryTemplateSource();

std::string NewLuaEmptyTemplateSource();

std::string NewCppScriptTemplateSource(const std::string& name);

std::string LowerFileExtension(const std::filesystem::path& path);

std::string DeclaredTypeInLine(const std::string& line);

std::vector<std::string> CollectProjectDeclaredTypes(const std::filesystem::path& sourceRoot);

bool TextRegistersSystem(const std::string& text, const std::string& name);

std::string NewCppEmptyTemplateSource(const std::string& name);

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
	}
}
