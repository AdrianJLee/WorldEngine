#pragma once

// WorldEngine Schema 2.0 — 单一事实源反射契约。
// 纯数据合同:不依赖 entt、UI 框架、Scene 类型;访问器由 schema-compiler 生成。
// 本头文件禁止任何静态初始化期注册与跨 DLL 单例。

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

namespace World
{
	class ScriptableEntity;   // ScriptBinding 的工厂签名只需要前置声明
}

namespace World::Schema
{
	// 3 = CPPT-6(2026-09-28):Schema::Value 追加容器替代项(数组/映射,递归承载嵌套值)、
	//     FieldSchema 尾部追加容器形状描述 ⇒ 反射结构布局变化,旧 Game.dll 必须被宿主按
	//     **等值**模块 ABI 拒绝(见 Modules/WeModule.h 的升版说明)。
	// 2 = CPPT-2(2026-09-27):FieldMetadata 尾部追加 Unit/Step(编辑期提示)。
	constexpr uint32_t WE_SCHEMA_ABI_VERSION = 3;

	// ---- 稳定身份 ----
	constexpr uint64_t Fnv1a64(const char* text)
	{
		uint64_t hash = 14695981039346656037ull;
		while (*text)
		{
			hash ^= static_cast<unsigned char>(*text++);
			hash *= 1099511628211ull;
		}
		return hash;
	}

	inline uint64_t Fnv1a64(const std::string& text)
	{
		uint64_t hash = 14695981039346656037ull;
		for (unsigned char c : text)
		{
			hash ^= c;
			hash *= 1099511628211ull;
		}
		return hash;
	}

	struct ModuleId
	{
		std::string Name;
		uint32_t Version = 0;
	};

	struct TypeId
	{
		std::string Name;
		uint64_t Hash = 0;

		TypeId() = default;
		explicit TypeId(std::string name) : Name(std::move(name)), Hash(Fnv1a64(Name)) {}

		bool operator==(const TypeId& other) const { return Hash == other.Hash && Name == other.Name; }
	};

	struct FieldId
	{
		uint64_t Value = 0;
		bool operator==(const FieldId& other) const { return Value == other.Value; }
	};

	enum class Kind : uint8_t
	{
		None = 0,
		Bool,
		Int8, Int16, Int32, Int64,
		UInt8, UInt16, UInt32, UInt64,
		Float, Double,
		Vec2, Vec3, Vec4,
		IVec2, IVec3, IVec4,
		UVec2, UVec3, UVec4,
		Quat, Mat3, Mat4,
		String,
		Enum,
		Asset,
		Object,
	};

	enum class TypeCategory : uint8_t
	{
		Struct,
		Component,
		Script,
		Enum,
	};

	// 容器**形状**(CPPT-6):容器不是新的 Kind,而是字段/属性的形状修饰 —— 与脚本属性层的
	// `ScriptPropertyCollection`(Components.h)同构,`ScriptProperties::SyncFromSchema` 按 1:1 映射。
	//   Array = std::vector<ElementKind>(数组;行 = 元素,Name = 下标字符串 1..n);
	//   Map   = std::map<std::string, ElementKind>(映射;键固定字符串,行名 = 键)。
	// 元素/键类型里只有"命名 struct"需要名字 + 嵌套 schema(见 FieldSchema 的
	// ElementTypeName / GetElementNested);更深的匿名嵌套用"命名 struct 再套容器"表达。
	enum class CollectionKind : uint8_t
	{
		None = 0,
		Array,
		Map,
	};

	// 形状的可读名(None/Array/Map):诊断、序列化与脚本属性层共用。
	// 定义在 SchemaRegistry.cpp(本头文件不引 WLD 导出宏,保持纯数据合同)。
	const char* CollectionKindName(CollectionKind collection);

