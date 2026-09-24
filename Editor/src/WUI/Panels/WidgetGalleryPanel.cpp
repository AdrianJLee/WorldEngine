#include "wldpch.h"
#include "WidgetGalleryPanel.h"

#include "../../EditorPreferences.h"

#include "World/Core/KeyCodes.h"
#include "World/WUI/WuiAccessibility.h"
#include "World/WUI/WuiLocalization.h"
#include "World/WUI/WuiWidgets.h"
#include "World/WUI/Widgets/WuiChrome.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <sstream>
#include <system_error>

// 构建期常量由 CMake 注入(见根 CMakeLists 的 WLD_OUTPUT_DIR);单独做语法检查时给个兜底,
// 不影响正常构建(与其它面板依赖 WLD_PROJECT_DIR/WLD_LOCAL_DIR 的用法一致)。
#ifndef WLD_OUTPUT_DIR
#define WLD_OUTPUT_DIR "build/x64-Debug/"
#endif

namespace World
{
	namespace
	{
		// ---- 稳定 a11y id 约定 ----
		// 工作台里每个可交互件都是 `wui.workbench.<area>.<name>`;控件自己登记的节点
		// (Button/Checkbox/Combo/TextField/DragFloat)自动带上这套 id,工作台另行手工登记
		// 左树行、画布、状态行等"不是控件"的节点。探针按 id 驱动,不依赖文案(语言无关)。
		constexpr const char* kCaptureButtonId = "wui.workbench.btn.capture";
		constexpr const char* kApproveButtonId = "wui.workbench.btn.approve";
		constexpr const char* kTreeSearchId = "wui.workbench.tree.search";
		constexpr const char* kTreeRowPrefix = "wui.workbench.tree.row.";
		constexpr const char* kTreeListId = "wui.workbench.tree.list";
		constexpr const char* kCanvasId = "wui.workbench.canvas";
		constexpr const char* kCanvasLabelId = "wui.workbench.canvas.label";
		constexpr const char* kStateComboId = "wui.workbench.prop.__state";
		constexpr const char* kStatusId = "wui.workbench.status";
		constexpr const char* kInfoId = "wui.workbench.info";
		constexpr const char* kPropLabelPrefix = "wui.workbench.prop.";

		constexpr float kMinCanvasSize = 120.0f;
		constexpr float kMaxCanvasSize = 640.0f;

		void RegisterWorkbenchNode(Wui::WuiId id, const char* kind, const Wui::WuiRect& rect,
			const std::string& label, const std::string& value = std::string(),
			bool enabled = true, bool interactive = true)
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
			node.Interactive = interactive;
			Wui::WuiAccessibility::Get().Register(node);
		}

		// 按可用宽度裁剪(省略号在末尾);命中测试与 a11y 文案都用裁剪后的字符串。
		std::string ClipText(const Wui::WuiContext& ctx, const std::string& text, float width,
			float fontSize)
		{
			if (text.empty() || width <= 8.0f)
				return std::string();
			if (ctx.MeasureTextWidth(text, fontSize) <= width)
				return text;
			std::string out = text;
			while (!out.empty())
			{
				out.pop_back();
				while (!out.empty() && (static_cast<unsigned char>(out.back()) & 0xC0) == 0x80)
					out.pop_back();
				if (ctx.MeasureTextWidth(out + "...", fontSize) <= width)
					break;
			}
			return out.empty() ? std::string() : out + "...";
		}

		float SafeFloat(const std::string& text, float fallback)
		{
			try
			{
				return std::stof(text);
			}
			catch (...)
			{
				return fallback;
			}
		}

		std::string FormatFloat(float value)
		{
			char buffer[32] = {};
			std::snprintf(buffer, sizeof(buffer), "%.3f", value);
			return buffer;
		}

		// ---- 落盘目录:`<build>/wui-workbench`(与 plan P0-4 的 `build/wui-workbench/**` 同义)----
		// 注意:引擎的 Exe 运行时会自己把工作目录切到 WLD_OUTPUT_DIR(实测:相对路径会落在
		// build/x64-Debug 下),因此这里用**构建期常量** WLD_OUTPUT_DIR 解析,不依赖 cwd。
		const std::filesystem::path& WorkbenchDir()
		{
			static const std::filesystem::path directory =
				std::filesystem::path(std::string(WLD_OUTPUT_DIR)) / "wui-workbench";
			return directory;
		}

		void EnsureWorkbenchDir()
		{
			std::error_code error;
			std::filesystem::create_directories(WorkbenchDir(), error);
		}

		std::string JsonEscape(const std::string& text)
		{
			std::string out;
			out.reserve(text.size() + 8);
			for (const char ch : text)
			{
				switch (ch)
				{
				case '"': out += "\\\""; break;
				case '\\': out += "\\\\"; break;
				case '\n': out += "\\n"; break;
				case '\r': out += "\\r"; break;
				case '\t': out += "\\t"; break;
				default:
					if (static_cast<unsigned char>(ch) < 0x20)
					{
						char buffer[8] = {};
						std::snprintf(buffer, sizeof(buffer), "\\u%04x", static_cast<unsigned char>(ch));
						out += buffer;
					}
					else
					{
						out.push_back(ch);
					}
					break;
				}
			}
			return out;
		}

		std::string ReadFirstLine(const std::filesystem::path& path)
		{
			std::ifstream file(path, std::ios::binary);
			if (!file)
				return std::string();
			std::string line;
			std::getline(file, line);
			while (!line.empty() && (line.back() == '\r' || line.back() == '\n'))
				line.pop_back();
			return line;
		}

		// Approve 的 `<commit>`:优先读 .git(git 目录/文件两种形态都认),读不到记 "unknown"
		// (报告里说明口径,不假装知道提交号)。
		std::string ResolveGitCommit()
		{
			std::error_code error;
			std::filesystem::path candidate(".git");
			if (std::filesystem::is_directory(candidate, error))
			{
				// 普通 checkout
			}
			else if (std::filesystem::exists(candidate, error))
			{
				const std::string pointer = ReadFirstLine(candidate);
				const std::string prefix = "gitdir:";
				if (pointer.rfind(prefix, 0) != 0)
					return "unknown";
				std::string target = pointer.substr(prefix.size());
				while (!target.empty() && target.front() == ' ')
					target.erase(target.begin());
				candidate = std::filesystem::path(target);
				if (candidate.is_relative())
					candidate = std::filesystem::path(".git").parent_path() / candidate;
			}
			else
			{
				return "unknown";
			}
			const std::string head = ReadFirstLine(candidate / "HEAD");
			const std::string refPrefix = "ref:";
			if (head.rfind(refPrefix, 0) != 0)
				return head.empty() ? std::string("unknown") : head.substr(0, 12);
			std::string ref = head.substr(refPrefix.size());
			while (!ref.empty() && ref.front() == ' ')
				ref.erase(ref.begin());
			const std::string hash = ReadFirstLine(candidate / ref);
			if (!hash.empty())
				return hash.substr(0, 12);
			std::ifstream packed(candidate / "packed-refs", std::ios::binary);
			std::string line;
			while (std::getline(packed, line))
			{
				const size_t space = line.find(' ');
				if (space == std::string::npos)
					continue;
				if (line.substr(space + 1) == ref)
					return line.substr(0, std::min<size_t>(12, space));
			}
			return "unknown";
		}

		// SizeNotes 里的 preferred WxH → 单件画布尺寸(解析不出来就用 240x120)。
		std::pair<float, float> SizeNotesToSize(const std::string& notes)
		{
			const size_t marker = notes.find("preferred");
			const size_t from = marker == std::string::npos ? 0 : marker;
			float width = 240.0f;
			float height = 120.0f;
			const size_t x = notes.find('x', from);
			if (x != std::string::npos)
			{
				size_t start = x;
				while (start > 0 && (std::isdigit(static_cast<unsigned char>(notes[start - 1]))
					|| notes[start - 1] == '.'))
					--start;
				const std::string widthText = notes.substr(start, x - start);
				size_t end = x + 1;
				while (end < notes.size() && (std::isdigit(static_cast<unsigned char>(notes[end]))
					|| notes[end] == '.'))
					++end;
				const std::string heightText = notes.substr(x + 1, end - x - 1);
				if (!widthText.empty())
					width = SafeFloat(widthText, width);
				if (!heightText.empty())
					height = SafeFloat(heightText, height);
			}
			width = std::clamp(width, kMinCanvasSize, kMaxCanvasSize);
			height = std::clamp(height, 28.0f, kMaxCanvasSize);
			return { width, height };
		}

		const char* StatusText(Wui::WuiComponentStatus status)
		{
			switch (status)
			{
			case Wui::WuiComponentStatus::Approved: return "Approved";
			case Wui::WuiComponentStatus::Deprecated: return "Deprecated";
			default: return "Draft";
			}
		}

