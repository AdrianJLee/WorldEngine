#include "wldpch.h"
#include "WUI/Panels/InputMapPanel.h"

#include "World/Asset/ProjectManifest.h"
#include "World/Core/KeyCodes.h"
#include "World/Gameplay/Framework/InputGlyphs.h"
#include "World/WUI/Widgets/WuiChrome.h"
#include "World/WUI/WuiLocalization.h"
#include "World/WUI/WuiWidgets.h"

#include <algorithm>
#include <vector>

namespace World
{
	namespace
	{
		// ================= 版式度量(整个面板共用一套,不做"每块各自试数字") =================
		constexpr float kRowH = 24.0f;        // 数据行高
		constexpr float kCtrlH = 20.0f;       // 行内控件**统一**高度(胶囊与按钮同高 ⇒ 不会再错位)
		constexpr float kGap = 4.0f;
		constexpr float kSectionGap = 14.0f;
		constexpr float kPad = 10.0f;
		constexpr float kIndent = 16.0f;
		constexpr float kToolbarH = 32.0f;
		constexpr float kFooterH = 24.0f;
		constexpr float kHeadH = 20.0f;
		// **同级**之间的间隙:必须大于级内行距(kGap),同级与跨级才分得开(见 LevelStyle)。
		constexpr float kMappingSiblingGap = 7.0f;
		constexpr float kCaptionH = 15.0f;
		constexpr float kCardH = 26.0f;
		constexpr float kEmptyH = 52.0f;
		constexpr float kMinSectionW = 80.0f;

		// 行内控件的垂直中心:所有控件都按"行中心"摆 —— 错位从数学上就不可能发生。
		float RowCtrlY(float rowY) { return rowY + (kRowH - kCtrlH) * 0.5f; }
		// 窄列降级时的第 n 行(0 起)。
		float LineY(float rowY, int line) { return rowY + static_cast<float>(line) * (kRowH + kGap); }

		// 按钮宽度**按实测文字算**:ButtonEx 用 15px 字体、左右各 8px 内边距。
		// (教训 2026-10-09:按经验拍宽度时 "+Mouse" 需要 ~66px 只给了 58、"Del" 需要 ~40 只给了 26,
		//  文字溢出边框 —— 用户看到的"删除按钮错位"就是这么来的。宽度跟着文字走,换语言也不会崩。)
		float ButtonW(const Wui::WuiContext& ctx, const std::string& label)
		{
			return ctx.MeasureTextWidth(label, 15.0f) + 18.0f;
		}

		std::string Tr(const char* key, const char* fallback) { return Wui::Tr(key, fallback); }

		// 状态行:文案走本地化,**名字原样拼在后面**(名字是用户数据,不翻译)。
		std::string WithName(const std::string& text, const std::string& name)
		{
			return name.empty() ? text : (text + " '" + name + "'");
		}

		Gameplay::InputBinding NextBindingFor(const Gameplay::InputAction& action, Gameplay::InputDevice device,
			const int* candidates, std::size_t candidateCount)
		{
			for (std::size_t i = 0; i < candidateCount; ++i)
			{
				const bool used = std::any_of(action.Bindings.begin(), action.Bindings.end(),
					[device, &candidates, i](const Gameplay::InputBinding& existing)
					{
						return existing.Device == device && existing.Code == candidates[i];
					});
				if (!used)
					return Gameplay::InputBinding{ device, candidates[i] };
			}
			return Gameplay::InputBinding{ device, candidates[0] };
		}

		Gameplay::InputAction* FindAction(Gameplay::InputMap& map, const std::string& name)
		{
			for (Gameplay::InputAction& action : map.Actions())
				if (action.Name == name)
					return &action;
			return nullptr;
		}

		std::vector<std::string> BuildActionOptions(const Gameplay::InputMap& map)
		{
			std::vector<std::string> options;
			options.push_back(Tr("panel.input_map.combo.none", "(none)"));
			for (const Gameplay::InputAction& action : map.Actions())
				options.push_back(action.Name);
			return options;
		}

		const std::vector<std::string>& GamepadAxisOptions()
		{
			static const std::vector<std::string> options = { "(none)", "LX", "LY", "RX", "RY", "LT", "RT" };
			return options;
		}

		const std::vector<std::string>& TriggerTypeOptions()
		{
			static const std::vector<std::string> options = {
				"pressed", "released", "hold", "tap", "double_tap", "pulse", "chord" };
			return options;
		}

		const char* TriggerTypeName(Gameplay::TriggerType type)
		{
			switch (type)
			{
			case Gameplay::TriggerType::Pressed: return "pressed";
			case Gameplay::TriggerType::Released: return "released";
			case Gameplay::TriggerType::Hold: return "hold";
			case Gameplay::TriggerType::Tap: return "tap";
			case Gameplay::TriggerType::DoubleTap: return "double_tap";
			case Gameplay::TriggerType::Pulse: return "pulse";
			case Gameplay::TriggerType::Chord: return "chord";
			}
			return "pressed";
		}

		int TriggerTypeIndex(Gameplay::TriggerType type)
		{
			const std::vector<std::string>& options = TriggerTypeOptions();
			const std::string name = TriggerTypeName(type);
			const auto it = std::find(options.begin(), options.end(), name);
			return it == options.end() ? 0 : static_cast<int>(it - options.begin());
		}

		Gameplay::TriggerType TriggerTypeFromIndex(int index)
		{
			switch (index)
			{
			case 1: return Gameplay::TriggerType::Released;
			case 2: return Gameplay::TriggerType::Hold;
			case 3: return Gameplay::TriggerType::Tap;
			case 4: return Gameplay::TriggerType::DoubleTap;
			case 5: return Gameplay::TriggerType::Pulse;
			case 6: return Gameplay::TriggerType::Chord;
			default: return Gameplay::TriggerType::Pressed;
			}
		}

		std::string BindingLabel(const Gameplay::InputBinding& binding)
		{
			return Gameplay::InputGlyphs::GetGlyphText(binding.Device, binding.Code);
		}

		// ---------------- 三段布局:左 Actions / 中 Axes / 右 Contexts ----------------
		// 每段**自带**"标题 + 添加框 + 自己的滚动区":滚轮只影响鼠标所在的那一段,
		// 添加框永远停在段顶。窗口不够宽时依次降级 3 列 → 2 列 → 1 列(每段仍然自带滚动区)。
		struct SectionLayouts
		{
			Wui::WuiRect Actions;
			Wui::WuiRect Axes;
			Wui::WuiRect Contexts;
		};

		// 一列(窄窗)时**按内容多少**分配高度:7 条动作与 1 条轴各占 1/3 是浪费 ——
		// 动作被切得只剩两行,轴那一格却空着。先按内容比例分,再把不足最小高度的段抬起来
		// (缺口按其余段的富余量等比例扣),保证每段都还有能用的列表高度。
		SectionLayouts StackedSectionLayouts(const Wui::WuiRect& body, float wantedActions,
			float wantedAxes, float wantedContexts)
		{
			SectionLayouts out;
			const float gap = kSectionGap;
			const float avail = std::max(90.0f, body.H - gap * 2.0f);
			const float minH = std::min(112.0f, avail / 3.0f);
			float alloc[3] = { std::max(1.0f, wantedActions), std::max(1.0f, wantedAxes),
				std::max(1.0f, wantedContexts) };
			const float total = alloc[0] + alloc[1] + alloc[2];
			for (float& value : alloc)
				value = avail * value / total;
			for (int pass = 0; pass < 2; ++pass)
			{
				float deficit = 0.0f;
				float surplus = 0.0f;
				for (const float value : alloc)
				{
					if (value < minH)
						deficit += minH - value;
					else
						surplus += value - minH;
				}
				if (deficit <= 0.0f || surplus <= 0.0f)
					break;
				const float take = std::min(deficit, surplus);
				for (float& value : alloc)
					value = (value < minH)
						? (value + take * ((minH - value) / deficit))
						: (value - take * ((value - minH) / surplus));
			}
			float y = body.Y;
			out.Actions = { body.X, y, body.W, alloc[0] };
			y += alloc[0] + gap;
			out.Axes = { body.X, y, body.W, alloc[1] };
			y += alloc[1] + gap;
			out.Contexts = { body.X, y, body.W, std::max(60.0f, body.Y + body.H - y) };
			return out;
		}

