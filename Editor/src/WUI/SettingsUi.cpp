#include "wldpch.h"
#include "SettingsUi.h"

#include "World/WUI/WuiAccessibility.h"
#include "World/WUI/WuiLocalization.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>

namespace World::Editor
{
	namespace
	{
		using Settings::SettingApply;
		using Settings::SettingDescriptor;
		using Settings::SettingsRegistry;
		using Settings::SettingScope;
		using Settings::SettingType;

		// 排版(设计单位):标签列宽 290、控件列 180、复位按钮贴控件右侧。
		// 与 PreferencesPanel/SettingsPanel 既有列宽保持同一基准。
		constexpr float kLabelColumn = 290.0f;
		constexpr float kControlWidth = 180.0f;
		constexpr float kRowHeight = 30.0f;
		constexpr float kGroupHeaderHeight = 24.0f;
		constexpr float kToolbarHeight = 32.0f;
		constexpr float kResetWidth = 52.0f;

		std::string SettingKey(const SettingDescriptor& descriptor)
		{
			return "settings." + descriptor.Id;
		}

		// 控件/无障碍节点 id:默认 "settings.<Id>";迁移过来的行可以给 AccessId 顶掉整串,
		// 保住既有脚本按老 id(`settings3d.msaa` 之类)读写的能力。
		Wui::WuiId SettingAccessId(const SettingDescriptor& descriptor)
		{
			if (!descriptor.AccessId.empty())
				return Wui::HashId(descriptor.AccessId.c_str());
			return Wui::HashId(("settings." + descriptor.Id).c_str());
		}

		// 存盘值 → 选项下标(找不到时回落到 0)。
		int OptionIndex(const SettingDescriptor& descriptor, const std::string& value)
		{
			for (size_t i = 0; i < descriptor.Options.size(); ++i)
				if (descriptor.Options[i].Value == value)
					return static_cast<int>(i);
			return 0;
		}

		const char* AccessKind(SettingType type)
		{
			switch (type)
			{
				case SettingType::Bool: return "checkbox";
				case SettingType::Int:
				case SettingType::Float: return "drag-number";
				case SettingType::Enum: return "combo";
				case SettingType::Path:
				case SettingType::Text:
				default: return "text-field";
			}
		}

		// 术语对照的无障碍写法:中文界面下 "中文 (English)"(脚本中英都能检索)。
		std::string AccessLabel(const Wui::LocalizedLabel& label)
		{
			if (label.Term.empty())
				return label.Text;
			return label.Text + " (" + label.Term + ")";
		}

		std::string ApplyBadgeText(SettingApply apply)
		{
			switch (apply)
			{
				case SettingApply::Restart: return Wui::Tr("settings.apply.restart", "Restart");
				case SettingApply::NextPlay: return Wui::Tr("settings.apply.next_play", "Next Play");
				case SettingApply::Immediate:
				default: return {};
			}
		}

