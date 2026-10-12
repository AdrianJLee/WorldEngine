#include "wldpch.h"
#include "WUI/Panels/TilemapEditorPanel.h"
#include "World/Renderer/TextureAtlas.h"
#include "World/Scene/Components.h"
#include "World/Scene/Entity.h"
#include "World/Core/Log.h"
#include "World/Utils/Paths.h"
#include "World/WUI/WuiWidgets.h"
#include "World/WUI/Widgets/WuiChrome.h"
#include "World/Renderer/Texture/TextureLibrary.h"
#include "World/WUI/WuiTextureRegistry.h"
#include "WUI/Common/EditorAssetCatalog.h"
#include "World/Core/StringPool.h"

#include <filesystem>
#include <algorithm>
#include <sstream>
#include <iomanip>
#include <cmath>

namespace World
{
	TilemapEditorPanel::TilemapEditorPanel(std::string logicalPath)
		: m_LogicalPath(std::move(logicalPath))
	{
		std::replace(m_LogicalPath.begin(), m_LogicalPath.end(), '\\', '/');
		m_PanelId = "tilemap:" + m_LogicalPath;
		m_PanelTitle = "瓦片地图: " + std::filesystem::path(m_LogicalPath).filename().string();
		Reload();
	}

	std::filesystem::path TilemapEditorPanel::ResolvePhysicalPath() const
	{
		std::filesystem::path p(m_LogicalPath);
		if (std::filesystem::exists(p))
			return std::filesystem::absolute(p);
		std::filesystem::path fromProj = Paths::ProjectDir() / p;
		if (std::filesystem::exists(fromProj))
			return std::filesystem::absolute(fromProj);
		std::filesystem::path fromAssetRoot = Paths::AssetRoot() / p;
		if (std::filesystem::exists(fromAssetRoot))
			return std::filesystem::absolute(fromAssetRoot);
		const std::string s = m_LogicalPath;
		if (s.rfind("assets/", 0) == 0 || s.rfind("assets\\", 0) == 0)
		{
			std::filesystem::path stripped = Paths::AssetRoot() / s.substr(7);
			if (std::filesystem::exists(stripped))
				return std::filesystem::absolute(stripped);
		}
		if (!p.is_absolute())
			return Paths::ProjectDir() / p;
		return p;
	}

	void TilemapEditorPanel::EnsureAtlasAndTextureLoaded()
	{
		if (!m_Tilemap || m_Tilemap->GetAtlasPath().empty())
			return;

		const std::string atlasPath = m_Tilemap->GetAtlasPath();
		if (atlasPath != m_LoadedAtlasPath || !m_Atlas)
		{
			std::filesystem::path p(atlasPath);
			std::filesystem::path resolved = p;
			if (!std::filesystem::exists(resolved))
			{
				resolved = Paths::ProjectDir() / p;
				if (!std::filesystem::exists(resolved))
					resolved = Paths::AssetRoot() / p;
			}
			m_Atlas = TextureAtlas::LoadFromFile(resolved.string());
			m_LoadedAtlasPath = atlasPath;
		}

		if (!m_Atlas || m_Atlas->GetTexturePath().empty())
			return;

		const std::string texPath = m_Atlas->GetTexturePath();
		if (texPath == m_LoadedTexturePath && m_UiTextureId != 0)
			return;

		std::string logical = texPath;
		if (logical.rfind("assets/", 0) == 0 || logical.rfind("assets\\", 0) == 0)
			logical = logical.substr(7);

		PathId pid = StringPool::Get().InternPath(logical);
		auto gpu = TextureLibrary::Get().Get(pid, true);
		if (gpu)
		{
			m_GpuTexture = gpu;
			m_LoadedTexturePath = texPath;
			if (m_UiTextureId == 0)
				m_UiTextureId = Wui::WuiTextureRegistry::Get().Register(gpu);
			else
				Wui::WuiTextureRegistry::Get().Update(m_UiTextureId, gpu);
		}
	}

	bool TilemapEditorPanel::Reload(std::string* message)
	{
		const auto phys = ResolvePhysicalPath();
		m_Tilemap = Tilemap::LoadFromFile(phys.string());
		if (!m_Tilemap)
		{
			m_StatusFeedback = "无法载入瓦片地图: " + m_LogicalPath;
			if (message) *message = m_StatusFeedback;
			return false;
		}

		m_EditName = m_Tilemap->GetName();
		m_EditAtlasPath = m_Tilemap->GetAtlasPath();
		m_EditGridW = m_Tilemap->GetWidth();
		m_EditGridH = m_Tilemap->GetHeight();
		m_EditCellW = m_Tilemap->GetCellSize().x;
		m_EditCellH = m_Tilemap->GetCellSize().y;

		EnsureAtlasAndTextureLoaded();
		if (m_Atlas && m_Atlas->GetSubTextureCount() > 0)
		{
			if (m_SelectedTileId < 1 || m_SelectedTileId > static_cast<int>(m_Atlas->GetSubTextureCount()))
				m_SelectedTileId = 1;
		}
		m_HasRectStart = false;
		m_Dirty = false;
		m_StatusFeedback = "就绪: 已载入 " + std::to_string(m_Tilemap->GetWidth()) + "x"
			+ std::to_string(m_Tilemap->GetHeight()) + " 瓦片地图";
		if (message) *message = m_StatusFeedback;
		return true;
	}