		SectionLayouts ComputeSectionLayouts(const Wui::WuiRect& body, float wantedActions = 0.0f,
			float wantedAxes = 0.0f, float wantedContexts = 0.0f)
		{
			SectionLayouts out;
			const float gap = kSectionGap;
			if (body.W >= 900.0f)
			{
				// 三列:上下文最需要横向空间(它要装下映射 + 触发器两层嵌套)。
				const float avail = body.W - gap * 2.0f;
				const float wActions = std::max(260.0f, avail * 0.31f);
				const float wAxes = std::max(230.0f, avail * 0.25f);
				const float wContexts = std::max(280.0f, avail - wActions - wAxes);
				out.Actions = { body.X, body.Y, wActions, body.H };
				out.Axes = { body.X + wActions + gap, body.Y, wAxes, body.H };
				out.Contexts = { body.X + wActions + gap + wAxes + gap, body.Y, wContexts, body.H };
			}
			else if (body.W >= 620.0f)
			{
				// 两列:左列上 Actions / 下 Axes,右列 Contexts 独占。
				const float wLeft = std::floor((body.W - gap) * 0.47f);
				const float hTop = std::floor((body.H - gap) * 0.5f);
				out.Actions = { body.X, body.Y, wLeft, hTop };
				out.Axes = { body.X, body.Y + hTop + gap, wLeft, body.H - hTop - gap };
				out.Contexts = { body.X + wLeft + gap, body.Y, body.W - wLeft - gap, body.H };
			}
			else
			{
				// 一列(独立滚动区保持不变 —— 段语义不随窗口宽度改变)。
				out = StackedSectionLayouts(body, wantedActions, wantedAxes, wantedContexts);
			}
			return out;
		}

		// ================= 三级视觉语言:上下文 → 映射 → 触发器 =================
		// 用户口径「层级、同级之间区分不明显」的根因:三级只有**一条细竖线**的区别,
		// 而同级之间(两个映射、两个触发器)连一条分界线都没有。
		// 修法是让每一级同时具备三样东西,并把"同级间隙"做成比上下级行距**更大**:
		//   ① 缩进步长(越内越窄)     ② 盒子底色(越深越靠内)     ③ 左侧导轨色(一级最亮)
		//   ④ 同级间隙 SiblingGap > 级内行距 ⇒ 视觉分组天然成立("靠近的是一伙的")
		struct LevelStyle
		{
			float Indent;          // 相对上一级的缩进步长
			Wui::WuiColor Box;     // 盒子底色(越深越靠内)
			Wui::WuiColor Rail;    // 左侧导轨色
			float RailWidth;
			float SiblingGap;      // **同级**之间的间隙(必须大于级内行距)
			float Radius;
		};

		LevelStyle ContextLevel(const Wui::WuiTheme& theme)
		{ return { 0.0f, theme.PanelHeader, theme.Accent, 3.0f, 10.0f, 5.0f }; }

		LevelStyle MappingLevel(const Wui::WuiTheme& theme)
		{ return { kIndent, theme.ContentBg, theme.BorderStrong, 2.0f, kMappingSiblingGap, 4.0f }; }

		LevelStyle TriggerLevel(const Wui::WuiTheme& theme)
		{ return { kIndent, theme.WindowBg, theme.Border, 2.0f, kGap, 3.0f }; }

		// 一级的盒子 + 左侧导轨(+ 悬停描边)。必须在**该级所有控件之前**画,控件才压在盒子上。
		void LevelSurface(Wui::WuiContext& ctx, const Wui::WuiTheme& theme, const Wui::WuiRect& box,
			const LevelStyle& style, bool hoverLift)
		{
			if (box.W <= 1.0f || box.H <= 1.0f)
				return;
			Wui::PanelBackground(ctx, box, style.Box, style.Radius);
			if (hoverLift && ctx.IsHovered(box))
				Wui::HighlightOutline(ctx, box, style.Rail, style.Radius, 1.0f);
			Wui::PanelBackground(ctx, { box.X + 1.0f, box.Y + 3.0f, style.RailWidth, box.H - 6.0f },
				style.Rail, style.RailWidth * 0.5f);
		}

		// 一个映射**块**的高度 = 映射行 + 同级间隙 + 它挂的触发器 + "+ Trigger" 行。
		// 盒子高度、卡片高度、滚动内容高度三处都从这里取(各算一遍迟早错开)。
		float MappingBlockHeight(const Gameplay::ActionBindingConfig& mapping, float sectionWidth)
		{
			const float mappingH = (sectionWidth < 470.0f) ? (kRowH * 2.0f + kGap) : kRowH;
			const float triggerH = (sectionWidth < 480.0f) ? (kRowH * 2.0f + kGap) : kRowH;
			float height = mappingH + kMappingSiblingGap;
			height += (triggerH + kGap) * static_cast<float>(mapping.Triggers.size());
			height += kRowH + kGap;                          // "+ Trigger" 行
			return height;
		}

		// 一条上下文的**卡片高度**:同一份事实源。
		float ContextCardHeight(const Gameplay::InputMappingContext& context, float sectionWidth)
		{
			const float headH = (sectionWidth < 560.0f) ? (kRowH * 2.0f + kGap) : kRowH;
			float height = 6.0f + headH + kGap;
			for (const Gameplay::ActionBindingConfig& mapping : context.GetMappings())
				height += MappingBlockHeight(mapping, sectionWidth);
			height += kRowH + kGap;                          // "+ Mapping" 行
			return height + 6.0f;
		}

		// 段内三条固定带(标题 / 添加框 / 列标题)+ 剩下的滚动列表。
		struct SectionBands
		{
			Wui::WuiRect Head;
			Wui::WuiRect Add;
			Wui::WuiRect Captions;
			Wui::WuiRect List;
		};

		SectionBands SplitBands(const Wui::WuiRect& section)
		{
			SectionBands bands;
			float y = section.Y;
			bands.Head = { section.X, y, section.W, kHeadH };
			y += kHeadH + kGap;
			bands.Add = { section.X, y, section.W, kCardH };
			y += kCardH + kGap;
			bands.Captions = { section.X, y, section.W, kCaptionH };
			y += kCaptionH;
			bands.List = { section.X, y, section.W, std::max(40.0f, section.Y + section.H - y) };
			return bands;
		}

		// 段标题:强调竖条 + 标题 + 右端计数胶囊;区块说明走**悬停提示**(不占版面)。
		void SectionHeadBlock(Wui::WuiContext& ctx, const Wui::WuiTheme& theme, const Wui::WuiRect& head,
			const std::string& title, std::size_t count, const std::string& hint)
		{
			Wui::SectionHeader(ctx, { head.X, head.Y, head.W, kHeadH }, title, theme.Accent, theme, 13.5f);
			const std::string badge = std::to_string(count);
			const float badgeW = std::max(24.0f, ctx.MeasureTextWidth(badge, 11.0f) + 14.0f);
			Wui::Badge(ctx, { head.X + head.W - badgeW, head.Y + 2.0f, badgeW, 16.0f }, badge,
				theme.PanelHeader, theme.TextMuted, theme, 11.0f, false, 8.0f);
			if (!hint.empty())
				Wui::Tooltip(ctx, head, hint);
		}

		struct Caption
		{
			float X;
			std::string Text;
		};

		void CaptionRow(Wui::WuiContext& ctx, const Wui::WuiTheme& theme, const Wui::WuiRect& row,
			std::initializer_list<Caption> captions)
		{
			for (const Caption& caption : captions)
				if (!caption.Text.empty())
					Wui::Label(ctx, { caption.X, row.Y + 1.0f }, caption.Text, theme.TextMuted, 10.5f);
		}

		// 数据行底:悬停高亮 / 斑马纹(必须在行内控件**之前**画,控件才压在底色上)。
		void RowSurface(Wui::WuiContext& ctx, const Wui::WuiTheme& theme, const Wui::WuiRect& row, bool striped)
		{
			if (ctx.IsHovered(row))
				Wui::HoverRow(ctx, row, true, false, theme, 3.0f);
			else if (striped)
				Wui::PanelBackground(ctx, row, theme.ContentBg, 3.0f);
		}

		// "新增"卡片:输入框 + 按钮放一块底色里 —— 与数据行区分开(一眼看出"这里是入口")。
		void AddCardSurface(Wui::WuiContext& ctx, const Wui::WuiTheme& theme, const Wui::WuiRect& rect)
		{
			Wui::PanelBackground(ctx, rect, theme.ContentBg, 4.0f);
		}

