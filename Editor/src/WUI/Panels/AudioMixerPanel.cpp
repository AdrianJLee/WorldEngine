#include "wldpch.h"
#include "WUI/Panels/AudioMixerPanel.h"
#include "World/Audio/AudioSystem.h"
#include "World/WUI/WuiWidgets.h"
#include "World/WUI/Widgets/WuiChrome.h"

namespace World
{
	static Wui::WuiColor BusColor(Audio::BusId id)
	{
		switch (id)
		{
		case Audio::kBusMaster:   return { 0.95f, 0.75f, 0.20f, 1.0f }; // Gold
		case Audio::kBusMusic:    return { 0.45f, 0.55f, 0.95f, 1.0f }; // Blue
		case Audio::kBusSfx:      return { 0.95f, 0.55f, 0.15f, 1.0f }; // Orange
		case Audio::kBusVoice:    return { 0.20f, 0.85f, 0.85f, 1.0f }; // Cyan
		case Audio::kBusUi:       return { 0.90f, 0.35f, 0.70f, 1.0f }; // Magenta
		case Audio::kBusAmbience: return { 0.25f, 0.85f, 0.45f, 1.0f }; // Green
		default:                  return { 0.70f, 0.70f, 0.70f, 1.0f };
		}
	}

	static const char* BusRoleDescription(Audio::BusId id)
	{
		switch (id)
		{
		case Audio::kBusMaster:   return "主输出 (True-Peak 限幅保护)";
		case Audio::kBusMusic:    return "背景配乐 (支持小节节拍对齐)";
		case Audio::kBusSfx:      return "场景音效 (3D 空间衰减/遮挡)";
		case Audio::kBusVoice:    return "对白配音 (触发侧链闪避源)";
		case Audio::kBusUi:       return "界面音效 (不受暂停菜单影响)";
		case Audio::kBusAmbience: return "环境底噪 (立体声背景声景)";
		default:                  return "通用子混音总线";
		}
	}

