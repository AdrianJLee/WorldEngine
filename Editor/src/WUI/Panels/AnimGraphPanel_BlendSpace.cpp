#include "WUI/Panels/AnimGraphPanel.h"

#include "World/WUI/WuiAccessibility.h"
#include "World/WUI/WuiLocalization.h"
#include "World/WUI/Widgets/WuiChrome.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <initializer_list>
#include <string_view>
#include <utility>

namespace World
{
	namespace
	{
		constexpr float kRowH = 24.0f;
		constexpr float kSampleRowH = 28.0f;
		constexpr float kCanvasPadLeft = 46.0f;
		constexpr float kCanvasPadBottom = 22.0f;

		std::string AGTr(const char* key, const char* fallback)
		{
			return Wui::Tr(std::string("panel.animgraph.") + key, fallback);
		}

		std::string AGFmt(const char* key, const char* fallback,
			std::initializer_list<std::pair<std::string_view, std::string_view>> args = {})
		{
			return Wui::TrFormat(std::string("panel.animgraph.") + key, fallback,
				std::vector<std::pair<std::string_view, std::string_view>>(args));
		}

		struct BlendMap
		{
			Wui::WuiRect Plot;
			float MinX = 0.0f;
			float MaxX = 6.0f;
			float MinY = -180.0f;
			float MaxY = 180.0f;
			bool TwoD = true;

			glm::vec2 ToScreen(float x, float y) const
			{
				const float tx = (x - MinX) / std::max(0.0001f, MaxX - MinX);
				const float ty = (y - MinY) / std::max(0.0001f, MaxY - MinY);
				return { Plot.X + std::clamp(tx, 0.0f, 1.0f) * Plot.W,
					Plot.Y + Plot.H - std::clamp(ty, 0.0f, 1.0f) * Plot.H };
			}

			glm::vec2 ToParam(glm::vec2 screen) const
			{
				const float tx = (screen.x - Plot.X) / std::max(1.0f, Plot.W);
				const float ty = 1.0f - (screen.y - Plot.Y) / std::max(1.0f, Plot.H);
				return { MinX + std::clamp(tx, 0.0f, 1.0f) * (MaxX - MinX),
					MinY + std::clamp(ty, 0.0f, 1.0f) * (MaxY - MinY) };
			}
		};

		Wui::WuiColor WithAlpha(Wui::WuiColor color, float alpha)
		{
			color.A = alpha;
			return color;
		}

		BlendMap MakeBlendMap(const Wui::WuiRect& canvas, const AnimState& state, bool twoD,
			float paramX, float paramY)
		{
			BlendMap map;
			map.TwoD = twoD;
			map.Plot = { canvas.X + kCanvasPadLeft, canvas.Y + 8.0f,
				std::max(40.0f, canvas.W - kCanvasPadLeft - 8.0f),
				std::max(40.0f, canvas.H - kCanvasPadBottom - 8.0f) };

			float maxX = twoD ? 6.0f : 4.0f;
			float maxY = 180.0f;
			float minX = 0.0f;
			if (twoD)
			{
				for (const BlendSample2D& sample : state.Blend2D.GetSamples())
				{
					maxX = std::max(maxX, sample.Point.x * 1.15f);
					minX = std::min(minX, sample.Point.x);
					maxY = std::max(maxY, std::abs(sample.Point.y));
				}
				maxX = std::max(maxX, paramX * 1.10f);
				maxY = std::max(maxY, std::abs(paramY));
			}
			else
			{
				for (const BlendSample1D& sample : state.Blend1D.GetSamples())
					maxX = std::max(maxX, sample.Value * 1.15f);
				maxX = std::max(maxX, paramX * 1.10f);
			}

			map.MinX = std::min(0.0f, minX);
			map.MaxX = std::max(map.MinX + 0.5f, maxX);
			map.MinY = -std::max(45.0f, maxY);
			map.MaxY = std::max(45.0f, maxY);
			return map;
		}


	}

	// ---------------------------------------------------------------------------
	// 2D 剖分可视化:三角形来自 BlendSpace2D::GetTriangles(引擎侧 Delaunay 的唯一实现),
	// 面板不自己再算一遍 —— 连线与运行时权重必须同源。空/单点/共线时引擎返回空表,
	// 画布显示对应的空态文案。
	// ---------------------------------------------------------------------------

