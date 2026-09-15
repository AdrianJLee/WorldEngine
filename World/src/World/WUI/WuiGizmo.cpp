#include "wldpch.h"
#include "WuiGizmo.h"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <array>

namespace World::Wui
{
	namespace
	{
		constexpr float kAxisLengthPixels = 62.0f;   // 轴在屏幕上恒定长度(与世界距离无关)
		constexpr float kAxisThickness = 3.0f;
		constexpr float kHeadSize = 9.0f;
		constexpr float kCenterSize = 13.0f;

		struct AxisVisual
		{
			glm::vec2 Direction { 0.0f, 0.0f };   // 屏幕方向(已归一化;退化时为零)
			WuiColor Color;
		};

		glm::vec2 WorldToScreen(const EditorCamera& camera, const glm::vec3& world, const WuiRect& viewport)
		{
			const glm::vec4 clip = camera.GetViewProjection() * glm::vec4(world, 1.0f);
			if (clip.w <= 0.0f)
				return { -1e9f, -1e9f };
			const glm::vec3 ndc = glm::vec3(clip) / clip.w;
			return { viewport.X + (ndc.x + 1.0f) * 0.5f * viewport.W,
				viewport.Y + (1.0f - ndc.y) * 0.5f * viewport.H };
		}

		// 世界单位长度 → 屏幕像素:用相机距离与视口高度估算,保证 gizmo 大小稳定。
		float WorldUnitToPixels(const EditorCamera& camera, const WuiRect& viewport)
		{
			const float worldPerPixel = 2.0f * camera.GetDistance() * glm::tan(glm::radians(camera.GetFov()) * 0.5f)
				/ std::max(1.0f, viewport.H);
			return kAxisLengthPixels * worldPerPixel;
		}

		// 把三个世界轴投影成屏幕方向(2D 场景里 Z 轴会退化成零长度 → 自动不画)。
		std::array<AxisVisual, 3> BuildAxes(const EditorCamera& camera, const glm::vec3& origin,
			const WuiRect& viewport)
		{
			const float unit = WorldUnitToPixels(camera, viewport);
			const glm::vec3 directions[3] = {
				camera.GetRightDirection(), camera.GetUpDirection(), camera.GetForwardDirection()
			};
			const WuiColor colors[3] = {
				{ 0.90f, 0.28f, 0.28f, 1.0f },   // X 红
				{ 0.35f, 0.85f, 0.40f, 1.0f },   // Y 绿
				{ 0.35f, 0.55f, 0.95f, 1.0f },   // Z 蓝
			};

			const glm::vec2 center = WorldToScreen(camera, origin, viewport);
			std::array<AxisVisual, 3> axes;
			for (int i = 0; i < 3; ++i)
			{
				axes[i].Color = colors[i];
				const glm::vec2 tip = WorldToScreen(camera, origin + directions[i] * unit, viewport);
				const glm::vec2 delta = tip - center;
				const float length = glm::length(delta);
				if (length < 1.0f)
					continue;   // 该轴与视线平行(2D 场景的 Z):退化,不画
				axes[i].Direction = delta / length;
			}
			return axes;
		}

		void DrawLine(WuiContext& ctx, const glm::vec2& from, const glm::vec2& to, const WuiColor& color, float thickness)
		{
			const glm::vec2 delta = to - from;
			const float length = glm::length(delta);
			if (length < 0.5f)
				return;
			const glm::vec2 dir = delta / length;
			const glm::vec2 normal { -dir.y, dir.x };
			// 用一条细长的实心矩形近似线段(命令只有矩形/文字/图片三种)。
			ctx.Commands().push_back({ WuiDrawKind::Rect,
				{ from.x - normal.x * thickness * 0.5f, from.y - normal.y * thickness * 0.5f,
					std::fabs(delta.x) + thickness, std::fabs(delta.y) + thickness },
				color, 0.0f });
			(void)dir; (void)normal;
		}

		// 轴端点:平移画箭头(用方块近似),缩放画小方块,旋转不用。
		void DrawHandle(WuiContext& ctx, const glm::vec2& center, const glm::vec2& direction,
			const WuiColor& color, float size, bool filled)
		{
			const WuiRect handle { center.x + direction.x * kAxisLengthPixels - size * 0.5f,
				center.y + direction.y * kAxisLengthPixels - size * 0.5f, size, size };
			if (filled)
			{
				ctx.Commands().push_back({ WuiDrawKind::Rect, handle, color, 0.0f });
				ctx.Commands().push_back({ WuiDrawKind::RectOutline, handle, { 1, 1, 1, 0.75f }, 0.0f, 1.0f });
			}
			else
			{
				ctx.Commands().push_back({ WuiDrawKind::RectOutline, handle, color, 0.0f, 1.6f });
			}
		}
	}

