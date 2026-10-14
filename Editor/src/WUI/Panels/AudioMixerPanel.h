#pragma once

#include "WUI/Common/EditorPanel.h"
#include "World/Audio/AudioTypes.h"
#include "World/Audio/SoundEvent.h"
#include "World/Audio/AiAudioStudio.h"
#include <string>
#include <vector>
#include <memory>

namespace World::Audio
{
	class AudioBuffer;
}

namespace World
{
	enum class AudioItemKind : uint8_t
	{
		SoundEvent = 0,
		AudioSlice = 1,
		MixerConfig = 2,
		VoiceProfile = 3
	};

	struct AudioLibraryItem
	{
		std::string displayName; // 左侧列表显示名称，如 footstep_player.wsound, footstep_grass.waudio
		std::string id;          // 事件 ID 或切片标识（如 footstep_player）
		std::string path;        // 真实物理文件路径或规范路径
		AudioItemKind kind { AudioItemKind::SoundEvent };
	};

	class AudioMixerPanel final : public EditorPanel
	{
	public:
		static void RequestOpenAsset(const std::string& path);

		const char* Id() const override { return "audio_mixer"; }
		const char* Title() const override { return "音频管理器 (Audio Manager)"; }

		void OnRender(Wui::WuiContext& ctx, const Wui::WuiRect& rect, PanelHost& host) override;

	private:
		static std::string s_PendingOpenAsset;
		void RefreshLibrary();
		void ImportWavAsset(const std::string& wavPath);
		void SelectItem(int index);
		void PlayCurrent();
		void PauseCurrent();
		void StopCurrent();
		void SaveCurrentEvent();

		void RenderLeftLibrary(Wui::WuiContext& ctx, const Wui::WuiRect& rect, PanelHost& host);
		void RenderRightInspector(Wui::WuiContext& ctx, const Wui::WuiRect& rect, PanelHost& host);
		void RenderGlobalVolume(Wui::WuiContext& ctx, const Wui::WuiRect& rect, PanelHost& host);
		void RenderProfilerHud(Wui::WuiContext& ctx, const Wui::WuiRect& rect, PanelHost& host);
		std::shared_ptr<Audio::AudioBuffer> GetSelectedBuffer();

		// Library state
		std::vector<AudioLibraryItem> m_Items;
		std::string m_SearchFilter;
		int m_SelectedIndex { -1 };
		float m_LibraryScrollY { 0.0f };

		// Available slice assets for variation selection
		std::vector<std::string> m_AvailableSliceAssets;
		int m_SelectedSliceAssetIndex { 0 };

		// Selected item state
		Audio::SoundEvent m_SelectedEvent;
		Audio::TimbreProfile m_SelectedTimbre;
		std::string m_LoadedMixerYaml;
		void RenderVoiceInspector(Wui::WuiContext& ctx, const Wui::WuiRect& rect, PanelHost& host, const AudioLibraryItem& item);
		void RenderMixerInspector(Wui::WuiContext& ctx, const Wui::WuiRect& rect, PanelHost& host, const AudioLibraryItem& item);
		bool m_IsEventLoaded { false };
		std::string m_LoadedEventPath;
		std::string m_ImportWavInput { "projects/assets/audio/sfx/sample.wav" };

		// Playback / Transport state
		Audio::VoiceHandle m_CurrentVoice { Audio::kInvalidVoiceHandle };
		bool m_IsPlaying { false };
		bool m_IsPaused { false };
		bool m_Looping { false };
		float m_PlaybackTime { 0.0f };
		float m_Duration { 1.8f };

		// Sound properties
		int m_SelectedBus { 2 };       // 0: Master, 1: Music, 2: SFX, 3: Voice, 4: UI
		float m_Volume { 1.0f };       // 0.0f ~ 1.5f (0% ~ 150%)
		float m_Pitch { 1.0f };        // 0.5f ~ 2.0f
		bool m_Spatial3D { false };
		float m_MinDistance { 1.0f };  // 0.1m ~ 10m
		float m_MaxDistance { 25.0f }; // 5m ~ 100m
		float m_PitchRandomSemi { 1.5f }; // ±1.5 semitones

		// Global volume bus controls (5 buses)
		float m_BusVolumes[6] { 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f };
		float m_BusReverbSend[6] { 0.0f, 0.0f, 0.2f, 0.1f, 0.0f, 0.3f };
		bool m_BusMuted[6] { false, false, false, false, false, false };
		bool m_GlobalBusesSynced { false };

		std::string m_StatusNotice;

		// Profiler & Zero-Crossing state
		std::vector<float> m_SpectrumBands;
		uint32_t m_SnappedLoopStart { 0 };
		uint32_t m_SnappedLoopEnd { 0 };
		bool m_HasSnappedLoop { false };
	};
}