	// 类型化边界值。热路径直接用字段成员,不经过 Value。
	//
	// CPPT-6:容器值以**递归替代项**加入(数组 = std::vector<Value>;映射 = std::map<std::string,Value>)。
	// 命名 struct 的值也用同形 map 承载(键 = 字段名,未设字段 = monostate),所以
	// `Array, Of(MyStruct)` / `Map, Of(MyStruct)` 的元素就是一层 map —— 不需要新的 Kind。
	//
	// 别名无法自引用(`using Value = std::variant<…, std::vector<Value>>` 里的 Value 不在自身
	// 作用域内),所以 Value 是**继承 variant 的结构体**;variant 的构造/赋值与全部访问器
	// (std::get / std::get_if / std::holds_alternative / std::visit / .index())对派生类照常可用
	// (MSVC 14.51 / C++17 实测,见 CPPT-6 报告)。
	struct Value;
	using ValueList = std::vector<Value>;
	using ValueMap = std::map<std::string, Value>;
	using ValueAlternatives = std::variant<
		std::monostate,
		bool,
		int8_t, int16_t, int32_t, int64_t,
		uint8_t, uint16_t, uint32_t, uint64_t,
		float, double,
		glm::vec2, glm::vec3, glm::vec4,
		glm::ivec2, glm::ivec3, glm::ivec4,
		glm::uvec2, glm::uvec3, glm::uvec4,
		glm::quat,
		glm::mat3, glm::mat4,
		std::string,
		ValueList, ValueMap>;

	struct Value : ValueAlternatives
	{
		using Base = ValueAlternatives;
		using Base::Base;
		using Base::operator=;
		Value() = default;
	};

	// ---- 容器装箱/拆箱(生成代码用;T = 元素的 C++ 类型,Pack/Unpack = 元素 <-> Value)----
	// 未设(monostate)/形状不符 → 不写目标,返回 false(调用方保持实例原值)。
	template <typename T, typename Pack>
	Value PackSequence(const std::vector<T>& items, Pack&& pack)
	{
		ValueList out;
		out.reserve(items.size());
		for (const T& item : items)
			out.push_back(pack(item));
		return Value(std::move(out));
	}

	template <typename T, typename Pack>
	Value PackMap(const std::map<std::string, T>& items, Pack&& pack)
	{
		ValueMap out;
		for (const auto& [key, item] : items)
			out.emplace(key, pack(item));
		return Value(std::move(out));
	}

	template <typename T, typename Unpack>
	bool UnpackSequence(const Value& value, std::vector<T>* out, Unpack&& unpack)
	{
		if (!out)
			return false;
		const ValueList* items = std::get_if<ValueList>(&value);
		if (!items)
			return false;   // 未设 / 形状不符:保留目标原值
		out->clear();
		out->reserve(items->size());
		for (const Value& item : *items)
			out->push_back(unpack(item));
		return true;
	}

	template <typename T, typename Unpack>
	bool UnpackMap(const Value& value, std::map<std::string, T>* out, Unpack&& unpack)
	{
		if (!out)
			return false;
		const ValueMap* items = std::get_if<ValueMap>(&value);
		if (!items)
			return false;
		out->clear();
		for (const auto& [key, item] : *items)
			out->emplace(key, unpack(item));
		return true;
	}

	struct EnumSchema
	{
		std::string Name;
		bool IsSigned = true;
		uint8_t UnderlyingSize = 4;
		std::vector<std::pair<std::string, int64_t>> Values;

		const char* FindName(int64_t value) const
		{
			for (const auto& [name, v] : Values)
				if (v == value)
					return name.c_str();
			return nullptr;
		}
	};

	struct FieldMetadata
	{
		std::string DisplayName; // 空则回退字段名
		std::string Group;
		std::optional<float> Min;
		std::optional<float> Max;
		bool ReadOnly = false;
		bool Transient = false;
		// ---- P4-U9:编辑期字段语义(全部是"怎么编"的提示,不参与存储/ABI) ----
		// 一句话说明(英文 canonical,空 = 无说明):属性面板的行悬停提示 + 无障碍 Tooltip。
		std::string Doc;
		// true = 这个 Vec3/Vec4 是颜色 → 用取色器(色块 + hex + 预设)而不是四个数字框。
		bool Color = false;
		// 非空 = 这个字符串字段是"某类资产的路径"(值 = 资产类型名,如 Material/Model/Texture/
		// Script)→ 用可搜索的资产下拉(带"(无)")而不是裸文本框。
		std::string AssetType;
		// 非空 = 这个字符串字段是从固定集合里选(如 Primitive = cube/plane/sphere)→ 用下拉。
		std::vector<std::string> Choices;
		// ---- CPPT-2(2026-09-27):计量单位与拖拽步长(编辑期提示,不参与存储/ABI 比较) ----
		// Unit = 行后缀(如 "m" / "deg" / "%";空 = 无单位,面板不画后缀);
		// Step = 数值拖拽步长(空 = 面板默认)。两者只由 schema-compiler 从
		// WE_FIELD(..., Unit("m"), Step(0.1)) 带进生成物,序列化与属性表都不写它们。
		std::string Unit;
		std::optional<float> Step;
	};

