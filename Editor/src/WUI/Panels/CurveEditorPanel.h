#pragma once

#include "WUI/Common/EditorPanel.h"
#include "World/Math/Curve/CubicBezierCurve.h"
#include <vector>
#include <string>

namespace World
{
	class CurveEditorPanel final : public EditorPanel
	{
	public:
		CurveEditorPanel();
		~CurveEditorPanel() override = default;

		const char* Id() const override { return "curve_editor"; }
		const char* Title() const override { return m_Title.c_str(); }
		void OnRender(Wui::WuiContext& ctx, const Wui::WuiRect& rect, PanelHost& host) override;
		bool OnShortcut(uint32_t keyCode, bool ctrl, bool shift, bool alt) override;

		const CubicBezierCurve& GetCurve() const { return m_Curve; }
		CubicBezierCurve& GetCurveMut() { return m_Curve; }
		void SetCurve(const CubicBezierCurve& curve);

		// 文档与资产接口
		void LoadAsset(const std::string& logicalPath);
		bool SaveAsset();
		void Revert();
		void FrameAll();

		bool IsDirty() const { return m_IsDirty; }
		const std::string& GetLoadedPath() const { return m_LoadedPath; }

		// 一键切线预设
		void SetSelectedTangentFlat();
		void SetSelectedTangentLinear();
		void SetSelectedTangentSmooth();
		void SetSelectedTangentStep();

	private:
		std::filesystem::path ResolvePhysicalPath(const std::string& path) const;
		void UpdateTitle();
		float TimeToScreenX(float t, const Wui::WuiRect& r) const;
		float ValueToScreenY(float v, const Wui::WuiRect& r) const;
		float ScreenXToTime(float sx, const Wui::WuiRect& r) const;
		float ScreenYToValue(float sy, const Wui::WuiRect& r) const;

	private:
		CubicBezierCurve m_Curve;
		int m_SelectedKeyframeIndex = 0;
		float m_PreviewT = 0.5f;
		float m_EvaluatedValue = 0.5f;
		std::vector<std::string> m_TangentModeComboOptions;
		std::vector<std::string> m_ExtrapComboOptions;

		// 视口平移与缩放范围
		float m_ViewMinTime = -0.2f;
		float m_ViewMaxTime = 1.2f;
		float m_ViewMinValue = -0.2f;
		float m_ViewMaxValue = 1.2f;

		// 交互与悬停状态
		int m_HoveredKeyframeIndex = -1;
		bool m_HoveredInHandle = false;
		bool m_HoveredOutHandle = false;
		bool m_SnapToGrid = false;
		float m_SnapTimeStep = 0.05f;
		float m_SnapValStep = 0.05f;

		// 动态走带播放
		bool m_IsPlaying = false;
		float m_PlaySpeed = 1.0f;

		// 平移与拖拽
		bool m_IsPanning = false;
		glm::vec2 m_PanStartMouse { 0.0f, 0.0f };
		float m_PanStartMinTime = 0.0f;
		float m_PanStartMaxTime = 0.0f;
		float m_PanStartMinValue = 0.0f;
		float m_PanStartMaxValue = 0.0f;

		bool m_IsDraggingKeyframe = false;
		bool m_IsDraggingInHandle = false;
		bool m_IsDraggingOutHandle = false;
		glm::vec2 m_DragStartMouse { 0.0f, 0.0f };
		float m_DragKeyframeStartTime = 0.0f;
		float m_DragKeyframeStartVal = 0.0f;

		// 右键上下文菜单状态
		glm::vec2 m_ContextMenuPos { 0.0f, 0.0f };
		float m_ContextMenuTime = 0.0f;
		float m_ContextMenuValue = 0.0f;
		bool m_ContextMenuOnKeyframe = false;

		// 文档路径与标题状态
		std::string m_LoadedPath;
		std::string m_Title = "Curve Editor - [Untitled]";
		std::string m_StatusFeedback;
		bool m_IsDirty = false;
	};
}
