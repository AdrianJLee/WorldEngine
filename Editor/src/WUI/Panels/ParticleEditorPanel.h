#pragma once

#include "WUI/Common/EditorPanel.h"
#include "World/Renderer/ParticleAsset.h"
#include "World/Renderer/ParticleSystem.h"

#include <glm/glm.hpp>
#include <string>
#include <vector>
#include <memory>

namespace World
{
	enum class ParticleEditorViewMode : uint8_t
	{
		Visual = 0,
		Code = 1
	};

	class ParticleEditorPanel final : public EditorPanel
	{
	public:
		ParticleEditorPanel();
		~ParticleEditorPanel() override;

		const char* Id() const override { return "particle_editor"; }
		const char* Title() const override { return "视觉特效编辑器 (VFX System Editor)"; }

		void OnRender(Wui::WuiContext& ctx, const Wui::WuiRect& rect, PanelHost& host) override;
		bool OnShortcut(uint32_t keyCode, bool ctrl, bool shift, bool alt) override;

		static void RequestOpenAsset(const std::string& path);
		void LoadAsset(const std::string& path);
		void SaveAsset();
		void NewAsset();

		void ApplyCodeToAsset();
		void SyncCodeFromAsset();

		const std::string& GetLoadedPath() const noexcept { return m_LoadedPath; }
		const ParticleAsset& GetAsset() const noexcept { return m_Asset; }
		bool IsDirty() const noexcept { return m_Dirty; }
		ParticleEditorViewMode GetViewMode() const noexcept { return m_ViewMode; }

	private:
		void RefreshAvailableAssets();
		void SyncSimulationFromAsset();
		void StepSimulation(float dt);

		static std::string s_PendingOpenPath;
		std::string m_LoadedPath;
		ParticleAsset m_Asset;
		bool m_Dirty = false;
		std::string m_StatusFeedback;
		std::vector<std::string> m_AvailableAssets;
		size_t m_SelectedEmitterIndex = 0;

		ParticleEditorViewMode m_ViewMode = ParticleEditorViewMode::Visual;
		std::string m_CodeBuffer;
		float m_CodeScrollY = 0.0f;
		int m_SelectedCodeLine = -1;

		// 3D 自由轨道相机状态
		float m_CameraDistance = 6.0f;
		float m_OrbitYaw = 0.785f;    // ~45 度
		float m_OrbitPitch = 0.45f;   // ~25 度
		glm::vec3 m_CameraFocus { 0.0f, 1.0f, 0.0f };
		bool m_IsOrbiting = false;
		glm::vec2 m_LastMousePos { 0.0f };

		// 走带控制状态机
		bool m_IsPlaying = true;
		float m_PlaybackSpeed = 1.0f;
		float m_SimTime = 0.0f;

		// 内部实时物理推演池 (Live Simulation Pools)
		std::vector<std::unique_ptr<ParticlePool>> m_LivePools;

		// 7 大模块卡片折叠状态 (对齐方案 2.4 M4 规范)
		bool m_CardSpawningExpanded = true;
		bool m_CardMaterialExpanded = true;
		bool m_CardMotionExpanded = true;
		bool m_CardSizeRotationExpanded = true;
		bool m_CardColorAlphaExpanded = true;
		bool m_CardCollisionExpanded = true;
		bool m_CardSubEmittersExpanded = true;
	};
}
