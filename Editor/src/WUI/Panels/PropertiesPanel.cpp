#include "wldpch.h"
#include "PropertiesPanel.h"

// P2 W5b:Reload 按钮要复用 EditorLayer 的热重载入口(与帧边界轮询、AI 通道 script.reload
// 同一条语义)。PanelHost 是跨任务冻结的窄接口,本包文件边界内不能扩展它,因此只 include。
#include "../../EditorLayer.h"

#include "World/Core/KeyCodes.h"
#include "World/WUI/WuiAccessibility.h"
#include "World/WUI/WuiWidgets.h"

namespace World
{
	namespace
	{
		// ---- 分区滚动布局常量 ----
		constexpr float kContentTop = 40.0f;      // "Add Component" 行高
		constexpr float kSectionHeader = 24.0f;   // 与 WuiSection 的标题行一致
		constexpr float kSectionGap = 2.0f;
		constexpr float kRowHeight = 22.0f;
		constexpr float kScrollbarWidth = 10.0f;
		// 只有在"确实还有内容可滚"时,上/下按钮才注册成可点击节点(与真实可用性一致)。
		constexpr float kScrollEpsilon = 0.5f;

		// ---- 无障碍登记(与 WuiWidgets.cpp 的 RegisterAccessNode 同一格式) ----
		// 面板内的字段/只读值/自定义检查器统一登记,id 由脚本用 Wui::HashId 直接计算。
		void RegisterNode(Wui::WuiId id, const char* kind, const Wui::WuiRect& rect, const std::string& label,
			const std::string& value, bool enabled = true)
		{
			if (id == 0)
				return;
			Wui::WuiAccessNode node;
			node.Id = id;
			node.Window = Wui::WuiAccessibility::Get().CurrentWindow();
			node.Panel = Wui::WuiAccessibility::Get().CurrentPanel();
			node.Kind = kind;
			node.Label = label;
			node.Value = value;
			node.Rect = rect;
			node.Enabled = enabled;
			// 不可用的控件不可被 ui.invoke 点击(与真实鼠标路径一致)。
			node.Interactive = enabled;
			Wui::WuiAccessibility::Get().Register(node);
		}

		std::string FormatFloatText(float value, int decimals = 3)
		{
			char buffer[48] = {};
			std::snprintf(buffer, sizeof(buffer), "%.*f", decimals, value);
			return buffer;
		}

		// 只读展示用:把 schema 值渲染成一行文本(交互路径的控件不参与)。
		// 这里按 variant 的**实际类型**格式化——枚举 getter 产出的是 int64_t/uint64_t,
		// 早前按 Kind 硬取 int32_t 会让 Play/Simulate 下抛 std::bad_variant_access。
		std::string FormatValueByVariant(const Schema::Value& value)
		{
			char buffer[160] = {};
			return std::visit([&buffer](const auto& item) -> std::string
			{
				using T = std::decay_t<decltype(item)>;
				if constexpr (std::is_same_v<T, std::monostate>)
					return "(none)";
				else if constexpr (std::is_same_v<T, bool>)
					return item ? "true" : "false";
				else if constexpr (std::is_same_v<T, int8_t> || std::is_same_v<T, int16_t>
					|| std::is_same_v<T, int32_t> || std::is_same_v<T, int64_t>)
					return std::to_string(static_cast<int64_t>(item));
				else if constexpr (std::is_same_v<T, uint8_t> || std::is_same_v<T, uint16_t>
					|| std::is_same_v<T, uint32_t> || std::is_same_v<T, uint64_t>)
					return std::to_string(static_cast<uint64_t>(item));
				else if constexpr (std::is_same_v<T, float>)
					return FormatFloatText(item);
				else if constexpr (std::is_same_v<T, double>)
					return FormatFloatText(static_cast<float>(item));
				else if constexpr (std::is_same_v<T, glm::vec2>)
				{
					std::snprintf(buffer, sizeof(buffer), "(%.2f, %.2f)", item.x, item.y);
					return buffer;
				}
				else if constexpr (std::is_same_v<T, glm::vec3>)
				{
					std::snprintf(buffer, sizeof(buffer), "(%.2f, %.2f, %.2f)", item.x, item.y, item.z);
					return buffer;
				}
				else if constexpr (std::is_same_v<T, glm::vec4>)
				{
					std::snprintf(buffer, sizeof(buffer), "(%.2f, %.2f, %.2f, %.2f)", item.x, item.y, item.z, item.w);
					return buffer;
				}
				else if constexpr (std::is_same_v<T, std::string>)
					return item;
				else
					return "(...)";   // 对象/矩阵/四元数等:只读态给出占位,避免误导
			}, value);
		}

		// 枚举显示名解析(只读值 + 自定义检查器的下拉都用它)。
		std::string EnumNameOf(const Schema::EnumSchema& schema, int64_t raw)
		{
			if (const char* name = schema.FindName(raw))
				return name;
			return std::to_string(raw);
		}

		int64_t EnumRawOf(const Schema::EnumSchema& schema, const Schema::Value& value)
		{
			if (std::holds_alternative<int64_t>(value))
				return std::get<int64_t>(value);
			if (std::holds_alternative<uint64_t>(value))
				return static_cast<int64_t>(std::get<uint64_t>(value));
			return 0;
		}

		std::string FormatReadOnlyValue(const Schema::FieldSchema& field, const Schema::Value& value)
		{
			if (field.K == Schema::Kind::Enum)
			{
				const Schema::EnumSchema* schema = field.GetEnum ? field.GetEnum() : nullptr;
				if (schema)
					return EnumNameOf(*schema, EnumRawOf(*schema, value));
			}
			return FormatValueByVariant(value);
		}

		// 自定义检查器的无障碍 id 契约:properties.TransformComponent.Location 等
		// (脚本用同一 FNV-1a 32 位算法直接计算,无需先 ui.tree)。
		std::string PropPath(const std::string& typeName, const std::string& fieldName)
		{
			return "properties." + typeName + "." + fieldName;
		}

		Wui::WuiRect ComponentRect(const Wui::WuiRect& rect, int index, float height)
		{
			return { rect.X, rect.Y + kRowHeight * static_cast<float>(index), rect.W, height };
		}

		Wui::WuiRect ScaledComponentRect(const Wui::WuiRect& rect, int index, int count)
		{
			const float slot = rect.W / static_cast<float>(count);
			return { rect.X + slot * static_cast<float>(index), rect.Y, slot - 2.0f, rect.H };
		}

		// 自定义检查器的 Vec3 行(度/单位由调用方处理),返回本帧是否有编辑。
		bool DrawVec3Row(Wui::WuiContext& ctx, const std::string& baseId, const Wui::WuiRect& row,
			const std::string& label, glm::vec3& value, const Wui::WuiTheme& theme, bool reachable)
		{
			Label(ctx, { row.X + 4, row.Y + 3 }, label, theme.TextMuted, 13.0f);
			const Wui::WuiRect ctrl { row.X + std::min(140.0f, row.W * 0.45f), row.Y + 1,
				row.W - std::min(140.0f, row.W * 0.45f) - 4, 20 };
			static const char* const kSuffix[3] = { ".x", ".y", ".z" };
			static const char* const kShort[3] = { "x", "y", "z" };
			bool changed = false;
			for (int c = 0; c < 3; ++c)
			{
				float component = value[c];
				const Wui::WuiRect slot = ScaledComponentRect(ctrl, c, 3);
				Wui::DragFloat(ctx, Wui::HashId((baseId + kSuffix[c]).c_str()), slot, component, 0.01f, 1.0f, -1.0f, theme);
				if (component != value[c])
				{
					value[c] = component;
					changed = true;
				}
				RegisterNode(Wui::HashId((baseId + kSuffix[c]).c_str()), "drag-float", slot,
					label + "." + kShort[c], FormatFloatText(component), reachable);
			}
			return changed;
		}

