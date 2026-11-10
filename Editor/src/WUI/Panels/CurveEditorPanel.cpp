#include "wldpch.h"
#include "WUI/Panels/CurveEditorPanel.h"
#include "World/WUI/WuiWidgets.h"
#include "World/WUI/Widgets/WuiChrome.h"
#include "World/Core/KeyCodes.h"
#include "World/Utils/Paths.h"
#include "World/Core/Log.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <filesystem>

namespace World
{
	static bool SaveCurveToFile(const std::string& filepath, const CubicBezierCurve& curve)
	{
		return curve.SaveToFile(filepath);
	}

	static bool LoadCurveFromFile(const std::string& filepath, CubicBezierCurve& outCurve)
	{
		return outCurve.LoadFromFile(filepath);
	}

	static bool ClipLineSegment2D(glm::vec2& p0, glm::vec2& p1, float minX, float maxX, float minY, float maxY)
	{
		float t0 = 0.0f, t1 = 1.0f;
		const float dx = p1.x - p0.x;
		const float dy = p1.y - p0.y;

		const auto clipTest = [&](float p, float q) -> bool
		{
			if (p < 0.0f)
			{
				const float r = q / p;
				if (r > t1) return false;
				if (r > t0) t0 = r;
			}
			else if (p > 0.0f)
			{
				const float r = q / p;
				if (r < t0) return false;
				if (r < t1) t1 = r;
			}
			else if (q < 0.0f)
			{
				return false;
			}
			return true;
		};

		if (clipTest(-dx, p0.x - minX) &&
			clipTest(dx, maxX - p0.x) &&
			clipTest(-dy, p0.y - minY) &&
			clipTest(dy, maxY - p0.y))
		{
			if (t1 < 1.0f)
			{
				p1.x = p0.x + t1 * dx;
				p1.y = p0.y + t1 * dy;
			}
			if (t0 > 0.0f)
			{
				p0.x = p0.x + t0 * dx;
				p0.y = p0.y + t0 * dy;
			}
			return true;
		}
		return false;
	}

	static void DrawClippedLine(Wui::WuiContext& ctx, glm::vec2 p0, glm::vec2 p1, const Wui::WuiRect& bounds, const Wui::WuiColor& color, float thickness)
	{
		if (ClipLineSegment2D(p0, p1, bounds.X, bounds.X + bounds.W, bounds.Y, bounds.Y + bounds.H))
		{
			Wui::LineSegment(ctx, p0, p1, color, thickness);
		}
	}

	CurveEditorPanel::CurveEditorPanel()
		: m_TangentModeComboOptions({ "Aligned (平滑对齐)", "Free (自由切线)", "Clamped (极值平直)", "Broken (独立折线)" })
		, m_ExtrapComboOptions({ "Clamp (边界保持)", "Cycle (循环)", "PingPong (往返)", "Linear (延伸)" })
	{
		m_Curve = CubicBezierCurve::EaseInOut();
		UpdateTitle();
	}

	void CurveEditorPanel::SetCurve(const CubicBezierCurve& curve)
	{
		m_Curve = curve;
		m_SelectedKeyframeIndex = 0;
		m_IsDirty = true;
		UpdateTitle();
		FrameAll();
	}

	std::filesystem::path CurveEditorPanel::ResolvePhysicalPath(const std::string& path) const
	{
		std::string normalized = path;
		std::replace(normalized.begin(), normalized.end(), '\\', '/');
		if (normalized.empty())
			normalized = "curves/new_curve.wcurve";

		std::string logical = normalized;
		if (logical.rfind("assets/", 0) == 0 || logical.rfind("assets\\", 0) == 0)
			logical = logical.substr(7);

		std::filesystem::path p(normalized);
		if (p.is_absolute() && std::filesystem::exists(p))
			return p;

		// 资产内容根优先：统一保存在 <ProjectRoot>/assets/curves 之下
		std::filesystem::path fromAsset = Paths::AssetRoot() / logical;
		if (std::filesystem::exists(fromAsset))
			return fromAsset;

		// 历史兼容兜底（老项目根下遗留文件）
		std::filesystem::path fromProj = Paths::ProjectDir() / logical;
		if (std::filesystem::exists(fromProj))
			return fromProj;

		// 默认落盘位置：必须在资产内容根目录内部
		return fromAsset;
	}

	void CurveEditorPanel::LoadAsset(const std::string& logicalPath)
	{
		std::filesystem::path fullPath = ResolvePhysicalPath(logicalPath);

		CubicBezierCurve loaded;
		if (LoadCurveFromFile(fullPath.string(), loaded))
		{
			m_Curve = loaded;
			std::error_code ec;
			std::filesystem::path rel = std::filesystem::relative(fullPath, Paths::AssetRoot(), ec);
			if (!ec && !rel.empty() && rel.string().find("..") == std::string::npos)
				m_LoadedPath = rel.generic_string();
			else
				m_LoadedPath = logicalPath;

			m_IsDirty = false;
			m_SelectedKeyframeIndex = 0;
			m_StatusFeedback = "Loaded: " + m_LoadedPath;
			UpdateTitle();
			FrameAll();
		}
		else
		{
			m_StatusFeedback = "Failed to load: " + logicalPath;
		}
	}

	bool CurveEditorPanel::SaveAsset()
	{
		if (m_LoadedPath.empty())
		{
			m_LoadedPath = "curves/new_curve.wcurve";
		}

		std::filesystem::path fullPath = ResolvePhysicalPath(m_LoadedPath);
		// 严禁写到项目根目录！必须确保保存在资产内容根 (Paths::AssetRoot()) 之下
		if (!fullPath.is_absolute() || fullPath.string().find(Paths::AssetRoot().string()) == std::string::npos)
		{
			std::string sub = m_LoadedPath;
			if (sub.rfind("assets/", 0) == 0 || sub.rfind("assets\\", 0) == 0)
				sub = sub.substr(7);
			fullPath = Paths::AssetRoot() / sub;
		}

		std::error_code ec;
		std::filesystem::create_directories(fullPath.parent_path(), ec);

		if (SaveCurveToFile(fullPath.string(), m_Curve))
		{
			m_IsDirty = false;
			std::filesystem::path rel = std::filesystem::relative(fullPath, Paths::AssetRoot(), ec);
			if (!ec && !rel.empty() && rel.string().find("..") == std::string::npos)
				m_LoadedPath = rel.generic_string();
			m_StatusFeedback = "Saved: " + m_LoadedPath;
			UpdateTitle();
			return true;
		}
		m_StatusFeedback = "Save failed: " + fullPath.string();
		return false;
	}

	void CurveEditorPanel::Revert()
	{
		if (!m_LoadedPath.empty())
			LoadAsset(m_LoadedPath);
		else
		{
			m_Curve = CubicBezierCurve::EaseInOut();
			m_IsDirty = false;
			m_SelectedKeyframeIndex = 0;
			UpdateTitle();
			FrameAll();
		}
	}

	void CurveEditorPanel::FrameAll()
	{
		if (m_Curve.KeyframeCount() == 0)
		{
			m_ViewMinTime = -0.2f;
			m_ViewMaxTime = 1.2f;
			m_ViewMinValue = -0.2f;
			m_ViewMaxValue = 1.2f;
			return;
		}

		float minT = m_Curve.GetKeyframe(0).Time;
		float maxT = m_Curve.GetKeyframe(0).Time;
		float minV = m_Curve.GetKeyframe(0).Value;
		float maxV = m_Curve.GetKeyframe(0).Value;

		for (size_t i = 0; i < m_Curve.KeyframeCount(); ++i)
		{
			const auto& kf = m_Curve.GetKeyframe(i);
			minT = std::min(minT, kf.Time);
			maxT = std::max(maxT, kf.Time);
			minV = std::min(minV, kf.Value);
			maxV = std::max(maxV, kf.Value);
		}

		float spanT = std::max(maxT - minT, 0.1f);
		float spanV = std::max(maxV - minV, 0.1f);

		m_ViewMinTime = minT - spanT * 0.15f;
		m_ViewMaxTime = maxT + spanT * 0.15f;
		m_ViewMinValue = minV - spanV * 0.15f;
		m_ViewMaxValue = maxV + spanV * 0.15f;
	}