		// 键位/设备胶囊:宽度按**实测**文本算(中文、手柄符号、缩写宽度差得远,按字符数估一定歪)。
		float ChipWidth(const Wui::WuiContext& ctx, const std::string& glyph)
		{
			return std::max(26.0f, ctx.MeasureTextWidth(glyph, 11.0f) + 14.0f);
		}

		void Chip(Wui::WuiContext& ctx, const Wui::WuiTheme& theme, float x, float centerY, float width,
			const std::string& text, const Wui::WuiColor& fill, const Wui::WuiColor& textColor)
		{
			Wui::Badge(ctx, { x, centerY - kCtrlH * 0.5f, width, kCtrlH }, text, fill, textColor, theme,
				11.0f, false, 3.0f);
		}

		// 一条绑定 = 胶囊 + 行内 ×(Muted 色调:它是最次要的操作,不该和"已有键位"一样抢眼)。
		float BindingChip(Wui::WuiContext& ctx, const Wui::WuiTheme& theme, const std::string& removeId,
			float x, float rowY, const std::string& glyph, bool& removed)
		{
			const float chipW = ChipWidth(ctx, glyph);
			Chip(ctx, theme, x, RowCtrlY(rowY) + kCtrlH * 0.5f, chipW, glyph, theme.ActiveBg, theme.Text);
			const std::string kill = "x";
			const float killW = ButtonW(ctx, kill);
			if (Wui::ButtonEx(ctx, Wui::HashId(removeId.c_str()),
				{ x + chipW + 1.0f, RowCtrlY(rowY), killW, kCtrlH }, kill, theme, true, false,
				Tr("panel.input_map.tip.remove_binding", "Remove this binding"), Wui::ButtonTone::Muted))
			{
				removed = true;
			}
			return chipW + 1.0f + killW + kGap;
		}
	}

	// ================= 装载 / 保存 =================

	void InputMapPanel::EnsureLoaded()
	{
		if (m_Loaded)
			return;
		m_Loaded = true;

		std::filesystem::path manifestPath;
		if (World::Asset::ProjectManifest::Locate(std::filesystem::current_path(), &manifestPath))
		{
			std::string error;
			World::Asset::ProjectManifest manifest;
			if (World::Asset::ProjectManifest::Load(manifestPath, &manifest, &error))
				m_Path = manifest.ResolveContentRoot(manifestPath) / "input.weinput";
		}

		if (m_Path.empty())
		{
			m_Status = Tr("panel.input_map.status.no_content_root", "content root not found; open a project first");
			m_StatusError = true;
			return;
		}

		std::string loadError;
		if (Gameplay::InputMap::Load(m_Path, &m_Map, &loadError))
		{
			m_Status = WithName(Tr("panel.input_map.status.loaded", "loaded"), m_Path.filename().string());
			return;
		}

		// 文件不存在/解析失败:B7 —— **不要**擅自写一份默认映射进用户的工程。
		// 空模型 + 明确提示,让用户用各段顶部的"添加"自己建。
		m_Status = Tr("panel.input_map.status.no_map",
			"no input map at assets/input.weinput (use the Add boxes at the top of each section)");
	}

	bool InputMapPanel::Save()
	{
		if (m_Path.empty())
		{
			m_Status = Tr("panel.input_map.status.no_content_root", "content root not found; cannot save");
			m_StatusError = true;
			return false;
		}
		std::string error;
		if (!Gameplay::InputMap::Save(m_Path, m_Map, &error))
		{
			m_Status = WithName(Tr("panel.input_map.status.save_failed", "save failed"), error);
			m_StatusError = true;
			return false;
		}
		m_Status = WithName(Tr("panel.input_map.status.saved", "saved"), m_Path.filename().string());
		m_StatusError = false;
		return true;
	}

	void InputMapPanel::MarkDirtyAndSave(const std::string& what)
	{
		m_Status = what;
		m_StatusError = false;
		Save();
	}

	// ================= 各段列表高度(必须与 Render*Section 的推进逐行一致) =================

	float InputMapPanel::ActionsListHeight(float sectionWidth) const
	{
		const float rowH = (sectionWidth < 430.0f) ? (kRowH * 3.0f + kGap * 2.0f) : kRowH;
		if (m_Map.Actions().empty())
			return kGap * 2.0f + kEmptyH;
		return kGap * 2.0f + rowH * static_cast<float>(m_Map.Actions().size());
	}

	float InputMapPanel::AxesListHeight(float sectionWidth) const
	{
		const float rowH = (sectionWidth < 560.0f) ? (kRowH * 4.0f + kGap * 3.0f) : kRowH;
		if (m_Map.Axes().empty())
			return kGap * 2.0f;
		return kGap * 2.0f + rowH * static_cast<float>(m_Map.Axes().size());
	}

	float InputMapPanel::ContextsListHeight(float sectionWidth) const
	{
		if (m_Map.Contexts().empty())
			return kGap * 2.0f + kEmptyH;
		float height = kGap * 2.0f;
		for (const Gameplay::InputMappingContext& context : m_Map.Contexts())
			height += ContextCardHeight(context, sectionWidth);
		return height;
	}

	// ================= 段 1:动作(左) =================

