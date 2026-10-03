#include "WuiComponentRegistry_Internal.h"

namespace World::Wui
{

using namespace WuiComponentRegistryDetail;

namespace WuiComponentRegistryDetail
{

		// ---- 属性覆盖解析 ----

float ClampFloat(float value, float low, float high){
			return value < low ? low : (value > high ? high : value);
		}


const std::string* FindProperty(const WuiComponentDraw& draw, const char* name){
			for (const std::pair<std::string, std::string>& entry : draw.Properties)
				if (entry.first == name)
					return &entry.second;
			return nullptr;
		}


std::string TrimmedLower(const std::string& text){
			size_t begin = 0;
			size_t end = text.size();
			while (begin < end && (text[begin] == ' ' || text[begin] == '\t' || text[begin] == '\r' || text[begin] == '\n'))
				++begin;
			while (end > begin && (text[end - 1] == ' ' || text[end - 1] == '\t' || text[end - 1] == '\r' || text[end - 1] == '\n'))
				--end;
			std::string out = text.substr(begin, end - begin);
			for (char& ch : out)
				if (ch >= 'A' && ch <= 'Z')
					ch = static_cast<char>(ch - 'A' + 'a');
			return out;
		}


std::optional<bool> BoolOverride(const WuiComponentDraw& draw, const char* name){
			const std::string* text = FindProperty(draw, name);
			if (!text)
				return std::nullopt;
			const std::string lower = TrimmedLower(*text);
			if (lower == "true" || lower == "1" || lower == "on" || lower == "yes" || lower == "checked")
				return true;
			if (lower == "false" || lower == "0" || lower == "off" || lower == "no" || lower == "unchecked")
				return false;
			return std::nullopt;   // 认不出的写法 = 没给
		}


bool BoolProperty(const WuiComponentDraw& draw, const char* name, bool fallback){
			return BoolOverride(draw, name).value_or(fallback);
		}


std::optional<float> FloatOverride(const WuiComponentDraw& draw, const char* name){
			const std::string* text = FindProperty(draw, name);
			if (!text || text->empty())
				return std::nullopt;
			char* end = nullptr;
			const float parsed = std::strtof(text->c_str(), &end);
			if (end == text->c_str() || !std::isfinite(parsed))
				return std::nullopt;
			return parsed;
		}


std::optional<int64_t> IntOverride(const WuiComponentDraw& draw, const char* name){
			const std::string* text = FindProperty(draw, name);
			if (!text || text->empty())
				return std::nullopt;
			char* end = nullptr;
			const long long parsed = std::strtoll(text->c_str(), &end, 10);
			if (end == text->c_str())
				return std::nullopt;
			return static_cast<int64_t>(parsed);
		}


std::string TextProperty(const WuiComponentDraw& draw, const char* name, const std::string& fallback){
			const std::string* text = FindProperty(draw, name);
			return (text && !text->empty()) ? *text : fallback;
		}


bool IsChinese(const WuiComponentDraw& draw){
			return draw.Locale.rfind("zh", 0) == 0;
		}


		// 文案:属性覆盖 > Locale 默认(英文 / 简体中文)。
std::string LocalizedText(const WuiComponentDraw& draw, const char* name, const char* english, const char* chinese){
			return TextProperty(draw, name, IsChinese(draw) ? std::string(chinese) : std::string(english));
		}


		// 逗号/竖线/分号分隔的选项表(工作台"属性 → 下拉选项")。少于两项时不采用覆盖值。
std::vector<std::string> OptionList(const WuiComponentDraw& draw, const char* name, std::initializer_list<const char*> defaults){
			std::vector<std::string> fallback(defaults.begin(), defaults.end());
			const std::string* text = FindProperty(draw, name);
			if (!text || text->empty())
				return fallback;
			std::vector<std::string> parsed;
			std::string current;
			for (char ch : *text)
			{
				const bool separator = ch == ',' || ch == '|' || ch == ';';
				if (!separator)
				{
					current.push_back(ch);
					continue;
				}
				if (!TrimmedLower(current).empty())
					parsed.push_back(current);
				current.clear();
			}
			if (!TrimmedLower(current).empty())
				parsed.push_back(current);
			return parsed.size() >= 2 ? parsed : fallback;
		}


int HexDigit(char ch){
			if (ch >= '0' && ch <= '9')
				return ch - '0';
			if (ch >= 'a' && ch <= 'f')
				return ch - 'a' + 10;
			if (ch >= 'A' && ch <= 'F')
				return ch - 'A' + 10;
			return -1;
		}


		// "#RRGGBB" / "#RRGGBBAA"(可省 '#',大小写不限);失败返回 false 并保持原值。
bool ParseHexColor(const std::string& text, glm::vec4& out){
			std::string digits;
			for (char ch : text)
			{
				if (ch == '#')
					continue;
				if (HexDigit(ch) < 0)
					return false;
				digits.push_back(ch);
			}
			if (digits.size() != 6 && digits.size() != 8)
				return false;
			const auto channel = [&](size_t index) -> float
			{
				return static_cast<float>(HexDigit(digits[index]) * 16 + HexDigit(digits[index + 1])) / 255.0f;
			};
			out = { channel(0), channel(2), channel(4), digits.size() == 8 ? channel(6) : 1.0f };
			return true;
		}


bool& BoolState(WuiContext& ctx, const char* slot, bool initial){
			BoolSlot& state = ctx.Persist<BoolSlot>(HashId(slot), BoolSlot {});
			if (!state.Initialized)
			{
				state.Value = initial;
				state.Initialized = true;
			}
			return state.Value;
		}


		// 数值槽:属性文本变化时重设(工作台改一次属性立刻生效);非法文本忽略。
float& DrivenFloat(WuiContext& ctx, const char* slot, const WuiComponentDraw& draw, const char* name, float initial, float min, float max){
			FloatSlot& state = ctx.Persist<FloatSlot>(HashId(slot), FloatSlot {});
			if (!state.Initialized)
			{
				state.Value = initial;
				state.Initialized = true;
			}
			const std::string* text = FindProperty(draw, name);
			const std::string applied = text ? *text : std::string();
			if (text && applied != state.Applied)
			{
				if (const std::optional<float> parsed = FloatOverride(draw, name))
					state.Value = ClampFloat(*parsed, min, max);
				state.Applied = applied;
			}
			else if (!text)
			{
				state.Applied.clear();
			}
			return state.Value;
		}


int64_t& DrivenInt(WuiContext& ctx, const char* slot, const WuiComponentDraw& draw, const char* name, int64_t initial, int64_t min, int64_t max){
			IntSlot& state = ctx.Persist<IntSlot>(HashId(slot), IntSlot {});
			if (!state.Initialized)
			{
				state.Value = initial;
				state.Initialized = true;
			}
			const std::string* text = FindProperty(draw, name);
			const std::string applied = text ? *text : std::string();
			if (text && applied != state.Applied)
			{
				if (const std::optional<int64_t> parsed = IntOverride(draw, name))
					state.Value = std::max(min, std::min(max, *parsed));
				state.Applied = applied;
			}
			else if (!text)
			{
				state.Applied.clear();
			}
			return state.Value;
		}


std::string& DrivenText(WuiContext& ctx, const char* slot, const WuiComponentDraw& draw, const char* name, const std::string& initial){
			TextSlot& state = ctx.Persist<TextSlot>(HashId(slot), TextSlot {});
			if (!state.Initialized)
			{
				state.Value = initial;
				state.Initialized = true;
			}
			const std::string* text = FindProperty(draw, name);
			const std::string applied = text ? *text : std::string();
			if (text && applied != state.Applied)
			{
				state.Value = *text;
				state.Applied = applied;
			}
			else if (!text)
			{
				state.Applied.clear();
			}
			return state.Value;
		}


glm::vec4& DrivenColor(WuiContext& ctx, const char* slot, const WuiComponentDraw& draw, const char* name, const glm::vec4& initial){
			ColorSlot& state = ctx.Persist<ColorSlot>(HashId(slot), ColorSlot {});
			if (!state.Initialized)
			{
				state.Value = initial;
				state.Initialized = true;
			}
			const std::string* text = FindProperty(draw, name);
			const std::string applied = text ? *text : std::string();
			if (text && applied != state.Applied)
			{
				glm::vec4 parsed {};
				if (ParseHexColor(*text, parsed))
					state.Value = parsed;
				state.Applied = applied;
			}
			else if (!text)
			{
				state.Applied.clear();
			}
			return state.Value;
		}


		// ---- 状态词 ----

bool StateIs(const WuiComponentDraw& draw, std::initializer_list<const char*> states){
			for (const char* state : states)
				if (draw.State == state)
					return true;
			return false;
		}


		// 画布槽:宽度/高度按 UiScale 缩放、按 Density 压高度(默认行高 = theme.ControlHeight),
		// 在 draw.Rect 内左对齐、垂直居中;绝不越出 draw.Rect。
Slot Canvas(const WuiComponentDraw& draw, const WuiTheme& theme, float preferredWidth, float preferredHeight ){
			Slot slot;
			slot.Scale = ClampFloat(draw.UiScale, 0.5f, 3.0f);
			slot.Density = ClampFloat(draw.Density, 0.5f, 2.0f);
			const float baseWidth = std::max(8.0f, preferredWidth) * slot.Scale;
			const float baseHeight = std::max(8.0f, preferredHeight > 0.0f ? preferredHeight : theme.ControlHeight);
			const float height = std::min(draw.Rect.H, baseHeight * slot.Scale * slot.Density);
			const float width = std::min(draw.Rect.W, baseWidth);
			slot.Rect = { draw.Rect.X, draw.Rect.Y + (draw.Rect.H - height) * 0.5f, width, height };
			return slot;
		}


WuiId ShellId(const char* componentId){
			const std::string key = std::string("showcase.") + componentId;
			return HashId(key.c_str());
		}


		// 组件外壳锚点:控件自己不登记 a11y 时,探针/工作台仍有稳定 id 可用。
		// 控件自己登记同 id 节点时后登记覆盖本节点(kind 变成控件自己的 role)。
WuiId BeginShowcase(const WuiComponentDraw& draw, const char* componentId, const char* displayName, const WuiRect& rect){
			const WuiId id = ShellId(componentId);
			if (WuiAccessibility::Get().Enabled())
			{
				WuiAccessNode node;
				node.Id = id;
				node.Window = WuiAccessibility::Get().CurrentWindow();
				node.Panel = WuiAccessibility::Get().CurrentPanel();
				node.Kind = "component-root";
				node.Label = displayName;
				node.Value = draw.State;
				node.Rect = rect;
				node.Interactive = false;
				WuiAccessibility::Get().Register(node);
			}
			return id;
		}


