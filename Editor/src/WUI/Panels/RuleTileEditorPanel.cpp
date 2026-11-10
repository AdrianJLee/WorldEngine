#include "wldpch.h"
#include "WUI/Panels/RuleTileEditorPanel.h"
#include "World/WUI/WuiWidgets.h"
#include "World/WUI/Widgets/WuiChrome.h"
#include "World/Utils/Paths.h"
#include "World/Renderer/Texture/TextureLibrary.h"
#include "World/WUI/WuiTextureRegistry.h"
#include "WUI/Common/EditorPanel.h"
#include <algorithm>
#include <filesystem>
#include <fstream>

namespace World
{
	namespace
	{
		// 8 邻域相对偏移与索引映射:
		// [0:NW(0,0), 1:N(1,0), 2:NE(2,0), 3:W(0,1), 4:E(2,1), 5:SW(0,2), 6:S(1,2), 7:SE(2,2)]
		int NeighborIndexFromGrid(int col, int row)
		{
			if (row == 0)
			{
				if (col == 0) return 0; // NW
				if (col == 1) return 1; // N
				if (col == 2) return 2; // NE
			}
			else if (row == 1)
			{
				if (col == 0) return 3; // W
				if (col == 2) return 4; // E
			}
			else if (row == 2)
			{
				if (col == 0) return 5; // SW
				if (col == 1) return 6; // S
				if (col == 2) return 7; // SE
			}
			return -1; // (1, 1) 是中心输出格
		}
	}

	RuleTileEditorPanel::RuleTileEditorPanel(std::string logicalPath)
		: m_LogicalPath(std::move(logicalPath))
	{
		std::replace(m_LogicalPath.begin(), m_LogicalPath.end(), '\\', '/');
		m_PanelId = "ruletile:" + m_LogicalPath;
		m_PanelTitle = "规则瓦片: " + std::filesystem::path(m_LogicalPath).filename().string();
		InitSandbox();
		Reload();
	}

	void RuleTileEditorPanel::InitSandbox()
	{
		std::fill(std::begin(m_Sandbox), std::end(m_Sandbox), 0);
		// 默认在中间放置一个十字形状，方便打开即见自适应效果
		m_Sandbox[2 * 8 + 3] = 1; m_Sandbox[2 * 8 + 4] = 1;
		m_Sandbox[3 * 8 + 2] = 1; m_Sandbox[3 * 8 + 3] = 1; m_Sandbox[3 * 8 + 4] = 1; m_Sandbox[3 * 8 + 5] = 1;
		m_Sandbox[4 * 8 + 2] = 1; m_Sandbox[4 * 8 + 3] = 1; m_Sandbox[4 * 8 + 4] = 1; m_Sandbox[4 * 8 + 5] = 1;
		m_Sandbox[5 * 8 + 3] = 1; m_Sandbox[5 * 8 + 4] = 1;
	}

	std::filesystem::path RuleTileEditorPanel::ResolvePhysicalPath() const
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

	void RuleTileEditorPanel::EnsureAtlasLoaded()
	{
		if (!m_RuleTile || m_RuleTile->GetAtlasPath().empty())
			return;

		std::string atlasPath = m_RuleTile->GetAtlasPath();
		if (atlasPath.rfind("assets/", 0) == 0 || atlasPath.rfind("assets\\", 0) == 0)
			atlasPath = atlasPath.substr(7);

		if (atlasPath != m_LoadedAtlasPath || !m_Atlas)
		{
			std::filesystem::path p(atlasPath);
			std::filesystem::path phys;
			if (std::filesystem::exists(p)) phys = p;
			else if (std::filesystem::exists(Paths::AssetRoot() / p)) phys = Paths::AssetRoot() / p;
			else if (std::filesystem::exists(Paths::ProjectDir() / p)) phys = Paths::ProjectDir() / p;

			m_Atlas = TextureAtlas::LoadFromFile(phys.string());
			m_LoadedAtlasPath = atlasPath;
			m_AtlasTextureId = 0;

			if (m_Atlas && !m_Atlas->GetTexturePath().empty())
			{
				std::string texPath = m_Atlas->GetTexturePath();
				if (texPath.rfind("assets/", 0) == 0 || texPath.rfind("assets\\", 0) == 0)
					texPath = texPath.substr(7);

				PathId pid = StringPool::Get().InternPath(texPath);
				auto gpu = TextureLibrary::Get().Get(pid, true);
				if (gpu)
				{
					m_AtlasTextureId = Wui::WuiTextureRegistry::Get().Register(gpu);
				}
			}
		}
	}

