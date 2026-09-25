#include "wldpch.h"
#include "World/WUI/WuiTexturePicker.h"

#include "World/WUI/WuiAccessibility.h"
#include "World/WUI/WuiContext.h"
#include "World/WUI/WuiLocalization.h"
#include "World/WUI/WuiWidgets.h"
#include "World/WUI/Widgets/WuiChrome.h"

#include <algorithm>
#include <cctype>
#include <memory>
#include <string>
#include <vector>

namespace World::Wui
{
	namespace
	{
		// 槽内布局:定位按钮恒定占 26 + 6 间隙(与编辑器侧既有的 26px 定位槽同口径);
		// 徽标条最多吃掉槽宽的 45%,放不下去的徽标**不画**(状态与完整 token 表仍在 a11y 上)。
		constexpr float kRevealWidth = 26.0f;
		constexpr float kSlotGap = 6.0f;
		constexpr float kBadgePadding = 10.0f;
		constexpr float kBadgeGap = 4.0f;
		constexpr float kMinComboWidth = 64.0f;
		constexpr size_t kMaxBadges = 3;

		// 徽标 token 表:token(调用方/资产侧给的机器串)→ 本地化文案 + 语义色。
		// 未知 token 按原样以中性色显示 —— 组件不猜、也不静默丢掉调用方的标记。
		enum class BadgeTone : uint8_t { Accent = 0, Success, Warning, Danger, Muted };

		struct BadgeSpec
		{
			const char* Token;
			const char* Key;
			const char* English;
			BadgeTone Tone;
		};

		const BadgeSpec kBadgeSpecs[] = {
			{ "asset", "wui.texture-picker.badge.asset", "Asset", BadgeTone::Accent },
			{ "source", "wui.texture-picker.badge.source", "Source image", BadgeTone::Accent },
			{ "container", "wui.texture-picker.badge.container", "Single file", BadgeTone::Accent },
			{ "baked", "wui.texture-picker.badge.baked", "Baked", BadgeTone::Success },
			{ "stale", "wui.texture-picker.badge.stale", "Needs rebake", BadgeTone::Warning },
			{ "needs-rebake", "wui.texture-picker.badge.stale", "Needs rebake", BadgeTone::Warning },
			{ "unbaked", "wui.texture-picker.badge.unbaked", "Not baked", BadgeTone::Muted },
			{ "legacy", "wui.texture-picker.badge.legacy", "Legacy settings", BadgeTone::Warning },
			{ "missing", "wui.texture-picker.badge.missing", "Missing", BadgeTone::Danger },
			{ "missing-source", "wui.texture-picker.badge.missing-source", "No source image", BadgeTone::Danger },
			{ "no-source", "wui.texture-picker.badge.missing-source", "No source image", BadgeTone::Danger },
			{ "unreadable", "wui.texture-picker.badge.unreadable", "Unreadable", BadgeTone::Danger },
		};

