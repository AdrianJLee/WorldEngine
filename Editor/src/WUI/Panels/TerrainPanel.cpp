#include "WUI/Panels/TerrainPanel.h"
#include "World/WUI/WuiWidgets.h"
#include "World/Core/Log.h"

#include <cmath>
#include <filesystem>
#include <iostream>

namespace World
{
	TerrainPanel::TerrainPanel()
	{
		CreateDefaultTerrain(256, 256, 1.0f);
	}

	void TerrainPanel::CreateDefaultTerrain(uint32_t width, uint32_t height, float cellSize)
	{
		m_Terrain = std::make_shared<TerrainData>(width, height, cellSize, 1.0f);
		if (m_Terrain)
		{
			m_Terrain->GetLayer(0).Name = "Grass (草地)";
			m_Terrain->GetLayer(1).Name = "Dirt (泥土)";
			m_Terrain->GetLayer(2).Name = "Rock (岩石)";
			m_Terrain->GetLayer(3).Name = "Sand (沙地)";
		}
		m_Dirty = false;
		m_StatusFeedback = "已初始化 " + std::to_string(width) + "x" + std::to_string(height) + " 默认平坦地形。";
	}

	bool TerrainPanel::LoadTerrain(const std::string& path)
	{
		if (!m_Terrain)
			m_Terrain = std::make_shared<TerrainData>();

		if (m_Terrain->LoadFromFile(path))
		{
			m_LoadedPath = path;
			m_Dirty = false;
			m_StatusFeedback = "成功载入地形资产: " + path;
			return true;
		}
		m_StatusFeedback = "载入地形失败: " + path;
		return false;
	}

	bool TerrainPanel::SaveTerrain(const std::string& path)
	{
		if (!m_Terrain)
			return false;

		if (m_Terrain->SaveToFile(path))
		{
			m_LoadedPath = path;
			m_Dirty = false;
			m_StatusFeedback = "成功保存地形资产: " + path;
			return true;
		}
		m_StatusFeedback = "保存地形失败: " + path;
		return false;
	}

	bool TerrainPanel::OnShortcut(uint32_t keyCode, bool ctrl, bool shift, bool alt)
	{
		(void)keyCode;
		(void)ctrl;
		(void)shift;
		(void)alt;
		return false;
	}

	void TerrainPanel::OnRender(Wui::WuiContext& ctx, const Wui::WuiRect& rect, PanelHost& host)
	{
		const Wui::WuiTheme& theme = host.Theme();
		Wui::Panel(ctx, rect, Title(), theme);

		const float pad = theme.Pad;
		const float rowH = theme.RowHeight;
		const float startX = rect.X + pad;
		const float contentW = rect.W - pad * 2.0f;
		float curY = rect.Y + 32.0f + pad;

		// 顶部选项卡
		const std::vector<std::string> tabs = { "雕刻 (Sculpt)", "混绘 (Paint)", "设置 (Settings)" };
		if (Wui::TabBar(ctx, Wui::HashId("terrain.tabs"), { startX, curY, contentW, 28.0f }, tabs, m_ActiveTabIdx, theme))
		{
			m_ActiveTab = static_cast<TerrainToolTab>(m_ActiveTabIdx);
		}
		curY += 34.0f;

		// 分流页面渲染
		switch (m_ActiveTab)
		{
		case TerrainToolTab::Sculpt:
			RenderSculptTab(ctx, startX, curY, contentW, theme, host);
			break;
		case TerrainToolTab::Paint:
			RenderPaintTab(ctx, startX, curY, contentW, theme, host);
			break;
		case TerrainToolTab::Settings:
			RenderSettingsTab(ctx, startX, curY, contentW, theme, host);
			break;
		}

		// 底部状态信息栏
		if (!m_StatusFeedback.empty())
		{
			const float statusY = rect.Y + rect.H - 24.0f;
			Wui::Label(ctx, { startX, statusY }, m_StatusFeedback, theme.TextMuted, theme.FontSizeSmall);
		}
	}

