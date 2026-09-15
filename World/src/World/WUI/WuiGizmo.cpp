// D7-1b:真 3D gizmo(ImGuizmo 式)——屏幕空间恒定尺寸、深度排序、射线约束拖动。
// 绘制走 WuiDrawKind::Quad(任意四边形):斜线/箭头/圆环都能正确成形,
// 不再用轴对齐矩形近似(那样斜轴会画成斜边包围盒)。
#include "wldpch.h"
#include "WuiGizmo.h"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <array>
#include <cmath>

namespace World::Wui
{
	namespace
	{
		constexpr float kAxisPixels = 64.0f;    // 轴长(屏幕像素)
		constexpr float kLineThickness = 3.5f;
		constexpr float kArrowSize = 12.0f;
		constexpr float kBoxSize = 11.0f;
		constexpr float kRingPixels = 52.0f;    // 旋转环半径(屏幕像素)
		constexpr int kRingSegments = 40;
		constexpr float kPickTolerance = 8.0f;  // 命中容差(像素)

		const WuiColor kAxisColors[3] = {
			{ 0.92f, 0.30f, 0.30f, 1.0f },   // X 红
			{ 0.38f, 0.88f, 0.42f, 1.0f },   // Y 绿
			{ 0.36f, 0.58f, 0.96f, 1.0f },   // Z 蓝
		};
		const WuiColor kHighlight { 1.0f, 0.95f, 0.35f, 1.0f };

		glm::vec2 WorldToScreen(const GizmoCamera& camera, const glm::vec3& world, const WuiRect& viewport)
		{
			const glm::vec4 clip = camera.ViewProjection * glm::vec4(world, 1.0f);
			if (clip.w <= 1e-6f)
				return { -1e9f, -1e9f };
			const glm::vec3 ndc = glm::vec3(clip) / clip.w;
			return { viewport.X + (ndc.x + 1.0f) * 0.5f * viewport.W,
				viewport.Y + (1.0f - ndc.y) * 0.5f * viewport.H };
		}

		float WorldPerPixel(const GizmoCamera& camera, const WuiRect& viewport)
		{
			const float height = std::max(1.0f, viewport.H);
			return 2.0f * camera.Distance * std::tan(glm::radians(camera.FovDegrees) * 0.5f) / height;
		}

		void PushQuad(WuiContext& ctx, const glm::vec2& a, const glm::vec2& b, const glm::vec2& c,
			const glm::vec2& d, const WuiColor& color)
		{
			WuiDrawCommand command;
			command.Kind = WuiDrawKind::Quad;
			command.Color = color;
			command.Vertices = { a, b, c, d };
			ctx.Commands().push_back(std::move(command));
		}

		// 任意方向线段 → 四边形。
		void PushLine(WuiContext& ctx, const glm::vec2& from, const glm::vec2& to,
			const WuiColor& color, float thickness)
		{
			const glm::vec2 delta = to - from;
			const float length = glm::length(delta);
			if (length < 0.5f)
				return;
			const glm::vec2 normal { -delta.y / length, delta.x / length };
			const glm::vec2 offset = normal * (thickness * 0.5f);
			PushQuad(ctx, from + offset, to + offset, to - offset, from - offset, color);
		}

		// 箭头:线 + 三角(用两个顶点重合的四边形表示)。
		void PushArrow(WuiContext& ctx, const glm::vec2& from, const glm::vec2& to, const WuiColor& color)
		{
			const glm::vec2 delta = to - from;
			const float length = glm::length(delta);
			if (length < 1.0f)
				return;
			const glm::vec2 dir = delta / length;
			const glm::vec2 normal { -dir.y, dir.x };
			const glm::vec2 base = to - dir * kArrowSize;
			PushLine(ctx, from, base, color, kLineThickness);
			PushQuad(ctx, to, base + normal * (kArrowSize * 0.45f), base + normal * (kArrowSize * 0.45f),
				base - normal * (kArrowSize * 0.45f), color);
		}

		void PushBox(WuiContext& ctx, const glm::vec2& center, const WuiColor& color, float size)
		{
			const glm::vec2 half { size * 0.5f, size * 0.5f };
			PushQuad(ctx, center - half, center + glm::vec2 { half.x, -half.y },
				center + half, center + glm::vec2 { -half.x, half.y }, color);
		}
	}

