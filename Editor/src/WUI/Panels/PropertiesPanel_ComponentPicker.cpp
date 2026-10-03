#include "PropertiesPanel_Internal.h"

namespace World
{

using namespace PropertiesPanelDetail;


	// ---- 组件选择器绘制 ----

void PropertiesPanel::DrawAddComponentPicker(Wui::WuiContext& ctx, Entity entity, Scene* scene, Schema::SchemaRegistry& schemas){
		const Wui::WuiTheme& theme = m_Host.Theme();
		const Wui::WuiId addModal = Wui::HashId("prop.add.modal");
		const Wui::WuiId searchId = Wui::HashId("prop.add.search");
		const Wui::WuiId listId = Wui::HashId("prop.add.list");
		// 打开弹层的那一帧不吃按键:按钮的键盘激活(Enter/Space)与选择器的回车是同一个事件。
		const bool justOpened = ctx.Frame() == m_AddOpenedFrame;

		// 候选 = 当前实体还没有的组件(与旧菜单同一口径:有 Storage 且未拥有);不写死任何清单。
		std::vector<const Schema::TypeSchema*> candidates;
		for (const Schema::TypeSchema* schema : schemas.List(Schema::TypeCategory::Component))
			if (schema && schema->Storage && !entity.HasComponent(schema->Storage->ComponentId))
				candidates.push_back(schema);

		// 行构造:过滤 + 分组(最近使用 → 按 Category 分层 → 未分类最后),同层按名称排序。
		const auto buildRows = [&](const std::string& filter)
		{
			const std::string needle = LowerAscii(filter);
			std::vector<const Schema::TypeSchema*> matched;
			for (const Schema::TypeSchema* schema : candidates)
				if (MatchesComponentFilter(*schema, needle))
					matched.push_back(schema);

			const auto makeItem = [](const Schema::TypeSchema& schema)
			{
				const Wui::LocalizedLabel label = SchemaComponentLabel(schema);
				PickerRow row;
				row.Text = label.Text;
				row.Term = label.Term;
				row.Doc = ComponentDocLabel(schema);
				// 分层分类路径 = TypeSchema::CategoryPath(方案 §8.2 里的 Category 字符串;
				// 本结构已有 TypeCategory Category 枚举,所以生成物里叫 CategoryPath)。
				row.Category = CategoryLabel(schema.CategoryPath);
				// 行 id 沿用既有脚本契约:prop.add.<DisplayName>(schema 生成物里 = 短类型名)。
				row.NodeId = "prop.add." + schema.DisplayName;
				row.Schema = &schema;
				return row;
			};
			const auto makeHeader = [](std::string text, std::string nodeId)
			{
				PickerRow row;
				row.Header = true;
				row.Text = std::move(text);
				row.NodeId = std::move(nodeId);
				return row;
			};
			const auto byName = [](const Schema::TypeSchema* left, const Schema::TypeSchema* right)
			{
				const std::string leftName = LowerAscii(SchemaComponentLabel(*left).Text);
				const std::string rightName = LowerAscii(SchemaComponentLabel(*right).Text);
				if (leftName != rightName)
					return leftName < rightName;
				return left->DisplayName < right->DisplayName;
			};
			// P4-U7(用户 2026-09-21:「左侧加个分栏标签」):分类侧栏的过滤 —— 空 = 全部;
			// 只保留该分类的候选,后面的分组/未分类逻辑照旧。
			std::vector<const Schema::TypeSchema*> visible;
			for (const Schema::TypeSchema* schema : matched)
				if (m_AddCategoryAll || schema->CategoryPath == m_AddCategoryFilter)
					visible.push_back(schema);
			matched = std::move(visible);

			std::vector<PickerRow> rows;
			std::vector<bool> used(matched.size(), false);

			// ① 最近使用(最多 5,按 MRU 顺序置顶;已被拥有/不匹配过滤的自动消失)。
			std::vector<PickerRow> recentRows;
			for (const std::string& recentName : m_RecentComponents)
			{
				if (recentRows.size() >= kPickerRecentMax)
					break;
				for (size_t i = 0; i < matched.size(); ++i)
				{
					if (used[i] || SchemaTypeKeyName(*matched[i]) != recentName)
						continue;
					used[i] = true;
					recentRows.push_back(makeItem(*matched[i]));
					break;
				}
			}
			if (!recentRows.empty())
			{
				rows.push_back(makeHeader(Wui::Tr("panel.properties.add.recent", "Recently Used"),
					"prop.add.group.recent"));
				rows.insert(rows.end(), recentRows.begin(), recentRows.end());
			}

			// ② 按 Category 分层(路径排序 → 同层按名称排序);③ 未分类排最后。
			std::map<std::string, std::vector<const Schema::TypeSchema*>> groups;
			std::vector<const Schema::TypeSchema*> uncategorized;
			for (size_t i = 0; i < matched.size(); ++i)
			{
				if (used[i])
					continue;
				if (matched[i]->CategoryPath.empty())
					uncategorized.push_back(matched[i]);
				else
					groups[matched[i]->CategoryPath].push_back(matched[i]);
			}
			for (auto& [category, items] : groups)
			{
				std::stable_sort(items.begin(), items.end(), byName);
				rows.push_back(makeHeader(CategoryLabel(category),
					"prop.add.group." + CategoryKeyName(category)));
				for (const Schema::TypeSchema* schema : items)
					rows.push_back(makeItem(*schema));
			}
			if (!uncategorized.empty())
			{
				std::stable_sort(uncategorized.begin(), uncategorized.end(), byName);
				rows.push_back(makeHeader(Wui::Tr("panel.properties.add.uncategorized", "Uncategorized"),
					"prop.add.group.uncategorized"));
				for (const Schema::TypeSchema* schema : uncategorized)
					rows.push_back(makeItem(*schema));
			}
			return rows;
		};
		const auto heightOf = [](const std::vector<PickerRow>& rows)
		{
			float height = 0.0f;
			for (const PickerRow& row : rows)
				height += row.Header ? kPickerGroupHeader : kPickerRow;
			return height;
		};

		// ---- 几何:居中模态窗口(用户 2026-09-21:「添加组件在右下角太难用了,为什么不弹出个居中窗口呢」)----
		// 用引擎既有的 WuiModal 组件:遮罩 + 居中 + 输入封锁 + Esc + 底部按钮条,与其它模态同一套交互。
		Wui::ModalFrameDesc frameDesc;
		frameDesc.Id = addModal;
		frameDesc.Title = Wui::Tr("panel.properties.add_component", "Add Component");
		frameDesc.Size = { 560.0f, 520.0f };
		Wui::WuiRect frame;
		bool escapePressed = false;
		if (!Wui::BeginModalFrame(ctx, frameDesc, &frame, &escapePressed, theme))
			return;
		// 列表占满模态正文区(固定高度 + 内部滚动):右侧滚动条因此有稳定的轨道,
		// 内容多少都不会让窗口/分栏跳来跳去(与 Unity 的 Add Component 同一形态)。
		const float listTop = frame.Y + 82.0f;
		const float listBottom = frame.Y + frame.H - Wui::ModalFooterHeight - 18.0f;
		const float listHeight = std::max(60.0f, listBottom - listTop);
		const Wui::WuiRect searchRect { frame.X + 16.0f, frame.Y + 48.0f, frame.W - 32.0f, 24.0f };
		// P4-U7:左侧分类栏(用户 2026-09-21「左侧加个分栏标签」)+ 右侧滚动条
		// (用户「滚轮下滑有问题…最好右侧加个滚轮进度」)。
		const float sidebarWidth = 168.0f;
		const Wui::WuiRect sidebarRect { frame.X + 16.0f, listTop, sidebarWidth, listHeight };
		const Wui::WuiRect listRect { sidebarRect.X + sidebarWidth + 10.0f, listTop,
			frame.W - 32.0f - sidebarWidth - 10.0f, listHeight };

		// ---- 搜索框:自动聚焦,输入即过滤(占位文案与 a11y Placeholder 是同一句)----
		Wui::TextFieldA11y a11y;
		a11y.Label = Wui::Tr("panel.properties.add_component", "Add Component");
		a11y.Placeholder = Wui::Tr("panel.properties.add.search_hint", "Search components…");
		// TextField 在回车/Esc 时会把焦点清 0(它的返回值是"输入结束"语义)→ 先记录本帧是否聚焦。
		const bool searchFocused = ctx.Focus() == searchId;
		bool searchCancelled = false;
		Wui::TextField(ctx, searchId, searchRect, m_AddSearch, theme, &searchCancelled, &a11y);
		if (m_AddSearch.empty())
			Wui::Label(ctx, { searchRect.X + 8.0f, searchRect.Y + 5.0f }, a11y.Placeholder,
				theme.TextDisabled, 12.0f);
		// 搜索词一变就把键盘高亮归零 → "输入后回车 = 添加第一个匹配项"。
		if (m_AddSearchLast != m_AddSearch)
		{
			m_AddSearchLast = m_AddSearch;
			m_AddHighlight = -1;
		}

		// ---- 左侧分类栏:全部 + 出现过的 CategoryPath(带计数),点击过滤 ----
		// 分栏标签 = 左列(fixed 168px),右列是候选列表;两者等高,都在模态正文区内。
		// 行数超出栏高时按视口外不绘制/不登记处理(分类数量少,正常不会触发)。
		{
			std::map<std::string, int> counts;
			int total = 0;
			for (const Schema::TypeSchema* schema : candidates)
			{
				if (!MatchesComponentFilter(*schema, LowerAscii(m_AddSearch)))
					continue;
				++counts[schema->CategoryPath];
				++total;
			}
			// 侧栏底色用列表/输入框的 ContentBg(比模态面板底更深一档,形成"分栏"层次)。
			Wui::PanelBackground(ctx, sidebarRect, theme.ContentBg, 4.0f);
			// W3.5:"只裁剪、不滚动"走库件 P1c-LIB1 的 Wui::ClipScope(RAII):
			// 渲染裁剪(ClipPush/ClipPop)与裁剪栈(PushClipRect/PopClipRect)同进同出 ——
			// 比原来只发渲染命令多维护 ClipAllows(焦点环/条目剔除)一处,作用域位置逐字对齐
			// (原来 ClipPop 是本块最后一条语句,现在 = 析构点)。
			Wui::ClipScope sidebarClip(ctx, sidebarRect);
			float itemY = sidebarRect.Y + 4.0f;
			// filter:"" + all=true → 全部;all=false 时 filter 为空 = 未分类,非空 = 该分类。
			const auto sidebarItem = [&](const std::string& id, const std::string& label,
				bool all, const std::string& filter, int count)
			{
				const Wui::WuiRect item { sidebarRect.X + 4.0f, itemY, sidebarRect.W - 8.0f, 22.0f };
				itemY += 22.0f;
				if (item.Y + item.H > sidebarRect.Y + sidebarRect.H + 0.5f)
					return;   // 栏高之外:不绘制也不登记(与滚动区同一口径)
				const bool selectedCategory = m_AddCategoryAll == all && m_AddCategoryFilter == filter;
				if (selectedCategory)
					Wui::PanelBackground(ctx, item, theme.ActiveBg, 3.0f);
				const std::string text = label + "  (" + std::to_string(count) + ")";
				if (Wui::MenuItem(ctx, Wui::HashId(id.c_str()), item, text, true, theme))
				{
					m_AddCategoryAll = all;
					m_AddCategoryFilter = filter;
					m_AddScroll = 0.0f;
					m_AddHighlight = -1;
				}
			};
			sidebarItem("prop.add.cat.all", Wui::Tr("panel.properties.add.all", "All"), true, std::string(), total);
			for (const auto& [category, count] : counts)
			{
				if (category.empty())
					continue;
				sidebarItem("prop.add.cat." + CategoryKeyName(category), CategoryLabel(category),
					false, category, count);
			}
			if (counts.count(std::string()) > 0)
				sidebarItem("prop.add.cat.uncategorized",
					Wui::Tr("panel.properties.add.uncategorized", "Uncategorized"), false, std::string(),
					counts[std::string()]);
			// (析构点 = 原来的 ClipPop:作用域随本块结束)
		}

		// ---- 列表:本帧最终搜索词决定行(与用户看到的同帧一致)----
		const std::vector<PickerRow> rows = buildRows(m_AddSearch);
		const float rowsHeight = heightOf(rows);
		// 右侧滚动条(用户 2026-09-21:「最好右侧加个滚轮进度」):轨道贴右缘,滑块既是进度
		// 也能拖动/点击定位。列表本体缩到滑块左边,裁剪与命中都由 BeginScrollArea 统一压栈。
		const Wui::WuiRect listClip { listRect.X, listRect.Y,
			std::max(80.0f, listRect.W - kScrollbarWidth - 4.0f), listRect.H };
		const Wui::WuiRect scrollTrack { listClip.X + listClip.W + 4.0f, listRect.Y, kScrollbarWidth, listRect.H };
		const float maxScroll = std::max(0.0f, rowsHeight - listClip.H);
		// 滚轮:落在大列表区由 BeginScrollArea 处理;落在模态其它位置(侧栏/搜索框/行间空白)
		// 同样翻列表 —— 居中模态里"滚轮在哪都翻列表"才符合预期。
		if (!ctx.IsHovered(listClip) && ctx.IsHovered(frame) && ctx.Input().Wheel != 0.0f)
			m_AddScroll -= ctx.Input().Wheel * 40.0f;
		if (std::getenv("WLD_TRACE_UI") && ctx.Input().Wheel != 0.0f)
			WLD_CORE_INFO("[ui] picker wheel {0} at ({1},{2}) listHover={3} frameHover={4} frame=({5},{6},{7},{8}) scroll={9} max={10}",
				ctx.Input().Wheel, static_cast<int>(ctx.Input().MousePos.x), static_cast<int>(ctx.Input().MousePos.y),
				ctx.IsHovered(listClip) ? 1 : 0, ctx.IsHovered(frame) ? 1 : 0,
				static_cast<int>(frame.X), static_cast<int>(frame.Y), static_cast<int>(frame.W), static_cast<int>(frame.H),
				m_AddScroll, maxScroll);

		int itemCount = 0;
		for (const PickerRow& row : rows)
			if (!row.Header)
				++itemCount;
		if (m_AddHighlight >= itemCount)
			m_AddHighlight = itemCount - 1;
		// ↑/↓ 移动高亮(长按连发);无高亮时 ↓ 取第一项、↑ 取最后一项。
		// highlightMoved 只用于"键盘移动过高亮"这一帧的可见性修正(见下面的 reveal)。
		bool highlightMoved = false;
		if (itemCount > 0 && ctx.WasKeyTriggered(KeyCodes::Down))
		{
			m_AddHighlight = m_AddHighlight < 0 ? 0 : std::min(itemCount - 1, m_AddHighlight + 1);
			highlightMoved = true;
		}
		if (itemCount > 0 && ctx.WasKeyTriggered(KeyCodes::Up))
		{
			m_AddHighlight = m_AddHighlight < 0 ? itemCount - 1 : std::max(0, m_AddHighlight - 1);
			highlightMoved = true;
		}

		// ---- 添加:鼠标点击 / Enter(高亮项;无高亮 = 第一个匹配)----
		const auto activate = [&](const Schema::TypeSchema& schema)
		{
			const uint32_t componentId = schema.Storage->ComponentId;
			const entt::entity handle = entity;
			if (scene->DeferStructuralChange([handle, componentId](Scene& target)
			{
				Entity added(&target, handle);
				if (added.IsValid() && added.CanAddComponent(componentId))
					added.AddComponent(componentId);
			}))
				m_Host.MarkDocumentDirty();
			// MRU 置顶并落盘(<local>/wui-properties.json)。
			TouchRecent(SchemaTypeKeyName(schema));
			// 新分区自动展开(与分区绘制读同一个持久化键),再由 OnRender 连续几帧滚到可见。
			const bool defaultOpen = false;
			ctx.Persist<bool>(Wui::HashId(("prop.open." + schema.DisplayName).c_str()), defaultOpen) = true;
			m_RevealSection = schema.DisplayName;
			m_RevealFrames = kPickerRevealFrames;
			CloseAddComponentPicker(ctx);
			ctx.RecordOp("properties", "add-component", schema.DisplayName, SchemaTypeKeyName(schema));
		};

		Wui::BeginScrollArea(ctx, listClip, rowsHeight, m_AddScroll, theme);
		float rowY = listClip.Y - m_AddScroll;
		int itemIndex = -1;
		float highlightTop = 0.0f;
		float highlightHeight = 0.0f;
		for (const PickerRow& row : rows)
		{
			const float rowHeight = row.Header ? kPickerGroupHeader : kPickerRow;
			const Wui::WuiRect item { listClip.X, rowY, listClip.W, rowHeight };
			rowY += rowHeight;
			// 滚出视口的行不绘制也不登记(与属性面板滚动区同一口径 —— AI 点不到用户看不到的行)。
			if (!ctx.ClipAllows(item))
				continue;
			if (row.Header)
			{
				Wui::Label(ctx, { item.X + 6.0f, item.Y + 4.0f }, row.Text, theme.TextMuted, 12.0f);
				// 分组标题:只读文本节点(kind="text"),不参与点击(与只读属性行同一登记口径)。
				RegisterNode(Wui::HashId(row.NodeId.c_str()), "text", item, row.Text, std::string(), false);
				continue;
			}
			++itemIndex;
			const bool highlighted = itemIndex == m_AddHighlight;
			const bool hovered = ctx.IsHovered(item);
			if (highlighted || hovered)
				Wui::PanelBackground(ctx, item, highlighted ? theme.ActiveBg : theme.ButtonHover, 2.0f);
			if (highlighted)
			{
				highlightTop = item.Y;
				highlightHeight = item.H;
			}
			// 名称(术语对照)+ 右侧灰字分类 + 名下 Doc 小字;都按真实可用宽度裁剪。
			const float categoryWidth = ctx.MeasureTextWidth(row.Category, 12.0f);
			// 分类文本过长(长路径的本地化)时不画它,把整行宽度留给名称;分类仍在节点 value 里。
			const bool showCategory = categoryWidth <= item.W * 0.45f;
			const float nameBudget = std::max(40.0f, item.W - 16.0f - (showCategory ? categoryWidth : 0.0f) - 8.0f);
			Wui::LabelWithTerm(ctx, { item.X + 8.0f, item.Y + 4.0f }, row.Text, row.Term, theme.Text,
				14.0f, theme, nameBudget);
			if (showCategory)
				Wui::Label(ctx, { item.X + item.W - 6.0f - categoryWidth, item.Y + 5.0f }, row.Category,
					theme.TextMuted, 12.0f);
			if (!row.Doc.empty())
			{
				Wui::LabelWithTerm(ctx, { item.X + 8.0f, item.Y + 23.0f }, row.Doc, std::string(),
					theme.TextDisabled, theme.FontSizeCaption, theme, item.W - 16.0f);
				Wui::Tooltip(ctx, item, row.Doc);
			}
			// 无障碍:一行一个稳定节点(id = prop.add.<DisplayName>),value = 分类、tooltip = Doc。
			const Wui::LocalizedLabel label = SchemaComponentLabel(*row.Schema);
			RegisterNode(Wui::HashId(row.NodeId.c_str()), "menu-item", item, TermText(label),
				row.Category, true, row.Doc);
			if (hovered)
				ctx.SetCursor(Wui::WuiCursor::Hand);
			// 交互:单击 = 选中(高亮),回车/双击/底部 Add = 真正添加 —— 居中窗口的常规手感。
			// (旧版"单击即加"在弹层贴着按钮时会把"打开弹层的点击"也算进去,实测误加过组件。)
			if (!justOpened && ctx.IsClicked(item))
				m_AddHighlight = itemIndex;
			if (!justOpened && ctx.IsDoubleClicked(item))
			{
				activate(*row.Schema);
				break;
			}
		}
		Wui::EndScrollArea(ctx);

		// 键盘把高亮项移出可视区时把它带回视野 —— **只在高亮刚被键盘移动的那一帧**做。
		// (先前每帧无条件执行:滚轮往下滚时"高亮项已在视口上方"会立刻把偏移拉回去,
		//  用户表现就是"选中一个组件之后滚轮滚不下去"。)
		if (highlightMoved && m_AddHighlight >= 0 && highlightHeight > 0.0f)
		{
			const float top = highlightTop - listClip.Y;
			if (top < 0.0f)
				m_AddScroll = std::max(0.0f, m_AddScroll + top);
			else if (top + highlightHeight > listClip.H)
				m_AddScroll = std::min(maxScroll, m_AddScroll + top + highlightHeight - listClip.H);
		}
		if (rows.empty())
		{
			const std::string empty = Wui::Tr("panel.properties.add.empty", "No matching components");
			Wui::Label(ctx, { listClip.X + 8.0f, listClip.Y + 8.0f }, empty, theme.TextMuted, 13.0f);
			RegisterNode(Wui::HashId("prop.add.empty"), "text", listClip, empty, std::string(), false);
		}

		// ---- 右侧滚动条:进度 + 拖动/点击定位(内容不超一屏时不画,和真实滚动条一致)----
		if (maxScroll > 0.0f)
		{
			const float thumbLength = std::clamp(listClip.H * listClip.H / std::max(1.0f, rowsHeight),
				28.0f, std::max(28.0f, listClip.H));
			const float travel = std::max(0.0f, listClip.H - thumbLength);
			const float fraction = maxScroll > 0.0f ? std::clamp(m_AddScroll / maxScroll, 0.0f, 1.0f) : 0.0f;
			const Wui::WuiRect thumb { scrollTrack.X + 2.0f, scrollTrack.Y + travel * fraction,
				scrollTrack.W - 4.0f, thumbLength };
			Wui::PanelBackground(ctx, scrollTrack, theme.PanelHeader, 3.0f);
			Wui::PanelBackground(ctx, thumb,
				(m_AddThumbDragging || ctx.IsHovered(thumb)) ? theme.Accent : theme.ButtonHover, 3.0f);
			RegisterNode(Wui::HashId("prop.add.scroll"), "scrollbar", scrollTrack,
				Wui::Tr("panel.properties.add.scroll", "Scroll"), std::to_string(static_cast<int>(fraction * 100.0f + 0.5f)) + "%",
				false);

			// 拖动滑块 = 直接定位;点轨道 = 翻到该位置(滑块自己那一下不重复触发定位)。
			if (ctx.IsClicked(thumb))
			{
				m_AddThumbDragging = true;
				m_AddThumbGrabOffset = ctx.Input().MousePos.y - thumb.Y;
			}
			else if (ctx.IsClicked(scrollTrack))
				m_AddScroll = std::clamp((ctx.Input().MousePos.y - scrollTrack.Y - thumbLength * 0.5f)
					/ std::max(1.0f, travel) * maxScroll, 0.0f, maxScroll);
			if (m_AddThumbDragging)
			{
				if (!ctx.Input().MouseDown[0])
					m_AddThumbDragging = false;
				else
					m_AddScroll = std::clamp((ctx.Input().MousePos.y - m_AddThumbGrabOffset - scrollTrack.Y)
						/ std::max(1.0f, travel) * maxScroll, 0.0f, maxScroll);
			}
		}
		else
			m_AddThumbDragging = false;

		// ---- Enter:添加高亮项;没有高亮 = 第一个匹配项 ----
		// 只有键盘在搜索框(searchFocused)或列表侧(m_AddListFocus)时才吃回车 ——
		// 焦点在别的控件上时,回车属于那个控件。
		if (itemCount > 0 && !justOpened && (searchFocused || m_AddListFocus)
			&& ctx.WasKeyPressed(KeyCodes::Enter))
		{
			const int wanted = m_AddHighlight >= 0 ? m_AddHighlight : 0;
			int current = 0;
			for (const PickerRow& row : rows)
			{
				if (row.Header)
					continue;
				if (current++ == wanted)
				{
					activate(*row.Schema);
					break;
				}
			}
		}

		// ---- Tab:搜索框 ⇄ 列表(文本控件持焦点时 WuiContext 不处理 Tab,这里显式接管)----
		if (ctx.WasKeyPressed(KeyCodes::Tab))
		{
			m_AddListFocus = !m_AddListFocus;
			ctx.SetFocus(m_AddListFocus ? listId : searchId);
			if (m_AddListFocus && m_AddHighlight < 0 && itemCount > 0)
				m_AddHighlight = 0;
		}
		ctx.RegisterFocusable(listId, listRect);

		// ---- 底部按钮条:Add(选中项) / Cancel;Esc = 取消 ----
		// Add 只在"有高亮行"时可用(先选再加,避免误加);双击行 = 直接加(见上面的行循环)。
		bool canAdd = m_AddHighlight >= 0 && m_AddHighlight < itemCount;
		const Wui::ModalButtonDesc footerButtons[2] = {
			{ Wui::Tr("panel.properties.add.cancel", "Cancel"), Wui::HashId("prop.add.cancel"), true },
			{ Wui::Tr("panel.properties.add.confirm", "Add"), Wui::HashId("prop.add.confirm"), canAdd },
		};
		const int footerClicked = Wui::ModalButtons(ctx, frame, footerButtons, 2, theme);
		if (footerClicked == 1 && canAdd)
		{
			int current = 0;
			for (const PickerRow& row : rows)
			{
				if (row.Header)
					continue;
				if (current++ == m_AddHighlight)
				{
					activate(*row.Schema);
					break;
				}
			}
		}
		else if (footerClicked == 0)
			CloseAddComponentPicker(ctx);

		// Esc:第一下清空搜索(焦点留在搜索框),搜索为空时第二下关闭模态(与旧口径一致)。
		if (m_AddOpen && searchCancelled)
		{
			if (!m_AddSearch.empty())
			{
				m_AddSearch.clear();
				ctx.SetFocus(searchId);
			}
			else
				CloseAddComponentPicker(ctx);
		}
		else if (m_AddOpen && escapePressed)
			CloseAddComponentPicker(ctx);
		Wui::EndModalFrame(ctx);
		if (!m_AddOpen)
		{
			// 关闭后不复用上一次的状态(下次打开由 OpenAddComponentPicker 重新初始化)。
			m_AddSearch.clear();
			m_AddSearchLast.clear();
			m_AddHighlight = -1;
			m_AddListFocus = false;
			m_AddScroll = 0.0f;
			m_AddThumbDragging = false;
			m_AddThumbGrabOffset = 0.0f;
		}
	}

}