		// 一行设置:标签 + 控件 + 单位 + 复位 + 生效徽标 + tooltip + 无障碍节点。
		// 返回 true = 本帧改动了值(changed 输出参数里带出错信息)。
		bool DrawSettingRow(Wui::WuiContext& ctx, const Wui::WuiTheme& theme, const SettingDescriptor& descriptor,
			float x, float y, float width, std::string* error)
		{
			const std::string key = SettingKey(descriptor);
			const Wui::WuiId id = SettingAccessId(descriptor);
			const Wui::LocalizedLabel label = Wui::TrLabel(key, descriptor.Label);
			std::string tooltip = Wui::Tr(key + ".tooltip", descriptor.Tooltip);
			const std::string current = descriptor.Read ? descriptor.Read() : std::string();
			const bool isDefault = !descriptor.IsDefault || descriptor.IsDefault();
			const Wui::WuiRect controlRect { x + kLabelColumn, y, kControlWidth, 22.0f };
			const bool enabled = !descriptor.IsEnabled || descriptor.IsEnabled();
			if (!enabled && !descriptor.DisabledReason.empty())
				tooltip += "\n" + Wui::Tr(key + ".disabled", descriptor.DisabledReason);

			// 悬停说明:整行可触发(名称/用途/默认值/生效时机)。
			Wui::Tooltip(ctx, { x, y, width, kRowHeight }, tooltip);

			if (!enabled)
			{
				// 禁用行:标签与值都用禁用色,不给可点控件 —— 用户看到的是"为什么不能用"。
				Wui::LabelWithTerm(ctx, { x, y + 3.0f }, label.Text, label.Term, theme.TextDisabled, 13.0f, theme,
					kLabelColumn - 16.0f);
				Wui::Label(ctx, { controlRect.X, y + 4.0f }, current, theme.TextDisabled, 13.0f);
				Wui::WuiAccessNode disabledNode;
				disabledNode.Id = id;
				disabledNode.Window = Wui::WuiAccessibility::Get().CurrentWindow();
				disabledNode.Panel = Wui::WuiAccessibility::Get().CurrentPanel();
				disabledNode.Kind = AccessKind(descriptor.Type);
				disabledNode.Label = AccessLabel(label);
				disabledNode.Value = current;
				disabledNode.Rect = controlRect;
				disabledNode.Tooltip = tooltip;
				disabledNode.Enabled = false;
				disabledNode.Interactive = false;
				disabledNode.Visible = true;
				Wui::WuiAccessibility::Get().Register(disabledNode);
				return false;
			}

			bool changed = false;
			std::string newValue = current;
			switch (descriptor.Type)
			{
				case SettingType::Bool:
				{
					// 勾选框自己画标签(标签列左侧),与 Blender/Fluent 的勾选行排版一致。
					bool value = current == "true";
					const Wui::WuiRect boxRect { x, y + 3.0f, kLabelColumn - 8.0f, 20.0f };
					if (Wui::Checkbox(ctx, id, boxRect, label.Text, label.Term, value, theme))
					{
						newValue = value ? "true" : "false";
						changed = true;
					}
					break;
				}
				case SettingType::Enum:
				{
					Wui::LabelWithTerm(ctx, { x, y + 3.0f }, label.Text, label.Term, theme.Text, 13.0f, theme,
						kLabelColumn - 16.0f);
					std::vector<std::string> options;
					options.reserve(descriptor.Options.size());
					for (const Settings::SettingOption& option : descriptor.Options)
						options.push_back(Wui::Tr(key + ".option." + option.Value, option.Label));
					int selected = OptionIndex(descriptor, current);
					const bool picked = descriptor.Searchable
						? Wui::SearchableCombo(ctx, id, controlRect, label.Text, options, selected, theme)
						: Wui::Combo(ctx, id, controlRect, label.Text, options, selected, theme);
					if (picked && selected >= 0 && selected < static_cast<int>(descriptor.Options.size()))
					{
						newValue = descriptor.Options[static_cast<size_t>(selected)].Value;
						changed = true;
					}
					break;
				}
				case SettingType::Int:
				{
					Wui::LabelWithTerm(ctx, { x, y + 3.0f }, label.Text, label.Term, theme.Text, 13.0f, theme,
						kLabelColumn - 16.0f);
					int64_t value = std::atoll(current.c_str());
					const int64_t minValue = static_cast<int64_t>(descriptor.Min);
					const int64_t maxValue = static_cast<int64_t>(descriptor.Max > descriptor.Min ? descriptor.Max : 2147483647.0);
					if (Wui::DragInt(ctx, id, controlRect, value, minValue, maxValue, theme))
					{
						newValue = std::to_string(value);
						changed = true;
					}
					break;
				}
				case SettingType::Float:
				{
					Wui::LabelWithTerm(ctx, { x, y + 3.0f }, label.Text, label.Term, theme.Text, 13.0f, theme,
						kLabelColumn - 16.0f);
					float value = static_cast<float>(std::atof(current.c_str()));
					const float speed = static_cast<float>(descriptor.Step > 0.0 ? descriptor.Step : 0.01);
					const float minValue = static_cast<float>(descriptor.Min);
					const float maxValue = static_cast<float>(descriptor.Max > descriptor.Min ? descriptor.Max : 1.0e9);
					if (Wui::DragFloat(ctx, id, controlRect, value, speed, minValue, maxValue, theme))
					{
						char buffer[32] = {};
						std::snprintf(buffer, sizeof(buffer), "%g", static_cast<double>(value));
						newValue = buffer;
						changed = true;
					}
					break;
				}
				case SettingType::Path:
				case SettingType::Text:
				default:
				{
					Wui::LabelWithTerm(ctx, { x, y + 3.0f }, label.Text, label.Term, theme.Text, 13.0f, theme,
						kLabelColumn - 16.0f);
					// 缓冲用**派生 id**:TextField 内部会拿控件 id 存自己的编辑态(WuiEditState),
					// 同一个 id 再存 std::string 会类型冲突(实测:日志刷 "persisted state id reused
					// with different types",退出时 0xC0000005)。
					std::string& buffer = ctx.Persist<std::string>(
						Wui::HashId((key + ".buffer").c_str()), current);
					if (Wui::TextField(ctx, id, controlRect, buffer, theme))
					{
						newValue = buffer;
						changed = true;
					}
					break;
				}
			}

			if (changed)
			{
				std::string writeError;
				if (!SettingsRegistry::Get().Set(descriptor.Id, newValue, &writeError))
				{
					if (error) *error = writeError;
					changed = false;
				}
			}

			// 单位(控件右侧)。
			if (!descriptor.Unit.empty())
				Wui::Label(ctx, { controlRect.X + controlRect.W + 6.0f, y + 4.0f }, descriptor.Unit, theme.TextMuted, 12.0f);

			// 复位:只在偏离默认值时出现(与"只看已修改"同一判据)。
			if (!isDefault && descriptor.Reset)
			{
				const Wui::WuiRect resetRect { controlRect.X + controlRect.W + 26.0f, y + 1.0f, kResetWidth, 20.0f };
				if (Wui::Button(ctx, Wui::HashId((key + ".reset").c_str()), resetRect,
					Wui::Tr("settings.reset_row", "Reset"), theme))
				{
					std::string resetError;
					if (!SettingsRegistry::Get().Reset(descriptor.Id, &resetError) && error)
						*error = resetError;
					changed = true;
				}
			}

			// 生效时机徽标(黄色,右对齐):立即生效不打扰用户。
			const std::string badge = ApplyBadgeText(descriptor.Apply);
			if (!badge.empty())
			{
				const float badgeWidth = ctx.MeasureTextWidth(badge, 11.0f);
				Wui::Label(ctx, { x + width - badgeWidth, y + 5.0f }, badge, theme.Warning, 11.0f);
			}

			// 无障碍节点:在控件自己登记之后**覆盖**同 id 节点,补上设置名/tooltip/术语写法
			// (控件自带的节点不含描述符信息,脚本按 "settings.<id>" 找不到语义)。
			// 但 value 要用**控件自己登记的**那份:文本行的"正在编辑的缓冲"只有控件知道,
			// 用描述符的已提交值会把实时输入藏起来(实测:ui.type 后树里看不到变化)。
			const Wui::WuiAccessNode* live = Wui::WuiAccessibility::Get().Find(id);
			Wui::WuiAccessNode node;
			node.Id = id;
			node.Window = Wui::WuiAccessibility::Get().CurrentWindow();
			node.Panel = Wui::WuiAccessibility::Get().CurrentPanel();
			// 可搜索下拉的触发器 kind 是 "search-combo"(控件自己登记的),这里的覆盖必须尊重它,
			// 否则脚本按 kind 找控件会落空(实测被这一行覆盖回 "combo")。
			node.Kind = (descriptor.Type == SettingType::Enum && descriptor.Searchable)
				? "search-combo" : AccessKind(descriptor.Type);
			node.Label = AccessLabel(label);
			node.Value = live ? live->Value : current;
			node.Rect = descriptor.Type == SettingType::Bool
				? Wui::WuiRect { x, y + 3.0f, kLabelColumn - 8.0f, 20.0f }
				: controlRect;
			node.Tooltip = tooltip;
			node.Enabled = true;
			node.Interactive = true;
			node.Focused = ctx.Focus() == id;
			node.Visible = true;
			Wui::WuiAccessibility::Get().Register(node);
			return changed;
		}
	}

