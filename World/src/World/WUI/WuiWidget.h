#pragma once

#include "World/WUI/WuiContext.h"
#include "World/WUI/WuiCore.h"
#include "World/WUI/WuiWidgets.h"

#include <functional>
#include <memory>
#include <vector>

namespace World::Wui
{
	class WuiWidget;
	using WuiWidgetPtr = std::shared_ptr<WuiWidget>;

	struct WuiMeasure
	{
		float Width = 0;
		float Height = 0;
	};

	// 树绘制上下文:向渲染后端输出命令,并维护嵌套裁剪。
	class WuiPaintContext
	{
	public:
		explicit WuiPaintContext(WuiContext& context) : m_Context(context) {}
		WuiContext& Context() { return m_Context; }
		const WuiRect& Clip() const { return m_Clip; }
		void PushClip(const WuiRect& rect);
		void PopClip();

	private:
		WuiContext& m_Context;
		std::vector<WuiRect> m_ClipStack;
		WuiRect m_Clip { 0, 0, 1e9f, 1e9f };
	};

	// 保留模式 widget 基类:测量/布局/绘制/命中/焦点与脏标记。
	class WuiWidget : public std::enable_shared_from_this<WuiWidget>
	{
	public:
		virtual ~WuiWidget() = default;

		WuiId Id() const { return m_Id; }
		void SetId(WuiId id) { m_Id = id; }
		const WuiRect& Rect() const { return m_Rect; }
		WuiWidgetPtr Parent() const { return m_Parent.lock(); }
		void SetParent(const WuiWidgetPtr& parent) { m_Parent = parent; }

		virtual WuiMeasure Measure(const WuiConstraints& constraints) = 0;
		virtual void Arrange(const WuiRect& rect) { m_Rect = rect; }
		virtual void Paint(WuiPaintContext& context) = 0;
		virtual WuiWidgetPtr HitTest(glm::vec2 point);

		// ---- 交互状态(组件统一解剖:Id/Rect/Disabled/Hovered/Active/Focused) ----
		bool Disabled = false;
		bool Hovered(WuiContext& ctx) const { return !Disabled && ctx.IsHovered(m_Rect); }
		bool Active(WuiContext& ctx) const { return Hovered(ctx) && ctx.Input().MouseDown[0]; }
		// 点击边沿;禁用或未命中返回 false。
		bool Clicked(WuiContext& ctx) const { return !Disabled && ctx.IsClicked(m_Rect); }
		bool Focused(WuiContext& ctx) const { return m_Id != 0 && ctx.Focus() == m_Id; }

		// 布局/绘制缓存:子树脏时重算,否则复用上一帧结果。
		void Invalidate();
		bool IsDirty() const { return m_Dirty; }

	protected:
		void MarkClean() { m_Dirty = false; }

		// U2e:对象式控件的公共焦点处理 —— 在第 1 帧绘制时把这颗控件登记进
		// WuiContext 的焦点表(登记顺序 = 绘制顺序 = 视觉顺序,Tab / Shift+Tab 由它统一排序),
		// 并在自身绘制末尾画焦点环。
		// 只做登记,不抢焦点:获得焦点仍靠点击或 Tab。Id()==0 的控件(未命名的叶控件)
		// 完全不参与焦点体系,既有命令流逐字节不变。
		// 派生类在各自 Paint 的开头调用 RegisterFocusable(),末尾调用 PaintFocusRing()。
		// (刻意不叫 DrawFocusRing:成员名会遮蔽同命名空间的自由函数,虽然仍能编译,
		//  但读代码的人容易以为它调的是自己。)
		void RegisterFocusable(WuiPaintContext& context);
		void PaintFocusRing(WuiPaintContext& context);

		WuiRect m_Rect;
		WuiId m_Id = 0;
		std::weak_ptr<WuiWidget> m_Parent;
		bool m_Dirty = true;
	};

	struct WuiFlexChild
	{
		WuiWidgetPtr Widget;
		WuiFlexItem Item;
	};