	struct TypeSchema;

	// 组件存储绑定(entt 桥接)。Schema 核心不依赖 entt/Scene 类型,
	// 桥接文件把具体签名转换为不透明指针;注册表只需读取 ComponentId。
	struct StorageBinding
	{
		uint32_t ComponentId = 0;
		void (*Add)(void* entity) = nullptr;
		void (*Copy)(void* dstEntity, void* srcEntity) = nullptr;
		void (*CopyAll)(void* dstRegistry, void* srcRegistry, const void* entityMap) = nullptr;
	};

	// 脚本工厂绑定(2026-09-26 重写):schema 里 Category==Script 的类型 = 一个工厂,
	// 由桥接文件生成(见 ComponentSchemaBridge.h 的 MakeScriptBinding<T>())。
	//
	// 旧口径是 `Bind(void* nativeScript)`,把函数指针写进 CppScriptComponent —— 那让"组件数据"
	// 与"只能由 C++ 现场填的绑定"混在一起。现在组件只存 ScriptName,实例化时按名字查这里的工厂。
	struct ScriptBinding
	{
		ScriptableEntity* (*Create)() = nullptr;
		void (*Destroy)(ScriptableEntity*) = nullptr;
	};

	// schema-compiler 在模块生成 TU 中显式特化这两个模板;头文件只通过
	// WE_SCHEMA_BODY 声明友元,不包含任何生成代码,也不做静态初始化期注册。
	template <typename T>
	struct GeneratedAccess;
	template <typename T>
	struct GeneratedEnum;

	struct FieldSchema
	{
		FieldId Id;
		std::string Name;
		Kind K = Kind::None;
		Value (*Get)(const void*) = nullptr;          // 叶类型(标量/数学/字符串/枚举/资产路径)
		void (*Set)(void*, const Value&) = nullptr;
		void* (*GetPtr)(void*) = nullptr;             // Kind==Object
		const void* (*GetPtrConst)(const void*) = nullptr;
		const TypeSchema* (*GetNested)() = nullptr;   // Kind==Object
		const EnumSchema* (*GetEnum)() = nullptr;     // Kind==Enum
		const char* AssetTypeName = nullptr;          // Kind==Asset
		FieldMetadata Meta;
		Value Default;

		// ---- CPPT-6:容器形状(全部追加在尾部:既有按位置初始化的生成物/测试不受影响)----
		// Collection != None 时:K == Kind::Object(与脚本属性模型一致:容器属性的 Type 是 Object,
		// 元素/键类型另存),Get/Set 读写整个容器(Value = ValueList / ValueMap);
		// GetPtr/GetPtrConst/GetNested 未使用。
		//   ElementKind     = 元素/值类型(叶 kind,或 Object = 命名 struct;Enum/Asset 走下面的槽);
		//   KeyKind         = 映射键类型(仅 Map;当前恒为 String —— 键固定 std::string);
		//   ElementTypeName = ElementKind==Object 时的命名 struct 全名(Enum 时 = 枚举名);
		//   GetElementNested= ElementKind==Object 时的嵌套 TypeSchema(容器元素的递归读写用);
		//   GetEnum         = ElementKind==Enum 时的枚举 schema(有符号/无符号判定);
		//   AssetTypeName   = ElementKind==Asset 时的资产类型名。
		CollectionKind Collection = CollectionKind::None;
		Kind ElementKind = Kind::None;
		Kind KeyKind = Kind::String;
		const char* ElementTypeName = nullptr;
		const TypeSchema* (*GetElementNested)() = nullptr;
	};

	struct TypeSchema
	{
		TypeId Id;
		std::string DisplayName;
		uint32_t AbiVersion = WE_SCHEMA_ABI_VERSION;
		size_t Size = 0;
		TypeCategory Category = TypeCategory::Struct;
		std::vector<FieldSchema> Fields;
		const StorageBinding* Storage = nullptr; // Category==Component
		const ScriptBinding* Script = nullptr;   // Category==Script