	void CurveEditorPanel::SetSelectedTangentFlat()
	{
		if (m_SelectedKeyframeIndex >= 0 && m_SelectedKeyframeIndex < static_cast<int>(m_Curve.KeyframeCount()))
		{
			auto& kf = m_Curve.GetKeyframeMut(m_SelectedKeyframeIndex);
			kf.InTangent = 0.0f;
			kf.OutTangent = 0.0f;
			kf.Mode = TangentMode::Aligned;
			m_Curve.PrecomputeLut(256);
			m_IsDirty = true;
			UpdateTitle();
		}
	}

	void CurveEditorPanel::SetSelectedTangentLinear()
	{
		if (m_SelectedKeyframeIndex >= 0 && m_SelectedKeyframeIndex < static_cast<int>(m_Curve.KeyframeCount()))
		{
			auto& kf = m_Curve.GetKeyframeMut(m_SelectedKeyframeIndex);
			size_t idx = static_cast<size_t>(m_SelectedKeyframeIndex);
			float inSlope = 0.0f;
			float outSlope = 0.0f;
			if (idx > 0)
			{
				const auto& prev = m_Curve.GetKeyframe(idx - 1);
				float dt = kf.Time - prev.Time;
				if (std::abs(dt) > 1e-5f) inSlope = (kf.Value - prev.Value) / dt;
			}
			if (idx + 1 < m_Curve.KeyframeCount())
			{
				const auto& next = m_Curve.GetKeyframe(idx + 1);
				float dt = next.Time - kf.Time;
				if (std::abs(dt) > 1e-5f) outSlope = (next.Value - kf.Value) / dt;
			}
			kf.InTangent = inSlope;
			kf.OutTangent = outSlope;
			kf.Mode = TangentMode::Broken;
			m_Curve.PrecomputeLut(256);
			m_IsDirty = true;
			UpdateTitle();
		}
	}

	void CurveEditorPanel::SetSelectedTangentSmooth()
	{
		m_Curve.AutoSmoothTangents();
		m_Curve.PrecomputeLut(256);
		m_IsDirty = true;
		UpdateTitle();
	}

	void CurveEditorPanel::SetSelectedTangentStep()
	{
		if (m_SelectedKeyframeIndex >= 0 && m_SelectedKeyframeIndex < static_cast<int>(m_Curve.KeyframeCount()))
		{
			auto& kf = m_Curve.GetKeyframeMut(m_SelectedKeyframeIndex);
			kf.InTangent = 0.0f;
			kf.OutTangent = 9999.0f;
			kf.Mode = TangentMode::Broken;
			m_Curve.PrecomputeLut(256);
			m_IsDirty = true;
			UpdateTitle();
		}
	}

	bool CurveEditorPanel::OnShortcut(uint32_t keyCode, bool ctrl, bool shift, bool alt)
	{
		(void)shift;
		(void)alt;
		if (ctrl && keyCode == KeyCodes::S)
		{
			SaveAsset();
			return true;
		}
		if (keyCode == KeyCodes::F)
		{
			FrameAll();
			return true;
		}
		return false;
	}

	void CurveEditorPanel::UpdateTitle()
	{
		char buf[256];
		if (m_LoadedPath.empty())
			std::snprintf(buf, sizeof(buf), "Curve Editor - [Untitled]%s", m_IsDirty ? "*" : "");
		else
		{
			std::string name = std::filesystem::path(m_LoadedPath).filename().string();
			std::snprintf(buf, sizeof(buf), "Curve Editor - [%s]%s", name.c_str(), m_IsDirty ? "*" : "");
		}
		m_Title = buf;
	}

	float CurveEditorPanel::TimeToScreenX(float t, const Wui::WuiRect& r) const
	{
		float span = m_ViewMaxTime - m_ViewMinTime;
		if (span <= 1e-5f) return r.X;
		return r.X + (t - m_ViewMinTime) / span * r.W;
	}

	float CurveEditorPanel::ValueToScreenY(float v, const Wui::WuiRect& r) const
	{
		float span = m_ViewMaxValue - m_ViewMinValue;
		if (span <= 1e-5f) return r.Y + r.H * 0.5f;
		return r.Y + (1.0f - (v - m_ViewMinValue) / span) * r.H;
	}

	float CurveEditorPanel::ScreenXToTime(float sx, const Wui::WuiRect& r) const
	{
		float span = m_ViewMaxTime - m_ViewMinTime;
		if (r.W <= 1e-5f) return m_ViewMinTime;
		return m_ViewMinTime + (sx - r.X) / r.W * span;
	}