	void InputMapPanel::RenderActionsSection(Wui::WuiContext& ctx, const Wui::WuiTheme& theme,
		const Wui::WuiRect& rect)
	{
		if (rect.W < kMinSectionW || rect.H < 80.0f)
			return;
		const SectionBands bands = SplitBands(rect);
		const std::string addLabel = Tr("panel.input_map.add", "Add");
		const std::string delLabel = Tr("panel.input_map.delete", "Del");
		const std::string keyLabel = Tr("panel.input_map.add_key", "+Key");
		const std::string mouseLabel = Tr("panel.input_map.add_mouse", "+Mouse");
		const std::string padLabel = Tr("panel.input_map.add_pad", "+Pad");
		const float addW = ButtonW(ctx, addLabel);
		const float delW = ButtonW(ctx, delLabel);
		const float keyW = ButtonW(ctx, keyLabel);
		const float mouseW = ButtonW(ctx, mouseLabel);
		const float padW = ButtonW(ctx, padLabel);
		const float opsTotal = keyW + mouseW + padW + kGap * 2.0f;
		const float left = bands.List.X + kPad;
		const float right = bands.List.X + bands.List.W - kPad;
		const float delX = right - delW;
		const float nameW = std::max(84.0f, std::min(200.0f,
			std::max(90.0f, delX - kGap - opsTotal - left - kPad) * 0.42f));
		const bool stacked = bands.List.W < 430.0f;

		SectionHeadBlock(ctx, theme, bands.Head, Tr("panel.input_map.actions", "Actions"),
			m_Map.Actions().size(), Tr("panel.input_map.actions.hint",
				"An action is a named intent (Jump, Fire). Bind it to keys, mouse or gamepad here."));

		// 添加框:固定在段顶(滚轮只滚下面的列表)。
		const auto createAction = [this]
		{
			if (m_NewActionName.empty())
				return;
			if (FindAction(m_Map, m_NewActionName))
			{
				m_Status = WithName(Tr("panel.input_map.status.exists_action", "Action already exists"),
					m_NewActionName);
				m_StatusError = true;
				return;
			}
			Gameplay::InputAction created;
			created.Name = m_NewActionName;
			m_Map.Actions().push_back(created);
			MarkDirtyAndSave(WithName(Tr("panel.input_map.status.added_action", "Added action"), created.Name));
			m_NewActionName.clear();
		};
		{
			AddCardSurface(ctx, theme, bands.Add);
			const float fieldW = std::max(56.0f, bands.Add.W - addW - kGap - 12.0f);
			const Wui::TextFieldA11y a11y {
				Tr("panel.input_map.new_action.name", "New action name"),
				Tr("panel.input_map.new_action.hint", "new action name, then Add") };
			if (Wui::TextField(ctx, Wui::HashId("input.action.new.name"),
				{ bands.Add.X + 4.0f, bands.Add.Y + 3.0f, fieldW, kCardH - 6.0f }, m_NewActionName, theme,
				nullptr, &a11y))
				createAction();
			if (Wui::ButtonEx(ctx, Wui::HashId("input.action.new.add"),
				{ bands.Add.X + 4.0f + fieldW + kGap, bands.Add.Y + 3.0f, addW, kCardH - 6.0f }, addLabel,
				theme, !m_NewActionName.empty(), false, a11y.Placeholder, Wui::ButtonTone::Action))
				createAction();
		}

		// 窄列(堆叠)时名称与胶囊不在同一行,列标题只留首列与末列 —— 两行文字会叠在一起。
		CaptionRow(ctx, theme, bands.Captions, {
			{ left, Tr("panel.input_map.col.action", "ACTION") },
			{ stacked ? 0.0f : (left + nameW + kPad),
				stacked ? std::string() : Tr("panel.input_map.col.bindings", "BINDINGS") },
			{ delX, Tr("panel.input_map.col.remove", "REMOVE") } });

		Wui::BeginScrollArea(ctx, bands.List, ActionsListHeight(bands.List.W), m_ScrollActions, theme,
			Wui::HashId("input.scroll.actions"));
		float y = bands.List.Y - m_ScrollActions + kGap;
		if (m_Map.Actions().empty())
		{
			Wui::EmptyState(ctx, { left, y, right - left, kEmptyH }, "",
				Tr("panel.input_map.empty.actions.title", "No actions yet"),
				Tr("panel.input_map.empty.actions.hint",
					"Type a name above and press Add to create your first action."), "", 0, theme);
			y += kEmptyH;
		}
		for (std::size_t index = 0; index < m_Map.Actions().size();)
		{
			Gameplay::InputAction& action = m_Map.Actions()[index];
			const std::string idBase = "input.action." + action.Name;
			const bool rebinding = m_RebindingAction == action.Name;
			const bool appending = m_AppendKeyAction == action.Name;
			const float rowH = stacked ? (kRowH * 3.0f + kGap * 2.0f) : kRowH;
			RowSurface(ctx, theme, { bands.List.X, y - 2.0f, bands.List.W, rowH + 4.0f }, (index % 2) == 1);

			// 名称列 = 改键入口。宽度按实测文字收缩:占满整列会变成"一表全是框"。
			const float nameWidth = std::min(nameW, ButtonW(ctx, action.Name));
			if (Wui::ButtonEx(ctx, Wui::HashId((idBase + ".rebind").c_str()),
				{ left, RowCtrlY(y), nameWidth, kCtrlH }, action.Name, theme, true, rebinding,
				Tr("panel.input_map.tip.rebind", "Click, then press any key to replace every binding of")
					+ " '" + action.Name + "'"))
			{
				m_AppendKeyAction.clear();
				m_RebindingAction = rebinding ? std::string() : action.Name;
				m_Status = rebinding ? Tr("panel.input_map.status.rebind_cancelled", "rebind cancelled")
					: (Tr("panel.input_map.status.rebind_armed",
						"press any key to replace every binding of") + " '" + action.Name + "'");
				m_StatusError = false;
			}

			const float chipsY = stacked ? LineY(y, 1) : y;
			const float opsY = stacked ? LineY(y, 2) : y;
			const float chipsLimit = stacked ? right : (delX - kGap - opsTotal - kGap);
			float bx = stacked ? left : (left + nameWidth + kPad);

			if (rebinding || appending)
			{
				const std::string waiting = rebinding
					? Tr("panel.input_map.waiting_replace", "press any key to replace...")
					: Tr("panel.input_map.waiting_add", "press any key to add...");
				const float chipW = ChipWidth(ctx, waiting);
				Chip(ctx, theme, bx, RowCtrlY(chipsY) + kCtrlH * 0.5f, chipW, waiting, theme.Warning,
					theme.WindowBg);
				bx += chipW + kGap;
			}
			bool removed = false;
			for (std::size_t bindingIndex = 0; bindingIndex < action.Bindings.size(); ++bindingIndex)
			{
				if (bx + 34.0f > chipsLimit)
					break;                                   // 放不下就不画,不让胶囊压到操作列
				bx += BindingChip(ctx, theme,
					idBase + ".binding." + std::to_string(bindingIndex) + ".remove", bx, chipsY,
					BindingLabel(action.Bindings[bindingIndex]), removed);
				if (removed)
				{
					action.Bindings.erase(action.Bindings.begin() + static_cast<std::ptrdiff_t>(bindingIndex));
					break;
				}
			}
			if (removed)
			{
				MarkDirtyAndSave(WithName(Tr("panel.input_map.status.removed_binding", "Removed a binding from"),
					action.Name));
				continue;                                    // 容器已变,本帧不再用该引用
			}
			if (action.Bindings.empty() && !rebinding && !appending)
			{
				const std::string unbound = Tr("panel.input_map.unbound", "unbound");
				Chip(ctx, theme, bx, RowCtrlY(chipsY) + kCtrlH * 0.5f, ChipWidth(ctx, unbound), unbound,
					theme.PanelHeader, theme.TextDisabled);
			}

			// 行尾动作列:固定顺序、固定宽度 —— 一眼看出能加什么。色调 Action = 与已有键位区分。
			const float opsX = stacked ? (right - opsTotal) : (delX - kGap - opsTotal);
			if (Wui::ButtonEx(ctx, Wui::HashId((idBase + ".addkey").c_str()),
				{ opsX, RowCtrlY(opsY), keyW, kCtrlH }, keyLabel, theme, true, appending,
				Tr("panel.input_map.tip.add_key", "Add a keyboard binding: click, then press the key"),
				Wui::ButtonTone::Action))
			{
				m_RebindingAction.clear();
				m_AppendKeyAction = appending ? std::string() : action.Name;
				m_Status = appending ? Tr("panel.input_map.status.addkey_cancelled", "add-key cancelled")
					: (Tr("panel.input_map.status.append_armed", "press any key to ADD a binding to")
						+ " '" + action.Name + "'");
				m_StatusError = false;
			}
			if (Wui::ButtonEx(ctx, Wui::HashId((idBase + ".addmouse").c_str()),
				{ opsX + keyW + kGap, RowCtrlY(opsY), mouseW, kCtrlH }, mouseLabel, theme, true, false,
				Tr("panel.input_map.tip.add_mouse", "Add the next free mouse binding"),
				Wui::ButtonTone::Action))
			{
				static const int mouseButtons[] = { 0, 1, 2 };
				action.Bindings.push_back(NextBindingFor(action, Gameplay::InputDevice::Mouse, mouseButtons, 3));
				MarkDirtyAndSave(WithName(Tr("panel.input_map.status.added_binding", "Added a binding for"),
					action.Name));
			}
			if (Wui::ButtonEx(ctx, Wui::HashId((idBase + ".addpad").c_str()),
				{ opsX + keyW + mouseW + kGap * 2.0f, RowCtrlY(opsY), padW, kCtrlH }, padLabel, theme, true,
				false, Tr("panel.input_map.tip.add_pad", "Add the next free gamepad button"),
				Wui::ButtonTone::Action))
			{
				static const int padButtons[] = { 0, 1, 2, 3, 4, 5, 6, 7 };
				action.Bindings.push_back(NextBindingFor(action, Gameplay::InputDevice::Gamepad, padButtons, 8));
				MarkDirtyAndSave(WithName(Tr("panel.input_map.status.added_binding", "Added a binding for"),
					action.Name));
			}
			if (Wui::ButtonEx(ctx, Wui::HashId((idBase + ".delete").c_str()),
				{ delX, RowCtrlY(y), delW, kCtrlH }, delLabel, theme, true, false,
				Tr("panel.input_map.tip.delete_action", "Delete the action") + " '" + action.Name + "'",
				Wui::ButtonTone::Muted))
			{
				m_Map.Actions().erase(m_Map.Actions().begin() + static_cast<std::ptrdiff_t>(index));
				MarkDirtyAndSave(WithName(Tr("panel.input_map.status.deleted_action", "Deleted action"),
					action.Name));
				continue;
			}
			++index;
			y += rowH;
		}
		Wui::EndScrollArea(ctx);
	}

	// ================= 段 2:轴(中) =================

