#include "wldpch.h"
#include "WUI/Panels/SpriteAtlasEditorPanel.h"
#include "World/Renderer/SpriteAnimation.h"
#include "World/Core/Log.h"
#include "World/Utils/Paths.h"
#include "World/WUI/WuiWidgets.h"
#include "World/WUI/Widgets/WuiChrome.h"
#include "World/Renderer/Texture/TextureLibrary.h"
#include "World/WUI/WuiTextureRegistry.h"
#include "World/Core/StringPool.h"

#include <filesystem>
#include <algorithm>
#include <sstream>
#include <iomanip>
#include <cmath>

namespace World
{
	SpriteAtlasEditorPanel::SpriteAtlasEditorPanel(std::string logicalPath)
		: m_LogicalPath(std::move(logicalPath))
	{
		std::replace(m_LogicalPath.begin(), m_LogicalPath.end(), '\\', '/');
		m_PanelId = "atlas:" + m_LogicalPath;
		m_PanelTitle = "图集: " + std::filesystem::path(m_LogicalPath).filename().string();
		Reload();
	}

	std::filesystem::path SpriteAtlasEditorPanel::ResolvePhysicalPath() const
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

	void SpriteAtlasEditorPanel::EnsureTextureLoaded()
	{
		if (!m_Atlas || m_Atlas->GetTexturePath().empty())
			return;
		std::string texPath = m_Atlas->GetTexturePath();
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

	bool SpriteAtlasEditorPanel::Reload(std::string* message)
	{
		const auto phys = ResolvePhysicalPath();
		m_Atlas = TextureAtlas::LoadFromFile(phys.string());
		if (!m_Atlas)
		{
			m_StatusFeedback = "无法载入图集: " + m_LogicalPath;
			if (message) *message = m_StatusFeedback;
			return false;
		}

		m_EditTexturePath = m_Atlas->GetTexturePath();
		m_EditAtlasW = m_Atlas->GetWidth();
		m_EditAtlasH = m_Atlas->GetHeight();
		m_SelectedIndex = m_Atlas->GetSubTextureCount() > 0 ? 0 : -1;
		SyncFromSelected();
		m_Dirty = false;
		m_StatusFeedback = "就绪: 已载入 " + std::to_string(m_Atlas->GetSubTextureCount()) + " 个子精灵";
		if (message) *message = m_StatusFeedback;
		return true;
	}

	bool SpriteAtlasEditorPanel::Save(std::string* message)
	{
		CommitToSelected();
		if (!m_Atlas)
		{
			m_StatusFeedback = "无可用图集数据保存";
			if (message) *message = m_StatusFeedback;
			return false;
		}

		m_Atlas->SetTexturePath(m_EditTexturePath);
		m_Atlas->SetDimensions(static_cast<uint32_t>(std::max<int64_t>(1, m_EditAtlasW)),
			static_cast<uint32_t>(std::max<int64_t>(1, m_EditAtlasH)));

		const auto phys = ResolvePhysicalPath();
		if (!m_Atlas->SaveToFile(phys.string()))
		{
			m_StatusFeedback = "保存失败: " + phys.string();
			if (message) *message = m_StatusFeedback;
			return false;
		}

		TextureAtlas::ClearCache();
		m_Dirty = false;
		m_StatusFeedback = "图集已保存: " + m_LogicalPath;
		if (message) *message = m_StatusFeedback;
		return true;
	}

	void SpriteAtlasEditorPanel::SyncFromSelected()
	{
		if (!m_Atlas || m_SelectedIndex < 0 || m_SelectedIndex >= static_cast<int>(m_Atlas->GetSubTextureCount()))
		{
			m_EditName.clear();
			m_EditRectX = m_EditRectY = 0.0f;
			m_EditRectW = m_EditRectH = 32.0f;
			m_EditPivotX = m_EditPivotY = 0.5f;
			m_EditBorderL = m_EditBorderR = m_EditBorderT = m_EditBorderB = 0.0f;
			return;
		}

		const SubTexture2D* sub = m_Atlas->GetSubTexture(static_cast<size_t>(m_SelectedIndex));
		if (!sub) return;

		m_EditName = sub->Name;
		m_EditRectX = sub->PixelRect.x;
		m_EditRectY = sub->PixelRect.y;
		m_EditRectW = sub->PixelRect.z;
		m_EditRectH = sub->PixelRect.w;
		m_EditPivotX = sub->Pivot.x;
		m_EditPivotY = sub->Pivot.y;
		m_EditBorderL = sub->Border.x;
		m_EditBorderR = sub->Border.y;
		m_EditBorderT = sub->Border.z;
		m_EditBorderB = sub->Border.w;
	}

	void SpriteAtlasEditorPanel::CommitToSelected()
	{
		if (!m_Atlas || m_SelectedIndex < 0 || m_SelectedIndex >= static_cast<int>(m_Atlas->GetSubTextureCount()))
			return;

		std::vector<SubTexture2D> list = m_Atlas->GetAllSubTextures();
		if (static_cast<size_t>(m_SelectedIndex) >= list.size())
			return;

		const uint32_t atlasW = m_Atlas->GetWidth();
		const uint32_t atlasH = m_Atlas->GetHeight();
		list[static_cast<size_t>(m_SelectedIndex)] = SubTexture2D(
			m_EditName,
			{ m_EditRectX, m_EditRectY, m_EditRectW, m_EditRectH },
			atlasW,
			atlasH,
			{ m_EditPivotX, m_EditPivotY },
			{ m_EditBorderL, m_EditBorderR, m_EditBorderT, m_EditBorderB }
		);

		auto rebuilt = std::make_shared<TextureAtlas>(m_Atlas->GetTexturePath(), atlasW, atlasH);
		for (const auto& s : list)
			rebuilt->AddSubTexture(s);
		m_Atlas = std::move(rebuilt);
	}

	void SpriteAtlasEditorPanel::AddSprite(const std::string& name, const glm::vec4& pixelRect,
		const glm::vec2& pivot, const glm::vec4& border)
	{
		if (!m_Atlas)
			m_Atlas = std::make_shared<TextureAtlas>(m_EditTexturePath, static_cast<uint32_t>(m_EditAtlasW), static_cast<uint32_t>(m_EditAtlasH));

		CommitToSelected();
		const uint32_t atlasW = m_Atlas->GetWidth();
		const uint32_t atlasH = m_Atlas->GetHeight();
		SubTexture2D sub(name, pixelRect, atlasW, atlasH, pivot, border);
		m_Atlas->AddSubTexture(sub);
		m_SelectedIndex = static_cast<int>(m_Atlas->GetSubTextureCount()) - 1;
		SyncFromSelected();
		m_Dirty = true;
		m_StatusFeedback = "已添加子精灵: " + name;
	}

	bool SpriteAtlasEditorPanel::DeleteSprite(size_t index)
	{
		if (!m_Atlas || index >= m_Atlas->GetSubTextureCount())
			return false;

		std::vector<SubTexture2D> list = m_Atlas->GetAllSubTextures();
		std::string delName = list[index].Name;
		list.erase(list.begin() + static_cast<std::ptrdiff_t>(index));

		const uint32_t atlasW = m_Atlas->GetWidth();
		const uint32_t atlasH = m_Atlas->GetHeight();
		auto rebuilt = std::make_shared<TextureAtlas>(m_Atlas->GetTexturePath(), atlasW, atlasH);
		for (const auto& s : list)
			rebuilt->AddSubTexture(s);
		m_Atlas = std::move(rebuilt);

		if (m_Atlas->GetSubTextureCount() == 0)
			m_SelectedIndex = -1;
		else if (m_SelectedIndex >= static_cast<int>(m_Atlas->GetSubTextureCount()))
			m_SelectedIndex = static_cast<int>(m_Atlas->GetSubTextureCount()) - 1;

		SyncFromSelected();
		m_Dirty = true;
		m_StatusFeedback = "已删除子精灵: " + delName;
		return true;
	}

	void SpriteAtlasEditorPanel::AutoGridSlice(float cellW, float cellH)
	{
		if (cellW <= 0.0f || cellH <= 0.0f) return;
		if (!m_Atlas)
			m_Atlas = std::make_shared<TextureAtlas>(m_EditTexturePath, static_cast<uint32_t>(m_EditAtlasW), static_cast<uint32_t>(m_EditAtlasH));

		const uint32_t atlasW = m_Atlas->GetWidth();
		const uint32_t atlasH = m_Atlas->GetHeight();
		const int cols = static_cast<int>(atlasW / cellW);
		const int rows = static_cast<int>(atlasH / cellH);
		if (cols <= 0 || rows <= 0) return;

		auto rebuilt = std::make_shared<TextureAtlas>(m_Atlas->GetTexturePath(), atlasW, atlasH);
		int count = 0;
		for (int r = 0; r < rows; ++r)
		{
			for (int c = 0; c < cols; ++c)
			{
				std::string name = "slice_" + std::to_string(count++);
				glm::vec4 rect { static_cast<float>(c) * cellW, static_cast<float>(r) * cellH, cellW, cellH };
				rebuilt->AddSubTexture(SubTexture2D(name, rect, atlasW, atlasH, { 0.5f, 0.5f }));
			}
		}
		m_Atlas = std::move(rebuilt);
		m_SelectedIndex = m_Atlas->GetSubTextureCount() > 0 ? 0 : -1;
		SyncFromSelected();
		m_Dirty = true;
		m_StatusFeedback = "自动网格切分完成: 共切分出 " + std::to_string(count) + " 个子精灵";
	}

	void SpriteAtlasEditorPanel::OnRender(Wui::WuiContext& ctx, const Wui::WuiRect& rect, PanelHost& host)
	{
		const Wui::WuiTheme& theme = host.Theme();
		Wui::PanelBackground(ctx, rect, theme.PanelBg);

		EnsureTextureLoaded();

		float curY = rect.Y + 6.0f;

		// 1. Top Bar: Header, logical path, Save button
		const Wui::WuiRect topBar { rect.X + 8.0f, curY, rect.W - 16.0f, 32.0f };
		Wui::BarSurface(ctx, topBar, theme.PanelHeader, theme.Border);
		Wui::Label(ctx, { topBar.X + 10.0f, topBar.Y + 7.0f }, "2D 图集编辑器 (Sprite Atlas Workstation)", theme.Accent, 13.5f);

		std::string pathText = "资产: " + m_LogicalPath;
		Wui::Label(ctx, { topBar.X + 280.0f, topBar.Y + 8.0f }, pathText, theme.TextMuted, 11.0f);

		const Wui::WuiRect btnSave { topBar.X + topBar.W - 110.0f, topBar.Y + 4.0f, 102.0f, 24.0f };
		Wui::Tooltip(ctx, btnSave, "保存图集配置到 .watlas 文件并刷新缓存 (atlas.btn.save)");
		if (Wui::Button(ctx, Wui::HashId("atlas.btn.save"), btnSave, m_Dirty ? "💾 保存图集 *" : "💾 保存图集", theme))
		{
			Save();
		}

		curY += 38.0f;
		const float contentH = rect.H - (curY - rect.Y) - 8.0f;
		const float leftW = std::min(320.0f, rect.W * 0.36f);
		const float rightX = rect.X + 8.0f + leftW + 8.0f;
		const float rightW = rect.W - 16.0f - leftW - 8.0f;

		// 2. Left Column: Texture Configuration, Auto Grid Slicing, Slices List
		const Wui::WuiRect leftCol { rect.X + 8.0f, curY, leftW, contentH };
		Wui::PanelBackground(ctx, leftCol, theme.ContentBg, 4.0f);

		float lY = leftCol.Y + 8.0f;
		Wui::Label(ctx, { leftCol.X + 10.0f, lY }, "源纹理设置 (Texture Configuration)", theme.Text, 12.0f);
		lY += 22.0f;

		// Texture path input
		Wui::Label(ctx, { leftCol.X + 10.0f, lY + 3.0f }, "贴图路径:", theme.TextMuted, 11.0f);
		const Wui::WuiRect texRect { leftCol.X + 70.0f, lY, leftCol.W - 78.0f, 22.0f };
		std::string oldTex = m_EditTexturePath;
		if (Wui::TextField(ctx, Wui::HashId("atlas.field.tex_path"), texRect, m_EditTexturePath, theme) || m_EditTexturePath != oldTex)
		{
			if (m_Atlas) m_Atlas->SetTexturePath(m_EditTexturePath);
			m_Dirty = true;
			EnsureTextureLoaded();
		}
		lY += 26.0f;

		// Dimensions input
		Wui::Label(ctx, { leftCol.X + 10.0f, lY + 3.0f }, "大图尺寸:", theme.TextMuted, 11.0f);
		const float dimW = (leftCol.W - 100.0f) * 0.5f;
		const Wui::WuiRect dimRectW { leftCol.X + 70.0f, lY, dimW, 22.0f };
		const Wui::WuiRect dimRectH { leftCol.X + 74.0f + dimW, lY, dimW, 22.0f };
		int64_t oldW = m_EditAtlasW;
		int64_t oldH = m_EditAtlasH;
		if (Wui::DragInt(ctx, Wui::HashId("atlas.field.tex_w"), dimRectW, m_EditAtlasW, 1, 8192, theme) ||
			Wui::DragInt(ctx, Wui::HashId("atlas.field.tex_h"), dimRectH, m_EditAtlasH, 1, 8192, theme))
		{
			if (m_EditAtlasW != oldW || m_EditAtlasH != oldH)
			{
				if (m_Atlas) m_Atlas->SetDimensions(static_cast<uint32_t>(m_EditAtlasW), static_cast<uint32_t>(m_EditAtlasH));
				m_Dirty = true;
			}
		}
		lY += 30.0f;

		// Auto Grid Slice Box
		const Wui::WuiRect gridBox { leftCol.X + 6.0f, lY, leftCol.W - 12.0f, 56.0f };
		Wui::PanelBackground(ctx, gridBox, theme.PanelBg, 3.0f);
		Wui::Label(ctx, { gridBox.X + 8.0f, gridBox.Y + 5.0f }, "一键自动网格切图 (Auto Grid Slice)", theme.Accent, 11.0f);

		float gX = gridBox.X + 8.0f;
		Wui::Label(ctx, { gX, gridBox.Y + 28.0f }, "格宽:", theme.TextMuted, 10.5f);
		gX += 30.0f;
		const Wui::WuiRect gwRect { gX, gridBox.Y + 26.0f, 44.0f, 20.0f };
		Wui::DragFloat(ctx, Wui::HashId("atlas.field.grid_w"), gwRect, m_GridCellW, 1.0f, 4.0f, 512.0f, theme);
		gX += 48.0f;

		Wui::Label(ctx, { gX, gridBox.Y + 28.0f }, "高:", theme.TextMuted, 10.5f);
		gX += 20.0f;
		const Wui::WuiRect ghRect { gX, gridBox.Y + 26.0f, 44.0f, 20.0f };
		Wui::DragFloat(ctx, Wui::HashId("atlas.field.grid_h"), ghRect, m_GridCellH, 1.0f, 4.0f, 512.0f, theme);
		gX += 48.0f;

		const Wui::WuiRect btnAutoGrid { gX, gridBox.Y + 25.0f, gridBox.W - (gX - gridBox.X) - 6.0f, 22.0f };
		Wui::Tooltip(ctx, btnAutoGrid, "按设定的行列像素宽高一键自动切分整张大图 (atlas.btn.auto_grid)");
		if (Wui::Button(ctx, Wui::HashId("atlas.btn.auto_grid"), btnAutoGrid, "⚡ 自动切分", theme))
		{
			AutoGridSlice(m_GridCellW, m_GridCellH);
		}
		lY += 62.0f;

		// Actions: Add Slice / Delete Slice / Generate Animation (.wanim)
		const float btnHalfW = (leftCol.W - 20.0f) * 0.5f;
		const Wui::WuiRect btnAdd { leftCol.X + 6.0f, lY, btnHalfW, 22.0f };
		Wui::Tooltip(ctx, btnAdd, "添加新的子精灵切片 (atlas.btn.add)");
		if (Wui::Button(ctx, Wui::HashId("atlas.btn.add"), btnAdd, "＋ 添加切片", theme))
		{
			size_t curCount = m_Atlas ? m_Atlas->GetSubTextureCount() : 0;
			std::string newName = "sprite_" + std::to_string(curCount);
			AddSprite(newName, { 0.0f, 0.0f, 32.0f, 32.0f });
		}

		const Wui::WuiRect btnDel { leftCol.X + 10.0f + btnHalfW, lY, btnHalfW, 22.0f };
		Wui::Tooltip(ctx, btnDel, "删除当前选中的子精灵切片 (atlas.btn.del)");
		if (Wui::Button(ctx, Wui::HashId("atlas.btn.del"), btnDel, "✖ 删除切片", theme))
		{
			if (m_SelectedIndex >= 0)
				DeleteSprite(static_cast<size_t>(m_SelectedIndex));
		}
		lY += 26.0f;

		// One-click generate animation (.wanim) from all slices
		const Wui::WuiRect btnMakeAnim { leftCol.X + 6.0f, lY, leftCol.W - 12.0f, 22.0f };
		Wui::Tooltip(ctx, btnMakeAnim, "基于当前图集切片一键生成序列帧动画 (.wanim) 并打开编辑器 (atlas.btn.make_anim)");
		if (Wui::Button(ctx, Wui::HashId("atlas.btn.make_anim"), btnMakeAnim, "🎬 基于当前切片生成动画 (.wanim)", theme))
		{
			const auto phys = ResolvePhysicalPath();
			std::filesystem::path dir = phys.parent_path();
			std::string baseName = phys.stem().string() + "_anim";
			std::filesystem::path target = dir / (baseName + ".wanim");
			for (int idx = 1; std::filesystem::exists(target); ++idx)
				target = dir / (baseName + "_" + std::to_string(idx) + ".wanim");

			SpriteAnimation anim(phys.stem().string(), 12.0f, AnimationLoopMode::Loop);
			anim.SetAtlasPath(m_LogicalPath);
			if (m_Atlas && m_Atlas->GetSubTextureCount() > 0)
			{
				for (const auto& sub : m_Atlas->GetAllSubTextures())
					anim.AddFrame(SpriteAnimationFrame(sub.Name, 0.0833f));
			}
			else
			{
				anim.AddFrame(SpriteAnimationFrame("frame_0", 0.0833f));
			}
			anim.SaveToFile(target.string());
			m_StatusFeedback = "已生成序列帧动画: " + target.filename().string();
			std::error_code ec;
			const std::filesystem::path rel = std::filesystem::relative(target, Paths::ProjectDir(), ec);
			host.OpenAnimationEditor(ec ? target.generic_string() : rel.generic_string());
		}
		lY += 28.0f;

		// Slices List with ScrollArea
		float listH = leftCol.H - (lY - leftCol.Y) - 6.0f;
		const Wui::WuiRect listRect { leftCol.X + 6.0f, lY, leftCol.W - 12.0f, listH };
		Wui::PanelBackground(ctx, listRect, theme.PanelBg, 2.0f);

		if (m_Atlas)
		{
			const auto& allSubs = m_Atlas->GetAllSubTextures();
			const float totalH = static_cast<float>(allSubs.size()) * 26.0f + 6.0f;
			Wui::BeginScrollArea(ctx, listRect, totalH, m_ListScrollY, theme, Wui::HashId("atlas.scroll.list"));
			float itemY = listRect.Y - m_ListScrollY + 3.0f;
			for (size_t i = 0; i < allSubs.size(); ++i)
			{
				const Wui::WuiRect itemRect { listRect.X + 4.0f, itemY, listRect.W - 8.0f, 22.0f };
				bool isSelected = (static_cast<int>(i) == m_SelectedIndex);

				std::string itemLabel = (isSelected ? "● " : "  ") + allSubs[i].Name + " ["
					+ std::to_string(static_cast<int>(allSubs[i].PixelRect.z)) + "x"
					+ std::to_string(static_cast<int>(allSubs[i].PixelRect.w)) + "]";

				const std::string btnId = "atlas.item." + std::to_string(i);
				if (Wui::Button(ctx, Wui::HashId(btnId.c_str()), itemRect, itemLabel, theme))
				{
					CommitToSelected();
					m_SelectedIndex = static_cast<int>(i);
					SyncFromSelected();
				}
				itemY += 26.0f;
			}
			Wui::EndScrollArea(ctx);
		}

		// 3. Right Column: Inspector + Visual Preview Canvas
		const Wui::WuiRect rightCol { rightX, curY, rightW, contentH };
		Wui::PanelBackground(ctx, rightCol, theme.ContentBg, 4.0f);

		Wui::Label(ctx, { rightCol.X + 12.0f, rightCol.Y + 8.0f }, "子精灵参数检视 (Sprite Inspector)", theme.Text, 12.5f);

		float inspY = rightCol.Y + 30.0f;
		if (m_SelectedIndex >= 0 && m_Atlas && m_SelectedIndex < static_cast<int>(m_Atlas->GetSubTextureCount()))
		{
			bool fieldsChanged = false;

			// Row 1: Name field
			Wui::Label(ctx, { rightCol.X + 12.0f, inspY + 4.0f }, "名称 (Name):", theme.TextMuted, 11.5f);
			const Wui::WuiRect nameRect { rightCol.X + 120.0f, inspY, std::min(260.0f, rightCol.W - 140.0f), 24.0f };
			Wui::Tooltip(ctx, nameRect, "子精灵唯一标识名称 (atlas.field.name)");
			std::string oldName = m_EditName;
			if (Wui::TextField(ctx, Wui::HashId("atlas.field.name"), nameRect, m_EditName, theme) || m_EditName != oldName)
			{
				fieldsChanged = true;
			}
			inspY += 30.0f;

			// Row 2: Pixel Rect (X, Y, W, H)
			Wui::Label(ctx, { rightCol.X + 12.0f, inspY + 4.0f }, "区域 (Rect X/Y/W/H):", theme.TextMuted, 11.5f);
			float fW = std::min(70.0f, (rightCol.W - 150.0f) / 4.0f);
			float fX = rightCol.X + 120.0f;

			const Wui::WuiRect rX { fX, inspY, fW, 24.0f };
			Wui::Tooltip(ctx, rX, "子图左上角像素 X 坐标 (atlas.field.x)");
			if (Wui::DragFloat(ctx, Wui::HashId("atlas.field.x"), rX, m_EditRectX, 1.0f, 0.0f, 8192.0f, theme))
				fieldsChanged = true;
			fX += fW + 4.0f;

			const Wui::WuiRect rY { fX, inspY, fW, 24.0f };
			Wui::Tooltip(ctx, rY, "子图左上角像素 Y 坐标 (atlas.field.y)");
			if (Wui::DragFloat(ctx, Wui::HashId("atlas.field.y"), rY, m_EditRectY, 1.0f, 0.0f, 8192.0f, theme))
				fieldsChanged = true;
			fX += fW + 4.0f;

			const Wui::WuiRect rW { fX, inspY, fW, 24.0f };
			Wui::Tooltip(ctx, rW, "子图像素宽度 (atlas.field.w)");
			if (Wui::DragFloat(ctx, Wui::HashId("atlas.field.w"), rW, m_EditRectW, 1.0f, 1.0f, 8192.0f, theme))
				fieldsChanged = true;
			fX += fW + 4.0f;

			const Wui::WuiRect rH { fX, inspY, fW, 24.0f };
			Wui::Tooltip(ctx, rH, "子图像素高度 (atlas.field.h)");
			if (Wui::DragFloat(ctx, Wui::HashId("atlas.field.h"), rH, m_EditRectH, 1.0f, 1.0f, 8192.0f, theme))
				fieldsChanged = true;

			inspY += 30.0f;

			// Row 3: Pivot (X, Y)
			Wui::Label(ctx, { rightCol.X + 12.0f, inspY + 4.0f }, "中心锚点 (Pivot):", theme.TextMuted, 11.5f);
			const Wui::WuiRect pX { rightCol.X + 120.0f, inspY, fW, 24.0f };
			Wui::Tooltip(ctx, pX, "归一化轴心锚点 X [0..1] (atlas.field.pivot_x)");
			if (Wui::DragFloat(ctx, Wui::HashId("atlas.field.pivot_x"), pX, m_EditPivotX, 0.01f, 0.0f, 1.0f, theme))
				fieldsChanged = true;

			const Wui::WuiRect pY { rightCol.X + 120.0f + fW + 4.0f, inspY, fW, 24.0f };
			Wui::Tooltip(ctx, pY, "归一化轴心锚点 Y [0..1] (atlas.field.pivot_y)");
			if (Wui::DragFloat(ctx, Wui::HashId("atlas.field.pivot_y"), pY, m_EditPivotY, 0.01f, 0.0f, 1.0f, theme))
				fieldsChanged = true;

			inspY += 26.0f;

			// Fast Preset Buttons (2 rows of 3 buttons)
			float preX = rightCol.X + 120.0f;
			const float preW = 85.0f;
			const Wui::WuiRect btnP0 { preX, inspY, preW, 20.0f };
			Wui::Tooltip(ctx, btnP0, "快捷设为几何中心 (0.5, 0.5)");
			if (Wui::Button(ctx, Wui::HashId("atlas.btn.pivot_center"), btnP0, "中心 (0.5, 0.5)", theme))
			{
				m_EditPivotX = 0.5f; m_EditPivotY = 0.5f; fieldsChanged = true;
			}
			preX += preW + 4.0f;

			const Wui::WuiRect btnP1 { preX, inspY, preW, 20.0f };
			Wui::Tooltip(ctx, btnP1, "快捷设为脚底中心 (0.5, 0.0)");
			if (Wui::Button(ctx, Wui::HashId("atlas.btn.pivot_bottom"), btnP1, "脚底 (0.5, 0.0)", theme))
			{
				m_EditPivotX = 0.5f; m_EditPivotY = 0.0f; fieldsChanged = true;
			}
			preX += preW + 4.0f;

			const Wui::WuiRect btnP2 { preX, inspY, preW, 20.0f };
			Wui::Tooltip(ctx, btnP2, "快捷设为左上角 (0.0, 1.0)");
			if (Wui::Button(ctx, Wui::HashId("atlas.btn.pivot_topleft"), btnP2, "左上 (0.0, 1.0)", theme))
			{
				m_EditPivotX = 0.0f; m_EditPivotY = 1.0f; fieldsChanged = true;
			}

			inspY += 24.0f;

			preX = rightCol.X + 120.0f;
			const Wui::WuiRect btnP3 { preX, inspY, preW, 20.0f };
			Wui::Tooltip(ctx, btnP3, "快捷设为右上角 (1.0, 1.0)");
			if (Wui::Button(ctx, Wui::HashId("atlas.btn.pivot_topright"), btnP3, "右上 (1.0, 1.0)", theme))
			{
				m_EditPivotX = 1.0f; m_EditPivotY = 1.0f; fieldsChanged = true;
			}
			preX += preW + 4.0f;

			const Wui::WuiRect btnP4 { preX, inspY, preW, 20.0f };
			Wui::Tooltip(ctx, btnP4, "快捷设为左下角 (0.0, 0.0)");
			if (Wui::Button(ctx, Wui::HashId("atlas.btn.pivot_bottomleft"), btnP4, "左下 (0.0, 0.0)", theme))
			{
				m_EditPivotX = 0.0f; m_EditPivotY = 0.0f; fieldsChanged = true;
			}
			preX += preW + 4.0f;

			const Wui::WuiRect btnP5 { preX, inspY, preW, 20.0f };
			Wui::Tooltip(ctx, btnP5, "快捷设为右下角 (1.0, 0.0)");
			if (Wui::Button(ctx, Wui::HashId("atlas.btn.pivot_bottomright"), btnP5, "右下 (1.0, 0.0)", theme))
			{
				m_EditPivotX = 1.0f; m_EditPivotY = 0.0f; fieldsChanged = true;
			}

			inspY += 28.0f;

			// Row 4: 9-Slice Border (L, R, T, B)
			Wui::Label(ctx, { rightCol.X + 12.0f, inspY + 4.0f }, "九宫格边距 (Border):", theme.TextMuted, 11.5f);
			float bX = rightCol.X + 120.0f;

			const Wui::WuiRect bL { bX, inspY, fW, 24.0f };
			Wui::Tooltip(ctx, bL, "九宫格左侧边距像素 (atlas.field.border_l)");
			if (Wui::DragFloat(ctx, Wui::HashId("atlas.field.border_l"), bL, m_EditBorderL, 1.0f, 0.0f, 512.0f, theme))
				fieldsChanged = true;
			bX += fW + 4.0f;

			const Wui::WuiRect bR { bX, inspY, fW, 24.0f };
			Wui::Tooltip(ctx, bR, "九宫格右侧边距像素 (atlas.field.border_r)");
			if (Wui::DragFloat(ctx, Wui::HashId("atlas.field.border_r"), bR, m_EditBorderR, 1.0f, 0.0f, 512.0f, theme))
				fieldsChanged = true;
			bX += fW + 4.0f;

			const Wui::WuiRect bT { bX, inspY, fW, 24.0f };
			Wui::Tooltip(ctx, bT, "九宫格顶部边距像素 (atlas.field.border_t)");
			if (Wui::DragFloat(ctx, Wui::HashId("atlas.field.border_t"), bT, m_EditBorderT, 1.0f, 0.0f, 512.0f, theme))
				fieldsChanged = true;
			bX += fW + 4.0f;

			const Wui::WuiRect bB { bX, inspY, fW, 24.0f };
			Wui::Tooltip(ctx, bB, "九宫格底部边距像素 (atlas.field.border_b)");
			if (Wui::DragFloat(ctx, Wui::HashId("atlas.field.border_b"), bB, m_EditBorderB, 1.0f, 0.0f, 512.0f, theme))
				fieldsChanged = true;

			if (fieldsChanged)
			{
				m_Dirty = true;
				CommitToSelected();
			}
			inspY += 30.0f;
		}
		else
		{
			Wui::Label(ctx, { rightCol.X + 24.0f, inspY + 20.0f }, "请在左侧列表中选择一个切片，或点击 [＋ 添加切片]", theme.TextMuted, 12.0f);
			inspY += 60.0f;
		}

		// 4. Visual Slice & 9-Slice Preview Canvas
		float canvasY = inspY + 8.0f;
		float canvasH = (rightCol.Y + rightCol.H - 26.0f) - canvasY;
		if (canvasH > 50.0f)
		{
			const Wui::WuiRect canvasBox { rightCol.X + 8.0f, canvasY, rightCol.W - 16.0f, canvasH };
			Wui::PanelBackground(ctx, canvasBox, theme.PanelBg, 3.0f);

			std::string sliceName = (m_SelectedIndex >= 0 && !m_EditName.empty()) ? m_EditName : "(未选)";
			Wui::Label(ctx, { canvasBox.X + 10.0f, canvasBox.Y + 6.0f }, "切片图形与锚点预览 (Slice Preview) · " + sliceName, theme.Accent, 12.0f);

			const Wui::WuiRect innerCanvas { canvasBox.X + 6.0f, canvasBox.Y + 24.0f, canvasBox.W - 12.0f, canvasBox.H - 30.0f };
			Wui::DrawPanelSurface(ctx, innerCanvas, theme);

			if (m_SelectedIndex >= 0 && m_Atlas)
			{
				const float sliceW = m_EditRectW > 0.0f ? m_EditRectW : 32.0f;
				const float sliceH = m_EditRectH > 0.0f ? m_EditRectH : 32.0f;

				const float maxDrawW = innerCanvas.W - 24.0f;
				const float maxDrawH = innerCanvas.H - 24.0f;
				float scale = std::min(maxDrawW / sliceW, maxDrawH / sliceH);
				if (scale > 1.0f) scale = std::floor(scale);
				scale = std::max(scale, 0.1f);

				const float drawW = sliceW * scale;
				const float drawH = sliceH * scale;
				const Wui::WuiRect imgRect {
					innerCanvas.X + (innerCanvas.W - drawW) * 0.5f,
					innerCanvas.Y + (innerCanvas.H - drawH) * 0.5f,
					drawW,
					drawH
				};

				if (m_UiTextureId != 0 && m_Atlas->GetWidth() > 0 && m_Atlas->GetHeight() > 0)
				{
					const float invW = 1.0f / static_cast<float>(m_Atlas->GetWidth());
					const float invH = 1.0f / static_cast<float>(m_Atlas->GetHeight());
					const float u0 = m_EditRectX * invW;
					const float v0 = m_EditRectY * invH;
					const float uW = sliceW * invW;
					const float vH = sliceH * invH;

					Wui::Image(ctx, imgRect, m_UiTextureId, { u0, v0, uW, vH }, theme);
				}
				else
				{
					Wui::PanelBackground(ctx, imgRect, theme.PanelBg, 2.0f);
					Wui::HighlightOutline(ctx, imgRect, theme.Accent, 2.0f, 1.0f);
				}

				// 9-Slice Guidelines (green overlay lines)
				if (m_EditBorderL > 0.0f || m_EditBorderR > 0.0f || m_EditBorderT > 0.0f || m_EditBorderB > 0.0f)
				{
					const Wui::WuiColor guideCol { 0.25f, 0.95f, 0.40f, 0.85f };
					if (m_EditBorderL > 0.0f)
					{
						float lx = imgRect.X + m_EditBorderL * scale;
						Wui::LineSegment(ctx, { lx, imgRect.Y }, { lx, imgRect.Y + imgRect.H }, guideCol, 1.0f);
					}
					if (m_EditBorderR > 0.0f)
					{
						float rx = imgRect.X + (sliceW - m_EditBorderR) * scale;
						Wui::LineSegment(ctx, { rx, imgRect.Y }, { rx, imgRect.Y + imgRect.H }, guideCol, 1.0f);
					}
					if (m_EditBorderT > 0.0f)
					{
						float ty = imgRect.Y + m_EditBorderT * scale;
						Wui::LineSegment(ctx, { imgRect.X, ty }, { imgRect.X + imgRect.W, ty }, guideCol, 1.0f);
					}
					if (m_EditBorderB > 0.0f)
					{
						float by = imgRect.Y + (sliceH - m_EditBorderB) * scale;
						Wui::LineSegment(ctx, { imgRect.X, by }, { imgRect.X + imgRect.W, by }, guideCol, 1.0f);
					}
				}

				// Pivot Crosshair (cyan cross)
				const float pvScreenX = imgRect.X + m_EditPivotX * imgRect.W;
				const float pvScreenY = imgRect.Y + (1.0f - m_EditPivotY) * imgRect.H;
				const Wui::WuiColor pvCol { 0.15f, 0.95f, 0.95f, 1.0f };
				Wui::LineSegment(ctx, { pvScreenX - 8.0f, pvScreenY }, { pvScreenX + 8.0f, pvScreenY }, pvCol, 1.5f);
				Wui::LineSegment(ctx, { pvScreenX, pvScreenY - 8.0f }, { pvScreenX, pvScreenY + 8.0f }, pvCol, 1.5f);

				// Bottom Tag
				std::ostringstream tagStr;
				tagStr << static_cast<int>(sliceW) << " x " << static_cast<int>(sliceH) << " px ("
					   << static_cast<int>(scale) << "x 缩放) · 锚点: (" << std::fixed << std::setprecision(2) << m_EditPivotX
					   << ", " << m_EditPivotY << ")";
				if (m_EditBorderL > 0.0f || m_EditBorderR > 0.0f || m_EditBorderT > 0.0f || m_EditBorderB > 0.0f)
				{
					tagStr << " · 九宫格: [" << static_cast<int>(m_EditBorderL) << ", " << static_cast<int>(m_EditBorderR)
						   << ", " << static_cast<int>(m_EditBorderT) << ", " << static_cast<int>(m_EditBorderB) << "]";
				}
				Wui::Label(ctx, { innerCanvas.X + 8.0f, innerCanvas.Y + innerCanvas.H - 16.0f }, tagStr.str(), theme.TextMuted, 10.0f);
			}
		}

		// Status Feedback Bar
		const Wui::WuiRect statusRect { rightCol.X + 12.0f, rightCol.Y + rightCol.H - 22.0f, rightCol.W - 24.0f, 18.0f };
		Wui::Label(ctx, { statusRect.X, statusRect.Y + 1.0f }, m_StatusFeedback, theme.TextMuted, 10.5f);
	}
}

