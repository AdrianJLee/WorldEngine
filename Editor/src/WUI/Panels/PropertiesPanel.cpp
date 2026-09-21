#include "wldpch.h"
#include "PropertiesPanel.h"

// P2 W5b:Reload 按钮要复用 EditorLayer 的热重载入口(与帧边界轮询、AI 通道 script.reload
// 同一条语义)。PanelHost 是跨任务冻结的窄接口,本包文件边界内不能扩展它,因此只 include。
#include "../../EditorLayer.h"

#include "World/Core/KeyCodes.h"
#include "World/WUI/WuiAccessibility.h"
#include "World/WUI/WuiJson.h"
#include "World/WUI/WuiLocalization.h"
#include "World/WUI/WuiWidgets.h"
#include "World/WUI/Widgets/WuiModal.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <iterator>
#include <map>

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
			const std::string& value, bool enabled = true, const std::string& tooltip = std::string(),
			bool focused = false)
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
			// P4-UX7:只能靠悬停看到的信息(用途/说明)必须同时进节点,脚本与读屏才拿得到。
			node.Tooltip = tooltip;
			node.Rect = rect;
			node.Enabled = enabled;
			// 不可用的控件不可被 ui.invoke 点击(与真实鼠标路径一致)。
			node.Interactive = enabled;
			node.Focused = focused;
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
					return Wui::Tr("panel.properties.value_none", "(none)");
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
					return Wui::Tr("panel.properties.value_complex", "(...)");   // 对象/矩阵/四元数等:只读态给出占位,避免误导
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

		// ---- schema 显示点本地化 ----
		// DisplayName / 字段名是生成文件里的**稳定标识**:属性行 id(properties.<Type>.<Field>)、
		// 分区折叠键(prop.open.<DisplayName>)、控件 hash、Lua 代理名都由它派生,所以这里
		// 只翻译**显示文案**,一律不回写 schema,也不改任何 id / 持久化 key / hash。
		// 目录键用短类型名:World::TransformComponent → schema.component.TransformComponent。
		std::string SchemaTypeKeyName(const Schema::TypeSchema& schema)
		{
			const std::string& name = schema.Id.Name;
			const size_t separator = name.rfind("::");
			return separator == std::string::npos ? name : name.substr(separator + 2);
		}

		// C++ 标识符 → 人类可读英文(仅在显示点;不改 schema):
		//  - 驼峰/下划线分词:TilingFactor → "Tiling Factor"、ShowCollider → "Show Collider";
		//  - 全大写缩写整体保留:ID → "ID"、PersistenceID → "Persistence ID";
		//  - 缩写后接单词:"FOVValue" → "FOV Value"(带 islower 前瞻,不拆开缩写);
		//  - 成员前缀 m_/s_/g_(生成物里 SceneCamera 用 m_ 前缀)不作为字段语义:
		//    m_PerspectiveFOV → "Perspective FOV"。
		std::string HumanizeIdentifier(const std::string& name)
		{
			std::string text = name;
			if (text.size() > 2 && text[1] == '_'
				&& (text[0] == 'm' || text[0] == 's' || text[0] == 'g')
				&& std::isupper(static_cast<unsigned char>(text[2])))
				text.erase(0, 2);

			std::string out;
			out.reserve(text.size() + 4);
			bool previousUpper = false;
			for (size_t i = 0; i < text.size(); ++i)
			{
				const char c = text[i];
				if (c == '_' || c == '-' || c == ' ')
				{
					if (!out.empty() && out.back() != ' ')
						out.push_back(' ');
					previousUpper = false;
					continue;
				}
				const bool upper = std::isupper(static_cast<unsigned char>(c)) != 0;
				const char prev = out.empty() ? '\0' : out.back();
				const bool prevLowerOrDigit = prev != '\0' && prev != ' '
					&& (std::islower(static_cast<unsigned char>(prev)) || std::isdigit(static_cast<unsigned char>(prev)));
				if (upper && prevLowerOrDigit)
					out.push_back(' ');
				else if (upper && previousUpper && i + 1 < text.size()
					&& std::islower(static_cast<unsigned char>(text[i + 1])))
					out.push_back(' ');
				out.push_back(c);
				previousUpper = upper;
			}
			while (!out.empty() && out.back() == ' ')
				out.pop_back();
			return out;
		}

		// 术语对照:中文界面下把英文术语以 Caption/次要色画在主文案之后(方案 §7.8);
		// 无障碍节点 label 一律写成 "中文 (English)",id / 控件 hash / PropPath 不受影响。
		std::string TermText(const Wui::LocalizedLabel& label)
		{
			return label.Term.empty() ? label.Text : label.Text + " (" + label.Term + ")";
		}

		// 组件分区标题 / "Add Component" 菜单项的显示文案(键用短类型名,标识仍用 schema->DisplayName)。
		Wui::LocalizedLabel SchemaComponentLabel(const Schema::TypeSchema& schema)
		{
			return Wui::TrLabel("schema.component." + SchemaTypeKeyName(schema), schema.DisplayName);
		}

		// 字段标签显示文案:Meta.DisplayName 优先,空则人类可读化 C++ 字段名;
		// 键分别用 DisplayName / 字段名(两者都是生成物里的稳定标识)。
		Wui::LocalizedLabel SchemaFieldLabel(const Schema::FieldSchema& field)
		{
			if (!field.Meta.DisplayName.empty())
				return Wui::TrLabel("schema.field." + field.Meta.DisplayName, field.Meta.DisplayName);
			return Wui::TrLabel("schema.field." + field.Name, HumanizeIdentifier(field.Name));
		}

		// 自定义检查器按字段名取 schema 字段,与通用路径共用同一显示文案(找不到字段时仍可读)。
		Wui::LocalizedLabel SchemaFieldLabel(const Schema::TypeSchema& schema, const std::string& fieldName)
		{
			for (const Schema::FieldSchema& field : schema.Fields)
				if (field.Name == fieldName)
					return SchemaFieldLabel(field);
			return Wui::TrLabel("schema.field." + fieldName, HumanizeIdentifier(fieldName));
		}

		// 自定义检查器的枚举 → 显示文案(下拉选项与只读值共用):键 panel.properties.camera.<name>;
		// 写回相机的仍是 schema 枚举 raw 值 —— 枚举名只用来查显示文案,不参与任何比较。
		std::string CameraProjectionLabel(const std::string& enumName)
		{
			if (enumName == "Perspective")
				return Wui::Tr("panel.properties.camera.perspective", "Perspective");
			if (enumName == "Orthographic")
				return Wui::Tr("panel.properties.camera.orthographic", "Orthographic");
			return enumName;   // 未知枚举名原样显示,不猜翻译
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
			const Wui::LocalizedLabel& label, glm::vec3& value, const Wui::WuiTheme& theme, bool reachable)
		{
			// 术语对照的文本预算 = 本行真实标签列宽(控件列起点 - 标签起点),窄处自动省略。
			const float labelBudget = std::min(140.0f, row.W * 0.45f) - 4.0f;
			const std::string labelText = TermText(label);
			Wui::LabelWithTerm(ctx, { row.X + 4, row.Y + 3 }, label.Text, label.Term, theme.TextMuted, 13.0f, theme, labelBudget);
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
					labelText + "." + kShort[c], FormatFloatText(component), reachable);
			}
			return changed;
		}

		// 浮点行(带范围;无范围时用 1/-1 哨兵,与 schema 字段路径一致)。
		bool DrawFloatRow(Wui::WuiContext& ctx, const std::string& idText, const Wui::WuiRect& row,
			const Wui::LocalizedLabel& label, float& value, float lo, float hi, const Wui::WuiTheme& theme, bool reachable)
		{
			const float labelBudget = std::min(140.0f, row.W * 0.45f) - 4.0f;
			Wui::LabelWithTerm(ctx, { row.X + 4, row.Y + 3 }, label.Text, label.Term, theme.TextMuted, 13.0f, theme, labelBudget);
			const Wui::WuiRect ctrl { row.X + std::min(140.0f, row.W * 0.45f), row.Y + 1,
				row.W - std::min(140.0f, row.W * 0.45f) - 4, 20 };
			const float before = value;
			Wui::DragFloat(ctx, Wui::HashId(idText.c_str()), ctrl, value, 0.01f, lo, hi, theme);
			RegisterNode(Wui::HashId(idText.c_str()), "drag-float", ctrl, TermText(label), FormatFloatText(value), reachable);
			return value != before;
		}

		// 只读行(登记 properties.<...> 文本节点,enabled=false,不可点击)。
		void DrawReadOnlyRow(Wui::WuiContext& ctx, const std::string& idText, const Wui::WuiRect& row,
			const Wui::LocalizedLabel& label, const std::string& value, const Wui::WuiTheme& theme)
		{
			const float labelBudget = std::min(140.0f, row.W * 0.45f) - 4.0f;
			Wui::LabelWithTerm(ctx, { row.X + 4, row.Y + 3 }, label.Text, label.Term, theme.TextMuted, 13.0f, theme, labelBudget);
			Label(ctx, { row.X + std::min(140.0f, row.W * 0.45f), row.Y + 3 }, value, theme.Text, 13.0f);
			RegisterNode(Wui::HashId(idText.c_str()), "text", row, TermText(label), value, false);
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

		// 状态名显示文案(仅用于显示):拼接结果不参与任何比较/存储,id/hash 也不用状态文本。
		std::string ScriptStateLabel(ScriptInstanceState state)
		{
			const std::string name = ScriptStateName(state);
			if (name == "?")
				return name;   // 未知状态保持原占位符
			return Wui::Tr("panel.properties.script_state." + name, name);
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

		// ---- U6:Add Component 选择器的布局常量与行模型(方案 §8.2;交互冻结)----
		// 组件 22+ 之后,220px 的无搜索平铺菜单只能瞪眼扫。选择器:宽 320、最大高 420、
		// 内容富余时自适应高度、超出时内部滚动。
		constexpr float kPickerWidth = 320.0f;
		constexpr float kPickerMaxHeight = 420.0f;
		constexpr float kPickerSearchRow = 30.0f;   // 搜索框行(含内边距)
		constexpr float kPickerGroupHeader = 20.0f; // 分组标题行
		constexpr float kPickerRow = 40.0f;         // 候选项:名称行 + 名下 Doc 行
		constexpr float kPickerPad = 4.0f;
		constexpr size_t kPickerRecentMax = 5;
		constexpr int kPickerRevealFrames = 5;

		// 选择器里的一行:分组标题(kind="text")或候选项(kind="menu-item")。
		struct PickerRow
		{
			bool Header = false;
			std::string Text;      // 标题 / 行主文案(本地化)
			std::string Term;      // 行的英文术语(中文界面下的对照)
			std::string Doc;       // 一句话说明(默认英文 = schema->Doc,中文走目录)
			std::string Category;  // 分类(本地化;空分类 = "未分类"文案)—— 也是节点的 value
			std::string NodeId;    // 稳定无障碍 id(标题 = prop.add.group.*,行 = prop.add.<DisplayName>)
			const Schema::TypeSchema* Schema = nullptr;
		};

		// ASCII 不分大小写的子串匹配(非 ASCII 字节原样比较:中文没有大小写)。
		std::string LowerAscii(std::string text)
		{
			std::transform(text.begin(), text.end(), text.begin(),
				[](unsigned char c) { return static_cast<char>(std::tolower(c)); });
			return text;
		}

		bool ContainsInsensitive(const std::string& haystack, const std::string& needleLower)
		{
			if (needleLower.empty())
				return true;
			return LowerAscii(haystack).find(needleLower) != std::string::npos;
		}

		// 分类目录键 = schema.category.<路径,把 '/' 换成 '.'>(与 schema.component.* 同一口径);
		// 英文默认 = Category 原文(默认语言不查表)。
		std::string CategoryKeyName(const std::string& category)
		{
			std::string key = category;
			for (char& character : key)
				if (character == '/')
					character = '.';
			return key;
		}

		std::string CategoryLabel(const std::string& category)
		{
			if (category.empty())
				return Wui::Tr("panel.properties.add.uncategorized", "Uncategorized");
			return Wui::Tr("schema.category." + CategoryKeyName(category), category);
		}

		// 一句话说明:英文默认 = schema->Doc 原文;中文目录用 schema.component.<短名>.doc 覆盖。
		std::string ComponentDocLabel(const Schema::TypeSchema& schema)
		{
			if (schema.Doc.empty())
				return std::string();
			return Wui::Tr("schema.component." + SchemaTypeKeyName(schema) + ".doc", schema.Doc);
		}

		// 搜索命中口径(方案 §8.2):本地化名 / DisplayName / 短类型名 / 字段名,子串匹配。
		bool MatchesComponentFilter(const Schema::TypeSchema& schema, const std::string& needleLower)
		{
			if (needleLower.empty())
				return true;
			if (ContainsInsensitive(SchemaComponentLabel(schema).Text, needleLower))
				return true;
			if (ContainsInsensitive(schema.DisplayName, needleLower))
				return true;
			if (ContainsInsensitive(SchemaTypeKeyName(schema), needleLower))
				return true;
			for (const Schema::FieldSchema& field : schema.Fields)
			{
				if (ContainsInsensitive(field.Name, needleLower))
					return true;
				if (!field.Meta.DisplayName.empty() && ContainsInsensitive(field.Meta.DisplayName, needleLower))
					return true;
			}
			return false;
		}
	}

	PropertiesPanel::PropertiesPanel(PanelHost& host)
		: m_Host(host), m_StatePath(std::string(WLD_EDITOR_DIR) + "wui-properties.json")
	{
		// 选择器 MRU 与其它面板状态同口径(wui-layout.json / wui-browser.json):读失败/文件不
		// 存在都只是"没有最近使用",不影响面板可用性。
		LoadState();
	}

	void PropertiesPanel::LoadState()
	{
		try
		{
			std::error_code existsError;
			if (!std::filesystem::exists(m_StatePath, existsError))
				return;
			std::ifstream stream(m_StatePath, std::ios::binary);
			if (!stream)
				return;
			const std::string text((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
			std::string error;
			const auto parsed = Wui::JsonValue::Parse(text, &error);
			if (!parsed)
				return;
			if (const Wui::JsonValue* value = parsed->Find("recentComponents"))
			{
				m_RecentComponents.clear();
				for (const Wui::JsonValue& item : value->Array)
				{
					const std::string name = item.AsString("");
					if (name.empty())
						continue;
					m_RecentComponents.push_back(name);
					if (m_RecentComponents.size() >= kPickerRecentMax)
						break;
				}
			}
		}
		catch (const std::exception& error)
		{
			WLD_CORE_WARN("Failed to load properties panel state: {0}", error.what());
		}
	}

	void PropertiesPanel::SaveState() const
	{
		try
		{
			// 目录可能不存在(全新 checkout / 打包产物):先补目录;任何写入失败都只告警 ——
			// "最近使用"丢了可以接受,不能让编辑动作崩。
			const std::filesystem::path path(m_StatePath);
			std::error_code dirError;
			if (!path.parent_path().empty())
				std::filesystem::create_directories(path.parent_path(), dirError);
			Wui::JsonValue root;
			root.type = Wui::JsonValue::Type::Object;
			Wui::JsonValue recent;
			recent.type = Wui::JsonValue::Type::Array;
			for (const std::string& name : m_RecentComponents)
				recent.Array.push_back(Wui::JsonValue::MakeString(name));
			root.Object.push_back({ "recentComponents", std::move(recent) });
			std::ofstream stream(m_StatePath, std::ios::binary | std::ios::trunc);
			if (stream)
				stream << root.Dump();
		}
		catch (const std::exception& error)
		{
			WLD_CORE_WARN("Failed to save properties panel state: {0}", error.what());
		}
	}

	void PropertiesPanel::TouchRecent(const std::string& shortName)
	{
		if (shortName.empty())
			return;
		m_RecentComponents.erase(std::remove(m_RecentComponents.begin(), m_RecentComponents.end(), shortName),
			m_RecentComponents.end());
		m_RecentComponents.insert(m_RecentComponents.begin(), shortName);
		if (m_RecentComponents.size() > kPickerRecentMax)
			m_RecentComponents.resize(kPickerRecentMax);
		SaveState();
	}

	void PropertiesPanel::OpenAddComponentPicker(Wui::WuiContext& ctx)
	{
		// 每次都从干净状态开始:搜索清空、无高亮、键盘在搜索框一侧(下次打开搜索词清空)。
		m_AddSearch.clear();
		m_AddSearchLast.clear();
		m_AddHighlight = -1;
		m_AddListFocus = false;
		m_AddScroll = 0.0f;
		m_AddCategoryAll = true;
		m_AddCategoryFilter.clear();
		m_AddOpenedFrame = ctx.Frame();
		m_AddOpen = true;
		const Wui::WuiId modalId = Wui::HashId("prop.add.modal");
		ctx.SetModal(modalId);
		// 面板级模态:宿主帧初封锁整窗输入,渲染本面板前解开(见 EditorShell::RenderTabs)。
		m_Host.SetPanelModalOwner(Id());
		ctx.SetFocus(Wui::HashId("prop.add.search"));
		ctx.RecordOp("properties", "open-add-picker", "prop.add", "focus=search");
	}

	void PropertiesPanel::CloseAddComponentPicker(Wui::WuiContext& ctx)
	{
		m_AddOpen = false;
		ctx.ClearModal();
		m_Host.SetPanelModalOwner(std::string());
	}

	void PropertiesPanel::DrawAddComponentPicker(Wui::WuiContext& ctx,
		Entity entity, Scene* scene, Schema::SchemaRegistry& schemas)
	{
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
			ctx.Commands().push_back({ Wui::WuiDrawKind::Rect, sidebarRect, theme.ContentBg, 4.0f });
			ctx.Commands().push_back({ Wui::WuiDrawKind::ClipPush, sidebarRect });
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
					ctx.Commands().push_back({ Wui::WuiDrawKind::Rect, item, theme.ActiveBg, 3.0f });
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
			ctx.Commands().push_back({ Wui::WuiDrawKind::ClipPop });
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
			// MRU 置顶并落盘(<Editor>/wui-properties.json)。
			TouchRecent(SchemaTypeKeyName(schema));
			// 新分区自动展开(与分区绘制读同一个持久化键),再由 OnRender 连续几帧滚到可见。
			const bool defaultOpen = schema.Id.Name == "World::LuaScriptComponent";
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
				ctx.Commands().push_back({ Wui::WuiDrawKind::Rect, item,
					highlighted ? theme.ActiveBg : theme.ButtonHover, 2.0f });
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
			ctx.Commands().push_back({ Wui::WuiDrawKind::Rect, scrollTrack, theme.PanelHeader, 3.0f });
			ctx.Commands().push_back({ Wui::WuiDrawKind::Rect, thumb,
				(m_AddThumbDragging || ctx.IsHovered(thumb)) ? theme.Accent : theme.ButtonHover, 3.0f });
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

	void PropertiesPanel::OnRender(Wui::WuiContext& ctx, const Wui::WuiRect& rect, PanelHost& host)
	{
		const Wui::WuiTheme& theme = host.Theme();
		// Play/Simulate = 只读查看:字段只显示不写回,并给出提示(用户确认的语义)。
		m_ReadOnly = host.IsReadOnlyMode();
		Entity entity = host.GetSelectedEntity();
		if (!entity.IsValid() || entity.GetScene() != host.GetActiveScene().get())
		{
			Label(ctx, { rect.X + 8, rect.Y + 8 },
				Wui::Tr("panel.properties.no_entity_selected", "No entity selected"), theme.TextMuted, 14.0f);
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
			// 用户可见文案走本地化表:内联英文 = 默认语言,zh-CN 目录提供中文覆盖。
			Label(ctx, { rect.X + 8, rect.Y + 8 },
				Wui::Tr("panel.properties.readonly_notice",
					"Play/Simulate running: read-only (pause or exit to edit)"),
				theme.TextMuted, 13.0f);
		}

		const Wui::WuiRect addButton { rect.X + 8, rect.Y + (m_ReadOnly ? 30.0f : 8.0f), 140, 24 };
		if (!m_ReadOnly && Button(ctx, Wui::HashId("prop.add"), addButton,
			Wui::Tr("panel.properties.add_component", "Add Component"), theme))
			OpenAddComponentPicker(ctx);
		// U6b:平铺菜单 → **居中模态**的搜索选择器(候选/分类/说明全部来自 schema)。
		if (!m_ReadOnly && m_AddOpen)
			DrawAddComponentPicker(ctx, entity, scene, schemas);

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
		// U6:添加组件后要滚到可见的分区顶部(标题行位置)。
		float revealTargetY = -1.0f;
		float sectionTop = 0.0f;
		for (size_t i = 0; i < m_Sections.size(); ++i)
		{
			if (i >= componentSchemas.size())
				break;
			const Schema::TypeSchema* schema = componentSchemas[i];
			const bool defaultOpen = schema->Id.Name == "World::LuaScriptComponent";
			bool& open = ctx.Persist<bool>(Wui::HashId(("prop.open." + schema->DisplayName).c_str()), defaultOpen);
			m_Sections[i].Open = open;
			if (!m_RevealSection.empty() && m_Sections[i].Title == m_RevealSection)
				revealTargetY = sectionTop;
			contentHeight += kSectionHeader + (open ? m_Sections[i].ContentHeight : 0.0f) + kSectionGap;
			sectionTop += kSectionHeader + (open ? m_Sections[i].ContentHeight : 0.0f) + kSectionGap;
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

		// ---- U6:添加组件后把新分区滚到可见 ----
		// 分区高度是"上一帧实测值"(添加那一帧才第一次测量,且新增分区会把布局重置为 0),
		// 所以连续几帧重算偏移:布局稳定后自然停在正确位置,不需要动画/定时器。
		if (!m_RevealSection.empty())
		{
			if (revealTargetY >= 0.0f)
				scroll.Offset = std::clamp(revealTargetY, 0.0f, maxScroll);
			if (revealTargetY < 0.0f || --m_RevealFrames <= 0)
			{
				m_RevealSection.clear();
				m_RevealFrames = 0;
			}
		}

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
			// 标题 = 主文案(中文界面为译文)+ 英文术语(Caption/次要色,窄处自动省略);前缀仍是展开标记。
			const Wui::LocalizedLabel sectionLabel = SchemaComponentLabel(*schema);
			Wui::LabelWithTerm(ctx, { header.X + 6, header.Y + 3 }, (open ? "- " : "+ ") + sectionLabel.Text,
				sectionLabel.Term, theme.Text, 14.0f, theme, header.W - 12.0f);
			// 无障碍 id / 操作记录仍用 section.Title(=<DisplayName>),节点 id 逐字节不变。
			const std::string headerId = "properties.section." + section.Title;
			RegisterNode(Wui::HashId(headerId.c_str()), "button", header, TermText(sectionLabel), open ? "open" : "closed");
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

		// ---- U6:Ctrl+Shift+A 打开组件选择器(面板级快捷键)----
		// 判定必须放在 OnRender **末尾**:文本焦点是在本帧绘制文本控件时才登记的,提前读拿到的是
		// 上一帧的旧状态(内容浏览器在这个坑上踩过两次:Ctrl+A 把输入框里的全选变成了全选文件)。
		if (!m_ReadOnly && !Wui::WuiTextFocus::Get().Active() && ctx.Input().Ctrl && ctx.Input().Shift
			&& ctx.IsHovered(rect) && ctx.WasKeyTriggered(KeyCodes::A))
			OpenAddComponentPicker(ctx);
	}

	float PropertiesPanel::DrawSchemaFields(Wui::WuiContext& ctx, Wui::WuiId base, const Wui::WuiRect& rect,
		void* instance, const std::string& typeName, const Schema::TypeSchema& schema,
		const Wui::WuiRect& visibleRect)
	{
		const Wui::WuiTheme& theme = m_Host.Theme();
		float y = 0;
		bool changed = false;
		const float labelWidth = std::min(140.0f, rect.W * 0.45f);
		// 术语对照的文本预算:标签从 +4 起画,到控件列起点为止(窄处自动省略,不压控件)。
		const float labelBudget = labelWidth - 4.0f;
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
			// 显示文案:Meta.DisplayName 优先,空则人类可读化 C++ 字段名后查目录;
			// 行 id(fid / propId)与持久化仍用 field.Name,不受影响。
			const Wui::LocalizedLabel label = SchemaFieldLabel(field);
			// 无障碍节点 label 按约定写成 "中文 (English)";显示值/句子本身不加英文。
			const std::string labelText = TermText(label);

			if (field.K == Schema::Kind::Object)
			{
				const std::string idText = propId(typeName, field.Name);
				const Schema::TypeSchema* nested = field.GetNested ? field.GetNested() : nullptr;
				void* nestedInstance = field.GetPtr ? field.GetPtr(instance) : nullptr;
				// UUID 等身份标识只读展示,不提供编辑控件。
				if (nested && nestedInstance && (nested->Id.Name == "World::UUID" || nested->DisplayName == "UUID"))
				{
					std::string display = Wui::Tr("panel.properties.invalid_value", "(invalid)");
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
					Wui::LabelWithTerm(ctx, { row.X + 4, row.Y + 3 }, label.Text, label.Term, theme.TextMuted, 13.0f,
						theme, labelBudget);
					Label(ctx, { ctrl.X, row.Y + 3 }, display, theme.Text, 13.0f);
					RegisterNode(Wui::HashId(idText.c_str()), "text", row, labelText, display, false);
					y += 20;
					continue;
				}
				bool& open = ctx.Persist<bool>(fid, false);
				if (ctx.IsClicked(row))
					open = !open;
				Wui::LabelWithTerm(ctx, { row.X + 4, row.Y + 3 }, (open ? "- " : "+ ") + label.Text, label.Term,
					theme.Text, 13.0f, theme, labelBudget);
				RegisterNode(Wui::HashId(idText.c_str()), "button", row, labelText, open ? "open" : "closed", reachable(row));
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
				std::string text = label.Text;
				if (field.Get)
					text += ": " + FormatReadOnlyValue(field, field.Get(instance));
				// 值行整行可用;术语作为行尾 Caption 对照(句子/值本身不加英文)。
				Wui::LabelWithTerm(ctx, { row.X + 4, row.Y + 3 }, text, label.Term, theme.TextMuted, 13.0f, theme,
					row.W - 8.0f);
				// 只读字段登记为不可交互文本节点:ui.tree 能断言"可见但禁用"。
				RegisterNode(Wui::HashId(idText.c_str()), "text", row,
					labelText, field.Get ? FormatReadOnlyValue(field, field.Get(instance)) : std::string(),
					false);
				y += 20;
				continue;
			}

			// 交互字段:标签登记为静态节点(不可点),控件本体按真实 kind 登记
			// (脚本用 properties.<Type>.<Field> 直接 ui.invoke)。
			const std::string idText = propId(typeName, field.Name);
			RegisterNode(Wui::HashId(idText.c_str()), "label", row, labelText, std::string(), false);
			Wui::LabelWithTerm(ctx, { row.X + 4, row.Y + 3 }, label.Text, label.Term, theme.TextMuted, 13.0f,
				theme, labelBudget);
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
					RegisterNode(Wui::HashId(idText.c_str()), "checkbox", ctrl, labelText, b ? "true" : "false", reachable(ctrl));
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
					RegisterNode(Wui::HashId(idText.c_str()), "drag-int", ctrl, labelText, std::to_string(raw), reachable(ctrl));
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
					RegisterNode(Wui::HashId(idText.c_str()), "drag-int", ctrl, labelText, std::to_string(signedRaw), reachable(ctrl));
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
					RegisterNode(Wui::HashId(idText.c_str()), "drag-float", ctrl, labelText, FormatFloatText(f), reachable(ctrl));
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
							labelText + "." + kShort[c], FormatFloatText(f), reachable(slotRect));
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
					RegisterNode(Wui::HashId(idText.c_str()), "text-field", ctrl, labelText, current, reachable(ctrl));
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
						RegisterNode(Wui::HashId(idText.c_str()), "combo", ctrl, labelText,
							(selected >= 0 && selected < static_cast<int>(names.size())) ? names[selected] : std::string(),
							reachable(ctrl));
					}
					break;
				}
				default:
					Label(ctx, { ctrl.X, ctrl.Y + 3 },
						Wui::Tr("panel.properties.unsupported", "(unsupported)"), theme.TextMuted, 12.0f);
					RegisterNode(Wui::HashId(idText.c_str()), "text", row, labelText,
						Wui::Tr("panel.properties.unsupported", "(unsupported)"), false);
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
			std::vector<std::string> names;    // 原始脚本名:匹配与写回都用它(稳定标识)
			std::vector<std::string> labels;   // 下拉显示文案(可本地化)
			// 下拉 label 只进无障碍节点(Combo 只画当前值):按术语约定写成 "脚本 (Script)"。
			const Wui::LocalizedLabel scriptFieldLabel = Wui::TrLabel("panel.properties.native_script", "Script");
			std::vector<const Schema::TypeSchema*> scripts = schemas.List(Schema::TypeCategory::Script);
			int selected = -1;
			for (size_t i = 0; i < scripts.size(); ++i)
			{
				names.push_back(scripts[i]->DisplayName);
				labels.push_back(Wui::Tr("schema.script." + scripts[i]->DisplayName, scripts[i]->DisplayName));
				if (scripts[i]->DisplayName == script->ScriptName)
					selected = static_cast<int>(i);
			}
			if (Combo(ctx, base ^ 1u, { rect.X, rect.Y, rect.W, 22 }, TermText(scriptFieldLabel), labels, selected, theme)
				&& selected >= 0)
			{
				if (scripts[selected]->Script)
					scripts[selected]->Script->Bind(static_cast<void*>(script));
				// 写回原字符串而不是显示文案:ScriptName 必须与 schema 名逐字一致。
				script->ScriptName = names[selected];
				script->ResetEditorFieldState();
				m_Host.MarkDocumentDirty();
			}
			float y = 26;
			Label(ctx, { rect.X, rect.Y + y },
				Wui::Tr("panel.properties.script_state", "state") + ": " + ScriptStateLabel(script->State),
				theme.TextMuted, 13.0f);
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
			// 状态行只走 Tr(状态词,不加英文术语);状态名/前后缀都不参与比较或存储。
			std::string state = Wui::Tr("panel.properties.script_state", "state") + ": " + ScriptStateLabel(script->State);
			state += " " + (script->IsLoaded
				? Wui::Tr("panel.properties.script_loaded", "(loaded)")
				: Wui::Tr("panel.properties.script_not_loaded", "(not loaded)"));
			Label(ctx, { rect.X, rect.Y + y }, state, theme.TextMuted, 13.0f);
			y += 18;
			if (!script->ReloadDiagnostic.empty())
			{
				Label(ctx, { rect.X, rect.Y + y },
					Wui::Tr("panel.properties.reload_prefix", "[reload]") + " " + TruncateForPanel(script->ReloadDiagnostic),
					{ 1.0f, 0.75f, 0.3f, 1 }, 12.0f);
				y += 16;
			}
			if (!script->LastError.empty())
			{
				Label(ctx, { rect.X, rect.Y + y },
					Wui::Tr("panel.properties.error_prefix", "[error]") + " " + TruncateForPanel(script->LastError),
					{ 1, 0.4f, 0.4f, 1 }, 12.0f);
				y += 16;
			}
			if (Button(ctx, Wui::HashId("lua.reload"), { rect.X, rect.Y + y, std::min(rect.W, 160.0f), 22 },
				Wui::Tr("panel.properties.reload_script", "Reload Script"), theme))
			{
				std::string message;
				const bool ok = EditorLayer::ReloadLuaScriptComponent(*script, entity.GetScene(), &message);
				m_LuaReloadOk = ok;
				m_LuaReloadMessage = ok ? message : (Wui::Tr("panel.properties.reload_failed", "failed:") + " " + message);
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
		// 自定义检查器也走同一份字段标签(schema 字段名 → 显示文案),id 仍是 PropPath。
		const Wui::LocalizedLabel locationLabel = SchemaFieldLabel(schema, "Location");
		const Wui::LocalizedLabel rotationLabel = SchemaFieldLabel(schema, "Rotation");
		const Wui::LocalizedLabel scaleLabel = SchemaFieldLabel(schema, "Scale");

		if (!writable)
		{
			// Play/Simulate:只读展示(与通用只读字段同一契约:properties.<...> 文本节点)。
			DrawReadOnlyRow(ctx, PropPath(typeName, "Location"), ComponentRect(rect, 0, 20),
				locationLabel, FormatFloatText(transform.Location.x, 2) + ", "
					+ FormatFloatText(transform.Location.y, 2) + ", " + FormatFloatText(transform.Location.z, 2), theme);
			const glm::vec3 degrees = glm::degrees(transform.Rotation);
			DrawReadOnlyRow(ctx, PropPath(typeName, "Rotation"), ComponentRect(rect, 1, 20),
				rotationLabel, FormatFloatText(degrees.x, 2) + ", " + FormatFloatText(degrees.y, 2) + ", "
					+ FormatFloatText(degrees.z, 2), theme);
			DrawReadOnlyRow(ctx, PropPath(typeName, "Scale"), ComponentRect(rect, 2, 20),
				scaleLabel, FormatFloatText(transform.Scale.x, 2) + ", " + FormatFloatText(transform.Scale.y, 2) + ", "
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
		changed |= DrawVec3Row(ctx, PropPath(typeName, "Location"), locationRow, locationLabel, transform.Location, theme, reachable(locationRow));
		changed |= DrawVec3Row(ctx, PropPath(typeName, "Rotation"), rotationRow, rotationLabel, rotationDegrees, theme, reachable(rotationRow));
		changed |= DrawVec3Row(ctx, PropPath(typeName, "Scale"), scaleRow, scaleLabel, transform.Scale, theme, reachable(scaleRow));
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
		const Wui::LocalizedLabel primaryLabel = SchemaFieldLabel(*primaryField);
		if (writable)
		{
			bool primary = std::get<bool>(primaryField->Get(instance));
			const bool before = primary;
			// 复选框列固定在 +140(标签列宽 140):术语对照预算 136,不压到控件上。
			Wui::LabelWithTerm(ctx, { primaryRow.X + 4, primaryRow.Y + 3 }, primaryLabel.Text, primaryLabel.Term,
				theme.TextMuted, 13.0f, theme, 136.0f);
			Wui::Checkbox(ctx, Wui::HashId(PropPath(typeName, "Primary").c_str()),
				{ primaryRow.X + 140.0f, primaryRow.Y + 1, 20, 20 }, "", primary, theme);
			const Wui::WuiRect primaryBox { primaryRow.X + 140.0f, primaryRow.Y + 1, 20, 20 };
			RegisterNode(Wui::HashId(PropPath(typeName, "Primary").c_str()), "checkbox",
				primaryBox, TermText(primaryLabel), primary ? "true" : "false", reachable(primaryBox));
			if (primary != before)
			{
				primaryField->Set(instance, Schema::Value(primary));
				changed = true;
			}
		}
		else
		{
			DrawReadOnlyRow(ctx, PropPath(typeName, "Primary"), primaryRow, primaryLabel,
				std::get<bool>(primaryField->Get(instance)) ? "true" : "false", theme);
		}
		y += kRowHeight;

		const Wui::WuiRect fixedRow { rect.X, rect.Y + y, rect.W, kRowHeight };
		const Wui::LocalizedLabel fixedLabel = SchemaFieldLabel(*fixedField);
		if (writable)
		{
			bool fixed = std::get<bool>(fixedField->Get(instance));
			const bool before = fixed;
			Wui::LabelWithTerm(ctx, { fixedRow.X + 4, fixedRow.Y + 3 }, fixedLabel.Text, fixedLabel.Term,
				theme.TextMuted, 13.0f, theme, 136.0f);
			Wui::Checkbox(ctx, Wui::HashId(PropPath(typeName, "FixedAspectRatio").c_str()),
				{ fixedRow.X + 140.0f, fixedRow.Y + 1, 20, 20 }, "", fixed, theme);
			const Wui::WuiRect fixedBox { fixedRow.X + 140.0f, fixedRow.Y + 1, 20, 20 };
			RegisterNode(Wui::HashId(PropPath(typeName, "FixedAspectRatio").c_str()), "checkbox",
				fixedBox, TermText(fixedLabel), fixed ? "true" : "false", reachable(fixedBox));
			if (fixed != before)
			{
				fixedField->Set(instance, Schema::Value(fixed));
				changed = true;
			}
		}
		else
		{
			DrawReadOnlyRow(ctx, PropPath(typeName, "FixedAspectRatio"), fixedRow, fixedLabel,
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
		const Wui::LocalizedLabel projectionLabel = Wui::TrLabel("panel.properties.camera.projection", "Projection");
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
					// 选项文案可本地化;写回仍按下标取 enumSchema->Values[i].second(raw 枚举值)。
					names.push_back(CameraProjectionLabel(enumSchema->Values[i].first));
					if (enumSchema->Values[i].second == raw)
						selected = static_cast<int>(i);
				}
			}
			Wui::LabelWithTerm(ctx, { projectionRow.X + 4, projectionRow.Y + 3 }, projectionLabel.Text,
				projectionLabel.Term, theme.TextMuted, 13.0f, theme, 136.0f);
			const Wui::WuiRect comboRect { projectionRow.X + 140.0f, projectionRow.Y + 1, projectionRow.W - 144.0f, 20 };
			if (Wui::Combo(ctx, Wui::HashId(projectionId.c_str()), comboRect, "", names, selected, theme))
			{
				projectionField->Set(cameraInstance, Schema::Value(enumSchema->Values[selected].second));
				projection = camera->GetProjectionType();
				changed = true;
			}
			RegisterNode(Wui::HashId(projectionId.c_str()), "combo", comboRect, TermText(projectionLabel),
				(selected >= 0 && selected < static_cast<int>(names.size())) ? names[selected] : std::string(),
				reachable(comboRect));
		}
		else
		{
			DrawReadOnlyRow(ctx, projectionId, projectionRow, projectionLabel,
				CameraProjectionLabel(projection == SceneCamera::ProjectionType::Perspective ? "Perspective" : "Orthographic"),
				theme);
		}
		y += kRowHeight;

		// 按当前投影类型只显示对应参数(迁移前语义),全部经 SceneCamera setter 提交。
		if (projection == SceneCamera::ProjectionType::Perspective)
		{
			float fov = camera->GetPerspectiveFOV();
			const Wui::WuiRect fovRow { rect.X, rect.Y + y, rect.W, kRowHeight };
			const bool fovChanged = DrawFloatRow(ctx, PropPath(cameraType, "Perspective.FOV"),
				fovRow, Wui::TrLabel("panel.properties.camera.fov", "FOV"), fov, 1.0f, -1.0f, theme, reachable(fovRow));
			y += kRowHeight;
			float nearClip = camera->GetPerspectiveNearClip();
			const Wui::WuiRect nearRow { rect.X, rect.Y + y, rect.W, kRowHeight };
			const bool nearChanged = DrawFloatRow(ctx, PropPath(cameraType, "Perspective.NearClip"),
				nearRow, Wui::TrLabel("panel.properties.camera.near_clip", "NearClip"), nearClip, 1.0f, -1.0f, theme,
				reachable(nearRow));
			y += kRowHeight;
			float farClip = camera->GetPerspectiveFarClip();
			const Wui::WuiRect farRow { rect.X, rect.Y + y, rect.W, kRowHeight };
			const bool farChanged = DrawFloatRow(ctx, PropPath(cameraType, "Perspective.FarClip"),
				farRow, Wui::TrLabel("panel.properties.camera.far_clip", "FarClip"), farClip, 1.0f, -1.0f, theme,
				reachable(farRow));
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
				zoomRow, Wui::TrLabel("panel.properties.camera.zoom", "Zoom"), zoom, 1.0f, -1.0f, theme, reachable(zoomRow));
			y += kRowHeight;
			float nearClip = camera->GetOrthographicNearClip();
			const Wui::WuiRect nearRow { rect.X, rect.Y + y, rect.W, kRowHeight };
			const bool nearChanged = DrawFloatRow(ctx, PropPath(cameraType, "Orthographic.NearClip"),
				nearRow, Wui::TrLabel("panel.properties.camera.near_clip", "NearClip"), nearClip, 1.0f, -1.0f, theme,
				reachable(nearRow));
			y += kRowHeight;
			float farClip = camera->GetOrthographicFarClip();
			const Wui::WuiRect farRow { rect.X, rect.Y + y, rect.W, kRowHeight };
			const bool farChanged = DrawFloatRow(ctx, PropPath(cameraType, "Orthographic.FarClip"),
				farRow, Wui::TrLabel("panel.properties.camera.far_clip", "FarClip"), farClip, 1.0f, -1.0f, theme,
				reachable(farRow));
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