	float CurveEditorPanel::ScreenYToValue(float sy, const Wui::WuiRect& r) const
	{
		float span = m_ViewMaxValue - m_ViewMinValue;
		if (r.H <= 1e-5f) return m_ViewMinValue;
		return m_ViewMinValue + (1.0f - (sy - r.Y) / r.H) * span;
	}
	void CurveEditorPanel::OnRender(Wui::WuiContext& ctx, const Wui::WuiRect& rect, PanelHost& host)
	{
		const Wui::WuiTheme& theme = host.Theme();
		Wui::PanelBackground(ctx, rect, theme.PanelBg);
		Wui::WuiId baseId = 0x510000;

		float y = rect.Y + 6.0f;

		// ---- 1. 顶栏第一行：标题、文档操作与视图控制 (彻底去除多余的固定预设按钮组) ----
		Wui::Label(ctx, { rect.X + 12.0f, y + 4.0f }, m_Title, theme.Text, theme.FontSizeTitle);

		float actionX = rect.X + 240.0f;
		if (Wui::Button(ctx, baseId++, { actionX, y, 54.0f, 22.0f }, "Save", theme))
		{
			SaveAsset();
			host.MarkDocumentDirty();
		}
		actionX += 58.0f;
		if (Wui::Button(ctx, baseId++, { actionX, y, 62.0f, 22.0f }, "Revert", theme))
		{
			Revert();
		}
		actionX += 66.0f;
		if (Wui::Button(ctx, baseId++, { actionX, y, 84.0f, 22.0f }, "Frame All (F)", theme))
		{
			FrameAll();
		}
		actionX += 90.0f;

		// 吸附开关 (独立按钮显示明确状态)
		std::string snapLabel = m_SnapToGrid ? "[x] Snap 0.05" : "[ ] Snap 0.05";
		if (Wui::Button(ctx, baseId++, { actionX, y, 92.0f, 22.0f }, snapLabel, theme))
		{
			m_SnapToGrid = !m_SnapToGrid;
		}
		actionX += 98.0f;

		if (!m_StatusFeedback.empty())
		{
			Wui::Label(ctx, { actionX, y + 4.0f }, m_StatusFeedback, theme.Success, theme.FontSizeSmall);
		}

		y += 28.0f;

		// ---- 2. 顶栏第二行：切线快捷键 + 外插循环设置 (使用无截断的 Combo 下拉框) ----
		float toolX = rect.X + 12.0f;
		Wui::Label(ctx, { toolX, y + 4.0f }, "Tangents:", theme.TextMuted, theme.FontSizeSmall);
		toolX += 62.0f;

		if (Wui::Button(ctx, baseId++, { toolX, y, 46.0f, 22.0f }, "Flat", theme)) SetSelectedTangentFlat();
		toolX += 50.0f;
		if (Wui::Button(ctx, baseId++, { toolX, y, 52.0f, 22.0f }, "Linear", theme)) SetSelectedTangentLinear();
		toolX += 56.0f;
		if (Wui::Button(ctx, baseId++, { toolX, y, 58.0f, 22.0f }, "Smooth", theme)) SetSelectedTangentSmooth();
		toolX += 62.0f;
		if (Wui::Button(ctx, baseId++, { toolX, y, 46.0f, 22.0f }, "Step", theme)) SetSelectedTangentStep();
		toolX += 58.0f;

		// 分隔线
		Wui::LineSegment(ctx, { toolX, y + 2.0f }, { toolX, y + 20.0f }, theme.BorderStrong, 1.0f);
		toolX += 10.0f;

		// Pre-Loop 下拉选择框 (无截断)
		Wui::Label(ctx, { toolX, y + 4.0f }, "Pre-Loop:", theme.TextMuted, theme.FontSizeSmall);
		toolX += 62.0f;
		int preExtrap = static_cast<int>(m_Curve.PreExtrapolation);
		if (Wui::Combo(ctx, baseId++, { toolX, y, 136.0f, 22.0f }, "PreLoopCombo", m_ExtrapComboOptions, preExtrap, theme))
		{
			m_Curve.PreExtrapolation = static_cast<CurveExtrapolation>(preExtrap);
			m_IsDirty = true;
			UpdateTitle();
		}
		toolX += 144.0f;

		// Post-Loop 下拉选择框 (无截断)
		Wui::Label(ctx, { toolX, y + 4.0f }, "Post-Loop:", theme.TextMuted, theme.FontSizeSmall);
		toolX += 68.0f;
		int postExtrap = static_cast<int>(m_Curve.PostExtrapolation);
		if (Wui::Combo(ctx, baseId++, { toolX, y, 136.0f, 22.0f }, "PostLoopCombo", m_ExtrapComboOptions, postExtrap, theme))
		{
			m_Curve.PostExtrapolation = static_cast<CurveExtrapolation>(postExtrap);
			m_IsDirty = true;
			UpdateTitle();
		}
		y += 28.0f;

		// ---- 3. 主画布区域 (视口、网格、[0, 1]工作区底衬、贝塞尔曲线、手柄、准星) ----
		const float canvasMarginX = 12.0f;
		const float canvasWidth = std::max(rect.W - canvasMarginX * 2.0f, 100.0f);
		const float bottomBarHeight = 124.0f;
		const float canvasHeight = std::max(rect.H - (y - rect.Y) - bottomBarHeight, 160.0f);
		const Wui::WuiRect canvasRect { rect.X + canvasMarginX, y, canvasWidth, canvasHeight };

		// 画布背景与外边框
		Wui::PanelBackground(ctx, canvasRect, theme.ContentBg, 3.0f);
		Wui::LineSegment(ctx, { canvasRect.X, canvasRect.Y }, { canvasRect.X + canvasRect.W, canvasRect.Y }, theme.Border, 1.0f);
		Wui::LineSegment(ctx, { canvasRect.X, canvasRect.Y + canvasRect.H }, { canvasRect.X + canvasRect.W, canvasRect.Y + canvasRect.H }, theme.Border, 1.0f);
		Wui::LineSegment(ctx, { canvasRect.X, canvasRect.Y }, { canvasRect.X, canvasRect.Y + canvasRect.H }, theme.Border, 1.0f);
		Wui::LineSegment(ctx, { canvasRect.X + canvasRect.W, canvasRect.Y }, { canvasRect.X + canvasRect.W, canvasRect.Y + canvasRect.H }, theme.Border, 1.0f);

		// [0.0, 1.0] 单位安全工作区高亮底衬
		float u0x = TimeToScreenX(0.0f, canvasRect);
		float u1x = TimeToScreenX(1.0f, canvasRect);
		float u0y = ValueToScreenY(0.0f, canvasRect);
		float u1y = ValueToScreenY(1.0f, canvasRect);

		float unitLeft = std::max(std::min(u0x, u1x), canvasRect.X);
		float unitRight = std::min(std::max(u0x, u1x), canvasRect.X + canvasRect.W);
		float unitTop = std::max(std::min(u0y, u1y), canvasRect.Y);
		float unitBottom = std::min(std::max(u0y, u1y), canvasRect.Y + canvasRect.H);

		if (unitRight > unitLeft && unitBottom > unitTop)
		{
			Wui::WuiRect unitRect { unitLeft, unitTop, unitRight - unitLeft, unitBottom - unitTop };
			Wui::WuiColor unitBg = theme.ContentBg;
			Wui::PanelBackground(ctx, unitRect, unitBg, 0.0f);
			DrawClippedLine(ctx, glm::vec2(u0x, u0y), glm::vec2(u1x, u0y), canvasRect, theme.BorderStrong, 1.0f);
			DrawClippedLine(ctx, glm::vec2(u1x, u0y), glm::vec2(u1x, u1y), canvasRect, theme.BorderStrong, 1.0f);
			DrawClippedLine(ctx, glm::vec2(u1x, u1y), glm::vec2(u0x, u1y), canvasRect, theme.BorderStrong, 1.0f);
			DrawClippedLine(ctx, glm::vec2(u0x, u1y), glm::vec2(u0x, u0y), canvasRect, theme.BorderStrong, 1.0f);
			if (u1x - 52.0f >= canvasRect.X && u1y + 2.0f <= canvasRect.Y + canvasRect.H)
				Wui::Label(ctx, { u1x - 52.0f, u1y + 2.0f }, "[1.0, 1.0]", theme.TextDisabled, theme.FontSizeCaption);
		}

		// 动态参考网格与主次刻度
		auto calcNiceStep = [](float span, int maxTicks) -> float {
			float rawStep = span / static_cast<float>(maxTicks);
			if (rawStep <= 1e-6f) return 1.0f;
			float exp = std::floor(std::log10(rawStep));
			float frac = rawStep / std::pow(10.0f, exp);
			float nice = 1.0f;
			if (frac > 5.0f) nice = 10.0f;
			else if (frac > 2.0f) nice = 5.0f;
			else if (frac > 1.0f) nice = 2.0f;
			return nice * std::pow(10.0f, exp);
		};

		float stepT = calcNiceStep(m_ViewMaxTime - m_ViewMinTime, 8);
		float stepV = calcNiceStep(m_ViewMaxValue - m_ViewMinValue, 6);

		// 时间轴垂直网格线
		float startT = std::floor(m_ViewMinTime / stepT) * stepT;
		for (float t = startT; t <= m_ViewMaxTime; t += stepT)
		{
			float sx = TimeToScreenX(t, canvasRect);
			if (sx >= canvasRect.X && sx <= canvasRect.X + canvasRect.W)
			{
				bool isOrigin = std::abs(t) < stepT * 0.001f;
				Wui::WuiColor lineCol = isOrigin ? theme.Accent : theme.Border;
				float thick = isOrigin ? 1.5f : 1.0f;
				Wui::LineSegment(ctx, { sx, canvasRect.Y }, { sx, canvasRect.Y + canvasRect.H }, lineCol, thick);

				char buf[32];
				std::snprintf(buf, sizeof(buf), (std::abs(t) < 0.001f) ? "0.0s" : "%.2gs", t);
				Wui::Label(ctx, { sx + 3.0f, canvasRect.Y + canvasRect.H - 16.0f }, buf, theme.TextDisabled, theme.FontSizeCaption);
			}
		}

		// 数值轴水平网格线
		float startV = std::floor(m_ViewMinValue / stepV) * stepV;
		for (float v = startV; v <= m_ViewMaxValue; v += stepV)
		{
			float sy = ValueToScreenY(v, canvasRect);
			if (sy >= canvasRect.Y && sy <= canvasRect.Y + canvasRect.H)
			{
				bool isOrigin = std::abs(v) < stepV * 0.001f;
				Wui::WuiColor lineCol = isOrigin ? theme.Accent : theme.Border;
				float thick = isOrigin ? 1.5f : 1.0f;
				Wui::LineSegment(ctx, { canvasRect.X, sy }, { canvasRect.X + canvasRect.W, sy }, lineCol, thick);

				char buf[32];
				std::snprintf(buf, sizeof(buf), (std::abs(v) < 0.001f) ? "0.0" : "%.2g", v);
				Wui::Label(ctx, { canvasRect.X + 4.0f, sy - 14.0f }, buf, theme.TextDisabled, theme.FontSizeCaption);
			}
		}

		// 绘制外插虚线轨迹
		if (m_Curve.KeyframeCount() >= 2)
		{
			float firstT = m_Curve.GetKeyframe(0).Time;
			float lastT = m_Curve.GetKeyframe(m_Curve.KeyframeCount() - 1).Time;
			Wui::WuiColor ghostCol = { theme.Accent.R, theme.Accent.G, theme.Accent.B, 0.35f };

			if (m_ViewMinTime < firstT)
			{
				const int ghostSamples = 60;
				float gT0 = m_ViewMinTime;
				float gT1 = firstT;
				glm::vec2 gPrev { TimeToScreenX(gT0, canvasRect), ValueToScreenY(m_Curve.Evaluate(gT0), canvasRect) };
				for (int i = 1; i <= ghostSamples; ++i)
				{
					float curT = gT0 + (gT1 - gT0) * (static_cast<float>(i) / ghostSamples);
					glm::vec2 gCur { TimeToScreenX(curT, canvasRect), ValueToScreenY(m_Curve.Evaluate(curT), canvasRect) };
					if (i % 2 == 1) DrawClippedLine(ctx, gPrev, gCur, canvasRect, ghostCol, 1.5f);
					gPrev = gCur;
				}
			}

			if (m_ViewMaxTime > lastT)
			{
				const int ghostSamples = 60;
				float gT0 = lastT;
				float gT1 = m_ViewMaxTime;
				glm::vec2 gPrev { TimeToScreenX(gT0, canvasRect), ValueToScreenY(m_Curve.Evaluate(gT0), canvasRect) };
				for (int i = 1; i <= ghostSamples; ++i)
				{
					float curT = gT0 + (gT1 - gT0) * (static_cast<float>(i) / ghostSamples);
					glm::vec2 gCur { TimeToScreenX(curT, canvasRect), ValueToScreenY(m_Curve.Evaluate(curT), canvasRect) };
					if (i % 2 == 1) DrawClippedLine(ctx, gPrev, gCur, canvasRect, ghostCol, 1.5f);
					gPrev = gCur;
				}
			}
		}

		// 绘制贝塞尔曲线实体线段 (细分采样)
		const int numSamples = std::clamp(static_cast<int>(canvasRect.W / 2.5f), 80, 500);
		float tStart = ScreenXToTime(canvasRect.X, canvasRect);
		glm::vec2 prevPt { canvasRect.X, ValueToScreenY(m_Curve.Evaluate(tStart), canvasRect) };
		for (int i = 1; i <= numSamples; ++i)
		{
			float sx = canvasRect.X + (static_cast<float>(i) / numSamples) * canvasRect.W;
			float t = ScreenXToTime(sx, canvasRect);
			float val = m_Curve.Evaluate(t);
			glm::vec2 currPt { sx, ValueToScreenY(val, canvasRect) };
			DrawClippedLine(ctx, prevPt, currPt, canvasRect, theme.Accent, 2.5f);
			prevPt = currPt;
		}

		// 关键帧点与切线手柄
		const bool hasSelectedKf = (m_SelectedKeyframeIndex >= 0 && m_SelectedKeyframeIndex < static_cast<int>(m_Curve.KeyframeCount()));
		glm::vec2 inHandleScreenPos { 0.0f, 0.0f };
		glm::vec2 outHandleScreenPos { 0.0f, 0.0f };
		glm::vec2 selectedKfScreenPt { 0.0f, 0.0f };

		const glm::vec2 mousePos = ctx.Input().MousePos;
		const bool inCanvas = canvasRect.Contains(mousePos);
		m_HoveredKeyframeIndex = -1;
		m_HoveredInHandle = false;
		m_HoveredOutHandle = false;

		for (size_t k = 0; k < m_Curve.KeyframeCount(); ++k)
		{
			const auto& kf = m_Curve.GetKeyframe(k);
			float ptX = TimeToScreenX(kf.Time, canvasRect);
			float ptY = ValueToScreenY(kf.Value, canvasRect);
			glm::vec2 ptPos { ptX, ptY };

			bool isSel = (static_cast<int>(k) == m_SelectedKeyframeIndex);
			bool isHover = inCanvas && (glm::distance(mousePos, ptPos) <= 7.0f);
			if (isHover) m_HoveredKeyframeIndex = static_cast<int>(k);

			if (ptX >= canvasRect.X - 8.0f && ptX <= canvasRect.X + canvasRect.W + 8.0f &&
				ptY >= canvasRect.Y - 8.0f && ptY <= canvasRect.Y + canvasRect.H + 8.0f)
			{
				if (isSel)
				{
					selectedKfScreenPt = ptPos;
					DrawClippedLine(ctx, glm::vec2(ptX - 7.0f, ptY), glm::vec2(ptX, ptY - 7.0f), canvasRect, theme.Accent, 2.0f);
					DrawClippedLine(ctx, glm::vec2(ptX, ptY - 7.0f), glm::vec2(ptX + 7.0f, ptY), canvasRect, theme.Accent, 2.0f);
					DrawClippedLine(ctx, glm::vec2(ptX + 7.0f, ptY), glm::vec2(ptX, ptY + 7.0f), canvasRect, theme.Accent, 2.0f);
					DrawClippedLine(ctx, glm::vec2(ptX, ptY + 7.0f), glm::vec2(ptX - 7.0f, ptY), canvasRect, theme.Accent, 2.0f);
				}

				Wui::WuiColor fillCol = isSel ? theme.Warning : (isHover ? theme.Accent : theme.Text);
				DrawClippedLine(ctx, glm::vec2(ptX - 4.0f, ptY), glm::vec2(ptX, ptY - 4.0f), canvasRect, fillCol, 2.0f);
				DrawClippedLine(ctx, glm::vec2(ptX, ptY - 4.0f), glm::vec2(ptX + 4.0f, ptY), canvasRect, fillCol, 2.0f);
				DrawClippedLine(ctx, glm::vec2(ptX + 4.0f, ptY), glm::vec2(ptX, ptY + 4.0f), canvasRect, fillCol, 2.0f);
				DrawClippedLine(ctx, glm::vec2(ptX, ptY + 4.0f), glm::vec2(ptX - 4.0f, ptY), canvasRect, fillCol, 2.0f);
			}
		}

		if (hasSelectedKf)
		{
			const auto& selKf = m_Curve.GetKeyframe(m_SelectedKeyframeIndex);
			const float handlePixelLength = 48.0f;

			float inSx = selectedKfScreenPt.x - handlePixelLength;
			float inT = ScreenXToTime(inSx, canvasRect);
			float inVal = selKf.Value - selKf.InTangent * (selKf.Time - inT);
			float inSy = ValueToScreenY(inVal, canvasRect);
			inHandleScreenPos = { inSx, inSy };

			float outSx = selectedKfScreenPt.x + handlePixelLength;
			float outT = ScreenXToTime(outSx, canvasRect);
			float outVal = selKf.Value + selKf.OutTangent * (outT - selKf.Time);
			float outSy = ValueToScreenY(outVal, canvasRect);
			outHandleScreenPos = { outSx, outSy };

			m_HoveredInHandle = inCanvas && (glm::distance(mousePos, inHandleScreenPos) <= 7.0f);
			m_HoveredOutHandle = inCanvas && (glm::distance(mousePos, outHandleScreenPos) <= 7.0f);

			Wui::WuiColor inColor = m_HoveredInHandle ? theme.Warning : theme.Info;
			Wui::WuiColor outColor = m_HoveredOutHandle ? theme.Warning : theme.Accent;

			DrawClippedLine(ctx, selectedKfScreenPt, inHandleScreenPos, canvasRect, inColor, 1.5f);
			DrawClippedLine(ctx, selectedKfScreenPt, outHandleScreenPos, canvasRect, outColor, 1.5f);

			auto drawHandleCircle = [&](glm::vec2 hp, const Wui::WuiColor& c, bool hover) {
				float r = hover ? 5.0f : 3.5f;
				DrawClippedLine(ctx, glm::vec2(hp.x - r, hp.y), glm::vec2(hp.x, hp.y - r), canvasRect, c, 2.0f);
				DrawClippedLine(ctx, glm::vec2(hp.x, hp.y - r), glm::vec2(hp.x + r, hp.y), canvasRect, c, 2.0f);
				DrawClippedLine(ctx, glm::vec2(hp.x + r, hp.y), glm::vec2(hp.x, hp.y + r), canvasRect, c, 2.0f);
				DrawClippedLine(ctx, glm::vec2(hp.x, hp.y + r), glm::vec2(hp.x - r, hp.y), canvasRect, c, 2.0f);
			};
			drawHandleCircle(inHandleScreenPos, inColor, m_HoveredInHandle);
			drawHandleCircle(outHandleScreenPos, outColor, m_HoveredOutHandle);
		}

		// 动态走带光标与相交求值高亮球
		if (m_IsPlaying)
		{
			m_PreviewT += 0.016f * m_PlaySpeed;
			if (m_PreviewT > 1.0f) m_PreviewT = std::fmod(m_PreviewT, 1.0f);
		}

		m_EvaluatedValue = m_Curve.Evaluate(m_PreviewT);
		float playheadX = TimeToScreenX(m_PreviewT, canvasRect);
		float playheadY = ValueToScreenY(m_EvaluatedValue, canvasRect);

		if (playheadX >= canvasRect.X && playheadX <= canvasRect.X + canvasRect.W)
		{
			Wui::LineSegment(ctx, { playheadX, canvasRect.Y }, { playheadX, canvasRect.Y + canvasRect.H }, theme.Success, 1.5f);
			DrawClippedLine(ctx, glm::vec2(playheadX - 5.0f, canvasRect.Y), glm::vec2(playheadX + 5.0f, canvasRect.Y), canvasRect, theme.Success, 2.0f);
			DrawClippedLine(ctx, glm::vec2(playheadX + 5.0f, canvasRect.Y), glm::vec2(playheadX, canvasRect.Y + 8.0f), canvasRect, theme.Success, 2.0f);
			DrawClippedLine(ctx, glm::vec2(playheadX, canvasRect.Y + 8.0f), glm::vec2(playheadX - 5.0f, canvasRect.Y), canvasRect, theme.Success, 2.0f);

			if (playheadY >= canvasRect.Y && playheadY <= canvasRect.Y + canvasRect.H)
			{
				DrawClippedLine(ctx, glm::vec2(playheadX - 4.0f, playheadY), glm::vec2(playheadX + 4.0f, playheadY), canvasRect, theme.Success, 2.5f);
				DrawClippedLine(ctx, glm::vec2(playheadX, playheadY - 4.0f), glm::vec2(playheadX, playheadY + 4.0f), canvasRect, theme.Success, 2.5f);
			}
		}

		// 准星十字线与光标跟随坐标 HUD
		if (inCanvas && !m_IsPanning && !m_IsDraggingKeyframe && !m_IsDraggingInHandle && !m_IsDraggingOutHandle)
		{
			Wui::WuiColor crosshairCol { theme.BorderStrong.R, theme.BorderStrong.G, theme.BorderStrong.B, 0.4f };
			Wui::LineSegment(ctx, { mousePos.x, canvasRect.Y }, { mousePos.x, canvasRect.Y + canvasRect.H }, crosshairCol, 1.0f);
			Wui::LineSegment(ctx, { canvasRect.X, mousePos.y }, { canvasRect.X + canvasRect.W, mousePos.y }, crosshairCol, 1.0f);

			float hoverT = ScreenXToTime(mousePos.x, canvasRect);
			float hoverV = ScreenYToValue(mousePos.y, canvasRect);
			char hudBuf[64];
			std::snprintf(hudBuf, sizeof(hudBuf), "T: %.3fs | V: %.3f", hoverT, hoverV);

			float hudX = std::min(mousePos.x + 12.0f, canvasRect.X + canvasRect.W - 120.0f);
			float hudY = std::max(mousePos.y - 20.0f, canvasRect.Y + 4.0f);
			Wui::WuiRect hudRect { hudX, hudY, 114.0f, 18.0f };
			Wui::PanelBackground(ctx, hudRect, theme.PanelBg, 2.0f);
			Wui::Label(ctx, { hudX + 4.0f, hudY + 2.0f }, hudBuf, theme.Text, theme.FontSizeCaption);
		}

		// ---- 4. 画布鼠标与键盘交互处理 (包含右键菜单触发) ----
		// 中键平移 (Pan)
		if (ctx.Input().MouseClicked[2] && inCanvas)
		{
			m_IsPanning = true;
			m_PanStartMouse = mousePos;
			m_PanStartMinTime = m_ViewMinTime;
			m_PanStartMaxTime = m_ViewMaxTime;
			m_PanStartMinValue = m_ViewMinValue;
			m_PanStartMaxValue = m_ViewMaxValue;
		}
		else if (m_IsPanning)
		{
			if (ctx.Input().MouseDown[2])
			{
				float dx = mousePos.x - m_PanStartMouse.x;
				float dy = mousePos.y - m_PanStartMouse.y;
				float spanT = m_PanStartMaxTime - m_PanStartMinTime;
				float spanV = m_PanStartMaxValue - m_PanStartMinValue;
				float dt = -dx / canvasRect.W * spanT;
				float dv = dy / canvasRect.H * spanV;
				m_ViewMinTime = m_PanStartMinTime + dt;
				m_ViewMaxTime = m_PanStartMaxTime + dt;
				m_ViewMinValue = m_PanStartMinValue + dv;
				m_ViewMaxValue = m_PanStartMaxValue + dv;
			}
			else
			{
				m_IsPanning = false;
			}
		}

		// 中键双击 Frame All
		if (ctx.Input().MouseDoubleClicked[2] && inCanvas)
		{
			FrameAll();
		}

		// 滚轮缩放 (Zoom)
		if (inCanvas && ctx.Input().Wheel != 0.0f)
		{
			float wheel = ctx.Input().Wheel;
			float zoomFactor = (wheel > 0.0f) ? 0.85f : 1.15f;
			float mouseT = ScreenXToTime(mousePos.x, canvasRect);
			float mouseV = ScreenYToValue(mousePos.y, canvasRect);
			m_ViewMinTime = mouseT + (m_ViewMinTime - mouseT) * zoomFactor;
			m_ViewMaxTime = mouseT + (m_ViewMaxTime - mouseT) * zoomFactor;
			m_ViewMinValue = mouseV + (m_ViewMinValue - mouseV) * zoomFactor;
			m_ViewMaxValue = mouseV + (m_ViewMaxValue - mouseV) * zoomFactor;
		}

		// 右键点击：触发面板上下文菜单
		if (ctx.Input().MouseClicked[1] && inCanvas)
		{
			m_ContextMenuPos = mousePos;
			m_ContextMenuTime = ScreenXToTime(mousePos.x, canvasRect);
			m_ContextMenuValue = ScreenYToValue(mousePos.y, canvasRect);

			// 检查是否右键在关键帧上
			m_ContextMenuOnKeyframe = false;
			for (size_t k = 0; k < m_Curve.KeyframeCount(); ++k)
			{
				const auto& kf = m_Curve.GetKeyframe(k);
				glm::vec2 kfPos { TimeToScreenX(kf.Time, canvasRect), ValueToScreenY(kf.Value, canvasRect) };
				if (glm::distance(mousePos, kfPos) <= 8.0f)
				{
					m_SelectedKeyframeIndex = static_cast<int>(k);
					m_ContextMenuOnKeyframe = true;
					break;
				}
			}
			ctx.OpenPopup(Wui::HashId("curve_editor.context_menu"));
		}

		// 左键点击与拖拽：手柄 / 关键帧 / 空白双击加点
		if (ctx.Input().MouseClicked[0] && inCanvas)
		{
			bool hit = false;

			if (hasSelectedKf)
			{
				if (glm::distance(mousePos, inHandleScreenPos) <= 9.0f)
				{
					m_IsDraggingInHandle = true;
					m_DragStartMouse = mousePos;
					hit = true;
				}
				else if (glm::distance(mousePos, outHandleScreenPos) <= 9.0f)
				{
					m_IsDraggingOutHandle = true;
					m_DragStartMouse = mousePos;
					hit = true;
				}
			}

			if (!hit)
			{
				for (size_t k = 0; k < m_Curve.KeyframeCount(); ++k)
				{
					const auto& kf = m_Curve.GetKeyframe(k);
					glm::vec2 kfPos { TimeToScreenX(kf.Time, canvasRect), ValueToScreenY(kf.Value, canvasRect) };
					if (glm::distance(mousePos, kfPos) <= 9.0f)
					{
						m_SelectedKeyframeIndex = static_cast<int>(k);
						m_IsDraggingKeyframe = true;
						m_DragStartMouse = mousePos;
						m_DragKeyframeStartTime = kf.Time;
						m_DragKeyframeStartVal = kf.Value;
						hit = true;
						break;
					}
				}
			}

			if (!hit && ctx.Input().MouseDoubleClicked[0])
			{
				float addT = ScreenXToTime(mousePos.x, canvasRect);
				float addV = ScreenYToValue(mousePos.y, canvasRect);
				if (m_SnapToGrid)
				{
					addT = std::round(addT / m_SnapTimeStep) * m_SnapTimeStep;
					addV = std::round(addV / m_SnapValStep) * m_SnapValStep;
				}
				Keyframe newKf(addT, addV, 0.0f, 0.0f, TangentMode::Aligned);
				size_t newIdx = m_Curve.AddKeyframe(newKf);
				m_Curve.PrecomputeLut(256);
				m_SelectedKeyframeIndex = static_cast<int>(newIdx);
				m_IsDirty = true;
				UpdateTitle();
				host.MarkDocumentDirty();
			}
		}

		// 拖拽过程更新
		if (ctx.Input().MouseDown[0] && hasSelectedKf)
		{
			auto& mutKf = m_Curve.GetKeyframeMut(m_SelectedKeyframeIndex);

			if (m_IsDraggingInHandle)
			{
				float curT = ScreenXToTime(mousePos.x, canvasRect);
				float curV = ScreenYToValue(mousePos.y, canvasRect);
				float dt = curT - mutKf.Time;
				float dv = curV - mutKf.Value;
				if (std::abs(dt) > 1e-4f)
				{
					float slope = dv / dt;
					mutKf.InTangent = slope;
					if (mutKf.Mode == TangentMode::Aligned)
						mutKf.OutTangent = slope;
					m_Curve.PrecomputeLut(256);
					m_IsDirty = true;
					UpdateTitle();
				}
			}
			else if (m_IsDraggingOutHandle)
			{
				float curT = ScreenXToTime(mousePos.x, canvasRect);
				float curV = ScreenYToValue(mousePos.y, canvasRect);
				float dt = curT - mutKf.Time;
				float dv = curV - mutKf.Value;
				if (std::abs(dt) > 1e-4f)
				{
					float slope = dv / dt;
					mutKf.OutTangent = slope;
					if (mutKf.Mode == TangentMode::Aligned)
						mutKf.InTangent = slope;
					m_Curve.PrecomputeLut(256);
					m_IsDirty = true;
					UpdateTitle();
				}
			}
			else if (m_IsDraggingKeyframe)
			{
				float curT = ScreenXToTime(mousePos.x, canvasRect);
				float curV = ScreenYToValue(mousePos.y, canvasRect);
				float startT = ScreenXToTime(m_DragStartMouse.x, canvasRect);
				float startV = ScreenYToValue(m_DragStartMouse.y, canvasRect);
				float dt = curT - startT;
				float dv = curV - startV;

				float newT = m_DragKeyframeStartTime + dt;
				float newV = m_DragKeyframeStartVal + dv;

				if (ctx.Input().Shift)
				{
					if (std::abs(mousePos.x - m_DragStartMouse.x) > std::abs(mousePos.y - m_DragStartMouse.y))
						newV = m_DragKeyframeStartVal;
					else
						newT = m_DragKeyframeStartTime;
				}

				if (m_SnapToGrid)
				{
					newT = std::round(newT / m_SnapTimeStep) * m_SnapTimeStep;
					newV = std::round(newV / m_SnapValStep) * m_SnapValStep;
				}

				size_t idx = static_cast<size_t>(m_SelectedKeyframeIndex);
				float minT = (idx > 0) ? m_Curve.GetKeyframe(idx - 1).Time + 0.001f : -100.0f;
				float maxT = (idx + 1 < m_Curve.KeyframeCount()) ? m_Curve.GetKeyframe(idx + 1).Time - 0.001f : 100.0f;
				mutKf.Time = std::clamp(newT, minT, maxT);
				mutKf.Value = newV;
				m_Curve.PrecomputeLut(256);
				m_IsDirty = true;
				UpdateTitle();
			}
		}

		if (ctx.Input().MouseReleased[0])
		{
			m_IsDraggingKeyframe = false;
			m_IsDraggingInHandle = false;
			m_IsDraggingOutHandle = false;
		}

		if (inCanvas && (ctx.WasKeyPressed(KeyCodes::Delete) || ctx.WasKeyPressed(KeyCodes::Backspace)))
		{
			if (hasSelectedKf)
			{
				m_Curve.RemoveKeyframe(m_SelectedKeyframeIndex);
				m_Curve.PrecomputeLut(256);
				if (m_SelectedKeyframeIndex >= static_cast<int>(m_Curve.KeyframeCount()))
					m_SelectedKeyframeIndex = static_cast<int>(m_Curve.KeyframeCount()) - 1;
				m_IsDirty = true;
				UpdateTitle();
				host.MarkDocumentDirty();
			}
		}

		if (inCanvas && ctx.WasKeyPressed(KeyCodes::F))
		{
			FrameAll();
		}

		y += canvasHeight + 10.0f;
		// ---- 5. 底部第一行：走带试听控制与当前求值 (全部采用数值输入框，杜绝滑动条) ----
		float bottomX = rect.X + 12.0f;

		// 播放/暂停走带按钮
		const char* playIcon = m_IsPlaying ? "|| Pause" : "> Play";
		if (Wui::Button(ctx, baseId++, { bottomX, y, 66.0f, 22.0f }, playIcon, theme))
		{
			m_IsPlaying = !m_IsPlaying;
		}
		bottomX += 72.0f;

		Wui::Label(ctx, { bottomX, y + 4.0f }, "Eval Time (s):", theme.TextMuted, theme.FontSizeSmall);
		bottomX += 82.0f;

		// 采用 DragFloat 代替 SliderFloat，直接展示数值并支持点击输入和拖动微调
		if (Wui::DragFloat(ctx, baseId++, { bottomX, y, 76.0f, 22.0f }, m_PreviewT, 0.01f, 0.0f, 100.0f, theme))
		{
			m_EvaluatedValue = m_Curve.Evaluate(m_PreviewT);
		}
		bottomX += 82.0f;

		char evalBuf[32];
		std::snprintf(evalBuf, sizeof(evalBuf), "f(t) = %.3f", m_EvaluatedValue);
		Wui::Label(ctx, { bottomX, y + 4.0f }, evalBuf, theme.Text, theme.FontSizeSmall);
		bottomX += 90.0f;

		if (Wui::Button(ctx, baseId++, { bottomX, y, 106.0f, 22.0f }, "+ Add Key at Eval T", theme))
		{
			Keyframe newKf(m_PreviewT, m_EvaluatedValue, 0.0f, 0.0f, TangentMode::Aligned);
			size_t newIdx = m_Curve.AddKeyframe(newKf);
			m_Curve.PrecomputeLut(256);
			m_SelectedKeyframeIndex = static_cast<int>(newIdx);
			m_IsDirty = true;
			UpdateTitle();
			host.MarkDocumentDirty();
		}
		y += 26.0f;

		// ---- 6. 底部第二行：选中关键帧微调 (无滑动条，采用高精度 DragFloat 与无截断 Combo) ----
		if (m_Curve.KeyframeCount() > 0)
		{
			if (m_SelectedKeyframeIndex >= static_cast<int>(m_Curve.KeyframeCount()))
				m_SelectedKeyframeIndex = static_cast<int>(m_Curve.KeyframeCount()) - 1;
			if (m_SelectedKeyframeIndex < 0)
				m_SelectedKeyframeIndex = 0;

			auto& selectedKf = m_Curve.GetKeyframeMut(m_SelectedKeyframeIndex);
			float kfRowX = rect.X + 12.0f;

			char kfTitle[64];
			std::snprintf(kfTitle, sizeof(kfTitle), "#%d/%d:", m_SelectedKeyframeIndex + 1, static_cast<int>(m_Curve.KeyframeCount()));
			Wui::Label(ctx, { kfRowX, y + 4.0f }, kfTitle, theme.Text, theme.FontSizeSmall);
			kfRowX += 48.0f;

			if (Wui::Button(ctx, baseId++, { kfRowX, y, 24.0f, 22.0f }, "<", theme))
			{
				if (m_SelectedKeyframeIndex > 0) m_SelectedKeyframeIndex--;
			}
			kfRowX += 28.0f;
			if (Wui::Button(ctx, baseId++, { kfRowX, y, 24.0f, 22.0f }, ">", theme))
			{
				if (m_SelectedKeyframeIndex + 1 < static_cast<int>(m_Curve.KeyframeCount())) m_SelectedKeyframeIndex++;
			}
			kfRowX += 32.0f;

			// 切线模式下拉框 (取代被挤成 Clam... 的分段按钮)
			int curMode = static_cast<int>(selectedKf.Mode);
			if (Wui::Combo(ctx, baseId++, { kfRowX, y, 134.0f, 22.0f }, "KfModeCombo", m_TangentModeComboOptions, curMode, theme))
			{
				selectedKf.Mode = static_cast<TangentMode>(curMode);
				if (selectedKf.Mode == TangentMode::Aligned)
					selectedKf.InTangent = selectedKf.OutTangent;
				m_Curve.PrecomputeLut(256);
				m_IsDirty = true;
				UpdateTitle();
			}
			kfRowX += 140.0f;

			// Time (s) 采用 DragFloat
			Wui::Label(ctx, { kfRowX, y + 4.0f }, "Time (s):", theme.TextMuted, theme.FontSizeSmall);
			kfRowX += 54.0f;
			if (Wui::DragFloat(ctx, baseId++, { kfRowX, y, 76.0f, 22.0f }, selectedKf.Time, 0.01f, 0.0f, 0.0f, theme))
			{
				m_Curve.PrecomputeLut(256);
				m_IsDirty = true;
				UpdateTitle();
			}
			kfRowX += 82.0f;

			// Value 采用 DragFloat
			Wui::Label(ctx, { kfRowX, y + 4.0f }, "Value:", theme.TextMuted, theme.FontSizeSmall);
			kfRowX += 40.0f;
			if (Wui::DragFloat(ctx, baseId++, { kfRowX, y, 76.0f, 22.0f }, selectedKf.Value, 0.01f, 0.0f, 0.0f, theme))
			{
				m_Curve.PrecomputeLut(256);
				m_IsDirty = true;
				UpdateTitle();
			}
			kfRowX += 82.0f;

			// In-Tangent 采用 DragFloat
			Wui::Label(ctx, { kfRowX, y + 4.0f }, "In-Tan:", theme.TextMuted, theme.FontSizeSmall);
			kfRowX += 46.0f;
			if (Wui::DragFloat(ctx, baseId++, { kfRowX, y, 70.0f, 22.0f }, selectedKf.InTangent, 0.05f, 0.0f, 0.0f, theme))
			{
				if (selectedKf.Mode == TangentMode::Aligned) selectedKf.OutTangent = selectedKf.InTangent;
				m_Curve.PrecomputeLut(256);
				m_IsDirty = true;
				UpdateTitle();
			}
			kfRowX += 76.0f;

			// Out-Tangent 采用 DragFloat
			Wui::Label(ctx, { kfRowX, y + 4.0f }, "Out-Tan:", theme.TextMuted, theme.FontSizeSmall);
			kfRowX += 54.0f;
			if (Wui::DragFloat(ctx, baseId++, { kfRowX, y, 70.0f, 22.0f }, selectedKf.OutTangent, 0.05f, 0.0f, 0.0f, theme))
			{
				if (selectedKf.Mode == TangentMode::Aligned) selectedKf.InTangent = selectedKf.OutTangent;
				m_Curve.PrecomputeLut(256);
				m_IsDirty = true;
				UpdateTitle();
			}
			kfRowX += 76.0f;

			if (Wui::Button(ctx, baseId++, { kfRowX, y, 54.0f, 22.0f }, "Delete", theme))
			{
				m_Curve.RemoveKeyframe(m_SelectedKeyframeIndex);
				m_Curve.PrecomputeLut(256);
				if (m_SelectedKeyframeIndex >= static_cast<int>(m_Curve.KeyframeCount()))
					m_SelectedKeyframeIndex = static_cast<int>(m_Curve.KeyframeCount()) - 1;
				m_IsDirty = true;
				UpdateTitle();
				host.MarkDocumentDirty();
			}
		}
		y += 26.0f;

		// ---- 7. 底部操作指引胶囊栏 ----
		Wui::WuiRect footerRect { rect.X + 12.0f, y, rect.W - 24.0f, 20.0f };
		Wui::PanelBackground(ctx, footerRect, theme.HoverBg, 2.0f);

		const char* guideText = "[MMB] Pan   [Wheel] Zoom   [RMB] Context Menu   [F] Frame All   [Double-Click Canvas] Add Key   [Delete] Delete Key   [Shift] Lock Axis   [Ctrl+S] Save";
		Wui::Label(ctx, { footerRect.X + 8.0f, footerRect.Y + 3.0f }, guideText, theme.TextDisabled, theme.FontSizeCaption);

		// ---- 8. 右键上下文菜单渲染 (支持直接在面板空白处或关键帧上操作) ----
		Wui::WuiId contextMenuId = Wui::HashId("curve_editor.context_menu");
		Wui::WuiRect menuPanel;
		size_t menuItemsCount = m_ContextMenuOnKeyframe ? 9u : 8u;

		if (Wui::BeginContextMenu(ctx, contextMenuId, m_ContextMenuPos, 210.0f, menuItemsCount, &menuPanel, theme))
		{
			float itemY = menuPanel.Y + 4.0f;
			auto nextItem = [&itemY, &menuPanel]()
			{
				Wui::WuiRect item { menuPanel.X + 4.0f, itemY, menuPanel.W - 8.0f, 22.0f };
				itemY += 22.0f;
				return item;
			};

			if (m_ContextMenuOnKeyframe && hasSelectedKf)
			{
				// 关键帧右键操作
				if (Wui::ContextMenuItem(ctx, Wui::HashId("ctx.flat"), nextItem(), "Tangent: Flat (水平)", theme))
				{
					SetSelectedTangentFlat();
					host.MarkDocumentDirty();
					ctx.CloseAllPopups();
				}
				if (Wui::ContextMenuItem(ctx, Wui::HashId("ctx.linear"), nextItem(), "Tangent: Linear (直线)", theme))
				{
					SetSelectedTangentLinear();
					host.MarkDocumentDirty();
					ctx.CloseAllPopups();
				}
				if (Wui::ContextMenuItem(ctx, Wui::HashId("ctx.smooth"), nextItem(), "Tangent: Smooth (平滑)", theme))
				{
					SetSelectedTangentSmooth();
					host.MarkDocumentDirty();
					ctx.CloseAllPopups();
				}
				if (Wui::ContextMenuItem(ctx, Wui::HashId("ctx.step"), nextItem(), "Tangent: Step (阶梯)", theme))
				{
					SetSelectedTangentStep();
					host.MarkDocumentDirty();
					ctx.CloseAllPopups();
				}

				Wui::ContextMenuSeparator(ctx, nextItem(), theme);

				auto& kf = m_Curve.GetKeyframeMut(m_SelectedKeyframeIndex);
				if (Wui::ContextMenuToggleItem(ctx, Wui::HashId("ctx.mode_aligned"), nextItem(), "Mode: Aligned (对齐)", kf.Mode == TangentMode::Aligned, theme))
				{
					kf.Mode = TangentMode::Aligned;
					kf.InTangent = kf.OutTangent;
					m_Curve.PrecomputeLut(256);
					m_IsDirty = true;
					UpdateTitle();
					host.MarkDocumentDirty();
					ctx.CloseAllPopups();
				}
				if (Wui::ContextMenuToggleItem(ctx, Wui::HashId("ctx.mode_broken"), nextItem(), "Mode: Broken (折线)", kf.Mode == TangentMode::Broken, theme))
				{
					kf.Mode = TangentMode::Broken;
					m_IsDirty = true;
					UpdateTitle();
					host.MarkDocumentDirty();
					ctx.CloseAllPopups();
				}

				Wui::ContextMenuSeparator(ctx, nextItem(), theme);

				if (Wui::ContextMenuItem(ctx, Wui::HashId("ctx.del"), nextItem(), "Delete Keyframe (删除)", theme))
				{
					m_Curve.RemoveKeyframe(m_SelectedKeyframeIndex);
					m_Curve.PrecomputeLut(256);
					if (m_SelectedKeyframeIndex >= static_cast<int>(m_Curve.KeyframeCount()))
						m_SelectedKeyframeIndex = static_cast<int>(m_Curve.KeyframeCount()) - 1;
					m_IsDirty = true;
					UpdateTitle();
					host.MarkDocumentDirty();
					ctx.CloseAllPopups();
				}
			}
			else
			{
				// 画布空白处右键操作
				char addLabel[64];
				std::snprintf(addLabel, sizeof(addLabel), "Add Keyframe (t=%.2f, v=%.2f)", m_ContextMenuTime, m_ContextMenuValue);
				if (Wui::ContextMenuItem(ctx, Wui::HashId("ctx.add_key"), nextItem(), addLabel, theme))
				{
					float addT = m_ContextMenuTime;
					float addV = m_ContextMenuValue;
					if (m_SnapToGrid)
					{
						addT = std::round(addT / m_SnapTimeStep) * m_SnapTimeStep;
						addV = std::round(addV / m_SnapValStep) * m_SnapValStep;
					}
					Keyframe newKf(addT, addV, 0.0f, 0.0f, TangentMode::Aligned);
					size_t newIdx = m_Curve.AddKeyframe(newKf);
					m_Curve.PrecomputeLut(256);
					m_SelectedKeyframeIndex = static_cast<int>(newIdx);
					m_IsDirty = true;
					UpdateTitle();
					host.MarkDocumentDirty();
					ctx.CloseAllPopups();
				}

				Wui::ContextMenuSeparator(ctx, nextItem(), theme);

				if (Wui::ContextMenuItem(ctx, Wui::HashId("ctx.frame_all"), nextItem(), "Frame All (全景居中 - F)", theme))
				{
					FrameAll();
					ctx.CloseAllPopups();
				}

				Wui::ContextMenuSeparator(ctx, nextItem(), theme);

				if (Wui::ContextMenuItem(ctx, Wui::HashId("ctx.preset_linear"), nextItem(), "Preset: Linear (线性)", theme))
				{
					m_Curve = CubicBezierCurve::Linear();
					m_SelectedKeyframeIndex = 0;
					m_IsDirty = true;
					UpdateTitle();
					FrameAll();
					host.MarkDocumentDirty();
					ctx.CloseAllPopups();
				}
				if (Wui::ContextMenuItem(ctx, Wui::HashId("ctx.preset_ease_in"), nextItem(), "Preset: Ease In (缓入)", theme))
				{
					m_Curve = CubicBezierCurve::EaseIn();
					m_SelectedKeyframeIndex = 0;
					m_IsDirty = true;
					UpdateTitle();
					FrameAll();
					host.MarkDocumentDirty();
					ctx.CloseAllPopups();
				}
				if (Wui::ContextMenuItem(ctx, Wui::HashId("ctx.preset_ease_out"), nextItem(), "Preset: Ease Out (缓出)", theme))
				{
					m_Curve = CubicBezierCurve::EaseOut();
					m_SelectedKeyframeIndex = 0;
					m_IsDirty = true;
					UpdateTitle();
					FrameAll();
					host.MarkDocumentDirty();
					ctx.CloseAllPopups();
				}
				if (Wui::ContextMenuItem(ctx, Wui::HashId("ctx.preset_ease_in_out"), nextItem(), "Preset: Ease In-Out (缓入缓出)", theme))
				{
					m_Curve = CubicBezierCurve::EaseInOut();
					m_SelectedKeyframeIndex = 0;
					m_IsDirty = true;
					UpdateTitle();
					FrameAll();
					host.MarkDocumentDirty();
					ctx.CloseAllPopups();
				}
				if (Wui::ContextMenuItem(ctx, Wui::HashId("ctx.preset_bounce"), nextItem(), "Preset: Bounce (弹跳)", theme))
				{
					m_Curve = CubicBezierCurve::Bounce();
					m_SelectedKeyframeIndex = 0;
					m_IsDirty = true;
					UpdateTitle();
					FrameAll();
					host.MarkDocumentDirty();
					ctx.CloseAllPopups();
				}
			}

			Wui::EndContextMenu(ctx, contextMenuId, menuPanel, theme);
		}
	}
}
