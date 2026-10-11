#pragma once

#include "WUI/Common/EditorPanel.h"

namespace World
{
	class AudioMixerPanel final : public EditorPanel
	{
	public:
		const char* Id() const override { return "audio_mixer"; }
		const char* Title() const override { return "Audio Mixer"; }

		void OnRender(Wui::WuiContext& ctx, const Wui::WuiRect& rect, PanelHost& host) override;

	private:
		int m_SelectedBusIndex { 0 };
	};
}