	void TerrainPanel::RenderSculptTab(Wui::WuiContext& ctx, float startX, float& currentY, float contentWidth, const Wui::WuiTheme& theme, PanelHost&)
	{
		const float rowH = theme.RowHeight;
		const float halfW = (contentWidth - theme.PadSmall) * 0.5f;

		Wui::Label(ctx, { startX, currentY + 4.0f }, "雕刻工具 (Sculpt Mode)", theme.Accent, theme.FontSizeBody);
		currentY += rowH;

		const std::vector<std::string> sculptTools = { "抬升", "下凹", "平滑", "压平", "侵蚀", "地洞" };
		if (Wui::Segmented(ctx, Wui::HashId("terrain.sculpt.tools"), { startX, currentY, contentWidth, 26.0f }, sculptTools, m_SculptModeIdx, theme))
		{
			m_SculptMode = static_cast<TerrainSculptMode>(m_SculptModeIdx);
		}
		currentY += 32.0f;

		// 笔刷半径与强度
		Wui::Label(ctx, { startX, currentY + 4.0f }, "笔刷半径 (Radius):", theme.Text, theme.FontSizeSmall);
		Wui::DragFloat(ctx, Wui::HashId("terrain.brush.radius"), { startX + 130.0f, currentY, contentWidth - 130.0f, 22.0f }, m_BrushRadius, 0.5f, 0.5f, 100.0f, theme);
		currentY += 28.0f;

		Wui::Label(ctx, { startX, currentY + 4.0f }, "笔刷强度 (Strength):", theme.Text, theme.FontSizeSmall);
		Wui::DragFloat(ctx, Wui::HashId("terrain.brush.strength"), { startX + 130.0f, currentY, contentWidth - 130.0f, 22.0f }, m_BrushStrength, 0.01f, 0.01f, 1.0f, theme);
		currentY += 28.0f;

		// 衰减曲线
		const std::vector<std::string> falloffTypes = { "Linear 线性", "Smooth 平滑", "Sharp 尖锐", "Constant 平头" };
		Wui::Label(ctx, { startX, currentY + 4.0f }, "衰减曲线 (Falloff):", theme.Text, theme.FontSizeSmall);
		if (Wui::Combo(ctx, Wui::HashId("terrain.brush.falloff"), { startX + 130.0f, currentY, contentWidth - 130.0f, 22.0f }, "Falloff", falloffTypes, m_FalloffIdx, theme))
		{
			m_Falloff = static_cast<TerrainBrushFalloff>(m_FalloffIdx);
		}
		currentY += 32.0f;

		if (m_SculptMode == TerrainSculptMode::Flatten)
		{
			Wui::Label(ctx, { startX, currentY + 4.0f }, "目标高度 (Target Y):", theme.Text, theme.FontSizeSmall);
			Wui::DragFloat(ctx, Wui::HashId("terrain.target.height"), { startX + 130.0f, currentY, contentWidth - 130.0f, 22.0f }, m_TargetHeight, 0.5f, -100.0f, 1000.0f, theme);
			currentY += 28.0f;
		}

		// 快速生成与操作
		if (Wui::Button(ctx, Wui::HashId("terrain.gen.hills"), { startX, currentY, halfW, 26.0f }, "生成山丘地貌", theme))
		{
			if (m_Terrain)
			{
				const uint32_t w = m_Terrain->GetWidth();
				const uint32_t h = m_Terrain->GetHeight();
				for (uint32_t z = 0; z < h; ++z)
				{
					for (uint32_t x = 0; x < w; ++x)
					{
						const float fx = static_cast<float>(x) * 0.05f;
						const float fz = static_cast<float>(z) * 0.05f;
						const float elevation = std::sin(fx) * 8.0f + std::cos(fz) * 6.0f + std::sin(fx * 2.3f + fz * 1.7f) * 3.5f;
						m_Terrain->SetHeight(x, z, std::max(0.0f, elevation));
					}
				}
				m_StatusFeedback = "已程序化生成起伏山丘地貌。";
			}
		}

		if (Wui::Button(ctx, Wui::HashId("terrain.reset.flat"), { startX + halfW + theme.PadSmall, currentY, halfW, 26.0f }, "重置为平坦", theme))
		{
			if (m_Terrain)
			{
				const uint32_t w = m_Terrain->GetWidth();
				const uint32_t h = m_Terrain->GetHeight();
				for (uint32_t z = 0; z < h; ++z)
				{
					for (uint32_t x = 0; x < w; ++x)
					{
						m_Terrain->SetHeight(x, z, 0.0f);
					}
				}
				m_StatusFeedback = "已重置为平坦地形。";
			}
		}
		currentY += 34.0f;
	}

