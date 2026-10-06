#pragma once

// 游戏 UI 框架(GameUI)— 宿主无关的 `.wui` 运行时接线(工作包 M4,提升到引擎)。
//
// 职责(一个小类,让宿主 UI 帧只调两三行):
//   ① 加载:环境变量 `WLD_UI_DOC`(绝对路径或内容根相对)→ 内容根 `assets/ui/*.wui`
//      (字典序第一个);都不存在 = 静默关闭(发布路径默认零开销);找到但解析 / 构建失败
//      = 一条可读警告,不崩。
//   ② 每帧:组 `UiSurface` → `ComputeUiViewport` → `UiScreen::Layout` → `UiPainter::Paint`。
//      Runtime 口径 = 整窗(`input.ViewportSize`)+ 原点 (0,0);宿主也可用
//      `SetSurface` / `SetOrigin` 把 UI 画进物理面的**子矩形**(编辑器 Play 用它把游戏 UI
//      落进视口面板的场景矩形)。
//   ③ 无障碍:两种接入方式(见 `UiHostAccessibilityMode`)——
//      * `OwnChannel`(默认,Runtime):UiHost 开/关通道,DrawFrame 前 `BeginFrame(窗口 key)`;
//      * `SharedChannel`(编辑器 Play):通道与每帧 `BeginFrame("main")` 由宿主负责,
//        本类只登记节点(退出 Play 时 `Shutdown` 只清本窗口登记,不关通道)。
//      `WLD_UI_A11Y_DUMP` 是只读验证钩子(第一帧 `EndFrame` 之后写一次树;仅 `OwnChannel` 宿主会调)。
//
// 边界:不改 `Engine/**` 之外的东西;`.wui` 未启用时 `DrawFrame` / `EndFrame` 是纯空操作,
// 宿主行为与引入本类之前逐字节一致。

#include "World/Core/Export.h"
#include "World/UI/UiScreen.h"
#include "World/UI/UiTypes.h"
#include "World/UI/UiWorldProjector.h"
#include "World/WUI/WuiContext.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <utility>

#include <glm/glm.hpp>

namespace World
{
	// 谁拥有 `WuiAccessibility` 通道 / 谁负责每帧重置本窗口的节点。
	enum class UiHostAccessibilityMode : uint8_t
	{
		// 默认(Runtime M4 口径):UiHost 拥有无障碍通道 —— Initialize 时 SetEnabled(true),
		// DrawFrame 每帧 BeginFrame(窗口 key) 重置本窗口节点,Shutdown 时 SetEnabled(false)。
		OwnChannel = 0,
		// 共享通道(编辑器 Play/Simulate):通道由宿主(编辑器 AI 控制)管理,编辑器 shell 每帧
		// 已经 `BeginFrame("main", …)`。UiHost 不改开关、不重置窗口,只把游戏 UI 节点登记进
		// 同一份树(window 键仍由 `m_WindowKey` 决定);Shutdown 只 `ClearWindow(窗口 key)`。
		SharedChannel,
	};

	class WLD_API UiHost
	{
	public:
		// 解析并加载游戏 UI 文档。contentRoot 为空时回退 `World::Paths::AssetRoot()`。
		void Initialize(const std::filesystem::path& contentRoot);
		// 停止绘制并释放本对象持有的 UI 文档(幂等)。
		//   OwnChannel  :关闭无障碍通道(SetEnabled(false))。
		//   SharedChannel:只 ClearWindow(窗口 key) 清掉本片登记,不关宿主通道。
		void Shutdown();

		bool Enabled() const { return m_Enabled; }
		const std::string& DocumentPath() const { return m_DocumentPath; }

		// ---- 无障碍接入方式(持久设置,Shutdown 不改;默认 OwnChannel)----
		void SetAccessibilityMode(UiHostAccessibilityMode mode) { m_A11yMode = mode; }
		UiHostAccessibilityMode AccessibilityMode() const { return m_A11yMode; }
		// 无障碍节点归属面板 id;空 = 用文档 `Screen` 名(Runtime 的既有口径)。
		void SetAccessibilityPanel(std::string panelId) { m_PanelId = std::move(panelId); }

		// ---- 绘制位置(默认 = 整面 `input.ViewportSize`、原点 (0,0),即 Runtime 口径)----
		// 显式指定物理面(尺寸 + DPI)。宿主用它把 UI 落进一个子矩形。
		void SetSurface(const UI::UiSurface& surface);
		// 物理面左上角在宿主坐标系中的位置;填进 `UiViewport::PhysicalOrigin`。
		void SetOrigin(glm::vec2 origin);
		// 回到"整面 = input.ViewportSize、原点 (0,0)"的 Runtime 口径。
		void ClearSurface();

		// ---- 世界空间 UI(M8;默认关闭)----
		// 开启后,`Layout` 之后会对带 `World` 锚点的节点调 `ApplyWorldAnchors`:
		// 位置 = 目标世界坐标经 `camera` 投影到屏幕,再按节点 Pivot 摆放。纯屏幕空间的节点不受影响。
		// 相机每帧喂(`SetWorldCamera`),目标位置由宿主经 `SetWorldPositionResolver` 提供。
		void SetWorldSpace(bool enabled) { m_WorldSpaceEnabled = enabled; }
		bool WorldSpaceEnabled() const { return m_WorldSpaceEnabled; }
		void SetWorldCamera(const UI::UiWorldCamera& camera) { m_WorldCamera = camera; }
		void SetWorldPositionResolver(UI::UiWorldPositionResolver resolver)
		{
			m_WorldResolver = std::move(resolver);
		}

		// 在宿主 `ctx.BeginFrame(input)` 之后、宿主覆盖层之前调用;未启用时零操作。
		void DrawFrame(Wui::WuiContext& ctx, const Wui::WuiInputState& input);
		// 在 `ctx.EndFrame()` 之后调用:第一帧落一次无障碍树(`WLD_UI_A11Y_DUMP`);未启用时零操作。
		void EndFrame();

	private:
		std::filesystem::path ResolveDocumentPath() const;
		// candidate 既可为 `.wui` 文件,也可为目录(取其中字典序第一个 `.wui`);无 → 空路径。
		static std::filesystem::path FirstUiDocument(const std::filesystem::path& candidate);

		UI::UiScreen m_Screen;
		std::filesystem::path m_ContentRoot;
		std::string m_DocumentPath;
		std::string m_A11yDumpPath;
		std::string m_WindowKey = "main";
		// 无障碍节点归属面板覆盖;空 = 文档 Screen 名。
		std::string m_PanelId;
		// 显式物理面(编辑器 Play);未设置 = 沿用 input.ViewportSize。
		UI::UiSurface m_Surface;
		glm::vec2 m_Origin { 0.0f, 0.0f };
		bool m_HasSurface = false;
		UiHostAccessibilityMode m_A11yMode = UiHostAccessibilityMode::OwnChannel;
		bool m_WorldSpaceEnabled = false;
		UI::UiWorldCamera m_WorldCamera;
		UI::UiWorldPositionResolver m_WorldResolver;
		bool m_Enabled = false;
		bool m_A11yDumpWritten = false;
		// 绘制期未知类型 / 提示只报一次,避免每帧刷屏。
		bool m_PaintProblemReported = false;
		// 世界空间锚点解析失败只报一次(目标缺失是每帧都会发生的事)。
		bool m_WorldProblemReported = false;
	};
}
