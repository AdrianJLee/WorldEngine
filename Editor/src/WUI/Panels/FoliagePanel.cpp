#include "World/Scene/Components.h"
#include "World/Foliage/FoliageRenderer.h"
#include "WUI/Panels/FoliagePanel.h"
#include "World/WUI/WuiWidgets.h"
#include "World/Core/Log.h"
#include "World/Core/KeyCodes.h"

#include <filesystem>

namespace World
{
	FoliagePanel::FoliagePanel()
	{
		ResetDefaultPalette();
	}

	void FoliagePanel::ResetDefaultPalette()
	{
		m_Foliage = std::make_shared<FoliageData>();
		if (m_Foliage)
		{
			m_Foliage->Clear();
			m_Foliage->RemoveType(0);

			FoliageType grass;
			grass.Name = "0: 翠绿草丛 (Wild Grass)";
			grass.Density = 3.0f;
			grass.ScaleMinMax = glm::vec2(0.8f, 1.3f);
			grass.SlopeLimit = glm::vec2(0.0f, 45.0f);
			grass.CullDistance = 150.0f;
			grass.AlignToNormal = true;
			m_Foliage->AddType(grass);

			FoliageType flower;
			flower.Name = "1: 雏菊野花 (Wild Flowers)";
			flower.Density = 1.0f;
			flower.ScaleMinMax = glm::vec2(0.6f, 1.0f);
			flower.SlopeLimit = glm::vec2(0.0f, 35.0f);
			flower.CullDistance = 120.0f;
			flower.AlignToNormal = true;
			m_Foliage->AddType(flower);

			FoliageType shrub;
			shrub.Name = "2: 矮生灌木 (Forest Shrub)";
			shrub.Density = 0.6f;
			shrub.ScaleMinMax = glm::vec2(0.8f, 1.8f);
			shrub.SlopeLimit = glm::vec2(0.0f, 40.0f);
			shrub.CullDistance = 180.0f;
			shrub.AlignToNormal = true;
			m_Foliage->AddType(shrub);

			FoliageType rock;
			rock.Name = "3: 地表碎石 (Scatter Rocks)";
			rock.Density = 0.4f;
			rock.ScaleMinMax = glm::vec2(0.5f, 2.0f);
			rock.SlopeLimit = glm::vec2(0.0f, 60.0f);
			rock.CullDistance = 200.0f;
			rock.AlignToNormal = true;
			m_Foliage->AddType(rock);
		}
		m_StatusFeedback = "植被生态调色板已就绪，预置 4 种自然植被与碎石。";
	}

	bool FoliagePanel::LoadFoliageAsset(const std::string& path)
	{
		if (!m_Foliage)
			m_Foliage = std::make_shared<FoliageData>();

		if (m_Foliage->LoadFromFile(path))
		{
			m_LoadedPath = path;
			m_StatusFeedback = "已载入植被资产: " + path + " (实例总数: " + std::to_string(m_Foliage->GetTotalInstanceCount()) + ")";
			return true;
		}
		m_StatusFeedback = "载入植被资产失败: " + path;
		return false;
	}

	bool FoliagePanel::SaveFoliageAsset(const std::string& path)
	{
		if (!m_Foliage)
			return false;

		if (m_Foliage->SaveToFile(path))
		{
			m_LoadedPath = path;
			m_StatusFeedback = "成功保存植被资产: " + path + " (实例总数: " + std::to_string(m_Foliage->GetTotalInstanceCount()) + ")";
			return true;
		}
		m_StatusFeedback = "保存植被资产失败: " + path;
		return false;
	}

	bool FoliagePanel::OnShortcut(uint32_t keyCode, bool ctrl, bool shift, bool alt)
	{
		(void)ctrl;
		(void)alt;
		if (keyCode == KeyCodes::LeftBracket)
		{
			if (shift)
				SetBrushDensity(m_BrushDensity - 0.2f);
			else
				SetBrushRadius(m_BrushRadius - 1.0f);
			return true;
		}
		else if (keyCode == KeyCodes::RightBracket)
		{
			if (shift)
				SetBrushDensity(m_BrushDensity + 0.2f);
			else
				SetBrushRadius(m_BrushRadius + 1.0f);
			return true;
		}
		return false;
	}

