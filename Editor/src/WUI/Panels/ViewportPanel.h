#pragma once

#include "ViewportHost.h"
#include "World/Scene/Components.h"
#include "World/WUI/WuiAccessibility.h"
#include "World/WUI/WuiWidget.h"
#include "World/WUI/WuiWidgets.h"
#include "World/WUI/Widgets/WuiChrome.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <memory>
#include <string>

namespace World
{
	// P4-U22:视口 / 预览面板**共用**的相机手感与坐标系指示器。
	//
	// 为什么放在这个头里:本轮改动只允许落在这几个面板文件里,而"轨道拖拽符号"与
	// "三轴指示器"必须由三个预览面板 + 主视口共用同一份实现 —— 各自复制一份正是
	// "某个面板上下反了 / 另一个面板又是另一个手感"的成因(2026-09-22 用户实测)。
	namespace ViewChrome
	{
		// 离屏预览纹理贴到面板上的 UV 口径。离屏目标的行序由**采样方**统一决定
		// (见 World/Renderer/ProjectionConventions.h):引擎里场景纹理一律 {0,1,1,-1}。
		// 三个预览面板都从这里取,避免"某个面板漏翻一次"—— 漏翻的表现是预览上下镜像
		// (材质预览里就是"主光跑到球的下方",用户报的"上下反/左右反"根因)。
		inline const Wui::WuiRect kPreviewImageUv { 0.0f, 1.0f, 1.0f, -1.0f };

		// 轨道拖拽灵敏度:1 像素 = 0.01 弧度(与历史口径一致)。
		inline constexpr float kOrbitRadiansPerPixel = 0.01f;
		// 俯仰限位 ±89°。
		inline constexpr float kOrbitPitchLimit = 1.55334f;

		// 左键轨道拖拽 —— **符号唯一事实源**。
		// 口径:画面按 kPreviewImageUv **正立**显示时,鼠标向右 / 向下拖 = 物体跟着手走:
		//   yaw   -= dx(水平方向不受上下翻影响,与历史一致,用户已验收);
		//   pitch += dy(正立画面下的跟手方向)。
		// 历史教训:2026-09-21 的 U13g 把 pitch 改成 `-= dy`,那是在**画面上下镜像**的前提下
		// 让手感看起来对 —— 画面本身仍是倒的。U22 把画面翻正,符号必须同时回到这一条,
		// 否则"手感对了、画面是倒的"会一直互相掩盖。
		inline void ApplyOrbitDrag(float& yaw, float& pitch, const glm::vec2& delta,
			float pitchLimit = kOrbitPitchLimit)
		{
			yaw -= delta.x * kOrbitRadiansPerPixel;
			pitch = std::clamp(pitch + delta.y * kOrbitRadiansPerPixel, -pitchLimit, pitchLimit);
		}

		// 轨道相机的世界基,与渲染用的 lookAt 数学同一条:
		// eye = focus + d * (cos p sin y, sin p, cos p cos y),up = +Y。
		inline void OrbitBasis(float yaw, float pitch, glm::vec3* right, glm::vec3* up,
			glm::vec3* forward)
		{
			const glm::vec3 eye { std::cos(pitch) * std::sin(yaw), std::sin(pitch),
				std::cos(pitch) * std::cos(yaw) };
			const glm::vec3 f = -glm::normalize(eye);
			glm::vec3 r = glm::cross(f, glm::vec3 { 0.0f, 1.0f, 0.0f });
			const float length = glm::length(r);
			r = length > 1e-6f ? r / length : glm::vec3 { 1.0f, 0.0f, 0.0f };
			if (forward)
				*forward = f;
			if (right)
				*right = r;
			if (up)
				*up = glm::normalize(glm::cross(r, f));
		}

		// 由朝向反解 yaw/pitch(度)。三轴读数与主视口共用这一条口径。
		inline void BasisYawPitch(const glm::vec3& forward, float* yawDegrees, float* pitchDegrees)
		{
			const float length = glm::length(forward);
			const glm::vec3 f = length > 1e-6f ? forward / length : glm::vec3 { 0.0f, 0.0f, -1.0f };
			if (yawDegrees)
				*yawDegrees = glm::degrees(std::atan2(-f.x, -f.z));
			if (pitchDegrees)
				*pitchDegrees = glm::degrees(std::asin(std::clamp(-f.y, -1.0f, 1.0f)));
		}

