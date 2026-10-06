#include "wldpch.h"
#include "World/WUI/Widgets/WuiPlot.h"

#include "World/WUI/WuiWidgets.h"

#include <algorithm>
#include <cmath>

namespace World::Wui
{
	WuiMeasure WuiPlot::Measure(const WuiConstraints& constraints)
	{
		MarkClean();
		const float h = std::max(constraints.MinH, MinHeight);
		return { constraints.MinW, std::min(h, constraints.MaxH) };
	}

	void WuiPlot::Paint(WuiPaintContext& context)
	{
		if (m_Rect.W <= 0.0f || m_Rect.H <= 0.0f)
			return;

		WuiContext& ctx = context.Context();

		// 固定网格线:横向 3 条、纵向 4 条,不随尺寸变化产生数量抖动
		if (GridColor.A > 0.001f)
		{
			for (int i = 1; i <= 3; ++i)
			{
				const float gy = m_Rect.Y + (m_Rect.H * static_cast<float>(i)) / 4.0f;
				LineSegment(ctx, { m_Rect.X, gy }, { m_Rect.X + m_Rect.W, gy }, GridColor, 1.0f);
			}
			for (int i = 1; i <= 4; ++i)
			{
				const float gx = m_Rect.X + (m_Rect.W * static_cast<float>(i)) / 5.0f;
				LineSegment(ctx, { gx, m_Rect.Y }, { gx, m_Rect.Y + m_Rect.H }, GridColor, 1.0f);
			}
		}

		// 量程定标:
		//  - MinValue <= 0 时取 0;
		//  - MaxValue <= 0 时自动定标为样本最大值(至少 1e-3,防除零);
		//  - 显式传入的 Min/Max 必须被尊重。
		float minVal = (MinValue <= 0.0f) ? 0.0f : MinValue;
		float maxVal = MaxValue;
		if (maxVal <= 0.0f)
		{
			float peak = 0.0f;
			if (Samples && SampleCount > 0)
			{
				for (size_t i = 0; i < SampleCount; ++i)
				{
					if (Samples[i] > peak)
						peak = Samples[i];
				}
			}
			maxVal = std::max(peak, 1e-3f);
		}
		if (maxVal <= minVal)
			maxVal = minVal + 1e-3f;

		// 绘图区边距:留出顶部与底部内衬,使峰值样本落在绘图区顶部附近并留出清晰边距
		float padTop = 6.0f;
		float padBottom = 4.0f;
		if (m_Rect.H < padTop + padBottom + 2.0f)
		{
			padTop = m_Rect.H * 0.1f;
			padBottom = m_Rect.H * 0.1f;
		}
		const float innerTop = m_Rect.Y + padTop;
		const float innerBottom = m_Rect.Y + m_Rect.H - padBottom;
		const float innerH = std::max(1.0f, innerBottom - innerTop);

		auto MapY = [&](float val) -> float {
			float t = (val - minVal) / (maxVal - minVal);
			t = std::max(0.0f, std::min(1.0f, t));
			return innerBottom - t * innerH;
		};

		// ---------------------------------------------------------------------------
		// 序列绘制:按**像素列**做 min/max 抽取(经典波形画法)。
		//
		// 为什么必须抽取:趋势缓冲 600 点,而面板绘图宽度常只有 ~220px
		// ⇒ 每个相邻样本对只有 0.37px 宽。亚像素线段在渲染端会退化成空图元,
		// 于是一条**恒定**的序列(如显存长期 49.6MB)会**完全不显示** —— 看起来像"没数据"。
		// 按列取 [min,max] 同时解决三件事:
		//   1) 恒定序列渲染成连续 1px 线(可见);
		//   2) 尖峰不被抽掉(列内保留极值,不会被下采样抹平);
		//   3) 绘制命令从 O(样本数) 降到 O(列数) —— 600 点到 ~220 条。
		// ---------------------------------------------------------------------------
		const int columns = std::max(1, static_cast<int>(m_Rect.W));
		const auto drawSeries = [&](const float* samples, size_t count, const WuiColor& color,
			bool withFill, const WuiColor& fillColor, float thickness)
		{
			if (!samples || count == 0)
				return;

			// 样本少于像素宽:直接折线,保留最精确的表示。
			if (count <= static_cast<size_t>(columns))
			{
				const float denom = (count > 1) ? static_cast<float>(count - 1) : 1.0f;
				if (count == 1)
				{
					const float y = MapY(samples[0]);
					if (withFill && fillColor.A > 0.001f)
					{
						WuiDrawCommand fillCmd;
						fillCmd.Kind = WuiDrawKind::Quad;
						fillCmd.Color = fillColor;
						fillCmd.Vertices = {
							glm::vec2{ m_Rect.X, y }, glm::vec2{ m_Rect.X + m_Rect.W, y },
							glm::vec2{ m_Rect.X + m_Rect.W, innerBottom }, glm::vec2{ m_Rect.X, innerBottom } };
						ctx.Commands().push_back(std::move(fillCmd));
					}
					if (color.A > 0.001f)
						LineSegment(ctx, { m_Rect.X, y }, { m_Rect.X + m_Rect.W, y }, color, thickness);
					return;
				}
				for (size_t i = 0; i + 1 < count; ++i)
				{
					const float x0 = m_Rect.X + (static_cast<float>(i) / denom) * m_Rect.W;
					const float x1 = m_Rect.X + (static_cast<float>(i + 1) / denom) * m_Rect.W;
					const float y0 = MapY(samples[i]);
					const float y1 = MapY(samples[i + 1]);
					if (withFill && fillColor.A > 0.001f)
					{
						WuiDrawCommand fillCmd;
						fillCmd.Kind = WuiDrawKind::Quad;
						fillCmd.Color = fillColor;
						fillCmd.Vertices = {
							glm::vec2{ x0, y0 }, glm::vec2{ x1, y1 },
							glm::vec2{ x1, innerBottom }, glm::vec2{ x0, innerBottom } };
						ctx.Commands().push_back(std::move(fillCmd));
					}
					if (color.A > 0.001f)
						LineSegment(ctx, { x0, y0 }, { x1, y1 }, color, thickness);
				}
				return;
			}

			// ---------------------------------------------------------------------
			// 两件事分开画,**这是消锯齿的关键**:
			//
			//   A. **连续折线**(每列取该列最后一个样本 —— 保持时序,邻列相连)
			//      ⇒ 曲线是连的,不再是"一列一根竖条"的梳齿。
			//
			//   B. **包络竖条**(该列 min..max),只在跨度肉眼可见时才画
			//      ⇒ 尖峰/噪声范围仍然看得见,但平直数据不会画出一片梳齿。
			//
			// 反例(修之前):每列只画一根竖条 ⇒ 相邻列各自起跳、互不相连,
			// 面板越窄列数越少、竖条越高差越大 ⇒ 看起来就是**锯齿/梳子**。
			// ---------------------------------------------------------------------
			float previousX = 0.0f;
			float previousY = 0.0f;
			bool hasPrevious = false;

			for (int column = 0; column < columns; ++column)
			{
				const size_t begin = (count * static_cast<size_t>(column)) / static_cast<size_t>(columns);
				size_t finish = (count * static_cast<size_t>(column + 1)) / static_cast<size_t>(columns);
				if (finish <= begin)
					finish = begin + 1;
				if (finish > count)
					finish = count;
				if (begin >= count)
					break;

				float low = samples[begin];
				float high = samples[begin];
				for (size_t i = begin + 1; i < finish; ++i)
				{
					if (samples[i] < low) low = samples[i];
					if (samples[i] > high) high = samples[i];
				}

				const float x = m_Rect.X + static_cast<float>(column);
				const float xMid = x + 0.5f;
				const float yHigh = MapY(high);
				const float yLow = std::max(MapY(low), yHigh + 1.0f);
				// 折线走"该列最后一个样本":与真实时序一致(不是 min/max 的中点)。
				const float yTrace = MapY(samples[finish - 1]);

				if (withFill && fillColor.A > 0.001f)
				{
					WuiDrawCommand fillCmd;
					fillCmd.Kind = WuiDrawKind::Quad;
					fillCmd.Color = fillColor;
					fillCmd.Vertices = {
						glm::vec2{ x, yTrace }, glm::vec2{ x + 1.0f, yTrace },
						glm::vec2{ x + 1.0f, innerBottom }, glm::vec2{ x, innerBottom } };
					ctx.Commands().push_back(std::move(fillCmd));
				}

				// B. 包络:跨度 <= 1.5px 时它只是把折线加粗,徒增锯齿感 ⇒ 不画。
				if (color.A > 0.001f && (yLow - yHigh) > 1.5f)
					LineSegment(ctx, { xMid, yHigh }, { xMid, yLow }, color, 1.0f);

				// A. 连续折线。
				if (color.A > 0.001f)
				{
					if (hasPrevious)
						LineSegment(ctx, { previousX, previousY }, { xMid, yTrace }, color, thickness);
					previousX = xMid;
					previousY = yTrace;
					hasPrevious = true;
				}
			}
		};

		// 第二序列先画(不填充),主序列压在上面。
		drawSeries(Samples2, SampleCount2, LineColor2, FillSecond, FillColor2, 1.0f);

		if (Style == Kind::Line)
		{
			drawSeries(Samples, SampleCount, LineColor, true, FillColor, 1.5f);
		}
		else if (Style == Kind::Bars && Samples && SampleCount > 0)
		{
			const float slotW = m_Rect.W / static_cast<float>(SampleCount);
			const float barW = (slotW > 2.0f) ? (slotW - 1.0f) : slotW;
			for (size_t i = 0; i < SampleCount; ++i)
			{
				const float barX = m_Rect.X + static_cast<float>(i) * slotW;
				const float y = MapY(Samples[i]);
				const float barH = std::max(0.0f, innerBottom - y);
				if (FillColor.A > 0.001f && barH > 0.0f)
					ctx.Commands().push_back({ WuiDrawKind::Rect, { barX, y, barW, barH }, FillColor });
				if (LineColor.A > 0.001f && barW > 0.0f)
					ctx.Commands().push_back({ WuiDrawKind::Rect, { barX, y, barW, std::min(1.5f, barH > 0.0f ? barH : 1.0f) }, LineColor });
			}
		}

		// 参考线(p50/p95/p99 这类标记)。**落在量程之外的直接跳过**:
		// 画到边缘会让人误读成"刚好到顶/到底",那是假信息。
		for (uint32_t i = 0; i < GuideCount && i < kMaxGuides; ++i)
		{
			const Guide& guide = Guides[i];
			if (guide.Color.A <= 0.001f)
				continue;
			if (guide.Value < minVal || guide.Value > maxVal)
				continue;
			const float guideY = MapY(guide.Value);
			LineSegment(ctx, { m_Rect.X, guideY }, { m_Rect.X + m_Rect.W, guideY }, guide.Color, 1.0f);
		}

		// 告警阈值水平线(WarnThreshold >= 0 时绘制)
		if (WarnThreshold >= 0.0f && ThresholdColor.A > 0.001f)
		{
			const float warnY = MapY(WarnThreshold);
			LineSegment(ctx, { m_Rect.X, warnY }, { m_Rect.X + m_Rect.W, warnY }, ThresholdColor, 1.0f);
		}
	}
}