	void AnimGraphPanel::RebuildTriangulation()
	{
		m_TrianglesDirty = false;
		m_Triangles.clear();

		const AnimState* state = SelectedState();
		if (state == nullptr || state->Kind != AnimStateKind::BlendSpace2D)
			return;

		// 单一事实源:三角形直接取引擎 Delaunay 的结果(BlendSpace2D::GetTriangles),
		// 面板只负责画线 —— 可视化连线与运行时混合用的必须是同一份剖分。
		for (const BlendTriangle2D& triangle : state->Blend2D.GetTriangles())
		{
			m_Triangles.push_back({ static_cast<int>(triangle.A), static_cast<int>(triangle.B),
				static_cast<int>(triangle.C) });
		}
	}

	// ---------------------------------------------------------------------------
	// 画布绘制
	// ---------------------------------------------------------------------------

	void AnimGraphPanel::DrawBlendCanvas(Wui::WuiContext& ctx, const Wui::WuiRect& canvas, PanelHost& host,
		const Wui::WuiTheme& theme, const AnimState& state, bool blend2d)
	{
		(void)host;
		Wui::PanelBackground(ctx, canvas, theme.ContentBg, theme.Radius);
		const BlendMap map = MakeBlendMap(canvas, state, blend2d, m_ParamSpeed, m_ParamDirection);

		Wui::PanelBackground(ctx, map.Plot, theme.PanelBg, 2.0f);

		// 低饱和网格 + 刻度
		const int xDivisions = 6;
		const int yDivisions = blend2d ? 4 : 2;
		for (int i = 0; i <= xDivisions; ++i)
		{
			const float x = map.Plot.X + map.Plot.W * static_cast<float>(i) / static_cast<float>(xDivisions);
			Wui::LineSegment(ctx, { x, map.Plot.Y }, { x, map.Plot.Y + map.Plot.H },
				WithAlpha(theme.Border, 0.55f), 1.0f);
			const float value = map.MinX + (map.MaxX - map.MinX) * static_cast<float>(i)
				/ static_cast<float>(xDivisions);
			char label[32] = {};
			std::snprintf(label, sizeof(label), "%.1f", static_cast<double>(value));
			Wui::Label(ctx, { x - 9.0f, map.Plot.Y + map.Plot.H + 3.0f }, label, theme.TextMuted,
				theme.FontSizeCaption);
		}
		for (int i = 0; i <= yDivisions; ++i)
		{
			const float y = map.Plot.Y + map.Plot.H * static_cast<float>(i) / static_cast<float>(yDivisions);
			Wui::LineSegment(ctx, { map.Plot.X, y }, { map.Plot.X + map.Plot.W, y },
				WithAlpha(theme.Border, 0.55f), 1.0f);
			if (blend2d)
			{
				const float value = map.MaxY - (map.MaxY - map.MinY) * static_cast<float>(i)
					/ static_cast<float>(yDivisions);
				char label[32] = {};
				std::snprintf(label, sizeof(label), "%.0f", static_cast<double>(value));
				Wui::Label(ctx, { map.Plot.X - 34.0f, y - 7.0f }, label, theme.TextMuted, theme.FontSizeCaption);
			}
		}

		// 轴线
		Wui::LineSegment(ctx, { map.Plot.X, map.Plot.Y + map.Plot.H },
			{ map.Plot.X + map.Plot.W, map.Plot.Y + map.Plot.H }, theme.BorderStrong, 1.0f);
		Wui::LineSegment(ctx, { map.Plot.X, map.Plot.Y }, { map.Plot.X, map.Plot.Y + map.Plot.H },
			theme.BorderStrong, 1.0f);

		Wui::Label(ctx, { map.Plot.X + map.Plot.W - 74.0f, map.Plot.Y - 1.0f }, AGTr("blend.axis.speed", "Speed (m/s)"),
			theme.TextMuted, theme.FontSizeCaption);
		if (blend2d)
			Wui::Label(ctx, { map.Plot.X + 4.0f, map.Plot.Y + 2.0f }, AGTr("blend.axis.direction", "Direction (deg)"), theme.TextMuted,
				theme.FontSizeCaption);

		const std::vector<BlendSample2D>& samples2d = state.Blend2D.GetSamples();
		const std::vector<BlendSample1D>& samples1d = state.Blend1D.GetSamples();
		const size_t sampleCount = blend2d ? samples2d.size() : samples1d.size();

		// 无障碍:画布节点在空态也要登记(自绘内容必须始终可读)。
		{
			Wui::WuiAccessNode canvasNode;
			canvasNode.Id = Wui::HashId("animgraph.blend.canvas");
			canvasNode.Window = Wui::WuiAccessibility::Get().CurrentWindow();
			canvasNode.Panel = Wui::WuiAccessibility::Get().CurrentPanel();
			canvasNode.Kind = "blend-space-canvas";
			canvasNode.Label = blend2d ? AGTr("blend.node.2d", "Blend Space 2D") : AGTr("blend.node.1d", "Blend Space 1D");
			canvasNode.Value = "samples=" + std::to_string(sampleCount)
				+ " triangles=" + std::to_string(blend2d ? m_Triangles.size() : 0)
				+ " paramX=" + state.ParameterX + (blend2d ? (" paramY=" + state.ParameterY) : std::string());
			canvasNode.Rect = canvas;
			canvasNode.Interactive = false;
			Wui::WuiAccessibility::Get().Register(canvasNode);
		}

		if (sampleCount == 0)
		{
			Wui::Label(ctx, { map.Plot.X + 12.0f, map.Plot.Y + map.Plot.H * 0.5f - 8.0f },
				AGTr("blend.empty", "No samples yet - use + Add Sample below."), theme.TextMuted, theme.FontSizeSmall);
			return;
		}
		if (blend2d && m_TrianglesDirty)
			RebuildTriangulation();

		// Delaunay 连线(只画索引缓存里的三角形;拖动期间故意保持旧连线,松手才重算)
		if (blend2d && !samples2d.empty() && m_Triangles.empty())
		{
			const std::string hint = samples2d.size() < 3
				? AGFmt("blend.need_three", "Add at least 3 samples to form blend triangles (currently {count}).",
					{ { "count", std::to_string(samples2d.size()) } })
				: AGTr("blend.collinear", "Samples are collinear - add a point off the line to form triangles.");
			Wui::Label(ctx, { map.Plot.X + 12.0f, map.Plot.Y + map.Plot.H * 0.5f - 8.0f }, hint,
				theme.Warning, theme.FontSizeSmall);
		}
		if (blend2d)
		{
			for (const std::array<int, 3>& tri : m_Triangles)
			{
				if (tri[0] < 0 || tri[1] < 0 || tri[2] < 0
					|| static_cast<size_t>(tri[0]) >= samples2d.size()
					|| static_cast<size_t>(tri[1]) >= samples2d.size()
					|| static_cast<size_t>(tri[2]) >= samples2d.size())
					continue;
				const glm::vec2 a = map.ToScreen(samples2d[static_cast<size_t>(tri[0])].Point.x,
					samples2d[static_cast<size_t>(tri[0])].Point.y);
				const glm::vec2 b = map.ToScreen(samples2d[static_cast<size_t>(tri[1])].Point.x,
					samples2d[static_cast<size_t>(tri[1])].Point.y);
				const glm::vec2 c = map.ToScreen(samples2d[static_cast<size_t>(tri[2])].Point.x,
					samples2d[static_cast<size_t>(tri[2])].Point.y);
				const Wui::WuiColor line = WithAlpha(theme.Accent, 0.30f);
				Wui::LineSegment(ctx, a, b, line, 1.0f);
				Wui::LineSegment(ctx, b, c, line, 1.0f);
				Wui::LineSegment(ctx, c, a, line, 1.0f);
			}
		}
		else
		{
			const float centerY = map.Plot.Y + map.Plot.H * 0.5f;
			Wui::LineSegment(ctx, { map.Plot.X, centerY }, { map.Plot.X + map.Plot.W, centerY },
				WithAlpha(theme.Accent, 0.28f), 1.0f);
		}

		// 样本点(选中 = Accent 实心;悬停 = Accent 描边;其余 = 面板色 + 强描边)
		for (size_t i = 0; i < sampleCount; ++i)
		{
			const float px = blend2d ? samples2d[i].Point.x : samples1d[i].Value;
			const float py = blend2d ? samples2d[i].Point.y : 0.0f;
			const glm::vec2 screen = map.ToScreen(px, py);
			const bool selected = static_cast<int>(i) == m_SelectedSample;
			const bool hovered = static_cast<int>(i) == m_HoverSample;
			const float size = selected || hovered ? 11.0f : 9.0f;
			const Wui::WuiRect dot { screen.x - size * 0.5f, screen.y - size * 0.5f, size, size };
			Wui::PanelBackground(ctx, dot, selected ? theme.Accent : theme.CardBg, 2.0f);
			Wui::HighlightOutline(ctx, dot, selected ? theme.Accent : theme.BorderStrong, 2.0f, 1.0f);

			const std::string& clip = blend2d ? samples2d[i].ClipName : samples1d[i].ClipName;
			if (!clip.empty())
			{
				const float labelW = ctx.MeasureTextWidth(clip, theme.FontSizeCaption);
				float labelX = screen.x + 8.0f;
				if (labelX + labelW > map.Plot.X + map.Plot.W)
					labelX = screen.x - 8.0f - labelW;
				const float labelY = screen.y - theme.FontSizeCaption - 3.0f < map.Plot.Y
					? screen.y + 5.0f
					: screen.y - theme.FontSizeCaption - 3.0f;
				Wui::Label(ctx, { labelX, labelY }, clip, selected ? theme.Text : theme.TextMuted,
					theme.FontSizeCaption);
			}
		}

		// 当前参数十字准星(实时区滑杆驱动)
		{
			const float px = std::clamp(m_ParamSpeed, map.MinX, map.MaxX);
			const float py = blend2d ? std::clamp(m_ParamDirection, map.MinY, map.MaxY) : 0.0f;
			const glm::vec2 cross = map.ToScreen(px, py);
			const Wui::WuiColor crossColor = WithAlpha(theme.Info, 0.75f);
			Wui::LineSegment(ctx, { cross.x, map.Plot.Y }, { cross.x, map.Plot.Y + map.Plot.H },
				crossColor, 1.0f);
			if (blend2d)
				Wui::LineSegment(ctx, { map.Plot.X, cross.y }, { map.Plot.X + map.Plot.W, cross.y },
					crossColor, 1.0f);
			Wui::PanelBackground(ctx, { cross.x - 3.0f, cross.y - 3.0f, 6.0f, 6.0f }, theme.Info, 3.0f);

			char readout[96] = {};
			if (blend2d)
				std::snprintf(readout, sizeof(readout),
					AGTr("blend.readout.2d", "Speed %.2f m/s   Direction %.0f deg").c_str(),
					static_cast<double>(m_ParamSpeed), static_cast<double>(m_ParamDirection));
			else
				std::snprintf(readout, sizeof(readout),
					AGTr("blend.readout.1d", "Speed %.2f m/s").c_str(), static_cast<double>(m_ParamSpeed));
			Wui::Label(ctx, { map.Plot.X + 6.0f, map.Plot.Y + map.Plot.H - 16.0f }, readout,
				theme.TextMuted, theme.FontSizeCaption);
		}

		// 无障碍:每个样本点(画布节点已在函数开头登记)。
		for (size_t i = 0; i < sampleCount; ++i)
		{
			const std::string& clip = blend2d ? samples2d[i].ClipName : samples1d[i].ClipName;
			Wui::WuiAccessNode node;
			node.Id = Wui::HashId(("animgraph.blend.sample." + std::to_string(i)).c_str());
			node.Window = Wui::WuiAccessibility::Get().CurrentWindow();
			node.Panel = Wui::WuiAccessibility::Get().CurrentPanel();
			node.Kind = "blend-sample";
			node.Label = clip.empty()
				? AGFmt("blend.sample.label", "Sample {index}", { { "index", std::to_string(i) } }) : clip;
			char value[96] = {};
			if (blend2d)
				std::snprintf(value, sizeof(value), "x=%.2f y=%.1f", static_cast<double>(samples2d[i].Point.x),
					static_cast<double>(samples2d[i].Point.y));
			else
				std::snprintf(value, sizeof(value), "x=%.2f", static_cast<double>(samples1d[i].Value));
			node.Value = value;
			node.Rect = { map.Plot.X, map.Plot.Y, map.Plot.W, map.Plot.H };
			node.Interactive = false;
			Wui::WuiAccessibility::Get().Register(node);
		}
	}