		// 浮点行(带范围;无范围时用 1/-1 哨兵,与 schema 字段路径一致)。
		bool DrawFloatRow(Wui::WuiContext& ctx, const std::string& idText, const Wui::WuiRect& row,
			const std::string& label, float& value, float lo, float hi, const Wui::WuiTheme& theme, bool reachable)
		{
			Label(ctx, { row.X + 4, row.Y + 3 }, label, theme.TextMuted, 13.0f);
			const Wui::WuiRect ctrl { row.X + std::min(140.0f, row.W * 0.45f), row.Y + 1,
				row.W - std::min(140.0f, row.W * 0.45f) - 4, 20 };
			const float before = value;
			Wui::DragFloat(ctx, Wui::HashId(idText.c_str()), ctrl, value, 0.01f, lo, hi, theme);
			RegisterNode(Wui::HashId(idText.c_str()), "drag-float", ctrl, label, FormatFloatText(value), reachable);
			return value != before;
		}

		// 只读行(登记 properties.<...> 文本节点,enabled=false,不可点击)。
		void DrawReadOnlyRow(Wui::WuiContext& ctx, const std::string& idText, const Wui::WuiRect& row,
			const std::string& label, const std::string& value, const Wui::WuiTheme& theme)
		{
			Label(ctx, { row.X + 4, row.Y + 3 }, label, theme.TextMuted, 13.0f);
			Label(ctx, { row.X + std::min(140.0f, row.W * 0.45f), row.Y + 3 }, value, theme.Text, 13.0f);
			RegisterNode(Wui::HashId(idText.c_str()), "text", row, label, value, false);
		}

		std::string ScriptStateName(ScriptInstanceState state)
		{
			switch (state)
			{
				case ScriptInstanceState::Pending: return "Pending";
				case ScriptInstanceState::Creating: return "Creating";
				case ScriptInstanceState::Running: return "Running";
				case ScriptInstanceState::Destroying: return "Destroying";
				case ScriptInstanceState::Stopped: return "Stopped";
				case ScriptInstanceState::Faulted: return "Faulted";
				default: return "?";
			}
		}

		// 面板里诊断/错误只显示第一行并截断;完整文本由 AI 通道 script.status 提供。
		std::string TruncateForPanel(const std::string& text, size_t limit = 72)
		{
			const size_t newline = text.find('\n');
			std::string line = text.substr(0, newline == std::string::npos ? text.size() : newline);
			if (line.size() > limit)
				line = line.substr(0, limit) + "...";
			return line;
		}

		// 字符串字段的编辑期缓冲区:Enter/失焦提交,Escape 丢弃。
		struct SchemaTextState
		{
			std::string Buffer;
			bool Editing = false;
		};
	}