	bool RuleTileEditorPanel::Reload(std::string* message)
	{
		const auto phys = ResolvePhysicalPath();
		m_RuleTile = RuleTile::LoadFromFile(phys.string());
		if (!m_RuleTile)
		{
			m_RuleTile = std::make_shared<RuleTile>("CustomRuleTile", "assets/textures/characters/dungeon_atlas.watlas", 1);
			m_RuleTile->AddRule(RuleTileRule {});
			m_StatusFeedback = "无法读取规则文件，已创建默认规则模板: " + m_LogicalPath;
			if (message) *message = m_StatusFeedback;
			return false;
		}

		EnsureAtlasLoaded();
		m_Dirty = false;
		m_StatusFeedback = "就绪: 已载入规则瓦片 " + m_RuleTile->GetName() + " (含 " + std::to_string(m_RuleTile->GetRules().size()) + " 条规则)";
		if (message) *message = m_StatusFeedback;
		return true;
	}

	bool RuleTileEditorPanel::Save(std::string* message)
	{
		if (!m_RuleTile)
		{
			m_StatusFeedback = "无可用规则瓦片数据保存";
			if (message) *message = m_StatusFeedback;
			return false;
		}

		const auto phys = ResolvePhysicalPath();
		if (!m_RuleTile->SaveToFile(phys.string()))
		{
			m_StatusFeedback = "规则瓦片保存失败: " + phys.string();
			if (message) *message = m_StatusFeedback;
			return false;
		}

		m_Dirty = false;
		m_StatusFeedback = "规则瓦片已保存: " + m_LogicalPath;
		if (message) *message = m_StatusFeedback;
		return true;
	}