		// 细线段 = 一个旋转后的四边形(2D 管线逐顶点颜色,不需要新的绘制类型)。
		inline void PushLine(Wui::WuiContext& ctx, const glm::vec2& from, const glm::vec2& to,
			const Wui::WuiColor& color, float thickness)
		{
			const glm::vec2 d = to - from;
			const float length = glm::length(d);
			if (length < 1e-3f)
				return;
			const glm::vec2 n { -d.y / length * thickness * 0.5f,
				d.x / length * thickness * 0.5f };
			Wui::WuiDrawCommand command;
			command.Kind = Wui::WuiDrawKind::Quad;
			command.Color = color;
			command.Vertices = { from + n, to + n, to - n, from - n };
			ctx.Commands().push_back(std::move(command));
		}

		// 坐标系指示器:右下角 ~48 设计单位的半透明块,X 红 / Y 绿 / Z 蓝,
		// 轴顶点按**该窗口画面**的朝向投影(viewRight/viewUp = 正立画面的屏幕基)。
		// U24 起实现迁到 World/WUI/Widgets/WuiChrome.cpp(AxisGizmo):主视口与三个预览面板
		// 共用同一份"臂长/线宽/标号间隙/正对相机实心点+背离空心环"口径;这里只保留
		// 调用点与命名空间(draw 的几何契约不变:`<panel>.axis` 的 rect/value 仍由调用方登记)。
		inline Wui::WuiRect DrawAxisIndicator(Wui::WuiContext& ctx, const Wui::WuiRect& viewport,
			const glm::vec3& viewRight, const glm::vec3& viewUp, float size = 48.0f)
		{
			return Wui::AxisGizmo(ctx, viewport, viewRight, viewUp, size);
		}

		// 坐标系读数(探针按它断言符号与模式)。
		inline std::string AxisReadout(float yawDegrees, float pitchDegrees, bool twoDimensional)
		{
			char text[128] = {};
			std::snprintf(text, sizeof(text), "yaw=%.2f;pitch=%.2f;mode=%s",
				static_cast<double>(yawDegrees), static_cast<double>(pitchDegrees),
				twoDimensional ? "2d" : "3d");
			return text;
		}

		// <panel>.axis 无障碍节点(只读,non-interactive:坐标指示器不可点)。
		inline Wui::WuiRect RegisterAxisNode(const Wui::WuiRect& rect, const std::string& id,
			const std::string& label, const std::string& value, const std::string& tooltip)
		{
			Wui::WuiAccessNode node;
			node.Id = Wui::HashId(id.c_str());
			node.Window = Wui::WuiAccessibility::Get().CurrentWindow();
			node.Panel = Wui::WuiAccessibility::Get().CurrentPanel();
			node.Kind = "gizmo";
			node.Label = label;
			node.Value = value;
			node.Tooltip = tooltip;
			node.Rect = rect;
			node.Enabled = true;
			node.Interactive = false;
			node.Visible = true;
			Wui::WuiAccessibility::Get().Register(node);
			return rect;
		}
	}

	// 视口面板:场景画面、拾取、Gizmo 与播放控制。Gizmo 拖拽前后快照仅用于标脏。
	class ViewportPanel final : public EditorPanel
	{
	public:
		explicit ViewportPanel(ViewportHost& host) : m_Host(host) {}
		const char* Id() const override { return "view"; }
		const char* Title() const override { return "View"; }
		void OnRender(Wui::WuiContext& ctx, const Wui::WuiRect& rect, PanelHost& host) override;

	private:
		ViewportHost& m_Host;
		std::shared_ptr<Wui::WuiBox> m_Root;
		std::shared_ptr<Wui::WuiImage> m_SceneImage;
		// P4-U8a:悬浮的运行控制药丸(独立小布局树,按绝对矩形摆在画面之上,不占布局)。
		std::shared_ptr<Wui::WuiBox> m_ToolPill;
		std::vector<std::shared_ptr<Wui::WuiImageButton>> m_Tools;
		bool m_GizmoActive = false;
		TransformComponent m_GizmoBefore;
	};
}