	bool ManipulateGizmo(const GizmoCamera& camera, GizmoOperation operation,
		TransformComponent& transform, const WuiRect& viewport, WuiContext& ctx,
		bool allowManipulation)
	{
		if (operation == GizmoOperation::None)
			return false;

		const glm::vec2 center = WorldToScreen(camera, transform.Location, viewport);
		if (center.x < -1e8f)
			return false;

		// 轴取**物体自身坐标系**(Local):从已缓存的变换矩阵取列向量并归一化,
		// 这样箭头方向与物体朝向一致(用户反馈"与世界轴不一致");同时避免依赖欧拉角单位约定。
		glm::vec3 axisWorld[3] = {
			{ 1.0f, 0.0f, 0.0f }, { 0.0f, 1.0f, 0.0f }, { 0.0f, 0.0f, 1.0f }
		};
		{
			const glm::mat3 basis = glm::mat3(transform.Transform);
			for (int i = 0; i < 3; ++i)
			{
				const float length = glm::length(basis[i]);
				if (length > 1e-5f)
					axisWorld[i] = basis[i] / length;
			}
		}
		const float unit = kAxisPixels * WorldPerPixel(camera, viewport);

		// 轴向屏幕方向(退化轴 = 与视线平行,长度近 0 → 不画也不可拖)。
		glm::vec2 axisScreen[3] = {};
		bool axisValid[3] = { false, false, false };
		for (int i = 0; i < 3; ++i)
		{
			const glm::vec2 tip = WorldToScreen(camera, transform.Location + axisWorld[i] * unit, viewport);
			const glm::vec2 delta = tip - center;
			if (glm::length(delta) < 6.0f)
				continue;
			axisScreen[i] = glm::normalize(delta);
			axisValid[i] = true;
		}

		// 旋转环:每个轴一个圆,投影成椭圆;半径用屏幕像素估值(与轴长一致)。
		auto ringRadiusFor = [&](int axisIndex)
		{
			const glm::vec3& normal = axisWorld[axisIndex];
			const glm::vec3 tangentA = glm::normalize(glm::cross(normal, camera.Up + glm::vec3 { 0.001f, 0.0f, 0.0f }));
			const glm::vec3 tangentB = glm::cross(normal, tangentA);
			const float worldRadius = kRingPixels * WorldPerPixel(camera, viewport);
			std::array<glm::vec2, kRingSegments> points;
			for (int i = 0; i < kRingSegments; ++i)
			{
				const float angle = glm::two_pi<float>() * static_cast<float>(i) / kRingSegments;
				points[i] = WorldToScreen(camera,
					transform.Location + (tangentA * std::cos(angle) + tangentB * std::sin(angle)) * worldRadius,
					viewport);
			}
			return points;
		};

		// ---- 命中判定(先算,再决定高亮与拖拽)----
		const glm::vec2 mouse = ctx.Input().MousePos;
		int hoveredAxis = -1;
		if (ctx.IsHovered(viewport))
		{
			for (int i = 0; i < 3; ++i)
			{
				if (!axisValid[i])
					continue;
				const glm::vec2 toMouse = mouse - center;
				const float along = glm::dot(toMouse, axisScreen[i]);
				if (along < 10.0f || along > kAxisPixels + 12.0f)
					continue;
				const glm::vec2 closest = center + axisScreen[i] * along;
				if (glm::length(mouse - closest) <= kPickTolerance)
				{
					hoveredAxis = i;
					break;
				}
			}
		}

		// ---- 绘制(按轴与相机朝向排序:远的先画)----
		int order[3] = { 0, 1, 2 };
		std::sort(order, order + 3, [&](int a, int b)
		{
			return glm::dot(axisWorld[a], camera.Forward) > glm::dot(axisWorld[b], camera.Forward);
		});

		const bool centralHandle = operation == GizmoOperation::Scale || operation == GizmoOperation::Rotate;
		if (centralHandle)
			PushBox(ctx, center, hoveredAxis < 0 ? kAxisColors[1] : kHighlight, kArrowSize);

		for (const int i : order)
		{
			const WuiColor color = (hoveredAxis == i) ? kHighlight : kAxisColors[i];
			if (operation == GizmoOperation::Rotate)
			{
				const auto ring = ringRadiusFor(i);
				for (int s = 0; s < kRingSegments; ++s)
					PushLine(ctx, ring[s], ring[(s + 1) % kRingSegments], color, 2.5f);
				continue;
			}
			if (!axisValid[i])
				continue;
			const glm::vec2 tip = center + axisScreen[i] * kAxisPixels;
			if (operation == GizmoOperation::Translate)
				PushArrow(ctx, center, tip, color);
			else
			{
				PushLine(ctx, center, tip, color, kLineThickness);
				PushBox(ctx, tip, color, kBoxSize);
			}
		}

		if (!allowManipulation)
			return false;   // Play/Simulate:只读查看

		// ---- 拖拽 ----
		static bool dragging = false;
		static int activeAxis = -1;
		static glm::vec2 startMouse {};
		// 拖拽期间锁存参考系:轴/中心会随物体移动,若每帧重新采样就会形成反馈回路 → 抖动。
		static glm::vec2 startAxisScreen[3] = {};
		static glm::vec2 startCenter {};
		static glm::vec3 startLocation {};
		static glm::vec3 startScale {};
		static float startAngle = 0.0f;
		static float startPointerAngle = 0.0f;

		if (!dragging && ctx.Input().MouseClicked[0] && ctx.IsHovered(viewport))
		{
			int picked = hoveredAxis;
			if (picked < 0 && operation == GizmoOperation::Rotate)
			{
				// 环命中:算鼠标到**投影环折线**的最短距离,取最近的那个环。
				// (此前用"到中心的距离 ≈ 半径"近似,投影成椭圆后完全不准 → 很难选中对应轴。)
				float bestDistance = kPickTolerance + 2.0f;
				for (int i = 0; i < 3; ++i)
				{
					const auto ring = ringRadiusFor(i);
					float ringDistance = 1e9f;
					for (int s = 0; s < kRingSegments; ++s)
					{
						const glm::vec2& a = ring[s];
						const glm::vec2& b = ring[(s + 1) % kRingSegments];
						const glm::vec2 ab = b - a;
						const float lengthSquared = glm::dot(ab, ab);
						const float t = lengthSquared > 1e-6f
							? glm::clamp(glm::dot(mouse - a, ab) / lengthSquared, 0.0f, 1.0f) : 0.0f;
						ringDistance = std::min(ringDistance, glm::length(mouse - (a + ab * t)));
					}
					if (ringDistance <= kPickTolerance && ringDistance < bestDistance)
					{
						bestDistance = ringDistance;
						picked = i;
					}
				}
			}
			else if (picked < 0 && operation == GizmoOperation::Scale
				&& glm::length(mouse - center) <= kArrowSize)
			{
				picked = -2;   // 中心方块 = 等比缩放
			}

			if (picked != -1)
			{
				dragging = true;
				activeAxis = picked;
				startMouse = mouse;
				startCenter = center;
				for (int i = 0; i < 3; ++i)
					startAxisScreen[i] = axisScreen[i];
				startLocation = transform.Location;
				startScale = transform.Scale;
				startAngle = transform.Rotation.z;
				startPointerAngle = std::atan2(mouse.y - startCenter.y, mouse.x - startCenter.x);
			}
		}

		if (dragging && ctx.Input().MouseDown[0])
		{
			// 一律相对"按下时"的锁存状态计算(绝对量),不再逐帧累加 → 不抖。
			const glm::vec2 delta = mouse - startMouse;
			const float worldPerPixel = WorldPerPixel(camera, viewport);

			if (operation == GizmoOperation::Translate && activeAxis >= 0 && activeAxis < 3)
			{
				const float pixels = glm::dot(delta, startAxisScreen[activeAxis]);
				transform.Location = startLocation + axisWorld[activeAxis] * (pixels * worldPerPixel);
			}
			else if (operation == GizmoOperation::Scale)
			{
				// 沿"选中轴的屏幕方向"投影位移;轴指向相机(屏幕投影退化)时改用
				// "离 gizmo 中心的距离变化" → 往外拉一定变大,不会出现"往外拉反而缩小"。
				float drive = 0.0f;
				if (activeAxis >= 0 && activeAxis < 3)
				{
					const glm::vec2 axisDirection = startAxisScreen[activeAxis];
					const float axisLength = glm::length(axisDirection);
					if (axisLength > 0.3f)
						drive = glm::dot(delta, axisDirection / axisLength);
					else
						drive = glm::length(mouse - startCenter) - glm::length(startMouse - startCenter);
				}
				else
				{
					drive = delta.x - delta.y;   // 中心方块:等比缩放(保留原有手感)
				}
				const float amount = 1.0f + drive * 0.006f;
				if (activeAxis == 0)
					transform.Scale.x = std::max(0.01f, startScale.x * amount);
				else if (activeAxis == 1)
					transform.Scale.y = std::max(0.01f, startScale.y * amount);
				else if (activeAxis == 2)
					transform.Scale.z = std::max(0.01f, startScale.z * amount);
				else
					transform.Scale = glm::max(startScale * amount, glm::vec3(0.01f));
			}
			else if (operation == GizmoOperation::Rotate)
			{
				// 拖动角度 = 鼠标绕(锁存的)gizmo 中心的极角差,按选中轴作用到对应欧拉角。
				const float pointerAngle = std::atan2(mouse.y - startCenter.y, mouse.x - startCenter.x);
				const float degrees = glm::degrees(pointerAngle - startPointerAngle);
				if (activeAxis == 0)
					transform.Rotation.x = startAngle + degrees;
				else if (activeAxis == 1)
					transform.Rotation.y = startAngle + degrees;
				else if (activeAxis == 2)
					transform.Rotation.z = startAngle + degrees;
			}

			transform.RecalculateTransform();
		}

		if (dragging && ctx.Input().MouseReleased[0])
			dragging = false;

		// 返回值 = "本帧 gizmo 是否占用鼠标":拖拽中,或指针正悬停在手柄/圆环/中心块上。
		// 视口面板据此**优先于实体拾取**处理点击,否则点箭头会先被"点空白清除选择"吃掉(实测拖不动)。
		const bool hoveringHandle = hoveredAxis >= 0
			|| (operation == GizmoOperation::Scale && glm::length(mouse - center) <= kArrowSize)
			|| (operation == GizmoOperation::Rotate && glm::length(mouse - center) <= kArrowSize);
		return dragging || (hoveringHandle && ctx.IsHovered(viewport));
	}
}
