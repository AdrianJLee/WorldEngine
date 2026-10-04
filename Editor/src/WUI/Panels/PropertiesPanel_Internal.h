#include "wldpch.h"
#include "WUI/Panels/PropertiesPanel.h"
#include "WUI/Common/EditorAssetCatalog.h"

// P2 W5b:Reload 按钮要复用 EditorLayer 的热重载入口(与帧边界轮询、AI 通道 script.reload
// 同一条语义)。PanelHost 是跨任务冻结的窄接口,本包文件边界内不能扩展它,因此只 include。
#include "App/EditorLayer.h"

#include "World/Core/KeyCodes.h"
#include "World/Asset/ScriptArtifact.h"
#include "World/Asset/ProjectManifest.h"
#include "World/Gameplay/Prefab/Prefab.h"
// 2026-09-26 脚本组件重写:属性表(`PropertyNode`)的唯一维护点(注解/schema 声明 → 属性表)。
#include "World/Script/Runtime/ComponentPropertyModel.h"
// 2026-09-26 SCRIPT-V6:声明的权威解析在引擎侧(名字/类型/Doc/**脚本里的默认值**)——
// 编辑器只调 `ScriptEngine::SyncScriptDeclarations`,不再自己扫注解。
#include "World/Script/Runtime/ScriptEngine.h"
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
namespace PropertiesPanelDetail {}   // 前置声明:下面的 using 必须先见到这个名字
using namespace PropertiesPanelDetail;   // 等价于拆分前的文件内匿名命名空间可见性
	namespace PropertiesPanelDetail
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
int VecFieldLayout(float controlWidth);

int VecFieldRows(int layout, int components);

float VecFieldHeight(int layout, int components);

void RegisterNode(Wui::WuiId id, const char* kind, const Wui::WuiRect& rect, const std::string& label, const std::string& value, bool enabled = true, const std::string& tooltip = std::string(), bool focused = false);

std::string FormatFloatText(float value, int decimals = 3);

int StepDecimals(bool hasStep, float step);

void DrawScriptIntControl(Wui::WuiContext& ctx, Wui::WuiId id, const Wui::WuiRect& rect, int64_t& raw, int64_t lo, int64_t hi, const Schema::FieldMetadata& meta, const Wui::WuiTheme& theme);

const std::filesystem::path& CachedContentRoot();

bool PrefabSourceExists(const std::string& path);

std::string FormatValueByVariant(const Schema::Value& value);

std::string EnumNameOf(const Schema::EnumSchema& schema, int64_t raw);

int64_t EnumRawOf(const Schema::EnumSchema& schema, const Schema::Value& value);


		// 定义在下方(CPPT-3 的只读摘要与行悬停共用);先声明以便 FormatReadOnlyValue 引用。
		std::string ScriptLeafTypeText(Schema::Kind kind);
std::string FormatReadOnlyValue(const Schema::FieldSchema& field, const Schema::Value& value);

std::string PropPath(const std::string& typeName, const std::string& fieldName);

std::string SchemaTypeKeyName(const Schema::TypeSchema& schema);

std::string HumanizeIdentifier(const std::string& name);

std::string TermText(const Wui::LocalizedLabel& label);

Wui::LocalizedLabel SchemaComponentLabel(const Schema::TypeSchema& schema);

Wui::LocalizedLabel SchemaFieldLabel(const Schema::FieldSchema& field);

Wui::LocalizedLabel SchemaFieldLabel(const Schema::TypeSchema& schema, const std::string& fieldName);

std::string CameraProjectionLabel(const std::string& enumName);

Wui::WuiRect ComponentRect(const Wui::WuiRect& rect, int index, float height);

float DrawVec3Row(Wui::WuiContext& ctx, const std::string& baseId, float x, float y, float width, const Wui::LocalizedLabel& label, glm::vec3& value, const Wui::WuiTheme& theme, bool& changed);

bool DrawFloatRow(Wui::WuiContext& ctx, const std::string& idText, const Wui::WuiRect& row, const Wui::LocalizedLabel& label, float& value, float lo, float hi, const Wui::WuiTheme& theme, bool reachable);

void DrawReadOnlyRow(Wui::WuiContext& ctx, const std::string& idText, const Wui::WuiRect& row, const Wui::LocalizedLabel& label, const std::string& value, const Wui::WuiTheme& theme);

Schema::Value DefaultPropertyValue(Schema::Kind kind);

bool PropertyNodeValueMatchesType(const PropertyNode& property);