	bool TilemapEditorPanel::Save(std::string* message)
	{
		if (!m_Tilemap)
		{
			m_StatusFeedback = "无可用瓦片地图数据保存";
			if (message) *message = m_StatusFeedback;
			return false;
		}

		if (!m_EditName.empty())
			m_Tilemap->SetName(m_EditName);
		m_Tilemap->SetAtlasPath(m_EditAtlasPath);
		const uint32_t targetW = static_cast<uint32_t>(std::clamp<int64_t>(m_EditGridW, 1, 128));
		const uint32_t targetH = static_cast<uint32_t>(std::clamp<int64_t>(m_EditGridH, 1, 128));
		if (targetW != m_Tilemap->GetWidth() || targetH != m_Tilemap->GetHeight())
			m_Tilemap->Resize(targetW, targetH);
		m_Tilemap->SetCellSize({ std::max(0.05f, m_EditCellW), std::max(0.05f, m_EditCellH) });

		const auto phys = ResolvePhysicalPath();
		if (!m_Tilemap->SaveToFile(phys.string()))
		{
			m_StatusFeedback = "保存失败: " + phys.string();
			if (message) *message = m_StatusFeedback;
			return false;
		}

		Tilemap::ClearCache();
		m_Dirty = false;
		m_StatusFeedback = "瓦片地图已保存: " + m_LogicalPath;
		if (message) *message = m_StatusFeedback;
		return true;
	}

	void TilemapEditorPanel::PaintCell(uint32_t x, uint32_t y, uint16_t tileId, bool setSolid)
	{
		if (!m_Tilemap)
			return;
		m_Tilemap->SetTile(x, y, tileId);
		m_Tilemap->SetSolid(x, y, tileId == 0 ? false : setSolid);
		m_Dirty = true;
	}

	void TilemapEditorPanel::FillRectCells(uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1, uint16_t tileId, bool setSolid)
	{
		if (!m_Tilemap)
			return;
		m_Tilemap->FillRect(x0, y0, x1, y1, tileId, setSolid);
		m_Dirty = true;
	}

	void TilemapEditorPanel::FloodFillCells(uint32_t startX, uint32_t startY, uint16_t tileId, bool setSolid)
	{
		if (!m_Tilemap)
			return;
		m_Tilemap->FloodFill(startX, startY, tileId, setSolid);
		m_Dirty = true;
	}

	void TilemapEditorPanel::ClearAll()
	{
		if (!m_Tilemap)
			return;
		m_Tilemap->Clear();
		m_HasRectStart = false;
		m_Dirty = true;
		m_StatusFeedback = "已清空全部瓦片与碰撞层";
	}

