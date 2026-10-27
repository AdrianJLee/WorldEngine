#include "WUI/Panels/AnimGraphPanel.h"

#include "World/WUI/WuiAccessibility.h"
#include "World/WUI/WuiLocalization.h"
#include "World/WUI/Widgets/WuiChrome.h"
#include "World/Core/KeyCodes.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <string_view>

namespace World
{
	namespace
	{
		std::string PPTr(const char* key, const char* fallback)
		{
			return Wui::Tr(std::string("panel.animgraph.") + key, fallback);
		}

		Wui::WuiColor Alpha(Wui::WuiColor color, float alpha)
		{
			color.A = alpha;
			return color;
		}
	}

	void AnimGraphPanel::RenderPosePreview(Wui::WuiContext& ctx, const Wui::WuiRect& area,
		const Wui::WuiTheme& theme)
	{
		Wui::PanelBackground(ctx, area, theme.ContentBg, theme.Radius);

		// 标头与控制栏
		const float headerH = 26.0f;
		Wui::Label(ctx, { area.X + theme.PadSmall, area.Y + 6.0f },
			PPTr("pose.title", "Pose Preview"), theme.TextMuted, theme.FontSizeCaption);

		// 右侧悬浮控制栏 (Frame / Grid / Bones / Names)
		const float btnW = 44.0f;
		const float btnH = 20.0f;
		const float gap = 4.0f;
		const float toolW = btnW * 4.0f + gap * 5.0f;
		float bx = area.X + area.W - toolW - 4.0f;
		const float by = area.Y + 3.0f;

		// Frame (Center)
		if (Wui::Button(ctx, Wui::HashId("animgraph.pose.center"), { bx, by, btnW, btnH }, "Center", theme))
		{
			m_PosePan = glm::vec2 { 0.0f, 0.0f };
			m_PoseZoom = 1.0f;
		}
		Wui::Tooltip(ctx, { bx, by, btnW, btnH }, "Reset pose preview camera to center.");
		bx += btnW + gap;

		// Grid 开关
		if (m_PoseShowGrid)
			Wui::PanelBackground(ctx, { bx, by, btnW, btnH }, Alpha(theme.Accent, 0.25f), 3.0f);
		if (Wui::Button(ctx, Wui::HashId("animgraph.pose.grid"), { bx, by, btnW, btnH }, "Grid", theme))
			m_PoseShowGrid = !m_PoseShowGrid;
		Wui::Tooltip(ctx, { bx, by, btnW, btnH }, "Toggle reference grid lines.");
		bx += btnW + gap;

		// Bones 开关
		if (m_PoseShowBones)
			Wui::PanelBackground(ctx, { bx, by, btnW, btnH }, Alpha(theme.Accent, 0.25f), 3.0f);
		if (Wui::Button(ctx, Wui::HashId("animgraph.pose.bones"), { bx, by, btnW, btnH }, "Bones", theme))
			m_PoseShowBones = !m_PoseShowBones;
		Wui::Tooltip(ctx, { bx, by, btnW, btnH }, "Toggle bone connection lines.");
		bx += btnW + gap;

		// Names 关节标签开关
		if (m_PoseShowNames)
			Wui::PanelBackground(ctx, { bx, by, btnW, btnH }, Alpha(theme.Accent, 0.25f), 3.0f);
		if (Wui::Button(ctx, Wui::HashId("animgraph.pose.names"), { bx, by, btnW, btnH }, "Names", theme))
			m_PoseShowNames = !m_PoseShowNames;
		Wui::Tooltip(ctx, { bx, by, btnW, btnH }, "Toggle joint name labels.");

		if (!m_ModelValid || m_Model.Nodes.empty())
		{
			Wui::Label(ctx, { area.X + theme.Pad, area.Y + area.H * 0.5f - 8.0f },
				PPTr("pose.select_model", "选择带模型的角色"), theme.TextMuted, theme.FontSizeSmall);
			Wui::Tooltip(ctx, area, PPTr("pose.select_model.tooltip", "选择带 SkinnedMeshRenderer 的角色，或开启模拟查看内部动画图姿态。"));
			return;
		}

		AnimPose pose;
		bool posed = false;
		if (m_Simulate && m_SimGraphReady)
			posed = m_SimGraph.EvaluatePose(m_Model, pose);
		else
			posed = m_Graph.EvaluatePose(m_Model, pose);

		const size_t nodeCount = m_Model.Nodes.size();
		std::vector<uint8_t> visible(nodeCount, 1);
		if (!m_Model.Skins.empty())
		{
			std::fill(visible.begin(), visible.end(), 0);
			for (const Asset::WModelSkin& skin : m_Model.Skins)
				for (uint32_t jointNode : skin.JointNodes)
					if (static_cast<size_t>(jointNode) < nodeCount)
						visible[static_cast<size_t>(jointNode)] = 1;
			if (std::none_of(visible.begin(), visible.end(), [](uint8_t value) { return value != 0; }))
				std::fill(visible.begin(), visible.end(), 1);
		}
		if (!posed || pose.Joints.size() != nodeCount)
		{
			pose.Joints.assign(nodeCount, JointTransform {});
			for (size_t i = 0; i < nodeCount; ++i)
			{
				pose.Joints[i].Translation = m_Model.Nodes[i].Translation;
				pose.Joints[i].Rotation = m_Model.Nodes[i].Rotation;
				pose.Joints[i].Scale = m_Model.Nodes[i].Scale;
			}
		}

		std::vector<glm::mat4> world(nodeCount, glm::mat4(1.0f));
		std::vector<uint8_t> state(nodeCount, 0);
		std::function<glm::mat4(int)> computeWorld = [&](int index) -> glm::mat4
		{
			if (index < 0 || static_cast<size_t>(index) >= nodeCount)
				return glm::mat4(1.0f);
			if (state[static_cast<size_t>(index)] == 2)
				return world[static_cast<size_t>(index)];
			if (state[static_cast<size_t>(index)] == 1)
				return glm::mat4(1.0f);
			state[static_cast<size_t>(index)] = 1;
			const glm::mat4 local = pose.Joints[static_cast<size_t>(index)].ToMatrix();
			const int parent = m_Model.Nodes[static_cast<size_t>(index)].Parent;
			world[static_cast<size_t>(index)] = parent >= 0 ? computeWorld(parent) * local : local;
			state[static_cast<size_t>(index)] = 2;
			return world[static_cast<size_t>(index)];
		};
		for (size_t i = 0; i < nodeCount; ++i)
			computeWorld(static_cast<int>(i));

		const Wui::WuiRect plot { area.X + 8.0f, area.Y + headerH + 2.0f,
			std::max(20.0f, area.W - 16.0f), std::max(20.0f, area.H - headerH - 8.0f) };

		// 画布区域平移与滚轮缩放交互
		const bool plotHovered = ctx.IsHovered(plot);
		if (plotHovered && ctx.Input().Wheel != 0.0f)
		{
			m_PoseZoom = std::clamp(m_PoseZoom + ctx.Input().Wheel * 0.15f * m_PoseZoom, 0.25f, 6.0f);
		}

		const bool dragButton = ctx.Input().MouseDown[1] || ctx.Input().MouseDown[2]
			|| (ctx.Input().MouseDown[0] && ctx.Input().KeyDown[KeyCodes::LeftAlt]);
		if (plotHovered && (ctx.Input().MouseClicked[1] || ctx.Input().MouseClicked[2]
			|| (ctx.Input().MouseClicked[0] && ctx.Input().KeyDown[KeyCodes::LeftAlt])))
		{
			m_PosePanning = true;
			m_PosePanStart = ctx.Input().MousePos;
		}
		if (m_PosePanning && dragButton)
		{
			const glm::vec2 delta = ctx.Input().MousePos - m_PosePanStart;
			m_PosePanStart = ctx.Input().MousePos;
			m_PosePan += delta;
			ctx.SetCursor(Wui::WuiCursor::Hand);
		}
		if (m_PosePanning && !dragButton)
			m_PosePanning = false;

		Wui::ClipScope plotClip(ctx, plot);

		// 背景参考网格
		if (m_PoseShowGrid)
		{
			for (float gx = plot.X; gx <= plot.X + plot.W; gx += 20.0f)
				Wui::LineSegment(ctx, { gx, plot.Y }, { gx, plot.Y + plot.H }, Alpha(theme.Border, 0.34f), 1.0f);
			for (float gy = plot.Y; gy <= plot.Y + plot.H; gy += 20.0f)
				Wui::LineSegment(ctx, { plot.X, gy }, { plot.X + plot.W, gy }, Alpha(theme.Border, 0.34f), 1.0f);
		}

		glm::vec2 minPoint { std::numeric_limits<float>::max(), std::numeric_limits<float>::max() };
		glm::vec2 maxPoint { std::numeric_limits<float>::lowest(), std::numeric_limits<float>::lowest() };
		for (size_t i = 0; i < nodeCount; ++i)
		{
			if (visible[i] == 0)
				continue;
			const glm::vec3 position = glm::vec3(world[i][3]);
			if (!std::isfinite(position.x) || !std::isfinite(position.y)
				|| std::fabs(position.x) > 3.0f || position.y < -0.5f || position.y > 3.0f)
				continue;
			minPoint = { std::min(minPoint.x, position.x), std::min(minPoint.y, position.y) };
			maxPoint = { std::max(maxPoint.x, position.x), std::max(maxPoint.y, position.y) };
		}
		if (minPoint.x > maxPoint.x || minPoint.y > maxPoint.y)
		{
			minPoint = { -0.9f, 0.0f };
			maxPoint = { 0.9f, 1.8f };
		}
		const glm::vec2 extent = glm::max(maxPoint - minPoint, glm::vec2 { 0.001f, 0.001f });
		const float baseScale = std::min((plot.W - 32.0f) / extent.x, (plot.H - 32.0f) / extent.y) * 0.92f;
		const float finalScale = baseScale * m_PoseZoom;
		const glm::vec2 modelCenter = (minPoint + maxPoint) * 0.5f;
		const glm::vec2 screenCenter { plot.X + plot.W * 0.5f + m_PosePan.x, plot.Y + plot.H * 0.5f + m_PosePan.y };

		auto project = [&](size_t index) -> glm::vec2
		{
			const glm::vec3 position = glm::vec3(world[index][3]);
			return { screenCenter.x + (position.x - modelCenter.x) * finalScale,
				screenCenter.y - (position.y - modelCenter.y) * finalScale };
		};

		// 地面参考基线 (Y = 0)
		const float groundScreenY = screenCenter.y - (0.0f - modelCenter.y) * finalScale;
		if (groundScreenY >= plot.Y && groundScreenY <= plot.Y + plot.H)
			Wui::LineSegment(ctx, { plot.X, groundScreenY }, { plot.X + plot.W, groundScreenY },
				Alpha(theme.Success, 0.40f), 1.0f);

		int rootIndex = -1;
		for (size_t i = 0; i < nodeCount; ++i)
		{
			if (visible[i] == 0)
				continue;
			const int parent = m_Model.Nodes[i].Parent;
			if (parent < 0 || static_cast<size_t>(parent) >= nodeCount)
			{
				rootIndex = static_cast<int>(i);
				break;
			}
		}

		// 绘制骨骼连线
		if (m_PoseShowBones)
		{
			for (size_t i = 0; i < nodeCount; ++i)
			{
				if (visible[i] == 0)
					continue;
				const int parent = m_Model.Nodes[i].Parent;
				if (parent < 0 || static_cast<size_t>(parent) >= nodeCount || visible[static_cast<size_t>(parent)] == 0)
					continue;
				Wui::LineSegment(ctx, project(static_cast<size_t>(parent)), project(i),
					Alpha(theme.TextMuted, 0.85f), 2.0f);
			}
		}

		// 绘制关节圆点与标签
		const glm::vec2 mousePos = ctx.Input().MousePos;
		for (size_t i = 0; i < nodeCount; ++i)
		{
			if (visible[i] == 0)
				continue;
			const glm::vec2 point = project(i);
			const bool isRoot = static_cast<int>(i) == rootIndex;
			const float size = isRoot ? 8.0f : 4.0f;
			const Wui::WuiColor color = isRoot ? theme.Accent : theme.Text;

			// 关节圆形
			Wui::PanelBackground(ctx, { point.x - size * 0.5f, point.y - size * 0.5f, size, size },
				color, size * 0.5f);

			// 悬停检测与 Tooltip
			if (plotHovered && glm::length(point - mousePos) <= 7.0f)
			{
				char jtip[192];
				const glm::vec3 pos = glm::vec3(world[i][3]);
				std::snprintf(jtip, sizeof(jtip), "Joint #%zu: %s\nPos: (%.2f, %.2f, %.2f)%s",
					i, m_Model.Nodes[i].Name.c_str(), pos.x, pos.y, pos.z, isRoot ? " [Root]" : "");
				Wui::Tooltip(ctx, { point.x - 6.0f, point.y - 6.0f, 12.0f, 12.0f }, jtip);
			}

			// 关节名称标签
			if (m_PoseShowNames)
			{
				Wui::Label(ctx, { point.x + 6.0f, point.y - 5.0f }, m_Model.Nodes[i].Name,
					theme.TextDisabled, theme.FontSizeCaption);
			}
		}

		Wui::Tooltip(ctx, area, PPTr("pose.tooltip",
			"正面 XY 投影：线段 = 父子骨骼，圆点 = 关节，蓝色大点 = 根节点。右键/Alt+左键拖拽平移，滚轮缩放。"));

		Wui::WuiAccessNode node;
		node.Id = Wui::HashId("animgraph.pose_preview");
		node.Window = Wui::WuiAccessibility::Get().CurrentWindow();
		node.Panel = Wui::WuiAccessibility::Get().CurrentPanel();
		node.Kind = "pose-preview";
		node.Label = PPTr("pose.title", "Pose Preview");
		node.Value = posed ? "posed" : "bind-pose";
		node.Rect = area;
		node.Interactive = false;
		Wui::WuiAccessibility::Get().Register(node);
	}
}
