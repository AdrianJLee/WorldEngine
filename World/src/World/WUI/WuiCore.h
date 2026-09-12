#pragma once

// WUI Core:纯逻辑(布局/样式/命中/焦点),不依赖任何渲染后端。

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <glm/glm.hpp>

namespace World::Wui
{
	using WuiId = uint32_t;

	inline WuiId HashId(const char* text)
	{
		uint32_t hash = 2166136261u;
		while (*text)
		{
			hash = (hash ^ static_cast<uint8_t>(*text++)) * 16777619u;
		}
		return hash;
	}

	struct WuiRect
	{
		float X = 0, Y = 0, W = 0, H = 0;

		bool Contains(glm::vec2 point) const
		{
			return point.x >= X && point.y >= Y && point.x <= X + W && point.y <= Y + H;
		}
	};

	struct WuiColor
	{
		float R = 1, G = 1, B = 1, A = 1;
	};

	enum class WuiAlign : uint8_t
	{
		Start,
		Center,
		End,
		Stretch,
	};

	enum class WuiDirection : uint8_t
	{
		Row,
		Column,
	};

	// ---- 样式与级联 ----
	struct WuiStyle
	{
		std::optional<WuiColor> TextColor;
		std::optional<WuiColor> FillColor;
		std::optional<WuiColor> BorderColor;
		std::optional<float> FontSize;
		std::optional<float> PaddingX;
		std::optional<float> PaddingY;
		std::optional<float> Gap;
		std::optional<float> BorderWidth;
		std::optional<float> Rounding;

		// other 覆盖本对象已设置的属性。
		WuiStyle Overlay(const WuiStyle& other) const;
	};

	struct WuiStyleRule
	{
		std::string Selector; // "Panel" 或 ".class"
		WuiStyle Props;
	};

	class WuiStyleSheet
	{
	public:
		void AddRule(const std::string& selector, const WuiStyle& props);
		// 按注册顺序叠加匹配规则,后注册覆盖先注册;parent 作为基础再叠加。
		WuiStyle Resolve(const WuiStyle& parent, const std::string& type, const std::vector<std::string>& classes) const;

	private:
		bool Matches(const WuiStyleRule& rule, const std::string& type, const std::vector<std::string>& classes) const;
		std::vector<WuiStyleRule> m_Rules;
	};

	// ---- 约束与 Flex 布局 ----
	struct WuiConstraints
	{
		float MinW = 0, MinH = 0;
		float MaxW = 1e30f, MaxH = 1e30f;
	};

	struct WuiFlexItem
	{
		float MinMain = 0, MaxMain = 1e30f;
		float MinCross = 0, MaxCross = 1e30f;
		float Grow = 0;
	};

	struct WuiFlexLayout
	{
		WuiDirection Direction = WuiDirection::Column;
		WuiAlign AlignMain = WuiAlign::Start;
		WuiAlign AlignCross = WuiAlign::Start;
		float Gap = 0;
	};

	struct WuiFlexResult
	{
		std::vector<WuiRect> Rects;
		float MainSize = 0;
		float CrossSize = 0;
	};

	WuiFlexResult SolveFlex(const WuiFlexLayout& layout, const std::vector<WuiFlexItem>& items, const WuiConstraints& container);

	// ---- 命中与焦点 ----
	bool HitTest(const WuiRect& rect, glm::vec2 point);

	// 按树序找下一个可聚焦 id(线性遍历,无环);无下一项返回 nullopt。
	std::optional<WuiId> NextFocus(const std::vector<WuiId>& order, WuiId current, bool backwards);
}