	void FoliagePanel::OnRender(Wui::WuiContext& ctx, const Wui::WuiRect& rect, PanelHost& host)
	{
		const Wui::WuiTheme& theme = host.Theme();
		Wui::Panel(ctx, rect, Title(), theme);

		const float pad = theme.Pad;
		const float startX = rect.X + pad;
		const float contentW = rect.W - pad * 2.0f;
		float curY = rect.Y + 32.0f + pad;

		// 双向绑定场景 FoliageComponent
		Ref<Scene> activeScene = host.GetActiveScene();
		if (activeScene)
		{
			for (auto [handle, comp] : activeScene->GetRegistry().view<FoliageComponent>().each())
			{
				m_Foliage = FoliageRenderer::GetOrCreateFoliageData(comp.FoliageAsset);
				break;
			}
		}

		// 顶部统计
		const size_t totalInstances = m_Foliage ? m_Foliage->GetTotalInstanceCount() : 0;
		char statBuf[128] = {};
		std::snprintf(statBuf, sizeof(statBuf), "全图植被: %zu 颗实例 | 生态种类: %zu 种",
			totalInstances, m_Foliage ? m_Foliage->GetTypeCount() : 0);
		Wui::Label(ctx, { startX, curY }, statBuf, theme.TextMuted, theme.FontSizeSmall);
		curY += 22.0f;

		// 选项卡
		const std::vector<std::string> tools = { "生态调色板", "风场与优化" };
		if (Wui::TabBar(ctx, Wui::HashId("foliage.tabs"), { startX, curY, contentW, 28.0f }, tools, m_ToolIdx, theme))
		{
			m_Tool = (m_ToolIdx == 0) ? FoliageBrushTool::Paint : FoliageBrushTool::Settings;
		}
		curY += 34.0f;

		if (m_ToolIdx == 0)
			RenderPaintTab(ctx, startX, curY, contentW, theme, host);
		else
			RenderSettingsTab(ctx, startX, curY, contentW, theme, host);

		// 底部状态信息
		if (!m_StatusFeedback.empty())
		{
			Wui::Label(ctx, { startX, rect.Y + rect.H - 24.0f }, m_StatusFeedback, theme.TextMuted, theme.FontSizeSmall);
		}
	}

	void FoliagePanel::RenderPaintTab(Wui::WuiContext& ctx, float startX, float& curY, float contentW, const Wui::WuiTheme& theme, PanelHost&)
	{
		const float rowH = theme.RowHeight;

		Wui::Label(ctx, { startX, curY + 4.0f }, "植被生态调色板 (Foliage Ecosystem Palette)", theme.Accent, theme.FontSizeBody);
		curY += rowH;

		// 4 种植被生态 2x2 网格卡片
		const size_t typeCount = m_Foliage ? m_Foliage->GetTypeCount() : 0;
		const float colW = (contentW - theme.PadSmall) * 0.5f;
		const float cardH = 26.0f;
		const char* shortNames[4] = { "✦ 0: 翠绿草丛", "❀ 1: 雏菊野花", "♣ 2: 矮生灌木", "▲ 3: 地表碎石" };

		for (size_t i = 0; i < typeCount && i < 4; ++i)
		{
			const auto& t = m_Foliage->GetType(i);
			const int row = static_cast<int>(i / 2);
			const int col = static_cast<int>(i % 2);
			const Wui::WuiRect cardRect {
				startX + static_cast<float>(col) * (colW + theme.PadSmall),
				curY + static_cast<float>(row) * (cardH + 4.0f),
				colW, cardH
			};
			auto cardTheme = theme;
			const bool isSelected = (m_ActiveTypeIdx == static_cast<int>(i));
			if (isSelected)
			{
				cardTheme.ButtonBg = theme.ActiveBg;
				cardTheme.Border = theme.Accent;
			}

			if (Wui::Button(ctx, Wui::HashId(("foliage.type.card." + std::to_string(i)).c_str()), cardRect, shortNames[i], cardTheme))
			{
				m_ActiveTypeIdx = static_cast<int>(i);
				m_ScaleMin = t.ScaleMinMax.x;
				m_ScaleMax = t.ScaleMinMax.y;
				m_SlopeMax = t.SlopeLimit.y;
				m_CullDist = t.CullDistance;
				m_AlignNormal = t.AlignToNormal;
			}
		}
		curY += 2.0f * (cardH + 4.0f) + 6.0f;

		curY += 4.0f;
		// 笔刷参数
		Wui::Label(ctx, { startX, curY + 4.0f }, "半径 (m):", theme.Text, theme.FontSizeSmall);
		Wui::DragFloat(ctx, Wui::HashId("foliage.brush.radius"), { startX + 70.0f, curY, contentW - 70.0f, 22.0f }, m_BrushRadius, 0.5f, 0.5f, 100.0f, theme);
		curY += 28.0f;

		Wui::Label(ctx, { startX, curY + 4.0f }, "密度:", theme.Text, theme.FontSizeSmall);
		Wui::DragFloat(ctx, Wui::HashId("foliage.brush.density"), { startX + 70.0f, curY, contentW - 70.0f, 22.0f }, m_BrushDensity, 0.1f, 0.1f, 10.0f, theme);
		curY += 28.0f;

		// 选中生态项细节调整
		if (m_Foliage && static_cast<size_t>(m_ActiveTypeIdx) < m_Foliage->GetTypeCount())
		{
			auto& t = const_cast<FoliageType&>(m_Foliage->GetType(m_ActiveTypeIdx));
			Wui::Label(ctx, { startX, curY + 4.0f }, "坡度 (°):", theme.Text, theme.FontSizeSmall);
			if (Wui::DragFloat(ctx, Wui::HashId("foliage.slope.max"), { startX + 70.0f, curY, contentW - 70.0f, 22.0f }, m_SlopeMax, 1.0f, 0.0f, 90.0f, theme))
			{
				t.SlopeLimit.y = m_SlopeMax;
			}
			curY += 28.0f;

			Wui::Label(ctx, { startX, curY + 4.0f }, "对齐法线:", theme.Text, theme.FontSizeSmall);
			if (Wui::Checkbox(ctx, Wui::HashId("foliage.align.norm"), { startX + 70.0f, curY, 80.0f, 22.0f }, "开启", m_AlignNormal, theme))
			{
				t.AlignToNormal = m_AlignNormal;
			}
			curY += 32.0f;
		}

		// 操作提示药丸
		Wui::Label(ctx, { startX, curY }, "操作指南: 视口左键拖拽植树种草 | Shift+左键拖拽擦除 | [ / ] 调半径", theme.TextMuted, theme.FontSizeSmall);
		curY += 26.0f;

		// 清理与持久化按钮
		const float halfW = (contentW - theme.PadSmall) * 0.5f;
		if (Wui::Button(ctx, Wui::HashId("foliage.btn.clear_type"), { startX, curY, halfW, 28.0f }, "清空选中类型", theme))
		{
			if (m_Foliage)
			{
				m_Foliage->EraseFoliage(glm::vec3(0.0f), 100000.0f, static_cast<int32_t>(m_ActiveTypeIdx));
				m_StatusFeedback = "已清空选中类型的植被。";
			}
		}

		if (Wui::Button(ctx, Wui::HashId("foliage.btn.clear_all"), { startX + halfW + theme.PadSmall, curY, halfW, 28.0f }, "清空全图植被", theme))
		{
			if (m_Foliage)
			{
				m_Foliage->Clear();
				m_StatusFeedback = "已彻底清空场景内所有植被实例。";
			}
		}
		curY += 34.0f;

		if (Wui::Button(ctx, Wui::HashId("foliage.btn.save_asset"), { startX, curY, contentW, 28.0f }, "保存到 default.wfoliage 资产", theme))
		{
			SaveFoliageAsset("projects/assets/foliage/default.wfoliage");
		}
		curY += 34.0f;
	}

