#pragma once

#include "WUI/Common/EditorPanel.h"
#include "World/Audio/SoundEvent.h"
#include "World/Audio/AudioTypes.h"
#include <string>
#include <vector>

namespace World
{
	class SoundEventEditorPanel final : public EditorPanel
	{
	public:
		const char* Id() const override { return "sound_event_editor"; }
		const char* Title() const override { return "声效事件编辑器 (Sound Event Editor)"; }

		void OnRender(Wui::WuiContext& ctx, const Wui::WuiRect& rect, PanelHost& host) override;

		static void RequestOpenEvent(const std::string& path);
		void LoadEvent(const std::string& path);
		void SaveEvent();

		const std::string& GetLoadedPath() const noexcept { return m_LoadedPath; }
		const Audio::SoundEvent& GetEvent() const noexcept { return m_Event; }

	private:
		void RefreshAvailableFiles();
		void EnsureBuffersForEvent();
		void PlayAudition();
		void StopAudition();

		static std::string s_PendingOpenPath;
		std::string m_LoadedPath;
		Audio::SoundEvent m_Event;
		std::string m_StatusFeedback;
		std::vector<std::string> m_AvailableSoundFiles;

		// Slice asset selector for variations
		std::vector<std::string> m_AvailableSliceAssets;
		int m_SelectedSliceAssetIndex { 0 };

		int m_SelectedBusIndex { 2 }; // Default SFX
		float m_MinPitchSemi { -1.5f };
		float m_MaxPitchSemi { 1.5f };
		float m_MinVolume { 0.85f };
		float m_MaxVolume { 1.0f };
		float m_MinDist { 1.0f };
		float m_MaxDist { 25.0f };
		bool m_Is3D { true };

		int m_SelectedContainerType { 1 }; // 0: Single, 1: Random, 2: Sequence, 3: Switch, 4: Blend, 5: Layer
		int m_MaxInstances { 8 };
		float m_CooldownSeconds { 0.05f };
		int m_StealPolicyIndex { 0 }; // 0: StealOldest, 1: StealQuiet, 2: StealReject
		int m_SpatialModeIndex { 1 }; // 0: 2D Pan, 1: 3D Spatial, 2: None

		Audio::VoiceHandle m_CurrentAuditionVoice { Audio::kInvalidVoiceHandle };
	};
}
