#include "wldpch.h"
#include "WUI/Panels/SoundEventEditorPanel.h"
#include "World/Audio/AudioSystem.h"
#include "World/WUI/WuiWidgets.h"
#include "World/WUI/Widgets/WuiChrome.h"

#include <fstream>
#include <sstream>

namespace World
{
	std::string SoundEventEditorPanel::s_PendingOpenPath;

	void SoundEventEditorPanel::RequestOpenEvent(const std::string& path)
	{
		s_PendingOpenPath = path;
	}

	void SoundEventEditorPanel::LoadEvent(const std::string& path)
	{
		m_LoadedPath = path;
		std::ifstream in(path);
		if (in.is_open())
		{
			std::stringstream ss;
			ss << in.rdbuf();
			m_Event.DeserializeYaml(ss.str());
		}
		else
		{
			m_Event = Audio::SoundEvent("New_SoundEvent", Audio::kBusSfx);
		}
		m_SelectedBusIndex = static_cast<int>(m_Event.GetBusId());
		m_Is3D = m_Event.GetSpatial().is3D;
		m_MinDist = m_Event.GetSpatial().minDistance;
		m_MaxDist = m_Event.GetSpatial().maxDistance;
		m_StatusFeedback = "已载入声音事件: " + path;
	}

	void SoundEventEditorPanel::SaveEvent()
	{
		if (m_LoadedPath.empty())
		{
			m_StatusFeedback = "未指定保存路径。";
			return;
		}

		m_Event.SetBusId(static_cast<Audio::BusId>(m_SelectedBusIndex));
		m_Event.GetSpatial().is3D = m_Is3D;
		m_Event.GetSpatial().minDistance = m_MinDist;
		m_Event.GetSpatial().maxDistance = m_MaxDist;
		m_Event.SetVolumeRange(m_MinVolume, m_MaxVolume);
		m_Event.SetPitchRangeSemitones(m_MinPitchSemi, m_MaxPitchSemi);

		std::ofstream out(m_LoadedPath);
		if (out.is_open())
		{
			out << m_Event.SerializeYaml();
			m_StatusFeedback = "已成功保存修改到资产文件: " + m_LoadedPath;
		}
		else
		{
			m_StatusFeedback = "保存失败: 无法写入文件 " + m_LoadedPath;
		}
	}

