#include "wldpch.h"
#include "WuiGizmo.h"

#include <glm/gtc/matrix_transform.hpp>

namespace World::Wui
{
	namespace
	{
		glm::vec2 WorldToScreen(const EditorCamera& camera, const glm::vec3& world, const WuiRect& viewport)
		{
			const glm::vec4 clip = camera.GetViewProjection() * glm::vec4(world, 1.0f);
			if (clip.w <= 0.0f)
				return { -1e9f, -1e9f };
			const glm::vec3 ndc = glm::vec3(clip) / clip.w;
			return { viewport.X + (ndc.x + 1.0f) * 0.5f * viewport.W,
				viewport.Y + (1.0f - ndc.y) * 0.5f * viewport.H };
		}
	}

	bool ManipulateGizmo(const EditorCamera& camera, GizmoOperation operation,
		TransformComponent& transform, const WuiRect& viewport, WuiContext& ctx)
	{
		if (operation == GizmoOperation::None)
			return false;

		const glm::vec2 center = WorldToScreen(camera, transform.Location, viewport);
		const float handleRadius = 7.0f;
		const WuiRect handle { center.x - handleRadius, center.y - handleRadius, handleRadius * 2, handleRadius * 2 };

		WuiColor handleColor { 0.95f, 0.6f, 0.2f, 1.0f };
		if (operation == GizmoOperation::Rotate) handleColor = { 0.35f, 0.75f, 0.95f, 1.0f };
		else if (operation == GizmoOperation::Scale) handleColor = { 0.5f, 0.9f, 0.45f, 1.0f };
		ctx.Commands().push_back({ WuiDrawKind::Rect, handle, handleColor, 3.5f });
		ctx.Commands().push_back({ WuiDrawKind::RectOutline, handle, WuiColor { 1, 1, 1, 0.8f }, 3.5f, 1.0f });

		static bool dragging = false;
		static glm::vec2 lastMouse {};
		static glm::vec3 startLocation {};
		static glm::vec3 startScale {};
		static float startRotation = 0;

		if (ctx.Input().MouseClicked[0] && ctx.IsHovered(handle))
		{
			dragging = true;
			lastMouse = ctx.Input().MousePos;
			startLocation = transform.Location;
			startScale = transform.Scale;
			startRotation = transform.Rotation.z;
		}

		if (dragging && ctx.Input().MouseDown[0])
		{
			const glm::vec2 delta = ctx.Input().MousePos - lastMouse;
			lastMouse = ctx.Input().MousePos;
			const float worldPerPixel = 2.0f * camera.GetDistance() * glm::tan(glm::radians(45.0f) * 0.5f)
				/ std::max(1.0f, viewport.H);
			if (operation == GizmoOperation::Translate)
			{
				transform.Location = startLocation
					+ camera.GetRightDirection() * (delta.x * worldPerPixel)
					+ camera.GetUpDirection() * (-delta.y * worldPerPixel);
			}
			else if (operation == GizmoOperation::Rotate)
			{
				transform.Rotation.z = startRotation + delta.x * 0.01f;
			}
			else
			{
				const float amount = 1.0f + (delta.x - delta.y) * 0.005f;
				transform.Scale = { startScale.x * amount, startScale.y * amount, startScale.z * amount };
			}
		}

		if (dragging && ctx.Input().MouseReleased[0])
			dragging = false;

		return dragging || (ctx.Input().MouseDown[0] && ctx.IsHovered(handle));
	}
}
