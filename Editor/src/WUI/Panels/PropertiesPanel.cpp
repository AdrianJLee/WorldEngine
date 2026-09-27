#include "wldpch.h"
#include "PropertiesPanel.h"
#include "EditorAssetCatalog.h"

// P2 W5b:Reload 按钮要复用 EditorLayer 的热重载入口(与帧边界轮询、AI 通道 script.reload
// 同一条语义)。PanelHost 是跨任务冻结的窄接口,本包文件边界内不能扩展它,因此只 include。
#include "../../EditorLayer.h"

#include "World/Core/KeyCodes.h"
#include "World/Core/Asset/ScriptArtifact.h"
#include "World/Core/Asset/ProjectManifest.h"
#include "World/Gameplay/Prefab.h"
// 2026-09-26 脚本组件重写:属性表(`ScriptProperty`)的唯一维护点(注解/schema 声明 → 属性表)。
#include "World/Script/ScriptProperties.h"
// 2026-09-26 SCRIPT-V6:声明的权威解析在引擎侧(名字/类型/Doc/**脚本里的默认值**)——
// 编辑器只调 `ScriptEngine::SyncScriptDeclarations`,不再自己扫注解。
#include "World/Scene/ScriptEngine.h"
#include "World/WUI/WuiAccessibility.h"
#include "World/WUI/WuiJson.h"
#include "World/WUI/WuiLocalization.h"
#include "World/WUI/WuiWidgets.h"
#include "World/WUI/Widgets/WuiModal.h"
// WUI-P1c-W3.1:面板不再自己拼底色/描边/文字 —— 统一走 WUI 库的"基础表面与高亮"
// (PanelBackground / HighlightOutline)与 Wui::Label。逐条分类与缺件清单见
// tools/agents/reports/WUI-P1c-w3.1-properties.md。
#include "World/WUI/Widgets/WuiChrome.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <sstream>
#include <utility>

namespace World
{
	namespace
	{
		// ---- 分区滚动布局常量 ----
		constexpr float kContentTop = 40.0f;      // "Add Component" 行高
		constexpr float kSectionHeader = 24.0f;   // 与 WuiSection 的标题行一致
		constexpr float kSectionGap = 4.0f;       // 4px 栅格
		// VEC-H2:行高/动作列宽都取自库件(单一事实源),面板不再自带 22/20/9 这类魔法数字。
		const float kRowHeight = Wui::PropertyRowHeight();
		constexpr float kScrollbarWidth = 10.0f;
		// 只有在"确实还有内容可滚"时,上/下按钮才注册成可点击节点(与真实可用性一致)。
		constexpr float kScrollEpsilon = 0.5f;

		// ---- VEC-A4:向量行统一走 WUI 组件库(Vec2Field/Vec3Field/Vec4Field) ----
		// 控件列窄于 kVecFieldNarrowWidth 时用库件的竖排(layout 1);行高按"每个分量一格
		// kVecFieldSlotHeight"给足 —— 库件把传入 rect 等分给各分量,高度不够会把两位小数的
		// 读数压到互相重叠(材质编辑器对 Vec4 的 2×2 用两倍行高,是同一条口径)。
		constexpr float kVecFieldNarrowWidth = 180.0f;
		const float kVecFieldSlotHeight = Wui::PropertyRowHeight();

		// ---- VEC-C2:脚本属性行的行尾动作列 ----
		// `↺` 复位(库件 ResetDefaultButton:自己画回旋箭头,不依赖字体字形)、数组/映射元素行的
		// `-` 删除列。两列都在**控件列**右侧:控件列宽相应收窄,集合行的删除列在缩进后的行矩形之外
		// (容器把子行矩形按 kCollectionActionWidth 收窄后再递归,按钮落回容器行的右缘)。
		const float kCollectionActionWidth = Wui::CollectionActionColumnWidth();

		int VecFieldLayout(float controlWidth)
		{
			return controlWidth < kVecFieldNarrowWidth ? 1 : 0;
		}

		// 与 WuiWidgets.cpp 的 VecFieldCore 同一条排布:横排 Vec2/Vec3 = 一行,Vec4 = 2×2
		// 两行;竖排(layout 1)= 每个分量一行。
		int VecFieldRows(int layout, int components)
		{
			if (layout == 1)
				return components;
			return components == 4 ? 2 : 1;
		}

		float VecFieldHeight(int layout, int components)
		{
			return kVecFieldSlotHeight * static_cast<float>(VecFieldRows(layout, components));
		}

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

		// P4-U13b:实例条/动作行按钮(带禁用态 + 理由)已收进库件 `Wui::ActionButton`
		// (VEC-H2):同一套底色/描边/文字与"灰按钮不能没有理由"的无障碍口径,面板只做编排。

		std::string FormatFloatText(float value, int decimals = 3)
		{
			char buffer[48] = {};
			std::snprintf(buffer, sizeof(buffer), "%.*f", decimals, value);
			return buffer;
		}

		// ---- CPPT-3-FIX1:数值行的 Unit / Step 接线 ----
		// Step → 显示小数位(0.1 → 1 位、0.01 → 2 位、≥1 → 0 位;未声明 = 既有 3 位口径)。
		int StepDecimals(bool hasStep, float step)
		{
			if (!hasStep || !(step > 0.0f))
				return 3;
			if (step >= 1.0f) return 0;
			if (step >= 0.1f) return 1;
			if (step >= 0.01f) return 2;
			return 3;
		}

		// 整数数值行的控件选型:声明了 Unit 的行改走 `NumberFieldInt`(值区固定宽 + 单位后缀,
		// Step(1) 时给 [−]/[+] 步进 —— 该件的步进粒度就是 1);没有 Unit 的行保持既有 `DragInt`
		// (自由拖动;Step 的"1 单位"本来就是拖动/方向键的固有粒度)。
		void DrawScriptIntControl(Wui::WuiContext& ctx, Wui::WuiId id, const Wui::WuiRect& rect,
			int64_t& raw, int64_t lo, int64_t hi, const Schema::FieldMetadata& meta, const Wui::WuiTheme& theme)
		{
			if (meta.Unit.empty())
			{
				Wui::DragInt(ctx, id, rect, raw, lo, hi, theme);
				return;
			}
			Wui::WuiNumberStyle style;
			style.Unit = meta.Unit.c_str();
			style.Decimals = 0;
			style.Steppers = meta.Step.has_value() && std::fabs(*meta.Step - 1.0f) < 1e-4f;
			Wui::NumberFieldInt(ctx, id, rect, raw, lo, hi, theme, style);
		}

		// 内容根(开发布局 projects/default/assets,打包由清单决定):**解析一次**缓存起来。
		// 实例条每帧都要判断"来源资产还在不在",每帧重读 project.we.yaml 是不可接受的。
		const std::filesystem::path& CachedContentRoot()
		{
			static const std::filesystem::path root = []
			{
				std::filesystem::path manifestPath;
				if (!Asset::ProjectManifest::Locate(std::filesystem::current_path(), &manifestPath))
					return std::filesystem::path {};
				std::string error;
				Asset::ProjectManifest manifest;
				if (!Asset::ProjectManifest::Load(manifestPath, &manifest, &error))
					return std::filesystem::path {};
				return manifest.ResolveContentRoot(manifestPath);
			}();
			return root;
		}

