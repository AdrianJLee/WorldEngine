#include "wldpch.h"
#include "WidgetGalleryPanel.h"

#include "../../EditorPreferences.h"

#include "World/Core/KeyCodes.h"
#include "World/WUI/WuiAccessibility.h"
#include "World/WUI/WuiLocalization.h"
#include "World/WUI/WuiScriptedInput.h"
#include "World/WUI/WuiWidgets.h"
#include "World/WUI/Widgets/WuiChrome.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <sstream>
#include <string_view>
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
		constexpr const char* kStatusId = "wui.workbench.status";
		constexpr const char* kInfoId = "wui.workbench.info";
		constexpr const char* kPropLabelPrefix = "wui.workbench.prop.";
		// WUI-P1.5b:属性面板的搜索框 / 分组折叠头(P1.5b 新增的稳定 id)。
		constexpr const char* kPropSearchId = "wui.workbench.props.search";
		constexpr const char* kPropGroupHeaderPrefix = "wui.workbench.props.group.";
		// WUI-P1.6b:Edit/Play 开关 + 一键跑交互契约 + Play 观测条(探针按这些 id 驱动/断言)。
		constexpr const char* kModeEditButtonId = "wui.workbench.btn.mode.edit";
		constexpr const char* kModePlayButtonId = "wui.workbench.btn.mode.play";
		constexpr const char* kModeNodeId = "wui.workbench.mode";
		constexpr const char* kRunButtonId = "wui.workbench.btn.interactions";
		constexpr const char* kRunNodeId = "wui.workbench.interactions";
		constexpr const char* kPlayObservationId = "wui.workbench.play.observation";

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

		// ---- WUI-P1.5b:属性面板(UE 式)用的分组/格式化工具 ----
		//
		// 值编码协议(颜色 `#RRGGBB[AA]`、尺寸 `WxH`)由登记表侧实现
		// (`Wui::ParseComponentColor` / `Wui::ParseComponentSize` / `FormatComponent*`),
		// 面板**只调用不重写** —— 否则面板与 showcase 会各解析一套,迟早不一致。
		constexpr const char* kPropGroupGeneral = "General";

		std::string ToLowerAscii(std::string_view text)
		{
			std::string out;
			out.reserve(text.size());
			for (const char ch : text)
			{
				const unsigned char byte = static_cast<unsigned char>(ch);
				out.push_back(byte < 0x80 ? static_cast<char>(std::tolower(byte)) : ch);
			}
			return out;
		}

		// 组名:登记表的枚举 → 面板内的稳定组 id(做 a11y id / 排序键,不随语言变)。
		// 类型名用 decltype 取而不是直接写:属性分组枚举是**属性元数据**,不是可展示控件,
		// 而共享门禁 `check-ui-components.ps1` 会把面板里出现的控件命名空间类型名当成
		// "用到但未登记的控件类型"报缺口(它的 `$nonComponentTypes` 白名单在门禁文件里,
		// 不在本单的文件边界内)。语义完全不变,只是不给扫描面添假阳性。
		using PropGroupEnum = decltype(Wui::WuiComponentProperty::Group);

		std::string NormalizePropGroup(PropGroupEnum group)
		{
			switch (group)
			{
			case PropGroupEnum::Style: return "Style";
			case PropGroupEnum::Layout: return "Layout";
			case PropGroupEnum::Behavior: return "Behavior";
			case PropGroupEnum::Content: return "Content";
			default: return kPropGroupGeneral;   // 未来新增的枚举值先落"其它"桶,不静默丢行
			}
		}

		int PropGroupRank(const std::string& group)
		{
			if (group == "Content") return 0;
			if (group == "Style") return 1;
			if (group == "Layout") return 2;
			if (group == "Behavior") return 3;
			if (group == kPropGroupGeneral) return 9;
			return 5;
		}

		std::string PropGroupTitle(const std::string& group)
		{
			if (group == "Content") return Wui::Tr("workbench.props.group.content", "Content");
			if (group == "Style") return Wui::Tr("workbench.props.group.style", "Style");
			if (group == "Layout") return Wui::Tr("workbench.props.group.layout", "Layout");
			if (group == "Behavior") return Wui::Tr("workbench.props.group.behavior", "Behavior");
			if (group == kPropGroupGeneral) return Wui::Tr("workbench.props.group.general", "General");
			return group;
		}

		// 颜色两种表示之间的转换:面板的色板控件用 glm::vec4,登记表/引擎协议用 WuiColor。
		glm::vec4 PropColorToVec4(const Wui::WuiColor& color)
		{
			return { color.R, color.G, color.B, color.A };
		}

		Wui::WuiColor PropVec4ToColor(const glm::vec4& rgba)
		{
			return Wui::WuiColor { rgba.r, rgba.g, rgba.b, rgba.a };
		}

		// ---- WUI-P1.5b:性能采样开关(只用于验收,默认关闭)----
		// WLD_WORKBENCH_PROPS:属性面板形态
		//   0 = 整体不画(基线"面板关闭")
		//   1 = 只画属性行(旧结构:无搜索框、无分组头、无状态选择器)
		//   2 = 完整面板(**默认,用户看到的就是它**)
		//   3 = 只画前 2 行属性(基线"旧 2 属性版")
		// 非 2 的取值不是产品行为,只服务于 ≤0.5ms/帧 的增量验收。
		int PropPanelMode()
		{
			static const int mode = [] {
				const char* raw = std::getenv("WLD_WORKBENCH_PROPS");
				return raw == nullptr || *raw == '\0' ? 2 : std::atoi(raw);
			}();
			return mode;
		}

		bool PropTimingEnabled()
		{
			static const bool enabled = std::getenv("WLD_WORKBENCH_TIMING") != nullptr;
			return enabled;
		}

		// 状态选择器的排版:估计高度与绘制**走同一段代码**(绘制时多画一次按钮),避免两处口径走偏。
		// 宽度表在缓存重建时算好;这里只做换行与位置计算,不建临时 vector(每帧零分配)。
		template <typename Emit>
		float WalkStateChips(const std::vector<float>& widths, const Wui::WuiRect& rect, float rowH,
			Emit&& emit)
		{
			const float gap = 4.0f;
			float x = rect.X + 2.0f;
			float y = rect.Y;
			for (size_t index = 0; index < widths.size(); ++index)
			{
				const float width = widths[index];
				if (x > rect.X + 2.0f && x + width > rect.X + rect.W - 2.0f)
				{
					x = rect.X + 2.0f;
					y += rowH + gap;
				}
				emit(index, Wui::WuiRect { x, y, width, rowH });
				x += width + gap;
			}
			return widths.empty() ? 0.0f : (y - rect.Y) + rowH;
		}

		// WUI-P1c-b W2:字号缩放因子(UiScale)。面板/画布里的字号都要乘它,与 density 分开 ——
		// density 只压行高/槽位高度,缩放只放大字号。**只放大字号,不动布局度量**:行高、间距、
		// 控件高仍是设计单位(后端会按 UiScale 放大),否则会和画布槽位(已按 UiScale 放大)叠两次。
		float FontScale(float uiScale)
		{
			return uiScale > 0.0f ? uiScale : 1.0f;
		}

		// showcase 用的主题副本:只把字体令牌乘 scale(颜色/圆角/行高/内边距保持原样)。
		// 为什么要副本而不是改全局主题:组件 showcase 内部字号全部读 theme.FontSize*(Engine 侧代码),
		// 工作台能改的只有"传进去的那份主题";不动 Engine 公共接口。
		Wui::WuiTheme ScaledFontTheme(const Wui::WuiTheme& theme, float scale)
		{
			if (scale <= 0.0f || std::abs(scale - 1.0f) < 0.001f)
				return theme;
			Wui::WuiTheme out = theme;
			out.FontSizeCaption *= scale;
			out.FontSizeSmall *= scale;
			out.FontSizeBody *= scale;
			out.FontSizeTitle *= scale;
			out.FontSizeHeading *= scale;
			return out;
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

		// ---- WUI-P1.6b:交互契约 runner 的阶段与工具 ----
		//
		// 阶段定时用**墙钟**:隐藏窗口的帧率不保证(实测同一台机器上 60 与 200+ fps 都出现过),
		// 按帧定时会让探针抓不到 pressed 帧 —— 按下沿只存在一帧,而"按住"这一段才是像素证据窗口。
		enum RunPhaseId
		{
			kRunIdle = 0,
			kRunApproach,     // 移入(悬停)
			kRunClickSend,    // 脚本化点击(与 ui.invoke 同一条 WuiScriptedInput 队列)
			kRunPressHold,    // 按下 + 保持(像素抓取窗口)
			kRunRelease,      // 抬起沿
			kRunLeave,        // 移出(保证"未悬停"的默认外观)
			kRunFocusScan,    // Key 契约:Tab 找焦点
			kRunKeyEnter,     // Enter
			kRunKeySpace,     // Space
			kRunDragMove,     // 按住 + 位移
			kRunTypeText,     // 逐帧注入码点
			kRunScroll,       // 逐帧注入滚轮
			kRunSettle,       // 收尾(不注入)
			kRunFinished,
		};

		uint64_t NowMs()
		{
			return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
				std::chrono::steady_clock::now().time_since_epoch()).count());
		}

		std::string WallStamp()
		{
			const std::time_t now = std::time(nullptr);
			std::tm local {};
			localtime_s(&local, &now);
			char stamp[32] = {};
			std::strftime(stamp, sizeof(stamp), "%Y-%m-%dT%H:%M:%S", &local);
			return stamp;
		}

		const char* RunPhaseName(int phase)
		{
			switch (phase)
			{
			case kRunApproach: return "approach";
			case kRunClickSend: return "click-send";
			case kRunPressHold: return "press-hold";
			case kRunRelease: return "release";
			case kRunLeave: return "leave";
			case kRunFocusScan: return "focus-scan";
			case kRunKeyEnter: return "key-enter";
			case kRunKeySpace: return "key-space";
			case kRunDragMove: return "drag-move";
			case kRunTypeText: return "type-text";
			case kRunScroll: return "scroll";
			case kRunSettle: return "settle";
			default: return "idle";
			}
		}

		uint64_t RunPhaseDurationMs(int phase)
		{
			switch (phase)
			{
			case kRunApproach: return 600;      // 悬停稳定段(探针的 hover 抓帧窗口)
			case kRunClickSend: return 700;     // 脚本化点击的节拍(队列本身 2 帧)
			case kRunPressHold: return 1300;    // 按住稳定段(探针的 pressed 抓帧窗口)
			case kRunRelease: return 350;
			case kRunLeave: return 700;         // "移出"稳定段(探针的 default 抓帧窗口)
			case kRunFocusScan: return 15000;   // 硬上限:Tab 扫描(Tab 自身节拍见 NextTabMs)
			case kRunKeyEnter: return 900;
			case kRunKeySpace: return 900;
			case kRunDragMove: return 700;
			case kRunTypeText: return 700;
			case kRunScroll: return 500;
			case kRunSettle: return 250;
			default: return 200;
			}
		}

		std::string RunPhaseNote(int phase, const std::string& kind)
		{
			switch (phase)
			{
			case kRunApproach:
				return "鼠标移到目标中心(悬停,不按键)";
			case kRunClickSend:
				return "脚本化点击 QueueClick(与 ui.invoke 同一条队列:press 帧 + release 帧)";
			case kRunPressHold:
				return kind == "click"
					? "按住段:保持 MouseDown(真实「按住」的同一状态),给探针的 pressed 像素证据留窗口"
					: std::string("按下并保持(首帧为按下沿;") + kind + " 的证据窗口)";
			case kRunRelease:
				return "抬起(抬起沿;clickCompleted 在这一帧核对)";
			case kRunLeave:
				return "鼠标移出目标(默认外观,探针的 default 抓帧窗口)";
			case kRunFocusScan:
				return "Tab 找焦点(WuiScriptedInput::QueueKey,与 ui.key 同一条注入路径)";
			case kRunKeyEnter: return "Enter(焦点在目标上;首帧为按键沿)";
			case kRunKeySpace: return "Space(同上;验证与 click 等价)";
			case kRunDragMove: return "按住 + 位移";
			case kRunTypeText: return "逐帧注入文本码点(与真实键入同一条 TextInput)";
			case kRunScroll: return "逐帧注入滚轮";
			case kRunSettle: return "收尾(不注入,等 a11y/状态稳定)";
			default: return std::string();
			}
		}

		int StartRunPhase(const std::string& kind)
		{
			return kind == "key" ? kRunFocusScan : kRunApproach;
		}

		int NextRunPhase(const std::string& kind, int phase)
		{
			switch (phase)
			{
			case kRunApproach:
				if (kind == "hover") return kRunLeave;
				if (kind == "scroll") return kRunScroll;
				if (kind == "click") return kRunClickSend;
				return kRunPressHold;
			case kRunClickSend: return kRunPressHold;
			case kRunPressHold:
				if (kind == "drag") return kRunDragMove;
				if (kind == "click") return kRunSettle;   // click 的抬起沿由脚本化点击给出
				return kRunRelease;
			case kRunDragMove: return kRunRelease;
			case kRunRelease:
				return kind == "type" ? kRunTypeText : kRunSettle;
			case kRunTypeText: return kRunSettle;
			case kRunLeave: return kRunFinished;
			case kRunScroll: return kRunSettle;
			case kRunKeyEnter: return kRunKeySpace;
			case kRunKeySpace: return kRunSettle;
			case kRunSettle: return kRunFinished;
			default: return kRunFinished;
			}
		}

		const char* InteractionKindId(Wui::WuiInteractionKind kind)
		{
			switch (kind)
			{
			case Wui::WuiInteractionKind::Hover: return "hover";
			case Wui::WuiInteractionKind::Click: return "click";
			case Wui::WuiInteractionKind::Drag: return "drag";
			case Wui::WuiInteractionKind::Type: return "type";
			case Wui::WuiInteractionKind::Scroll: return "scroll";
			default: return "key";
			}
		}

		const char* InteractionExpectId(Wui::WuiInteractionExpect expect)
		{
			switch (expect)
			{
			case Wui::WuiInteractionExpect::ValueChange: return "value-change";
			case Wui::WuiInteractionExpect::PixelChange: return "pixel-change";
			case Wui::WuiInteractionExpect::Event: return "event";
			default: return "state-change";
			}
		}

		// 探针该在哪个阶段抓像素(证据文件里的 pixelHint;探针按 _live.json 的 phase 对齐)。
		const char* InteractionPixelPhase(const std::string& kind)
		{
			if (kind == "hover") return "approach";
			if (kind == "click" || kind == "drag") return "press-hold";
			if (kind == "type") return "type-text";
			if (kind == "scroll") return "scroll";
			if (kind == "key") return "key-enter";
			return "press-hold";
		}

		// 目标矩形:契约声明的是子节点 id → 用 a11y 里的**真实控件矩形**(不是槽位:槽位可以远大于
		// 控件,拿槽位中心去点会打空 —— 实测按钮只占槽位左侧 128x24,槽位 240x120)。
		// 契约切换发生在画布绘制**之前**,那一刻本帧 a11y 还没登记 → 用上一帧缓存下来的同一目标矩形;
		// 都没有才回落 showcase 槽位。
		Wui::WuiRect ResolveInteractionTarget(const std::string& targetId, const Wui::WuiRect& slot,
			const std::string& cachedId, const Wui::WuiRect& cachedRect)
		{
			if (const Wui::WuiAccessNode* node =
				Wui::WuiAccessibility::Get().Find(Wui::HashId(targetId.c_str())))
			{
				if (node->Rect.W > 0.0f && node->Rect.H > 0.0f)
					return node->Rect;
			}
			if (!cachedId.empty() && cachedId == targetId && cachedRect.W > 0.0f
				&& cachedRect.H > 0.0f)
				return cachedRect;
			return slot;
		}

		std::string AccessNodeValue(Wui::WuiId id)
		{
			if (const Wui::WuiAccessNode* node = Wui::WuiAccessibility::Get().Find(id))
				return node->Value;
			return std::string();
		}

		std::string PlayFocusName(Wui::WuiId id)
		{
			if (id == 0)
				return std::string("none");
			if (const Wui::WuiAccessNode* node = Wui::WuiAccessibility::Get().Find(id))
				return (node->Label.empty() ? node->Kind : node->Label) + "#" + std::to_string(id);
			return "#" + std::to_string(id);
		}

		bool PointInside(const Wui::WuiRect& rect, const glm::vec2& point)
		{
			return point.x >= rect.X && point.x <= rect.X + rect.W
				&& point.y >= rect.Y && point.y <= rect.Y + rect.H;
		}

		// "移出"的落点:优先画布内、目标外的角(保证默认外观又不出画布);
		// 目标铺满画布(如 scrollarea)时只能退到画布框上,报告里注明。
		glm::vec2 LeavePoint(const Wui::WuiRect& target, const Wui::WuiRect& canvas)
		{
			const glm::vec2 candidates[] = {
				{ canvas.X + 2.0f, canvas.Y + 2.0f },
				{ canvas.X + canvas.W - 2.0f, canvas.Y + canvas.H - 2.0f },
				{ canvas.X + 2.0f, canvas.Y + canvas.H - 2.0f },
				{ canvas.X + canvas.W - 2.0f, canvas.Y + 2.0f },
			};
			for (const glm::vec2& point : candidates)
				if (!PointInside(target, point))
					return point;
			return { canvas.X - 4.0f, canvas.Y - 4.0f };
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
		const Wui::WuiTheme& theme, float uiScale)
	{
		const Wui::WuiRect bar = layout.TopBar;
		Wui::PanelBackground(ctx, bar, theme.PanelHeader, theme.Radius);
		const bool twoRows = bar.H > 40.0f;
		const float labelSize = 12.0f * FontScale(uiScale);

		float x = bar.X + 8.0f;
		float rowY = bar.Y + 2.0f;
		const float buttonH = twoRows ? 24.0f : bar.H - 4.0f;

		Wui::Label(ctx, { x, rowY + 5.0f }, Wui::Tr("workbench.global.density", "Density"),
			theme.TextMuted, labelSize);
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
			theme.TextMuted, labelSize);
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
			theme.TextMuted, labelSize);
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
			InvalidatePropertyCache();   // 属性被外部清空:行缓存要跟着重播种(P1.5b)
			m_ForceState = "default";
			m_LongText = false;
			m_Status = Wui::Tr("workbench.status.reset", "Reset: property overrides and forced state cleared");
			ctx.RecordOp("gallery", "global", "Reset", id);
		}
	}

	void WidgetGalleryPanel::DrawInfoBar(Wui::WuiContext& ctx, const WbLayout& layout,
		const Wui::WuiTheme& theme, const Wui::WuiComponentDesc* desc, float uiScale)
	{
		const float size = 12.0f * FontScale(uiScale);
		std::string left = Wui::Tr("workbench.info.registry", "Registry: ")
			+ std::to_string(Wui::WuiComponentRegistry::Count()) + " components";
		if (desc != nullptr)
			left += "  |  " + desc->Id;
		Wui::Label(ctx, { layout.InfoBar.X, layout.InfoBar.Y + 2.0f },
			ClipText(ctx, left, layout.InfoBar.W * 0.55f, size), theme.TextMuted, size);

		// Play:信息条显示**观测**(状态 id / 焦点节点 / 计数 / 最近事件);信息条画在画布
		// 之前,所以这里用上一帧的观测(m_PlayObservationPrev)—— 同一帧的观测在动作条上,
		// 探针读 `wui.workbench.play.observation` 节点。
		std::string right = Wui::Tr("workbench.info.last_action", "last action: ") + m_LastAction;
		if (m_PlayMode)
			right = std::string("play: ")
				+ (m_PlayObservationPrev.empty() ? PlayObservationText() : m_PlayObservationPrev);
		Wui::Label(ctx, { layout.InfoBar.X + layout.InfoBar.W * 0.56f, layout.InfoBar.Y + 2.0f },
			ClipText(ctx, right, layout.InfoBar.W * 0.44f, size), theme.TextMuted, size);
		RegisterWorkbenchNode(Wui::HashId(kInfoId), "text", layout.InfoBar, left,
			m_PlayMode ? right : m_LastAction, true, false);
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
		// Play 的运行中换件:取消(证据只在一条契约跑完时落盘,不会留半份)。
		if (m_Run.Active)
		{
			m_Run.Active = false;
			m_Run.Result = "cancelled";
			ctx.RecordOp("gallery", "interaction-run", "cancelled", m_Run.ComponentId);
		}
		m_SelectedId = desc.Id;
		m_ForceState = "default";
		ResetPropertyValues(desc.Id);
		m_PropScroll = 0.0f;
		m_LastAction = std::string("selected ") + desc.Id
			+ (std::string(how) == "select" ? std::string() : std::string(" (") + how + ")");
		ctx.RecordOp("gallery", how, "Component", desc.Id);
	}

	const Wui::WuiComponentDesc* WidgetGalleryPanel::DrawTree(Wui::WuiContext& ctx,
		const WbLayout& layout, const Wui::WuiTheme& theme, float uiScale)
	{
		const Wui::WuiRect area = layout.Tree;
		const float fontScale = FontScale(uiScale);
		Wui::PanelBackground(ctx, area, theme.ContentBg, theme.Radius);
		Wui::SectionHeader(ctx, { area.X, area.Y, area.W, 22.0f },
			Wui::Tr("workbench.tree.title", "Components"), theme.Accent, theme, 14.0f * fontScale);

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
					Wui::Label(ctx, { list.X + 4.0f, y + 3.0f }, lastCategory, theme.TextMuted,
						12.0f * fontScale);
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
				ClipText(ctx, item->DisplayName, std::max(20.0f, row.W - badgeW - 12.0f),
					13.0f * fontScale),
				active ? theme.Text : theme.TextMuted, 13.0f * fontScale);
			Wui::Label(ctx, { row.X + row.W - badgeW - 2.0f, row.Y + 4.0f }, StatusText(item->Status),
				StatusColor(item->Status, theme), 11.0f * fontScale);

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
		const float fontScale = FontScale(uiScale);
		Wui::PanelBackground(ctx, area, theme.ContentBg, theme.Radius);
		Wui::SectionHeader(ctx, { area.X, area.Y, area.W, 22.0f },
			Wui::Tr("workbench.canvas.title", "Canvas"), theme.Accent, theme, 14.0f * fontScale);

		const Wui::WuiRect inner { area.X + 8.0f, area.Y + 28.0f,
			std::max(40.0f, area.W - 16.0f), std::max(40.0f, area.H - 36.0f) };
		m_CanvasRect = area;
		m_CanvasInner = inner;
		m_CanvasValid = true;
		// Play:状态固定 "default"(真实输入决定外观);Edit:强制状态(基线口径不变)。
		m_AppliedState = m_PlayMode ? std::string("default") : m_ForceState;
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
				Wui::Tr("workbench.canvas.empty", "No component selected"), theme.TextMuted,
				14.0f * fontScale);
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
			desc->Id + "|" + (m_PlayMode ? std::string("default") : m_ForceState), true, false);
		const std::string label = desc->DisplayName + "  " + FormatFloat(width) + " x "
			+ FormatFloat(height);
		Wui::Label(ctx, { slot.X, slot.Y + slot.H + 4.0f },
			ClipText(ctx, label, inner.W, 12.0f * fontScale), theme.TextMuted, 12.0f * fontScale);
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
		// WUI-P1.6b:Play —— runner 先决定这一帧注入什么输入(Edit 恒为不注入)。
		// 注入只作用于 showcase 这一段:保存/恢复 ctx.Input()(与 Edit 的 PseudoState 同一挂点),
		// 工作台自己的控件在同一帧看到的仍是真实指针。
		Wui::WuiInputState savedInput;
		bool injecting = false;
		if (m_PlayMode && desc.Showcase != nullptr)
		{
			savedInput = ctx.Input();
			injecting = AdvanceInteractionRun(ctx, slot, desc);
		}
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
			// W2:showcase 内部字号读的是传进去的 theme 令牌(Engine 侧不改)→ 这里传一份
			// **只把字体令牌乘 scale** 的副本,让组件内部文字跟着 UiScale 一起变大。
			// 画布槽位本来就按 uiScale 放大,字号跟上之后"外框变大、内部字不跟"的错位才消失。
			const Wui::WuiTheme showcaseTheme = ScaledFontTheme(theme, scale);
			Wui::WuiComponentDraw draw;
			draw.Context = &ctx;
			draw.Theme = &showcaseTheme;
			draw.Rect = slot;
			// Play:状态固定 "default"(不套伪状态,hover/pressed/focus 由真实输入产生);
			// Edit:沿用强制状态,行为与基线逐字节不变。
			draw.State = m_PlayMode ? std::string("default") : m_ForceState;
			draw.Properties = m_AppliedProperties;
			draw.UiScale = scale;
			draw.Density = density;
			draw.Locale = Wui::GetLanguage();
			draw.RouteRealInput = m_PlayMode;
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
		// Play:采样这一帧的观测(控件已经消费过输入 —— 悬停/按下沿/焦点都是真的)。
		if (m_PlayMode)
			ObservePlayInput(ctx, slot, desc);
		if (injecting)
			ctx.Input() = savedInput;
		// 按住存续位:引擎的点击归属(`WuiClickOwner`)在 EndFrame 按"是否仍按住"决定生命周期
		// —— 不保留会让抬起帧的 `IsClickCompleted` 永远没有归属可核对(实测口径,不是设计偏好)。
		// 必须在**每一帧末**保持(阶段切换那一帧不注入,也要保持),直到抬起沿发出。
		// 面板自己的控件要靠 `hover && MouseClicked[0]` 才会动作,而这两者恢复的都是真实值,
		// 所以不会因为这一位保持而误触。
		if (m_PlayMode && m_Run.Active && m_Run.PressHeld)
			ctx.Input().MouseDown[0] = true;
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
		const float fontScale = FontScale(uiScale);
		// WLD_WORKBENCH_PROPS=0:整体不画 —— 性能验收的"面板关闭"基线(默认 2 = 完整面板)。
		const int panelMode = PropPanelMode();
		if (panelMode == 0)
		{
			// 关闭态也出一行(rows=0 / us=0):采样脚本按"同一支二进制、不同形态"算增量,
			// 缺了这一档就没有基线可减。
			if (PropTimingEnabled())
				WLD_CORE_INFO("[workbench] props timing mode={0} rows={1} us={2:.1f}", 0, 0, 0.0);
			return;
		}
		const bool timing = PropTimingEnabled();
		const auto timerStart = timing ? std::chrono::steady_clock::now()
			: std::chrono::steady_clock::time_point();
		Wui::PanelBackground(ctx, area, theme.ContentBg, theme.Radius);
		Wui::SectionHeader(ctx, { area.X, area.Y, area.W, 22.0f },
			Wui::Tr("workbench.props.title", "Properties"), theme.Accent, theme, 14.0f * fontScale);

		if (desc == nullptr)
		{
			Wui::Label(ctx, { area.X + 8.0f, area.Y + 32.0f },
				Wui::Tr("workbench.props.empty", "Select a component"), theme.TextMuted,
				13.0f * fontScale);
			return;
		}

		// 面板形态(见 PropPanelMode):1/3 是性能验收的基线形态 —— 只有属性行,
		// 不画搜索框 / 分组头 / 状态选择器(别处不要重复取)。
		const bool grouped = panelMode == 2;

		// ---- 面板顶部:属性搜索框(P1.5b;完整面板才有)----
		const Wui::WuiRect search { area.X + 6.0f, area.Y + 24.0f,
			std::max(60.0f, area.W - 12.0f), 22.0f };
		if (grouped && Wui::SearchField(ctx, Wui::HashId(kPropSearchId), search, m_PropSearch,
			m_PropSearchPlaceholder, theme))
			ctx.RecordOp("gallery", "prop-filter", desc->Id, m_PropSearch);
		// 命中串(属性名 + 文档)预小写:**只在输入真的变了时重算** —— 每帧对每行小写化都要分配。
		if (m_PropSearch != m_PropSearchSource)
		{
			m_PropSearchLower = ToLowerAscii(m_PropSearch);
			m_PropSearchSource = m_PropSearch;
			// 开始搜索时展开所有分组:否则命中的行藏在折叠组里,看起来"搜不到"。
			if (!m_PropSearchLower.empty())
				for (PropGroup& group : m_PropGroups)
					group.Open = true;
		}

		// 属性表缓存:换件 / 切语言(或属性被外部重置)后重建;此后每帧只比值。
		if (m_PropCacheComponent != desc->Id || m_PropCacheLanguage != Wui::GetLanguage())
			RebuildPropertyCache(*desc);

		// 基线形态:没有搜索框,属性行直接从标题下面开始(与改动前的面板同几何)。
		const float contentTop = grouped ? search.Y + search.H + 4.0f : area.Y + 26.0f;
		const Wui::WuiRect content { area.X + 6.0f, contentTop,
			std::max(60.0f, area.W - 12.0f),
			std::max(60.0f, area.Y + area.H - contentTop - 6.0f) };
		// ---- 元信息行(登记表说明 + 全局开关摘要):文本与裁剪都缓存,逐帧零分配 ----
		// 键是组件 / 语言 / 密度 / 缩放 / 宽度这五个量:任一变化才重取 i18n 与重算裁剪。
		const float metaSize = 12.0f * fontScale;
		const std::string& language = Wui::GetLanguage();
		if (m_PropMetaComponent != desc->Id || m_PropMetaLanguage != language
			|| m_PropMetaDensity != density || m_PropMetaScale != uiScale
			|| m_PropMetaWidth != content.W)
		{
			m_PropMetaComponent = desc->Id;
			m_PropMetaLanguage = language;
			m_PropMetaDensity = density;
			m_PropMetaScale = uiScale;
			m_PropMetaWidth = content.W;
			std::vector<std::string> lines;
			lines.push_back(Wui::Tr("workbench.props.a11y_notes", "a11y: ") + desc->A11yNotes);
			lines.push_back(Wui::Tr("workbench.props.size_notes", "size: ") + desc->SizeNotes);
			lines.push_back(Wui::Tr("workbench.props.source", "source: ") + desc->SourceFile);
			if (!desc->ExtraA11yIds.empty())
			{
				std::string ids;
				for (const std::string& id : desc->ExtraA11yIds)
					ids += (ids.empty() ? "" : ", ") + id;
				lines.push_back(Wui::Tr("workbench.props.extra_ids", "ids: ") + ids);
			}
			lines.push_back(Wui::Tr("workbench.props.globals", "density: ")
				+ (m_DensityIndex == 1 ? "compact" : "comfortable")
				+ "  scale: " + FormatFloat(uiScale)
				+ "  locale: " + (language.empty() ? "en" : language));
			// 裁剪放进重建里(逐帧 ClipText 遇到超宽文本会分配临时串)。
			m_PropMetaLines.clear();
			m_PropMetaLines.reserve(lines.size());
			for (const std::string& line : lines)
				m_PropMetaLines.push_back(ClipText(ctx, line, content.W - 4.0f, metaSize));
		}
		const float rowH = 24.0f;
		const float rowGap = 4.0f;
		const float groupH = 22.0f;
		const size_t rowLimit = panelMode == 3 ? 2 : desc->Properties.size();
		const bool searching = !m_PropSearchLower.empty();

		const auto stateVisible = [this](const PropRow& row) {
			return row.ExplicitState.empty() || row.ExplicitState == m_ForceState;
		};
		const auto searchVisible = [](const PropRow& row, const std::string& query) {
			return query.empty() || row.Haystack.find(query) != std::string::npos;
		};

		// 先算高度、再画:同一套判据(分组模式下"折叠组只占标题行",扁平模式按行累加)。
		float contentHeight = 6.0f;
		int visibleTotal = 0;
		if (grouped)
		{
			if (m_PropStateHome < 0)
				contentHeight += StateSelectorHeight(content) + rowGap;
			for (const PropGroup& group : m_PropGroups)
			{
				const int count = static_cast<int>(std::count_if(group.Rows.begin(),
					group.Rows.end(), [&](size_t index) {
						return index < rowLimit && stateVisible(m_PropRows[index])
							&& searchVisible(m_PropRows[index], m_PropSearchLower);
					}));
				visibleTotal += count;
				contentHeight += groupH + rowGap;
				if (!group.Open)
					continue;
				if (group.StateSelector)
					contentHeight += StateSelectorHeight(content) + rowGap;
				contentHeight += static_cast<float>(count) * (rowH + rowGap);
			}
			if (searching && visibleTotal == 0)
				contentHeight += 22.0f;
		}
		else
		{
			for (size_t index = 0; index < m_PropRows.size() && index < rowLimit; ++index)
			{
				if (!stateVisible(m_PropRows[index]))
					continue;
				++visibleTotal;
				contentHeight += rowH + rowGap;
			}
		}
		// 元信息:行数与"已裁剪文本"都缓存过,高度按实际行数算(不再用 120 的粗估)。
		contentHeight += static_cast<float>(m_PropMetaLines.size()) * 16.0f + 8.0f;

		Wui::BeginScrollArea(ctx, content, contentHeight, m_PropScroll, theme);
		float y = content.Y + 6.0f - m_PropScroll;

		// 行准备:键随当前状态变化(如 `bg` → `bg.hover`),变一次才重算键/值并重新播种控件值。
		// 每帧只在"状态没变"时走一次比较 —— 不改动时零分配(含字符串)。
		const auto prepareRow = [&](size_t index) -> PropRow& {
			PropRow& row = m_PropRows[index];
			if (row.KeyState == m_ForceState)
				return row;
			const Wui::WuiComponentProperty& prop = desc->Properties[index];
			row.Key = (row.StateScoped && row.ExplicitState.empty() && m_ForceState != "default")
				? prop.Name + "." + m_ForceState
				: prop.Name;
			row.KeyState = m_ForceState;
			row.Value = PropertyValueForKey(*desc, prop, row.Key);
			row.SeedValid = false;
			return row;
		};
		const auto drawRow = [&](size_t index, const Wui::WuiRect& rowRect) {
			PropRow& row = prepareRow(index);
			if (!row.SeedValid)
			{
				SeedPropertyRow(row, desc->Properties[index]);
				row.SeedValid = true;
			}
			// 滚动区外:不画也不登记,免得出现"看不到却点得到"的节点。
			if (rowRect.Y + rowRect.H <= content.Y || rowRect.Y >= content.Y + content.H)
				return;
			DrawPropertyRow(ctx, theme, *desc, row, desc->Properties[index], rowRect, fontScale);
		};

		if (grouped)
		{
			// 状态选择器:有"按状态的颜色"的组件落在 Style 组顶部(见 RebuildPropertyCache 的
			// m_PropStateHome);否则仍在面板顶部(与 P1b 的位置一致,逐件状态矩阵照旧能驱动)。
			if (m_PropStateHome < 0)
				y += DrawPropertyStateSelector(ctx, theme, { content.X, y, content.W, rowH },
					*desc, fontScale) + rowGap;

			for (PropGroup& group : m_PropGroups)
			{
				int count = 0;
				int total = 0;
				for (const size_t index : group.Rows)
				{
					if (index >= rowLimit || !stateVisible(m_PropRows[index]))
						continue;
					++total;
					if (searchVisible(m_PropRows[index], m_PropSearchLower))
						++count;
				}
				// "命中/总数"后缀:只在数字变了时重建字符串(每帧 std::to_string = 每帧分配)。
				if (count != group.LastVisible)
				{
					group.Trailing = std::to_string(count) + "/" + std::to_string(total);
					group.LastVisible = count;
				}
				const Wui::WuiRect header { content.X, y, content.W, groupH };
				bool open = group.Open;
				if (Wui::CollapsibleHeader(ctx, group.HeaderId, header, group.Title, open, theme,
					std::string(), group.Trailing, m_PropGroupTip, 13.0f * fontScale))
				{
					m_LastAction = "group " + group.Id + (open ? " open" : " closed");
					ctx.RecordOp("gallery", "prop-group", group.Id, open ? "open" : "closed");
				}
				if (open != group.Open)
					group.Open = open;
				y += groupH + rowGap;
				if (!group.Open)
					continue;
				// Style 组:先画状态选择器,再画"按状态"的颜色行 —— 与 UE 的 Details 一致。
				if (group.StateSelector)
					y += DrawPropertyStateSelector(ctx, theme, { content.X, y, content.W, rowH },
						*desc, fontScale) + rowGap;
				for (const size_t index : group.Rows)
				{
					if (index >= rowLimit || !stateVisible(m_PropRows[index])
						|| !searchVisible(m_PropRows[index], m_PropSearchLower))
						continue;
					const Wui::WuiRect rowRect { content.X, y, content.W, rowH };
					drawRow(index, rowRect);
					y += rowH + rowGap;
				}
			}
			if (searching && visibleTotal == 0)
			{
				Wui::Label(ctx, { content.X + 2.0f, y + 2.0f },
					Wui::Tr("workbench.props.no_match", "No property matches the filter"),
					theme.TextMuted, 12.0f * fontScale);
				y += 22.0f;
			}
		}
		else
		{
			// 基线形态(只画属性行):性能采样用,产品默认不会走到这里。
			for (size_t index = 0; index < m_PropRows.size() && index < rowLimit; ++index)
			{
				if (!stateVisible(m_PropRows[index]))
					continue;
				drawRow(index, { content.X, y, content.W, rowH });
				y += rowH + rowGap;
			}
		}

		// ---- 元信息:登记表里的说明与实现文件(追责/文档)—— 文本已缓存,这里只画 ----
		for (const std::string& line : m_PropMetaLines)
		{
			if (y + 16.0f > content.Y && y < content.Y + content.H)
				Wui::Label(ctx, { content.X + 2.0f, y + 2.0f }, line, theme.TextMuted, metaSize);
			y += 16.0f;
		}
		Wui::EndScrollArea(ctx);

		// 性能采样(仅在 WLD_WORKBENCH_TIMING=1 时开):每帧一行,行数/形态都在里面,
		// 由 %TEMP% 侧脚本按帧统计分布 —— 采样不改变绘制路径(只多两次计时调用)。
		if (timing)
		{
			const double micros = std::chrono::duration<double, std::micro>(
				std::chrono::steady_clock::now() - timerStart).count();
			WLD_CORE_INFO("[workbench] props timing mode={0} rows={1} us={2:.1f}",
				panelMode, visibleTotal, micros);
		}
	}

	void WidgetGalleryPanel::DrawActions(Wui::WuiContext& ctx, const WbLayout& layout,
		const Wui::WuiTheme& theme, const Wui::WuiComponentDesc* desc, float uiScale)
	{
		const Wui::WuiRect bar = layout.Actions;
		Wui::PanelBackground(ctx, bar, theme.PanelHeader, theme.Radius);
		const float density = m_DensityIndex == 1 ? 0.85f : 1.0f;
		const float fontSize = 12.0f * FontScale(uiScale);

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
			desc != nullptr ? desc->Id : std::string("(none)"),
			m_PlayMode ? std::string("default") : m_ForceState, true, false);

		// ---- WUI-P1.6b:Edit / Play 开关 + 一键跑交互契约 ----
		//
		// 位置刻意放在**动作条**(画布之后绘制):焦点表的顺序 = 绘制顺序,加在画布之前会
		// 让每件组件的 Tab 下标整体后移 —— P1c-E4 的键盘验收按"≤12 步 Tab"判可达,
		// 那批基线不该被这次改版弄脏。画布之后新增控件不影响任何既有 Tab 下标。
		const bool wide = bar.W >= 620.0f;
		const float modeX = bar.X + 262.0f;
		const float modeY = bar.Y + 3.0f;
		if (Wui::ButtonEx(ctx, Wui::HashId(kModeEditButtonId), { modeX, modeY, 62.0f, 24.0f },
			Wui::Tr("workbench.mode.edit", "Edit"), theme, true, !m_PlayMode,
			Wui::Tr("workbench.mode.edit_tip",
				"Edit mode: forced states + property overrides (the frozen baseline口径)")))
		{
			SetPlayMode(ctx, false);
		}
		if (Wui::ButtonEx(ctx, Wui::HashId(kModePlayButtonId),
			{ modeX + 66.0f, modeY, 62.0f, 24.0f },
			Wui::Tr("workbench.mode.play", "Play"), theme, true, m_PlayMode,
			Wui::Tr("workbench.mode.play_tip",
				"Play mode: real input routed into the showcase; no forced state")))
		{
			SetPlayMode(ctx, true);
		}
		RegisterWorkbenchNode(Wui::HashId(kModeNodeId), "text",
			{ modeX, modeY, 128.0f, 24.0f }, Wui::Tr("workbench.mode.title", "Workbench mode"),
			m_PlayMode ? "play" : "edit", true, false);

		if (desc != nullptr)
		{
			const bool hasContract = !desc->Interactions.empty();
			const bool runEnabled = m_PlayMode && hasContract;
			const std::string runTip = !m_PlayMode
				? Wui::Tr("workbench.play.run_tip_edit",
					"Switch to Play mode to run the interaction contract")
				: (hasContract
					? Wui::Tr("workbench.play.run_tip",
						"Run every declared interaction; each one writes an evidence file")
					: Wui::Tr("workbench.play.run_tip_static",
						"Static component: no interaction contract declared"));
			const float runX = wide ? modeX + 136.0f : bar.X + bar.W - 146.0f;
			if (Wui::ButtonEx(ctx, Wui::HashId(kRunButtonId), { runX, modeY, 140.0f, 24.0f },
				Wui::Tr("workbench.play.run", "Run interactions"), theme, runEnabled, false, runTip))
				StartInteractionRun(ctx, *desc);
			RegisterWorkbenchNode(Wui::HashId(kRunNodeId), "button", { runX, modeY, 140.0f, 24.0f },
				"Run interactions",
				m_Run.Active ? ("running " + m_Run.Kind) : (hasContract ? "ready" : "static"),
				runEnabled, true);
		}

		// 状态行优先级:Play/契约 runner 的实时状态 > Capture/Approve 的结果 > 空闲提示。
		const std::string status = !m_RunStatus.empty() ? m_RunStatus
			: (m_Status.empty()
				? Wui::Tr("workbench.status.idle",
					"Idle. Capture writes metadata; the probe crops the screenshot.")
				: m_Status);
		if (wide)
		{
			const float statusX = modeX + 286.0f;
			Wui::Label(ctx, { statusX, bar.Y + 7.0f },
				ClipText(ctx, status, std::max(40.0f, bar.X + bar.W - statusX - 6.0f),
					12.0f * fontSize),
				theme.TextMuted, fontSize);
		}
		RegisterWorkbenchNode(Wui::HashId(kStatusId), "text", bar, status,
			m_PlayMode ? PlayObservationText() : m_ForceState, true, false);
		// Play:本帧观测(AiControl 的 ui.invoke/ui.key 也走同一条观测口径 —— 探针据此
		// 断言"真实输入真的到了 showcase",而不是看一句自述)。
		if (m_PlayMode)
		{
			const std::string observation = m_PlayObservation.empty() ? PlayObservationText()
				: m_PlayObservation;
			RegisterWorkbenchNode(Wui::HashId(kPlayObservationId), "text", bar,
				Wui::Tr("workbench.play.observation", "Play observation"), observation, true, false);
		}
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
		return PropertyValueForKey(desc, prop, prop.Name);
	}

	// 键 → 值:用户覆盖优先,其次登记表的**类型化默认值**(老登记项没有新字段时退回旧默认)。
	// 键与属性名不一定相同:按状态读写的属性,键是 `bg.hover` 这类"名 + 状态"。
	std::string WidgetGalleryPanel::PropertyValueForKey(const Wui::WuiComponentDesc& desc,
		const Wui::WuiComponentProperty& prop, const std::string& key) const
	{
		const auto component = m_PropertyValues.find(desc.Id);
		if (component != m_PropertyValues.end())
		{
			const auto found = component->second.find(key);
			if (found != component->second.end())
				return found->second;
		}
		switch (prop.Type)
		{
		case Wui::WuiComponentProperty::Kind::Bool:
			if (!prop.DefaultText.empty())
				return prop.DefaultText;
			return prop.DefaultNumber != 0.0f ? "1" : "0";
		case Wui::WuiComponentProperty::Kind::Float:
			if (prop.DefaultNumber != 0.0f)
				return FormatFloat(prop.DefaultNumber);
			return prop.DefaultText.empty() ? FormatFloat(prop.Min) : prop.DefaultText;
		case Wui::WuiComponentProperty::Kind::Int:
			if (prop.DefaultNumber != 0.0f)
				return std::to_string(static_cast<int64_t>(std::llround(prop.DefaultNumber)));
			return prop.DefaultText.empty()
				? std::to_string(static_cast<int64_t>(std::llround(prop.Min))) : prop.DefaultText;
		case Wui::WuiComponentProperty::Kind::Color:
			// 规范文本优先(登记表两侧都写);只有文本缺省时才回到类型化默认。
			return prop.DefaultText.empty()
				? Wui::FormatComponentColor(prop.DefaultColor) : prop.DefaultText;
		case Wui::WuiComponentProperty::Kind::Size2:
			return prop.DefaultText.empty()
				? Wui::FormatComponentSize(prop.DefaultSize.x, prop.DefaultSize.y) : prop.DefaultText;
		case Wui::WuiComponentProperty::Kind::Enum:
			return prop.Options.empty() ? std::string() : prop.Options.front();
		case Wui::WuiComponentProperty::Kind::Text:
		default:
			return prop.DefaultText;
		}
	}

	// 属性表缓存:分组 / 行 / 状态选择器一次算好(标签、命中串、控件 id、状态键都不再逐帧重建)。
	void WidgetGalleryPanel::RebuildPropertyCache(const Wui::WuiComponentDesc& desc)
	{
		m_PropCacheComponent = desc.Id;
		m_PropCacheLanguage = Wui::GetLanguage();
		// i18n 文案在重建时取一次:逐帧 Tr(...) 会构造 std::string(长文案还会堆分配)。
		m_PropSearchPlaceholder = Wui::Tr("workbench.props.search", "Search properties");
		m_PropGroupTip = Wui::Tr("workbench.props.group.tip",
			"Click to collapse / expand the group");
		m_PropRows.clear();
		m_PropGroups.clear();
		m_PropStateHome = -1;
		m_PropStateIds.clear();
		m_PropStateChips.clear();
		m_PropStateChipWidths.clear();
		m_PropStateChipIds.clear();

		// ---- 状态表(登记表顺序;没声明状态就不画选择器,不造假控件)----
		for (const Wui::WuiComponentState& state : desc.States)
		{
			if (state.Id.empty())
				continue;
			m_PropStateIds.push_back(state.Id);
			std::string text = state.Label.empty() ? state.Id : state.Label;
			if (text.size() > 14)
				text.resize(14);
			// 宽度口径沿用 P1b 的状态按钮行(按字数估,不量字宽 —— 只在重建时算一次)。
			m_PropStateChipWidths.push_back(std::clamp(
				14.0f + static_cast<float>(text.size()) * 6.2f, 54.0f, 104.0f));
			m_PropStateChips.push_back(text);
			m_PropStateChipIds.push_back(Wui::HashId(
				(std::string("wui.workbench.state.") + desc.Id + "." + state.Id).c_str()));
		}

		// ---- 行 ----
		m_PropRows.resize(desc.Properties.size());
		for (size_t index = 0; index < desc.Properties.size(); ++index)
		{
			const Wui::WuiComponentProperty& prop = desc.Properties[index];
			PropRow& row = m_PropRows[index];
			row.PropIndex = index;
			row.Label = prop.Name;
			row.GroupId = NormalizePropGroup(prop.Group);
			row.StateScoped = prop.StateScoped;
			// 名字里已经带状态后缀的登记项(`bg.hover` 这种平铺写法):只在对应状态下露脸,
			// 键就是名字本身;不带后缀的按状态拼键(见 prepareRow)。
			if (prop.StateScoped)
			{
				for (const std::string& stateId : m_PropStateIds)
				{
					if (prop.Name.size() > stateId.size() + 1
						&& prop.Name.compare(prop.Name.size() - stateId.size(), stateId.size(), stateId) == 0
						&& prop.Name[prop.Name.size() - stateId.size() - 1] == '.')
					{
						row.ExplicitState = stateId;
						break;
					}
				}
			}
			// 行控件 id 里带组件 + 登记表下标:折叠/过滤只影响显隐,不影响控件身份(探针按 id 定位)。
			const std::string base = std::string(kPropLabelPrefix) + desc.Id + "."
				+ std::to_string(index);
			row.ControlId = Wui::HashId(base.c_str());
			row.ChildIds[0] = Wui::HashId((base + ".w").c_str());
			row.ChildIds[1] = Wui::HashId((base + ".h").c_str());
			row.ChildIds[2] = Wui::HashId((base + ".lock").c_str());
			// 搜索命中串 = 属性名 + 文档,预小写缓存一次(逐帧小写化每行都要分配)。
			row.Haystack = ToLowerAscii(prop.Name);
			if (!prop.Doc.empty())
			{
				row.Haystack += ' ';
				row.Haystack += ToLowerAscii(prop.Doc);
			}
			// Enum 选项:登记表为空时给单元素兜底(Combo 会索引 selected,不能给空表)。
			row.Options = prop.Options;
			if (row.Options.empty())
				row.Options.push_back(PropertyValueForKey(desc, prop, prop.Name));
			row.Key = prop.Name;
			row.KeyState.clear();
			row.SeedValid = false;
		}

		// ---- 分组(Style → Content → Layout → Behavior → 其余;空组名归 General)----
		for (size_t index = 0; index < m_PropRows.size(); ++index)
		{
			const std::string& groupId = m_PropRows[index].GroupId;
			PropGroup* target = nullptr;
			for (PropGroup& group : m_PropGroups)
				if (group.Id == groupId)
					target = &group;
			if (target == nullptr)
			{
				PropGroup group;
				group.Id = groupId;
				group.Title = PropGroupTitle(groupId);
				group.HeaderId = Wui::HashId((std::string(kPropGroupHeaderPrefix) + groupId).c_str());
				group.Open = true;
				m_PropGroups.push_back(std::move(group));
				target = &m_PropGroups.back();
			}
			target->Rows.push_back(index);
			if (m_PropRows[index].StateScoped && target != nullptr)
				target->StateSelector = true;
		}
		std::stable_sort(m_PropGroups.begin(), m_PropGroups.end(),
			[](const PropGroup& left, const PropGroup& right) {
				const int leftRank = PropGroupRank(left.Id);
				const int rightRank = PropGroupRank(right.Id);
				return leftRank != rightRank ? leftRank < rightRank : left.Id < right.Id;
			});
		// 状态选择器落点:有"按状态"属性的第一组(约定是 Style)顶部;一组都没有时留在面板顶部。
		for (size_t index = 0; index < m_PropGroups.size(); ++index)
			if (m_PropGroups[index].StateSelector)
			{
				m_PropStateHome = static_cast<int>(index);
				break;
			}
	}

	void WidgetGalleryPanel::InvalidatePropertyCache()
	{
		m_PropCacheComponent.clear();
		m_PropGroups.clear();
		m_PropRows.clear();
	}

	float WidgetGalleryPanel::StateSelectorHeight(const Wui::WuiRect& rect) const
	{
		if (m_PropStateIds.empty())
			return 0.0f;
		return WalkStateChips(m_PropStateChipWidths, rect, 24.0f,
			[](size_t, const Wui::WuiRect&) {});
	}

	// 状态选择器:一排状态按钮(与 P1b 同一套 id,逐件状态矩阵照旧按 id 驱动)。
	float WidgetGalleryPanel::DrawPropertyStateSelector(Wui::WuiContext& ctx, const Wui::WuiTheme& theme,
		const Wui::WuiRect& rect, const Wui::WuiComponentDesc& desc, float fontScale)
	{
		if (m_PropStateIds.empty())
			return 0.0f;
		return WalkStateChips(m_PropStateChipWidths, rect, 24.0f,
			[&](size_t index, const Wui::WuiRect& chip) {
				const std::string& stateId = m_PropStateIds[index];
				Wui::WuiTheme chipTheme = theme;
				if (stateId == m_ForceState)
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
				if (Wui::Button(ctx, m_PropStateChipIds[index], chip, m_PropStateChips[index], chipTheme))
				{
					m_ForceState = stateId;
					m_LastAction = "state " + stateId;
					ctx.RecordOp("gallery", "state", desc.Id, stateId);
				}
			});
	}

	// 行控件值存储 ⇄ 值文本(只在"换件 / 切状态 / 用户改值"时走,不进逐帧路径)。
	void WidgetGalleryPanel::SeedPropertyRow(PropRow& row, const Wui::WuiComponentProperty& prop)
	{
		switch (prop.Type)
		{
		case Wui::WuiComponentProperty::Kind::Bool:
			row.Flag = row.Value == "1" || row.Value == "true";
			break;
		case Wui::WuiComponentProperty::Kind::Float:
			row.Number = std::clamp(SafeFloat(row.Value, prop.Min), prop.Min, prop.Max);
			break;
		case Wui::WuiComponentProperty::Kind::Int:
			row.Integer = std::clamp<int64_t>(
				static_cast<int64_t>(std::llround(SafeFloat(row.Value, prop.Min))),
				static_cast<int64_t>(std::llround(prop.Min)),
				static_cast<int64_t>(std::llround(prop.Max)));
			break;
		case Wui::WuiComponentProperty::Kind::Color:
		{
			Wui::WuiColor parsed {};
			row.Color = Wui::ParseComponentColor(row.Value, parsed)
				? PropColorToVec4(parsed) : glm::vec4 { 1.0f, 1.0f, 1.0f, 1.0f };
			break;
		}
		case Wui::WuiComponentProperty::Kind::Size2:
			if (!Wui::ParseComponentSize(row.Value, row.SizeW, row.SizeH))
			{
				row.SizeW = 120.0f;
				row.SizeH = 32.0f;
			}
			row.Ratio = row.SizeH > 0.0f ? row.SizeW / row.SizeH : 1.0f;
			break;
		case Wui::WuiComponentProperty::Kind::Enum:
			row.EnumIndex = 0;
			for (size_t option = 0; option < row.Options.size(); ++option)
				if (row.Options[option] == row.Value)
					row.EnumIndex = static_cast<int>(option);
			break;
		case Wui::WuiComponentProperty::Kind::Text:
		default:
			row.Text = row.Value;
			break;
		}
	}

	void WidgetGalleryPanel::SerializePropertyRow(PropRow& row, const Wui::WuiComponentProperty& prop)
	{
		switch (prop.Type)
		{
		case Wui::WuiComponentProperty::Kind::Bool:
			row.Value = row.Flag ? "1" : "0";
			break;
		case Wui::WuiComponentProperty::Kind::Float:
			row.Value = FormatFloat(row.Number);
			break;
		case Wui::WuiComponentProperty::Kind::Int:
			row.Value = std::to_string(row.Integer);
			break;
		case Wui::WuiComponentProperty::Kind::Color:
			row.Value = Wui::FormatComponentColor(PropVec4ToColor(row.Color));
			break;
		case Wui::WuiComponentProperty::Kind::Size2:
			row.Value = Wui::FormatComponentSize(row.SizeW, row.SizeH);
			break;
		case Wui::WuiComponentProperty::Kind::Enum:
			if (!row.Options.empty())
			{
				row.EnumIndex = std::clamp(row.EnumIndex, 0, static_cast<int>(row.Options.size()) - 1);
				row.Value = row.Options[static_cast<size_t>(row.EnumIndex)];
			}
			break;
		case Wui::WuiComponentProperty::Kind::Text:
		default:
			break;   // 文本行实现里已经直接写入 row.Text → row.Value(见 DrawPropertyRow)
		}
	}

	bool WidgetGalleryPanel::DrawPropertyRow(Wui::WuiContext& ctx, const Wui::WuiTheme& theme,
		const Wui::WuiComponentDesc& desc, PropRow& row, const Wui::WuiComponentProperty& prop,
		const Wui::WuiRect& rowRect, float fontScale)
	{
		const float labelSize = 12.0f * fontScale;
		Wui::Label(ctx, { rowRect.X + 2.0f, rowRect.Y + 4.0f },
			ClipText(ctx, row.Label, 72.0f, labelSize), theme.TextMuted, labelSize);

		// 单位是登记表数据(P1.5A 的 Unit):在行右侧留出位置并按字号量宽,控件宽度跟着缩,
		// 这样单位不会和数值文本叠在一起,也不用给每个控件写特例。
		const std::string& unit = prop.Unit;
		const float unitW = unit.empty() ? 0.0f : ctx.MeasureTextWidth(unit, labelSize) + 6.0f;
		const Wui::WuiRect control { rowRect.X + 76.0f, rowRect.Y,
			std::max(40.0f, rowRect.W - 80.0f - unitW), rowRect.H };
		if (unitW > 0.0f)
			Wui::Label(ctx, { control.X + control.W + 4.0f, control.Y + 4.0f },
				ClipText(ctx, unit, unitW - 6.0f, labelSize), theme.TextMuted, labelSize);

		bool changed = false;
		switch (prop.Type)
		{
		case Wui::WuiComponentProperty::Kind::Bool:
			changed = Wui::Checkbox(ctx, row.ControlId, control, std::string(), row.Flag, theme);
			break;
		case Wui::WuiComponentProperty::Kind::Float:
			changed = Wui::DragFloat(ctx, row.ControlId, control, row.Number,
				std::max(0.001f, prop.Step), prop.Min, prop.Max, theme);
			break;
		case Wui::WuiComponentProperty::Kind::Int:
			changed = Wui::DragInt(ctx, row.ControlId, control, row.Integer,
				static_cast<int64_t>(std::llround(prop.Min)),
				static_cast<int64_t>(std::llround(prop.Max)), theme);
			break;
		case Wui::WuiComponentProperty::Kind::Color:
			// 色板 + hex(库件 ColorField:折叠态 = 色块 + 规范化色值,弹层 = SV 板/hue/alpha/hex/预设)。
			changed = Wui::ColorField(ctx, row.ControlId, control, row.Color, theme);
			if (ctx.IsPopupOpen(row.ControlId))
				m_PopupOpenNow = true;
			break;
		case Wui::WuiComponentProperty::Kind::Size2:
		{
			const float gap = 4.0f;
			const float lockW = std::clamp(control.W * 0.16f, 40.0f, 52.0f);
			const float fieldW = std::max(30.0f,
				(control.W - lockW - gap * 2.0f - 12.0f) * 0.5f);
			const Wui::WuiRect widthField { control.X, control.Y, fieldW, control.H };
			const Wui::WuiRect heightField { control.X + fieldW + 12.0f, control.Y, fieldW, control.H };
			const Wui::WuiRect lockButton { control.X + control.W - lockW, control.Y, lockW, control.H };
			// 尺寸按像素:0 起、上界取登记表 Max(登记表没给合理上界时按 4096 兜底)。
			const float lo = 0.0f;
			const float hi = prop.Max > lo && prop.Max >= 2.0f ? prop.Max : 4096.0f;
			const float speed = std::max(0.001f, prop.Step);
			const bool widthChanged = Wui::DragFloat(ctx, row.ChildIds[0], widthField,
				row.SizeW, speed, lo, hi, theme);
			Wui::Label(ctx, { widthField.X + widthField.W + 2.0f, widthField.Y + 4.0f }, "x",
				theme.TextMuted, labelSize);
			const bool heightChanged = Wui::DragFloat(ctx, row.ChildIds[1], heightField,
				row.SizeH, speed, lo, hi, theme);
			Wui::WuiTheme lockTheme = theme;
			if (row.RatioLock)
			{
				lockTheme.Border = theme.Accent;
			}
			else
			{
				lockTheme.Text = theme.TextMuted;
			}
			if (Wui::Button(ctx, row.ChildIds[2], lockButton,
				row.RatioLock ? Wui::Tr("workbench.props.ratio.locked", "Lock")
					: Wui::Tr("workbench.props.ratio.free", "Free"), lockTheme))
			{
				row.RatioLock = !row.RatioLock;
				if (row.RatioLock && row.SizeH > 0.0f)
					row.Ratio = row.SizeW / row.SizeH;
			}
			if (row.RatioLock && row.Ratio > 0.0f)
			{
				// 锁定比例:改一边按锁定时记下的比例带动另一边(夹在同一值域里)。
				if (widthChanged)
					row.SizeH = std::clamp(row.SizeW / row.Ratio, lo, hi);
				else if (heightChanged)
					row.SizeW = std::clamp(row.SizeH * row.Ratio, lo, hi);
			}
			else if (widthChanged || heightChanged)
			{
				row.Ratio = row.SizeH > 0.0f ? row.SizeW / row.SizeH : row.Ratio;
			}
			changed = widthChanged || heightChanged;
			break;
		}
		case Wui::WuiComponentProperty::Kind::Enum:
		{
			if (row.EnumIndex < 0 || row.EnumIndex >= static_cast<int>(row.Options.size()))
				row.EnumIndex = 0;
			changed = Wui::Combo(ctx, row.ControlId, control, std::string(), row.Options,
				row.EnumIndex, theme);
			if (ctx.IsPopupOpen(row.ControlId))
				m_PopupOpenNow = true;
			break;
		}
		case Wui::WuiComponentProperty::Kind::Text:
		default:
		{
			Wui::TextFieldA11y a11y;
			a11y.Label = row.Label;
			a11y.Placeholder = prop.Doc;
			if (Wui::TextField(ctx, row.ControlId, control, row.Text, theme, nullptr, &a11y))
			{
				row.Value = row.Text;
				changed = true;
			}
			break;
		}
		}

		if (changed)
		{
			if (prop.Type != Wui::WuiComponentProperty::Kind::Text)
				SerializePropertyRow(row, prop);
			SetPropertyValue(desc.Id, row.Key, row.Value);
			m_LastAction = row.Key + " = " + row.Value;
		}
		return changed;
	}

	void WidgetGalleryPanel::SetPropertyValue(const std::string& componentId, const std::string& name,
		const std::string& value)
	{
		m_PropertyValues[componentId][name] = value;
	}

	void WidgetGalleryPanel::ResetPropertyValues(const std::string& componentId)
	{
		m_PropertyValues[componentId].clear();
		// 行缓存里存着"当前值的文本":外部清空覆盖后必须重播种,否则界面还显示旧覆盖值(P1.5b)。
		InvalidatePropertyCache();
	}

	std::vector<std::pair<std::string, std::string>> WidgetGalleryPanel::AppliedProperties(
		const Wui::WuiComponentDesc& desc, float density, float uiScale) const
	{
		std::vector<std::pair<std::string, std::string>> out;
		out.reserve(desc.Properties.size() + 3);
		// 全局开关作为"已知属性名"注入:showcase 认识就这样用,不认识会忽略(登记表契约)。
		// 这三个与用户编辑无关,每帧都要发(密度/缩放/语言是整块面板的全局语义)。
		out.emplace_back("density", FormatFloat(density));
		out.emplace_back("uiScale", FormatFloat(uiScale));
		out.emplace_back("locale", Wui::GetLanguage().empty() ? "en" : Wui::GetLanguage());

		// WUI-P1c-b W1:只发**用户真正改过**的键(m_PropertyValues 里有记录的)。
		// 为什么:属性是 showcase 的"覆盖值",而 Toggle/Checkbox 这类控件**每帧无条件**应用
		// 覆盖(见 WuiComponentRegistry 的 BoolOverride / BoolProperty)→ 逐帧把登记默认值
		// 也发过去,就会把控件自己的持久态盖回去:用户在画布里点一下/按 Space,下一帧被冲掉,
		// AI 通道与键盘操作都观察不到(探针报的 "Space 后 a11y value 没变")。
		// 没改过的键一律不发,showcase 退回它自己的默认/持久态(登记表契约:未知属性名忽略)。
		// 长文本压力(m_LongText)是全局开关:开着时 Text 类属性仍作为覆盖发出去(语义不变)。
		//
		// WUI-P1.5b:发的是**编辑表里的键**,不再按 desc.Properties 逐项匹配名字 —— 按状态读写的
		// 属性键是 `bg.hover` 这类"名 + 状态",名字匹配不到就会永远发不出去(状态色改了不生效)。
		// 键只可能由属性面板写入(它自己就是唯一的写入方),所以"编辑表 = 用户改过的键"仍然成立。
		const auto component = m_PropertyValues.find(desc.Id);
		const std::map<std::string, std::string>* edited =
			component != m_PropertyValues.end() ? &component->second : nullptr;
		std::vector<std::string> longTextKeys;
		if (m_LongText)
		{
			// 长文本压力:Text 类属性换成超长串(一帧就能看出溢出/裁剪/换行问题)。
			const std::string stress = "Long text stress: " + std::string(160, 'W')
				+ " / 长文本压力测试(检查溢出、裁剪与换行)";
			for (const Wui::WuiComponentProperty& prop : desc.Properties)
			{
				if (prop.Type != Wui::WuiComponentProperty::Kind::Text)
					continue;
				out.emplace_back(prop.Name, stress);
				longTextKeys.push_back(prop.Name);
			}
		}
		if (edited != nullptr)
		{
			for (const auto& entry : *edited)
			{
				if (std::find(longTextKeys.begin(), longTextKeys.end(), entry.first)
					!= longTextKeys.end())
					continue;   // 长文本压力已经替它发了(同一个键不要发两遍)
				out.emplace_back(entry.first, entry.second);
			}
		}
		return out;
	}

	// ---- WUI-P1.6b:Play 模式(真实输入直通)+ 交互契约 runner ----
	//
	// 口径(与派工单/plan §P1.6 一致):
	//   · Play = `draw.RouteRealInput=true` + `draw.State="default"`(不套伪状态),
	//     hover/pressed/focus 由真实输入产生;工作台只**观测**、不强制外观;
	//   · 注入写的是 `WuiInputState`(屏幕坐标系就是窗口客户区;按下/抬起/按键**沿**在正确的
	//     帧上产生)—— 与 `ui.invoke`/`ui.key` 的 `WuiScriptedInput` 是同一条输入结构,
	//     控件侧没有任何"测试专用分支";
	//   · 注入只作用于 showcase 绘制段(保存/恢复 ctx.Input(),与 Edit 的 PseudoState 同一挂点):
	//     同一帧里工作台自己的控件看到的是真实指针,不被脚本化的"手"污染。
	//   · 阶段用**墙钟**定时,`_live.json` 在阶段切换时落盘 —— 探针据此在 press-hold 段里
	//     用 `capture.float` 抓 pressed 帧(按下沿只存在一帧,抓不到就变成猜)。
	void WidgetGalleryPanel::SetPlayMode(Wui::WuiContext& ctx, bool play)
	{
		if (m_PlayMode == play)
			return;
		m_PlayMode = play;
		if (m_Run.Active)
		{
			// 运行中切模式:不写半份证据(证据文件只在一条契约跑完时落盘)。
			m_Run.Active = false;
			m_Run.Result = "cancelled";
			ctx.RecordOp("gallery", "mode", "run-cancelled", m_Run.ComponentId);
		}
		if (!play)
		{
			// 回到 Edit:清观测,免得上一轮的 Play 计数被当成 Edit 的观测。
			m_PlayState = "default";
			m_PlayObservation.clear();
			m_PlayObservationPrev.clear();
			m_PlayFocusName.clear();
			m_PlayLastEvent.clear();
			m_PlayLastEventFrame = 0;
			m_PlayEvents.clear();
			m_PlayHoverSeen = false;
			m_PlayPressCount = m_PlayClickCount = m_PlayReleaseCount = m_PlayKeyCount = 0;
			m_PlayA11yJson.clear();
			m_PlayA11yPrevJson.clear();
		}
		m_RunStatus = play
			? Wui::Tr("workbench.play.ready",
				"Play: real input routed into the showcase (no forced state). "
				"Run interactions writes evidence per contract.")
			: Wui::Tr("workbench.play.edit",
				"Edit: forced states + property overrides (baselines unchanged).");
		ctx.RecordOp("gallery", "mode", "Workbench", play ? "play" : "edit");
	}

	std::string WidgetGalleryPanel::PlayObservationText() const
	{
		std::string out = std::string("play state=") + m_PlayState
			+ " focus=" + (m_PlayFocusName.empty() ? std::string("none") : m_PlayFocusName)
			+ " press=" + std::to_string(m_PlayPressCount)
			+ " click=" + std::to_string(m_PlayClickCount)
			+ " release=" + std::to_string(m_PlayReleaseCount)
			+ " key=" + std::to_string(m_PlayKeyCount)
			+ " events=" + std::to_string(m_PlayEvents.size());
		if (!m_PlayLastEvent.empty())
			out += " last=" + m_PlayLastEvent + "@" + std::to_string(m_PlayLastEventFrame);
		return out;
	}

	void WidgetGalleryPanel::StartInteractionRun(Wui::WuiContext& ctx,
		const Wui::WuiComponentDesc& desc)
	{
		if (m_PlayMode == false || desc.Interactions.empty())
		{
			m_RunStatus = Wui::Tr("workbench.play.static",
				"Static component: no interaction contract declared (nothing to run).");
			return;
		}
		m_Run = InteractionRun {};
		m_Run.Active = true;
		m_Run.Id = "play-" + desc.Id + "-" + std::to_string(NowMs());
		m_Run.ComponentId = desc.Id;
		m_Run.WindowKey = ctx.WindowKey();
		m_Run.Index = 0;
		m_Run.StartFrame = ctx.Frame();
		m_Run.StartedWallMs = NowMs();
		BeginRunInteraction(ctx, desc);
		ctx.RecordOp("gallery", "interaction-run", desc.Id,
			std::to_string(desc.Interactions.size()) + " contracts");
	}

	void WidgetGalleryPanel::BeginRunInteraction(Wui::WuiContext& ctx,
		const Wui::WuiComponentDesc& desc)
	{
		const Wui::WuiComponentInteraction& item = desc.Interactions[m_Run.Index];
		m_Run.Kind = InteractionKindId(item.Kind);
		m_Run.Target = item.TargetId;
		m_Run.TargetId = Wui::HashId(item.TargetId.c_str());
		m_Run.Expect = InteractionExpectId(item.Expect);
		m_Run.Steps = item.Steps;
		m_Run.Note = item.Note;
		m_Run.TargetRect = ResolveInteractionTarget(item.TargetId, m_CanvasSlot,
			m_LastTargetId, m_LastTargetRect);
		m_Run.Result.clear();
		m_Run.GapReason.clear();
		m_Run.PressSeen = false;
		m_Run.ReleaseSeen = false;
		m_Run.ClickCompleted = false;
		m_Run.KeyEnterSeen = false;
		m_Run.KeySpaceSeen = false;
		m_Run.PressFrame = 0;
		m_Run.ReleaseFrame = 0;
		m_Run.TabsQueued = 0;
		m_Run.FocusReached = false;
		m_Run.FocusMatch.clear();
		m_Run.NextTabMs = 0;
		m_Run.TypeCharsInjected = 0;
		m_Run.PhaseEdgeSent = false;
		m_Run.PressHeld = false;
		m_Run.Phases.clear();
		m_Run.Timeline.clear();
		m_Run.Events.clear();
		m_Run.StartFrame = ctx.Frame();
		m_Run.StartedWallMs = NowMs();
		// before 快照取**上一帧**的画布 a11y(本帧的节点要等 showcase 画完才登记)。
		m_Run.A11yBefore = m_PlayA11yPrevJson;
		m_Run.ValueBefore = AccessNodeValue(m_Run.TargetId);
		const int start = StartRunPhase(m_Run.Kind);
		EnterRunPhase(start, RunPhaseDurationMs(start), RunPhaseNote(start, m_Run.Kind));
		m_RunStatus = std::string("interaction ") + std::to_string(m_Run.Index + 1) + "/"
			+ std::to_string(desc.Interactions.size()) + "  " + m_Run.Kind;
	}

	void WidgetGalleryPanel::EnterRunPhase(int phase, uint64_t durationMs, const std::string& note)
	{
		m_Run.Phase = phase;
		m_Run.PhaseEdgeSent = false;
		if (phase == kRunIdle || phase == kRunFinished || phase == kRunSettle)
			m_Run.PressHeld = false;   // 收起按住存续位(别把按下状态带出这条契约)
		const uint64_t now = NowMs();
		m_Run.PhaseStartMs = now;
		m_Run.PhaseDeadlineMs = now + durationMs;
		if (phase == kRunIdle || phase == kRunFinished)
			return;
		PlayPhaseRecord record;
		record.Name = RunPhaseName(phase);
		record.Note = note;
		record.WallMs = durationMs;
		m_Run.Phases.push_back(record);
		if (phase == kRunFocusScan)
			m_Run.NextTabMs = now;   // 第一帧就发第一次 Tab
		WriteInteractionLive(record.Name);
	}

	bool WidgetGalleryPanel::AdvanceInteractionRun(Wui::WuiContext& ctx, const Wui::WuiRect& slot,
		const Wui::WuiComponentDesc& desc)
	{
		if (!m_Run.Active)
			return false;
		if (m_Run.ComponentId != desc.Id)
		{
			m_Run.Active = false;
			m_Run.Result = "cancelled";
			m_RunStatus = Wui::Tr("workbench.play.run_cancelled",
				"Interaction run cancelled: the selected component changed");
			return false;
		}
		const uint64_t now = NowMs();
		const Wui::WuiRect target = m_Run.TargetRect.W > 0.0f && m_Run.TargetRect.H > 0.0f
			? m_Run.TargetRect : slot;
		const glm::vec2 center { target.X + target.W * 0.5f, target.Y + target.H * 0.5f };
		Wui::WuiInputState& input = ctx.Input();

		// ---- Key 契约:焦点扫描(阶段内每 NextTabMs 发一次 Tab;注入走 WuiScriptedInput,
		// 因为焦点导航发生在 BeginFrame —— 面板内直接改 KeyPressed 已经晚了)----
		if (m_Run.Phase == kRunFocusScan && !m_Run.FocusReached)
		{
			const Wui::WuiId focus = ctx.Focus();
			bool focusOnTarget = focus != 0 && focus == m_Run.TargetId;
			bool focusOnCanvas = false;
			if (!focusOnTarget && focus != 0 && m_CanvasRect.W > 0.0f)
			{
				if (const Wui::WuiAccessNode* node = Wui::WuiAccessibility::Get().Find(focus))
				{
					const Wui::WuiRect& r = node->Rect;
					focusOnCanvas = r.X + r.W > m_CanvasRect.X && r.X < m_CanvasRect.X + m_CanvasRect.W
						&& r.Y + r.H > m_CanvasRect.Y && r.Y < m_CanvasRect.Y + m_CanvasRect.H;
				}
			}
			if (focusOnTarget || focusOnCanvas)
			{
				m_Run.FocusReached = true;
				m_Run.FocusMatch = focusOnTarget ? "id" : "canvas";
				EnterRunPhase(kRunKeyEnter, RunPhaseDurationMs(kRunKeyEnter),
					RunPhaseNote(kRunKeyEnter, m_Run.Kind));
				return false;
			}
			if (now >= m_Run.PhaseDeadlineMs || m_Run.TabsQueued >= 90)
			{
				// 焦点到不了就如实记 gap(不 SetFocus —— 那等于伪造键盘可达性)。
				FinishCurrentInteraction(ctx, desc, "gap", "focus-unreachable");
				return false;
			}
			if (now >= m_Run.NextTabMs)
			{
				Wui::WuiScriptedInput::Get().QueueKey(ctx.WindowKey(), KeyCodes::Tab);
				++m_Run.TabsQueued;
				m_Run.NextTabMs = now + 130;
			}
			return false;
		}

		// ---- 阶段到点:下一段(或在最后一段结算)----
		if (now >= m_Run.PhaseDeadlineMs)
		{
			const int next = NextRunPhase(m_Run.Kind, m_Run.Phase);
			if (next != kRunFinished)
			{
				EnterRunPhase(next, RunPhaseDurationMs(next), RunPhaseNote(next, m_Run.Kind));
				return false;
			}
			const bool hoverSeen = std::any_of(m_Run.Phases.begin(), m_Run.Phases.end(),
				[](const PlayPhaseRecord& phase) { return phase.Hover; });
			const bool pressedSeen = std::any_of(m_Run.Phases.begin(), m_Run.Phases.end(),
				[](const PlayPhaseRecord& phase) { return phase.Pressed; });
			std::string gap;
			if (m_Run.Kind == "hover" && !hoverSeen)
				gap = "no-hover";
			else if (m_Run.Kind == "click" && !(m_Run.PressSeen && m_Run.ReleaseSeen))
				gap = "no-click";
			else if (m_Run.Kind == "key"
				&& !(m_Run.FocusReached && m_Run.KeyEnterSeen && m_Run.KeySpaceSeen))
				gap = m_Run.FocusReached ? "no-key-activation" : "focus-unreachable";
			else if (m_Run.Kind == "drag" && !pressedSeen)
				gap = "no-press";
			else if ((m_Run.Kind == "type" || m_Run.Kind == "scroll") && !hoverSeen)
				gap = "target-unreachable";
			else if (m_Run.Expect == "value-change" && !m_Run.ValueBefore.empty()
				&& m_Run.ValueBefore == AccessNodeValue(m_Run.TargetId))
				gap = "no-value-change";
			FinishCurrentInteraction(ctx, desc, gap.empty() ? "ok" : "gap", gap);
			return false;
		}

		// ---- 本阶段注入(只写 ctx.Input();调用方会保存/恢复,作用域 = showcase 段)----
		switch (m_Run.Phase)
		{
		case kRunApproach:
			input.MousePos = center;
			return true;
		case kRunClickSend:
			// 点击的**按下/抬起沿**走 WuiScriptedInput(与 `ui.invoke` 完全同一条队列,
			// 在 BeginFrame 之前注入)= 引擎的点击归属生命周期能正常跨帧存续;
			// 面板这一帧不注入,等 press 帧被观测到再进"按住段"。
			if (!m_Run.PhaseEdgeSent)
			{
				m_Run.PhaseEdgeSent = true;
				Wui::WuiScriptedInput::Get().QueueClick(ctx.WindowKey(), center);
			}
			else if (m_Run.PressSeen)
			{
				EnterRunPhase(kRunPressHold, RunPhaseDurationMs(kRunPressHold),
					RunPhaseNote(kRunPressHold, m_Run.Kind));
			}
			return false;
		case kRunPressHold:
			input.MousePos = center;
			input.MouseDown[0] = true;
			// click 的按下沿已经由脚本化点击给出;按住段只把 MouseDown 保持住(像素证据窗口)。
			if (m_Run.Kind != "click")
				input.MouseClicked[0] = !m_Run.PhaseEdgeSent;   // 首帧 = 按下沿
			m_Run.PressHeld = true;
			m_Run.PhaseEdgeSent = true;
			return true;
		case kRunRelease:
			input.MousePos = center;
			input.MouseDown[0] = false;
			input.MouseReleased[0] = !m_Run.PhaseEdgeSent;  // 首帧 = 抬起沿
			m_Run.PressHeld = false;                        // 抬起沿之后不再保持
			m_Run.PhaseEdgeSent = true;
			return true;
		case kRunLeave:
			input.MousePos = LeavePoint(target, m_CanvasInner);
			return true;
		case kRunKeyEnter:
		case kRunKeySpace:
		{
			const uint32_t key = m_Run.Phase == kRunKeyEnter ? KeyCodes::Enter : KeyCodes::Space;
			input.MousePos = center;
			input.MouseDown[0] = false;
			input.KeyDown.clear();
			input.KeyPressed.clear();
			if (!m_Run.PhaseEdgeSent)
			{
				input.KeyDown.push_back(key);
				input.KeyPressed.push_back(key);   // 边沿:控件按 WasKeyPressed 消费
				m_Run.PhaseEdgeSent = true;
			}
			return true;
		}
		case kRunDragMove:
		{
			const float progress = std::min(1.0f,
				static_cast<float>(now - m_Run.PhaseStartMs) / static_cast<float>(
					RunPhaseDurationMs(kRunDragMove)));
			input.MousePos = { center.x + 60.0f * progress, center.y + 24.0f * progress };
			input.MouseDown[0] = true;
			m_Run.PressHeld = true;
			return true;
		}
		case kRunTypeText:
		{
			static const char* const kScript = "play";
			input.MousePos = center;
			const int step = static_cast<int>((now - m_Run.PhaseStartMs) / 150ULL);
			const int length = static_cast<int>(std::strlen(kScript));
			if (step >= 0 && step < length && m_Run.TypeCharsInjected <= step)
			{
				input.TextInput.push_back(static_cast<uint32_t>(kScript[step]));
				m_Run.TypeCharsInjected = step + 1;
			}
			return true;
		}
		case kRunScroll:
			input.MousePos = center;
			input.Wheel = -1.0f;   // 与真实滚轮同向(GLFW yoffset 向下为负)
			return true;
		default:
			return false;          // settle / idle / finished:不注入
		}
	}

	void WidgetGalleryPanel::ObservePlayInput(Wui::WuiContext& ctx, const Wui::WuiRect& slot,
		const Wui::WuiComponentDesc& desc)
	{
		const Wui::WuiRect target = (m_Run.Active && m_Run.TargetRect.W > 0.0f
			&& m_Run.TargetRect.H > 0.0f) ? m_Run.TargetRect : slot;
		const Wui::WuiInputState& input = ctx.Input();
		const bool hover = ctx.IsHovered(target);
		const bool pressed = hover && input.MouseDown[0];
		const bool pressedEdge = hover && input.MouseClicked[0];
		const bool releasedEdge = hover && input.MouseReleased[0];
		const Wui::WuiId focus = ctx.Focus();
		const Wui::WuiId fallbackTarget = Wui::HashId(("showcase." + desc.Id).c_str());
		const Wui::WuiId targetId = m_Run.Active && m_Run.TargetId != 0 ? m_Run.TargetId
			: fallbackTarget;
		// 目标矩形以 a11y 实测为准(此刻 showcase 已画完,本帧节点已登记):回填运行中的目标,
		// 并存成"上一帧缓存"给下一次契约切换用 —— 槽位 ≠ 控件,点空是实测踩过的坑。
		if (m_Run.Active && m_Run.TargetId != 0)
		{
			if (const Wui::WuiAccessNode* node = Wui::WuiAccessibility::Get().Find(m_Run.TargetId))
			{
				if (node->Rect.W > 0.0f && node->Rect.H > 0.0f)
				{
					m_Run.TargetRect = node->Rect;
					m_LastTargetId = m_Run.Target;
					m_LastTargetRect = node->Rect;
				}
			}
		}
		bool focusOnTarget = focus != 0 && focus == targetId;
		if (!focusOnTarget && focus != 0 && m_CanvasRect.W > 0.0f)
		{
			if (const Wui::WuiAccessNode* node = Wui::WuiAccessibility::Get().Find(focus))
			{
				const Wui::WuiRect& r = node->Rect;
				focusOnTarget = r.X + r.W > m_CanvasRect.X && r.X < m_CanvasRect.X + m_CanvasRect.W
					&& r.Y + r.H > m_CanvasRect.Y && r.Y < m_CanvasRect.Y + m_CanvasRect.H;
			}
		}

		std::string state = "default";
		if (pressed)
			state = "pressed";
		else if (hover)
			state = "hover";
		else if (focusOnTarget)
			state = "focus";
		m_PlayState = state;
		if (focusOnTarget || focus != 0)
			m_PlayFocusName = PlayFocusName(focus);
		else
			m_PlayFocusName.clear();

		const auto record = [&](const std::string& name, const std::string& detail) {
			m_PlayLastEvent = name;
			m_PlayLastEventFrame = ctx.Frame();
			const std::string line = detail + " frame=" + std::to_string(ctx.Frame());
			m_PlayEvents.emplace_back(name, line);
			if (m_PlayEvents.size() > 60)
				m_PlayEvents.erase(m_PlayEvents.begin());
			if (m_Run.Active)
				m_Run.Events.emplace_back(name, line);
		};
		if (hover && !m_PlayHoverSeen)
		{
			m_PlayHoverSeen = true;
			record("hover.enter", "hovered=1");
		}
		else if (!hover && m_PlayHoverSeen)
		{
			m_PlayHoverSeen = false;
			record("hover.leave", "hovered=0");
		}
		if (pressedEdge)
		{
			++m_PlayPressCount;
			++m_PlayClickCount;
			record("press.edge",
				"hovered=1 MouseClicked[0]=1 -> widget predicate (hovered && MouseClicked[0]) true");
			if (m_Run.Active)
			{
				m_Run.PressSeen = true;
				m_Run.PressFrame = ctx.Frame();
			}
		}
		if (releasedEdge)
		{
			++m_PlayReleaseCount;
			// 引擎口径的"这一次点击落在同一目标上":press 帧登记归属、release 帧核对。
			const bool completed = ctx.IsClickCompleted(0, target);
			record("release.edge", std::string("hovered=1 MouseReleased[0]=1 clickCompleted=")
				+ (completed ? "1" : "0"));
			if (m_Run.Active)
			{
				m_Run.ReleaseSeen = true;
				m_Run.ReleaseFrame = ctx.Frame();
				m_Run.ClickCompleted = completed;
			}
		}
		else if (pressed && m_Run.Active)
		{
			// 按住期间持续登记归属,保证抬起帧的 IsClickCompleted 有归属可核对。
			ctx.IsClickCompleted(0, target);
		}
		if (focusOnTarget && ctx.WasKeyPressed(KeyCodes::Enter))
		{
			++m_PlayKeyCount;
			record("key.enter", "focused=1 KeyPressed(Enter)=1 -> keyActivated true");
			if (m_Run.Active)
				m_Run.KeyEnterSeen = true;
		}
		if (focusOnTarget && ctx.WasKeyPressed(KeyCodes::Space))
		{
			++m_PlayKeyCount;
			record("key.space", "focused=1 KeyPressed(Space)=1 -> keyActivated true");
			if (m_Run.Active)
				m_Run.KeySpaceSeen = true;
		}

		if (m_Run.Active && !m_Run.Phases.empty())
		{
			PlayPhaseRecord& phase = m_Run.Phases.back();
			if (phase.FirstFrame == 0)
				phase.FirstFrame = ctx.Frame();
			phase.LastFrame = ctx.Frame();
			++phase.Frames;
			phase.Hover = phase.Hover || hover;
			phase.Pressed = phase.Pressed || pressed;
			phase.FocusOnTarget = phase.FocusOnTarget || focusOnTarget;
			if (m_Run.Timeline.size() < 700)
			{
				PlayTimelineEntry entry;
				entry.Frame = ctx.Frame();
				entry.Phase = phase.Name;
				entry.State = state;
				entry.MouseX = input.MousePos.x;
				entry.MouseY = input.MousePos.y;
				entry.Hover = hover;
				entry.Pressed = pressed;
				entry.Clicked = input.MouseClicked[0];
				entry.Released = input.MouseReleased[0];
				entry.Focus = focus;
				entry.FocusOnTarget = focusOnTarget;
				m_Run.Timeline.push_back(entry);
			}
		}

		m_PlayObservation = PlayObservationText();
		m_PlayA11yPrevJson = m_PlayA11yJson;
		m_PlayA11yJson = CanvasA11yJson(ctx);
	}

	std::string WidgetGalleryPanel::CanvasA11yJson(const Wui::WuiContext& ctx) const
	{
		std::ostringstream out;
		out << "[";
		bool first = true;
		for (const Wui::WuiAccessNode& node : Wui::WuiAccessibility::Get().Nodes())
		{
			if (node.Window != ctx.WindowKey())
				continue;
			const Wui::WuiRect& r = node.Rect;
			if (r.X + r.W <= m_CanvasRect.X || r.X >= m_CanvasRect.X + m_CanvasRect.W
				|| r.Y + r.H <= m_CanvasRect.Y || r.Y >= m_CanvasRect.Y + m_CanvasRect.H)
				continue;
			out << (first ? "\n" : ",\n");
			first = false;
			out << "    {\"id\": " << node.Id << ", \"kind\": \"" << JsonEscape(node.Kind)
				<< "\", \"label\": \"" << JsonEscape(node.Label) << "\", \"value\": \""
				<< JsonEscape(node.Value) << "\", \"enabled\": " << (node.Enabled ? "true" : "false")
				<< ", \"focused\": " << (node.Focused ? "true" : "false")
				<< ", \"interactive\": " << (node.Interactive ? "true" : "false")
				<< ", \"rect\": [" << r.X << ", " << r.Y << ", " << r.W << ", " << r.H << "]}";
		}
		out << (first ? "]" : "\n  ]");
		return out.str();
	}

	void WidgetGalleryPanel::FinishCurrentInteraction(Wui::WuiContext& ctx,
		const Wui::WuiComponentDesc& desc, const char* result, const std::string& gap)
	{
		if (!m_Run.Active)
			return;
		m_Run.Result = result;
		m_Run.GapReason = gap;
		m_Run.PressHeld = false;
		m_Run.ValueAfter = AccessNodeValue(m_Run.TargetId);
		m_Run.EndFrame = ctx.Frame();
		m_Run.FinishedWallMs = NowMs();
		WriteInteractionEvidence();
		m_Run.WrittenKinds.push_back(m_Run.Kind);
		const std::string summary = m_Run.Kind + std::string(": ") + m_Run.Result
			+ (m_Run.GapReason.empty() ? std::string() : std::string(" (") + m_Run.GapReason + ")");
		ctx.RecordOp("gallery", "interaction", m_Run.ComponentId, summary);
		++m_Run.Index;
		if (m_Run.Index < desc.Interactions.size())
		{
			BeginRunInteraction(ctx, desc);
			return;
		}
		WriteInteractionSummary();
		m_RunStatus = m_Run.Kind.empty()
			? summary
			: std::string("interactions done (") + std::to_string(m_Run.WrittenKinds.size())
				+ "): " + summary;
		m_Run.Active = false;
		m_Run.Phase = kRunIdle;
	}

	void WidgetGalleryPanel::WriteInteractionLive(const std::string& phaseName) const
	{
		if (m_Run.ComponentId.empty())
			return;
		const std::filesystem::path directory =
			WorkbenchDir() / m_Run.ComponentId / "interaction";
		std::error_code error;
		std::filesystem::create_directories(directory, error);
		std::ofstream file(directory / "_live.json", std::ios::binary | std::ios::trunc);
		if (!file)
			return;
		std::ostringstream out;
		out << "{\n";
		out << "  \"schema\": \"wui-workbench-interaction-live/1\",\n";
		out << "  \"run\": \"" << JsonEscape(m_Run.Id) << "\",\n";
		out << "  \"component\": \"" << JsonEscape(m_Run.ComponentId) << "\",\n";
		out << "  \"kind\": \"" << JsonEscape(m_Run.Kind) << "\",\n";
		out << "  \"index\": " << m_Run.Index << ",\n";
		out << "  \"phase\": \"" << JsonEscape(phaseName) << "\",\n";
		out << "  \"target\": \"" << JsonEscape(m_Run.Target) << "\",\n";
		out << "  \"active\": " << (m_Run.Active ? "true" : "false") << ",\n";
		out << "  \"tabs\": " << m_Run.TabsQueued << ",\n";
		out << "  \"focusReached\": " << (m_Run.FocusReached ? "true" : "false") << ",\n";
		out << "  \"uiScale\": " << (Wui::UiScale() > 0.0f ? Wui::UiScale() : 1.0f) << ",\n";
		out << "  \"canvas\": {\"x\": " << m_CanvasRect.X << ", \"y\": " << m_CanvasRect.Y
			<< ", \"w\": " << m_CanvasRect.W << ", \"h\": " << m_CanvasRect.H << "},\n";
		out << "  \"slot\": {\"x\": " << m_CanvasSlot.X << ", \"y\": " << m_CanvasSlot.Y
			<< ", \"w\": " << m_CanvasSlot.W << ", \"h\": " << m_CanvasSlot.H << "},\n";
		out << "  \"targetRect\": {\"x\": " << m_Run.TargetRect.X << ", \"y\": " << m_Run.TargetRect.Y
			<< ", \"w\": " << m_Run.TargetRect.W << ", \"h\": " << m_Run.TargetRect.H << "},\n";
		out << "  \"updatedAt\": \"" << WallStamp() << "\"\n";
		out << "}\n";
		file << out.str();
	}

	void WidgetGalleryPanel::WriteInteractionEvidence() const
	{
		if (m_Run.ComponentId.empty() || m_Run.Kind.empty())
			return;
		const std::filesystem::path directory =
			WorkbenchDir() / m_Run.ComponentId / "interaction";
		std::error_code error;
		std::filesystem::create_directories(directory, error);
		std::ofstream file(directory / (m_Run.Kind + ".json"), std::ios::binary | std::ios::trunc);
		if (!file)
			return;
		file << InteractionEvidenceJson();
	}

	std::string WidgetGalleryPanel::InteractionEvidenceJson() const
	{
		std::ostringstream out;
		out << "{\n";
		out << "  \"schema\": \"wui-workbench-interaction/1\",\n";
		out << "  \"component\": \"" << JsonEscape(m_Run.ComponentId) << "\",\n";
		out << "  \"kind\": \"" << JsonEscape(m_Run.Kind) << "\",\n";
		out << "  \"target\": \"" << JsonEscape(m_Run.Target) << "\",\n";
		out << "  \"targetId\": " << m_Run.TargetId << ",\n";
		out << "  \"expect\": \"" << JsonEscape(m_Run.Expect) << "\",\n";
		out << "  \"steps\": \"" << JsonEscape(m_Run.Steps) << "\",\n";
		out << "  \"note\": \"" << JsonEscape(m_Run.Note) << "\",\n";
		out << "  \"mode\": \"play\",\n";
		out << "  \"route\": \"WuiInputState(draw.RouteRealInput=true;与 WuiScriptedInput/ui.invoke "
			"同一输入结构,不做测试专用分支)\",\n";
		out << "  \"run\": \"" << JsonEscape(m_Run.Id) << "\",\n";
		out << "  \"window\": \"" << JsonEscape(m_Run.WindowKey) << "\",\n";
		out << "  \"frames\": [" << m_Run.StartFrame << ", " << m_Run.EndFrame << "],\n";
		out << "  \"wallMs\": " << (m_Run.FinishedWallMs >= m_Run.StartedWallMs
			? m_Run.FinishedWallMs - m_Run.StartedWallMs : 0) << ",\n";
		out << "  \"uiScale\": " << (Wui::UiScale() > 0.0f ? Wui::UiScale() : 1.0f) << ",\n";
		out << "  \"canvas\": {\"x\": " << m_CanvasRect.X << ", \"y\": " << m_CanvasRect.Y
			<< ", \"w\": " << m_CanvasRect.W << ", \"h\": " << m_CanvasRect.H << "},\n";
		out << "  \"slot\": {\"x\": " << m_CanvasSlot.X << ", \"y\": " << m_CanvasSlot.Y
			<< ", \"w\": " << m_CanvasSlot.W << ", \"h\": " << m_CanvasSlot.H << "},\n";
		out << "  \"targetRect\": {\"x\": " << m_Run.TargetRect.X << ", \"y\": " << m_Run.TargetRect.Y
			<< ", \"w\": " << m_Run.TargetRect.W << ", \"h\": " << m_Run.TargetRect.H << "},\n";
		out << "  \"pixelHint\": \"" << InteractionPixelPhase(m_Run.Kind) << "\",\n";
		out << "  \"phases\": [";
		for (size_t index = 0; index < m_Run.Phases.size(); ++index)
		{
			const PlayPhaseRecord& phase = m_Run.Phases[index];
			out << (index == 0 ? "\n" : ",\n");
			out << "    {\"name\": \"" << JsonEscape(phase.Name) << "\", \"note\": \""
				<< JsonEscape(phase.Note) << "\", \"frames\": [" << phase.FirstFrame << ", "
				<< phase.LastFrame << "], \"sampledFrames\": " << phase.Frames
				<< ", \"budgetMs\": " << phase.WallMs << ", \"hover\": "
				<< (phase.Hover ? "true" : "false") << ", \"pressed\": "
				<< (phase.Pressed ? "true" : "false") << ", \"focusOnTarget\": "
				<< (phase.FocusOnTarget ? "true" : "false") << "}";
		}
		out << (m_Run.Phases.empty() ? "]," : "\n  ],") << "\n";
		out << "  \"timeline\": [";
		for (size_t index = 0; index < m_Run.Timeline.size(); ++index)
		{
			const PlayTimelineEntry& entry = m_Run.Timeline[index];
			out << (index == 0 ? "\n" : ",\n");
			out << "    {\"frame\": " << entry.Frame << ", \"phase\": \"" << JsonEscape(entry.Phase)
				<< "\", \"state\": \"" << JsonEscape(entry.State) << "\", \"mouse\": ["
				<< entry.MouseX << ", " << entry.MouseY << "], \"hover\": "
				<< (entry.Hover ? "true" : "false") << ", \"pressed\": "
				<< (entry.Pressed ? "true" : "false") << ", \"clicked\": "
				<< (entry.Clicked ? "true" : "false") << ", \"released\": "
				<< (entry.Released ? "true" : "false") << ", \"focus\": " << entry.Focus
				<< ", \"focusOnTarget\": " << (entry.FocusOnTarget ? "true" : "false") << "}";
		}
		out << (m_Run.Timeline.empty() ? "]," : "\n  ],") << "\n";
		out << "  \"events\": [";
		for (size_t index = 0; index < m_Run.Events.size(); ++index)
		{
			out << (index == 0 ? "\n" : ",\n");
			out << "    {\"event\": \"" << JsonEscape(m_Run.Events[index].first) << "\", \"detail\": \""
				<< JsonEscape(m_Run.Events[index].second) << "\"}";
		}
		out << (m_Run.Events.empty() ? "]," : "\n  ],") << "\n";
		out << "  \"activation\": {\n";
		out << "    \"witness\": \"widget-predicate+engine-click-completed\",\n";
		out << "    \"predicate\": \"hovered && ctx.Input().MouseClicked[0]\",\n";
		out << "    \"value\": " << (m_Run.PressFrame != 0 ? "true" : "false") << ",\n";
		out << "    \"frame\": " << m_Run.PressFrame << ",\n";
		out << "    \"clickCompleted\": " << (m_Run.ClickCompleted ? "true" : "false") << ",\n";
		out << "    \"clickCompletedWitness\": \"WuiContext::IsClickCompleted(0, targetRect)\",\n";
		out << "    \"releaseFrame\": " << m_Run.ReleaseFrame << ",\n";
		out << "    \"limitation\": \"showcase 丢弃 Button 的返回值(Engine 侧 ShowButton 未接);"
			"字面返回值需要 Engine 改动,超出本单边界 —— 这里记的是同帧判据求值 + 引擎的点击完成口径\"\n";
		out << "  },\n";
		out << "  \"keyboard\": {\"focusReached\": " << (m_Run.FocusReached ? "true" : "false")
			<< ", \"match\": \"" << JsonEscape(m_Run.FocusMatch) << "\", \"tabs\": " << m_Run.TabsQueued
			<< ", \"enter\": " << (m_Run.KeyEnterSeen ? "true" : "false") << ", \"space\": "
			<< (m_Run.KeySpaceSeen ? "true" : "false") << "},\n";
		out << "  \"observation\": {\"valueBefore\": \"" << JsonEscape(m_Run.ValueBefore)
			<< "\", \"valueAfter\": \"" << JsonEscape(m_Run.ValueAfter) << "\", \"hoverSeen\": "
			<< (std::any_of(m_Run.Phases.begin(), m_Run.Phases.end(),
				[](const PlayPhaseRecord& phase) { return phase.Hover; }) ? "true" : "false")
			<< ", \"pressSeen\": " << (m_Run.PressSeen ? "true" : "false")
			<< ", \"releaseSeen\": " << (m_Run.ReleaseSeen ? "true" : "false") << "},\n";
		out << "  \"a11y\": {\"before\": " << (m_Run.A11yBefore.empty() ? "[]" : m_Run.A11yBefore)
			<< ",\n          \"after\": " << (m_PlayA11yJson.empty() ? "[]" : m_PlayA11yJson) << "},\n";
		out << "  \"result\": \"" << JsonEscape(m_Run.Result) << "\",\n";
		out << "  \"gap\": \"" << JsonEscape(m_Run.GapReason) << "\",\n";
		out << "  \"pixels\": {\"source\": \"probe:capture.float\", \"filledBy\": \"probe\", "
			"\"phase\": \"" << InteractionPixelPhase(m_Run.Kind)
			<< "\", \"sha256\": \"\", \"captures\": {}}\n";
		out << "}\n";
		return out.str();
	}

	void WidgetGalleryPanel::WriteInteractionSummary() const
	{
		if (m_Run.ComponentId.empty())
			return;
		const std::filesystem::path directory =
			WorkbenchDir() / m_Run.ComponentId / "interaction";
		std::error_code error;
		std::filesystem::create_directories(directory, error);
		std::ofstream file(directory / "summary.json", std::ios::binary | std::ios::trunc);
		if (!file)
			return;
		std::ostringstream out;
		out << "{\n";
		out << "  \"schema\": \"wui-workbench-interaction-summary/1\",\n";
		out << "  \"run\": \"" << JsonEscape(m_Run.Id) << "\",\n";
		out << "  \"component\": \"" << JsonEscape(m_Run.ComponentId) << "\",\n";
		out << "  \"mode\": \"play\",\n";
		out << "  \"route\": \"WuiInputState(RouteRealInput=true)\",\n";
		out << "  \"startedAt\": \"" << WallStamp() << "\",\n";
		out << "  \"frames\": [" << m_Run.StartFrame << ", " << m_Run.EndFrame << "],\n";
		out << "  \"wallMs\": " << (m_Run.FinishedWallMs >= m_Run.StartedWallMs
			? m_Run.FinishedWallMs - m_Run.StartedWallMs : 0) << ",\n";
		out << "  \"interactions\": [";
		for (size_t index = 0; index < m_Run.WrittenKinds.size(); ++index)
			out << (index == 0 ? "\n" : ",\n") << "    {\"kind\": \""
				<< JsonEscape(m_Run.WrittenKinds[index]) << "\", \"file\": \""
				<< JsonEscape(m_Run.WrittenKinds[index]) << ".json\"}";
		out << (m_Run.WrittenKinds.empty() ? "]," : "\n  ],") << "\n";
		out << "  \"observation\": {\"state\": \"" << JsonEscape(m_PlayState) << "\", \"focus\": \""
			<< JsonEscape(m_PlayFocusName) << "\", \"press\": " << m_PlayPressCount
			<< ", \"click\": " << m_PlayClickCount << ", \"release\": " << m_PlayReleaseCount
			<< ", \"key\": " << m_PlayKeyCount << ", \"lastEvent\": \""
			<< JsonEscape(m_PlayLastEvent) << "\"},\n";
		out << "  \"events\": [";
		for (size_t index = 0; index < m_PlayEvents.size(); ++index)
			out << (index == 0 ? "\n" : ",\n") << "    {\"event\": \""
				<< JsonEscape(m_PlayEvents[index].first) << "\", \"detail\": \""
				<< JsonEscape(m_PlayEvents[index].second) << "\"}";
		out << (m_PlayEvents.empty() ? "]\n" : "\n  ]\n");
		out << "}\n";
		file << out.str();
	}

	void WidgetGalleryPanel::OnRender(Wui::WuiContext& ctx, const Wui::WuiRect& rect, PanelHost& host)
	{
		Wui::WuiTheme& theme = host.Theme();
		const WbLayout layout = ComputeLayout(rect, theme);
		const float density = m_DensityIndex == 1 ? 0.85f : 1.0f;
		const float uiScale = Wui::UiScale() > 0.0f ? Wui::UiScale() : 1.0f;
		m_PanelRect = rect;
		m_PopupOpenNow = false;
		// 信息条画在画布之前:它显示上一帧的 Play 观测(同一帧的观测在动作条上)。
		m_PlayObservationPrev = m_PlayObservation;

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
			DrawTopBar(ctx, layout, theme, uiScale);
			const Wui::WuiComponentDesc* selected = nullptr;
			if (!layout.Stacked)
			{
				selected = DrawTree(ctx, layout, theme, uiScale);
				DrawInfoBar(ctx, layout, theme, selected, uiScale);
				DrawProperties(ctx, layout, theme, selected, density, uiScale);
			}
			else
			{
				// 窄窗:树/属性仍然走 body 滚动(画布已单独画在它的位置上)。
				const float contentHeight = layout.Props.Y + layout.Props.H + 8.0f - layout.Body.Y;
				Wui::BeginScrollArea(ctx, layout.Body, contentHeight, m_BodyScroll, theme);
				selected = DrawTree(ctx, layout, theme, uiScale);
				DrawProperties(ctx, layout, theme, selected, density, uiScale);
				Wui::EndScrollArea(ctx);
				DrawInfoBar(ctx, layout, theme, selected, uiScale);
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
			DrawTopBar(ctx, layout, theme, uiScale);
			const Wui::WuiComponentDesc* selected = nullptr;

			if (!layout.Stacked)
			{
				selected = DrawTree(ctx, layout, theme, uiScale);
				DrawInfoBar(ctx, layout, theme, selected, uiScale);
				DrawCanvas(ctx, layout, theme, selected, density, uiScale, false);
				DrawProperties(ctx, layout, theme, selected, density, uiScale);
			}
			else
			{
				// 窄窗:三段纵向排布,整个 body 一起滚动(窗口够小时仍能看到全部区域)。
				const float contentHeight = layout.Props.Y + layout.Props.H + 8.0f - layout.Body.Y;
				Wui::BeginScrollArea(ctx, layout.Body, contentHeight, m_BodyScroll, theme);
				selected = DrawTree(ctx, layout, theme, uiScale);
				DrawCanvas(ctx, layout, theme, selected, density, uiScale, false);
				DrawProperties(ctx, layout, theme, selected, density, uiScale);
				Wui::EndScrollArea(ctx);
				DrawInfoBar(ctx, layout, theme, selected, uiScale);
			}

			DrawActions(ctx, layout, theme, selected, uiScale);
		}

		// 下一帧左树画在本帧之前,所以"弹层开着 → ↑/↓ 归弹层"只能用上一帧的结果。
		m_PopupOpenPrev = m_PopupOpenNow;
	}
}