	void PropertiesPanel::OnRender(Wui::WuiContext& ctx, const Wui::WuiRect& rect, PanelHost& host)
	{
		const Wui::WuiTheme& theme = host.Theme();
		// Play/Simulate = 只读查看:字段只显示不写回,并给出提示(用户确认的语义)。
		m_ReadOnly = host.IsReadOnlyMode();
		Entity entity = host.GetSelectedEntity();
		if (!entity.IsValid() || entity.GetScene() != host.GetActiveScene().get())
		{
			Label(ctx, { rect.X + 8, rect.Y + 8 }, "No entity selected", theme.TextMuted, 14.0f);
			return;
		}
		Scene* scene = entity.GetScene();
		Schema::SchemaRegistry& schemas = scene->GetContext().Schemas();
		if (m_ReadOnly)
		{
			// 诊断(WLD_TRACE_UI=1):只读态确实解析到实体时打一行(选择变化才打)。
			// 与 EditorLayer 的 "selection cleared" 对照即可判断选择是否被归属校验清掉。
			if (std::getenv("WLD_TRACE_UI"))
			{
				static uint32_t lastHandle = ~0u;
				static const void* lastScene = nullptr;
				const uint32_t handle = static_cast<uint32_t>(static_cast<entt::entity>(entity));
				if (handle != lastHandle || static_cast<const void*>(scene) != lastScene)
				{
					lastHandle = handle;
					lastScene = static_cast<const void*>(scene);
					WLD_CORE_INFO("[ui] properties resolved entity (read-only): handle={0} scene={1}",
						handle, static_cast<const void*>(scene));
				}
			}
			Label(ctx, { rect.X + 8, rect.Y + 8 }, "Play/Simulate 运行中:只读查看(暂停或退出后可编辑)",
				theme.TextMuted, 13.0f);
		}

		const Wui::WuiRect addButton { rect.X + 8, rect.Y + (m_ReadOnly ? 30.0f : 8.0f), 140, 24 };
		if (!m_ReadOnly && Button(ctx, Wui::HashId("prop.add"), addButton, "Add Component", theme))
			ctx.OpenPopup(Wui::HashId("prop.add.popup"));

		const Wui::WuiId addPopup = Wui::HashId("prop.add.popup");
		if (ctx.IsPopupOpen(addPopup))
		{
			ctx.PushOverlay();
			std::vector<const Schema::TypeSchema*> candidates;
			for (const Schema::TypeSchema* schema : schemas.List(Schema::TypeCategory::Component))
				if (schema && schema->Storage && !entity.HasComponent(schema->Storage->ComponentId))
					candidates.push_back(schema);
			const Wui::WuiRect panel { addButton.X, addButton.Y + addButton.H, 220, candidates.size() * 22.0f + 8 };
			DrawPanelSurface(ctx, panel, theme);
			for (size_t i = 0; i < candidates.size(); ++i)
			{
				const Wui::WuiRect item { panel.X + 4, panel.Y + 4 + i * 22, panel.W - 8, 22 };
				if (MenuItem(ctx, Wui::HashId(("prop.add." + candidates[i]->DisplayName).c_str()), item, candidates[i]->DisplayName, true, theme))
				{
					const uint32_t componentId = candidates[i]->Storage->ComponentId;
					const entt::entity handle = entity;
					if (scene->DeferStructuralChange([handle, componentId](Scene& s)
					{
						Entity target(&s, handle);
						if (target.IsValid() && target.CanAddComponent(componentId))
							target.AddComponent(componentId);
					}))
						host.MarkDocumentDirty();
					ctx.CloseAllPopups();
				}
			}
			ctx.ClosePopupsOnOutsideClick({ addPopup }, panel);
			if (ctx.IsKeyPressed(KeyCodes::Escape))
				ctx.ClosePopup(addPopup);
			ctx.PopOverlay();
		}

		// 组件分区进入保留模式布局树;字段内容复用已测的 schema 绘制逻辑。
		std::vector<const Schema::TypeSchema*> componentSchemas;
		for (const Schema::TypeSchema* schema : schemas.List(Schema::TypeCategory::Component))
		{
			if (!schema || !schema->Storage || !entity.HasComponent(schema->Storage->ComponentId))
				continue;
			componentSchemas.push_back(schema);
		}

		std::vector<std::string> schemaNames;
		for (const Schema::TypeSchema* schema : componentSchemas)
			schemaNames.push_back(schema->DisplayName);
		if (schemaNames != m_LastSchemaNames)
		{
			m_LastSchemaNames = std::move(schemaNames);
			m_Sections.clear();
			for (const Schema::TypeSchema* schema : componentSchemas)
			{
				// P2 W5b:Lua 脚本分区默认展开 —— 诊断与 Reload 按钮必须真的在无障碍树里,
				// 才能被 AI 通道 ui.invoke 无鼠标驱动(其它组件分区保持默认折叠)。
				const bool defaultOpen = schema->Id.Name == "World::LuaScriptComponent";
				m_Sections.push_back({ schema->DisplayName, defaultOpen, 0.0f });
			}
		}

		// ---- 滚动布局(与迁移前一致的分区顺序;标题/展开态由面板持久化)----
		// 内容高度取上一帧实测值(首帧按 0 计),分区每帧重绘,下一帧即精确。
		const float viewportHeight = std::max(0.0f, rect.H - kContentTop - 4.0f);
		float contentHeight = 0.0f;
		for (size_t i = 0; i < m_Sections.size(); ++i)
		{
			if (i >= componentSchemas.size())
				break;
			const Schema::TypeSchema* schema = componentSchemas[i];
			const bool defaultOpen = schema->Id.Name == "World::LuaScriptComponent";
			bool& open = ctx.Persist<bool>(Wui::HashId(("prop.open." + schema->DisplayName).c_str()), defaultOpen);
			m_Sections[i].Open = open;
			contentHeight += kSectionHeader + (open ? m_Sections[i].ContentHeight : 0.0f) + kSectionGap;
		}

		const Wui::WuiRect scrollViewport { rect.X + 6, rect.Y + kContentTop, rect.W - 12 - kScrollbarWidth, viewportHeight };
		const Wui::WuiRect visibleContent { scrollViewport.X, scrollViewport.Y, scrollViewport.W, viewportHeight };
		const float maxScroll = std::max(0.0f, contentHeight - viewportHeight);
		ScrollState& scroll = ctx.Persist<ScrollState>(Wui::HashId("prop.scroll.state"), {});
		const uint32_t entityHandle = static_cast<uint32_t>(static_cast<entt::entity>(entity));
		if (scroll.Handle != entityHandle)
		{
			scroll.Handle = entityHandle;
			scroll.Offset = 0.0f;
		}
		if (ctx.IsHovered(scrollViewport) && ctx.Input().Wheel != 0.0f)
			scroll.Offset -= ctx.Input().Wheel * 40.0f;
		scroll.Offset = std::clamp(scroll.Offset, 0.0f, maxScroll);

		// 分区区在可视裁剪内绘制:滚出可视区的控件保留在无障碍树里(可见性由中心点判定,
		// 滚回可视区即可被 ui.invoke 命中 —— 不会出现"AI 点到用户看不到的控件")。
		const Wui::WuiRect contentRect { rect.X + 6, rect.Y + kContentTop - scroll.Offset,
			rect.W - 12 - kScrollbarWidth, contentHeight };
		float sectionY = 0.0f;
		ctx.Commands().push_back({ Wui::WuiDrawKind::ClipPush, scrollViewport });
		for (size_t i = 0; i < m_Sections.size(); ++i)
		{
			if (i >= componentSchemas.size())
				break;
			const Schema::TypeSchema* schema = componentSchemas[i];
			SectionEntry& section = m_Sections[i];
			// 分区顺序可能因 schema 列表变化而与 m_Sections 错位:名字不同则本帧跳过绘制。
			if (section.Title != schema->DisplayName)
				break;
			const bool defaultOpen = schema->Id.Name == "World::LuaScriptComponent";
			bool& open = ctx.Persist<bool>(Wui::HashId(("prop.open." + schema->DisplayName).c_str()), defaultOpen);

			// 标题行:与 WuiSection 相同的底色/文字与展开行为;同时登记为可点节点
			// (properties.section.<DisplayName>),脚本可展开/折叠分区。
			const float rowY = contentRect.Y + sectionY;
			const Wui::WuiRect header { contentRect.X, rowY, contentRect.W, kSectionHeader };
			ctx.Commands().push_back({ Wui::WuiDrawKind::Rect, header, open ? Wui::WuiColor { 0.27f, 0.28f, 0.31f, 1 } : Wui::WuiColor { 0.2f, 0.21f, 0.23f, 1 }, 2.0f });
			ctx.Commands().push_back({ Wui::WuiDrawKind::Text, { header.X + 6, header.Y + 3, 0, 0 },
				theme.Text, 0, 1.0f, (open ? "- " : "+ ") + section.Title, 14.0f, false });
			const std::string headerId = "properties.section." + section.Title;
			RegisterNode(Wui::HashId(headerId.c_str()), "button", header, headerId, open ? "open" : "closed");
			if (ctx.IsClicked(header))
			{
				open = !open;
				section.Open = open;
				ctx.RecordOp("properties", "toggle-section", section.Title, open ? "open" : "closed");
			}

			const Wui::WuiRect inner { contentRect.X + 10, rowY + kSectionHeader, contentRect.W - 10, 0 };
			if (open)
			{
				const float measured = DrawComponentInspector(ctx, inner, entity, *schema, visibleContent);
				section.ContentHeight = measured + 4.0f;
				sectionY += kSectionHeader + section.ContentHeight + kSectionGap;
			}
			else
			{
				// 折叠态保留上次实测高度:重新展开时不会把布局跳成 0 再恢复。
				sectionY += kSectionHeader + kSectionGap;
			}
		}
		ctx.Commands().push_back({ Wui::WuiDrawKind::ClipPop });

		// ---- 滚动条 + 上/下翻页按钮(始终固定在可视区右缘;脚本可 ui.invoke)----
		// 只有确实还有内容可滚时才登记为可点节点,disabled 时 ui.invoke 的拒绝文案与
		// 真实可用性一致。点击"v"把滚动位置推进一页 —— 属性面板可脚本化的关键路径。
		if (viewportHeight >= kSectionHeader * 3.0f)
		{
			const Wui::WuiRect track { rect.X + rect.W - kScrollbarWidth - 2, scrollViewport.Y, kScrollbarWidth, viewportHeight };
			const float trackInner = std::max(0.0f, track.H - kSectionHeader * 2.0f);
			const float thumbLength = contentHeight > viewportHeight
				? std::clamp(viewportHeight * viewportHeight / std::max(viewportHeight, contentHeight), 24.0f, trackInner)
				: trackInner;
			const float thumbTravel = std::max(0.0f, trackInner - thumbLength);
			const float fraction = maxScroll > 0.0f ? scroll.Offset / maxScroll : 0.0f;
			const Wui::WuiRect upButton { track.X, track.Y, track.W, kSectionHeader };
			const Wui::WuiRect downButton { track.X, track.Y + track.H - kSectionHeader, track.W, kSectionHeader };
			const Wui::WuiRect thumb { track.X, track.Y + kSectionHeader + thumbTravel * fraction, track.W, thumbLength };

			ctx.Commands().push_back({ Wui::WuiDrawKind::Rect, track, theme.PanelHeader, 2.0f });
			ctx.Commands().push_back({ Wui::WuiDrawKind::Rect, upButton, ctx.IsHovered(upButton) ? theme.ButtonHover : theme.ButtonBg, 2.0f });
			ctx.Commands().push_back({ Wui::WuiDrawKind::Rect, downButton, ctx.IsHovered(downButton) ? theme.ButtonHover : theme.ButtonBg, 2.0f });
			ctx.Commands().push_back({ Wui::WuiDrawKind::Rect, thumb, theme.Accent, 2.0f });
			ctx.Commands().push_back({ Wui::WuiDrawKind::Text, { upButton.X + 1, upButton.Y + 4, 0, 0 }, theme.Text, 0, 1.0f, "^", 13.0f, false });
			ctx.Commands().push_back({ Wui::WuiDrawKind::Text, { downButton.X + 1, downButton.Y + 4, 0, 0 }, theme.Text, 0, 1.0f, "v", 13.0f, false });

			const bool canScrollUp = scroll.Offset > kScrollEpsilon;
			const bool canScrollDown = scroll.Offset < maxScroll - kScrollEpsilon;
			const std::string upId = PropPath("properties", "scroll.up");
			const std::string downId = PropPath("properties", "scroll.down");
			RegisterNode(Wui::HashId(upId.c_str()), "button", upButton, upId,
				canScrollUp ? "enabled" : "top", canScrollUp);
			RegisterNode(Wui::HashId(downId.c_str()), "button", downButton, downId,
				canScrollDown ? "enabled" : "bottom", canScrollDown);
			if (canScrollUp && ctx.IsClicked(upButton))
				scroll.Offset = std::max(0.0f, scroll.Offset - (viewportHeight - kRowHeight));
			if (canScrollDown && ctx.IsClicked(downButton))
				scroll.Offset = std::min(maxScroll, scroll.Offset + (viewportHeight - kRowHeight));

			// 拖动滚动条滑块(与真实滚动条一致的直接定位)。
			if (ctx.IsClicked(thumb))
			{
				m_ScrollThumbDragging = true;
				m_ScrollThumbGrabOffset = ctx.Input().MousePos.y - thumb.Y;
			}
			if (m_ScrollThumbDragging)
			{
				if (ctx.Input().MouseDown[0])
				{
					const float travel = std::max(0.0f, thumbTravel);
					if (travel > 0.0f)
					{
						const float local = ctx.Input().MousePos.y - track.Y - kSectionHeader - m_ScrollThumbGrabOffset;
						scroll.Offset = std::clamp(local / travel, 0.0f, 1.0f) * maxScroll;
					}
				}
				else
					m_ScrollThumbDragging = false;
			}
		}
	}

