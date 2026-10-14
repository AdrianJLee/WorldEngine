#include "wldpch.h"
#include "WUI/Panels/AudioProfilerPanel.h"
#include "World/Audio/AudioSystem.h"
#include "World/Audio/AudioProfiler.h"
#include "World/Audio/AudioTypes.h"
#include "World/Audio/VoicePool.h"
#include "World/Audio/Voice.h"
#include "World/Audio/AudioComponents.h"
#include "World/Scene/Components.h"
#include "World/Scene/Entity.h"
#include "World/Scene/Scene.h"
#include "World/Core/StringPool.h"
#include "World/WUI/WuiWidgets.h"
#include "World/WUI/Widgets/WuiChrome.h"

#include <cmath>
#include <cstdio>
#include <algorithm>
#include <sstream>
#include <iomanip>
#include <filesystem>

namespace World
{
	namespace
	{
		struct VoiceDetailRow
		{
			Audio::VoiceHandle handle { Audio::kInvalidVoiceHandle };
			std::string nameOrEvent;
			glm::vec3 worldPos { 0.0f };
			bool has3DPos { false };
			float distanceToListener { 0.0f };
			float volumeDb { -96.0f };
			float lpfHz { 20000.0f };
			std::string stateStr;
			Wui::WuiColor stateColor { 1.0f, 1.0f, 1.0f, 1.0f };
			Entity entity;
		};
	}

	void AudioProfilerPanel::OnRender(Wui::WuiContext& ctx, const Wui::WuiRect& rect, PanelHost& host)
	{
		const Wui::WuiTheme& theme = host.Theme();
		Wui::PanelBackground(ctx, rect, theme.PanelBg, 0.0f);

		const float pad = 8.0f;
		const float topCardsH = 68.0f;
		const float specH = 132.0f;
		const float spacing = 8.0f;

		const Wui::WuiRect topRect { rect.X + pad, rect.Y + pad, rect.W - pad * 2.0f, topCardsH };
		const Wui::WuiRect specRect { rect.X + pad, topRect.Y + topCardsH + spacing, rect.W - pad * 2.0f, specH };

		const float tableY = specRect.Y + specH + spacing;
		const float tableH = std::max(120.0f, rect.Y + rect.H - tableY - pad);
		const Wui::WuiRect tableRect { rect.X + pad, tableY, rect.W - pad * 2.0f, tableH };

		RenderTopMetricCards(ctx, topRect, host);
		RenderFftSpectrum(ctx, specRect, host);
		RenderActiveVoicesTable(ctx, tableRect, host);
	}