		Wui::WuiColor StatusColor(Wui::WuiComponentStatus status, const Wui::WuiTheme& theme)
		{
			switch (status)
			{
			case Wui::WuiComponentStatus::Approved: return theme.Success;
			case Wui::WuiComponentStatus::Deprecated: return theme.Danger;
			default: return theme.Warning;
			}
		}

		// showcase 是否往 overlay 层画了**整窗大小**的矩形 = 模态遮罩的指纹(视角无关,不看组件名)。
		// 只有这种组件需要"专用舞台":它的遮挡区会把工作台整块面板的命中打死(实测踩过)。
		bool DetectsFullWindowOverlay(const Wui::WuiContext& ctx, size_t from)
		{
			const glm::vec2 viewport = ctx.ViewportSize();
			const float windowArea = std::max(1.0f, viewport.x * viewport.y);
			const std::vector<Wui::WuiDrawCommand>& overlay = ctx.OverlayCommands();
			for (size_t index = from; index < overlay.size(); ++index)
			{
				const Wui::WuiDrawCommand& command = overlay[index];
				if (command.Kind != Wui::WuiDrawKind::Rect)
					continue;
				if (command.Rect.W * command.Rect.H >= windowArea * 0.6f)
					return true;
			}
			return false;
		}

		// WUI-P1b:状态按钮行的排版。**同一份矩形既用来估算滚动区高度、又用来画**,
		// 避免"估算一处、绘制另一处"慢慢走偏。原点取传入 rect 的左上角,宽度用 rect.W。
		std::vector<Wui::WuiRect> StateChipRects(
			const std::vector<Wui::WuiComponentState>& states, const Wui::WuiRect& origin, float rowH)
		{
			std::vector<Wui::WuiRect> rects;
			if (states.empty())
				return rects;
			const float gap = 4.0f;
			float x = origin.X + 2.0f;
			float y = origin.Y;
			for (const Wui::WuiComponentState& state : states)
			{
				std::string text = state.Id;
				if (text.size() > 14)
					text.resize(14);
				const float width = std::clamp(14.0f + static_cast<float>(text.size()) * 6.2f, 54.0f, 104.0f);
				if (x > origin.X + 2.0f && x + width > origin.X + origin.W - 2.0f)
				{
					x = origin.X + 2.0f;
					y += rowH + gap;
				}
				rects.push_back({ x, y, width, rowH });
				x += width + gap;
			}
			return rects;
		}

	}

	WidgetGalleryPanel::WbLayout WidgetGalleryPanel::ComputeLayout(const Wui::WuiRect& rect,
		const Wui::WuiTheme& theme)
	{
		WbLayout layout;
		const float pad = theme.Pad > 0.0f ? theme.Pad : 8.0f;
		const float innerX = rect.X + pad;
		const float innerW = std::max(120.0f, rect.W - 2.0f * pad);
		float y = rect.Y + pad;

		layout.TopBar = { innerX, y, innerW, 26.0f };
		// 顶栏在窄窗换行成两行(语言/长文本/重置下移一行),避免控件互相压住。
		layout.TopBar.H = innerW < 900.0f ? 56.0f : 26.0f;
		y += layout.TopBar.H + 6.0f;
		layout.InfoBar = { innerX, y, innerW, 18.0f };
		y += 18.0f + 8.0f;

		const float actionH = 30.0f;
		layout.Actions = { innerX, rect.Y + rect.H - pad - actionH, innerW, actionH };
		layout.Body = { innerX, y, innerW, std::max(80.0f, layout.Actions.Y - 8.0f - y) };

		layout.Stacked = innerW < 900.0f;
		if (!layout.Stacked)
		{
			const float treeW = std::max(180.0f, innerW * 0.26f);
			const float propsW = std::max(240.0f, innerW * 0.30f);
			layout.Tree = { innerX, layout.Body.Y, treeW, layout.Body.H };
			const float canvasX = innerX + treeW + 10.0f;
			layout.Canvas = { canvasX, layout.Body.Y,
				std::max(180.0f, innerW - treeW - propsW - 20.0f), layout.Body.H };
			layout.Props = { layout.Canvas.X + layout.Canvas.W + 10.0f, layout.Body.Y, propsW,
				layout.Body.H };
		}
		else
		{
			// 窄窗退化单列:三段共用同一宽度,外层滚动(见 OnRender)。
			const float treeH = std::clamp(layout.Body.H * 0.34f, 120.0f, 240.0f);
			const float canvasH = std::max(240.0f, layout.Body.H * 0.5f);
			const float propsH = std::max(220.0f, layout.Body.H * 0.6f);
			layout.Tree = { innerX, y, innerW, treeH };
			layout.Canvas = { innerX, y + treeH + 10.0f, innerW, canvasH };
			layout.Props = { innerX, y + treeH + 10.0f + canvasH + 10.0f, innerW, propsH };
		}
		return layout;
	}

	void WidgetGalleryPanel::DrawTopBar(Wui::WuiContext& ctx, const WbLayout& layout,
		const Wui::WuiTheme& theme)
	{
		const Wui::WuiRect bar = layout.TopBar;
		Wui::PanelBackground(ctx, bar, theme.PanelHeader, theme.Radius);
		const bool twoRows = bar.H > 40.0f;

		float x = bar.X + 8.0f;
		float rowY = bar.Y + 2.0f;
		const float buttonH = twoRows ? 24.0f : bar.H - 4.0f;

		Wui::Label(ctx, { x, rowY + 5.0f }, Wui::Tr("workbench.global.density", "Density"),
			theme.TextMuted, 12.0f);
		x += 56.0f;
		if (Wui::Button(ctx, Wui::HashId("wui.workbench.btn.density.comfortable"),
			{ x, rowY, 92.0f, buttonH },
			Wui::Tr("workbench.density.comfortable", "Comfortable"), theme))
		{
			m_DensityIndex = 0;
			ctx.RecordOp("gallery", "global", "Density", "comfortable");
		}
		x += 96.0f;
		if (Wui::Button(ctx, Wui::HashId("wui.workbench.btn.density.compact"),
			{ x, rowY, 74.0f, buttonH },
			Wui::Tr("workbench.density.compact", "Compact"), theme))
		{
			m_DensityIndex = 1;
			ctx.RecordOp("gallery", "global", "Density", "compact");
		}
		x += 82.0f;

		Wui::Label(ctx, { x, rowY + 5.0f }, Wui::Tr("workbench.global.scale", "UI scale"),
			theme.TextMuted, 12.0f);
		x += 52.0f;
		static const float kScales[3] = { 1.0f, 1.25f, 1.5f };
		static const char* kScaleLabels[3] = { "100%", "125%", "150%" };
		for (int index = 0; index < 3; ++index)
		{
			const std::string id = std::string("wui.workbench.btn.scale.") + std::to_string(index);
			if (Wui::Button(ctx, Wui::HashId(id.c_str()), { x, rowY, 56.0f, buttonH },
				kScaleLabels[index], theme))
			{
				m_UiScaleIndex = index;
				Editor::EditorPreferences::Get().SetUiScale(kScales[index]);
				Wui::SetUiScale(kScales[index]);
				ctx.RecordOp("gallery", "global", "UiScale", kScaleLabels[index]);
			}
			x += 60.0f;
		}
		x += 6.0f;
		if (twoRows)
		{
			// 第二行:语言 / 长文本压力 / 重置。
			x = bar.X + 8.0f;
			rowY = bar.Y + 30.0f;
		}

		m_LanguageOptions = { "en", "zh-CN" };
		Wui::Label(ctx, { x, rowY + 5.0f }, Wui::Tr("workbench.global.language", "Language"),
			theme.TextMuted, 12.0f);
		x += 54.0f;
		const Wui::WuiId languageComboId = Wui::HashId("wui.workbench.btn.language");
		if (Wui::Combo(ctx, languageComboId, { x, rowY, 96.0f, buttonH },
			std::string(), m_LanguageOptions, m_LanguageIndex, theme))
		{
			m_LanguageIndex = std::clamp(m_LanguageIndex, 0,
				static_cast<int>(m_LanguageOptions.size()) - 1);
			const std::string language = m_LanguageOptions[static_cast<size_t>(m_LanguageIndex)];
			Editor::EditorPreferences::Get().SetLanguage(language);
			Wui::SetLanguage(language);
			ctx.RecordOp("gallery", "global", "Language", language);
		}
		if (ctx.IsPopupOpen(languageComboId))
			m_PopupOpenNow = true;   // 弹层开着:↑/↓ 归它,组件树不抢键
		x += 104.0f;

		if (Wui::Button(ctx, Wui::HashId("wui.workbench.btn.longtext"),
			{ x, rowY, 132.0f, buttonH },
			(m_LongText ? "[x] " : "[ ] ") + Wui::Tr("workbench.global.long_text", "Long text stress"),
			theme))
		{
			m_LongText = !m_LongText;
			ctx.RecordOp("gallery", "global", "LongText", m_LongText ? "on" : "off");
		}

		if (Wui::Button(ctx, Wui::HashId("wui.workbench.btn.reset"),
			{ bar.X + bar.W - 74.0f, rowY, 66.0f, buttonH },
			Wui::Tr("workbench.global.reset", "Reset"), theme))
		{
			const std::string id = m_SelectedId.empty() ? std::string("<first>") : m_SelectedId;
			m_PropertyValues.clear();
			m_ForceState = "default";
			m_LongText = false;
			m_Status = Wui::Tr("workbench.status.reset", "Reset: property overrides and forced state cleared");
			ctx.RecordOp("gallery", "global", "Reset", id);
		}
	}

	void WidgetGalleryPanel::DrawInfoBar(Wui::WuiContext& ctx, const WbLayout& layout,
		const Wui::WuiTheme& theme, const Wui::WuiComponentDesc* desc)
	{
		const float size = 12.0f;
		std::string left = Wui::Tr("workbench.info.registry", "Registry: ")
			+ std::to_string(Wui::WuiComponentRegistry::Count()) + " components";
		if (desc != nullptr)
			left += "  |  " + desc->Id;
		Wui::Label(ctx, { layout.InfoBar.X, layout.InfoBar.Y + 2.0f },
			ClipText(ctx, left, layout.InfoBar.W * 0.55f, size), theme.TextMuted, size);

		const std::string right = Wui::Tr("workbench.info.last_action", "last action: ") + m_LastAction;
		Wui::Label(ctx, { layout.InfoBar.X + layout.InfoBar.W * 0.56f, layout.InfoBar.Y + 2.0f },
			ClipText(ctx, right, layout.InfoBar.W * 0.44f, size), theme.TextMuted, size);
		RegisterWorkbenchNode(Wui::HashId(kInfoId), "text", layout.InfoBar, left, m_LastAction, true, false);
	}

	std::vector<const Wui::WuiComponentDesc*> WidgetGalleryPanel::FilteredComponents() const
	{
		// 过滤后按 Category → DisplayName 分组(登记表已按此排序,这里保持原序)。
		const auto& all = Wui::WuiComponentRegistry::All();
		std::vector<const Wui::WuiComponentDesc*> visible;
		visible.reserve(all.size());
		for (const Wui::WuiComponentDesc& item : all)
		{
			if (!m_Search.empty())
			{
				std::string haystack = item.Id + " " + item.DisplayName + " " + item.Category;
				std::string needle = m_Search;
				std::transform(haystack.begin(), haystack.end(), haystack.begin(),
					[](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
				std::transform(needle.begin(), needle.end(), needle.begin(),
					[](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
				if (haystack.find(needle) == std::string::npos)
					continue;
			}
			visible.push_back(&item);
		}
		return visible;
	}

	const Wui::WuiComponentDesc* WidgetGalleryPanel::ResolveSelection(
		const std::vector<const Wui::WuiComponentDesc*>& visible) const
	{
		for (const Wui::WuiComponentDesc* item : visible)
			if (item->Id == m_SelectedId)
				return item;
		// 还没选过 / 选中件被搜索过滤掉:退回过滤后的第一件(与左树高亮同一口径)。
		return visible.empty() ? nullptr : visible.front();
	}

	void WidgetGalleryPanel::SelectComponent(Wui::WuiContext& ctx, const Wui::WuiComponentDesc& desc,
		const char* how)
	{
		m_SelectedId = desc.Id;
		m_ForceState = "default";
		ResetPropertyValues(desc.Id);
		m_PropScroll = 0.0f;
		m_LastAction = std::string("selected ") + desc.Id
			+ (std::string(how) == "select" ? std::string() : std::string(" (") + how + ")");
		ctx.RecordOp("gallery", how, "Component", desc.Id);
	}

	const Wui::WuiComponentDesc* WidgetGalleryPanel::DrawTree(Wui::WuiContext& ctx,
		const WbLayout& layout, const Wui::WuiTheme& theme)
	{
		const Wui::WuiRect area = layout.Tree;
		Wui::PanelBackground(ctx, area, theme.ContentBg, theme.Radius);
		Wui::SectionHeader(ctx, { area.X, area.Y, area.W, 22.0f },
			Wui::Tr("workbench.tree.title", "Components"), theme.Accent, theme, 14.0f);

		const float rowH = m_DensityIndex == 1 ? 20.0f : 24.0f;
		const Wui::WuiRect search { area.X + 6.0f, area.Y + 26.0f,
			std::max(60.0f, area.W - 12.0f), 24.0f };
		if (Wui::SearchField(ctx, Wui::HashId(kTreeSearchId), search, m_Search,
			Wui::Tr("workbench.tree.search", "Search components"), theme))
			ctx.RecordOp("gallery", "filter", "Components", m_Search);

		const std::vector<const Wui::WuiComponentDesc*> visible = FilteredComponents();

		const float listTop = search.Y + search.H + 6.0f;
		const float listHeight = std::max(40.0f, area.Y + area.H - listTop - 6.0f);
		const Wui::WuiRect list { area.X + 4.0f, listTop, std::max(40.0f, area.W - 8.0f), listHeight };
		const float listBottom = list.Y + list.H;

		float contentHeight = 4.0f;
		{
			std::string lastCategory;
			for (const Wui::WuiComponentDesc* item : visible)
			{
				if (item->Category != lastCategory)
				{
					contentHeight += 20.0f;
					lastCategory = item->Category;
				}
				contentHeight += rowH;
			}
		}
		// 滚轮归属要在 BeginScrollArea **之前**取:滚动区自己会登记裁剪/覆盖层矩形,
		// 之后 IsHovered 不再代表"指针在我的列表里"(与弹出层"点外关闭"同一口径)。
		const bool listHovered = ctx.IsHovered(list);
		const bool listHoveredRaw = ctx.HitTestRaw(list, ctx.Input().MousePos);
		const float wheel = ctx.Input().Wheel;
		const float maxScroll = std::max(0.0f, contentHeight - list.H);

		Wui::BeginScrollArea(ctx, list, contentHeight, m_TreeScroll, theme);
		// 覆盖层(模态遮罩/弹层)盖住列表时 BeginScrollArea 的 IsHovered 判 false,但用户指着
		// 列表滚动时列表仍应跟着滚 —— 用不带遮挡的原始命中补一次。两条路径互斥:同时应用会
		// 把同一格滚轮算两遍(实测 40+28px),列表"滚过头"、逐件定位的探针也会更难收敛。
		if (wheel != 0.0f && listHoveredRaw && !listHovered)
			m_TreeScroll = std::clamp(m_TreeScroll - wheel * 40.0f, 0.0f, maxScroll);

		// 键盘导航归属:指针在列表里,或最近一次交互(点行 / 滚轮)落在列表里。
		if (wheel != 0.0f && listHoveredRaw)
			m_TreeKeyboardFocus = true;
		else if (ctx.Input().MouseClicked[0] && ctx.HitTestRaw(m_PanelRect, ctx.Input().MousePos)
			&& !listHoveredRaw)
			m_TreeKeyboardFocus = false;   // 在面板别处点了:键盘导航交还出去

		// ↑/↓/Home/End 切换选中件(键盘可选的验收项)。弹层开着时键盘归弹层,组件树不抢键;
		// 文本编辑中同理(搜索框里按 ↑/↓ 不该跳组件)。
		if (!visible.empty() && (listHoveredRaw || m_TreeKeyboardFocus)
			&& !ctx.IsTextInputActive() && !m_PopupOpenPrev)
		{
			size_t index = 0;
			bool found = false;
			for (size_t probe = 0; probe < visible.size(); ++probe)
			{
				if (visible[probe]->Id == m_SelectedId)
				{
					index = probe;
					found = true;
					break;
				}
			}
			size_t next = index;
			if (ctx.WasKeyTriggered(KeyCodes::Down))
				next = std::min(visible.size() - 1, index + 1);
			else if (ctx.WasKeyTriggered(KeyCodes::Up))
				next = index == 0 ? 0 : index - 1;
			else if (ctx.WasKeyTriggered(KeyCodes::Home))
				next = 0;
			else if (ctx.WasKeyTriggered(KeyCodes::End))
				next = visible.size() - 1;
			if (next != index || !found)
				SelectComponent(ctx, *visible[next], "keyboard");
		}

		const Wui::WuiComponentDesc* selected = nullptr;
		int rowIndex = 0;
		float y = list.Y + 4.0f - m_TreeScroll;
		std::string lastCategory;
		for (const Wui::WuiComponentDesc* item : visible)
		{
			if (item->Category != lastCategory)
			{
				lastCategory = item->Category;
				if (y + 20.0f > list.Y && y < listBottom)
					Wui::Label(ctx, { list.X + 4.0f, y + 3.0f }, lastCategory, theme.TextMuted, 12.0f);
				y += 20.0f;
			}
			const Wui::WuiRect row { list.X, y, list.W, rowH };
			y += rowH;
			const bool active = item->Id == m_SelectedId
				|| (m_SelectedId.empty() && !visible.empty() && item == visible.front());
			if (active)
				selected = item;
			if (row.Y + row.H <= list.Y || row.Y >= listBottom)
			{
				++rowIndex;
				continue;
			}

			const bool hovered = ctx.IsHovered(row);
			Wui::HoverRow(ctx, row, hovered, active, theme, 2.0f);

			const float badgeW = 66.0f;
			Wui::Label(ctx, { row.X + 6.0f, row.Y + 3.0f },
				ClipText(ctx, item->DisplayName, std::max(20.0f, row.W - badgeW - 12.0f), 13.0f),
				active ? theme.Text : theme.TextMuted, 13.0f);
			Wui::Label(ctx, { row.X + row.W - badgeW - 2.0f, row.Y + 4.0f }, StatusText(item->Status),
				StatusColor(item->Status, theme), 11.0f);

			// 行 id 按**过滤后序**稳定编号;精确选中项另有 wui.workbench.canvas.selected 节点,
			// 探针不必从行号反推组件 id。
			const std::string rowId = std::string(kTreeRowPrefix) + std::to_string(rowIndex);
			RegisterWorkbenchNode(Wui::HashId(rowId.c_str()), "list-item", row,
				item->DisplayName, item->Id + "|" + StatusText(item->Status), true, true);

			if (hovered && ctx.Input().MouseClicked[0] && !ctx.IsPointerClickConsumed(0))
			{
				if (std::getenv("WLD_TRACE_UI"))
					WLD_CORE_INFO("[workbench] row click {0} at ({1},{2})",
						item->Id, static_cast<int>(ctx.Input().MousePos.x),
						static_cast<int>(ctx.Input().MousePos.y));
				SelectComponent(ctx, *item, "select");
				m_TreeKeyboardFocus = true;
			}
			++rowIndex;
		}
		Wui::EndScrollArea(ctx);

		// 选中件被搜索过滤掉 / 首次进入:退回过滤后的第一件(与左树高亮同一口径)。
		if (selected == nullptr && !visible.empty())
		{
			selected = visible.front();
			m_SelectedId = selected->Id;
		}

		if (!visible.empty() && m_LastScrolledSelection != m_SelectedId)
		{
			// 选中**变化时**把选中行滚进视口(鼠标点行 / 首次默认选中 / 自动化逐件选)——
			// 只在选择变化那一次滚:每帧都滚会跟用户的滚轮/拖动抢滚动位置(实测会卡住列表)。
			m_LastScrolledSelection = m_SelectedId;
			const auto rowTopFor = [&](size_t upto) {
				// 行在"未滚动"坐标系里的顶边(含它前面的分类头)。
				float top = list.Y + 4.0f;
				std::string category;
				for (size_t index = 0; index < upto; ++index)
				{
					if (visible[index]->Category != category)
					{
						category = visible[index]->Category;
						top += 20.0f;
					}
					top += rowH;
				}
				if (!visible.empty() && visible[upto]->Category != category)
					top += 20.0f;
				return top;
			};
			size_t currentIndex = 0;
			for (size_t index = 0; index < visible.size(); ++index)
				if (visible[index]->Id == m_SelectedId)
					currentIndex = index;
			const float selectedTop = rowTopFor(currentIndex);
			const float selectedBottom = selectedTop + rowH;
			if (selectedTop < list.Y + 4.0f + m_TreeScroll)
				m_TreeScroll = std::max(0.0f, selectedTop - list.Y - 4.0f);
			else if (selectedBottom > list.Y + 4.0f + m_TreeScroll + list.H)
				m_TreeScroll = std::min(std::max(0.0f, contentHeight - list.H),
					selectedBottom - list.Y - 4.0f - list.H);
		}
		// 列表本身的稳定锚点(探针用它滚动 / 发键盘事件),以及"当前选中件"的只读节点。
		RegisterWorkbenchNode(Wui::HashId(kTreeListId), "list", list,
			Wui::Tr("workbench.tree.title", "Components"),
			selected != nullptr ? selected->Id : std::string(), true, true);
		RegisterWorkbenchNode(Wui::HashId("wui.workbench.canvas.selected"), "text", list,
			selected != nullptr ? selected->DisplayName : std::string(),
			selected != nullptr ? selected->Id : std::string(), true, false);
		return selected;
	}

	void WidgetGalleryPanel::DrawCanvas(Wui::WuiContext& ctx, const WbLayout& layout,
		const Wui::WuiTheme& theme, const Wui::WuiComponentDesc* desc, float density, float uiScale,
		bool overlayStage)
	{
		const Wui::WuiRect area = layout.Canvas;
		Wui::PanelBackground(ctx, area, theme.ContentBg, theme.Radius);
		Wui::SectionHeader(ctx, { area.X, area.Y, area.W, 22.0f },
			Wui::Tr("workbench.canvas.title", "Canvas"), theme.Accent, theme, 14.0f);

		const Wui::WuiRect inner { area.X + 8.0f, area.Y + 28.0f,
			std::max(40.0f, area.W - 16.0f), std::max(40.0f, area.H - 36.0f) };
		m_CanvasRect = area;
		m_CanvasInner = inner;
		m_CanvasValid = true;
		m_AppliedState = m_ForceState;
		if (desc != nullptr)
			m_AppliedProperties = AppliedProperties(*desc, density, uiScale);
		else
			m_AppliedProperties.clear();

		// 参考网格(40px 主格,200px 加重)。
		const Wui::WuiColor gridColor { theme.Border.R, theme.Border.G, theme.Border.B, 0.55f };
		const Wui::WuiColor majorColor { theme.BorderStrong.R, theme.BorderStrong.G,
			theme.BorderStrong.B, 0.9f };
		if (m_ShowGrid)
		{
			for (float x = inner.X; x <= inner.X + inner.W + 0.5f; x += 40.0f)
			{
				const bool major = std::fmod(x - inner.X, 200.0f) < 0.5f;
				Wui::PanelBackground(ctx, { x, inner.Y, 1.0f, inner.H },
					major ? majorColor : gridColor, 0.0f);
			}
			for (float y = inner.Y; y <= inner.Y + inner.H + 0.5f; y += 40.0f)
			{
				const bool major = std::fmod(y - inner.Y, 200.0f) < 0.5f;
				Wui::PanelBackground(ctx, { inner.X, y, inner.W, 1.0f },
					major ? majorColor : gridColor, 0.0f);
			}
		}

		if (desc == nullptr)
		{
			Wui::Label(ctx, { inner.X + 10.0f, inner.Y + 12.0f },
				Wui::Tr("workbench.canvas.empty", "No component selected"), theme.TextMuted, 14.0f);
			RegisterWorkbenchNode(Wui::HashId(kCanvasId), "canvas", area, std::string(),
				std::string(), true, false);
			return;
		}

		// 尺寸:登记表 SizeNotes 的 preferred WxH,再按缩放与画布可用空间收敛。
		const std::pair<float, float> preferred = SizeNotesToSize(desc->SizeNotes);
		const float scale = std::max(0.5f, uiScale);
		float width = std::max(kMinCanvasSize, preferred.first * scale);
		float height = std::max(28.0f, preferred.second * scale);
		if (width > inner.W - 16.0f)
		{
			const float shrink = (inner.W - 16.0f) / width;
			width *= shrink;
			height *= shrink;
		}
		if (height > inner.H - 16.0f)
		{
			const float shrink = (inner.H - 16.0f) / height;
			width *= shrink;
			height *= shrink;
		}
		width = std::max(40.0f, width);
		height = std::max(16.0f, height);

		const Wui::WuiRect slot { inner.X + 8.0f, inner.Y + 8.0f, width, height };
		Wui::PanelBackground(ctx, { slot.X - 2.0f, slot.Y - 2.0f, slot.W + 4.0f, slot.H + 4.0f },
			theme.WindowBg, 2.0f);

		m_CanvasSlot = slot;
		// 非专用舞台:showcase 就在画布位置画。专用舞台的 showcase 由 OnRender 在**内容之后**
		// 再画(见 DrawCanvasOverlay):顺序很重要 —— 模态打开时会 ConsumePointerClick,
		// 先画会把本帧落在树/按钮上的点击一起吞掉。
		if (!overlayStage)
			DrawCanvasShowcase(ctx, theme, *desc, density, uiScale, false, area);

		RegisterWorkbenchNode(Wui::HashId(kCanvasId), "canvas", slot, desc->DisplayName,
			desc->Id + "|" + m_ForceState, true, false);
		const std::string label = desc->DisplayName + "  " + FormatFloat(width) + " x "
			+ FormatFloat(height);
		Wui::Label(ctx, { slot.X, slot.Y + slot.H + 4.0f },
			ClipText(ctx, label, inner.W, 12.0f), theme.TextMuted, 12.0f);
		RegisterWorkbenchNode(Wui::HashId(kCanvasLabelId), "text",
			{ slot.X, slot.Y + slot.H + 4.0f, inner.W, 16.0f }, label, std::string(), true, false);
	}

	// 画布里的 showcase 本体。overlayStage=true:整段画在 overlay 层并被裁剪到画布矩形 ——
	// 整窗遮罩(模态)因此只落在画布里,不压暗组件树/属性区;它的遮挡区按 overlay 深度登记,
	// 不会把工作台自己的控件挡掉(见 OnRender 的分层顺序)。
	void WidgetGalleryPanel::DrawCanvasShowcase(Wui::WuiContext& ctx, const Wui::WuiTheme& theme,
		const Wui::WuiComponentDesc& desc, float density, float uiScale, bool overlayStage,
		const Wui::WuiRect& clipRect)
	{
		const Wui::WuiRect slot = m_CanvasSlot;
		const float scale = std::max(0.5f, uiScale);
		const size_t overlayBefore = ctx.OverlayCommands().size();
		// overlayStage 时 ctx.Commands() 就是 overlay 命令流,下面取两条增量里的较大者。
		const size_t mainBefore = ctx.Commands().size();
		if (overlayStage)
		{
			ctx.PushOverlay();
			Wui::WuiDrawCommand clipPush;
			clipPush.Kind = Wui::WuiDrawKind::ClipPush;
			clipPush.Rect = clipRect;
			clipPush.Color = theme.PanelBg;
			ctx.Commands().push_back(clipPush);
			ctx.PushClipRect(clipRect);
		}
		if (desc.Showcase != nullptr)
		{
			Wui::WuiComponentDraw draw;
			draw.Context = &ctx;
			draw.Theme = &theme;
			draw.Rect = slot;
			draw.State = m_ForceState;
			draw.Properties = m_AppliedProperties;
			draw.UiScale = scale;
			draw.Density = density;
			draw.Locale = Wui::GetLanguage();
			desc.Showcase(draw);
		}
		else
		{
			Wui::Label(ctx, { slot.X + 8.0f, slot.Y + 8.0f }, desc.DisplayName, theme.Text, 14.0f);
			Wui::Label(ctx, { slot.X + 8.0f, slot.Y + 28.0f },
				Wui::Tr("workbench.canvas.no_showcase",
					"Showcase pending (registered by the component owner)"),
				theme.TextMuted, 12.0f);
		}
		if (overlayStage)
		{
			Wui::WuiDrawCommand clipPop;
			clipPop.Kind = Wui::WuiDrawKind::ClipPop;
			ctx.Commands().push_back(clipPop);
			ctx.PopClipRect();
			ctx.PopOverlay();
		}
		const size_t overlayAfter = ctx.OverlayCommands().size();
		const size_t mainAfter = ctx.Commands().size();
		const size_t overlayDelta = overlayAfter >= overlayBefore ? overlayAfter - overlayBefore : 0;
		const size_t mainDelta = mainAfter >= mainBefore ? mainAfter - mainBefore : 0;
		m_ShowcaseCommands = std::max(overlayDelta, mainDelta);
		if (!overlayStage && DetectsFullWindowOverlay(ctx, overlayBefore))
		{
			// 首次遇到这个组件就往 overlay 层画了整窗矩形(模态遮罩)——记下来,下一帧起改走
			// 专用舞台,免得它登记的全窗遮挡区把工作台自己也点不动(实测踩过)。
			m_OverlayStageComponents.insert(desc.Id);
		}
	}

	void WidgetGalleryPanel::DrawProperties(Wui::WuiContext& ctx, const WbLayout& layout,
		const Wui::WuiTheme& theme, const Wui::WuiComponentDesc* desc, float density, float uiScale)
	{
		const Wui::WuiRect area = layout.Props;
		Wui::PanelBackground(ctx, area, theme.ContentBg, theme.Radius);
		Wui::SectionHeader(ctx, { area.X, area.Y, area.W, 22.0f },
			Wui::Tr("workbench.props.title", "Properties"), theme.Accent, theme, 14.0f);

		if (desc == nullptr)
		{
			Wui::Label(ctx, { area.X + 8.0f, area.Y + 32.0f },
				Wui::Tr("workbench.props.empty", "Select a component"), theme.TextMuted, 13.0f);
			return;
		}

		const Wui::WuiRect content { area.X + 6.0f, area.Y + 26.0f,
			std::max(60.0f, area.W - 12.0f), std::max(60.0f, area.H - 32.0f) };
		const float rowH = 24.0f;
		const float rowGap = 4.0f;
		float contentHeight = 6.0f + rowH + rowGap;
		for (size_t index = 0; index < desc->Properties.size(); ++index)
			contentHeight += rowH + rowGap;
		// WUI-P1b:状态按钮行(见下面绘制)自己占的高度。
		{
			const std::vector<Wui::WuiRect> chips = StateChipRects(desc->States,
				{ content.X, 0.0f, content.W, rowH }, rowH);
			if (!chips.empty())
				contentHeight += (chips.back().Y - chips.front().Y) + rowH + rowGap;
		}
		contentHeight += 120.0f;   // 元信息(A11yNotes / SizeNotes / SourceFile / 开关摘要)

		Wui::BeginScrollArea(ctx, content, contentHeight, m_PropScroll, theme);
		float y = content.Y + 6.0f - m_PropScroll;

		// ---- 伪状态:由 desc.States 生成(登记表没列状态就不显示下拉,不造假控件)----
		if (!desc->States.empty())
		{
			m_StateOptions.clear();
			int selectedIndex = 0;
			for (size_t index = 0; index < desc->States.size(); ++index)
			{
				m_StateOptions.push_back(desc->States[index].Label.empty() ? desc->States[index].Id
					: desc->States[index].Label);
				if (desc->States[index].Id == m_ForceState)
					selectedIndex = static_cast<int>(index);
			}
			Wui::Label(ctx, { content.X + 2.0f, y + 4.0f },
				Wui::Tr("workbench.props.state", "State"), theme.TextMuted, 12.0f);
			int& stateIndex = ctx.Persist<int>(Wui::HashId("wui.workbench.prop.__state_index"),
				selectedIndex);
			if (stateIndex < 0 || stateIndex >= static_cast<int>(desc->States.size()))
				stateIndex = selectedIndex;
			if (Wui::Combo(ctx, Wui::HashId(kStateComboId),
				{ content.X + 76.0f, y, std::max(80.0f, content.W - 80.0f), rowH }, std::string(),
				m_StateOptions, stateIndex, theme))
			{
				m_ForceState = desc->States[static_cast<size_t>(stateIndex)].Id;
				m_LastAction = "state " + m_ForceState;
				ctx.RecordOp("gallery", "state", desc->Id, m_ForceState);
			}
			else
			{
				m_ForceState = desc->States[static_cast<size_t>(stateIndex)].Id;
			}
			if (ctx.IsPopupOpen(Wui::HashId(kStateComboId)))
				m_PopupOpenNow = true;   // 状态下拉开着:↑/↓ 归它,组件树不抢键
			y += rowH + rowGap;

			// WUI-P1b:状态按钮行 —— 逐件状态矩阵的**直接驱动路径**。
			// 每个状态一个按钮,id = `wui.workbench.state.<组件id>.<状态id>`(稳定,探针按它点)。
			// 为什么要有它:状态下拉是`Wui::Combo`的弹层,实测里注入点击只有上面两条能落下,
			// 第三条起收不到(节点 visible/interactive 都是 true 也点不动)。逐件出状态矩阵
			// 不能建在这个命中行为上;顺带也让"这件有哪些状态"一眼可见,不用先拉开下拉。
			const std::vector<Wui::WuiRect> chips = StateChipRects(desc->States,
				{ content.X, y, content.W, rowH }, rowH);
			for (size_t index = 0; index < chips.size() && index < desc->States.size(); ++index)
			{
				const Wui::WuiRect chip = chips[index];
				if (chip.Y + chip.H <= content.Y || chip.Y >= content.Y + content.H)
					continue;   // 滚动区外:不画也不登记,免得出现"看不到却点得到"的节点
				const std::string& stateId = desc->States[index].Id;
				const bool active = stateId == m_ForceState;
				Wui::WuiTheme chipTheme = theme;
				if (active)
				{
					chipTheme.Border = theme.Accent;
				}
				else
				{
					// 未选中:平铺底,靠文字与边框区分,不抢选中项的视觉权重。
					chipTheme.ButtonBg = theme.ContentBg;
					chipTheme.ButtonHover = theme.HoverBg;
					chipTheme.Text = theme.TextMuted;
				}
				const std::string chipId = std::string("wui.workbench.state.") + desc->Id + "." + stateId;
				std::string chipText = stateId;
				if (chipText.size() > 14)
					chipText.resize(14);
				if (Wui::Button(ctx, Wui::HashId(chipId.c_str()), chip, chipText, chipTheme))
				{
					stateIndex = static_cast<int>(index);
					m_ForceState = stateId;
					m_LastAction = "state " + stateId;
					ctx.RecordOp("gallery", "state", desc->Id, stateId);
				}
			}
			y += (chips.empty() ? 0.0f : (chips.back().Y - chips.front().Y) + rowH + rowGap);
		}

		// ---- 属性:类型由 Kind 决定,完全由登记表生成 ----
		for (size_t index = 0; index < desc->Properties.size(); ++index)
		{
			const Wui::WuiComponentProperty& prop = desc->Properties[index];
			const Wui::WuiRect row { content.X, y, content.W, rowH };
			if (row.Y + row.H > content.Y && row.Y < content.Y + content.H)
			{
				// id 里带上组件:属性控件与控件自身内部持久态共用 id 空间,而"行 0"在不同组件
				// 可能是完全不同的控件类型(TextField 的 WuiEditState vs DragFloat 的
				// WuiNumericState)——不区分组件就会报 "persisted state id reused with
				// different types",随后浮窗渲染直接失败(实测)。带上组件 id 后每件一套。
				const std::string propId = std::string(kPropLabelPrefix) + desc->Id + "."
					+ std::to_string(index);
				Wui::Label(ctx, { row.X + 2.0f, row.Y + 4.0f },
					ClipText(ctx, prop.Name, 72.0f, 12.0f), theme.TextMuted, 12.0f);
				const Wui::WuiRect control { row.X + 76.0f, row.Y,
					std::max(60.0f, row.W - 80.0f), rowH };
				const std::string value = PropertyValue(*desc, prop);
				switch (prop.Type)
				{
				case Wui::WuiComponentProperty::Kind::Bool:
				{
					bool flag = value == "1" || value == "true";
					if (Wui::Checkbox(ctx, Wui::HashId(propId.c_str()), control, std::string(), flag, theme))
					{
						SetPropertyValue(desc->Id, prop.Name, flag ? "1" : "0");
						m_LastAction = prop.Name + " = " + (flag ? "1" : "0");
					}
					break;
				}
				case Wui::WuiComponentProperty::Kind::Float:
				case Wui::WuiComponentProperty::Kind::Int:
				{
					if (prop.Type == Wui::WuiComponentProperty::Kind::Int)
					{
						int64_t number = static_cast<int64_t>(std::llround(SafeFloat(value, prop.Min)));
						number = std::clamp(number, static_cast<int64_t>(prop.Min),
							static_cast<int64_t>(prop.Max));
						if (Wui::DragInt(ctx, Wui::HashId(propId.c_str()), control, number,
							static_cast<int64_t>(prop.Min), static_cast<int64_t>(prop.Max), theme))
						{
							SetPropertyValue(desc->Id, prop.Name, std::to_string(number));
							m_LastAction = prop.Name + " = " + std::to_string(number);
						}
					}
					else
					{
						float number = std::clamp(SafeFloat(value, prop.Min), prop.Min, prop.Max);
						if (Wui::DragFloat(ctx, Wui::HashId(propId.c_str()), control, number,
							std::max(0.001f, prop.Step), prop.Min, prop.Max, theme))
						{
							SetPropertyValue(desc->Id, prop.Name, FormatFloat(number));
							m_LastAction = prop.Name + " = " + FormatFloat(number);
						}
					}
					break;
				}
				case Wui::WuiComponentProperty::Kind::Enum:
				{
					m_PropertyEnumOptions = prop.Options.empty()
						? std::vector<std::string> { value } : prop.Options;
					int selected = 0;
					for (size_t option = 0; option < m_PropertyEnumOptions.size(); ++option)
						if (m_PropertyEnumOptions[option] == value)
							selected = static_cast<int>(option);
					int& cached = m_PropertyEnumIndex[desc->Id + "|" + prop.Name];
					if (cached < 0 || cached >= static_cast<int>(m_PropertyEnumOptions.size()))
						cached = selected;
					if (Wui::Combo(ctx, Wui::HashId(propId.c_str()), control, std::string(),
						m_PropertyEnumOptions, cached, theme))
					{
						const std::string chosen = m_PropertyEnumOptions[static_cast<size_t>(cached)];
						SetPropertyValue(desc->Id, prop.Name, chosen);
						m_LastAction = prop.Name + " = " + chosen;
					}
					if (ctx.IsPopupOpen(Wui::HashId(propId.c_str())))
						m_PopupOpenNow = true;
					break;
				}
				case Wui::WuiComponentProperty::Kind::Text:
				default:
				{
					std::string buffer = value;
					Wui::TextFieldA11y a11y;
					a11y.Label = prop.Name;
					if (Wui::TextField(ctx, Wui::HashId(propId.c_str()), control, buffer, theme, nullptr,
						&a11y))
					{
						SetPropertyValue(desc->Id, prop.Name, buffer);
						m_LastAction = prop.Name + " = " + buffer;
					}
					break;
				}
				}

				// 属性控件与控件内部持久态共用 id 是安全的(Widget 自己的 ctx.Persist 归它),
				// 但工作台自己**不再**往同一个 id 里塞第二种类型的状态。
			}
			y += rowH + rowGap;
		}

		// ---- 元信息:登记表里的说明与实现文件(追责/文档)----
		const float metaSize = 12.0f;
		const auto metaLine = [&](const std::string& text, const Wui::WuiColor& color)
		{
			if (y + 16.0f > content.Y && y < content.Y + content.H)
				Wui::Label(ctx, { content.X + 2.0f, y + 2.0f },
					ClipText(ctx, text, content.W - 4.0f, metaSize), color, metaSize);
			y += 16.0f;
		};
		metaLine(Wui::Tr("workbench.props.a11y_notes", "a11y: ") + desc->A11yNotes, theme.TextMuted);
		metaLine(Wui::Tr("workbench.props.size_notes", "size: ") + desc->SizeNotes, theme.TextMuted);
		metaLine(Wui::Tr("workbench.props.source", "source: ") + desc->SourceFile, theme.TextMuted);
		if (!desc->ExtraA11yIds.empty())
		{
			std::string ids;
			for (const std::string& id : desc->ExtraA11yIds)
				ids += (ids.empty() ? "" : ", ") + id;
			metaLine(Wui::Tr("workbench.props.extra_ids", "ids: ") + ids, theme.TextMuted);
		}
		metaLine(Wui::Tr("workbench.props.globals", "density: ")
			+ (m_DensityIndex == 1 ? "compact" : "comfortable")
			+ "  scale: " + FormatFloat(uiScale)
			+ "  locale: " + (Wui::GetLanguage().empty() ? "en" : Wui::GetLanguage()),
			theme.TextMuted);
		Wui::EndScrollArea(ctx);
	}

	void WidgetGalleryPanel::DrawActions(Wui::WuiContext& ctx, const WbLayout& layout,
		const Wui::WuiTheme& theme, const Wui::WuiComponentDesc* desc, float uiScale)
	{
		const Wui::WuiRect bar = layout.Actions;
		Wui::PanelBackground(ctx, bar, theme.PanelHeader, theme.Radius);
		const float density = m_DensityIndex == 1 ? 0.85f : 1.0f;

		if (Wui::Button(ctx, Wui::HashId(kCaptureButtonId), { bar.X + 8.0f, bar.Y + 3.0f, 110.0f, 24.0f },
			Wui::Tr("workbench.action.capture", "Capture"), theme))
		{
			if (desc != nullptr && m_CanvasValid)
			{
				WriteCaptureMetadata(*desc, density, uiScale);
				m_Status = Wui::Tr("workbench.status.captured", "Capture metadata written: ")
					+ (WorkbenchDir() / "current.json").string();
				m_LastAction = "capture " + desc->Id;
				ctx.RecordOp("gallery", "capture", desc->Id, "build/wui-workbench/current.json");
			}
			else
			{
				m_Status = Wui::Tr("workbench.status.no_component",
					"Action skipped: no component selected");
			}
		}

		if (Wui::Button(ctx, Wui::HashId(kApproveButtonId), { bar.X + 124.0f, bar.Y + 3.0f, 130.0f, 24.0f },
			Wui::Tr("workbench.action.approve", "Approve"), theme))
		{
			if (desc != nullptr)
			{
				WriteApproval(*desc, density, uiScale);
				const std::string commit = ResolveGitCommit();
				m_Status = Wui::Tr("workbench.status.approved", "Recorded approval: ") + desc->Id
					+ " @ " + commit;
				m_LastAction = "approve " + desc->Id;
				ctx.RecordOp("gallery", "approve", desc->Id, commit);
			}
			else
			{
				m_Status = Wui::Tr("workbench.status.no_component",
					"Action skipped: no component selected");
			}
		}

		RegisterWorkbenchNode(Wui::HashId("wui.workbench.canvas.state"), "text", bar,
			desc != nullptr ? desc->Id : std::string("(none)"), m_ForceState, true, false);
		const std::string status = m_Status.empty()
			? Wui::Tr("workbench.status.idle",
				"Idle. Capture writes metadata; the probe crops the screenshot.")
			: m_Status;
		Wui::Label(ctx, { bar.X + 266.0f, bar.Y + 7.0f },
			ClipText(ctx, status, std::max(40.0f, bar.W - 274.0f), 12.0f), theme.TextMuted, 12.0f);
		RegisterWorkbenchNode(Wui::HashId(kStatusId), "text", bar, status, m_ForceState, true, false);
	}

	std::string WidgetGalleryPanel::CaptureMetadataJson(const Wui::WuiComponentDesc& desc, float density,
		float uiScale) const
	{
		std::ostringstream out;
		out << "{\n";
		out << "  \"component\": \"" << JsonEscape(desc.Id) << "\",\n";
		out << "  \"displayName\": \"" << JsonEscape(desc.DisplayName) << "\",\n";
		// WUI-P1b:探针要按登记表逐个状态驱动/断言,这些字段让它不必反查 C++ 源。
		out << "  \"typeName\": \"" << JsonEscape(desc.TypeName) << "\",\n";
		out << "  \"category\": \"" << JsonEscape(desc.Category) << "\",\n";
		out << "  \"status\": \"" << JsonEscape(StatusText(desc.Status)) << "\",\n";
		out << "  \"sourceFile\": \"" << JsonEscape(desc.SourceFile) << "\",\n";
		out << "  \"a11yNotes\": \"" << JsonEscape(desc.A11yNotes) << "\",\n";
		out << "  \"sizeNotes\": \"" << JsonEscape(desc.SizeNotes) << "\",\n";
		out << "  \"states\": [";
		for (size_t index = 0; index < desc.States.size(); ++index)
		{
			out << (index == 0 ? "" : ", ") << "{\"id\": \"" << JsonEscape(desc.States[index].Id)
				<< "\", \"label\": \"" << JsonEscape(desc.States[index].Label) << "\"}";
		}
		out << "],\n";
		out << "  \"commands\": " << m_ShowcaseCommands << ",\n";
		out << "  \"state\": \"" << JsonEscape(m_AppliedState) << "\",\n";
		out << "  \"commit\": \"" << ResolveGitCommit() << "\",\n";
		out << "  \"window\": \"" << JsonEscape(Wui::WuiAccessibility::Get().CurrentWindow()) << "\",\n";
		out << "  \"canvas\": {\"x\": " << m_CanvasRect.X << ", \"y\": " << m_CanvasRect.Y
			<< ", \"w\": " << m_CanvasRect.W << ", \"h\": " << m_CanvasRect.H << "},\n";
		out << "  \"slot\": {\"x\": " << m_CanvasInner.X << ", \"y\": " << m_CanvasInner.Y
			<< ", \"w\": " << m_CanvasInner.W << ", \"h\": " << m_CanvasInner.H << "},\n";
		// WUI-P1b:"slot" 是画布内框(历史字段,含义不变);showcase 自己那块占位矩形另给一个键 ——
		// image / spacer 判定"占位尺寸正确"要的是它(用户裁决 2 的豁免断言)。
		out << "  \"showcaseSlot\": {\"x\": " << m_CanvasSlot.X << ", \"y\": " << m_CanvasSlot.Y
			<< ", \"w\": " << m_CanvasSlot.W << ", \"h\": " << m_CanvasSlot.H << "},\n";
		out << "  \"uiScale\": " << uiScale << ",\n";
		out << "  \"density\": " << density << ",\n";
		out << "  \"overlayStage\": " << (m_CanvasOverlayStage ? "true" : "false") << ",\n";
		out << "  \"locale\": \"" << JsonEscape(Wui::GetLanguage()) << "\",\n";
		out << "  \"longText\": " << (m_LongText ? "true" : "false") << ",\n";
		out << "  \"a11yIds\": [";
		for (size_t index = 0; index < desc.ExtraA11yIds.size(); ++index)
			out << (index == 0 ? "" : ", ") << "\"" << JsonEscape(desc.ExtraA11yIds[index]) << "\"";
		out << "],\n";
		out << "  \"properties\": {";
		bool first = true;
		for (const auto& [name, value] : m_AppliedProperties)
		{
			out << (first ? "\n" : ",\n") << "    \"" << JsonEscape(name) << "\": \"" << JsonEscape(value)
				<< "\"";
			first = false;
		}
		out << (first ? "}\n" : "\n  }\n");
		out << "}\n";
		return out.str();
	}

	void WidgetGalleryPanel::WriteCaptureMetadata(const Wui::WuiComponentDesc& desc, float density,
		float uiScale) const
	{
		EnsureWorkbenchDir();
		std::ofstream file(WorkbenchDir() / "current.json", std::ios::binary | std::ios::trunc);
		if (!file)
			return;
		file << CaptureMetadataJson(desc, density, uiScale);
	}

	void WidgetGalleryPanel::WriteApproval(const Wui::WuiComponentDesc& desc, float density, float uiScale)
	{
		EnsureWorkbenchDir();
		// 只按 component 做"合并写":把已有条目解析出来(平铺解析器,不引 JSON 依赖),
		// 保留其它组件的记录,只改当前这一条。口径见 approved.json 的 note 字段。
		struct Entry
		{
			std::string Component;
			std::string Commit;
			std::string ApprovedAt;
			std::string Status;
			std::string A11y;
			std::string Size;
			std::string Source;
			std::string Locale;
			float Density = 1.0f;
			float UiScale = 1.0f;
		};
		std::vector<Entry> entries;
		{
			std::ifstream existing(WorkbenchDir() / "approved.json", std::ios::binary);
			std::ostringstream buffer;
			buffer << existing.rdbuf();
			const std::string text = buffer.str();
			const auto readField = [&text](const std::string& key, size_t from) {
				const std::string needle = "\"" + key + "\": \"";
				const size_t at = text.find(needle, from);
				if (at == std::string::npos)
					return std::string();
				const size_t start = at + needle.size();
				const size_t end = text.find('"', start);
				return end == std::string::npos ? std::string() : text.substr(start, end - start);
			};
			size_t cursor = 0;
			while (true)
			{
				const std::string needle = "\"component\": \"";
				const size_t at = text.find(needle, cursor);
				if (at == std::string::npos)
					break;
				const size_t start = at + needle.size();
				const size_t end = text.find('"', start);
				if (end == std::string::npos)
					break;
				Entry entry;
				entry.Component = text.substr(start, end - start);
				entry.Commit = readField("commit", at);
				entry.ApprovedAt = readField("approvedAt", at);
				entry.Status = readField("status", at);
				entry.A11y = readField("a11yNotes", at);
				entry.Size = readField("sizeNotes", at);
				entry.Source = readField("sourceFile", at);
				entry.Locale = readField("locale", at);
				entries.push_back(entry);
				cursor = end;
			}
		}

		Entry entry;
		entry.Component = desc.Id;
		entry.Commit = ResolveGitCommit();
		entry.Status = StatusText(desc.Status);
		entry.A11y = desc.A11yNotes;
		entry.Size = desc.SizeNotes;
		entry.Source = desc.SourceFile;
		entry.Density = density;
		entry.UiScale = uiScale;
		entry.Locale = Wui::GetLanguage().empty() ? "en" : Wui::GetLanguage();
		{
			const std::time_t now = std::time(nullptr);
			char stamp[32] = {};
			std::tm local {};
			localtime_s(&local, &now);
			std::strftime(stamp, sizeof(stamp), "%Y-%m-%dT%H:%M:%S", &local);
			entry.ApprovedAt = stamp;
		}

		bool replaced = false;
		for (Entry& item : entries)
		{
			if (item.Component == entry.Component)
			{
				item = entry;
				replaced = true;
			}
		}
		if (!replaced)
			entries.push_back(entry);
		std::sort(entries.begin(), entries.end(),
			[](const Entry& left, const Entry& right) { return left.Component < right.Component; });

		std::ofstream file(WorkbenchDir() / "approved.json", std::ios::binary | std::ios::trunc);
		if (!file)
			return;
		std::ostringstream out;
		out << "{\n";
		out << "  \"note\": \"WUI component approvals: the component was reviewed at the recorded "
			"commit. Changing a component's look requires re-approval (plan P1).\",\n";
		out << "  \"approved\": [\n";
		for (size_t index = 0; index < entries.size(); ++index)
		{
			const Entry& item = entries[index];
			out << "    { \"component\": \"" << JsonEscape(item.Component) << "\", \"commit\": \""
				<< JsonEscape(item.Commit) << "\", \"approvedAt\": \"" << JsonEscape(item.ApprovedAt)
				<< "\", \"status\": \"" << JsonEscape(item.Status) << "\", \"a11yNotes\": \""
				<< JsonEscape(item.A11y) << "\", \"sizeNotes\": \"" << JsonEscape(item.Size)
				<< "\", \"sourceFile\": \"" << JsonEscape(item.Source) << "\", \"locale\": \""
				<< JsonEscape(item.Locale) << "\", \"density\": " << item.Density
				<< ", \"uiScale\": " << item.UiScale << " }"
				<< (index + 1 < entries.size() ? "," : "") << "\n";
		}
		out << "  ]\n}\n";
		file << out.str();
	}

	std::string WidgetGalleryPanel::PropertyValue(const Wui::WuiComponentDesc& desc,
		const Wui::WuiComponentProperty& prop) const
	{
		const auto component = m_PropertyValues.find(desc.Id);
		if (component != m_PropertyValues.end())
		{
			const auto found = component->second.find(prop.Name);
			if (found != component->second.end())
				return found->second;
		}
		switch (prop.Type)
		{
		case Wui::WuiComponentProperty::Kind::Bool:
			return "0";
		case Wui::WuiComponentProperty::Kind::Float:
			return FormatFloat(prop.Min);
		case Wui::WuiComponentProperty::Kind::Int:
			return std::to_string(static_cast<int64_t>(prop.Min));
		case Wui::WuiComponentProperty::Kind::Enum:
			return prop.Options.empty() ? std::string() : prop.Options.front();
		case Wui::WuiComponentProperty::Kind::Text:
		default:
			return prop.DefaultText;
		}
	}

	void WidgetGalleryPanel::SetPropertyValue(const std::string& componentId, const std::string& name,
		const std::string& value)
	{
		m_PropertyValues[componentId][name] = value;
	}

	void WidgetGalleryPanel::ResetPropertyValues(const std::string& componentId)
	{
		m_PropertyValues[componentId].clear();
	}

	std::vector<std::pair<std::string, std::string>> WidgetGalleryPanel::AppliedProperties(
		const Wui::WuiComponentDesc& desc, float density, float uiScale) const
	{
		std::vector<std::pair<std::string, std::string>> out;
		out.reserve(desc.Properties.size() + 3);
		// 全局开关作为"已知属性名"注入:showcase 认识就这样用,不认识会忽略(登记表契约)。
		out.emplace_back("density", FormatFloat(density));
		out.emplace_back("uiScale", FormatFloat(uiScale));
		out.emplace_back("locale", Wui::GetLanguage().empty() ? "en" : Wui::GetLanguage());
		for (const Wui::WuiComponentProperty& prop : desc.Properties)
		{
			std::string value = PropertyValue(desc, prop);
			// 长文本压力:Text 类属性换成超长串(一帧就能看出溢出/裁剪/换行问题)。
			if (m_LongText && prop.Type == Wui::WuiComponentProperty::Kind::Text)
				value = "Long text stress: " + std::string(160, 'W')
					+ " / 长文本压力测试(检查溢出、裁剪与换行)";
			out.emplace_back(prop.Name, value);
		}
		return out;
	}

	void WidgetGalleryPanel::OnRender(Wui::WuiContext& ctx, const Wui::WuiRect& rect, PanelHost& host)
	{
		Wui::WuiTheme& theme = host.Theme();
		const WbLayout layout = ComputeLayout(rect, theme);
		const float density = m_DensityIndex == 1 ? 0.85f : 1.0f;
		const float uiScale = Wui::UiScale() > 0.0f ? Wui::UiScale() : 1.0f;
		m_PanelRect = rect;
		m_PopupOpenNow = false;

		// "专用舞台"顺序(覆盖层组件):画布先画,其余内容画到更深的分层 —— 否则模态遮罩登记的
		// 全窗遮挡区会把工作台自己整块面板的命中打死(实测:选中 modal 之后连 Capture 都点不动,
		// 逐件遍历从 scrollarea 起全部卡死)。
		const std::vector<const Wui::WuiComponentDesc*> visible = FilteredComponents();
		const Wui::WuiComponentDesc* ahead = ResolveSelection(visible);
		const bool overlayStage = ahead != nullptr && m_OverlayStageComponents.count(ahead->Id) != 0;
		m_CanvasOverlayStage = overlayStage;

		if (overlayStage)
		{
			// 画布底(背景/网格/槽位)先画 —— 它在正常命令层,遮罩盖在它上面才是"画布里的模态"。
			DrawCanvas(ctx, layout, theme, ahead, density, uiScale, true);
			// 内容层比 showcase 深两层:showcase 里的模态自己在 depth+1 上登记遮挡区,
			// 内容画在 depth ≥ 该深度才不会被它挡住(见 DrawCanvas 的注释)。
			ctx.PushOverlay();
			ctx.PushOverlay();
			DrawTopBar(ctx, layout, theme);
			const Wui::WuiComponentDesc* selected = nullptr;
			if (!layout.Stacked)
			{
				selected = DrawTree(ctx, layout, theme);
				DrawInfoBar(ctx, layout, theme, selected);
				DrawProperties(ctx, layout, theme, selected, density, uiScale);
			}
			else
			{
				// 窄窗:树/属性仍然走 body 滚动(画布已单独画在它的位置上)。
				const float contentHeight = layout.Props.Y + layout.Props.H + 8.0f - layout.Body.Y;
				Wui::BeginScrollArea(ctx, layout.Body, contentHeight, m_BodyScroll, theme);
				selected = DrawTree(ctx, layout, theme);
				DrawProperties(ctx, layout, theme, selected, density, uiScale);
				Wui::EndScrollArea(ctx);
				DrawInfoBar(ctx, layout, theme, selected);
			}
			DrawActions(ctx, layout, theme, selected, uiScale);
			ctx.PopOverlay();
			ctx.PopOverlay();
			// showcase 最后画(仍在 overlay 层、裁剪到画布矩形):模态打开会 ConsumePointerClick,
			// 放在内容之后才吞不到本帧落在树/属性/按钮上的点击;裁剪则保证整窗遮罩只盖画布。
			if (ahead != nullptr)
				DrawCanvasShowcase(ctx, theme, *ahead, density, uiScale, true, layout.Canvas);
		}
		else
		{
			DrawTopBar(ctx, layout, theme);
			const Wui::WuiComponentDesc* selected = nullptr;

			if (!layout.Stacked)
			{
				selected = DrawTree(ctx, layout, theme);
				DrawInfoBar(ctx, layout, theme, selected);
				DrawCanvas(ctx, layout, theme, selected, density, uiScale, false);
				DrawProperties(ctx, layout, theme, selected, density, uiScale);
			}
			else
			{
				// 窄窗:三段纵向排布,整个 body 一起滚动(窗口够小时仍能看到全部区域)。
				const float contentHeight = layout.Props.Y + layout.Props.H + 8.0f - layout.Body.Y;
				Wui::BeginScrollArea(ctx, layout.Body, contentHeight, m_BodyScroll, theme);
				selected = DrawTree(ctx, layout, theme);
				DrawCanvas(ctx, layout, theme, selected, density, uiScale, false);
				DrawProperties(ctx, layout, theme, selected, density, uiScale);
				Wui::EndScrollArea(ctx);
				DrawInfoBar(ctx, layout, theme, selected);
			}

			DrawActions(ctx, layout, theme, selected, uiScale);
		}

		// 下一帧左树画在本帧之前,所以"弹层开着 → ↑/↓ 归弹层"只能用上一帧的结果。
		m_PopupOpenPrev = m_PopupOpenNow;
	}
}