	void TilemapEditorPanel::OnRender(Wui::WuiContext& ctx, const Wui::WuiRect& rect, PanelHost& host)
	{
		const Wui::WuiTheme& theme = host.Theme();
		Wui::PanelBackground(ctx, rect, theme.PanelBg);

		if (!m_Tilemap)
			m_Tilemap = std::make_shared<Tilemap>(
				std::filesystem::path(m_LogicalPath).stem().string(),
				static_cast<uint32_t>(m_EditGridW), static_cast<uint32_t>(m_EditGridH));

		EnsureAtlasAndTextureLoaded();

		float curY = rect.Y + 6.0f;

		// 1. Top Bar: Header, logical path, Save button, and Instantiate in Scene button
		const Wui::WuiRect topBar { rect.X + 8.0f, curY, rect.W - 16.0f, 32.0f };
		Wui::BarSurface(ctx, topBar, theme.PanelHeader, theme.Border);
		Wui::Label(ctx, { topBar.X + 10.0f, topBar.Y + 7.0f }, "2D 瓦片地图工作台 (Tilemap & Palette Workstation)", theme.Accent, 13.5f);

		std::string pathText = "资产: " + m_LogicalPath;
		Wui::Label(ctx, { topBar.X + 350.0f, topBar.Y + 8.0f }, pathText, theme.TextMuted, 11.0f);

		const float btnTopW = 126.0f;
		const Wui::WuiRect btnInst { topBar.X + topBar.W - 240.0f, topBar.Y + 4.0f, btnTopW, 24.0f };
		Wui::Tooltip(ctx, btnInst, "将当前瓦片地图作为带碰撞实体的 Tilemap 放入当前场景中 (tilemap.btn.instantiate)");
		if (Wui::Button(ctx, Wui::HashId("tilemap.btn.instantiate"), btnInst, "🏗 放入当前场景", theme))
		{
			if (m_Dirty)
				Save();
			if (auto scene = host.GetActiveScene())
			{
				const std::string entName = !m_EditName.empty()
					? m_EditName
					: std::filesystem::path(m_LogicalPath).stem().string();
				Entity ent = Entity::CreateEntity(scene.get(), entName);
				ent.AddComponent<TransformComponent>();
				auto& tc = ent.AddComponent<TilemapComponent>();
				tc.Tilemap.Path = StringPool::Get().InternPath(m_LogicalPath);
				tc.CellSize = { m_EditCellW, m_EditCellH };
				tc.GenerateCollision = true;

				auto& rb = ent.AddComponent<RigidBody2DComponent>();
				rb.Type = RigidBody2DComponent::BodyType::Static;

				host.SetSelectedEntity(ent);
				host.MarkDocumentDirty();
				m_StatusFeedback = "已在当前场景创建瓦片地图实体 '" + entName + "' 并附加静态刚体";
			}
			else
			{
				m_StatusFeedback = "当前无活动场景，无法放入实体";
			}
		}

		const Wui::WuiRect btnSave { topBar.X + topBar.W - 110.0f, topBar.Y + 4.0f, 102.0f, 24.0f };
		Wui::Tooltip(ctx, btnSave, "保存瓦片地图配置与数据到 .wtilemap 文件并刷新缓存 (tilemap.btn.save)");
		if (Wui::Button(ctx, Wui::HashId("tilemap.btn.save"), btnSave, m_Dirty ? "💾 保存地图 *" : "💾 保存地图", theme))
		{
			Save();
		}

		curY += 38.0f;

		// 2. Tool Strip (Tools: Brush, Eraser, FloodFill, RectFill, CollisionToggle, Options, Clear)
		const Wui::WuiRect toolBar { rect.X + 8.0f, curY, rect.W - 16.0f, 34.0f };
		Wui::PanelBackground(ctx, toolBar, theme.ContentBg, 3.0f);

		float tX = toolBar.X + 8.0f;

		// Brush
		const bool isBrush = (m_ActiveTool == TilePaintTool::Brush);
		const Wui::WuiRect btnBrush { tX, toolBar.Y + 5.0f, 68.0f, 24.0f };
		Wui::Tooltip(ctx, btnBrush, "笔刷涂刷工具 (点击/拖拽涂刷选中瓦片) (tilemap.tool.brush)");
		if (Wui::Button(ctx, Wui::HashId("tilemap.tool.brush"), btnBrush, isBrush ? "✔ 笔刷" : "🖌 笔刷", theme))
		{
			m_ActiveTool = TilePaintTool::Brush;
			m_HasRectStart = false;
		}
		tX += 74.0f;

		// Eraser
		const bool isEraser = (m_ActiveTool == TilePaintTool::Eraser);
		const Wui::WuiRect btnEraser { tX, toolBar.Y + 5.0f, 76.0f, 24.0f };
		Wui::Tooltip(ctx, btnEraser, "橡皮擦工具 (点击/拖拽清除瓦片与碰撞) (tilemap.tool.eraser)");
		if (Wui::Button(ctx, Wui::HashId("tilemap.tool.eraser"), btnEraser, isEraser ? "✔ 橡皮擦" : "🧹 橡皮擦", theme))
		{
			m_ActiveTool = TilePaintTool::Eraser;
			m_HasRectStart = false;
		}
		tX += 82.0f;

		// Flood Fill
		const bool isFlood = (m_ActiveTool == TilePaintTool::FloodFill);
		const Wui::WuiRect btnFlood { tX, toolBar.Y + 5.0f, 76.0f, 24.0f };
		Wui::Tooltip(ctx, btnFlood, "油漆桶泛洪填充工具 (4-邻域 BFS 填充连通区域) (tilemap.tool.flood)");
		if (Wui::Button(ctx, Wui::HashId("tilemap.tool.flood"), btnFlood, isFlood ? "✔ 油漆桶" : "🪣 油漆桶", theme))
		{
			m_ActiveTool = TilePaintTool::FloodFill;
			m_HasRectStart = false;
		}
		tX += 82.0f;

		// Rect Fill
		const bool isRect = (m_ActiveTool == TilePaintTool::RectFill);
		const Wui::WuiRect btnRect { tX, toolBar.Y + 5.0f, 88.0f, 24.0f };
		Wui::Tooltip(ctx, btnRect, "矩形填充工具 (点击两次选择对角区域进行填充) (tilemap.tool.rect)");
		if (Wui::Button(ctx, Wui::HashId("tilemap.tool.rect"), btnRect, isRect ? "✔ 矩形填充" : "⬜ 矩形填充", theme))
		{
			m_ActiveTool = TilePaintTool::RectFill;
			m_HasRectStart = false;
		}
		tX += 94.0f;

		// Collision Toggle Tool
		const bool isCol = (m_ActiveTool == TilePaintTool::CollisionToggle);
		const Wui::WuiRect btnCol { tX, toolBar.Y + 5.0f, 88.0f, 24.0f };
		Wui::Tooltip(ctx, btnCol, "碰撞涂刷工具 (点击切换单元格碰撞实体状态) (tilemap.tool.collision)");
		if (Wui::Button(ctx, Wui::HashId("tilemap.tool.collision"), btnCol, isCol ? "✔ 碰撞涂刷" : "🧱 碰撞涂刷", theme))
		{
			m_ActiveTool = TilePaintTool::CollisionToggle;
			m_HasRectStart = false;
		}
		tX += 98.0f;

		// Paint Solid Checkbox
		const Wui::WuiRect btnSolidToggle { tX, toolBar.Y + 5.0f, 130.0f, 24.0f };
		Wui::Tooltip(ctx, btnSolidToggle, "落笔涂刷/填充时是否同步标记为碰撞体 (tilemap.btn.paint_solid)");
		std::string solidStr = m_PaintSolid ? "✔ 落笔附带碰撞: 开" : "落笔附带碰撞: 关";
		if (Wui::Button(ctx, Wui::HashId("tilemap.btn.paint_solid"), btnSolidToggle, solidStr, theme))
		{
			m_PaintSolid = !m_PaintSolid;
		}
		tX += 136.0f;

		// Show Collision Outline Checkbox
		const Wui::WuiRect btnShowCol { tX, toolBar.Y + 5.0f, 120.0f, 24.0f };
		Wui::Tooltip(ctx, btnShowCol, "显示/隐藏碰撞体与合并矩形线框 (tilemap.btn.show_collision)");
		std::string showColStr = m_ShowCollisionOverlay ? "👁 碰撞框显示: 开" : "👁 碰撞框显示: 关";
		if (Wui::Button(ctx, Wui::HashId("tilemap.btn.show_collision"), btnShowCol, showColStr, theme))
		{
			m_ShowCollisionOverlay = !m_ShowCollisionOverlay;
		}
		tX += 126.0f;

		// Clear Grid
		const Wui::WuiRect btnClear { tX, toolBar.Y + 5.0f, 68.0f, 24.0f };
		Wui::Tooltip(ctx, btnClear, "清空整个网格的全部瓦片与碰撞 (tilemap.btn.clear)");
		if (Wui::Button(ctx, Wui::HashId("tilemap.btn.clear"), btnClear, "🗑 清空", theme))
		{
			ClearAll();
		}

		curY += 40.0f;
		const float contentH = rect.H - (curY - rect.Y) - 8.0f;
		const float leftW = std::min(340.0f, rect.W * 0.35f);
		const float rightX = rect.X + 8.0f + leftW + 8.0f;
		const float rightW = rect.W - 16.0f - leftW - 8.0f;

		// 3. Left Column: Map Config & Visual Tile Palette
		const Wui::WuiRect leftCol { rect.X + 8.0f, curY, leftW, contentH };
		Wui::PanelBackground(ctx, leftCol, theme.ContentBg, 4.0f);

		float lY = leftCol.Y + 8.0f;
		Wui::Label(ctx, { leftCol.X + 10.0f, lY }, "地图与图集配置 (Map & Atlas Setup)", theme.Text, 12.0f);
		lY += 22.0f;

		// Atlas path SearchableCombo + Open Atlas button
		Wui::Label(ctx, { leftCol.X + 10.0f, lY + 3.0f }, "图集资产:", theme.TextMuted, 11.0f);
		const float openBtnW = 68.0f;
		const Wui::WuiRect atlasFieldRect { leftCol.X + 68.0f, lY, leftCol.W - 76.0f - openBtnW - 4.0f, 22.0f };
		Wui::Tooltip(ctx, atlasFieldRect, "从工程资产中选择关联 2D 图集 (.watlas)，无需手动输入路径 (tilemap.field.atlas_path)");

		const auto& atlasPaths = Editor::AssetCatalog::PathsForName("SpriteAtlas");
		std::vector<std::string> atlasOptions;
		atlasOptions.push_back("(none)");
		for (const auto& p : atlasPaths) atlasOptions.push_back(p);
		int atlasSelected = 0;
		const auto foundAtlas = std::find(atlasOptions.begin(), atlasOptions.end(), m_EditAtlasPath);
		if (foundAtlas != atlasOptions.end())
			atlasSelected = static_cast<int>(foundAtlas - atlasOptions.begin());
		else if (!m_EditAtlasPath.empty())
		{
			atlasOptions.push_back(m_EditAtlasPath);
			atlasSelected = static_cast<int>(atlasOptions.size()) - 1;
		}
		const int beforeAtlasPick = atlasSelected;
		if (Wui::SearchableCombo(ctx, Wui::HashId("tilemap.field.atlas_path"), atlasFieldRect, "", atlasOptions, atlasSelected, theme)
			&& atlasSelected != beforeAtlasPick)
		{
			m_EditAtlasPath = atlasSelected <= 0 ? "" : atlasOptions[static_cast<size_t>(atlasSelected)];
			m_Tilemap->SetAtlasPath(m_EditAtlasPath);
			m_LoadedAtlasPath.clear();
			EnsureAtlasAndTextureLoaded();
			m_Dirty = true;
		}

		const Wui::WuiRect btnOpenAtlas { atlasFieldRect.X + atlasFieldRect.W + 4.0f, lY, openBtnW, 22.0f };
		Wui::Tooltip(ctx, btnOpenAtlas, "在 2D 图集编辑器中打开当前关联图集 (tilemap.btn.open_atlas)");
		if (Wui::Button(ctx, Wui::HashId("tilemap.btn.open_atlas"), btnOpenAtlas, "打开图集", theme))
		{
			if (!m_EditAtlasPath.empty())
				host.OpenAtlasEditor(m_EditAtlasPath);
		}
		lY += 26.0f;

		// Grid Width & Height
		Wui::Label(ctx, { leftCol.X + 10.0f, lY + 3.0f }, "网格宽高:", theme.TextMuted, 11.0f);
		const float halfFieldW = (leftCol.W - 82.0f) * 0.5f;
		const Wui::WuiRect gwRect { leftCol.X + 68.0f, lY, halfFieldW, 22.0f };
		const Wui::WuiRect ghRect { leftCol.X + 72.0f + halfFieldW, lY, halfFieldW, 22.0f };
		Wui::Tooltip(ctx, gwRect, "瓦片地图列数 Width (1..128) (tilemap.field.grid_w)");
		Wui::Tooltip(ctx, ghRect, "瓦片地图行数 Height (1..128) (tilemap.field.grid_h)");
		int64_t oldGW = m_EditGridW;
		int64_t oldGH = m_EditGridH;
		if (Wui::DragInt(ctx, Wui::HashId("tilemap.field.grid_w"), gwRect, m_EditGridW, 1, 128, theme) ||
			Wui::DragInt(ctx, Wui::HashId("tilemap.field.grid_h"), ghRect, m_EditGridH, 1, 128, theme))
		{
			m_EditGridW = std::clamp<int64_t>(m_EditGridW, 1, 128);
			m_EditGridH = std::clamp<int64_t>(m_EditGridH, 1, 128);
			if (m_EditGridW != oldGW || m_EditGridH != oldGH)
			{
				m_Tilemap->Resize(static_cast<uint32_t>(m_EditGridW), static_cast<uint32_t>(m_EditGridH));
				m_Dirty = true;
			}
		}
		lY += 26.0f;

		// Cell Size W & H
		Wui::Label(ctx, { leftCol.X + 10.0f, lY + 3.0f }, "单格尺寸:", theme.TextMuted, 11.0f);
		const Wui::WuiRect cwRect { leftCol.X + 68.0f, lY, halfFieldW, 22.0f };
		const Wui::WuiRect chRect { leftCol.X + 72.0f + halfFieldW, lY, halfFieldW, 22.0f };
		Wui::Tooltip(ctx, cwRect, "单格世界宽度 (m) (tilemap.field.cell_w)");
		Wui::Tooltip(ctx, chRect, "单格世界高度 (m) (tilemap.field.cell_h)");
		float oldCW = m_EditCellW;
		float oldCH = m_EditCellH;
		if (Wui::DragFloat(ctx, Wui::HashId("tilemap.field.cell_w"), cwRect, m_EditCellW, 0.05f, 0.05f, 64.0f, theme) ||
			Wui::DragFloat(ctx, Wui::HashId("tilemap.field.cell_h"), chRect, m_EditCellH, 0.05f, 0.05f, 64.0f, theme))
		{
			if (m_EditCellW != oldCW || m_EditCellH != oldCH)
			{
				m_Tilemap->SetCellSize({ m_EditCellW, m_EditCellH });
				m_Dirty = true;
			}
		}
		lY += 28.0f;

		// Visual Tile Palette Header
		Wui::Label(ctx, { leftCol.X + 10.0f, lY }, "瓦片调色板 (Visual Tile Palette)", theme.Accent, 11.5f);
		lY += 20.0f;

		const float palH = std::max(40.0f, leftCol.H - (lY - leftCol.Y) - 6.0f);
		const Wui::WuiRect palRect { leftCol.X + 6.0f, lY, leftCol.W - 12.0f, palH };
		Wui::PanelBackground(ctx, palRect, theme.PanelBg, 3.0f);

		if (m_Atlas && m_Atlas->GetSubTextureCount() > 0)
		{
			const auto& allSubs = m_Atlas->GetAllSubTextures();
			const float rowH = 34.0f;
			const float totalH = static_cast<float>(allSubs.size()) * rowH + 8.0f;
			Wui::BeginScrollArea(ctx, palRect, totalH, m_PaletteScrollY, theme, Wui::HashId("tilemap.scroll.palette"));

			float itemY = palRect.Y - m_PaletteScrollY + 4.0f;
			const float invAtlasW = m_Atlas->GetWidth() > 0 ? (1.0f / static_cast<float>(m_Atlas->GetWidth())) : 1.0f;
			const float invAtlasH = m_Atlas->GetHeight() > 0 ? (1.0f / static_cast<float>(m_Atlas->GetHeight())) : 1.0f;

			for (size_t i = 0; i < allSubs.size(); ++i)
			{
				const int tileId = static_cast<int>(i) + 1;
				const bool selected = (m_SelectedTileId == tileId);
				const auto& sub = allSubs[i];

				const Wui::WuiRect thumbRect { palRect.X + 6.0f, itemY + 2.0f, 28.0f, 28.0f };
				if (m_UiTextureId != 0 && m_Atlas->GetWidth() > 0 && m_Atlas->GetHeight() > 0)
				{
					const float u0 = sub.PixelRect.x * invAtlasW;
					const float v0 = sub.PixelRect.y * invAtlasH;
					const float uW = sub.PixelRect.z * invAtlasW;
					const float vH = sub.PixelRect.w * invAtlasH;
					Wui::Image(ctx, thumbRect, m_UiTextureId, { u0, v0, uW, vH }, theme);
				}
				else
				{
					Wui::PanelBackground(ctx, thumbRect, theme.ContentBg, 2.0f);
				}
				Wui::HighlightOutline(ctx, thumbRect, selected ? theme.Accent : theme.Border, 2.0f, 1.0f);

				const Wui::WuiRect itemBtnRect { palRect.X + 38.0f, itemY + 2.0f, palRect.W - 44.0f, 28.0f };
				std::string itemLabel = (selected ? "● #" : "  #") + std::to_string(tileId) + " " + sub.Name
					+ " [" + std::to_string(static_cast<int>(sub.PixelRect.z)) + "x"
					+ std::to_string(static_cast<int>(sub.PixelRect.w)) + "]";

				const std::string itemId = "tilemap.palette.item." + std::to_string(i);
				if (Wui::Button(ctx, Wui::HashId(itemId.c_str()), itemBtnRect, itemLabel, theme))
				{
					m_SelectedTileId = tileId;
					if (m_ActiveTool == TilePaintTool::Eraser)
						m_ActiveTool = TilePaintTool::Brush;
				}
				if (selected)
				{
					Wui::HighlightOutline(ctx, itemBtnRect, theme.Accent, 2.0f, 1.0f);
				}

				itemY += rowH;
			}
			Wui::EndScrollArea(ctx);
		}
		else
		{
			Wui::Label(ctx, { palRect.X + 16.0f, palRect.Y + 16.0f }, "未关联有效图集或图集内无子切片", theme.TextMuted, 11.5f);
		}

		// 4. Right Column: Interactive 2D Tile Grid Canvas + Viewport Zoom/Pan + Status Bar
		const Wui::WuiRect rightCol { rightX, curY, rightW, contentH };
		Wui::PanelBackground(ctx, rightCol, theme.ContentBg, 4.0f);

		std::string toolTitle = "笔刷 (Brush)";
		if (m_ActiveTool == TilePaintTool::Eraser) toolTitle = "橡皮擦 (Eraser)";
		else if (m_ActiveTool == TilePaintTool::FloodFill) toolTitle = "油漆桶 (Flood Fill)";
		else if (m_ActiveTool == TilePaintTool::RectFill) toolTitle = "矩形填充 (Rect Fill)";
		else if (m_ActiveTool == TilePaintTool::CollisionToggle) toolTitle = "碰撞涂刷 (Collision)";

		// 4.1 Canvas Viewport Toolbar (Tool Status on left, Zoom Controls on right)
		const Wui::WuiRect canvasBar { rightCol.X + 8.0f, rightCol.Y + 6.0f, rightCol.W - 16.0f, 26.0f };
		Wui::PanelBackground(ctx, canvasBar, theme.PanelHeader, 3.0f);

		std::string toolInfo = "2D 网格画布 · 当前工具: " + toolTitle + " | 选瓦片 #" + std::to_string(m_SelectedTileId);
		Wui::Label(ctx, { canvasBar.X + 8.0f, canvasBar.Y + 5.0f }, toolInfo, theme.Accent, 11.5f);

		// Zoom Control Buttons on the right
		float zX = canvasBar.X + canvasBar.W - 275.0f;
		const std::string zoomPercentStr = "缩放: " + std::to_string(static_cast<int>(std::round(m_Zoom * 100.0f))) + "%";
		Wui::Label(ctx, { zX, canvasBar.Y + 6.0f }, zoomPercentStr, theme.Text, 11.0f);
		zX += 72.0f;

		// [＋ 放大]
		const Wui::WuiRect btnZoomIn { zX, canvasBar.Y + 2.0f, 42.0f, 22.0f };
		Wui::Tooltip(ctx, btnZoomIn, "放大网格画布 (也可使用鼠标滚轮向前滚动) (tilemap.btn.zoom_in)");
		if (Wui::Button(ctx, Wui::HashId("tilemap.btn.zoom_in"), btnZoomIn, "＋", theme))
		{
			SetZoom(m_Zoom * 1.25f);
		}
		zX += 46.0f;

		// [－ 缩小]
		const Wui::WuiRect btnZoomOut { zX, canvasBar.Y + 2.0f, 42.0f, 22.0f };
		Wui::Tooltip(ctx, btnZoomOut, "缩小网格画布 (也可使用鼠标滚轮向后滚动) (tilemap.btn.zoom_out)");
		if (Wui::Button(ctx, Wui::HashId("tilemap.btn.zoom_out"), btnZoomOut, "－", theme))
		{
			SetZoom(m_Zoom * 0.8f);
		}
		zX += 46.0f;

		// [⟲ 适应]
		const Wui::WuiRect btnFit { zX, canvasBar.Y + 2.0f, 52.0f, 22.0f };
		Wui::Tooltip(ctx, btnFit, "重置缩放并居中自适应网格画布 (tilemap.btn.zoom_fit)");
		if (Wui::Button(ctx, Wui::HashId("tilemap.btn.zoom_fit"), btnFit, "⟲ 适应", theme))
		{
			ResetView();
		}
		zX += 56.0f;

		// [1:1]
		const Wui::WuiRect btn11 { zX, canvasBar.Y + 2.0f, 44.0f, 22.0f };
		Wui::Tooltip(ctx, btn11, "恢复 100% 原始大小 (tilemap.btn.zoom_100)");
		if (Wui::Button(ctx, Wui::HashId("tilemap.btn.zoom_100"), btn11, "1:1", theme))
		{
			m_Zoom = 1.0f;
		}

		// 4.2 Interactive Canvas Surface
		const Wui::WuiRect innerCanvas { rightCol.X + 8.0f, rightCol.Y + 36.0f, rightCol.W - 16.0f, std::max(60.0f, rightCol.H - 64.0f) };
		Wui::DrawPanelSurface(ctx, innerCanvas, theme);
		ctx.PushClipRect(innerCanvas);

		// Handle Mouse Wheel Zooming (centered on mouse cursor)
		if (ctx.IsHovered(innerCanvas))
		{
			const float wheel = ctx.Input().Wheel;
			if (wheel != 0.0f)
			{
				const float oldZoom = m_Zoom;
				const float factor = (wheel > 0.0f) ? 1.15f : 0.87f;
				const float newZoom = std::clamp(m_Zoom * factor, 0.2f, 8.0f);
				if (std::abs(newZoom - oldZoom) > 0.001f)
				{
					const glm::vec2 mouse = ctx.Input().MousePos;
					const float centerRelX = mouse.x - (innerCanvas.X + innerCanvas.W * 0.5f);
					const float centerRelY = mouse.y - (innerCanvas.Y + innerCanvas.H * 0.5f);
					m_PanX = (m_PanX - centerRelX) * (newZoom / oldZoom) + centerRelX;
					m_PanY = (m_PanY - centerRelY) * (newZoom / oldZoom) + centerRelY;
					m_Zoom = newZoom;
				}
			}

			// Handle Right-Click or Middle-Click Pan Drag
			if (ctx.Input().MouseClicked[1] || ctx.Input().MouseClicked[2])
			{
				m_IsPanning = true;
				m_PanStartMouse = ctx.Input().MousePos;
				m_PanStartPanX = m_PanX;
				m_PanStartPanY = m_PanY;
			}
		}

		if (m_IsPanning)
		{
			if (ctx.Input().MouseDown[1] || ctx.Input().MouseDown[2])
			{
				m_PanX = m_PanStartPanX + (ctx.Input().MousePos.x - m_PanStartMouse.x);
				m_PanY = m_PanStartPanY + (ctx.Input().MousePos.y - m_PanStartMouse.y);
			}
			else
			{
				m_IsPanning = false;
			}
		}

		const uint32_t gridW = std::max(1u, m_Tilemap->GetWidth());
		const uint32_t gridH = std::max(1u, m_Tilemap->GetHeight());
		const float availW = std::max(32.0f, innerCanvas.W - 24.0f);
		const float availH = std::max(32.0f, innerCanvas.H - 24.0f);
		const float baseCellPx = std::max(4.0f, std::floor(std::min(availW / static_cast<float>(gridW), availH / static_cast<float>(gridH))));
		const float cellPx = std::max(2.0f, baseCellPx * m_Zoom);
		const float totalGridPxW = cellPx * static_cast<float>(gridW);
		const float totalGridPxH = cellPx * static_cast<float>(gridH);
		const float gridOriginX = innerCanvas.X + (innerCanvas.W - totalGridPxW) * 0.5f + m_PanX;
		const float gridOriginY = innerCanvas.Y + (innerCanvas.H - totalGridPxH) * 0.5f + m_PanY;
		const Wui::WuiRect gridBounds { gridOriginX, gridOriginY, totalGridPxW, totalGridPxH };

		Wui::PanelBackground(ctx, innerCanvas, theme.PanelBg, 1.0f);

		const float invAtlasW = (m_Atlas && m_Atlas->GetWidth() > 0) ? (1.0f / static_cast<float>(m_Atlas->GetWidth())) : 1.0f;
		const float invAtlasH = (m_Atlas && m_Atlas->GetHeight() > 0) ? (1.0f / static_cast<float>(m_Atlas->GetHeight())) : 1.0f;

		// Draw each cell (row = gridH - 1 - y so y=0 is at the bottom, matching +Y up world space)
		for (uint32_t y = 0; y < gridH; ++y)
		{
			const uint32_t row = gridH - 1u - y;
			for (uint32_t x = 0; x < gridW; ++x)
			{
				const Wui::WuiRect cellRect {
					gridOriginX + static_cast<float>(x) * cellPx,
					gridOriginY + static_cast<float>(row) * cellPx,
					cellPx,
					cellPx
				};

				// Viewport clipping: skip cells outside innerCanvas
				if (cellRect.X + cellRect.W < innerCanvas.X || cellRect.X > innerCanvas.X + innerCanvas.W ||
					cellRect.Y + cellRect.H < innerCanvas.Y || cellRect.Y > innerCanvas.Y + innerCanvas.H)
				{
					continue;
				}

				const uint16_t tileId = m_Tilemap->GetTile(x, y);
				if (tileId > 0)
				{
					if (m_UiTextureId != 0 && m_Atlas && tileId <= m_Atlas->GetSubTextureCount()
						&& m_Atlas->GetWidth() > 0 && m_Atlas->GetHeight() > 0)
					{
						if (const SubTexture2D* sub = m_Atlas->GetSubTexture(static_cast<size_t>(tileId - 1)))
						{
							const float u0 = sub->PixelRect.x * invAtlasW;
							const float v0 = sub->PixelRect.y * invAtlasH;
							const float uW = sub->PixelRect.z * invAtlasW;
							const float vH = sub->PixelRect.w * invAtlasH;
							Wui::Image(ctx, cellRect, m_UiTextureId, { u0, v0, uW, vH }, theme);
						}
					}
					else
					{
						Wui::PanelBackground(ctx,
							{ cellRect.X + 1.0f, cellRect.Y + 1.0f, std::max(1.0f, cellRect.W - 2.0f), std::max(1.0f, cellRect.H - 2.0f) },
							theme.Accent, 1.0f);
					}
				}

				if (m_ShowCollisionOverlay && m_Tilemap->IsSolid(x, y))
				{
					Wui::HighlightOutline(ctx,
						{ cellRect.X + 1.0f, cellRect.Y + 1.0f, std::max(1.0f, cellRect.W - 2.0f), std::max(1.0f, cellRect.H - 2.0f) },
						{ 0.95f, 0.38f, 0.18f, 0.85f }, 1.0f, 1.5f);
				}
			}
		}

		// Low-contrast grid lines
		const Wui::WuiColor gridLineCol { 0.38f, 0.42f, 0.48f, 0.35f };
		for (uint32_t x = 0; x <= gridW; ++x)
		{
			const float lx = gridOriginX + static_cast<float>(x) * cellPx;
			if (lx >= innerCanvas.X && lx <= innerCanvas.X + innerCanvas.W)
			{
				const float ly0 = std::max(innerCanvas.Y, gridOriginY);
				const float ly1 = std::min(innerCanvas.Y + innerCanvas.H, gridOriginY + totalGridPxH);
				if (ly1 > ly0)
					Wui::LineSegment(ctx, { lx, ly0 }, { lx, ly1 }, gridLineCol, 1.0f);
			}
		}
		for (uint32_t r = 0; r <= gridH; ++r)
		{
			const float ly = gridOriginY + static_cast<float>(r) * cellPx;
			if (ly >= innerCanvas.Y && ly <= innerCanvas.Y + innerCanvas.H)
			{
				const float lx0 = std::max(innerCanvas.X, gridOriginX);
				const float lx1 = std::min(innerCanvas.X + innerCanvas.W, gridOriginX + totalGridPxW);
				if (lx1 > lx0)
					Wui::LineSegment(ctx, { lx0, ly }, { lx1, ly }, gridLineCol, 1.0f);
			}
		}

		// Merged Box2D collider rectangles overlay
		const std::vector<MergedColliderRect> mergedRects = m_Tilemap->ComputeMergedColliderRects();
		if (m_ShowCollisionOverlay)
		{
			const Wui::WuiColor mergedCol { 0.20f, 0.95f, 0.42f, 0.95f };
			for (const auto& mr : mergedRects)
			{
				const float sx = gridOriginX + mr.X * cellPx;
				const float sy = gridOriginY + (static_cast<float>(gridH) - (mr.Y + mr.Height)) * cellPx;
				const float sw = mr.Width * cellPx;
				const float sh = mr.Height * cellPx;
				Wui::HighlightOutline(ctx, { sx, sy, sw, sh }, mergedCol, 2.0f, 2.0f);
			}
		}

		// Mouse interaction & hover preview inside the grid (taking zoom and pan into account)
		if (ctx.IsHovered(innerCanvas) && !m_IsPanning)
		{
			const glm::vec2 mouse = ctx.Input().MousePos;
			const float relX = mouse.x - gridOriginX;
			const float relY = mouse.y - gridOriginY;

			if (relX >= 0.0f && relX < totalGridPxW && relY >= 0.0f && relY < totalGridPxH)
			{
				const uint32_t gx = static_cast<uint32_t>(relX / cellPx);
				const uint32_t row = static_cast<uint32_t>(relY / cellPx);
				if (gx < gridW && row < gridH)
				{
					const uint32_t gy = gridH - 1u - row;

					const Wui::WuiRect hoverCellRect {
						gridOriginX + static_cast<float>(gx) * cellPx,
						gridOriginY + static_cast<float>(row) * cellPx,
						cellPx,
						cellPx
					};
					Wui::HighlightOutline(ctx, hoverCellRect, theme.Accent, 1.0f, 1.5f);

					// Rect Fill Preview Outline
					if (m_ActiveTool == TilePaintTool::RectFill && m_HasRectStart)
					{
						const uint32_t minX = std::min(m_RectStartX, gx);
						const uint32_t maxX = std::max(m_RectStartX, gx);
						const uint32_t minY = std::min(m_RectStartY, gy);
						const uint32_t maxY = std::max(m_RectStartY, gy);
						const float rx = gridOriginX + static_cast<float>(minX) * cellPx;
						const float ry = gridOriginY + static_cast<float>(gridH - 1u - maxY) * cellPx;
						const float rw = static_cast<float>(maxX - minX + 1u) * cellPx;
						const float rh = static_cast<float>(maxY - minY + 1u) * cellPx;
						Wui::HighlightOutline(ctx, { rx, ry, rw, rh }, theme.Accent, 2.0f, 2.0f);
					}

					const bool clicked = ctx.Input().MouseClicked[0];
					const bool held = ctx.Input().MouseDown[0];
					const uint16_t activeTile = static_cast<uint16_t>(std::max(1, m_SelectedTileId));

					if (m_ActiveTool == TilePaintTool::Brush && (clicked || held))
					{
						PaintCell(gx, gy, activeTile, m_PaintSolid);
					}
					else if (m_ActiveTool == TilePaintTool::Eraser && (clicked || held))
					{
						PaintCell(gx, gy, 0, false);
					}
					else if (m_ActiveTool == TilePaintTool::FloodFill && clicked)
					{
						FloodFillCells(gx, gy, activeTile, m_PaintSolid);
					}
					else if (m_ActiveTool == TilePaintTool::RectFill && clicked)
					{
						if (!m_HasRectStart)
						{
							m_HasRectStart = true;
							m_RectStartX = gx;
							m_RectStartY = gy;
							m_StatusFeedback = "矩形起点: (" + std::to_string(gx) + ", " + std::to_string(gy) + ")，请点击终点完成填充";
						}
						else
						{
							FillRectCells(m_RectStartX, m_RectStartY, gx, gy, activeTile, m_PaintSolid);
							m_HasRectStart = false;
							m_StatusFeedback = "已完成矩形区域填充";
						}
					}
					else if (m_ActiveTool == TilePaintTool::CollisionToggle && clicked)
					{
						const bool nextSolid = !m_Tilemap->IsSolid(gx, gy);
						m_Tilemap->SetSolid(gx, gy, nextSolid);
						m_Dirty = true;
					}
				}
			}
		}

		// Bottom Status Bar with real-time painted & merged collider counts and zoom feedback
		const uint32_t paintedCount = m_Tilemap->CountPaintedTiles();
		const uint32_t solidCount = m_Tilemap->CountSolidTiles();
		const size_t mergedCount = mergedRects.size();

		std::ostringstream statusLine;
		statusLine << "已绘制瓦片: " << paintedCount
				   << " | 独立碰撞格: " << solidCount
				   << " -> 合并后 Box2D 碰撞盒: " << mergedCount << " 个"
				   << " | 缩放: " << static_cast<int>(std::round(m_Zoom * 100.0f)) << "%"
				   << " | 提示: 鼠标滚轮缩放画布，按住右键/中键拖拽平移";
		if (!m_StatusFeedback.empty())
			statusLine << "  |  " << m_StatusFeedback;

		ctx.PopClipRect();

		Wui::Label(ctx, { rightCol.X + 12.0f, rightCol.Y + rightCol.H - 22.0f }, statusLine.str(), theme.TextMuted, 10.5f);
	}
}