		// 禁用态:控件没有 enabled 参数时,用主题里的禁用令牌表达(TextDisabled/ContentBg),
		// 仍是控件自己的绘制路径(不再叠一层半透明遮罩这种假外观)。
WuiTheme DisabledTheme(const WuiTheme& theme){
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


bool DisabledFor(const WuiComponentDraw& draw, const char* property ){
			return draw.State == "disabled" || BoolProperty(draw, property, false);
		}


WuiTheme ThemedFor(const WuiComponentDraw& draw, const WuiTheme& theme, const char* property ){
			return DisabledFor(draw, property) ? DisabledTheme(theme) : theme;
		}


		// 长文本压力:LongTextState 时把默认文案换成明显超宽的一串,用于观察裁剪/溢出行为。
std::string MaybeLongText(const WuiComponentDraw& draw, const std::string& text){
			if (draw.State != "long-text")
				return text;
			return text + " — The quick brown fox jumps over the lazy dog 0123456789";
		}


		// "x,y,z" 形式的向量覆盖值;非法文本忽略。
glm::vec3& DrivenVec3(WuiContext& ctx, const char* slot, const WuiComponentDraw& draw, const char* name, const glm::vec3& initial){
			Vec3Slot& state = ctx.Persist<Vec3Slot>(HashId(slot), Vec3Slot {});
			if (!state.Initialized)
			{
				state.Value = initial;
				state.Initialized = true;
			}
			const std::string* text = FindProperty(draw, name);
			const std::string applied = text ? *text : std::string();
			if (text && applied != state.Applied)
			{
				glm::vec3 parsed {};
				int count = 0;
				size_t start = 0;
				while (start <= text->size() && count < 3)
				{
					const size_t separator = text->find_first_of(",;| ", start);
					const std::string token = text->substr(start,
						separator == std::string::npos ? std::string::npos : separator - start);
					if (!token.empty())
					{
						char* end = nullptr;
						const float value = std::strtof(token.c_str(), &end);
						if (end != token.c_str() && std::isfinite(value))
							parsed[count++] = value;
					}
					if (separator == std::string::npos)
						break;
					start = separator + 1;
				}
				if (count == 3)
					state.Value = parsed;
				state.Applied = applied;
			}
			else if (!text)
			{
				state.Applied.clear();
			}
			return state.Value;
		}


glm::vec4& DrivenVecN(WuiContext& ctx, const char* slot, const WuiComponentDraw& draw, const char* name, int count, const glm::vec4& initial){
			VecNSlot& state = ctx.Persist<VecNSlot>(HashId(slot), VecNSlot {});
			if (!state.Initialized)
			{
				state.Value = initial;
				state.Initialized = true;
			}
			const std::string* text = FindProperty(draw, name);
			const std::string applied = text ? *text : std::string();
			if (text && applied != state.Applied)
			{
				glm::vec4 parsed { 0.0f, 0.0f, 0.0f, 0.0f };
				int found = 0;
				size_t start = 0;
				while (start <= text->size() && found < count)
				{
					const size_t separator = text->find_first_of(",;| ", start);
					const std::string token = text->substr(start,
						separator == std::string::npos ? std::string::npos : separator - start);
					if (!token.empty())
					{
						char* end = nullptr;
						const float value = std::strtof(token.c_str(), &end);
						if (end != token.c_str() && std::isfinite(value))
							parsed[found++] = value;
					}
					if (separator == std::string::npos)
						break;
					start = separator + 1;
				}
				if (found == count)
					state.Value = parsed;
				state.Applied = applied;
			}
			else if (!text)
				state.Applied.clear();
			return state.Value;
		}


RegistryStore& Store(){
			static RegistryStore store;
			return store;
		}


void EnsureSorted(RegistryStore& store){
			if (store.Sorted)
				return;
			std::sort(store.Items.begin(), store.Items.end(),
				[](const WuiComponentDesc& left, const WuiComponentDesc& right)
				{
					if (left.Category != right.Category)
						return left.Category < right.Category;
					if (left.DisplayName != right.DisplayName)
						return left.DisplayName < right.DisplayName;
					return left.Id < right.Id;   // 同分类同名时的确定性 tie-break
				});
			store.Index.clear();
			for (size_t i = 0; i < store.Items.size(); ++i)
				store.Index[store.Items[i].Id] = i;
			store.Sorted = true;
		}

std::once_flag& BuiltinsOnce(){
			static std::once_flag once;
			return once;
		}

}

const std::vector<WuiComponentDesc>& WuiComponentRegistry::All(){
		std::call_once(BuiltinsOnce(), RegisterBuiltins);
		RegistryStore& store = Store();
		EnsureSorted(store);
		return store.Items;
	}


const WuiComponentDesc* WuiComponentRegistry::Find(const std::string& id){
		std::call_once(BuiltinsOnce(), RegisterBuiltins);
		RegistryStore& store = Store();
		EnsureSorted(store);
		const auto found = store.Index.find(id);
		if (found == store.Index.end())
			return nullptr;
		return &store.Items[found->second];
	}


void WuiComponentRegistry::Register(WuiComponentDesc desc){
		RegistryStore& store = Store();
		if (desc.Id.empty())
		{
			WLD_CORE_WARN("[wui] component registry: 拒绝空 id 的登记项(DisplayName='{0}')", desc.DisplayName);
			return;
		}
		const auto found = store.Index.find(desc.Id);
		if (found != store.Index.end())
		{
			// 幂等:同 id 覆盖 + 一条可读告警(后登记者胜,注册顺序不参与语义)。
			WLD_CORE_WARN("[wui] component registry: 组件 id '{0}' 重复登记,覆盖旧条目('{1}')",
				desc.Id, store.Items[found->second].DisplayName);
			store.Items[found->second] = std::move(desc);
			store.Sorted = false;
			return;
		}
		store.Index[desc.Id] = store.Items.size();
		store.Items.push_back(std::move(desc));
		store.Sorted = false;
	}


size_t WuiComponentRegistry::Count(){
		std::call_once(BuiltinsOnce(), RegisterBuiltins);
		return Store().Items.size();
	}


	// ---- 属性覆盖的值编码协议(WUI-P1.5;声明与口径见 WuiComponentRegistry.h)----

bool ParseComponentColor(const std::string& text, WuiColor& out){
		glm::vec4 parsed {};
		if (!ParseHexColor(text, parsed))
			return false;
		out = { parsed.r, parsed.g, parsed.b, parsed.a };
		return true;
	}


bool ParseComponentSize(const std::string& text, float& width, float& height){
		// "<宽>x<高>":分隔符接受 'x' / 'X' / '*',两侧允许空格;两个数都必须是有限数,
		// 且除了空白不允许有别的残留字符("128x24x2"、"12px" 一律判非法)。
		size_t begin = 0;
		size_t end = text.size();
		while (begin < end && (text[begin] == ' ' || text[begin] == '\t'))
			++begin;
		while (end > begin && (text[end - 1] == ' ' || text[end - 1] == '\t'))
			--end;
		if (begin >= end)
			return false;
		const std::string token = text.substr(begin, end - begin);
		const size_t split = token.find_first_of("xX*");
		if (split == std::string::npos || split == 0 || split + 1 >= token.size())
			return false;
		const std::string left = token.substr(0, split);
		const std::string right = token.substr(split + 1);
		char* leftEnd = nullptr;
		char* rightEnd = nullptr;
		const float parsedWidth = std::strtof(left.c_str(), &leftEnd);
		const float parsedHeight = std::strtof(right.c_str(), &rightEnd);
		const auto onlySpace = [](const char* tail)
		{
			for (; *tail != '\0'; ++tail)
				if (*tail != ' ' && *tail != '\t')
					return false;
			return true;
		};
		if (leftEnd == left.c_str() || rightEnd == right.c_str())
			return false;
		if (!std::isfinite(parsedWidth) || !std::isfinite(parsedHeight))
			return false;
		if (!onlySpace(leftEnd) || !onlySpace(rightEnd))
			return false;
		width = parsedWidth;
		height = parsedHeight;
		return true;
	}

namespace WuiComponentRegistryDetail
{
		// 0..1 通道 → 两位大写 hex(自动夹取,面板回显用)。
std::string FormatColorChannel(float value){
			const float clamped = ClampFloat(value, 0.0f, 1.0f);
			const int quantized = static_cast<int>(clamped * 255.0f + 0.5f);
			static const char kDigits[] = "0123456789ABCDEF";
			std::string out;
			out.push_back(kDigits[(quantized >> 4) & 0xF]);
			out.push_back(kDigits[quantized & 0xF]);
			return out;
		}


		// 设计单位 → 最多两位小数、去尾零、去尾点("128.00" → "128")。
std::string FormatDimension(float value){
			char buffer[32] = {};
			std::snprintf(buffer, sizeof(buffer), "%.2f", static_cast<double>(value));
			std::string out = buffer;
			while (!out.empty() && out.back() == '0')
				out.pop_back();
			if (!out.empty() && out.back() == '.')
				out.pop_back();
			return out.empty() ? std::string("0") : out;
		}

}

std::string FormatComponentColor(const WuiColor& color){
		std::string out = "#" + FormatColorChannel(color.R) + FormatColorChannel(color.G)
			+ FormatColorChannel(color.B);
		if (color.A < 0.999f)
			out += FormatColorChannel(color.A);
		return out;
	}


std::string FormatComponentSize(float width, float height){
		return FormatDimension(width) + "x" + FormatDimension(height);
	}

namespace WuiComponentRegistryDetail
{
		// ---- 登记表构造小工具 ----

Property PropBool(const char* name){
			Property property;
			property.Name = name;
			property.Type = Property::Kind::Bool;
			return property;
		}


Property PropFloat(const char* name, float min, float max, float step){
			Property property;
			property.Name = name;
			property.Type = Property::Kind::Float;
			property.Min = min;
			property.Max = max;
			property.Step = step;
			return property;
		}


Property PropInt(const char* name, float min, float max, float step){
			Property property;
			property.Name = name;
			property.Type = Property::Kind::Int;
			property.Min = min;
			property.Max = max;
			property.Step = step;
			return property;
		}


Property PropText(const char* name, const char* sample){
			Property property;
			property.Name = name;
			property.Type = Property::Kind::Text;
			property.DefaultText = sample;
			return property;
		}


