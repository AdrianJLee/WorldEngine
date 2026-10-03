#include "WidgetGalleryPanel_Internal.h"

namespace World
{

using namespace WidgetGalleryPanelDetail;

namespace WidgetGalleryPanelDetail
{

void RegisterWorkbenchNode(Wui::WuiId id, const char* kind, const Wui::WuiRect& rect, const std::string& label, const std::string& value , bool enabled , bool interactive ){
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
std::string ClipText(const Wui::WuiContext& ctx, const std::string& text, float width, float fontSize){
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


float SafeFloat(const std::string& text, float fallback){
			try
			{
				return std::stof(text);
			}
			catch (...)
			{
				return fallback;
			}
		}


std::string FormatFloat(float value){
			char buffer[32] = {};
			std::snprintf(buffer, sizeof(buffer), "%.3f", value);
			return buffer;
		}


std::string ToLowerAscii(std::string_view text){
			std::string out;
			out.reserve(text.size());
			for (const char ch : text)
			{
				const unsigned char byte = static_cast<unsigned char>(ch);
				out.push_back(byte < 0x80 ? static_cast<char>(std::tolower(byte)) : ch);
			}
			return out;
		}


std::string NormalizePropGroup(PropGroupEnum group){
			switch (group)
			{
			case PropGroupEnum::Style: return "Style";
			case PropGroupEnum::Layout: return "Layout";
			case PropGroupEnum::Behavior: return "Behavior";
			case PropGroupEnum::Content: return "Content";
			default: return kPropGroupGeneral;   // 未来新增的枚举值先落"其它"桶,不静默丢行
			}
		}


int PropGroupRank(const std::string& group){
			if (group == "Content") return 0;
			if (group == "Style") return 1;
			if (group == "Layout") return 2;
			if (group == "Behavior") return 3;
			if (group == kPropGroupGeneral) return 9;
			return 5;
		}


std::string PropGroupTitle(const std::string& group){
			if (group == "Content") return Wui::Tr("workbench.props.group.content", "Content");
			if (group == "Style") return Wui::Tr("workbench.props.group.style", "Style");
			if (group == "Layout") return Wui::Tr("workbench.props.group.layout", "Layout");
			if (group == "Behavior") return Wui::Tr("workbench.props.group.behavior", "Behavior");
			if (group == kPropGroupGeneral) return Wui::Tr("workbench.props.group.general", "General");
			return group;
		}


		// 颜色两种表示之间的转换:面板的色板控件用 glm::vec4,登记表/引擎协议用 WuiColor。
glm::vec4 PropColorToVec4(const Wui::WuiColor& color){
			return { color.R, color.G, color.B, color.A };
		}


Wui::WuiColor PropVec4ToColor(const glm::vec4& rgba){
			return Wui::WuiColor { rgba.r, rgba.g, rgba.b, rgba.a };
		}


		// ---- WUI-P1.5b:性能采样开关(只用于验收,默认关闭)----
		// WLD_WORKBENCH_PROPS:属性面板形态
		//   0 = 整体不画(基线"面板关闭")
		//   1 = 只画属性行(旧结构:无搜索框、无分组头、无状态选择器)
		//   2 = 完整面板(**默认,用户看到的就是它**)
		//   3 = 只画前 2 行属性(基线"旧 2 属性版")
		// 非 2 的取值不是产品行为,只服务于 ≤0.5ms/帧 的增量验收。
int PropPanelMode(){
			static const int mode = [] {
				const char* raw = std::getenv("WLD_WORKBENCH_PROPS");
				return raw == nullptr || *raw == '\0' ? 2 : std::atoi(raw);
			}();
			return mode;
		}


bool PropTimingEnabled(){
			static const bool enabled = std::getenv("WLD_WORKBENCH_TIMING") != nullptr;
			return enabled;
		}


		// WUI-P1c-b W2:字号缩放因子(UiScale)。面板/画布里的字号都要乘它,与 density 分开 ——
		// density 只压行高/槽位高度,缩放只放大字号。**只放大字号,不动布局度量**:行高、间距、
		// 控件高仍是设计单位(后端会按 UiScale 放大),否则会和画布槽位(已按 UiScale 放大)叠两次。
float FontScale(float uiScale){
			return uiScale > 0.0f ? uiScale : 1.0f;
		}


		// showcase 用的主题副本:只把字体令牌乘 scale(颜色/圆角/行高/内边距保持原样)。
		// 为什么要副本而不是改全局主题:组件 showcase 内部字号全部读 theme.FontSize*(Engine 侧代码),
		// 工作台能改的只有"传进去的那份主题";不动 Engine 公共接口。
Wui::WuiTheme ScaledFontTheme(const Wui::WuiTheme& theme, float scale){
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
const std::filesystem::path& WorkbenchDir(){
			static const std::filesystem::path directory =
				std::filesystem::path(std::string(WLD_OUTPUT_DIR)) / "wui-workbench";
			return directory;
		}


void EnsureWorkbenchDir(){
			std::error_code error;
			std::filesystem::create_directories(WorkbenchDir(), error);
		}


std::string JsonEscape(const std::string& text){
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


std::string ReadFirstLine(const std::filesystem::path& path){
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
std::string ResolveGitCommit(){
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
std::pair<float, float> SizeNotesToSize(const std::string& notes){
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


const char* StatusText(Wui::WuiComponentStatus status){
			switch (status)
			{
			case Wui::WuiComponentStatus::Approved: return "Approved";
			case Wui::WuiComponentStatus::Deprecated: return "Deprecated";
			default: return "Draft";
			}
		}


Wui::WuiColor StatusColor(Wui::WuiComponentStatus status, const Wui::WuiTheme& theme){
			switch (status)
			{
			case Wui::WuiComponentStatus::Approved: return theme.Success;
			case Wui::WuiComponentStatus::Deprecated: return theme.Danger;
			default: return theme.Warning;
			}
		}


		// showcase 是否往 overlay 层画了**整窗大小**的矩形 = 模态遮罩的指纹(视角无关,不看组件名)。
		// 只有这种组件需要"专用舞台":它的遮挡区会把工作台整块面板的命中打死(实测踩过)。
bool DetectsFullWindowOverlay(const Wui::WuiContext& ctx, size_t from){
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

}
}
