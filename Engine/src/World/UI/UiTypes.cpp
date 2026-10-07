#include "wldpch.h"
#include "World/UI/UiTypes.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace World::UI
{
	namespace
	{
		std::string_view Trim(std::string_view text)
		{
			while (!text.empty() && (text.front() == ' ' || text.front() == '\t'))
				text.remove_prefix(1);
			while (!text.empty() && (text.back() == ' ' || text.back() == '\t'))
				text.remove_suffix(1);
			return text;
		}
	}

	// ---- 缩放策略 ----

	const char* UiScaleModeName(UiScaleMode mode)
	{
		switch (mode)
		{
		case UiScaleMode::ConstantPixelSize: return "ConstantPixelSize";
		case UiScaleMode::ScaleWithScreenSize: return "ScaleWithScreenSize";
		case UiScaleMode::Expand: return "Expand";
		case UiScaleMode::Shrink: return "Shrink";
		}
		return "ScaleWithScreenSize";
	}

	bool ParseUiScaleMode(std::string_view name, UiScaleMode& out)
	{
		const std::string_view text = Trim(name);
		if (text == "ConstantPixelSize" || text == "constant-pixel") { out = UiScaleMode::ConstantPixelSize; return true; }
		if (text == "ScaleWithScreenSize" || text == "scale-with-screen") { out = UiScaleMode::ScaleWithScreenSize; return true; }
		if (text == "Expand" || text == "expand") { out = UiScaleMode::Expand; return true; }
		if (text == "Shrink" || text == "shrink") { out = UiScaleMode::Shrink; return true; }
		return false;
	}

	Wui::WuiRect UiViewport::DesignRectToPhysical(const Wui::WuiRect& r) const
	{
		const glm::vec2 origin = DesignToPhysical(glm::vec2 { r.X, r.Y });
		return Wui::WuiRect { origin.x, origin.y, r.W * Scale, r.H * Scale };
	}

	UiViewport ComputeUiViewport(const UiDesign& design, const UiSafeArea& safe, const UiSurface& surface)
	{
		UiViewport viewport;
		viewport.DesignResolution = design.Resolution;
		viewport.PhysicalSize = surface.PhysicalSize;

		const float designW = std::max(design.Resolution.x, 1.0f);
		const float designH = std::max(design.Resolution.y, 1.0f);
		const float physicalW = std::max(surface.PhysicalSize.x, 1.0f);
		const float physicalH = std::max(surface.PhysicalSize.y, 1.0f);
		const float dpiScale = surface.DpiScale > 0.0f ? surface.DpiScale : 1.0f;

		const float ratioW = physicalW / designW;
		const float ratioH = physicalH / designH;

		float strategyScale = 1.0f;
		switch (design.ScaleMode)
		{
		case UiScaleMode::ConstantPixelSize:
			strategyScale = 1.0f;
			break;
		case UiScaleMode::Expand:
			strategyScale = std::max(ratioW, ratioH);
			break;
		case UiScaleMode::Shrink:
			strategyScale = std::min(ratioW, ratioH);
			break;
		case UiScaleMode::ScaleWithScreenSize:
		{
			// 对数插值(Unity MatchWidthOrHeight 口径):Match=0 → 纯宽度比,Match=1 → 纯高度比。
			const float match = std::clamp(design.Match, 0.0f, 1.0f);
			const float logW = std::log2(std::max(ratioW, 1e-6f));
			const float logH = std::log2(std::max(ratioH, 1e-6f));
			strategyScale = std::exp2(logW + (logH - logW) * match);
			break;
		}
		}

		viewport.Scale = std::max(strategyScale * dpiScale, 1e-6f);
		viewport.PhysicalOrigin = glm::vec2 { 0.0f, 0.0f };

		Wui::WuiRect content { 0.0f, 0.0f, design.Resolution.x, design.Resolution.y };
		if (safe.Enabled)
		{
			const float left = safe.Left / viewport.Scale;
			const float top = safe.Top / viewport.Scale;
			const float right = safe.Right / viewport.Scale;
			const float bottom = safe.Bottom / viewport.Scale;
			content.X += left;
			content.Y += top;
			content.W = std::max(content.W - left - right, 0.0f);
			content.H = std::max(content.H - top - bottom, 0.0f);
		}
		viewport.ContentRect = content;
		return viewport;
	}

	bool HitTestDesign(const Wui::WuiRect& designRect, const UiViewport& viewport, glm::vec2 physicalPoint)
	{
		return designRect.Contains(viewport.PhysicalToDesign(physicalPoint));
	}

	// ---- 锚点 ----

	Wui::WuiRect ResolveAnchor(const UiAnchor& anchor, const Wui::WuiRect& parent)
	{
		const float pw = parent.W;
		const float ph = parent.H;

		Wui::WuiRect rect;

		// 点锚定(Min == Max):Offset 是"枢轴相对锚点"的位置(Unity anchoredPosition 口径);
		// 拉伸(Min != Max):Offset 是最小角相对锚框的偏移(Unity offsetMin 口径)。
		if (anchor.Min.x == anchor.Max.x)
		{
			rect.W = std::max(anchor.Size.x, 0.0f);
			rect.X = parent.X + anchor.Min.x * pw + anchor.Offset.x - anchor.Pivot.x * rect.W;
		}
		else
		{
			rect.X = parent.X + anchor.Min.x * pw + anchor.Offset.x;
			rect.W = std::max((anchor.Max.x - anchor.Min.x) * pw + anchor.Size.x, 0.0f);
		}

		if (anchor.Min.y == anchor.Max.y)
		{
			rect.H = std::max(anchor.Size.y, 0.0f);
			rect.Y = parent.Y + anchor.Min.y * ph + anchor.Offset.y - anchor.Pivot.y * rect.H;
		}
		else
		{
			rect.Y = parent.Y + anchor.Min.y * ph + anchor.Offset.y;
			rect.H = std::max((anchor.Max.y - anchor.Min.y) * ph + anchor.Size.y, 0.0f);
		}

		return rect;
	}

	// ---- 布局容器 ----

	const char* UiLayoutKindName(UiLayoutKind kind)
	{
		switch (kind)
		{
		case UiLayoutKind::Absolute: return "Absolute";
		case UiLayoutKind::Column: return "Column";
		case UiLayoutKind::Row: return "Row";
		case UiLayoutKind::Overlay: return "Overlay";
		case UiLayoutKind::Grid: return "Grid";
		case UiLayoutKind::Flex: return "Flex";
		}
		return "Absolute";
	}

	bool ParseUiLayoutKind(std::string_view name, UiLayoutKind& out)
	{
		const std::string_view text = Trim(name);
		if (text == "Absolute" || text == "absolute") { out = UiLayoutKind::Absolute; return true; }
		if (text == "Column" || text == "column") { out = UiLayoutKind::Column; return true; }
		if (text == "Row" || text == "row") { out = UiLayoutKind::Row; return true; }
		if (text == "Overlay" || text == "overlay") { out = UiLayoutKind::Overlay; return true; }
		if (text == "Grid" || text == "grid") { out = UiLayoutKind::Grid; return true; }
		if (text == "Flex" || text == "flex") { out = UiLayoutKind::Flex; return true; }
		return false;
	}

	Wui::WuiRect ApplyPadding(const UiLayoutSpec& spec, const Wui::WuiRect& parent)
	{
		Wui::WuiRect inner;
		inner.X = parent.X + spec.Padding[0];
		inner.Y = parent.Y + spec.Padding[1];
		inner.W = std::max(parent.W - spec.Padding[0] - spec.Padding[2], 0.0f);
		inner.H = std::max(parent.H - spec.Padding[1] - spec.Padding[3], 0.0f);
		return inner;
	}

	Wui::WuiRect SolveChildSlot(const UiLayoutSpec& spec, const Wui::WuiRect& parent,
		std::size_t index, std::size_t count)
	{
		// Absolute / Overlay / Flex:子节点相对整个(已扣 padding 的)父矩形自己求解锚点;
		// Flex 由调用方喂 Wui::SolveFlex 分配主轴尺寸。
		const Wui::WuiRect inner = ApplyPadding(spec, parent);
		if (spec.Kind == UiLayoutKind::Absolute || spec.Kind == UiLayoutKind::Overlay ||
			spec.Kind == UiLayoutKind::Flex || count == 0)
			return inner;

		if (spec.Kind == UiLayoutKind::Column)
		{
			const float total = inner.H - spec.Gap * static_cast<float>(count - 1);
			const float each = std::max(total / static_cast<float>(count), 0.0f);
			return Wui::WuiRect { inner.X, inner.Y + (each + spec.Gap) * static_cast<float>(index), inner.W, each };
		}

		if (spec.Kind == UiLayoutKind::Row)
		{
			const float total = inner.W - spec.Gap * static_cast<float>(count - 1);
			const float each = std::max(total / static_cast<float>(count), 0.0f);
			return Wui::WuiRect { inner.X + (each + spec.Gap) * static_cast<float>(index), inner.Y, each, inner.H };
		}

		// Grid
		const int columns = std::max(spec.Columns, 1);
		const std::size_t cols = static_cast<std::size_t>(columns);
		const std::size_t rows = (count + cols - 1) / cols;
		const float cellW = std::max((inner.W - spec.Gap * static_cast<float>(cols - 1)) / static_cast<float>(cols), 0.0f);
		const float cellH = std::max((inner.H - spec.Gap * static_cast<float>(rows - 1)) / static_cast<float>(rows), 0.0f);
		const std::size_t row = index / cols;
		const std::size_t col = index % cols;
		return Wui::WuiRect {
			inner.X + (cellW + spec.Gap) * static_cast<float>(col),
			inner.Y + (cellH + spec.Gap) * static_cast<float>(row),
			cellW, cellH };
	}

	// ---- 稳定 Id ----

	std::string MakeStableId(std::string_view parentPath, std::string_view type, std::size_t index)
	{
		std::string id;
		if (!parentPath.empty())
		{
			id.assign(parentPath);
			id.push_back('.');
		}
		id.append(type.empty() ? std::string_view("Node") : type);
		id.push_back('#');
		id.append(std::to_string(index));
		return id;
	}

	bool IsValidUiNodeId(std::string_view id)
	{
		if (id.empty())
			return false;
		for (const char c : id)
		{
			const bool alnum = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
			const bool symbol = c == '_' || c == '-' || c == '.' || c == '#';
			if (!alnum && !symbol)
				return false;
		}
		return true;
	}

	bool IsDefaultAnchor(const UiAnchor& anchor)
	{
		return anchor.Min.x == 0.0f && anchor.Min.y == 0.0f &&
			anchor.Max.x == 0.0f && anchor.Max.y == 0.0f &&
			anchor.Pivot.x == 0.5f && anchor.Pivot.y == 0.5f &&
			anchor.Offset.x == 0.0f && anchor.Offset.y == 0.0f &&
			anchor.Size.x == 0.0f && anchor.Size.y == 0.0f &&
			!anchor.RelativeToSafeArea;
	}

	bool IsDefaultLayout(const UiLayoutSpec& spec)
	{
		return spec.Kind == UiLayoutKind::Absolute && spec.Gap == 0.0f && spec.Columns == 2 &&
			!spec.RowMajor && !spec.AutoSize &&
			spec.Padding[0] == 0.0f && spec.Padding[1] == 0.0f &&
			spec.Padding[2] == 0.0f && spec.Padding[3] == 0.0f;
	}

	// ---- 滚动(M10)----

	bool IsUiPropertyTruthy(std::string_view value)
	{
		// 与 `UiInputRouter`/`UiPainter` 的 BoolProp 同一真值集合;空串 = 属性缺席。
		return value == "1" || value == "true" || value == "True" || value == "yes" || value == "on";
	}

	bool IsUiScrollContainerProps(std::string_view scrollable, std::string_view scroll, std::string_view overflow)
	{
		if (!scrollable.empty() && IsUiPropertyTruthy(scrollable))
			return true;
		if (!scroll.empty() && IsUiPropertyTruthy(scroll))
			return true;
		return overflow == "scroll" || overflow == "auto" || overflow == "Scroll";
	}

	float ClampUiScrollOffset(float offset, float contentSize, float containerSize)
	{
		// NaN/负值一律归零(不产生反向滚动)。
		if (!(offset > 0.0f))
			return 0.0f;
		const float maxOffset = std::max(contentSize - containerSize, 0.0f);
		return std::min(offset, maxOffset);
	}

	// ---- 主题令牌与样式继承(M14;仅追加)----

	std::string FormatUiTokenChain(const std::vector<std::string>& chain)
	{
		std::string text;
		for (std::size_t i = 0; i < chain.size(); ++i)
		{
			if (i > 0)
				text += " -> ";
			text += kUiTokenPrefix;
			text += chain[i];
		}
		return text;
	}

	bool IsUiTokenRef(std::string_view value)
	{
		return !UiTokenRefId(value).empty();
	}

	std::string_view UiTokenRefId(std::string_view value)
	{
		if (value.size() < 2 || value.front() != kUiTokenPrefix)
			return std::string_view();
		return Trim(value.substr(1));
	}

	UiTokenResult ResolveUiTokenValue(std::string_view value, std::string_view propName,
		const UiTokenFinder& find, int maxDepth)
	{
		UiTokenResult result;

		if (!IsUiTokenRef(value))
		{
			// 字面值原样返回:没有令牌引用的旧文档逐字节行为不变。
			result.Value.assign(value);
			result.Ok = true;
			return result;
		}

		result.WasToken = true;
		const int limit = maxDepth > 0 ? maxDepth : 32;
		std::vector<std::string> chain;
		std::string current(value);

		for (int depth = 0;; ++depth)
		{
			const std::string_view id = UiTokenRefId(current);
			if (id.empty())
			{
				result.Error = "invalid token reference '" + current + "' (expected '$name')";
				return result;
			}
			if (std::find(chain.begin(), chain.end(), std::string(id)) != chain.end())
			{
				// 环:把闭环完整打印出来(如 $a -> $b -> $a)。
				chain.emplace_back(id);
				result.Chain = FormatUiTokenChain(chain);
				result.Error = "token cycle: " + result.Chain;
				return result;
			}
			if (depth >= limit)
			{
				result.Chain = FormatUiTokenChain(chain);
				result.Error = "token reference chain too deep (> " + std::to_string(limit) + "): " + result.Chain;
				return result;
			}

			chain.emplace_back(id);
			const std::string* next = find ? find(id, propName) : nullptr;
			if (next == nullptr)
			{
				result.Chain = FormatUiTokenChain(chain);
				result.Error = "undefined UI token '$" + std::string(id) + "'";
				if (!propName.empty())
					result.Error += " (no value for property '" + std::string(propName) + "')";
				if (chain.size() > 1)
					result.Error += "; referenced from " + result.Chain;
				return result;
			}
			current = *next;
			if (!IsUiTokenRef(current))
			{
				result.Chain = FormatUiTokenChain(chain);
				result.Value = current;
				result.Ok = true;
				return result;
			}
		}
	}
}