	void AudioMixerPanel::OnRender(Wui::WuiContext& ctx, const Wui::WuiRect& rect, PanelHost& host)
	{
		const Wui::WuiTheme& theme = host.Theme();
		auto& audio = Audio::AudioSystem::Get();
		auto& mixer = audio.GetMixerGraph();

		// Modern dark brushed graphite console background
		Wui::PanelBackground(ctx, rect, { 0.11f, 0.12f, 0.14f, 1.0f });

		// --- 1. Top Header Bar: Purpose, Flow & Help Toggle (Height 42px) ---
		const Wui::WuiRect topBar { rect.X, rect.Y, rect.W, 42.0f };
		Wui::BarSurface(ctx, topBar, { 0.14f, 0.15f, 0.18f, 1.0f }, { 0.22f, 0.24f, 0.28f, 1.0f });

		Wui::Label(ctx, { rect.X + 14.0f, rect.Y + 8.0f }, "商业化音频混音台 (Audio Bus Mixer & DSP Matrix)", { 0.95f, 0.75f, 0.20f, 1.0f }, 14.5f);
		Wui::Label(ctx, { rect.X + 14.0f, rect.Y + 25.0f }, "信号流向: 声效事件 (.wsound) -> 分类子总线 (Sub-Buses) -> 侧链闪避/DSP链 -> 主输出 (Master) -> 声卡驱动 (DAC)", theme.TextMuted, 10.5f);

		// Help toggle button on the right
		const Wui::WuiRect btnHelp { rect.X + rect.W - 195.0f, rect.Y + 9.0f, 180.0f, 24.0f };
		std::string helpBtnText = m_ShowHelpGuide ? "▲ 收起功能说明与指南" : "▼ 展开混音台功能说明与指南";
		if (Wui::Button(ctx, Wui::HashId("mixer.btn.toggle_help"), btnHelp, helpBtnText, theme))
		{
			m_ShowHelpGuide = !m_ShowHelpGuide;
		}

		float curY = rect.Y + 44.0f;

		// --- 2. Collapsible Workflow & Concepts Guide Card ---
		if (m_ShowHelpGuide)
		{
			const Wui::WuiRect guideBox { rect.X + 12.0f, curY + 6.0f, rect.W - 24.0f, 68.0f };
			Wui::BarSurface(ctx, guideBox, { 0.13f, 0.15f, 0.18f, 1.0f }, { 0.25f, 0.40f, 0.60f, 0.6f });

			Wui::Label(ctx, { guideBox.X + 10.0f, guideBox.Y + 6.0f }, "【音频混音台核心功能与控制逻辑说明】", { 0.35f, 0.75f, 0.95f, 1.0f }, 11.5f);
			Wui::Label(ctx, { guideBox.X + 10.0f, guideBox.Y + 22.0f }, "• [静音 M]: 切断该通道输出；[独奏 S]: 仅监听该通道，排查混音杂音；每个通道内置 5-Band EQ、低通滤波与压限器插槽。", theme.Text, 10.5f);
			Wui::Label(ctx, { guideBox.X + 10.0f, guideBox.Y + 36.0f }, "• [混音快照 Snapshots]: 模拟游戏情境过渡（如暂停菜单下压背景乐并加低通闷响、水下滤波、战斗强化等）。", theme.Text, 10.5f);
			Wui::Label(ctx, { guideBox.X + 10.0f, guideBox.Y + 50.0f }, "• [侧链闪避 Ducking]: 当 Voice 通道发声时，系统自动在 50ms 内压低 Music/Ambience -8dB，对白结束平滑回弹。", { 0.4f, 0.85f, 0.5f, 1.0f }, 10.5f);

			curY += 80.0f;
		}

		// --- 3. Snapshot Simulation & Global Transport Toolbar (Height 68px) ---
		const Wui::WuiRect snapSection { rect.X + 12.0f, curY + 4.0f, rect.W - 24.0f, 66.0f };
		Wui::BarSurface(ctx, snapSection, { 0.13f, 0.14f, 0.16f, 1.0f }, { 0.20f, 0.22f, 0.26f, 1.0f });

		// Row 1: Snapshot simulation
		Wui::Label(ctx, { snapSection.X + 10.0f, snapSection.Y + 6.0f }, "游戏情境混音快照模拟 (Snapshots):", { 0.95f, 0.85f, 0.30f, 1.0f }, 11.5f);

		float snapBtnX = snapSection.X + 225.0f;
		const Wui::WuiRect sDefault { snapBtnX, snapSection.Y + 4.0f, 140.0f, 22.0f };
		const Wui::WuiRect sPause { snapBtnX + 146.0f, snapSection.Y + 4.0f, 160.0f, 22.0f };
		const Wui::WuiRect sDucking { snapBtnX + 312.0f, snapSection.Y + 4.0f, 160.0f, 22.0f };
		const Wui::WuiRect sClear { snapBtnX + 478.0f, snapSection.Y + 4.0f, 120.0f, 22.0f };

		if (Wui::Button(ctx, Wui::HashId("snap.sim.default"), sDefault, (m_ActiveSnapshotIndex == 0 ? "[* 正常游玩态 (0dB)]" : "正常游玩态 (0dB)"), theme))
		{
			m_ActiveSnapshotIndex = 0;
			audio.ClearSnapshot(0.2f);
			m_StatusNotice = "已切换到 [正常游玩态]：全部总线恢复设计基准增益。";
		}
		if (Wui::Button(ctx, Wui::HashId("snap.sim.pause"), sPause, (m_ActiveSnapshotIndex == 1 ? "[* 模拟暂停菜单 (Pause)]" : "模拟暂停菜单 (Pause)"), theme))
		{
			m_ActiveSnapshotIndex = 1;
			audio.TriggerSnapshot("Snapshot_PauseMenu", 0.3f);
			m_StatusNotice = "已触发 [暂停菜单快照]：Music 下压 -10dB + 800Hz 低通闷响，UI 总线保持 100% 清晰。";
		}
		if (Wui::Button(ctx, Wui::HashId("snap.sim.ducking"), sDucking, (m_ActiveSnapshotIndex == 2 ? "[* 触发角色对白闪避]" : "触发角色对白闪避"), theme))
		{
			m_ActiveSnapshotIndex = 2;
			audio.PostEvent2D("ui_click");
			m_StatusNotice = "已模拟 [角色对白闪避]：监测 Voice 通道发声，观察 Music 通道闪避下压。";
		}
		if (Wui::Button(ctx, Wui::HashId("snap.sim.clear"), sClear, "清除快照过渡", theme))
		{
			m_ActiveSnapshotIndex = 0;
			audio.ClearSnapshot(0.3f);
			m_StatusNotice = "已清除快照过渡，平滑恢复基准。";
		}

		// Row 2: Audition Transport buttons
		Wui::Label(ctx, { snapSection.X + 10.0f, snapSection.Y + 36.0f }, "全局声音通道测试播控 (Audition):", theme.TextMuted, 11.5f);

		float audBtnX = snapSection.X + 225.0f;
		const Wui::WuiRect btnAudUi { audBtnX, snapSection.Y + 34.0f, 140.0f, 22.0f };
		const Wui::WuiRect btnAudStep { audBtnX + 146.0f, snapSection.Y + 34.0f, 160.0f, 22.0f };
		const Wui::WuiRect btnAudStop { audBtnX + 312.0f, snapSection.Y + 34.0f, 110.0f, 22.0f };
		const Wui::WuiRect btnAudReset { audBtnX + 428.0f, snapSection.Y + 34.0f, 130.0f, 22.0f };

		if (Wui::Button(ctx, Wui::HashId("aud.test.ui_sfx"), btnAudUi, "▶ 触发 UI 点击声 (2D)", theme))
		{
			audio.PostEvent2D("ui_click");
			m_StatusNotice = "已向 [UI] 总线发送 2D 交互音效事件。";
		}
		if (Wui::Button(ctx, Wui::HashId("aud.test.step_3d"), btnAudStep, "▶ 触发 3D 空间脚步声", theme))
		{
			audio.PostEvent("footstep_player", glm::vec3(0.0f, 0.0f, 0.0f));
			m_StatusNotice = "已向 [SFX] 总线发送 3D 空间定位脚步声事件。";
		}
		if (Wui::Button(ctx, Wui::HashId("aud.test.stop_all"), btnAudStop, "■ 停止全部声音", theme))
		{
			audio.StopAll(0.05f);
			m_StatusNotice = "已发送停止命令，淡出切断所有声道。";
		}
		if (Wui::Button(ctx, Wui::HashId("aud.test.reset_faders"), btnAudReset, " 重置所有推子 0dB", theme))
		{
			for (Audio::BusId bid : mixer.GetBusOrder()) audio.SetBusVolume(bid, 1.0f);
			m_StatusNotice = "已将所有通道条音量推子归零重置 (0.0 dB / 100%)。";
		}

		curY += 76.0f;

		if (!m_StatusNotice.empty())
		{
			Wui::Label(ctx, { rect.X + 16.0f, curY + 2.0f }, "状态指示: " + m_StatusNotice, { 0.35f, 0.85f, 0.50f, 1.0f }, 11.0f);
			curY += 18.0f;
		}

		// --- 4. Vertical Channel Strips Grid ---
		float availableHeight = rect.H - (curY - rect.Y) - 16.0f;
		if (availableHeight < 280.0f) availableHeight = 280.0f;

		const std::vector<Audio::BusId>& busOrder = mixer.GetBusOrder();
		float stripWidth = 118.0f;
		float stripGap = 8.0f;
		float startX = rect.X + 14.0f;

		for (size_t i = 0; i < busOrder.size(); ++i)
		{
			Audio::BusId bid = busOrder[i];
			auto* bus = mixer.FindBus(bid);
			if (!bus) continue;

			float curX = startX + static_cast<float>(i) * (stripWidth + stripGap);
			if (curX + stripWidth > rect.X + rect.W - 12.0f) break;

			const Wui::WuiRect stripRect { curX, curY, stripWidth, availableHeight };
			const Wui::WuiColor busAccent = BusColor(bid);

			// Strip card chassis
			Wui::BarSurface(ctx, stripRect, { 0.15f, 0.16f, 0.19f, 1.0f }, { 0.22f, 0.24f, 0.28f, 1.0f });

			// Accent color top marker
			Wui::PanelBackground(ctx, { curX, curY, stripWidth, 4.0f }, busAccent);

			float innerY = curY + 8.0f;

			// Channel Title and Tag
			Wui::Label(ctx, { curX + 10.0f, innerY }, bus->GetName(), theme.Text, 13.5f);
			char busBadge[16];
			std::snprintf(busBadge, sizeof(busBadge), "B%02u", bid);
			Wui::Label(ctx, { curX + stripWidth - 34.0f, innerY + 1.0f }, busBadge, busAccent, 10.0f);
			innerY += 18.0f;

			// Channel description subtitle
			Wui::Label(ctx, { curX + 10.0f, innerY }, BusRoleDescription(bid), theme.TextMuted, 8.5f);
			innerY += 16.0f;

			// DSP Insert Slots rack
			const Wui::WuiRect dspBox { curX + 6.0f, innerY, stripWidth - 12.0f, 44.0f };
			Wui::BarSurface(ctx, dspBox, { 0.10f, 0.11f, 0.13f, 1.0f }, { 0.18f, 0.20f, 0.24f, 1.0f });
			Wui::Label(ctx, { curX + 10.0f, innerY + 3.0f }, "1: 5-Band EQ (均衡)", { 0.25f, 0.85f, 0.40f, 1.0f }, 9.0f);
			Wui::Label(ctx, { curX + 10.0f, innerY + 16.0f }, "2: LowPass (切高频)", { 0.35f, 0.75f, 0.95f, 1.0f }, 9.0f);
			Wui::Label(ctx, { curX + 10.0f, innerY + 29.0f }, "3: Compressor (压限)", { 0.95f, 0.65f, 0.25f, 1.0f }, 9.0f);
			innerY += 48.0f;

			// Dynamic Sidechain Ducking indicator if applicable
			if (bid == Audio::kBusMusic || bid == Audio::kBusAmbience)
			{
				float duckFac = bus->GetDuckingFactor();
				if (duckFac < 0.95f)
				{
					Wui::WuiRect duckBadge { curX + 6.0f, innerY, stripWidth - 12.0f, 16.0f };
					Wui::BarSurface(ctx, duckBadge, { 0.30f, 0.12f, 0.12f, 1.0f }, { 0.85f, 0.25f, 0.25f, 1.0f });
					char duckStr[32];
					std::snprintf(duckStr, sizeof(duckStr), "Ducking: -%.1fdB", Audio::LinearToDb(duckFac));
					Wui::Label(ctx, { duckBadge.X + 8.0f, duckBadge.Y + 2.0f }, duckStr, { 1.0f, 0.6f, 0.6f, 1.0f }, 9.0f);
				}
				else
				{
					Wui::Label(ctx, { curX + 10.0f, innerY + 2.0f }, "[闪避待命 0dB]", theme.TextMuted, 9.0f);
				}
				innerY += 20.0f;
			}
			else if (bid == Audio::kBusMaster)
			{
				Wui::Label(ctx, { curX + 10.0f, innerY + 2.0f }, "[True-Peak -0.1dB]", { 0.95f, 0.75f, 0.20f, 1.0f }, 9.0f);
				innerY += 20.0f;
			}

			// Mute and Solo Button Row
			const Wui::WuiRect muteRect { curX + 6.0f, innerY, 50.0f, 22.0f };
			const Wui::WuiRect soloRect { curX + 60.0f, innerY, 50.0f, 22.0f };

			std::string muteLabel = bus->IsMute() ? "[静音]" : "静音 M";
			if (Wui::Button(ctx, Wui::HashId(("daw.mute." + bus->GetName()).c_str()), muteRect, muteLabel, theme))
			{
				bus->SetMute(!bus->IsMute());
			}

			std::string soloLabel = bus->IsSolo() ? "[独奏]" : "独奏 S";
			if (Wui::Button(ctx, Wui::HashId(("daw.solo." + bus->GetName()).c_str()), soloRect, soloLabel, theme))
			{
				bus->SetSolo(!bus->IsSolo());
			}
			innerY += 26.0f;

			// Dual Channel Vertical LED VU Meter (L & R)
			float meterW = 10.0f;
			float meterH = availableHeight - (innerY - curY) - 52.0f;
			if (meterH < 80.0f) meterH = 80.0f;

			const Wui::WuiRect meterRectL { curX + 12.0f, innerY, meterW, meterH };
			const Wui::WuiRect meterRectR { curX + 24.0f, innerY, meterW, meterH };

			// Meter groove
			Wui::PanelBackground(ctx, meterRectL, { 0.08f, 0.09f, 0.11f, 1.0f });
			Wui::PanelBackground(ctx, meterRectR, { 0.08f, 0.09f, 0.11f, 1.0f });

			Audio::StereoMeter meter = bus->GetMeter();
			float normL = std::clamp((meter.leftPeakDb + 60.0f) / 60.0f, 0.0f, 1.0f);
			float normR = std::clamp((meter.rightPeakDb + 60.0f) / 60.0f, 0.0f, 1.0f);

			if (normL > 0.02f)
			{
				float fillHL = meterH * normL;
				Wui::WuiColor meterColor = (normL > 0.9f) ? Wui::WuiColor{ 0.95f, 0.25f, 0.25f, 1.0f }
					: (normL > 0.7f ? Wui::WuiColor{ 0.95f, 0.85f, 0.25f, 1.0f } : Wui::WuiColor{ 0.25f, 0.85f, 0.35f, 1.0f });
				Wui::PanelBackground(ctx, { curX + 12.0f, innerY + meterH - fillHL, meterW, fillHL }, meterColor);
			}
			if (normR > 0.02f)
			{
				float fillHR = meterH * normR;
				Wui::WuiColor meterColor = (normR > 0.9f) ? Wui::WuiColor{ 0.95f, 0.25f, 0.25f, 1.0f }
					: (normR > 0.7f ? Wui::WuiColor{ 0.95f, 0.85f, 0.25f, 1.0f } : Wui::WuiColor{ 0.25f, 0.85f, 0.35f, 1.0f });
				Wui::PanelBackground(ctx, { curX + 24.0f, innerY + meterH - fillHR, meterW, fillHR }, meterColor);
			}

			// Fader Scale tick marks
			float faderTrackX = curX + 46.0f;
			Wui::Label(ctx, { faderTrackX, innerY }, "+6", theme.TextMuted, 8.5f);
			Wui::Label(ctx, { faderTrackX, innerY + meterH * 0.25f }, " 0", { 0.95f, 0.85f, 0.25f, 1.0f }, 8.5f);
			Wui::Label(ctx, { faderTrackX, innerY + meterH * 0.5f }, "-12", theme.TextMuted, 8.5f);
			Wui::Label(ctx, { faderTrackX, innerY + meterH * 0.75f }, "-24", theme.TextMuted, 8.5f);
			Wui::Label(ctx, { faderTrackX, innerY + meterH - 8.0f }, "-inf", theme.TextMuted, 8.0f);

			// Fader Slider
			float vol = bus->GetVolume();
			const Wui::WuiRect faderRect { curX + 64.0f, innerY + meterH * 0.3f, 48.0f, 18.0f };
			Wui::SliderFloat(ctx, Wui::HashId(("daw.fader." + bus->GetName()).c_str()), faderRect, vol, 0.0f, 1.5f, theme);
			if (std::abs(vol - bus->GetVolume()) > 0.001f)
			{
				bus->SetVolume(vol);
			}

			// Numeric dB Gain Readout Box
			float bottomY = innerY + meterH + 8.0f;
			const Wui::WuiRect readoutBox { curX + 6.0f, bottomY, stripWidth - 12.0f, 22.0f };
			Wui::BarSurface(ctx, readoutBox, { 0.10f, 0.11f, 0.13f, 1.0f }, { 0.18f, 0.20f, 0.24f, 1.0f });

			char dbStr[32];
			float currentGainDb = (bus->GetVolume() > 0.0001f) ? Audio::LinearToDb(bus->GetVolume()) : -96.0f;
			std::snprintf(dbStr, sizeof(dbStr), "%.1fdB (%.0f%%)", currentGainDb, bus->GetVolume() * 100.0f);
			Wui::Label(ctx, { curX + 12.0f, bottomY + 4.0f }, dbStr, theme.Text, 10.0f);
		}
	}
}
