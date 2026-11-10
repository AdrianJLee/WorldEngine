#pragma once

#include "WUI/Common/EditorPanel.h"
#include "World/Audio/SoundEvent.h"
#include "World/Audio/AudioTypes.h"
#include "World/WUI/WuiCodeEditor.h"
#include "World/WUI/WuiTextBuffer.h"

#include <string>
#include <vector>

namespace World
{
	enum class SoundEventViewMode : uint8_t
	{
		Visual = 0,
		Code = 1
	};

	class SoundEventEditorPanel final : public EditorPanel
	{
	public:
		const char* Id() const override { return "sound_event_editor"; }
		const char* Title() const override { return "声效事件工作台 (Sound Event Studio)"; }

		void OnRender(Wui::WuiContext& ctx, const Wui::WuiRect& rect, PanelHost& host) override;
		bool OnShortcut(uint32_t keyCode, bool ctrl, bool shift, bool alt) override;

		static void RequestOpenEvent(const std::string& path);
		void LoadEvent(const std::string& path);
		void SaveEvent();
		void NewEvent();

		const std::string& GetLoadedPath() const noexcept { return m_LoadedPath; }
		const Audio::SoundEvent& GetEvent() const noexcept { return m_Event; }

	private:
		void RefreshAvailableFiles();
		void EnsureBuffersForEvent();
		void PlayAudition();
		void StopAudition();
		void ApplyCodeToEvent();
		void SyncCodeFromEvent();

		void RenderSidebar(Wui::WuiContext& ctx, const Wui::WuiRect& rect, const Wui::WuiTheme& theme, PanelHost& host);
		void RenderTopBar(Wui::WuiContext& ctx, const Wui::WuiRect& rect, const Wui::WuiTheme& theme, PanelHost& host);
		void RenderVisualInspector(Wui::WuiContext& ctx, const Wui::WuiRect& rect, const Wui::WuiTheme& theme, PanelHost& host);
		void RenderCodeView(Wui::WuiContext& ctx, const Wui::WuiRect& rect, const Wui::WuiTheme& theme, PanelHost& host);

		static std::string s_PendingOpenPath;
		std::string m_LoadedPath;
		Audio::SoundEvent m_Event;
		std::string m_StatusFeedback;
		bool m_StatusIsError { false };
		bool m_Dirty { false };

		// View Mode (Visual vs YAML Code)
		SoundEventViewMode m_ViewMode { SoundEventViewMode::Visual };
		int m_SelectedViewModeIndex { 0 };

		// Sidebar & Layout
		float m_SidebarWidth { 230.0f };
		std::string m_SearchFilter;
		std::vector<std::string> m_AvailableSoundFiles;

		// Code Editor
		Wui::WuiTextBuffer m_CodeBuffer;
		Wui::WuiCodeEditorOptions m_CodeOptions;
		std::string m_CodeStatusFeedback;
		bool m_CodeStatusIsError { false };
		bool m_CodeInitialized { false };

		// Available audio slices for picker
		std::vector<std::string> m_AvailableSliceAssets;
		int m_SelectedSliceAssetIndex { 0 };

		// Temporary input buffers for dynamic containers
		std::string m_EventIdBuffer;
		std::string m_NewBranchName { "Default" };
		std::string m_NewBranchPath { "assets/audio/sfx/footstep_grass.waudio" };
		float m_NewBlendThreshold { 1000.0f };
		std::string m_NewBlendPath { "assets/audio/sfx/ui_click.waudio" };
		float m_NewLayerVol { 1.0f };
		std::string m_NewLayerPath { "assets/audio/sfx/impact_metal.waudio" };

		// Visual Properties State
		int m_SelectedBusIndex { 2 }; // Default SFX
		float m_MinPitchSemi { -1.5f };
		float m_MaxPitchSemi { 1.5f };
		float m_MinVolume { 0.85f };
		float m_MaxVolume { 1.0f };
		float m_MinDist { 1.0f };
		float m_MaxDist { 25.0f };
		bool m_Is3D { true };
		int m_AttenuationModelIndex { 0 }; // 0: InverseSquare, 1: Linear, 2: Logarithmic

		int m_SelectedContainerType { 1 }; // 0: Single, 1: Random, 2: Sequence, 3: Switch, 4: Blend, 5: Layer
		bool m_ShuffleNoRepeat { true };
		int m_MaxInstances { 8 };
		float m_CooldownSeconds { 0.05f };
		int m_StealPolicyIndex { 0 }; // 0: StealOldest, 1: StealQuiet, 2: StealReject
		int m_SpatialModeIndex { 1 }; // 0: 2D Pan, 1: 3D Spatial, 2: None

		Audio::VoiceHandle m_CurrentAuditionVoice { Audio::kInvalidVoiceHandle };
	};
}