	void RuleTileEditorPanel::OnRender(Wui::WuiContext& ctx, const Wui::WuiRect& rect, PanelHost& host)
	{
		const Wui::WuiTheme& theme = host.Theme();
		Wui::WuiId baseId = Wui::HashId(m_PanelId.c_str());

		if (!m_RuleTile)
		{
			Reload();
			if (!m_RuleTile)
			{
				Wui::Label(ctx, { rect.X + 16.0f, rect.Y + 16.0f }, "无法载入规则瓦片资产", theme.Danger, 14.0f);
				return;
			}
		}

		EnsureAtlasLoaded();

		// 1. 顶栏操作区 (Toolbar: 高度 36px)
		const float barH = 36.0f;
		const Wui::WuiRect barRect { rect.X, rect.Y, rect.W, barH };
		Wui::PanelBackground(ctx, barRect, theme.PanelHeader, 0.0f);
		Wui::LineSegment(ctx, { barRect.X, barRect.Y + barH }, { barRect.X + barRect.W, barRect.Y + barH }, theme.Border, 1.0f);

		float curX = rect.X + 12.0f;
		const float btnY = rect.Y + 6.0f;
		const float btnH = 24.0f;

		// 标题
		Wui::Label(ctx, { curX, btnY + 4.0f }, "规则瓦片工作台", theme.Text, theme.FontSizeBody);
		curX += 110.0f;

		// 关联图集
		std::string atlasName = m_RuleTile->GetAtlasPath();
		if (atlasName.empty()) atlasName = "(未绑定图集)";
		else atlasName = std::filesystem::path(atlasName).filename().string();

		Wui::Label(ctx, { curX, btnY + 4.0f }, "图集: " + atlasName, theme.TextMuted, 12.0f);
		curX += ctx.MeasureTextWidth("图集: " + atlasName, 12.0f) + 8.0f;

		// 换绑图集按钮
		const Wui::WuiRect btnPickAtlas { curX, btnY, 26.0f, btnH };
		Wui::Tooltip(ctx, btnPickAtlas, "选择绑定的 .watlas 精灵图集");
		if (Wui::Button(ctx, baseId++, btnPickAtlas, "...", theme))
		{
			host.OpenAssetPicker("Select Sprite Atlas", AssetCategory::Texture, [this](const std::string& path) {
				if (m_RuleTile)
				{
					m_RuleTile->SetAtlasPath(path);
					EnsureAtlasLoaded();
					m_Dirty = true;
				}
			});
		}
		curX += 34.0f;

		// 默认回退瓦片
		std::string defLabel = "默认切片: #" + std::to_string(m_RuleTile->GetDefaultTileId());
		Wui::Label(ctx, { curX, btnY + 4.0f }, defLabel, theme.TextMuted, 12.0f);
		curX += ctx.MeasureTextWidth(defLabel, 12.0f) + 6.0f;

		const Wui::WuiRect btnSetDef { curX, btnY, 78.0f, btnH };
		Wui::Tooltip(ctx, btnSetDef, "将当前右侧选中的切片设为默认切片");
		if (Wui::Button(ctx, baseId++, btnSetDef, "设为当前", theme))
		{
			m_RuleTile->SetDefaultTileId(static_cast<uint16_t>(m_SelectedTileId));
			m_Dirty = true;
		}
		curX += 86.0f;

		// 生成标准 47 规则模板按钮
		const Wui::WuiRect btnTmpl { curX, btnY, 150.0f, btnH };
		Wui::Tooltip(ctx, btnTmpl, "一键自动填充 47 瓦片全套拓扑连接规则 (以当前选定切片为基准)");
		if (Wui::Button(ctx, baseId++, btnTmpl, "⚡ 生成 47 瓦片标准规则", theme))
		{
			*m_RuleTile = RuleTile::CreateStandard47Template(m_RuleTile->GetName(), m_RuleTile->GetAtlasPath(), static_cast<uint16_t>(m_SelectedTileId));
			m_Dirty = true;
			m_StatusFeedback = "已生成标准 47 瓦片自动拼接规则模板";
		}
		curX += 158.0f;

		// 添加新规则
		const Wui::WuiRect btnAddRule { curX, btnY, 82.0f, btnH };
		if (Wui::Button(ctx, baseId++, btnAddRule, "+ 添加规则", theme))
		{
			RuleTileRule newRule {};
			newRule.OutputTileId = static_cast<uint16_t>(m_SelectedTileId);
			m_RuleTile->AddRule(newRule);
			m_Dirty = true;
		}
		curX += 90.0f;

		// 保存按钮
		const Wui::WuiRect btnSave { rect.X + rect.W - 84.0f, btnY, 72.0f, btnH };
		auto saveTheme = theme;
		if (m_Dirty) { saveTheme.ButtonBg = theme.ActiveBg; saveTheme.Border = theme.Accent; }
		if (Wui::Button(ctx, baseId++, btnSave, "💾 保存", saveTheme))
		{
			Save();
		}

		// 2. 主区域两栏排布 (左栏: 规则卡片列表 58% | 右栏: 切片盘 + 8x8 测试沙盒 42%)
		const float contentY = barRect.Y + barH + 4.0f;
		const float contentH = rect.H - barH - 28.0f;
		const float leftW = std::max(360.0f, (rect.W - 16.0f) * 0.58f);
		const float rightW = rect.W - leftW - 16.0f;

		const Wui::WuiRect leftCol { rect.X + 8.0f, contentY, leftW, contentH };
		const Wui::WuiRect rightCol { leftCol.X + leftCol.W + 8.0f, contentY, rightW, contentH };

		// --- 左栏: 规则卡片列表 ---
		auto& rules = m_RuleTile->GetRules();
		const float cardH = 96.0f;
		const float totalRulesH = static_cast<float>(rules.size()) * (cardH + 8.0f) + 16.0f;

		Wui::Label(ctx, { leftCol.X + 4.0f, leftCol.Y + 2.0f },
			"规则列表 (共 " + std::to_string(rules.size()) + " 条，由上至下优先匹配):", theme.TextMuted, 12.0f);

		const Wui::WuiRect rulesScrollRect { leftCol.X, leftCol.Y + 22.0f, leftCol.W, leftCol.H - 22.0f };
		Wui::BeginScrollArea(ctx, rulesScrollRect, totalRulesH, m_RulesScroll, theme);

		float cardY = rulesScrollRect.Y + 6.0f - m_RulesScroll;
		int ruleToDelete = -1;
		int ruleToMoveUp = -1;
		int ruleToMoveDown = -1;

		for (size_t rIdx = 0; rIdx < rules.size(); ++rIdx)
		{
			if (cardY + cardH < rulesScrollRect.Y || cardY > rulesScrollRect.Y + rulesScrollRect.H)
			{
				cardY += cardH + 8.0f;
				continue;
			}

			auto& rule = rules[rIdx];
			const Wui::WuiRect card { leftCol.X + 4.0f, cardY, leftCol.W - 12.0f, cardH };
			const bool isCardSelected = (static_cast<int>(rIdx) == m_SelectedRuleIndex);

			Wui::PanelBackground(ctx, card, isCardSelected ? theme.ActiveBg : theme.ButtonBg, theme.Radius);
			Wui::HighlightOutline(ctx, card, isCardSelected ? theme.Accent : theme.Border, theme.Radius, isCardSelected ? 2.0f : 1.0f);

			// 卡片内部: 3x3 邻域矩阵 (尺寸 84x84, 格子 26x26)
			const float mX = card.X + 8.0f;
			const float mY = card.Y + 6.0f;
			const float cellSize = 26.0f;
			const float cellGap = 2.0f;

			for (int row = 0; row < 3; ++row)
			{
				for (int col = 0; col < 3; ++col)
				{
					const Wui::WuiRect cRect { mX + col * (cellSize + cellGap), mY + row * (cellSize + cellGap), cellSize, cellSize };
					int nIdx = NeighborIndexFromGrid(col, row);

					if (nIdx < 0)
					{
						// (1, 1) 中心格: 输出瓦片缩略图
						Wui::PanelBackground(ctx, cRect, { 0.1f, 0.12f, 0.16f, 1.0f }, 2.0f);
						Wui::HighlightOutline(ctx, cRect, theme.Accent, 2.0f, 1.5f);

						if (m_AtlasTextureId != 0 && m_Atlas && rule.OutputTileId >= 1 && rule.OutputTileId <= m_Atlas->GetSubTextureCount()
							&& m_Atlas->GetWidth() > 0 && m_Atlas->GetHeight() > 0)
						{
							if (const SubTexture2D* sub = m_Atlas->GetSubTexture(static_cast<size_t>(rule.OutputTileId - 1)))
							{
								const float invW = 1.0f / static_cast<float>(m_Atlas->GetWidth());
								const float invH = 1.0f / static_cast<float>(m_Atlas->GetHeight());
								Wui::Image(ctx, cRect, m_AtlasTextureId, {
									sub->PixelRect.x * invW, sub->PixelRect.y * invH,
									sub->PixelRect.z * invW, sub->PixelRect.w * invH
								}, theme);
							}
						}
						else
						{
							Wui::Label(ctx, { cRect.X + 4.0f, cRect.Y + 6.0f }, "#" + std::to_string(rule.OutputTileId), theme.Text, 10.0f);
						}
					}
					else
					{
						// 8 邻域方向方格
						uint8_t state = rule.Neighbors[nIdx];
						Wui::WuiColor fillCol { 0.14f, 0.16f, 0.20f, 1.0f };
						Wui::WuiColor textCol = theme.TextMuted;
						const char* glyph = "·";

						if (state == static_cast<uint8_t>(RuleNeighbor::This))
						{
							fillCol = { 0.18f, 0.65f, 0.28f, 1.0f }; // 绿色: 必须相邻
							textCol = { 1.0f, 1.0f, 1.0f, 1.0f };
							glyph = "✓";
						}
						else if (state == static_cast<uint8_t>(RuleNeighbor::NotThis))
						{
							fillCol = { 0.78f, 0.22f, 0.22f, 1.0f }; // 红色: 必须无相邻
							textCol = { 1.0f, 1.0f, 1.0f, 1.0f };
							glyph = "✕";
						}

						Wui::PanelBackground(ctx, cRect, fillCol, 2.0f);
						Wui::LineSegment(ctx, { cRect.X, cRect.Y }, { cRect.X + cRect.W, cRect.Y }, theme.Border, 0.5f);
						Wui::Label(ctx, { cRect.X + 8.0f, cRect.Y + 5.0f }, glyph, textCol, 12.0f);

						// 点击循环切换三态: Ignore(0) -> This(1) -> NotThis(2) -> Ignore(0)
						if (ctx.IsHovered(cRect) && ctx.Input().MouseDown[0])
						{
							rule.Neighbors[nIdx] = (state + 1) % 3;
							m_SelectedRuleIndex = static_cast<int>(rIdx);
							m_Dirty = true;
						}
					}
				}
			}

			// 卡片右半部: 规则信息与操作按钮
			const float infoX = mX + 3.0f * (cellSize + cellGap) + 14.0f;
			Wui::Label(ctx, { infoX, card.Y + 8.0f }, "规则 #" + std::to_string(rIdx + 1) + ": 输出切片 #" + std::to_string(rule.OutputTileId), theme.Text, 12.0f);

			// 改绑为选定切片按钮
			const Wui::WuiRect btnBindTile { infoX, card.Y + 30.0f, 110.0f, 22.0f };
			if (Wui::Button(ctx, baseId++, btnBindTile, "赋当前选中切片", theme))
			{
				rule.OutputTileId = static_cast<uint16_t>(m_SelectedTileId);
				m_SelectedRuleIndex = static_cast<int>(rIdx);
				m_Dirty = true;
			}

			// 排序与删除按钮
			const float actBtnW = 26.0f;
			const float actY = card.Y + 58.0f;
			const Wui::WuiRect btnUp   { infoX, actY, actBtnW, 22.0f };
			const Wui::WuiRect btnDown { infoX + 30.0f, actY, actBtnW, 22.0f };
			const Wui::WuiRect btnDel  { infoX + 66.0f, actY, 44.0f, 22.0f };

			if (Wui::Button(ctx, baseId++, btnUp, "↑", theme))
			{
				ruleToMoveUp = static_cast<int>(rIdx);
			}
			if (Wui::Button(ctx, baseId++, btnDown, "↓", theme))
			{
				ruleToMoveDown = static_cast<int>(rIdx);
			}
			if (Wui::Button(ctx, baseId++, btnDel, "删除", theme))
			{
				ruleToDelete = static_cast<int>(rIdx);
			}

			// 点击卡片设为活动选中规则
			if (ctx.IsHovered(card) && ctx.Input().MouseDown[0])
			{
				m_SelectedRuleIndex = static_cast<int>(rIdx);
			}

			cardY += cardH + 8.0f;
		}

		Wui::EndScrollArea(ctx);

		// 处理待移动与删除规则
		if (ruleToDelete >= 0 && ruleToDelete < static_cast<int>(rules.size()))
		{
			rules.erase(rules.begin() + ruleToDelete);
			m_Dirty = true;
		}
		else if (ruleToMoveUp > 0 && ruleToMoveUp < static_cast<int>(rules.size()))
		{
			std::swap(rules[ruleToMoveUp], rules[ruleToMoveUp - 1]);
			m_SelectedRuleIndex = ruleToMoveUp - 1;
			m_Dirty = true;
		}
		else if (ruleToMoveDown >= 0 && ruleToMoveDown + 1 < static_cast<int>(rules.size()))
		{
			std::swap(rules[ruleToMoveDown], rules[ruleToMoveDown + 1]);
			m_SelectedRuleIndex = ruleToMoveDown + 1;
			m_Dirty = true;
		}

		// --- 右栏: 切片盘 (上半部) + 8x8 测试沙盒 (下半部) ---
		const float splitH = rightCol.H * 0.52f;
		const Wui::WuiRect paletteRect { rightCol.X, rightCol.Y, rightCol.W, splitH };
		const Wui::WuiRect sandboxRect { rightCol.X, rightCol.Y + splitH + 8.0f, rightCol.W, rightCol.H - splitH - 8.0f };

		// 1) 切片盘
		Wui::Label(ctx, { paletteRect.X + 4.0f, paletteRect.Y + 2.0f },
			"图集切片盘 (当前选中 #" + std::to_string(m_SelectedTileId) + "):", theme.TextMuted, 12.0f);

		const size_t tileCount = m_Atlas ? m_Atlas->GetSubTextureCount() : 0;
		const float pGridPad = 4.0f;
		const float pTileSize = 34.0f;
		const int pCols = std::max(1, static_cast<int>((paletteRect.W - 12.0f) / (pTileSize + pGridPad)));
		const size_t pRows = (tileCount + pCols - 1) / pCols;
		const float totalPaletteH = static_cast<float>(pRows) * (pTileSize + pGridPad) + 12.0f;

		const Wui::WuiRect palScrollRect { paletteRect.X, paletteRect.Y + 22.0f, paletteRect.W, paletteRect.H - 22.0f };
		Wui::BeginScrollArea(ctx, palScrollRect, totalPaletteH, m_PaletteScroll, theme);

		float palY = palScrollRect.Y + 4.0f - m_PaletteScroll;
		for (size_t r = 0; r < pRows; ++r)
		{
			for (int c = 0; c < pCols; ++c)
			{
				size_t tIdx = r * pCols + c;
				if (tIdx >= tileCount) break;

				const int tId = static_cast<int>(tIdx + 1);
				const Wui::WuiRect tRect { palScrollRect.X + 4.0f + c * (pTileSize + pGridPad), palY, pTileSize, pTileSize };
				const bool isTileSel = (m_SelectedTileId == tId);

				Wui::PanelBackground(ctx, tRect, isTileSel ? theme.Selection : theme.ButtonBg, 2.0f);
				Wui::HighlightOutline(ctx, tRect, isTileSel ? theme.Accent : theme.Border, 2.0f, isTileSel ? 2.0f : 1.0f);

				if (m_AtlasTextureId != 0 && m_Atlas)
				{
					if (const SubTexture2D* sub = m_Atlas->GetSubTexture(tIdx))
					{
						const float invW = 1.0f / static_cast<float>(m_Atlas->GetWidth());
						const float invH = 1.0f / static_cast<float>(m_Atlas->GetHeight());
						Wui::Image(ctx, tRect, m_AtlasTextureId, {
							sub->PixelRect.x * invW, sub->PixelRect.y * invH,
							sub->PixelRect.z * invW, sub->PixelRect.w * invH
						}, theme);
					}
				}

				if (ctx.IsHovered(tRect) && ctx.Input().MouseDown[0])
				{
					m_SelectedTileId = tId;
				}

				Wui::Tooltip(ctx, tRect, "切片 #" + std::to_string(tId));
			}
			palY += pTileSize + pGridPad;
		}

		Wui::EndScrollArea(ctx);

		// 2) 8x8 测试沙盒 (实时交互验证规则自动拼接)
		Wui::LineSegment(ctx, { sandboxRect.X, sandboxRect.Y - 4.0f }, { sandboxRect.X + sandboxRect.W, sandboxRect.Y - 4.0f }, theme.Border, 1.0f);
		Wui::Label(ctx, { sandboxRect.X + 4.0f, sandboxRect.Y + 2.0f }, "自动拼接实时测试沙盒 (左键涂抹 / 右键清除):", theme.TextMuted, 12.0f);

		const Wui::WuiRect btnClearSand { sandboxRect.X + sandboxRect.W - 80.0f, sandboxRect.Y, 74.0f, 20.0f };
		if (Wui::Button(ctx, baseId++, btnClearSand, "重置沙盒", theme))
		{
			InitSandbox();
		}

		// 构建临时的 8x8 内存 Tilemap 供实时解析规则
		Tilemap sandMap("Sandbox", 8, 8, m_RuleTile->GetAtlasPath());
		for (int sy = 0; sy < 8; ++sy)
			for (int sx = 0; sx < 8; ++sx)
				sandMap.SetTile(sx, sy, m_Sandbox[sy * 8 + sx]);

		const float sCellSize = std::min(26.0f, (sandboxRect.H - 30.0f) / 8.0f);
		const float sStartX = sandboxRect.X + (sandboxRect.W - 8.0f * sCellSize) * 0.5f;
		const float sStartY = sandboxRect.Y + 26.0f;

		for (int sy = 0; sy < 8; ++sy)
		{
			for (int sx = 0; sx < 8; ++sx)
			{
				const Wui::WuiRect scRect { sStartX + sx * sCellSize, sStartY + sy * sCellSize, sCellSize, sCellSize };
				bool isSolid = m_Sandbox[sy * 8 + sx] != 0;

				// 交互修改沙盒
				if (ctx.IsHovered(scRect))
				{
					if (ctx.Input().MouseDown[0]) m_Sandbox[sy * 8 + sx] = 1;
					if (ctx.Input().MouseDown[1]) m_Sandbox[sy * 8 + sx] = 0;
				}

				if (isSolid)
				{
					uint16_t resolvedId = m_RuleTile->ResolveTile(sandMap, sx, sy);
					Wui::PanelBackground(ctx, scRect, { 0.1f, 0.12f, 0.16f, 1.0f }, 1.0f);

					if (m_AtlasTextureId != 0 && m_Atlas && resolvedId >= 1 && resolvedId <= m_Atlas->GetSubTextureCount()
						&& m_Atlas->GetWidth() > 0 && m_Atlas->GetHeight() > 0)
					{
						if (const SubTexture2D* sub = m_Atlas->GetSubTexture(static_cast<size_t>(resolvedId - 1)))
						{
							const float invW = 1.0f / static_cast<float>(m_Atlas->GetWidth());
							const float invH = 1.0f / static_cast<float>(m_Atlas->GetHeight());
							Wui::Image(ctx, scRect, m_AtlasTextureId, {
								sub->PixelRect.x * invW, sub->PixelRect.y * invH,
								sub->PixelRect.z * invW, sub->PixelRect.w * invH
							}, theme);
						}
					}
					else
					{
						Wui::PanelBackground(ctx, scRect, theme.Accent, 1.0f);
					}
				}
				else
				{
					Wui::PanelBackground(ctx, scRect, { 0.08f, 0.09f, 0.12f, 1.0f }, 0.0f);
				}

				Wui::LineSegment(ctx, { scRect.X, scRect.Y }, { scRect.X + scRect.W, scRect.Y }, theme.Border, 0.5f);
				Wui::LineSegment(ctx, { scRect.X, scRect.Y }, { scRect.X, scRect.Y + scRect.H }, theme.Border, 0.5f);
			}
		}

		// 底部状态栏
		const float statY = rect.Y + rect.H - 22.0f;
		Wui::LineSegment(ctx, { rect.X + 8.0f, statY }, { rect.X + rect.W - 8.0f, statY }, theme.Border, 1.0f);
		Wui::Label(ctx, { rect.X + 12.0f, statY + 4.0f }, m_StatusFeedback, theme.TextMuted, 11.5f);
	}
}
