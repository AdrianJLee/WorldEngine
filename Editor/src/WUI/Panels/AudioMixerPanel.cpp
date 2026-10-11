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
		case Audio::kBusMusic:    return { 0.55f, 0.40f, 0.95f, 1.0f }; // Purple
		case Audio::kBusSfx:      return { 0.95f, 0.55f, 0.15f, 1.0f }; // Orange
		case Audio::kBusVoice:    return { 0.20f, 0.85f, 0.85f, 1.0f }; // Cyan
		case Audio::kBusUi:       return { 0.90f, 0.30f, 0.65f, 1.0f }; // Magenta
		case Audio::kBusAmbience: return { 0.25f, 0.85f, 0.45f, 1.0f }; // Green
		default:                  return { 0.70f, 0.70f, 0.70f, 1.0f };
		}
	}

	void AudioMixerPanel::OnRender(Wui::WuiContext& ctx, const Wui::WuiRect& rect, PanelHost& host)
	{
		const Wui::WuiTheme& theme = host.Theme();
		auto& audio = Audio::AudioSystem::Get();
		auto& mixer = audio.GetMixerGraph();

		// Background dark console surface
		Wui::PanelBackground(ctx, rect, { 0.11f, 0.12f, 0.14f, 1.0f });

		// --- 1. Top DAW Transport & Snapshot Header Bar (Height 42px) ---
		const Wui::WuiRect topBar { rect.X, rect.Y, rect.W, 42.0f };
		Wui::BarSurface(ctx, topBar, { 0.14f, 0.15f, 0.18f, 1.0f }, { 0.22f, 0.24f, 0.28f, 1.0f });

		// Console Badge
		Wui::Label(ctx, { rect.X + 14.0f, rect.Y + 12.0f }, "WORLDENGINE DAW STUDIO MIXER", { 0.95f, 0.75f, 0.20f, 1.0f }, 14.0f);
		Wui::Label(ctx, { rect.X + 270.0f, rect.Y + 14.0f }, "48.0 kHz | 32-bit Float | Lock-Free SPSC | 64 Physical Voices", theme.TextMuted, 11.0f);

		// Transport audition buttons
		float transportX = rect.X + rect.W - 560.0f;
		if (transportX < rect.X + 420.0f) transportX = rect.X + 420.0f;

		const Wui::WuiRect btnPlayUi { transportX, rect.Y + 9.0f, 85.0f, 24.0f };
		const Wui::WuiRect btnPlayStep { transportX + 90.0f, rect.Y + 9.0f, 95.0f, 24.0f };
		const Wui::WuiRect btnStopAll { transportX + 190.0f, rect.Y + 9.0f, 75.0f, 24.0f };
		const Wui::WuiRect btnSnapPause { transportX + 270.0f, rect.Y + 9.0f, 110.0f, 24.0f };
		const Wui::WuiRect btnSnapClear { transportX + 385.0f, rect.Y + 9.0f, 75.0f, 24.0f };
		const Wui::WuiRect btnResetAll { transportX + 465.0f, rect.Y + 9.0f, 85.0f, 24.0f };

		if (Wui::Button(ctx, Wui::HashId("daw.btn.play_ui"), btnPlayUi, "▶ UI Click", theme))
		{
			audio.PostEvent2D("ui_click");
		}
		if (Wui::Button(ctx, Wui::HashId("daw.btn.play_step"), btnPlayStep, "▶ 3D Footstep", theme))
		{
			audio.PostEvent("footstep_player", glm::vec3(0.0f, 0.0f, 0.0f));
		}
		if (Wui::Button(ctx, Wui::HashId("daw.btn.stop_all"), btnStopAll, "■ Stop All", theme))
		{
			audio.StopAll(0.05f);
		}
		if (Wui::Button(ctx, Wui::HashId("daw.btn.snap_pause"), btnSnapPause, "• Pause Ducking", theme))
		{
			audio.TriggerSnapshot("Snapshot_PauseMenu", 0.3f);
		}
		if (Wui::Button(ctx, Wui::HashId("daw.btn.snap_clear"), btnSnapClear, "Clear Snap", theme))
		{
			audio.ClearSnapshot(0.3f);
		}
		if (Wui::Button(ctx, Wui::HashId("daw.btn.reset_vol"), btnResetAll, "Reset 0dB", theme))
		{
			for (Audio::BusId bid : mixer.GetBusOrder()) audio.SetBusVolume(bid, 1.0f);
		}

		// --- 2. Vertical Channel Strips Grid ---
		float stripsY = rect.Y + 50.0f;
		float availableHeight = rect.H - 58.0f;
		if (availableHeight < 280.0f) availableHeight = 280.0f;

		const std::vector<Audio::BusId>& busOrder = mixer.GetBusOrder();
		float stripWidth = 110.0f;
		float stripGap = 8.0f;
		float startX = rect.X + 16.0f;

		for (size_t i = 0; i < busOrder.size(); ++i)
		{
			Audio::BusId bid = busOrder[i];
			auto* bus = mixer.FindBus(bid);
			if (!bus) continue;

			float curX = startX + static_cast<float>(i) * (stripWidth + stripGap);
			if (curX + stripWidth > rect.X + rect.W - 12.0f) break;

			const Wui::WuiRect stripRect { curX, stripsY, stripWidth, availableHeight };
			const Wui::WuiColor busAccent = BusColor(bid);

			// Channel strip card background
			Wui::BarSurface(ctx, stripRect, { 0.15f, 0.16f, 0.19f, 1.0f }, { 0.22f, 0.24f, 0.28f, 1.0f });

			// Accent color header indicator bar (3px)
			Wui::PanelBackground(ctx, { curX, stripsY, stripWidth, 4.0f }, busAccent);

			float innerY = stripsY + 8.0f;

			// Channel Title Pill
			Wui::Label(ctx, { curX + 10.0f, innerY }, bus->GetName(), theme.Text, 13.0f);
			char busBadge[16];
			std::snprintf(busBadge, sizeof(busBadge), "B%02u", bid);
			Wui::Label(ctx, { curX + stripWidth - 32.0f, innerY + 1.0f }, busBadge, busAccent, 10.0f);
			innerY += 22.0f;

			// DSP Insert Slots rack box
			const Wui::WuiRect dspBox { curX + 6.0f, innerY, stripWidth - 12.0f, 48.0f };
			Wui::BarSurface(ctx, dspBox, { 0.11f, 0.12f, 0.14f, 1.0f }, { 0.18f, 0.20f, 0.24f, 1.0f });
			Wui::Label(ctx, { curX + 10.0f, innerY + 4.0f }, "1: 5-Band EQ", { 0.2f, 0.8f, 0.4f, 1.0f }, 9.5f);
			Wui::Label(ctx, { curX + 10.0f, innerY + 18.0f }, "2: LowPass Fltr", { 0.3f, 0.7f, 0.9f, 1.0f }, 9.5f);
			Wui::Label(ctx, { curX + 10.0f, innerY + 32.0f }, "3: Compressor", { 0.9f, 0.6f, 0.2f, 1.0f }, 9.5f);
			innerY += 54.0f;

			// Mute / Solo Button Row
			const Wui::WuiRect muteRect { curX + 6.0f, innerY, 46.0f, 22.0f };
			const Wui::WuiRect soloRect { curX + 56.0f, innerY, 46.0f, 22.0f };

			std::string muteLabel = bus->IsMute() ? "[ M ]" : "M";
			if (Wui::Button(ctx, Wui::HashId(("daw.mute." + bus->GetName()).c_str()), muteRect, muteLabel, theme))
			{
				bus->SetMute(!bus->IsMute());
			}

			std::string soloLabel = bus->IsSolo() ? "[ S ]" : "S";
			if (Wui::Button(ctx, Wui::HashId(("daw.solo." + bus->GetName()).c_str()), soloRect, soloLabel, theme))
			{
				bus->SetSolo(!bus->IsSolo());
			}
			innerY += 28.0f;

			// Dual Channel Vertical LED Meter (L & R)
			float meterW = 10.0f;
			float meterH = availableHeight - (innerY - stripsY) - 52.0f;
			if (meterH < 80.0f) meterH = 80.0f;

			const Wui::WuiRect meterRectL { curX + 10.0f, innerY, meterW, meterH };
			const Wui::WuiRect meterRectR { curX + 22.0f, innerY, meterW, meterH };

			// Meter groove backgrounds
			Wui::PanelBackground(ctx, meterRectL, { 0.08f, 0.09f, 0.11f, 1.0f });
			Wui::PanelBackground(ctx, meterRectR, { 0.08f, 0.09f, 0.11f, 1.0f });

			Audio::StereoMeter meter = bus->GetMeter();
			// Map dB [-60dB, 0dB] to fill height [0.0, 1.0]
			float normL = std::clamp((meter.leftPeakDb + 60.0f) / 60.0f, 0.0f, 1.0f);
			float normR = std::clamp((meter.rightPeakDb + 60.0f) / 60.0f, 0.0f, 1.0f);

			if (normL > 0.02f)
			{
				float fillHL = meterH * normL;
				Wui::WuiColor meterColor = (normL > 0.9f) ? Wui::WuiColor{ 0.95f, 0.25f, 0.25f, 1.0f }
					: (normL > 0.7f ? Wui::WuiColor{ 0.95f, 0.85f, 0.25f, 1.0f } : Wui::WuiColor{ 0.25f, 0.85f, 0.35f, 1.0f });
				Wui::PanelBackground(ctx, { curX + 10.0f, innerY + meterH - fillHL, meterW, fillHL }, meterColor);
			}
			if (normR > 0.02f)
			{
				float fillHR = meterH * normR;
				Wui::WuiColor meterColor = (normR > 0.9f) ? Wui::WuiColor{ 0.95f, 0.25f, 0.25f, 1.0f }
					: (normR > 0.7f ? Wui::WuiColor{ 0.95f, 0.85f, 0.25f, 1.0f } : Wui::WuiColor{ 0.25f, 0.85f, 0.35f, 1.0f });
				Wui::PanelBackground(ctx, { curX + 22.0f, innerY + meterH - fillHR, meterW, fillHR }, meterColor);
			}

			// Fader Scale tick marks
			float faderTrackX = curX + 44.0f;
			Wui::Label(ctx, { faderTrackX, innerY }, "+6", theme.TextMuted, 8.5f);
			Wui::Label(ctx, { faderTrackX, innerY + meterH * 0.25f }, " 0", { 0.95f, 0.85f, 0.25f, 1.0f }, 8.5f);
			Wui::Label(ctx, { faderTrackX, innerY + meterH * 0.5f }, "-12", theme.TextMuted, 8.5f);
			Wui::Label(ctx, { faderTrackX, innerY + meterH * 0.75f }, "-24", theme.TextMuted, 8.5f);
			Wui::Label(ctx, { faderTrackX, innerY + meterH - 8.0f }, "-inf", theme.TextMuted, 8.0f);

			// Vertical Volume Fader Slider (mapped horizontally in widget call for standard WUI)
			float vol = bus->GetVolume();
			const Wui::WuiRect faderRect { curX + 60.0f, innerY + meterH * 0.3f, 44.0f, 18.0f };
			Wui::SliderFloat(ctx, Wui::HashId(("daw.fader." + bus->GetName()).c_str()), faderRect, vol, 0.0f, 1.5f, theme);
			if (std::abs(vol - bus->GetVolume()) > 0.001f)
			{
				bus->SetVolume(vol);
			}

			// Numeric dB Gain Readout Box
			float bottomY = innerY + meterH + 8.0f;
			const Wui::WuiRect readoutBox { curX + 6.0f, bottomY, stripWidth - 12.0f, 20.0f };
			Wui::BarSurface(ctx, readoutBox, { 0.10f, 0.11f, 0.13f, 1.0f }, { 0.18f, 0.20f, 0.24f, 1.0f });

			char dbStr[32];
			float currentGainDb = (bus->GetVolume() > 0.0001f) ? Audio::LinearToDb(bus->GetVolume()) : -96.0f;
			std::snprintf(dbStr, sizeof(dbStr), "%.1f dB", currentGainDb);
			Wui::Label(ctx, { curX + 16.0f, bottomY + 3.0f }, dbStr, theme.Text, 11.0f);
		}
	}
}
