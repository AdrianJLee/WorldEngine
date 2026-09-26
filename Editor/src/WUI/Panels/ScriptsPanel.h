#pragma once

#include "EditorPanel.h"

#include <filesystem>
#include <string>
#include <vector>

namespace World
{
	// P2 W8:脚本工作流面板(独立窗口:id "scripts"、标题 "Scripts")。
	//
	// 两段内容:
	//   ① 场景脚本:当前活动场景里全部 LuauScriptComponent 的 Tag / 脚本路径 / State /
	//      LastError / ReloadDiagnostic + 每行 Reload 按钮(经 PanelHost,最终复用
	//      EditorLayer::ReloadLuauScriptComponent 这条唯一重载入口);
	//   ② 磁盘脚本:扫 WLD_ASSETPATH/scripts(0.5s 节流,排除 intermediate/),按逻辑路径
	//      排序,每行主按钮"在引擎内打开"(W9-2:EditorShell::OpenScriptEditor,默认附加到
	//      主窗口)+ 次按钮 External(系统默认程序打开)+ 顶部 New(从 templates/WorldScript.lua
	//      复制成 scripts/script_<n>.lua,磁盘冲突递增、不覆盖)。
	//
	// 动作控件绘制时登记无障碍节点(scripts.*),AI 通道的 ui.tree / ui.invoke 能驱动它们;
	// 场景枚举一律走 const Scene::GetRegistry(),Play/Simulate 下不触碰结构写禁令。
	class ScriptsPanel final : public EditorPanel
	{
	public:
		const char* Id() const override { return "scripts"; }
		const char* Title() const override { return "Scripts"; }
		void OnRender(Wui::WuiContext& ctx, const Wui::WuiRect& rect, PanelHost& host) override;

	private:
		struct DiskScript
		{
			std::string LogicalPath;         // 相对内容根,如 scripts/tests/UiProbe.lua
			std::filesystem::path DiskPath;  // 绝对/相对内容根的磁盘路径(仅作提示)
		};

		// 0.5s 节流重扫;force = 新建脚本之后立刻刷新(不等节流窗口)。
		void RefreshDiskScripts(bool force);
		void SetStatus(std::string text, bool error);

		std::vector<DiskScript> m_DiskScripts;
		double m_NextScanSeconds = 0.0;
		std::string m_Status;
		bool m_StatusIsError = false;
		// 刚新建出来的脚本逻辑路径:该磁盘行高亮,提示"新文件在这里"。
		std::string m_SelectedDisk;
	};
}