	void InputMapPanel::RenderAxesSection(Wui::WuiContext& ctx, const Wui::WuiTheme& theme,
		const Wui::WuiRect& rect)
	{
		if (rect.W < kMinSectionW || rect.H < 80.0f)
			return;
		const SectionBands bands = SplitBands(rect);
		const std::string delLabel = Tr("panel.input_map.delete", "Del");
		const std::string addLabel = Tr("panel.input_map.add_axis", "Add Axis");
		const float delW = ButtonW(ctx, delLabel);
		const float addW = ButtonW(ctx, addLabel);
		const float left = bands.List.X + kPad;
		const float right = bands.List.X + bands.List.W - kPad;
		const float delX = right - delW;
		const bool stacked = bands.List.W < 560.0f;
		const float nameW = std::min(140.0f, std::max(56.0f, (right - left) * 0.16f));
		const float x0 = left + nameW + kPad;
		const float fieldAvail = std::max(120.0f, delX - kGap - x0);
		const float comboW = std::max(52.0f, std::min(120.0f,
			(fieldAvail - 74.0f - 66.0f - kGap * 3.0f) * 0.5f));

		SectionHeadBlock(ctx, theme, bands.Head, Tr("panel.input_map.axes", "Axes"), m_Map.Axes().size(),
			Tr("panel.input_map.axes.hint",
				"An axis is a 1D value (MoveX) built from two actions or one gamepad stick."));

		// 添加框:固定在段顶。
		const auto createAxis = [this]
		{
			if (m_NewAxisName.empty())
				return;
			const bool exists = std::any_of(m_Map.Axes().begin(), m_Map.Axes().end(),
				[this](const Gameplay::InputAxis& existing) { return existing.Name == m_NewAxisName; });
			if (exists)
			{
				m_Status = WithName(Tr("panel.input_map.status.exists_axis", "Axis already exists"), m_NewAxisName);
				m_StatusError = true;
				return;
			}
			Gameplay::InputAxis created;
			created.Name = m_NewAxisName;
			m_Map.Axes().push_back(created);
			MarkDirtyAndSave(WithName(Tr("panel.input_map.status.added_axis", "Added axis"), created.Name));
			m_NewAxisName.clear();
		};
		{
			AddCardSurface(ctx, theme, bands.Add);
			const float fieldW = std::max(56.0f, bands.Add.W - addW - kGap - 12.0f);
			const Wui::TextFieldA11y a11y {
				Tr("panel.input_map.new_axis.name", "New axis name"),
				Tr("panel.input_map.new_axis.hint", "new axis name, then Add Axis") };
			if (Wui::TextField(ctx, Wui::HashId("input.axis.new.name"),
				{ bands.Add.X + 4.0f, bands.Add.Y + 3.0f, fieldW, kCardH - 6.0f }, m_NewAxisName, theme,
				nullptr, &a11y))
				createAxis();
			if (Wui::ButtonEx(ctx, Wui::HashId("input.axis.new.add"),
				{ bands.Add.X + 4.0f + fieldW + kGap, bands.Add.Y + 3.0f, addW, kCardH - 6.0f }, addLabel,
				theme, !m_NewAxisName.empty(), false, a11y.Placeholder, Wui::ButtonTone::Action))
				createAxis();
		}

		const std::string captionPositive = Tr("panel.input_map.col.positive", "POSITIVE");
		const std::string captionNegative = Tr("panel.input_map.col.negative", "NEGATIVE");
		const std::string captionGamepad = Tr("panel.input_map.col.gamepad", "GAMEPAD");
		const std::string captionDeadzone = Tr("panel.input_map.col.deadzone", "DEADZONE");
		if (!stacked)
			CaptionRow(ctx, theme, bands.Captions, {
				{ left, Tr("panel.input_map.col.axis", "AXIS") },
				{ x0, captionPositive },
				{ x0 + comboW + kGap, captionNegative },
				{ x0 + (comboW + kGap) * 2.0f, captionGamepad },
				{ x0 + (comboW + kGap) * 2.0f + 74.0f + kGap, captionDeadzone },
				{ delX, Tr("panel.input_map.col.remove", "REMOVE") } });

		const std::vector<std::string> actionOptions = BuildActionOptions(m_Map);
		Wui::BeginScrollArea(ctx, bands.List, AxesListHeight(bands.List.W), m_ScrollAxes, theme,
			Wui::HashId("input.scroll.axes"));
		float y = bands.List.Y - m_ScrollAxes + kGap;
		for (std::size_t index = 0; index < m_Map.Axes().size();)
		{
			Gameplay::InputAxis& axis = m_Map.Axes()[index];
			const std::string idBase = "input.axis." + axis.Name;
			bool changed = false;
			const float rowH = stacked ? (kRowH * 4.0f + kGap * 3.0f) : kRowH;
			RowSurface(ctx, theme, { bands.List.X, y - 2.0f, bands.List.W, rowH + 4.0f }, (index % 2) == 1);

			Wui::Label(ctx, { left, RowCtrlY(y) + 4.0f }, axis.Name, theme.Text, 12.5f);

			// 字段位置:宽列四个字段一排;窄列**一行一个字段**并给行内短标签 ——
			// 字段宽度优先:挤成半宽会把 "MoveRight" 中间省略成 "M...t",那是读不出来的。
			const float stackLabelW = 58.0f;
			const float stackFieldX = left + stackLabelW;
			const float stackFieldW = std::max(60.0f, right - stackFieldX);
			const float posX = stacked ? stackFieldX : x0;
			const float posY = stacked ? LineY(y, 1) : y;
			const float negX = stacked ? stackFieldX : (x0 + comboW + kGap);
			const float negY = stacked ? LineY(y, 2) : y;
			const float padX = stacked ? stackFieldX : (x0 + (comboW + kGap) * 2.0f);
			const float padY = stacked ? LineY(y, 3) : y;
			const float dzX = stacked ? (stackFieldX + 74.0f + kGap * 2.0f + 54.0f)
				: (padX + 74.0f + kGap);
			const float posW = stacked ? stackFieldW : comboW;
			const float negW = posW;
			if (stacked)
			{
				Wui::Label(ctx, { left, RowCtrlY(posY) + 4.0f }, captionPositive, theme.TextMuted, 10.5f);
				Wui::Label(ctx, { left, RowCtrlY(negY) + 4.0f }, captionNegative, theme.TextMuted, 10.5f);
				Wui::Label(ctx, { left, RowCtrlY(padY) + 4.0f }, captionGamepad, theme.TextMuted, 10.5f);
				Wui::Label(ctx, { stackFieldX + 74.0f + kGap * 2.0f, RowCtrlY(padY) + 4.0f },
					captionDeadzone, theme.TextMuted, 10.5f);
			}

			int positive = 0;
			{
				const auto it = std::find(actionOptions.begin(), actionOptions.end(), axis.PositiveAction);
				positive = it == actionOptions.end() ? 0 : static_cast<int>(it - actionOptions.begin());
			}
			const int positiveBefore = positive;
			Wui::Combo(ctx, Wui::HashId((idBase + ".positive").c_str()),
				{ posX, RowCtrlY(posY), posW, kCtrlH }, captionPositive, actionOptions, positive, theme);
			if (positive != positiveBefore)
			{
				axis.PositiveAction = positive == 0 ? std::string()
					: actionOptions[static_cast<std::size_t>(positive)];
				changed = true;
			}
			int negative = 0;
			{
				const auto it = std::find(actionOptions.begin(), actionOptions.end(), axis.NegativeAction);
				negative = it == actionOptions.end() ? 0 : static_cast<int>(it - actionOptions.begin());
			}
			const int negativeBefore = negative;
			Wui::Combo(ctx, Wui::HashId((idBase + ".negative").c_str()),
				{ negX, RowCtrlY(negY), negW, kCtrlH }, captionNegative, actionOptions, negative, theme);
			if (negative != negativeBefore)
			{
				axis.NegativeAction = negative == 0 ? std::string()
					: actionOptions[static_cast<std::size_t>(negative)];
				changed = true;
			}
			int gamepad = 0;
			{
				const auto it = std::find(GamepadAxisOptions().begin(), GamepadAxisOptions().end(),
					axis.GamepadAxis);
				gamepad = it == GamepadAxisOptions().end() ? 0
					: static_cast<int>(it - GamepadAxisOptions().begin());
			}
			const int gamepadBefore = gamepad;
			Wui::Combo(ctx, Wui::HashId((idBase + ".gamepad").c_str()),
				{ padX, RowCtrlY(padY), 74.0f, kCtrlH }, captionGamepad, GamepadAxisOptions(), gamepad, theme);
			if (gamepad != gamepadBefore)
			{
				axis.GamepadAxis = gamepad == 0 ? std::string()
					: GamepadAxisOptions()[static_cast<std::size_t>(gamepad)];
				changed = true;
			}
			if (Wui::DragFloat(ctx, Wui::HashId((idBase + ".deadzone").c_str()),
				{ dzX, RowCtrlY(padY), 66.0f, kCtrlH }, axis.DeadZone, 0.005f, 0.0f, 0.95f, theme))
				changed = true;

			if (Wui::ButtonEx(ctx, Wui::HashId((idBase + ".delete").c_str()),
				{ delX, RowCtrlY(y), delW, kCtrlH }, delLabel, theme, true, false,
				Tr("panel.input_map.tip.delete_axis", "Delete the axis") + " '" + axis.Name + "'",
				Wui::ButtonTone::Muted))
			{
				const std::string removedAxis = axis.Name;
				m_Map.Axes().erase(m_Map.Axes().begin() + static_cast<std::ptrdiff_t>(index));
				MarkDirtyAndSave(WithName(Tr("panel.input_map.status.deleted_axis", "Deleted axis"), removedAxis));
				continue;
			}
			if (changed)
				MarkDirtyAndSave(WithName(Tr("panel.input_map.status.updated_axis", "Updated axis"), axis.Name));
			++index;
			y += rowH;
		}
		Wui::EndScrollArea(ctx);
	}

