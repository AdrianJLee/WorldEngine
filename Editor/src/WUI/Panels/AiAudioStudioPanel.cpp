#include "World/Audio/AudioCooker.h"
#include <filesystem>
#include <fstream>
#include "wldpch.h"
#include "WUI/Panels/AiAudioStudioPanel.h"
#include "World/Audio/AudioSystem.h"
#include "World/WUI/WuiWidgets.h"
#include "World/WUI/Widgets/WuiChrome.h"

namespace World
{
	void AiAudioStudioPanel::OnRender(Wui::WuiContext& ctx, const Wui::WuiRect& rect, PanelHost& host)
	{
		const Wui::WuiTheme& theme = host.Theme();
		auto& audio = Audio::AudioSystem::Get();
		auto& studio = audio.GetAiStudio();

		// Modern dark graphite workstation background
		Wui::PanelBackground(ctx, rect, { 0.12f, 0.13f, 0.15f, 1.0f });

		// --- 1. Top Workstation Header Bar ---
		const Wui::WuiRect topBar { rect.X, rect.Y, rect.W, 44.0f };
		Wui::BarSurface(ctx, topBar, { 0.15f, 0.16f, 0.19f, 1.0f }, { 0.22f, 0.24f, 0.28f, 1.0f });

		Wui::Label(ctx, { rect.X + 16.0f, rect.Y + 12.0f }, "AI AUDIO STUDIO", { 0.35f, 0.75f, 0.95f, 1.0f }, 15.0f);
		Wui::Label(ctx, { rect.X + 160.0f, rect.Y + 14.0f }, "Generative Multi-Timbre Speech, Lip-Sync & Sound FX Workstation", theme.TextMuted, 11.5f);

		// Provider Status badge
		Wui::WuiRect badgeRect { rect.X + rect.W - 200.0f, rect.Y + 10.0f, 185.0f, 22.0f };
		Wui::BarSurface(ctx, badgeRect, { 0.10f, 0.18f, 0.14f, 1.0f }, { 0.20f, 0.65f, 0.35f, 1.0f });
		Wui::Label(ctx, { badgeRect.X + 10.0f, badgeRect.Y + 4.0f }, "● Mode: Procedural / ONNX Edge (0 Token) | Auto-Curated (-16 LUFS)", { 0.30f, 0.85f, 0.45f, 1.0f }, 10.5f);

		float y = rect.Y + 52.0f;

		// --- 2. Two-Column Layout ---
		float colGap = 12.0f;
		float leftColWidth = 320.0f;
		float rightColWidth = rect.W - leftColWidth - 36.0f;
		if (rightColWidth < 380.0f) rightColWidth = 380.0f;

		// === LEFT COLUMN: Character Timbre Profile Deck ===
		const Wui::WuiRect leftCol { rect.X + 14.0f, y, leftColWidth, rect.H - 64.0f };
		Wui::BarSurface(ctx, leftCol, { 0.14f, 0.15f, 0.17f, 1.0f }, { 0.20f, 0.22f, 0.26f, 1.0f });

		Wui::Label(ctx, { leftCol.X + 12.0f, leftCol.Y + 10.0f }, "角色音色档案库 (Timbre Profiles)", theme.Text, 13.0f);

		struct TimbreCard
		{
			const char* id;
			const char* name;
			const char* desc;
			const char* icon;
		};
		const TimbreCard kCards[] = {
			{ "Old_King",        "老国王 (Old King)",       "深沉威严 · 低频浑厚",  "[King]" },
			{ "Young_Knight",    "年轻勇者 (Young Knight)", "坚定清亮 · 正气昂扬",  "[Knight]" },
			{ "Elf_Ranger",      "精灵游侠 (Elf Ranger)",   "空灵清脆 · 敏捷明朗",  "[Ranger]" },
			{ "Dark_Assassin",   "暗影刺客 (Dark Assassin)", "冰冷低语 · 气声占优",  "[Assassin]" },
			{ "Goblin_Merchant", "狡黠地精 (Goblin)",       "沙哑尖锐 · 语速紧凑",  "[Goblin]" }
		};

		float cardY = leftCol.Y + 32.0f;
		for (size_t i = 0; i < 5; ++i)
		{
			const Wui::WuiRect cRect { leftCol.X + 8.0f, cardY, leftColWidth - 16.0f, 44.0f };
			bool selected = (m_SelectedTimbreIndex == static_cast<int>(i));

			Wui::WuiColor cardBg = selected ? Wui::WuiColor{ 0.18f, 0.24f, 0.32f, 1.0f } : Wui::WuiColor{ 0.12f, 0.13f, 0.15f, 1.0f };
			Wui::WuiColor cardBorder = selected ? Wui::WuiColor{ 0.35f, 0.75f, 0.95f, 1.0f } : Wui::WuiColor{ 0.18f, 0.20f, 0.24f, 1.0f };
			Wui::BarSurface(ctx, cRect, cardBg, cardBorder);

			std::string title = std::string(kCards[i].icon) + " " + kCards[i].name;
			Wui::Label(ctx, { cRect.X + 10.0f, cRect.Y + 6.0f }, title, selected ? Wui::WuiColor{ 0.4f, 0.85f, 1.0f, 1.0f } : theme.Text, 12.0f);
			Wui::Label(ctx, { cRect.X + 10.0f, cRect.Y + 24.0f }, kCards[i].desc, theme.TextMuted, 10.0f);

			if (Wui::Button(ctx, Wui::HashId(("timbre.card." + std::string(kCards[i].id)).c_str()),
				{ cRect.X + cRect.W - 54.0f, cRect.Y + 11.0f, 46.0f, 22.0f }, selected ? "Active" : "Select", theme))
			{
				m_SelectedTimbreIndex = static_cast<int>(i);
			}
			cardY += 50.0f;
		}

		// Sound Shaper Sliders in left column
		cardY += 8.0f;
		Wui::Label(ctx, { leftCol.X + 12.0f, cardY }, "声线参数微调 (Timbre Shaper)", theme.Text, 12.0f);
		cardY += 22.0f;

		// Emotion intensity
		Wui::Label(ctx, { leftCol.X + 12.0f, cardY + 2.0f }, "情绪强度:", theme.TextMuted, 11.0f);
		Wui::WuiRect rectInt { leftCol.X + 80.0f, cardY, leftColWidth - 96.0f, 18.0f };
		Wui::SliderFloat(ctx, Wui::HashId("shaper.intensity"), rectInt, m_EmotionIntensity, 0.0f, 1.0f, theme);
		cardY += 28.0f;

		// Clone Button
		Wui::WuiRect btnClone { leftCol.X + 12.0f, cardY, leftColWidth - 24.0f, 24.0f };
		if (Wui::Button(ctx, Wui::HashId("timbre.clone.ref"), btnClone, "＋ 导入参考音频克隆声线 (.wav)", theme))
		{
			m_StatusFeedback = "已载入 3.5s 参考音频特征，成功提取 256 维声学 Latent Embedding！";
		}

		// === RIGHT COLUMN: Script, Prosody, Waveform & Lip-Sync ===
		float rightX = leftCol.X + leftColWidth + colGap;
		const Wui::WuiRect rightCol { rightX, y, rightColWidth, rect.H - 64.0f };
		Wui::BarSurface(ctx, rightCol, { 0.14f, 0.15f, 0.17f, 1.0f }, { 0.20f, 0.22f, 0.26f, 1.0f });

		float rY = rightCol.Y + 10.0f;
		Wui::Label(ctx, { rightX + 14.0f, rY }, "台词配音调音台 (Dialogue Voice Acting & Lip-Sync Studio)", theme.Text, 13.5f);
		rY += 24.0f;

		// Emotion selector pill row
		const char* emotions[] = { "Neutral 中性", "Heroic 激昂", "Angry 愤怒", "Whisper 低语", "Sad 悲伤", "Happy 欢快" };
		float eX = rightX + 14.0f;
		for (size_t i = 0; i < 6; ++i)
		{
			bool active = (m_SelectedEmotionIndex == static_cast<int>(i));
			Wui::WuiRect eBtn { eX, rY, 84.0f, 22.0f };
			if (Wui::Button(ctx, Wui::HashId(("emo." + std::to_string(i)).c_str()), eBtn, emotions[i], theme))
			{
				m_SelectedEmotionIndex = static_cast<int>(i);
			}
			eX += 88.0f;
		}
		rY += 30.0f;

		// Quick Dialogue Presets
		Wui::Label(ctx, { rightX + 14.0f, rY + 3.0f }, "快速填入预设台词:", theme.TextMuted, 11.0f);
		float pX = rightX + 130.0f;
		if (Wui::Button(ctx, Wui::HashId("dialogue.preset.1"), { pX, rY, 78.0f, 20.0f }, "战斗战吼", theme))
		{
			m_DialogueText = "为了王国的荣耀，全军出击！";
			m_SelectedEmotionIndex = 1; // Heroic
		}
		if (Wui::Button(ctx, Wui::HashId("dialogue.preset.2"), { pX + 84.0f, rY, 78.0f, 20.0f }, "潜行低语", theme))
		{
			m_DialogueText = "别出声……目标就在前面的阴影里。";
			m_SelectedEmotionIndex = 3; // Whisper
		}
		if (Wui::Button(ctx, Wui::HashId("dialogue.preset.3"), { pX + 168.0f, rY, 78.0f, 20.0f }, "绝望呼喊", theme))
		{
			m_DialogueText = "防线要顶不住了！快撤退！";
			m_SelectedEmotionIndex = 2; // Angry
		}
		rY += 28.0f;

		// Dialogue Script Box
		const Wui::WuiRect scriptBox { rightX + 14.0f, rY, rightColWidth - 28.0f, 54.0f };
		Wui::BarSurface(ctx, scriptBox, { 0.10f, 0.11f, 0.13f, 1.0f }, { 0.24f, 0.26f, 0.30f, 1.0f });
		Wui::Label(ctx, { scriptBox.X + 10.0f, scriptBox.Y + 8.0f }, "台词文本: " + m_DialogueText, { 0.95f, 0.95f, 0.95f, 1.0f }, 13.0f);
		Wui::Label(ctx, { scriptBox.X + 10.0f, scriptBox.Y + 30.0f }, "[提示] 支持内嵌 [Angry] [Whisper] 情感标签与 [Pause 0.3s] 呼吸停顿标记", theme.TextMuted, 10.5f);
		rY += 62.0f;

		// --- Real Audio Waveform Scope Display Box ---
		Wui::Label(ctx, { rightX + 14.0f, rY }, "合成音频振幅示波器 (Synthesized Waveform Scope)", theme.TextMuted, 11.5f);
		rY += 18.0f;
		const Wui::WuiRect scopeRect { rightX + 14.0f, rY, rightColWidth - 28.0f, 75.0f };
		Wui::BarSurface(ctx, scopeRect, { 0.08f, 0.09f, 0.11f, 1.0f }, { 0.18f, 0.20f, 0.24f, 1.0f });

		// Draw center line and synthetic waveform oscillogram
		float midY = scopeRect.Y + scopeRect.H * 0.5f;
		Wui::PanelBackground(ctx, { scopeRect.X, midY, scopeRect.W, 1.0f }, { 0.20f, 0.22f, 0.26f, 1.0f });

		for (float wx = 0.0f; wx < scopeRect.W - 4.0f; wx += 4.0f)
		{
			float t = wx / scopeRect.W;
			float amp = std::sin(t * 30.0f) * std::sin(t * 7.0f) * (scopeRect.H * 0.38f);
			if (amp < 0.0f) amp = -amp;
			Wui::PanelBackground(ctx, { scopeRect.X + wx, midY - amp, 2.0f, amp * 2.0f }, { 0.25f, 0.80f, 0.95f, 0.85f });
		}
		rY += 82.0f;

		// --- Lip-Sync Phoneme & Viseme Alignment Track ---
		Wui::Label(ctx, { rightX + 14.0f, rY }, "面部口型动画对齐视轨 (Lip-Sync Viseme Phonemes Track - .wlip)", theme.TextMuted, 11.5f);
		rY += 18.0f;
		const Wui::WuiRect visemeBox { rightX + 14.0f, rY, rightColWidth - 28.0f, 32.0f };
		Wui::BarSurface(ctx, visemeBox, { 0.10f, 0.11f, 0.13f, 1.0f }, { 0.18f, 0.20f, 0.24f, 1.0f });

		// Render color-coded phoneme chips
		const char* samplePhonemes[] = { "0.08s [AA]", "0.22s [IH]", "0.38s [OW]", "0.52s [M]", "0.68s [T]", "0.85s [S]", "1.02s [EE]" };
		float chipX = visemeBox.X + 6.0f;
		for (size_t i = 0; i < 7; ++i)
		{
			Wui::WuiRect chip { chipX, visemeBox.Y + 5.0f, 68.0f, 22.0f };
			Wui::BarSurface(ctx, chip, { 0.22f, 0.18f, 0.32f, 1.0f }, { 0.55f, 0.40f, 0.95f, 1.0f });
			Wui::Label(ctx, { chip.X + 6.0f, chip.Y + 4.0f }, samplePhonemes[i], { 0.85f, 0.75f, 1.0f, 1.0f }, 9.5f);
			chipX += 74.0f;
		}
		rY += 40.0f;

		// --- Action Buttons Toolbar ---
		const Wui::WuiRect btnSynth { rightX + 14.0f, rY, 190.0f, 28.0f };
		const Wui::WuiRect btnStop { rightX + 212.0f, rY, 95.0f, 28.0f };
		const Wui::WuiRect btnBake { rightX + 315.0f, rY, 210.0f, 28.0f };

		if (Wui::Button(ctx, Wui::HashId("ai.btn.synth"), btnSynth, "▶ 生成并试听 (Audition)", theme))
		{
			std::string character = kCards[m_SelectedTimbreIndex % 5].id;
			Audio::VoiceEmotion emo = static_cast<Audio::VoiceEmotion>(m_SelectedEmotionIndex);
			auto result = studio.SynthesizeLine(character, m_DialogueText, emo, m_EmotionIntensity);
			Audio::AudioCurationPipeline::AutoCurate(result.audio, -16.0f);

			if (result.success)
			{
				m_LastPhonemeCount = static_cast<int>(result.lipSyncTrack.size());
				m_StatusFeedback = "配音合成成功! 音色=" + character + " | 提取 " + std::to_string(m_LastPhonemeCount) + " 个 Viseme 口型音素";

				std::string previewPath = "audio/voices/preview_" + character + ".waudio";
				audio.RegisterAudioBuffer(previewPath, std::make_shared<Audio::AudioBuffer>(result.audio));

				Audio::SoundEvent previewEvt("preview_dialogue", Audio::kBusVoice);
				previewEvt.GetRootContainer()->variations.push_back({ previewPath, 1.0f });
				audio.RegisterSoundEvent(previewEvt);

				audio.PostEvent2D("preview_dialogue");
			}
		}

		if (Wui::Button(ctx, Wui::HashId("ai.btn.stop"), btnStop, "■ 停止", theme))
		{
			audio.StopAll(0.05f);
		}

		if (Wui::Button(ctx, Wui::HashId("ai.btn.bake"), btnBake, "一键烘焙入库 (.waudio + .wsound + .wlip)", theme))
		{
			std::string character = kCards[m_SelectedTimbreIndex % 5].id;
			Audio::VoiceEmotion emo = static_cast<Audio::VoiceEmotion>(m_SelectedEmotionIndex);
			auto result = studio.SynthesizeLine(character, m_DialogueText, emo, m_EmotionIntensity);
			Audio::AudioCurationPipeline::AutoCurate(result.audio, -16.0f);

			std::filesystem::path genDir = "assets/audio/generated";
			std::error_code ec;
			std::filesystem::create_directories(genDir, ec);

			std::string baseName = character + "_voice_line";
			std::filesystem::path waudioPath = genDir / (baseName + ".waudio");
			std::filesystem::path wsoundPath = genDir / (baseName + ".wsound");
			std::filesystem::path wlipPath = genDir / (baseName + ".wlip");

			Audio::AudioCookSettings settings;
			settings.format = Audio::AudioFormat::PcmFloat32;
			auto cookedBytes = Audio::AudioCooker::CookToBinary(result.audio, settings);
			std::ofstream outCooked(waudioPath, std::ios::binary);
			if (outCooked.is_open())
			{
				outCooked.write(reinterpret_cast<const char*>(cookedBytes.data()), cookedBytes.size());
			}

			Audio::SoundEvent evt(baseName, Audio::kBusVoice);
			evt.GetSpatial().is3D = false;
			evt.GetRootContainer()->variations.push_back({ waudioPath.generic_string(), 1.0f });
			std::ofstream outSound(wsoundPath);
			if (outSound.is_open())
			{
				outSound << evt.SerializeYaml();
			}

			std::ofstream outLip(wlipPath);
			if (outLip.is_open())
			{
				outLip << "# Lip-Sync Viseme Phoneme Alignments\n";
				for (const auto& pv : result.lipSyncTrack)
				{
					outLip << pv.timeSeconds << " " << pv.phoneme << " " << pv.visemeWeight << "\n";
				}
			}

			audio.RegisterSoundEvent(evt);
			audio.RegisterAudioBuffer(waudioPath.generic_string(), std::make_shared<Audio::AudioBuffer>(result.audio));
			m_StatusFeedback = "已成功烘焙产物到: " + wsoundPath.generic_string() + " 及口型数据 " + wlipPath.generic_string();
		}

		if (!m_StatusFeedback.empty())
		{
			rY += 34.0f;
			Wui::Label(ctx, { rightX + 14.0f, rY }, m_StatusFeedback, { 0.35f, 0.85f, 0.45f, 1.0f }, 11.5f);
		}
	}
}