	float PropertiesPanel::DrawSchemaFields(Wui::WuiContext& ctx, Wui::WuiId base, const Wui::WuiRect& rect,
		void* instance, const std::string& typeName, const Schema::TypeSchema& schema,
		const Wui::WuiRect& visibleRect)
	{
		const Wui::WuiTheme& theme = m_Host.Theme();
		float y = 0;
		bool changed = false;
		const float labelWidth = std::min(140.0f, rect.W * 0.45f);
		// 稳定无障碍 id 契约:properties.<TypeDisplayName>.<字段名>(脚本用同样字符串算 HashId)。
		const auto propId = [](const std::string& type, const std::string& field)
		{ return "properties." + type + "." + field; };
		// 只登记"中心点落在面板可视区内"的控件:滚出去的控件保留节点但 visible=false,
		// 与控件的真实可点性一致(滚回来即可被 ui.invoke 命中)。
		const auto reachable = [&visibleRect](const Wui::WuiRect& control)
		{
			return control.X + control.W * 0.5f >= visibleRect.X
				&& control.X + control.W * 0.5f <= visibleRect.X + visibleRect.W
				&& control.Y + control.H * 0.5f >= visibleRect.Y
				&& control.Y + control.H * 0.5f <= visibleRect.Y + visibleRect.H;
		};
		for (const Schema::FieldSchema& field : schema.Fields)
		{
			if (field.Meta.Transient)
				continue;
			const Wui::WuiId fid = Wui::HashId(("f." + typeName + "." + field.Name).c_str()) ^ base;
			const Wui::WuiRect row { rect.X, rect.Y + y, rect.W, 22 };
			const Wui::WuiRect ctrl { row.X + labelWidth, row.Y + 1, row.W - labelWidth - 4, 20 };
			const std::string label = field.Meta.DisplayName.empty() ? field.Name : field.Meta.DisplayName;

			if (field.K == Schema::Kind::Object)
			{
				const std::string idText = propId(typeName, field.Name);
				const Schema::TypeSchema* nested = field.GetNested ? field.GetNested() : nullptr;
				void* nestedInstance = field.GetPtr ? field.GetPtr(instance) : nullptr;
				// UUID 等身份标识只读展示,不提供编辑控件。
				if (nested && nestedInstance && (nested->Id.Name == "World::UUID" || nested->DisplayName == "UUID"))
				{
					std::string display = "(invalid)";
					if (!nested->Fields.empty() && nested->Fields[0].Get)
					{
						const Schema::Value inner = nested->Fields[0].Get(nestedInstance);
						if (std::holds_alternative<uint64_t>(inner))
						{
							char buffer[32];
							std::snprintf(buffer, sizeof(buffer), "%llu", static_cast<unsigned long long>(std::get<uint64_t>(inner)));
							display = buffer;
						}
					}
					Label(ctx, { row.X + 4, row.Y + 3 }, label, theme.TextMuted, 13.0f);
					Label(ctx, { ctrl.X, row.Y + 3 }, display, theme.Text, 13.0f);
					RegisterNode(Wui::HashId(idText.c_str()), "text", row, label, display, false);
					y += 20;
					continue;
				}
				bool& open = ctx.Persist<bool>(fid, false);
				if (ctx.IsClicked(row))
					open = !open;
				Label(ctx, { row.X + 4, row.Y + 3 }, (open ? "- " : "+ ") + label, theme.Text, 13.0f);
				RegisterNode(Wui::HashId(idText.c_str()), "button", row, label, open ? "open" : "closed", reachable(row));
				y += 20;
				if (open && nested && nestedInstance)
					y += DrawSchemaFields(ctx, fid ^ 0x9e3779b9u, { row.X + 10, row.Y + 20, row.W - 10, 0 },
						nestedInstance, nested->DisplayName, *nested, visibleRect);
				continue;
			}

			if (m_ReadOnly || field.Meta.ReadOnly || !field.Get || !field.Set)
			{
				const std::string idText = propId(typeName, field.Name);
				// 只读也要显示"值":否则 Play/Simulate 下属性面板只剩字段名,看起来像"什么都不显示"。
				std::string text = label;
				if (field.Get)
					text += ": " + FormatReadOnlyValue(field, field.Get(instance));
				Label(ctx, { row.X + 4, row.Y + 3 }, text, theme.TextMuted, 13.0f);
				// 只读字段登记为不可交互文本节点:ui.tree 能断言"可见但禁用"。
				RegisterNode(Wui::HashId(idText.c_str()), "text", row,
					label, field.Get ? FormatReadOnlyValue(field, field.Get(instance)) : std::string(),
					false);
				y += 20;
				continue;
			}

			// 交互字段:标签登记为静态节点(不可点),控件本体按真实 kind 登记
			// (脚本用 properties.<Type>.<Field> 直接 ui.invoke)。
			const std::string idText = propId(typeName, field.Name);
			RegisterNode(Wui::HashId(idText.c_str()), "label", row, label, std::string(), false);
			Label(ctx, { row.X + 4, row.Y + 3 }, label, theme.TextMuted, 13.0f);
			Schema::Value value = field.Get(instance);
			bool fieldChanged = false;
			switch (field.K)
			{
				case Schema::Kind::Bool:
				{
					bool b = std::get<bool>(value);
					const bool before = b;
					Checkbox(ctx, fid, ctrl, "", b, theme);
					fieldChanged = b != before;
					if (fieldChanged) value = b;
					RegisterNode(Wui::HashId(idText.c_str()), "checkbox", ctrl, label, b ? "true" : "false", reachable(ctrl));
					break;
				}
				case Schema::Kind::Int8:
				case Schema::Kind::Int16:
				case Schema::Kind::Int32:
				case Schema::Kind::Int64:
				{
					int64_t raw = field.K == Schema::Kind::Int8 ? std::get<int8_t>(value)
						: field.K == Schema::Kind::Int16 ? std::get<int16_t>(value)
						: field.K == Schema::Kind::Int32 ? std::get<int32_t>(value) : std::get<int64_t>(value);
					const int64_t before = raw;
					const int64_t lo = field.Meta.Min.has_value() ? static_cast<int64_t>(*field.Meta.Min) : INT64_MIN;
					const int64_t hi = field.Meta.Max.has_value() ? static_cast<int64_t>(*field.Meta.Max) : INT64_MAX;
					DragInt(ctx, fid, ctrl, raw, lo, hi, theme);
					fieldChanged = raw != before;
					if (fieldChanged)
					{
						if (field.K == Schema::Kind::Int8) value = static_cast<int8_t>(raw);
						else if (field.K == Schema::Kind::Int16) value = static_cast<int16_t>(raw);
						else if (field.K == Schema::Kind::Int32) value = static_cast<int32_t>(raw);
						else value = raw;
					}
					RegisterNode(Wui::HashId(idText.c_str()), "drag-int", ctrl, label, std::to_string(raw), reachable(ctrl));
					break;
				}
				case Schema::Kind::UInt8:
				case Schema::Kind::UInt16:
				case Schema::Kind::UInt32:
				case Schema::Kind::UInt64:
				{
					uint64_t raw = field.K == Schema::Kind::UInt8 ? std::get<uint8_t>(value)
						: field.K == Schema::Kind::UInt16 ? std::get<uint16_t>(value)
						: field.K == Schema::Kind::UInt32 ? std::get<uint32_t>(value) : std::get<uint64_t>(value);
					int64_t signedRaw = static_cast<int64_t>(raw);
					const int64_t before = signedRaw;
					const int64_t lo = field.Meta.Min.has_value() ? static_cast<int64_t>(*field.Meta.Min) : 0;
					const int64_t hi = field.Meta.Max.has_value() ? static_cast<int64_t>(*field.Meta.Max) : INT64_MAX;
					DragInt(ctx, fid, ctrl, signedRaw, lo, hi, theme);
					fieldChanged = signedRaw != before;
					if (fieldChanged)
					{
						if (field.K == Schema::Kind::UInt8) value = static_cast<uint8_t>(signedRaw);
						else if (field.K == Schema::Kind::UInt16) value = static_cast<uint16_t>(signedRaw);
						else if (field.K == Schema::Kind::UInt32) value = static_cast<uint32_t>(signedRaw);
						else value = static_cast<uint64_t>(signedRaw);
					}
					RegisterNode(Wui::HashId(idText.c_str()), "drag-int", ctrl, label, std::to_string(signedRaw), reachable(ctrl));
					break;
				}
				case Schema::Kind::Float:
				case Schema::Kind::Double:
				{
					float f = field.K == Schema::Kind::Float ? std::get<float>(value) : static_cast<float>(std::get<double>(value));
					const float before = f;
					const float lo = field.Meta.Min.has_value() ? *field.Meta.Min : 1.0f;   // 1,-1 哨兵 = 无范围
					const float hi = field.Meta.Max.has_value() ? *field.Meta.Max : -1.0f;
					DragFloat(ctx, fid, ctrl, f, 0.01f, lo, hi, theme);
					fieldChanged = f != before;
					if (fieldChanged) value = field.K == Schema::Kind::Float ? Schema::Value(f) : Schema::Value(static_cast<double>(f));
					RegisterNode(Wui::HashId(idText.c_str()), "drag-float", ctrl, label, FormatFloatText(f), reachable(ctrl));
					break;
				}
				case Schema::Kind::Vec2:
				case Schema::Kind::Vec3:
				case Schema::Kind::Vec4:
				{
					const int components = field.K == Schema::Kind::Vec2 ? 2 : (field.K == Schema::Kind::Vec3 ? 3 : 4);
					const float slot = ctrl.W / components;
					for (int c = 0; c < components; ++c)
					{
						float f = field.K == Schema::Kind::Vec2 ? std::get<glm::vec2>(value)[c]
							: field.K == Schema::Kind::Vec3 ? std::get<glm::vec3>(value)[c] : std::get<glm::vec4>(value)[c];
						const float before = f;
						DragFloat(ctx, fid ^ static_cast<Wui::WuiId>(c + 1), { ctrl.X + slot * c, ctrl.Y, slot - 2, ctrl.H }, f, 0.01f, 1.0f, -1.0f, theme);
						if (f != before)
						{
							fieldChanged = true;
							if (field.K == Schema::Kind::Vec2) std::get<glm::vec2>(value)[c] = f;
							else if (field.K == Schema::Kind::Vec3) std::get<glm::vec3>(value)[c] = f;
							else std::get<glm::vec4>(value)[c] = f;
						}
						static const char* const kSuffix[4] = { ".x", ".y", ".z", ".w" };
						static const char* const kShort[4] = { "x", "y", "z", "w" };
						const std::string componentId = idText + kSuffix[c];
						const Wui::WuiRect slotRect { ctrl.X + slot * static_cast<float>(c), ctrl.Y, slot - 2, ctrl.H };
						RegisterNode(Wui::HashId(componentId.c_str()), "drag-float", slotRect,
							label + "." + kShort[c], FormatFloatText(f), reachable(slotRect));
					}
					break;
				}
				case Schema::Kind::String:
				case Schema::Kind::Asset:
				{
					// 与 TextField 的 WuiEditState 共用 fid 会导致类型混淆,
					// 编辑缓冲必须使用独立 id。
					auto& state = ctx.Persist<SchemaTextState>(Wui::HashId("schema.text.state") ^ fid, {});
					const std::string current = std::get<std::string>(value);
					if (!state.Editing)
						state.Buffer = current;
					bool cancelled = false;
					if (TextField(ctx, fid, ctrl, state.Buffer, theme, &cancelled))
					{
						// Enter 提交
						if (state.Editing && state.Buffer != current)
						{
							value = state.Buffer;
							fieldChanged = true;
						}
						state.Editing = false;
					}
					else if (state.Editing)
					{
						if (cancelled)
						{
							// Escape 丢弃
							state.Editing = false;
						}
						else if (ctx.Focus() != fid)
						{
							// 失焦提交
							if (state.Buffer != current)
							{
								value = state.Buffer;
								fieldChanged = true;
							}
							state.Editing = false;
						}
					}
					// 本次点击进入编辑
					if (!state.Editing && ctx.Focus() == fid)
						state.Editing = true;
					RegisterNode(Wui::HashId(idText.c_str()), "text-field", ctrl, label, current, reachable(ctrl));
					break;
				}
				case Schema::Kind::Enum:
				{
					const Schema::EnumSchema* es = field.GetEnum ? field.GetEnum() : nullptr;
					if (es)
					{
						std::vector<std::string> names;
						int selected = 0;
						const int64_t raw = es->IsSigned ? std::get<int64_t>(value) : static_cast<int64_t>(std::get<uint64_t>(value));
						for (size_t i = 0; i < es->Values.size(); ++i)
						{
							names.push_back(es->Values[i].first);
							if (es->Values[i].second == raw)
								selected = static_cast<int>(i);
						}
						const int before = selected;
						Combo(ctx, fid, ctrl, "", names, selected, theme);
						fieldChanged = selected != before;
						if (fieldChanged)
							value = es->IsSigned ? Schema::Value(es->Values[selected].second) : Schema::Value(static_cast<uint64_t>(es->Values[selected].second));
						RegisterNode(Wui::HashId(idText.c_str()), "combo", ctrl, label,
							(selected >= 0 && selected < static_cast<int>(names.size())) ? names[selected] : std::string(),
							reachable(ctrl));
					}
					break;
				}
				default:
					Label(ctx, { ctrl.X, ctrl.Y + 3 }, "(unsupported)", theme.TextMuted, 12.0f);
					RegisterNode(Wui::HashId(idText.c_str()), "text", row, label, "(unsupported)", false);
					break;
			}
			if (fieldChanged)
			{
				field.Set(instance, value);
				changed = true;
			}
			y += 22;
		}

		if (changed)
			m_Host.MarkDocumentDirty();
		return y;
	}

