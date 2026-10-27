#pragma once

#include "WUI/Common/EditorPanel.h"
#include "World/Audio/AiAudioStudio.h"
#include <string>
#include <vector>

namespace World
{
	class AiAudioStudioPanel final : public EditorPanel
	{
	public:
		const char* Id() const override { return "ai_audio_studio"; }
		const char* Title() const override { return "AI Audio Studio"; }

		void OnRender(Wui::WuiContext& ctx, const Wui::WuiRect& rect, PanelHost& host) override;

	private:
		int m_ActiveTab { 0 }; // 0: Dialogue, 1: SFX, 2: BGM Stems & Loop, 3: Phoneme Dict & Config

		// Tab 0: Dialogue & Lip-Sync
		int m_SelectedTimbreIndex { 0 };
		int m_SelectedEmotionIndex { 0 };
		float m_EmotionIntensity { 0.7f };
		std::string m_DialogueText { "为了{重|chóng}返王国的荣耀，夺回{法尔斯|fǎ ěr sī}之心！[Angry]" };
		std::string m_StatusFeedback;
		int m_LastPhonemeCount { 0 };

		// Tab 1: AI SFX Generator (6.2.1)
		std::string m_SfxPrompt { "heavy stone door grinding open, medieval dungeon" };
		float m_SfxDuration { 1.5f };
		int m_SfxVariationCount { 4 };
		bool m_SfxTrimSilence { true };
		bool m_SfxZeroCrossing { true };
		bool m_SfxNormalizeLufs { true };
		std::vector<Audio::SfxVariation> m_SfxCandidates;
		int m_SelectedCandidateIndex { -1 };
		std::string m_SfxStatus;

		// Tab 2: BGM Loops & Stems (6.2.2)
		std::string m_BgmAssetPath { "projects/assets/audio/music/battle_theme.waudio" };
		float m_BgmLoopStartSec { 2.0f };
		float m_BgmLoopEndSec { 28.5f };
		float m_BgmStemAmbientVol { 0.8f };
		float m_BgmStemPercussionVol { 1.0f };
		float m_BgmStemLeadVol { 0.75f };
		std::string m_BgmStatus;

		// Tab 3: Phoneme Dict & Config (6.1.2 & 6.3)
		std::string m_NewWord { "重创" };
		std::string m_NewPinyin { "zhòng chuāng" };
		std::string m_SidecarHost { "127.0.0.1" };
		int m_SidecarPort { 8090 };
		std::string m_DictStatus;
	};
}
