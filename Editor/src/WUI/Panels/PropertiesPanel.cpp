#include "PropertiesPanel_Internal.h"
#include "World/Core/Utf8.h"

namespace World
{

using namespace PropertiesPanelDetail;

namespace PropertiesPanelDetail
{

int VecFieldLayout(float controlWidth){
			return controlWidth < kVecFieldNarrowWidth ? 1 : 0;
		}


		// 与 WuiWidgets.cpp 的 VecFieldCore 同一条排布:横排 Vec2/Vec3 = 一行,Vec4 = 2×2
		// 两行;竖排(layout 1)= 每个分量一行。
int VecFieldRows(int layout, int components){
			if (layout == 1)
				return components;
			return components == 4 ? 2 : 1;
		}


float VecFieldHeight(int layout, int components){
			return kVecFieldSlotHeight * static_cast<float>(VecFieldRows(layout, components));
		}


		// ---- 无障碍登记(与 WuiWidgets.cpp 的 RegisterAccessNode 同一格式) ----
		// 面板内的字段/只读值/自定义检查器统一登记,id 由脚本用 Wui::HashId 直接计算。
void RegisterNode(Wui::WuiId id, const char* kind, const Wui::WuiRect& rect, const std::string& label, const std::string& value, bool enabled , const std::string& tooltip , bool focused ){
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

std::string FormatFloatText(float value, int decimals ){
			char buffer[48] = {};
			std::snprintf(buffer, sizeof(buffer), "%.*f", decimals, value);
			return buffer;
		}


		// ---- CPPT-3-FIX1:数值行的 Unit / Step 接线 ----
		// Step → 显示小数位(0.1 → 1 位、0.01 → 2 位、≥1 → 0 位;未声明 = 既有 3 位口径)。
int StepDecimals(bool hasStep, float step){
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
void DrawScriptIntControl(Wui::WuiContext& ctx, Wui::WuiId id, const Wui::WuiRect& rect, int64_t& raw, int64_t lo, int64_t hi, const Schema::FieldMetadata& meta, const Wui::WuiTheme& theme){
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


		// 内容根(开发布局 = 当前项目根的 assets,打包由清单决定):**解析一次**缓存起来。
		// 实例条每帧都要判断"来源资产还在不在",每帧重读 project.we.yaml 是不可接受的。
const std::filesystem::path& CachedContentRoot(){
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
bool PrefabSourceExists(const std::string& path){
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
std::string FormatValueByVariant(const Schema::Value& value){
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
std::string EnumNameOf(const Schema::EnumSchema& schema, int64_t raw){
			if (const char* name = schema.FindName(raw))
				return name;
			return std::to_string(raw);
		}


int64_t EnumRawOf(const Schema::EnumSchema& schema, const Schema::Value& value){
			if (std::holds_alternative<int64_t>(value))
				return std::get<int64_t>(value);
			if (std::holds_alternative<uint64_t>(value))
				return static_cast<int64_t>(std::get<uint64_t>(value));
			return 0;
		}


std::string FormatReadOnlyValue(const Schema::FieldSchema& field, const Schema::Value& value){
			if (field.K == Schema::Kind::Enum)
			{
				const Schema::EnumSchema* schema = field.GetEnum ? field.GetEnum() : nullptr;
				if (schema)
					return EnumNameOf(*schema, EnumRawOf(*schema, value));
			}
			// CPPT-3:没有行控件的数学类型(IVec*/UVec*/Quat/Mat*)→ 只读摘要显示类型名。
			// 这些值不进属性表/不进存档(引擎侧同步成 ReadOnly 摘要行),显示 "(0, 0, 0)" 一类
			// 伪值会误导 —— 与 Luau 裸 table 的 `table` 摘要同一口径。
			if (ComponentPropertyModel::IsSummaryKind(field.K))
				return ScriptLeafTypeText(field.K);
			return FormatValueByVariant(value);
		}


		// 自定义检查器的无障碍 id 契约:properties.TransformComponent.Location 等
		// (脚本用同一 FNV-1a 32 位算法直接计算,无需先 ui.tree)。
std::string PropPath(const std::string& typeName, const std::string& fieldName){
			return "properties." + typeName + "." + fieldName;
		}


		// ---- schema 显示点本地化 ----
		// DisplayName / 字段名是生成文件里的**稳定标识**:属性行 id(properties.<Type>.<Field>)、
		// 分区折叠键(prop.open.<DisplayName>)、控件 hash、Lua 代理名都由它派生,所以这里
		// 只翻译**显示文案**,一律不回写 schema,也不改任何 id / 持久化 key / hash。
		// 目录键用短类型名:World::TransformComponent → schema.component.TransformComponent。
std::string SchemaTypeKeyName(const Schema::TypeSchema& schema){
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
std::string HumanizeIdentifier(const std::string& name){
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
std::string TermText(const Wui::LocalizedLabel& label){
			return label.Term.empty() ? label.Text : label.Text + " (" + label.Term + ")";
		}


		// 组件分区标题 / "Add Component" 菜单项的显示文案(键用短类型名,标识仍用 schema->DisplayName)。
Wui::LocalizedLabel SchemaComponentLabel(const Schema::TypeSchema& schema){
			return Wui::TrLabel("schema.component." + SchemaTypeKeyName(schema), schema.DisplayName);
		}


		// 字段标签显示文案:Meta.DisplayName 优先,空则人类可读化 C++ 字段名;
		// 键分别用 DisplayName / 字段名(两者都是生成物里的稳定标识)。
Wui::LocalizedLabel SchemaFieldLabel(const Schema::FieldSchema& field){
			if (!field.Meta.DisplayName.empty())
				return Wui::TrLabel("schema.field." + field.Meta.DisplayName, field.Meta.DisplayName);
			return Wui::TrLabel("schema.field." + field.Name, HumanizeIdentifier(field.Name));
		}


		// 自定义检查器按字段名取 schema 字段,与通用路径共用同一显示文案(找不到字段时仍可读)。
Wui::LocalizedLabel SchemaFieldLabel(const Schema::TypeSchema& schema, const std::string& fieldName){
			for (const Schema::FieldSchema& field : schema.Fields)
				if (field.Name == fieldName)
					return SchemaFieldLabel(field);
			return Wui::TrLabel("schema.field." + fieldName, HumanizeIdentifier(fieldName));
		}


		// 自定义检查器的枚举 → 显示文案(下拉选项与只读值共用):键 panel.properties.camera.<name>;
		// 写回相机的仍是 schema 枚举 raw 值 —— 枚举名只用来查显示文案,不参与任何比较。
std::string CameraProjectionLabel(const std::string& enumName){
			if (enumName == "Perspective")
				return Wui::Tr("panel.properties.camera.perspective", "Perspective");
			if (enumName == "Orthographic")
				return Wui::Tr("panel.properties.camera.orthographic", "Orthographic");
			return enumName;   // 未知枚举名原样显示,不猜翻译
		}


Wui::WuiRect ComponentRect(const Wui::WuiRect& rect, int index, float height){
			return { rect.X, rect.Y + kRowHeight * static_cast<float>(index), rect.W, height };
		}


		// 自定义检查器的 Vec3 行(度/单位由调用方处理)。VEC-A4:控件本体 = 库件
		// `Wui::Vec3Field`(轴标签 / 拖动 / 键入 / ↑↓ 是库件那一套),行 id 仍是
		// `properties.<组件>.<字段>`;分量无障碍节点由库件登记(`...axis.0/1/2`)。
		// 返回本行占用的高度(竖排时是单行的 3 倍)。
float DrawVec3Row(Wui::WuiContext& ctx, const std::string& baseId, float x, float y, float width, const Wui::LocalizedLabel& label, glm::vec3& value, const Wui::WuiTheme& theme, bool& changed){
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
bool DrawFloatRow(Wui::WuiContext& ctx, const std::string& idText, const Wui::WuiRect& row, const Wui::LocalizedLabel& label, float& value, float lo, float hi, const Wui::WuiTheme& theme, bool reachable){
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
void DrawReadOnlyRow(Wui::WuiContext& ctx, const std::string& idText, const Wui::WuiRect& row, const Wui::LocalizedLabel& label, const std::string& value, const Wui::WuiTheme& theme){
			const float labelBudget = std::min(140.0f, row.W * 0.45f) - 4.0f;
			Wui::LabelWithTerm(ctx, { row.X + 4, row.Y + 3 }, label.Text, label.Term, theme.TextMuted, 13.0f, theme, labelBudget);
			Label(ctx, { row.X + std::min(140.0f, row.W * 0.45f), row.Y + 3 }, value, theme.Text, 13.0f);
			RegisterNode(Wui::HashId(idText.c_str()), "text", row, TermText(label), value, false);
		}


		// ---- 2026-09-26 脚本组件重写:统一脚本检视器的三个数据侧小工具 ----
		//
		// `PropertyNode::Value` 的类型编码必须与 schema 一致
		// (Bool→bool、Int*→对应宽度、Float→float、Double→double、String→string、
		//  Vec2/3/4→glm::vec2/3/4):属性行复用 `DrawSchemaFields`,那里按 Kind 直接
		// `std::get<T>(value)`,值停在 monostate(刚声明还没填值 / 手改过的场景缺 Value)
		// 会抛 std::bad_variant_access —— 向量行必须在下面补零值兜底,否则 A 期放行
		// Vec2/3/4 后脚本属性里"声明了但没值"的向量行会直接崩面板。
Schema::Value DefaultPropertyValue(Schema::Kind kind){
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
				case Schema::Kind::Name: return Schema::Value(std::string());   // 名字的规范零值 = 空名字
				case Schema::Kind::Text: return Schema::Value(std::string());   // 有界文本的零值 = 空串
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
				// 与 `ComponentPropertyModel::ValueMatchesKind` 同一口径,不往 variant 里塞表数据。
				case Schema::Kind::Object: return Schema::Value();
				default: return Schema::Value();
			}
		}


bool PropertyNodeValueMatchesType(const PropertyNode& property){
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
				case Schema::Kind::Name: return std::holds_alternative<std::string>(property.Value);
				case Schema::Kind::Text: return std::holds_alternative<std::string>(property.Value);
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
		// 真实默认值来自脚本本体,由引擎侧的属性入口填进 `PropertyNode.Value`(到了这里自然显示真值)。
		//
		// VEC-F2:单项 `↺` 现在只把该行清成"未设"(不再触发整表重同步)—— 展示值必须自己回落到
		// 声明默认值 `PropertyNode::Default`,否则复位会让该行瞬间显示成 0(与 D1 的
		// "未设 = 显示脚本默认值、存档不写"口径不一致)。同样是**只读回退**,不写回组件。
Schema::Value PropertyNodeDisplayValue(const PropertyNode& property){
			if (std::holds_alternative<std::monostate>(property.Value)
				&& ComponentPropertyModel::ValueMatchesKind(property.Default, property.Type))
				return property.Default;
			return PropertyNodeValueMatchesType(property)
				? property.Value : DefaultPropertyValue(property.Type);
		}


		// VEC-H4:**行展示值** = `PropertyNodeDisplayValue`,但"存的值类型与声明不符"时返回
		// 空表(monostate),让行落到 `—` 占位(反模式 4:多值/不可用时不许显示伪零)。
		// 判据只看"真的存了值且类型不符";`Value` 本身为空(未设)+ 默认值不可用仍是旧行为(显示零值,
		// 可编辑、可写盘)—— A 期验收的 Ghost 行(retro 25/25)因此逐条不变。
Schema::Value PropertyNodeDisplayValueForRow(const PropertyNode& property){
			if (!std::holds_alternative<std::monostate>(property.Value)
				&& !PropertyNodeValueMatchesType(property))
				return Schema::Value();
			return PropertyNodeDisplayValue(property);
		}


		// ---- VEC-C2:脚本属性行的**类型文案**(方案 v4 §1)----
		//
		// 脚本属性行没有注解说明(`PropertyNode::Doc` 为空)时,行悬停/读屏回落成类型文案
		// (`number/string/boolean/vec3/table/struct/array/map`),不再给英文兜底
		// "No description for this field"。类型名是脚本作者写的标识符(`---@field Speed number`),
		// 与脚本字段名同一条口径:**不过本地化目录**(不查 `schema.field.*`,也不进目录)。
std::string ScriptLeafTypeText(Schema::Kind kind){
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


std::string PropertyNodeTypeText(const PropertyNode& property){
			switch (property.Collection)
			{
				case PropertyCollection::Array: return "array";
				case PropertyCollection::Map: return "map";
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
			if (property.Type == Schema::Kind::Name)
				return std::string("name");
			if (property.Type == Schema::Kind::Text)
				return std::string("text");
				return property.TypeName.empty() ? std::string("asset") : property.TypeName;
			return ScriptLeafTypeText(property.Type);
		}


		// 数组/映射即使**一个元素都没有**也可以展开(底部有 `+` 加元素/键);结构化表没有子字段时
		// 才是只读摘要(裸 table)。两处判据(顶层行 / 嵌套子行)共用这一份,避免口径分叉。
bool PropertyNodeExpandable(const PropertyNode& property){
			if (property.ReadOnly)
				return false;
			if (property.Collection == PropertyCollection::Array
				|| property.Collection == PropertyCollection::Map)
				return true;
			return !property.Children.empty();
		}


		// 字段 schema 的类型文案:Object 行的合成 schema 把类型文案写在 `Meta.DisplayName`
		// (只读摘要行的文本同源),叶子行按 Kind 反推。
std::string ScriptFieldTypeText(const Schema::FieldSchema& field){
			if (field.K != Schema::Kind::Object)
				return ScriptLeafTypeText(field.K);
			if (!field.Meta.DisplayName.empty())
				return field.Meta.DisplayName;
			return field.GetNested ? "struct" : "table";
		}


ScriptTableSchemaArena& ScriptTableArena(){
			static thread_local ScriptTableSchemaArena arena;
			return arena;
		}


ScriptEnumArena& ScriptEnumArenaStore(){
			static thread_local ScriptEnumArena arena;
			return arena;
		}


		// PURE-ECS:两处 per-draw 合成 arena 的复位。两者原本只服务脚本属性路径(M8 之后那条路径
		// 已死),`Used` 从来没人清零;接上原生容器字段后必须**每个组件**重新开始,否则第 257 个
		// 节点(或第 33 个枚举)开始容器行会静默退化成只读摘要。
void ResetScriptRowArenas(){
			ScriptTableArena().Used = 0;
			ScriptEnumArenaStore().Used = 0;
		}


		// 从注册表取枚举 schema 的稳定访问器(nullptr = 找不到 / arena 溢出 → 行降级只读摘要)。
ScriptEnumGetter ScriptEnumAccessorFor(const Schema::SchemaRegistry& schemas, const std::string& enumName){
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
PropertyCollection ScriptTableCollectionOf(const Schema::TypeSchema* node){
			if (!node)
				return PropertyCollection::None;
			ScriptTableSchemaArena& arena = ScriptTableArena();
			// 用地址比较(不同对象之间的指针序在标准里未定义;地址转整数后比较是确定的)。
			const uintptr_t address = reinterpret_cast<uintptr_t>(node);
			const uintptr_t begin = reinterpret_cast<uintptr_t>(arena.Nodes.data());
			const uintptr_t end = reinterpret_cast<uintptr_t>(arena.Nodes.data() + arena.Used);
			if (address < begin || address >= end)
				return PropertyCollection::None;
			return arena.Collections[static_cast<size_t>(node - arena.Nodes.data())];
		}


		// VEC-H6:arena 节点 → 它描述的 `PropertyNode`。返回 nullptr = 该节点不在 arena
		// (顶层字段行的合成 schema —— 那时 instance 就是这条属性本身,见 ScriptRowModel)。
const PropertyNode* ScriptTableNodeOwner(const Schema::TypeSchema* node){
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


		// VEC-H6:当前正在画的行(合成脚本表)对应哪一条 `PropertyNode`。
		//  · 嵌套节点:instance = 父属性 → 行 = 父->Children[fieldIndex](与节点 Fields 同序);
		//  · 顶层行:合成 schema 不在 arena 里,instance 就是那条属性本身。
		// 调用方必须已确认这是合成路径(m_ScriptInspectingScriptRows)—— Play 里 C++ 实例走真实
		// 结构体指针,cast 成 PropertyNode* 是未定义行为。
const PropertyNode* ScriptRowModel(const Schema::TypeSchema& node, const void* instance, size_t fieldIndex, bool scriptRows){
			if (const PropertyNode* owner = ScriptTableNodeOwner(&node))
				return fieldIndex < owner->Children.size() ? &owner->Children[fieldIndex] : nullptr;
			// 顶层脚本行:instance 就是那条属性本身 —— **只对脚本合成路径成立**。
			// PURE-ECS 起原生组件的容器行也走 arena(`ScriptTableNodeOwner` 命中上一条分支),
			// 所以这里必须把"instance 强转成 PropertyNode*"限制在脚本路径,避免把真实
			// C++ 结构体指针当属性读(未定义行为)。
			return scriptRows ? static_cast<const PropertyNode*>(instance) : nullptr;
		}


		// VEC-H6:一条**叶子**属性是否偏离脚本声明的默认值(与面板显示同源):
		//  · 未设(monostate)→ 面板显示的就是默认值 ⇒ 一致(不出现 ↺);
		//  · 声明没给默认值(Default 也是 monostate)→ 有值即偏离;
		//  · 都有值 → ValuesEqual 逐字段比(浮点按位相等,不做容差)。
bool ScriptLeafRowModified(const PropertyNode& row){
			if (ComponentPropertyModel::IsUnset(row))
				return false;
			if (std::holds_alternative<std::monostate>(row.Default))
				return true;
			return !ComponentPropertyModel::ValuesEqual(row.Value, row.Default);
		}


const ComponentPropertyModel::Declaration* FindDeclarationField( const ComponentPropertyModel::Declaration& parent, const std::string& name){
			for (const ComponentPropertyModel::Declaration& field : parent.Fields)
				if (field.Name == name)
					return &field;
			return nullptr;
		}


		// VEC-H6:一条属性(叶子/容器,**递归**)是否偏离脚本声明默认 —— `↺` 的可见性判据。
		// 容器 = 任一子行偏离 **或** 形状偏离:数组/映射的默认形状 = 声明里的元素/键行(顺序 + 行名);
		// 结构体的形状由声明决定(合并时按声明重建),只需递归看值。
		// declaration 可空(场景独有行 / 声明读不出来)→ 退化成"只看值",不猜形状。
bool ScriptRowModified(const PropertyNode& row, const ComponentPropertyModel::Declaration* declaration){
			const bool container = row.Type == Schema::Kind::Object
				|| row.Collection == PropertyCollection::Array
				|| row.Collection == PropertyCollection::Map;
			if (!container)
				return ScriptLeafRowModified(row);
			if (declaration && !declaration->FieldsUnknown && !declaration->ReadOnly
				&& (row.Collection == PropertyCollection::Array
					|| row.Collection == PropertyCollection::Map))
			{
				// 形状:元素个数/键数或行名序列与声明的默认形状不同 = 改过(增/删/改键)。
				if (row.Children.size() != declaration->Fields.size())
					return true;
				for (size_t index = 0; index < row.Children.size(); ++index)
					if (row.Children[index].Name != declaration->Fields[index].Name)
						return true;
			}
			for (const PropertyNode& child : row.Children)
				if (ScriptRowModified(child, declaration ? FindDeclarationField(*declaration, child.Name) : nullptr))
					return true;
			return false;
		}


		// ---- CPPT-6-ED-COLLECTIONS:空 struct 容器的元素模板 ----
		//
		// 为什么需要:集合的 `+` 原来只从"最后一个已有元素"抄形状(VEC-C2);C++ 脚本的容器在
		// schema 声明里**没有元素行**(元素初值属于脚本成员,schema 看不到),空容器加出来的命名
		// struct 元素 = 没有子字段的裸 table(只读摘要)。这里按 schema/节点形状补一份元素模板:
		// 子字段的名字/类型/说明/初值齐全,空容器 `+` 直接得到可编辑行。
		//
		// 形状口径(与引擎 `ComponentPropertyModel::ElementValueOf` / `Schema::ReadStructValue` 对齐):
		//   * 命名 struct 元素 = Type=Object + **Collection=None** + Children(字段名 → 值):
		//     `ElementValueOf` 的 Object 分支按字段名折成 ValueMap,而 `Collection=Struct` 会折成
		//     ValueList —— `ReadStructValue` 只吃 ValueMap(CPPT-6 元素值契约)。
		//   * 元素是 Enum/Asset 时带上类型名(与容器行同一份 `TypeName`)—— 下拉/资产选择器靠它建;
		//   * 摘要 Kind(IVec*/UVec*/Quat/Mat*)与拿不到 schema 的嵌套保持只读,与引擎声明同一降级口径;
		//   * 叶子初值 = schema 声明的 `Default`(类型不符/未声明 → 类型零值),`Default` 留空
		//     (monostate):这一行算"场景自己的值",保存/重开由场景形状保留(C++ 容器没有声明形状)。
Schema::Value CollectionElementLeafSeed(const Schema::FieldSchema& field){
			if (ComponentPropertyModel::ValueMatchesKind(field.Default, field.K))
				return field.Default;
			if (field.K == Schema::Kind::Enum)
			{
				// 枚举零值按声明名的有符号口径(与生成访问器同一编码:无符号枚举 = uint64)。
				const Schema::EnumSchema* enumSchema = field.GetEnum ? field.GetEnum() : nullptr;
				if (enumSchema && !enumSchema->IsSigned)
					return Schema::Value(static_cast<uint64_t>(0));
			}
			return DefaultPropertyValue(field.K);
		}


void AppendCollectionElementFields(std::vector<PropertyNode>& out, const Schema::TypeSchema& type, int depth){
			if (depth > kScriptTableMaxDepth)
				return;
			for (const Schema::FieldSchema& field : type.Fields)
			{
				if (!ComponentPropertyModel::IsPropertyKind(field.K) && !ComponentPropertyModel::IsSummaryKind(field.K))
					continue;
				if (out.size() >= kScriptTableMaxChildren)
					return;   // 护栏与合成 arena 同量级(模板建得出,就画得出来)
				PropertyNode row;
				row.Name = field.Name;
				row.Type = field.K;
				row.Doc = field.Meta.Doc;
				if (field.Collection != Schema::CollectionKind::None)
				{
					// 命名 struct 里再套容器:建**空容器行**(形状来自 schema),元素由用户再加。
					row.Type = Schema::Kind::Object;
					row.Collection = field.Collection == Schema::CollectionKind::Map
						? PropertyCollection::Map : PropertyCollection::Array;
					row.ElementKind = field.ElementKind;
					row.KeyKind = field.KeyKind;
					if (field.ElementKind == Schema::Kind::Object)
					{
						const Schema::TypeSchema* nested = field.GetElementNested ? field.GetElementNested() : nullptr;
						if (!nested)
							continue;   // 元素 schema 拿不到:不进模板(与引擎"没 schema 不猜"同口径)
						row.TypeName = nested->Id.Name;
					}
					else if (field.ElementKind == Schema::Kind::Enum)
					{
						const Schema::EnumSchema* enumSchema = field.GetEnum ? field.GetEnum() : nullptr;
						if (enumSchema)
							row.TypeName = enumSchema->Name;
						else
							row.ReadOnly = true;
					}
					else if (field.ElementKind == Schema::Kind::Asset)
						row.TypeName = field.AssetTypeName ? field.AssetTypeName : "";
					else if (ComponentPropertyModel::IsSummaryKind(field.ElementKind))
						row.ReadOnly = true;
				}
				else if (field.K == Schema::Kind::Object)
				{
					const Schema::TypeSchema* nested = field.GetNested ? field.GetNested() : nullptr;
					if (!nested)
						continue;   // 引擎口径:嵌套 schema 拿不到的 Object 不进属性表
					row.Collection = PropertyCollection::Struct;
					row.TypeName = nested->Id.Name;
					AppendCollectionElementFields(row.Children, *nested, depth + 1);
					if (row.Children.empty())
						continue;   // 没有子字段的 struct 只能是裸 table 摘要 —— 不造一行空行
				}
				else
				{
					if (field.K == Schema::Kind::Enum)
					{
						const Schema::EnumSchema* enumSchema = field.GetEnum ? field.GetEnum() : nullptr;
						if (enumSchema)
							row.TypeName = enumSchema->Name;
						else
							row.ReadOnly = true;
					}
					else if (field.K == Schema::Kind::Asset)
						row.TypeName = field.AssetTypeName ? field.AssetTypeName : "";
					else if (ComponentPropertyModel::IsSummaryKind(field.K))
						row.ReadOnly = true;
					row.Value = CollectionElementLeafSeed(field);
				}
				out.push_back(std::move(row));
			}
		}


PropertyNode MakeCollectionElement(const PropertyNode& container, const Schema::SchemaRegistry* schemas, bool plainRows){
			PropertyNode child;
			child.Type = container.ElementKind;
			if (!container.Children.empty())
			{
				const PropertyNode& model = container.Children.back();
				child.Type = model.Type;
				child.TypeName = model.TypeName;
				child.Collection = model.Collection;
				child.ElementKind = model.ElementKind;
				child.KeyKind = model.KeyKind;
				child.ReadOnly = model.ReadOnly;
				// PURE-ECS:模型行抄形状**不抄子行** —— 原生路径的命名 struct 元素没有声明可
				// 依赖,子行只能从元素 schema 建(脚本路径的元素行来自 Luau 声明,那里不需要)。
				if (plainRows && child.Type == Schema::Kind::Object)
					HydratePlainStructElement(child, schemas);
			}
			else
			{
				// 叶子(Float/Vec3/Enum/Asset)只多带一份类型名 —— Enum/Asset 的下拉靠它;
				// 命名 struct 按元素 schema 递归补齐子字段(这就是"空容器 `+` 直接可编辑"的落点)。
				child.TypeName = container.TypeName;
				child.ReadOnly = container.ReadOnly;
				if (child.Type == Schema::Kind::Object && !child.ReadOnly && schemas && !container.TypeName.empty())
					if (const Schema::TypeSchema* elementSchema = schemas->Find(container.TypeName))
						AppendCollectionElementFields(child.Children, *elementSchema, 0);
				// 命名 struct 元素的行值 = 空 `ValueMap`(字段值在 `Children` 里):
				//   * 引擎 `ElementValueOf` 对 Collection=None 的 Object 行按字段名折成 `ValueMap`
				//     (`Schema::ReadStructValue` 只吃 `ValueMap`;Collection=Struct 会被折成 ValueList);
				//   * `IsSceneRecorded` 对 Collection=None 的行**只看 Value** —— 留 monostate 会被判"未设",
				//     整个容器不写场景(`+` 出来的元素保存后就没了);空 `ValueMap` 让它按"场景自己的值"落盘。
				if (!child.Children.empty())
					child.Value = Schema::Value(Schema::ValueMap {});
			}
			// Object 行(结构化表 / 命名 struct 元素)的值是 Children,Value 保持上面定好的形态;
			// 叶子行才回落类型零值。
			if (child.Type != Schema::Kind::Object)
				child.Value = DefaultPropertyValue(child.Type);
			return child;
		}


bool CollectionKeyTaken(const PropertyNode& container, const std::string& key){
			return std::any_of(container.Children.begin(), container.Children.end(),
				[&key](const PropertyNode& child) { return child.Name == key; });
		}


		// 数组行名 = 下标字符串 1..n:删掉中间元素后重排,让行 id / 存档顺序 / 脚本写回(`1..n`)
		// 共用同一份下标口径。
void RenumberArrayChildren(PropertyNode& container){
			for (size_t index = 0; index < container.Children.size(); ++index)
				container.Children[index].Name = std::to_string(index + 1);
		}


		// ---- VEC-F2:两级复原的公共口径 ----
		//
		// 单项 `↺` = 把该行清成"未设"(monostate):显示走 `PropertyNodeDisplayValue` 回落
		// 声明默认值,存档按 D1 判定"未设 → 整条不写"。**不触发任何整表重同步** ——
		// 旧实现在复位后强制 `SyncScriptDeclarations`,会把同一集合里用户刚做的增删(形状)按声明重建,
		// 表现就是"点一个元素的 `↺`,整个集合都回去了"。
		//
		// 集合头 `↺` = 复原整个集合:Luau 只对**这一条声明**做一次 `SyncFromDeclarations`
		// (默认形状 + 默认值,其它集合/其它属性一律不动);C++ 结构化表没有数组/映射
		// (增删只存在于脚本侧),递归把叶子清成默认值即可。
		// 按"名字路径"解析一条脚本属性(名字逐段比较:映射键里的 '.' 不会被拆成两段)。
PropertyNode* ResolveScriptRowPath(std::vector<PropertyNode>& properties, const std::vector<std::string>& path, size_t index ){
			if (index >= path.size())
				return nullptr;
			for (PropertyNode& property : properties)
			{
				if (property.Name != path[index])
					continue;
				if (index + 1 == path.size())
					return &property;
				return ResolveScriptRowPath(property.Children, path, index + 1);
			}
			return nullptr;
		}


const ComponentPropertyModel::Declaration* ResolveDeclarationPath( const std::vector<ComponentPropertyModel::Declaration>& declarations, const std::vector<std::string>& path, size_t index ){
			if (index >= path.size())
				return nullptr;
			for (const ComponentPropertyModel::Declaration& declaration : declarations)
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
void ResetScriptRowValues(PropertyNode& row){
			const bool container = row.Type == Schema::Kind::Object
				|| row.Collection == PropertyCollection::Array
				|| row.Collection == PropertyCollection::Map;
			if (container)
			{
				for (PropertyNode& child : row.Children)
					ResetScriptRowValues(child);
				return;
			}
			row.Value = row.Default;
		}


std::string ScriptRowPathText(const std::vector<std::string>& path){
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
std::string ScriptCollectionHeadResetLabel(PropertyCollection collection){
			switch (collection)
			{
				case PropertyCollection::Array: return "Reset the whole array";
				case PropertyCollection::Map: return "Reset the whole map";
				default: return "Reset the whole struct";
			}
		}


		// 集合头 `↺` 的悬停说明:说清"是整集合 + 会丢什么"(用户口径:复原整个集合,丢弃所有增删改)。
const char* ScriptCollectionHeadResetDoc(){
			return "Restore the entire collection to the script's default shape and values "
				"(discards all adds, edits and removals)";
		}


		// 单项 `↺` 的悬停说明:与集合头区分 —— 只动这一项。
const char* ScriptItemResetDoc(){
			return "Restore only this item to the script's default value (the row goes back to unset)";
		}


		// 顶层 Object 属性行的 instance 就是该属性本身:子 schema 的实例直接透传,
		// 子字段访问器再从它身上取 `Children[i]`。
void* ScriptTableIdentityPtr(void* instance){ return instance; }

const void* ScriptTableIdentityPtrConst(const void* instance){ return instance; }


		// 把一条结构化表属性递归合成成 arena 节点(返回下标;失败 = kScriptTableNoNode)。
		// `idPath` 同时是合成 TypeSchema 的 DisplayName:DrawSchemaFields 递归时拿它当行 id
		// 前缀,于是子行 id = `properties.<组件>.<属性>.<子字段>`(递归同名规则)。
		// CPPT7R-T1(C2):`declared` = 声明**这一行**的 `FieldSchema`(顶层 = 脚本/组件 schema 里的
		// 那条字段;结构体成员 = 结构体 schema 里的那条字段)—— 容器的 Range/Unit/Step 描述的是
		// **元素**,靠它透传给元素行(见下面的 elementRow 块)。
size_t BuildScriptTableSchema(const PropertyNode& property, const std::string& idPath, size_t depth, const Schema::SchemaRegistry* schemas, const Schema::FieldSchema* declared ){
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
				const PropertyNode& child = property.Children[index];
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
				else if (ComponentPropertyModel::IsSummaryKind(child.Type))
				{
					field.Meta.ReadOnly = true;
				}
				// CPPT7R-T1(C2):容器**元素行**吃父字段的编辑元数据(Range/Unit/Step)。引擎的
				// `PropertyNode` 只带 Doc/ReadOnly/TypeName(注解不进属性表/不进存档),
				// 元素行又是面板按父字段的 Children 合成的 ⇒ 元数据只能从声明里透传。
				// 口径 = 与叶子行逐字段一致(未声明范围就不夹取、没有范围+单位就保持 DragFloat,
				// 见本文件 Float/Int 分支):**不新增行为,只补齐缺的那一份声明**。
				// 结构化表元素(Object)不在这一层吃父元数据:它的子字段各自有 schema 声明。
				const bool elementRow = child.Type != Schema::Kind::Object
					&& (property.Collection == PropertyCollection::Array
						|| property.Collection == PropertyCollection::Map);
				if (declared && elementRow)
				{
					field.Meta.Min = declared->Meta.Min;
					field.Meta.Max = declared->Meta.Max;
					field.Meta.Unit = declared->Meta.Unit;
					field.Meta.Step = declared->Meta.Step;
				}
				if (child.Type == Schema::Kind::Object)
				{
					// 裸 table / 空结构 / 超护栏 → 只读摘要行(摘要文本由 Meta.DisplayName 携带,
					// 面板侧与 DrawSchemaFields 的 scriptPropertyRow 摘要分支同一判据)。
					// VEC-C2:类型文案(含 `struct`/`array`/`map`)也走 Meta.DisplayName —— 没有注解
					// 说明时行悬停用它回落(v4 §1);空数组/空映射仍可展开(子行只有底部 `+`)。
					field.Meta.DisplayName = PropertyNodeTypeText(child);
					// 结构体成员是**声明行**:名字在父结构体的 schema 里查得到那份 FieldSchema
					// ⇒ 往下传,使"结构体里的容器"的元素同样吃到元数据(递归透传)。
					const Schema::FieldSchema* childDeclared = nullptr;
					if (schemas && property.Collection == PropertyCollection::Struct
						&& !property.TypeName.empty())
					{
						if (const Schema::TypeSchema* owner = schemas->Find(property.TypeName))
							for (const Schema::FieldSchema& candidate : owner->Fields)
								if (candidate.Name == child.Name)
								{
									childDeclared = &candidate;
									break;
								}
					}
					const size_t childNode = PropertyNodeExpandable(child)
						? BuildScriptTableSchema(child, idPath + "." + child.Name, depth + 1, schemas, childDeclared)
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
		// CPPT7R-T1(C2):`declared` = 容器字段自己的 FieldSchema(顶层调用方从脚本/组件 schema 查),
		// 它带着 Range/Unit/Step —— 合成元素行时透传(见 BuildScriptTableSchema 的 elementRow 块)。
Schema::FieldSchema MakeScriptTableField(const PropertyNode& property, const std::string& idPath, const Schema::SchemaRegistry* schemas, const Schema::FieldSchema* declared ){
			Schema::FieldSchema field;
			field.Name = property.Name;
			field.K = Schema::Kind::Object;
			field.Meta.Doc = property.Doc;
			// VEC-C2:类型文案(裸 table → `table`;只读数组/映射 → `array`/`map`;可展开结构 → `struct`)
			// —— 摘要行文本与"没有注解说明"时的行悬停回落共用它(v4 §1)。
			field.Meta.DisplayName = PropertyNodeTypeText(property);
			if (!PropertyNodeExpandable(property))
				return field;
			const size_t nodeIndex = BuildScriptTableSchema(property, idPath, 1, schemas, declared);
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


		// 按名字在注册表里找命名 struct 的 schema(元素行 / `+` 追加共用)。
const Schema::TypeSchema* FindNamedStructSchema(const Schema::SchemaRegistry* schemas, const std::string& typeName){
			return (schemas && !typeName.empty()) ? schemas->Find(typeName) : nullptr;
		}


		// 原生路径:给一个命名 struct 元素(还没有子行)按 schema 补出可编辑子行并记下默认种子。
		// `Value` 保持 monostate:折叠时 `ElementValueOf` 对 Object 行按 `Children` 折成 ValueMap。
void HydratePlainStructElement(PropertyNode& element, const Schema::SchemaRegistry* schemas){
			if (element.Type != Schema::Kind::Object || !element.Children.empty() || element.ReadOnly)
				return;
			const Schema::TypeSchema* elementSchema = FindNamedStructSchema(schemas, element.TypeName);
			if (!elementSchema)
			{
				element.ReadOnly = true;   // schema 拿不到 → 只读摘要(与引擎降级同口径)
				return;
			}
			HydrateStructElementChildren(element, *elementSchema, Schema::Value(Schema::ValueMap {}), 0, schemas);
		}


		// 声明种子同时记成行默认值:`↺` 的语义 = 回到这个种子(原生容器没有别的"默认"可回落)。
void CapturePlainRowDefaults(PropertyNode& row){
			if (row.Type == Schema::Kind::Object)
			{
				for (PropertyNode& child : row.Children)
					CapturePlainRowDefaults(child);
				return;
			}
			if (std::holds_alternative<std::monostate>(row.Default))
				row.Default = row.Value;
		}


		// 命名 struct 元素的子行:形状从元素 schema 取(声明种子 → 默认值),值从实例的
		// ValueMap 覆盖(键缺失 = 保留声明种子,面板不会显示"未设")。
void HydrateStructElementChildren(PropertyNode& row, const Schema::TypeSchema& nested, const Schema::Value& value, int depth, const Schema::SchemaRegistry* schemas){
			AppendCollectionElementFields(row.Children, nested, depth);
			for (PropertyNode& child : row.Children)
				CapturePlainRowDefaults(child);
			const Schema::ValueMap* map = std::get_if<Schema::ValueMap>(&value);
			if (!map)
				return;
			for (PropertyNode& child : row.Children)
			{
				const Schema::ValueMap::const_iterator found = map->find(child.Name);
				if (found == map->end())
					continue;
				if (child.Type == Schema::Kind::Object)
				{
					// 嵌套命名 struct:`AppendCollectionElementFields` 把声明名写进了 `TypeName`
					// (与 schema 字段的 `GetNested()->Id.Name` 同一口径),按它回查子 schema。
					const Schema::TypeSchema* childSchema = (schemas && !child.TypeName.empty())
						? schemas->Find(child.TypeName) : nullptr;
					if (childSchema && depth + 1 <= kPlainContainerMaxDepth)
						HydrateStructElementChildren(child, *childSchema, found->second, depth + 1, schemas);
				}
				else
					child.Value = found->second;
			}
		}


		// 一个容器字段(Array/Map)从实例值水合出行模型。形状 = 实例里的元素(空容器 = 没有行,
		// 面板给"暂无元素 + `+`");元素类型/说明/编辑元数据 = 字段声明 + 元素 schema。
PropertyNode HydratePlainContainer(const Schema::FieldSchema& field, const Schema::Value& value, const Schema::SchemaRegistry* schemas){
			PropertyNode container;
			container.Name = field.Name;
			container.Type = Schema::Kind::Object;
			container.Doc = field.Meta.Doc;
			container.Collection = field.Collection == Schema::CollectionKind::Map
				? PropertyCollection::Map : PropertyCollection::Array;
			container.ElementKind = field.ElementKind;
			container.KeyKind = field.KeyKind;
			if (field.ElementTypeName)
				container.TypeName = field.ElementTypeName;
			const Schema::TypeSchema* elementSchema = field.GetElementNested ? field.GetElementNested() : nullptr;
			if (!elementSchema && schemas && !container.TypeName.empty())
				elementSchema = schemas->Find(container.TypeName);
			if (container.ElementKind == Schema::Kind::Enum)
			{
				const Schema::EnumSchema* enumSchema = field.GetEnum ? field.GetEnum() : nullptr;
				if (enumSchema)
					container.TypeName = enumSchema->Name;
				else
					container.ReadOnly = true;
			}
			else if (container.ElementKind == Schema::Kind::Object && !elementSchema)
			{
				container.ReadOnly = true;   // 元素 schema 拿不到:只读摘要(与引擎声明同一条降级)
			}
			else if (ComponentPropertyModel::IsSummaryKind(container.ElementKind))
			{
				container.ReadOnly = true;   // 面板没有行控件
			}

			// 逐元素建行。元数据(Range/Unit/Step)声明在**容器字段**上,由 BuildScriptTableSchema
			// 的 elementRow 分支透传给元素行 —— 与旧脚本路径逐字段一致。
			const auto makeElement = [&](const Schema::Value& elementValue, const std::string& name, int depth)
			{
				PropertyNode child;
				child.Name = name;
				child.Type = container.ElementKind;
				child.TypeName = container.TypeName;
				child.ReadOnly = container.ReadOnly;
				if (child.Type == Schema::Kind::Object && elementSchema)
					HydrateStructElementChildren(child, *elementSchema, elementValue, depth, schemas);
				else
					child.Value = elementValue;
				CapturePlainRowDefaults(child);
				if (child.Type != Schema::Kind::Object && !PropertyNodeValueMatchesType(child))
					child.Value = DefaultPropertyValue(child.Type);   // 存值类型不符 → 声明零值
				return child;
			};

			if (const Schema::ValueList* items = std::get_if<Schema::ValueList>(&value))
			{
				container.Children.reserve(items->size());
				for (size_t index = 0; index < items->size(); ++index)
					container.Children.push_back(makeElement((*items)[index], std::to_string(index + 1), 0));
			}
			else if (const Schema::ValueMap* items = std::get_if<Schema::ValueMap>(&value))
			{
				container.Children.reserve(items->size());
				for (const auto& [key, elementValue] : *items)
					container.Children.push_back(makeElement(elementValue, key, 0));
			}
			return container;
		}


		
		// 按名字在 schema 类型里找字段(C++ 脚本的字段说明 / 默认值都挂在 schema 上)。
const Schema::FieldSchema* FindScriptSchemaField(const Schema::TypeSchema& schema, const std::string& name){
			for (const Schema::FieldSchema& field : schema.Fields)
				if (field.Name == name)
					return &field;
			return nullptr;
		}




		// ---- VEC-C2:脚本声明签名(编辑态同步的门)----
		//
		// 为什么需要门:引擎的合并**在集合形状上以脚本声明为准**(`ComponentPropertyModel::ApplyInto`:
		// 数组/映射的子行按声明的名字重建),而面板的 `+`/`-`(方案 v3 §4 / v4 §3 的验收)改的是
		// 组件里的 `Children` —— 每帧无条件重合并会把面板刚做的增删还原回去。声明签名不变时跳过
		// 合并 ⇒ 值/形状以组件(场景)为准;脚本一改(内容指纹变 → 签名变)照旧整体重建,
		// 新字段/新说明/新默认值立即生效。
		//
		// 签名含:顺序 / 名字 / 类型 / 集合形态 / 元素与键类型 / 只读标记 / 元素行是否未知 /
		// 说明(改注释也要跟着刷新)/ 默认值(脚本表初值改了,没改过的字段要回新初值)。
void AppendDeclarationSignature(std::string& out, const std::vector<ComponentPropertyModel::Declaration>& declarations, int depth){
			if (depth > 6)
				return;
			for (const ComponentPropertyModel::Declaration& declaration : declarations)
			{
				out += declaration.Name;
				out += '|';
				out += ComponentPropertyModel::KindName(declaration.Type);
				out += '|';
				out += ComponentPropertyModel::CollectionName(declaration.Collection);
				out += '|';
				out += ComponentPropertyModel::KindName(declaration.ElementKind);
				out += '|';
				out += ComponentPropertyModel::KindName(declaration.KeyKind);
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


std::string ScriptDeclarationSignature(const std::vector<ComponentPropertyModel::Declaration>& declarations){
			std::string out;
			AppendDeclarationSignature(out, declarations, 0);
			return out;
		}


		// 面板里诊断/错误只显示第一行并截断;完整文本由 AI 通道 script.status 提供。
std::string TruncateForPanel(const std::string& text, size_t limit ){
			const size_t newline = text.find('\n');
			std::string line = text.substr(0, newline == std::string::npos ? text.size() : newline);
			// 按字节上限截断必须落在**字符边界**上,否则末尾是半截 UTF-8 序列 ⇒ 幽灵码点 ⇒ 空白。
			if (line.size() > limit)
				line = World::Utf8::TrimToBytes(line, limit) + "...";
			return line;
		}


		// ASCII 不分大小写的子串匹配(非 ASCII 字节原样比较:中文没有大小写)。
std::string LowerAscii(std::string text){
			std::transform(text.begin(), text.end(), text.begin(),
				[](unsigned char c) { return static_cast<char>(std::tolower(c)); });
			return text;
		}


bool ContainsInsensitive(const std::string& haystack, const std::string& needleLower){
			if (needleLower.empty())
				return true;
			return LowerAscii(haystack).find(needleLower) != std::string::npos;
		}


		// 分类目录键 = schema.category.<路径,把 '/' 换成 '.'>(与 schema.component.* 同一口径);
		// 英文默认 = Category 原文(默认语言不查表)。
std::string CategoryKeyName(const std::string& category){
			std::string key = category;
			for (char& character : key)
				if (character == '/')
					character = '.';
			return key;
		}


std::string CategoryLabel(const std::string& category){
			if (category.empty())
				return Wui::Tr("panel.properties.add.uncategorized", "Uncategorized");
			return Wui::Tr("schema.category." + CategoryKeyName(category), category);
		}


		// 一句话说明:英文默认 = schema->Doc 原文;中文目录用 schema.component.<短名>.doc 覆盖。
std::string ComponentDocLabel(const Schema::TypeSchema& schema){
			if (schema.Doc.empty())
				return std::string();
			return Wui::Tr("schema.component." + SchemaTypeKeyName(schema) + ".doc", schema.Doc);
		}


		// P4-U9:字段说明(悬浮提示 + 无障碍 Tooltip)。英文默认 = schema 里的 Doc 原文,
		// 中文目录用 schema.field.<短类型名>.<字段名>.doc 覆盖;空 = 该字段没写说明。
std::string FieldDocLabel(const Schema::TypeSchema& schema, const Schema::FieldSchema& field){
			if (field.Meta.Doc.empty())
				return std::string();
			return Wui::Tr(("schema.field." + SchemaTypeKeyName(schema) + "." + field.Name + ".doc").c_str(),
				field.Meta.Doc);
		}


		// 颜色字段的无障碍值文本(#RRGGBBAA,与取色器弹层里的 hex 行同一写法)。
std::string FormatColorHexText(const glm::vec4& color){
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
bool MatchesComponentFilter(const Schema::TypeSchema& schema, const std::string& needleLower){
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

PropertiesPanel::PropertiesPanel(PanelHost& host) : m_Host(host), m_StatePath(std::string(WLD_LOCAL_DIR) + "wui-properties.json"){
		// 选择器 MRU 与其它面板状态同口径(wui-layout.json / wui-browser.json):读失败/文件不
		// 存在都只是"没有最近使用",不影响面板可用性。
		LoadState();
	}


void PropertiesPanel::LoadState(){
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


void PropertiesPanel::SaveState() const{
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


void PropertiesPanel::TouchRecent(const std::string& shortName){
		if (shortName.empty())
			return;
		m_RecentComponents.erase(std::remove(m_RecentComponents.begin(), m_RecentComponents.end(), shortName),
			m_RecentComponents.end());
		m_RecentComponents.insert(m_RecentComponents.begin(), shortName);
		if (m_RecentComponents.size() > kPickerRecentMax)
			m_RecentComponents.resize(kPickerRecentMax);
		SaveState();
	}


void PropertiesPanel::OpenAddComponentPicker(Wui::WuiContext& ctx){
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


void PropertiesPanel::CloseAddComponentPicker(Wui::WuiContext& ctx){
		m_AddOpen = false;
		ctx.ClearModal();
		m_Host.SetPanelModalOwner(std::string());
	}


	// ---- P4-U9:移除组件的确认模态(与"添加组件"同一套面板级模态通道)----
void PropertiesPanel::OpenRemoveComponentConfirm(Wui::WuiContext& ctx, uint32_t componentId, const std::string& displayName){
		m_RemovePendingId = componentId;
		m_RemovePendingName = displayName;
		ctx.SetModal(Wui::HashId("prop.remove.modal"));
		m_Host.SetPanelModalOwner(Id());
		ctx.RecordOp("properties", "remove-component-ask", displayName, std::to_string(componentId));
	}


void PropertiesPanel::CloseRemoveComponentConfirm(Wui::WuiContext& ctx){
		m_RemovePendingId = 0;
		m_RemovePendingName.clear();
		ctx.ClearModal();
		m_Host.SetPanelModalOwner(std::string());
	}


void PropertiesPanel::DrawRemoveComponentConfirm(Wui::WuiContext& ctx, Entity entity){
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
	// C++ 的分两支(CPPT-6-ED-COLLECTIONS):数组/映射的声明形状 = **空容器**(元素初值在脚本成员里,
	// schema 看不到)→ 清 `Children` + 丢掉形状归属;非容器的结构化表形状由 schema 决定 → 递归清值。
	// container 是本帧正在画的那个容器(合成路径下指针在本次绘制内稳定);path = 该容器的完整名字路径
	// (调用方在请求时记下 —— 本函数是**延后**落地的,那时 m_ScriptRowPath 已经变了)。
void PropertiesPanel::ApplyScriptCollectionReset(Wui::WuiContext& ctx, PropertyNode& container, bool luau, const std::vector<std::string>& path){
		if (m_ReadOnly || container.ReadOnly)
			return;
		if (luau)
		{
			const ComponentPropertyModel::Declaration* declaration = m_ScriptDeclarationsValid
				? ResolveDeclarationPath(m_ScriptDeclarations, path) : nullptr;
			if (declaration && !declaration->FieldsUnknown && !declaration->ReadOnly)
			{
				// seed 与目标同形(名字/类型对齐),但子行清空 + 形状归属清零:
				// 合并后 = 声明的默认形状 + 每行的默认值(全"未设" ⇒ 存档整条不写)。
				PropertyNode seed = container;
				seed.Children.clear();
				seed.ShapeFromScene = false;
				std::vector<PropertyNode> next { std::move(seed) };
				ComponentPropertyModel::SyncFromDeclarations(next, { *declaration });
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
			// CPPT-6-ED-COLLECTIONS:C++ 数组/映射"整集合复原"= 回到声明默认形状(空容器)。
			// 清掉面板加的增删 + 丢掉"形状来自场景"的归属:复位后这条容器不再是"场景记录过",
			// 存档跟着不写,Play 时脚本成员初值生效(与 D1"未设不覆盖"同口径);下一帧
			// `ScriptRowModified` 回到未偏离 → 集合头 `↺` 自动消失。
			if (container.Collection == PropertyCollection::Array
				|| container.Collection == PropertyCollection::Map)
			{
				container.Children.clear();
				container.ShapeFromScene = false;
			}
			else
			{
				ResetScriptRowValues(container);
			}
		}

		m_Host.MarkDocumentDirty();
		// prefab 实例里的编辑同样记进覆盖集合(路径口径与 changedFields 一致)。
		RegisterPrefabOverrides(m_ScriptInspectingEntity,
			{ m_ScriptInspectingComponentName + "." + ScriptRowPathText(path) });
		ctx.RecordOp("properties", "script-collection-reset", ScriptRowPathText(path), "applied");
	}


bool PropertiesPanel::ApplyScriptRowDeclaredReset(const std::string& rowName){
		(void)rowName;
		return false;
	}


	// ---- 面板渲染主流程 ----

void PropertiesPanel::OnRender(Wui::WuiContext& ctx, const Wui::WuiRect& rect, PanelHost& host){
		const Wui::WuiTheme& theme = host.Theme();
		// Play/Simulate = 只读查看:字段只显示不写回,并给出提示(用户确认的语义)。
		m_ReadOnly = host.IsReadOnlyMode();
		Entity entity = host.GetSelectedEntity();
		if (!entity.IsValid() || entity.GetScene() != host.GetActiveScene().get())
		{
			// VEC-H4:空态走库件 `Wui::EmptyState`(§规则 21):标题 + 一句下一步提示,垂直居中,
			// 不再是一行贴在左上角的灰字。空态节点由库件登记(kind="empty-state")。
			// CPPT-6(用户 2026-09-28「有这种乱码符号处理下」):装饰标记用 `•`(U+2022)——
			// 旧的 `◌`(U+25CC)在 Inter 主面与 Noto 子集里都没有,stbtt 落到 `.notdef`(框+X)。
			// `•` 在全部 7 个运行时字体里都有(含主面 Inter,不依赖回退)。
			Wui::EmptyState(ctx, rect,
				Wui::Tr("panel.properties.no_entity_glyph", "\u2022"),
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
				const bool defaultOpen = false;
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
			const bool defaultOpen = false;
			bool& open = ctx.Persist<bool>(Wui::HashId(("prop.open." + schema->DisplayName).c_str()), defaultOpen);
			m_Sections[i].Open = open;
			if (!m_RevealSection.empty() && m_Sections[i].Title == m_RevealSection)
				revealTargetY = sectionTop;
			contentHeight += kSectionHeader + (open ? m_Sections[i].ContentHeight : 0.0f) + kSectionGap;
			sectionTop += kSectionHeader + (open ? m_Sections[i].ContentHeight : 0.0f) + kSectionGap;
		}

		// CPPT7R-T1(C3):末行留白 + 越界补偿。
		//
		// 无障碍的可见性判据是"节点**中心点**必须落在客户区内"(`WuiAccessibility::Register`),
		// 而滚动范围原来正好把内容底边贴到面板底边 —— 滚到底时最后一行的中心只剩半行余量;
		// 一旦面板底边落在客户区底边之下(停靠布局/窗口高度变化),末行就会被判 `visible=false`、
		// `ui.invoke` 点不到(鼠标点它露在上半部分的那半截仍然可用)。
		// 口径:滚动范围末尾多留**一行高**;面板底边越过客户区多少就再补多少 —— 保证"滚到底"
		// 一定能把末行整个抬进客户区(中心 + 半行都在区内)。
		if (contentHeight > viewportHeight)
		{
			const float viewportBottom = rect.Y + contentTop + viewportHeight;
			const float panelOverflow = std::max(0.0f, viewportBottom - ctx.ViewportSize().y);
			contentHeight += kRowHeight + panelOverflow;
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
				const bool defaultOpen = false;
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

}
