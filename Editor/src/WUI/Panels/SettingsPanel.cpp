#include "wldpch.h"
#include "SettingsPanel.h"

#include "World/Renderer/RenderSettings.h"
#include "World/Renderer/Renderer.h"
#include "World/Renderer/Renderer3D.h"
#include "World/Core/PhysicsSettings.h"
#include "World/WUI/WuiLocalization.h"
#include "World/WUI/WuiAccessibility.h"
#include "World/WUI/WuiWidget.h"

namespace World
{
	namespace
	{
		const uint32_t kShadowMapSizes[] = { 512u, 1024u, 2048u, 4096u };

		int ShadowMapSizeIndex(uint32_t size)
		{
			for (int index = 0; index < 4; ++index)
				if (kShadowMapSizes[index] == size)
					return index;
			return 2;   // 2048 默认
		}
	}

	void SettingsPanel::OnRender(Wui::WuiContext& ctx, const Wui::WuiRect& rect, PanelHost& host)
	{
		if (!m_Initialized)
		{
			m_Edit = RenderSettings::Get();
			m_Physics = PhysicsSettings::Get();
			m_Initialized = true;
		}

		const Wui::WuiTheme& theme = host.Theme();
		const float x = rect.X + 10.0f;
		const float width = rect.W - 20.0f;
		float y = rect.Y + 10.0f;
		bool changed = false;

		// P4-UX1:本地化试点 —— 源码内联英文是默认语言,zh-CN.json 提供中文覆盖;
		// 中文界面下用 LabelWithTerm/Checkbox(term) 带上英文术语对照(不堆括号、窄处自动省略)。
		{
			const Wui::LocalizedLabel header = Wui::TrLabel("settings.group.rendering", "Rendering");
			Wui::LabelWithTerm(ctx, { x, y }, header.Text, header.Term, theme.TextMuted, 12.0f, theme);
		}
		y += 20.0f;

		{
			const Wui::LocalizedLabel label = Wui::TrLabel("settings.culling", "Frustum Culling");
			if (Wui::Checkbox(ctx, Wui::HashId("settings3d.culling"), { x, y, width, 18.0f },
				label.Text, label.Term, m_Edit.Culling, theme))
				changed = true;
		}
		y += 24.0f;

		{
			const Wui::LocalizedLabel label = Wui::TrLabel("settings.shadows", "Directional Shadows");
			if (Wui::Checkbox(ctx, Wui::HashId("settings3d.shadows"), { x, y, width, 18.0f },
				label.Text, label.Term, m_Edit.Shadows, theme))
				changed = true;
		}
		y += 26.0f;

		// P4-perf:垂直同步/呈现模式。Vulkan 下改这一项会重建交换链(下一帧生效),
		// GL 下立刻改 swap interval;两者都不需要重启,方便直接对比帧率。
		{
			const Wui::LocalizedLabel label = Wui::TrLabel("settings.vsync", "Vertical Sync");
			if (Wui::Checkbox(ctx, Wui::HashId("settings3d.vsync"), { x, y, width, 18.0f },
				label.Text, label.Term, m_Edit.Vsync, theme))
			{
				changed = true;
				Renderer::SetVsync(m_Edit.Vsync);
			}
		}
		y += 26.0f;

		{
			std::vector<std::string> options { "512", "1024", "2048", "4096" };
			int selected = ShadowMapSizeIndex(m_Edit.ShadowMapSize);
			if (Wui::Combo(ctx, Wui::HashId("settings3d.shadow_map"), { x + 170.0f, y, 110.0f, 20.0f },
				"阴影贴图", options, selected, theme))
			{
				m_Edit.ShadowMapSize = kShadowMapSizes[selected];
				changed = true;
			}
			{
				const Wui::LocalizedLabel label = Wui::TrLabel("settings.shadow_map", "Shadow Map Size");
				Wui::LabelWithTerm(ctx, { x, y + 3.0f }, label.Text, label.Term, theme.Text, 13.0f, theme);
			}
			y += 26.0f;
		}

		{
			int64_t value = static_cast<int64_t>(m_Edit.MaxDirectionalLights);
			if (Wui::DragInt(ctx, Wui::HashId("settings3d.max_directional"), { x + 170.0f, y, 110.0f, 20.0f },
				value, 1, static_cast<int64_t>(Renderer3D::MaxDirectionalLightCapacity), theme))
			{
				m_Edit.MaxDirectionalLights = static_cast<uint32_t>(value);
				changed = true;
			}
			Wui::Label(ctx, { x, y + 3.0f }, "方向光上限", theme.Text, 13.0f);
			y += 26.0f;
		}

		{
			int64_t value = static_cast<int64_t>(m_Edit.MaxPointLights);
			if (Wui::DragInt(ctx, Wui::HashId("settings3d.max_point"), { x + 170.0f, y, 110.0f, 20.0f },
				value, 0, static_cast<int64_t>(Renderer3D::MaxPointLightCapacity), theme))
			{
				m_Edit.MaxPointLights = static_cast<uint32_t>(value);
				changed = true;
			}
			Wui::Label(ctx, { x, y + 3.0f }, "点光上限", theme.Text, 13.0f);
			y += 26.0f;
		}

		// 上限之和不能超过 UBO 容量(8)—— 清单校验会拒绝,面板这里先夹住。
		if (m_Edit.MaxDirectionalLights + m_Edit.MaxPointLights > Renderer3D::MaxLights)
		{
			m_Edit.MaxPointLights = Renderer3D::MaxLights - m_Edit.MaxDirectionalLights;
			changed = true;
		}

		// P4-1:纹理各向异性(1..16;设备不支持时引擎退化 1 + warn)。
		{
			int64_t value = static_cast<int64_t>(m_Edit.Anisotropy);
			if (Wui::DragInt(ctx, Wui::HashId("settings3d.anisotropy"), { x + 170.0f, y, 110.0f, 20.0f },
				value, 1, 16, theme))
			{
				m_Edit.Anisotropy = static_cast<uint32_t>(value);
				changed = true;
			}
			Wui::Label(ctx, { x, y + 3.0f }, "纹理各向异性", theme.Text, 13.0f);
			y += 26.0f;
		}

		// P4-3:渲染分辨率倍率(0.25..2.0;只缩放场景渲染目标,不动窗口/UI)。
		{
			float value = m_Edit.RenderScale;
			if (Wui::DragFloat(ctx, Wui::HashId("settings3d.render_scale"), { x + 170.0f, y, 110.0f, 20.0f },
				value, 0.05f, 0.25f, 2.0f, theme))
			{
				m_Edit.RenderScale = value;
				changed = true;
			}
			Wui::Label(ctx, { x, y + 3.0f }, "渲染分辨率倍率", theme.Text, 13.0f);
			y += 26.0f;
		}

		// P4-4:MSAA(1/2/4/8;**启动期生效** —— 渲染通道的采样数在渲染器 Init 时确定,
		// 预览面板也必须与场景通道同采样数,故不支持运行期切换)。
		{
			const std::vector<std::string> msaaOptions { "1", "2", "4", "8" };
			int selected = 0;
			if (m_Edit.Msaa >= 8u) selected = 3;
			else if (m_Edit.Msaa >= 4u) selected = 2;
			else if (m_Edit.Msaa >= 2u) selected = 1;
			if (Wui::Combo(ctx, Wui::HashId("settings3d.msaa"), { x + 170.0f, y, 110.0f, 20.0f },
				"MSAA", msaaOptions, selected, theme))
			{
				m_Edit.Msaa = static_cast<uint32_t>(std::stoul(msaaOptions[static_cast<size_t>(selected)]));
				changed = true;
			}
			Wui::Label(ctx, { x, y + 3.0f }, "MSAA(重启生效)", theme.Text, 13.0f);
			y += 26.0f;
		}

		// P4-1:物理(固定步长 1..240Hz;重力 = Y 轴加速度)。
		{
			const Wui::LocalizedLabel header = Wui::TrLabel("settings.group.physics", "Physics");
			Wui::LabelWithTerm(ctx, { x, y }, header.Text, header.Term, theme.TextMuted, 12.0f, theme);
		}
		y += 20.0f;
		{
			int64_t value = static_cast<int64_t>(m_Physics.FixedStepHz);
			if (Wui::DragInt(ctx, Wui::HashId("settings.physics.fixed_step"), { x + 170.0f, y, 110.0f, 20.0f },
				value, 1, 240, theme))
			{
				m_Physics.FixedStepHz = static_cast<uint32_t>(value);
				changed = true;
			}
			{
				const Wui::LocalizedLabel label = Wui::TrLabel("settings.physics.fixed_step", "Fixed Timestep");
				Wui::LabelWithTerm(ctx, { x, y + 3.0f }, label.Text, label.Term, theme.Text, 13.0f, theme);
			}
			y += 26.0f;
		}
		{
			float value = m_Physics.Gravity;
			if (Wui::DragFloat(ctx, Wui::HashId("settings.physics.gravity"), { x + 170.0f, y, 110.0f, 20.0f },
				value, 0.1f, -50.0f, 50.0f, theme))
			{
				m_Physics.Gravity = value;
				changed = true;
			}
			{
				const Wui::LocalizedLabel label = Wui::TrLabel("settings.physics.gravity", "Gravity");
				Wui::LabelWithTerm(ctx, { x, y + 3.0f }, label.Text, label.Term, theme.Text, 13.0f, theme);
			}
			y += 26.0f;
		}

		if (changed)
		{
			RenderSettings::Set(m_Edit);
			PhysicsSettings::Set(m_Physics);
			m_Status = Wui::Tr("settings.status.applied", "Applied (not saved to project.we.yaml)");
			m_StatusIsError = false;
		}

		y += 4.0f;
		const float buttonWidth = (width - 12.0f) / 2.0f;
		if (Wui::Button(ctx, Wui::HashId("settings3d.save"), { x, y, buttonWidth, 24.0f },
			Wui::Tr("settings.save", "Save to project.we.yaml"), theme))
		{
			std::string message;
			if (host.SaveProjectRenderSettings(m_Edit, &message) && host.SaveProjectPhysicsSettings(m_Physics, &message))
			{
				m_Status = message.empty()
					? Wui::Tr("settings.status.saved", "Saved to project.we.yaml") : message;
				m_StatusIsError = false;
			}
			else
			{
				m_Status = message.empty() ? Wui::Tr("settings.status.save_failed", "Save failed") : message;
				m_StatusIsError = true;
			}
		}
		if (Wui::Button(ctx, Wui::HashId("settings3d.reset"), { x + buttonWidth + 12.0f, y, buttonWidth, 24.0f },
			Wui::Tr("settings.reset", "Restore Defaults"), theme))
		{
			m_Edit = Asset::RenderingSettings {};
			RenderSettings::Set(m_Edit);
			Renderer::SetVsync(m_Edit.Vsync);
			m_Status = Wui::Tr("settings.status.defaults", "Restored built-in defaults (not saved)");
			m_StatusIsError = false;
		}
		y += 32.0f;

		// 生效态说明:哪些立即生效、哪些要重启 —— 用户不该靠猜。
		{
			char buffer[256] = {};
			std::snprintf(buffer, sizeof(buffer),
				"生效中:剔除=%s 阴影=%s vsync=%s 贴图=%u 光=%u+%u",
				RenderSettings::CullingEnabled() ? "on" : "off",
				RenderSettings::ShadowsEnabled() ? "on" : "off",
				Renderer::IsVsyncEnabled() ? "on" : "off",
				Renderer3D::GetShadowMapSize(),
				Renderer3D::GetMaxDirectionalLights(), Renderer3D::GetMaxPointLights());
			Wui::Label(ctx, { x, y }, buffer, theme.TextMuted, 12.0f);
			y += 18.0f;
			char physicsText[128] = {};
			std::snprintf(physicsText, sizeof(physicsText), "生效中物理:步长=%u Hz 重力=%.2f 各向异性=%u",
				PhysicsSettings::Get().FixedStepHz, PhysicsSettings::Get().Gravity,
				RenderSettings::Get().Anisotropy);
			Wui::Label(ctx, { x, y }, physicsText, theme.TextMuted, 12.0f);
			y += 18.0f;
		}
		Wui::Label(ctx, { x, y }, "提示:阴影贴图尺寸在下次启动/重载渲染器后生效;其它项立即生效。",
			theme.TextMuted, 12.0f);
		y += 20.0f;

		Wui::WuiAccessNode statusNode;
		statusNode.Id = Wui::HashId("settings.status");
		statusNode.Window = Wui::WuiAccessibility::Get().CurrentWindow();
		statusNode.Panel = Wui::WuiAccessibility::Get().CurrentPanel();
		statusNode.Kind = "status";
		statusNode.Label = "render settings status";
		statusNode.Value = m_Status;
		statusNode.Rect = { x, y, width, 16.0f };
		statusNode.Interactive = false;
		Wui::WuiAccessibility::Get().Register(statusNode);
		Wui::Label(ctx, { x, y }, m_Status,
			m_StatusIsError ? Wui::WuiColor { 1.0f, 0.45f, 0.40f, 1.0f } : theme.TextMuted, 12.0f);
	}
}