	// ---------------------------------------------------------------------------
	// 拖拽(实时回写;松手才重算三角剖分)
	// ---------------------------------------------------------------------------

	void AnimGraphPanel::UpdateBlendDrag(Wui::WuiContext& ctx, const Wui::WuiRect& canvas,
		const std::string& stateName, bool blend2d)
	{
		const AnimState* state = m_Graph.FindState(stateName);
		if (state == nullptr)
			return;
		const BlendMap map = MakeBlendMap(canvas, *state, blend2d, m_ParamSpeed, m_ParamDirection);

		const std::vector<BlendSample2D>& samples2d = state->Blend2D.GetSamples();
		const std::vector<BlendSample1D>& samples1d = state->Blend1D.GetSamples();
		const size_t sampleCount = blend2d ? samples2d.size() : samples1d.size();
		const glm::vec2 mouse = ctx.Input().MousePos;

		m_HoverSample = -1;
		for (size_t i = 0; i < sampleCount; ++i)
		{
			const glm::vec2 screen = map.ToScreen(blend2d ? samples2d[i].Point.x : samples1d[i].Value,
				blend2d ? samples2d[i].Point.y : 0.0f);
			if (glm::length(screen - mouse) <= 9.0f)
			{
				m_HoverSample = static_cast<int>(i);
				break;
			}
		}

		if (ctx.IsHovered(map.Plot) && ctx.ClipAllows(canvas))
		{
			if (m_HoverSample >= 0)
				ctx.SetCursor(Wui::WuiCursor::Hand);

			if (ctx.Input().MouseClicked[0])
			{
				m_SelectedSample = m_HoverSample;
				if (m_HoverSample >= 0)
				{
					m_DragSample = m_HoverSample;
					m_DragChanged = false;
				}
			}
		}

		if (m_DragSample >= 0 && ctx.Input().MouseDown[0])
		{
			ctx.SetCursor(Wui::WuiCursor::ResizeNWSE);
			const glm::vec2 param = map.ToParam(mouse);
			const float x = std::clamp(std::round(param.x / 0.05f) * 0.05f, map.MinX, map.MaxX);
			const float y = std::clamp(std::round(param.y), map.MinY, map.MaxY);

			AnimState updated = *state;
			if (blend2d)
			{
				if (static_cast<size_t>(m_DragSample) < updated.Blend2D.GetSamples().size())
				{
					const std::string clip = updated.Blend2D.GetSamples()[static_cast<size_t>(m_DragSample)].ClipName;
					BlendSpace2D rebuilt;
					const auto& current = updated.Blend2D.GetSamples();
					for (size_t i = 0; i < current.size(); ++i)
					{
						if (static_cast<int>(i) == m_DragSample)
							rebuilt.AddSample({ x, y }, clip);
						else
							rebuilt.AddSample(current[i].Point, current[i].ClipName);
					}
					updated.Blend2D = rebuilt;
				}
			}
			else if (static_cast<size_t>(m_DragSample) < updated.Blend1D.GetSamples().size())
			{
				const std::string clip = updated.Blend1D.GetSamples()[static_cast<size_t>(m_DragSample)].ClipName;
				BlendSpace1D rebuilt;
				const auto& current = updated.Blend1D.GetSamples();
				for (size_t i = 0; i < current.size(); ++i)
				{
					if (static_cast<int>(i) == m_DragSample)
						rebuilt.AddSample(x, clip);
					else
						rebuilt.AddSample(current[i].Value, current[i].ClipName);
				}
				updated.Blend1D = rebuilt;
			}

			ReplaceState(stateName, updated);
			// 拖动期间保持旧三角剖分(不每帧重算,派工 §3.2),松手再置脏。
			m_TrianglesDirty = false;
			m_DragChanged = true;
		}

		if (m_DragSample >= 0 && !ctx.Input().MouseDown[0])
		{
			m_DragSample = -1;
			if (m_DragChanged)
				m_TrianglesDirty = true;
			m_DragChanged = false;
		}
	}

