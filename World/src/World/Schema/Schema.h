#pragma once

// WorldEngine Schema 2.0 — 单一事实源反射契约。
// 纯数据合同:不依赖 entt、UI 框架、Scene 类型;访问器由 schema-compiler 生成。
// 本头文件禁止任何静态初始化期注册与跨 DLL 单例。

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

namespace World::Schema
{
	constexpr uint32_t WE_SCHEMA_ABI_VERSION = 1;

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

	// 类型化边界值。热路径直接用字段成员,不经过 Value。
	using Value = std::variant<
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
		std::string>;

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

	// 原生脚本绑定:桥接文件把 void* 转换回 NativeScriptComponent&。
	struct ScriptBinding
	{
		void (*Bind)(void* nativeScript) = nullptr;
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
	};

	// 资产字段操作:以路径字符串作为边界值。每个资产类型提供一个特化。
	template <typename AssetRef>
	struct AssetOps;
}

// ---- 注解宏 ----
// WE_FIELD 等为生成器扫描的声明性标记,展开为空或仅为友元声明;语法受限:
//   WE_FIELD(字段名, Kind[, Attr(...)])  Attr ∈ Group/DisplayName/Range/ReadOnly/Transient/Id/Default/Of
//   Kind==Enum/Object 需要 Of(类型名);Kind==Asset 需要 Of("资产类型名")。
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