	float PropertiesPanel::DrawComponentInspector(Wui::WuiContext& ctx, const Wui::WuiRect& rect, Entity entity,
		const Schema::TypeSchema& schema, const Wui::WuiRect& visibleRect)
	{
		const Wui::WuiTheme& theme = m_Host.Theme();
		void* instance = entity.GetComponent(schema.Storage->ComponentId);
		if (!instance)
			return 0;
		const Wui::WuiId base = Wui::HashId(schema.DisplayName.c_str());

		// 自定义检查器:与迁移前一致的三行 Location/Rotation(度)/Scale。
		if (schema.Id.Name == "World::TransformComponent")
			return DrawTransformInspector(ctx, rect, *static_cast<TransformComponent*>(instance), schema, visibleRect);

		// 自定义检查器:Primary / Fixed Aspect Ratio + 投影类型下拉 + 对应参数组。
		if (schema.Id.Name == "World::CameraComponent")
			return DrawCameraInspector(ctx, rect, instance, schema, visibleRect);

		if (schema.Id.Name == "World::NativeScriptComponent")
		{
			auto* script = static_cast<NativeScriptComponent*>(instance);
			Scene* scene = entity.GetScene();
			Schema::SchemaRegistry& schemas = scene->GetContext().Schemas();
			std::vector<std::string> names;
			std::vector<const Schema::TypeSchema*> scripts = schemas.List(Schema::TypeCategory::Script);
			int selected = -1;
			for (size_t i = 0; i < scripts.size(); ++i)
			{
				names.push_back(scripts[i]->DisplayName);
				if (scripts[i]->DisplayName == script->ScriptName)
					selected = static_cast<int>(i);
			}
			if (Combo(ctx, base ^ 1u, { rect.X, rect.Y, rect.W, 22 }, "Script", names, selected, theme) && selected >= 0)
			{
				if (scripts[selected]->Script)
					scripts[selected]->Script->Bind(static_cast<void*>(script));
				script->ScriptName = names[selected];
				script->ResetEditorFieldState();
				m_Host.MarkDocumentDirty();
			}
			float y = 26;
			Label(ctx, { rect.X, rect.Y + y }, "state: " + ScriptStateName(script->State), theme.TextMuted, 13.0f);
			y += 18;
			if (!script->LastError.empty())
			{
				Label(ctx, { rect.X, rect.Y + y }, script->LastError, { 1, 0.4f, 0.4f, 1 }, 12.0f);
				y += 18;
			}
			const Schema::TypeSchema* scriptSchema = schemas.Find(script->ScriptName);
			if (scriptSchema)
			{
				bool owned = false;
				ScriptableEntity* preview = script->GetOrCreateEditorInstance(!scene->IsActive(), owned);
				if (preview)
				{
					y += DrawSchemaFields(ctx, base ^ 2u, { rect.X, rect.Y + y, rect.W, 0 }, preview,
						scriptSchema->DisplayName, *scriptSchema, visibleRect);
					for (const Schema::FieldSchema& field : scriptSchema->Fields)
						if (field.Get)
							script->FieldValues[field.Name] = field.Get(preview);
					script->ReleaseEditorInstance(preview);
				}
			}
			return y;
		}

		if (schema.Id.Name == "World::LuaScriptComponent")
		{
			auto* script = static_cast<LuaScriptComponent*>(instance);
			const std::string before = script->ScriptFilePath;
			TextField(ctx, base ^ 1u, { rect.X, rect.Y, rect.W, 22 }, script->ScriptFilePath, theme);
			if (script->ScriptFilePath != before)
				m_Host.MarkDocumentDirty();

			// P2 W5b:状态 + 重载诊断 + 脚本错误 + Reload 按钮(稳定 id → 进无障碍树,
			// 可被 AI 通道 ui.invoke 无鼠标驱动)。
			const uint32_t handle = static_cast<uint32_t>(static_cast<entt::entity>(entity));
			float y = 26;
			std::string state = "state: " + std::string(ScriptStateName(script->State));
			state += script->IsLoaded ? " (loaded)" : " (not loaded)";
			Label(ctx, { rect.X, rect.Y + y }, state, theme.TextMuted, 13.0f);
			y += 18;
			if (!script->ReloadDiagnostic.empty())
			{
				Label(ctx, { rect.X, rect.Y + y }, "[reload] " + TruncateForPanel(script->ReloadDiagnostic),
					{ 1.0f, 0.75f, 0.3f, 1 }, 12.0f);
				y += 16;
			}
			if (!script->LastError.empty())
			{
				Label(ctx, { rect.X, rect.Y + y }, "[error] " + TruncateForPanel(script->LastError),
					{ 1, 0.4f, 0.4f, 1 }, 12.0f);
				y += 16;
			}
			if (Button(ctx, Wui::HashId("lua.reload"), { rect.X, rect.Y + y, std::min(rect.W, 160.0f), 22 },
				"Reload Script", theme))
			{
				std::string message;
				const bool ok = EditorLayer::ReloadLuaScriptComponent(*script, entity.GetScene(), &message);
				m_LuaReloadOk = ok;
				m_LuaReloadMessage = ok ? message : ("failed: " + message);
				m_LuaReloadHandle = handle;
				WLD_CORE_INFO("[hot-reload] properties button (handle={0}, path='{1}'): {2}",
					handle, script->ScriptFilePath, m_LuaReloadMessage);
			}
			y += 26;
			if (m_LuaReloadHandle == handle && !m_LuaReloadMessage.empty())
			{
				Label(ctx, { rect.X, rect.Y + y }, TruncateForPanel(m_LuaReloadMessage),
					m_LuaReloadOk ? theme.TextMuted : Wui::WuiColor { 1, 0.4f, 0.4f, 1 }, 12.0f);
				y += 16;
			}
			return y + 4;
		}

		return DrawSchemaFields(ctx, base, rect, instance, schema.DisplayName, schema, visibleRect);
	}