	void FoliagePanel::RenderEraseTab(Wui::WuiContext&, float, float&, float, const Wui::WuiTheme&, PanelHost&)
	{
	}

	void FoliagePanel::RenderSettingsTab(Wui::WuiContext& ctx, float startX, float& curY, float contentW, const Wui::WuiTheme& theme, PanelHost&)
	{
		const float rowH = theme.RowHeight;

		Wui::Label(ctx, { startX, curY + 4.0f }, "风场扰动与视距优化 (Wind & Optimization)", theme.Accent, theme.FontSizeBody);
		curY += rowH;

		static float windSpeed = 1.0f;
		static float windStrength = 0.5f;
		static float maxCullDist = 150.0f;

		Wui::Label(ctx, { startX, curY + 4.0f }, "风场速度 (Wind Speed):", theme.Text, theme.FontSizeSmall);
		Wui::DragFloat(ctx, Wui::HashId("foliage.wind.speed"), { startX + 130.0f, curY, contentW - 130.0f, 22.0f }, windSpeed, 0.05f, 0.0f, 5.0f, theme);
		curY += 28.0f;

		Wui::Label(ctx, { startX, curY + 4.0f }, "风场强度 (Strength):", theme.Text, theme.FontSizeSmall);
		Wui::DragFloat(ctx, Wui::HashId("foliage.wind.strength"), { startX + 130.0f, curY, contentW - 130.0f, 22.0f }, windStrength, 0.02f, 0.0f, 2.0f, theme);
		curY += 28.0f;

		Wui::Label(ctx, { startX, curY + 4.0f }, "视距剔除 (Cull Dist m):", theme.Text, theme.FontSizeSmall);
		Wui::DragFloat(ctx, Wui::HashId("foliage.cull.dist"), { startX + 130.0f, curY, contentW - 130.0f, 22.0f }, maxCullDist, 5.0f, 30.0f, 500.0f, theme);
		curY += 34.0f;

		Wui::Label(ctx, { startX, curY }, "视距优化采用 Screen-Door 随机抖动晶格平滑过渡消隐，杜绝 LOD 突变撕裂。", theme.TextMuted, theme.FontSizeSmall);
		curY += 24.0f;
	}
}
