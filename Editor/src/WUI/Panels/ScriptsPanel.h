#pragma once

#include "EditorPanel.h"

#include <filesystem>
#include <string>
#include <vector>

namespace World
{
	// P2 W8:脚本工作流面板(独立窗口:id "scripts"、标题 "Scripts")。
	//
	// 三段内容:
	//   ① 场景脚本:当前活动场景里**两个脚本组件**(LuauScriptComponent + CppScriptComponent,
	//      CPPT-3 起)的 Tag / 语言 / 脚本路径(或 C++ 类型名)/ State / LastError / ReloadDiagnostic;
	//      Luau 行带 Reload 按钮(经 PanelHost,最终复用 EditorLayer::ReloadLuauScriptComponent
	//      这条唯一重载入口);C++ 行提示走模块级 File ▸ Reload C++ Module(实例重载不支持);
	//   ② 磁盘脚本:扫 <当前内容根>/scripts(0.5s 节流,排除 intermediate/),按逻辑路径
	//      排序,每行主按钮"在引擎内打开"(W9-2:EditorShell::OpenScriptEditor,默认附加到
	//      主窗口)+ 次按钮 External(系统默认程序打开)+ 顶部 New(从 templates/WorldScript.lua
	//      复制成 scripts/script_<n>.lua,磁盘冲突递增、不覆盖)。
	//
	// PROJ-8/T1 追加第二段"项目源码"(用户 2026-09-28:项目的 C++ 文件要显示在该项目的编辑器里):
	//   ② 项目源码:当前项目 `<项目根>/src/**` 的 `.h/.cpp`(递归,跳过 build/ 与隐藏目录),
	//      每行显示**相对项目根**的路径,主按钮 Open in VS 走外部 Visual Studio
	//      (PanelHost::OpenScriptEditor → EditorShell 按扩展名分流;双击行同一条路径)。
	//      没有当前项目(启动器/未打开项目)时整段显示"先打开或新建项目"的可读提示。
	//
	// 动作控件绘制时登记无障碍节点(scripts.*),AI 通道的 ui.tree / ui.invoke 能驱动它们;
	// 场景枚举一律走 const Scene::GetRegistry(),Play/Simulate 下不触碰结构写禁令。
	class ScriptsPanel final : public EditorPanel
	{
	public:
		const char* Id() const override { return "scripts"; }
		const char* Title() const override { return "Scripts"; }
		void OnRender(Wui::WuiContext& ctx, const Wui::WuiRect& rect, PanelHost& host) override;

		// PROJ-11/T1:File ▸ 项目源码… 的落点 —— 下一次渲染时展开/高亮"项目源码"段
		// (面板本体由 EditorShell 持有;实例方法保证即使面板当前没渲染也先记下请求)。
		void FocusProjectSources();

	private:
		struct DiskScript
		{
			std::string LogicalPath;         // 相对内容根,如 scripts/tests/UiProbe.lua
			std::filesystem::path DiskPath;  // 绝对/相对内容根的磁盘路径(仅作提示)
		};

		// 0.5s 节流重扫;force = 新建脚本之后立刻刷新(不等节流窗口)。
		void RefreshDiskScripts(bool force);
		// PROJ-8/T1:0.5s 节流重扫当前项目的 `<项目根>/src/**`(`.h/.cpp`);
		// force = 面板刚打开 / 刚新建脚本时立刻刷新(不等节流窗口)。
		void RefreshProjectSources(bool force);
		void SetStatus(std::string text, bool error);

		struct ProjectSource
		{
			std::string RelativePath;        // 相对项目根,如 src/Scripts/MyScript.h
			std::filesystem::path DiskPath;  // 绝对路径(Open in VS / a11y value)
		};

		std::vector<DiskScript> m_DiskScripts;
		double m_NextScanSeconds = 0.0;
		std::vector<ProjectSource> m_ProjectSources;
		double m_NextProjectScanSeconds = 0.0;
		// 当前是否打开着项目(项目根里有 project.we.yaml);启动器/未打开项目 = false,
		// 整页显示"先打开或新建项目"的可读提示。
		bool m_HasProject = false;
		std::string m_Status;
		bool m_StatusIsError = false;
		// 刚新建出来的脚本逻辑路径:该磁盘行高亮,提示"新文件在这里"。
		std::string m_SelectedDisk;
		// PROJ-11/T1:项目源码段可折叠(默认展开);File ▸ 项目源码… 会强制展开并高亮 2.5s。
		bool m_ProjectSourcesCollapsed = false;
		bool m_FocusProjectSourcesRequested = false;
		double m_ProjectSourcesHighlightUntil = -1.0;
	};
}