		// P4-U13b:来源资产是否还在盘上。实例记录里存的是**逻辑路径**(相对内容根),
		// 所以先按原样试,再按内容根解析(与层级面板的 prefab 路径解析同一约定)。
		bool PrefabSourceExists(const std::string& path)
		{
			if (path.empty())
				return false;
			std::error_code error;
			if (std::filesystem::exists(std::filesystem::path(path), error))
				return true;
			const std::filesystem::path& contentRoot = CachedContentRoot();
			return !contentRoot.empty() && std::filesystem::exists(contentRoot / path, error);
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

		// 定义在下方(CPPT-3 的只读摘要与行悬停共用);先声明以便 FormatReadOnlyValue 引用。
		std::string ScriptLeafTypeText(Schema::Kind kind);

		std::string FormatReadOnlyValue(const Schema::FieldSchema& field, const Schema::Value& value)
		{
			if (field.K == Schema::Kind::Enum)
			{
				const Schema::EnumSchema* schema = field.GetEnum ? field.GetEnum() : nullptr;
				if (schema)
					return EnumNameOf(*schema, EnumRawOf(*schema, value));
			}
			// CPPT-3:没有行控件的数学类型(IVec*/UVec*/Quat/Mat*)→ 只读摘要显示类型名。
			// 这些值不进属性表/不进存档(引擎侧同步成 ReadOnly 摘要行),显示 "(0, 0, 0)" 一类
			// 伪值会误导 —— 与 Luau 裸 table 的 `table` 摘要同一口径。
			if (ScriptProperties::IsSummaryKind(field.K))
				return ScriptLeafTypeText(field.K);
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

		// 自定义检查器的 Vec3 行(度/单位由调用方处理)。VEC-A4:控件本体 = 库件
		// `Wui::Vec3Field`(轴标签 / 拖动 / 键入 / ↑↓ 是库件那一套),行 id 仍是
		// `properties.<组件>.<字段>`;分量无障碍节点由库件登记(`...axis.0/1/2`)。
		// 返回本行占用的高度(竖排时是单行的 3 倍)。
		float DrawVec3Row(Wui::WuiContext& ctx, const std::string& baseId, float x, float y, float width,
			const Wui::LocalizedLabel& label, glm::vec3& value, const Wui::WuiTheme& theme, bool& changed)
		{
			// VEC-H2:列宽/文字起点与库件行同一口径(PropertyRowLabelWidth + 左 8px 起画),保证
			// Transform 行与库件属性行的标签列严格对齐。
			const float labelWidth = Wui::PropertyRowLabelWidth({ x, y, width, kRowHeight });
			// 术语对照的文本预算 = 本行真实标签列宽(控件列起点 - 标签起点),窄处自动省略。
			const float labelBudget = labelWidth - 12.0f;
			Wui::LabelWithTerm(ctx, { x + 8.0f, y + (kRowHeight - 13.0f) * 0.5f - 2.0f }, label.Text, label.Term,
				theme.TextMuted, 13.0f, theme, labelBudget);
			const Wui::WuiRect ctrl { x + labelWidth, y, width - labelWidth - 4.0f, 0.0f };
			const int layout = VecFieldLayout(ctrl.W);
			const Wui::WuiRect field { ctrl.X, ctrl.Y, ctrl.W, VecFieldHeight(layout, 3) };
			changed = Wui::Vec3Field(ctx, Wui::HashId(baseId.c_str()), field, value, 0.01f, 1.0f, -1.0f,
				theme, layout);
			// 行高 = 控件高(库件 slot 已是 4px 栅格的行高,行与行直接相邻)。
			return field.H;
		}

		// 浮点行(带范围;无范围时用 1/-1 哨兵,与 schema 字段路径一致)。
		bool DrawFloatRow(Wui::WuiContext& ctx, const std::string& idText, const Wui::WuiRect& row,
			const Wui::LocalizedLabel& label, float& value, float lo, float hi, const Wui::WuiTheme& theme, bool reachable)
		{
			// VEC-H2:与库件属性行同一列宽/文字起点。
			const float labelWidth = Wui::PropertyRowLabelWidth(row);
			const float labelBudget = labelWidth - 12.0f;
			Wui::LabelWithTerm(ctx, { row.X + 8.0f, row.Y + (row.H - 13.0f) * 0.5f - 2.0f }, label.Text, label.Term,
				theme.TextMuted, 13.0f, theme, labelBudget);
			const Wui::WuiRect ctrl { row.X + labelWidth, row.Y + (row.H - 20.0f) * 0.5f,
				row.W - labelWidth - 4.0f, 20.0f };
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

		// ---- 2026-09-26 脚本组件重写:统一脚本检视器的三个数据侧小工具 ----
		//
		// `ScriptProperty::Value` 的类型编码必须与 schema 一致
		// (Bool→bool、Int*→对应宽度、Float→float、Double→double、String→string、
		//  Vec2/3/4→glm::vec2/3/4):属性行复用 `DrawSchemaFields`,那里按 Kind 直接
		// `std::get<T>(value)`,值停在 monostate(刚声明还没填值 / 手改过的场景缺 Value)
		// 会抛 std::bad_variant_access —— 向量行必须在下面补零值兜底,否则 A 期放行
		// Vec2/3/4 后脚本属性里"声明了但没值"的向量行会直接崩面板。
		Schema::Value DefaultScriptPropertyValue(Schema::Kind kind)
		{
			switch (kind)
			{
				case Schema::Kind::Bool: return Schema::Value(false);
				case Schema::Kind::Int8: return Schema::Value(static_cast<int8_t>(0));
				case Schema::Kind::Int16: return Schema::Value(static_cast<int16_t>(0));
				case Schema::Kind::Int32: return Schema::Value(static_cast<int32_t>(0));
				case Schema::Kind::Int64: return Schema::Value(static_cast<int64_t>(0));
				case Schema::Kind::UInt8: return Schema::Value(static_cast<uint8_t>(0));
				case Schema::Kind::UInt16: return Schema::Value(static_cast<uint16_t>(0));
				case Schema::Kind::UInt32: return Schema::Value(static_cast<uint32_t>(0));
				case Schema::Kind::UInt64: return Schema::Value(static_cast<uint64_t>(0));
				case Schema::Kind::Float: return Schema::Value(0.0f);
				case Schema::Kind::Double: return Schema::Value(0.0);
				case Schema::Kind::String: return Schema::Value(std::string());
				case Schema::Kind::Vec2: return Schema::Value(glm::vec2(0.0f));
				case Schema::Kind::Vec3: return Schema::Value(glm::vec3(0.0f));
				case Schema::Kind::Vec4: return Schema::Value(glm::vec4(0.0f));
				// CPPT-3:只读摘要行(IVec*/UVec*/Quat/Mat*)与 Enum/Asset 的规范零值;
				// Enum 走有符号口径(与 EnumSchema::IsSigned 的默认一致)、Asset = 空路径。
				case Schema::Kind::IVec2: return Schema::Value(glm::ivec2(0));
				case Schema::Kind::IVec3: return Schema::Value(glm::ivec3(0));
				case Schema::Kind::IVec4: return Schema::Value(glm::ivec4(0));
				case Schema::Kind::UVec2: return Schema::Value(glm::uvec2(0u));
				case Schema::Kind::UVec3: return Schema::Value(glm::uvec3(0u));
				case Schema::Kind::UVec4: return Schema::Value(glm::uvec4(0u));
				case Schema::Kind::Quat: return Schema::Value(glm::quat(1.0f, 0.0f, 0.0f, 0.0f));
				case Schema::Kind::Mat3: return Schema::Value(glm::mat3(1.0f));
				case Schema::Kind::Mat4: return Schema::Value(glm::mat4(1.0f));
				case Schema::Kind::Enum: return Schema::Value(static_cast<int64_t>(0));
				case Schema::Kind::Asset: return Schema::Value(std::string());
				// B 期:结构化表的值在 `Children` 里,`Value` 保持空表(monostate)——
				// 与 `ScriptProperties::ValueMatchesKind` 同一口径,不往 variant 里塞表数据。
				case Schema::Kind::Object: return Schema::Value();
				default: return Schema::Value();
			}
		}

		bool ScriptPropertyValueMatchesType(const ScriptProperty& property)
		{
			switch (property.Type)
			{
				case Schema::Kind::Bool: return std::holds_alternative<bool>(property.Value);
				case Schema::Kind::Int8: return std::holds_alternative<int8_t>(property.Value);
				case Schema::Kind::Int16: return std::holds_alternative<int16_t>(property.Value);
				case Schema::Kind::Int32: return std::holds_alternative<int32_t>(property.Value);
				case Schema::Kind::Int64: return std::holds_alternative<int64_t>(property.Value);
				case Schema::Kind::UInt8: return std::holds_alternative<uint8_t>(property.Value);
				case Schema::Kind::UInt16: return std::holds_alternative<uint16_t>(property.Value);
				case Schema::Kind::UInt32: return std::holds_alternative<uint32_t>(property.Value);
				case Schema::Kind::UInt64: return std::holds_alternative<uint64_t>(property.Value);
				case Schema::Kind::Float: return std::holds_alternative<float>(property.Value);
				case Schema::Kind::Double: return std::holds_alternative<double>(property.Value);
				case Schema::Kind::String: return std::holds_alternative<std::string>(property.Value);
				case Schema::Kind::Vec2: return std::holds_alternative<glm::vec2>(property.Value);
				case Schema::Kind::Vec3: return std::holds_alternative<glm::vec3>(property.Value);
				case Schema::Kind::Vec4: return std::holds_alternative<glm::vec4>(property.Value);
				case Schema::Kind::IVec2: return std::holds_alternative<glm::ivec2>(property.Value);
				case Schema::Kind::IVec3: return std::holds_alternative<glm::ivec3>(property.Value);
				case Schema::Kind::IVec4: return std::holds_alternative<glm::ivec4>(property.Value);
				case Schema::Kind::UVec2: return std::holds_alternative<glm::uvec2>(property.Value);
				case Schema::Kind::UVec3: return std::holds_alternative<glm::uvec3>(property.Value);
				case Schema::Kind::UVec4: return std::holds_alternative<glm::uvec4>(property.Value);
				case Schema::Kind::Quat: return std::holds_alternative<glm::quat>(property.Value);
				case Schema::Kind::Mat3: return std::holds_alternative<glm::mat3>(property.Value);
				case Schema::Kind::Mat4: return std::holds_alternative<glm::mat4>(property.Value);
				// Enum 存整数(有符号/无符号两种编码都可能出现在场景文本里)。
				case Schema::Kind::Enum: return std::holds_alternative<int64_t>(property.Value)
					|| std::holds_alternative<uint64_t>(property.Value);
				case Schema::Kind::Asset: return std::holds_alternative<std::string>(property.Value);
				// B 期:Object 的"值"是 Children(空表 = monostate),永远算匹配。
				case Schema::Kind::Object: return std::holds_alternative<std::monostate>(property.Value);
				default: return false;
			}
		}

		// 画属性行用的**展示值**:值没设(monostate)或类型不匹配时给该类型的规范零值(**不写回**组件)。
		//
		// 为什么不能写回:审查 P1-1 / 用户反馈④ —— "未设"必须保持未设,编辑期把它写成 0 会随场景落盘;
		// 真实默认值来自脚本本体,由引擎侧的属性入口填进 `ScriptProperty.Value`(到了这里自然显示真值)。
		//
		// VEC-F2:单项 `↺` 现在只把该行清成"未设"(不再触发整表重同步)—— 展示值必须自己回落到
		// 声明默认值 `ScriptProperty::Default`,否则复位会让该行瞬间显示成 0(与 D1 的
		// "未设 = 显示脚本默认值、存档不写"口径不一致)。同样是**只读回退**,不写回组件。
		Schema::Value ScriptPropertyDisplayValue(const ScriptProperty& property)
		{
			if (std::holds_alternative<std::monostate>(property.Value)
				&& ScriptProperties::ValueMatchesKind(property.Default, property.Type))
				return property.Default;
			return ScriptPropertyValueMatchesType(property)
				? property.Value : DefaultScriptPropertyValue(property.Type);
		}

		// VEC-H4:**行展示值** = `ScriptPropertyDisplayValue`,但"存的值类型与声明不符"时返回
		// 空表(monostate),让行落到 `—` 占位(反模式 4:多值/不可用时不许显示伪零)。
		// 判据只看"真的存了值且类型不符";`Value` 本身为空(未设)+ 默认值不可用仍是旧行为(显示零值,
		// 可编辑、可写盘)—— A 期验收的 Ghost 行(retro 25/25)因此逐条不变。
		Schema::Value ScriptPropertyDisplayValueForRow(const ScriptProperty& property)
		{
			if (!std::holds_alternative<std::monostate>(property.Value)
				&& !ScriptPropertyValueMatchesType(property))
				return Schema::Value();
			return ScriptPropertyDisplayValue(property);
		}

		// ---- VEC-C2:脚本属性行的**类型文案**(方案 v4 §1)----
		//
		// 脚本属性行没有注解说明(`ScriptProperty::Doc` 为空)时,行悬停/读屏回落成类型文案
		// (`number/string/boolean/vec3/table/struct/array/map`),不再给英文兜底
		// "No description for this field"。类型名是脚本作者写的标识符(`---@field Speed number`),
		// 与脚本字段名同一条口径:**不过本地化目录**(不查 `schema.field.*`,也不进目录)。
		std::string ScriptLeafTypeText(Schema::Kind kind)
		{
			switch (kind)
			{
				case Schema::Kind::Bool: return "boolean";
				case Schema::Kind::Int8:
				case Schema::Kind::Int16:
				case Schema::Kind::Int32:
				case Schema::Kind::Int64:
				case Schema::Kind::UInt8:
				case Schema::Kind::UInt16:
				case Schema::Kind::UInt32:
				case Schema::Kind::UInt64: return "integer";
				case Schema::Kind::Float:
				case Schema::Kind::Double: return "number";
				case Schema::Kind::String: return "string";
				case Schema::Kind::Vec2: return "vec2";
				case Schema::Kind::Vec3: return "vec3";
				case Schema::Kind::Vec4: return "vec4";
				// CPPT-3:面板没有行控件的数学类型也用脚本作者写下的类型名(只读摘要行/行悬停)。
				case Schema::Kind::IVec2: return "ivec2";
				case Schema::Kind::IVec3: return "ivec3";
				case Schema::Kind::IVec4: return "ivec4";
				case Schema::Kind::UVec2: return "uvec2";
				case Schema::Kind::UVec3: return "uvec3";
				case Schema::Kind::UVec4: return "uvec4";
				case Schema::Kind::Quat: return "quat";
				case Schema::Kind::Mat3: return "mat3";
				case Schema::Kind::Mat4: return "mat4";
				default: return "table";
			}
		}

		std::string ScriptPropertyTypeText(const ScriptProperty& property)
		{
			switch (property.Collection)
			{
				case ScriptPropertyCollection::Array: return "array";
				case ScriptPropertyCollection::Map: return "map";
				default: break;
			}
			if (property.Type == Schema::Kind::Object)
			{
				// 结构化表:能展开 = `struct`;裸 table / 降级只读摘要 = `table`(B3 的摘要口径不变)。
				const bool expandable = !property.ReadOnly && !property.Children.empty();
				return expandable ? "struct" : "table";
			}
			// CPPT-3:Enum/Asset 的类型文案带上声明名(枚举名 / 资产类型),没有 Doc 时行悬停用它。
			if (property.Type == Schema::Kind::Enum)
				return property.TypeName.empty() ? std::string("enum") : property.TypeName;
			if (property.Type == Schema::Kind::Asset)
				return property.TypeName.empty() ? std::string("asset") : property.TypeName;
			return ScriptLeafTypeText(property.Type);
		}

		// 数组/映射即使**一个元素都没有**也可以展开(底部有 `+` 加元素/键);结构化表没有子字段时
		// 才是只读摘要(裸 table)。两处判据(顶层行 / 嵌套子行)共用这一份,避免口径分叉。
		bool ScriptPropertyExpandable(const ScriptProperty& property)
		{
			if (property.ReadOnly)
				return false;
			if (property.Collection == ScriptPropertyCollection::Array
				|| property.Collection == ScriptPropertyCollection::Map)
				return true;
			return !property.Children.empty();
		}

		// 字段 schema 的类型文案:Object 行的合成 schema 把类型文案写在 `Meta.DisplayName`
		// (只读摘要行的文本同源),叶子行按 Kind 反推。
		std::string ScriptFieldTypeText(const Schema::FieldSchema& field)
		{
			if (field.K != Schema::Kind::Object)
				return ScriptLeafTypeText(field.K);
			if (!field.Meta.DisplayName.empty())
				return field.Meta.DisplayName;
			return field.GetNested ? "struct" : "table";
		}

		// ---- VEC-B3:嵌套 `---@class`(Object)属性的合成 schema ----
		//
		// `DrawSchemaFields` 的 Object 行只认 `Schema::FieldSchema` 里的**无捕获函数指针**
		// (`Get/Set/GetPtr/GetNested`),所以:
		//   * 子字段 i 的 Get/Set 以"父 ScriptProperty*"为 instance,读写 `Children[i].Value`;
		//   * 子字段 i 若是 Object,它的 GetPtr 以父为 instance,返回 `&Children[i]`(递归层用);
		//   * 节点 k 的 GetNested 返回 arena 里的第 k 个合成 TypeSchema。
		// 按编译期下标实例化访问器,运行时用下标表选中(与 B2 的护栏同口径:单层 <= 64、
		// 深度 <= 4)。arena 节点数另有面板侧上限;任何溢出都降级成只读摘要行,不崩、不展开错行。
		constexpr size_t kScriptTableMaxChildren = 64;
		constexpr size_t kScriptTableMaxNodes = 256;
		constexpr size_t kScriptTableMaxDepth = 4;
		constexpr size_t kScriptTableNoNode = static_cast<size_t>(-1);

		struct ScriptTableChildAccessors
		{
			Schema::Value (*Get)(const void*) = nullptr;
			void (*Set)(void*, const Schema::Value&) = nullptr;
			void* (*GetPtr)(void*) = nullptr;
			const void* (*GetPtrConst)(const void*) = nullptr;
		};

		struct ScriptTableSchemaArena
		{
			std::array<Schema::TypeSchema, kScriptTableMaxNodes> Nodes;
			// VEC-C2:每个节点对应的集合形态(容器行靠它判断"正在画的子行属于数组/映射")。
			// Nodes 与 Collections 同下标;普通 schema 的节点不在 arena 里 → 查不到 = None。
			std::array<ScriptPropertyCollection, kScriptTableMaxNodes> Collections {};
			// VEC-H6:每个节点描述的 ScriptProperty(它的 `Children` 与节点 `Fields` 同序)。
			// 复位可见性判定要拿 ScriptProperty 的 Value/Default/Children,而 FieldSchema 只有访问器。
			std::array<const ScriptProperty*, kScriptTableMaxNodes> Owners {};
			size_t Used = 0;
		};

		ScriptTableSchemaArena& ScriptTableArena()
		{
			static thread_local ScriptTableSchemaArena arena;
			return arena;
		}

		template <size_t Index>
		Schema::Value ScriptTableChildGet(const void* instance)
		{
			const auto* parent = static_cast<const ScriptProperty*>(instance);
			if (!parent || Index >= parent->Children.size())
				return Schema::Value();   // 合成与绘制同帧同源,正常不可达;防越界 UB
			return ScriptPropertyDisplayValueForRow(parent->Children[Index]);
		}

		template <size_t Index>
		void ScriptTableChildSet(void* instance, const Schema::Value& edited)
		{
			auto* parent = static_cast<ScriptProperty*>(instance);
			if (parent && Index < parent->Children.size())
				parent->Children[Index].Value = edited;
		}

		template <size_t Index>
		void* ScriptTableChildPtr(void* instance)
		{
			auto* parent = static_cast<ScriptProperty*>(instance);
			return (parent && Index < parent->Children.size())
				? static_cast<void*>(&parent->Children[Index]) : nullptr;
		}

		template <size_t Index>
		const void* ScriptTableChildPtrConst(const void* instance)
		{
			const auto* parent = static_cast<const ScriptProperty*>(instance);
			return (parent && Index < parent->Children.size())
				? static_cast<const void*>(&parent->Children[Index]) : nullptr;
		}

		template <size_t Node>
		const Schema::TypeSchema* ScriptTableNode()
		{
			ScriptTableSchemaArena& arena = ScriptTableArena();
			return Node < arena.Used ? &arena.Nodes[Node] : nullptr;
		}

		template <size_t... Index>
		constexpr std::array<ScriptTableChildAccessors, sizeof...(Index)> MakeScriptTableChildAccessors(
			std::index_sequence<Index...>)
		{
			return { ScriptTableChildAccessors { &ScriptTableChildGet<Index>, &ScriptTableChildSet<Index>,
				&ScriptTableChildPtr<Index>, &ScriptTableChildPtrConst<Index> }... };
		}

		template <size_t... Node>
		constexpr std::array<const Schema::TypeSchema* (*)(), sizeof...(Node)> MakeScriptTableNodeTable(
			std::index_sequence<Node...>)
		{
			return { &ScriptTableNode<Node>... };
		}

		const auto kScriptTableChildAccessors =
			MakeScriptTableChildAccessors(std::make_index_sequence<kScriptTableMaxChildren> {});
		const auto kScriptTableNodeTable =
			MakeScriptTableNodeTable(std::make_index_sequence<kScriptTableMaxNodes> {});

		// ---- CPPT-3:C++ Enum 属性的合成 schema ----
		//
		// 枚举行只认 `FieldSchema::GetEnum`(**无捕获**函数指针),所以把注册表里的 EnumSchema
		// 拷进一个稳定 arena,再用编译期下标表选中(与上面的脚本表访问器同一手法)。
		// arena 在绘制脚本属性表前重置;32 个枚举行/帧是面板侧护栏,溢出 = 该行降级只读摘要。
		constexpr size_t kScriptEnumMax = 32;

		struct ScriptEnumArena
		{
			std::array<Schema::EnumSchema, kScriptEnumMax> Enums {};
			size_t Used = 0;
		};

		ScriptEnumArena& ScriptEnumArenaStore()
		{
			static thread_local ScriptEnumArena arena;
			return arena;
		}

		template <size_t Index>
		const Schema::EnumSchema* ScriptEnumNode()
		{
			ScriptEnumArena& arena = ScriptEnumArenaStore();
			return Index < arena.Used ? &arena.Enums[Index] : nullptr;
		}

		template <size_t... Index>
		constexpr std::array<const Schema::EnumSchema* (*)(), sizeof...(Index)> MakeScriptEnumTable(
			std::index_sequence<Index...>)
		{
			return { &ScriptEnumNode<Index>... };
		}

		const auto kScriptEnumTable = MakeScriptEnumTable(std::make_index_sequence<kScriptEnumMax> {});

		using ScriptEnumGetter = const Schema::EnumSchema* (*)();

		// 从注册表取枚举 schema 的稳定访问器(nullptr = 找不到 / arena 溢出 → 行降级只读摘要)。
		ScriptEnumGetter ScriptEnumAccessorFor(const Schema::SchemaRegistry& schemas,
			const std::string& enumName)
		{
			if (enumName.empty())
				return nullptr;
			const Schema::EnumSchema* source = schemas.FindEnum(enumName);
			if (!source)
				return nullptr;
			ScriptEnumArena& arena = ScriptEnumArenaStore();
			if (arena.Used >= kScriptEnumMax)
				return nullptr;
			const size_t index = arena.Used++;
			arena.Enums[index] = *source;
			return kScriptEnumTable[index];
		}

		// 合成节点 → 集合形态。指针不在 arena 范围内(普通 schema 节点)= None:数组/映射的
		// 增删路径只对脚本合成 schema 生效,不会走到 Play 里 C++ 实例的嵌套结构上。
		ScriptPropertyCollection ScriptTableCollectionOf(const Schema::TypeSchema* node)
		{
			if (!node)
				return ScriptPropertyCollection::None;
			ScriptTableSchemaArena& arena = ScriptTableArena();
			// 用地址比较(不同对象之间的指针序在标准里未定义;地址转整数后比较是确定的)。
			const uintptr_t address = reinterpret_cast<uintptr_t>(node);
			const uintptr_t begin = reinterpret_cast<uintptr_t>(arena.Nodes.data());
			const uintptr_t end = reinterpret_cast<uintptr_t>(arena.Nodes.data() + arena.Used);
			if (address < begin || address >= end)
				return ScriptPropertyCollection::None;
			return arena.Collections[static_cast<size_t>(node - arena.Nodes.data())];
		}

		// VEC-H6:arena 节点 → 它描述的 `ScriptProperty`。返回 nullptr = 该节点不在 arena
		// (顶层字段行的合成 schema —— 那时 instance 就是这条属性本身,见 ScriptRowModel)。
		const ScriptProperty* ScriptTableNodeOwner(const Schema::TypeSchema* node)
		{
			if (!node)
				return nullptr;
			ScriptTableSchemaArena& arena = ScriptTableArena();
			const uintptr_t address = reinterpret_cast<uintptr_t>(node);
			const uintptr_t begin = reinterpret_cast<uintptr_t>(arena.Nodes.data());
			const uintptr_t end = reinterpret_cast<uintptr_t>(arena.Nodes.data() + arena.Used);
			if (address < begin || address >= end)
				return nullptr;
			return arena.Owners[static_cast<size_t>(node - arena.Nodes.data())];
		}

		// VEC-H6:当前正在画的行(合成脚本表)对应哪一条 `ScriptProperty`。
		//  · 嵌套节点:instance = 父属性 → 行 = 父->Children[fieldIndex](与节点 Fields 同序);
		//  · 顶层行:合成 schema 不在 arena 里,instance 就是那条属性本身。
		// 调用方必须已确认这是合成路径(m_ScriptInspectingScriptRows)—— Play 里 C++ 实例走真实
		// 结构体指针,cast 成 ScriptProperty* 是未定义行为。
		const ScriptProperty* ScriptRowModel(const Schema::TypeSchema& node, const void* instance, size_t fieldIndex)
		{
			if (const ScriptProperty* owner = ScriptTableNodeOwner(&node))
				return fieldIndex < owner->Children.size() ? &owner->Children[fieldIndex] : nullptr;
			return static_cast<const ScriptProperty*>(instance);
		}

		// VEC-H6:一条**叶子**属性是否偏离脚本声明的默认值(与面板显示同源):
		//  · 未设(monostate)→ 面板显示的就是默认值 ⇒ 一致(不出现 ↺);
		//  · 声明没给默认值(Default 也是 monostate)→ 有值即偏离;
		//  · 都有值 → ValuesEqual 逐字段比(浮点按位相等,不做容差)。
		bool ScriptLeafRowModified(const ScriptProperty& row)
		{
			if (ScriptProperties::IsUnset(row))
				return false;
			if (std::holds_alternative<std::monostate>(row.Default))
				return true;
			return !ScriptProperties::ValuesEqual(row.Value, row.Default);
		}

		const ScriptProperties::Declaration* FindDeclarationField(
			const ScriptProperties::Declaration& parent, const std::string& name)
		{
			for (const ScriptProperties::Declaration& field : parent.Fields)
				if (field.Name == name)
					return &field;
			return nullptr;
		}

		// VEC-H6:一条属性(叶子/容器,**递归**)是否偏离脚本声明默认 —— `↺` 的可见性判据。
		// 容器 = 任一子行偏离 **或** 形状偏离:数组/映射的默认形状 = 声明里的元素/键行(顺序 + 行名);
		// 结构体的形状由声明决定(合并时按声明重建),只需递归看值。
		// declaration 可空(场景独有行 / 声明读不出来)→ 退化成"只看值",不猜形状。
		bool ScriptRowModified(const ScriptProperty& row, const ScriptProperties::Declaration* declaration)
		{
			const bool container = row.Type == Schema::Kind::Object
				|| row.Collection == ScriptPropertyCollection::Array
				|| row.Collection == ScriptPropertyCollection::Map;
			if (!container)
				return ScriptLeafRowModified(row);
			if (declaration && !declaration->FieldsUnknown && !declaration->ReadOnly
				&& (row.Collection == ScriptPropertyCollection::Array
					|| row.Collection == ScriptPropertyCollection::Map))
			{
				// 形状:元素个数/键数或行名序列与声明的默认形状不同 = 改过(增/删/改键)。
				if (row.Children.size() != declaration->Fields.size())
					return true;
				for (size_t index = 0; index < row.Children.size(); ++index)
					if (row.Children[index].Name != declaration->Fields[index].Name)
						return true;
			}
			for (const ScriptProperty& child : row.Children)
				if (ScriptRowModified(child, declaration ? FindDeclarationField(*declaration, child.Name) : nullptr))
					return true;
			return false;
		}

		// ---- VEC-C2:数组/映射的元素增删(面板侧只改 `Children`)----
		//
		// 新增元素的值 = 该类型的规范零值(`+` 是"造一行",不是"设一个值");元素本身是嵌套集合
		// (如 `{{number}}`)时模板取已有同类元素的形态(Collection/ElementKind/KeyKind 在子项上),
		// 空容器没有模板 —— 不猜嵌套结构,按叶子样式落一行(可编辑、可存档)。
		ScriptProperty MakeCollectionElement(const ScriptProperty& container)
		{
			ScriptProperty child;
			child.Type = container.ElementKind;
			if (!container.Children.empty())
			{
				const ScriptProperty& model = container.Children.back();
				child.Type = model.Type;
				child.TypeName = model.TypeName;
				child.Collection = model.Collection;
				child.ElementKind = model.ElementKind;
				child.KeyKind = model.KeyKind;
				child.ReadOnly = model.ReadOnly;
			}
			child.Value = DefaultScriptPropertyValue(child.Type);
			return child;
		}

		bool CollectionKeyTaken(const ScriptProperty& container, const std::string& key)
		{
			return std::any_of(container.Children.begin(), container.Children.end(),
				[&key](const ScriptProperty& child) { return child.Name == key; });
		}

		// 数组行名 = 下标字符串 1..n:删掉中间元素后重排,让行 id / 存档顺序 / 脚本写回(`1..n`)
		// 共用同一份下标口径。
		void RenumberArrayChildren(ScriptProperty& container)
		{
			for (size_t index = 0; index < container.Children.size(); ++index)
				container.Children[index].Name = std::to_string(index + 1);
		}

		// ---- VEC-F2:两级复原的公共口径 ----
		//
		// 单项 `↺` = 把该行清成"未设"(monostate):显示走 `ScriptPropertyDisplayValue` 回落
		// 声明默认值,存档按 D1 判定"未设 → 整条不写"。**不触发任何整表重同步** ——
		// 旧实现在复位后强制 `SyncScriptDeclarations`,会把同一集合里用户刚做的增删(形状)按声明重建,
		// 表现就是"点一个元素的 `↺`,整个集合都回去了"。
		//
		// 集合头 `↺` = 复原整个集合:Luau 只对**这一条声明**做一次 `SyncFromDeclarations`
		// (默认形状 + 默认值,其它集合/其它属性一律不动);C++ 结构化表没有数组/映射
		// (增删只存在于脚本侧),递归把叶子清成默认值即可。
		// 按"名字路径"解析一条脚本属性(名字逐段比较:映射键里的 '.' 不会被拆成两段)。
		ScriptProperty* ResolveScriptRowPath(std::vector<ScriptProperty>& properties,
			const std::vector<std::string>& path, size_t index = 0)
		{
			if (index >= path.size())
				return nullptr;
			for (ScriptProperty& property : properties)
			{
				if (property.Name != path[index])
					continue;
				if (index + 1 == path.size())
					return &property;
				return ResolveScriptRowPath(property.Children, path, index + 1);
			}
			return nullptr;
		}

		const ScriptProperties::Declaration* ResolveDeclarationPath(
			const std::vector<ScriptProperties::Declaration>& declarations,
			const std::vector<std::string>& path, size_t index = 0)
		{
			if (index >= path.size())
				return nullptr;
			for (const ScriptProperties::Declaration& declaration : declarations)
			{
				if (declaration.Name != path[index])
					continue;
				if (index + 1 == path.size())
					return &declaration;
				return ResolveDeclarationPath(declaration.Fields, path, index + 1);
			}
			return nullptr;
		}

		// 把一条属性子树里的每个**叶子**清成声明默认值(容器递归;容器自身没有 Value)。
		// 用于 C++ 结构化表(形状由 schema 决定)与"声明的默认形状读不出来"时的 Luau 兜底:
		// 形状保持,值全部回到默认 —— 不猜一个可能不存在的默认形状。
		void ResetScriptRowValues(ScriptProperty& row)
		{
			const bool container = row.Type == Schema::Kind::Object
				|| row.Collection == ScriptPropertyCollection::Array
				|| row.Collection == ScriptPropertyCollection::Map;
			if (container)
			{
				for (ScriptProperty& child : row.Children)
					ResetScriptRowValues(child);
				return;
			}
			row.Value = row.Default;
		}

		std::string ScriptRowPathText(const std::vector<std::string>& path)
		{
			std::string text;
			for (const std::string& segment : path)
			{
				if (!text.empty())
					text += '.';
				text += segment;
			}
			return text;
		}

		// 集合头 `↺` 的按钮文案按集合形态分(数组 / 映射 / 结构化表)—— 与单项 `↺` 一眼可分。
		std::string ScriptCollectionHeadResetLabel(ScriptPropertyCollection collection)
		{
			switch (collection)
			{
				case ScriptPropertyCollection::Array: return "Reset the whole array";
				case ScriptPropertyCollection::Map: return "Reset the whole map";
				default: return "Reset the whole struct";
			}
		}

		// 集合头 `↺` 的悬停说明:说清"是整集合 + 会丢什么"(用户口径:复原整个集合,丢弃所有增删改)。
		const char* ScriptCollectionHeadResetDoc()
		{
			return "Restore the entire collection to the script's default shape and values "
				"(discards all adds, edits and removals)";
		}

		// 单项 `↺` 的悬停说明:与集合头区分 —— 只动这一项。
		const char* ScriptItemResetDoc()
		{
			return "Restore only this item to the script's default value (the row goes back to unset)";
		}

		// 顶层 Object 属性行的 instance 就是该属性本身:子 schema 的实例直接透传,
		// 子字段访问器再从它身上取 `Children[i]`。
		void* ScriptTableIdentityPtr(void* instance) { return instance; }
		const void* ScriptTableIdentityPtrConst(const void* instance) { return instance; }

		// 把一条结构化表属性递归合成成 arena 节点(返回下标;失败 = kScriptTableNoNode)。
		// `idPath` 同时是合成 TypeSchema 的 DisplayName:DrawSchemaFields 递归时拿它当行 id
		// 前缀,于是子行 id = `properties.<组件>.<属性>.<子字段>`(递归同名规则)。
		size_t BuildScriptTableSchema(const ScriptProperty& property, const std::string& idPath, size_t depth,
			const Schema::SchemaRegistry* schemas)
		{
			ScriptTableSchemaArena& arena = ScriptTableArena();
			if (depth > kScriptTableMaxDepth || arena.Used >= kScriptTableMaxNodes)
				return kScriptTableNoNode;
			const size_t nodeIndex = arena.Used++;
			Schema::TypeSchema& node = arena.Nodes[nodeIndex];
			node = Schema::TypeSchema {};
			node.DisplayName = idPath;
			node.Category = Schema::TypeCategory::Struct;
			arena.Collections[nodeIndex] = property.Collection;
			const size_t childCount = std::min(property.Children.size(), kScriptTableMaxChildren);
			node.Fields.reserve(childCount);
			arena.Owners[nodeIndex] = &property;
			for (size_t index = 0; index < childCount; ++index)
			{
				const ScriptProperty& child = property.Children[index];
				Schema::FieldSchema field;
				field.Name = child.Name;
				field.K = child.Type;
				field.Meta.Doc = child.Doc;
				// CPPT-3:子行也吃终态口径 —— ReadOnly 透传;Enum 从注册表合成下拉所需的
				// EnumSchema(拿不到 = 只读摘要);Asset 的资产类型名写进 Meta.AssetType;
				// 面板没有行控件的数学类型直接标只读(不进存档)。
				field.Meta.ReadOnly = child.ReadOnly;
				if (child.Type == Schema::Kind::Enum)
				{
					if (schemas && !child.TypeName.empty())
						field.GetEnum = ScriptEnumAccessorFor(*schemas, child.TypeName);
					if (!field.GetEnum)
						field.Meta.ReadOnly = true;
				}
				else if (child.Type == Schema::Kind::Asset)
				{
					field.Meta.AssetType = child.TypeName;
				}
				else if (ScriptProperties::IsSummaryKind(child.Type))
				{
					field.Meta.ReadOnly = true;
				}
				if (child.Type == Schema::Kind::Object)
				{
					// 裸 table / 空结构 / 超护栏 → 只读摘要行(摘要文本由 Meta.DisplayName 携带,
					// 面板侧与 DrawSchemaFields 的 scriptPropertyRow 摘要分支同一判据)。
					// VEC-C2:类型文案(含 `struct`/`array`/`map`)也走 Meta.DisplayName —— 没有注解
					// 说明时行悬停用它回落(v4 §1);空数组/空映射仍可展开(子行只有底部 `+`)。
					field.Meta.DisplayName = ScriptPropertyTypeText(child);
					const size_t childNode = ScriptPropertyExpandable(child)
						? BuildScriptTableSchema(child, idPath + "." + child.Name, depth + 1, schemas)
						: kScriptTableNoNode;
					if (childNode == kScriptTableNoNode)
					{
						field.Meta.DisplayName = "table";
					}
					else
					{
						field.GetNested = kScriptTableNodeTable[childNode];
						field.GetPtr = kScriptTableChildAccessors[index].GetPtr;
						field.GetPtrConst = kScriptTableChildAccessors[index].GetPtrConst;
					}
				}
				else
				{
					field.Get = kScriptTableChildAccessors[index].Get;
					field.Set = kScriptTableChildAccessors[index].Set;
				}
				node.Fields.push_back(std::move(field));
			}
			return nodeIndex;
		}

		// 顶层 Object 属性:可展开 → 填 GetNested/GetPtr;只读/降级 → 不填 nested,
		// 由 DrawSchemaFields 的 scriptPropertyRow 摘要分支画一行 `table`,不可展开、不进存档。
		// 注:摘要里的 N keys 需要运行期摘要,当前冻结模型没有该字段(见 VEC-B3 报告),
		// 所以拿不到 N 时只写类型名 `table`。
		Schema::FieldSchema MakeScriptTableField(const ScriptProperty& property, const std::string& idPath,
			const Schema::SchemaRegistry* schemas)
		{
			Schema::FieldSchema field;
			field.Name = property.Name;
			field.K = Schema::Kind::Object;
			field.Meta.Doc = property.Doc;
			// VEC-C2:类型文案(裸 table → `table`;只读数组/映射 → `array`/`map`;可展开结构 → `struct`)
			// —— 摘要行文本与"没有注解说明"时的行悬停回落共用它(v4 §1)。
			field.Meta.DisplayName = ScriptPropertyTypeText(property);
			if (!ScriptPropertyExpandable(property))
				return field;
			const size_t nodeIndex = BuildScriptTableSchema(property, idPath, 1, schemas);
			if (nodeIndex == kScriptTableNoNode)
			{
				field.Meta.DisplayName = "table";
				return field;
			}
			field.GetNested = kScriptTableNodeTable[nodeIndex];
			field.GetPtr = ScriptTableIdentityPtr;
			field.GetPtrConst = ScriptTableIdentityPtrConst;
			return field;
		}

		// 按名字在 schema 类型里找字段(C++ 脚本的字段说明 / 默认值都挂在 schema 上)。
		const Schema::FieldSchema* FindScriptSchemaField(const Schema::TypeSchema& schema, const std::string& name)
		{
			for (const Schema::FieldSchema& field : schema.Fields)
				if (field.Name == name)
					return &field;
			return nullptr;
		}

		// Luau:按脚本重同步属性表(保留同名同类型值;新字段带注解 Doc 与**脚本里的默认值**)。
		// SCRIPT-V1 起这是引擎侧唯一入口:声明解析、Doc、默认值、诊断都在 ScriptEngine 里,
		// 编辑器不再自己扫注解(第二份实现已删除;声明顺序/类型/doc/默认值只有引擎这一份)。
		void SyncLuauPropertiesFromAnnotations(LuauScriptComponent& component)
		{
			std::string error;
			ScriptEngine::SyncScriptDeclarations(component, nullptr, &error);
		}

		// ---- VEC-C2:脚本声明签名(编辑态同步的门)----
		//
		// 为什么需要门:引擎的合并**在集合形状上以脚本声明为准**(`ScriptProperties::ApplyInto`:
		// 数组/映射的子行按声明的名字重建),而面板的 `+`/`-`(方案 v3 §4 / v4 §3 的验收)改的是
		// 组件里的 `Children` —— 每帧无条件重合并会把面板刚做的增删还原回去。声明签名不变时跳过
		// 合并 ⇒ 值/形状以组件(场景)为准;脚本一改(内容指纹变 → 签名变)照旧整体重建,
		// 新字段/新说明/新默认值立即生效。
		//
		// 签名含:顺序 / 名字 / 类型 / 集合形态 / 元素与键类型 / 只读标记 / 元素行是否未知 /
		// 说明(改注释也要跟着刷新)/ 默认值(脚本表初值改了,没改过的字段要回新初值)。
		void AppendDeclarationSignature(std::string& out,
			const std::vector<ScriptProperties::Declaration>& declarations, int depth)
		{
			if (depth > 6)
				return;
			for (const ScriptProperties::Declaration& declaration : declarations)
			{
				out += declaration.Name;
				out += '|';
				out += ScriptProperties::KindName(declaration.Type);
				out += '|';
				out += ScriptProperties::CollectionName(declaration.Collection);
				out += '|';
				out += ScriptProperties::KindName(declaration.ElementKind);
				out += '|';
				out += ScriptProperties::KindName(declaration.KeyKind);
				out += declaration.ReadOnly ? "|ro" : "";
				out += declaration.FieldsUnknown ? "|unknown" : "";
				out += '|';
				out += declaration.Doc;
				out += '|';
				out += FormatValueByVariant(declaration.Default);
				out += '{';
				AppendDeclarationSignature(out, declaration.Fields, depth + 1);
				out += '}';
			}
		}

		std::string ScriptDeclarationSignature(const std::vector<ScriptProperties::Declaration>& declarations)
		{
			std::string out;
			AppendDeclarationSignature(out, declarations, 0);
			return out;
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

		// P4-U9:字段说明(悬浮提示 + 无障碍 Tooltip)。英文默认 = schema 里的 Doc 原文,
		// 中文目录用 schema.field.<短类型名>.<字段名>.doc 覆盖;空 = 该字段没写说明。
		std::string FieldDocLabel(const Schema::TypeSchema& schema, const Schema::FieldSchema& field)
		{
			if (field.Meta.Doc.empty())
				return std::string();
			return Wui::Tr(("schema.field." + SchemaTypeKeyName(schema) + "." + field.Name + ".doc").c_str(),
				field.Meta.Doc);
		}

		// 颜色字段的无障碍值文本(#RRGGBBAA,与取色器弹层里的 hex 行同一写法)。
		std::string FormatColorHexText(const glm::vec4& color)
		{
			const auto channel = [](float value)
			{
				return static_cast<int>(std::clamp(value, 0.0f, 1.0f) * 255.0f + 0.5f);
			};
			char buffer[16] = {};
			std::snprintf(buffer, sizeof(buffer), "#%02X%02X%02X%02X",
				channel(color.r), channel(color.g), channel(color.b), channel(color.a));
			return buffer;
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
		: m_Host(host), m_StatePath(std::string(WLD_LOCAL_DIR) + "wui-properties.json")
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

	// ---- P4-U9:移除组件的确认模态(与"添加组件"同一套面板级模态通道)----
	void PropertiesPanel::OpenRemoveComponentConfirm(Wui::WuiContext& ctx, uint32_t componentId,
		const std::string& displayName)
	{
		m_RemovePendingId = componentId;
		m_RemovePendingName = displayName;
		ctx.SetModal(Wui::HashId("prop.remove.modal"));
		m_Host.SetPanelModalOwner(Id());
		ctx.RecordOp("properties", "remove-component-ask", displayName, std::to_string(componentId));
	}

	void PropertiesPanel::CloseRemoveComponentConfirm(Wui::WuiContext& ctx)
	{
		m_RemovePendingId = 0;
		m_RemovePendingName.clear();
		ctx.ClearModal();
		m_Host.SetPanelModalOwner(std::string());
	}

	void PropertiesPanel::DrawRemoveComponentConfirm(Wui::WuiContext& ctx, Entity entity)
	{
		const Wui::WuiTheme& theme = m_Host.Theme();
		Wui::ModalFrameDesc frameDesc;
		frameDesc.Id = Wui::HashId("prop.remove.modal");
		frameDesc.Title = Wui::Tr("panel.properties.remove_component", "Remove Component");
		frameDesc.Size = { 460.0f, 170.0f };
		Wui::WuiRect frame;
		bool escapePressed = false;
		if (!Wui::BeginModalFrame(ctx, frameDesc, &frame, &escapePressed, theme))
			return;

		// 正文说清"移除哪个 + 会丢什么":破坏性操作不能只给一个按钮。
		Wui::Label(ctx, { frame.X + 16.0f, frame.Y + 50.0f },
			Wui::Tr("panel.properties.remove_confirm_body",
				"Remove this component from the selected entity? Its settings will be lost."),
			theme.Text, 13.0f);
		Wui::LabelWithTerm(ctx, { frame.X + 16.0f, frame.Y + 72.0f }, m_RemovePendingName, std::string(),
			theme.Warning, 13.0f, theme, frame.W - 32.0f);

		const Wui::ModalButtonDesc buttons[2] = {
			{ Wui::Tr("panel.properties.remove_cancel", "Cancel"), Wui::HashId("prop.remove.cancel"), true },
			{ Wui::Tr("panel.properties.remove_confirm", "Remove"), Wui::HashId("prop.remove.confirm"), true },
		};
		const int clicked = Wui::ModalButtons(ctx, frame, buttons, 2, theme);
		bool closeRequested = false;
		if (clicked == 1)
		{
			const uint32_t componentId = m_RemovePendingId;
			const std::string displayName = m_RemovePendingName;
			const entt::entity handle = entity;
			if (entity.IsValid() && entity.HasComponent(componentId))
			{
				// 结构性改动走场景自己的延迟队列(与添加组件同一路径);移除后当前选择保留,
				// 分区列表下一帧自然少一项(m_LastSchemaNames 变化 → 重建).
				if (entity.GetScene()->DeferStructuralChange([handle, componentId](Scene& target)
					{
						Entity removed(&target, handle);
						if (removed.IsValid() && removed.HasComponent(componentId))
							removed.RemoveComponent(componentId);
					}))
					m_Host.MarkDocumentDirty();
				ctx.RecordOp("properties", "remove-component", displayName, std::to_string(componentId));
			}
			closeRequested = true;
		}
		else if (clicked == 0 || escapePressed)
			closeRequested = true;
		// 先收 overlay 再清模态态(BeginModalFrame/EndModalFrame 必须成对;模态被别处清掉时不再重复收)。
		Wui::EndModalFrame(ctx);
		if (closeRequested && ctx.Modal() == frameDesc.Id)
			CloseRemoveComponentConfirm(ctx);
	}

	// ---- VEC-F2 / VEC-H6:集合头 `↺`(复原整个集合)----
	//
	// 用户口径:①(2026-09-27「并且在恢复时不需要二次确认」)集合头 `↺` **单击即复原** ——
	// 上一轮的二次确认模态整体删除;②两级 `↺` 都只在偏离脚本默认时出现(可见性在 DrawSchemaFields)。
	// 复原语义不变:Luau 只对**这一条声明**重建子树(默认形状 + 默认值,其余属性/集合一律不动);
	// 声明拿不到 / 默认形状读不出来(FieldsUnknown)= 形状保持,只把值清成默认(不猜形状);
	// C++ 的结构化表没有数组/映射(增删只存在于脚本侧),递归把叶子清成默认即可。
	// container 是本帧正在画的那个容器(合成路径下指针在本次绘制内稳定);path = 该容器的完整名字路径
	// (调用方在请求时记下 —— 本函数是**延后**落地的,那时 m_ScriptRowPath 已经变了)。
	void PropertiesPanel::ApplyScriptCollectionReset(Wui::WuiContext& ctx, ScriptProperty& container, bool luau,
		const std::vector<std::string>& path)
	{
		if (m_ReadOnly || container.ReadOnly)
			return;
		if (luau)
		{
			const ScriptProperties::Declaration* declaration = m_ScriptDeclarationsValid
				? ResolveDeclarationPath(m_ScriptDeclarations, path) : nullptr;
			if (declaration && !declaration->FieldsUnknown && !declaration->ReadOnly)
			{
				// seed 与目标同形(名字/类型对齐),但子行清空 + 形状归属清零:
				// 合并后 = 声明的默认形状 + 每行的默认值(全"未设" ⇒ 存档整条不写)。
				ScriptProperty seed = container;
				seed.Children.clear();
				seed.ShapeFromScene = false;
				std::vector<ScriptProperty> next { std::move(seed) };
				ScriptProperties::SyncFromDeclarations(next, { *declaration });
				if (!next.empty() && next[0].Name == container.Name)
				{
					container = std::move(next[0]);
				}
				else
				{
					ResetScriptRowValues(container);
				}
			}
			else
			{
				ResetScriptRowValues(container);
			}
		}
		else
		{
			ResetScriptRowValues(container);
		}

		m_Host.MarkDocumentDirty();
		// prefab 实例里的编辑同样记进覆盖集合(路径口径与 changedFields 一致)。
		RegisterPrefabOverrides(m_ScriptInspectingEntity,
			{ m_ScriptInspectingComponentName + "." + ScriptRowPathText(path) });
		ctx.RecordOp("properties", "script-collection-reset", ScriptRowPathText(path), "applied");
	}

	bool PropertiesPanel::ApplyScriptRowDeclaredReset(const std::string& rowName)
	{
		if (!m_ScriptInspectingScriptRows || !m_ScriptInspectingLuau)
			return false;   // C++ 的字段名不会被面板改(`+`/`-` 只存在于脚本集合):走"只清值"
		Entity entity = m_ScriptInspectingEntity;
		if (!entity.IsValid() || m_ScriptInspectingComponentId == 0
			|| !entity.HasComponent(m_ScriptInspectingComponentId))
			return false;
		auto* lua = static_cast<LuauScriptComponent*>(entity.GetComponent(m_ScriptInspectingComponentId));
		if (!lua)
			return false;
		std::vector<std::string> path = m_ScriptRowPath;
		path.push_back(rowName);
		ScriptProperty* row = ResolveScriptRowPath(lua->Properties, path);
		if (!row)
			return false;
		std::vector<ScriptProperties::Declaration> declarations;
		if (!ScriptEngine::DescribeScriptDeclarations(lua->ScriptPath, declarations, nullptr, nullptr))
			return false;
		const ScriptProperties::Declaration* declaration = ResolveDeclarationPath(declarations, path);
		if (!declaration || declaration->FieldsUnknown)
			return false;
		if (declaration->Type == Schema::Kind::Object)
			return false;   // 容器行走集合头 `↺`(复原整集合),不是单项
		row->Default = declaration->Default;
		row->Value = Schema::Value {};
		return true;
	}

	// ---- P4-U13b:prefab 实例条 + 破坏性动作确认 ----
	void PropertiesPanel::RegisterPrefabOverrides(Entity entity, const std::vector<std::string>& fields)
	{
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

	PropertiesPanel::InstanceBarInfo PropertiesPanel::ResolveInstanceBar(PanelHost& host, Entity entity)
	{
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

	PropertiesPanel::InstanceBarLayout PropertiesPanel::LayoutInstanceBar(Wui::WuiContext& ctx,
		const Wui::WuiRect& rect, const InstanceBarInfo& info) const
	{
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

	void PropertiesPanel::DrawInstanceBar(Wui::WuiContext& ctx, PanelHost& host, const Wui::WuiRect& rect,
		const InstanceBarInfo& info, const InstanceBarLayout& layout)
	{
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

	void PropertiesPanel::OpenPrefabActionConfirm(Wui::WuiContext& ctx, PrefabAction action, Entity root,
		const std::string& source)
	{
		m_PrefabActionPending = action;
		m_PrefabActionRoot = root;
		m_PrefabActionSource = source;
		ctx.SetModal(Wui::HashId("prop.prefab.action.modal"));
		// 面板级模态:宿主帧初封锁整窗输入,渲染本面板前解开(与"移除组件"同一条路径)。
		m_Host.SetPanelModalOwner(Id());
		ctx.RecordOp("properties", action == PrefabAction::Apply
			? "prefab-apply-ask" : "prefab-unpack-ask", source, std::string());
	}

	void PropertiesPanel::ClosePrefabActionConfirm(Wui::WuiContext& ctx)
	{
		m_PrefabActionPending = PrefabAction::None;
		m_PrefabActionRoot = Entity();
		m_PrefabActionSource.clear();
		ctx.ClearModal();
		m_Host.SetPanelModalOwner(std::string());
	}

	void PropertiesPanel::DrawPrefabActionConfirm(Wui::WuiContext& ctx)
	{
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
			Wui::PanelBackground(ctx, sidebarRect, theme.ContentBg, 4.0f);
			// W3.5:"只裁剪、不滚动"走库件 P1c-LIB1 的 Wui::ClipScope(RAII):
			// 渲染裁剪(ClipPush/ClipPop)与裁剪栈(PushClipRect/PopClipRect)同进同出 ——
			// 比原来只发渲染命令多维护 ClipAllows(焦点环/条目剔除)一处,作用域位置逐字对齐
			// (原来 ClipPop 是本块最后一条语句,现在 = 析构点)。
			Wui::ClipScope sidebarClip(ctx, sidebarRect);
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
					Wui::PanelBackground(ctx, item, theme.ActiveBg, 3.0f);
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
			// (析构点 = 原来的 ClipPop:作用域随本块结束)
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
			// MRU 置顶并落盘(<local>/wui-properties.json)。
			TouchRecent(SchemaTypeKeyName(schema));
			// 新分区自动展开(与分区绘制读同一个持久化键),再由 OnRender 连续几帧滚到可见。
			const bool defaultOpen = schema.Id.Name == "World::LuauScriptComponent";
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
				Wui::PanelBackground(ctx, item, highlighted ? theme.ActiveBg : theme.ButtonHover, 2.0f);
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
			Wui::PanelBackground(ctx, scrollTrack, theme.PanelHeader, 3.0f);
			Wui::PanelBackground(ctx, thumb,
				(m_AddThumbDragging || ctx.IsHovered(thumb)) ? theme.Accent : theme.ButtonHover, 3.0f);
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
			// VEC-H4:空态走库件 `Wui::EmptyState`(§规则 21):标题 + 一句下一步提示,垂直居中,
			// 不再是一行贴在左上角的灰字。空态节点由库件登记(kind="empty-state")。
			Wui::EmptyState(ctx, rect,
				Wui::Tr("panel.properties.no_entity_glyph", "\u25CC"),
				Wui::Tr("panel.properties.no_entity_selected", "No entity selected"),
				Wui::Tr("panel.properties.no_entity_hint",
					"Select an entity in the hierarchy or the viewport to inspect its properties"),
				std::string(), 0, theme);
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

		// P4-U13b:实例条常驻在组件列表**最上方**(用户口径:实例的三件事不能再藏在右键菜单里)。
		// 高度先由布局算出来,下面的 Add Component 行与组件列表整体让出这一段。
		const InstanceBarInfo instanceInfo = ResolveInstanceBar(host, entity);
		float instanceBarOffset = 0.0f;
		if (instanceInfo.InInstance)
		{
			// 只读提示占一行,实例条顺延(与 Add Component 行同一套行位计算)。
			const Wui::WuiRect barRect { rect.X + 6.0f,
				rect.Y + (m_ReadOnly ? 30.0f : 8.0f),
				std::max(0.0f, rect.W - 12.0f), 0.0f };
			const InstanceBarLayout layout = LayoutInstanceBar(ctx, barRect, instanceInfo);
			DrawInstanceBar(ctx, host, { barRect.X, barRect.Y, barRect.W, layout.Height }, instanceInfo, layout);
			instanceBarOffset = layout.Height + 8.0f;
		}
		const float contentTop = kContentTop + instanceBarOffset;

		const Wui::WuiRect addButton { rect.X + 8,
			rect.Y + (m_ReadOnly ? 30.0f : 8.0f) + instanceBarOffset, 140, 24 };
		if (!m_ReadOnly && Button(ctx, Wui::HashId("prop.add"), addButton,
			Wui::Tr("panel.properties.add_component", "Add Component"), theme))
			OpenAddComponentPicker(ctx);
		// U6b:平铺菜单 → **居中模态**的搜索选择器(候选/分类/说明全部来自 schema)。
		if (!m_ReadOnly && m_AddOpen)
			DrawAddComponentPicker(ctx, entity, scene, schemas);
		// P4-U9:移除组件的确认模态(与添加组件同一套面板级模态通道)。
		if (m_RemovePendingId != 0)
			DrawRemoveComponentConfirm(ctx, entity);
		// P4-U13b:实例破坏性动作(应用到资产 / 断开链接)的确认模态。
		if (m_PrefabActionPending != PrefabAction::None)
			DrawPrefabActionConfirm(ctx);
		// VEC-H6:集合头 `↺` 的二次确认已删除 —— 单击即复原(见 ApplyScriptCollectionReset)。

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
				const bool defaultOpen = schema->Id.Name == "World::LuauScriptComponent";
				m_Sections.push_back({ schema->DisplayName, defaultOpen, 0.0f });
			}
		}

		// ---- 滚动布局(与迁移前一致的分区顺序;标题/展开态由面板持久化)----
		// 内容高度取上一帧实测值(首帧按 0 计),分区每帧重绘,下一帧即精确。
		const float viewportHeight = std::max(0.0f, rect.H - contentTop - 4.0f);
		float contentHeight = 0.0f;
		// U6:添加组件后要滚到可见的分区顶部(标题行位置)。
		float revealTargetY = -1.0f;
		float sectionTop = 0.0f;
		for (size_t i = 0; i < m_Sections.size(); ++i)
		{
			if (i >= componentSchemas.size())
				break;
			const Schema::TypeSchema* schema = componentSchemas[i];
			const bool defaultOpen = schema->Id.Name == "World::LuauScriptComponent";
			bool& open = ctx.Persist<bool>(Wui::HashId(("prop.open." + schema->DisplayName).c_str()), defaultOpen);
			m_Sections[i].Open = open;
			if (!m_RevealSection.empty() && m_Sections[i].Title == m_RevealSection)
				revealTargetY = sectionTop;
			contentHeight += kSectionHeader + (open ? m_Sections[i].ContentHeight : 0.0f) + kSectionGap;
			sectionTop += kSectionHeader + (open ? m_Sections[i].ContentHeight : 0.0f) + kSectionGap;
		}

		const Wui::WuiRect scrollViewport { rect.X + 6, rect.Y + contentTop, rect.W - 12 - kScrollbarWidth, viewportHeight };
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
		const Wui::WuiRect contentRect { rect.X + 6, rect.Y + contentTop - scroll.Offset,
			rect.W - 12 - kScrollbarWidth, contentHeight };
		float sectionY = 0.0f;
		// W3.5:分区滚动区的裁剪改走库件 P1c-LIB1 的 Wui::ClipScope(RAII);作用域 = 原来的
		// ClipPush…ClipPop,额外维护的 ClipAllows 让"滚出视口的控件不画焦点环"这条语义在这里
		// 同样成立(本作用域内有控件绘制,实测差异见本片报告 §3)。
		{
			Wui::ClipScope sectionScrollClip(ctx, scrollViewport);
			for (size_t i = 0; i < m_Sections.size(); ++i)
			{
				if (i >= componentSchemas.size())
					break;
				const Schema::TypeSchema* schema = componentSchemas[i];
				SectionEntry& section = m_Sections[i];
				// 分区顺序可能因 schema 列表变化而与 m_Sections 错位:名字不同则本帧跳过绘制。
				if (section.Title != schema->DisplayName)
					break;
				const bool defaultOpen = schema->Id.Name == "World::LuauScriptComponent";
				bool& open = ctx.Persist<bool>(Wui::HashId(("prop.open." + schema->DisplayName).c_str()), defaultOpen);

				// 标题行:与 WuiSection 相同的底色/文字与展开行为;同时登记为可点节点
				// (properties.section.<DisplayName>),脚本可展开/折叠分区。
				const float rowY = contentRect.Y + sectionY;
				const Wui::WuiRect header { contentRect.X, rowY, contentRect.W, kSectionHeader };
				// VEC-H2:分区标题走库件 `Wui::PropertyGroupHeader`(展开标记 + 术语 + 悬停 Doc +
				// 行尾移除按钮;底色用主题令牌的 ActiveBg/PanelHeader/HoverBg,不再是手写灰度)。
				// 无障碍 id 与文案不变:id=HashId('properties.section.<DisplayName>')、kind=button、
				// value=open/closed;移除按钮 id=HashId('properties.section.remove.<DisplayName>')。
				const std::string headerId = "properties.section." + section.Title;
				std::string blockedReason;
				const bool core = schema->Core;
				const bool canRemove = !m_ReadOnly && !core
					&& entity.CanRemoveComponent(schema->Storage->ComponentId, &blockedReason);
				if (core)
					blockedReason = Wui::Tr("panel.properties.remove.core",
						"Core components cannot be removed");
				else if (m_ReadOnly)
					blockedReason = Wui::Tr("panel.properties.remove.readonly",
						"Read-only while Play/Simulate is running");
				const std::string removeHint = canRemove
					? Wui::Tr("panel.properties.remove.tooltip", "Remove this component")
					: blockedReason;
				const Wui::LocalizedLabel sectionLabel = SchemaComponentLabel(*schema);
				Wui::PropertyGroupHeaderDesc sectionHead;
				sectionHead.Label = sectionLabel.Text;
				sectionHead.Term = sectionLabel.Term;
				sectionHead.Tooltip = ComponentDocLabel(*schema);
				sectionHead.A11yLabel = TermText(sectionLabel);
				sectionHead.Open = open;
				sectionHead.ShowRemove = true;
				sectionHead.RemoveEnabled = canRemove;
				sectionHead.RemoveId = Wui::HashId(("properties.section.remove." + section.Title).c_str());
				sectionHead.RemoveLabel = Wui::Tr("panel.properties.remove_component", "Remove Component");
				sectionHead.RemoveTooltip = removeHint;
				const Wui::PropertyGroupHeaderResult sectionHeader =
					Wui::PropertyGroupHeader(ctx, Wui::HashId(headerId.c_str()), header, sectionHead, theme);
				if (sectionHeader.Toggled)
				{
					open = !open;
					section.Open = open;
					ctx.RecordOp("properties", "toggle-section", section.Title, open ? "open" : "closed");
				}
				if (sectionHeader.RemoveClicked)
					OpenRemoveComponentConfirm(ctx, schema->Storage->ComponentId, schema->DisplayName);

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
			// (析构点 = 原来的 ClipPop:作用域随本块结束)
		}

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

			// 轨道/上下按钮/滑块底色与箭头字形都走库原语;状态机(点击翻页 + 拖动定位 + 稳定
			// a11y id)仍在本面板 —— "有状态滚动条"是缺件,见报告。
			Wui::PanelBackground(ctx, track, theme.PanelHeader, 2.0f);
			Wui::PanelBackground(ctx, upButton, ctx.IsHovered(upButton) ? theme.ButtonHover : theme.ButtonBg, 2.0f);
			Wui::PanelBackground(ctx, downButton, ctx.IsHovered(downButton) ? theme.ButtonHover : theme.ButtonBg, 2.0f);
			Wui::PanelBackground(ctx, thumb, theme.Accent, 2.0f);
			Wui::Label(ctx, { upButton.X + 1, upButton.Y + 4 }, "^", theme.Text, 13.0f);
			Wui::Label(ctx, { downButton.X + 1, downButton.Y + 4 }, "v", theme.Text, 13.0f);

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
		const Wui::WuiRect& visibleRect, std::vector<std::string>* changedFields, bool scriptPropertyRow,
		ScriptCollectionRows* collectionRows, int depth)
	{
		const Wui::WuiTheme& theme = m_Host.Theme();
		float y = 0;
		bool changed = false;
		// VEC-H2:标签列宽来自库件(与 PropertyRow/PropertyGroupHeader 共用同一条口径),
		// 同一面板传同一值 ⇒ 标签列竖向对齐;面板不再自己算 0.45 倍。
		const float labelWidth = Wui::PropertyRowLabelWidth(rect);
		// VEC-H4:层级 = 标签文字缩进(每层 12px,4px 栅格)。**值列不跟着挪** —— 面板把同一个
		// labelWidth 传进每一行,缩进只作用在标签文字上,所以任意深度的字段列仍然竖向对齐。
		constexpr float kIndentPerLevel = 12.0f;
		const float labelIndent = static_cast<float>(depth) * kIndentPerLevel;
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
		// VEC-C2:数组/映射元素行的行尾 `-`(删除)。**只登记"这一行要删"**,真正的 erase /
		// 重排在本调用画完所有元素行之后统一做 —— 合成 schema 的元素访问器按编译期下标实例化,
		// 循环中途 erase 会让后面的行读到错元素(同帧一帧错位)。
		// 只读态不画增删控件(Play 里这些行本来就不出现;这条兜住 C++ 实例只读展示的路径)。
		const bool collectionWritable = collectionRows != nullptr && collectionRows->Writable
			&& collectionRows->Container != nullptr;
		std::string pendingEraseName;
		// VEC-H2:行尾 `-` 走库件 `Wui::CollectionActionButton`(方按钮 + 悬停/焦点/禁用态),
		// 落点 = 行矩形右侧的行外动作槽(容器已按 `CollectionActionColumnWidth()` 收窄子行)。
		const auto drawCollectionRemove = [&](const std::string& rowName, const Wui::WuiRect& row)
		{
			if (!collectionWritable)
				return;
			const Wui::WuiRect button { row.X + row.W + 2.0f, row.Y + (row.H - kRowHeight) * 0.5f, kRowHeight,
				kRowHeight };
			if (Wui::CollectionActionButton(ctx,
				Wui::HashId((collectionRows->IdText + ".remove." + rowName).c_str()), button, "-",
				Wui::Tr("panel.properties.collection_remove.tooltip",
					"Remove this element from the collection (the scene stores the list)"), true, theme))
				pendingEraseName = rowName;
		};
		// SCRIPT-V2:脚本属性行的说明 = 脚本自己的注释(`ScriptProperty::Doc`,由调用方带进
		// `FieldSchema.Meta.Doc`)—— **不**回落 schema 的 `schema.field.<Name>.doc`;没写说明就回落
		// 类型文案(VEC-C2 / v4 §1),不假装有文档。普通 schema 字段行为不变(仍走 schema 本地化表)。
		const auto fieldDocFor = [&](const Schema::FieldSchema& candidate)
		{
			if (!scriptPropertyRow)
				return FieldDocLabel(schema, candidate);
			if (!candidate.Meta.Doc.empty())
				return candidate.Meta.Doc;
			// VEC-C2(方案 v4 §1):没有注解说明 → 回落成**类型文案**,不再给英文兜底文案。
			// 推导字段(`level` 这类)与数组/映射子行同样走这一条。
			return ScriptFieldTypeText(candidate);
		};
		// VEC-H6:当前行的声明节点(名字路径 = m_ScriptRowPath + 行名)。声明读不出来 = nullptr ——
		// 复位可见性退化成"只看值/Default",不猜形状。
		const auto scriptRowDeclaration = [this](const std::string& rowName)
			-> const ScriptProperties::Declaration*
		{
			if (!m_ScriptDeclarationsValid)
				return nullptr;
			std::vector<std::string> path = m_ScriptRowPath;
			path.push_back(rowName);
			return ResolveDeclarationPath(m_ScriptDeclarations, path);
		};
		for (const Schema::FieldSchema& field : schema.Fields)
		{
			if (field.Meta.Transient)
				continue;
			const Wui::WuiId fid = Wui::HashId(("f." + typeName + "." + field.Name).c_str()) ^ base;
			const std::string rowIdText = propId(typeName, field.Name);
			const Wui::WuiId rowNodeId = Wui::HashId(rowIdText.c_str());
			// VEC-C2 / VEC-F2:**单项** `↺` 复位(叶子 / 数组元素 / 映射值行)。`modified` 语义 =
			// "编辑态可复位":Play/只读态用同一 rect 画禁用占位(库件两态共用同一几何,行布局零位移)。
			// 点中后只把**这一行**清成"未设" —— 显示由 `ScriptPropertyDisplayValue` 回落脚本默认值,
			// 存档按 D1 判定"未设不写";不再触发整表重同步(那会把同一集合的增删按声明重建 =
			// 用户反馈的"点一个元素把整个集合都复原了")。
			const Wui::WuiId resetId = Wui::HashId((rowIdText + ".reset").c_str());
			const bool resetEnabled = scriptPropertyRow && !m_ReadOnly && !field.Meta.ReadOnly;
			const std::string resetLabel = Wui::Tr("panel.properties.script_reset",
				"Reset this item to the script default");
			const std::string resetDoc = resetEnabled
				? Wui::Tr("panel.properties.script_reset.tooltip", ScriptItemResetDoc())
				: Wui::Tr("panel.properties.script_readonly_notice",
					"Play/Simulate: script properties are read-only (pause or stop to edit)");
			// VEC-H6:`↺` 只在"当前值/形状 != 脚本声明默认"时出现(用户口径:一致时不画,不是禁用态)。
			// 合成路径(编辑态 + Luau 只读)拿得到 ScriptProperty;Play 里的 C++ 实例走真实结构体指针,
			// 没有 Value/Default 可比 —— 保持既有"画禁用占位 + 理由"的口径(判据不可用时不去猜)。
			const size_t fieldIndex = static_cast<size_t>(&field - schema.Fields.data());
			const ScriptProperty* scriptRow = (scriptPropertyRow && m_ScriptInspectingScriptRows)
				? ScriptRowModel(schema, instance, fieldIndex) : nullptr;
			const bool resetModified = scriptRow
				? ScriptRowModified(*scriptRow, scriptRowDeclaration(field.Name)) : true;
			const auto applyItemReset = [&]()
			{
				if (!field.Set)
					return;
				// VEC-F2:单项复位 = 该行回到"未设",显示回落**脚本当前声明**里同名位置的默认值。
				// Luau 的数组在面板里 `+`/`-` 后会重排下标,行的旧 Default 会留在改名后的行上 ——
				// 所以先按声明刷新这一行的 Default;声明拿不到(C++ / 无 VM)才退回"只清 Value"。
				const bool declared = scriptPropertyRow && ApplyScriptRowDeclaredReset(field.Name);
				if (!declared)
					field.Set(instance, Schema::Value {});
				changed = true;
				if (changedFields)
					changedFields->push_back(typeName + "." + field.Name);
			};
			// 显示文案:普通字段 = Meta.DisplayName 优先,空则人类可读化 C++ 字段名后查目录;
			// **脚本属性行 = 脚本里的原始字段名,一律不过本地化表** —— 脚本字段不是 schema 字段,
			// 同名查 `schema.field.*` 会串台(用户实测:脚本字段 `Speed` 显示成别的组件的「速度」)。
			// 行 id(fid / propId)与持久化仍用 field.Name,不受影响。
			const Wui::LocalizedLabel label = scriptPropertyRow
				? Wui::LocalizedLabel { field.Name, std::string() }
				: SchemaFieldLabel(field);
			// 无障碍节点 label 按约定写成 "中文 (English)";显示值/句子本身不加英文。
			const std::string labelText = TermText(label);

			if (field.K == Schema::Kind::Object)
			{
				const std::string& idText = rowIdText;
				const Schema::TypeSchema* nested = field.GetNested ? field.GetNested() : nullptr;
				void* nestedInstance = field.GetPtr ? field.GetPtr(instance) : nullptr;
				// VEC-B3:脚本属性里的裸 table / 面板侧降级的结构化表 = 只读摘要行。
				// 这一支只对脚本属性行开放(`scriptPropertyRow`),普通 schema 的 Object 字段
				// 行为不变;摘要行不可展开、不可编辑、不进存档(值根本没有合成到这里)。
				if (scriptPropertyRow && (!nested || !nestedInstance))
				{
					const std::string summary = field.Meta.DisplayName.empty()
						? std::string("table") : field.Meta.DisplayName;
					const std::string summaryDoc = fieldDocFor(field);
					// 只读摘要行:行结构走属性行库件(内联值 + 悬停说明 + 行尾动作列),文本形态与旧口径一致。
					const Wui::WuiRect row { rect.X, rect.Y + y, rect.W, kRowHeight };
					Wui::PropertyRowDesc desc;
					desc.Label = label.Text;
					desc.Term = label.Term;
					desc.Tooltip = summaryDoc;
					desc.A11yKind = "text";
					desc.A11yLabel = labelText;
					desc.A11yValue = summary;
					desc.A11yEnabled = false;
					desc.InlineValue = true;
					desc.InlineValueText = summary;
					desc.LabelWidth = labelWidth;
					desc.LabelIndent = labelIndent;
					Wui::PropertyRow(ctx, rowNodeId, row, desc, theme);
					drawCollectionRemove(field.Name, row);
					y += kRowHeight;
					continue;
				}
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
					const Wui::WuiRect row { rect.X, rect.Y + y, rect.W, kRowHeight };
					Wui::PropertyRowDesc desc;
					desc.Label = label.Text;
					desc.Term = label.Term;
					desc.A11yKind = "text";
					desc.A11yLabel = labelText;
					desc.A11yValue = display;
					desc.A11yEnabled = false;
					desc.InlineValue = true;
					desc.InlineValueText = display;
					desc.LabelWidth = labelWidth;
					desc.LabelIndent = labelIndent;
					Wui::PropertyRow(ctx, rowNodeId, row, desc, theme);
					drawCollectionRemove(field.Name, row);
					y += kRowHeight;
					continue;
				}
				// VEC-H2:折叠分组头走库件 `Wui::PropertyGroupHeader`(展开标记 + 标签 + 悬停说明 +
				// 行尾集合复位;点复位不折叠)。语义与 id 契约与旧实现逐条一致。
				const Wui::WuiRect row { rect.X, rect.Y + y, rect.W, kRowHeight };
				bool& open = ctx.Persist<bool>(fid, false);
				// VEC-F2:集合头 `↺`(数组 / 映射 / 结构化表的折叠头行)= 复原**整个集合** ——
				// 与单项 `↺` 同一图标,但文案/说明说清"整集合 + 丢弃所有增删改";VEC-H6 起
				// **单击即复原**(二次确认已删除),且只在集合偏离默认时出现(见 head.ResetModified)。
				// 只读/Play 画禁用占位并给只读理由(与叶子行同一套 disabled hint 口径)。
				const bool headResetDrawn = scriptPropertyRow && nested != nullptr && nestedInstance != nullptr;
				// `nestedInstance` 只有在合成属性表路径上才是 `ScriptProperty*`(Play 里 C++ 实例走真实
				// 结构体指针)→ 形态/复位只对合成路径成立;Play 那一路只画禁用占位。
				const bool headScriptRow = headResetDrawn && m_ScriptInspectingScriptRows;
				const bool headResettable = headScriptRow && !m_ReadOnly && !field.Meta.ReadOnly;
				// 集合头 `↺` 的 id 契约:`properties.<组件>.<属性>.reset`(与单项同一字符串,
				// 差别只在落点:集合头落在容器行,单项落在元素/键值行)。
				ScriptPropertyCollection headKind = ScriptPropertyCollection::Struct;
				if (headScriptRow)
					headKind = static_cast<const ScriptProperty*>(nestedInstance)->Collection;
				const std::string headLabel = ScriptCollectionHeadResetLabel(headKind);
				const std::string headDoc = headResettable
					? std::string(ScriptCollectionHeadResetDoc())
					: Wui::Tr("panel.properties.script_readonly_notice",
						"Play/Simulate: script properties are read-only (pause or stop to edit)");
				Wui::PropertyGroupHeaderDesc head;
				head.Label = label.Text;
				head.Term = label.Term;
				head.Tooltip = fieldDocFor(field);
				head.LabelWidth = labelWidth;
				head.LabelIndent = labelIndent;
				// VEC-H4:集合头右侧常驻"当前条数"(数组/映射/结构化表都算),折叠时也知道里面有几项。
				if (headScriptRow && !field.Meta.ReadOnly)
				{
					const size_t childCount = static_cast<const ScriptProperty*>(nestedInstance)->Children.size();
					head.Trailing = Wui::Tr("panel.properties.collection_count", "{n} item(s)");
					const std::string marker = "{n}";
					const size_t at = head.Trailing.find(marker);
					if (at != std::string::npos)
						head.Trailing.replace(at, marker.size(), std::to_string(childCount));
					else
						head.Trailing = std::to_string(childCount);
				}
				head.A11yLabel = labelText;
				head.Open = open;
				head.Enabled = reachable(row);
				head.ShowReset = headResetDrawn;
				head.ResetEnabled = headResettable;
				// VEC-H6:集合头 `↺` 只在"有任一元素/键值/形状偏离默认"时出现。
				head.ResetModified = resetModified;
				head.ResetId = resetId;
				head.ResetLabel = headLabel;
				head.ResetTooltip = headDoc;
				const Wui::PropertyGroupHeaderResult header =
					Wui::PropertyGroupHeader(ctx, rowNodeId, row, head, theme);
				if (header.Toggled)
					open = !open;
				if (header.ResetClicked && headResettable)
				{
					// VEC-H6:记下请求,等本组件画完再落地(同一帧后面的子行仍按旧 schema 画)。
					m_PendingCollectionReset = static_cast<ScriptProperty*>(nestedInstance);
					m_PendingCollectionResetLuau = m_ScriptInspectingLuau;
					m_PendingCollectionResetPath = m_ScriptRowPath;
					m_PendingCollectionResetPath.push_back(field.Name);
				}
				drawCollectionRemove(field.Name, row);
				y += kRowHeight;
				if (open && nested && nestedInstance)
				{
					// VEC-C2:元素自身是数组/映射(嵌套集合,如 `{{number}}`)时,它的子行也要能增删;
					// 普通结构化表的子行不是集合行 —— 集合形态只从合成 arena 查(`None` = 不传上下文)。
					ScriptCollectionRows nestedRows;
					ScriptCollectionRows* nestedRowsPtr = nullptr;
					if (scriptPropertyRow)
					{
						const ScriptPropertyCollection nestedCollection = ScriptTableCollectionOf(nested);
						if (nestedCollection == ScriptPropertyCollection::Array
							|| nestedCollection == ScriptPropertyCollection::Map)
						{
							nestedRows.Container = static_cast<ScriptProperty*>(nestedInstance);
							nestedRows.Kind = nestedCollection;
							nestedRows.IdText = idText;
							nestedRows.Writable = !m_ReadOnly;
							nestedRowsPtr = &nestedRows;
						}
					}
					const float actionReserve = nestedRowsPtr ? kCollectionActionWidth : 0.0f;
					if (m_ScriptInspectingScriptRows)
						m_ScriptRowPath.push_back(field.Name);   // 子行路径 = 容器路径 + 本行名
					y += DrawSchemaFields(ctx, fid ^ 0x9e3779b9u,
						{ row.X, row.Y + kRowHeight, std::max(40.0f, row.W - actionReserve), 0 },
						nestedInstance, nested->DisplayName, *nested, visibleRect, changedFields,
						scriptPropertyRow, nestedRowsPtr, depth + 1);
					if (m_ScriptInspectingScriptRows)
						m_ScriptRowPath.pop_back();
				}
				continue;
			}

			if (m_ReadOnly || field.Meta.ReadOnly || !field.Get || !field.Set)
			{
				const std::string docText = fieldDocFor(field);
				// 只读也要显示"值":否则 Play/Simulate 下属性面板只剩字段名,看起来像"什么都不显示"。
				const std::string display = field.Get ? FormatReadOnlyValue(field, field.Get(instance)) : std::string();
				// VEC-H2:只读行也走属性行库件(内联「标签: 值」+ 不可交互文本节点 + 禁用复位占位)。
				const Wui::WuiRect row { rect.X, rect.Y + y, rect.W, kRowHeight };
				Wui::PropertyRowDesc desc;
				desc.Label = label.Text;
				desc.Term = label.Term;
				desc.Tooltip = docText;
				desc.A11yKind = "text";
				desc.A11yLabel = labelText;
				desc.A11yValue = display;
				desc.A11yEnabled = false;
				desc.InlineValue = true;
				desc.InlineValueText = display;
				desc.LabelWidth = labelWidth;
				desc.LabelIndent = labelIndent;
				// VEC-H4:禁用/只读行的 tooltip 必须给出**理由**(Blender HIG 的 disabled hint):
				// 先写用途,再写为什么现在是灰的。理由与面板顶部那行只读说明同源。
				if (m_ReadOnly || field.Meta.ReadOnly)
				{
					const std::string reason = m_ReadOnly
						? Wui::Tr("panel.properties.readonly_notice",
							"Play/Simulate running: read-only (pause or exit to edit)")
						: (scriptPropertyRow && ScriptProperties::IsSummaryKind(field.K)
							// CPPT-3:IVec*/UVec*/Quat/Mat* —— 面板没有行控件;值不进属性表/不进存档,
							// 这里给"为什么只读"的可读理由(禁用必须能解释原因)。
							? Wui::Tr("panel.properties.script_summary_readonly",
								"This field type has no editor control yet — shown as a read-only "
								"summary and not saved here")
							: Wui::Tr("panel.properties.field_core_readonly",
								"Core field: read-only by design, it cannot be edited here"));
					desc.Tooltip = docText.empty() ? reason : (docText + "\n" + reason);
				}
				// 只读态:复位按钮可见但禁用(disabled hint = 面板顶部那行"Play/Simulate 只读"说明)。
				desc.ShowReset = scriptPropertyRow;
				desc.ResetEnabled = false;
				desc.ResetModified = resetModified;
				desc.ResetId = resetId;
				desc.ResetLabel = resetLabel;
				desc.ResetTooltip = resetDoc;
				Wui::PropertyRow(ctx, rowNodeId, row, desc, theme);
				y += kRowHeight;
				continue;
			}

			// 交互字段:标签登记为静态节点(不可点),控件本体按真实 kind 登记
			// (脚本用 properties.<Type>.<Field> 直接 ui.invoke)。
			const std::string& idText = rowIdText;
			// P4-U9:字段说明(如果有)—— 悬停提示 + 无障碍节点 Tooltip。
			const std::string fieldDoc = fieldDocFor(field);
			// VEC-H2:行结构(标签列 + 值列 + 悬停说明 + 行尾复位)走库件;面板只把控件画进 FieldRect。
			// 向量行(非颜色)是多行控件:先"只算不画"拿值列宽 → 决定排布与行高,再画行。
			const bool vectorRow = (field.K == Schema::Kind::Vec2 || field.K == Schema::Kind::Vec3
				|| field.K == Schema::Kind::Vec4) && !(field.Meta.Color && field.K != Schema::Kind::Vec2);
			const int vectorComponents = field.K == Schema::Kind::Vec2 ? 2
				: (field.K == Schema::Kind::Vec3 ? 3 : 4);
			const Wui::PropertyRowLayout measured = Wui::MeasurePropertyRow(
				{ rect.X, rect.Y + y, rect.W, kRowHeight }, labelWidth, scriptPropertyRow);
			float rowHeight = kRowHeight;
			if (vectorRow)
				rowHeight = VecFieldHeight(VecFieldLayout(measured.Field.W), vectorComponents);
			const Wui::WuiRect row { rect.X, rect.Y + y, rect.W, rowHeight };
			// VEC-H4:行值先取(纯读),用来判定"值不可用"(脚本行 + 存的值类型与声明不符)。
			Schema::Value value = field.Get(instance);
			const bool valueUnavailable = scriptPropertyRow && field.K != Schema::Kind::Object
				&& std::holds_alternative<std::monostate>(value);
			// 不可用 = 值列给 "—"(多值/不可用的统一表达,反模式 4:不显示伪零);原因进 tooltip;
			// 不画字段控件、不写盘。修复路径:改脚本声明或场景里的那一条值。
			const std::string unavailableDoc = Wui::Tr("panel.properties.value_unavailable.tooltip",
				"The stored value's type does not match the script declaration, so it is not editable here");
			Wui::WuiRect ctrl;
			if (collectionWritable)
			{
				// VEC-H2:数组元素 / 映射键值行走库件 `Wui::CollectionRow` —— 行内 `↺`(单项复位)
				// 与行外 `-`(删除这条元素)都是库件的动作按钮,面板只消费事件(删除延迟到收口统一做)。
				Wui::CollectionRowDesc element;
				element.Label = label.Text;
				element.Term = label.Term;
				element.Tooltip = fieldDoc;
				element.A11yKind = "label";
				element.A11yLabel = labelText;
				element.A11yEnabled = false;
				element.LabelWidth = labelWidth;
				// 元素行比容器头再深一层(H4 §规则 2:12px/层,只挪标签,值列仍对齐)。
				element.LabelIndent = labelIndent + kIndentPerLevel;
				if (valueUnavailable)
				{
					element.FieldPlaceholder = Wui::Tr("panel.properties.value_mixed", "\u2014");
					element.Tooltip = unavailableDoc;
				}
				element.FieldHeight = vectorRow ? rowHeight : 0.0f;
				element.ActionsOutside = true;
				element.ShowReset = scriptPropertyRow;
				element.ResetEnabled = resetEnabled;
				element.ResetModified = resetModified;
				element.ResetId = resetId;
				element.ResetLabel = resetLabel;
				element.ResetTooltip = resetDoc;
				element.ShowRemove = true;
				element.RemoveId = Wui::HashId((collectionRows->IdText + ".remove." + field.Name).c_str());
				element.RemoveTooltip = Wui::Tr("panel.properties.collection_remove.tooltip",
					"Remove this element from the collection (the scene stores the list)");
				const Wui::CollectionRowResult elementRow =
					Wui::CollectionRow(ctx, rowNodeId, row, element, theme);
				if (elementRow.ResetClicked)
					applyItemReset();
				if (elementRow.RemoveClicked)
					pendingEraseName = field.Name;
				ctrl = elementRow.FieldRect;
			}
			else
			{
				Wui::PropertyRowDesc rowDesc;
				rowDesc.Label = label.Text;
				rowDesc.Term = label.Term;
				rowDesc.Tooltip = fieldDoc;
				rowDesc.A11yKind = "label";
				rowDesc.A11yLabel = labelText;
				rowDesc.A11yEnabled = false;      // 行标签不可交互;控件本体登记自己的节点
				rowDesc.LabelWidth = labelWidth;
				rowDesc.LabelIndent = labelIndent;
				if (valueUnavailable)
				{
					rowDesc.FieldPlaceholder = Wui::Tr("panel.properties.value_mixed", "\u2014");
					rowDesc.Tooltip = unavailableDoc;
					rowDesc.A11yValue = "unavailable";
				}
				rowDesc.FieldHeight = vectorRow ? rowHeight : 0.0f;
				rowDesc.ShowReset = scriptPropertyRow;
				rowDesc.ResetEnabled = resetEnabled;
				rowDesc.ResetModified = resetModified;
				rowDesc.ResetId = resetId;
				rowDesc.ResetLabel = resetLabel;
				rowDesc.ResetTooltip = resetDoc;
				const Wui::PropertyRowResult rowResult = Wui::PropertyRow(ctx, rowNodeId, row, rowDesc, theme);
				if (rowResult.ResetClicked)
					applyItemReset();
				ctrl = rowResult.FieldRect;
			}
			bool fieldChanged = false;
			// 本行推进量:普通行 = 库件行高(24);向量行按库件排布给足高度(见 VecFieldHeight)。
			float rowAdvance = rowHeight;
			if (valueUnavailable)
			{
				// 值不可用:值列已经画了 "—"(上面的行控件),这里不画字段控件、不写盘,只推进布局。
				y += rowAdvance;
				continue;
			}
			switch (field.K)
			{
				case Schema::Kind::Bool:
				{
					bool b = std::get<bool>(value);
					const bool before = b;
					Checkbox(ctx, fid, ctrl, "", b, theme);
					fieldChanged = b != before;
					if (fieldChanged) value = b;
					RegisterNode(Wui::HashId(idText.c_str()), "checkbox", ctrl, labelText, b ? "true" : "false",
						reachable(ctrl), fieldDoc);
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
					// CPPT-3-FIX1:声明 Unit 的行把行后缀接进库件数值样式(`WuiNumberStyle.Unit`)。
					const bool unitRow = !field.Meta.Unit.empty();
					DrawScriptIntControl(ctx, fid, ctrl, raw, lo, hi, field.Meta, theme);
					fieldChanged = raw != before;
					if (fieldChanged)
					{
						if (field.K == Schema::Kind::Int8) value = static_cast<int8_t>(raw);
						else if (field.K == Schema::Kind::Int16) value = static_cast<int16_t>(raw);
						else if (field.K == Schema::Kind::Int32) value = static_cast<int32_t>(raw);
						else value = raw;
					}
					RegisterNode(Wui::HashId(idText.c_str()), unitRow ? "number-field" : "drag-int", ctrl, labelText,
						unitRow ? (std::to_string(raw) + " " + field.Meta.Unit) : std::to_string(raw),
						reachable(ctrl), fieldDoc);
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
					// CPPT-3-FIX1:与有符号分支同一口径(Unit → NumberFieldInt + 单位后缀)。
					const bool unitRow = !field.Meta.Unit.empty();
					DrawScriptIntControl(ctx, fid, ctrl, signedRaw, lo, hi, field.Meta, theme);
					fieldChanged = signedRaw != before;
					if (fieldChanged)
					{
						if (field.K == Schema::Kind::UInt8) value = static_cast<uint8_t>(signedRaw);
						else if (field.K == Schema::Kind::UInt16) value = static_cast<uint16_t>(signedRaw);
						else if (field.K == Schema::Kind::UInt32) value = static_cast<uint32_t>(signedRaw);
						else value = static_cast<uint64_t>(signedRaw);
					}
					RegisterNode(Wui::HashId(idText.c_str()), unitRow ? "number-field" : "drag-int", ctrl, labelText,
						unitRow ? (std::to_string(signedRaw) + " " + field.Meta.Unit) : std::to_string(signedRaw),
						reachable(ctrl), fieldDoc);
					break;
				}
				case Schema::Kind::Float:
				case Schema::Kind::Double:
				{
					float f = field.K == Schema::Kind::Float ? std::get<float>(value) : static_cast<float>(std::get<double>(value));
					const float before = f;
					const float lo = field.Meta.Min.has_value() ? *field.Meta.Min : 1.0f;   // 1,-1 哨兵 = 无范围
					const float hi = field.Meta.Max.has_value() ? *field.Meta.Max : -1.0f;
					// CPPT-3-FIX1:声明 Unit 的浮点行把行后缀接进既有 WUI 数值样式(`WuiNumberStyle.Unit`;
					// DragBarFloat = 值区固定宽度 + 单位右对齐,与材质预览的角度行同一件)。
					// 只在**声明了有效范围**时改走 DragBarFloat:它的条体拖动按值域映射(无范围时内部用
					// ±1e30 哨兵,拖一次就冲出量程)—— 无范围的带单位行保持 DragFloat,拖拽/方向键步长 = Step。
					const std::string& unit = field.Meta.Unit;
					const int decimals = StepDecimals(field.Meta.Step.has_value(),
						field.Meta.Step.has_value() ? *field.Meta.Step : 0.0f);
					const float speed = (field.Meta.Step.has_value() && *field.Meta.Step > 0.0f)
						? *field.Meta.Step : 0.01f;
					if (!unit.empty() && lo < hi)
					{
						Wui::WuiNumberStyle style;
						style.Unit = unit.c_str();
						style.Decimals = decimals;
						style.ValueWidth = 68.0f;   // "-1000.0 hp" 这类值 + 单位也能完整显示
						Wui::DragBarFloat(ctx, fid, ctrl, f, lo, hi, theme, style);
					}
					else
					{
						DragFloat(ctx, fid, ctrl, f, speed, lo, hi, theme);
					}
					fieldChanged = f != before;
					if (fieldChanged) value = field.K == Schema::Kind::Float ? Schema::Value(f) : Schema::Value(static_cast<double>(f));
					if (!unit.empty() && lo < hi)
						RegisterNode(Wui::HashId(idText.c_str()), "slider", ctrl, labelText,
							FormatFloatText(f, decimals) + " " + unit, reachable(ctrl), fieldDoc);
					else
						RegisterNode(Wui::HashId(idText.c_str()), "drag-float", ctrl, labelText,
							FormatFloatText(f), reachable(ctrl), fieldDoc);
					break;
				}
				case Schema::Kind::Vec2:
				case Schema::Kind::Vec3:
				case Schema::Kind::Vec4:
				{
					// P4-U9:Color() 标记的向量 = 颜色 → 取色器(色块 + hex + R/G/B/A 滑杆 + 预设),
					// 不再让用户对着四个数字框猜颜色。
					if (field.Meta.Color && field.K != Schema::Kind::Vec2)
					{
						const glm::vec4 before = field.K == Schema::Kind::Vec3
							? glm::vec4 { std::get<glm::vec3>(value), 1.0f }
							: std::get<glm::vec4>(value);
						glm::vec4 edited = before;
						Wui::ColorField(ctx, fid, ctrl, edited, theme);
						if (edited != before)
						{
							fieldChanged = true;
							value = field.K == Schema::Kind::Vec3
								? Schema::Value(glm::vec3 { edited.x, edited.y, edited.z })
								: Schema::Value(edited);
						}
						RegisterNode(Wui::HashId(idText.c_str()), "color", ctrl, labelText,
							FormatColorHexText(edited), reachable(ctrl), fieldDoc);
						break;
					}
					const int components = field.K == Schema::Kind::Vec2 ? 2 : (field.K == Schema::Kind::Vec3 ? 3 : 4);
					// VEC-A4:向量行 = 库件 `Vec2Field`/`Vec3Field`/`Vec4Field`(与材质编辑器的
					// 参数行同一个控件:轴标签 + 数值区 + 拖动/键入/↑↓),不再逐分量手拼 DragFloat。
					// 行 id 仍是 `properties.<组件>.<字段>`;分量节点由库件登记为 `...axis.0/1/2/3`
					// (不再手写 `.x/.y/.z/.w` 节点)。
					const int layout = VecFieldLayout(ctrl.W);
					const float fieldHeight = VecFieldHeight(layout, components);
					const Wui::WuiRect fieldRect { ctrl.X, ctrl.Y, ctrl.W, fieldHeight };
					const Wui::WuiId rowId = Wui::HashId(idText.c_str());
					if (components == 2)
					{
						glm::vec2 vector = std::get<glm::vec2>(value);
						if (Wui::Vec2Field(ctx, rowId, fieldRect, vector, 0.01f, 1.0f, -1.0f, theme, layout))
						{
							value = vector;
							fieldChanged = true;
						}
					}
					else if (components == 3)
					{
						glm::vec3 vector = std::get<glm::vec3>(value);
						if (Wui::Vec3Field(ctx, rowId, fieldRect, vector, 0.01f, 1.0f, -1.0f, theme, layout))
						{
							value = vector;
							fieldChanged = true;
						}
					}
					else
					{
						glm::vec4 vector = std::get<glm::vec4>(value);
						if (Wui::Vec4Field(ctx, rowId, fieldRect, vector, 0.01f, 1.0f, -1.0f, theme, layout))
						{
							value = vector;
							fieldChanged = true;
						}
					}
					// 行变高(Vec4 横排 2×2 = 两行)后悬停说明仍覆盖整行:外层 tooltip 登记在
					// 行首 22px 的行矩形上,这里对控件矩形再挂一次(库件自身不带 tooltip)。
					if (!fieldDoc.empty())
						Wui::Tooltip(ctx, fieldRect, fieldDoc);
					rowAdvance = fieldHeight + 2.0f;
					break;
				}
				case Schema::Kind::String:
				case Schema::Kind::Asset:
				{
					const std::string current = std::get<std::string>(value);
					// P4-U9:资产路径(Asset("Material") / Of("Texture2D"))→ 可搜索资产下拉,
					// 明确给"(无)"选项 —— 手打路径既容易写错也发现不了拼写问题。
					const std::string assetType = !field.Meta.AssetType.empty() ? field.Meta.AssetType
						: (field.K == Schema::Kind::Asset && field.AssetTypeName ? std::string(field.AssetTypeName)
							: std::string());
					if (!assetType.empty())
					{
						const std::vector<std::string>& paths = Editor::AssetCatalog::PathsForName(assetType);
						std::vector<std::string> options;
						options.reserve(paths.size() + 2);
						options.push_back(Wui::Tr("panel.properties.asset_none", "(none)"));
						options.insert(options.end(), paths.begin(), paths.end());
						// 当前值不在扫描结果里(文件名写错/资产还没建):照样显示出来,不假装它是"(无)"。
						int selected = 0;
						const auto found = std::find(paths.begin(), paths.end(), current);
						if (found != paths.end())
							selected = static_cast<int>(found - paths.begin()) + 1;
						else if (!current.empty())
						{
							options.push_back(current);
							selected = static_cast<int>(options.size()) - 1;
						}
						// 注意类型:beforePick 必须是 int —— 写成 bool 时 selected(1) != true 恒为 false,
						// "选到第 1 个资产"就永远不会写回(实测踩过)。
						const int beforePick = selected;
						if (Wui::SearchableCombo(ctx, fid, ctrl, "", options, selected, theme)
							&& selected != beforePick)
						{
							value = selected <= 0 ? std::string() : options[static_cast<size_t>(selected)];
							fieldChanged = true;
						}
						RegisterNode(Wui::HashId(idText.c_str()), "searchable-combo", ctrl, labelText,
							current, reachable(ctrl), fieldDoc);
						break;
					}
					// P4-U9:固定集合的字符串(Choices("cube","sphere"...))→ 下拉,别无谓地手打。
					if (!field.Meta.Choices.empty())
					{
						std::vector<std::string> options = field.Meta.Choices;
						int selected = 0;
						const auto found = std::find(options.begin(), options.end(), current);
						if (found != options.end())
							selected = static_cast<int>(found - options.begin());
						const int beforePick = selected;
						if (Wui::Combo(ctx, fid, ctrl, "", options, selected, theme) && selected != beforePick)
						{
							value = options[static_cast<size_t>(selected)];
							fieldChanged = true;
						}
						RegisterNode(Wui::HashId(idText.c_str()), "combo", ctrl, labelText, current,
							reachable(ctrl), fieldDoc);
						break;
					}
					// 与 TextField 的 WuiEditState 共用 fid 会导致类型混淆,
					// 编辑缓冲必须使用独立 id。
					auto& state = ctx.Persist<SchemaTextState>(Wui::HashId("schema.text.state") ^ fid, {});
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
					RegisterNode(Wui::HashId(idText.c_str()), "text-field", ctrl, labelText, current,
						reachable(ctrl), fieldDoc);
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
							reachable(ctrl), fieldDoc);
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
				// P4-U13b:编辑实例字段 → 由"改动发生处"登记覆盖(不靠全量 diff 反推)。
				// 具体落账在 DrawComponentInspector(那里才知道编辑的是哪个实体)。
				if (changedFields)
					changedFields->push_back(typeName + "." + field.Name);
			}
			// 数组/映射的**叶子元素行**行尾 `-`(Object 元素行在各自分支里画)。
			// 叶子行的 `↺` 复位已随行结构(PropertyRow)画过,这里只剩 `-` 与推进。
			// 集合元素行走 CollectionRow 时,`-` 已由库件画过(不要重复登记/重复绘制)。
			if (!collectionWritable)
				drawCollectionRemove(field.Name, row);
			y += rowAdvance;
		}

		// ---- VEC-C2:数组/映射容器的收口(删除 / 追加 / 映射键名输入)----
		//
		// 放在所有元素行画完之后:合成 schema 的元素访问器按编译期下标实例化,循环中途 erase 会让
		// 后面的行读到错元素。删除按**行名**(数组 = 下标字符串,映射 = 键)定位,数组删完重排 1..n。
		if (collectionWritable)
		{
			ScriptProperty& container = *collectionRows->Container;
			const Wui::WuiId addingId = Wui::HashId(("script.collection.adding." + collectionRows->IdText).c_str());
			bool& adding = ctx.Persist<bool>(addingId, false);
			const Wui::WuiId keyFieldId = Wui::HashId((collectionRows->IdText + ".add.key").c_str());
			SchemaTextState& keyState = ctx.Persist<SchemaTextState>(
				Wui::HashId(("script.collection.add.state." + collectionRows->IdText).c_str()), {});
			const auto markContainerChanged = [&]()
			{
				changed = true;
				if (changedFields)
					changedFields->push_back(typeName);
			};
			// VEC-H4:空集合 = 一行次要说明(§规则 21 的空态口径);`+` 按钮仍在下一行,点它出现第一项。
			if (container.Children.empty())
			{
				const Wui::WuiRect emptyRow { rect.X, rect.Y + y, rect.W, kRowHeight };
				Wui::PropertyRowDesc emptyDesc;
				emptyDesc.A11yKind = "text";
				emptyDesc.A11yLabel = collectionRows->IdText;
				emptyDesc.A11yValue = "0";
				emptyDesc.A11yEnabled = false;
				emptyDesc.Enabled = false;
				emptyDesc.LabelWidth = labelWidth;
				emptyDesc.LabelIndent = labelIndent + kIndentPerLevel;
				emptyDesc.InlineValue = true;
				emptyDesc.InlineValueText = Wui::Tr("panel.properties.collection_empty", "No items yet");
				Wui::PropertyRow(ctx, Wui::HashId((collectionRows->IdText + ".empty").c_str()), emptyRow,
					emptyDesc, theme);
				y += kRowHeight;
			}
			if (collectionRows->Kind == ScriptPropertyCollection::Map && adding)
			{
				// 映射 `+`:先给一个**键名文本输入**,回车建行(空键 / 重名忽略;Esc 取消)。
				const Wui::WuiRect keyRect { rect.X, rect.Y + y + (kRowHeight - 20.0f) * 0.5f,
					std::max(60.0f, rect.W - 4.0f), 20.0f };
				bool cancelled = false;
				const bool committed = Wui::TextField(ctx, keyFieldId, keyRect, keyState.Buffer, theme, &cancelled);
				const std::string keyText = keyState.Buffer;
				RegisterNode(keyFieldId, "text-field", keyRect, "key",
					keyText.empty() ? "new key" : keyText, true,
					Wui::Tr("panel.properties.collection_add_key.tooltip",
						"Type a new key name, then press Enter to add the row (empty or duplicate keys are ignored)"));
				if (committed)
				{
					if (!keyText.empty() && !CollectionKeyTaken(container, keyText))
					{
						ScriptProperty child = MakeCollectionElement(container);
						child.Name = keyText;
						container.Children.push_back(std::move(child));
						markContainerChanged();
						WLD_CORE_INFO("[script-ui] collection add: {0}.{1}[{2}]", typeName, container.Name, keyText);
					}
					keyState.Buffer.clear();
					adding = false;
				}
				else if (cancelled || (keyText.empty() && ctx.Focus() != keyFieldId))
				{
					// Esc / 点空且没输入内容:收起输入行,不建行。
					keyState.Buffer.clear();
					adding = false;
				}
				y += kRowHeight;
			}
			else
			{
				// 追加行:`+` 与元素行的 `-` 同一条行外动作槽(CollectionActionColumnWidth)。
				const Wui::WuiRect addButton { rect.X + rect.W + 2.0f,
					rect.Y + y, kRowHeight, kRowHeight };
				const std::string addDoc = collectionRows->Kind == ScriptPropertyCollection::Map
					? Wui::Tr("panel.properties.collection_add_map.tooltip",
						"Add a key/value row (the key name is typed next)")
					: Wui::Tr("panel.properties.collection_append.tooltip", "Append one element to the list");
				if (Wui::CollectionActionButton(ctx, Wui::HashId((collectionRows->IdText + ".add").c_str()),
					addButton, "+", addDoc, true, theme, false))
				{
					if (collectionRows->Kind == ScriptPropertyCollection::Array)
					{
						ScriptProperty child = MakeCollectionElement(container);
						child.Name = std::to_string(container.Children.size() + 1);
						container.Children.push_back(std::move(child));
						markContainerChanged();
						WLD_CORE_INFO("[script-ui] collection add: {0}.{1} -> {2} element(s)", typeName,
							container.Name, container.Children.size());
					}
					else
					{
						adding = true;
						keyState.Buffer.clear();
						ctx.SetFocus(keyFieldId);
					}
				}
				y += kRowHeight;
			}
			if (!pendingEraseName.empty())
			{
				const auto found = std::find_if(container.Children.begin(), container.Children.end(),
					[&pendingEraseName](const ScriptProperty& child) { return child.Name == pendingEraseName; });
				if (found != container.Children.end())
				{
					WLD_CORE_INFO("[script-ui] collection remove: {0}.{1}[{2}]", typeName, container.Name,
						pendingEraseName);
					container.Children.erase(found);
					if (collectionRows->Kind == ScriptPropertyCollection::Array)
						RenumberArrayChildren(container);
					markContainerChanged();
				}
			}
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
		// P4-U13b:本分区内被编辑的字段名收在这里,分区画完统一登记成实例覆盖。
		// 归属判定(这个实体属于哪条实例记录)放在 RegisterPrefabOverrides —— 普通实体编辑不产生记录。
		std::vector<std::string> changedFields;
		float height = 0.0f;

		// 自定义检查器:与迁移前一致的三行 Location/Rotation(度)/Scale。
		if (schema.Id.Name == "World::TransformComponent")
			height = DrawTransformInspector(ctx, rect, *static_cast<TransformComponent*>(instance), schema,
				visibleRect, &changedFields);
		// 自定义检查器:Primary / Fixed Aspect Ratio + 投影类型下拉 + 对应参数组。
		else if (schema.Id.Name == "World::CameraComponent")
			height = DrawCameraInspector(ctx, rect, instance, schema, visibleRect, &changedFields);
		// 2026-09-26 脚本组件重写:两个脚本组件走**同一条**统一检视器(引用行 / 状态行 / 属性表 /
		// 动作行),不再各写一套。编辑态不创建脚本实例;Play/Simulate 只读。
		else if (schema.Id.Name == "World::CppScriptComponent" || schema.Id.Name == "World::LuauScriptComponent")
			height = DrawScriptComponentInspector(ctx, rect, entity, instance, schema, visibleRect, &changedFields);

		else
			height = DrawSchemaFields(ctx, base, rect, instance, schema.DisplayName, schema, visibleRect,
				&changedFields);

		RegisterPrefabOverrides(entity, changedFields);
		return height;
	}

	// ---- 2026-09-26 脚本组件重写:统一脚本检视器(两个脚本组件共用)----
	//
	// 布局:脚本引用行 → 状态行(State + LastError,Luau 再加 ReloadDiagnostic)→ 属性表
	//      (每个 `ScriptProperty` 一行)→ 动作行(Luau 的 Reload,id 保持 `lua.reload`)。
	//
	// 三条硬口径(方案 v2 §3 + 派工单):
	//   ① **编辑态不实例化脚本**:属性直接读写组件里的 `Properties`;不建 VM、不跑 OnCreate、
	//      不构造 `ScriptableEntity`(实例只由 Scene 在 Play/Simulate 按 schema 工厂创建);
	//   ② **Play/Simulate 只读**:控件走既有只读行 / 禁用按钮契约 + 一行只读说明;
	//      C++ 组件在运行实例在场时按**实例**读真实值(只读展示),Luau 用组件里保存的值;
	//   ③ **属性行复用既有类型化行渲染**:每条属性造一条临时 `FieldSchema`(Get/Set 直连属性值),
	//      逐个调用 `DrawSchemaFields` —— 控件 / 只读行 / 无障碍节点 / 悬停说明 / 预制体覆盖登记
	//      都不另写一套。
	//
	// 无障碍 id 契约:`properties.<组件名>.<属性名>`(脚本可直接算);状态与诊断行见各段注释。
	float PropertiesPanel::DrawScriptComponentInspector(Wui::WuiContext& ctx, const Wui::WuiRect& rect,
		Entity entity, void* instance, const Schema::TypeSchema& schema, const Wui::WuiRect& visibleRect,
		std::vector<std::string>* changedFields)
	{
		const Wui::WuiTheme& theme = m_Host.Theme();
		const bool luau = schema.Id.Name == "World::LuauScriptComponent";
		CppScriptComponent* cpp = luau ? nullptr : static_cast<CppScriptComponent*>(instance);
		LuauScriptComponent* lua = luau ? static_cast<LuauScriptComponent*>(instance) : nullptr;
		if ((!luau && !cpp) || (luau && !lua))
			return 0.0f;
		ScriptRuntimeState& runtime = luau ? lua->Runtime : cpp->Runtime;
		std::vector<ScriptProperty>& properties = luau ? lua->Properties : cpp->Properties;
		Scene* scene = entity.GetScene();
		if (!scene)
			return 0.0f;
		Schema::SchemaRegistry& schemas = scene->GetContext().Schemas();

		float y = 0.0f;
		bool changed = false;
		// 控件 id 混入组件显示名(与通用路径同一条:`f.<type>.<field>` ^ base)。
		const Wui::WuiId base = Wui::HashId(schema.DisplayName.c_str());
		// 实体句柄(Reload 结果按它区分;脚本注解同步记忆也按它分键)。
		const uint32_t handle = static_cast<uint32_t>(static_cast<entt::entity>(entity));
		// VEC-H6:上一帧/上一个组件的延后集合复原请求不跨调用存在(防御:指针只在本次绘制内有效)。
		m_PendingCollectionReset = nullptr;
		m_PendingCollectionResetPath.clear();

		// ---- VEC-H6:本帧的脚本声明(Luau 注解树)----
		// ① 编辑态同步的门(签名不变 → 不把面板刚做的集合增删按声明还原);
		// ② 集合头 `↺` 的"形状是否偏离默认"判定要按**声明的默认形状**比较(`ScriptRowModified`)。
		// 引擎按"路径 + 内容指纹 + VM 可用性"缓存解析,这里每帧取一次与既有口径同量级;取完到本函数
		// 结束前有效(成员只在本次绘制期间被引用,不跨帧持有)。
		m_ScriptDeclarations.clear();
		m_ScriptDeclarationsValid = false;
		if (luau)
			m_ScriptDeclarationsValid = ScriptEngine::DescribeScriptDeclarations(lua->ScriptPath,
				m_ScriptDeclarations, nullptr, nullptr) && !m_ScriptDeclarations.empty();

		// ---- SCRIPT-V2 + CPPT-3:属性表按脚本**声明**保持新鲜 ----
		// C++:字段说明来自 schema 的 `Doc("…")` —— 每次画都从 schema 取回(查不到 = 留空,走中性兜底);
		// **声明签名(名字 + Kind + 枚举/资产类型 + ReadOnly)变化才整体重建**(plan §4.6 "补 C++ 声明
		// 签名门"):重编 Game.dll → 同会话热重载 → 新字段/类型立即生效;签名不变时不重建,
		// 与 Luau 的 VEC-C2 签名门同一口径(不把面板刚做的值按声明还原)。
		const Schema::TypeSchema* cppSchema = (!luau && !cpp->ScriptName.empty())
			? schemas.Find(cpp->ScriptName) : nullptr;
		if (cppSchema && !m_ReadOnly)
		{
			std::string declarationSignature;
			for (const Schema::FieldSchema& field : cppSchema->Fields)
			{
				declarationSignature += field.Name;
				declarationSignature += '|';
				declarationSignature += ScriptProperties::KindName(field.K);
				declarationSignature += '|';
				if (field.GetEnum)
					if (const Schema::EnumSchema* enumSchema = field.GetEnum())
						declarationSignature += enumSchema->Name;
				declarationSignature += '|';
				declarationSignature += field.AssetTypeName ? field.AssetTypeName : "";
				if (field.Meta.ReadOnly)
					declarationSignature += "|ro";
				declarationSignature += ';';
			}
			const Wui::WuiId cppSignatureId = Wui::HashId(
				("cpp.script.decl.sig." + std::to_string(handle) + "." + schema.DisplayName).c_str());
			std::string& cppLastSignature = ctx.Persist<std::string>(cppSignatureId, std::string());
			if (declarationSignature != cppLastSignature)
			{
				// 迁移(同名同类型保值 / 新字段取声明默认值)由引擎的 SyncFromSchema 完成;
				// 模块热重载后的实例迁移在 Scene::RestoreNativeScriptInstances,这里是同会话兜底。
				ScriptProperties::SyncFromSchema(cpp->Properties, *cppSchema);
				cppLastSignature = declarationSignature;
			}
			for (ScriptProperty& property : cpp->Properties)
				if (const Schema::FieldSchema* field = FindScriptSchemaField(*cppSchema, property.Name))
					property.Doc = field->Meta.Doc;
		}
		// Luau:脚本注解 = 属性表的唯一声明来源。**脚本路径由任何来源变化**(下拉改选 / `scene.set` /
		// 反序列化 / Reload / 换实体 / 面板重开)都在这里收敛一次 —— 用户反馈②「脚本里新加字段
		// 不显示、Reload 也不管用」的落点。是否真的重新解析由引擎按"路径 + 内容指纹 + VM 可用性"
		// 判定(引擎内部单槽缓存):这里可以每帧调,但不会每帧重解析;同名同类型值继续保留。
		// Play/Simulate 是只读态:不在这期间重建属性表。
		if (luau && !m_ReadOnly)
		{
			// VEC-C2:每帧无条件重合并会把面板刚做的集合增删(`+`/`-`)按脚本声明还原回去
			// (引擎的合并以声明为集合形状的事实源)。所以按**声明签名**设门:签名不变 = 脚本没改
			// → 属性表(值与形状)以组件/场景为准;签名变了才整体重建(新字段/新说明/新初值立即生效)。
			// 声明读不出来(路径空 / 文件没了)→ 维持既有行为,交给引擎入口出诊断。
			// VEC-H6:声明在本函数开头取过一次(m_ScriptDeclarations)—— 这里复用同一份,不再重复解析。
			std::string declarationSignature;
			if (m_ScriptDeclarationsValid)
				declarationSignature = ScriptDeclarationSignature(m_ScriptDeclarations);
			const Wui::WuiId signatureId = Wui::HashId(
				("script.decl.sig." + std::to_string(handle) + "." + schema.DisplayName).c_str());
			std::string& lastSignature = ctx.Persist<std::string>(signatureId, std::string());
			if (!m_ScriptDeclarationsValid || declarationSignature != lastSignature)
			{
				SyncLuauPropertiesFromAnnotations(*lua);
				lastSignature = declarationSignature;
			}
		}

		// ---- 脚本引用行 ----
		const float labelWidth = std::min(140.0f, rect.W * 0.45f);
		const Wui::WuiRect refRow { rect.X, rect.Y, rect.W, 22.0f };
		// 引用行标签沿用面板既有 key(两个组件同一句话:这一行 = 脚本引用)。
		const Wui::LocalizedLabel refLabel = Wui::TrLabel("panel.properties.native_script", "Script");
		const std::string refLabelText = TermText(refLabel);
		Wui::LabelWithTerm(ctx, { refRow.X + 4.0f, refRow.Y + 3.0f }, refLabel.Text, refLabel.Term,
			theme.TextMuted, 13.0f, theme, labelWidth - 4.0f);
		// Luau 右侧留出"在脚本编辑器里打开"**图标按钮**(VEC-H6:纯图标;文字搬到 tooltip);
		// C++ 不要按钮(整行给下拉)。方形图标位 20×20 = 字段控件高(kPropertyFieldHeight)。
		constexpr float kOpenButtonWidth = 20.0f;
		const float refCtrlWidth = std::max(40.0f,
			refRow.W - labelWidth - 4.0f - (luau ? kOpenButtonWidth + 6.0f : 0.0f));
		const Wui::WuiRect refCtrl { refRow.X + labelWidth, refRow.Y + 1.0f, refCtrlWidth, 20.0f };
		const std::string refIdText = PropPath(schema.DisplayName, luau ? "ScriptPath" : "ScriptName");
		const Wui::WuiId refId = Wui::HashId(refIdText.c_str());
		const auto reachable = [&visibleRect](const Wui::WuiRect& control)
		{
			return control.X + control.W * 0.5f >= visibleRect.X
				&& control.X + control.W * 0.5f <= visibleRect.X + visibleRect.W
				&& control.Y + control.H * 0.5f >= visibleRect.Y
				&& control.Y + control.H * 0.5f <= visibleRect.Y + visibleRect.H;
		};

		if (luau)
		{
			// Luau = 脚本**资产**下拉(与 `Asset("Script")` 同一份清单:内容根下 .lua/.luau),
			// 可搜索;`(none)` = 清空引用;当前值不在清单里也照实显示(不假装是"(无)")。
			if (m_ReadOnly)
			{
				DrawReadOnlyRow(ctx, refIdText, refRow, refLabel, lua->ScriptPath, theme);
			}
			else
			{
				const std::vector<std::string>& paths = Editor::AssetCatalog::PathsForName("Script");
				std::vector<std::string> options;
				options.reserve(paths.size() + 2);
				options.push_back(Wui::Tr("panel.properties.asset_none", "(none)"));
				options.insert(options.end(), paths.begin(), paths.end());
				int selected = 0;
				const auto found = std::find(paths.begin(), paths.end(), lua->ScriptPath);
				if (found != paths.end())
					selected = static_cast<int>(found - paths.begin()) + 1;
				else if (!lua->ScriptPath.empty())
				{
					options.push_back(lua->ScriptPath);
					selected = static_cast<int>(options.size()) - 1;
				}
				const int beforePick = selected;
				if (Wui::SearchableCombo(ctx, refId, refCtrl, "", options, selected, theme)
					&& selected != beforePick)
				{
				lua->ScriptPath = selected <= 0 ? std::string() : options[static_cast<size_t>(selected)];
				// 换脚本 = 属性表按**新脚本的注解声明**重建(同名同类型保留值,其余丢弃)。
				// 引擎入口内部按"路径 + 内容指纹"判定,换路径必然重解析;这里同步调一次让本帧就用新表。
				SyncLuauPropertiesFromAnnotations(*lua);
				changed = true;
					if (changedFields)
						changedFields->push_back(schema.DisplayName + ".ScriptPath");
				}
				RegisterNode(refId, "searchable-combo", refCtrl, refLabelText, lua->ScriptPath,
					reachable(refCtrl),
					Wui::Tr("panel.properties.script_path.tooltip",
						"Luau script asset under the project content root (.luau/.lua); (none) clears the reference"));
			}
			// "在脚本编辑器里打开"(有脚本才可用;与内容浏览器双击同一条 EditorShell::OpenScriptEditor)。
			const Wui::WuiRect openRect { refCtrl.X + refCtrl.W + 6.0f, refRow.Y + 1.0f,
				std::min(kOpenButtonWidth, std::max(40.0f, refRow.W - (refCtrl.X + refCtrl.W + 6.0f))), 20.0f };
			// Play/Simulate 只读:打开脚本编辑器也一并禁用(与"这一块只读"同一句原因)。
			const bool hasScript = !lua->ScriptPath.empty();
			// VEC-H6:纯图标按钮 —— 按钮上原来的文字搬进 tooltip(中英都走 Wui::Tr),a11y 节点
			// label 仍是同一句文案(图标按钮没有可见文字,读屏/脚本靠 label 找得到它)。
			const std::string openLabel = Wui::Tr("panel.properties.open_script_editor", "Open in Editor");
			const std::string openDoc = m_ReadOnly
				? Wui::Tr("panel.properties.script_readonly_reason",
					"Play/Simulate is read-only: pause or stop to reload the script")
				: (hasScript
					? Wui::Tr("panel.properties.open_script_editor.tooltip",
						"Open this script asset in the built-in script editor (same panel as double-clicking it in the Content Browser)")
					: Wui::Tr("panel.properties.open_script_editor.none", "Pick a script asset first"));
			// 可用时:第一行 = 原来的按钮文字(用户口径「把文字放到悬浮提示里」),第二行 = 说明;
			// 不可用时:提示就是原因(与库件"灰按钮不能没有理由"同一口径)。
			const std::string openTip = (hasScript && !m_ReadOnly) ? (openLabel + "\n" + openDoc) : openDoc;
			if (Wui::OpenInEditorButton(ctx, Wui::HashId("script.open_in_editor"), openRect, theme,
				hasScript && !m_ReadOnly, openLabel, openTip))
			{
				m_Host.OpenScriptEditor(lua->ScriptPath);
				WLD_CORE_INFO("[script-ui] open in script editor: '{0}'", lua->ScriptPath);
			}
		}
		else
		{
			// C++ = **已注册脚本**下拉(schema `TypeCategory::Script`)。写回的是类型全名
			// (如 `Game::ExampleScript`)—— Scene 实例化就是按 `Schemas().Find(ScriptName)`
			// 解析 schema 工厂的;显示名只用于文案。
			const std::vector<const Schema::TypeSchema*> scripts = schemas.List(Schema::TypeCategory::Script);
			std::vector<std::string> ids;
			std::vector<std::string> labels;
			ids.reserve(scripts.size());
			labels.reserve(scripts.size());
			int selected = -1;
			for (const Schema::TypeSchema* script : scripts)
			{
				if (!script)
					continue;
				ids.push_back(script->Id.Name);
				labels.push_back(Wui::Tr("schema.script." + SchemaTypeKeyName(*script), script->DisplayName));
				if (script->Id.Name == cpp->ScriptName)
					selected = static_cast<int>(ids.size()) - 1;
			}
			if (m_ReadOnly)
			{
				const std::string shown = cpp->ScriptName.empty()
					? Wui::Tr("panel.properties.value_none", "(none)") : cpp->ScriptName;
				DrawReadOnlyRow(ctx, refIdText, refRow, refLabel, shown, theme);
			}
			else if (ids.empty())
			{
				const std::string hint = Wui::Tr("panel.properties.script_no_registered",
					"no C++ script is registered yet");
				Label(ctx, { refCtrl.X, refCtrl.Y + 3.0f }, hint, theme.TextMuted, 12.0f);
				RegisterNode(refId, "text", refCtrl, refLabelText, hint, false);
			}
			else
			{
				const int beforePick = selected;
				if (Wui::Combo(ctx, refId, refCtrl, "", labels, selected, theme)
					&& selected != beforePick && selected >= 0)
				{
					cpp->ScriptName = ids[static_cast<size_t>(selected)];
					// 换脚本 = 属性表按**新脚本的 schema 字段**重建(同名同类型保留值)。
					ScriptProperties::SyncFromSchema(cpp->Properties, *scripts[static_cast<size_t>(selected)]);
					changed = true;
					if (changedFields)
						changedFields->push_back(schema.DisplayName + ".ScriptName");
				}
				RegisterNode(refId, "combo", refCtrl, refLabelText, cpp->ScriptName, reachable(refCtrl),
					Wui::Tr("panel.properties.script_name.tooltip",
						"Registered C++ script (schema category Script); the property list follows the script you pick"));
			}
		}
		y = 26.0f;

		// 未注册提示行:存的名字在注册表里查不到(旧场景 / 模块没加载 / 脚本被删)→ 说出来,不静默。
		if (!luau && !cpp->ScriptName.empty() && schemas.Find(cpp->ScriptName) == nullptr)
		{
			const std::string hint = Wui::Tr("panel.properties.script_unregistered",
				"this C++ script is not registered (is its module loaded?)") + " " + cpp->ScriptName;
			Label(ctx, { rect.X + 4.0f, rect.Y + y }, TruncateForPanel(hint, 96), theme.Warning, 12.0f);
			RegisterNode(Wui::HashId((PropPath(schema.DisplayName, "ScriptName.unregistered")).c_str()),
				"text", { rect.X, rect.Y + y - 2.0f, rect.W, 18.0f }, hint, hint, false);
			y += 18.0f;
		}

		// ---- 状态行:Runtime.State + LastError(Luau 再加 ReloadDiagnostic)----
		// 状态名只用于显示(拼接结果不参与比较/存储)。"loaded" 的等价口径 = State==Running
		// (旧的双轨加载标记已随重写删除;AI 通道 script.status 的 loaded 字段用同一条口径)。
		std::string stateText = Wui::Tr("panel.properties.script_state", "state") + ": "
			+ ScriptStateLabel(runtime.State);
		stateText += " " + (runtime.State == ScriptInstanceState::Running
			? Wui::Tr("panel.properties.script_loaded", "(loaded)")
			: Wui::Tr("panel.properties.script_not_loaded", "(not loaded)"));
		Label(ctx, { rect.X + 4.0f, rect.Y + y }, stateText, theme.TextMuted, 13.0f);
		RegisterNode(Wui::HashId((PropPath(schema.DisplayName, "Runtime.State")).c_str()), "text",
			{ rect.X, rect.Y + y - 2.0f, rect.W, 18.0f }, stateText, stateText, false);
		y += 18.0f;
		if (luau && !lua->ReloadDiagnostic.empty())
		{
			const std::string text = Wui::Tr("panel.properties.reload_prefix", "[reload]") + " "
				+ TruncateForPanel(lua->ReloadDiagnostic);
			Label(ctx, { rect.X + 4.0f, rect.Y + y }, text, Wui::WuiColor { 1.0f, 0.75f, 0.3f, 1.0f }, 12.0f);
			RegisterNode(Wui::HashId((PropPath(schema.DisplayName, "Runtime.ReloadDiagnostic")).c_str()),
				"text", { rect.X, rect.Y + y - 2.0f, rect.W, 16.0f }, text, lua->ReloadDiagnostic, false);
			y += 16.0f;
		}
		if (!runtime.LastError.empty())
		{
			const std::string text = Wui::Tr("panel.properties.error_prefix", "[error]") + " "
				+ TruncateForPanel(runtime.LastError);
			Label(ctx, { rect.X + 4.0f, rect.Y + y }, text, Wui::WuiColor { 1.0f, 0.4f, 0.4f, 1.0f }, 12.0f);
			RegisterNode(Wui::HashId((PropPath(schema.DisplayName, "Runtime.LastError")).c_str()), "text",
				{ rect.X, rect.Y + y - 2.0f, rect.W, 16.0f }, text, runtime.LastError, false);
			y += 16.0f;
		}
		if (m_ReadOnly)
		{
			// 逐面板的只读说明(全局提示条之外,这一块自己说清楚为什么改不了)。
			const std::string notice = Wui::Tr("panel.properties.script_readonly_notice",
				"Play/Simulate: script properties are read-only (pause or stop to edit)");
			Label(ctx, { rect.X + 4.0f, rect.Y + y }, TruncateForPanel(notice, 110), theme.TextDisabled, 12.0f);
			RegisterNode(Wui::HashId((PropPath(schema.DisplayName, "Runtime.ReadOnly")).c_str()), "text",
				{ rect.X, rect.Y + y - 2.0f, rect.W, 16.0f }, notice, notice, false);
			y += 16.0f;
		}

		// ---- 属性表(本次核心)----
		if (properties.empty())
		{
			Label(ctx, { rect.X + 4.0f, rect.Y + y },
				Wui::Tr("panel.properties.script_no_properties",
					"This script declares no editable properties yet"),
				theme.TextMuted, 12.0f);
			y += 20.0f;
		}
		else if (!luau && m_ReadOnly && cpp->Instance != nullptr
			&& !cpp->ScriptName.empty() && schemas.Find(cpp->ScriptName) != nullptr)
		{
			// Play/Simulate + 运行实例在场:C++ 按**实例**读真实值(只读;不创建、不写)。
			m_ScriptInspectingScriptRows = false;
			const Schema::TypeSchema* liveSchema = schemas.Find(cpp->ScriptName);
			y += DrawSchemaFields(ctx, base ^ 0x51u, { rect.X, rect.Y + y, rect.W, 0 }, cpp->Instance,
				schema.DisplayName, *liveSchema, visibleRect, changedFields, /*scriptPropertyRow=*/true);
		}
		else
		{
			// VEC-B3:合成 schema 的 arena 每个脚本组件重建一次;下面的循环只增不减,
			// 固定数组地址稳定,递归绘制期间不会失效。CPPT-3:枚举 arena 同帧重置。
			ScriptTableArena().Used = 0;
			ScriptEnumArenaStore().Used = 0;
			// VEC-F2:集合头 `↺` 的确认请求要知道"正在画哪一个脚本组件"以及"这一行在哪条路径上"。
			m_ScriptInspectingEntity = entity;
			m_ScriptInspectingComponentId = schema.Storage ? schema.Storage->ComponentId : 0;
			m_ScriptInspectingLuau = luau;
			m_ScriptInspectingScriptRows = true;
			m_ScriptInspectingComponentName = schema.DisplayName;
			m_ScriptRowPath.clear();
			for (ScriptProperty& property : properties)
			{
				const bool editableKind = ScriptProperties::IsPropertyKind(property.Type);
				const bool summaryKind = ScriptProperties::IsSummaryKind(property.Type);
				if (!editableKind && !summaryKind)
					continue;   // Kind::None 等真正不认识、也没有摘要文案的类型(引擎不会产出)
				// CPPT-3:编辑元数据(颜色/范围/单位/固定集合/资产类型/说明)按名字从 schema
				// 声明回读 —— 不复制进属性表、不进存档(plan §4.4)。
				const Schema::FieldSchema* declared = cppSchema
					? FindScriptSchemaField(*cppSchema, property.Name) : nullptr;
				Schema::FieldSchema field;
				field.Name = property.Name;
				field.K = property.Type;
				// 行悬停/读屏 = 脚本注释(`ScriptProperty::Doc`,由脚本派生、不进存档);
				// 空 → 中性兜底(绝不回落 schema 的字段说明,见 DrawSchemaFields 的 fieldDocFor)。
				field.Meta.Doc = property.Doc;
				// CPPT-3:ReadOnly 透传(引擎把不可编辑的类型/缺枚举 schema 的字段标成只读摘要)。
				field.Meta.ReadOnly = property.ReadOnly || summaryKind;
				if (declared)
				{
					field.Meta.Color = declared->Meta.Color;
					field.Meta.Min = declared->Meta.Min;
					field.Meta.Max = declared->Meta.Max;
					field.Meta.Unit = declared->Meta.Unit;
					field.Meta.Step = declared->Meta.Step;
					field.Meta.Choices = declared->Meta.Choices;
					if (!declared->Meta.AssetType.empty())
						field.Meta.AssetType = declared->Meta.AssetType;
					else if (declared->AssetTypeName)
						field.Meta.AssetType = declared->AssetTypeName;
				}
				if (property.Type == Schema::Kind::Enum)
				{
					// 枚举下拉要吃 EnumSchema:按 TypeName 从注册表取(拿不到 = 只读摘要,不静默)。
					field.GetEnum = ScriptEnumAccessorFor(schemas, property.TypeName);
					if (!field.GetEnum)
						field.Meta.ReadOnly = true;
				}
				else if (property.Type == Schema::Kind::Asset && field.Meta.AssetType.empty())
				{
					// 资产下拉:注册的资产类型名(引擎写进 ScriptProperty::TypeName)。
					field.Meta.AssetType = property.TypeName;
				}
				if (property.Type == Schema::Kind::Object)
				{
					// 嵌套 `---@class`:合成 GetNested/GetPtr(可展开、子行可编辑);
					// 裸 table / 空结构 / 超护栏:降级成只读摘要行(看得到、不可编辑、不进存档)。
					field = MakeScriptTableField(property, schema.DisplayName + "." + property.Name, &schemas);
					field.Meta.ReadOnly = field.Meta.ReadOnly || property.ReadOnly;
				}
				else
				{
					// Get/Set 直连这条属性(instance 传 &property):无捕获 lambda → 函数指针。
					// Get 走**展示值**:未设(monostate)/ 类型不匹配时给规范零值但**不写回**组件 ——
					// "未设"要保持未设,真实默认值由引擎的属性入口填(审查 P1-1:不能编辑期写成 0 落盘)。
					field.Get = [](const void* value)
					{ return ScriptPropertyDisplayValueForRow(*static_cast<const ScriptProperty*>(value)); };
					// 只读摘要(引擎标记 / 摘要 Kind)→ 不给 Set,行走只读行(值列显示摘要/类型)。
					if (!field.Meta.ReadOnly)
						field.Set = [](void* value, const Schema::Value& edited)
						{ static_cast<ScriptProperty*>(value)->Value = edited; };
				}
				Schema::TypeSchema rowSchema;
				rowSchema.DisplayName = schema.DisplayName;
				rowSchema.Category = Schema::TypeCategory::Struct;
				rowSchema.Fields.push_back(field);
				// 属性名固定 → 行 id 稳定(可用 ui.invoke 直接驱动)。
				const Wui::WuiId rowBase = base ^ Wui::HashId(("script.prop." + property.Name).c_str());
				y += DrawSchemaFields(ctx, rowBase, { rect.X, rect.Y + y, rect.W, 0 }, &property,
					schema.DisplayName, rowSchema, visibleRect, changedFields, /*scriptPropertyRow=*/true);
			}
			// VEC-H6:集合头 `↺` 的落地(延后到这里):本组件的属性表已经全部画完,重建容器不会再让
			// 同一帧的"旧 schema vs 新 Children"错位;下一帧行/集合头 ↺ 的可见性自动按新状态算。
			if (m_PendingCollectionReset != nullptr)
			{
				// 指针越界防线(地址比较;指针序比较跨对象未定义):请求只可能指向本次画的属性表里的一条。
				const uintptr_t pendingAddress = reinterpret_cast<uintptr_t>(m_PendingCollectionReset);
				const uintptr_t beginAddress = reinterpret_cast<uintptr_t>(properties.data());
				const uintptr_t endAddress = reinterpret_cast<uintptr_t>(properties.data() + properties.size());
				if (beginAddress <= pendingAddress && pendingAddress < endAddress)
				{
					ApplyScriptCollectionReset(ctx, *m_PendingCollectionReset, m_PendingCollectionResetLuau,
						m_PendingCollectionResetPath);
					changed = true;
				}
				m_PendingCollectionReset = nullptr;
				m_PendingCollectionResetPath.clear();
			}
			m_ScriptRowPath.clear();
		}

		// ---- 动作行:Luau 保留 Reload(唯一入口 EditorLayer::ReloadLuauScriptComponent;id = lua.reload)----
		if (luau)
		{
			const Wui::WuiRect reloadRect { rect.X, rect.Y + y, std::min(rect.W, 160.0f), 22.0f };
			if (Wui::ActionButton(ctx, Wui::HashId("lua.reload"), reloadRect,
				Wui::Tr("panel.properties.reload_script", "Reload Script"),
				theme,
				!m_ReadOnly,
				m_ReadOnly
					? Wui::Tr("panel.properties.script_readonly_reason",
						"Play/Simulate is read-only: pause or stop to reload the script")
					: Wui::Tr("panel.properties.reload_script.tooltip",
						"Reload this instance from the script asset (same entry as the Scripts panel and script.reload)")))
			{
				std::string message;
				const bool ok = EditorLayer::ReloadLuauScriptComponent(*lua, scene, &message);
				m_LuaReloadOk = ok;
				m_LuaReloadMessage = ok
					? message : (Wui::Tr("panel.properties.reload_failed", "failed:") + " " + message);
				m_LuaReloadHandle = handle;
				WLD_CORE_INFO("[hot-reload] properties button (handle={0}, path='{1}'): {2}", handle,
					lua->ScriptPath, m_LuaReloadMessage);
			}
			y += 26.0f;
			if (m_LuaReloadHandle == handle && !m_LuaReloadMessage.empty())
			{
				Label(ctx, { rect.X + 4.0f, rect.Y + y - 4.0f }, TruncateForPanel(m_LuaReloadMessage),
					m_LuaReloadOk ? theme.TextMuted : Wui::WuiColor { 1.0f, 0.4f, 0.4f, 1.0f }, 12.0f);
				RegisterNode(Wui::HashId((PropPath(schema.DisplayName, "reload_result")).c_str()), "text",
					{ rect.X, rect.Y + y - 6.0f, rect.W, 16.0f }, m_LuaReloadMessage, m_LuaReloadMessage, false);
				y += 16.0f;
			}
		}

		if (changed)
			m_Host.MarkDocumentDirty();
		return y;
	}

	float PropertiesPanel::DrawTransformInspector(Wui::WuiContext& ctx, const Wui::WuiRect& rect,
		TransformComponent& transform, const Schema::TypeSchema& schema, const Wui::WuiRect& visibleRect,
		std::vector<std::string>* changedFields)
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
		// 逐行取"这一行是否被编辑":覆盖登记要精确到 Location/Rotation/Scale,
		// 不能只记"Transform 动过"(否则覆盖计数与实际改动对不上)。
		// 行高由库件的横排/竖排决定(窄控件列竖排 = 3 倍行高),下一行用上一行的返回值累加;
		// 行内无障碍节点由库件登记(前面板自算的 reachable 不再被向量行使用)。
		bool locationChanged = false;
		const float locationHeight = DrawVec3Row(ctx, PropPath(typeName, "Location"), rect.X, rect.Y, rect.W,
			locationLabel, transform.Location, theme, locationChanged);
		bool rotationChanged = false;
		const float rotationHeight = DrawVec3Row(ctx, PropPath(typeName, "Rotation"), rect.X,
			rect.Y + locationHeight, rect.W, rotationLabel, rotationDegrees, theme, rotationChanged);
		bool scaleChanged = false;
		const float scaleHeight = DrawVec3Row(ctx, PropPath(typeName, "Scale"), rect.X,
			rect.Y + locationHeight + rotationHeight, rect.W, scaleLabel, transform.Scale, theme, scaleChanged);
		changed |= locationChanged || rotationChanged || scaleChanged;
		if (changed)
		{
			transform.SetTransform(transform.Location, glm::radians(rotationDegrees), transform.Scale);
			m_Host.MarkDocumentDirty();
			if (changedFields)
			{
				if (locationChanged) changedFields->push_back(typeName + ".Location");
				if (rotationChanged) changedFields->push_back(typeName + ".Rotation");
				if (scaleChanged) changedFields->push_back(typeName + ".Scale");
			}
		}
		return locationHeight + rotationHeight + scaleHeight;
	}

	float PropertiesPanel::DrawCameraInspector(Wui::WuiContext& ctx, const Wui::WuiRect& rect, void* instance,
		const Schema::TypeSchema& schema, const Wui::WuiRect& visibleRect,
		std::vector<std::string>* changedFields)
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
		// 覆盖字段名与属性行 id 同一口径(PropPath 去掉 "properties." 前缀)。
		const auto markField = [changedFields](const std::string& field)
		{
			if (changedFields)
				changedFields->push_back(field);
		};

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
				markField(typeName + ".Primary");
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
				markField(typeName + ".FixedAspectRatio");
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
				markField(cameraType + ".ProjectionType");
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
				markField(cameraType + ".Perspective.FOV");
			}
			if (nearChanged)
			{
				camera->SetPerspectiveNearClip(nearClip);
				changed = true;
				markField(cameraType + ".Perspective.NearClip");
			}
			if (farChanged)
			{
				camera->SetPerspectiveFarClip(farClip);
				changed = true;
				markField(cameraType + ".Perspective.FarClip");
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
				markField(cameraType + ".Orthographic.Zoom");
			}
			if (nearChanged)
			{
				camera->SetOrthographicNearClip(nearClip);
				changed = true;
				markField(cameraType + ".Orthographic.NearClip");
			}
			if (farChanged)
			{
				camera->SetOrthographicFarClip(farClip);
				changed = true;
				markField(cameraType + ".Orthographic.FarClip");
			}
		}

		if (changed)
			m_Host.MarkDocumentDirty();
		return y + 2.0f;
	}
}
