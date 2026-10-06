#include "wldpch.h"
#include "World/UI/UiWorldProjector.h"

#include <algorithm>
#include <cmath>

namespace World::UI
{
	bool ProjectWorldToScreen(const UiWorldCamera& camera, const glm::vec3& world, glm::vec2& outScreen)
	{
		const glm::vec4 clip = camera.ViewProjection * glm::vec4(world, 1.0f);
		// w <= 0 = 目标在相机平面之后(或矩阵退化):投影没有意义,不能画在 (0,0)。
		if (!(clip.w > 1e-6f))
			return false;
		const glm::vec3 ndc = glm::vec3(clip) / clip.w;
		if (!std::isfinite(ndc.x) || !std::isfinite(ndc.y))
			return false;
		outScreen.x = (ndc.x * 0.5f + 0.5f) * camera.ScreenSize.x;
		// NDC 的 +Y 向上,屏幕 +Y 向下 ⇒ 翻转。
		outScreen.y = (1.0f - (ndc.y * 0.5f + 0.5f)) * camera.ScreenSize.y;
		return true;
	}

	UiWorldResult ApplyWorldAnchors(UiScreen& screen, const UiViewport& viewport,
		const UiWorldCamera& camera, const UiWorldPositionResolver& resolve)
	{
		UiWorldResult result;
		const std::vector<UiNodeInstance>& nodes = screen.Nodes();

		// 目标解析不到 ⇒ 把节点移到内容矩形之外:视觉上不可见,命中也不会中
		// (UI 不允许"看不见但能点到"—— 与 a11y 的 visible 口径一致)。
		const auto hideNode = [&](std::size_t index)
		{
			Wui::WuiRect rect = nodes[index].Rect;
			rect.X = viewport.ContentRect.X + viewport.ContentRect.W + 1.0e5f;
			rect.Y = viewport.ContentRect.Y + viewport.ContentRect.H + 1.0e5f;
			screen.SetNodeRect(index, rect);
		};

		for (std::size_t index = 0; index < nodes.size(); ++index)
		{
			const UiNodeInstance& node = nodes[index];
			if (node.Source == nullptr || !node.Source->World.Enabled)
				continue;

			const UiWorldAnchor& anchor = node.Source->World;
			if (!resolve)
			{
				result.Warnings.push_back("world anchor '" + node.Id + "': no position resolver was provided");
				result.Hidden++;
				hideNode(index);
				continue;
			}

			glm::vec3 worldPosition(0.0f);
			if (!resolve(anchor.Target, worldPosition))
			{
				result.Warnings.push_back("world anchor '" + node.Id + "': target '" + anchor.Target +
					"' could not be resolved this frame (node hidden)");
				result.Hidden++;
				hideNode(index);
				continue;
			}

			glm::vec2 screenPoint(0.0f);
			if (!ProjectWorldToScreen(camera, worldPosition + anchor.Offset, screenPoint))
			{
				result.Warnings.push_back("world anchor '" + node.Id + "': target '" + anchor.Target +
					"' is behind the camera (node hidden)");
				result.Hidden++;
				hideNode(index);
				continue;
			}

			// 物理像素 → 设计单位:与绘制/命中同一映射(`UiViewport::PhysicalToDesign`)。
			glm::vec2 design = viewport.PhysicalToDesign(screenPoint);

			Wui::WuiRect rect = node.Rect;   // 尺寸取自本帧布局结果
			if (anchor.KeepOnScreen)
			{
				const Wui::WuiRect& content = viewport.ContentRect;
				design.x = std::clamp(design.x, content.X, content.X + content.W);
				design.y = std::clamp(design.y, content.Y, content.Y + content.H);
			}

			// Pivot 决定投影点落在节点的哪个位置(默认居中 = 血条/名牌的常见口径)。
			rect.X = design.x - node.Source->Anchor.Pivot.x * rect.W;
			rect.Y = design.y - node.Source->Anchor.Pivot.y * rect.H;
			screen.SetNodeRect(index, rect);
			result.Anchored++;
		}

		return result;
	}
}