	void TerrainPanel::RenderPaintTab(Wui::WuiContext& ctx, float startX, float& currentY, float contentWidth, const Wui::WuiTheme& theme, PanelHost&)
	{
		const float rowH = theme.RowHeight;

		Wui::Label(ctx, { startX, currentY + 4.0f }, "地表混绘图层 (Splatmap Layers)", theme.Accent, theme.FontSizeBody);
		currentY += rowH;

		const std::vector<std::string> layers = { "0: 草地 (Grass)", "1: 泥土 (Dirt)", "2: 岩石 (Rock)", "3: 沙地 (Sand)" };
		if (Wui::Segmented(ctx, Wui::HashId("terrain.paint.layers"), { startX, currentY, contentWidth, 26.0f }, layers, m_PaintLayerIdx, theme))
		{
			m_ActivePaintLayer = static_cast<uint32_t>(m_PaintLayerIdx);
		}
		currentY += 34.0f;

		// 绘制当前图层属性说明
		if (m_Terrain && m_ActivePaintLayer < 4)
		{
			const TerrainLayer& layer = m_Terrain->GetLayer(m_ActivePaintLayer);
			Wui::Label(ctx, { startX, currentY }, "图层名称: " + layer.Name, theme.Text, theme.FontSizeSmall);
			currentY += 20.0f;
			Wui::Label(ctx, { startX, currentY }, "粗糙度: " + std::to_string(layer.Roughness) + "  金属度: " + std::to_string(layer.Metallic), theme.TextMuted, theme.FontSizeSmall);
			currentY += 20.0f;
		}

		Wui::Label(ctx, { startX, currentY + 4.0f }, "涂抹半径 (Radius):", theme.Text, theme.FontSizeSmall);
		Wui::DragFloat(ctx, Wui::HashId("terrain.paint.radius"), { startX + 130.0f, currentY, contentWidth - 130.0f, 22.0f }, m_BrushRadius, 0.5f, 0.5f, 100.0f, theme);
		currentY += 28.0f;

		Wui::Label(ctx, { startX, currentY + 4.0f }, "涂抹强度 (Strength):", theme.Text, theme.FontSizeSmall);
		Wui::DragFloat(ctx, Wui::HashId("terrain.paint.strength"), { startX + 130.0f, currentY, contentWidth - 130.0f, 22.0f }, m_BrushStrength, 0.01f, 0.01f, 1.0f, theme);
		currentY += 32.0f;
	}

	void TerrainPanel::RenderSettingsTab(Wui::WuiContext& ctx, float startX, float& currentY, float contentWidth, const Wui::WuiTheme& theme, PanelHost&)
	{
		const float rowH = theme.RowHeight;

		Wui::Label(ctx, { startX, currentY + 4.0f }, "地形规格配置 (Terrain Settings)", theme.Accent, theme.FontSizeBody);
		currentY += rowH;

		Wui::Label(ctx, { startX, currentY + 4.0f }, "分辨率 (Resolution):", theme.Text, theme.FontSizeSmall);
		Wui::DragInt(ctx, Wui::HashId("terrain.new.res"), { startX + 130.0f, currentY, contentWidth - 130.0f, 22.0f }, m_NewResolution, 64, 2048, theme);
		currentY += 28.0f;

		Wui::Label(ctx, { startX, currentY + 4.0f }, "格距 (Cell Size m):", theme.Text, theme.FontSizeSmall);
		Wui::DragFloat(ctx, Wui::HashId("terrain.new.cell"), { startX + 130.0f, currentY, contentWidth - 130.0f, 22.0f }, m_NewCellSize, 0.1f, 0.1f, 10.0f, theme);
		currentY += 32.0f;

		if (Wui::Button(ctx, Wui::HashId("terrain.btn.create"), { startX, currentY, contentWidth, 26.0f }, "新建地形 (Create New)", theme))
		{
			CreateDefaultTerrain(static_cast<uint32_t>(m_NewResolution), static_cast<uint32_t>(m_NewResolution), m_NewCellSize);
		}
		currentY += 32.0f;

		if (Wui::Button(ctx, Wui::HashId("terrain.btn.save"), { startX, currentY, contentWidth, 26.0f }, "保存到 default.wterrain", theme))
		{
			const std::string savePath = "projects/default/assets/terrains/default.wterrain";
			std::filesystem::create_directories("projects/default/assets/terrains");
			SaveTerrain(savePath);
		}
		currentY += 32.0f;
	}
}
