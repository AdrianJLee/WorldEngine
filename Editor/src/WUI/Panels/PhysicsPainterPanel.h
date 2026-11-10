#pragma once

#include "WUI/Common/EditorPanel.h"
#include "World/Scene/Entity.h"
#include "World/Scene/Scene.h"

#include <glm/glm.hpp>
#include <string>
#include <vector>

namespace World
{
	class TerrainData;

	class PhysicsPainterPanel final : public EditorPanel
	{
	public:
		PhysicsPainterPanel();
		explicit PhysicsPainterPanel(PanelHost& host);
		~PhysicsPainterPanel() override = default;

		const char* Id() const override { return "physicspainter"; }
		const char* Title() const override { return "物理画笔 (Physics Painter & Scatter)"; }

		void OnRender(Wui::WuiContext& ctx, const Wui::WuiRect& rect, PanelHost& host) override;

		// 视口联动静态查询与控制接口
		static bool IsActive() { return s_Active; }
		static void SetActive(bool active) { s_Active = active; }
		static void ToggleActive() { s_Active = !s_Active; }

		static float GetBrushRadius() { return s_BrushRadius; }
		static void SetBrushRadius(float r) { s_BrushRadius = std::clamp(r, 0.5f, 50.0f); }

		static int GetBrushDensity() { return s_BrushDensity; }
		static void SetBrushDensity(int d) { s_BrushDensity = std::clamp(d, 1, 30); }

		static bool GetRandomYaw() { return s_RandomYaw; }
		static bool GetAlignToNormal() { return s_AlignToNormal; }
		static float GetMaxSlope() { return s_MaxSlope; }

		// 视口拾取射线投射命中后触发单次物理散布
		static void PaintAt(Scene& scene, const glm::vec3& center, const glm::vec3& normal, PanelHost& host);

	private:
		// 静态画笔状态 (供视口每帧投射圈与快捷交互共享)
		static inline bool s_Active { false };
		static inline float s_BrushRadius { 4.0f };
		static inline int s_BrushDensity { 5 };
		static inline bool s_RandomYaw { true };
		static inline bool s_AlignToNormal { true };
		static inline float s_MaxSlope { 45.0f };
		static inline float s_ScaleMin { 0.8f };
		static inline float s_ScaleMax { 1.25f };
		static inline int s_PresetModelIdx { 0 }; // 0: 预设岩石 (Rock), 1: 预设箱体 (Crate), 2: 预设立柱 (Pillar), 3: 自定义资产
		static inline std::string s_CustomModelPath;
		static inline std::string s_StatusMessage;
		static inline size_t s_TotalPaintedCount { 0 };
	};
}