	// flex 容器:布局引擎的实际消费者。
	class WuiBox final : public WuiWidget
	{
	public:
		WuiDirection Direction = WuiDirection::Column;
		WuiAlign AlignMain = WuiAlign::Start;
		WuiAlign AlignCross = WuiAlign::Stretch;
		float Gap = 0;

		void Add(const WuiWidgetPtr& child, const WuiFlexItem& item = {});
		void Clear();
		const std::vector<WuiFlexChild>& Children() const { return m_Children; }

		WuiMeasure Measure(const WuiConstraints& constraints) override;
		void Arrange(const WuiRect& rect) override;
		void Paint(WuiPaintContext& context) override;
		WuiWidgetPtr HitTest(glm::vec2 point) override;

	private:
		std::vector<WuiFlexChild> m_Children;
		WuiMeasure m_Measured;
	};

	// ---- 叶子控件 ----

	class WuiLabel final : public WuiWidget
	{
	public:
		std::string Text;
		WuiColor Color { 1, 1, 1, 1 };
		float FontSize = 15.0f;
		bool Bold = false;
		float FixedWidth = -1.0f;
		float FixedHeight = -1.0f;

		WuiMeasure Measure(const WuiConstraints& constraints) override;
		void Paint(WuiPaintContext& context) override;
	};

	class WuiSpacer final : public WuiWidget
	{
	public:
		float Width = 0;
		float Height = 0;

		WuiMeasure Measure(const WuiConstraints&) override { MarkClean(); return { Width, Height }; }
		void Paint(WuiPaintContext&) override {}
	};

	class WuiButton final : public WuiWidget
	{
	public:
		std::string Label;
		std::function<void()> OnClick;
		bool Enabled = true;
		// P4-UX13:图标按钮(‹ › ↑ 这类单字形)需要**居中**;文字按钮保持左对齐(默认)。
		bool CenterLabel = false;

		WuiMeasure Measure(const WuiConstraints& constraints) override;
		void Paint(WuiPaintContext& context) override;
	};

	class WuiCheckbox final : public WuiWidget
	{
	public:
		std::string Label;
		bool* Value = nullptr;

		WuiMeasure Measure(const WuiConstraints& constraints) override;
		void Paint(WuiPaintContext& context) override;
	};

	class WuiImage final : public WuiWidget
	{
	public:
		uint64_t TextureId = 0;
		WuiRect Uv { 0, 0, 1, 1 };
		WuiColor Tint { 1, 1, 1, 1 };

		WuiMeasure Measure(const WuiConstraints&) override { MarkClean(); return { 0, 0 }; }
		void Paint(WuiPaintContext& context) override;
	};

	inline const WuiTheme& WuiDefaultTheme()
	{
		static WuiTheme theme;
		return theme;
	}

	class WuiTextField final : public WuiWidget
	{
	public:
		std::string* Buffer = nullptr;
		std::function<void()> OnCommit;
		std::function<void()> OnCancel;
		const WuiTheme* Theme = nullptr;
		// P4-U5a:进无障碍树的控件名与"空输入时的占位文案"(与画在框里的提示同一句)。
		std::string A11yLabel;
		std::string A11yPlaceholder;

		WuiMeasure Measure(const WuiConstraints& constraints) override;
		void Paint(WuiPaintContext& context) override;
	};

	class WuiDragFloat final : public WuiWidget
	{
	public:
		float* Value = nullptr;
		float Speed = 0.01f;
		float Min = 1.0f;
		float Max = -1.0f;
		const WuiTheme* Theme = nullptr;

		WuiMeasure Measure(const WuiConstraints& constraints) override;
		void Paint(WuiPaintContext& context) override;
	};

	class WuiDragInt final : public WuiWidget
	{
	public:
		int64_t* Value = nullptr;
		int64_t Min = INT64_MIN;
		int64_t Max = INT64_MAX;
		const WuiTheme* Theme = nullptr;