	void AudioProfilerPanel::RenderTopMetricCards(Wui::WuiContext& ctx, const Wui::WuiRect& rect, PanelHost& host)
	{
		const Wui::WuiTheme& theme = host.Theme();
		auto& audio = Audio::AudioSystem::Get();
		const auto profilerSnap = audio.GetProfiler().GetSnapshot();

		const float cardGap = 8.0f;
		const float cardW = std::max(60.0f, (rect.W - cardGap * 3.0f) / 4.0f);
		char buf[128];

		// ---- 卡片 1: CPU 混音耗时 (微秒) ----
		{
			const Wui::WuiRect card { rect.X, rect.Y, cardW, rect.H };
			Wui::PanelBackground(ctx, card, theme.ContentBg, 4.0f);

			Wui::Label(ctx, { card.X + 8.0f, card.Y + 8.0f }, "CPU 混音耗时 (Mix Time)", theme.TextMuted, 11.0f);

			const uint64_t cpuUs = profilerSnap.mixCpuTimeUs;
			const uint64_t maxUs = profilerSnap.maxMixCpuTimeUs;
			std::snprintf(buf, sizeof(buf), "%llu \xc2\xb5s", static_cast<unsigned long long>(cpuUs));

			Wui::WuiColor valColor = (cpuUs < 1000) ? Wui::WuiColor{ 0.35f, 0.85f, 0.55f, 1.0f } :
				((cpuUs < 4500) ? Wui::WuiColor{ 0.95f, 0.80f, 0.25f, 1.0f } : Wui::WuiColor{ 0.95f, 0.35f, 0.35f, 1.0f });
			Wui::Label(ctx, { card.X + 8.0f, card.Y + 26.0f }, buf, valColor, 18.0f);

			const float budgetPct = std::clamp((static_cast<float>(cpuUs) / 5333.3f) * 100.0f, 0.0f, 999.0f);
			std::snprintf(buf, sizeof(buf), "峰值: %llu \xc2\xb5s | 预算: %.1f%%", static_cast<unsigned long long>(maxUs), budgetPct);
			Wui::Label(ctx, { card.X + 8.0f, card.Y + 48.0f }, buf, theme.TextMuted, 10.5f);

			Wui::Tooltip(ctx, card, "音频硬件混音块 CPU 执行耗时 (256 帧 @ 48kHz = 5333 \xc2\xb5s 硬件预算)");
		}

		// ---- 卡片 2: 物理声道分配 (0 / 64) ----
		{
			const Wui::WuiRect card { rect.X + cardW + cardGap, rect.Y, cardW, rect.H };
			Wui::PanelBackground(ctx, card, theme.ContentBg, 4.0f);

			Wui::Label(ctx, { card.X + 8.0f, card.Y + 8.0f }, "物理声道 (Physical Voices)", theme.TextMuted, 11.0f);

			std::snprintf(buf, sizeof(buf), "%u / %u", profilerSnap.activePhysicalVoices, profilerSnap.totalPhysicalVoices);
			Wui::Label(ctx, { card.X + 8.0f, card.Y + 26.0f }, buf, { 0.25f, 0.85f, 0.95f, 1.0f }, 18.0f);

			const Wui::WuiRect barBg { card.X + 8.0f, card.Y + 50.0f, card.W - 16.0f, 8.0f };
			Wui::PanelBackground(ctx, barBg, theme.PanelBg, 2.0f);
			const float ratio = std::clamp(static_cast<float>(profilerSnap.activePhysicalVoices) /
				static_cast<float>(std::max(1u, profilerSnap.totalPhysicalVoices)), 0.0f, 1.0f);
			if (ratio > 0.005f)
			{
				Wui::PanelBackground(ctx, { barBg.X, barBg.Y, barBg.W * ratio, barBg.H }, { 0.25f, 0.85f, 0.95f, 0.95f }, 2.0f);
			}

			Wui::Tooltip(ctx, card, "当前执行 3D 空间化与 DSP 滤波运算的物理声部 (上限 64 声道)");
		}

		// ---- 卡片 3: 虚拟等待声道 (0 / 1024) ----
		{
			const Wui::WuiRect card { rect.X + (cardW + cardGap) * 2.0f, rect.Y, cardW, rect.H };
			Wui::PanelBackground(ctx, card, theme.ContentBg, 4.0f);

			Wui::Label(ctx, { card.X + 8.0f, card.Y + 8.0f }, "虚拟等待声道 (Virtual Voices)", theme.TextMuted, 11.0f);

			std::snprintf(buf, sizeof(buf), "%u / %u", profilerSnap.activeVirtualVoices, profilerSnap.totalVirtualVoices);
			Wui::Label(ctx, { card.X + 8.0f, card.Y + 26.0f }, buf, { 0.75f, 0.50f, 0.95f, 1.0f }, 18.0f);

			const Wui::WuiRect barBg { card.X + 8.0f, card.Y + 50.0f, card.W - 16.0f, 8.0f };
			Wui::PanelBackground(ctx, barBg, theme.PanelBg, 2.0f);
			const float ratio = std::clamp(static_cast<float>(profilerSnap.activeVirtualVoices) /
				static_cast<float>(std::max(1u, profilerSnap.totalVirtualVoices)), 0.0f, 1.0f);
			if (ratio > 0.005f)
			{
				Wui::PanelBackground(ctx, { barBg.X, barBg.Y, barBg.W * ratio, barBg.H }, { 0.75f, 0.50f, 0.95f, 0.95f }, 2.0f);
			}

			Wui::Tooltip(ctx, card, "超出听觉感知或衰减范围的虚拟声部 (仅推进光标, 0 CPU 混音开销)");
		}

		// ---- 卡片 4: DAC 削波报警灯 ----
		{
			const Wui::WuiRect card { rect.X + (cardW + cardGap) * 3.0f, rect.Y, cardW, rect.H };
			Wui::PanelBackground(ctx, card, theme.ContentBg, 4.0f);

			Wui::Label(ctx, { card.X + 8.0f, card.Y + 8.0f }, "DAC 输出电平 (Master Out)", theme.TextMuted, 11.0f);

			const bool isClipped = profilerSnap.masterClipping || audio.GetProfiler().IsMasterClipping();
			const Wui::WuiRect ledRect { card.X + 8.0f, card.Y + 26.0f, card.W - 16.0f, 22.0f };

			if (isClipped)
			{
				Wui::PanelBackground(ctx, ledRect, { 0.95f, 0.20f, 0.20f, 0.95f }, 3.0f);
				Wui::Label(ctx, { ledRect.X + 8.0f, ledRect.Y + 3.0f }, "\xe2\x97\x8f DAC 削波 (Clipping)!", { 1.0f, 1.0f, 1.0f, 1.0f }, 11.0f);

				const Wui::WuiRect clearBtn { card.X + 8.0f, card.Y + 50.0f, card.W - 16.0f, 14.0f };
				if (Wui::Button(ctx, Wui::HashId("profiler.btn.clear_clip"), clearBtn, "清除削波报警", theme))
				{
					audio.GetProfiler().ResetClippingFlags();
				}
			}
			else
			{
				Wui::PanelBackground(ctx, ledRect, { 0.15f, 0.35f, 0.20f, 0.90f }, 3.0f);
				Wui::Label(ctx, { ledRect.X + 8.0f, ledRect.Y + 3.0f }, "\xe2\x97\x8f 输出电平正常 (Normal)", { 0.45f, 0.85f, 0.55f, 0.95f }, 11.0f);
				Wui::Label(ctx, { card.X + 8.0f, card.Y + 50.0f }, "无数字削波溢出 (Headroom OK)", theme.TextMuted, 10.0f);
			}

			Wui::Tooltip(ctx, card, isClipped ? "警告: 主混音总线输出超过 0 dBFS 发生数字削波！请降低总音量或调整主控限幅器" : "主总线输出电平在安全动态余量内，无削波失真");
		}
	}

