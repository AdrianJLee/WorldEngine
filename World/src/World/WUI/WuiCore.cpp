#include "wldpch.h"
#include "World/WUI/WuiCore.h"

#include <algorithm>
#include <iterator>

namespace World::Wui
{
	WuiStyle WuiStyle::Overlay(const WuiStyle& other) const
	{
		WuiStyle result = *this;
		if (other.TextColor) result.TextColor = other.TextColor;
		if (other.FillColor) result.FillColor = other.FillColor;
		if (other.BorderColor) result.BorderColor = other.BorderColor;
		if (other.FontSize) result.FontSize = other.FontSize;
		if (other.PaddingX) result.PaddingX = other.PaddingX;
		if (other.PaddingY) result.PaddingY = other.PaddingY;
		if (other.Gap) result.Gap = other.Gap;
		if (other.BorderWidth) result.BorderWidth = other.BorderWidth;
		if (other.Rounding) result.Rounding = other.Rounding;
		return result;
	}

	void WuiStyleSheet::AddRule(const std::string& selector, const WuiStyle& props)
	{
		m_Rules.push_back({ selector, props });
	}

	bool WuiStyleSheet::Matches(const WuiStyleRule& rule, const std::string& type, const std::vector<std::string>& classes) const
	{
		if (!rule.Selector.empty() && rule.Selector.front() == '.')
		{
			const std::string wanted = rule.Selector.substr(1);
			return std::find(classes.begin(), classes.end(), wanted) != classes.end();
		}
		return rule.Selector == type;
	}

	WuiStyle WuiStyleSheet::Resolve(const WuiStyle& parent, const std::string& type, const std::vector<std::string>& classes) const
	{
		WuiStyle result = parent;
		for (const WuiStyleRule& rule : m_Rules)
			if (Matches(rule, type, classes))
				result = result.Overlay(rule.Props);
		return result;
	}

	WuiFlexResult SolveFlex(const WuiFlexLayout& layout, const std::vector<WuiFlexItem>& items, const WuiConstraints& container)
	{
		WuiFlexResult result;
		const bool row = layout.Direction == WuiDirection::Row;
		const size_t count = items.size();
		result.Rects.resize(count);
		if (count == 0)
		{
			result.CrossSize = row ? container.MinH : container.MinW;
			return result;
		}

		const float totalGap = layout.Gap * static_cast<float>(count - 1);
		const float boundedMain = row ? container.MaxW : container.MaxH;
		const float availableMain = boundedMain > 1e29f ? 0.0f : std::max(0.0f, boundedMain - totalGap);

		std::vector<float> desired(count);
		float sumDesired = 0;
		float totalGrow = 0;
		for (size_t i = 0; i < count; ++i)
		{
			const float minMain = items[i].MinMain;
			const float cap = availableMain > 0 ? std::min(items[i].MaxMain, availableMain) : items[i].MaxMain;
			desired[i] = std::max(minMain, cap);
			sumDesired += desired[i];
			totalGrow += items[i].Grow;
		}

		std::vector<float> main(count);
		float remaining = availableMain > 0 ? availableMain - sumDesired : 0.0f;
		for (size_t i = 0; i < count; ++i)
		{
			float extra = (remaining > 0 && totalGrow > 0) ? remaining * (items[i].Grow / totalGrow) : 0.0f;
			// MaxMain 是"内容首选尺寸";Grow 分配的自由空间可以超出首选尺寸,仅 MinMain 作下限。
			main[i] = std::max(items[i].MinMain, desired[i] + extra);
		}

		// 交叉轴:有界时以容器交叉尺寸为参考(Center/Stretch 相对容器);
		// 无界时取子项内容交叉尺寸。
		const float boundedCross = row ? container.MaxH : container.MaxW;
		float crossSize;
		if (boundedCross > 1e29f)
		{
			crossSize = row ? container.MinH : container.MinW;
			for (size_t i = 0; i < count; ++i)
				crossSize = std::max(crossSize, items[i].MaxCross);
		}
		else
		{
			crossSize = std::max(row ? container.MinH : container.MinW, boundedCross);
		}

		float cursor = 0;
		for (size_t i = 0; i < count; ++i)
		{
			WuiRect& rect = result.Rects[i];
			const float childCross = std::max(items[i].MinCross, std::min(items[i].MaxCross, crossSize));
			float crossPos = 0;
			float crossSpan = childCross;
			switch (layout.AlignCross)
			{
				case WuiAlign::Center: crossPos = (crossSize - childCross) * 0.5f; break;
				case WuiAlign::End: crossPos = crossSize - childCross; break;
				case WuiAlign::Stretch: crossSpan = crossSize; break;
				default: break;
			}
			if (row)
				rect = { cursor, crossPos, main[i], crossSpan };
			else
				rect = { crossPos, cursor, crossSpan, main[i] };
			cursor += main[i] + layout.Gap;
		}

		result.MainSize = cursor - layout.Gap;
		result.CrossSize = crossSize;
		return result;
	}

	bool HitTest(const WuiRect& rect, glm::vec2 point)
	{
		return rect.Contains(point);
	}

	std::optional<WuiId> NextFocus(const std::vector<WuiId>& order, WuiId current, bool backwards)
	{
		if (order.empty())
			return std::nullopt;
		auto it = std::find(order.begin(), order.end(), current);
		if (it == order.end())
			return backwards ? order.back() : order.front();
		if (backwards)
			return it == order.begin() ? order.back() : *std::prev(it);
		auto next = std::next(it);
		return next == order.end() ? order.front() : *next;
	}
}
