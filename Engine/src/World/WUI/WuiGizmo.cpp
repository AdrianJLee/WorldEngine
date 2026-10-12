// D7-1b:真 3D gizmo(ImGuizmo 式)——屏幕空间恒定尺寸、深度排序、射线约束拖动。
// 绘制走 WuiDrawKind::Quad(任意四边形):斜线/箭头/圆环都能正确成形,
// 不再用轴对齐矩形近似(那样斜轴会画成斜边包围盒)。
#include "wldpch.h"
#include "World/WUI/WuiGizmo.h"

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
			if (camera.IsOrthographic)
				return std::max(0.0001f, camera.OrthographicSize) / height;
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
			const glm::mat3 basis = glm::mat3_cast(transform.Rotation);
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
		int hoveredRing = -1;
		// 环到鼠标的最短距离(折线逐段):高亮、拾取、拖拽共用同一判定,
		// 避免"看着在这个环上却不亮/点不中"。
		const auto ringDistanceFor = [&](int axisIndex)
		{
			const auto ring = ringRadiusFor(axisIndex);
			float best = 1e9f;
			for (int s = 0; s < kRingSegments; ++s)
			{
				const glm::vec2& a = ring[s];
				const glm::vec2& b = ring[(s + 1) % kRingSegments];
				const glm::vec2 ab = b - a;
				const float lengthSquared = glm::dot(ab, ab);
				const float t = lengthSquared > 1e-6f
					? glm::clamp(glm::dot(mouse - a, ab) / lengthSquared, 0.0f, 1.0f) : 0.0f;
				best = std::min(best, glm::length(mouse - (a + ab * t)));
			}
			return best;
		};
		if (ctx.IsHovered(viewport))
		{
			if (operation == GizmoOperation::Rotate)
			{
				float bestDistance = kPickTolerance + 4.0f;
				for (int i = 0; i < 3; ++i)
				{
					const float distance = ringDistanceFor(i);
					if (distance <= bestDistance)
					{
						bestDistance = distance;
						hoveredRing = i;
					}
				}
			}
			else
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
			const bool hovered = (operation == GizmoOperation::Rotate) ? (hoveredRing == i) : (hoveredAxis == i);
			const WuiColor color = hovered ? kHighlight : kAxisColors[i];
			if (operation == GizmoOperation::Rotate)
			{
				const auto ring = ringRadiusFor(i);
				for (int s = 0; s < kRingSegments; ++s)
					PushLine(ctx, ring[s], ring[(s + 1) % kRingSegments], color, hovered ? 3.5f : 2.5f);
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
		// 旋转以 RotationQuat 为准(弧度):必须走 SetRotation,否则只改欧拉角不生效(实测"转不动")。
		static glm::vec3 startEuler {};
		static float startPointerAngle = 0.0f;
		// 旋转角度必须用"鼠标射线 ∩ 环平面"求:屏幕极角在环被压扁(斜视角)时与真实角度不成比例,
		// 三个轴会各转各的(实测"旋转是乱的")。
		static glm::quat startRotationQuat { 1.0f, 0.0f, 0.0f, 0.0f };
		static glm::vec3 rotatePlaneReference { 1.0f, 0.0f, 0.0f };
		// 旋转方向符号:屏幕 Y 朝下(角度顺时针增长),而绕轴的正向是右手系 ——
		// 轴指向相机时两者相反,必须取反,否则拖拽方向整体反着来(实测)。
		static float rotateSign = 1.0f;

		// 屏幕点 → 世界射线(用 gizmo 相机的 VP 反投影;GL 约定,深度 -1..1)。
		const auto screenRay = [&](const glm::vec2& screen, glm::vec3* origin, glm::vec3* direction)
		{
			const glm::mat4 inverse = glm::inverse(camera.ViewProjection);
			const float ndcX = (screen.x - viewport.X) / std::max(1.0f, viewport.W) * 2.0f - 1.0f;
			const float ndcY = 1.0f - (screen.y - viewport.Y) / std::max(1.0f, viewport.H) * 2.0f;
			const glm::vec4 nearClip = inverse * glm::vec4 { ndcX, ndcY, -1.0f, 1.0f };
			const glm::vec4 farClip = inverse * glm::vec4 { ndcX, ndcY, 1.0f, 1.0f };
			const glm::vec3 start = glm::vec3(nearClip) / nearClip.w;
			const glm::vec3 end = glm::vec3(farClip) / farClip.w;
			*origin = start;
			*direction = glm::normalize(end - start);
			return true;
		};
		// 射线 ∩ 平面:把鼠标映射到环所在平面上的点。
		const auto intersectPlane = [](const glm::vec3& origin, const glm::vec3& direction,
			const glm::vec3& planePoint, const glm::vec3& planeNormal, glm::vec3* hit)
		{
			const float denominator = glm::dot(direction, planeNormal);
			if (std::fabs(denominator) < 1e-5f)
				return false;
			const float t = glm::dot(planePoint - origin, planeNormal) / denominator;
			*hit = origin + direction * t;
			return true;
		};

		if (!dragging && ctx.Input().MouseClicked[0] && ctx.IsHovered(viewport))
		{
			int picked = hoveredAxis;
			if (picked < 0 && operation == GizmoOperation::Rotate)
			{
				picked = hoveredRing;   // 与高亮同一判定:亮点即点得中
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
				startEuler = transform.GetEulerAngles();
				startPointerAngle = std::atan2(mouse.y - startCenter.y, mouse.x - startCenter.x);
				rotateSign = (activeAxis >= 0 && activeAxis < 3
					&& glm::dot(axisWorld[activeAxis], -camera.Forward) > 0.0f) ? -1.0f : 1.0f;
				if (operation == GizmoOperation::Rotate && activeAxis >= 0 && activeAxis < 3)
				{
					startRotationQuat = transform.Rotation;
					glm::vec3 rayOrigin {}, rayDirection {}, hit {};
					if (screenRay(mouse, &rayOrigin, &rayDirection)
						&& intersectPlane(rayOrigin, rayDirection, startLocation, axisWorld[activeAxis], &hit))
					{
						const glm::vec3 offset = hit - startLocation;
						if (glm::length(offset) > 1e-4f)
							rotatePlaneReference = glm::normalize(offset);
					}
				}
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
				// 正解:鼠标射线 ∩ 环平面得到平面内向量,与"按下时的参考向量"求有符号角,
				// 再以该轴构造四元数作用到按下时的姿态上(与欧拉角/gimbal 无关)。
				glm::vec3 rayOrigin {}, rayDirection {}, hit {};
				if (activeAxis >= 0 && activeAxis < 3
					&& screenRay(mouse, &rayOrigin, &rayDirection)
					&& intersectPlane(rayOrigin, rayDirection, startLocation, axisWorld[activeAxis], &hit))
				{
					const glm::vec3 current = hit - startLocation;
					if (glm::length(current) > 1e-4f)
					{
						const glm::vec3 currentNormalized = glm::normalize(current);
						const glm::vec3 axis = axisWorld[activeAxis];
						const float angle = std::atan2(
							glm::dot(glm::cross(rotatePlaneReference, currentNormalized), axis),
							glm::dot(rotatePlaneReference, currentNormalized));
						const glm::quat delta = glm::angleAxis(angle, axis);
						transform.SetRotationQuat(delta * startRotationQuat);
						transform.RecalculateTransform();
					}
				}
			}

			if (operation != GizmoOperation::Rotate)
				transform.RecalculateTransform();
		}

		if (dragging && ctx.Input().MouseReleased[0])
			dragging = false;

		// 返回值 = "本帧 gizmo 是否占用鼠标":拖拽中,或指针正悬停在手柄/圆环/中心块上。
		// 视口面板据此**优先于实体拾取**处理点击,否则点箭头会先被"点空白清除选择"吃掉(实测拖不动)。
		const bool hoveringHandle = hoveredAxis >= 0
			|| hoveredRing >= 0
			|| (operation == GizmoOperation::Scale && glm::length(mouse - center) <= kArrowSize)
			|| (operation == GizmoOperation::Rotate && glm::length(mouse - center) <= kArrowSize);
		return dragging || (hoveringHandle && ctx.IsHovered(viewport));
	}

	// =========================================================================
	// 2D 矩形变换手柄 (Rect Transform Tool)
	// =========================================================================
	bool ManipulateRect2D(const GizmoCamera& camera,
		TransformComponent& transform, glm::vec2& pivot, glm::vec2 size,
		const WuiRect& viewport, WuiContext& ctx, bool allowManipulation)
	{
		const float w = std::max(0.01f, size.x * transform.Scale.x);
		const float h = std::max(0.01f, size.y * transform.Scale.y);
		const float x0 = -pivot.x * w;
		const float x1 = (1.0f - pivot.x) * w;
		const float y0 = -pivot.y * h;
		const float y1 = (1.0f - pivot.y) * h;

		const glm::mat3 rot = glm::mat3_cast(transform.Rotation);
		const glm::vec3 cBL = transform.Location + rot * glm::vec3(x0, y0, 0.0f);
		const glm::vec3 cBR = transform.Location + rot * glm::vec3(x1, y0, 0.0f);
		const glm::vec3 cTR = transform.Location + rot * glm::vec3(x1, y1, 0.0f);
		const glm::vec3 cTL = transform.Location + rot * glm::vec3(x0, y1, 0.0f);
		const glm::vec3 cPivot = transform.Location;

		const glm::vec2 sBL = WorldToScreen(camera, cBL, viewport);
		const glm::vec2 sBR = WorldToScreen(camera, cBR, viewport);
		const glm::vec2 sTR = WorldToScreen(camera, cTR, viewport);
		const glm::vec2 sTL = WorldToScreen(camera, cTL, viewport);
		const glm::vec2 sPivot = WorldToScreen(camera, cPivot, viewport);

		if (sBL.x < -1e8f || sBR.x < -1e8f || sTR.x < -1e8f || sTL.x < -1e8f)
			return false;

		const glm::vec2 sLeft = (sBL + sTL) * 0.5f;
		const glm::vec2 sRight = (sBR + sTR) * 0.5f;
		const glm::vec2 sBottom = (sBL + sBR) * 0.5f;
		const glm::vec2 sTop = (sTL + sTR) * 0.5f;

		// 0..3: 角点(BL, BR, TR, TL); 4..7: 边中点(Left, Right, Bottom, Top); 8: Pivot 锚点
		const glm::vec2 handles[9] = { sBL, sBR, sTR, sTL, sLeft, sRight, sBottom, sTop, sPivot };

		const glm::vec2 mouse = ctx.Input().MousePos;
		int hovered = -1;
		if (ctx.IsHovered(viewport))
		{
			float bestDist = kPickTolerance + 2.0f;
			for (int i = 0; i < 9; ++i)
			{
				const float d = glm::length(mouse - handles[i]);
				if (d <= bestDist)
				{
					bestDist = d;
					hovered = i;
				}
			}
		}

		// 绘制矩形边框 (青色)
		const WuiColor rectCol { 0.20f, 0.85f, 0.95f, 0.90f };
		PushLine(ctx, sBL, sBR, rectCol, 1.8f);
		PushLine(ctx, sBR, sTR, rectCol, 1.8f);
		PushLine(ctx, sTR, sTL, rectCol, 1.8f);
		PushLine(ctx, sTL, sBL, rectCol, 1.8f);

		// 绘制角手柄 (白色方块) 与 边中点手柄 (青色方块)
		for (int i = 0; i < 4; ++i)
		{
			PushBox(ctx, handles[i], (hovered == i) ? kHighlight : WuiColor{ 1.0f, 1.0f, 1.0f, 1.0f }, 8.0f);
		}
		for (int i = 4; i < 8; ++i)
		{
			PushBox(ctx, handles[i], (hovered == i) ? kHighlight : rectCol, 6.0f);
		}

		// 绘制中心 Pivot 锚点手柄 (金黄色十字圆心)
		PushBox(ctx, sPivot, (hovered == 8) ? kHighlight : WuiColor{ 1.0f, 0.68f, 0.15f, 1.0f }, 7.0f);

		if (!allowManipulation)
			return false;

		static bool s_RectDragging = false;
		static int s_RectActiveHandle = -1;
		static glm::vec2 s_RectStartMouse {};
		static glm::vec3 s_RectStartScale {};
		static glm::vec2 s_RectStartPivot {};

		if (!s_RectDragging && ctx.Input().MouseClicked[0] && ctx.IsHovered(viewport) && hovered >= 0)
		{
			s_RectDragging = true;
			s_RectActiveHandle = hovered;
			s_RectStartMouse = mouse;
			s_RectStartScale = transform.Scale;
			s_RectStartPivot = pivot;
		}

		if (s_RectDragging && ctx.Input().MouseDown[0])
		{
			const glm::vec2 mouseDelta = mouse - s_RectStartMouse;
			const float wpp = WorldPerPixel(camera, viewport);
			const glm::vec3 worldMove = camera.Right * (mouseDelta.x * wpp) - camera.Up * (mouseDelta.y * wpp);
			const glm::vec3 localAxisX = glm::normalize(rot * glm::vec3(1.0f, 0.0f, 0.0f));
			const glm::vec3 localAxisY = glm::normalize(rot * glm::vec3(0.0f, 1.0f, 0.0f));
			const float localDx = glm::dot(worldMove, localAxisX);
			const float localDy = glm::dot(worldMove, localAxisY);

			if (s_RectActiveHandle == 8) // 锚点拖拽
			{
				float newPx = std::clamp(s_RectStartPivot.x + localDx / std::max(0.01f, size.x * transform.Scale.x), 0.0f, 1.0f);
				float newPy = std::clamp(s_RectStartPivot.y + localDy / std::max(0.01f, size.y * transform.Scale.y), 0.0f, 1.0f);
				if (ctx.Input().Ctrl)
				{
					auto snap = [](float v) {
						if (std::abs(v - 0.5f) < 0.15f) return 0.5f;
						return v < 0.5f ? 0.0f : 1.0f;
					};
					newPx = snap(newPx);
					newPy = snap(newPy);
				}
				pivot.x = newPx;
				pivot.y = newPy;
			}
			else
			{
				const float dx = localDx / std::max(0.01f, size.x);
				const float dy = localDy / std::max(0.01f, size.y);

				if (s_RectActiveHandle == 2) // TR
				{
					transform.Scale.x = std::max(0.01f, s_RectStartScale.x + dx);
					transform.Scale.y = std::max(0.01f, s_RectStartScale.y + dy);
				}
				else if (s_RectActiveHandle == 0) // BL
				{
					transform.Scale.x = std::max(0.01f, s_RectStartScale.x - dx);
					transform.Scale.y = std::max(0.01f, s_RectStartScale.y - dy);
				}
				else if (s_RectActiveHandle == 1) // BR
				{
					transform.Scale.x = std::max(0.01f, s_RectStartScale.x + dx);
					transform.Scale.y = std::max(0.01f, s_RectStartScale.y - dy);
				}
				else if (s_RectActiveHandle == 3) // TL
				{
					transform.Scale.x = std::max(0.01f, s_RectStartScale.x - dx);
					transform.Scale.y = std::max(0.01f, s_RectStartScale.y + dy);
				}
				else if (s_RectActiveHandle == 5) // Right
				{
					transform.Scale.x = std::max(0.01f, s_RectStartScale.x + dx);
				}
				else if (s_RectActiveHandle == 4) // Left
				{
					transform.Scale.x = std::max(0.01f, s_RectStartScale.x - dx);
				}
				else if (s_RectActiveHandle == 7) // Top
				{
					transform.Scale.y = std::max(0.01f, s_RectStartScale.y + dy);
				}
				else if (s_RectActiveHandle == 6) // Bottom
				{
					transform.Scale.y = std::max(0.01f, s_RectStartScale.y - dy);
				}
				transform.RecalculateTransform();
			}
		}

		if (s_RectDragging && ctx.Input().MouseReleased[0])
		{
			s_RectDragging = false;
			s_RectActiveHandle = -1;
		}

		return s_RectDragging || (hovered >= 0 && ctx.IsHovered(viewport));
	}

	// =========================================================================
	// 2D 盒状碰撞体视口手柄 (Box Collider 2D Gizmo)
	// =========================================================================
	bool ManipulateBoxCollider2D(const GizmoCamera& camera,
		const TransformComponent& transform, glm::vec2& offset, glm::vec2& size,
		const WuiRect& viewport, WuiContext& ctx, bool allowManipulation)
	{
		const glm::vec3 centerWorld = transform.Location + glm::vec3(offset.x * transform.Scale.x, offset.y * transform.Scale.y, 0.0f);
		const float hw = (size.x * 0.5f) * std::abs(transform.Scale.x);
		const float hh = (size.y * 0.5f) * std::abs(transform.Scale.y);

		const glm::vec2 sCenter = WorldToScreen(camera, centerWorld, viewport);
		const glm::vec2 sLeft   = WorldToScreen(camera, centerWorld + glm::vec3(-hw, 0.0f, 0.0f), viewport);
		const glm::vec2 sRight  = WorldToScreen(camera, centerWorld + glm::vec3( hw, 0.0f, 0.0f), viewport);
		const glm::vec2 sBottom = WorldToScreen(camera, centerWorld + glm::vec3(0.0f, -hh, 0.0f), viewport);
		const glm::vec2 sTop    = WorldToScreen(camera, centerWorld + glm::vec3(0.0f,  hh, 0.0f), viewport);

		const glm::vec2 sBL = WorldToScreen(camera, centerWorld + glm::vec3(-hw, -hh, 0.0f), viewport);
		const glm::vec2 sBR = WorldToScreen(camera, centerWorld + glm::vec3( hw, -hh, 0.0f), viewport);
		const glm::vec2 sTR = WorldToScreen(camera, centerWorld + glm::vec3( hw,  hh, 0.0f), viewport);
		const glm::vec2 sTL = WorldToScreen(camera, centerWorld + glm::vec3(-hw,  hh, 0.0f), viewport);

		if (sCenter.x < -1e8f || sBL.x < -1e8f || sTR.x < -1e8f)
			return false;

		const glm::vec2 handles[5] = { sLeft, sRight, sBottom, sTop, sCenter };
		const glm::vec2 mouse = ctx.Input().MousePos;
		int hovered = -1;
		if (ctx.IsHovered(viewport))
		{
			float bestDist = kPickTolerance + 2.0f;
			for (int i = 0; i < 5; ++i)
			{
				const float d = glm::length(mouse - handles[i]);
				if (d <= bestDist)
				{
					bestDist = d;
					hovered = i;
				}
			}
		}

		// 绘制碰撞框轮廓 (鲜亮浅绿)
		const WuiColor colGreen { 0.20f, 0.95f, 0.42f, 0.95f };
		PushLine(ctx, sBL, sBR, colGreen, 2.0f);
		PushLine(ctx, sBR, sTR, colGreen, 2.0f);
		PushLine(ctx, sTR, sTL, colGreen, 2.0f);
		PushLine(ctx, sTL, sBL, colGreen, 2.0f);

		// 4 边手柄 + 1 中心手柄
		for (int i = 0; i < 4; ++i)
			PushBox(ctx, handles[i], (hovered == i) ? kHighlight : colGreen, 7.0f);
		PushBox(ctx, sCenter, (hovered == 4) ? kHighlight : WuiColor{ 0.95f, 0.85f, 0.20f, 1.0f }, 6.0f);

		if (!allowManipulation)
			return false;

		static bool s_BoxDragging = false;
		static int s_BoxActiveHandle = -1;
		static glm::vec2 s_BoxStartMouse {};
		static glm::vec2 s_BoxStartSize {};
		static glm::vec2 s_BoxStartOffset {};

		if (!s_BoxDragging && ctx.Input().MouseClicked[0] && ctx.IsHovered(viewport) && hovered >= 0)
		{
			s_BoxDragging = true;
			s_BoxActiveHandle = hovered;
			s_BoxStartMouse = mouse;
			s_BoxStartSize = size;
			s_BoxStartOffset = offset;
		}

		if (s_BoxDragging && ctx.Input().MouseDown[0])
		{
			const glm::vec2 mouseDelta = mouse - s_BoxStartMouse;
			const float wpp = WorldPerPixel(camera, viewport);
			const glm::vec2 worldDelta = mouseDelta * wpp;
			const float sx = std::max(0.001f, std::abs(transform.Scale.x));
			const float sy = std::max(0.001f, std::abs(transform.Scale.y));

			if (s_BoxActiveHandle == 4) // 中心偏移拖拽
			{
				offset.x = s_BoxStartOffset.x + worldDelta.x / sx;
				offset.y = s_BoxStartOffset.y - worldDelta.y / sy;
			}
			else if (s_BoxActiveHandle == 1) // Right
			{
				size.x = std::max(0.05f, s_BoxStartSize.x + (worldDelta.x * 2.0f) / sx);
			}
			else if (s_BoxActiveHandle == 0) // Left
			{
				size.x = std::max(0.05f, s_BoxStartSize.x - (worldDelta.x * 2.0f) / sx);
			}
			else if (s_BoxActiveHandle == 3) // Top
			{
				size.y = std::max(0.05f, s_BoxStartSize.y - (worldDelta.y * 2.0f) / sy);
			}
			else if (s_BoxActiveHandle == 2) // Bottom
			{
				size.y = std::max(0.05f, s_BoxStartSize.y + (worldDelta.y * 2.0f) / sy);
			}
		}

		if (s_BoxDragging && ctx.Input().MouseReleased[0])
		{
			s_BoxDragging = false;
			s_BoxActiveHandle = -1;
		}

		return s_BoxDragging || (hovered >= 0 && ctx.IsHovered(viewport));
	}

	// =========================================================================
	// 2D 圆形碰撞体视口手柄 (Circle Collider 2D Gizmo)
	// =========================================================================
	bool ManipulateCircleCollider2D(const GizmoCamera& camera,
		const TransformComponent& transform, glm::vec2& offset, float& radius,
		const WuiRect& viewport, WuiContext& ctx, bool allowManipulation)
	{
		const glm::vec3 centerWorld = transform.Location + glm::vec3(offset.x * transform.Scale.x, offset.y * transform.Scale.y, 0.0f);
		const float rWorld = radius * std::max(std::abs(transform.Scale.x), std::abs(transform.Scale.y));
		const glm::vec2 sCenter = WorldToScreen(camera, centerWorld, viewport);

		if (sCenter.x < -1e8f)
			return false;

		const glm::vec2 sRight  = WorldToScreen(camera, centerWorld + glm::vec3( rWorld, 0.0f, 0.0f), viewport);
		const glm::vec2 sTop    = WorldToScreen(camera, centerWorld + glm::vec3(0.0f,  rWorld, 0.0f), viewport);
		const glm::vec2 sLeft   = WorldToScreen(camera, centerWorld + glm::vec3(-rWorld, 0.0f, 0.0f), viewport);
		const glm::vec2 sBottom = WorldToScreen(camera, centerWorld + glm::vec3(0.0f, -rWorld, 0.0f), viewport);

		const glm::vec2 handles[5] = { sRight, sTop, sLeft, sBottom, sCenter };
		const glm::vec2 mouse = ctx.Input().MousePos;
		int hovered = -1;
		if (ctx.IsHovered(viewport))
		{
			float bestDist = kPickTolerance + 2.0f;
			for (int i = 0; i < 5; ++i)
			{
				const float d = glm::length(mouse - handles[i]);
				if (d <= bestDist)
				{
					bestDist = d;
					hovered = i;
				}
			}
		}

		// 绘制圆形轮廓
		const WuiColor colGreen { 0.20f, 0.95f, 0.42f, 0.95f };
		constexpr int kSegments = 32;
		glm::vec2 prevPt = WorldToScreen(camera, centerWorld + glm::vec3(rWorld, 0.0f, 0.0f), viewport);
		for (int i = 1; i <= kSegments; ++i)
		{
			const float angle = glm::two_pi<float>() * static_cast<float>(i) / kSegments;
			const glm::vec2 curPt = WorldToScreen(camera, centerWorld + glm::vec3(rWorld * std::cos(angle), rWorld * std::sin(angle), 0.0f), viewport);
			PushLine(ctx, prevPt, curPt, colGreen, 2.0f);
			prevPt = curPt;
		}

		for (int i = 0; i < 4; ++i)
			PushBox(ctx, handles[i], (hovered == i) ? kHighlight : colGreen, 7.0f);
		PushBox(ctx, sCenter, (hovered == 4) ? kHighlight : WuiColor{ 0.95f, 0.85f, 0.20f, 1.0f }, 6.0f);

		if (!allowManipulation)
			return false;

		static bool s_CircleDragging = false;
		static int s_CircleActiveHandle = -1;
		static glm::vec2 s_CircleStartMouse {};
		static float s_CircleStartRadius = 0.5f;
		static glm::vec2 s_CircleStartOffset {};

		if (!s_CircleDragging && ctx.Input().MouseClicked[0] && ctx.IsHovered(viewport) && hovered >= 0)
		{
			s_CircleDragging = true;
			s_CircleActiveHandle = hovered;
			s_CircleStartMouse = mouse;
			s_CircleStartRadius = radius;
			s_CircleStartOffset = offset;
		}

		if (s_CircleDragging && ctx.Input().MouseDown[0])
		{
			const glm::vec2 mouseDelta = mouse - s_CircleStartMouse;
			const float wpp = WorldPerPixel(camera, viewport);
			const glm::vec2 worldDelta = mouseDelta * wpp;
			const float s = std::max(0.001f, std::max(std::abs(transform.Scale.x), std::abs(transform.Scale.y)));

			if (s_CircleActiveHandle == 4) // 中心偏移
			{
				offset.x = s_CircleStartOffset.x + worldDelta.x / s;
				offset.y = s_CircleStartOffset.y - worldDelta.y / s;
			}
			else if (s_CircleActiveHandle == 0) // Right
			{
				radius = std::max(0.05f, s_CircleStartRadius + worldDelta.x / s);
			}
			else if (s_CircleActiveHandle == 1) // Top
			{
				radius = std::max(0.05f, s_CircleStartRadius - worldDelta.y / s);
			}
			else if (s_CircleActiveHandle == 2) // Left
			{
				radius = std::max(0.05f, s_CircleStartRadius - worldDelta.x / s);
			}
			else if (s_CircleActiveHandle == 3) // Bottom
			{
				radius = std::max(0.05f, s_CircleStartRadius + worldDelta.y / s);
			}
		}

		if (s_CircleDragging && ctx.Input().MouseReleased[0])
		{
			s_CircleDragging = false;
			s_CircleActiveHandle = -1;
		}

		return s_CircleDragging || (hovered >= 0 && ctx.IsHovered(viewport));
	}

	// =========================================================================
	// 2D 胶囊体碰撞体视口手柄 (Capsule Collider 2D Gizmo)
	// =========================================================================
	bool ManipulateCapsuleCollider2D(const GizmoCamera& camera,
		const TransformComponent& transform, glm::vec2& offset, float& radius, float& length, int direction,
		const WuiRect& viewport, WuiContext& ctx, bool allowManipulation)
	{
		const glm::vec3 centerWorld = transform.Location + glm::vec3(offset.x * transform.Scale.x, offset.y * transform.Scale.y, 0.0f);
		const float s = std::max(0.001f, std::max(std::abs(transform.Scale.x), std::abs(transform.Scale.y)));
		const float rW = radius * s;
		const float halfL = (length * 0.5f) * s;
		const glm::vec2 sCenter = WorldToScreen(camera, centerWorld, viewport);

		if (sCenter.x < -1e8f)
			return false;

		glm::vec2 sCapA {}, sCapB {}, sSideA {}, sSideB {};
		if (direction == 0) // 垂直 (Vertical: 上下半球，左右侧边)
		{
			sCapA = WorldToScreen(camera, centerWorld + glm::vec3(0.0f,  halfL + rW, 0.0f), viewport);
			sCapB = WorldToScreen(camera, centerWorld + glm::vec3(0.0f, -halfL - rW, 0.0f), viewport);
			sSideA = WorldToScreen(camera, centerWorld + glm::vec3( rW, 0.0f, 0.0f), viewport);
			sSideB = WorldToScreen(camera, centerWorld + glm::vec3(-rW, 0.0f, 0.0f), viewport);
		}
		else // 水平 (Horizontal: 左右半球，上下侧边)
		{
			sCapA = WorldToScreen(camera, centerWorld + glm::vec3( halfL + rW, 0.0f, 0.0f), viewport);
			sCapB = WorldToScreen(camera, centerWorld + glm::vec3(-halfL - rW, 0.0f, 0.0f), viewport);
			sSideA = WorldToScreen(camera, centerWorld + glm::vec3(0.0f,  rW, 0.0f), viewport);
			sSideB = WorldToScreen(camera, centerWorld + glm::vec3(0.0f, -rW, 0.0f), viewport);
		}

		const glm::vec2 handles[5] = { sCapA, sCapB, sSideA, sSideB, sCenter };
		const glm::vec2 mouse = ctx.Input().MousePos;
		int hovered = -1;
		if (ctx.IsHovered(viewport))
		{
			float bestDist = kPickTolerance + 2.0f;
			for (int i = 0; i < 5; ++i)
			{
				const float d = glm::length(mouse - handles[i]);
				if (d <= bestDist)
				{
					bestDist = d;
					hovered = i;
				}
			}
		}

		const WuiColor colGreen { 0.20f, 0.95f, 0.42f, 0.95f };
		// 绘制胶囊体两端端点轴线与侧边线
		if (direction == 0)
		{
			const glm::vec2 sTopL = WorldToScreen(camera, centerWorld + glm::vec3(-rW,  halfL, 0.0f), viewport);
			const glm::vec2 sTopR = WorldToScreen(camera, centerWorld + glm::vec3( rW,  halfL, 0.0f), viewport);
			const glm::vec2 sBotL = WorldToScreen(camera, centerWorld + glm::vec3(-rW, -halfL, 0.0f), viewport);
			const glm::vec2 sBotR = WorldToScreen(camera, centerWorld + glm::vec3( rW, -halfL, 0.0f), viewport);
			PushLine(ctx, sTopL, sBotL, colGreen, 2.0f);
			PushLine(ctx, sTopR, sBotR, colGreen, 2.0f);
			PushLine(ctx, sTopL, sCapA, colGreen, 1.5f);
			PushLine(ctx, sTopR, sCapA, colGreen, 1.5f);
			PushLine(ctx, sBotL, sCapB, colGreen, 1.5f);
			PushLine(ctx, sBotR, sCapB, colGreen, 1.5f);
		}
		else
		{
			const glm::vec2 sLeftT  = WorldToScreen(camera, centerWorld + glm::vec3(-halfL,  rW, 0.0f), viewport);
			const glm::vec2 sLeftB  = WorldToScreen(camera, centerWorld + glm::vec3(-halfL, -rW, 0.0f), viewport);
			const glm::vec2 sRightT = WorldToScreen(camera, centerWorld + glm::vec3( halfL,  rW, 0.0f), viewport);
			const glm::vec2 sRightB = WorldToScreen(camera, centerWorld + glm::vec3( halfL, -rW, 0.0f), viewport);
			PushLine(ctx, sLeftT, sRightT, colGreen, 2.0f);
			PushLine(ctx, sLeftB, sRightB, colGreen, 2.0f);
			PushLine(ctx, sLeftT, sCapB, colGreen, 1.5f);
			PushLine(ctx, sLeftB, sCapB, colGreen, 1.5f);
			PushLine(ctx, sRightT, sCapA, colGreen, 1.5f);
			PushLine(ctx, sRightB, sCapA, colGreen, 1.5f);
		}

		for (int i = 0; i < 4; ++i)
			PushBox(ctx, handles[i], (hovered == i) ? kHighlight : colGreen, 7.0f);
		PushBox(ctx, sCenter, (hovered == 4) ? kHighlight : WuiColor{ 0.95f, 0.85f, 0.20f, 1.0f }, 6.0f);

		if (!allowManipulation)
			return false;

		static bool s_CapDragging = false;
		static int s_CapActiveHandle = -1;
		static glm::vec2 s_CapStartMouse {};
		static float s_CapStartLength = 1.0f;
		static float s_CapStartRadius = 0.5f;
		static glm::vec2 s_CapStartOffset {};

		if (!s_CapDragging && ctx.Input().MouseClicked[0] && ctx.IsHovered(viewport) && hovered >= 0)
		{
			s_CapDragging = true;
			s_CapActiveHandle = hovered;
			s_CapStartMouse = mouse;
			s_CapStartLength = length;
			s_CapStartRadius = radius;
			s_CapStartOffset = offset;
		}

		if (s_CapDragging && ctx.Input().MouseDown[0])
		{
			const glm::vec2 mouseDelta = mouse - s_CapStartMouse;
			const float wpp = WorldPerPixel(camera, viewport);
			const glm::vec2 worldDelta = mouseDelta * wpp;

			if (s_CapActiveHandle == 4) // 中心偏移
			{
				offset.x = s_CapStartOffset.x + worldDelta.x / s;
				offset.y = s_CapStartOffset.y - worldDelta.y / s;
			}
			else if (s_CapActiveHandle == 0 || s_CapActiveHandle == 1) // 长度控制
			{
				if (direction == 0)
				{
					const float deltaL = (s_CapActiveHandle == 0) ? -worldDelta.y * 2.0f : worldDelta.y * 2.0f;
					length = std::max(0.05f, s_CapStartLength + deltaL / s);
				}
				else
				{
					const float deltaL = (s_CapActiveHandle == 0) ? worldDelta.x * 2.0f : -worldDelta.x * 2.0f;
					length = std::max(0.05f, s_CapStartLength + deltaL / s);
				}
			}
			else if (s_CapActiveHandle == 2 || s_CapActiveHandle == 3) // 半径控制
			{
				if (direction == 0)
				{
					const float deltaR = (s_CapActiveHandle == 2) ? worldDelta.x : -worldDelta.x;
					radius = std::max(0.05f, s_CapStartRadius + deltaR / s);
				}
				else
				{
					const float deltaR = (s_CapActiveHandle == 2) ? -worldDelta.y : worldDelta.y;
					radius = std::max(0.05f, s_CapStartRadius + deltaR / s);
				}
			}
		}

		if (s_CapDragging && ctx.Input().MouseReleased[0])
		{
			s_CapDragging = false;
			s_CapActiveHandle = -1;
		}

		return s_CapDragging || (hovered >= 0 && ctx.IsHovered(viewport));
	}
}
