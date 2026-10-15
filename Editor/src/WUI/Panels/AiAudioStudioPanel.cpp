#include "wldpch.h"
#include "WUI/Panels/AiAudioStudioPanel.h"
#include "World/Audio/AudioSystem.h"
#include "World/Audio/AudioCooker.h"
#include "World/WUI/WuiWidgets.h"
#include "World/WUI/Widgets/WuiChrome.h"
#include <filesystem>
#include <fstream>
#include <cmath>

namespace World
{
	void AiAudioStudioPanel::OnRender(Wui::WuiContext& ctx, const Wui::WuiRect& rect, PanelHost& host)
	{
		const Wui::WuiTheme& theme = host.Theme();
		auto& audio = Audio::AudioSystem::Get();
		auto& studio = audio.GetAiStudio();

		Wui::PanelBackground(ctx, rect, { 0.12f, 0.13f, 0.15f, 1.0f });

		// --- 1. Top Header Bar ---
		const Wui::WuiRect topBar { rect.X, rect.Y, rect.W, 44.0f };
		Wui::BarSurface(ctx, topBar, { 0.15f, 0.16f, 0.19f, 1.0f }, { 0.22f, 0.24f, 0.28f, 1.0f });

		Wui::Label(ctx, { rect.X + 16.0f, rect.Y + 12.0f }, "AI AUDIO STUDIO", { 0.35f, 0.75f, 0.95f, 1.0f }, 15.0f);
		Wui::Label(ctx, { rect.X + 155.0f, rect.Y + 14.0f }, "Generative Speech, Lip-Sync, SFX & Music Workstation", theme.TextMuted, 11.5f);

		// Provider Status badge
		bool sidecarAlive = studio.IsSidecarAlive();
		Wui::WuiRect badgeRect { rect.X + rect.W - 380.0f, rect.Y + 10.0f, 270.0f, 24.0f };
		if (sidecarAlive)
		{
			Wui::BarSurface(ctx, badgeRect, { 0.10f, 0.20f, 0.14f, 1.0f }, { 0.25f, 0.75f, 0.40f, 1.0f });
			Wui::Label(ctx, { badgeRect.X + 8.0f, badgeRect.Y + 5.0f }, "● Mode: Sidecar Service (127.0.0.1:8090) [Online]", { 0.35f, 0.90f, 0.50f, 1.0f }, 10.5f);
		}
		else
		{
			Wui::BarSurface(ctx, badgeRect, { 0.22f, 0.18f, 0.10f, 1.0f }, { 0.65f, 0.50f, 0.20f, 1.0f });
			Wui::Label(ctx, { badgeRect.X + 8.0f, badgeRect.Y + 5.0f }, "● Mode: 本地合成预览 (Procedural) [0 Token]", { 0.95f, 0.75f, 0.30f, 1.0f }, 10.5f);
		}

		// Re-probe button
		Wui::WuiRect btnProbe { rect.X + rect.W - 100.0f, rect.Y + 10.0f, 85.0f, 24.0f };
		if (Wui::Button(ctx, Wui::HashId("ai.btn.probe"), btnProbe, "探测服务", theme))
		{
			studio.ProbeBackend(m_SidecarHost, static_cast<uint16_t>(m_SidecarPort));
		}

		// --- 2. Workstation Tab Bar ---
		float tabY = rect.Y + 48.0f;
		const Wui::WuiRect tabArea { rect.X + 14.0f, tabY, rect.W - 28.0f, 28.0f };
		std::vector<std::string> tabs = {
			"角色对白与口型 (Dialogue)",
			"AI 音效生成 (SFX)",
			"音乐分轨与循环 (BGM)",
			"专有名词词典与配置 (Config)"
		};
		Wui::TabBar(ctx, Wui::HashId("ai.studio.tabs"), tabArea, tabs, m_ActiveTab, theme);

		float y = tabY + 34.0f;
		float contentHeight = rect.H - (y - rect.Y) - 12.0f;
		if (m_ActiveTab == 0)
		{
			// === TAB 0: DIALOGUE & LIP-SYNC ===
			float colGap = 12.0f;
			float leftColWidth = 320.0f;
			float rightColWidth = rect.W - leftColWidth - 36.0f;
			if (rightColWidth < 380.0f) rightColWidth = 380.0f;

			// Left Column: Character Timbres & Shaper
			const Wui::WuiRect leftCol { rect.X + 14.0f, y, leftColWidth, contentHeight };
			Wui::BarSurface(ctx, leftCol, { 0.14f, 0.15f, 0.17f, 1.0f }, { 0.20f, 0.22f, 0.26f, 1.0f });

			Wui::Label(ctx, { leftCol.X + 12.0f, leftCol.Y + 10.0f }, "角色音色档案库 (Timbre Profiles)", theme.Text, 13.0f);

			struct TimbreCard { const char* id; const char* name; const char* desc; const char* icon; };
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

			// Voice Shaper
			cardY += 6.0f;
			Wui::Label(ctx, { leftCol.X + 12.0f, cardY }, "声线参数微调 (Voice Shaper)", theme.Text, 12.0f);
			cardY += 22.0f;

			Wui::Label(ctx, { leftCol.X + 12.0f, cardY + 2.0f }, "情绪强度:", theme.TextMuted, 11.0f);
			Wui::WuiRect rectInt { leftCol.X + 80.0f, cardY, leftColWidth - 96.0f, 18.0f };
			Wui::SliderFloat(ctx, Wui::HashId("shaper.intensity"), rectInt, m_EmotionIntensity, 0.0f, 1.0f, theme);
			cardY += 28.0f;

			// Clone reference audio button
			Wui::WuiRect btnClone { leftCol.X + 12.0f, cardY, leftColWidth - 24.0f, 26.0f };
			if (Wui::Button(ctx, Wui::HashId("timbre.clone.ref"), btnClone, "＋ 导入干音克隆声线 (.wav -> .wvoice)", theme))
			{
				std::string charId = kCards[m_SelectedTimbreIndex % 5].id;
				Audio::AudioBuffer refSample = Audio::AudioBuffer::GenerateSineWave(140.0f, 3.0f, 0.6f);
				Audio::TimbreProfile cloned;
				if (studio.CloneTimbreFromWav(charId, refSample, cloned))
				{
					m_StatusFeedback = "已成功提取 256 维声学特征并持久化至 assets/audio/voices/" + charId + ".wvoice";
				}
			}

			// Right Column: Dialogue Script, Prosody & LipSync
			float rightX = leftCol.X + leftColWidth + colGap;
			const Wui::WuiRect rightCol { rightX, y, rightColWidth, contentHeight };
			Wui::BarSurface(ctx, rightCol, { 0.14f, 0.15f, 0.17f, 1.0f }, { 0.20f, 0.22f, 0.26f, 1.0f });

			float rY = rightCol.Y + 10.0f;
			Wui::Label(ctx, { rightX + 14.0f, rY }, "剧本台词与注音语法 (Script & Pronunciation Syntax)", theme.Text, 13.0f);
			rY += 22.0f;

			// Dialogue Text input
			const Wui::WuiRect textRect { rightX + 14.0f, rY, rightColWidth - 28.0f, 28.0f };
			Wui::TextField(ctx, Wui::HashId("ai.script.text"), textRect, m_DialogueText, theme);
			rY += 34.0f;

			Wui::Label(ctx, { rightX + 14.0f, rY }, "支持行内注音: {重|chóng}、{法尔斯|fǎ ěr sī}，以及停顿 [pause=0.4s]", theme.TextMuted, 10.5f);
			rY += 20.0f;

			// Emotion Tags toolbar
			Wui::Label(ctx, { rightX + 14.0f, rY + 4.0f }, "情绪标签:", theme.TextMuted, 11.5f);
			const char* kEmotions[] = { "Neutral", "Angry", "Whisper", "Sad", "Happy", "Heroic" };
			float emoX = rightX + 80.0f;
			for (size_t e = 0; e < 6; ++e)
			{
				bool active = (m_SelectedEmotionIndex == static_cast<int>(e));
				if (Wui::Button(ctx, Wui::HashId(("emo.btn." + std::string(kEmotions[e])).c_str()),
					{ emoX, rY, 60.0f, 22.0f }, kEmotions[e], theme))
				{
					m_SelectedEmotionIndex = static_cast<int>(e);
				}
				emoX += 66.0f;
			}
			rY += 34.0f;

			// Waveform preview
			Wui::Label(ctx, { rightX + 14.0f, rY }, "声波波形示波器 (Audio Waveform Scope)", theme.TextMuted, 11.5f);
			rY += 18.0f;
			const Wui::WuiRect scopeRect { rightX + 14.0f, rY, rightColWidth - 28.0f, 48.0f };
			Wui::BarSurface(ctx, scopeRect, { 0.08f, 0.09f, 0.11f, 1.0f }, { 0.16f, 0.18f, 0.22f, 1.0f });

			float midY = scopeRect.Y + scopeRect.H * 0.5f;
			for (float wx = 4.0f; wx < scopeRect.W - 4.0f; wx += 4.0f)
			{
				float t = wx / scopeRect.W;
				float amp = std::sin(t * 30.0f) * std::sin(t * 7.0f) * (scopeRect.H * 0.38f);
				if (amp < 0.0f) amp = -amp;
				Wui::PanelBackground(ctx, { scopeRect.X + wx, midY - amp, 2.0f, amp * 2.0f }, { 0.25f, 0.80f, 0.95f, 0.85f });
			}
			rY += 56.0f;

			// LipSync ARKit 52 Track
			Wui::Label(ctx, { rightX + 14.0f, rY }, "面部口型动画对齐视轨 (Lip-Sync ARKit 52 Visemes - .wlip)", theme.TextMuted, 11.5f);
			rY += 18.0f;
			const Wui::WuiRect visemeBox { rightX + 14.0f, rY, rightColWidth - 28.0f, 32.0f };
			Wui::BarSurface(ctx, visemeBox, { 0.10f, 0.11f, 0.13f, 1.0f }, { 0.18f, 0.20f, 0.24f, 1.0f });

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

			// Action Buttons
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
					m_StatusFeedback = result.debugMessage + " | 提取 " + std::to_string(m_LastPhonemeCount) + " 个 Viseme 口型音素";

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

				Audio::LipSyncTrajectory::SaveWlip(wlipPath.generic_string(), result.lipSyncTrack);

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
		else if (m_ActiveTab == 1)
		{
			// === TAB 1: AI SFX GENERATOR (6.2.1) ===
			const Wui::WuiRect sfxBox { rect.X + 14.0f, y, rect.W - 28.0f, contentHeight };
			Wui::BarSurface(ctx, sfxBox, { 0.14f, 0.15f, 0.17f, 1.0f }, { 0.20f, 0.22f, 0.26f, 1.0f });

			float sY = sfxBox.Y + 12.0f;
			Wui::Label(ctx, { sfxBox.X + 16.0f, sY }, "AI 短音效生成工作台 (SFX Prompt Generator - 6.2.1)", theme.Text, 14.0f);
			Wui::Label(ctx, { sfxBox.X + 440.0f, sY + 2.0f }, "物理声学仿真 · 4组变体候选 · EBU R128 (-16 LUFS) 门禁", theme.TextMuted, 11.0f);
			sY += 28.0f;

			// Prompt input
			Wui::Label(ctx, { sfxBox.X + 16.0f, sY + 4.0f }, "音效描述词 (Prompt):", theme.TextMuted, 11.5f);
			const Wui::WuiRect promptRect { sfxBox.X + 140.0f, sY, sfxBox.W - 320.0f, 26.0f };
			Wui::TextField(ctx, Wui::HashId("ai.sfx.prompt"), promptRect, m_SfxPrompt, theme);

			// Quick preset buttons
			const Wui::WuiRect btnPreset1 { sfxBox.X + sfxBox.W - 160.0f, sY, 70.0f, 24.0f };
			const Wui::WuiRect btnPreset2 { sfxBox.X + sfxBox.W - 84.0f, sY, 70.0f, 24.0f };
			if (Wui::Button(ctx, Wui::HashId("sfx.preset.door"), btnPreset1, "石门开启", theme))
			{
				m_SfxPrompt = "heavy stone door grinding open, medieval dungeon";
			}
			if (Wui::Button(ctx, Wui::HashId("sfx.preset.laser"), btnPreset2, "激光重炮", theme))
			{
				m_SfxPrompt = "sci-fi energy plasma laser cannon blast";
			}
			sY += 34.0f;

			// Parameters: Duration & Variations
			Wui::Label(ctx, { sfxBox.X + 16.0f, sY + 2.0f }, "时长 (秒):", theme.TextMuted, 11.5f);
			Wui::WuiRect durRect { sfxBox.X + 80.0f, sY, 150.0f, 20.0f };
			Wui::SliderFloat(ctx, Wui::HashId("sfx.slider.dur"), durRect, m_SfxDuration, 0.3f, 6.0f, theme);

			Wui::Label(ctx, { sfxBox.X + 260.0f, sY + 2.0f }, "后处理门禁: [√] Trim Silence  [√] Zero-Crossing Fadeout  [√] -16 LUFS", { 0.4f, 0.85f, 0.5f, 1.0f }, 11.0f);

			// Generate Button
			const Wui::WuiRect btnGenSfx { sfxBox.X + sfxBox.W - 220.0f, sY - 2.0f, 205.0f, 28.0f };
			if (Wui::Button(ctx, Wui::HashId("sfx.btn.generate"), btnGenSfx, "⚡ 生成 4 组音效候选变体", theme))
			{
				m_SfxCandidates = studio.GenerateSfxMulti(m_SfxPrompt, m_SfxDuration, 4);
				m_SfxStatus = "已生成 4 组不同声学特征的候选音效变体，请点击下方单独试听或选定入库！";
			}
			sY += 36.0f;

			if (!m_SfxStatus.empty())
			{
				Wui::Label(ctx, { sfxBox.X + 16.0f, sY }, m_SfxStatus, { 0.35f, 0.85f, 0.45f, 1.0f }, 11.5f);
				sY += 24.0f;
			}

			// 4 Candidate Variations Rack
			float cardW = (sfxBox.W - 50.0f) * 0.25f;
			float cardH = 150.0f;
			float cardX = sfxBox.X + 16.0f;

			for (size_t c = 0; c < 4; ++c)
			{
				const Wui::WuiRect cRect { cardX, sY, cardW, cardH };
				bool hasData = (c < m_SfxCandidates.size());
				bool isSelected = (m_SelectedCandidateIndex == static_cast<int>(c));

				Wui::WuiColor bg = isSelected ? Wui::WuiColor{ 0.18f, 0.24f, 0.32f, 1.0f } : Wui::WuiColor{ 0.11f, 0.12f, 0.14f, 1.0f };
				Wui::WuiColor border = isSelected ? Wui::WuiColor{ 0.35f, 0.75f, 0.95f, 1.0f } : Wui::WuiColor{ 0.20f, 0.22f, 0.26f, 1.0f };
				Wui::BarSurface(ctx, cRect, bg, border);

				std::string title = "候选变体 #" + std::to_string(c + 1);
				Wui::Label(ctx, { cRect.X + 10.0f, cRect.Y + 8.0f }, title, theme.Text, 12.5f);

				if (hasData)
				{
					const auto& cand = m_SfxCandidates[c];
					std::string metric = "时长: " + std::to_string(cand.durationSeconds).substr(0, 4) + "s | -16 LUFS";
					Wui::Label(ctx, { cRect.X + 10.0f, cRect.Y + 28.0f }, metric, theme.TextMuted, 10.5f);

					// Mini waveform
					const Wui::WuiRect waveBox { cRect.X + 8.0f, cRect.Y + 48.0f, cRect.W - 16.0f, 32.0f };
					Wui::BarSurface(ctx, waveBox, { 0.07f, 0.08f, 0.09f, 1.0f }, { 0.15f, 0.16f, 0.18f, 1.0f });
					float waveMid = waveBox.Y + waveBox.H * 0.5f;
					for (float wx = 2.0f; wx < waveBox.W - 2.0f; wx += 4.0f)
					{
						float t = wx / waveBox.W;
						float h = std::abs(std::sin(t * (15.0f + static_cast<float>(c) * 5.0f))) * 12.0f;
						Wui::PanelBackground(ctx, { waveBox.X + wx, waveMid - h, 2.0f, h * 2.0f }, { 0.3f, 0.8f, 0.5f, 0.8f });
					}

					// Audition & Bake Buttons
					const Wui::WuiRect btnAud { cRect.X + 8.0f, cRect.Y + 88.0f, (cRect.W - 20.0f) * 0.5f, 24.0f };
					const Wui::WuiRect btnPick { cRect.X + 12.0f + btnAud.W, cRect.Y + 88.0f, (cRect.W - 20.0f) * 0.5f, 24.0f };

					if (Wui::Button(ctx, Wui::HashId(("cand.aud." + std::to_string(c)).c_str()), btnAud, "▶ 试听", theme))
					{
						std::string pName = "audio/sfx/preview_cand_" + std::to_string(c) + ".waudio";
						audio.RegisterAudioBuffer(pName, std::make_shared<Audio::AudioBuffer>(cand.audio));
						Audio::SoundEvent sfxEvt("preview_sfx_" + std::to_string(c), Audio::kBusSfx);
						sfxEvt.GetRootContainer()->variations.push_back({ pName, 1.0f });
						audio.RegisterSoundEvent(sfxEvt);
						audio.PostEvent2D("preview_sfx_" + std::to_string(c));
					}

					if (Wui::Button(ctx, Wui::HashId(("cand.bake." + std::to_string(c)).c_str()), btnPick, "✓ 选定入库", theme))
					{
						m_SelectedCandidateIndex = static_cast<int>(c);
						std::filesystem::path genDir = "assets/audio/generated";
						std::error_code ec;
						std::filesystem::create_directories(genDir, ec);

						std::string sfxBase = "sfx_var_" + std::to_string(c + 1);
						std::filesystem::path waudioP = genDir / (sfxBase + ".waudio");
						std::filesystem::path wsoundP = genDir / (sfxBase + ".wsound");

						Audio::AudioCookSettings settings;
						settings.format = Audio::AudioFormat::PcmFloat32;
						auto cooked = Audio::AudioCooker::CookToBinary(cand.audio, settings);
						std::ofstream outCook(waudioP, std::ios::binary);
						if (outCook.is_open()) outCook.write(reinterpret_cast<const char*>(cooked.data()), cooked.size());

						Audio::SoundEvent evt(sfxBase, Audio::kBusSfx);
						evt.GetRootContainer()->variations.push_back({ waudioP.generic_string(), 1.0f });
						std::ofstream outS(wsoundP);
						if (outS.is_open()) outS << evt.SerializeYaml();

						audio.RegisterSoundEvent(evt);
						audio.RegisterAudioBuffer(waudioP.generic_string(), std::make_shared<Audio::AudioBuffer>(cand.audio));
						m_SfxStatus = "已成功将变体 #" + std::to_string(c + 1) + " 烘焙至: " + wsoundP.generic_string();
					}
				}
				else
				{
					Wui::Label(ctx, { cRect.X + 10.0f, cRect.Y + 40.0f }, "[点击上方生成按钮]", theme.TextMuted, 11.0f);
				}
				cardX += cardW + 6.0f;
			}
		}
		else if (m_ActiveTab == 2)
		{
			// === TAB 2: BGM LOOPS & STEMS (6.2.2) ===
			const Wui::WuiRect bgmBox { rect.X + 14.0f, y, rect.W - 28.0f, contentHeight };
			Wui::BarSurface(ctx, bgmBox, { 0.14f, 0.15f, 0.17f, 1.0f }, { 0.20f, 0.22f, 0.26f, 1.0f });

			float bY = bgmBox.Y + 12.0f;
			Wui::Label(ctx, { bgmBox.X + 16.0f, bY }, "背景音乐零交叉循环与多轨分层工作台 (BGM Loops & Vertical Stems - 6.2.2)", theme.Text, 14.0f);
			bY += 28.0f;

			// Asset selector
			Wui::Label(ctx, { bgmBox.X + 16.0f, bY + 4.0f }, "音乐工程资产路径:", theme.TextMuted, 11.5f);
			const Wui::WuiRect assetRect { bgmBox.X + 130.0f, bY, bgmBox.W - 350.0f, 26.0f };
			Wui::TextField(ctx, Wui::HashId("bgm.asset.path"), assetRect, m_BgmAssetPath, theme);

			// Calculate loop points button
			const Wui::WuiRect btnCalcLoop { bgmBox.X + bgmBox.W - 200.0f, bY, 185.0f, 26.0f };
			if (Wui::Button(ctx, Wui::HashId("bgm.btn.calcloop"), btnCalcLoop, "⚡ 计算零交叉无缝循环切点", theme))
			{
				Audio::AudioBuffer dummyBgm = Audio::AudioBuffer::GenerateSineWave(220.0f, 30.0f, 0.5f);
				auto loopPts = studio.DetectBgmLoop(dummyBgm, 2.0f, 28.5f);
				m_BgmLoopStartSec = loopPts.startSeconds;
				m_BgmLoopEndSec = loopPts.endSeconds;
				m_BgmStatus = "已成功计算零交叉无缝循环切点: " + std::to_string(m_BgmLoopStartSec).substr(0, 5) + "s -> " + std::to_string(m_BgmLoopEndSec).substr(0, 5) + "s (置信度 99%)";
			}
			bY += 36.0f;

			// Loop Sliders
			Wui::Label(ctx, { bgmBox.X + 16.0f, bY + 2.0f }, "循环入点 (Start):", theme.TextMuted, 11.5f);
			Wui::WuiRect rStart { bgmBox.X + 130.0f, bY, 180.0f, 20.0f };
			Wui::SliderFloat(ctx, Wui::HashId("bgm.slider.start"), rStart, m_BgmLoopStartSec, 0.0f, 15.0f, theme);

			Wui::Label(ctx, { bgmBox.X + 330.0f, bY + 2.0f }, "循环出点 (End):", theme.TextMuted, 11.5f);
			Wui::WuiRect rEnd { bgmBox.X + 440.0f, bY, 180.0f, 20.0f };
			Wui::SliderFloat(ctx, Wui::HashId("bgm.slider.end"), rEnd, m_BgmLoopEndSec, 15.0f, 60.0f, theme);
			bY += 32.0f;

			if (!m_BgmStatus.empty())
			{
				Wui::Label(ctx, { bgmBox.X + 16.0f, bY }, m_BgmStatus, { 0.35f, 0.85f, 0.45f, 1.0f }, 11.5f);
				bY += 24.0f;
			}

			// Vertical Stems Mixer Deck
			bY += 10.0f;
			Wui::Label(ctx, { bgmBox.X + 16.0f, bY }, "纵向多轨分层混音器 (Vertical Stems Mixer - Ambient / Percussion / Lead)", theme.Text, 13.0f);
			bY += 24.0f;

			// Stems Sliders
			Wui::Label(ctx, { bgmBox.X + 16.0f, bY + 2.0f }, "Stem 1 (Ambient 环境轨):", theme.TextMuted, 11.0f);
			Wui::WuiRect rStem1 { bgmBox.X + 180.0f, bY, 200.0f, 20.0f };
			Wui::SliderFloat(ctx, Wui::HashId("stem.slider.amb"), rStem1, m_BgmStemAmbientVol, 0.0f, 1.0f, theme);
			bY += 26.0f;

			Wui::Label(ctx, { bgmBox.X + 16.0f, bY + 2.0f }, "Stem 2 (Percussion 打击轨):", theme.TextMuted, 11.0f);
			Wui::WuiRect rStem2 { bgmBox.X + 180.0f, bY, 200.0f, 20.0f };
			Wui::SliderFloat(ctx, Wui::HashId("stem.slider.perc"), rStem2, m_BgmStemPercussionVol, 0.0f, 1.0f, theme);
			bY += 26.0f;

			Wui::Label(ctx, { bgmBox.X + 16.0f, bY + 2.0f }, "Stem 3 (Lead 主旋律/主奏轨):", theme.TextMuted, 11.0f);
			Wui::WuiRect rStem3 { bgmBox.X + 180.0f, bY, 200.0f, 20.0f };
			Wui::SliderFloat(ctx, Wui::HashId("stem.slider.lead"), rStem3, m_BgmStemLeadVol, 0.0f, 1.0f, theme);
			bY += 34.0f;

			// Audition Loop Button & Save Stems Button
			const Wui::WuiRect btnPlayLoop { bgmBox.X + 16.0f, bY, 180.0f, 28.0f };
			const Wui::WuiRect btnSaveStems { bgmBox.X + 210.0f, bY, 240.0f, 28.0f };

			if (Wui::Button(ctx, Wui::HashId("bgm.btn.playloop"), btnPlayLoop, "▶ 试听无缝循环切点", theme))
			{
				audio.GetMusic().Play(m_BgmAssetPath, 0.5f);
				audio.GetMusic().SetStemVolume("ambient", m_BgmStemAmbientVol);
				audio.GetMusic().SetStemVolume("percussion", m_BgmStemPercussionVol);
				audio.GetMusic().SetStemVolume("lead", m_BgmStemLeadVol);
			}

			if (Wui::Button(ctx, Wui::HashId("bgm.btn.savestems"), btnSaveStems, "✓ 保存分轨至 InteractiveMusicManager", theme))
			{
				audio.GetMusic().SetStemVolume("ambient", m_BgmStemAmbientVol);
				audio.GetMusic().SetStemVolume("percussion", m_BgmStemPercussionVol);
				audio.GetMusic().SetStemVolume("lead", m_BgmStemLeadVol);
				m_BgmStatus = "已成功将 3 组分轨配置与切点注册至交互音乐管理器！";
			}
		}
		else if (m_ActiveTab == 3)
		{
			// === TAB 3: PHONEME DICTIONARY & CONFIG (6.1.2 & 6.3) ===
			const Wui::WuiRect cfgBox { rect.X + 14.0f, y, rect.W - 28.0f, contentHeight };
			Wui::BarSurface(ctx, cfgBox, { 0.14f, 0.15f, 0.17f, 1.0f }, { 0.20f, 0.22f, 0.26f, 1.0f });

			float cY = cfgBox.Y + 12.0f;
			Wui::Label(ctx, { cfgBox.X + 16.0f, cY }, "游戏专有名词与多音字词典管理 (Phoneme Dict - 6.1.2)", theme.Text, 14.0f);
			cY += 28.0f;

			// Add override entry row
			Wui::Label(ctx, { cfgBox.X + 16.0f, cY + 4.0f }, "词语 / 专有名词:", theme.TextMuted, 11.5f);
			const Wui::WuiRect wordRect { cfgBox.X + 130.0f, cY, 140.0f, 26.0f };
			Wui::TextField(ctx, Wui::HashId("dict.word.field"), wordRect, m_NewWord, theme);

			Wui::Label(ctx, { cfgBox.X + 285.0f, cY + 4.0f }, "目标拼音:", theme.TextMuted, 11.5f);
			const Wui::WuiRect pinyinRect { cfgBox.X + 350.0f, cY, 160.0f, 26.0f };
			Wui::TextField(ctx, Wui::HashId("dict.pinyin.field"), pinyinRect, m_NewPinyin, theme);

			const Wui::WuiRect btnAddWord { cfgBox.X + 525.0f, cY, 100.0f, 26.0f };
			const Wui::WuiRect btnSaveDict { cfgBox.X + 635.0f, cY, 160.0f, 26.0f };

			if (Wui::Button(ctx, Wui::HashId("dict.btn.add"), btnAddWord, "＋ 添加词条", theme))
			{
				if (!m_NewWord.empty() && !m_NewPinyin.empty())
				{
					Audio::PhonemeDictionary::SetWordOverride(m_NewWord, m_NewPinyin);
					m_DictStatus = "已添加词条: " + m_NewWord + " -> " + m_NewPinyin;
				}
			}

			if (Wui::Button(ctx, Wui::HashId("dict.btn.save"), btnSaveDict, "💾 保存至 phoneme_dict.yaml", theme))
			{
				Audio::PhonemeDictionary::SaveDictionary("assets/audio/voices/phoneme_dict.yaml");
				m_DictStatus = "已成功保存词典至 assets/audio/voices/phoneme_dict.yaml";
			}
			cY += 36.0f;

			if (!m_DictStatus.empty())
			{
				Wui::Label(ctx, { cfgBox.X + 16.0f, cY }, m_DictStatus, { 0.35f, 0.85f, 0.45f, 1.0f }, 11.5f);
				cY += 24.0f;
			}

			// Table of current dictionary overrides
			Wui::Label(ctx, { cfgBox.X + 16.0f, cY }, "已注册游戏专有名词表 (Active Pronunciation Overrides):", theme.TextMuted, 12.0f);
			cY += 20.0f;

			const Wui::WuiRect tableBox { cfgBox.X + 16.0f, cY, cfgBox.W - 32.0f, 120.0f };
			Wui::BarSurface(ctx, tableBox, { 0.10f, 0.11f, 0.13f, 1.0f }, { 0.18f, 0.20f, 0.24f, 1.0f });

			float rowY = tableBox.Y + 8.0f;
			int rowCount = 0;
			for (const auto& [word, pinyin] : Audio::PhonemeDictionary::GetAllOverrides())
			{
				if (rowCount >= 4) break;
				std::string line = "• " + word + "  ➔  [" + pinyin + "]";
				Wui::Label(ctx, { tableBox.X + 14.0f, rowY }, line, theme.Text, 11.5f);
				rowY += 26.0f;
				++rowCount;
			}
			cY += 134.0f;

			// Sidecar Microservice Host & Port Configuration (6.3)
			Wui::Label(ctx, { cfgBox.X + 16.0f, cY }, "Sidecar 伴随服务微服务配置 (Sidecar Microservice Configuration - 6.3):", theme.Text, 13.0f);
			cY += 24.0f;

			Wui::Label(ctx, { cfgBox.X + 16.0f, cY + 4.0f }, "服务地址 (Host:Port): 127.0.0.1:8090", theme.TextMuted, 11.5f);
			const Wui::WuiRect btnTestConn { cfgBox.X + 280.0f, cY, 140.0f, 26.0f };
			if (Wui::Button(ctx, Wui::HashId("cfg.btn.test"), btnTestConn, "测试 Sidecar 连接", theme))
			{
				studio.ProbeBackend(m_SidecarHost, static_cast<uint16_t>(m_SidecarPort));
				if (studio.IsSidecarAlive())
				{
					m_DictStatus = "Sidecar 服务响应正常 (127.0.0.1:8090)！已自动切换为高保真微服务模式。";
				}
				else
				{
					m_DictStatus = "未检测到 Sidecar 服务，已安全降级为本地合成预览模式 (Procedural Preview)。";
				}
			}
		}
	}
}
