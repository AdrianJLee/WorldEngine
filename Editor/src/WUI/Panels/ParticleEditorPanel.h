#pragma once

#include "WUI/Common/EditorPanel.h"
#include "World/Renderer/ParticleAsset.h"
#include <string>
#include <vector>

namespace World
{
	class ParticleEditorPanel final : public EditorPanel
	{
	public:
		const char* Id() const override { return "particle_editor"; }
		const char* Title() const override { return "粒子系统编辑器 (Particle System Editor)"; }

		void OnRender(Wui::WuiContext& ctx, const Wui::WuiRect& rect, PanelHost& host) override;
		bool OnShortcut(uint32_t keyCode, bool ctrl, bool shift, bool alt) override;

		static void RequestOpenAsset(const std::string& path);
		void LoadAsset(const std::string& path);
		void SaveAsset();
		void NewAsset();

		const std::string& GetLoadedPath() const noexcept { return m_LoadedPath; }
		const ParticleAsset& GetAsset() const noexcept { return m_Asset; }
		bool IsDirty() const noexcept { return m_Dirty; }

	private:
		void RefreshAvailableAssets();

		static std::string s_PendingOpenPath;
		std::string m_LoadedPath;
		ParticleAsset m_Asset;
		bool m_Dirty = false;
		std::string m_StatusFeedback;
		std::vector<std::string> m_AvailableAssets;
		size_t m_SelectedEmitterIndex = 0;

		// 走带控制状态
		bool m_IsPlaying = true;
		float m_PlaybackSpeed = 1.0f;
		float m_TimelineScrub = 0.0f;

		// 模块卡片折叠状态
		bool m_CardSpawningExpanded = true;
		bool m_CardMaterialExpanded = true;
		bool m_CardMotionExpanded = true;
		bool m_CardColorExpanded = true;
		bool m_CardCollisionExpanded = true;
	};
}
