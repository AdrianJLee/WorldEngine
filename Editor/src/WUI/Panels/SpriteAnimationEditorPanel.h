#pragma once

#include "WUI/Common/EditorPanel.h"
#include "World/Renderer/SpriteAnimation.h"
#include "World/Renderer/TextureAtlas.h"
#include "World/RHI/Rhi.h"
#include <string>
#include <vector>
#include <memory>
#include <chrono>

namespace World
{
	class SpriteAnimationEditorPanel final : public EditorPanel
	{
	public:
		explicit SpriteAnimationEditorPanel(std::string logicalPath);
		~SpriteAnimationEditorPanel() override = default;

		const char* Id() const override { return m_PanelId.c_str(); }
		const char* Title() const override { return m_PanelTitle.c_str(); }

		void OnRender(Wui::WuiContext& ctx, const Wui::WuiRect& rect, PanelHost& host) override;

		const std::string& LogicalPath() const noexcept { return m_LogicalPath; }
		bool HasUnsavedChanges() const noexcept { return m_Dirty; }

		bool Reload(std::string* message = nullptr);
		bool Save(std::string* message = nullptr);

		void Play();
		void Stop();
		void Reset();
		void PrevFrame();
		void NextFrame();

		void AddFrame(const std::string& subTexture, float duration = 0.0833f, const std::string& eventTag = "");
		bool DeleteFrame(size_t index);

		std::shared_ptr<SpriteAnimation> GetAnimation() const noexcept { return m_Animation; }

	private:
		void SyncFromSelectedFrame();
		void CommitToSelectedFrame();
		std::filesystem::path ResolvePhysicalPath() const;

		std::string m_LogicalPath;
		std::string m_PanelId;
		std::string m_PanelTitle;

		std::shared_ptr<SpriteAnimation> m_Animation;
		bool m_IsPlaying { false };
		float m_PlayheadTime { 0.0f };
		int m_SelectedFrameIndex { 0 };
		uint32_t m_ActiveSampledFrame { 0 };
		bool m_Dirty { false };
		std::string m_StatusFeedback;

		std::chrono::steady_clock::time_point m_LastFrameTime {};

		float m_FrameRate { 12.0f };
		AnimationLoopMode m_LoopMode { AnimationLoopMode::Loop };

		std::string m_EditFrameSub;
		float m_EditFrameDuration { 0.0833f };
		std::string m_EditFrameEvent;

		float m_ListScrollY { 0.0f };
		uint64_t m_UiTextureId { 0 };
		Rhi::Handle<Rhi::Texture> m_GpuTexture { nullptr };
		std::shared_ptr<TextureAtlas> m_Atlas { nullptr };
		std::string m_LoadedAtlasPath;
		void EnsureAnimationAtlasLoaded();
	};
}