	// ---------------------------------------------------------------------------
	// 混合空间卡片(画布 + 参数名)
	// ---------------------------------------------------------------------------

	void AnimGraphPanel::RenderBlendSpaceCard(Wui::WuiContext& ctx, const Wui::WuiRect& area, PanelHost& host,
		const Wui::WuiTheme& theme)
	{
		Wui::PanelBackground(ctx, area, theme.CardBg, theme.Radius);
		const AnimState* state = SelectedState();
		if (state == nullptr || state->Kind == AnimStateKind::SingleClip)
			return;

		const bool blend2d = state->Kind == AnimStateKind::BlendSpace2D;
		Wui::SectionHeader(ctx, { area.X + theme.Pad, area.Y + 2.0f, area.W - theme.Pad * 2.0f, kRowH },
			blend2d ? AGTr("blend.node.2d", "Blend Space 2D") : AGTr("blend.node.1d", "Blend Space 1D"), theme.Text, theme, theme.FontSizeTitle);
		if (area.W > 430.0f)
			Wui::Label(ctx, { area.X + area.W - 214.0f, area.Y + 7.0f },
				AGTr("blend.hint", "Drag points - release to retriangulate"), theme.TextDisabled, theme.FontSizeCaption);

		const Wui::WuiRect canvas { area.X + theme.Pad, area.Y + kRowH + 2.0f,
			area.W - theme.Pad * 2.0f, 260.0f };
		m_CanvasRect = canvas;

		const std::string stateName = state->Name;
		UpdateBlendDrag(ctx, canvas, stateName, blend2d);
		state = m_Graph.FindState(stateName);
		if (state == nullptr)
			return;
		DrawBlendCanvas(ctx, canvas, host, theme, *state, blend2d);

		// 参数名(X / Y)。先快照:TextField 提交会触发 ReplaceState,不能在其后继续解引用旧指针。
		const std::string paramX = state->ParameterX;
		const std::string paramY = state->ParameterY;
		float y = canvas.Y + canvas.H + 4.0f;
		const float labelW = 24.0f;
		const float fieldW = std::max(60.0f, (area.W - theme.Pad * 2.0f - labelW - theme.PadSmall) * 0.5f);
		const Wui::WuiRect xField { area.X + theme.Pad + labelW, y, fieldW, kRowH };
		const Wui::WuiRect yField { xField.X + fieldW + theme.PadSmall, y, fieldW, kRowH };

		Wui::Label(ctx, { area.X + theme.Pad, y + 4.0f }, "X", theme.TextMuted, theme.FontSizeCaption);
		std::string xBuffer = paramX;
		Wui::TextFieldA11y xA11y { AGTr("blend.param.x.a11y", "Blend X parameter"), "Speed" };
		const Wui::WuiId xId = Wui::HashId("animgraph.blend.param.x");
		if (Wui::TextField(ctx, xId, xField, xBuffer, theme, nullptr, &xA11y) && xBuffer != paramX)
		{
			const AnimState* live = m_Graph.FindState(stateName);
			if (live != nullptr)
			{
				AnimState updated = *live;
				updated.ParameterX = xBuffer;
				ReplaceState(stateName, updated);
			}
		}
		Wui::Tooltip(ctx, xField, AGTr("blend.param.x.tooltip", "Parameter driving the horizontal axis (default Speed)."));

		if (blend2d)
		{
			Wui::Label(ctx, { yField.X - 14.0f, y + 4.0f }, "Y", theme.TextMuted, theme.FontSizeCaption);
			std::string yBuffer = paramY;
			Wui::TextFieldA11y yA11y { AGTr("blend.param.y.a11y", "Blend Y parameter"), "Direction" };
			const Wui::WuiId yId = Wui::HashId("animgraph.blend.param.y");
			if (Wui::TextField(ctx, yId, yField, yBuffer, theme, nullptr, &yA11y) && yBuffer != paramY)
			{
				const AnimState* live = m_Graph.FindState(stateName);
				if (live != nullptr)
				{
					AnimState updated = *live;
					updated.ParameterY = yBuffer;
					ReplaceState(stateName, updated);
				}
			}
			Wui::Tooltip(ctx, yField, AGTr("blend.param.y.tooltip", "Parameter driving the vertical axis (default Direction, degrees)."));
		}
	}
	// ---------------------------------------------------------------------------
	// 样本表格
	// ---------------------------------------------------------------------------

