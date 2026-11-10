#include "WUI/Panels/AnimGraphPanel.h"

#include "World/Core/KeyCodes.h"
#include "World/WUI/WuiAccessibility.h"
#include "World/WUI/WuiLocalization.h"
#include "World/WUI/Widgets/WuiChrome.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <initializer_list>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace World
{
	namespace
	{
		constexpr float kNodeW = 154.0f;
		constexpr float kNodeH = 42.0f;
		constexpr float kNodeGapX = 26.0f;
		constexpr float kNodeGapY = 12.0f;
		constexpr float kHeaderH = 32.0f;
		constexpr float kDragThreshold = 5.0f;
		constexpr float kZoomMin = 0.5f;
		constexpr float kZoomMax = 1.8f;
		constexpr float kFitZoomMax = 1.5f;
		constexpr float kZoomStep = 1.12f;
		constexpr float kPanLimit = 100000.0f;
		constexpr float kNodeLimit = 10000.0f;
		constexpr float kPanNone = -1.0e6f;
		constexpr float kToolbarButtonH = 24.0f;
		constexpr float kToolbarGap = 4.0f;
		constexpr float kToolbarMinusW = 24.0f;
		constexpr float kToolbarResetW = 44.0f;
		constexpr float kToolbarPlusW = 24.0f;
		constexpr float kToolbarFitW = 40.0f;
		constexpr float kLinkHitRadius = 6.0f;
		constexpr float kPi = 3.14159265358979323846f;
		constexpr float kScrollWheelStep = 40.0f;   // 与 Wui::BeginScrollArea 的滚轮步长一致。

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

		float NodeFontSize(float baseSize, float zoom, const Wui::WuiTheme& theme)
		{
			return std::clamp(baseSize * zoom, theme.FontSizeCaption * 0.8f, theme.FontSizeSmall * 1.3f);
		}

		glm::vec2 EdgePoint(const Wui::WuiRect& rect, glm::vec2 center, glm::vec2 direction)
		{
			const float halfW = std::max(0.5f, rect.W * 0.5f);
			const float halfH = std::max(0.5f, rect.H * 0.5f);
			const float ax = std::fabs(direction.x);
			const float ay = std::fabs(direction.y);
			float distance = std::numeric_limits<float>::max();
			if (ax > 1.0e-5f)
				distance = std::min(distance, halfW / ax);
			if (ay > 1.0e-5f)
				distance = std::min(distance, halfH / ay);
			if (!std::isfinite(distance))
				distance = 0.0f;
			return center + direction * distance;
		}

		float DistanceToSegment(glm::vec2 point, glm::vec2 a, glm::vec2 b)
		{
			const glm::vec2 ab = b - a;
			const float lengthSq = glm::dot(ab, ab);
			const float t = lengthSq > 1.0e-6f
				? std::clamp(glm::dot(point - a, ab) / lengthSq, 0.0f, 1.0f) : 0.0f;
			return glm::length(point - (a + ab * t));
		}

		Wui::WuiRect BoundsOfSegments(const std::vector<std::pair<glm::vec2, glm::vec2>>& segments)
		{
			if (segments.empty())
				return { 0.0f, 0.0f, 0.0f, 0.0f };
			float minX = std::numeric_limits<float>::max();
			float minY = std::numeric_limits<float>::max();
			float maxX = std::numeric_limits<float>::lowest();
			float maxY = std::numeric_limits<float>::lowest();
			for (const auto& segment : segments)
			{
				minX = std::min(minX, std::min(segment.first.x, segment.second.x));
				minY = std::min(minY, std::min(segment.first.y, segment.second.y));
				maxX = std::max(maxX, std::max(segment.first.x, segment.second.x));
				maxY = std::max(maxY, std::max(segment.first.y, segment.second.y));
			}
			return { minX, minY, std::max(0.0f, maxX - minX), std::max(0.0f, maxY - minY) };
		}

		struct GraphLink
		{
			std::string Source;
			std::string Target;
			std::string Condition;
			int Index = -1;
			bool SelfLoop = false;
			glm::vec2 ArrowDirection { 1.0f, 0.0f };
			std::vector<std::pair<glm::vec2, glm::vec2>> Segments;
			Wui::WuiRect Bounds { 0.0f, 0.0f, 0.0f, 0.0f };
			Wui::WuiRect LabelRect { 0.0f, 0.0f, 0.0f, 0.0f };
		};
	}

	void AnimGraphPanel::RenderStateGraph(Wui::WuiContext& ctx, const Wui::WuiRect& area, PanelHost& host,
		const Wui::WuiTheme& theme)
	{
		(void)host;
		Wui::PanelBackground(ctx, area, theme.ContentBg, theme.Radius);

		const auto& states = m_Graph.GetStates();
		const Wui::WuiRect canvas { area.X + 1.0f, area.Y + kHeaderH,
			std::max(30.0f, area.W - 2.0f), std::max(30.0f, area.H - kHeaderH - 1.0f) };

		auto zoomAround = [this](glm::vec2 localAnchor, float newZoom)
		{
			newZoom = std::clamp(newZoom, kZoomMin, kZoomMax);
			if (std::fabs(newZoom - m_StateGraphZoom) < 1.0e-6f)
				return;
			const glm::vec2 graphAnchor = localAnchor / m_StateGraphZoom - m_StateGraphPan;
			m_StateGraphPan = localAnchor / newZoom - graphAnchor;
			m_StateGraphZoom = newZoom;
		};

		const std::string title = SGTr("state_graph.title", "State Graph");
		const float titleX = area.X + theme.Pad;
		const float titleY = area.Y + (kHeaderH - theme.FontSizeCaption) * 0.5f;

		const float toolbarW = kToolbarMinusW + kToolbarResetW + kToolbarPlusW + kToolbarFitW
			+ kToolbarGap * 3.0f;
		const float toolbarX = area.X + std::max(theme.PadSmall, area.W - theme.PadSmall - toolbarW);
		const float toolbarY = area.Y + (kHeaderH - kToolbarButtonH) * 0.5f;
		const Wui::WuiRect minusRect { toolbarX, toolbarY, kToolbarMinusW, kToolbarButtonH };
		const Wui::WuiRect resetRect { minusRect.X + minusRect.W + kToolbarGap, toolbarY,
			kToolbarResetW, kToolbarButtonH };
		const Wui::WuiRect plusRect { resetRect.X + resetRect.W + kToolbarGap, toolbarY,
			kToolbarPlusW, kToolbarButtonH };
		const Wui::WuiRect fitRect { plusRect.X + plusRect.W + kToolbarGap, toolbarY,
			kToolbarFitW, kToolbarButtonH };
		const glm::vec2 canvasCenter { canvas.W * 0.5f, canvas.H * 0.5f };
		const std::string titleText = Wui::EllipsizeMiddleToWidth(ctx, title,
			std::max(20.0f, toolbarX - titleX - theme.PadSmall), theme.FontSizeCaption);
		Wui::Label(ctx, { titleX, titleY }, titleText, theme.TextMuted, theme.FontSizeCaption);

		if (Wui::ActionButton(ctx, Wui::HashId("animgraph.state_graph.zoom_out"), minusRect, "-", theme, true,
			SGTr("state_graph.zoom_out.tooltip", "Zoom out, anchored at the canvas center."), false))
			zoomAround(canvasCenter, m_StateGraphZoom / kZoomStep);
		if (Wui::ActionButton(ctx, Wui::HashId("animgraph.state_graph.zoom_reset"), resetRect, "100%", theme, true,
			SGTr("state_graph.zoom_reset.tooltip", "Reset zoom to 100%, anchored at the canvas center."), false))
			zoomAround(canvasCenter, 1.0f);
		if (Wui::ActionButton(ctx, Wui::HashId("animgraph.state_graph.zoom_in"), plusRect, "+", theme, true,
			SGTr("state_graph.zoom_in.tooltip", "Zoom in, anchored at the canvas center."), false))
			zoomAround(canvasCenter, m_StateGraphZoom * kZoomStep);
		if (Wui::ActionButton(ctx, Wui::HashId("animgraph.state_graph.fit"), fitRect,
			SGTr("state_graph.fit", "Fit"), theme, true,
			SGTr("state_graph.fit.tooltip", "Fit all states into the visible canvas."), false))
			m_StateGraphFitRequested = true;

		const std::string hint = SGTr("state_graph.hint",
			"Scroll to zoom · Middle/right-drag to pan · Double-click a state to open");
		const float hintX = titleX + ctx.MeasureTextWidth(titleText, theme.FontSizeCaption) + theme.Pad;
		const float hintW = toolbarX - hintX - theme.PadSmall;
		if (hintW >= 96.0f)
		{
			Wui::Label(ctx, { hintX, titleY }, Wui::EllipsizeMiddleToWidth(ctx, hint, hintW, theme.FontSizeCaption),
				theme.TextDisabled, theme.FontSizeCaption);
		}

		const glm::vec2 mouse = ctx.Input().MousePos;
		bool overCanvas = ctx.IsHovered(canvas) && ctx.ClipAllows(canvas);
		const float wheel = m_CanvasWheel;
		if (overCanvas && std::fabs(wheel) > 0.001f)
		{
			// 滚轮由 RenderCenterColumn 在父滚动区处理之前就转存到 m_CanvasWheel，
			// 父级这一帧看不到它 —— 不需要再"抵掉"滚动量。
			const glm::vec2 localAnchor = mouse - glm::vec2 { canvas.X, canvas.Y };
			const float oldZoom = m_StateGraphZoom;
			const float newZoom = std::clamp(oldZoom * std::pow(kZoomStep, wheel), kZoomMin, kZoomMax);
			if (std::fabs(newZoom - oldZoom) > 1.0e-6f)
			{
				const glm::vec2 graphAnchor = localAnchor / oldZoom - m_StateGraphPan;
				m_StateGraphPan = localAnchor / newZoom - graphAnchor;
				m_StateGraphZoom = newZoom;
			}
		}

		auto drawGrid = [&ctx, &canvas, &theme, this]()
		{
			float spacing = 32.0f * m_StateGraphZoom;
			while (spacing < 16.0f)
				spacing *= 2.0f;
			float offsetX = std::fmod(m_StateGraphPan.x * m_StateGraphZoom, spacing);
			float offsetY = std::fmod(m_StateGraphPan.y * m_StateGraphZoom, spacing);
			if (offsetX < 0.0f)
				offsetX += spacing;
			if (offsetY < 0.0f)
				offsetY += spacing;
			for (float gx = canvas.X + offsetX; gx <= canvas.X + canvas.W; gx += spacing)
				Wui::LineSegment(ctx, { gx, canvas.Y }, { gx, canvas.Y + canvas.H }, Alpha(theme.Border, 0.34f), 1.0f);
			for (float gy = canvas.Y + offsetY; gy <= canvas.Y + canvas.H; gy += spacing)
				Wui::LineSegment(ctx, { canvas.X, gy }, { canvas.X + canvas.W, gy }, Alpha(theme.Border, 0.34f), 1.0f);
		};

		auto registerCanvas = [&]()
		{
			char value[96] = {};
			std::snprintf(value, sizeof(value), "states=%d zoom=%.2f pan=%.1f,%.1f",
				static_cast<int>(states.size()), m_StateGraphZoom, m_StateGraphPan.x, m_StateGraphPan.y);
			Wui::WuiAccessNode node;
			node.Id = Wui::HashId("animgraph.state_graph.canvas");
			node.Window = Wui::WuiAccessibility::Get().CurrentWindow();
			node.Panel = Wui::WuiAccessibility::Get().CurrentPanel();
			node.Kind = "state-graph";
			node.Label = title;
			node.Value = value;
			node.Tooltip = hint;
			node.Rect = canvas;
			node.Interactive = false;
			Wui::WuiAccessibility::Get().Register(node);
		};

		if (states.empty())
		{
			if (m_StateGraphFitRequested)
			{
				m_StateGraphFitRequested = false;
				m_StateGraphZoom = 1.0f;
				m_StateGraphPan = glm::vec2 { 0.0f, 0.0f };
			}
			Wui::PanelBackground(ctx, canvas, theme.WindowBg, theme.Radius);
			{
				Wui::ClipScope clip(ctx, canvas);
				drawGrid();
				const std::string empty = SGTr("state_graph.empty", "No states yet. Create one with + on the left.");
				Wui::Label(ctx, { canvas.X + 12.0f, canvas.Y + canvas.H * 0.5f - 8.0f },
					Wui::EllipsizeMiddleToWidth(ctx, empty, std::max(0.0f, canvas.W - 24.0f), theme.FontSizeSmall),
					theme.TextMuted, theme.FontSizeSmall);
				Wui::Tooltip(ctx, canvas, SGTr("state_graph.empty.tooltip",
					"State graph shows states and transitions of this animation graph."));
			}
			registerCanvas();
			return;
		}

		for (auto it = m_StateGraphPositions.begin(); it != m_StateGraphPositions.end();)
		{
			if (m_Graph.FindState(it->first) == nullptr)
				it = m_StateGraphPositions.erase(it);
			else
				++it;
		}

		const float layoutWidth = std::max(kNodeW, canvas.W / std::max(kZoomMin, m_StateGraphZoom));
		const int columns = std::max(1, static_cast<int>((layoutWidth - 20.0f + kNodeGapX) / (kNodeW + kNodeGapX)));
		std::vector<glm::vec2> occupiedPositions;
		occupiedPositions.reserve(states.size());
		for (const auto& entry : states)
		{
			const auto found = m_StateGraphPositions.find(entry.first);
			if (found != m_StateGraphPositions.end())
				occupiedPositions.push_back(found->second);
		}
		int nextSlot = 0;
		for (const auto& entry : states)
		{
			if (m_StateGraphPositions.find(entry.first) != m_StateGraphPositions.end())
				continue;
			glm::vec2 position { 0.0f, 0.0f };
			bool freeSlot = false;
			for (int attempt = 0; attempt < 4096 && !freeSlot; ++attempt, ++nextSlot)
			{
				const int column = nextSlot % columns;
				const int row = nextSlot / columns;
				position = { 10.0f + static_cast<float>(column) * (kNodeW + kNodeGapX),
					8.0f + static_cast<float>(row) * (kNodeH + kNodeGapY) };
				freeSlot = true;
				for (const glm::vec2& occupied : occupiedPositions)
				{
					if (std::fabs(occupied.x - position.x) < kNodeW - 1.0f
						&& std::fabs(occupied.y - position.y) < kNodeH - 1.0f)
					{
						freeSlot = false;
						break;
					}
				}
			}
			m_StateGraphPositions.emplace(entry.first, position);
			occupiedPositions.push_back(position);
		}

		auto fitView = [&]()
		{
			float minX = std::numeric_limits<float>::max();
			float minY = std::numeric_limits<float>::max();
			float maxX = std::numeric_limits<float>::lowest();
			float maxY = std::numeric_limits<float>::lowest();
			for (const auto& entry : states)
			{
				const glm::vec2 position = m_StateGraphPositions[entry.first];
				minX = std::min(minX, position.x);
				minY = std::min(minY, position.y);
				maxX = std::max(maxX, position.x + kNodeW);
				maxY = std::max(maxY, position.y + kNodeH);
			}
			const float bboxW = std::max(1.0f, maxX - minX);
			const float bboxH = std::max(1.0f, maxY - minY);
			m_StateGraphZoom = std::clamp(std::min((canvas.W - 32.0f) / bboxW,
				(canvas.H - 32.0f) / bboxH), kZoomMin, kFitZoomMax);
			m_StateGraphPan.x = canvas.W * 0.5f / m_StateGraphZoom - (minX + maxX) * 0.5f;
			m_StateGraphPan.y = canvas.H * 0.5f / m_StateGraphZoom - (minY + maxY) * 0.5f;
		};

		if (m_StateGraphFitRequested)
		{
			m_StateGraphFitRequested = false;
			fitView();
		}

		auto nodeRect = [&canvas, this](const std::string& name) -> Wui::WuiRect
		{
			const auto position = m_StateGraphPositions.find(name);
			const glm::vec2 graphPos = position == m_StateGraphPositions.end()
				? glm::vec2 { 0.0f, 0.0f } : position->second;
			return { canvas.X + (graphPos.x + m_StateGraphPan.x) * m_StateGraphZoom,
				canvas.Y + (graphPos.y + m_StateGraphPan.y) * m_StateGraphZoom,
				kNodeW * m_StateGraphZoom, kNodeH * m_StateGraphZoom };
		};

		auto graphPoint = [&canvas, this](glm::vec2 screen)
		{
			return (screen - glm::vec2 { canvas.X, canvas.Y }) / m_StateGraphZoom - m_StateGraphPan;
		};

		std::vector<std::string> orderedNames;
		orderedNames.reserve(states.size());
		for (const auto& entry : states)
			orderedNames.push_back(entry.first);

		auto selectState = [this](const std::string& name)
		{
			m_SelectedState = name;
			m_SelectedSample = -1;
			m_SelectedTransition = -1;
			m_TransitionEditKey.clear();
			m_TrianglesDirty = true;
			m_NameBufferKey.clear();
			m_ConditionKey.clear();
			m_ConditionFocusKey.clear();
		};

		const bool anyDown = ctx.Input().MouseDown[0] || ctx.Input().MouseDown[1] || ctx.Input().MouseDown[2];
		if (!anyDown && !m_StateGraphPanning && m_StateGraphDragNode.empty())
			m_StateGraphPanStart.x = kPanNone;

		auto beginPan = [this, &mouse]()
		{
			m_StateGraphPanning = true;
			m_StateGraphPanStart = mouse;
			m_StateGraphPanOrigin = m_StateGraphPan;
			m_StateGraphDragPending.clear();
			m_StateGraphDragNode.clear();
		};

		if (overCanvas && (ctx.Input().MouseClicked[1] || ctx.Input().MouseClicked[2]))
			beginPan();
		else if (overCanvas && ctx.Input().MouseClicked[0] && ctx.IsKeyPressed(KeyCodes::Space))
			beginPan();

		if (m_StateGraphPanning)
		{
			if (anyDown)
			{
				m_StateGraphPan = m_StateGraphPanOrigin + (mouse - m_StateGraphPanStart) / m_StateGraphZoom;
				m_StateGraphPan.x = std::clamp(m_StateGraphPan.x, -kPanLimit, kPanLimit);
				m_StateGraphPan.y = std::clamp(m_StateGraphPan.y, -kPanLimit, kPanLimit);
				ctx.SetCursor(Wui::WuiCursor::Hand);
			}
			else
			{
				m_StateGraphPanning = false;
				m_StateGraphPanStart.x = kPanNone;
			}
		}

		int hoveredNode = -1;
		if (!m_StateGraphPanning && overCanvas)
		{
			for (int i = static_cast<int>(orderedNames.size()) - 1; i >= 0; --i)
			{
				if (ctx.IsHovered(nodeRect(orderedNames[static_cast<size_t>(i)])))
				{
					hoveredNode = i;
					break;
				}
			}
		}

		if (!m_StateGraphPanning && hoveredNode >= 0)
		{
			const std::string& name = orderedNames[static_cast<size_t>(hoveredNode)];
			const Wui::WuiRect rect = nodeRect(name);
			ctx.SetCursor(Wui::WuiCursor::Hand);
			if (ctx.IsDoubleClicked(rect))
			{
				selectState(name);
				m_StateGraphDragPending.clear();
				m_StateGraphDragNode.clear();
				m_StateGraphPanStart.x = kPanNone;
				m_CenterScroll = std::max(0.0f, area.H + 8.0f);
			}
			else if (ctx.Input().MouseClicked[0])
			{
				selectState(name);
				m_StateGraphDragPending = name;
				m_StateGraphDragNode.clear();
				m_StateGraphDragStart = mouse;
				const auto position = m_StateGraphPositions.find(name);
				m_StateGraphDragOffset = graphPoint(mouse)
					- (position == m_StateGraphPositions.end() ? glm::vec2 { 0.0f, 0.0f } : position->second);
				m_StateGraphPanStart.x = kPanNone;
			}
		}

		if (!m_StateGraphDragPending.empty() && m_StateGraphDragNode.empty() && ctx.Input().MouseDown[0]
			&& glm::length(mouse - m_StateGraphDragStart) > kDragThreshold)
		{
			m_StateGraphDragNode = m_StateGraphDragPending;
		}
		if (!m_StateGraphDragNode.empty() && ctx.Input().MouseDown[0])
		{
			const glm::vec2 next = graphPoint(mouse) - m_StateGraphDragOffset;
			m_StateGraphPositions[m_StateGraphDragNode] = {
				std::clamp(next.x, -kNodeLimit, kNodeLimit), std::clamp(next.y, -kNodeLimit, kNodeLimit) };
			ctx.SetCursor(Wui::WuiCursor::Hand);
		}

		auto buildLinks = [&]()
		{
			std::vector<GraphLink> links;
			for (const auto& entry : states)
			{
				const AnimState& state = entry.second;
				for (size_t transitionIndex = 0; transitionIndex < state.Transitions.size(); ++transitionIndex)
				{
					const AnimTransition& transition = state.Transitions[transitionIndex];
					if (m_Graph.FindState(transition.TargetState) == nullptr)
						continue;

					GraphLink link;
					link.Source = entry.first;
					link.Target = transition.TargetState;
					link.Index = static_cast<int>(transitionIndex);
					link.Condition = transition.ConditionExpression.empty()
						? SGTr("state_graph.always", "(always)") : transition.ConditionExpression;
					link.SelfLoop = entry.first == transition.TargetState;
					const Wui::WuiRect sourceRect = nodeRect(entry.first);
					const Wui::WuiRect targetRect = nodeRect(transition.TargetState);

					glm::vec2 labelCenter { 0.0f, 0.0f };
					if (link.SelfLoop)
					{
						const float loopOut = std::clamp(14.0f * m_StateGraphZoom, 10.0f, 22.0f);
						const float topY = sourceRect.Y + sourceRect.H * 0.35f;
						const float bottomY = sourceRect.Y + sourceRect.H * 0.65f;
						const int loopSegments = 12;
						auto loopPoint = [&](float t)
						{
							return glm::vec2 { sourceRect.X + sourceRect.W + loopOut * std::sin(kPi * t),
								topY + (bottomY - topY) * t };
						};
						glm::vec2 previous = loopPoint(0.0f);
						for (int segment = 1; segment <= loopSegments; ++segment)
						{
							const glm::vec2 current = loopPoint(static_cast<float>(segment) / static_cast<float>(loopSegments));
							link.Segments.push_back({ previous, current });
							previous = current;
						}
						link.ArrowDirection = glm::normalize(loopPoint(1.0f) - loopPoint(1.0f - 1.0f / loopSegments));
						labelCenter = { sourceRect.X + sourceRect.W + loopOut,
							(topY + bottomY) * 0.5f };
					}
					else
					{
						const glm::vec2 sourceCenter { sourceRect.X + sourceRect.W * 0.5f,
							sourceRect.Y + sourceRect.H * 0.5f };
						const glm::vec2 targetCenter { targetRect.X + targetRect.W * 0.5f,
							targetRect.Y + targetRect.H * 0.5f };
						const glm::vec2 delta = targetCenter - sourceCenter;
						const float length = glm::length(delta);
						if (length < 0.5f)
							continue;
						link.ArrowDirection = delta / length;
						const glm::vec2 sourceEdge = EdgePoint(sourceRect, sourceCenter, link.ArrowDirection);
						const glm::vec2 targetEdge = EdgePoint(targetRect, targetCenter, -link.ArrowDirection);
						link.Segments.push_back({ sourceEdge, targetEdge });
						labelCenter = (sourceEdge + targetEdge) * 0.5f;
					}

					link.Bounds = BoundsOfSegments(link.Segments);
					if (link.Bounds.H < 8.0f)
					{
						link.Bounds.Y -= (8.0f - link.Bounds.H) * 0.5f;
						link.Bounds.H = 8.0f;
					}
					if (link.Bounds.W < 8.0f)
					{
						link.Bounds.X -= (8.0f - link.Bounds.W) * 0.5f;
						link.Bounds.W = 8.0f;
					}

					const float labelFont = theme.FontSizeCaption;
					const float labelW = std::clamp(ctx.MeasureTextWidth(link.Condition, labelFont) + 8.0f,
						48.0f, 180.0f);
					if (link.SelfLoop)
						labelCenter.x += 8.0f + labelW * 0.5f;
					link.LabelRect = { labelCenter.x - labelW * 0.5f, labelCenter.y - 8.0f, labelW, 16.0f };
					links.push_back(std::move(link));
				}
			}
			return links;
		};

		std::vector<GraphLink> links = buildLinks();
		int hoveredLink = -1;
		if (!m_StateGraphPanning && overCanvas && hoveredNode < 0)
		{
			for (int i = static_cast<int>(links.size()) - 1; i >= 0; --i)
			{
				const GraphLink& link = links[static_cast<size_t>(i)];
				bool hit = link.LabelRect.Contains(mouse);
				if (!hit)
				{
					for (const auto& segment : link.Segments)
					{
						if (DistanceToSegment(mouse, segment.first, segment.second) <= kLinkHitRadius)
						{
							hit = true;
							break;
						}
					}
				}
				if (hit)
				{
					hoveredLink = i;
					break;
				}
			}
			if (hoveredLink >= 0)
				ctx.SetCursor(Wui::WuiCursor::Hand);
		}

		if (!m_StateGraphPanning && hoveredNode < 0 && hoveredLink >= 0 && ctx.Input().MouseClicked[0])
		{
			const GraphLink& link = links[static_cast<size_t>(hoveredLink)];
			selectState(link.Source);
			m_SelectedTransition = link.Index;
			m_TransitionEditKey = link.Source + "#" + std::to_string(link.Index);
			m_CenterTab = 0;
			m_RevealTransitionRow = true;
			m_StateGraphDragPending.clear();
			m_StateGraphDragNode.clear();
			m_StateGraphPanStart.x = kPanNone;
			ctx.ConsumePointerClick(0);
		}

		if (!m_StateGraphPanning && overCanvas && hoveredNode < 0 && hoveredLink < 0
			&& ctx.Input().MouseClicked[0] && !ctx.IsKeyPressed(KeyCodes::Space))
		{
			m_StateGraphPanning = false;
			m_StateGraphPanStart = mouse;
			m_StateGraphPanOrigin = m_StateGraphPan;
			m_StateGraphDragPending.clear();
			m_StateGraphDragNode.clear();
		}

		if (!m_StateGraphPanning && m_StateGraphDragNode.empty() && m_StateGraphDragPending.empty()
			&& m_StateGraphPanStart.x > kPanNone + 1.0f && ctx.Input().MouseDown[0]
			&& glm::length(mouse - m_StateGraphPanStart) > kDragThreshold)
		{
			m_StateGraphPanning = true;
		}

		if (m_StateGraphPanning && anyDown)
		{
			m_StateGraphPan = m_StateGraphPanOrigin + (mouse - m_StateGraphPanStart) / m_StateGraphZoom;
			m_StateGraphPan.x = std::clamp(m_StateGraphPan.x, -kPanLimit, kPanLimit);
			m_StateGraphPan.y = std::clamp(m_StateGraphPan.y, -kPanLimit, kPanLimit);
			ctx.SetCursor(Wui::WuiCursor::Hand);
			links = buildLinks();
			hoveredLink = -1;
		}

		if (!anyDown)
		{
			m_StateGraphPanning = false;
			m_StateGraphPanStart.x = kPanNone;
			m_StateGraphDragPending.clear();
			m_StateGraphDragNode.clear();
		}

		Wui::PanelBackground(ctx, canvas, theme.WindowBg, theme.Radius);
		{
			Wui::ClipScope clip(ctx, canvas);
			drawGrid();
			Wui::Tooltip(ctx, canvas, hint);

			for (size_t i = 0; i < links.size(); ++i)
			{
				const GraphLink& link = links[i];
				const bool selected = link.Source == m_SelectedState && link.Index == m_SelectedTransition;
				const bool hovered = static_cast<int>(i) == hoveredLink;
				const Wui::WuiColor lineColor = selected ? theme.Warning
					: (hovered ? Alpha(theme.Accent, 0.90f) : Alpha(theme.Accent, 0.80f));
				const float lineWidth = selected ? 2.6f : (hovered ? 2.2f : 1.8f);
				for (const auto& segment : link.Segments)
					Wui::LineSegment(ctx, segment.first, segment.second, lineColor, lineWidth);

				const float arrowLength = std::clamp(6.0f * m_StateGraphZoom, 4.0f, 8.0f);
				const glm::vec2 tip = link.Segments.back().second;
				const glm::vec2 direction = link.ArrowDirection;
				const glm::vec2 back = tip - direction * arrowLength;
				const glm::vec2 perpendicular { -direction.y, direction.x };
				Wui::LineSegment(ctx, tip, back + perpendicular * (arrowLength * 0.5f), lineColor, lineWidth);
				Wui::LineSegment(ctx, tip, back - perpendicular * (arrowLength * 0.5f), lineColor, lineWidth);

				// 条件文字只在选中/悬停该过渡时出现：常显会让多条长表达式互相压住、
				// 也盖住节点标题（实机 4 状态图已复现）。这与商业引擎"选中过渡才看规则"一致。
				if (selected || hovered)
				{
					const float labelFont = theme.FontSizeCaption;
					Wui::PanelBackground(ctx, link.LabelRect, Alpha(theme.ContentBg, 0.96f), 2.0f);
					Wui::Label(ctx, { link.LabelRect.X + 3.0f, link.LabelRect.Y + 1.0f },
						Wui::EllipsizeMiddleToWidth(ctx, link.Condition,
							std::max(0.0f, link.LabelRect.W - 6.0f), labelFont),
						selected ? theme.Warning : theme.TextMuted, labelFont);
				}
				Wui::Tooltip(ctx, link.LabelRect, link.Condition);

				Wui::WuiAccessNode linkNode;
				linkNode.Id = Wui::HashId(("animgraph.state_graph.transition." + link.Source + "." + std::to_string(link.Index)).c_str());
				linkNode.Window = Wui::WuiAccessibility::Get().CurrentWindow();
				linkNode.Panel = Wui::WuiAccessibility::Get().CurrentPanel();
				linkNode.Kind = "state-transition";
				linkNode.Label = link.Source + " -> " + link.Target;
				linkNode.Value = link.Condition;
				linkNode.Tooltip = link.Condition;
				linkNode.Rect = link.Bounds;
				linkNode.Interactive = true;
				Wui::WuiAccessibility::Get().Register(linkNode);
			}

			for (const std::string& name : orderedNames)
			{
				const Wui::WuiRect rect = nodeRect(name);
				const AnimState* state = m_Graph.FindState(name);
				if (state == nullptr)
					continue;
				const bool selected = name == m_SelectedState;
				const bool initial = name == m_InitialStateHint;
				const bool hovered = hoveredNode >= 0
					&& orderedNames[static_cast<size_t>(hoveredNode)] == name;
				const float radius = std::clamp(7.0f * m_StateGraphZoom, 4.0f, 9.0f);
				const Wui::WuiColor fill = selected ? Alpha(theme.Accent, 0.26f)
					: (hovered ? theme.HoverBg : theme.CardBg);
				Wui::PanelBackground(ctx, rect, fill, radius);
				if (initial)
					Wui::HighlightOutline(ctx, rect, theme.Warning, radius,
						std::clamp(1.5f * m_StateGraphZoom, 1.2f, 2.0f));
				if (selected)
					Wui::HighlightOutline(ctx, rect, theme.Accent, radius,
						std::clamp(2.0f * m_StateGraphZoom, 1.6f, 2.6f));
				else if (hovered)
					Wui::HighlightOutline(ctx, rect, Alpha(theme.Accent, 0.82f), radius,
						std::max(1.0f, m_StateGraphZoom));

				const float titleFont = NodeFontSize(theme.FontSizeSmall, m_StateGraphZoom, theme);
				const float badgeFont = NodeFontSize(theme.FontSizeCaption, m_StateGraphZoom, theme);
				const float badgeW = std::clamp(36.0f * m_StateGraphZoom, 26.0f, 52.0f);
				const float badgeH = std::clamp(16.0f * m_StateGraphZoom, 12.0f, 22.0f);
				const float pad = std::clamp(8.0f * m_StateGraphZoom, 4.0f, 12.0f);
				const float badgeX = rect.X + rect.W - badgeW - pad;
				const std::string badgeText = BadgeText(state->Kind);
				const std::string nodeTitleText = Wui::EllipsizeMiddleToWidth(ctx, name,
					std::max(20.0f, badgeX - rect.X - pad - 4.0f), titleFont);
				Wui::Label(ctx, { rect.X + pad, rect.Y + std::max(4.0f, 7.0f * m_StateGraphZoom) },
					nodeTitleText, theme.Text, titleFont);
				Wui::Badge(ctx, { badgeX, rect.Y + std::max(3.0f, 6.0f * m_StateGraphZoom), badgeW, badgeH },
					badgeText, Alpha(theme.Accent, 0.22f), theme.Accent, theme, badgeFont);
				const std::string nodeTooltip = SGFmt("state_graph.node.tooltip", "{name} · {type} · {initial}",
					{ { "name", name }, { "type", badgeText },
					  { "initial", initial ? SGTr("state_graph.initial", "initial state") : SGTr("state_graph.normal", "not initial") } });
				Wui::Tooltip(ctx, rect, nodeTooltip);

				char value[128] = {};
				std::snprintf(value, sizeof(value), "zoom=%.2f initial=%d selected=%d type=%s",
					m_StateGraphZoom, initial ? 1 : 0, selected ? 1 : 0, badgeText.c_str());
				Wui::WuiAccessNode node;
				node.Id = Wui::HashId(("animgraph.state_graph.node." + name).c_str());
				node.Window = Wui::WuiAccessibility::Get().CurrentWindow();
				node.Panel = Wui::WuiAccessibility::Get().CurrentPanel();
				node.Kind = "state-node";
				node.Label = name + " · " + badgeText;
				node.Value = value;
				node.Tooltip = nodeTooltip;
				node.Rect = rect;
				node.Interactive = true;
				Wui::WuiAccessibility::Get().Register(node);
			}

		}

		registerCanvas();
	}
}