		// ---- 类型级描述元数据(编辑期/UI 用:组件选择器分组、说明文案、搜索) ----
		// 来源 = 声明处的 WE_SCHEMA_META(Category(...), Doc(...)) 注解,由 schema-compiler 带进生成物。
		// 不进序列化、不参与 ABI/存储比较,运行时逻辑不读。
		// 命名说明:本结构已有一个 Category(TypeCategory 分类枚举),C++ 不允许同名成员,
		// 所以"分层分类路径"用 CategoryPath(方案里要求的 Category 字符串即此字段);空 = 未分类/无说明。
		// 位置刻意放在末尾:既有按位置初始化的 TypeSchema 聚合初始化(测试与旧生成物)不受影响。
		std::string CategoryPath;
		std::string Doc;
		// true = 核心组件:编辑器不提供"移除组件"(缺了它层级/存档/渲染就不成立)。
		// 同样只服务编辑期 UI,不进序列化、不参与 ABI/存储比较。位置固定在最后(生成物按位置初始化)。
		bool Core = false;
	};

	// 资产字段操作:以路径字符串作为边界值。每个资产类型提供一个特化。
	template <typename AssetRef>
	struct AssetOps;

	// ---- CPPT-6:通用字段/结构读写(叶 / 枚举 / 资产 / 命名 struct / 容器,递归同构)----
	// SchemaWriter/SchemaReader 与 schema-compiler 生成的容器访问器共用这一份实现,
	// 避免"每个形状一段 IO"。约定:
	//   * ReadSchemaField  : 字段 → Value(容器 = ValueList/ValueMap;命名 struct = 字段名 → Value 的 ValueMap);
	//   * WriteSchemaField : Value → 字段。monostate(未设)、容器形状不符、缺访问器一律**不写**并返回 false,
	//                        调用方因此可以"未设 = 保留实例成员初值";
	//   * 读写都不触碰 Transient 字段(与 SchemaWriter 既有口径一致)。
	// 定义在 Schema/SchemaAccess.cpp。
	Value ReadSchemaField(const FieldSchema& field, const void* instance);
	Value ReadStructValue(const TypeSchema& type, const void* instance);
	bool WriteSchemaField(const FieldSchema& field, void* instance, const Value& value);
	bool WriteStructValue(const TypeSchema& type, void* instance, const Value& value);
}

// ---- 注解宏 ----
// WE_FIELD 等为生成器扫描的声明性标记,展开为空或仅为友元声明;语法受限:
//   WE_FIELD(字段名, Kind[, Attr(...)])  Attr ∈ Group/DisplayName/Range/ReadOnly/Transient/Id/Default/Of
//   Kind==Enum/Object 需要 Of(类型名);Kind==Asset 需要 Of("资产类型名")。
//   CPPT-6 容器(形状,不是 Kind):
//     WE_FIELD(Scores, Array, Of(Float))        → std::vector<float>
//     WE_FIELD(Costs,  Map,   Of(Float))        → std::map<std::string, float>
//     WE_FIELD(Stats,  Array, Of(MyStruct))     → std::vector<MyStruct>(元素类型 = 已注册的命名 struct)
//     WE_FIELD(Lookup, Map,   Of(MyStruct))     → std::map<std::string, MyStruct>
//   元素类型 = 叶子 kind(含 Of(EnumName) / Of("AssetType"))或已注册的命名 struct;
//   更深的匿名嵌套用"命名 struct 再套容器"表达(Of(Array(...)) 会被编译器拒绝)。
// 访问器定义在模块的 <Module>SchemaRegistration.cpp(GeneratedAccess/GeneratedEnum 特化),
// WE_SCHEMA_BODY 只把 GeneratedAccess 声明为友元,授予私有成员访问权。

#define WE_SCHEMA_BODY(Module, TypeName, Category) \
	template <class> friend struct World::Schema::GeneratedAccess

#define WE_SCHEMA_END
#define WE_FIELD(...)
// 类型级描述元数据注解,写在 WE_SCHEMA_BODY 之后、第一个 WE_FIELD 之前;展开为空,仅供生成器扫描:
//   WE_SCHEMA_META(Category("Rendering/Light"), Doc("Point light with distance falloff."))
// Category = 分层分类路径(英文 canonical,空 = 未分类);Doc = 一句话说明(英文 canonical,空 = 无说明)。
// 两个属性都可以省略;属性名固定为 Category / Doc。
#define WE_SCHEMA_META(...)
#define WE_ENUM_SCHEMA(Module, TypeName, Underlying)
#define WE_ENUM_VALUE(...)
#define WE_ENUM_END