	void AnimGraphPanel::RenderSamplesCard(Wui::WuiContext& ctx, const Wui::WuiRect& area, PanelHost& host,
		const Wui::WuiTheme& theme)
	{
		(void)host;
		Wui::PanelBackground(ctx, area, theme.CardBg, theme.Radius);
		const AnimState* selected = SelectedState();
		if (selected == nullptr || selected->Kind == AnimStateKind::SingleClip)
			return;

		const bool blend2d = selected->Kind == AnimStateKind::BlendSpace2D;
		const std::string stateName = selected->Name;
		const std::vector<BlendSample2D> samples2d = selected->Blend2D.GetSamples();
		const std::vector<BlendSample1D> samples1d = selected->Blend1D.GetSamples();
		const size_t sampleCount = blend2d ? samples2d.size() : samples1d.size();

		const std::string samplesHeader = AGFmt("blend.samples", "SAMPLES ({count})",
			{ { "count", std::to_string(sampleCount) } });
		Wui::Label(ctx, { area.X + theme.Pad, area.Y + 5.0f }, samplesHeader, theme.TextMuted,
			theme.FontSizeCaption);
		if (blend2d && area.W > 320.0f)
			Wui::Label(ctx, { area.X + area.W - 150.0f, area.Y + 5.0f }, AGTr("blend.columns", "X (m/s)      Y (deg)"),
				theme.TextDisabled, theme.FontSizeCaption);

		auto copyState = [this, &stateName]() -> AnimState
		{
			const AnimState* live = m_Graph.FindState(stateName);
			return live ? *live : AnimState {};
		};

		float y = area.Y + kRowH + 1.0f;
		for (size_t i = 0; i < sampleCount; ++i)
		{
			const Wui::WuiRect row { area.X + theme.PadSmall, y, area.W - theme.PadSmall * 2.0f, kSampleRowH };
			if (row.Y + row.H > area.Y + area.H - kRowH - 2.0f)
				break;
			const bool hovered = ctx.IsHovered(row);
			const bool selectedRow = static_cast<int>(i) == m_SelectedSample;
			Wui::HoverRow(ctx, row, hovered, selectedRow, theme, 3.0f);
			if (ctx.IsClicked(row))
				m_SelectedSample = static_cast<int>(i);

			const float xW = 66.0f;
			const float yW = 66.0f;
			const float deleteW = kRowH;
			const float clipW = std::max(50.0f, row.W - xW - (blend2d ? yW : 0.0f) - deleteW - theme.Pad * 4.0f);
			const Wui::WuiRect xRect { row.X + theme.PadSmall, row.Y + 2.0f, xW, kRowH };
			const Wui::WuiRect yRect { xRect.X + xW + theme.PadSmall, xRect.Y, yW, kRowH };
			const Wui::WuiRect clipRect { (blend2d ? yRect.X + yW : xRect.X + xW) + theme.PadSmall,
				xRect.Y, clipW, kRowH };
			const Wui::WuiRect deleteRect { clipRect.X + clipW + theme.PadSmall, xRect.Y, deleteW, kRowH };

			float x = blend2d ? samples2d[i].Point.x : samples1d[i].Value;
			const float originalX = x;
			bool xChanged = Wui::DragFloat(ctx, Wui::HashId(("animgraph.sample.x." + std::to_string(i)).c_str()),
				xRect, x, 0.05f, -100.0f, 100.0f, theme);
			Wui::Tooltip(ctx, xRect, AGTr("blend.sample.x.tooltip", "Sample X coordinate (Speed, m/s)."));

			float sampleY = blend2d ? samples2d[i].Point.y : 0.0f;
			const float originalY = sampleY;
			bool yChanged = false;
			if (blend2d)
			{
				yChanged = Wui::DragFloat(ctx, Wui::HashId(("animgraph.sample.y." + std::to_string(i)).c_str()),
					yRect, sampleY, 0.5f, -360.0f, 360.0f, theme);
				Wui::Tooltip(ctx, yRect, AGTr("blend.sample.y.tooltip", "Sample Y coordinate (Direction, degrees)."));
			}

			const std::string currentClip = blend2d ? samples2d[i].ClipName : samples1d[i].ClipName;
			const std::vector<std::string> clips = ClipOptionsForCombo(currentClip);
			int clipIndex = 0;
			const auto found = std::find(clips.begin(), clips.end(), currentClip);
			if (found != clips.end())
				clipIndex = static_cast<int>(std::distance(clips.begin(), found));
			const bool clipChanged = Wui::Combo(ctx,
				Wui::HashId(("animgraph.sample.clip." + std::to_string(i)).c_str()), clipRect, "", clips,
				clipIndex, theme, currentClip.empty() ? AGTr("state.pick_clip", "(pick a clip)") : currentClip);
			Wui::Tooltip(ctx, clipRect, AGTr("blend.sample.clip.tooltip", "Clip sampled at this point."));

			if (Wui::ActionButton(ctx,
				Wui::HashId(("animgraph.sample.delete." + std::to_string(i)).c_str()), deleteRect, "-", theme,
				true, AGTr("blend.sample.delete.tooltip", "Remove this sample point."), true))
			{
				AnimState updated = copyState();
				if (blend2d)
				{
					BlendSpace2D rebuilt;
					for (size_t k = 0; k < updated.Blend2D.GetSamples().size(); ++k)
						if (k != i)
							rebuilt.AddSample(updated.Blend2D.GetSamples()[k].Point,
								updated.Blend2D.GetSamples()[k].ClipName);
					updated.Blend2D = rebuilt;
				}
				else
				{
					BlendSpace1D rebuilt;
					for (size_t k = 0; k < updated.Blend1D.GetSamples().size(); ++k)
						if (k != i)
							rebuilt.AddSample(updated.Blend1D.GetSamples()[k].Value,
								updated.Blend1D.GetSamples()[k].ClipName);
					updated.Blend1D = rebuilt;
				}
				ReplaceState(stateName, updated);
				m_SelectedSample = -1;
				m_TrianglesDirty = true;
				break;
			}

			if (xChanged || yChanged || clipChanged)
			{
				const std::string clip = (clipChanged && clipIndex >= 0 && static_cast<size_t>(clipIndex) < clips.size())
					? clips[static_cast<size_t>(clipIndex)]
					: currentClip;
				AnimState updated = copyState();
				if (blend2d)
				{
					BlendSpace2D rebuilt;
					for (size_t k = 0; k < updated.Blend2D.GetSamples().size(); ++k)
					{
						if (k == i)
							rebuilt.AddSample({ xChanged ? x : originalX, yChanged ? sampleY : originalY }, clip);
						else
							rebuilt.AddSample(updated.Blend2D.GetSamples()[k].Point,
								updated.Blend2D.GetSamples()[k].ClipName);
					}
					updated.Blend2D = rebuilt;
				}
				else
				{
					BlendSpace1D rebuilt;
					for (size_t k = 0; k < updated.Blend1D.GetSamples().size(); ++k)
					{
						if (k == i)
							rebuilt.AddSample(x, clip);
						else
							rebuilt.AddSample(updated.Blend1D.GetSamples()[k].Value,
								updated.Blend1D.GetSamples()[k].ClipName);
					}
					updated.Blend1D = rebuilt;
				}
				ReplaceState(stateName, updated);
				if (yChanged && m_DragSample < 0)
					m_TrianglesDirty = true;
			}

			y += kSampleRowH;
		}

		const Wui::WuiRect addRect { area.X + theme.Pad, area.Y + area.H - kRowH - 2.0f,
			area.W - theme.Pad * 2.0f, kRowH };
		if (Wui::ActionButton(ctx, Wui::HashId("animgraph.sample.add"), addRect, AGTr("blend.sample.add", "+ Add Sample"), theme, true,
			AGTr("blend.sample.add.tooltip", "Add a sample at the current preview parameters."), false))
		{
			AnimState updated = copyState();
			const std::string clip = updated.ClipName.empty() ? std::string("Idle") : updated.ClipName;
			if (blend2d)
				updated.Blend2D.AddSample({ std::max(0.0f, m_ParamSpeed), m_ParamDirection }, clip);
			else
				updated.Blend1D.AddSample(std::max(0.0f, m_ParamSpeed), clip);
			ReplaceState(stateName, updated);
			m_SelectedSample = -1;
			m_TrianglesDirty = true;
		}
	}
}
