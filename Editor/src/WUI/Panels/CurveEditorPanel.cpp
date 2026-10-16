#include "wldpch.h"
#include "WUI/Panels/CurveEditorPanel.h"
#include "World/WUI/WuiWidgets.h"
#include <algorithm>
#include <cmath>
#include <cstdio>

namespace World
{
	CurveEditorPanel::CurveEditorPanel()
		: m_PresetOptions({ "Linear", "EaseIn", "EaseOut", "EaseInOut", "Bounce" })
	{
		m_Curve = CubicBezierCurve::EaseInOut();
	}

	void CurveEditorPanel::OnRender(Wui::WuiContext& ctx, const Wui::WuiRect& rect, PanelHost& host)
	{
		const Wui::WuiTheme& theme = host.Theme();
		Wui::WuiId baseId = 0x510000;

		float y = rect.Y + 8.0f;

		// 标题
		Wui::Label(ctx, { rect.X + 12.0f, y }, "Curve Editor", theme.Text, theme.FontSizeTitle);
		y += 28.0f;

		// 预设分段选择器
		Wui::Label(ctx, { rect.X + 12.0f, y + 4.0f }, "Preset:", theme.TextMuted, theme.FontSizeBody);
		Wui::WuiRect presetRect { rect.X + 72.0f, y, std::min(rect.W - 84.0f, 360.0f), 24.0f };
		if (Wui::Segmented(ctx, baseId++, presetRect, m_PresetOptions, m_SelectedPreset, theme))
		{
			switch (m_SelectedPreset)
			{
			case 0: m_Curve = CubicBezierCurve::Linear(); break;
			case 1: m_Curve = CubicBezierCurve::EaseIn(); break;
			case 2: m_Curve = CubicBezierCurve::EaseOut(); break;
			case 3: m_Curve = CubicBezierCurve::EaseInOut(); break;
			case 4: m_Curve = CubicBezierCurve::Bounce(); break;
			default: break;
			}
			m_SelectedKeyframeIndex = 0;
		}
		y += 32.0f;

		// 画布区域 (网格 + 贝塞尔曲线 + 关键帧点)
		const float canvasMarginX = 12.0f;
		const float canvasWidth = std::max(rect.W - canvasMarginX * 2.0f, 100.0f);
		const float canvasHeight = 160.0f;
		Wui::WuiRect canvasRect { rect.X + canvasMarginX, y, canvasWidth, canvasHeight };

		// 外边框与参考网格线
		Wui::LineSegment(ctx, { canvasRect.X, canvasRect.Y }, { canvasRect.X + canvasRect.W, canvasRect.Y }, theme.Border, 1.0f);
		Wui::LineSegment(ctx, { canvasRect.X, canvasRect.Y + canvasRect.H }, { canvasRect.X + canvasRect.W, canvasRect.Y + canvasRect.H }, theme.Border, 1.0f);
		Wui::LineSegment(ctx, { canvasRect.X, canvasRect.Y }, { canvasRect.X, canvasRect.Y + canvasRect.H }, theme.Border, 1.0f);
		Wui::LineSegment(ctx, { canvasRect.X + canvasRect.W, canvasRect.Y }, { canvasRect.X + canvasRect.W, canvasRect.Y + canvasRect.H }, theme.Border, 1.0f);

		// 水平参考线 (0.25, 0.5, 0.75)
		for (float frac : { 0.25f, 0.5f, 0.75f })
		{
			float lineY = canvasRect.Y + (1.0f - frac) * canvasRect.H;
			Wui::LineSegment(ctx, { canvasRect.X, lineY }, { canvasRect.X + canvasRect.W, lineY }, theme.Border, 1.0f);
		}
		// 垂直参考线 (0.25, 0.5, 0.75)
		for (float frac : { 0.25f, 0.5f, 0.75f })
		{
			float lineX = canvasRect.X + frac * canvasRect.W;
			Wui::LineSegment(ctx, { lineX, canvasRect.Y }, { lineX, canvasRect.Y + canvasRect.H }, theme.Border, 1.0f);
		}

		// 采样绘制贝塞尔曲线线段
		constexpr int kCurveSegments = 40;
		glm::vec2 prevPt { canvasRect.X, canvasRect.Y + (1.0f - std::clamp(m_Curve.Evaluate(0.0f), 0.0f, 1.0f)) * canvasRect.H };
		for (int seg = 1; seg <= kCurveSegments; ++seg)
		{
			float t = static_cast<float>(seg) / static_cast<float>(kCurveSegments);
			float val = m_Curve.Evaluate(t);
			glm::vec2 currPt {
				canvasRect.X + t * canvasRect.W,
				canvasRect.Y + (1.0f - std::clamp(val, 0.0f, 1.0f)) * canvasRect.H
			};
			Wui::LineSegment(ctx, prevPt, currPt, theme.Accent, 2.0f);
			prevPt = currPt;
		}

		// 绘制关键帧控制点
		for (size_t k = 0; k < m_Curve.KeyframeCount(); ++k)
		{
			const auto& kf = m_Curve.GetKeyframe(k);
			float ptX = canvasRect.X + std::clamp(kf.Time, 0.0f, 1.0f) * canvasRect.W;
			float ptY = canvasRect.Y + (1.0f - std::clamp(kf.Value, 0.0f, 1.0f)) * canvasRect.H;

			Wui::WuiColor pointCol = (static_cast<int>(k) == m_SelectedKeyframeIndex) ? theme.Warning : theme.Text;
			// 绘制十字标记 (4px)
			Wui::LineSegment(ctx, { ptX - 4.0f, ptY }, { ptX + 4.0f, ptY }, pointCol, 2.0f);
			Wui::LineSegment(ctx, { ptX, ptY - 4.0f }, { ptX, ptY + 4.0f }, pointCol, 2.0f);
		}

		// 当前求值预览线
		float previewX = canvasRect.X + std::clamp(m_PreviewT, 0.0f, 1.0f) * canvasRect.W;
		m_EvaluatedValue = m_Curve.Evaluate(m_PreviewT);
		float previewY = canvasRect.Y + (1.0f - std::clamp(m_EvaluatedValue, 0.0f, 1.0f)) * canvasRect.H;
		Wui::LineSegment(ctx, { previewX, canvasRect.Y }, { previewX, canvasRect.Y + canvasRect.H }, theme.Success, 1.0f);
		Wui::LineSegment(ctx, { previewX - 3.0f, previewY }, { previewX + 3.0f, previewY }, theme.Success, 2.0f);

		y += canvasHeight + 16.0f;

		// 曲线求值预览滑块与读数
		Wui::Label(ctx, { rect.X + 12.0f, y + 4.0f }, "Evaluate T:", theme.TextMuted, theme.FontSizeBody);
		Wui::WuiRect sliderRect { rect.X + 96.0f, y, 160.0f, 24.0f };
		Wui::SliderFloat(ctx, baseId++, sliderRect, m_PreviewT, 0.0f, 1.0f, theme);

		char evalBuf[32];
		std::snprintf(evalBuf, sizeof(evalBuf), "f(%.2f) = %.3f", m_PreviewT, m_EvaluatedValue);
		Wui::Label(ctx, { rect.X + 268.0f, y + 4.0f }, evalBuf, theme.Text, theme.FontSizeBody);
		y += 32.0f;

		// 关键帧属性调整
		if (m_Curve.KeyframeCount() > 0)
		{
			if (m_SelectedKeyframeIndex >= static_cast<int>(m_Curve.KeyframeCount()))
			{
				m_SelectedKeyframeIndex = static_cast<int>(m_Curve.KeyframeCount()) - 1;
			}

			auto& selectedKf = m_Curve.GetKeyframeMut(m_SelectedKeyframeIndex);
			char kfTitle[64];
			std::snprintf(kfTitle, sizeof(kfTitle), "Keyframe #%d Properties:", m_SelectedKeyframeIndex + 1);
			Wui::Label(ctx, { rect.X + 12.0f, y + 4.0f }, kfTitle, theme.Text, theme.FontSizeBody);

			Wui::WuiRect prevBtnRect { rect.X + 220.0f, y, 36.0f, 24.0f };
			if (Wui::Button(ctx, baseId++, prevBtnRect, "<", theme))
			{
				if (m_SelectedKeyframeIndex > 0) m_SelectedKeyframeIndex--;
			}
			Wui::WuiRect nextBtnRect { rect.X + 260.0f, y, 36.0f, 24.0f };
			if (Wui::Button(ctx, baseId++, nextBtnRect, ">", theme))
			{
				if (m_SelectedKeyframeIndex + 1 < static_cast<int>(m_Curve.KeyframeCount())) m_SelectedKeyframeIndex++;
			}
			y += 28.0f;

			// Keyframe Time & Value
			Wui::Label(ctx, { rect.X + 12.0f, y + 4.0f }, "Time:", theme.TextMuted, theme.FontSizeSmall);
			Wui::WuiRect timeSliderRect { rect.X + 60.0f, y, 120.0f, 24.0f };
			Wui::SliderFloat(ctx, baseId++, timeSliderRect, selectedKf.Time, 0.0f, 1.0f, theme);

			Wui::Label(ctx, { rect.X + 190.0f, y + 4.0f }, "Value:", theme.TextMuted, theme.FontSizeSmall);
			Wui::WuiRect valSliderRect { rect.X + 240.0f, y, 120.0f, 24.0f };
			Wui::SliderFloat(ctx, baseId++, valSliderRect, selectedKf.Value, 0.0f, 1.0f, theme);
			y += 30.0f;

			// InTangent & OutTangent
			Wui::Label(ctx, { rect.X + 12.0f, y + 4.0f }, "InTan:", theme.TextMuted, theme.FontSizeSmall);
			Wui::WuiRect inTanRect { rect.X + 60.0f, y, 120.0f, 24.0f };
			Wui::SliderFloat(ctx, baseId++, inTanRect, selectedKf.InTangent, -4.0f, 4.0f, theme);

			Wui::Label(ctx, { rect.X + 190.0f, y + 4.0f }, "OutTan:", theme.TextMuted, theme.FontSizeSmall);
			Wui::WuiRect outTanRect { rect.X + 240.0f, y, 120.0f, 24.0f };
			Wui::SliderFloat(ctx, baseId++, outTanRect, selectedKf.OutTangent, -4.0f, 4.0f, theme);
		}
	}
}
