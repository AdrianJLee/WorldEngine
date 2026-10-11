#pragma once

#include "WUI/Common/EditorPanel.h"
#include <string>

namespace World
{
	class AiAudioStudioPanel final : public EditorPanel
	{
	public:
		const char* Id() const override { return "ai_audio_studio"; }
		const char* Title() const override { return "AI Audio Studio"; }

		void OnRender(Wui::WuiContext& ctx, const Wui::WuiRect& rect, PanelHost& host) override;

	private:
		int m_SelectedTimbreIndex { 0 };
		int m_SelectedEmotionIndex { 0 };
		float m_EmotionIntensity { 0.7f };
		std::string m_DialogueText { "为了王国的荣耀，全军出击！" };
		std::string m_StatusFeedback;
		int m_LastPhonemeCount { 0 };

		// SFX & Music fields
		std::string m_SfxPrompt { "sci-fi laser cannon blast" };
		std::string m_MusicPrompt { "epic fantasy orchestral battle" };
		float m_MusicBpm { 128.0f };
	};
}
