#pragma once

#include "WUI/Common/EditorPanel.h"
#include "World/Animation/AnimGraph.h"
#include "World/Asset/WModelIO.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace World
{
	// ---------------------------------------------------------------------------
	// M4 AnimGraph 编辑器面板(单例,id = "animgraph")。
	//
	// 职责边界(见 tasks/20261023-1400-animgraph-showcase-and-mannequin/contracts.md §3):
	//   · 资产读写**只**走 AnimGraph::LoadFromFile / SaveToFile —— 面板不含任何 .wanimgraph
	//     文本解析器,格式演进只有引擎侧一处事实源;
	//   · 编辑状态/样本/过渡/遮罩层 = 改动内存中的 AnimGraph,再落盘;
	//   · Live Preview 通过 AnimGraphSystem::SetParameter 驱动选中实体,不直接写
	//     AnimGraphComponent.CurrentStateHash(该字段由运行时回写)。
	//
	// 数据流:OnRender(每帧) —读盘只在 Load 时发生— 控件直接改模型 — Save 落盘。
	// 视图态(选中项/滚动/拖拽/编辑缓冲)全部留在面板成员里,不进 AnimGraph 资产。
	// ---------------------------------------------------------------------------
	class AnimGraphPanel final : public EditorPanel
	{
	public:
		AnimGraphPanel();
		~AnimGraphPanel() override = default;

		const char* Id() const override { return "animgraph"; }
		const char* Title() const override { return "Animation Graph"; }
		void OnRender(Wui::WuiContext& ctx, const Wui::WuiRect& rect, PanelHost& host) override;
		bool OnShortcut(uint32_t keyCode, bool ctrl, bool shift, bool alt) override;

		// 内容浏览器双击 `.wanimgraph` 的窄通道(与 UiDesignerPanel::RequestOpenPath 同款:
		// 面板是单实例,外部拿不到实例指针,只把"要打开哪份资产"排进待办,面板渲染时取走)。
		static void RequestOpenPath(const std::string& logicalPath);

		// 文档接口(工具栏 / 测试/AI 通道共用)。
		void LoadAsset(const std::string& logicalPath);
		bool SaveAsset();
		bool ReloadAsset();
		bool ApplyToSelected(PanelHost& host);
		bool IsDirty() const { return m_Dirty; }
		const std::string& GetLoadedPath() const { return m_LoadedPath; }

	private:
		// ---- 文档 ----
		std::filesystem::path ResolvePhysicalPath(const std::string& logicalPath) const;
		std::string ToLogicalPath(const std::filesystem::path& physical) const;
		void MakeDefaultGraph();
		void ConsumeOpenRequest();
		void MarkDirty();
		void SetStatus(const std::string& text, bool error);

		// 状态编辑 = 复制 → 改 → ReplaceState(契约只给了 const GetStates,
		// 因此没有可变访问时按 RemoveState + AddState 重建,见派工 §3.3)。
		bool ReplaceState(const std::string& originalName, const AnimState& state);
		void AddStateOfKind(AnimStateKind kind);
		void DeleteSelectedState();
		void RenameSelectedState(const std::string& newName);
		void CommitPendingCondition();
		const AnimState* SelectedState() const;

		// ---- 布局 ----
		void RenderToolbar(Wui::WuiContext& ctx, const Wui::WuiRect& bar, PanelHost& host,
			const Wui::WuiTheme& theme);
		void RenderOpenRow(Wui::WuiContext& ctx, const Wui::WuiRect& row, PanelHost& host,
			const Wui::WuiTheme& theme);
		void RenderLeftColumn(Wui::WuiContext& ctx, const Wui::WuiRect& area, PanelHost& host,
			const Wui::WuiTheme& theme);
		void RenderCenterColumn(Wui::WuiContext& ctx, const Wui::WuiRect& area, PanelHost& host,
			const Wui::WuiTheme& theme);
		void RenderLivePanel(Wui::WuiContext& ctx, const Wui::WuiRect& area, PanelHost& host,
			const Wui::WuiTheme& theme);
		void RenderStateCard(Wui::WuiContext& ctx, const Wui::WuiRect& area, PanelHost& host,
			const Wui::WuiTheme& theme);
		void RenderBlendSpaceCard(Wui::WuiContext& ctx, const Wui::WuiRect& area, PanelHost& host,
			const Wui::WuiTheme& theme);
		void RenderSamplesCard(Wui::WuiContext& ctx, const Wui::WuiRect& area, PanelHost& host,
			const Wui::WuiTheme& theme);
		void RenderTransitionsCard(Wui::WuiContext& ctx, const Wui::WuiRect& area, PanelHost& host,
			const Wui::WuiTheme& theme);
		void RenderLayersCard(Wui::WuiContext& ctx, const Wui::WuiRect& area, PanelHost& host,
			const Wui::WuiTheme& theme);
		void RenderLayerCard(Wui::WuiContext& ctx, const Wui::WuiRect& area, PanelHost& host,
			const Wui::WuiTheme& theme);
		void RenderStatusBar(Wui::WuiContext& ctx, const Wui::WuiRect& bar,
			const Wui::WuiTheme& theme);

		// ---- 混合空间画布(AnimGraphPanel_BlendSpace.cpp) ----
		void RebuildTriangulation();
		void DrawBlendCanvas(Wui::WuiContext& ctx, const Wui::WuiRect& canvas, PanelHost& host,
			const Wui::WuiTheme& theme, const AnimState& state, bool blend2d);
		void UpdateBlendDrag(Wui::WuiContext& ctx, const Wui::WuiRect& canvas, const std::string& stateName,
			bool blend2d);

		// ---- Live Preview ----
		void RefreshLiveBinding(PanelHost& host);
		void PushParameters(PanelHost& host);

		// ---- 模型/Clip ----
		void RefreshModel(PanelHost& host);
		std::string ModelLogicalPathForSelected(PanelHost& host) const;
		std::vector<std::string> ClipOptionsForCombo(const std::string& current) const;


	private:
		// 文档
		AnimGraph m_Graph;
		std::string m_LoadedPath;
		// 契约没有 GetInitialState 访问器:载入后 GetCurrentStateName() 就是 initial(LoadFromFile
		// 末尾会 SetInitialState),面板据此跟踪并在改名/删除时同步,避免写出悬空的 initial:。
		std::string m_InitialStateHint;
		bool m_Dirty = false;
		std::string m_Status;
		bool m_StatusError = false;
		float m_StatusAge = 0.0f;
		bool m_OpenRowVisible = false;
		std::string m_OpenPathBuffer;

		// 视图态
		std::string m_SelectedState;
		int m_SelectedSample = -1;
		std::string m_SelectedLayer;
		int m_SelectedTransition = -1;
		float m_StateScroll = 0.0f;
		float m_LayerScroll = 0.0f;
		float m_CenterScroll = 0.0f;
		Wui::WuiRect m_CanvasRect { 0, 0, 0, 0 };
		int m_HoverSample = -1;
		int m_DragSample = -1;
		bool m_DragChanged = false;
		std::string m_DeleteArmState;
		float m_DeleteArmAge = 0.0f;

		// 编辑缓冲(TextField 必须拿到跨帧稳定的引用)
		std::string m_NameBuffer;
		std::string m_NameBufferKey;
		std::string m_ConditionBuffer;
		std::string m_ConditionKey;
		bool m_NameFieldWasFocused = false;
		std::string m_ConditionFocusKey;
		// 上一帧字段矩形:本帧点在其外 = 提交(先于工具栏处理,Save/Apply 不会丢输入)。
		Wui::WuiRect m_NameFieldRect { 0.0f, 0.0f, 0.0f, 0.0f };
		Wui::WuiRect m_ConditionFieldRect { 0.0f, 0.0f, 0.0f, 0.0f };

		// 新建状态下拉(菜单目标跨帧保留;位置在打开那一帧钉住)
		bool m_NewStateMenuOpen = false;
		glm::vec2 m_NewStateMenuPos { 0.0f, 0.0f };

		// 三角剖分缓存(拖动结束才重算,见派工 §3.2)
		std::vector<std::array<int, 3>> m_Triangles;
		bool m_TrianglesDirty = true;

		// Live Preview
		float m_ParamSpeed = 0.0f;
		float m_ParamDirection = 0.0f;
		float m_ParamFalling = 0.0f;
		float m_ParamAimWeight = 0.0f;
		bool m_ParamsInitialized = false;
		entt::entity m_BoundEntity = entt::null;
		Scene* m_BoundScene = nullptr;
		std::string m_BoundName;

		// 模型/Clip 缓存(选中实体或模型路径变化时才重读)
		std::string m_ModelKey;
		std::string m_ModelError;
		bool m_ModelValid = false;
		Asset::WModelData m_Model;

		// 窄窗口形态(<900 时右栏折到底部)
		bool m_Narrow = false;
	};
}