	bool DrawSettingsPage(Wui::WuiContext& ctx, const Wui::WuiRect& rect, const Wui::WuiTheme& theme,
		SettingScope scope, const std::vector<std::string>& groups, SettingsPageState& state)
	{
		SettingsRegistry& registry = SettingsRegistry::Get();
		bool changed = false;
		std::string error;

		// ---- 工具栏:搜索 + 只看已修改 + 恢复默认 ----
		const float toolbarY = rect.Y;
		if (Wui::TextField(ctx, Wui::HashId("settings.search"), { rect.X, toolbarY, 220.0f, 24.0f },
			state.Search, theme))
			state.ScrollY = 0.0f;
		if (state.Search.empty())
		{
			// 占位提示(TextField 没有 placeholder 参数;空的时候画一行灰字)。
			Wui::Label(ctx, { rect.X + 8.0f, toolbarY + 5.0f },
				Wui::Tr("settings.search.hint", "Search settings…"), theme.TextDisabled, 12.0f);
		}
		{
			bool onlyModified = state.OnlyModified;
			const Wui::LocalizedLabel onlyModifiedLabel = Wui::TrLabel("settings.only_modified", "Only Modified");
			if (Wui::Checkbox(ctx, Wui::HashId("settings.only_modified"), { rect.X + 232.0f, toolbarY + 2.0f, 190.0f, 20.0f },
				onlyModifiedLabel.Text, onlyModifiedLabel.Term, onlyModified, theme))
				state.OnlyModified = onlyModified;
		}
		{
			const float resetWidth = 160.0f;
			const Wui::WuiRect resetRect { rect.X + rect.W - resetWidth, toolbarY, resetWidth, 24.0f };
			const bool hasModified = registry.HasModified(scope);
			if (Wui::Button(ctx, Wui::HashId("settings.reset_scope"), resetRect,
				Wui::Tr("settings.reset", "Restore Defaults"), theme) && hasModified)
			{
				if (!ResetSettingsScope(scope, &error))
					Wui::Label(ctx, { rect.X, rect.Y + rect.H - 16.0f }, error, theme.Danger, 12.0f);
			}
		}

		// ---- 行集合(搜索/只看已修改) ----
		const std::vector<const SettingDescriptor*> filtered = registry.Search(scope, state.Search, state.OnlyModified);
		std::vector<const SettingDescriptor*> rows;
		for (const std::string& group : groups)
			for (const SettingDescriptor* descriptor : filtered)
				if (descriptor->Group == group)
					rows.push_back(descriptor);

		const Wui::WuiRect content { rect.X, rect.Y + kToolbarHeight, rect.W, rect.H - kToolbarHeight };
		float contentHeight = 8.0f;
		for (const std::string& group : groups)
		{
			bool hasAny = false;
			for (const SettingDescriptor* descriptor : rows)
				if (descriptor->Group == group)
					hasAny = true;
			if (!hasAny)
				continue;
			const size_t count = static_cast<size_t>(std::count_if(rows.begin(), rows.end(),
				[&group](const SettingDescriptor* descriptor) { return descriptor->Group == group; }));
			contentHeight += kGroupHeaderHeight + kRowHeight * static_cast<float>(count) + 6.0f;
		}

		Wui::BeginScrollArea(ctx, content, contentHeight, state.ScrollY, theme);
		float y = content.Y + 4.0f - state.ScrollY;
		for (const std::string& group : groups)
		{
			std::vector<const SettingDescriptor*> groupRows;
			for (const SettingDescriptor* descriptor : rows)
				if (descriptor->Group == group)
					groupRows.push_back(descriptor);
			if (groupRows.empty())
				continue;
			const Wui::LocalizedLabel header = Wui::TrLabel("settings.group." + group, group);
			Wui::LabelWithTerm(ctx, { rect.X, y }, header.Text, header.Term, theme.TextMuted, 12.0f, theme,
				kLabelColumn);
			y += kGroupHeaderHeight;
			for (const SettingDescriptor* descriptor : groupRows)
			{
				if (DrawSettingRow(ctx, theme, *descriptor, rect.X, y, rect.W, &error))
					changed = true;
				y += kRowHeight;
			}
			y += 6.0f;
		}
		Wui::EndScrollArea(ctx);

		// ---- 空状态 ----
		if (rows.empty())
		{
			const std::string message = state.Search.empty() && !state.OnlyModified
				? Wui::Tr("settings.empty", "No settings in this category yet.")
				: Wui::Tr("settings.empty.filtered", "No settings match the current filter.");
			const float textWidth = ctx.MeasureTextWidth(message, 13.0f);
			Wui::Label(ctx, { content.X + (content.W - textWidth) * 0.5f, content.Y + 24.0f },
				message, theme.TextMuted, 13.0f);
		}
		return changed;
	}

	bool SettingsScopeModified(SettingScope scope)
	{
		return SettingsRegistry::Get().HasModified(scope);
	}

	bool ResetSettingsScope(SettingScope scope, std::string* error)
	{
		bool anyReset = false;
		for (const SettingDescriptor* descriptor : SettingsRegistry::Get().OfScope(scope))
		{
			if (!descriptor->Reset)
				continue;
			if (descriptor->IsDefault && descriptor->IsDefault())
				continue;
			std::string resetError;
			if (!SettingsRegistry::Get().Reset(descriptor->Id, &resetError))
			{
				if (error) *error = resetError;
				continue;
			}
			anyReset = true;
		}
		return anyReset;
	}
}
