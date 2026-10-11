#pragma once

#include "WUI/Common/EditorPanel.h"
#include "World/Audio/SoundEvent.h"
#include <string>

namespace World
{
	class SoundEventEditorPanel final : public EditorPanel
	{
	public:
		const char* Id() const override { return "sound_event_editor"; }
		const char* Title() const override { return "Sound Event Editor"; }

		void OnRender(Wui::WuiContext& ctx, const Wui::WuiRect& rect, PanelHost& host) override;

		static void RequestOpenEvent(const std::string& path);

	private:
		void LoadEvent(const std::string& path);
		void SaveEvent();

		static std::string s_PendingOpenPath;
		std::string m_LoadedPath;
		Audio::SoundEvent m_Event;
		std::string m_StatusFeedback;
		int m_SelectedBusIndex { 2 }; // Default SFX
		int m_SelectedContainerType { 1 }; // Default Random
		float m_MinPitchSemi { -1.5f };
		float m_MaxPitchSemi { 1.5f };
		float m_MinVolume { 0.85f };
		float m_MaxVolume { 1.0f };
		float m_MinDist { 1.0f };
		float m_MaxDist { 25.0f };
		bool m_Is3D { true };
	};
}
