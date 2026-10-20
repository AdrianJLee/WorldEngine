#include "WUI/Panels/FoliagePanel.h"
#include "World/WUI/WuiWidgets.h"
#include "World/Core/Log.h"

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

			FoliageType grass;
			grass.Name = "0: 杂草灌木 (Grass Clump)";
			grass.Density = 3.0f;
			grass.ScaleMinMax = glm::vec2(0.8f, 1.3f);
			grass.SlopeLimit = glm::vec2(0.0f, 40.0f);
			grass.CullDistance = 150.0f;
			grass.AlignToNormal = true;
			m_Foliage->AddType(grass);

			FoliageType flower;
			flower.Name = "1: 野花草丛 (Wild Flowers)";
			flower.Density = 1.0f;
			flower.ScaleMinMax = glm::vec2(0.6f, 1.0f);
			flower.SlopeLimit = glm::vec2(0.0f, 30.0f);
			flower.CullDistance = 100.0f;
			flower.AlignToNormal = true;
			m_Foliage->AddType(flower);

			FoliageType rock;
			rock.Name = "2: 地表碎石 (Scatter Rocks)";
			rock.Density = 0.5f;
			rock.ScaleMinMax = glm::vec2(0.5f, 2.0f);
			rock.SlopeLimit = glm::vec2(0.0f, 60.0f);
			rock.CullDistance = 200.0f;
			rock.AlignToNormal = true;
			m_Foliage->AddType(rock);
		}
		m_StatusFeedback = "植被调色板已初始化，当前共有 3 种生态预设。";
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

	bool FoliagePanel::OnShortcut(uint32_t, bool, bool, bool)
	{
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

		// 顶部工具选项卡
		const std::vector<std::string> tools = { "散布笔刷 (Paint)", "擦除工具 (Erase)", "生态配置 (Settings)" };
		if (Wui::TabBar(ctx, Wui::HashId("foliage.tabs"), { startX, curY, contentW, 28.0f }, tools, m_ToolIdx, theme))
		{
			m_Tool = static_cast<FoliageBrushTool>(m_ToolIdx);
		}
		curY += 34.0f;

		switch (m_Tool)
		{
		case FoliageBrushTool::Paint:
			RenderPaintTab(ctx, startX, curY, contentW, theme, host);
			break;
		case FoliageBrushTool::Erase:
			RenderEraseTab(ctx, startX, curY, contentW, theme, host);
			break;
		case FoliageBrushTool::Settings:
			RenderSettingsTab(ctx, startX, curY, contentW, theme, host);
			break;
		}

		// 底部状态信息
		const size_t totalInstances = m_Foliage ? m_Foliage->GetTotalInstanceCount() : 0;
		const std::string fullStatus = m_StatusFeedback + " [全图植被: " + std::to_string(totalInstances) + " 颗]";
		Wui::Label(ctx, { startX, rect.Y + rect.H - 24.0f }, fullStatus, theme.TextMuted, theme.FontSizeSmall);
	}

	void FoliagePanel::RenderPaintTab(Wui::WuiContext& ctx, float startX, float& curY, float contentW, const Wui::WuiTheme& theme, PanelHost&)
	{
		const float rowH = theme.RowHeight;

		Wui::Label(ctx, { startX, curY + 4.0f }, "植被散布调色板 (Foliage Palette)", theme.Accent, theme.FontSizeBody);
		curY += rowH;

		// 类型选择
		std::vector<std::string> typeNames;
		if (m_Foliage)
		{
			for (size_t i = 0; i < m_Foliage->GetTypeCount(); ++i)
				typeNames.push_back(m_Foliage->GetType(i).Name);
		}
		if (typeNames.empty())
			typeNames.push_back("无预设");

		if (Wui::Segmented(ctx, Wui::HashId("foliage.types.sel"), { startX, curY, contentW, 26.0f }, typeNames, m_ActiveTypeIdx, theme))
		{
			if (m_Foliage && static_cast<size_t>(m_ActiveTypeIdx) < m_Foliage->GetTypeCount())
			{
				const auto& t = m_Foliage->GetType(m_ActiveTypeIdx);
				m_ScaleMin = t.ScaleMinMax.x;
				m_ScaleMax = t.ScaleMinMax.y;
				m_SlopeMax = t.SlopeLimit.y;
				m_CullDist = t.CullDistance;
				m_AlignNormal = t.AlignToNormal;
			}
		}
		curY += 34.0f;

		// 笔刷参数
		Wui::Label(ctx, { startX, curY + 4.0f }, "散布半径 (Radius m):", theme.Text, theme.FontSizeSmall);
		Wui::DragFloat(ctx, Wui::HashId("foliage.brush.radius"), { startX + 130.0f, curY, contentW - 130.0f, 22.0f }, m_BrushRadius, 0.5f, 0.5f, 100.0f, theme);
		curY += 28.0f;

		Wui::Label(ctx, { startX, curY + 4.0f }, "散布密度 (Density):", theme.Text, theme.FontSizeSmall);
		Wui::DragFloat(ctx, Wui::HashId("foliage.brush.density"), { startX + 130.0f, curY, contentW - 130.0f, 22.0f }, m_BrushDensity, 0.1f, 0.1f, 20.0f, theme);
		curY += 28.0f;

		Wui::Label(ctx, { startX, curY + 4.0f }, "最大坡度 (Max Slope °):", theme.Text, theme.FontSizeSmall);
		Wui::DragFloat(ctx, Wui::HashId("foliage.slope.max"), { startX + 130.0f, curY, contentW - 130.0f, 22.0f }, m_SlopeMax, 1.0f, 0.0f, 90.0f, theme);
		curY += 28.0f;

		Wui::Label(ctx, { startX, curY + 4.0f }, "对齐法线 (Align Normal):", theme.Text, theme.FontSizeSmall);
		Wui::Checkbox(ctx, Wui::HashId("foliage.align.norm"), { startX + 130.0f, curY, 80.0f, 22.0f }, "开启", m_AlignNormal, theme);
		curY += 34.0f;

		// 批量模拟散布按钮
		if (Wui::Button(ctx, Wui::HashId("foliage.btn.scatter_sample"), { startX, curY, contentW, 26.0f }, "全关卡程序化散布 (Scatter All)", theme))
		{
			if (m_Foliage)
			{
				uint32_t total = 0;
				for (float z = 20.0f; z < 236.0f; z += 40.0f)
				{
					for (float x = 20.0f; x < 236.0f; x += 40.0f)
					{
						total += m_Foliage->PaintFoliage(glm::vec3(x, 0.0f, z), 20.0f, m_BrushDensity, static_cast<size_t>(m_ActiveTypeIdx), nullptr);
					}
				}
				m_StatusFeedback = "已在场景中散布 " + std::to_string(total) + " 颗植被实例。";
			}
		}
		curY += 32.0f;
	}

	void FoliagePanel::RenderEraseTab(Wui::WuiContext& ctx, float startX, float& curY, float contentW, const Wui::WuiTheme& theme, PanelHost&)
	{
		const float rowH = theme.RowHeight;

		Wui::Label(ctx, { startX, curY + 4.0f }, "植被橡皮擦 (Foliage Eraser)", theme.Accent, theme.FontSizeBody);
		curY += rowH;

		Wui::Label(ctx, { startX, curY + 4.0f }, "擦除半径 (Erase Radius):", theme.Text, theme.FontSizeSmall);
		Wui::DragFloat(ctx, Wui::HashId("foliage.erase.radius"), { startX + 130.0f, curY, contentW - 130.0f, 22.0f }, m_BrushRadius, 0.5f, 0.5f, 100.0f, theme);
		curY += 32.0f;

		if (Wui::Button(ctx, Wui::HashId("foliage.btn.clear_all"), { startX, curY, contentW, 26.0f }, "清空场景全部植被 (Clear All)", theme))
		{
			if (m_Foliage)
			{
				m_Foliage->Clear();
				m_StatusFeedback = "已彻底清空场景内所有植被实例。";
			}
		}
		curY += 32.0f;
	}

	void FoliagePanel::RenderSettingsTab(Wui::WuiContext& ctx, float startX, float& curY, float contentW, const Wui::WuiTheme& theme, PanelHost&)
	{
		const float rowH = theme.RowHeight;

		Wui::Label(ctx, { startX, curY + 4.0f }, "生态资产存储 (Foliage Storage)", theme.Accent, theme.FontSizeBody);
		curY += rowH;

		if (Wui::Button(ctx, Wui::HashId("foliage.btn.save_asset"), { startX, curY, contentW, 26.0f }, "保存到 default.wfoliage", theme))
		{
			std::filesystem::create_directories("projects/default/assets/foliage");
			SaveFoliageAsset("projects/default/assets/foliage/default.wfoliage");
		}
		curY += 32.0f;

		if (Wui::Button(ctx, Wui::HashId("foliage.btn.load_asset"), { startX, curY, contentW, 26.0f }, "从 default.wfoliage 重新载入", theme))
		{
			LoadFoliageAsset("projects/default/assets/foliage/default.wfoliage");
		}
		curY += 32.0f;

		if (Wui::Button(ctx, Wui::HashId("foliage.btn.reset_palette"), { startX, curY, contentW, 26.0f }, "重置默认生态预设 (Reset)", theme))
		{
			ResetDefaultPalette();
		}
		curY += 32.0f;
	}
}
