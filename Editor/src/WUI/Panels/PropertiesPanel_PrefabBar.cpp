#include "PropertiesPanel_Internal.h"

namespace World
{

using namespace PropertiesPanelDetail;


	// ---- P4-U13b:prefab 实例条 + 破坏性动作确认 ----
void PropertiesPanel::RegisterPrefabOverrides(Entity entity, const std::vector<std::string>& fields){
		if (m_ReadOnly || fields.empty() || !entity.IsValid())
			return;
		Scene* scene = entity.GetScene();
		if (!scene)
			return;

		// 归属:实体自己是实例根,或沿父链找到实例根(实例子树内的成员被编辑同样算这棵实例的覆盖)。
		constexpr int kMaxAncestorDepth = 64;
		entt::entity current = static_cast<entt::entity>(entity);
		Gameplay::PrefabInstanceRecord* record = nullptr;
		for (int depth = 0; depth < kMaxAncestorDepth && current != entt::null; ++depth)
		{
			record = scene->FindPrefabInstance(current);
			if (record)
				break;
			// 父链只走 const 注册表:Play/Simulate 下活动场景的非 const GetRegistry() 会触发断言。
			const entt::registry& registry = static_cast<const Scene*>(scene)->GetRegistry();
			if (!registry.valid(current))
				break;
			const auto* hierarchy = registry.try_get<HierarchyComponent>(current);
			if (!hierarchy || hierarchy->Parent == entt::null || !registry.valid(hierarchy->Parent))
				break;
			current = hierarchy->Parent;
		}
		if (!record)
			return;   // 普通实体(不属于任何实例):编辑不产生覆盖记录

		const size_t countBefore = Gameplay::GetOverrideCount(*record);
		std::string joined;
		for (const std::string& field : fields)
		{
			Gameplay::MarkOverride(*record, static_cast<entt::entity>(entity), field);
			if (!joined.empty())
				joined += ", ";
			joined += field;
		}
		// 只在覆盖集合真的长大时记一条日志:拖动数值控件会每帧改值,否则日志会被刷屏。
		if (Gameplay::GetOverrideCount(*record) != countBefore)
			WLD_CORE_INFO("[prefab] override registered on '{0}' (handle={1}): {2}",
				record->PrefabPath, static_cast<uint32_t>(static_cast<entt::entity>(entity)), joined);
	}


PropertiesPanel::InstanceBarInfo PropertiesPanel::ResolveInstanceBar(PanelHost& host, Entity entity){
		InstanceBarInfo info;
		if (!host.PrefabInstanceInfo(entity, &info.Source, &info.Overrides, &info.Root))
			return info;   // 不属于任何实例 → 不画实例条
		info.InInstance = true;
		Scene* scene = entity.GetScene();
		Gameplay::PrefabInstanceRecord* record = scene && info.Root.IsValid()
			? scene->FindPrefabInstance(static_cast<entt::entity>(info.Root)) : nullptr;
		if (!record)
			return info;
		// 来源资产不在盘上 = 回滚/应用都做不到;实例条要给可读提示而不是点了才失败。
		info.SourceMissing = !PrefabSourceExists(record->PrefabPath);
		info.CanRevert = !info.SourceMissing && Gameplay::CanRevert(*record, *scene);
		info.CanApply = !info.SourceMissing && !record->PrefabPath.empty();
		return info;
	}


PropertiesPanel::InstanceBarLayout PropertiesPanel::LayoutInstanceBar(Wui::WuiContext& ctx, const Wui::WuiRect& rect, const InstanceBarInfo& info) const{
		constexpr float pad = 8.0f;
		constexpr float titleHeight = 22.0f;
		constexpr float buttonHeight = 22.0f;
		constexpr float rowGap = 6.0f;
		constexpr float hintHeight = 16.0f;

		InstanceBarLayout layout;
		layout.HasHint = m_ReadOnly || info.SourceMissing;
		const float innerWidth = std::max(40.0f, rect.W - pad * 2.0f);
		const std::string labels[3] = {
			Wui::Tr("panel.properties.prefab.revert", "Revert to Asset"),
			Wui::Tr("panel.properties.prefab.apply", "Apply to Asset"),
			Wui::Tr("panel.properties.prefab.unpack", "Unpack"),
		};
		float buttonWidth[3] = { 0.0f, 0.0f, 0.0f };
		for (int i = 0; i < 3; ++i)
			buttonWidth[i] = ctx.MeasureTextWidth(labels[i], 13.0f) + 20.0f;
		// 先排按钮(窄面板放不下就换行),行数决定实例条高度 —— 不用省略号牺牲按钮语义。
		float buttonY = rect.Y + pad + titleHeight + rowGap;
		const float buttonTop = buttonY;
		float buttonX = rect.X + pad;
		for (int i = 0; i < 3; ++i)
		{
			const float width = std::min(buttonWidth[i], innerWidth);
			if (i > 0 && buttonX + width > rect.X + pad + innerWidth)
			{
				buttonY += buttonHeight + rowGap;
				buttonX = rect.X + pad;
			}
			layout.Buttons[i] = { buttonX, buttonY, width, buttonHeight };
			buttonX += width + rowGap;
		}
		const float buttonRowsHeight = (buttonY - buttonTop) + buttonHeight;
		layout.Height = pad + titleHeight + rowGap + buttonRowsHeight
			+ (layout.HasHint ? rowGap * 0.5f + hintHeight : 0.0f) + pad;
		layout.Hint = { rect.X + pad, buttonY + buttonHeight + rowGap * 0.5f, innerWidth, hintHeight };
		return layout;
	}


void PropertiesPanel::DrawInstanceBar(Wui::WuiContext& ctx, PanelHost& host, const Wui::WuiRect& rect, const InstanceBarInfo& info, const InstanceBarLayout& layout){
		const Wui::WuiTheme& theme = m_Host.Theme();
		constexpr float pad = 8.0f;
		constexpr float titleHeight = 22.0f;

		const std::string sourceName = std::filesystem::path(info.Source).filename().string();
		const std::string fileName = sourceName.empty()
			? Wui::Tr("panel.properties.prefab.no_source", "(no source asset)") : sourceName;
		const std::string revertText = Wui::Tr("panel.properties.prefab.revert", "Revert to Asset");
		const std::string applyText = Wui::Tr("panel.properties.prefab.apply", "Apply to Asset");
		const std::string unpackText = Wui::Tr("panel.properties.prefab.unpack", "Unpack");

		// 卡片底 + 左侧 accent 条:与 prefab 编辑横幅同一套"这是资产链接,不是普通组件"的表达。
		Wui::PanelBackground(ctx, rect, theme.PanelHeader, theme.Radius);
		Wui::PanelBackground(ctx, { rect.X, rect.Y, 3.0f, rect.H }, theme.Accent, 0.0f);
		Wui::HighlightOutline(ctx, rect, theme.Border, theme.Radius, 1.0f);
		RegisterNode(Wui::HashId("properties.prefab.bar"), "group",
			{ rect.X, rect.Y, rect.W, titleHeight + pad },
			Wui::Tr("panel.properties.prefab.bar", "Prefab instance"), info.Source,
			true, Wui::Tr("panel.properties.prefab.bar.tooltip",
				"This entity belongs to a prefab instance: edits are tracked as overrides"), false);

		// 标题行:[预] 文件名 · N 处覆盖
		const Wui::WuiRect badge { rect.X + pad, rect.Y + pad + 2.0f, 16.0f, 16.0f };
		// W3.5:圆形强调色底 + 粗体字形 = 库件 P1c-LIB1 的 Wui::Badge(本面板最后一条即时绘制)。
		// 底色命令逐字段等价(PanelBackground(Accent, H*0.5) 与 Badge 的 radius 参数同形);
		// 字形改用库件的居中口径:水平 (16 − 字形宽)/2、垂直 (16 − 11)/2 = 2.5 —— 与原手写
		// 的 (X+3.0, Y+2.0) 有 0/0.5px 级的差,已按像素证据记录(见 W3.5 报告 §3)。
		Wui::Badge(ctx, badge, Wui::Tr("panel.properties.prefab.badge", "预"), theme.Accent,
			Wui::WuiColor { 1.0f, 1.0f, 1.0f, 1.0f }, theme, 11.0f, true, badge.H * 0.5f);
		const float nameX = badge.X + badge.W + 6.0f;
		Wui::Label(ctx, { nameX, rect.Y + pad + 3.0f }, fileName, theme.Text, 13.0f);
		const std::string countText = std::to_string(info.Overrides) + " "
			+ Wui::Tr("panel.properties.prefab.overrides", "override(s)");
		const float countX = nameX + ctx.MeasureTextWidth(fileName, 13.0f) + 8.0f;
		Wui::Label(ctx, { countX, rect.Y + pad + 4.0f }, countText, theme.TextMuted, 12.0f);
		// 覆盖计数是脚本/读屏要读的数字:单独一个稳定 id,value 就是纯数字。
		RegisterNode(Wui::HashId("properties.prefab.overrides"), "text",
			{ countX, rect.Y + pad + 2.0f,
				std::max(24.0f, ctx.MeasureTextWidth(countText, 12.0f)), 16.0f },
			Wui::Tr("panel.properties.prefab.overrides.label", "Overrides"),
			std::to_string(info.Overrides), true,
			Wui::Tr("panel.properties.prefab.overrides.tooltip",
				"Fields edited on this instance that differ from the prefab asset"), false);

		// 动作行:回滚(不需要确认)/ 应用到资产(确认)/ 断开链接(确认)。
		const bool readOnlyReason = m_ReadOnly;
		const bool canRevert = info.CanRevert && !readOnlyReason;
		const bool canApply = info.CanApply && !readOnlyReason;
		const bool canUnpack = !readOnlyReason;
		const std::string revertHint = canRevert
			? Wui::Tr("panel.properties.prefab.revert.tooltip",
				"Discard overrides and restore this subtree from the prefab asset")
			: (readOnlyReason
				? Wui::Tr("panel.properties.prefab.readonly", "Read-only while Play/Simulate is running")
				: Wui::Tr("panel.properties.prefab.revert.blocked",
					"Unavailable: the prefab asset could not be found"));
		const std::string applyHint = canApply
			? Wui::Tr("panel.properties.prefab.apply.tooltip",
				"Write this instance back to the prefab asset (asks for confirmation)")
			: (readOnlyReason
				? Wui::Tr("panel.properties.prefab.readonly", "Read-only while Play/Simulate is running")
				: Wui::Tr("panel.properties.prefab.apply.blocked",
					"Unavailable: the prefab asset could not be found"));
		const std::string unpackHint = canUnpack
			? Wui::Tr("panel.properties.prefab.unpack.tooltip",
				"Turn this subtree into plain entities (asks for confirmation); it stops following the asset")
			: Wui::Tr("panel.properties.prefab.readonly", "Read-only while Play/Simulate is running");

		if (Wui::ActionButton(ctx, Wui::HashId("properties.prefab.revert"), layout.Buttons[0], revertText, theme,
			canRevert, revertHint))
		{
			std::string message;
			if (!host.PrefabInstanceRevert(info.Root, &message) || !message.empty())
				host.Notify(message);
		}
		if (Wui::ActionButton(ctx, Wui::HashId("properties.prefab.apply"), layout.Buttons[1], applyText, theme,
			canApply, applyHint))
			OpenPrefabActionConfirm(ctx, PrefabAction::Apply, info.Root, info.Source);
		if (Wui::ActionButton(ctx, Wui::HashId("properties.prefab.unpack"), layout.Buttons[2], unpackText, theme,
			canUnpack, unpackHint))
			OpenPrefabActionConfirm(ctx, PrefabAction::Unpack, info.Root, info.Source);

		// 只读/来源缺失的可读提示(不是"按钮点了没反应")。
		if (layout.HasHint)
		{
			const std::string hint = m_ReadOnly
				? Wui::Tr("panel.properties.prefab.readonly", "Read-only while Play/Simulate is running")
				: Wui::Tr("panel.properties.prefab.missing", "Source asset not found: ") + info.Source;
			Wui::Label(ctx, { layout.Hint.X, layout.Hint.Y + 1.0f }, hint,
				m_ReadOnly ? theme.TextMuted : theme.Warning, 12.0f);
			RegisterNode(Wui::HashId("properties.prefab.hint"), "text", layout.Hint, hint, std::string(),
				false, hint, false);
		}
	}


void PropertiesPanel::OpenPrefabActionConfirm(Wui::WuiContext& ctx, PrefabAction action, Entity root, const std::string& source){
		m_PrefabActionPending = action;
		m_PrefabActionRoot = root;
		m_PrefabActionSource = source;
		ctx.SetModal(Wui::HashId("prop.prefab.action.modal"));
		// 面板级模态:宿主帧初封锁整窗输入,渲染本面板前解开(与"移除组件"同一条路径)。
		m_Host.SetPanelModalOwner(Id());
		ctx.RecordOp("properties", action == PrefabAction::Apply
			? "prefab-apply-ask" : "prefab-unpack-ask", source, std::string());
	}


void PropertiesPanel::ClosePrefabActionConfirm(Wui::WuiContext& ctx){
		m_PrefabActionPending = PrefabAction::None;
		m_PrefabActionRoot = Entity();
		m_PrefabActionSource.clear();
		ctx.ClearModal();
		m_Host.SetPanelModalOwner(std::string());
	}


void PropertiesPanel::DrawPrefabActionConfirm(Wui::WuiContext& ctx){
		const Wui::WuiTheme& theme = m_Host.Theme();
		const bool apply = m_PrefabActionPending == PrefabAction::Apply;
		Wui::ModalFrameDesc frameDesc;
		frameDesc.Id = Wui::HashId("prop.prefab.action.modal");
		frameDesc.Title = apply
			? Wui::Tr("panel.properties.prefab.apply.confirm_title", "Apply to Prefab Asset")
			: Wui::Tr("panel.properties.prefab.unpack.confirm_title", "Unpack (Break Prefab Link)");
		frameDesc.Size = { 470.0f, 180.0f };
		Wui::WuiRect frame;
		bool escapePressed = false;
		if (!Wui::BeginModalFrame(ctx, frameDesc, &frame, &escapePressed, theme))
			return;

		// 确认文案说清后果:会写回资产 / 之后不再跟随资产。逐行给(Wui::Label 不换行),
		// 破坏性操作的说明不能省略成省略号。
		const std::array<std::string, 3> bodyLines = apply
			? std::array<std::string, 3> {
				Wui::Tr("panel.properties.prefab.apply.confirm_body",
					"Write this instance back to the prefab asset?"),
				Wui::Tr("panel.properties.prefab.apply.confirm_body2",
					"The asset file will be overwritten, and its other instances"),
				Wui::Tr("panel.properties.prefab.apply.confirm_body3",
					"will follow the new values.") }
			: std::array<std::string, 3> {
				Wui::Tr("panel.properties.prefab.unpack.confirm_body",
					"Break the link to the prefab asset?"),
				Wui::Tr("panel.properties.prefab.unpack.confirm_body2",
					"These entities stay as they are now, but they"),
				Wui::Tr("panel.properties.prefab.unpack.confirm_body3",
					"stop following the asset (revert/apply go away).") };
		for (int line = 0; line < 3; ++line)
			Wui::Label(ctx, { frame.X + 16.0f, frame.Y + 50.0f + 18.0f * static_cast<float>(line) },
				bodyLines[line], theme.Text, 13.0f);
		Wui::LabelWithTerm(ctx, { frame.X + 16.0f, frame.Y + 108.0f },
			std::filesystem::path(m_PrefabActionSource).filename().string(), std::string(),
			theme.Warning, 13.0f, theme, frame.W - 32.0f);

		const Wui::ModalButtonDesc buttons[2] = {
			{ Wui::Tr("panel.properties.prefab.confirm_cancel", "Cancel"),
				Wui::HashId("prop.prefab.action.cancel"), true },
			{ apply ? Wui::Tr("panel.properties.prefab.apply.confirm", "Apply to Asset")
				: Wui::Tr("panel.properties.prefab.unpack.confirm", "Unpack"),
				Wui::HashId("prop.prefab.action.ok"), true },
		};
		const int clicked = Wui::ModalButtons(ctx, frame, buttons, 2, theme);
		bool closeRequested = false;
		if (clicked == 1)
		{
			std::string message;
			const bool ok = apply
				? m_Host.PrefabInstanceApply(m_PrefabActionRoot, &message)
				: m_Host.PrefabInstanceUnpack(m_PrefabActionRoot, &message);
			if (!message.empty())
				m_Host.Notify(message);
			if (ok)
				ctx.RecordOp("properties", apply ? "prefab-apply" : "prefab-unpack",
					m_PrefabActionSource, message);
			else
				WLD_CORE_WARN("Prefab {0} failed (properties bar): {1}", apply ? "apply" : "unpack", message);
			closeRequested = true;
		}
		else if (clicked == 0 || escapePressed)
			closeRequested = true;
		// 先收 overlay 再清模态态(BeginModalFrame/EndModalFrame 必须成对)。
		Wui::EndModalFrame(ctx);
		if (closeRequested && ctx.Modal() == frameDesc.Id)
			ClosePrefabActionConfirm(ctx);
	}

}