	void AudioProfilerPanel::RenderFftSpectrum(Wui::WuiContext& ctx, const Wui::WuiRect& rect, PanelHost& host)
	{
		const Wui::WuiTheme& theme = host.Theme();
		auto& audio = Audio::AudioSystem::Get();

		Wui::PanelBackground(ctx, rect, theme.ContentBg, 4.0f);

		// Header
		Wui::Label(ctx, { rect.X + 10.0f, rect.Y + 8.0f },
			"64 频段全量 FFT 频谱柱状图与实时峰值保持线 (64-Band Real-Time Spectrum & Peak Hold)",
			{ 0.35f, 0.85f, 0.95f, 1.0f }, 12.0f);

		char infoBuf[128];
		const bool beat = audio.IsBeatDetected();
		std::snprintf(infoBuf, sizeof(infoBuf), "采样率: 48 kHz | 混音块: 256 帧 | 节拍: %s", beat ? "\xe2\x97\x8f BEAT" : "\xe2\x97\x8b IDLE");
		Wui::Label(ctx, { rect.X + rect.W - 250.0f, rect.Y + 8.0f }, infoBuf,
			beat ? Wui::WuiColor{ 0.95f, 0.85f, 0.25f, 1.0f } : theme.TextMuted, 11.0f);

		// Query 64 bands from FFT analyzer
		audio.GetSpectrum(m_SpectrumBands, 64);
		if (m_SpectrumBands.size() < 64)
			m_SpectrumBands.assign(64, 0.0f);
		if (m_PeakHoldBands.size() < 64)
			m_PeakHoldBands.assign(64, 0.0f);
		if (m_PeakHoldTimers.size() < 64)
			m_PeakHoldTimers.assign(64, 0.0f);

		// Decay and update peak holds
		constexpr float dt = 0.016f;
		for (size_t b = 0; b < 64; ++b)
		{
			const float cur = std::clamp(m_SpectrumBands[b], 0.0f, 1.0f);
			if (cur >= m_PeakHoldBands[b])
			{
				m_PeakHoldBands[b] = cur;
				m_PeakHoldTimers[b] = 0.5f; // Hold peak for 500ms
			}
			else if (m_PeakHoldTimers[b] > 0.0f)
			{
				m_PeakHoldTimers[b] -= dt;
			}
			else
			{
				m_PeakHoldBands[b] = std::max(0.0f, m_PeakHoldBands[b] - 0.7f * dt);
			}
		}

		// Draw canvas
		const float padX = 10.0f;
		const float topPad = 28.0f;
		const float botPad = 18.0f;
		const float innerW = rect.W - padX * 2.0f;
		const float innerH = rect.H - topPad - botPad;
		const float barGap = 1.0f;
		const float barW = std::max(2.0f, (innerW - 63.0f * barGap) / 64.0f);

		for (uint32_t b = 0; b < 64; ++b)
		{
			const float val = std::clamp(m_SpectrumBands[b], 0.0f, 1.0f);
			const float barH = std::max(2.0f, val * innerH);
			const float bx = rect.X + padX + static_cast<float>(b) * (barW + barGap);
			const float by = rect.Y + rect.H - botPad - barH;

			// 4-stage color spectrum across frequency range
			const float t = static_cast<float>(b) / 63.0f;
			Wui::WuiColor barColor;
			if (t < 0.25f)
			{
				const float u = t / 0.25f;
				barColor = { glm::mix(0.15f, 0.20f, u), glm::mix(0.70f, 0.88f, u), glm::mix(0.95f, 0.65f, u), val > 0.03f ? 0.95f : 0.35f };
			}
			else if (t < 0.50f)
			{
				const float u = (t - 0.25f) / 0.25f;
				barColor = { glm::mix(0.20f, 0.40f, u), glm::mix(0.88f, 0.85f, u), glm::mix(0.65f, 0.25f, u), val > 0.03f ? 0.95f : 0.35f };
			}
			else if (t < 0.75f)
			{
				const float u = (t - 0.50f) / 0.25f;
				barColor = { glm::mix(0.40f, 0.95f, u), glm::mix(0.85f, 0.75f, u), glm::mix(0.25f, 0.20f, u), val > 0.03f ? 0.95f : 0.35f };
			}
			else
			{
				const float u = (t - 0.75f) / 0.25f;
				barColor = { glm::mix(0.95f, 0.98f, u), glm::mix(0.75f, 0.30f, u), glm::mix(0.20f, 0.25f, u), val > 0.03f ? 0.95f : 0.35f };
			}

			// Spectrum bar
			Wui::PanelBackground(ctx, { bx, by, barW, barH }, barColor, 1.0f);

			// Real-time Peak Hold Line (2px cap)
			const float peakVal = std::clamp(m_PeakHoldBands[b], 0.0f, 1.0f);
			if (peakVal > 0.02f)
			{
				const float peakY = rect.Y + rect.H - botPad - (peakVal * innerH);
				Wui::PanelBackground(ctx, { bx, peakY, barW, 2.0f }, { 1.0f, 0.95f, 0.55f, 0.95f }, 0.5f);
			}
		}

		// Frequency markings
		const float labelY = rect.Y + rect.H - botPad + 3.0f;
		Wui::Label(ctx, { rect.X + padX, labelY }, "20Hz", theme.TextMuted, 9.5f);
		Wui::Label(ctx, { rect.X + padX + innerW * 0.16f - 10.0f, labelY }, "60Hz", theme.TextMuted, 9.5f);
		Wui::Label(ctx, { rect.X + padX + innerW * 0.33f - 12.0f, labelY }, "250Hz", theme.TextMuted, 9.5f);
		Wui::Label(ctx, { rect.X + padX + innerW * 0.50f - 10.0f, labelY }, "1kHz", theme.TextMuted, 9.5f);
		Wui::Label(ctx, { rect.X + padX + innerW * 0.67f - 10.0f, labelY }, "4kHz", theme.TextMuted, 9.5f);
		Wui::Label(ctx, { rect.X + padX + innerW * 0.83f - 12.0f, labelY }, "10kHz", theme.TextMuted, 9.5f);
		Wui::Label(ctx, { rect.X + padX + innerW - 28.0f, labelY }, "20kHz", theme.TextMuted, 9.5f);
	}