		std::string TrimmedLower(const std::string& text)
		{
			size_t begin = 0;
			size_t end = text.size();
			while (begin < end && std::isspace(static_cast<unsigned char>(text[begin])))
				++begin;
			while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1])))
				--end;
			std::string out = text.substr(begin, end - begin);
			for (char& ch : out)
				ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
			return out;
		}

		// 逗号/竖线/分号/空白分隔的 token 表(空 token 忽略;大小写不敏感)。
		std::vector<std::string> SplitTokens(const std::string& text)
		{
			std::vector<std::string> tokens;
			std::string current;
			const auto flush = [&]()
			{
				const std::string token = TrimmedLower(current);
				if (!token.empty())
					tokens.push_back(token);
				current.clear();
			};
			for (char ch : text)
			{
				const bool separator = ch == ',' || ch == '|' || ch == ';'
					|| std::isspace(static_cast<unsigned char>(ch)) != 0;
				if (separator)
					flush();
				else
					current.push_back(ch);
			}
			flush();
			return tokens;
		}

		const BadgeSpec* FindBadgeSpec(const std::string& token)
		{
			for (const BadgeSpec& spec : kBadgeSpecs)
				if (token == spec.Token)
					return &spec;
			return nullptr;
		}

		WuiColor ToneColor(const WuiTheme& theme, BadgeTone tone)
		{
			switch (tone)
			{
			case BadgeTone::Success: return theme.Success;
			case BadgeTone::Warning: return theme.Warning;
			case BadgeTone::Danger: return theme.Danger;
			case BadgeTone::Muted: return theme.TextMuted;
			case BadgeTone::Accent:
			default: return theme.Accent;
			}
		}

		const char* DefaultTokenForState(WuiTexturePickerState state)
		{
			switch (state)
			{
			case WuiTexturePickerState::Missing: return "missing";
			case WuiTexturePickerState::MissingSource: return "missing-source";
			case WuiTexturePickerState::Stale: return "stale";
			case WuiTexturePickerState::Unbaked: return "unbaked";
			case WuiTexturePickerState::Legacy: return "legacy";
			case WuiTexturePickerState::Container: return "container";
			case WuiTexturePickerState::Empty:
			case WuiTexturePickerState::Set:
			case WuiTexturePickerState::Auto:
			default:
				return nullptr;
			}
		}

		bool HasToken(const std::vector<std::string>& tokens, const char* token)
		{
			for (const std::string& candidate : tokens)
				if (candidate == token)
					return true;
			return false;
		}

		// 只读 / 不可挑选:把**这一次调用**的输入边沿掐掉 —— 控件仍走自己的绘制路径
		// (外观、命令流、a11y 节点都在),但打不开弹层、点不动、键盘也不激活。
		// 与登记表里的 PseudoState 同一套做法(改 ctx.Input() 再还原)。
		class ScopedInputMute
		{
		public:
			explicit ScopedInputMute(WuiContext& ctx) : m_Context(ctx), m_Saved(ctx.Input())
			{
				WuiInputState& input = ctx.Input();
				for (int button = 0; button < 3; ++button)
				{
					input.MouseDown[button] = false;
					input.MouseClicked[button] = false;
					input.MouseReleased[button] = false;
					input.MouseDoubleClicked[button] = false;
				}
				input.Wheel = 0.0f;
				input.KeyDown.clear();
				input.KeyPressed.clear();
				input.KeyRepeated.clear();
				input.TextInput.clear();
			}
			~ScopedInputMute() { m_Context.Input() = m_Saved; }

			ScopedInputMute(const ScopedInputMute&) = delete;
			ScopedInputMute& operator=(const ScopedInputMute&) = delete;

		private:
			WuiContext& m_Context;
			WuiInputState m_Saved;
		};

		// 只读外观:沿用组件库"用主题禁用令牌表达禁用"的口径(DisableTheme 同款,
		// 不叠半透明遮罩、不改几何)。
		WuiTheme ReadOnlyTheme(const WuiTheme& theme)
		{
			WuiTheme out = theme;
			out.Text = theme.TextDisabled;
			out.TextMuted = theme.TextDisabled;
			out.ButtonBg = theme.ContentBg;
			out.ButtonHover = theme.ContentBg;
			out.ActiveBg = theme.ContentBg;
			out.HoverBg = theme.ContentBg;
			out.Accent = theme.TextDisabled;
			out.FocusRing = theme.TextDisabled;
			out.Border = { theme.Border.R, theme.Border.G, theme.Border.B, 0.6f };
			return out;
		}

		// 当前值的显示名:Value(资产把导入源名写进括号里)。
		std::string DisplayValue(const std::string& value, const std::string& importSourceName)
		{
			if (value.empty() || importSourceName.empty())
				return value;
			return value + " (" + importSourceName + ")";
		}

	}

	const char* WuiTexturePickerStateToken(WuiTexturePickerState state)
	{
		switch (state)
		{
		case WuiTexturePickerState::Empty: return "empty";
		case WuiTexturePickerState::Set: return "set";
		case WuiTexturePickerState::Missing: return "missing";
		case WuiTexturePickerState::MissingSource: return "missing-source";
		case WuiTexturePickerState::Stale: return "stale";
		case WuiTexturePickerState::Unbaked: return "unbaked";
		case WuiTexturePickerState::Legacy: return "legacy";
		case WuiTexturePickerState::Container: return "container";
		case WuiTexturePickerState::Auto:
		default:
			return "auto";
		}
	}

	WuiTexturePickerState WuiTexturePickerDeriveState(const std::string& value, const std::string& badges)
	{
		if (value.empty())
			return WuiTexturePickerState::Empty;
		const std::vector<std::string> tokens = SplitTokens(badges);
		// 顺序 = 严重度:资产缺 > 源图缺 > 需重烘 > 未烘焙 > 旧式 > 单文件 > 正常。
		if (HasToken(tokens, "missing") || HasToken(tokens, "unreadable"))
			return WuiTexturePickerState::Missing;
		if (HasToken(tokens, "missing-source") || HasToken(tokens, "no-source"))
			return WuiTexturePickerState::MissingSource;
		if (HasToken(tokens, "stale") || HasToken(tokens, "needs-rebake"))
			return WuiTexturePickerState::Stale;
		if (HasToken(tokens, "unbaked"))
			return WuiTexturePickerState::Unbaked;
		if (HasToken(tokens, "legacy"))
			return WuiTexturePickerState::Legacy;
		if (HasToken(tokens, "container"))
			return WuiTexturePickerState::Container;
		return WuiTexturePickerState::Set;
	}

	std::string WuiTexturePickerBadgeText(const std::string& token)
	{
		const std::string normalized = TrimmedLower(token);
		const BadgeSpec* spec = FindBadgeSpec(normalized);
		if (spec == nullptr)
			return token;   // 未知标记:原样显示(中性色),不静默吞掉
		return Tr(spec->Key, spec->English);
	}

	bool WuiTexturePicker(WuiContext& ctx, WuiId id, const WuiRect& rect, std::string& value,
		const WuiTexturePickerOptions& options, const WuiTheme& theme)
	{
		const bool interactive = !options.ReadOnly;
		const bool pickable = interactive && options.AllowPick;
		const std::string label = options.Label.empty()
			? Tr("wui.texture-picker.label", "Texture") : options.Label;

		// ---- 当前值与候选清单 ----
		const std::string noneLabel = options.NoneLabel.empty()
			? Tr("wui.texture-picker.none", "(none)") : options.NoneLabel;
		std::vector<std::string> optionLabels;
		std::vector<const WuiTexturePickerEntry*> optionEntries;
		if (options.AllowClear)
			optionLabels.push_back(noneLabel);
		for (const WuiTexturePickerEntry& entry : options.Entries)
		{
			// 显示名 = 完整逻辑路径(资产条目把导入源名写在括号里)——搜索/读屏/回显读的都是它;
			// 越界只由 SearchableCombo 在**绘制期**中间省略(WUI-P7 口径:不许提前截断)
			optionLabels.push_back(entry.Label.empty() ? entry.Value : entry.Label);
			optionEntries.push_back(&entry);
		}
		const size_t firstEntryIndex = options.AllowClear ? 1u : 0u;

		// 值不在候选清单里(绝对路径 / 内容根外 / 刚被删):补一条兜底条目,否则下拉会回显 "(无)"
		// 而引用其实还在 —— 那是误导(既有面板同一条口径)。
		int fallbackIndex = -1;
		if (!value.empty())
		{
			bool found = false;
			for (size_t i = 0; i < optionEntries.size(); ++i)
			{
				if (optionEntries[i]->Value == value)
				{
					found = true;
					break;
				}
			}
			if (!found)
			{
				optionLabels.push_back(DisplayValue(value, options.ImportSourceName));
				fallbackIndex = static_cast<int>(optionLabels.size()) - 1;
			}
		}

		// 徽标 token:只认调用方给当前值的那些(options.Badges)—— 组件不替调用方决定
		// "当前值该显示什么徽标"(候选条目的 Badges 是给调用方转发用的,见头文件)。
		std::string badgeTokens = options.Badges;
		const WuiTexturePickerState state = options.State != WuiTexturePickerState::Auto
			? options.State
			: WuiTexturePickerDeriveState(value, badgeTokens);
		if (badgeTokens.empty())
			if (const char* token = DefaultTokenForState(state))
				badgeTokens = token;

		// ---- 槽内布局:列表 | 徽标条 | 定位按钮 ----
		const float badgeFontSize = std::min(16.0f, std::max(8.0f, options.BadgeFontSize));
		const float badgeFillAlpha = std::min(1.0f, std::max(0.0f, options.BadgeFillAlpha));
		const float badgeHeight = std::max(14.0f, badgeFontSize + 5.0f);
		const std::vector<std::string> tokens = SplitTokens(badgeTokens);
		struct Chip { std::string Text; float Width = 0.0f; const BadgeSpec* Spec = nullptr; };
		std::vector<Chip> chips;
		float chipsWidth = 0.0f;
		// 徽标条预算:给列表留至少 64px,放不下的徽标不画(a11y 上仍有完整 token 表)。
		const float boxReserve = options.AllowReveal ? kRevealWidth + kSlotGap : 0.0f;
		const float badgeBudget = std::max(0.0f, rect.W - boxReserve - kMinComboWidth - kSlotGap);
		for (const std::string& token : tokens)
		{
			if (chips.size() >= kMaxBadges)
				break;
			Chip chip;
			chip.Text = WuiTexturePickerBadgeText(token);
			if (chip.Text.empty())
				continue;
			chip.Spec = FindBadgeSpec(token);
			chip.Width = ctx.MeasureTextWidth(chip.Text, badgeFontSize) + kBadgePadding;
			const float needed = chipsWidth + (chips.empty() ? 0.0f : kBadgeGap) + chip.Width;
			if (needed > badgeBudget)
				break;   // 放不下的徽标不画(a11y 上仍有完整状态与提示)
			chipsWidth = needed;
			chips.push_back(std::move(chip));
		}

		const float revealReserve = options.AllowReveal ? kRevealWidth + kSlotGap : 0.0f;
		const float badgeReserve = chips.empty() ? 0.0f : chipsWidth + kSlotGap;
		const float comboWidth = std::max(24.0f, rect.W - revealReserve - badgeReserve);
		const WuiRect comboRect { rect.X, rect.Y, std::min(rect.W, comboWidth), rect.H };

		// ---- 选中下标:每帧由 Value 重算(候选清单/外部值变了立刻对齐,不缓存陈旧下标)----
		// SearchableCombo 只在**确认选择**那一帧写这个下标,而那一帧 value 也会被本件同步更新,
		// 所以"每帧重算"与"用户在弹层里的临时高亮"不冲突。
		int& selected = ctx.Persist<int>(id ^ 0x5A1Du, 0);
		{
			int index = -1;
			if (value.empty())
				index = options.AllowClear ? 0 : -1;
			else
				for (size_t i = 0; i < optionEntries.size(); ++i)
					if (optionEntries[i]->Value == value)
					{
						index = static_cast<int>(firstEntryIndex) + static_cast<int>(i);
						break;
					}
			if (index < 0 && !value.empty())
				index = fallbackIndex;   // 兜底条目 = 当前值;没有兜底条目 = 无选中项
			selected = index;
		}

		// 只读:把上一次可能开着的弹层收掉(否则会出现"只读但列表还开着")。
		if (options.ReadOnly && ctx.IsPopupOpen(id))
			ctx.ClosePopup(id);

		const WuiTheme& fieldTheme = options.ReadOnly ? ReadOnlyTheme(theme) : theme;
		bool changed = false;
		// 不可挑选(只读 / AllowPick=false):只掐这一次调用的输入边沿 —— 绘制路径与命令流不变,
		// 但弹层打不开、点不动、键盘不激活。只读态另有禁用色 + 节点 enabled=false。
		std::unique_ptr<ScopedInputMute> mute;
		if (!pickable)
			mute = std::make_unique<ScopedInputMute>(ctx);
		{
			if (SearchableCombo(ctx, id, comboRect, label, optionLabels, selected, fieldTheme))
			{
				std::string next = value;
				if (options.AllowClear && selected == 0)
					next.clear();
				else if (selected >= static_cast<int>(firstEntryIndex)
					&& selected - static_cast<int>(firstEntryIndex) < static_cast<int>(optionEntries.size()))
					next = optionEntries[static_cast<size_t>(selected - static_cast<int>(firstEntryIndex))]->Value;
				else if (selected == fallbackIndex)
					next = value;   // 兜底条目 = 当前值,不算变化
				if (next != value)
				{
					value = next;
					changed = true;
				}
			}
		}
		mute.reset();

		// 不可挑选 / 只读:覆盖控件自己登记的那条节点(同 id 后登记者胜)——
		// "点得动"这件事必须与用户看到的一致(只读不许被脚本点开)。
		if ((!pickable || options.ReadOnly) && WuiAccessibility::Get().Enabled())
		{
			WuiAccessNode node;
			node.Id = id;
			node.Window = WuiAccessibility::Get().CurrentWindow();
			node.Panel = WuiAccessibility::Get().CurrentPanel();
			node.Kind = "search-combo";
			node.Label = label;
			node.Value = selected >= 0 && selected < static_cast<int>(optionLabels.size())
				? optionLabels[static_cast<size_t>(selected)] : std::string();
			node.Tooltip = options.ReadOnly
				? Tr("wui.texture-picker.readonly", "Read-only: this reference cannot be changed here")
				: Tr("wui.texture-picker.no-pick", "This reference is shown as-is; picking is disabled");
			node.Rect = comboRect;
			node.Enabled = interactive;
			node.Interactive = false;
			node.Focused = ctx.Focus() == id;
			node.Visible = true;
			WuiAccessibility::Get().Register(node);
		}

		// ---- 徽标条 ----
		float chipCursor = comboRect.X + comboRect.W + (chips.empty() ? 0.0f : kSlotGap);
		for (const Chip& chip : chips)
		{
			const WuiRect chipRect { chipCursor, rect.Y + (rect.H - badgeHeight) * 0.5f,
				chip.Width, badgeHeight };
			const WuiColor tone = chip.Spec != nullptr ? ToneColor(fieldTheme, chip.Spec->Tone)
				: fieldTheme.TextMuted;
			Badge(ctx, chipRect, chip.Text, WuiColor { tone.R, tone.G, tone.B, badgeFillAlpha }, tone,
				fieldTheme, badgeFontSize, true, -1.0f);
			chipCursor += chip.Width + kBadgeGap;
		}

		// ---- 定位按钮(在资源管理器中显示)----
		if (options.AllowReveal)
		{
			const WuiRect revealRect { rect.X + rect.W - kRevealWidth, rect.Y, kRevealWidth, rect.H };
			const std::string revealKey = options.IdPrefix.empty()
				? std::string() : options.IdPrefix + ".locate";
			const WuiId revealId = revealKey.empty() ? HashId("wui.texture-picker.locate")
				: HashId(revealKey.c_str());
			const std::string revealLabel = Tr("wui.texture-picker.reveal", "Locate");
			const std::string revealDoc = value.empty()
				? Tr("wui.texture-picker.reveal.none", "No texture reference to locate yet")
				: Tr("wui.texture-picker.reveal.tooltip",
					"Select this texture in the Content Browser and navigate to its folder. Image "
					"sources and .wtex texture assets both work.");
			const bool clicked = ButtonEx(ctx, revealId, revealRect, revealLabel, fieldTheme,
				interactive && !value.empty(), false, revealDoc);
			if (clicked && options.RevealRequested != nullptr)
				*options.RevealRequested = true;
		}

		// ---- 状态节点(稳定 id;状态值按 token,探针不读中英文文案)----
		if (WuiAccessibility::Get().Enabled() && !options.IdPrefix.empty())
		{
			const std::string stateKey = options.IdPrefix + ".state";
			WuiAccessNode node;
			node.Id = HashId(stateKey.c_str());
			node.Window = WuiAccessibility::Get().CurrentWindow();
			node.Panel = WuiAccessibility::Get().CurrentPanel();
			node.Kind = "texture-ref";
			node.Label = label;
			node.Value = WuiTexturePickerStateToken(state);
			std::string doc = DisplayValue(value, options.ImportSourceName);
			for (const std::string& token : tokens)
			{
				if (!doc.empty())
					doc += " · ";
				doc += WuiTexturePickerBadgeText(token);
			}
			node.Tooltip = doc;
			node.Rect = rect;
			node.Enabled = interactive;
			node.Interactive = false;
			node.Visible = true;
			WuiAccessibility::Get().Register(node);
		}

		// ---- 拖放 ----
		// 跨窗口:调用方把 AssetDropBridge 取到的一次投放转成逻辑路径走 DroppedValue(交接语义)。
		if (interactive && !options.DroppedValue.empty() && options.DroppedValue != value)
		{
			value = options.DroppedValue;
			changed = true;
		}
		// 同窗口:核心 WUI 自己的拖拽通道(内容浏览器 → 本槽位落在同一个窗口时)。
		if (interactive)
		{
			ctx.DropTarget(comboRect, "file:");
			std::string payload;
			if (ctx.AcceptDrop(&payload, "file:") && payload.rfind("file:", 0) == 0
				&& payload.size() > 5)
			{
				const std::string dropped = payload.substr(5);
				if (dropped != value)
				{
					value = dropped;
					changed = true;
				}
			}
		}
		return changed;
	}
}