	void SoundEventEditorPanel::OnRender(Wui::WuiContext& ctx, const Wui::WuiRect& rect, PanelHost& host)
	{
		const Wui::WuiTheme& theme = host.Theme();
		auto& audio = Audio::AudioSystem::Get();

		// Handle pending file open request
		if (!s_PendingOpenPath.empty())
		{
			std::string p = s_PendingOpenPath;
			s_PendingOpenPath.clear();
			LoadEvent(p);
		}

		// Background dark surface
		Wui::PanelBackground(ctx, rect, { 0.12f, 0.13f, 0.15f, 1.0f });

		// --- 1. Top Header Bar ---
		const Wui::WuiRect topBar { rect.X, rect.Y, rect.W, 42.0f };
		Wui::BarSurface(ctx, topBar, { 0.15f, 0.16f, 0.19f, 1.0f }, { 0.22f, 0.24f, 0.28f, 1.0f });

		Wui::Label(ctx, { rect.X + 14.0f, rect.Y + 12.0f }, "SOUND EVENT ASSET EDITOR", { 0.95f, 0.55f, 0.15f, 1.0f }, 14.0f);
		std::string pathLabel = m_LoadedPath.empty() ? "(未打开事件)" : ("资产: " + m_LoadedPath);
		Wui::Label(ctx, { rect.X + 260.0f, rect.Y + 14.0f }, pathLabel, theme.TextMuted, 11.0f);

		float y = rect.Y + 50.0f;

		// --- 2. Event Configuration & Randomization Controls ---
		const Wui::WuiRect configBox { rect.X + 14.0f, y, rect.W - 28.0f, 130.0f };
		Wui::BarSurface(ctx, configBox, { 0.14f, 0.15f, 0.17f, 1.0f }, { 0.20f, 0.22f, 0.26f, 1.0f });

		float cY = configBox.Y + 10.0f;
		Wui::Label(ctx, { configBox.X + 12.0f, cY }, "【事件属性与发声规则配置】", { 0.35f, 0.75f, 0.95f, 1.0f }, 12.0f);
		cY += 24.0f;

		// Bus routing pills
		Wui::Label(ctx, { configBox.X + 12.0f, cY + 2.0f }, "目标混音总线:", theme.TextMuted, 11.5f);
		const char* busNames[] = { "Master", "Music", "SFX", "Voice", "UI", "Ambience" };
		float bX = configBox.X + 110.0f;
		for (size_t i = 0; i < 6; ++i)
		{
			bool active = (m_SelectedBusIndex == static_cast<int>(i));
			Wui::WuiRect bBtn { bX, cY, 68.0f, 20.0f };
			if (Wui::Button(ctx, Wui::HashId(("se.bus." + std::to_string(i)).c_str()), bBtn, active ? ("* " + std::string(busNames[i])) : busNames[i], theme))
			{
				m_SelectedBusIndex = static_cast<int>(i);
			}
			bX += 72.0f;
		}
		cY += 28.0f;

		// Random pitch & volume sliders
		Wui::Label(ctx, { configBox.X + 12.0f, cY + 2.0f }, "音高随机微调范围 (半音):", theme.TextMuted, 11.5f);
		Wui::WuiRect rectPitchMin { configBox.X + 170.0f, cY, 80.0f, 18.0f };
		Wui::WuiRect rectPitchMax { configBox.X + 260.0f, cY, 80.0f, 18.0f };
		Wui::SliderFloat(ctx, Wui::HashId("se.pitch.min"), rectPitchMin, m_MinPitchSemi, -6.0f, 0.0f, theme);
		Wui::SliderFloat(ctx, Wui::HashId("se.pitch.max"), rectPitchMax, m_MaxPitchSemi, 0.0f, 6.0f, theme);

		Wui::Label(ctx, { configBox.X + 360.0f, cY + 2.0f }, "音量随机范围:", theme.TextMuted, 11.5f);
		Wui::WuiRect rectVolMin { configBox.X + 460.0f, cY, 80.0f, 18.0f };
		Wui::WuiRect rectVolMax { configBox.X + 550.0f, cY, 80.0f, 18.0f };
		Wui::SliderFloat(ctx, Wui::HashId("se.vol.min"), rectVolMin, m_MinVolume, 0.2f, 1.0f, theme);
		Wui::SliderFloat(ctx, Wui::HashId("se.vol.max"), rectVolMax, m_MaxVolume, 1.0f, 1.5f, theme);
		cY += 28.0f;

		// 3D Spatial distance range
		Wui::Label(ctx, { configBox.X + 12.0f, cY + 2.0f }, "3D 空间衰减半径:", theme.TextMuted, 11.5f);
		Wui::WuiRect rectMinD { configBox.X + 130.0f, cY, 80.0f, 18.0f };
		Wui::WuiRect rectMaxD { configBox.X + 250.0f, cY, 80.0f, 18.0f };
		Wui::Label(ctx, { configBox.X + 220.0f, cY + 2.0f }, "到", theme.TextMuted, 11.5f);
		Wui::SliderFloat(ctx, Wui::HashId("se.dist.min"), rectMinD, m_MinDist, 0.1f, 10.0f, theme);
		Wui::SliderFloat(ctx, Wui::HashId("se.dist.max"), rectMaxD, m_MaxDist, 10.0f, 100.0f, theme);

		y += 140.0f;

		// --- 3. Container Tree & Sound Variations Section ---
		const Wui::WuiRect containerBox { rect.X + 14.0f, y, rect.W - 28.0f, 140.0f };
		Wui::BarSurface(ctx, containerBox, { 0.14f, 0.15f, 0.17f, 1.0f }, { 0.20f, 0.22f, 0.26f, 1.0f });

		float tY = containerBox.Y + 10.0f;
		Wui::Label(ctx, { containerBox.X + 12.0f, tY }, "【容器层级树与变体音频切片】", { 0.35f, 0.75f, 0.95f, 1.0f }, 12.0f);
		tY += 24.0f;

		// Container type pill selector
		const char* cTypes[] = { "Single (单音源)", "Random (随机洗牌防重复)", "Sequence (顺序连招)", "Switch (材质状态切换)" };
		float ctX = containerBox.X + 12.0f;
		for (size_t i = 0; i < 4; ++i)
		{
			bool active = (m_SelectedContainerType == static_cast<int>(i));
			Wui::WuiRect ctBtn { ctX, tY, 150.0f, 22.0f };
			if (Wui::Button(ctx, Wui::HashId(("se.ctype." + std::to_string(i)).c_str()), ctBtn, active ? ("* " + std::string(cTypes[i])) : cTypes[i], theme))
			{
				m_SelectedContainerType = static_cast<int>(i);
			}
			ctX += 156.0f;
		}
		tY += 30.0f;

		// Variations list
		Wui::Label(ctx, { containerBox.X + 12.0f, tY }, "已关联变体切片:", theme.TextMuted, 11.5f);
		tY += 18.0f;

		const Wui::WuiRect itemRow1 { containerBox.X + 12.0f, tY, containerBox.W - 24.0f, 22.0f };
		Wui::BarSurface(ctx, itemRow1, { 0.11f, 0.12f, 0.14f, 1.0f }, { 0.18f, 0.20f, 0.24f, 1.0f });
		Wui::Label(ctx, { itemRow1.X + 8.0f, itemRow1.Y + 4.0f }, "● 变体 1: audio/sfx/footstep_grass_01.waudio (权重 1.0)", theme.Text, 11.0f);
		tY += 26.0f;

		const Wui::WuiRect itemRow2 { containerBox.X + 12.0f, tY, containerBox.W - 24.0f, 22.0f };
		Wui::BarSurface(ctx, itemRow2, { 0.11f, 0.12f, 0.14f, 1.0f }, { 0.18f, 0.20f, 0.24f, 1.0f });
		Wui::Label(ctx, { itemRow2.X + 8.0f, itemRow2.Y + 4.0f }, "● 变体 2: audio/sfx/footstep_grass_02.waudio (权重 1.0)", theme.Text, 11.0f);

		y += 150.0f;

		// --- 4. Audition & Save Action Toolbar ---
		const Wui::WuiRect btnPlay { rect.X + 14.0f, y, 160.0f, 28.0f };
		const Wui::WuiRect btnStop { rect.X + 180.0f, y, 90.0f, 28.0f };
		const Wui::WuiRect btnSave { rect.X + 276.0f, y, 200.0f, 28.0f };

		if (Wui::Button(ctx, Wui::HashId("se.btn.play"), btnPlay, "▶ 实时试听此事件", theme))
		{
			audio.PostEvent("footstep_player", glm::vec3(0.0f));
			m_StatusFeedback = "正在试听事件回放 (已应用随机音调与权重洗牌)...";
		}
		if (Wui::Button(ctx, Wui::HashId("se.btn.stop"), btnStop, "■ 停止", theme))
		{
			audio.StopAll(0.05f);
		}
		if (Wui::Button(ctx, Wui::HashId("se.btn.save"), btnSave, "💾 保存修改到资产 (.wsound)", theme))
		{
			SaveEvent();
		}

		if (!m_StatusFeedback.empty())
		{
			Wui::Label(ctx, { rect.X + 490.0f, y + 6.0f }, m_StatusFeedback, { 0.35f, 0.85f, 0.50f, 1.0f }, 11.5f);
		}
	}
}
