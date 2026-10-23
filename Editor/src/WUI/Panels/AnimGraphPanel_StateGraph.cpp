#include "WUI/Panels/AnimGraphPanel.h"

#include "World/WUI/WuiAccessibility.h"
#include "World/WUI/WuiLocalization.h"
#include "World/WUI/Widgets/WuiChrome.h"

#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <string_view>

namespace World
{
	namespace
	{
		constexpr float kNodeW = 154.0f;
		constexpr float kNodeH = 42.0f;
		constexpr float kNodeGapX = 26.0f;
		constexpr float kNodeGapY = 12.0f;
		constexpr float kCanvasTop = 22.0f;
		constexpr float kDragThreshold = 5.0f;

		std::string SGTr(const char* key, const char* fallback)
		{
			return Wui::Tr(std::string("panel.animgraph.") + key, fallback);
		}

		std::string SGFmt(const char* key, const char* fallback,
			std::initializer_list<std::pair<std::string_view, std::string_view>> args = {})
		{
			return Wui::TrFormat(std::string("panel.animgraph.") + key, fallback,
				std::vector<std::pair<std::string_view, std::string_view>>(args));
		}

		std::string BadgeText(AnimStateKind kind)
		{
			switch (kind)
			{
				case AnimStateKind::BlendSpace1D: return "1D";
				case AnimStateKind::BlendSpace2D: return "2D";
				default: return "CLIP";
			}
		}

		Wui::WuiColor Alpha(Wui::WuiColor color, float alpha)
		{
			color.A = alpha;
			return color;
		}

		glm::vec2 ClampLocal(glm::vec2 value, const Wui::WuiRect& canvas)
		{
			return { std::clamp(value.x, 0.0f, std::max(0.0f, canvas.W - kNodeW)),
				std::clamp(value.y, 0.0f, std::max(0.0f, canvas.H - kNodeH)) };
		}
	}

