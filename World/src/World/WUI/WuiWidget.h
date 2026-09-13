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

		// 布局/绘制缓存:子树脏时重算,否则复用上一帧结果。
		void Invalidate();
		bool IsDirty() const { return m_Dirty; }

	protected:
		void MarkClean() { m_Dirty = false; }

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
		WuiColor IdleFill { 0, 0, 0, 0 };
		WuiColor HoverFill { 1, 1, 1, 0.06f };
		WuiColor SelectedFill { 0.28f, 0.45f, 0.85f, 0.35f };

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
