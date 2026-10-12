#include "wldpch.h"
#include "WUI/Panels/SpriteAtlasEditorPanel.h"
#include "World/Core/Log.h"
#include "World/Utils/Paths.h"
#include "World/WUI/WuiWidgets.h"
#include "World/WUI/Widgets/WuiChrome.h"

#include <filesystem>
#include <algorithm>
#include <sstream>

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
			m_Atlas = std::make_shared<TextureAtlas>("", 256, 256);

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
		const std::string delName = list[index].Name;
		list.erase(list.begin() + static_cast<std::ptrdiff_t>(index));

		auto rebuilt = std::make_shared<TextureAtlas>(m_Atlas->GetTexturePath(), m_Atlas->GetWidth(), m_Atlas->GetHeight());
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

	void SpriteAtlasEditorPanel::OnRender(Wui::WuiContext& ctx, const Wui::WuiRect& rect, PanelHost& host)
	{
		const Wui::WuiTheme& theme = host.Theme();
		Wui::PanelBackground(ctx, rect, theme.PanelBg);

		float curY = rect.Y + 6.0f;

		// 1. Top Bar: Header, logical path, Save button
		const Wui::WuiRect topBar { rect.X + 8.0f, curY, rect.W - 16.0f, 32.0f };
		Wui::BarSurface(ctx, topBar, theme.PanelHeader, theme.Border);
		Wui::Label(ctx, { topBar.X + 10.0f, topBar.Y + 7.0f }, "图集编辑器 (Sprite Atlas Editor)", theme.Accent, 13.5f);

		std::string pathText = "资产: " + m_LogicalPath;
		Wui::Label(ctx, { topBar.X + 260.0f, topBar.Y + 8.0f }, pathText, theme.TextMuted, 11.0f);

		const Wui::WuiRect btnSave { topBar.X + topBar.W - 110.0f, topBar.Y + 4.0f, 102.0f, 24.0f };
		Wui::Tooltip(ctx, btnSave, "保存图集配置到 .watlas 文件并刷新缓存 (atlas.btn.save)");
		if (Wui::Button(ctx, Wui::HashId("atlas.btn.save"), btnSave, m_Dirty ? "💾 保存图集 *" : "💾 保存图集", theme))
		{
			Save();
		}

		curY += 38.0f;
		const float contentH = rect.H - (curY - rect.Y) - 8.0f;
		const float leftW = std::min(300.0f, rect.W * 0.35f);
		const float rightX = rect.X + 8.0f + leftW + 8.0f;
		const float rightW = rect.W - 16.0f - leftW - 8.0f;

		// 2. Left Column: Summary, Toolbar, Slices List
		const Wui::WuiRect leftCol { rect.X + 8.0f, curY, leftW, contentH };
		Wui::PanelBackground(ctx, leftCol, theme.ContentBg, 4.0f);

		Wui::Label(ctx, { leftCol.X + 10.0f, leftCol.Y + 8.0f }, "图集概览", theme.Text, 12.5f);
		std::string texPath = m_Atlas ? m_Atlas->GetTexturePath() : "无";
		Wui::Label(ctx, { leftCol.X + 10.0f, leftCol.Y + 28.0f }, "源纹理: " + texPath, theme.TextMuted, 11.0f);

		uint32_t w = m_Atlas ? m_Atlas->GetWidth() : 0;
		uint32_t h = m_Atlas ? m_Atlas->GetHeight() : 0;
		size_t count = m_Atlas ? m_Atlas->GetSubTextureCount() : 0;
		std::string dimText = "尺寸: " + std::to_string(w) + "x" + std::to_string(h) + " · 切片数: " + std::to_string(count);
		Wui::Label(ctx, { leftCol.X + 10.0f, leftCol.Y + 46.0f }, dimText, theme.TextMuted, 11.0f);

		// Toolbar: Add / Delete
		float btnY = leftCol.Y + 68.0f;
		const float btnHalfW = (leftCol.W - 24.0f) * 0.5f;
		const Wui::WuiRect btnAdd { leftCol.X + 8.0f, btnY, btnHalfW, 24.0f };
		Wui::Tooltip(ctx, btnAdd, "添加新的子精灵切片 (atlas.btn.add)");
		if (Wui::Button(ctx, Wui::HashId("atlas.btn.add"), btnAdd, "＋ 添加子精灵", theme))
		{
			std::string newName = "sprite_" + std::to_string(count);
			AddSprite(newName, { 0.0f, 0.0f, 32.0f, 32.0f });
		}

		const Wui::WuiRect btnDel { leftCol.X + 16.0f + btnHalfW, btnY, btnHalfW, 24.0f };
		Wui::Tooltip(ctx, btnDel, "删除当前选中的子精灵切片 (atlas.btn.del)");
		if (Wui::Button(ctx, Wui::HashId("atlas.btn.del"), btnDel, "✖ 删除", theme))
		{
			if (m_SelectedIndex >= 0)
				DeleteSprite(static_cast<size_t>(m_SelectedIndex));
		}

		// Slices List
		float listY = btnY + 30.0f;
		float listH = leftCol.H - (listY - leftCol.Y) - 8.0f;
		const Wui::WuiRect listRect { leftCol.X + 6.0f, listY, leftCol.W - 12.0f, listH };
		Wui::PanelBackground(ctx, listRect, theme.PanelBg, 2.0f);

		if (m_Atlas)
		{
			const auto& allSubs = m_Atlas->GetAllSubTextures();
			float itemY = listRect.Y + 4.0f;
			for (size_t i = 0; i < allSubs.size(); ++i)
			{
				if (itemY + 24.0f > listRect.Y + listRect.H)
					break; // Bound within display list

				const Wui::WuiRect itemRect { listRect.X + 4.0f, itemY, listRect.W - 8.0f, 24.0f };
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
				itemY += 28.0f;
			}
		}

		// 3. Right Column: Inspector
		const Wui::WuiRect rightCol { rightX, curY, rightW, contentH };
		Wui::PanelBackground(ctx, rightCol, theme.ContentBg, 4.0f);

		Wui::Label(ctx, { rightCol.X + 12.0f, rightCol.Y + 10.0f }, "子精灵检视 (Sprite Inspector)", theme.Text, 13.0f);

		float inspY = rightCol.Y + 36.0f;
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
			inspY += 32.0f;

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

			inspY += 32.0f;

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

			inspY += 28.0f;

			// Fast Preset Buttons
			float preX = rightCol.X + 120.0f;
			const float preW = 60.0f;
			const Wui::WuiRect btnP0 { preX, inspY, preW, 22.0f };
			Wui::Tooltip(ctx, btnP0, "快捷设为几何中心 (0.5, 0.5)");
			if (Wui::Button(ctx, Wui::HashId("atlas.btn.pivot_center"), btnP0, "中心", theme))
			{
				m_EditPivotX = 0.5f; m_EditPivotY = 0.5f; fieldsChanged = true;
			}
			preX += preW + 4.0f;

			const Wui::WuiRect btnP1 { preX, inspY, preW, 22.0f };
			Wui::Tooltip(ctx, btnP1, "快捷设为脚底中心 (0.5, 0.0)");
			if (Wui::Button(ctx, Wui::HashId("atlas.btn.pivot_bottom"), btnP1, "脚底", theme))
			{
				m_EditPivotX = 0.5f; m_EditPivotY = 0.0f; fieldsChanged = true;
			}
			preX += preW + 4.0f;

			const Wui::WuiRect btnP2 { preX, inspY, preW, 22.0f };
			Wui::Tooltip(ctx, btnP2, "快捷设为左上角 (0.0, 1.0)");
			if (Wui::Button(ctx, Wui::HashId("atlas.btn.pivot_topleft"), btnP2, "左上", theme))
			{
				m_EditPivotX = 0.0f; m_EditPivotY = 1.0f; fieldsChanged = true;
			}
			preX += preW + 4.0f;

			const Wui::WuiRect btnP3 { preX, inspY, preW, 22.0f };
			Wui::Tooltip(ctx, btnP3, "快捷设为右上角 (1.0, 1.0)");
			if (Wui::Button(ctx, Wui::HashId("atlas.btn.pivot_topright"), btnP3, "右上", theme))
			{
				m_EditPivotX = 1.0f; m_EditPivotY = 1.0f; fieldsChanged = true;
			}
			preX += preW + 4.0f;

			const Wui::WuiRect btnP4 { preX, inspY, preW, 22.0f };
			Wui::Tooltip(ctx, btnP4, "快捷设为左下角 (0.0, 0.0)");
			if (Wui::Button(ctx, Wui::HashId("atlas.btn.pivot_bottomleft"), btnP4, "左下", theme))
			{
				m_EditPivotX = 0.0f; m_EditPivotY = 0.0f; fieldsChanged = true;
			}
			preX += preW + 4.0f;

			const Wui::WuiRect btnP5 { preX, inspY, preW, 22.0f };
			Wui::Tooltip(ctx, btnP5, "快捷设为右下角 (1.0, 0.0)");
			if (Wui::Button(ctx, Wui::HashId("atlas.btn.pivot_bottomright"), btnP5, "右下", theme))
			{
				m_EditPivotX = 1.0f; m_EditPivotY = 0.0f; fieldsChanged = true;
			}

			inspY += 34.0f;

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
		}
		else
		{
			Wui::Label(ctx, { rightCol.X + 24.0f, inspY + 20.0f }, "请在左侧切片列表中选择子精灵，或点击 [＋ 添加子精灵] 创建新切片。", theme.TextMuted, 12.0f);
		}

		// Status Feedback Bar
		const Wui::WuiRect statusRect { rightCol.X + 12.0f, rightCol.Y + rightCol.H - 26.0f, rightCol.W - 24.0f, 20.0f };
		Wui::Label(ctx, { statusRect.X, statusRect.Y + 3.0f }, m_StatusFeedback, theme.TextMuted, 11.0f);
	}
}