		WuiMeasure Measure(const WuiConstraints& constraints) override;
		void Paint(WuiPaintContext& context) override;
	};

	class WuiCombo final : public WuiWidget
	{
	public:
		std::string Label;
		const std::vector<std::string>* Options = nullptr;
		int* Selected = nullptr;
		const WuiTheme* Theme = nullptr;

		WuiMeasure Measure(const WuiConstraints& constraints) override;
		void Paint(WuiPaintContext& context) override;
	};

	class WuiScrollArea final : public WuiWidget
	{
	public:
		WuiWidgetPtr Child;
		float ContentHeight = 0;
		const WuiTheme* Theme = nullptr;

		WuiMeasure Measure(const WuiConstraints& constraints) override;
		void Arrange(const WuiRect& rect) override;
		void Paint(WuiPaintContext& context) override;
		WuiWidgetPtr HitTest(glm::vec2 point) override;

	private:
		float m_ScrollY = 0;
	};

	class WuiProgress final : public WuiWidget
	{
	public:
		float Fraction = 0; // 0..1
		WuiColor TrackColor { 0.2f, 0.21f, 0.23f, 1 };
		WuiColor FillColor { 0.3f, 0.5f, 0.9f, 1 };

		WuiMeasure Measure(const WuiConstraints& constraints) override;
		void Paint(WuiPaintContext& context) override;
	};

	class WuiListRow final : public WuiWidget
	{
	public:
		std::string Text;
		bool Selected = false;
		std::function<void()> OnClick;
		float FontSize = 14;
		// 文本左侧缩进(层级树等场景用),不影响选中/悬停底色范围。
		float Indent = 0.0f;
		WuiColor IdleFill { 0, 0, 0, 0 };
		WuiColor HoverFill { 1, 1, 1, 0.06f };
		WuiColor SelectedFill { 0.28f, 0.45f, 0.85f, 0.35f };
		// P4-U5a(2026-09-21):SetId 之后本行进无障碍树(kind="list-row")。
		// 为什么在控件里做而不是各面板各写一遍:层级面板这类"行列表"是读屏/脚本最需要枚举的东西,
		// 每处手写一次必然漏(实测:层级面板 9 行实体在 ui.tree 里 0 个节点)。
		// AccessValue 放"这一行代表什么"(如实体 handle);AccessTooltip 放悬停说明。
		std::string AccessValue;
		std::string AccessTooltip;

		WuiMeasure Measure(const WuiConstraints& constraints) override;
		void Paint(WuiPaintContext& context) override;
	};

	class WuiImageButton final : public WuiWidget
	{
	public:
		uint64_t TextureId = 0;
		WuiRect Uv { 0, 1, 1, -1 };
		std::function<void()> OnClick;
		bool Dim = false;

		WuiMeasure Measure(const WuiConstraints& constraints) override;
		void Paint(WuiPaintContext& context) override;
	};

	class WuiSection final : public WuiWidget
	{
	public:
		std::string Title;
		bool Open = false;
		float ContentHeight = 0;
		std::function<void(WuiContext&, const WuiRect&)> DrawContent;
		WuiColor HeaderFill { 0.2f, 0.21f, 0.23f, 1 };

		WuiMeasure Measure(const WuiConstraints& constraints) override;
		void Arrange(const WuiRect& rect) override;
		void Paint(WuiPaintContext& context) override;
		WuiWidgetPtr HitTest(glm::vec2 point) override;
		const WuiRect& ContentRect() const { return m_ContentRect; }

	private:
		WuiRect m_ContentRect;
	};

	class WuiCustom final : public WuiWidget
	{
	public:
		float ContentHeight = 0;
		std::function<void(WuiContext&, const WuiRect&)> Draw;

		WuiMeasure Measure(const WuiConstraints& constraints) override;
		void Paint(WuiPaintContext& context) override;
	};

	// 便捷:根据约束框递归测量并布局整棵树。
	void LayoutWidgetTree(const WuiWidgetPtr& root, const WuiRect& rect);
}
