#pragma once

#include "WUI/Common/EditorPanel.h"
#include "World/Audio/AudioTypes.h"
#include <vector>
#include <string>

namespace World
{
	class AudioProfilerPanel : public EditorPanel
	{
	public:
		AudioProfilerPanel() = default;
		~AudioProfilerPanel() override = default;

		const char* Id() const override { return "audio_profiler"; }
		const char* Title() const override { return "音频分析器 (Audio Profiler)"; }
		void OnRender(Wui::WuiContext& ctx, const Wui::WuiRect& rect, PanelHost& host) override;

	private:
		void RenderTopMetricCards(Wui::WuiContext& ctx, const Wui::WuiRect& rect, PanelHost& host);
		void RenderFftSpectrum(Wui::WuiContext& ctx, const Wui::WuiRect& rect, PanelHost& host);
		void RenderActiveVoicesTable(Wui::WuiContext& ctx, const Wui::WuiRect& rect, PanelHost& host);

		std::vector<float> m_SpectrumBands;
		std::vector<float> m_PeakHoldBands;
		std::vector<float> m_PeakHoldTimers;
		uint32_t m_MaxDiscoveredVoiceHandle { 0 };
		int m_TableScrollOffset { 0 };
	};
}