Schema::Value PropertyNodeDisplayValue(const PropertyNode& property);

Schema::Value PropertyNodeDisplayValueForRow(const PropertyNode& property);

std::string ScriptLeafTypeText(Schema::Kind kind);

std::string PropertyNodeTypeText(const PropertyNode& property);

bool PropertyNodeExpandable(const PropertyNode& property);

std::string ScriptFieldTypeText(const Schema::FieldSchema& field);


		// ---- VEC-B3:嵌套 `---@class`(Object)属性的合成 schema ----
		//
		// `DrawSchemaFields` 的 Object 行只认 `Schema::FieldSchema` 里的**无捕获函数指针**
		// (`Get/Set/GetPtr/GetNested`),所以:
		//   * 子字段 i 的 Get/Set 以"父 PropertyNode*"为 instance,读写 `Children[i].Value`;
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
			std::array<PropertyCollection, kScriptTableMaxNodes> Collections {};
			// VEC-H6:每个节点描述的 PropertyNode(它的 `Children` 与节点 `Fields` 同序)。
			// 复位可见性判定要拿 PropertyNode 的 Value/Default/Children,而 FieldSchema 只有访问器。
			std::array<const PropertyNode*, kScriptTableMaxNodes> Owners {};
			size_t Used = 0;
		};
