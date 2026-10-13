#include "WUI/Panels/ViewportPanel_ToolGizmos.h"
#include "wldpch.h"
#include "World/Scene/Components.h"
#include "World/Scene/Entity.h"
#include "World/WUI/WuiGizmo.h"
#include "World/WUI/WuiWidgets.h"
#include "World/WUI/Widgets/WuiChrome.h"
#include "World/Core/StringPool.h"

#include <vector>
#include <algorithm>
#include <cmath>

namespace World
{
	// 自由多边形光源视口手柄: 绘制多边形闭合线框, 并在各个顶点绘制控制手柄圆点, 支持鼠标拖拽移动顶点
	bool ManipulateFreeformLight2D(const Wui::GizmoCamera& camera,
		const TransformComponent& transform, FreeformLight2DComponent& light,
		const Wui::WuiRect& viewport, Wui::WuiContext& ctx,
		bool allowManipulation)
	{
		auto pts = light.GetLocalPolygon();
		if (pts.size() < 3)
			return false;

		const glm::quat rot = transform.Rotation;
		const size_t n = pts.size();
		std::vector<glm::vec2> screenPts;
		screenPts.reserve(n);
		bool allValid = true;

		for (const auto& localPt : pts)
		{
			const glm::vec3 worldPos = transform.Location + rot * glm::vec3(localPt.x * transform.Scale.x, localPt.y * transform.Scale.y, 0.0f);
			const glm::vec4 clip = camera.ViewProjection * glm::vec4(worldPos, 1.0f);
			if (clip.w <= 1e-4f)
			{
				allValid = false;
				break;
			}
			const glm::vec3 ndc = glm::vec3(clip) / clip.w;
			screenPts.push_back({
				viewport.X + (ndc.x + 1.0f) * 0.5f * viewport.W,
				viewport.Y + (1.0f - ndc.y) * 0.5f * viewport.H
			});
		}

		if (!allValid || screenPts.size() < 3)
			return false;

		// 1. 渲染光照多边形闭合线框 (鲜亮柔和的光源色调)
		const Wui::WuiColor wireColor {
			std::clamp(light.Color.r, 0.35f, 1.0f),
			std::clamp(light.Color.g, 0.35f, 1.0f),
			std::clamp(light.Color.b, 0.35f, 1.0f),
			0.95f
		};
		for (size_t i = 0; i < n; ++i)
		{
			const glm::vec2& p0 = screenPts[i];
			const glm::vec2& p1 = screenPts[(i + 1) % n];
			Wui::LineSegment(ctx, p0, p1, wireColor, 2.0f);
		}

		// 2. 控制圆点手柄拾取检测
		const glm::vec2 mouse = ctx.Input().MousePos;
		int hovered = -1;
		if (ctx.IsHovered(viewport))
		{
			float bestDist = 12.0f; // 12px 拾取判定阈值
			for (size_t i = 0; i < n; ++i)
			{
				const float d = glm::length(mouse - screenPts[i]);
				if (d <= bestDist)
				{
					bestDist = d;
					hovered = static_cast<int>(i);
				}
			}
		}

		// 3. 绘制顶点控制圆点手柄 (未激活天青蓝, 悬停/激活亮金黄)
		for (size_t i = 0; i < n; ++i)
		{
			const bool isHov = (hovered == static_cast<int>(i));
			const float r = isHov ? 6.5f : 5.0f;
			const Wui::WuiRect hRect { screenPts[i].x - r, screenPts[i].y - r, r * 2.0f, r * 2.0f };
			const Wui::WuiColor hCol = isHov
				? Wui::WuiColor { 1.0f, 0.92f, 0.25f, 1.0f }
				: Wui::WuiColor { 0.25f, 0.85f, 1.0f, 0.95f };
			const Wui::WuiColor borderCol { 1.0f, 1.0f, 1.0f, 0.95f };
			Wui::PanelBackground(ctx, hRect, hCol, r);
			Wui::HighlightOutline(ctx, hRect, borderCol, r, 1.5f);
		}

		// 4. 鼠标直接拖拽顶点移动
		if (!allowManipulation)
			return hovered >= 0;

		static bool s_Dragging = false;
		static int s_ActiveIndex = -1;
		static glm::vec2 s_StartMouse {};
		static glm::vec2 s_StartVertex {};

		if (!s_Dragging && ctx.Input().MouseClicked[0] && ctx.IsHovered(viewport) && hovered >= 0)
		{
			s_Dragging = true;
			s_ActiveIndex = hovered;
			s_StartMouse = mouse;
			s_StartVertex = pts[static_cast<size_t>(hovered)];
		}

		if (s_Dragging && ctx.Input().MouseDown[0])
		{
			const glm::vec2 mouseDelta = mouse - s_StartMouse;
			const float height = std::max(1.0f, viewport.H);
			float wpp = 0.01f;
			if (camera.IsOrthographic)
				wpp = std::max(0.0001f, camera.OrthographicSize) / height;
			else
				wpp = 2.0f * camera.Distance * std::tan(glm::radians(camera.FovDegrees) * 0.5f) / height;

			const glm::vec3 worldMove = camera.Right * (mouseDelta.x * wpp) - camera.Up * (mouseDelta.y * wpp);
			const glm::quat invRot = glm::conjugate(rot);
			const glm::vec3 unrotated = invRot * worldMove;
			const float sx = (std::abs(transform.Scale.x) > 1e-4f) ? transform.Scale.x : 1.0f;
			const float sy = (std::abs(transform.Scale.y) > 1e-4f) ? transform.Scale.y : 1.0f;
			const glm::vec2 localDelta { unrotated.x / sx, unrotated.y / sy };

			if (static_cast<size_t>(s_ActiveIndex) < pts.size())
			{
				pts[static_cast<size_t>(s_ActiveIndex)] = s_StartVertex + localDelta;
				light.SetPoints(pts);
			}
		}

		if (s_Dragging && !ctx.Input().MouseDown[0])
		{
			s_Dragging = false;
			s_ActiveIndex = -1;
		}

		return s_Dragging || (hovered >= 0);
	}

