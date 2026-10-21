#include "WUI/Panels/TerrainPanel.h"
#include "World/WUI/WuiWidgets.h"
#include "World/Core/Log.h"
#include "World/Core/KeyCodes.h"
#include "World/Core/StringPool.h"
#include "World/Command/CommandHistory.h"
#include "World/Scene/Components.h"

#include <algorithm>
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
			m_Terrain->GetLayer(0).AlbedoTexture = AssetRef { StringPool::Get().InternPath("textures/terrain/grass_albedo.png"), AssetId{} };
			m_Terrain->GetLayer(0).NormalTexture = AssetRef { StringPool::Get().InternPath("textures/terrain/grass_normal.png"), AssetId{} };
			m_Terrain->GetLayer(0).Roughness = 0.8f;
			m_Terrain->GetLayer(0).Tiling = glm::vec2(10.0f, 10.0f);

			m_Terrain->GetLayer(1).Name = "Dirt (泥土)";
			m_Terrain->GetLayer(1).AlbedoTexture = AssetRef { StringPool::Get().InternPath("textures/terrain/dirt_albedo.png"), AssetId{} };
			m_Terrain->GetLayer(1).NormalTexture = AssetRef { StringPool::Get().InternPath("textures/terrain/dirt_normal.png"), AssetId{} };
			m_Terrain->GetLayer(1).Roughness = 0.9f;
			m_Terrain->GetLayer(1).Tiling = glm::vec2(10.0f, 10.0f);

			m_Terrain->GetLayer(2).Name = "Rock (岩石)";
			m_Terrain->GetLayer(2).AlbedoTexture = AssetRef { StringPool::Get().InternPath("textures/terrain/rock_albedo.png"), AssetId{} };
			m_Terrain->GetLayer(2).NormalTexture = AssetRef { StringPool::Get().InternPath("textures/terrain/rock_normal.png"), AssetId{} };
			m_Terrain->GetLayer(2).Roughness = 0.6f;
			m_Terrain->GetLayer(2).Tiling = glm::vec2(5.0f, 5.0f);

			m_Terrain->GetLayer(3).Name = "Sand (沙地)";
			m_Terrain->GetLayer(3).AlbedoTexture = AssetRef { StringPool::Get().InternPath("textures/terrain/sand_albedo.png"), AssetId{} };
			m_Terrain->GetLayer(3).NormalTexture = AssetRef { StringPool::Get().InternPath("textures/terrain/sand_normal.png"), AssetId{} };
			m_Terrain->GetLayer(3).Roughness = 0.9f;
			m_Terrain->GetLayer(3).Tiling = glm::vec2(10.0f, 10.0f);
		}
		m_Dirty = false;
		m_StatusFeedback = "已初始化 " + std::to_string(width) + "x" + std::to_string(height) + " 商业级地貌底座。";
	}

	bool TerrainPanel::LoadTerrain(const std::string& path)
	{
		if (!m_Terrain)
			m_Terrain = std::make_shared<TerrainData>();

		if (m_Terrain->LoadFromFile(path))
		{
			m_LoadedPath = path;
			m_Dirty = false;
			m_StatusFeedback = "成功载入地貌资产: " + path;
			return true;
		}
		m_StatusFeedback = "载入地貌失败: " + path;
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
			m_StatusFeedback = "成功保存地貌资产: " + path;
			return true;
		}
		m_StatusFeedback = "保存地貌失败: " + path;
		return false;
	}

	bool TerrainPanel::OnShortcut(uint32_t keyCode, bool ctrl, bool shift, bool alt)
	{
		(void)ctrl;
		(void)alt;
		if (keyCode == KeyCodes::LeftBracket)
		{
			if (shift)
				SetBrushStrength(m_BrushStrength - 0.05f);
			else
				SetBrushRadius(m_BrushRadius - 1.0f);
			return true;
		}
		else if (keyCode == KeyCodes::RightBracket)
		{
			if (shift)
				SetBrushStrength(m_BrushStrength + 0.05f);
			else
				SetBrushRadius(m_BrushRadius + 1.0f);
			return true;
		}
		return false;
	}

	void TerrainPanel::OnRender(Wui::WuiContext& ctx, const Wui::WuiRect& rect, PanelHost& host)
	{
		const Wui::WuiTheme& theme = host.Theme();
		Wui::Panel(ctx, rect, Title(), theme);

		const float pad = theme.Pad;
		const float startX = rect.X + pad;
		const float contentW = rect.W - pad * 2.0f;
		float curY = rect.Y + 32.0f + pad;

		// 1. 自动双向数据绑定：检查场景中是否存在绑定 TerrainComponent 的实体
		Ref<Scene> activeScene = host.GetActiveScene();
		m_BoundEntity = {};
		if (activeScene)
		{
			Entity selected = host.GetSelectedEntity();
			if (selected.IsValid() && selected.GetScene() == activeScene.get() && selected.HasComponent<TerrainComponent>())
			{
				m_BoundEntity = selected;
			}
			else
			{
				const auto& registry = activeScene->GetRegistry();
				for (auto [handle, comp] : registry.view<TerrainComponent>().each())
				{
					m_BoundEntity = Entity(activeScene.get(), handle);
					break;
				}
			}
		}

		if (m_BoundEntity.IsValid())
		{
			auto& comp = m_BoundEntity.GetComponent<TerrainComponent>();
			m_Terrain = TerrainRenderer::GetOrCreateTerrainData(comp.TerrainAsset, comp.CellSize, comp.HeightScale);
		}
		else
		{
			m_Terrain.reset();
		}

		// 2. 若场景不存在任何 TerrainComponent 实体，渲染新建地貌向导
		if (!m_BoundEntity.IsValid() || !m_Terrain)
		{
			RenderWizard(ctx, startX, curY, contentW, theme, host);
			if (!m_StatusFeedback.empty())
			{
				const float statusY = rect.Y + rect.H - 24.0f;
				Wui::Label(ctx, { startX, statusY }, m_StatusFeedback, theme.TextMuted, theme.FontSizeSmall);
			}
			return;
		}

		// 3. 顶部信息卡片 (实体名称与网格尺寸)
		std::string entityTag = "Landscape";
		if (m_BoundEntity.HasComponent<TagComponent>())
			entityTag = StringPool::Get().NameOf(m_BoundEntity.GetComponent<TagComponent>().Tag);
		char tagBuf[128] = {};
		std::snprintf(tagBuf, sizeof(tagBuf), "绑定实体: [%s]  规格: %ux%u (%.0fm x %.0fm)",
			entityTag.c_str(), m_Terrain->GetWidth(), m_Terrain->GetHeight(),
			m_Terrain->GetWorldWidth(), m_Terrain->GetWorldHeight());
		Wui::Label(ctx, { startX, curY }, tagBuf, theme.TextMuted, theme.FontSizeSmall);
		curY += 22.0f;

		// 极简三栏选项卡: [雕刻 (Sculpt)] [混绘 (Paint)] [管理 (Manage)]
		const std::vector<std::string> tabs = { "雕刻", "混绘", "管理" };
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

	void TerrainPanel::RenderWizard(Wui::WuiContext& ctx, float startX, float& currentY, float contentWidth, const Wui::WuiTheme& theme, PanelHost& host)
	{
		const float pad = theme.PadSmall;

		Wui::Label(ctx, { startX, currentY + 2.0f }, "✦ 快速创建 3D 地貌 (Create Landscape)", theme.Accent, theme.FontSizeBody);
		currentY += 26.0f;

		Wui::Label(ctx, { startX, currentY }, "当前场景尚未添加地貌。请选择关卡规格以一键生成：", theme.TextMuted, theme.FontSizeSmall);
		currentY += 22.0f;

		const struct PresetSpec { int64_t res; const char* label; } presets[3] = {
			{ 127, "127x127\n小型局部" },
			{ 255, "255x255\n标准关卡(推荐)" },
			{ 511, "511x511\n宏大开放世界" }
		};
		const float presetW = (contentWidth - 2.0f * pad) / 3.0f;
		const float presetH = 34.0f;
		for (int i = 0; i < 3; ++i)
		{
			const Wui::WuiRect btnRect { startX + static_cast<float>(i) * (presetW + pad), currentY, presetW, presetH };
			auto btnTheme = theme;
			if (m_WizardResolution == presets[i].res)
			{
				btnTheme.ButtonBg = theme.ActiveBg;
				btnTheme.Border = theme.Accent;
			}
			if (Wui::Button(ctx, Wui::HashId(("terrain.wizard.preset." + std::to_string(i)).c_str()), btnRect, presets[i].label, btnTheme))
			{
				m_WizardResolution = presets[i].res;
			}
		}
		currentY += presetH + 14.0f;

		// 醒目大按钮: 创建地貌实体并开始编辑
		const Wui::WuiRect createBtnRect { startX, currentY, contentWidth, 34.0f };
		auto createTheme = theme;
		createTheme.ButtonBg = theme.ActiveBg;
		createTheme.Border = theme.Accent;
		if (Wui::Button(ctx, Wui::HashId("terrain.wizard.create"), createBtnRect, "✦ 创建地貌实体并开始编辑 (Create Landscape)", createTheme))
		{
			Ref<Scene> scene = host.GetActiveScene();
			if (scene)
			{
				Entity created;
				const uint32_t res = static_cast<uint32_t>(m_WizardResolution);
				const float cell = m_WizardCellSize;
				const float hScale = m_WizardHeightScale;
				if (scene->DeferStructuralChange([&created, res, cell, hScale](Scene& s)
					{
						created = Entity::CreateEntity(&s, "Landscape");
						created.AddComponent<TransformComponent>();
						auto& comp = created.AddComponent<TerrainComponent>();
						comp.TerrainAsset = AssetRef { StringPool::Get().InternPath("terrains/default.wterrain"), AssetId{} };
						comp.CellSize = cell;
						comp.HeightScale = hScale;
						comp.ChunkResolution = 64;
					}))
				{
					host.SetSelectedEntity(created);
					
					host.MarkDocumentDirty();
					CreateDefaultTerrain(res, res, cell);
					SaveTerrain("projects/assets/terrains/default.wterrain");
					m_StatusFeedback = "已成功创建 3D 地貌实体。";
				}
			}
		}
		currentY += 40.0f;
	}

	void TerrainPanel::RenderSculptTab(Wui::WuiContext& ctx, float startX, float& currentY, float contentWidth, const Wui::WuiTheme& theme, PanelHost&)
	{
		const float rowH = theme.RowHeight;

		Wui::Label(ctx, { startX, currentY + 4.0f }, "雕刻工具 (Sculpt Tools)", theme.Accent, theme.FontSizeBody);
		currentY += rowH;

		// 4 种核心雕刻工具 (抬升/下凹、平滑、平整、噪波)
		const struct ModeCard { TerrainSculptMode mode; const char* label; const char* tip; } sculptModes[4] = {
			{ TerrainSculptMode::Raise,   "✦ 抬升/下凹", "抬升地貌高程 [按住 Shift 自动反转为下凹]" },
			{ TerrainSculptMode::Smooth,  "〰 柔和平滑", "平滑陡峭落差与粗糙尖角，使山脊自然过渡" },
			{ TerrainSculptMode::Flatten, "▬ 平台平整", "将地貌高程推至设定的基准高度水平面" },
			{ TerrainSculptMode::Erode,   "≈ 自然侵蚀", "模拟雨水冲刷侵蚀沟壑与风化地貌" }
		};
		const float colW = (contentWidth - theme.PadSmall) * 0.5f;
		const float cardH = 26.0f;
		for (int i = 0; i < 4; ++i)
		{
			const int row = i / 2;
			const int col = i % 2;
			const Wui::WuiRect btnRect {
				startX + static_cast<float>(col) * (colW + theme.PadSmall),
				currentY + static_cast<float>(row) * (cardH + 4.0f),
				colW, cardH
			};
			auto btnTheme = theme;
			if (m_SculptMode == sculptModes[i].mode)
			{
				btnTheme.ButtonBg = theme.ActiveBg;
				btnTheme.Border = theme.Accent;
			}
			Wui::Tooltip(ctx, btnRect, sculptModes[i].tip);
			if (Wui::Button(ctx, Wui::HashId(("terrain.sculpt.btn." + std::to_string(i)).c_str()), btnRect, sculptModes[i].label, btnTheme))
			{
				m_SculptMode = sculptModes[i].mode;
				m_SculptModeIdx = i;
			}
		}
		currentY += 2.0f * (cardH + 4.0f) + 8.0f;

		// 笔刷参数
		Wui::Label(ctx, { startX, currentY + 3.0f }, "半径 (m):", theme.Text, theme.FontSizeSmall);
		Wui::DragFloat(ctx, Wui::HashId("terrain.brush.radius"), { startX + 70.0f, currentY, contentWidth - 70.0f, 22.0f }, m_BrushRadius, 0.5f, 0.5f, 150.0f, theme);
		currentY += 28.0f;

		Wui::Label(ctx, { startX, currentY + 3.0f }, "强度 (%):", theme.Text, theme.FontSizeSmall);
		Wui::DragFloat(ctx, Wui::HashId("terrain.brush.strength"), { startX + 70.0f, currentY, contentWidth - 70.0f, 22.0f }, m_BrushStrength, 0.01f, 0.01f, 1.0f, theme);
		currentY += 28.0f;

		Wui::Label(ctx, { startX, currentY + 3.0f }, "衰减 (%):", theme.Text, theme.FontSizeSmall);
		Wui::DragFloat(ctx, Wui::HashId("terrain.brush.falloff_factor"), { startX + 70.0f, currentY, contentWidth - 70.0f, 22.0f }, m_BrushFalloffFactor, 0.02f, 0.0f, 1.0f, theme);
		currentY += 28.0f;

		if (m_SculptMode == TerrainSculptMode::Flatten)
		{
			Wui::Label(ctx, { startX, currentY + 3.0f }, "目标高度 (Target Y m):", theme.Text, theme.FontSizeSmall);
			Wui::DragFloat(ctx, Wui::HashId("terrain.target.height"), { startX + 130.0f, currentY, contentWidth - 130.0f, 22.0f }, m_TargetHeight, 0.5f, -100.0f, 1000.0f, theme);
			currentY += 28.0f;
		}

		// 操作提示药丸
		currentY += 4.0f;
		Wui::Label(ctx, { startX, currentY }, "操作指南: 视口左键拖拽雕刻 | Shift 反转 | [ / ] 调半径 | Shift+[ / ] 调强度", theme.TextMuted, theme.FontSizeSmall);
		currentY += 22.0f;
	}

	void TerrainPanel::RenderPaintTab(Wui::WuiContext& ctx, float startX, float& currentY, float contentWidth, const Wui::WuiTheme& theme, PanelHost&)
	{
		const float rowH = theme.RowHeight;

		Wui::Label(ctx, { startX, currentY + 4.0f }, "材质图层混绘 (Surface Material Splatmap)", theme.Accent, theme.FontSizeBody);
		currentY += rowH;

		const char* layerNames[4] = { "✦ 0: 翠绿草地", "■ 1: 松软泥土", "▲ 2: 崖壁岩石", "≈ 3: 河滩细沙" };
		const float colW = (contentWidth - theme.PadSmall) * 0.5f;
		const float cardH = 26.0f;

		for (uint32_t i = 0; i < 4; ++i)
		{
			const int row = static_cast<int>(i / 2);
			const int col = static_cast<int>(i % 2);
			const Wui::WuiRect cardRect {
				startX + static_cast<float>(col) * (colW + theme.PadSmall),
				currentY + static_cast<float>(row) * (cardH + 4.0f),
				colW, cardH
			};
			auto cardTheme = theme;
			const bool isSelected = (m_ActivePaintLayer == i);
			if (isSelected)
			{
				cardTheme.ButtonBg = theme.ActiveBg;
				cardTheme.Border = theme.Accent;
			}

			if (Wui::Button(ctx, Wui::HashId(("terrain.paint.layer." + std::to_string(i)).c_str()), cardRect, layerNames[i], cardTheme))
			{
				m_ActivePaintLayer = i;
				m_PaintLayerIdx = static_cast<int>(i);
			}
		}
		currentY += 2.0f * (cardH + 4.0f) + 6.0f;

		currentY += 6.0f;
		if (m_Terrain && m_ActivePaintLayer < 4)
		{
			const TerrainLayer& layer = m_Terrain->GetLayer(m_ActivePaintLayer);
			char paramBuf[128] = {};
			std::snprintf(paramBuf, sizeof(paramBuf), "当前材质规格: 粗糙度=%.2f  贴图平铺=(%.0f, %.0f)",
				layer.Roughness, layer.Tiling.x, layer.Tiling.y);
			Wui::Label(ctx, { startX, currentY }, paramBuf, theme.TextMuted, theme.FontSizeSmall);
			currentY += 22.0f;
		}

		Wui::Label(ctx, { startX, currentY + 3.0f }, "半径 (m):", theme.Text, theme.FontSizeSmall);
		Wui::DragFloat(ctx, Wui::HashId("terrain.paint.radius"), { startX + 70.0f, currentY, contentWidth - 70.0f, 22.0f }, m_BrushRadius, 0.5f, 0.5f, 150.0f, theme);
		currentY += 28.0f;

		Wui::Label(ctx, { startX, currentY + 3.0f }, "强度 (%):", theme.Text, theme.FontSizeSmall);
		Wui::DragFloat(ctx, Wui::HashId("terrain.paint.strength"), { startX + 70.0f, currentY, contentWidth - 70.0f, 22.0f }, m_BrushStrength, 0.01f, 0.01f, 1.0f, theme);
		currentY += 28.0f;

		Wui::Label(ctx, { startX, currentY + 3.0f }, "衰减 (%):", theme.Text, theme.FontSizeSmall);
		Wui::DragFloat(ctx, Wui::HashId("terrain.paint.falloff_factor"), { startX + 70.0f, currentY, contentWidth - 70.0f, 22.0f }, m_BrushFalloffFactor, 0.02f, 0.0f, 1.0f, theme);
		currentY += 32.0f;

		Wui::Label(ctx, { startX, currentY }, "混绘技巧: 视口左键拖拽涂抹材质 | 按住 Shift 拖拽平滑擦除图层权重", theme.TextMuted, theme.FontSizeSmall);
		currentY += 20.0f;
	}

	void TerrainPanel::RenderSettingsTab(Wui::WuiContext& ctx, float startX, float& currentY, float contentWidth, const Wui::WuiTheme& theme, PanelHost& host)
	{
		const float rowH = theme.RowHeight;
		const float halfW = (contentWidth - theme.PadSmall) * 0.5f;

		Wui::Label(ctx, { startX, currentY + 4.0f }, "地貌管理与统计 (Landscape Management)", theme.Accent, theme.FontSizeBody);
		currentY += rowH;

		if (m_Terrain)
		{
			char statsBuf[256] = {};
			const uint32_t w = m_Terrain->GetWidth();
			const uint32_t h = m_Terrain->GetHeight();
			const float ww = m_Terrain->GetWorldWidth();
			const float wh = m_Terrain->GetWorldHeight();
			const uint32_t cx = m_Terrain->GetChunkCountX(64);
			const uint32_t cz = m_Terrain->GetChunkCountZ(64);
			std::snprintf(statsBuf, sizeof(statsBuf), "网格: %u x %u (%.0fm x %.0fm) | 分块: %u x %u (%u 块)",
				w, h, ww, wh, cx, cz, cx * cz);
			Wui::Label(ctx, { startX, currentY }, statsBuf, theme.Text, theme.FontSizeSmall);
			currentY += 24.0f;
		}

		if (m_BoundEntity.IsValid() && m_BoundEntity.HasComponent<TerrainComponent>())
		{
			auto& comp = m_BoundEntity.GetComponent<TerrainComponent>();
			Wui::Label(ctx, { startX, currentY + 3.0f }, "CDLOD 距离比:", theme.Text, theme.FontSizeSmall);
			if (Wui::DragFloat(ctx, Wui::HashId("terrain.settings.lod_ratio"), { startX + 130.0f, currentY, contentWidth - 130.0f, 22.0f }, comp.LodDistanceRatio, 0.1f, 0.5f, 10.0f, theme))
			{
				host.MarkDocumentDirty();
			}
			currentY += 28.0f;

			Wui::Label(ctx, { startX, currentY + 3.0f }, "高度倍率 (Scale):", theme.Text, theme.FontSizeSmall);
			if (Wui::DragFloat(ctx, Wui::HashId("terrain.settings.hscale"), { startX + 130.0f, currentY, contentWidth - 130.0f, 22.0f }, comp.HeightScale, 0.5f, 0.1f, 500.0f, theme))
			{
				if (m_Terrain) m_Terrain->SetHeightScale(comp.HeightScale);
				host.MarkDocumentDirty();
			}
			currentY += 32.0f;
		}

		// 快速生成起伏山峦与平坦重置
		if (Wui::Button(ctx, Wui::HashId("terrain.gen.mountains"), { startX, currentY, halfW, 28.0f }, "✦ 生成起伏山峦", theme))
		{
			if (m_Terrain)
			{
				const uint32_t w = m_Terrain->GetWidth();
				const uint32_t h = m_Terrain->GetHeight();
				for (uint32_t z = 0; z < h; ++z)
				{
					for (uint32_t x = 0; x < w; ++x)
					{
						const float fx = static_cast<float>(x) * 0.04f;
						const float fz = static_cast<float>(z) * 0.04f;
						const float elevation = std::sin(fx) * 12.0f + std::cos(fz) * 10.0f + std::sin(fx * 2.3f + fz * 1.7f) * 5.0f;
						m_Terrain->SetHeight(x, z, std::max(0.0f, elevation));
					}
				}
				m_Terrain->MarkDirty(0.0f, 0.0f, m_Terrain->GetWorldWidth(), m_Terrain->GetWorldHeight());
				m_StatusFeedback = "已程序化生成自然山峦地貌。";
			}
		}

		if (Wui::Button(ctx, Wui::HashId("terrain.reset.flat"), { startX + halfW + theme.PadSmall, currentY, halfW, 28.0f }, "↺ 重置为平坦", theme))
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
				m_Terrain->MarkDirty(0.0f, 0.0f, m_Terrain->GetWorldWidth(), m_Terrain->GetWorldHeight());
				m_StatusFeedback = "已重置为平坦地形。";
			}
		}
		currentY += 34.0f;

		// 资产持久化
		if (Wui::Button(ctx, Wui::HashId("terrain.save.asset"), { startX, currentY, contentWidth, 28.0f }, "保存到 default.wterrain 资产", theme))
		{
			SaveTerrain("projects/assets/terrains/default.wterrain");
		}
		currentY += 34.0f;
	}
}