	float PropertiesPanel::DrawTransformInspector(Wui::WuiContext& ctx, const Wui::WuiRect& rect,
		TransformComponent& transform, const Schema::TypeSchema& schema, const Wui::WuiRect& visibleRect)
	{
		const Wui::WuiTheme& theme = m_Host.Theme();
		const std::string& typeName = schema.DisplayName;
		const bool writable = !m_ReadOnly;

		if (!writable)
		{
			// Play/Simulate:只读展示(与通用只读字段同一契约:properties.<...> 文本节点)。
			DrawReadOnlyRow(ctx, PropPath(typeName, "Location"), ComponentRect(rect, 0, 20),
				"Location", FormatFloatText(transform.Location.x, 2) + ", "
					+ FormatFloatText(transform.Location.y, 2) + ", " + FormatFloatText(transform.Location.z, 2), theme);
			const glm::vec3 degrees = glm::degrees(transform.Rotation);
			DrawReadOnlyRow(ctx, PropPath(typeName, "Rotation"), ComponentRect(rect, 1, 20),
				"Rotation", FormatFloatText(degrees.x, 2) + ", " + FormatFloatText(degrees.y, 2) + ", "
					+ FormatFloatText(degrees.z, 2), theme);
			DrawReadOnlyRow(ctx, PropPath(typeName, "Scale"), ComponentRect(rect, 2, 20),
				"Scale", FormatFloatText(transform.Scale.x, 2) + ", " + FormatFloatText(transform.Scale.y, 2) + ", "
					+ FormatFloatText(transform.Scale.z, 2), theme);
			return 66.0f;
		}

		bool changed = false;
		// Rotation 面板按度数显示;写回统一走 SetTransform(同步 RotationQuat 与矩阵)。
		glm::vec3 rotationDegrees = glm::degrees(transform.Rotation);
		const auto reachable = [&visibleRect](const Wui::WuiRect& control)
		{
			return control.X + control.W * 0.5f >= visibleRect.X
				&& control.X + control.W * 0.5f <= visibleRect.X + visibleRect.W
				&& control.Y + control.H * 0.5f >= visibleRect.Y
				&& control.Y + control.H * 0.5f <= visibleRect.Y + visibleRect.H;
		};
		const Wui::WuiRect locationRow = ComponentRect(rect, 0, 20);
		const Wui::WuiRect rotationRow = ComponentRect(rect, 1, 20);
		const Wui::WuiRect scaleRow = ComponentRect(rect, 2, 20);
		changed |= DrawVec3Row(ctx, PropPath(typeName, "Location"), locationRow, "Location", transform.Location, theme, reachable(locationRow));
		changed |= DrawVec3Row(ctx, PropPath(typeName, "Rotation"), rotationRow, "Rotation", rotationDegrees, theme, reachable(rotationRow));
		changed |= DrawVec3Row(ctx, PropPath(typeName, "Scale"), scaleRow, "Scale", transform.Scale, theme, reachable(scaleRow));
		if (changed)
		{
			transform.SetTransform(transform.Location, glm::radians(rotationDegrees), transform.Scale);
			m_Host.MarkDocumentDirty();
		}
		return 66.0f;
	}