	bool ManipulateFreeformLight2D(const Wui::GizmoCamera& camera,
		Entity selected, const Wui::WuiRect& viewport, Wui::WuiContext& ctx,
		bool allowManipulation)
	{
		if (!selected.IsValid() || !selected.HasComponent<FreeformLight2DComponent>() || !selected.HasComponent<TransformComponent>())
			return false;
		auto& light = selected.GetComponent<FreeformLight2DComponent>();
		auto& transform = selected.GetComponent<TransformComponent>();
		return ManipulateFreeformLight2D(camera, transform, light, viewport, ctx, allowManipulation);
	}

	// 2D 多边形碰撞体视口手柄: 绘制多边形闭合线框, 并在各个顶点绘制控制手柄圆点, 支持鼠标拖拽移动顶点实时写回 Points
	bool ManipulatePolygonCollider2D(const Wui::GizmoCamera& camera,
		const TransformComponent& transform, PolygonCollider2DComponent& poly,
		const Wui::WuiRect& viewport, Wui::WuiContext& ctx,
		bool allowManipulation)
	{
		auto pts = poly.GetPoints();
		if (pts.size() < 3)
			return false;

		const glm::quat rot = transform.Rotation;
		const size_t n = pts.size();
		std::vector<glm::vec2> screenPts;
		screenPts.reserve(n);
		bool allValid = true;

		for (const auto& localPt : pts)
		{
			const glm::vec3 worldPos = transform.Location + rot * glm::vec3(
				poly.Offset.x + localPt.x * transform.Scale.x,
				poly.Offset.y + localPt.y * transform.Scale.y,
				0.0f
			);
			const glm::vec4 clip = camera.ViewProjection * glm::vec4(worldPos, 1.0f);
			if (clip.w <= 1e-4f)
			{
				allValid = false;
				break;
			}
			const glm::vec3 ndc = glm::vec3(clip) / clip.w;
			screenPts.push_back({
				viewport.X + (ndc.x + 1.0f) * 0.5f * viewport.W,
				viewport.Y + (1.0f - ndc.y) * 0.5f * viewport.H
			});
		}

		if (!allValid || screenPts.size() < 3)
			return false;

		// 1. 渲染多边形闭合线框 (鲜亮翠绿物理碰撞色)
		const Wui::WuiColor wireColor { 0.2f, 0.92f, 0.35f, 0.95f };
		for (size_t i = 0; i < n; ++i)
		{
			const glm::vec2& p0 = screenPts[i];
			const glm::vec2& p1 = screenPts[(i + 1) % n];
			Wui::LineSegment(ctx, p0, p1, wireColor, 2.0f);
		}

		// 2. 控制圆点手柄拾取检测
		const glm::vec2 mouse = ctx.Input().MousePos;
		int hovered = -1;
		if (ctx.IsHovered(viewport))
		{
			float bestDist = 12.0f; // 12px 拾取判定阈值
			for (size_t i = 0; i < n; ++i)
			{
				const float d = glm::length(mouse - screenPts[i]);
				if (d <= bestDist)
				{
					bestDist = d;
					hovered = static_cast<int>(i);
				}
			}
		}

		// 3. 绘制顶点控制圆点手柄 (未激活翡翠绿, 悬停/激活亮金黄; 半径 5px / 6.5px)
		for (size_t i = 0; i < n; ++i)
		{
			const bool isHov = (hovered == static_cast<int>(i));
			const float r = isHov ? 6.5f : 5.0f;
			const Wui::WuiRect hRect { screenPts[i].x - r, screenPts[i].y - r, r * 2.0f, r * 2.0f };
			const Wui::WuiColor hCol = isHov
				? Wui::WuiColor { 1.0f, 0.92f, 0.25f, 1.0f }
				: Wui::WuiColor { 0.2f, 0.85f, 0.45f, 0.95f };
			const Wui::WuiColor borderCol { 1.0f, 1.0f, 1.0f, 0.95f };
			Wui::PanelBackground(ctx, hRect, hCol, r);
			Wui::HighlightOutline(ctx, hRect, borderCol, r, 1.5f);
		}

		// 4. 鼠标直接拖拽顶点移动
		if (!allowManipulation)
			return hovered >= 0;

		static bool s_PolyDragging = false;
		static int s_PolyActiveIndex = -1;
		static glm::vec2 s_PolyStartMouse {};
		static glm::vec2 s_PolyStartVertex {};

		if (!s_PolyDragging && ctx.Input().MouseClicked[0] && ctx.IsHovered(viewport) && hovered >= 0)
		{
			s_PolyDragging = true;
			s_PolyActiveIndex = hovered;
			s_PolyStartMouse = mouse;
			s_PolyStartVertex = pts[static_cast<size_t>(hovered)];
		}

		if (s_PolyDragging && ctx.Input().MouseDown[0])
		{
			const glm::vec2 mouseDelta = mouse - s_PolyStartMouse;
			const float height = std::max(1.0f, viewport.H);
			float wpp = 0.01f;
			if (camera.IsOrthographic)
				wpp = std::max(0.0001f, camera.OrthographicSize) / height;
			else
				wpp = 2.0f * camera.Distance * std::tan(glm::radians(camera.FovDegrees) * 0.5f) / height;

			const glm::vec3 worldMove = camera.Right * (mouseDelta.x * wpp) - camera.Up * (mouseDelta.y * wpp);
			const glm::quat invRot = glm::conjugate(rot);
			const glm::vec3 unrotated = invRot * worldMove;
			const float sx = (std::abs(transform.Scale.x) > 1e-4f) ? transform.Scale.x : 1.0f;
			const float sy = (std::abs(transform.Scale.y) > 1e-4f) ? transform.Scale.y : 1.0f;
			const glm::vec2 localDelta { unrotated.x / sx, unrotated.y / sy };

			if (static_cast<size_t>(s_PolyActiveIndex) < pts.size())
			{
				pts[static_cast<size_t>(s_PolyActiveIndex)] = s_PolyStartVertex + localDelta;
				poly.SetPoints(pts);
			}
		}

		if (s_PolyDragging && !ctx.Input().MouseDown[0])
		{
			s_PolyDragging = false;
			s_PolyActiveIndex = -1;
		}

		return s_PolyDragging || (hovered >= 0);
	}

