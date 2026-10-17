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
		const char* Title() const override { return "Curve Editor"; }
		void OnRender(Wui::WuiContext& ctx, const Wui::WuiRect& rect, PanelHost& host) override;

		const CubicBezierCurve& GetCurve() const { return m_Curve; }
		CubicBezierCurve& GetCurveMut() { return m_Curve; }
		void SetCurve(const CubicBezierCurve& curve) { m_Curve = curve; }

	private:
		CubicBezierCurve m_Curve;
		int m_SelectedPreset = 3; // EaseInOut
		int m_SelectedKeyframeIndex = 0;
		float m_PreviewT = 0.5f;
		float m_EvaluatedValue = 0.5f;
		std::vector<std::string> m_PresetOptions;
	};
}
