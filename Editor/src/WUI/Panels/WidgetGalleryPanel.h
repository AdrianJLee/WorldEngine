#pragma once

#include "EditorPanel.h"

#include "World/WUI/WuiComponentRegistry.h"
#include "World/WUI/WuiWidget.h"
#include "World/WUI/Widgets/WuiControls.h"

#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace World
{
	// WUI-P0b:组件验收工作台(gallery 面板)。
	//
	// 与旧"画廊"的区别:这里**不手写控件演示**,而是从 WuiComponentRegistry(唯一事实源)
	// 拉取登记项 —— 左=组件树(按 Category 分组 + 搜索)、中=画布(调 desc.Showcase 画**一件**
	// 真实控件)、右=由 desc.Properties/desc.States 自动生成的属性编辑器,加上密度/缩放/
	// 语言/长文本压力/强制伪状态这组全局开关,底部能 Capture(导出画布裁剪矩形 + a11y 片段)
	// 与 Approve(把"该 id 在 <commit> 通过"记进 build/wui-workbench/approved.json)。
	//
	// 因此"加控件只改登记表,不改工作台":工作台里没有任何一件组件的名字。
	// 每个可交互件都有稳定 a11y id(`wui.workbench.<area>.<name>`),探针按 id 驱动。
	// 全部交互写入操作日志(Operations 面板可查,与旧画廊同一口径)。
	class WidgetGalleryPanel final : public EditorPanel
	{
	public:
		const char* Id() const override { return "gallery"; }
		const char* Title() const override { return "Widget Gallery"; }
		void OnRender(Wui::WuiContext& ctx, const Wui::WuiRect& rect, PanelHost& host) override;

	private:
		// 面板级状态(跨帧保留;控件本身是无状态的立即模式调用)。
		std::string m_Search;                 // 左树过滤
		std::string m_SelectedId;             // 当前选中组件 id(空 = 取登记表第一件)
		std::string m_LastScrolledSelection;  // 已经"滚进视口"过的选中件(只在选中变化时滚,避免抢滚轮)
		std::string m_ForceState = "default"; // 强制伪状态
		std::string m_LastAction = "(none)";
		std::string m_Status;                 // 底部状态行(Capture / Approve 结果)
		float m_TreeScroll = 0.0f;
		float m_PropScroll = 0.0f;
		float m_BodyScroll = 0.0f;            // 窄窗单列时的整体滚动
		bool m_LongText = false;              // 长文本压力
		// 键盘导航:指针在列表里、或最近一次交互(点击行 / 滚轮)落在列表里 → ↑/↓ 切换选中。
		// 指针离开列表后仍保留(键盘优先的操作习惯),在面板别处点击时清掉(见 DrawTree)。
		bool m_TreeKeyboardFocus = false;
		// 本帧是否有工作台自己的弹层开着(状态下拉 / 属性枚举下拉 / 语言下拉)。
		// 弹层开着时 ↑/↓ 归弹层,组件树不抢键 —— 树先画,所以用上一帧的结果(m_PopupOpenPrev)。
		bool m_PopupOpenNow = false;
		bool m_PopupOpenPrev = false;
		int m_DensityIndex = 0;               // 0 = comfortable / 1 = compact
		int m_UiScaleIndex = 1;               // 0 = 100% / 1 = 125% / 2 = 150%
		int m_LanguageIndex = 0;              // 0 = en / 1 = zh-CN
		bool m_ShowGrid = true;

		// ---- 覆盖层组件的"专用舞台"(WUI-P0b-2)----
		// showcase 若往 overlay 层画了**整窗大小**的矩形(模态遮罩),它就不能和面板控件同层:
		// 那条遮挡区会盖住整窗,下一帧把工作台自己的树/属性/按钮全部点不动(实测:选中 modal
		// 之后整块面板失去命中,探针从 scrollarea 起全部报 "selection did not stick")。
		// 检测到这种组件就记下 id,之后每帧走"专用舞台"顺序:先把画布 + showcase 画在浅层
		// (遮罩只盖画布),再把其余面板内容画到更深的 overlay 层(盖掉遮罩的其它部分,
		// 而且不会被它的遮挡区挡住)。检测按"整窗矩形面积"判定,与具体组件名无关。
		std::set<std::string> m_OverlayStageComponents;
		Wui::WuiRect m_PanelRect {};          // 本帧面板矩形(清键盘焦点用)
		bool m_CanvasOverlayStage = false;    // 本帧画布是否走专用舞台(写进捕获元数据)
		// 本帧 showcase 实际往命令流里写了多少条绘制命令(WUI-P1b:探针断言"绘制命令数 > 0"
		// 要求的是真实命令数,不能拿"画布上有非背景像素"当替身)。只统计 showcase 自己那段。
		size_t m_ShowcaseCommands = 0;

		// 画布实测矩形(客户区坐标)+ 该帧实际送进 showcase 的属性(写 Capture 元数据用)。
		Wui::WuiRect m_CanvasRect {};
		Wui::WuiRect m_CanvasInner {};
		Wui::WuiRect m_CanvasSlot {};         // 本帧 showcase 的槽位矩形
		bool m_CanvasValid = false;
		std::string m_AppliedState = "default";
		std::vector<std::pair<std::string, std::string>> m_AppliedProperties;

		// Combo 要求 vector 引用:**只在更新事件那一帧**重建(随后 Clear）。
		std::vector<std::string> m_LanguageOptions;

		// ---- WUI-P1.5b:属性面板(UE 式:分组 + 搜索 + 类型化行 + 状态选择器)----
		//
		// 属性表来自**静态**登记表:行标签/搜索命中串/控件 id/状态键都在换件或属性被外部重置时
		// 一次性重建,此后每帧只做"值是否变了"的比较 —— 不改动时零分配(见任务报告的性能数字)。
		struct PropRow
		{
			size_t PropIndex = 0;          // 登记表下标(行 id 与它绑定:分组/过滤下仍然稳定)
			Wui::WuiId ControlId = 0;      // 行控件的 a11y id(重建时算一次)
			std::string GroupId;           // Content / Style / Layout / Behavior / General
			std::string Label;             // 行标签(属性名)
			std::string Haystack;          // 预小写的"名 + 文档"(搜索用)
			bool StateScoped = false;      // 按状态读写(键 = name,或 name.<state>)
			std::string ExplicitState;     // 属性名里已带状态后缀时:该状态 id(如 hover)
			std::string Key;               // 当前状态对应的键(状态变化时才重算)
			std::string KeyState;          // Key 对应的状态 id(判"要不要重算")
			std::string Value;             // 当前值的文本(用户改值后立即同步)
			// 行控件的值存储:控件直接读写它们,避免每帧解析文本。
			float Number = 0.0f;           // Float
			int64_t Integer = 0;           // Int
			bool Flag = false;             // Bool
			glm::vec4 Color { 1, 1, 1, 1 };  // Color
			float SizeW = 120.0f;          // Size2 宽
			float SizeH = 32.0f;           // Size2 高
			float Ratio = 0.0f;            // Size2 锁定比例时记下的宽/高
			bool RatioLock = false;        // Size2 比例锁
			std::string Text;              // Text
			int EnumIndex = 0;             // Enum
			std::vector<std::string> Options;    // Enum 选项(登记表为空时用单元素兜底)
			Wui::WuiId ChildIds[3] = { 0, 0, 0 };  // Size2:宽/高/比例锁三个子控件 id
			bool SeedValid = false;        // 值存储是否已与 Key 同步过
		};

		struct PropGroup
		{
			std::string Id;                // 规范组名(见 NormalizePropGroup)
			std::string Title;             // 显示名(Tr 只在重建时调一次)
			Wui::WuiId HeaderId = 0;       // 折叠头 a11y id(重建时算一次)
			std::vector<size_t> Rows;      // 行下标(指向 m_PropRows)
			bool Open = true;
			bool StateSelector = false;    // 本组顶部画状态选择器(Style 组)
			int LastVisible = -1;          // 最近一次算出的可见行数(缓存 trailing 文本用)
			std::string Trailing;          // "可见/总数"(只在可见数变化时重算)
		};

		std::vector<PropRow> m_PropRows;
		std::vector<PropGroup> m_PropGroups;
		std::string m_PropCacheComponent;     // 缓存属于哪个组件(空 = 需要重建)
		std::string m_PropCacheLanguage;      // 缓存对应的语言(切语言要重取 i18n 文案)
		std::string m_PropSearchPlaceholder;  // 搜索框占位文案(缓存:逐帧不再构造 std::string)
		std::string m_PropGroupTip;           // 分组折叠头的提示文案(同上)
		// 元信息行(登记表说明 + 全局开关摘要):文本与"已裁剪"的结果都缓存,
		// 只在组件/语言/密度/缩放/宽度变化时重建 —— 逐帧零分配。
		std::vector<std::string> m_PropMetaLines;
		std::string m_PropMetaComponent;
		std::string m_PropMetaLanguage;
		float m_PropMetaDensity = -1.0f;
		float m_PropMetaScale = -1.0f;
		float m_PropMetaWidth = -1.0f;
		int m_PropStateHome = -1;             // 状态选择器的落点:<0 = 面板顶部,否则组下标
		std::string m_PropSearch;             // 属性搜索输入(用户/脚本编辑)
		std::string m_PropSearchLower;        // 预小写缓存(只在输入变化时重算)
		std::string m_PropSearchSource;       // 上次小写化时的原始输入
		std::vector<std::string> m_PropStateIds;     // 状态选择器:状态 id(登记表顺序)
		std::vector<std::string> m_PropStateChips;   // 状态选择器:按钮文案
		std::vector<float> m_PropStateChipWidths;    // 状态选择器:按钮宽(重建时算一次)
		std::vector<Wui::WuiId> m_PropStateChipIds;  // 状态选择器:按钮 id

		// 属性值缓存:组件 id → (属性名 → 文本值)。数值/枚举/文本控件都读写这里。
		// 键不一定是属性名:按状态读写的属性用 `bg.hover` 这类"名 + 状态"(P1.5b)。
		// **不要**把枚举下标塞进 ctx.Persist:同一个 id 还被 TextField 等控件的内部
		// 焦点/编辑态使用,类型不一致会触发 "persisted state id reused with different types"
		// (实测该告警随后就是浮窗渲染失败:string too long)。枚举下标现存在 PropRow::EnumIndex,
		// 控件自己的持久态归控件。
		std::map<std::string, std::map<std::string, std::string>> m_PropertyValues;

		// ---- 绘制helper(全部只写 ctx,不碰任何组件内部状态)----
		struct WbLayout
		{
			Wui::WuiRect TopBar {};
			Wui::WuiRect InfoBar {};
			Wui::WuiRect Tree {};
			Wui::WuiRect Canvas {};
			Wui::WuiRect Props {};
			Wui::WuiRect Actions {};
			Wui::WuiRect Body {};
			bool Stacked = false;
		};

		WbLayout ComputeLayout(const Wui::WuiRect& rect, const Wui::WuiTheme& theme);
		// uiScale:面板内部字号乘法因子(WUI-P1c-b W2);与 density 分开,不混用。
		void DrawTopBar(Wui::WuiContext& ctx, const WbLayout& layout, const Wui::WuiTheme& theme,
			float uiScale);
		void DrawInfoBar(Wui::WuiContext& ctx, const WbLayout& layout, const Wui::WuiTheme& theme,
			const Wui::WuiComponentDesc* desc, float uiScale);
		// 左树过滤(搜索框)后的组件表 —— 树绘制与"当前选中"解析共用同一顺序。
		std::vector<const Wui::WuiComponentDesc*> FilteredComponents() const;
		const Wui::WuiComponentDesc* ResolveSelection(
			const std::vector<const Wui::WuiComponentDesc*>& visible) const;
		// 切换选中件(鼠标点行 / 键盘 ↑↓ 同一条路径):清强制状态与属性覆盖、页面回到顶部。
		void SelectComponent(Wui::WuiContext& ctx, const Wui::WuiComponentDesc& desc,
			const char* how);
		// 返回本帧选中的登记项(可能来自左树的点击)。
		const Wui::WuiComponentDesc* DrawTree(Wui::WuiContext& ctx, const WbLayout& layout,
			const Wui::WuiTheme& theme, float uiScale);
		// overlayStage = true:画布走"专用舞台"(先画布,其余内容由 OnRender 画到更深一层)。
		void DrawCanvas(Wui::WuiContext& ctx, const WbLayout& layout, const Wui::WuiTheme& theme,
			const Wui::WuiComponentDesc* desc, float density, float uiScale, bool overlayStage);
		// 画布里的 showcase 本体(专用舞台时被 OnRender 延后到内容之后调用)。
		void DrawCanvasShowcase(Wui::WuiContext& ctx, const Wui::WuiTheme& theme,
			const Wui::WuiComponentDesc& desc, float density, float uiScale, bool overlayStage,
			const Wui::WuiRect& clipRect);
		void DrawProperties(Wui::WuiContext& ctx, const WbLayout& layout, const Wui::WuiTheme& theme,
			const Wui::WuiComponentDesc* desc, float density, float uiScale);
		// ---- WUI-P1.5b 属性面板内部件 ----
		// 属性表缓存(分组 / 行 / 状态选择器):换件或属性被外部重置后重建,每帧不再动字符串。
		void RebuildPropertyCache(const Wui::WuiComponentDesc& desc);
		void InvalidatePropertyCache();
		// 状态选择器(一排状态按钮;id 沿用 `wui.workbench.state.<组件>.<状态>`,探针按它驱动态)。
		// 返回本帧选择器占的高度(0 = 没有状态可选)。
		float DrawPropertyStateSelector(Wui::WuiContext& ctx, const Wui::WuiTheme& theme,
			const Wui::WuiRect& rect, const Wui::WuiComponentDesc& desc, float fontScale);
		float StateSelectorHeight(const Wui::WuiRect& rect) const;
		// 一行属性(类型化库件);返回值 = 本帧改了值(调用方据此记操作记录)。
		bool DrawPropertyRow(Wui::WuiContext& ctx, const Wui::WuiTheme& theme,
			const Wui::WuiComponentDesc& desc, PropRow& row, const Wui::WuiComponentProperty& prop,
			const Wui::WuiRect& rowRect, float fontScale);
		// 行控件值存储 ⇄ 文本(值编码协议:颜色 #RRGGBB[AA]、尺寸 WxH、其余沿用文本)。
		void SeedPropertyRow(PropRow& row, const Wui::WuiComponentProperty& prop);
		void SerializePropertyRow(PropRow& row, const Wui::WuiComponentProperty& prop);
		// 键 → 值(用户覆盖优先,其次登记表默认值)。
		std::string PropertyValueForKey(const Wui::WuiComponentDesc& desc,
			const Wui::WuiComponentProperty& prop, const std::string& key) const;
		void DrawActions(Wui::WuiContext& ctx, const WbLayout& layout, const Wui::WuiTheme& theme,
			const Wui::WuiComponentDesc* desc, float uiScale);

		// Capture 元数据(Capture 按钮 → 探针按它裁剪整窗抓图)。
		std::string CaptureMetadataJson(const Wui::WuiComponentDesc& desc, float density,
			float uiScale) const;
		void WriteCaptureMetadata(const Wui::WuiComponentDesc& desc, float density, float uiScale) const;
		void WriteApproval(const Wui::WuiComponentDesc& desc, float density, float uiScale);

		// 属性值读写(缓存 + 默认值)。
		std::string PropertyValue(const Wui::WuiComponentDesc& desc, const Wui::WuiComponentProperty& prop) const;
		void SetPropertyValue(const std::string& componentId, const std::string& name, const std::string& value);
		void ResetPropertyValues(const std::string& componentId);
		std::vector<std::pair<std::string, std::string>> AppliedProperties(
			const Wui::WuiComponentDesc& desc, float density, float uiScale) const;
	};
}
