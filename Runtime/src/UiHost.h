#pragma once

// GameUI(M4)宿主接线:把 `Engine/src/World/UI/` 的 `.wui` 文档 / 实例树 / 绘制接到 Runtime。
//
// 职责(一个小类,让 `RuntimeLayer::OnUiFrame` 只调两三行):
//   ① 加载:环境变量 `WLD_UI_DOC`(绝对路径或内容根相对)→ 内容根 `assets/ui/*.wui`
//      (字典序第一个);都不存在 = 静默关闭(发布路径默认零开销);找到但解析 / 构建失败
//      = 一条可读警告,不崩。
//   ② 每帧:`input.ViewportSize` 组 `UiSurface` → `ComputeUiViewport` → `UiScreen::Layout`
//      → `UiPainter::Paint`,顺序在场景贴图之后、`DrawGameHud` 之前(与现有脚本站位一致)。
//   ③ 无障碍:与加载条件一致地开 `WuiAccessibility`(每帧 `BeginFrame("main", …)` 后节点
//      可被 AI 读到);`WLD_UI_A11Y_DUMP` 是只读验证钩子(第一帧 `EndFrame` 之后写一次树)。
//
// 边界:不改 `Engine/**`;`.wui` 未启用时 `DrawFrame` / `EndFrame` 是纯空操作,
// 宿主行为与引入本类之前逐字节一致。

#include "World/UI/UiScreen.h"
#include "World/WUI/WuiContext.h"

#include <filesystem>
#include <string>

namespace World
{
	class UiHost
	{
	public:
		// 解析并加载游戏 UI 文档。contentRoot 为空时回退 `World::Paths::AssetRoot()`。
		void Initialize(const std::filesystem::path& contentRoot);
		// 关闭无障碍通道并释放本对象持有的 UI 文档(幂等)。
		void Shutdown();

		bool Enabled() const { return m_Enabled; }
		const std::string& DocumentPath() const { return m_DocumentPath; }

		// 在 `ctx.BeginFrame(input)` 之后、`DrawGameHud` 之前调用;未启用时零操作。
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
		bool m_Enabled = false;
		bool m_A11yDumpWritten = false;
		// 绘制期未知类型 / 提示只报一次,避免每帧刷屏。
		bool m_PaintProblemReported = false;
	};
}
