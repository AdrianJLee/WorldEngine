#pragma once

#include "EditorPanel.h"

#include "World/WUI/WuiComponentRegistry.h"
#include "World/WUI/WuiWidget.h"
#include "World/WUI/Widgets/WuiControls.h"

#include <map>
#include <memory>
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
		int m_DensityIndex = 0;               // 0 = comfortable / 1 = compact
		int m_UiScaleIndex = 1;               // 0 = 100% / 1 = 125% / 2 = 150%
		int m_LanguageIndex = 0;              // 0 = en / 1 = zh-CN
		bool m_ShowGrid = true;

		// 画布实测矩形(客户区坐标)+ 该帧实际送进 showcase 的属性(写 Capture 元数据用)。
		Wui::WuiRect m_CanvasRect {};
		Wui::WuiRect m_CanvasInner {};
		bool m_CanvasValid = false;
		std::string m_AppliedState = "default";
		std::vector<std::pair<std::string, std::string>> m_AppliedProperties;

		// Combo 要求 vector 引用:**只在更新事件那一帧**重建(随后 Clear）。
		std::vector<std::string> m_LanguageOptions;
		std::vector<std::string> m_StateOptions;
		std::vector<std::string> m_PropertyEnumOptions;

		// 属性值缓存:组件 id → (属性名 → 文本值)。数值/枚举/文本控件都读写这里。
		std::map<std::string, std::map<std::string, std::string>> m_PropertyValues;
		// 枚举属性的下拉选中项(属性名 → 选项下标)。
		// **不要**把下标塞进 ctx.Persist:同一个 id 还被 TextField 等控件的内部
		// 焦点/编辑态使用,类型不一致会触发 "persisted state id reused with different types"
		// (实测该告警随后就是浮窗渲染失败:string too long)。控件自己的持久态归控件。
		std::map<std::string, int> m_PropertyEnumIndex;

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
		void DrawTopBar(Wui::WuiContext& ctx, const WbLayout& layout, const Wui::WuiTheme& theme);
		void DrawInfoBar(Wui::WuiContext& ctx, const WbLayout& layout, const Wui::WuiTheme& theme,
			const Wui::WuiComponentDesc* desc);
		// 返回本帧选中的登记项(可能来自左树的点击)。
		const Wui::WuiComponentDesc* DrawTree(Wui::WuiContext& ctx, const WbLayout& layout,
			const Wui::WuiTheme& theme);
		void DrawCanvas(Wui::WuiContext& ctx, const WbLayout& layout, const Wui::WuiTheme& theme,
			const Wui::WuiComponentDesc* desc, float density, float uiScale);
		void DrawProperties(Wui::WuiContext& ctx, const WbLayout& layout, const Wui::WuiTheme& theme,
			const Wui::WuiComponentDesc* desc, float density, float uiScale);
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