	// ================= 段 3:上下文(右) =================

	void InputMapPanel::RenderContextsSection(Wui::WuiContext& ctx, const Wui::WuiTheme& theme,
		const Wui::WuiRect& rect)
	{
		if (rect.W < kMinSectionW || rect.H < 80.0f)
			return;
		const SectionBands bands = SplitBands(rect);
		const std::string delLabel = Tr("panel.input_map.delete", "Del");
		const std::string addLabel = Tr("panel.input_map.add_context", "Add Context");
		const std::string addMapping = Tr("panel.input_map.add_mapping", "+ Mapping");
		const std::string addTrigger = Tr("panel.input_map.add_trigger", "+ Trigger");
		const float delW = ButtonW(ctx, delLabel);
		const float addW = ButtonW(ctx, addLabel);
		const float mappingW = ButtonW(ctx, addMapping);
		const float triggerW = ButtonW(ctx, addTrigger);
		const float left = bands.List.X + kPad;
		const float right = bands.List.X + bands.List.W - kPad;
		const float delX = right - delW;
		const bool headStacked = bands.List.W < 560.0f;
		const bool mappingStacked = bands.List.W < 470.0f;
		const bool triggerStacked = bands.List.W < 480.0f;
		const float mappingNameW = std::min(140.0f, std::max(80.0f, (right - left) * 0.22f));
		const LevelStyle contextLevel = ContextLevel(theme);
		const LevelStyle mappingLevel = MappingLevel(theme);
		const LevelStyle triggerLevel = TriggerLevel(theme);

		SectionHeadBlock(ctx, theme, bands.Head, Tr("panel.input_map.contexts", "Contexts"),
			m_Map.Contexts().size(), Tr("panel.input_map.contexts.hint",
				"A context is a layer of bindings; 'default' ones are pushed automatically on load."));

		// 添加框:固定在段顶(默认 consume=false / default=true —— 基础上下文最常见的形态)。
		{
			AddCardSurface(ctx, theme, bands.Add);
			const float fieldW = std::max(56.0f, bands.Add.W - addW - kGap - 12.0f);
			const Wui::TextFieldA11y a11y {
				Tr("panel.input_map.new_context.name", "New context name"),
				Tr("panel.input_map.new_context.hint", "new context name, then Add Context") };
			const bool submitted = Wui::TextField(ctx, Wui::HashId("input.context.new.name"),
				{ bands.Add.X + 4.0f, bands.Add.Y + 3.0f, fieldW, kCardH - 6.0f }, m_NewContextName, theme,
				nullptr, &a11y);
			const bool clicked = Wui::ButtonEx(ctx, Wui::HashId("input.context.new.add"),
				{ bands.Add.X + 4.0f + fieldW + kGap, bands.Add.Y + 3.0f, addW, kCardH - 6.0f }, addLabel,
				theme, !m_NewContextName.empty(), false, a11y.Placeholder, Wui::ButtonTone::Action);
			if ((submitted || clicked) && !m_NewContextName.empty())
			{
				Gameplay::InputMappingContext created(m_NewContextName, 0, false);
				created.SetAutoPush(true);
				m_Map.Contexts().push_back(std::move(created));
				MarkDirtyAndSave(WithName(Tr("panel.input_map.status.added_context", "Added context"),
					m_NewContextName));
				m_NewContextName.clear();
			}
		}

		const std::vector<std::string> actionOptions = BuildActionOptions(m_Map);
		Wui::BeginScrollArea(ctx, bands.List, ContextsListHeight(bands.List.W), m_ScrollContexts, theme,
			Wui::HashId("input.scroll.contexts"));
		float y = bands.List.Y - m_ScrollContexts + kGap;
		if (m_Map.Contexts().empty())
		{
			Wui::EmptyState(ctx, { left, y, right - left, kEmptyH }, "",
				Tr("panel.input_map.empty.contexts.title", "No contexts yet"),
				Tr("panel.input_map.empty.contexts.hint",
					"Contexts let the same key mean different things (Gameplay / Menu / Vehicle)."),
				"", 0, theme);
			y += kEmptyH;
		}
		for (std::size_t index = 0; index < m_Map.Contexts().size();)
		{
			Gameplay::InputMappingContext& context = m_Map.Contexts()[index];
			const std::string idBase = "input.context." + std::to_string(index);
			bool changed = false;
			const float headH = headStacked ? (kRowH * 2.0f + kGap) : kRowH;
			const float mappingH = mappingStacked ? (kRowH * 2.0f + kGap) : kRowH;
			const float triggerH = triggerStacked ? (kRowH * 2.0f + kGap) : kRowH;

			// 一条上下文 = 一张卡片(卡片高度与列表高度共用同一个事实源)。
			const float cardHeight = ContextCardHeight(context, bands.List.W);
			const Wui::WuiRect card { bands.List.X, y - 4.0f, bands.List.W, cardHeight };
			LevelSurface(ctx, theme, card, contextLevel, false);
			y += 6.0f;

			// ---- 上下文头:名字 / 优先级 / 独占输入 / 默认启用 / 删除 ----
			const float headY = y;
			Wui::Label(ctx, { left, RowCtrlY(headY) + 4.0f }, context.GetName(), theme.Text, 13.0f);
			const float prioX = left + mappingNameW + kPad;
			if (headStacked)
			{
				Wui::Label(ctx, { left, RowCtrlY(LineY(headY, 1)) + 4.0f },
					Tr("panel.input_map.priority", "priority"), theme.TextMuted, 10.5f);
			}
			else
			{
				Wui::Label(ctx, { prioX, RowCtrlY(headY) + 4.0f },
					Tr("panel.input_map.priority", "priority"), theme.TextMuted, 10.5f);
			}
			const float prioFieldX = headStacked ? (left + 54.0f) : (prioX + 50.0f);
			const float prioY = headStacked ? LineY(headY, 1) : headY;
			int64_t priority = context.GetPriority();
			if (Wui::DragInt(ctx, Wui::HashId((idBase + ".priority").c_str()),
				{ prioFieldX, RowCtrlY(prioY), 56.0f, kCtrlH }, priority, -100, 100, theme))
			{
				context.SetPriority(static_cast<int32_t>(priority));
				changed = true;
			}
			bool consume = context.ConsumesInput();
			if (Wui::Checkbox(ctx, Wui::HashId((idBase + ".consume").c_str()),
				{ prioFieldX + 62.0f, RowCtrlY(prioY), 110.0f, kCtrlH },
				Tr("panel.input_map.consume", "consume"), consume, theme))
			{
				context.SetConsumeInput(consume);
				changed = true;
			}
			bool autoPush = context.IsAutoPush();
			if (Wui::Checkbox(ctx, Wui::HashId((idBase + ".default").c_str()),
				{ prioFieldX + 176.0f, RowCtrlY(prioY), 100.0f, kCtrlH },
				Tr("panel.input_map.default", "default"), autoPush, theme))
			{
				context.SetAutoPush(autoPush);
				changed = true;
			}
			if (Wui::ButtonEx(ctx, Wui::HashId((idBase + ".delete").c_str()),
				{ delX, RowCtrlY(headY), delW, kCtrlH }, delLabel, theme, true, false,
				Tr("panel.input_map.tip.delete_context", "Delete the context") + " '" + context.GetName() + "'",
				Wui::ButtonTone::Muted))
			{
				const std::string removedContext = context.GetName();
				m_Map.Contexts().erase(m_Map.Contexts().begin() + static_cast<std::ptrdiff_t>(index));
				MarkDirtyAndSave(WithName(Tr("panel.input_map.status.deleted_context", "Deleted context"),
					removedContext));
				continue;
			}
			if (changed)
				MarkDirtyAndSave(WithName(Tr("panel.input_map.status.updated_context", "Updated context"),
					context.GetName()));
			// 一级标题右侧挂"有几个映射":层级不只靠缩进,也把这一层的规模写在脸上。
			// 放在头部控件**之后**算位置:头部中间是 priority/consume/default,右侧是删除 ——
			// 空位不够时宁可不画,也不让胶囊压到勾选框或删除按钮上。
			if (!headStacked)
			{
				const std::string count = std::to_string(context.GetMappings().size()) + " "
					+ Tr("panel.input_map.mapping.count", "mapping(s)");
				const float chipW = ChipWidth(ctx, count);
				const float defaultEndX = prioFieldX + 176.0f + 100.0f;
				const float chipX = std::max(defaultEndX + kGap, delX - kGap - chipW);
				if (chipX + chipW <= delX - kGap + 0.5f)
					Chip(ctx, theme, chipX, RowCtrlY(headY) + kCtrlH * 0.5f, chipW, count,
						theme.PanelBg, theme.TextMuted);
			}
			y += headH + kGap;

			// ---- 映射(缩进一层)----
			std::vector<Gameplay::ActionBindingConfig>& mappings = context.Mappings();
			bool mappingRemoved = false;
			for (std::size_t mappingIndex = 0; mappingIndex < mappings.size(); ++mappingIndex)
			{
				Gameplay::ActionBindingConfig& mapping = mappings[mappingIndex];
				const std::string mappingIdBase = idBase + ".mapping." + std::to_string(mappingIndex);
				const float indent = left + kIndent;
				const float line2 = LineY(y, 1);
				// 二级(映射)自己一个盒子:同级之间因此有明确边界(靠"靠近的是一伙的"分组)。
				const float blockH = MappingBlockHeight(mapping, bands.List.W);
				const float boxLeft = left + kIndent - 9.0f;
				const float boxRight = delX - kGap;
				const Wui::WuiRect mappingBox { boxLeft, y - 4.0f, std::max(24.0f, boxRight - boxLeft),
					blockH - 3.0f };
				LevelSurface(ctx, theme, mappingBox, mappingLevel, true);

				int actionIndex = 0;
				{
					const auto it = std::find(actionOptions.begin(), actionOptions.end(), mapping.ActionName);
					actionIndex = it == actionOptions.end() ? 0 : static_cast<int>(it - actionOptions.begin());
				}
				const int actionIndexBefore = actionIndex;
				Wui::Combo(ctx, Wui::HashId((mappingIdBase + ".action").c_str()),
					{ indent, RowCtrlY(y), mappingNameW, kCtrlH },
					Tr("panel.input_map.col.action", "action"), actionOptions, actionIndex, theme);
				if (actionIndex != actionIndexBefore)
				{
					mapping.ActionName = actionIndex == 0 ? std::string()
						: actionOptions[static_cast<std::size_t>(actionIndex)];
					mapping.Action = StringPool::Get().InternName(mapping.ActionName);
					MarkDirtyAndSave(WithName(
						Tr("panel.input_map.status.retargeted_mapping", "Retargeted a mapping in"),
						context.GetName()));
				}
				// 这条映射绑了什么 / 挂了几个触发器:两个胶囊一眼看完(胶囊=已有值,按钮=操作)。
				const std::string bindingLabel = BindingLabel(mapping.Binding);
				const std::string triggerCount = std::to_string(mapping.Triggers.size()) + " "
					+ Tr("panel.input_map.trigger.count", "trigger(s)");
				const float chipY = mappingStacked ? RowCtrlY(line2) : RowCtrlY(y);
				const float chipX = mappingStacked ? indent : (indent + mappingNameW + kPad);
				Chip(ctx, theme, chipX, chipY + kCtrlH * 0.5f, ChipWidth(ctx, bindingLabel), bindingLabel,
					theme.ActiveBg, theme.Text);
				const float countX = chipX + ChipWidth(ctx, bindingLabel) + kGap;
				Chip(ctx, theme, countX, chipY + kCtrlH * 0.5f, ChipWidth(ctx, triggerCount), triggerCount,
					theme.PanelBg, mapping.Triggers.empty() ? theme.TextDisabled : theme.TextMuted);

				const float triggerX = mappingStacked ? (right - triggerW) : (delX - kGap - triggerW);
				const float triggerY = mappingStacked ? line2 : y;
				if (Wui::ButtonEx(ctx, Wui::HashId((mappingIdBase + ".addtrigger").c_str()),
					{ triggerX, RowCtrlY(triggerY), triggerW, kCtrlH }, addTrigger, theme,
					!mapping.ActionName.empty(), false,
					Tr("panel.input_map.tip.add_trigger", "Add a trigger (hold / tap / double-tap / pulse ...)"),
					Wui::ButtonTone::Action))
				{
					mapping.Triggers.push_back(Gameplay::InputTrigger::MakePressed());
					MarkDirtyAndSave(WithName(
						Tr("panel.input_map.status.added_trigger", "Added a trigger to"), context.GetName()));
				}
				if (Wui::ButtonEx(ctx, Wui::HashId((mappingIdBase + ".remove").c_str()),
					{ delX, RowCtrlY(y), delW, kCtrlH }, delLabel, theme, true, false,
					Tr("panel.input_map.tip.delete_mapping", "Remove this mapping from") + " '"
						+ context.GetName() + "'", Wui::ButtonTone::Muted))
				{
					mappings.erase(mappings.begin() + static_cast<std::ptrdiff_t>(mappingIndex));
					MarkDirtyAndSave(WithName(
						Tr("panel.input_map.status.removed_mapping", "Removed a mapping from"),
						context.GetName()));
					mappingRemoved = true;
					break;
				}
				y += mappingH + mappingLevel.SiblingGap;

				// ---- 触发器(再缩进一层)----
				bool triggerRemoved = false;
				for (std::size_t triggerIndex = 0; triggerIndex < mapping.Triggers.size(); ++triggerIndex)
				{
					Gameplay::InputTrigger& trigger = mapping.Triggers[triggerIndex];
					const std::string triggerIdBase = mappingIdBase + ".trigger." + std::to_string(triggerIndex);
					const float triggerIndent = indent + kIndent;
					bool triggerChanged = false;
					// 三级(触发器)再内缩一层、底色更深 —— 与父级(映射)一眼分得开。
					const float triggerBoxLeft = boxLeft + kIndent;
					LevelSurface(ctx, theme,
						{ triggerBoxLeft, y - 3.0f, std::max(20.0f, boxRight - triggerBoxLeft),
							triggerH + 6.0f },
						triggerLevel, true);

					int typeIndex = TriggerTypeIndex(trigger.Type);
					const int typeIndexBefore = typeIndex;
					Wui::Combo(ctx, Wui::HashId((triggerIdBase + ".type").c_str()),
						{ triggerIndent, RowCtrlY(y), 110.0f, kCtrlH }, "type", TriggerTypeOptions(),
						typeIndex, theme);
					if (typeIndex != typeIndexBefore)
					{
						trigger.Type = TriggerTypeFromIndex(typeIndex);
						triggerChanged = true;
					}
					// 只显示该类型真正消费的参数;标签在字段**左侧**固定槽位(写右边会被下一个字段压掉)。
					float paramX = triggerStacked ? triggerIndent : (triggerIndent + 118.0f);
					const float paramY = triggerStacked ? LineY(y, 1) : y;
					const auto showParam = [&](const char* suffix, const std::string& label, float& value)
					{
						Wui::Label(ctx, { paramX, RowCtrlY(paramY) + 4.0f }, label, theme.TextMuted, 10.5f);
						paramX += 54.0f;
						if (Wui::DragFloat(ctx, Wui::HashId((triggerIdBase + "." + suffix).c_str()),
							{ paramX, RowCtrlY(paramY), 70.0f, kCtrlH }, value, 0.005f, 0.0f, 2.0f, theme))
							triggerChanged = true;
						paramX += 70.0f + 14.0f;
					};
					switch (trigger.Type)
					{
					case Gameplay::TriggerType::Hold:
					case Gameplay::TriggerType::Tap:
						showParam("duration", Tr("panel.input_map.param.duration", "duration"), trigger.Duration);
						break;
					case Gameplay::TriggerType::DoubleTap:
						showParam("interval", Tr("panel.input_map.param.interval", "interval"), trigger.Interval);
						showParam("duration", Tr("panel.input_map.param.duration", "duration"), trigger.Duration);
						break;
					case Gameplay::TriggerType::Pulse:
						showParam("interval", Tr("panel.input_map.param.interval", "interval"), trigger.Interval);
						break;
					default:
						break;
					}
					if (triggerChanged)
						MarkDirtyAndSave(WithName(
							Tr("panel.input_map.status.updated_trigger", "Updated a trigger in"),
							context.GetName()));
					if (Wui::ButtonEx(ctx, Wui::HashId((triggerIdBase + ".remove").c_str()),
						{ delX, RowCtrlY(y), delW, kCtrlH }, delLabel, theme, true, false,
						Tr("panel.input_map.tip.delete_trigger", "Remove this trigger"), Wui::ButtonTone::Muted))
					{
						mapping.Triggers.erase(mapping.Triggers.begin()
							+ static_cast<std::ptrdiff_t>(triggerIndex));
						MarkDirtyAndSave(WithName(
							Tr("panel.input_map.status.removed_trigger", "Removed a trigger from"),
							context.GetName()));
						triggerRemoved = true;
						break;
					}
					y += triggerH + triggerLevel.SiblingGap;
				}
				if (triggerRemoved)
				{
					--mappingIndex;
					continue;
				}
				y += kRowH + kGap;                            // "+ Trigger" 行(按钮画在映射行上)
			}
			if (mappingRemoved)
				continue;                                    // 上下文容器已变

			if (Wui::ButtonEx(ctx, Wui::HashId((idBase + ".addmapping").c_str()),
				{ left + kIndent, RowCtrlY(y), mappingW, kCtrlH }, addMapping, theme,
				m_Map.Actions().size() > 0, false,
				Tr("panel.input_map.tip.add_mapping", "Add a mapping (which action this context binds)"),
				Wui::ButtonTone::Action))
			{
				Gameplay::ActionBindingConfig created;
				created.ActionName = m_Map.Actions().front().Name;
				created.Action = m_Map.Actions().front().Id;
				created.Binding = { Gameplay::InputDevice::Key, KeyCodes::Space };
				mappings.push_back(created);
				MarkDirtyAndSave(WithName(Tr("panel.input_map.status.added_mapping", "Added a mapping to"),
					context.GetName()));
			}
			y += kRowH + kGap;
			y += 6.0f;
			// 正常路径必须推进下标:上面两条 `continue`(删除上下文 / 删除映射)刻意**跳过**自增
			// (容器已变,同一位置已换成下一个元素);这里若忘了自增,本循环会永远停在同一个上下文上,
			// 每帧空转 100% CPU —— 表现就是"一打开输入映射面板就白屏卡死"。与 actions / axes
			// 两个同级循环同一形态(`++index;`),不要漏。
			++index;
		}
		Wui::EndScrollArea(ctx);
	}