	void AudioProfilerPanel::RenderActiveVoicesTable(Wui::WuiContext& ctx, const Wui::WuiRect& rect, PanelHost& host)
	{
		const Wui::WuiTheme& theme = host.Theme();
		auto& audio = Audio::AudioSystem::Get();
		const auto profilerSnap = audio.GetProfiler().GetSnapshot();

		Wui::PanelBackground(ctx, rect, theme.ContentBg, 4.0f);

		// Collect active voices
		std::vector<VoiceDetailRow> rows;
		std::vector<Audio::VoiceHandle> collectedHandles;

		auto activeScene = host.GetActiveScene();
		if (activeScene)
		{
			const auto& entityVoiceMap = audio.GetEntityVoiceMap();
			for (const auto& [entityId, handle] : entityVoiceMap)
			{
				Audio::Voice* voice = audio.GetVoicePool().FindVoice(handle);
				if (!voice || !voice->IsActive())
					continue;

				VoiceDetailRow row;
				row.handle = handle;
				collectedHandles.push_back(handle);
				m_MaxDiscoveredVoiceHandle = std::max(m_MaxDiscoveredVoiceHandle, handle);

				Entity ent(activeScene.get(), static_cast<entt::entity>(entityId));
				if (ent.IsValid())
				{
					row.entity = ent;
					if (ent.HasComponent<TagComponent>())
						row.nameOrEvent = StringPool::Get().NameOf(ent.GetComponent<TagComponent>().Tag);

					if (ent.HasComponent<AudioSourceComponent>())
					{
						const auto& src = ent.GetComponent<AudioSourceComponent>();
						std::string soundPath = StringPool::Get().PathOf(src.Sound.Path);
						std::string fname = std::filesystem::path(soundPath).filename().string();
						if (!fname.empty())
						{
							if (!row.nameOrEvent.empty())
								row.nameOrEvent += " (" + fname + ")";
							else
								row.nameOrEvent = fname;
						}
					}
					if (ent.HasComponent<TransformComponent>())
					{
						row.worldPos = ent.GetComponent<TransformComponent>().Location;
						row.has3DPos = true;
						row.distanceToListener = glm::distance(row.worldPos, audio.GetListenerPosition());
					}
				}
				if (row.nameOrEvent.empty())
					row.nameOrEvent = "ECS AudioSource";

				const float gain = std::max(voice->GetLeftGain(), voice->GetRightGain());
				row.volumeDb = (gain > 0.00001f) ? (20.0f * std::log10(gain)) : -96.0f;
				row.lpfHz = voice->GetAirAbsorptionCutoff();

				switch (voice->GetState())
				{
				case Audio::VoiceState::Playing:
					row.stateStr = "Physical";
					row.stateColor = { 0.25f, 0.85f, 0.95f, 1.0f };
					break;
				case Audio::VoiceState::Virtual:
					row.stateStr = "Virtual";
					row.stateColor = { 0.75f, 0.50f, 0.95f, 1.0f };
					break;
				case Audio::VoiceState::Stopping:
					row.stateStr = "Stopping";
					row.stateColor = { 0.95f, 0.65f, 0.25f, 1.0f };
					break;
				case Audio::VoiceState::Paused:
					row.stateStr = "Paused";
					row.stateColor = { 0.65f, 0.65f, 0.65f, 1.0f };
					break;
				default:
					row.stateStr = "Active";
					row.stateColor = theme.Text;
					break;
				}
				rows.push_back(std::move(row));
			}
		}

		// Also check any standalone or audition voices
		const uint32_t totalActive = profilerSnap.activePhysicalVoices + profilerSnap.activeVirtualVoices;
		if (rows.size() < totalActive)
		{
			const uint32_t scanLimit = std::max(128u, m_MaxDiscoveredVoiceHandle + 32u);
			for (Audio::VoiceHandle h = 1; h <= scanLimit; ++h)
			{
				if (std::find(collectedHandles.begin(), collectedHandles.end(), h) != collectedHandles.end())
					continue;

				if (auto* voice = audio.GetVoicePool().FindVoice(h))
				{
					if (!voice->IsActive())
						continue;

					VoiceDetailRow row;
					row.handle = h;
					m_MaxDiscoveredVoiceHandle = std::max(m_MaxDiscoveredVoiceHandle, h);
					row.nameOrEvent = "Event / 2D Audition";
					row.has3DPos = false;
					row.distanceToListener = 0.0f;

					const float gain = std::max(voice->GetLeftGain(), voice->GetRightGain());
					row.volumeDb = (gain > 0.00001f) ? (20.0f * std::log10(gain)) : -96.0f;
					row.lpfHz = voice->GetAirAbsorptionCutoff();

					switch (voice->GetState())
					{
					case Audio::VoiceState::Playing:
						row.stateStr = "Physical";
						row.stateColor = { 0.25f, 0.85f, 0.95f, 1.0f };
						break;
					case Audio::VoiceState::Virtual:
						row.stateStr = "Virtual";
						row.stateColor = { 0.75f, 0.50f, 0.95f, 1.0f };
						break;
					case Audio::VoiceState::Stopping:
						row.stateStr = "Stopping";
						row.stateColor = { 0.95f, 0.65f, 0.25f, 1.0f };
						break;
					case Audio::VoiceState::Paused:
						row.stateStr = "Paused";
						row.stateColor = { 0.65f, 0.65f, 0.65f, 1.0f };
						break;
					default:
						row.stateStr = "Active";
						row.stateColor = theme.Text;
						break;
					}
					rows.push_back(std::move(row));
					if (rows.size() >= totalActive)
						break;
				}
			}
		}

		// Responsive column widths
		constexpr float colHandleW = 56.0f;
		constexpr float colCoordW = 140.0f;
		constexpr float colDistW = 85.0f;
		constexpr float colVolW = 85.0f;
		constexpr float colLpfW = 95.0f;
		constexpr float colStateW = 90.0f;
		constexpr float colActionW = 65.0f;
		const float fixedCols = colHandleW + colCoordW + colDistW + colVolW + colLpfW + colStateW + colActionW + 24.0f;
		const float colAssetW = std::max(160.0f, rect.W - fixedCols);

		// Table Header
		const Wui::WuiRect headerRect { rect.X + 4.0f, rect.Y + 4.0f, rect.W - 8.0f, 24.0f };
		Wui::PanelBackground(ctx, headerRect, theme.PanelBg, 2.0f);

		float colX = headerRect.X + 6.0f;
		Wui::Label(ctx, { colX, headerRect.Y + 4.0f }, "句柄", theme.TextMuted, 10.5f);
		colX += colHandleW;
		Wui::Label(ctx, { colX, headerRect.Y + 4.0f }, "所属事件 / 资产", theme.TextMuted, 10.5f);
		colX += colAssetW;
		Wui::Label(ctx, { colX, headerRect.Y + 4.0f }, "空间坐标 X/Y/Z", theme.TextMuted, 10.5f);
		colX += colCoordW;
		Wui::Label(ctx, { colX, headerRect.Y + 4.0f }, "与听者距离", theme.TextMuted, 10.5f);
		colX += colDistW;
		Wui::Label(ctx, { colX, headerRect.Y + 4.0f }, "音量 dBFS", theme.TextMuted, 10.5f);
		colX += colVolW;
		Wui::Label(ctx, { colX, headerRect.Y + 4.0f }, "低通滤波 Hz", theme.TextMuted, 10.5f);
		colX += colLpfW;
		Wui::Label(ctx, { colX, headerRect.Y + 4.0f }, "当前状态", theme.TextMuted, 10.5f);
		colX += colStateW;
		Wui::Label(ctx, { colX, headerRect.Y + 4.0f }, "操作", theme.TextMuted, 10.5f);

		// Table Rows
		float rowY = headerRect.Y + headerRect.H + 2.0f;
		constexpr float rowH = 24.0f;

		if (rows.empty())
		{
			Wui::Label(ctx, { rect.X + 16.0f, rowY + 12.0f },
				"当前无活跃声部 (No active voices playing). 在场景中播放或使用 AI 命令触发音频.", theme.TextMuted, 11.0f);
			return;
		}

		char cellBuf[64];
		for (size_t i = 0; i < rows.size(); ++i)
		{
			if (rowY + rowH > rect.Y + rect.H - 4.0f)
				break;

			const auto& row = rows[i];
			const Wui::WuiRect rowRect { rect.X + 4.0f, rowY, rect.W - 8.0f, rowH };
			const bool isHovered = ctx.IsHovered(rowRect);

			// Zebra striping and hover
			const Wui::WuiColor rowBg = isHovered ? Wui::WuiColor{ theme.ContentBg.R + 0.05f, theme.ContentBg.G + 0.05f, theme.ContentBg.B + 0.05f, 0.95f } :
				((i % 2 == 1) ? theme.PanelBg : theme.ContentBg);
			Wui::PanelBackground(ctx, rowRect, rowBg, 2.0f);

			// Row click to select entity
			if (isHovered && ctx.IsClicked(rowRect) && row.entity.IsValid())
			{
				host.SetSelectedEntity(row.entity);
			}

			float cellX = rowRect.X + 6.0f;

			// 1. Handle
			std::snprintf(cellBuf, sizeof(cellBuf), "#%u", row.handle);
			Wui::Label(ctx, { cellX, rowY + 4.0f }, cellBuf, theme.Text, 11.0f);
			cellX += colHandleW;

			// 2. Name or Event
			Wui::Label(ctx, { cellX, rowY + 4.0f }, row.nameOrEvent.c_str(), theme.Text, 11.0f);
			cellX += colAssetW;

			// 3. World Coordinates
			if (row.has3DPos)
				std::snprintf(cellBuf, sizeof(cellBuf), "(%.1f, %.1f, %.1f)", row.worldPos.x, row.worldPos.y, row.worldPos.z);
			else
				std::snprintf(cellBuf, sizeof(cellBuf), "2D (非空间化)");
			Wui::Label(ctx, { cellX, rowY + 4.0f }, cellBuf, row.has3DPos ? theme.Text : theme.TextMuted, 10.5f);
			cellX += colCoordW;

			// 4. Distance to Listener
			if (row.has3DPos)
				std::snprintf(cellBuf, sizeof(cellBuf), "%.1f m", row.distanceToListener);
			else
				std::snprintf(cellBuf, sizeof(cellBuf), "\xe2\x80\x94");
			Wui::Label(ctx, { cellX, rowY + 4.0f }, cellBuf, theme.Text, 10.5f);
			cellX += colDistW;

			// 5. Volume dBFS
			std::snprintf(cellBuf, sizeof(cellBuf), "%.1f dB", row.volumeDb);
			const Wui::WuiColor volColor = (row.volumeDb >= 0.0f) ? Wui::WuiColor{ 0.95f, 0.35f, 0.35f, 1.0f } :
				((row.volumeDb >= -6.0f) ? Wui::WuiColor{ 0.95f, 0.80f, 0.25f, 1.0f } : Wui::WuiColor{ 0.35f, 0.85f, 0.55f, 1.0f });
			Wui::Label(ctx, { cellX, rowY + 4.0f }, cellBuf, volColor, 10.5f);
			cellX += colVolW;

			// 6. LPF Cutoff Hz
			if (row.lpfHz >= 19999.0f)
				std::snprintf(cellBuf, sizeof(cellBuf), ">20 kHz (直通)");
			else
				std::snprintf(cellBuf, sizeof(cellBuf), "%.0f Hz", row.lpfHz);
			Wui::Label(ctx, { cellX, rowY + 4.0f }, cellBuf, (row.lpfHz >= 19999.0f) ? theme.TextMuted : theme.Text, 10.5f);
			cellX += colLpfW;

			// 7. State badge
			const Wui::WuiRect stateBadge { cellX, rowY + 2.0f, 74.0f, 20.0f };
			Wui::PanelBackground(ctx, stateBadge, { row.stateColor.R, row.stateColor.G, row.stateColor.B, 0.20f }, 2.0f);
			Wui::Label(ctx, { stateBadge.X + 6.0f, stateBadge.Y + 2.5f }, row.stateStr.c_str(), row.stateColor, 10.5f);
			cellX += colStateW;

			// 8. Action button (Stop)
			const Wui::WuiRect btnStop { cellX, rowY + 2.0f, 52.0f, 20.0f };
			if (Wui::Button(ctx, Wui::HashId(("profiler.btn.stop_" + std::to_string(row.handle)).c_str()), btnStop, "停止", theme))
			{
				audio.StopVoice(row.handle, 0.05f);
			}

			rowY += rowH + 2.0f;
		}
	}
}
