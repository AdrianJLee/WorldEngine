#include "WUI/Panels/AnimGraphPanel.h"

#include "World/WUI/WuiAccessibility.h"
#include "World/WUI/WuiLocalization.h"
#include "World/WUI/Widgets/WuiChrome.h"

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
		Wui::Label(ctx, { area.X + theme.PadSmall, area.Y + 3.0f },
			PPTr("pose.title", "Pose Preview"), theme.TextMuted, theme.FontSizeCaption);
		Wui::Badge(ctx, { area.X + area.W - 44.0f, area.Y + 2.0f, 38.0f, 16.0f },
			PPTr("pose.front", "正面"), Alpha(theme.Accent, 0.20f), theme.Accent, theme, theme.FontSizeCaption);

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

		const Wui::WuiRect plot { area.X + 8.0f, area.Y + 22.0f,
			std::max(20.0f, area.W - 16.0f), std::max(20.0f, area.H - 28.0f) };
		for (float gx = plot.X; gx <= plot.X + plot.W; gx += 20.0f)
			Wui::LineSegment(ctx, { gx, plot.Y }, { gx, plot.Y + plot.H }, Alpha(theme.Border, 0.34f), 1.0f);
		for (float gy = plot.Y; gy <= plot.Y + plot.H; gy += 20.0f)
			Wui::LineSegment(ctx, { plot.X, gy }, { plot.X + plot.W, gy }, Alpha(theme.Border, 0.34f), 1.0f);

		glm::vec2 minPoint { std::numeric_limits<float>::max(), std::numeric_limits<float>::max() };
		glm::vec2 maxPoint { std::numeric_limits<float>::lowest(), std::numeric_limits<float>::lowest() };
		for (size_t i = 0; i < nodeCount; ++i)
		{
			if (visible[i] == 0)
				continue;
			const glm::vec3 position = glm::vec3(world[i][3]);
			// 骨骼量级是米：|x| > 3m 或 y 越界的节点是异常/辅助节点，不应影响取景，
			// 否则整张图会被一个离群点带跑（实测出现人被裁到画面右上角）。
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
		const float scale = std::min((plot.W - 32.0f) / extent.x, (plot.H - 32.0f) / extent.y) * 0.92f;
		const glm::vec2 modelCenter = (minPoint + maxPoint) * 0.5f;
		const glm::vec2 screenCenter { plot.X + plot.W * 0.5f, plot.Y + plot.H * 0.5f };
		auto project = [&](size_t index) -> glm::vec2
		{
			const glm::vec3 position = glm::vec3(world[index][3]);
			return { screenCenter.x + (position.x - modelCenter.x) * scale,
				screenCenter.y - (position.y - modelCenter.y) * scale };
		};

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
		for (size_t i = 0; i < nodeCount; ++i)
		{
			if (visible[i] == 0)
				continue;
			const glm::vec2 point = project(i);
			const float size = static_cast<int>(i) == rootIndex ? 8.0f : 3.0f;
			const Wui::WuiColor color = static_cast<int>(i) == rootIndex ? theme.Accent : theme.Text;
			Wui::PanelBackground(ctx, { point.x - size * 0.5f, point.y - size * 0.5f, size, size },
				color, size * 0.5f);
		}

		Wui::Tooltip(ctx, area, PPTr("pose.tooltip",
			"正面 XY 投影：线段 = 父子骨骼，圆点 = 关节，蓝色大点 = 根节点。模拟开启时与状态机同帧推进。"));

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