ScriptTableSchemaArena& ScriptTableArena();


		template <size_t Index>
		Schema::Value ScriptTableChildGet(const void* instance)
		{
			const auto* parent = static_cast<const PropertyNode*>(instance);
			if (!parent || Index >= parent->Children.size())
				return Schema::Value();   // 合成与绘制同帧同源,正常不可达;防越界 UB
			return PropertyNodeDisplayValueForRow(parent->Children[Index]);
		}

		template <size_t Index>
		void ScriptTableChildSet(void* instance, const Schema::Value& edited)
		{
			auto* parent = static_cast<PropertyNode*>(instance);
			if (!parent || Index >= parent->Children.size())
				return;
			PropertyNode& child = parent->Children[Index];
			// PURE-ECS:行内 `↺` 写回的是 monostate(= "清成未设")。原生组件(纯 ECS)的容器元素
			// 没有"脚本成员初值"可回落,`↺` 的语义就是**回到这一行的声明默认**(`+` 追加时的同一种子,
			// 见 `AppendCollectionElementFields`/`MakeCollectionElement`),所以把 monostate 落成
			// `Default` 而不是留在行上 —— 折回 Value 时不会出现"未设"元素。
			child.Value = std::holds_alternative<std::monostate>(edited) ? child.Default : edited;
		}

		template <size_t Index>
		void* ScriptTableChildPtr(void* instance)
		{
			auto* parent = static_cast<PropertyNode*>(instance);
			return (parent && Index < parent->Children.size())
				? static_cast<void*>(&parent->Children[Index]) : nullptr;
		}

		template <size_t Index>
		const void* ScriptTableChildPtrConst(const void* instance)
		{
			const auto* parent = static_cast<const PropertyNode*>(instance);
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
ScriptEnumArena& ScriptEnumArenaStore();


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
void ResetScriptRowArenas();


		using ScriptEnumGetter = const Schema::EnumSchema* (*)();
ScriptEnumGetter ScriptEnumAccessorFor(const Schema::SchemaRegistry& schemas, const std::string& enumName);

PropertyCollection ScriptTableCollectionOf(const Schema::TypeSchema* node);

const PropertyNode* ScriptTableNodeOwner(const Schema::TypeSchema* node);

const PropertyNode* ScriptRowModel(const Schema::TypeSchema& node, const void* instance, size_t fieldIndex, bool scriptRows);

bool ScriptLeafRowModified(const PropertyNode& row);

const ComponentPropertyModel::Declaration* FindDeclarationField( const ComponentPropertyModel::Declaration& parent, const std::string& name);

bool ScriptRowModified(const PropertyNode& row, const ComponentPropertyModel::Declaration* declaration);

Schema::Value CollectionElementLeafSeed(const Schema::FieldSchema& field);

void AppendCollectionElementFields(std::vector<PropertyNode>& out, const Schema::TypeSchema& type, int depth);


		// ---- VEC-C2:数组/映射的元素增删(面板侧只改 `Children`)----
		//
		// 新增元素的值 = 该类型的规范值(`+` 是"造一行",不是"设一个值");元素本身是嵌套集合
		// (如 `{{number}}`)时模板取已有同类元素的形态(Collection/ElementKind/KeyKind 在子项上)。
		// 空容器没有模型行可抄 → 按声明/节点形状建模板(见上,CPPT-6-ED-COLLECTIONS);
		// `schemas` 只有 C++ 脚本合成路径传得进来(Luau 的元素来自注解声明,保持既有回落)。
		// 前向声明:水合辅助定义在合成 schema 构建器之后(`+` 追加 struct 元素时要用)。
		void HydratePlainStructElement(PropertyNode& element, const Schema::SchemaRegistry* schemas);
		void HydrateStructElementChildren(PropertyNode& row, const Schema::TypeSchema& nested,
			const Schema::Value& value, int depth, const Schema::SchemaRegistry* schemas);
PropertyNode MakeCollectionElement(const PropertyNode& container, const Schema::SchemaRegistry* schemas, bool plainRows);

bool CollectionKeyTaken(const PropertyNode& container, const std::string& key);

void RenumberArrayChildren(PropertyNode& container);

PropertyNode* ResolveScriptRowPath(std::vector<PropertyNode>& properties, const std::vector<std::string>& path, size_t index = 0);

const ComponentPropertyModel::Declaration* ResolveDeclarationPath( const std::vector<ComponentPropertyModel::Declaration>& declarations, const std::vector<std::string>& path, size_t index = 0);

void ResetScriptRowValues(PropertyNode& row);

std::string ScriptRowPathText(const std::vector<std::string>& path);

std::string ScriptCollectionHeadResetLabel(PropertyCollection collection);

const char* ScriptCollectionHeadResetDoc();

const char* ScriptItemResetDoc();

void* ScriptTableIdentityPtr(void* instance);

const void* ScriptTableIdentityPtrConst(const void* instance);

size_t BuildScriptTableSchema(const PropertyNode& property, const std::string& idPath, size_t depth, const Schema::SchemaRegistry* schemas, const Schema::FieldSchema* declared = nullptr);

Schema::FieldSchema MakeScriptTableField(const PropertyNode& property, const std::string& idPath, const Schema::SchemaRegistry* schemas, const Schema::FieldSchema* declared = nullptr);


// ---- PURE-ECS:原生组件容器字段的"水合" ----
		//
		// 纯 ECS 组件是纯数据,没有挂在组件里的属性模型(旧的 CppScriptComponent 有一个
		// `std::vector<PropertyNode> Properties` 成员,面板改的是它)。原生容器因此每帧从
		// **实例值**水合出行模型:形状/类型名/说明来自字段声明,值来自实例 ⇒ 面板显示的永远
		// 等于结构体里的值;回写走 `ComponentPropertyModel::FoldContainer`(折成生成访问器期望的
		// `ValueList`/`ValueMap`,含命名 struct 元素的"字段名 → Value"形态)。
		constexpr int kPlainContainerMaxDepth = 4;
const Schema::TypeSchema* FindNamedStructSchema(const Schema::SchemaRegistry* schemas, const std::string& typeName);

void HydratePlainStructElement(PropertyNode& element, const Schema::SchemaRegistry* schemas);

void CapturePlainRowDefaults(PropertyNode& row);

void HydrateStructElementChildren(PropertyNode& row, const Schema::TypeSchema& nested, const Schema::Value& value, int depth, const Schema::SchemaRegistry* schemas);

PropertyNode HydratePlainContainer(const Schema::FieldSchema& field, const Schema::Value& value, const Schema::SchemaRegistry* schemas);

const Schema::FieldSchema* FindScriptSchemaField(const Schema::TypeSchema& schema, const std::string& name);

void AppendDeclarationSignature(std::string& out, const std::vector<ComponentPropertyModel::Declaration>& declarations, int depth);

std::string ScriptDeclarationSignature(const std::vector<ComponentPropertyModel::Declaration>& declarations);

std::string TruncateForPanel(const std::string& text, size_t limit = 72);


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
std::string LowerAscii(std::string text);

bool ContainsInsensitive(const std::string& haystack, const std::string& needleLower);

std::string CategoryKeyName(const std::string& category);

std::string CategoryLabel(const std::string& category);

std::string ComponentDocLabel(const Schema::TypeSchema& schema);

std::string FieldDocLabel(const Schema::TypeSchema& schema, const Schema::FieldSchema& field);

std::string FormatColorHexText(const glm::vec4& color);

bool MatchesComponentFilter(const Schema::TypeSchema& schema, const std::string& needleLower);

	}
}