	// ================= 页面 =================

	void InputMapPanel::OnRender(Wui::WuiContext& ctx, const Wui::WuiRect& rect, PanelHost& host)
	{
		const Wui::WuiTheme& theme = host.Theme();
		EnsureLoaded();

		Wui::PanelBackground(ctx, rect, theme.PanelBg, 0.0f);

		// ---- 工具条(固定):当前文件 + 保存。放在滚动区**外面**,内容滚到哪儿都看得见。----
		const Wui::WuiRect toolbar { rect.X, rect.Y, rect.W, kToolbarH };
		Wui::PanelBackground(ctx, toolbar, theme.PanelHeader, 0.0f);
		const std::string saveLabel = Tr("panel.input_map.save", "Save Input Map");
		const float saveW = ButtonW(ctx, saveLabel);
		const float saveX = toolbar.X + toolbar.W - kPad - saveW;
		if (Wui::ButtonEx(ctx, Wui::HashId("input.save"),
			{ saveX, toolbar.Y + 5.0f, saveW, kToolbarH - 10.0f }, saveLabel, theme, !m_Path.empty(), true,
			m_Path.empty()
				? Tr("panel.input_map.save.tip_none", "No content root to save into; open a project first")
				: Tr("panel.input_map.save.tip", "Write it back to assets/input.weinput")))
			Save();
		if (!m_Path.empty())
		{
			const float budget = saveX - kPad - (toolbar.X + kPad);
			if (budget > 40.0f)
			{
				const std::string fileName = Wui::EllipsizeMiddleToWidth(ctx, m_Path.filename().string(),
					budget, theme.FontSizeSmall);
				Wui::Label(ctx, { saveX - kPad - ctx.MeasureTextWidth(fileName, theme.FontSizeSmall),
					toolbar.Y + (kToolbarH - theme.FontSizeSmall) * 0.5f }, fileName, theme.TextMuted,
					theme.FontSizeSmall);
			}
		}

		// ---- 状态行(固定页脚)----
		const Wui::WuiRect footer { rect.X, rect.Y + rect.H - kFooterH, rect.W, kFooterH };
		Wui::PanelBackground(ctx, footer, theme.PanelHeader, 0.0f);
		{
			const Wui::WuiColor dot = m_StatusError ? theme.Danger : theme.Success;
			Wui::PanelBackground(ctx, { footer.X + kPad, footer.Y + kFooterH * 0.5f - 4.0f, 8.0f, 8.0f },
				dot, 4.0f);
			const float textBudget = std::max(60.0f, footer.W - kPad * 2.0f - 16.0f);
			Wui::Label(ctx, { footer.X + kPad + 14.0f, footer.Y + 5.0f },
				Wui::EllipsizeMiddleToWidth(ctx, m_Status, textBudget, theme.FontSizeSmall), theme.Text,
				theme.FontSizeSmall);
		}

		// ---- 三段并排(左 / 中 / 右)----
		const Wui::WuiRect body { rect.X, rect.Y + kToolbarH + 6.0f, rect.W,
			std::max(60.0f, rect.H - kToolbarH - kFooterH - 10.0f) };
		// 一列时按各段内容高度分配(此时段宽 = body.W,与下面的高度计算同一口径)。
		float wantedActions = 0.0f;
		float wantedAxes = 0.0f;
		float wantedContexts = 0.0f;
		if (body.W < 620.0f)
		{
			const float fixedBands = kHeadH + kGap + kCardH + kGap + kCaptionH;
			wantedActions = fixedBands + ActionsListHeight(body.W);
			wantedAxes = fixedBands + AxesListHeight(body.W);
			wantedContexts = fixedBands + ContextsListHeight(body.W);
		}
		const SectionLayouts layout = ComputeSectionLayouts(body, wantedActions, wantedAxes, wantedContexts);
		RenderActionsSection(ctx, theme, layout.Actions);
		RenderAxesSection(ctx, theme, layout.Axes);
		RenderContextsSection(ctx, theme, layout.Contexts);

		// ---------------- 按键捕获 ----------------
		// 捕获读**本帧真实按下的键**(`WuiInputState::KeyPressed`,引擎 KeyCodes = GLFW 空间;
		// 与编辑器文本/快捷键同一份数据),不用候选表 —— 任意键都能录,键码空间天然正确。
		// 用 KeyPressed(本帧沿)而不是按住状态:一次按下只录一个键,长按不会反复覆盖。
		// M39 的教训:此前那份硬编码候选表把 VK 码混进了 GLFW 码空间,其中 25 项恒不匹配、
		// 其余常用键压根不在表内 —— 表现就是"很多键位录不上"。
		if (!m_RebindingAction.empty() || !m_AppendKeyAction.empty())
		{
			const bool replacing = !m_RebindingAction.empty();
			const std::string target = replacing ? m_RebindingAction : m_AppendKeyAction;
			if (ctx.IsKeyPressed(KeyCodes::Escape))
			{
				m_RebindingAction.clear();
				m_AppendKeyAction.clear();
				m_Status = Tr("panel.input_map.status.rebind_cancelled", "capture cancelled (Esc)");
				m_StatusError = false;
			}
			else
			{
				for (const uint32_t code : ctx.Input().KeyPressed)
				{
					Gameplay::InputAction* action = FindAction(m_Map, target);
					if (!action)
						break;
					const int keyCode = static_cast<int>(code);
					if (replacing)
					{
						action->Bindings.clear();
						action->Bindings.push_back({ Gameplay::InputDevice::Key, keyCode });
					}
					else
					{
						action->Bindings.push_back({ Gameplay::InputDevice::Key, keyCode });
					}
					m_RebindingAction.clear();
					m_AppendKeyAction.clear();
					MarkDirtyAndSave(WithName(replacing
						? Tr("panel.input_map.status.replaced_binding", "Replaced the binding of")
						: Tr("panel.input_map.status.added_binding", "Added a binding to"), target));
					break;
				}
			}
		}
	}
}