	bool ManipulatePolygonCollider2D(const Wui::GizmoCamera& camera,
		Entity selected, const Wui::WuiRect& viewport, Wui::WuiContext& ctx,
		bool allowManipulation)
	{
		if (!selected.IsValid() || !selected.HasComponent<PolygonCollider2DComponent>() || !selected.HasComponent<TransformComponent>())
			return false;
		auto& poly = selected.GetComponent<PolygonCollider2DComponent>();
		auto& transform = selected.GetComponent<TransformComponent>();
		return ManipulatePolygonCollider2D(camera, transform, poly, viewport, ctx, allowManipulation);
	}

	// 2D 折线链/地表碰撞体视口手柄: 绘制折线轮廓, 并在各个顶点绘制控制手柄圆点, 支持鼠标拖拽移动顶点实时写回 Points
	bool ManipulateChainCollider2D(const Wui::GizmoCamera& camera,
		const TransformComponent& transform, ChainCollider2DComponent& chain,
		const Wui::WuiRect& viewport, Wui::WuiContext& ctx,
		bool allowManipulation)
	{
		auto pts = chain.GetPoints();
		if (pts.size() < 2)
			return false;

		const glm::quat rot = transform.Rotation;
		const size_t n = pts.size();
		std::vector<glm::vec2> screenPts;
		screenPts.reserve(n);
		bool allValid = true;

		for (const auto& localPt : pts)
		{
			const glm::vec3 worldPos = transform.Location + rot * glm::vec3(
				chain.Offset.x + localPt.x * transform.Scale.x,
				chain.Offset.y + localPt.y * transform.Scale.y,
				0.0f
			);
			const glm::vec4 clip = camera.ViewProjection * glm::vec4(worldPos, 1.0f);
			if (clip.w <= 1e-4f)
			{
				allValid = false;
				break;
			}
			const glm::vec3 ndc = glm::vec3(clip) / clip.w;
			screenPts.push_back({
				viewport.X + (ndc.x + 1.0f) * 0.5f * viewport.W,
				viewport.Y + (1.0f - ndc.y) * 0.5f * viewport.H
			});
		}

		if (!allValid || screenPts.size() < 2)
			return false;

		// 1. 渲染折线轮廓 (清爽亮天蓝)
		const Wui::WuiColor wireColor { 0.3f, 0.85f, 1.0f, 0.95f };
		const size_t segmentCount = chain.Loop ? n : (n - 1);
		for (size_t i = 0; i < segmentCount; ++i)
		{
			const glm::vec2& p0 = screenPts[i];
			const glm::vec2& p1 = screenPts[(i + 1) % n];
			Wui::LineSegment(ctx, p0, p1, wireColor, 2.0f);
		}

		// 2. 控制圆点手柄拾取检测
		const glm::vec2 mouse = ctx.Input().MousePos;
		int hovered = -1;
		if (ctx.IsHovered(viewport))
		{
			float bestDist = 12.0f; // 12px 拾取判定阈值
			for (size_t i = 0; i < n; ++i)
			{
				const float d = glm::length(mouse - screenPts[i]);
				if (d <= bestDist)
				{
					bestDist = d;
					hovered = static_cast<int>(i);
				}
			}
		}

		// 3. 绘制顶点控制圆点手柄 (未激活亮蓝, 悬停/激活亮金黄; 半径 5px / 6.5px)
		for (size_t i = 0; i < n; ++i)
		{
			const bool isHov = (hovered == static_cast<int>(i));
			const float r = isHov ? 6.5f : 5.0f;
			const Wui::WuiRect hRect { screenPts[i].x - r, screenPts[i].y - r, r * 2.0f, r * 2.0f };
			const Wui::WuiColor hCol = isHov
				? Wui::WuiColor { 1.0f, 0.92f, 0.25f, 1.0f }
				: Wui::WuiColor { 0.25f, 0.75f, 1.0f, 0.95f };
			const Wui::WuiColor borderCol { 1.0f, 1.0f, 1.0f, 0.95f };
			Wui::PanelBackground(ctx, hRect, hCol, r);
			Wui::HighlightOutline(ctx, hRect, borderCol, r, 1.5f);
		}

		// 4. 鼠标直接拖拽顶点移动
		if (!allowManipulation)
			return hovered >= 0;

		static bool s_ChainDragging = false;
		static int s_ChainActiveIndex = -1;
		static glm::vec2 s_ChainStartMouse {};
		static glm::vec2 s_ChainStartVertex {};

		if (!s_ChainDragging && ctx.Input().MouseClicked[0] && ctx.IsHovered(viewport) && hovered >= 0)
		{
			s_ChainDragging = true;
			s_ChainActiveIndex = hovered;
			s_ChainStartMouse = mouse;
			s_ChainStartVertex = pts[static_cast<size_t>(hovered)];
		}

		if (s_ChainDragging && ctx.Input().MouseDown[0])
		{
			const glm::vec2 mouseDelta = mouse - s_ChainStartMouse;
			const float height = std::max(1.0f, viewport.H);
			float wpp = 0.01f;
			if (camera.IsOrthographic)
				wpp = std::max(0.0001f, camera.OrthographicSize) / height;
			else
				wpp = 2.0f * camera.Distance * std::tan(glm::radians(camera.FovDegrees) * 0.5f) / height;

			const glm::vec3 worldMove = camera.Right * (mouseDelta.x * wpp) - camera.Up * (mouseDelta.y * wpp);
			const glm::quat invRot = glm::conjugate(rot);
			const glm::vec3 unrotated = invRot * worldMove;
			const float sx = (std::abs(transform.Scale.x) > 1e-4f) ? transform.Scale.x : 1.0f;
			const float sy = (std::abs(transform.Scale.y) > 1e-4f) ? transform.Scale.y : 1.0f;
			const glm::vec2 localDelta { unrotated.x / sx, unrotated.y / sy };

			if (static_cast<size_t>(s_ChainActiveIndex) < pts.size())
			{
				pts[static_cast<size_t>(s_ChainActiveIndex)] = s_ChainStartVertex + localDelta;
				chain.SetPoints(pts);
			}
		}

		if (s_ChainDragging && !ctx.Input().MouseDown[0])
		{
			s_ChainDragging = false;
			s_ChainActiveIndex = -1;
		}

		return s_ChainDragging || (hovered >= 0);
	}

	bool ManipulateChainCollider2D(const Wui::GizmoCamera& camera,
		Entity selected, const Wui::WuiRect& viewport, Wui::WuiContext& ctx,
		bool allowManipulation)
	{
		if (!selected.IsValid() || !selected.HasComponent<ChainCollider2DComponent>() || !selected.HasComponent<TransformComponent>())
			return false;
		auto& chain = selected.GetComponent<ChainCollider2DComponent>();
		auto& transform = selected.GetComponent<TransformComponent>();
		return ManipulateChainCollider2D(camera, transform, chain, viewport, ctx, allowManipulation);
	}
}