	bool ManipulateGizmo(const EditorCamera& camera, GizmoOperation operation,
		TransformComponent& transform, const WuiRect& viewport, WuiContext& ctx,
		bool allowManipulation)
	{
		if (operation == GizmoOperation::None)
			return false;

		const glm::vec2 center = WorldToScreen(camera, transform.Location, viewport);
		if (center.x < -1e8f)
			return false;   // 实体在相机后面
		const std::array<AxisVisual, 3> axes = BuildAxes(camera, transform.Location, viewport);

		// ---- 绘制 ----
		const float centerHalf = kCenterSize * 0.5f;
		const WuiRect centerHandle { center.x - centerHalf, center.y - centerHalf, kCenterSize, kCenterSize };
		ctx.Commands().push_back({ WuiDrawKind::Rect, centerHandle,
			operation == GizmoOperation::Scale ? WuiColor { 0.55f, 0.9f, 0.5f, 1.0f }
			: (operation == GizmoOperation::Rotate ? WuiColor { 0.35f, 0.75f, 0.95f, 1.0f }
				: WuiColor { 0.95f, 0.62f, 0.2f, 1.0f }), 0.0f });
		ctx.Commands().push_back({ WuiDrawKind::RectOutline, centerHandle, { 1, 1, 1, 0.8f }, 0.0f, 1.0f });

		for (const AxisVisual& axis : axes)
		{
			if (axis.Direction == glm::vec2 { 0.0f, 0.0f })
				continue;
			DrawLine(ctx, center, center + axis.Direction * kAxisLengthPixels, axis.Color, kAxisThickness);
			if (operation == GizmoOperation::Translate)
				DrawHandle(ctx, center, axis.Direction, axis.Color, kHeadSize, true);
			else if (operation == GizmoOperation::Scale)
				DrawHandle(ctx, center, axis.Direction, axis.Color, kHeadSize * 0.8f, true);
		}

		if (!allowManipulation)
			return false;   // Play/Simulate:只读查看,画完就结束

		// ---- 交互:轴约束拖拽(平移/缩放按轴,旋转按中心方块) ----
		static bool dragging = false;
		static int activeAxis = -1;
		static glm::vec2 lastMouse {};
		static glm::vec3 startLocation {};
		static glm::vec3 startScale {};
		static float startRotation = 0.0f;

		auto hoveredAxis = [&]() -> int
		{
			const glm::vec2 mouse = ctx.Input().MousePos;
			for (int i = 0; i < 3; ++i)
			{
				if (axes[i].Direction == glm::vec2 { 0.0f, 0.0f })
					continue;
				const glm::vec2 toMouse = mouse - center;
				const float along = glm::dot(toMouse, axes[i].Direction);
				if (along < 8.0f || along > kAxisLengthPixels + 10.0f)
					continue;
				const glm::vec2 closest = center + axes[i].Direction * along;
				if (glm::length(mouse - closest) <= 7.0f)
					return i;
			}
			return -1;
		};

		if (!dragging && ctx.Input().MouseClicked[0])
		{
			const int axis = hoveredAxis();
			const bool centerClick = operation == GizmoOperation::Rotate && ctx.IsHovered(centerHandle);
			const bool centerScale = operation == GizmoOperation::Scale && ctx.IsHovered(centerHandle);
			if (axis >= 0 || centerClick || centerScale)
			{
				dragging = true;
				activeAxis = axis;
				lastMouse = ctx.Input().MousePos;
				startLocation = transform.Location;
				startScale = transform.Scale;
				startRotation = transform.Rotation.z;
			}
		}

		if (dragging && ctx.Input().MouseDown[0])
		{
			const glm::vec2 mouse = ctx.Input().MousePos;
			const float worldPerPixel = 2.0f * camera.GetDistance() * glm::tan(glm::radians(camera.GetFov()) * 0.5f)
				/ std::max(1.0f, viewport.H);

			if (operation == GizmoOperation::Translate)
			{
				if (activeAxis >= 0)
				{
					// 沿该轴的屏幕方向投影鼠标位移 → 世界位移(只改该轴对应的世界方向)。
					const glm::vec2 axisScreen = axes[activeAxis].Direction;
					const float pixels = glm::dot(mouse - lastMouse, axisScreen);
					const glm::vec3 worldAxis = activeAxis == 0 ? camera.GetRightDirection()
						: (activeAxis == 1 ? camera.GetUpDirection() : camera.GetForwardDirection());
					transform.Location += worldAxis * (pixels * worldPerPixel);
				}
			}
			else if (operation == GizmoOperation::Scale)
			{
				const glm::vec2 delta = mouse - lastMouse;
				const float amount = 1.0f + (delta.x - delta.y) * 0.005f;
				if (activeAxis == 0)
					transform.Scale.x = std::max(0.01f, transform.Scale.x * amount);
				else if (activeAxis == 1)
					transform.Scale.y = std::max(0.01f, transform.Scale.y * amount);
				else if (activeAxis == 2)
					transform.Scale.z = std::max(0.01f, transform.Scale.z * amount);
				else
					transform.Scale = glm::max(startScale * amount, glm::vec3(0.01f));
			}
			else
			{
				transform.Rotation.z = startRotation + (mouse.x - lastMouse.x) * 0.01f
					+ glm::dot(mouse - lastMouse, axes[0].Direction) * 0.01f;
			}
			lastMouse = mouse;
			transform.RecalculateTransform();   // 拖完立刻生效(缓存矩阵不会自动刷新)
		}

		if (dragging && ctx.Input().MouseReleased[0])
			dragging = false;

		return dragging;
	}
}