	float PropertiesPanel::DrawCameraInspector(Wui::WuiContext& ctx, const Wui::WuiRect& rect, void* instance,
		const Schema::TypeSchema& schema, const Wui::WuiRect& visibleRect)
	{
		const Wui::WuiTheme& theme = m_Host.Theme();
		// CameraComponent 的 schema 字段:Primary / FixedAspectRatio / Camera(Object Of SceneCamera)。
		const Schema::FieldSchema* primaryField = nullptr;
		const Schema::FieldSchema* fixedField = nullptr;
		const Schema::FieldSchema* cameraField = nullptr;
		for (const Schema::FieldSchema& field : schema.Fields)
		{
			if (field.Name == "Primary") primaryField = &field;
			else if (field.Name == "FixedAspectRatio") fixedField = &field;
			else if (field.K == Schema::Kind::Object) cameraField = &field;
		}
		void* cameraInstance = cameraField && cameraField->GetPtr ? cameraField->GetPtr(instance) : nullptr;
		const Schema::TypeSchema* cameraSchema = cameraField && cameraField->GetNested ? cameraField->GetNested() : nullptr;
		if (!primaryField || !fixedField || !cameraInstance || !cameraSchema)
			return 0;
		auto* camera = static_cast<SceneCamera*>(cameraInstance);

		const std::string& typeName = schema.DisplayName;
		const std::string& cameraType = cameraSchema->DisplayName;
		const bool writable = !m_ReadOnly;
		const auto reachable = [&visibleRect](const Wui::WuiRect& control)
		{
			return control.X + control.W * 0.5f >= visibleRect.X
				&& control.X + control.W * 0.5f <= visibleRect.X + visibleRect.W
				&& control.Y + control.H * 0.5f >= visibleRect.Y
				&& control.Y + control.H * 0.5f <= visibleRect.Y + visibleRect.H;
		};
		float y = 0.0f;
		bool changed = false;

		const Wui::WuiRect primaryRow { rect.X, rect.Y + y, rect.W, kRowHeight };
		if (writable)
		{
			bool primary = std::get<bool>(primaryField->Get(instance));
			const bool before = primary;
			Label(ctx, { primaryRow.X + 4, primaryRow.Y + 3 }, "Primary", theme.TextMuted, 13.0f);
			Wui::Checkbox(ctx, Wui::HashId(PropPath(typeName, "Primary").c_str()),
				{ primaryRow.X + 140.0f, primaryRow.Y + 1, 20, 20 }, "", primary, theme);
			const Wui::WuiRect primaryBox { primaryRow.X + 140.0f, primaryRow.Y + 1, 20, 20 };
			RegisterNode(Wui::HashId(PropPath(typeName, "Primary").c_str()), "checkbox",
				primaryBox, "Primary", primary ? "true" : "false", reachable(primaryBox));
			if (primary != before)
			{
				primaryField->Set(instance, Schema::Value(primary));
				changed = true;
			}
		}
		else
		{
			DrawReadOnlyRow(ctx, PropPath(typeName, "Primary"), primaryRow, "Primary",
				std::get<bool>(primaryField->Get(instance)) ? "true" : "false", theme);
		}
		y += kRowHeight;

		const Wui::WuiRect fixedRow { rect.X, rect.Y + y, rect.W, kRowHeight };
		if (writable)
		{
			bool fixed = std::get<bool>(fixedField->Get(instance));
			const bool before = fixed;
			Label(ctx, { fixedRow.X + 4, fixedRow.Y + 3 }, "FixedAspectRatio", theme.TextMuted, 13.0f);
			Wui::Checkbox(ctx, Wui::HashId(PropPath(typeName, "FixedAspectRatio").c_str()),
				{ fixedRow.X + 140.0f, fixedRow.Y + 1, 20, 20 }, "", fixed, theme);
			const Wui::WuiRect fixedBox { fixedRow.X + 140.0f, fixedRow.Y + 1, 20, 20 };
			RegisterNode(Wui::HashId(PropPath(typeName, "FixedAspectRatio").c_str()), "checkbox",
				fixedBox, "FixedAspectRatio", fixed ? "true" : "false", reachable(fixedBox));
			if (fixed != before)
			{
				fixedField->Set(instance, Schema::Value(fixed));
				changed = true;
			}
		}
		else
		{
			DrawReadOnlyRow(ctx, PropPath(typeName, "FixedAspectRatio"), fixedRow, "FixedAspectRatio",
				std::get<bool>(fixedField->Get(instance)) ? "true" : "false", theme);
		}
		y += kRowHeight;

		const Schema::FieldSchema* projectionField = nullptr;
		for (const Schema::FieldSchema& field : cameraSchema->Fields)
			if (field.K == Schema::Kind::Enum)
			{
				projectionField = &field;
				break;
			}
		const std::string projectionId = PropPath(cameraType, "ProjectionType");
		const Wui::WuiRect projectionRow { rect.X, rect.Y + y, rect.W, kRowHeight };
		SceneCamera::ProjectionType projection = camera->GetProjectionType();
		if (projectionField && projectionField->GetEnum && projectionField->Set && writable)
		{
			const Schema::EnumSchema* enumSchema = projectionField->GetEnum();
			const int64_t raw = enumSchema ? EnumRawOf(*enumSchema, projectionField->Get(cameraInstance)) : 0;
			std::vector<std::string> names;
			int selected = 0;
			if (enumSchema)
			{
				for (size_t i = 0; i < enumSchema->Values.size(); ++i)
				{
					names.push_back(enumSchema->Values[i].first);
					if (enumSchema->Values[i].second == raw)
						selected = static_cast<int>(i);
				}
			}
			Label(ctx, { projectionRow.X + 4, projectionRow.Y + 3 }, "Projection", theme.TextMuted, 13.0f);
			const Wui::WuiRect comboRect { projectionRow.X + 140.0f, projectionRow.Y + 1, projectionRow.W - 144.0f, 20 };
			if (Wui::Combo(ctx, Wui::HashId(projectionId.c_str()), comboRect, "", names, selected, theme))
			{
				projectionField->Set(cameraInstance, Schema::Value(enumSchema->Values[selected].second));
				projection = camera->GetProjectionType();
				changed = true;
			}
			RegisterNode(Wui::HashId(projectionId.c_str()), "combo", comboRect, "Projection",
				(selected >= 0 && selected < static_cast<int>(names.size())) ? names[selected] : std::string(),
				reachable(comboRect));
		}
		else
		{
			DrawReadOnlyRow(ctx, projectionId, projectionRow, "Projection",
				projection == SceneCamera::ProjectionType::Perspective ? "Perspective" : "Orthographic", theme);
		}
		y += kRowHeight;

		// 按当前投影类型只显示对应参数(迁移前语义),全部经 SceneCamera setter 提交。
		if (projection == SceneCamera::ProjectionType::Perspective)
		{
			float fov = camera->GetPerspectiveFOV();
			const Wui::WuiRect fovRow { rect.X, rect.Y + y, rect.W, kRowHeight };
			const bool fovChanged = DrawFloatRow(ctx, PropPath(cameraType, "Perspective.FOV"),
				fovRow, "FOV", fov, 1.0f, -1.0f, theme, reachable(fovRow));
			y += kRowHeight;
			float nearClip = camera->GetPerspectiveNearClip();
			const Wui::WuiRect nearRow { rect.X, rect.Y + y, rect.W, kRowHeight };
			const bool nearChanged = DrawFloatRow(ctx, PropPath(cameraType, "Perspective.NearClip"),
				nearRow, "NearClip", nearClip, 1.0f, -1.0f, theme, reachable(nearRow));
			y += kRowHeight;
			float farClip = camera->GetPerspectiveFarClip();
			const Wui::WuiRect farRow { rect.X, rect.Y + y, rect.W, kRowHeight };
			const bool farChanged = DrawFloatRow(ctx, PropPath(cameraType, "Perspective.FarClip"),
				farRow, "FarClip", farClip, 1.0f, -1.0f, theme, reachable(farRow));
			y += kRowHeight;
			if (fovChanged)
			{
				camera->SetPerspectiveFOV(fov);
				changed = true;
			}
			if (nearChanged)
			{
				camera->SetPerspectiveNearClip(nearClip);
				changed = true;
			}
			if (farChanged)
			{
				camera->SetPerspectiveFarClip(farClip);
				changed = true;
			}
		}
		else
		{
			float zoom = camera->GetOrthographicZoom();
			const Wui::WuiRect zoomRow { rect.X, rect.Y + y, rect.W, kRowHeight };
			const bool zoomChanged = DrawFloatRow(ctx, PropPath(cameraType, "Orthographic.Zoom"),
				zoomRow, "Zoom", zoom, 1.0f, -1.0f, theme, reachable(zoomRow));
			y += kRowHeight;
			float nearClip = camera->GetOrthographicNearClip();
			const Wui::WuiRect nearRow { rect.X, rect.Y + y, rect.W, kRowHeight };
			const bool nearChanged = DrawFloatRow(ctx, PropPath(cameraType, "Orthographic.NearClip"),
				nearRow, "NearClip", nearClip, 1.0f, -1.0f, theme, reachable(nearRow));
			y += kRowHeight;
			float farClip = camera->GetOrthographicFarClip();
			const Wui::WuiRect farRow { rect.X, rect.Y + y, rect.W, kRowHeight };
			const bool farChanged = DrawFloatRow(ctx, PropPath(cameraType, "Orthographic.FarClip"),
				farRow, "FarClip", farClip, 1.0f, -1.0f, theme, reachable(farRow));
			y += kRowHeight;
			if (zoomChanged)
			{
				camera->SetOrthographicZoom(zoom);
				changed = true;
			}
			if (nearChanged)
			{
				camera->SetOrthographicNearClip(nearClip);
				changed = true;
			}
			if (farChanged)
			{
				camera->SetOrthographicFarClip(farClip);
				changed = true;
			}
		}

		if (changed)
			m_Host.MarkDocumentDirty();
		return y + 2.0f;
	}
}
