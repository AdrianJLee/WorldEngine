#pragma once

#include "WUI/Common/EditorPanel.h"

#include "World/UI/UiDocument.h"
#include "World/UI/UiScreen.h"

#include "World/WUI/Widgets/WuiChrome.h"

#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

namespace World
{
	// 任务 20261006-1400-game-ui-framework / 工作包 M5:
	// GameUI 可视化设计器面板(独立窗口形态)。
	//
	// 三栏:
	//   · 左 = 节点大纲树(按文档层级,显示 Type + Id);
	//   · 中 = 画布(设计空间线框视图:节点框 + 类型标签 + 安全区虚线 + 选中高亮 +
	//            锚点标记;拖动选中节点改 Anchor.Offset);
	//   · 右 = 属性(Node / Props / Anchor / Layout,全部复用 Wui::PropertyRow 一族)。
	//
	// 数据流:m_Document 是权威模型 —— UiScreen::Build 复制一份实例树并逐帧 Layout
	// 给出设计空间矩形;编辑只改 m_Document 并置 m_ScreenDirty,下一帧重建实例树;
	// 保存走 Ui::UiDocumentIO::SaveFile(原子写盘)。
	//
	// 边界:引擎运行时已冻结,本面板只消费 Ui* 公共头,不改 Engine/src/**。
	class UiDesignerPanel final : public EditorPanel
	{
	public:
		const char* Id() const override { return "ui_designer"; }
		const char* Title() const override { return "UI Designer"; }
		void OnRender(Wui::WuiContext& ctx, const Wui::WuiRect& rect, PanelHost& host) override;

		// M7a:`.wui` 的"按路径打开"入口(内容浏览器双击 → 本面板)。
		// 面板按 id 单实例注册(宿主拿得到实例、别的面板拿不到),所以这里用一条
		// 文件内待办:登记逻辑路径 → 面板渲染时取走一次并 LoadFrom。
		// 与 Editor::TextureSettingsRequests 同口径 —— 面板间协作走窄通道,不扩 PanelHost 接口。
		// logicalPath 相对内容根(也接受绝对路径);空字符串 = 清除待办。
		static void RequestOpenPath(const std::string& logicalPath);

	private:
		// ---- 文档生命周期 ----
		// 取走并应用一次"按路径打开"待办(没有待办时什么都不做)。
		void ConsumeOpenRequest();
		bool LoadFrom(const std::filesystem::path& path);
		bool SaveTo(const std::filesystem::path& path);
		std::filesystem::path ResolveInputPath(const std::string& text);
		const std::filesystem::path& ContentRoot();
		void RebuildScreen();
		World::UI::UiNode* MutableSelectedNode();
		const World::UI::UiNode* SelectedNode() const;
		bool IsCollapsed(const std::string& nodeId) const;

		// ---- 分段渲染 ----
		void RenderToolbar(Wui::WuiContext& ctx, const Wui::WuiRect& rect, PanelHost& host);
		void RenderOutline(Wui::WuiContext& ctx, const Wui::WuiRect& rect, PanelHost& host);
		void RenderCanvas(Wui::WuiContext& ctx, const Wui::WuiRect& rect, PanelHost& host);
		void RenderProperties(Wui::WuiContext& ctx, const Wui::WuiRect& rect, PanelHost& host);
		void AppendOutlineItems(const std::vector<World::UI::UiNode>& nodes, int depth,
			std::vector<Wui::TreeViewItem>& out);
		float PropertyContentHeight(const World::UI::UiNode* node) const;

		// ---- 画布 ----
		void LayoutCanvas(const Wui::WuiRect& rect);
		void HandleCanvasInput(Wui::WuiContext& ctx, const Wui::WuiRect& rect);
		void DrawCanvas(Wui::WuiContext& ctx, const Wui::WuiRect& rect, PanelHost& host);
		void DrawAnchorMarkers(Wui::WuiContext& ctx, const Wui::WuiTheme& theme,
			const World::UI::UiNodeInstance& node);

		// ---- 模型 ----
		World::UI::UiDocument m_Document;
		World::UI::UiScreen m_Screen;
		World::UI::UiViewport m_Viewport;
		bool m_HasDocument = false;
		bool m_ScreenDirty = false;

		std::filesystem::path m_Path;
		std::string m_PathBuffer;
		std::string m_Status;
		std::string m_SelectedId;
		std::string m_BufferNodeId;

		// 大纲:折叠记录(键 = 节点 Id;默认展开)与行 -> 节点 Id 的映射。
		std::unordered_map<std::string, bool> m_Collapsed;
		std::vector<std::string> m_OutlineIds;
		float m_OutlineScroll = 0.0f;

		// 画布拖动改 Anchor.Offset 的状态。
		bool m_DraggingOffset = false;
		std::string m_DragNodeId;
		glm::vec2 m_DragStartDesign { 0.0f, 0.0f };
		glm::vec2 m_DragStartOffset { 0.0f, 0.0f };

		// 三栏宽度(像素;Splitter 直接改这两个值)与属性列滚动量。
		float m_LeftWidth = 210.0f;
		float m_RightWidth = 320.0f;
		float m_PropertyScroll = 0.0f;

		// 属性分组折叠态(与 PropertyContentHeight 共用同一份事实源)。
		bool m_ShowNodeSection = true;
		bool m_ShowPropsSection = true;
		bool m_ShowAnchorSection = true;
		bool m_ShowLayoutSection = true;

		std::filesystem::path m_ContentRoot;
		bool m_ContentRootResolved = false;
	};
}