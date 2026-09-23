#pragma once

// WUI 组件库:在保留模式 Widget 树(WuiWidget.h)之上提供的通用控件。
// 组件统一解剖:Id / Rect / 状态(Disabled、Hovered、Active、Focused)+ 主题取自 WuiTheme。
// 交互状态由 WuiContext(焦点、输入、持久化)驱动,绘制只输出 WuiDrawCommand。

#include "World/WUI/WuiWidget.h"

#include <functional>
#include <string>
#include <vector>

namespace World::Wui
{
	// 水平分隔线。
	class WuiSeparator final : public WuiWidget
	{
	public:
		float Thickness = 1.0f;
		const WuiTheme* Theme = nullptr;

		WuiMeasure Measure(const WuiConstraints& constraints) override;
		void Paint(WuiPaintContext& context) override;
	};

	// 图标按钮:纹理 + 悬停底色,可选 Tooltip。
	class WuiIconButton final : public WuiWidget
	{
	public:
		uint64_t TextureId = 0;
		WuiRect Uv { 0, 0, 1, 1 };
		float IconSize = 16.0f;
		std::string Tooltip;
		const WuiTheme* Theme = nullptr;
		std::function<void()> OnClick;

		WuiMeasure Measure(const WuiConstraints& constraints) override;
		void Paint(WuiPaintContext& context) override;
	};

	// 开关:布尔值驱动,点击切换。
	class WuiToggle final : public WuiWidget
	{
	public:
		std::string Label;
		bool* Value = nullptr;
		const WuiTheme* Theme = nullptr;
		std::function<void()> OnChanged;

		WuiMeasure Measure(const WuiConstraints& constraints) override;
		void Paint(WuiPaintContext& context) override;
	};

	// 数值滑条:点击定位 + 按住拖动;值域 [Min, Max]。
	class WuiSlider final : public WuiWidget
	{
	public:
		float* Value = nullptr;
		float Min = 0.0f;
		float Max = 1.0f;
		const WuiTheme* Theme = nullptr;
		std::function<void()> OnChanged;

		WuiMeasure Measure(const WuiConstraints& constraints) override;
		void Paint(WuiPaintContext& context) override;
	};

	// 列表条目:层级面板、文件列表、设置项通用行。
	class WuiListItem final : public WuiWidget
	{
	public:
		std::string Label;
		bool Selected = false;
		uint64_t IconTexture = 0; // 0 = 无图标
		const WuiTheme* Theme = nullptr;
		std::function<void()> OnSelect;   // 单击
		std::function<void()> OnActivate; // 双击

		WuiMeasure Measure(const WuiConstraints& constraints) override;
		void Paint(WuiPaintContext& context) override;
	};

	// 树形条目:Expanded 为空表示叶子;箭头区域切换展开,其余区域选择。
	class WuiTreeItem final : public WuiWidget
	{
	public:
		std::string Label;
		int Depth = 0;
		bool Selected = false;
		bool* Expanded = nullptr;
		const WuiTheme* Theme = nullptr;
		std::function<void()> OnSelect;
		std::function<void()> OnToggle;

		bool Leaf() const { return Expanded == nullptr; }
		WuiMeasure Measure(const WuiConstraints& constraints) override;
		void Paint(WuiPaintContext& context) override;
	};

	// 标签页:等宽页签 + 选中下划线。
	class WuiTabs final : public WuiWidget
	{
	public:
		std::vector<std::string> Labels;
		int* Selected = nullptr;
		const WuiTheme* Theme = nullptr;
		std::function<void(int)> OnChanged;

		WuiMeasure Measure(const WuiConstraints& constraints) override;
		void Paint(WuiPaintContext& context) override;
	};

	// 悬浮提示:Anchor 命中时在 Overlay 层绘制提示框。
	class WuiTooltip final : public WuiWidget
	{
	public:
		std::string Text;
		WuiRect Anchor;
		const WuiTheme* Theme = nullptr;

		WuiMeasure Measure(const WuiConstraints&) override { MarkClean(); return { 0, 0 }; }
		void Paint(WuiPaintContext& context) override;
	};

	// 弹出菜单按钮:点击展开条目列表,选择后关闭;支持外部点击关闭。
	class WuiMenuButton final : public WuiWidget
	{
	public:
		std::string Label;
		std::vector<std::string> Items;
		const WuiTheme* Theme = nullptr;
		std::function<void(int)> OnSelect;

		WuiMeasure Measure(const WuiConstraints& constraints) override;
		void Paint(WuiPaintContext& context) override;
	};
}
