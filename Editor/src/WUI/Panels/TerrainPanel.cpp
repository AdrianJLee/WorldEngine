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
		std::snprintf(tagBuf, sizeof(tagBuf), "绑定实体: [%s]  分辨率: %ux%u (%.1fm x %.1fm)",
			entityTag.c_str(), m_Terrain->GetWidth(), m_Terrain->GetHeight(),
			m_Terrain->GetWorldWidth(), m_Terrain->GetWorldHeight());
		Wui::Label(ctx, { startX, curY }, tagBuf, theme.TextMuted, theme.FontSizeSmall);
		curY += 22.0f;

		// 选项卡切换组
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

	void TerrainPanel::RenderWizard(Wui::WuiContext& ctx, float startX, float& currentY, float contentWidth, const Wui::WuiTheme& theme, PanelHost& host)
	{
		const float pad = theme.PadSmall;

		// 向导卡片标题
		Wui::Label(ctx, { startX, currentY + 2.0f }, "✦ 新建 3D 地貌向导 (New Landscape Wizard)", theme.Accent, theme.FontSizeBody);
		currentY += 26.0f;

		Wui::Label(ctx, { startX, currentY }, "当前场景尚未添加地貌。请选择预设规格或自定义参数：", theme.TextMuted, theme.FontSizeSmall);
		currentY += 22.0f;

		// 预设分辨率规格 (4 个药丸按钮)
		const struct PresetSpec { int64_t res; const char* label; } presets[4] = {
			{ 63, "63x63\n小型" },
			{ 127, "127x127\n标准" },
			{ 255, "255x255\n关卡(推荐)" },
			{ 511, "511x511\n宏大" }
		};
		const float presetW = (contentWidth - 3.0f * pad) * 0.25f;
		const float presetH = 34.0f;
		for (int i = 0; i < 4; ++i)
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
		currentY += presetH + 12.0f;

		// 分辨率、单元格尺寸、高度范围微调
		Wui::Label(ctx, { startX, currentY + 3.0f }, "网格分辨率 (Resolution):", theme.Text, theme.FontSizeSmall);
		Wui::DragInt(ctx, Wui::HashId("terrain.wizard.res"), { startX + 150.0f, currentY, contentWidth - 150.0f, 22.0f }, m_WizardResolution, 16, 2048, theme);
		currentY += 28.0f;

		Wui::Label(ctx, { startX, currentY + 3.0f }, "单元格间距 (CellSize m):", theme.Text, theme.FontSizeSmall);
		Wui::DragFloat(ctx, Wui::HashId("terrain.wizard.cell"), { startX + 150.0f, currentY, contentWidth - 150.0f, 22.0f }, m_WizardCellSize, 0.1f, 0.1f, 50.0f, theme);
		currentY += 28.0f;

		Wui::Label(ctx, { startX, currentY + 3.0f }, "高度范围 (HeightScale m):", theme.Text, theme.FontSizeSmall);
		Wui::DragFloat(ctx, Wui::HashId("terrain.wizard.height_scale"), { startX + 150.0f, currentY, contentWidth - 150.0f, 22.0f }, m_WizardHeightScale, 0.5f, 1.0f, 500.0f, theme);
		currentY += 34.0f;

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
						comp.CellSize = cell;
						comp.HeightScale = hScale;
						comp.MaxHeight = hScale;
						comp.ChunkResolution = (res <= 64 ? 32 : 64);

						const std::string dir = "projects/default/assets/terrains";
						std::filesystem::create_directories(dir);
						const std::string path = dir + "/landscape_" + std::to_string(res) + ".wterrain";
						TerrainData defaultData(res, res, cell, hScale);
						defaultData.GetLayer(0).Name = "Grass (草地)";
						defaultData.GetLayer(1).Name = "Dirt (泥土)";
						defaultData.GetLayer(2).Name = "Rock (岩石)";
						defaultData.GetLayer(3).Name = "Sand (沙地)";
						defaultData.SaveToFile(path);
						comp.TerrainAsset.Path = StringPool::Get().InternPath(path);
					}) && created.IsValid())
				{
					host.SetSelectedEntity(created);
					host.PushCommand(std::make_unique<EntityCreateCommand>(created, "Create Landscape"));
					host.SetEditorToolMode(EditorToolMode::Landscape);
					host.MarkDocumentDirty();
					m_StatusFeedback = "成功创建地貌实体并初始化资产: " + std::to_string(res) + "x" + std::to_string(res);
				}
			}
		}
		currentY += 40.0f;
	}

	void TerrainPanel::RenderSculptTab(Wui::WuiContext& ctx, float startX, float& currentY, float contentWidth, const Wui::WuiTheme& theme, PanelHost&)
	{
		const float rowH = theme.RowHeight;
		const float halfW = (contentWidth - theme.PadSmall) * 0.5f;

		Wui::Label(ctx, { startX, currentY + 4.0f }, "雕刻工具模式 (Sculpt Tools)", theme.Accent, theme.FontSizeBody);
		currentY += rowH;

		// 6 种雕刻模式卡片按钮 (2 行 3 列)
		const struct ModeCard { TerrainSculptMode mode; const char* label; const char* tip; } sculptModes[6] = {
			{ TerrainSculptMode::Raise,   "✦ 抬升 (Raise)",  "抬升地表高度 [Shift 反转为下凹]" },
			{ TerrainSculptMode::Lower,   "▼ 下凹 (Lower)",  "降低地表高度 [Shift 反转为抬升]" },
			{ TerrainSculptMode::Smooth,  "〰 平滑 (Smooth)", "平滑地表落差与粗糙棱角" },
			{ TerrainSculptMode::Flatten, "▬ 压平 (Flatten)","将地貌高程推至设定目标高度" },
			{ TerrainSculptMode::Erode,   "≈ 侵蚀 (Erode)",  "模拟水力/热侵蚀自然冲刷沟壑" },
			{ TerrainSculptMode::Hole,    "⬡ 地洞 (Hole)",   "镂空洞穴地貌，支持进出地下" }
		};
		const float colW = (contentWidth - 2.0f * theme.PadSmall) / 3.0f;
		const float cardH = 26.0f;
		for (int i = 0; i < 6; ++i)
		{
			const int row = i / 3;
			const int col = i % 3;
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

		// 笔刷半径与强度 (带单位 m 与 %)
		Wui::Label(ctx, { startX, currentY + 3.0f }, "笔刷半径 (Radius m):", theme.Text, theme.FontSizeSmall);
		Wui::DragFloat(ctx, Wui::HashId("terrain.brush.radius"), { startX + 140.0f, currentY, contentWidth - 140.0f, 22.0f }, m_BrushRadius, 0.5f, 0.5f, 150.0f, theme);
		currentY += 28.0f;

		Wui::Label(ctx, { startX, currentY + 3.0f }, "笔刷强度 (Strength %):", theme.Text, theme.FontSizeSmall);
		Wui::DragFloat(ctx, Wui::HashId("terrain.brush.strength"), { startX + 140.0f, currentY, contentWidth - 140.0f, 22.0f }, m_BrushStrength, 0.01f, 0.01f, 1.0f, theme);
		currentY += 28.0f;

		Wui::Label(ctx, { startX, currentY + 3.0f }, "内衰减比例 (Falloff %):", theme.Text, theme.FontSizeSmall);
		Wui::DragFloat(ctx, Wui::HashId("terrain.brush.falloff_factor"), { startX + 140.0f, currentY, contentWidth - 140.0f, 22.0f }, m_BrushFalloffFactor, 0.02f, 0.0f, 1.0f, theme);
		currentY += 28.0f;

		// 衰减曲线下拉
		const std::vector<std::string> falloffTypes = { "Linear 线性衰减", "Smooth 平滑余弦", "Sharp 尖锐衰减", "Constant 无衰减平头" };
		Wui::Label(ctx, { startX, currentY + 3.0f }, "衰减曲线 (Falloff):", theme.Text, theme.FontSizeSmall);
		if (Wui::Combo(ctx, Wui::HashId("terrain.brush.falloff"), { startX + 140.0f, currentY, contentWidth - 140.0f, 22.0f }, "Falloff", falloffTypes, m_FalloffIdx, theme))
		{
			m_Falloff = static_cast<TerrainBrushFalloff>(m_FalloffIdx);
		}
		currentY += 32.0f;

		if (m_SculptMode == TerrainSculptMode::Flatten)
		{
			Wui::Label(ctx, { startX, currentY + 3.0f }, "目标高度 (Target Y m):", theme.Text, theme.FontSizeSmall);
			Wui::DragFloat(ctx, Wui::HashId("terrain.target.height"), { startX + 140.0f, currentY, contentWidth - 140.0f, 22.0f }, m_TargetHeight, 0.5f, -100.0f, 1000.0f, theme);
			currentY += 28.0f;
		}

		// 快速程序化辅助
		if (Wui::Button(ctx, Wui::HashId("terrain.gen.hills"), { startX, currentY, halfW, 26.0f }, "✦ 程序化生成山丘", theme))
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
				m_Terrain->MarkDirty(0.0f, 0.0f, m_Terrain->GetWorldWidth(), m_Terrain->GetWorldHeight());
				m_StatusFeedback = "已程序化生成起伏山丘地貌。";
			}
		}

		if (Wui::Button(ctx, Wui::HashId("terrain.reset.flat"), { startX + halfW + theme.PadSmall, currentY, halfW, 26.0f }, "↺ 重置为平坦", theme))
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

		// 快捷键提示条
		Wui::Label(ctx, { startX, currentY }, "快捷键: [ / ] 调半径 | Shift+[ / ] 调强度 | Shift+拖拽反向", theme.TextMuted, theme.FontSizeSmall);
		currentY += 20.0f;
	}

	void TerrainPanel::RenderPaintTab(Wui::WuiContext& ctx, float startX, float& currentY, float contentWidth, const Wui::WuiTheme& theme, PanelHost&)
	{
		const float rowH = theme.RowHeight;

		Wui::Label(ctx, { startX, currentY + 4.0f }, "地表材质调色盘 (Splatmap Material Palette)", theme.Accent, theme.FontSizeBody);
		currentY += rowH;

		// 4 个材质图层卡片 (Grass, Dirt, Rock, Sand)
		const struct LayerDefaultSpec { const char* defaultName; const char* desc; } defaultLayers[4] = {
			{ "Grass (草地)", "基底草皮材质，包含细微起伏与漫反射叶绿质感" },
			{ "Dirt (泥土)",  "泥土沙砾表层，适合林间小道与洼地沉积" },
			{ "Rock (岩石)",  "陡峭崖壁裸露岩石，高粗糙度 PBR 材质" },
			{ "Sand (沙地)",  "细腻河滩/沙漠沙石，微弱反光与平整沙丘" }
		};

		for (uint32_t i = 0; i < 4; ++i)
		{
			const Wui::WuiRect cardRect { startX, currentY, contentWidth, 32.0f };
			auto cardTheme = theme;
			const bool isSelected = (m_ActivePaintLayer == i);
			if (isSelected)
			{
				cardTheme.ButtonBg = theme.ActiveBg;
				cardTheme.Border = theme.Accent;
			}

			std::string layerTitle = "Layer " + std::to_string(i) + ": " + defaultLayers[i].defaultName;
			if (m_Terrain)
			{
				const TerrainLayer& l = m_Terrain->GetLayer(i);
				if (!l.Name.empty())
					layerTitle = "Layer " + std::to_string(i) + ": " + l.Name;
			}

			if (Wui::Button(ctx, Wui::HashId(("terrain.paint.layer." + std::to_string(i)).c_str()), cardRect, layerTitle, cardTheme))
			{
				m_ActivePaintLayer = i;
				m_PaintLayerIdx = static_cast<int>(i);
			}
			currentY += 36.0f;
		}

		currentY += 6.0f;
		// 活跃图层参数展示
		if (m_Terrain && m_ActivePaintLayer < 4)
		{
			const TerrainLayer& layer = m_Terrain->GetLayer(m_ActivePaintLayer);
			char paramBuf[128] = {};
			std::snprintf(paramBuf, sizeof(paramBuf), "当前图层 PBR: Roughness=%.2f  Metallic=%.2f  Tiling=(%.1f, %.1f)",
				layer.Roughness, layer.Metallic, layer.Tiling.x, layer.Tiling.y);
			Wui::Label(ctx, { startX, currentY }, paramBuf, theme.TextMuted, theme.FontSizeSmall);
			currentY += 22.0f;
		}

		Wui::Label(ctx, { startX, currentY + 3.0f }, "涂抹半径 (Radius m):", theme.Text, theme.FontSizeSmall);
		Wui::DragFloat(ctx, Wui::HashId("terrain.paint.radius"), { startX + 140.0f, currentY, contentWidth - 140.0f, 22.0f }, m_BrushRadius, 0.5f, 0.5f, 150.0f, theme);
		currentY += 28.0f;

		Wui::Label(ctx, { startX, currentY + 3.0f }, "涂抹强度 (Strength %):", theme.Text, theme.FontSizeSmall);
		Wui::DragFloat(ctx, Wui::HashId("terrain.paint.strength"), { startX + 140.0f, currentY, contentWidth - 140.0f, 22.0f }, m_BrushStrength, 0.01f, 0.01f, 1.0f, theme);
		currentY += 28.0f;

		Wui::Label(ctx, { startX, currentY + 3.0f }, "内衰减比例 (Falloff %):", theme.Text, theme.FontSizeSmall);
		Wui::DragFloat(ctx, Wui::HashId("terrain.paint.falloff_factor"), { startX + 140.0f, currentY, contentWidth - 140.0f, 22.0f }, m_BrushFalloffFactor, 0.02f, 0.0f, 1.0f, theme);
		currentY += 32.0f;

		Wui::Label(ctx, { startX, currentY }, "混绘技巧: 左键拖拽涂抹材质 | 按住 Shift 拖拽平滑过渡图层权重", theme.TextMuted, theme.FontSizeSmall);
		currentY += 20.0f;
	}

	void TerrainPanel::RenderSettingsTab(Wui::WuiContext& ctx, float startX, float& currentY, float contentWidth, const Wui::WuiTheme& theme, PanelHost& host)
	{
		const float rowH = theme.RowHeight;

		Wui::Label(ctx, { startX, currentY + 4.0f }, "地貌规格与统计 (Landscape Settings & Stats)", theme.Accent, theme.FontSizeBody);
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
			std::snprintf(statsBuf, sizeof(statsBuf), "网格规格: %u x %u (%.1fm x %.1fm) | CDLOD 分块: %u x %u (%u 块)",
				w, h, ww, wh, cx, cz, cx * cz);
			Wui::Label(ctx, { startX, currentY }, statsBuf, theme.Text, theme.FontSizeSmall);
			currentY += 24.0f;
		}

		// CDLOD 距离比调节
		if (m_BoundEntity.IsValid() && m_BoundEntity.HasComponent<TerrainComponent>())
		{
			auto& comp = m_BoundEntity.GetComponent<TerrainComponent>();
			Wui::Label(ctx, { startX, currentY + 3.0f }, "CDLOD 距离比:", theme.Text, theme.FontSizeSmall);
			if (Wui::DragFloat(ctx, Wui::HashId("terrain.settings.lod_ratio"), { startX + 140.0f, currentY, contentWidth - 140.0f, 22.0f }, comp.LodDistanceRatio, 0.1f, 0.5f, 10.0f, theme))
			{
				host.MarkDocumentDirty();
			}
			currentY += 28.0f;

			Wui::Label(ctx, { startX, currentY + 3.0f }, "高度倍率 (Scale):", theme.Text, theme.FontSizeSmall);
			if (Wui::DragFloat(ctx, Wui::HashId("terrain.settings.hscale"), { startX + 140.0f, currentY, contentWidth - 140.0f, 22.0f }, comp.HeightScale, 0.5f, 0.1f, 500.0f, theme))
			{
				if (m_Terrain) m_Terrain->SetHeightScale(comp.HeightScale);
				host.MarkDocumentDirty();
			}
			currentY += 32.0f;
		}

		// 操作按钮组
		if (Wui::Button(ctx, Wui::HashId("terrain.settings.focus"), { startX, currentY, contentWidth, 26.0f }, "⌖ 视口聚焦地貌 (Focus In Viewport)", theme))
		{
			if (m_BoundEntity.IsValid())
			{
				host.SetSelectedEntity(m_BoundEntity);
				host.FocusIndependentWindow("view");
			}
		}
		currentY += 32.0f;

		if (Wui::Button(ctx, Wui::HashId("terrain.settings.save"), { startX, currentY, contentWidth, 26.0f }, "💾 保存地貌资产 (Save Terrain Asset)", theme))
		{
			std::string savePath = "projects/default/assets/terrains/default.wterrain";
			if (m_BoundEntity.IsValid() && m_BoundEntity.HasComponent<TerrainComponent>())
			{
				const auto& comp = m_BoundEntity.GetComponent<TerrainComponent>();
				if (comp.TerrainAsset.HasPath())
					savePath = StringPool::Get().PathOf(comp.TerrainAsset.Path);
			}
			std::filesystem::path p(savePath);
			if (p.has_parent_path())
				std::filesystem::create_directories(p.parent_path());
			SaveTerrain(savePath);
		}
		currentY += 32.0f;

		if (Wui::Button(ctx, Wui::HashId("terrain.settings.export_raw16"), { startX, currentY, contentWidth, 26.0f }, "⇪ 导出 16-bit RAW 高度图 (Export RAW16)", theme))
		{
			if (m_Terrain)
			{
				const std::string rawPath = "projects/default/assets/terrains/heightmap_export.raw";
				std::filesystem::path p(rawPath);
				if (p.has_parent_path())
					std::filesystem::create_directories(p.parent_path());
				if (m_Terrain->ExportHeightmapRaw16(rawPath))
					m_StatusFeedback = "成功导出 16 位 RAW 高度图至: " + rawPath;
				else
					m_StatusFeedback = "导出 RAW 高度图失败。";
			}
		}
		currentY += 32.0f;
	}
}
