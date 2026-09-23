#pragma once

#include "World/WUI/WuiWidgets.h"

#include <cstdint>
#include <filesystem>
#include <string>

namespace World::Editor
{
	// P4-UX10:启动时如何处理"上次还开着的独立窗口"。
	// 默认 Ask —— 用户 2026-09-20 明确要求"默认设置应该是询问":引擎不知道用户当下
	// 需不需要这些窗口,与其替他决定(全弹出来 / 全丢掉),不如问一次并允许记住选择。
	enum class RestoreWindowsMode : int
	{
		Ask = 0,     // 启动时询问(默认;可勾"记住我的选择"写成下面三种之一)
		Tabs = 1,    // 一律恢复为顶栏标签(零噪声:不弹 OS 窗口、不抢焦点)
		Layout = 2,  // 按上次形态:挂靠过的恢复成 chip,浮窗恢复成窗口但**不抢焦点**
		None = 3,    // 不恢复(并把记录清掉,下次不再问)
	};

	// 编辑器用户偏好(P4-UX1):用户级、不随项目提交 —— 存 `local/editor-prefs.json`。
	// 与 `Game/project.we.yaml`(项目级)严格分家:语言/主题/密度/字号属于"这台机器上的这个人"。
	//
	// 交互口径:任何一次修改立即应用 + 立即落盘(自动保存),不需要"保存"按钮。
	// P4-UX7:全部偏好项都在 `SettingsRegistry` 里注册(Id/类型/生效时机/tooltip),
	// 面板只按注册表分组渲染(Editor/src/WUI/SettingsUi.*),不再手写一行行控件。
	struct EditorPreferencesData
	{
		std::string Language = "en";                    // en / zh-CN(目录见 Editor/assets/localization)
		Wui::WuiThemeMode Theme = Wui::WuiThemeMode::Dark;
		// P4-UX2c:内容缩放(布局 + 控件 + 文字一起)。默认值由 DPI 推定(96 DPI → 1.30,
		// 见 WuiTheme.cpp 的 ScaleFromEnvironment);这里是偏好文件缺失时的兜底值。
		float UiScale = 1.30f;                          // 0.8..1.8
		bool TermHints = true;                          // 非英文界面下显示英文术语对照

		// ---- P4-UX7:编辑器 / 工作流 / 自动化 / 诊断 ----
		float ScriptFontSize = 14.0f;                   // 10..32,脚本编辑器初始字号(用户明确要过)
		bool AssetHotReload = true;                     // 资产/材质热重载;WLD_ASSET_HOTRELOAD 覆盖
		// 启动恢复策略(见 RestoreWindowsMode);`WLD_RESTORE_WINDOWS=ask|tabs|layout|none` 覆盖。
		RestoreWindowsMode RestoreWindows = RestoreWindowsMode::Ask;
		// AI 控制通道端口:0 = 关闭。服务器在启动时创建 → 重启生效
		// (`--ai-control=<port>` 命令行优先级更高,自动化脚本不受偏好影响)。
		int AiControlPort = 0;                          // 0..65535
		// 日志级别:0=Trace 1=Debug 2=Info 3=Warn 4=Error 5=Off(改完立即生效)。
		int LogLevel = 2;
		// 诊断开关(调试用;映射到既有的 WLD_* 环境变量,启动期读取 → 重启生效)。
		bool DiagFrameTiming = false;                   // WLD_FRAME_TIMING
		bool DiagPresentTrace = false;                  // WLD_VK_PRESENT_TRACE
		bool DiagGlTrace = false;                       // WLD_GL_TRACE_DRAW
		bool DiagAssetTrace = false;                    // WLD_ASSET_HOTRELOAD_TRACE
		bool DiagVulkanValidation = true;               // WLD_VULKAN_VALIDATION(默认开)
		// P4-U6:视口范围可视化开关(用户 2026-09-21:「应该在 view 中有个选项可以选是否显示范围;
		// 如果这个选项没开应该只画选中」)。false = 只画选中实体,true = 画全场景。
		bool ViewportLightRangesAll = false;
		bool ViewportColliderOutlinesAll = false;
	};

	class EditorPreferences
	{
	public:
		static EditorPreferences& Get();

		void Load(const std::filesystem::path& path);
		const EditorPreferencesData& Data() const { return m_Data; }
		const std::filesystem::path& Path() const { return m_Path; }
		// 每次修改 +1:面板据此刷新(脚本编辑器同步新字号、设置页刷新"只看已修改")。
		uint64_t Generation() const { return m_Generation; }

		// 修改 = 应用 + 落盘(失败只告警,不影响本次会话)。
		void SetLanguage(const std::string& language);
		void SetThemeMode(Wui::WuiThemeMode mode);
		void SetUiScale(float scale);
		void SetShowTermHints(bool enabled);
		void SetScriptFontSize(float size);
		void SetAssetHotReload(bool enabled);
		void SetRestoreWindows(RestoreWindowsMode mode);
		void SetAiControlPort(int port);
		void SetLogLevel(int level);
		void SetDiagFrameTiming(bool enabled);
		void SetDiagPresentTrace(bool enabled);
		void SetDiagGlTrace(bool enabled);
		void SetDiagAssetTrace(bool enabled);
		void SetDiagVulkanValidation(bool enabled);
		// P4-U6:视口"范围可视化"开关(立即生效,只影响绘制)。
		void SetViewportLightRangesAll(bool enabled);
		void SetViewportColliderOutlinesAll(bool enabled);

		// 把当前值应用到 WUI(主题/字号/语言/术语对照)与日志级别。
		void Apply() const;
		bool Save(std::string* error = nullptr) const;
		// P4-UX7:把全部偏好注册进 SettingsRegistry(幂等;重复调用先清掉再注册)。
		static void RegisterSettings();

	private:
		// 诊断开关 → 环境变量(只在环境变量未显式设置时生效:环境变量 > 偏好文件)。
		void ApplyDiagnosticEnvironment() const;
		// 修改后的统一收尾:代次 +1、落盘(失败只告警)。
		void Commit();

		std::filesystem::path m_Path;
		EditorPreferencesData m_Data;
		uint64_t m_Generation = 0;
	};
}
