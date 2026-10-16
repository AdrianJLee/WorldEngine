#pragma once

#include "WUI/Common/EditorPanel.h"
#include "World/Physics/PhysicsMaterial.h"
#include <string>
#include <vector>

namespace World
{
	class PhysicsMaterialEditorPanel final : public EditorPanel
	{
	public:
		const char* Id() const override { return "physics_material_editor"; }
		const char* Title() const override { return "物理材质编辑器 (Physics Material Editor)"; }

		void OnRender(Wui::WuiContext& ctx, const Wui::WuiRect& rect, PanelHost& host) override;
		bool OnShortcut(uint32_t keyCode, bool ctrl, bool shift, bool alt) override;

		static void RequestOpenAsset(const std::string& path);
		void LoadAsset(const std::string& path);
		void SaveAsset();

		const std::string& GetLoadedPath() const noexcept { return m_LoadedPath; }
		const PhysicsMaterial& GetMaterial() const noexcept { return m_Material; }
		bool IsDirty() const noexcept { return m_Dirty; }

	private:
		void RefreshAvailableAssets();
		void ApplyPreset(const std::string& name);

		static std::string s_PendingOpenPath;
		std::string m_LoadedPath;
		PhysicsMaterial m_Material;
		bool m_Dirty = false;
		std::string m_StatusFeedback;
		std::vector<std::string> m_AvailableAssets;

		// 碰撞模拟测试 (Surface B)
		float m_OtherFriction = 0.5f;
		float m_OtherRestitution = 0.0f;
		int m_OtherSurfacePreset = 0; // 0: Default, 1: Ice, 2: Rubber
	};
}
