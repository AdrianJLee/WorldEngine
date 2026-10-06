#pragma once

#include "WUI/Common/EditorPanel.h"

#include "World/UI/UiDocument.h"
#include "World/UI/UiScreen.h"

#include "World/WUI/Widgets/WuiChrome.h"
#include "World/WUI/WuiUndoStack.h"

#include <filesystem>
#include <cstdint>
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
	// M12(设计器工业化):选中节点画 8 个拖拽手柄(拖角改 Anchor.Size 两轴、拖边改单轴;
	// Min!=Max 的拉伸节点 Size 是"尺寸增量");Ctrl+Z / Ctrl+Y 走**面板本地** WuiUndoStack
	// (一次拖动 = 一条记录);大纲树支持同父内上移/下移(改 Children 顺序 = 改绘制顺序,
	// 保存后文档顺序真的变了)。
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

		// ---- M12:撤销/重做 / 手柄缩放 / 层级 ----
		void HandleShortcuts(Wui::WuiContext& ctx);
		// M19:方向键微调选中节点(Shift = ×10);Alt 关闭吸附。空格/中键拖动 = 平移画布。
		void NudgeSelected(int axis, float amount);
		// M19:画布适配比(不含用户缩放)—— LayoutCanvas 与"以光标为中心缩放"共用。
		float CanvasFitScale(const Wui::WuiRect& rect) const;
		// M19:把 Offset/Size 吸附到整数设计单位(Alt 或极小缩放时跳过)。
		glm::vec2 SnapDesign(glm::vec2 value) const;
		// M19:对齐参考线 —— 选中框的边/中心与父框(或视口内容矩形)对齐时点亮。
		void DrawAlignmentGuides(Wui::WuiContext& ctx, const Wui::WuiTheme& theme,
			const Wui::WuiRect& box);
		void UndoDocument();
		void RedoDocument();
		void PushDocumentUndo(const std::string& name, const World::UI::UiDocument& before);
		void RestoreDocument(const World::UI::UiDocument& document);
		void NoteNodeEdit(const std::string& name);
		void CommitNodeEdit();
		void CancelNodeEdit();
		void CancelDrag();
		void MoveSelectedNode(int direction);
		bool CanMoveSelected(int direction) const;
		bool SelectedNodeBox(Wui::WuiRect& out) const;
		void DrawResizeHandles(Wui::WuiContext& ctx, const Wui::WuiTheme& theme,
			const Wui::WuiRect& box);

		// ---- M16:空态文档浏览 / 节点增删复制 ----
		// 没有文档时画布空态列出内容根里扫到的 `.wui`(点一条即 LoadFrom);有文档时工具栏
		// 提供 New / Add(类型来自 UI::UiNodeRegistry::All())/ Delete / Duplicate。
		void ScanRecentDocuments();
		void RenderDocumentBrowser(Wui::WuiContext& ctx, const Wui::WuiRect& rect, PanelHost& host);
		bool CreateNewDocument();
		bool AddNode(const std::string& type);
		void DeleteSelectedNode();
		void DuplicateSelectedNode();
		bool CanDeleteSelected() const;
		bool CanDuplicateSelected() const;

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

		// ---- M12:撤销/重做(面板本地栈;不往 WuiContext / 引擎加全局状态)----
		// 一条记录 = 一份"整档前像";Undo 恢复前像,Redo 恢复记录时的后像。
		Wui::WuiUndoStack m_Undo;
		// 上一帧结束时是否有文本控件持焦点(与 EditorShell 的 W9-2 判定同口径):
		// 文本焦点在时不抢 Ctrl+Z/Y,让给文本编辑。
		bool m_TextFocusLatched = false;

		// 属性行编辑合并:首次改动抓节点前像,鼠标抬起(或文本提交)时落成一条记录。
		bool m_PendingEditValid = false;
		std::string m_PendingEditNodeId;
		std::string m_PendingEditName;
		World::UI::UiNode m_PendingEditBefore;
		World::UI::UiNode m_FrameNodeBefore;   // 属性页每帧起点快照(仅在无待提交编辑时维护)

		// 画布拖动:Move = 改 Anchor.Offset;Resize = 改 Anchor.Size(拖 8 手柄之一)。
		enum class CanvasDrag { None, Move, Resize };
		CanvasDrag m_Drag = CanvasDrag::None;
		int m_DragHandle = 0;                 // 位掩码(见 UiDesignerPanel.cpp 的 HandleBits)
		int m_HoverHandle = 0;                // 悬停手柄(仅反馈用)
		std::string m_DragNodeId;
		glm::vec2 m_DragStartDesign { 0.0f, 0.0f };
		glm::vec2 m_DragStartOffset { 0.0f, 0.0f };
		glm::vec2 m_DragStartSize { 0.0f, 0.0f };
		// M19:画布缩放/平移(滚轮缩放;中键或空格+左键拖动平移;"Fit" 按钮复位)。
		float m_CanvasZoom = 1.0f;
		glm::vec2 m_CanvasPan { 0.0f, 0.0f };
		bool m_CanvasPanning = false;
		glm::vec2 m_CanvasPanStart { 0.0f, 0.0f };
		glm::vec2 m_CanvasPanMouse { 0.0f, 0.0f };
		World::UI::UiDocument m_DragBefore;   // 拖动起点整档快照(松开落一条撤销)
		bool m_DragBeforeValid = false;

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

		// ---- M16:空态文档浏览 / 节点增删复制 ----
		// 没有文档时列出的候选(按路径排序,上限 50;一次扫描后缓存)。
		struct RecentDocument
		{
			std::string RelativePath;    // 相对内容根(按钮文案用)
			std::filesystem::path Path;  // 绝对路径(LoadFrom 用)
		};
		std::vector<RecentDocument> m_RecentDocuments;
		bool m_RecentScanned = false;
		uint64_t m_RecentScanFrame = 0;
		float m_RecentScroll = 0.0f;
		// Add 下拉的当前项(Wui::Combo 的 selected 由调用方持有;越界时每帧回落到 0)。
		int m_AddTypeIndex = 0;
		// 未保存标记:新建 / 任何文档改动置位,LoadFrom / SaveTo 清除(状态行显示)。
		bool m_Dirty = false;
	};
}