	void AnimGraphPanel::RenderStateGraph(Wui::WuiContext& ctx, const Wui::WuiRect& area, PanelHost& host,
		const Wui::WuiTheme& theme)
	{
		(void)host;
		Wui::PanelBackground(ctx, area, theme.ContentBg, theme.Radius);
		Wui::Label(ctx, { area.X + theme.Pad, area.Y + 4.0f }, SGTr("state_graph.title", "State Graph"),
			theme.TextMuted, theme.FontSizeCaption);

		const Wui::WuiRect canvas { area.X + 1.0f, area.Y + kCanvasTop, area.W - 2.0f,
			std::max(30.0f, area.H - kCanvasTop - 1.0f) };

		// 低饱和基准网格：只作为空间参照，不抢节点前景。
		for (float gx = canvas.X; gx <= canvas.X + canvas.W; gx += 28.0f)
			Wui::LineSegment(ctx, { gx, canvas.Y }, { gx, canvas.Y + canvas.H }, Alpha(theme.Border, 0.38f), 1.0f);
		for (float gy = canvas.Y; gy <= canvas.Y + canvas.H; gy += 28.0f)
			Wui::LineSegment(ctx, { canvas.X, gy }, { canvas.X + canvas.W, gy }, Alpha(theme.Border, 0.38f), 1.0f);

		const auto& states = m_Graph.GetStates();
		if (states.empty())
		{
			Wui::Label(ctx, { canvas.X + 12.0f, canvas.Y + canvas.H * 0.5f - 8.0f },
				SGTr("state_graph.empty", "No states yet. Create one with + on the left."),
				theme.TextMuted, theme.FontSizeSmall);
			Wui::Tooltip(ctx, canvas, SGTr("state_graph.empty.tooltip", "State graph shows states and transitions of this animation graph."));
			return;
		}

		for (auto it = m_StateGraphPositions.begin(); it != m_StateGraphPositions.end();)
		{
			if (m_Graph.FindState(it->first) == nullptr)
				it = m_StateGraphPositions.erase(it);
			else
				++it;
		}

		const int columns = std::max(1, static_cast<int>((canvas.W + kNodeGapX) / (kNodeW + kNodeGapX)));
		int index = 0;
		for (const auto& entry : states)
		{
			auto found = m_StateGraphPositions.find(entry.first);
			if (found == m_StateGraphPositions.end())
			{
				const int column = index % columns;
				const int row = index / columns;
				m_StateGraphPositions.emplace(entry.first, ClampLocal(
					{ 10.0f + static_cast<float>(column) * (kNodeW + kNodeGapX),
						8.0f + static_cast<float>(row) * (kNodeH + kNodeGapY) }, canvas));
			}
			else
			{
				found->second = ClampLocal(found->second, canvas);
			}
			++index;
		}

		auto nodeRect = [&canvas, this](const std::string& name) -> Wui::WuiRect
		{
			const auto position = m_StateGraphPositions.find(name);
			const glm::vec2 local = position == m_StateGraphPositions.end() ? glm::vec2 { 0.0f } : position->second;
			return { canvas.X + local.x, canvas.Y + local.y, kNodeW, kNodeH };
		};

		auto center = [](const Wui::WuiRect& rect) -> glm::vec2
		{
			return { rect.X + rect.W * 0.5f, rect.Y + rect.H * 0.5f };
		};

		// 过渡先画：节点后画会自然遮住穿过节点的线段。
		for (const auto& entry : states)
		{
			const AnimState& state = entry.second;
			for (const AnimTransition& transition : state.Transitions)
			{
				if (m_Graph.FindState(transition.TargetState) == nullptr)
					continue;
				const Wui::WuiRect sourceRect = nodeRect(entry.first);
				const Wui::WuiRect targetRect = nodeRect(transition.TargetState);
				const glm::vec2 sourceCenter = center(sourceRect);
				const glm::vec2 targetCenter = center(targetRect);
				const std::string condition = transition.ConditionExpression.empty()
					? SGTr("state_graph.always", "(always)") : transition.ConditionExpression;

				if (entry.first == transition.TargetState)
				{
					const glm::vec2 loopTop { sourceRect.X + sourceRect.W * 0.5f,
						std::max(canvas.Y + 2.0f, sourceRect.Y - 14.0f) };
					Wui::LineSegment(ctx, { sourceRect.X + sourceRect.W * 0.35f, sourceRect.Y },
						{ loopTop.x - 14.0f, loopTop.y }, theme.Accent, 1.5f);
					Wui::LineSegment(ctx, { loopTop.x - 14.0f, loopTop.y }, { loopTop.x + 14.0f, loopTop.y },
						theme.Accent, 1.5f);
					Wui::LineSegment(ctx, { loopTop.x + 14.0f, loopTop.y },
						{ sourceRect.X + sourceRect.W * 0.65f, sourceRect.Y }, theme.Accent, 1.5f);
					Wui::Label(ctx, { loopTop.x + 18.0f, loopTop.y - 7.0f }, condition, theme.TextMuted,
						theme.FontSizeCaption);
					Wui::Tooltip(ctx, { loopTop.x + 14.0f, loopTop.y - 8.0f, 120.0f, 16.0f }, condition);
					continue;
				}

				const glm::vec2 delta = targetCenter - sourceCenter;
				const float length = glm::length(delta);
				if (length < 0.5f)
					continue;
				const glm::vec2 direction = delta / length;
				const glm::vec2 sourceEdge = sourceCenter + direction * std::min(sourceRect.W, sourceRect.H) * 0.5f;
				const glm::vec2 targetEdge = targetCenter - direction * std::min(targetRect.W, targetRect.H) * 0.5f;
				Wui::LineSegment(ctx, sourceEdge, targetEdge, Alpha(theme.Accent, 0.82f), 1.8f);

				const glm::vec2 perpendicular { -direction.y, direction.x };
				const glm::vec2 arrowBack = targetEdge - direction * 8.0f;
				Wui::LineSegment(ctx, targetEdge, arrowBack + perpendicular * 4.0f, theme.Accent, 2.0f);
				Wui::LineSegment(ctx, targetEdge, arrowBack - perpendicular * 4.0f, theme.Accent, 2.0f);

				const glm::vec2 midpoint = (sourceEdge + targetEdge) * 0.5f;
				const float textW = std::min(150.0f, std::max(48.0f,
					ctx.MeasureTextWidth(condition, theme.FontSizeCaption)));
				const Wui::WuiRect conditionRect { midpoint.x - textW * 0.5f, midpoint.y - 8.0f, textW, 16.0f };
				Wui::PanelBackground(ctx, conditionRect, Alpha(theme.ContentBg, 0.82f), 2.0f);
				Wui::Label(ctx, { conditionRect.X + 2.0f, conditionRect.Y + 1.0f },
					Wui::EllipsizeMiddleToWidth(ctx, condition, conditionRect.W - 4.0f, theme.FontSizeCaption),
					theme.TextMuted, theme.FontSizeCaption);
				Wui::Tooltip(ctx, conditionRect, condition);
			}
		}

		std::vector<std::string> orderedNames;
		orderedNames.reserve(states.size());
		for (const auto& entry : states)
			orderedNames.push_back(entry.first);

		for (const std::string& name : orderedNames)
		{
			const Wui::WuiRect rect = nodeRect(name);
			const AnimState* state = m_Graph.FindState(name);
			if (state == nullptr)
				continue;
			const bool selected = name == m_SelectedState;
			const bool initial = name == m_InitialStateHint;
			Wui::PanelBackground(ctx, rect, selected ? Alpha(theme.Accent, 0.26f) : theme.CardBg, 7.0f);
			if (initial)
				Wui::HighlightOutline(ctx, rect, theme.Warning, 7.0f, 1.6f);
			if (selected)
				Wui::HighlightOutline(ctx, rect, theme.Accent, 7.0f, 1.8f);

			const float badgeW = 36.0f;
			const std::string title = Wui::EllipsizeMiddleToWidth(ctx, name,
				std::max(40.0f, rect.W - badgeW - 18.0f), theme.FontSizeSmall);
			Wui::Label(ctx, { rect.X + 8.0f, rect.Y + 7.0f }, title, theme.Text, theme.FontSizeSmall);
			Wui::Badge(ctx, { rect.X + rect.W - badgeW - 7.0f, rect.Y + 6.0f, badgeW, 16.0f },
				BadgeText(state->Kind), Alpha(theme.Accent, 0.22f), theme.Accent, theme, theme.FontSizeCaption);
			Wui::Tooltip(ctx, rect, SGFmt("state_graph.node.tooltip", "{name} · {type} · {initial}",
				{ { "name", name }, { "type", BadgeText(state->Kind) },
				  { "initial", initial ? SGTr("state_graph.initial", "initial state") : SGTr("state_graph.normal", "not initial") } }));

			Wui::WuiAccessNode node;
			node.Id = Wui::HashId(("animgraph.state_graph.node." + name).c_str());
			node.Window = Wui::WuiAccessibility::Get().CurrentWindow();
			node.Panel = Wui::WuiAccessibility::Get().CurrentPanel();
			node.Kind = "state-node";
			node.Label = name + " · " + BadgeText(state->Kind);
			node.Value = initial ? "initial" : (selected ? "selected" : "normal");
			node.Rect = rect;
			node.Interactive = true;
			Wui::WuiAccessibility::Get().Register(node);
		}

		Wui::WuiAccessNode canvasNode;
		canvasNode.Id = Wui::HashId("animgraph.state_graph.canvas");
		canvasNode.Window = Wui::WuiAccessibility::Get().CurrentWindow();
		canvasNode.Panel = Wui::WuiAccessibility::Get().CurrentPanel();
		canvasNode.Kind = "state-graph";
		canvasNode.Label = SGTr("state_graph.title", "State Graph");
		canvasNode.Value = "states=" + std::to_string(states.size());
		canvasNode.Rect = canvas;
		canvasNode.Interactive = false;
		Wui::WuiAccessibility::Get().Register(canvasNode);

		const glm::vec2 mouse = ctx.Input().MousePos;
		int hovered = -1;
		for (int i = static_cast<int>(orderedNames.size()) - 1; i >= 0; --i)
		{
			if (Wui::HitTest(nodeRect(orderedNames[static_cast<size_t>(i)]), mouse))
			{
				hovered = i;
				break;
			}
		}
		if (hovered >= 0 && ctx.IsHovered(canvas) && ctx.ClipAllows(area))
			ctx.SetCursor(Wui::WuiCursor::Hand);

		if (hovered >= 0 && ctx.IsHovered(canvas) && ctx.ClipAllows(area))
		{
			const std::string& name = orderedNames[static_cast<size_t>(hovered)];
			const Wui::WuiRect rect = nodeRect(name);
			if (ctx.IsDoubleClicked(rect))
			{
				m_SelectedState = name;
				m_SelectedSample = -1;
				m_SelectedTransition = -1;
				m_TrianglesDirty = true;
				m_CenterScroll = area.H + 8.0f;
				m_StateGraphDragPending.clear();
				m_StateGraphDragNode.clear();
			}
			else if (ctx.Input().MouseClicked[0])
			{
				const bool changed = m_SelectedState != name;
				m_SelectedState = name;
				if (changed)
				{
					m_SelectedSample = -1;
					m_SelectedTransition = -1;
					m_TrianglesDirty = true;
					m_NameBufferKey.clear();
					m_ConditionKey.clear();
				}
				m_StateGraphDragPending = name;
				m_StateGraphDragStart = mouse;
				m_StateGraphDragOffset = glm::vec2 { rect.X, rect.Y } - mouse;
			}
		}

		if (!m_StateGraphDragPending.empty() && m_StateGraphDragNode.empty() && ctx.Input().MouseDown[0])
		{
			if (glm::length(mouse - m_StateGraphDragStart) > kDragThreshold)
				m_StateGraphDragNode = m_StateGraphDragPending;
		}
		if (!m_StateGraphDragNode.empty() && ctx.Input().MouseDown[0])
		{
			ctx.SetCursor(Wui::WuiCursor::ResizeNWSE);
			m_StateGraphPositions[m_StateGraphDragNode] = ClampLocal(
				{ mouse.x - canvas.X + m_StateGraphDragOffset.x,
					mouse.y - canvas.Y + m_StateGraphDragOffset.y }, canvas);
		}
		if (!ctx.Input().MouseDown[0])
		{
			m_StateGraphDragPending.clear();
			m_StateGraphDragNode.clear();
		}
	}
}