		// ---- 登记现有控件(P0-2) ----
		// Status:P1a 起**全部为 Draft**。Approved 只能由 P1 的"像素基线 + 探针 + 批准提交号"证据产生
		// (工作台的 Approve 记录在 build/**/approved.json,不直接改登记表);Deprecated 留给将来退役的件。

void RegisterBuiltins(){
			// ---- Buttons ----
			// WUI-P1.5:Button 样板 —— 属性按 UE 式三块 + Behavior 分组;5 态颜色是**真实生效**的
			// 覆盖值(名字 "<通道>.<状态>",StateScoped=true),`preferred` 决定画布槽位,
			// 字号/粗细/内边距改的是真实绘制参数;全部未覆盖 = 旧硬编码/主题令牌(像素基线不动)。
			WuiComponentRegistry::Register(Desc(
				"button", "WuiButton", "Button", "Buttons", WuiComponentStatus::Draft,
				"Engine/src/World/WUI/WuiWidgets.cpp",
				"role=button;id=HashId('showcase.button')(控件自身登记);label=label 属性;disabled 用主题禁用令牌;焦点环走 DrawFocusRing",
				"showcase 首选 128x24(preferred 属性可覆盖,宽×高设计单位);控件无最小宽(窄于文字会溢出);H=preferred 高度(未覆盖=theme.ControlHeight)×Density×UiScale",
				ShellIds("button"),
				StateList({ "default", "hover", "pressed", "focus", "disabled", "long-text" }),
				{
					// Content
					Grouped(PropText("label", "Apply"), WuiComponentPropertyGroup::Content,
						"按钮上的文字(文本归 Content;字号/颜色归 Style)。"),
					// Style:排版 + 内边距
					Grouped(UnitOf(NumberDefault(PropFloat("fontSize", 6.0f, 48.0f, 1.0f), 15.0f), "px"),
						WuiComponentPropertyGroup::Style, "文字字号(设计单位;未覆盖 = 15)。"),
					Grouped(PropBool("bold"), WuiComponentPropertyGroup::Style,
						"文字是否粗体(未覆盖 = 常规)。"),
					Grouped(UnitOf(NumberDefault(PropFloat("padding", 0.0f, 40.0f, 1.0f), 8.0f), "px"),
						WuiComponentPropertyGroup::Style, "文字左内边距(未覆盖 = 8)。"),
					// Style:5 态 × 3 通道颜色(#RRGGBB / #RRGGBBAA;未覆盖 = 主题令牌)
					PropColor("bg.default", "#22272F", true, "Normal 态填充(未覆盖 = 主题 ButtonBg)。"),
					PropColor("bg.hover", "#2A3038", true, "Hover 态填充(未覆盖 = 主题 ButtonHover)。"),
					PropColor("bg.pressed", "#2A3038", true, "Pressed 态填充(未覆盖 = Hover 的兜底值)。"),
					PropColor("bg.focus", "#22272F", true, "Focus 且未悬停时的填充(未覆盖 = Normal 的兜底值)。"),
					PropColor("bg.disabled", "#101318", true, "Disabled 态填充(未覆盖 = 主题 ContentBg)。"),
					PropColor("border.default", "#2B3138", true, "Normal 态描边(未覆盖 = 主题 Border)。"),
					PropColor("border.hover", "#2B3138", true, "Hover 态描边(未覆盖 = 主题 Border)。"),
					PropColor("border.pressed", "#2B3138", true, "Pressed 态描边(未覆盖 = 主题 Border)。"),
					PropColor("border.focus", "#4C8DFF", true,
						"Focus 态描边 + 焦点环颜色(未覆盖 = 主题 FocusRing)。"),
					PropColor("border.disabled", "#2B313899", true,
						"Disabled 态描边(未覆盖 = 主题 Border 的 60% alpha)。"),
					PropColor("text.default", "#D7DCE3", true, "Normal 态文字(未覆盖 = 主题 Text)。"),
					PropColor("text.hover", "#D7DCE3", true, "Hover 态文字(未覆盖 = 主题 Text)。"),
					PropColor("text.pressed", "#4C8DFF", true, "Pressed 态文字(未覆盖 = 主题 Accent)。"),
					PropColor("text.focus", "#D7DCE3", true, "Focus 态文字(未覆盖 = 主题 Text)。"),
					PropColor("text.disabled", "#5A626D", true, "Disabled 态文字(未覆盖 = 主题 TextDisabled)。"),
					// Layout
					PropSize2("preferred", 128.0f, 24.0f, "首选尺寸 宽×高(设计单位;未覆盖 = 128x24)。"),
					// Behavior
					Grouped(PropBool("disabled"), WuiComponentPropertyGroup::Behavior,
						"按禁用态绘制(禁用的**行为**由 ButtonEx/调用方决定)。"),
				},
				&ShowButton,
				{
					Item(WuiInteractionKind::Hover, "showcase.button", WuiInteractionExpect::PixelChange,
						"鼠标移到按钮中心(1 帧)→ 移开(1 帧)",
						"悬停填充 = bg.hover(未覆盖 = 主题 ButtonHover);两帧的像素哈希应不同"),
					Item(WuiInteractionKind::Click, "showcase.button", WuiInteractionExpect::Event,
						"在按钮中心注入按下(1 帧)+ 抬起(1 帧)",
						"一次激活;画布命令数不因点击变化,事件由操作记录/调用方观测"),
					Item(WuiInteractionKind::Key, "showcase.button", WuiInteractionExpect::Event,
						"Tab 到按钮出现焦点环 → Enter;再验一次 Space",
						"Enter/Space 等价于一次点击(只认本帧新按下);焦点环颜色 = border.focus"),
				}));

			WuiComponentRegistry::Register(Desc(
				"button.icon", "WuiImageButton", "Icon Button", "Buttons", WuiComponentStatus::Draft,
				"Engine/src/World/WUI/Widgets/WuiChrome.cpp",
				"role=button;id=HashId('showcase.button.icon')(控件自己登记,与保留模式 WuiImageButton 同一口径);label=label 属性;enabled/interactive/focused 跟随 enabled 与焦点;enabled=true 时进焦点表(Tab 可达、Enter/Space 激活,P1c-E4);disabled 不进焦点表但节点仍在;textureId=0 时退化成语义文字",
				"showcase 首选 32x24(方形图标位);enabled=false 时节点仍在,enabled/interactive 都是 false",
				ShellIds("button.icon"),
				StateList({ "default", "hover", "disabled", "long-text" }),
				{ PropText("label", "Save"), PropBool("disabled") },
				&ShowIconButton));

			// VEC-H6:脚本引用行的"在编辑器里打开"图标按钮(纯矢量 glyph `</>`;按钮文字进悬停提示)。
			// 用户口径:「Lua 脚本组件中的"在编辑器里打开"能不能换成一个图标,然后把文字放到悬浮提示里。」
			WuiComponentRegistry::Register(Desc(
				"button.open-in-editor", "OpenInEditorButton", "Open In Editor", "Buttons",
				WuiComponentStatus::Draft,
				"Engine/src/World/WUI/WuiWidgets.cpp",
				"role=button;id=HashId('showcase.button.open-in-editor')(控件自身登记);label=label 属性(图标没有可见文字 —— 语义全在 label/value/tooltip);value=tooltip 属性;enabled=false 时节点仍在(Enabled/Interactive=false)但不进焦点表,悬停仍给理由;启用态进焦点表,Tab 可达、Enter/Space 激活",
				"showcase 首选 24x24(方形图标位;面板脚本引用行用 20x20,与 kPropertyFieldHeight 对齐);glyph = 矢量 `</>`(两段折线尖括号 + 斜线),不依赖图标字体/纹理,主题与缩放无关",
				ShellIds("button.open-in-editor"),
				StateList({ "default", "hover", "focus", "disabled" }),
				{
					PropText("label", "Open in Editor"),
					PropText("tooltip", "Open this script asset in the built-in script editor"),
					PropBool("disabled"),
				},
				&ShowOpenInEditorButton,
				{
					Item(WuiInteractionKind::Hover, "showcase.button.open-in-editor",
						WuiInteractionExpect::PixelChange,
						"鼠标移到按钮中心(1 帧)→ 移开(1 帧)",
						"悬停填充 = theme.ButtonHover + Accent 描边(两帧像素哈希应不同)"),
					Item(WuiInteractionKind::Click, "showcase.button.open-in-editor",
						WuiInteractionExpect::Event,
						"在按钮中心注入按下(1 帧)+ 抬起(1 帧)",
						"一次激活(面板据此打开脚本编辑器);画布命令数不因点击变化"),
					Item(WuiInteractionKind::Key, "showcase.button.open-in-editor",
						WuiInteractionExpect::Event,
						"Tab 到按钮出现焦点环 → Enter;再验一次 Space",
						"Enter/Space 等价于一次点击(启用态才进焦点表)"),
				}));

			// VEC-H7:复位 `↺` 从"常态只剩一条灰线"改成一眼可见的可点控件 —— 四态 + 20×20 底板 +
			// 完整 a11y(与行内 `-`/`+` 同一套外观与节点契约)。
			WuiComponentRegistry::Register(Desc(
				"button.reset-default", "ResetDefaultButton", "Reset Default", "Buttons", WuiComponentStatus::Draft,
				"Engine/src/World/WUI/WuiWidgets.cpp",
				"role=button(VEC-H7 起;此前 role=reset-default,与行内 `-`/`+`/分组头对齐);"
				"id=HashId('showcase.button.reset-default')(控件自身登记);value=modified/default;"
				"enabled/interactive 跟随 modified(禁用态仍登记节点 + 理由进 Tooltip,不进焦点表);"
				"tooltip 进节点 Tooltip —— 悬停提示与读屏第二通道同源",
				"命中区 = 调用方给的 rect(属性行 24x24,4px 栅格,≥ 20x20);可见底板 = 20x20 居中"
				"(缩小绘制面、不缩小可点面);字形自己画(弧线 + 箭头随底板等比、线宽 ≥ 1px),不依赖图标字体/纹理",
				ShellIds("button.reset-default"),
				StateList({ "default", "modified", "hover", "focus", "pressed", "disabled" }),
				{ PropText("label", "Reset"), PropText("tooltip", "Restore the default value"), PropBool("modified") },
				&ShowResetDefaultButton,
				{
					Item(WuiInteractionKind::Hover, "showcase.button.reset-default",
						WuiInteractionExpect::PixelChange,
						"鼠标移到按钮中心(1 帧)→ 移开(1 帧)",
						"常态本来就有底板/描边/亮字形(VEC-H7);悬停再变 ButtonHover 底 + Accent 描边 + Accent 字形"
						"(两帧像素哈希应不同)"),
					Item(WuiInteractionKind::Click, "showcase.button.reset-default",
						WuiInteractionExpect::Event,
						"在按钮中心注入按下(1 帧,按下态 = Selection 底 + Accent 描边)+ 抬起(1 帧)",
						"一次复位事件(调用方把值写回默认);画布命令数不因点击变化"),
					Item(WuiInteractionKind::Key, "showcase.button.reset-default",
						WuiInteractionExpect::Event,
						"Tab 到按钮出现焦点环 → Enter;再验一次 Space",
						"Enter/Space 等价于一次复位(启用态才进焦点表;disabled 不进焦点表)"),
				}));

			WuiComponentRegistry::Register(Desc(
				"toggle", "Toggle", "Toggle", "Buttons", WuiComponentStatus::Draft,
				"Engine/src/World/WUI/WuiWidgets.cpp",
				"role=toggle;id=HashId('showcase.toggle')(控件自身登记);value=on/off;状态写回控件自己的 Persist<bool>(id)",
				"showcase 首选 148x24;标签从 24px 起画,16x16 方块垂直居中;H 同上",
				ShellIds("toggle"),
				StateList({ "default", "on", "off", "hover", "focus", "disabled", "long-text" }),
				{ PropText("label", "Enabled"), PropBool("value"), PropBool("disabled") },
				&ShowToggle));

			WuiComponentRegistry::Register(Desc(
				"segmented", "Segmented", "Segmented", "Buttons", WuiComponentStatus::Draft,
				"Engine/src/World/WUI/WuiWidgets.cpp",
				"role=segmented;id=HashId('showcase.segmented')(控件自己登记,覆盖外壳锚点);组节点 interactive=true —— 分段铺满整条矩形,按它注入的点击落在组中心那一格分段;组节点 focused=组内是否有焦点(P1c-E4);逐格精确点击/读焦点位用子节点 kind=segmented-option(派生 id=HashId(str(父id)+'.segment.'+i),focused=焦点是否在该格);value=当前选中项文案",
				"showcase 首选 210x24;项等分宽度;建议 2–4 项(空表直接返回)",
				ShellIds("segmented"),
				StateList({ "default", "hover", "focus" }),
				{ PropText("options", "Light,Medium,Heavy"), PropInt("selected", 0, 2, 1) },
				&ShowSegmented));

			// ---- Inputs ----
			WuiComponentRegistry::Register(Desc(
				"checkbox", "Checkbox", "Checkbox", "Inputs", WuiComponentStatus::Draft,
				"Engine/src/World/WUI/WuiWidgets.cpp",
				"role=checkbox;id=HashId('showcase.checkbox')(控件自身登记);value=true/false;返回值=本帧是否改值",
				"showcase 首选 168x24;16x16 方块 + 24px 起画文字;H 同上",
				ShellIds("checkbox"),
				StateList({ "default", "checked", "unchecked", "hover", "focus", "disabled", "long-text" }),
				{ PropText("label", "Enabled"), PropBool("checked"), PropBool("disabled") },
				&ShowCheckbox));

			WuiComponentRegistry::Register(Desc(
				"checkbox.mixed", "CheckboxMixed", "Checkbox (Mixed)", "Inputs", WuiComponentStatus::Draft,
				"Engine/src/World/WUI/WuiWidgets.cpp",
				"role=checkbox;id=HashId('showcase.checkbox.mixed');value=mixed/true/false;mixed 由调用方按值传入(本控件不持久化三态)",
				"同 checkbox:首选 168x24",
				ShellIds("checkbox.mixed"),
				StateList({ "default", "mixed", "checked", "unchecked", "disabled", "long-text" }),
				{ PropText("label", "Select all"), PropBool("mixed"), PropBool("disabled") },
				&ShowCheckboxMixed));

			WuiComponentRegistry::Register(Desc(
				"slider.float", "SliderFloat", "Slider", "Inputs", WuiComponentStatus::Draft,
				"Engine/src/World/WUI/WuiWidgets.cpp",
				"role=slider;id=HashId('showcase.slider.float')(控件自身登记);value=三位小数文本;焦点上 ←/→ = 1% 值域步进",
				"showcase 首选 190x24;轨道 4px 垂直居中;min/max 覆盖非法时退回 0..1",
				ShellIds("slider.float"),
				StateList({ "default", "hover", "focus", "disabled" }),
				{ PropFloat("value", 0.0f, 1.0f, 0.01f), PropFloat("min", -1000.0f, 1000.0f, 0.1f),
					PropFloat("max", -1000.0f, 1000.0f, 0.1f), PropBool("disabled") },
				&ShowSliderFloat));

			WuiComponentRegistry::Register(Desc(
				"dragfloat", "DragFloat", "Drag Float", "Inputs", WuiComponentStatus::Draft,
				"Engine/src/World/WUI/WuiWidgets.cpp",
				"role=drag-float(编辑态切 text-field);id=HashId('showcase.dragfloat');value=数值文本;↑/↓ = 步进,speed 为拖动灵敏度;Shift = 0.1× 精细、Ctrl = 10× 粗调(拖动与 ↑/↓ 共用同一倍率,VEC-H4)",
				"showcase 首选 150x24;范围默认 0..10;min>=max 视为无界(与控件哨兵约定一致)",
				ShellIds("dragfloat"),
				StateList({ "default", "hover", "focus", "disabled" }),
				{ PropFloat("value", -1000.0f, 1000.0f, 0.1f), PropFloat("min", -1000.0f, 1000.0f, 0.1f),
					PropFloat("max", -1000.0f, 1000.0f, 0.1f), PropFloat("speed", 0.001f, 100.0f, 0.01f),
					PropBool("disabled") },
				&ShowDragFloat));

			WuiComponentRegistry::Register(Desc(
				"dragbar.float", "DragBarFloat", "Drag Bar", "Inputs", WuiComponentStatus::Draft,
				"Engine/src/World/WUI/WuiWidgets.cpp",
				"role=slider(编辑态切 text-field);id=HashId('showcase.dragbar.float');value=显示值+单位;右侧值区宽度固定(style.ValueWidth,最小 40)",
				"showcase 首选 200x24;值区固定宽度是「点恢复默认不改行矩形」的前提",
				ShellIds("dragbar.float"),
				StateList({ "default", "hover", "focus" }),
				{ PropFloat("value", 0.0f, 1.0f, 0.01f), PropFloat("min", -1000.0f, 1000.0f, 0.1f),
					PropFloat("max", -1000.0f, 1000.0f, 0.1f), PropText("unit", "%") },
				&ShowDragBarFloat));

			WuiComponentRegistry::Register(Desc(
				"numberfield.int", "NumberFieldInt", "Number Field", "Inputs", WuiComponentStatus::Draft,
				"Engine/src/World/WUI/WuiWidgets.cpp",
				"role=number-field(编辑态切 text-field);id=HashId('showcase.numberfield.int');value=整数+单位;steppers=true 时额外登记 DerivedChildId(id,'.dec'/'.inc',0) 两个 stepper-button",
				"showcase 首选 128x24;步进钮宽 = clamp(18..24, 18% 宽);计数/索引类字段不做拖动改值",
				ShellIds("numberfield.int"),
				StateList({ "default", "hover", "focus", "disabled" }),
				{ PropInt("value", 1, 16384, 1), PropText("unit", "px"), PropBool("steppers"), PropBool("disabled") },
				&ShowNumberFieldInt));

			WuiComponentRegistry::Register(Desc(
				"stepper.int", "StepperInt", "Stepper", "Inputs", WuiComponentStatus::Draft,
				"Engine/src/World/WUI/WuiWidgets.cpp",
				"role=stepper(编辑态切 text-field);id=HashId('showcase.stepper.int');[−]/[+] 为 DerivedChildId(id,'.dec'/'.inc',0)",
				"showcase 首选 132x24;小范围整数(本条目展示 1..16);值区可键入",
				ShellIds("stepper.int"),
				StateList({ "default", "hover", "focus", "disabled" }),
				{ PropInt("value", 1, 16, 1), PropBool("disabled") },
				&ShowStepperInt));

			WuiComponentRegistry::Register(Desc(
				"textfield", "WuiTextField", "Text Field", "Inputs", WuiComponentStatus::Draft,
				"Engine/src/World/WUI/WuiWidgets.cpp",
				"role=text-field;id=HashId('showcase.textfield')(控件自身登记);label=属性 label、占位=属性 placeholder(label/value 都进节点);光标走 TextCursorByte",
				"showcase 首选 210x24;行内无纵向余量;长文本由控件自己滚动/裁剪",
				ShellIds("textfield"),
				StateList({ "default", "hover", "focus", "disabled", "long-text" }),
				{ PropText("value", "Player"), PropText("label", "Name"), PropText("placeholder", "Enter a name"),
					PropBool("disabled") },
				&ShowTextField));

			WuiComponentRegistry::Register(Desc(
				"textfield.error", "TextFieldEx", "Text Field (Error)", "Inputs", WuiComponentStatus::Draft,
				"Engine/src/World/WUI/WuiWidgets.cpp",
				"role=text-field;id=HashId('showcase.textfield.error');value 追加 ' error=<文本>';错误行画在控件下方一行(Caption),调用方负责留高度",
				"showcase 首选 210x24 + 下方 14px 错误行;错误态描边用 theme.Danger",
				ShellIds("textfield.error"),
				StateList({ "default", "valid", "focus", "disabled" }),
				{ PropText("value", "0.0"), PropText("label", "Mass"), PropText("error", "Must be greater than zero") },
				&ShowTextFieldError));

			WuiComponentRegistry::Register(Desc(
				"combo", "Combo", "Combo", "Inputs", WuiComponentStatus::Draft,
				"Engine/src/World/WUI/WuiWidgets.cpp",
				"role=combo;id=HashId('showcase.combo');弹层条目 kind=combo-option(id=HashId(str(父id)+'.option.'+i));state=open 时 showcase 打开弹层",
				"showcase 首选 190x24;弹层高 = 条目 22px×n + 8,在画布下方需要留空间",
				ShellIds("combo"),
				StateList({ "default", "hover", "focus", "open", "disabled" }),
				{ PropText("options", "Low,Medium,High,Ultra"), PropInt("selected", 0, 3, 1), PropBool("disabled") },
				&ShowCombo));

			WuiComponentRegistry::Register(Desc(
				"combo.searchable", "SearchableCombo", "Searchable Combo", "Inputs", WuiComponentStatus::Draft,
				"Engine/src/World/WUI/WuiWidgets.cpp",
				"role=search-combo;id=HashId('showcase.combo.searchable');弹层 combo-option + combo-item(id=HashId(itemKey));过滤串用 id^0x5A17 的持久槽(与编辑缓冲不同 id)",
				"showcase 首选 190x24;选项多时用滚轮;同一 id 的不同类型持久槽是崩溃源,勿混用",
				ShellIds("combo.searchable"),
				StateList({ "default", "hover", "focus", "open" }),
				{ PropText("options", "Low,Medium,High,Ultra"), PropInt("selected", 0, 3, 1) },
				&ShowSearchableCombo));

			// M4-TEX-P10:纹理引用的**唯一实现**(概念归属表 tools/agents/wui-concept-owners.json 的
			// owner)。面板的材质槽位 / `.slang` 参数行 / Texture Settings 都不许再自建 —— 一律走本件。
			// 属性按 P1.5 三块 + Behavior;状态词与 a11y value 都是稳定 token(探针不读中英文文案)。
			WuiComponentRegistry::Register(Desc(
				"wui.texture-picker", "WuiTexturePicker", "Texture Picker", "Inputs",
				WuiComponentStatus::Draft,
				"Engine/src/World/WUI/WuiTexturePicker.cpp",
				"role=search-combo(id=调用方 id;工作台 = HashId('showcase.wui.texture-picker'));状态节点 <IdPrefix>.state(kind=texture-ref,value=auto/empty/set/missing/missing-source/stale/unbaked/legacy/container);定位按钮 <IdPrefix>.locate;候选项 combo-option/combo-item 由 SearchableCombo 登记(搜索匹配完整逻辑路径,越界只在绘制期中间省略);只读/不可挑选时节点 enabled/interactive=false",
				"showcase 首选 300x24;定位按钮恒定 26+6,徽标条最多占槽宽 45%(放不下的徽标不画,状态与 token 表仍在 a11y 上);列表弹层由 SearchableCombo 画在槽位上下方(空间不够自动向上翻)",
				A11yIds({ "showcase.wui.texture-picker", "showcase.wui.texture-picker.state",
					"showcase.wui.texture-picker.locate" }),
				StateList({ "default", "hover", "focus", "open", "empty", "missing", "missing-source", "stale",
					"unbaked", "legacy", "container", "external", "long-text", "disabled" }),
				{
					// ---- Content ----
					Grouped(PropText("label", "Albedo"), WuiComponentPropertyGroup::Content,
						"控件标签(a11y label + 列表搜索框的占位)。"),
					Grouped(PropText("Value", "textures/Icon.wtex"), WuiComponentPropertyGroup::Content,
						"当前纹理引用(逻辑路径:源图或 .wtex 资产)。不在候选清单里时补一条兜底条目,"
						"下拉不会回显成 '(none)'。"),
					Grouped(PropText("Badges", "asset,container,baked"), WuiComponentPropertyGroup::Content,
						"当前值的徽标 token 表(逗号/竖线/分号分隔):asset/source/container/baked/stale/"
						"needs-rebake/unbaked/legacy/missing/missing-source/no-source/unreadable;"
						"未知 token 原样以中性色显示(不静默吞掉)。"),
					Grouped(PropText("ImportSourceName", "Icon.png"), WuiComponentPropertyGroup::Content,
						"导入源名(只用于括号里的显示与兜底条目名;值本身仍是逻辑路径)。"),
					// ---- Style ----
					Grouped(UnitOf(NumberDefault(PropFloat("badgeFontSize", 8.0f, 16.0f, 0.5f), 11.0f), "px"),
						WuiComponentPropertyGroup::Style,
						"徽标字号(未覆盖 = 11px);徽标高 = 字号 + 5,宽度按量出来的文本宽 + 10。"),
					Grouped(NumberDefault(PropFloat("badgeFillAlpha", 0.0f, 1.0f, 0.05f), 0.22f),
						WuiComponentPropertyGroup::Style,
						"徽标底色透明度(未覆盖 = 0.22;文字色始终是语义色的不透明版)。"),
					// ---- Layout ----
					PropSize2("preferred", 300.0f, 24.0f, "首选尺寸 宽×高(设计单位;未覆盖 = 300x24)。"),
					// ---- Behavior ----
					Grouped(PropBool("AllowClear"), WuiComponentPropertyGroup::Behavior,
						"列表里给 '(none)' 项,选中即清空引用(工作台上默认开)。"),
					Grouped(PropBool("AllowReveal"), WuiComponentPropertyGroup::Behavior,
						"右侧定位按钮(在资源管理器中显示);值空时按钮禁用并给出理由。"),
					Grouped(PropBool("AllowPick"), WuiComponentPropertyGroup::Behavior,
						"点击展开搜索列表;关闭 = 只展示引用(不可挑选,节点 interactive=false)。"),
					Grouped(PropBool("ReadOnly"), WuiComponentPropertyGroup::Behavior,
						"只读:不可挑选、不可拖放、不响应键盘,节点 enabled=false。"),
					Grouped(PropText("State", "auto"), WuiComponentPropertyGroup::Behavior,
						"显式状态(auto/empty/set/missing/missing-source/stale/unbaked/legacy/container);"
						"auto = 由 Value + Badges 推导。"),
				},
				&ShowTexturePicker,
				{
					Item(WuiInteractionKind::Click, "showcase.wui.texture-picker", WuiInteractionExpect::PixelChange,
						"点击槽位中心 → 展开搜索列表(弹层画出候选项)",
						"候选项 label 是完整逻辑路径(资产带 '(源图名)' 后缀);越界只在绘制期中间省略"),
					Item(WuiInteractionKind::Key, "showcase.wui.texture-picker", WuiInteractionExpect::PixelChange,
						"Tab 到槽位 → Enter 展开;Esc 关闭",
						"与点击同一条打开路径;焦点在槽位上时 Enter/Space 都展开"),
				}));

			WuiComponentRegistry::Register(Desc(
				"colorfield", "ColorField", "Color Field", "Inputs", WuiComponentStatus::Draft,
				"Engine/src/World/WUI/WuiWidgets.cpp",
				"role=color-field;id=HashId('showcase.colorfield');value=#RRGGBB(带 alpha 8 位);弹层子节点 .sv./.hue./.alpha./.slider./.preset./.hex.(派生 id 见 WuiWidgets.cpp DerivedChildId)",
				"showcase 首选 190x22;弹层 220x132 需要画布右侧/下方留空间",
				ShellIds("colorfield"),
				StateList({ "default", "hover", "focus", "open", "disabled" }),
				{ PropText("color", "#4C8DFF"), PropBool("disabled") },
				&ShowColorField));

			WuiComponentRegistry::Register(Desc(
				"vec3field", "Vec3Field", "Vec3 Field", "Inputs", WuiComponentStatus::Draft,
				"Engine/src/World/WUI/WuiWidgets.cpp",
				"role=vec3-field;id=HashId('showcase.vec3field');value='x,y,z';三个分量各登记 vec3-axis(id=HashId(str(父id)+'.axis.'+i));↑/↓ 调当前分量,Shift = 0.1×、Ctrl = 10×(VEC-H4,与 DragFloat 同一倍率)",
				"showcase 首选 220x24;窄画布(<约 220)时 layout=1 竖排(本条目用 state=vertical 展示)",
				ShellIds("vec3field"),
				StateList({ "default", "hover", "focus", "vertical", "disabled" }),
				{ PropText("value", "0,1,0"), PropFloat("speed", 0.001f, 1.0f, 0.001f), PropBool("disabled") },
				&ShowVec3Field));

			WuiComponentRegistry::Register(Desc(
				"vec2field", "Vec2Field", "Vec2 Field", "Inputs", WuiComponentStatus::Draft,
				"Engine/src/World/WUI/WuiWidgets.cpp",
				"role=vec2-field;id=HashId('showcase.vec2field');value='x,y';两个分量各登记 vec2-axis(id=HashId(str(父id)+'.axis.'+i),label='X'/'Y');默认一行两段,layout=1 两行",
				"showcase 首选 220x24;一行两段(与 Vec3Field 同一行高);窄列(<约 140)用 layout=1 竖排(本条目用 state=vertical 展示)",
				ShellIds("vec2field"),
				StateList({ "default", "hover", "focus", "vertical", "disabled" }),
				{ PropText("value", "0,1"), PropFloat("speed", 0.001f, 1.0f, 0.001f), PropBool("disabled") },
				&ShowVec2Field));

			WuiComponentRegistry::Register(Desc(
				"vec4field", "Vec4Field", "Vec4 Field", "Inputs", WuiComponentStatus::Draft,
				"Engine/src/World/WUI/WuiWidgets.cpp",
				"role=vec4-field;id=HashId('showcase.vec4field');value='x,y,z,w';四个分量各登记 vec4-axis(id=HashId(str(父id)+'.axis.'+i),label='X'/'Y'/'Z'/'W');默认 2×2(第一行 x y / 第二行 z w),layout=1 四行",
				"showcase 首选 220x52(2×2:两行各 26);layout=1 竖排要 4 行(窄列/矮槽下更挤,本条目用 state=vertical 展示)",
				ShellIds("vec4field"),
				StateList({ "default", "hover", "focus", "vertical", "disabled" }),
				{ PropText("value", "0,1,0,1"), PropFloat("speed", 0.001f, 1.0f, 0.001f), PropBool("disabled") },
				&ShowVec4Field));

			WuiComponentRegistry::Register(Desc(
				"searchfield", "SearchField", "Search Field", "Inputs", WuiComponentStatus::Draft,
				"Engine/src/World/WUI/Widgets/WuiChrome.cpp",
				"role=text-field;id=HashId('showcase.searchfield') —— P1c-E4 起**无条件**登记节点并进焦点表(Tab 可达;以前空且未聚焦时不登记节点,焦点入口又藏在聚焦分支里 → 永远到不了);label=占位文案、value=内容(空时回落占位文案)、focused 跟随焦点;聚焦后同一节点由内部 TextField 重登记(同 id/同 kind)",
				"showcase 首选 200x24;放大镜占 18px、清除按钮占 18px",
				ShellIds("searchfield"),
				StateList({ "default", "focus", "disabled" }),
				{ PropText("value", ""), PropText("placeholder", "Search assets"), PropBool("disabled") },
				&ShowSearchField));

			WuiComponentRegistry::Register(Desc(
				"codeeditor", "CodeEditor", "Code Editor", "Inputs", WuiComponentStatus::Draft,
				"Engine/src/World/WUI/WuiCodeEditor.cpp",
				"role=code-editor;id=HashId('showcase.codeeditor')(控件自己登记);interactive=true(点它 = 聚焦 + caret 定位,之后 ui.type/ui.key 落在该焦点上);enabled=!readonly;value='<n> lines[/, read-only]';P1c-E4 起进焦点表(Tab 可达、DrawFocusRing),持焦后 Tab 归编辑器(缩进)、Escape 退出;补全浮层可见时另登记 '<CompletionIdPrefix>.<i>' 与 '.status',悬停登记 '.hover'",
				"showcase 首选 280x104;行高 19×UiScale;ErrorLine 用 state=error 展示",
				ShellIds("codeeditor"),
				StateList({ "default", "focus", "error" }),
				{ PropBool("readonly") },
				&ShowCodeEditor));

			// ---- Containers ----
			WuiComponentRegistry::Register(Desc(
				"tabs", "TabBar", "Tabs", "Containers", WuiComponentStatus::Draft,
				"Engine/src/World/WUI/WuiWidgets.cpp",
				"role=tab-bar;id=HashId('showcase.tabs')(控件自己登记,覆盖外壳锚点);父节点 interactive=true —— 标签等分整条矩形,按它注入的点击落在组中心那一个标签上;父节点 focused=组内是否有焦点(P1c-E4);逐标签精确点击/读焦点位用子节点 kind=tab(id=HashId(str(父id)+'.tab.'+i),value=true/false,focused=焦点是否在该标签)",
				"showcase 首选 240x24;标签等分整条矩形;中键点击上报 closeRequested(本 showcase 不接)",
				ShellIds("tabs"),
				StateList({ "default", "hover", "focus" }),
				{ PropText("tabs", "General,Rendering,Physics"), PropInt("active", 0, 2, 1) },
				&ShowTabs));

			WuiComponentRegistry::Register(Desc(
				"treenode", "TreeNode", "Tree Node", "Containers", WuiComponentStatus::Draft,
				"Engine/src/World/WUI/WuiWidgets.cpp",
				"role=tree-node;id=HashId('showcase.treenode')(控件自身登记);value=open/closed;leaf=true 时 interactive=false 且不进 Tab 顺序",
				"showcase 首选 190x20;展开/收起标记用 '+'/'-' 文本,不依赖字体箭头字形",
				ShellIds("treenode"),
				StateList({ "default", "open", "closed", "hover", "focus", "disabled" }),
				{ PropText("label", "Materials"), PropBool("leaf") },
				&ShowTreeNode));

			WuiComponentRegistry::Register(Desc(
				"treeview", "TreeView", "Tree View", "Containers", WuiComponentStatus::Draft,
				"Engine/src/World/WUI/WuiChrome.cpp",
				"role=tree(容器,id 同传入 id)+ tree-item(每行节点 id=条目自己的 Id —— showcase 用 HashId('showcase.treeview.item.N'),ExtraA11yIds 可复算;value='depth=.. expanded=.. children=..');容器 P1c-E4 起登记:rect=可见行带、value='items=N selected=<行>'、interactive=true(点它 = 命中中心那一行)、focused=焦点在树上;焦点在树上时容器与当前行都带 focused=true;键盘:↑/↓/Home/End 移光标、←/→ 折叠展开、Enter 激活(只报告,改模型归调用方)",
				"showcase 首选 230x96(自带滚动裁剪);行高 22×Density;半滚出行不登记节点(中心必须落在可视区)",
				A11yIds({ "showcase.treeview", "showcase.treeview.item.0", "showcase.treeview.item.1",
					"showcase.treeview.item.2", "showcase.treeview.item.3" }),
				StateList({ "default", "hover", "selected", "focus", "disabled" }),
				{ PropText("label", "Assets"), PropText("disabled", "Locked"), PropFloat("scroll", 0.0f, 200.0f, 1.0f) },
				&ShowTreeView));

			WuiComponentRegistry::Register(Desc(
				"listview", "ListView", "List View", "Containers", WuiComponentStatus::Draft,
				"Engine/src/World/WUI/WuiChrome.cpp",
				"role=list(容器,P1c-E4 起 id 由调用方下沉:rect=可见行带、value='items=N selected=<行>'、interactive=true(点它 = 命中中心那一行)、focused=焦点在列表上)+ list-row(控件用调用方给的 items[i].Id 登记,showcase 用 HashId('showcase.listview.item.N'));label=行文案、value=SubLabel、disabled 行 enabled/interactive=false;焦点在列表上时当前行 focused=true;键盘:↑/↓/Home/End 移光标(KeyMoveTo)、Enter/Space 激活(KeyActivate,只报告);行中心必须落在可视区内才登记(半滚出的行不登记)",
				"showcase 首选 230x96(自带滚动裁剪);行高 24×Density;选中行底色 theme.PanelBg、悬停 ButtonHover",
				A11yIds({ "showcase.listview", "showcase.listview.item.0", "showcase.listview.item.1",
					"showcase.listview.item.2" }),
				StateList({ "default", "hover", "selected", "disabled", "scrolled" }),
				{ PropText("label", "Textures"), PropText("disabled", "Locked"), PropFloat("scroll", 0.0f, 200.0f, 1.0f) },
				&ShowListView));

			WuiComponentRegistry::Register(Desc(
				"table.header", "TableHeader", "Table Header", "Containers", WuiComponentStatus::Draft,
				"Engine/src/World/WUI/WuiWidgets.cpp",
				"role=table-header-row;id=HashId('showcase.table.header')(控件自己登记,覆盖外壳锚点);整行 interactive=true —— 列等分整行,按它注入的点击落在组中心那一列并触发排序,value='sort=<列> asc|desc';逐列精确点击用子节点 kind=table-header(id=HashId(str(父id)+'.col.'+i),value=asc/desc/空)",
				"showcase 首选 260x24 表头 + 22px 数据行;列宽等分(真实表格由调用方给列宽表)",
				ShellIds("table.header"),
				StateList({ "default", "hover", "focus" }),
				{ PropText("columns", "Name,Type,Size"), PropInt("sort", 0, 2, 1), PropBool("ascending") },
				&ShowTableHeader));

			WuiComponentRegistry::Register(Desc(
				"scrollarea", "WuiScrollArea", "Scroll Area", "Containers", WuiComponentStatus::Draft,
				"Engine/src/World/WUI/WuiWidgets.cpp",
				"role=scroll-area(P1c-E4:id 由调用方下沉;value='scroll=<y>/<max>'、interactive=false —— 容器自己不是点击目标,内容行才是);进焦点表后 Tab 可达并画焦点环,焦点在它上面时 ↑/↓=40px、PageUp/PageDown=0.9 屏、Space=下一页、Home/End=两端;鼠标悬停滚轮照旧(旧调用点不传 id = 行为不变)",
				"showcase 首选 220x96,内容 8 行×24;state=scrolled 展示滚动后的裁剪边界",
				ShellIds("scrollarea"),
				StateList({ "default", "scrolled" }),
				{ PropFloat("scroll", 0.0f, 400.0f, 1.0f), PropText("rowPrefix", "Row ") },
				&ShowScrollArea));

			WuiComponentRegistry::Register(Desc(
				"modal", "BeginModal", "Modal Dialog", "Containers", WuiComponentStatus::Draft,
				"Engine/src/World/WUI/WuiWidgets.cpp",
				"role=无(外框不登记节点;内部按钮各自登记 button = HashId('showcase.modal.confirm'/'showcase.modal.cancel'));外壳锚点 kind=component-root",
				"居中外框按 ctx.ViewportSize() 计算:size 传入即被采用;遮罩/外框会登记覆盖层(下一帧挡下层),工作台宜给它独立画布",
				A11yIds({ "showcase.modal", "showcase.modal.confirm", "showcase.modal.cancel" }),
				StateList({ "default", "long-text" }),
				{ PropText("title", "Delete entity?"), PropText("message", "This action cannot be undone."),
					PropText("confirm", "Confirm"), PropText("cancel", "Cancel") },
				&ShowModal));

			WuiComponentRegistry::Register(Desc(
				"empty.state", "EmptyState", "Empty State", "Containers", WuiComponentStatus::Draft,
				"Engine/src/World/WUI/WuiWidgets.cpp",
				"role=empty-state;节点 id = actionLabel 非空时 DerivedChildId(actionId,'.empty-state.',0),否则 HashId('empty-state:'+title);value=hint;interactive=false;有 action 时按钮由 Button 自己登记",
				"showcase 首选 280x120;左右各留 24px 安全边距;state=empty 展示无 glyph/无按钮的纯文案形态",
				ShellIds("empty.state"),
				StateList({ "default", "empty" }),
				{ PropText("title", "No assets yet"), PropText("hint", "Import a texture or a model to get started."),
					PropText("action", "Import") },
				&ShowEmptyState));

			WuiComponentRegistry::Register(Desc(
				"splitter", "Splitter", "Splitter", "Containers", WuiComponentStatus::Draft,
				"Engine/src/World/WUI/WuiWidgets.cpp",
				"role=splitter;id=HashId('showcase.splitter')(控件自身登记);value=当前值;命中带宽固定 6px(传入矩形即那条带)",
				"showcase 首选 200x72(两栏各一块底板);拖动按按下时的值 + 轴向位移累加,夹在 [min,max]",
				ShellIds("splitter"),
				StateList({ "default", "hover", "focus" }),
				{ PropFloat("value", 24.0f, 200.0f, 1.0f) },
				&ShowSplitter));

			WuiComponentRegistry::Register(Desc(
				"box", "WuiBox", "Box (Layout)", "Containers", WuiComponentStatus::Draft,
				"Engine/src/World/WUI/WuiWidget.cpp",
				"role=无(布局容器不登记节点;子件各自登记自己的 a11y);showcase 外壳锚点 kind=component-root、interactive=false",
				"填满画布(showcase 首选 240x84);Direction/Gap 由属性控制;子件按 intrinsic 尺寸经 SolveFlex 排布 —— 与面板同一条布局路径",
				ShellIds("box"),
				StateList({ "default", "row" }),
				{ PropFloat("gap", 0.0f, 24.0f, 1.0f), PropText("direction", "column") },
				&ShowBox));

			WuiComponentRegistry::Register(Desc(
				"spacer", "WuiSpacer", "Spacer", "Containers", WuiComponentStatus::Draft,
				"Engine/src/World/WUI/WuiWidget.cpp",
				"role=无(不画像素、不进焦点表,所以没有自己的节点);showcase 外壳锚点 kind=component-root、interactive=false,并用左右两个 WuiLabel 标出它撑开的间距",
				"自身尺寸 = Width×Height(默认 0x0,由调用方给);showcase 首选 220x20;示例间距 48px(UiScale 参与缩放)",
				ShellIds("spacer"),
				StateList({ "default" }),
				{ PropFloat("width", 0.0f, 240.0f, 1.0f), PropText("left", "Left"), PropText("right", "Right") },
				&ShowSpacer));

			WuiComponentRegistry::Register(Desc(
				"listrow", "WuiListRow", "List Row", "Containers", WuiComponentStatus::Draft,
				"Engine/src/World/WUI/WuiWidget.cpp",
				"role=list-row(SetId 后才进树与焦点表:id=HashId('showcase.listrow');label=行文本;value=AccessValue;focused=焦点在该行;P1c-E4 起 Tab 可达,Enter/Space 与点击走同一个 OnClick — 行级焦点,不抢调用方的选中模型);外层 WuiBox/同级标题不登记节点",
				"showcase 首选 224x72(父行 + 本行 + 兄弟行);行高 = FontSize+6;Indent 只挪文字,不动选中/悬停底色",
				ShellIds("listrow"),
				StateList({ "default", "hover", "selected" }),
				{ PropText("label", "Textures/Icon.png"), PropBool("selected"), PropFloat("indent", 0.0f, 32.0f, 1.0f) },
				&ShowListRow));

			// ---- Chrome ----
			WuiComponentRegistry::Register(Desc(
				"sectionheader", "SectionHeader", "Section Header", "Chrome", WuiComponentStatus::Draft,
				"Engine/src/World/WUI/Widgets/WuiChrome.cpp",
				"role=无(纯展示);外壳锚点 kind=component-root、interactive=false;字号默认 15(UiScale 参与缩放)",
				"showcase 首选 220x24;标题 + 下方 1px 分隔线;长标题按传入宽度绘制(控件不自行省略)",
				ShellIds("sectionheader"),
				StateList({ "default", "long-text" }),
				{ PropText("title", "Transform") },
				&ShowSectionHeader));

			WuiComponentRegistry::Register(Desc(
				"separator", "WuiSeparator", "Separator", "Chrome", WuiComponentStatus::Draft,
				"Engine/src/World/WUI/Widgets/WuiControls.cpp",
				"role=无(纯展示分隔线,不登记节点);外壳锚点 kind=component-root、interactive=false;不进焦点表 —— 分隔线不是输入目标;线色取主题 Border 令牌、厚度=thickness 属性",
				"showcase 首选 220x12(画布高度用 12 而不是 1:线仍画在矩形垂直中心、厚度=thickness×UiScale,占位高一点便于看与点);宽度由调用方给,不改布局",
				ShellIds("separator"),
				StateList({ "default" }),
				{ PropFloat("thickness", 0.5f, 6.0f, 0.5f) },
				&ShowSeparator));

			WuiComponentRegistry::Register(Desc(
				"breadcrumb", "Breadcrumb", "Breadcrumb", "Chrome", WuiComponentStatus::Draft,
				"Engine/src/World/WUI/Widgets/WuiChrome.cpp",
				"role=breadcrumb;id=HashId('showcase.breadcrumb')(P1c-E4 起由控件自己登记:id 参数已下沉进 Breadcrumb,以前的外壳代登记已删);interactive=true —— 点它 = 命中组中心所在的那一段;label=整条路径、value='segments=N cursor=<光标段>'、focused=焦点在整条上;键盘:←/→ 移段光标(该段画悬停同档高亮)、Enter/Space 激活光标段(返回值 = 段下标,与点击同语义);路径段不单独登记节点",
				"showcase 首选 230x20;段宽按粗略字宽(7px/字符)算,仅影响命中不影响绘制",
				ShellIds("breadcrumb"),
				StateList({ "default", "hover" }),
				{ PropText("path", "assets/textures/icon.png") },
				&ShowBreadcrumb));

			WuiComponentRegistry::Register(Desc(
				"tooltip", "Tooltip", "Tooltip", "Chrome", WuiComponentStatus::Draft,
				"Engine/src/World/WUI/Widgets/WuiChrome.cpp",
				"role=无独立节点(tooltip 是文本,不是可聚焦控件);宿主画完所有面板后调 DrawTooltip 一次,画到 overlay;文本进 ctx.Tooltip() 并由调用控件带进 a11y 节点 Tooltip 字段",
				"气泡跟随光标右下 16/20,超出视口翻到另一侧;最大文本宽 380px,超宽换行",
				ShellIds("tooltip"),
				StateList({ "default" }),
				{ PropText("label", "Hover me"), PropText("tooltip", "Adds a new entity to the current scene.") },
				&ShowTooltip));

			WuiComponentRegistry::Register(Desc(
				"label", "WuiLabel", "Label", "Chrome", WuiComponentStatus::Draft,
				"Engine/src/World/WUI/WuiWidget.cpp",
				"role=无(WuiLabel 不登记节点,纯展示);showcase 外壳锚点 kind=component-root、interactive=false;文本/字号/粗体都由调用方给",
				"showcase 首选 220x20;宽高 = 字宽×文本 / FontSize+6;不裁剪(超宽会溢出调用方的矩形)",
				ShellIds("label"),
				StateList({ "default", "long-text" }),
				{ PropText("text", "Player Name"), PropFloat("fontSize", 8.0f, 32.0f, 1.0f), PropBool("bold") },
				&ShowLabel));

			WuiComponentRegistry::Register(Desc(
				"image", "WuiImage", "Image", "Chrome", WuiComponentStatus::Draft,
				"Engine/src/World/WUI/WuiWidget.cpp",
				"role=无(纯展示纹理,不登记节点);showcase 外壳锚点 kind=component-root、interactive=false;纹理 id 必须来自宿主的 WuiTextureRegistry(0 = 空图,节点仍在)",
				"showcase 首选 96x96;纹理 id / UV / tint 由调用方给(视口、材质预览、模型预览都是这条路径)",
				ShellIds("image"),
				StateList({ "default" }),
				{ PropInt("textureId", 0.0f, 100000.0f, 1.0f), PropText("tint", "#FFFFFF") },
				&ShowImage));

			WuiComponentRegistry::Register(Desc(
				"badge", "Badge", "Badge", "Chrome", WuiComponentStatus::Draft,
				"Engine/src/World/WUI/Widgets/WuiChrome.cpp",
				"role=无(纯展示徽标,不登记节点);外壳锚点 kind=component-root、interactive=false;填充/字色/字号/粗体都由调用方给(粗体默认开);文字不裁剪(超宽会溢出调用方的矩形)",
				"showcase 首选 168x18,同一帧画两枚(中性底 + 强调色底);正方形(如 16x16 实例徽标)+ radius<0 = 圆,胶囊给 W≥H 的矩形",
				ShellIds("badge"),
				StateList({ "default", "long-text" }),
				{ PropText("text", "Material Shader"), PropText("fill", "#2A3038"), PropText("textColor", "#D7DCE3"),
					PropFloat("fontSize", 8.0f, 20.0f, 0.5f), PropBool("bold") },
				&ShowBadge));

			WuiComponentRegistry::Register(Desc(
				"icon", "Icon", "Icon (Tinted)", "Chrome", WuiComponentStatus::Draft,
				"Engine/src/World/WUI/Widgets/WuiChrome.cpp",
				"role=无(纯展示图标,不登记节点);外壳锚点 kind=component-root、interactive=false;纹理 id 来自宿主 WuiTextureRegistry(与 WuiImage 同一口径),tint 由调用方给;textureId=0 = 空图 → 兜底占位(底 + 2×2 棋盘 + 描边),不是空白、也不是纯色",
				"showcase 首选 32x32;方形图标位(内容网格 20x20 / 列表 16x16 / 工具条 18x18 都按调用方给的矩形画,控件不改布局)",
				ShellIds("icon"),
				StateList({ "default" }),
				{ PropInt("textureId", 0.0f, 100000.0f, 1.0f), PropText("tint", "#FFFFFF") },
				&ShowIcon));

			// ---- Menus ----
			WuiComponentRegistry::Register(Desc(
				"contextmenu", "BeginContextMenu", "Context Menu", "Menus", WuiComponentStatus::Draft,
				"Engine/src/World/WUI/Widgets/WuiChrome.cpp",
				"role=menu-item(每项 id=HashId('showcase.contextmenu.<action>'));带勾选项 value=checked/unchecked;菜单面板本身不登记节点;位置在打开时钉住",
				"面板高 = 条目 22px×itemCount + 8;showcase 画 4 项(含 1 分隔与 1 禁用项)",
				A11yIds({ "showcase.contextmenu", "showcase.contextmenu.copy", "showcase.contextmenu.visible",
					"showcase.contextmenu.delete" }),
				StateList({ "default", "hover", "focus", "disabled" }),
				{ PropText("copy", "Copy"), PropText("visible", "Visible"), PropText("delete", "Delete"),
					PropBool("disabled") },
				&ShowContextMenu));

			// ---- Feedback ----
			WuiComponentRegistry::Register(Desc(
				"progress", "WuiProgress", "Progress Bar", "Feedback", WuiComponentStatus::Draft,
				"Engine/src/World/WUI/WuiWidget.cpp",
				"role=无(WuiProgress 不登记节点 —— 它是保留模式控件,读数面板用同样的 LayoutWidgetTree+Paint);外壳锚点 kind=component-root;Fraction 夹在 0..1",
				"showcase 首选 200x12;轨道/填充都是 3px 圆角矩形;调用方决定行高",
				ShellIds("progress"),
				StateList({ "default", "disabled" }),
				{ PropFloat("value", 0.0f, 1.0f, 0.01f), PropBool("disabled") },
				&ShowProgress));

			// ---- P1c-LIB2 渐变填充 / 可折叠分区标题 / 禁用+理由按钮 / 有状态滚动条 + P1c-LIB3 线段原语 ----
			WuiComponentRegistry::Register(Desc(
				"gradient", "GradientFill", "Gradient Fill", "Chrome", WuiComponentStatus::Draft,
				"Engine/src/World/WUI/WuiWidgets.cpp",
				"role=无(纯绘制原语,不登记节点);外壳锚点 kind=component-root、interactive=false;四角颜色顺序 TL/TR/BR/BL(与 WuiDrawCommand::Corners 一致),矩形逐字段透传、不裁剪不圆角",
				"showcase 首选 220x64;两个方向态(纵向上→下 / 横向左→右)共用同一块矩形;四角颜色由调用方给,控件不改几何",
				ShellIds("gradient"),
				StateList({ "default", "horizontal" }),
				{ PropText("top", "#3D4554"), PropText("bottom", "#0D0F14") },
				&ShowGradient));

			WuiComponentRegistry::Register(Desc(
				"line-segment", "LineSegment", "Line Segment", "Chrome", WuiComponentStatus::Draft,
				"Engine/src/World/WUI/WuiWidgets.cpp",
				"role=无(纯绘制原语,不登记节点);外壳锚点 kind=component-root、interactive=false;端点原样使用(投影/近平面裁剪/夹取留在调用方),法线=(-dy,dx)/|d|、偏移=法线*thickness/2、顶点顺序 {from+n,to+n,to-n,from-n},|to-from|<0.5 不产出命令",
				"showcase 首选 220x64;一条线段 = 一条 Quad 命令(端点由调用方给);thickness 不钳制,斜线态用 x2 厚度;需要裁剪时由调用方套 ClipScope",
				ShellIds("line-segment"),
				StateList({ "default", "diagonal" }),
				{ PropFloat("thickness", 0.5f, 16.0f, 0.5f), PropText("color", "#E4C08A") },
				&ShowLineSegment));

			WuiComponentRegistry::Register(Desc(
				"collapsible", "CollapsibleHeader", "Section Header (Collapsible)", "Containers",
				WuiComponentStatus::Draft,
				"Engine/src/World/WUI/Widgets/WuiChrome.cpp",
				"role=button;id=HashId('showcase.collapsible')(控件自己登记;面板用 caller-provided 稳定 id,如 properties.section.<DisplayName> 的 HashId);label=title、value=open/closed、interactive=true、focused 跟随焦点;进焦点表(Tab 可达),Enter/Space = 切换",
				"showcase 首选 260x24;整行可点;展开/折叠只改底色与标记,自身矩形不变(布局高度由调用方按 open 算)",
				ShellIds("collapsible"),
				StateList({ "default", "collapsed", "hover", "focus" }),
				{ PropText("title", "Shader Parameters"), PropText("trailing", "12 items"), PropBool("open") },
				&ShowCollapsibleHeader));

			// ---- VEC-H2:属性面板的行语义(属性行 / 折叠分组头 / 集合行 / 集合动作按钮)----
			WuiComponentRegistry::Register(Desc(
				"property-row", "PropertyRow", "Property Row", "Properties", WuiComponentStatus::Draft,
				"Engine/src/World/WUI/WuiWidgets.cpp",
				"role=label/调用方声明的行 kind(交互字段的标签节点 A11yEnabled=false、只读值行走 kind=text);id=调用方的稳定行 id(面板:HashId('properties.<组件>.<字段>'));label=A11yLabel、value=A11yValue、tooltip=悬停说明/禁用理由(同源);行尾复位是子节点 kind=button(VEC-H7 起;此前 reset-default,调用方按 HashId(行 id 文本 + '.reset') 给 id,固定占位、两态同矩形,按钮自己登记 label/value/enabled/tooltip);VEC-H6:ResetModified=false(值 == 脚本默认)→ 行尾复位**不出现**(不画图标、不登记节点,占位槽照旧);VEC-H7:复位按钮必须在行悬停底色之后绘制,否则悬停时被底色盖掉",
				"行高 24(4px 栅格)、字段列高 20、行尾动作列 24;标签列宽 = min(140, 行宽×0.45)(PropertyRowLabelWidth;同一面板传同一值 ⇒ 竖向对齐);字段列 = 行宽 − 标签列 − 4 − 动作列;行矩形与动作落点不随状态变化;LabelIndent = 只挪标签文字(每层 12px + 1px 树导线),**值列不跟着挪**(缩进不破坏列对齐);FieldPlaceholder 非空时值列画占位文本(多值不同/值不可用的 \"—\"),此时调用方不画字段控件",
				A11yIds({ "showcase.property-row", "showcase.property-row.reset" }),
				StateList({ "default", "hover", "focus", "modified", "mixed", "disabled" }),
				{
					PropText("label", "Move Speed"),
					PropText("tooltip", "Movement speed in units per second (default 5)."),
					PropFloat("value", 0.0f, 20.0f, 0.1f),
					PropFloat("indent", 0.0f, 24.0f, 4.0f),
					PropBool("modified"),
					PropBool("mixed"),
					PropBool("reset"),
					PropBool("disabled"),
				},
				&ShowPropertyRow,
				{
					Item(WuiInteractionKind::Hover, "showcase.property-row", WuiInteractionExpect::PixelChange,
						"鼠标移到行中心(1 帧)→ 移开(1 帧)",
						"行底 = theme.HoverBg(悬停反馈);行矩形不变,字段控件不被挪动"),
					Item(WuiInteractionKind::Click, "showcase.property-row.reset", WuiInteractionExpect::Event,
						"点行尾 ↺ 图标(modified=true 时可用)",
						"一次复位事件;调用方据此把值写回默认(禁用态 Enabled=false 且带理由;ResetModified=false 时该节点整个不出现)"),
					Item(WuiInteractionKind::Key, "showcase.property-row.reset", WuiInteractionExpect::Event,
						"Tab 到 ↺ 出现焦点环 → Enter;再验一次 Space",
						"启用态进焦点表;禁用态不进(Tab 扫不到)"),
				}));

			WuiComponentRegistry::Register(Desc(
				"property-group-header", "PropertyGroupHeader", "Property Group Header", "Properties",
				WuiComponentStatus::Draft,
				"Engine/src/World/WUI/WuiWidgets.cpp",
				"role=button;id=调用方稳定 id(面板:HashId('properties.<组件>.<字段>') / 'properties.section.<DisplayName>');label=A11yLabel、value=open/closed、interactive=true、focused 跟随焦点;进焦点表(Tab 可达),Enter/Space = 切换;行尾动作(复位/删除)各是子节点(VEC-H7 起都是 kind=button),点动作不会折叠",
				"行高 24;展开 = theme.ActiveBg、折叠 = theme.PanelHeader、悬停 = theme.HoverBg(CollapsibleHeader 同语法);折叠命中区 = 整行减去动作列(或调用方传入的 ToggleWidth);Trailing 文本右对齐在动作列左侧;标签列 = 行宽 − 动作列 − Trailing(调用方不给 LabelWidth 时的默认;VEC-H5:分区头不受属性行 140 上限约束,长组件名 + 英文术语对照要放得下);展开标记 ▶/▼ 单独一笔画、固定推进 13px,不参与主名缩略(主名优先于英文术语降级,见 LabelWithTerm)",
				A11yIds({ "showcase.property-group-header", "showcase.property-group-header.reset" }),
				StateList({ "default", "collapsed", "hover", "focus", "modified", "disabled" }),
				{
					PropText("label", "Scores"),
					PropText("tooltip", "Per-level score entries (default 1.5, 2.5, 3.5)."),
					PropText("trailing", "3 items"),
					PropBool("open"),
					PropBool("reset"),
					PropBool("disabled"),
				},
				&ShowPropertyGroupHeader,
				{
					Item(WuiInteractionKind::Click, "showcase.property-group-header",
						WuiInteractionExpect::ValueChange,
						"点分组头左半(避开行尾动作)→ 再点一次",
						"节点 value 在 open / closed 之间来回;折叠只改底色与 -/+ 标记,自身矩形不变"),
					Item(WuiInteractionKind::Click, "showcase.property-group-header.reset",
						WuiInteractionExpect::Event,
						"点行尾 ↺(reset=true / modified 态)",
						"集合级复位(单击即复原;VEC-H6 起真实调用方不再弹二次确认;ResetModified=false 时该节点整个不出现)"),
					Item(WuiInteractionKind::Key, "showcase.property-group-header", WuiInteractionExpect::ValueChange,
						"Tab 到分组头 → Enter;再验一次 Space",
						"键盘与点击同语义(焦点环 + 值迁移)"),
				}));

			WuiComponentRegistry::Register(Desc(
				"collection-row", "CollectionRow", "Collection Row", "Properties", WuiComponentStatus::Draft,
				"Engine/src/World/WUI/WuiWidgets.cpp",
				"role=label/调用方声明的行 kind;id=调用方稳定行 id(面板:HashId('properties.<组件>.<字段>.<下标|键>'));label=元素下标/键、value=A11yValue、tooltip=悬停说明;行尾 `-` 是子节点 kind=button(id=调用方的 ...remove.<行名>),`+` 同理(...add);ActionsOutside=true 时动作落在行矩形右侧预留槽(面板集合元素的既有几何,槽宽 = CollectionActionColumnWidth())",
				"行高 24;值列 = 行宽 − 标签列 − 4 − 行内动作列;Indent 只挪本行内容、不改行矩形;动作按钮 24×24(与行同高、居中)",
				A11yIds({ "showcase.collection-row", "showcase.collection-row.reset",
					"showcase.collection-row.remove" }),
				StateList({ "default", "hover", "focus", "add", "disabled" }),
				{
					PropText("label", "1"),
					PropText("tooltip", "Element 1 of Scores (default 1.5)."),
					PropFloat("value", 0.0f, 20.0f, 0.1f),
					PropBool("reset"),
					PropBool("remove"),
					PropBool("add"),
					PropBool("disabled"),
				},
				&ShowCollectionRow,
				{
					Item(WuiInteractionKind::Click, "showcase.collection-row.reset", WuiInteractionExpect::Event,
						"点元素行尾 `↺`(reset=true / modified 态)",
						"单项复位(只清这一项;调用方按脚本声明回填默认值)"),
					Item(WuiInteractionKind::Click, "showcase.collection-row.remove", WuiInteractionExpect::Event,
						"点元素行尾 `-`",
						"一次删除事件;真实调用方在画完所有元素行之后统一 erase(避免同帧错位)"),
					Item(WuiInteractionKind::Click, "showcase.collection-row.field",
						WuiInteractionExpect::ValueChange, "在值列里横向拖动数值(或点进去键入)",
						"值列矩形由库件给出(FieldRect),字段控件仍走它自己的输入路径"),
				}));

			WuiComponentRegistry::Register(Desc(
				"collection-action-button", "CollectionActionButton", "Collection Action Button", "Buttons",
				WuiComponentStatus::Draft,
				"Engine/src/World/WUI/WuiWidgets.cpp",
				"role=button;id=调用方稳定 id(面板:HashId('...remove.<行名>') / '...add');label=字形(- / + / x)、value=tooltip、tooltip=用途或禁用理由(disabled 时 Enabled=false、Interactive=false,悬停仍给理由);启用态进焦点表,Enter/Space = 激活",
				"首选 24×24(4px 栅格;与行同高,行内垂直居中);danger=true 时悬停用 theme.Danger 描边 + 22% 底(破坏性动作的既定语法)",
				A11yIds({ "showcase.collection-action-button" }),
				StateList({ "default", "hover", "focus", "disabled" }),
				{
					PropText("glyph", "-"),
					PropText("tooltip", "Remove this element from the collection"),
					PropBool("danger"),
					PropBool("disabled"),
				},
				&ShowCollectionActionButton,
				{
					Item(WuiInteractionKind::Click, "showcase.collection-action-button",
						WuiInteractionExpect::Event, "点按钮中心", "一次激活事件(调用方执行删除/追加)"),
					Item(WuiInteractionKind::Key, "showcase.collection-action-button",
						WuiInteractionExpect::Event, "Tab 到按钮 → Enter;再验一次 Space",
						"键盘与点击同语义"),
				}));

			WuiComponentRegistry::Register(Desc(
				"button.disabled", "ButtonEx", "Button (Disabled + Reason)", "Buttons", WuiComponentStatus::Draft,
				"Engine/src/World/WUI/WuiWidgets.cpp",
				"role=button;id=HashId('showcase.button.disabled')(控件自己登记);label=label 属性;enabled=false 时 Enabled=false 且 Value/Tooltip=理由(灰按钮不能没有理由),Interactive 仍为 true;焦点环走 DrawFocusRing,Enter/Space = 激活",
				"showcase 首选 148x24(高 = theme.ControlHeight);禁用态同尺寸弱化绘制,不改矩形;primary=true 时 Accent 填充",
				ShellIds("button.disabled"),
				StateList({ "default", "hover", "focus", "disabled", "primary", "long-text" }),
				{ PropText("label", "Assign"), PropText("reason", "Select a material instance first"),
					PropBool("disabled"), PropBool("primary") },
				&ShowButtonEx));

			WuiComponentRegistry::Register(Desc(
				"scrollbar", "ScrollBar", "Scroll Bar", "Containers", WuiComponentStatus::Draft,
				"Engine/src/World/WUI/WuiWidgets.cpp",
				"role=scrollbar;id=HashId('showcase.scrollbar')(控件自己登记);value='scroll=<y>/<max> ratio=<0..1>'(前缀与 BeginScrollArea 的 scroll=<y>/<max> 同口径);还有滚动量时 Enabled/Interactive=true 并进焦点表,内容装得下时 Enabled/Interactive=false",
				"showcase 首选 14x168(rect.H >= 72 才画上下翻页按钮,按钮高 24);内容高/视口高由调用方给,控件不改布局",
				ShellIds("scrollbar"),
				StateList({ "default", "hover", "focus", "top", "bottom", "disabled" }),
				{ PropFloat("scroll", 0.0f, 400.0f, 1.0f) },
				&ShowScrollBar));
		}

}
}
