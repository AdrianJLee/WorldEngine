#pragma once

#include "EditorPanel.h"

#include <filesystem>
#include <string>
#include <vector>

namespace World
{
	// P2 W8:脚本工作流面板(独立窗口:id "scripts"、标题 "Scripts")。
	//
	// 面板内容:
	//   ① 磁盘脚本:扫 <当前内容根>/scripts(0.5s 节流,排除 intermediate/),按逻辑路径
	//      排序,每行主按钮"在引擎内打开"(W9-2:EditorShell::OpenScriptEditor,默认附加到
	//      主窗口)+ 次按钮 External(系统默认程序打开)+ 顶部 New(从 templates/WorldScript.lua
	//      复制成 scripts/systems/script_<n>.lua,磁盘冲突递增、不覆盖;落在这里才会被自动加载);
	//   ② 项目源码:展示 <项目根>/src/** 源码入口。
	//
	// 第二段"项目源码"(PROJ-8/T1 起;**CPPSRC-1 改为一行入口**):
	//   CPPSRC-1(用户 2026-09-29「之前写的 c++ 脚本在编辑器里的展示要改一下,改成像 asset 资产一样」):
	//   项目 `<项目根>/src/**` 的 `.h/.cpp` 不再在这里列行 —— 展示面搬到内容浏览器的「项目 C++」根
	//   (与资产同一套网格/列表/类型列/搜索,双击走外部 Visual Studio)。本段只保留:
	//   `scripts.project_sources.open_in_browser` 一行入口(点击 → PanelHost::FocusContentBrowserProjectSources)
	//   + 解析后的 `<项目根>/src` 路径 / 空态提示。段头折叠与 File ▸ 项目源码… 的语义保持。
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
