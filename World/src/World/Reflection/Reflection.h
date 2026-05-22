#pragma once
#include <any>
#include <glm/glm.hpp>
#include "World/Core/ComponentRegistry.h"
namespace World
{
	// 数据类型枚举
	enum class DataType : uint8_t
	{
		None = 0,

		// ==========================================
		// 1. 基础标量类型 (按内存大小升序，1 -> 2 -> 4 -> 8 字节)
		// ==========================================
		Bool,       // 1 字节
		Char,       // 1 字节
		Int8,       // 1 字节 (新增)
		UInt8,      // 1 字节 (新增)
		Int16,      // 2 字节 (新增)
		UInt16,     // 2 字节 (新增)
		Int32,      // 4 字节
		UInt32,     // 4 字节
		Int64,      // 8 字节 (新增，配套常见 ID 存储)
		UInt64,     // 8 字节
		Float,      // 4 字节
		Double,     // 8 字节 (新增，大世界坐标/大物理系统常用)

		// ==========================================
		// 2. 数学向量类型 (分量数量升序，2 -> 3 -> 4)
		// ==========================================
		Vec2,       // 8 字节 (2 * float)
		Vec3,       // 12字节 (3 * float)
		Vec4,       // 16字节 (4 * float)

		IVec2,      // 8 字节 (2 * int32，新增：UI坐标/网格索引)
		IVec3,      // 12字节 (3 * int32，新增：3D网格索引)
		IVec4,      // 16字节 (4 * int32，新增：骨骼动画影响的 Bone IDs)

		UVec2,      // 8 字节 (2 * uint32，新增)
		UVec3,      // 12字节 (3 * uint32，新增)
		UVec4,      // 16字节 (4 * uint32，新增)

		// ==========================================
		// 3. 矩阵类型 (游戏引擎中 Mat3/Mat4 最常用)
		// ==========================================
		Mat3,       // 36字节 (3x3 float，新增：通常用于传递法线矩阵)
		Mat4,       // 64字节 (4x4 float，变换矩阵)

		// ==========================================
		// 4. 高级/复合/引用类型 (属于不确定大小或堆分配类型)
		// ==========================================
		String,     // 字符串 (内部一般对应 std::string)
		Binary,     // 二进制大对象 (新增，对应 std::vector<uint8_t> 或 裸数据指针，用于自定义序列化)
		Enum,       // 枚举类型 (新增，底层通常是 uint32_t，但反射时需要特殊处理)
		Object,     // 嵌套子对象/组件 (新增，用于支持“类中类”的深层反射)

	};

	template<typename T>
	inline static DataType GetDataType()
	{
		if constexpr (std::is_same_v<T, bool>) return DataType::Bool;
		else if constexpr (std::is_same_v<T, char>) return DataType::Char;
		else if constexpr (std::is_same_v<T, int8_t>) return DataType::Int8;
		else if constexpr (std::is_same_v<T, uint8_t>) return DataType::UInt8;
		else if constexpr (std::is_same_v<T, int16_t>) return DataType::Int16;
		else if constexpr (std::is_same_v<T, uint16_t>) return DataType::UInt16;
		else if constexpr (std::is_same_v<T, int32_t>) return DataType::Int32;
		else if constexpr (std::is_same_v<T, uint32_t>) return DataType::UInt32;
		else if constexpr (std::is_same_v<T, int64_t>) return DataType::Int64;
		else if constexpr (std::is_same_v<T, uint64_t>) return DataType::UInt64;
		else if constexpr (std::is_same_v<T, float>) return DataType::Float;
		else if constexpr (std::is_same_v<T, double>) return DataType::Double;
		else if constexpr (std::is_same_v<T, glm::vec2>) return DataType::Vec2;
		else if constexpr (std::is_same_v<T, glm::vec3>) return DataType::Vec3;
		else if constexpr (std::is_same_v<T, glm::vec4>) return DataType::Vec4;
		else if constexpr (std::is_same_v<T, glm::ivec2>) return DataType::IVec2;
		else if constexpr (std::is_same_v<T, glm::ivec3>) return DataType::IVec3;
		else if constexpr (std::is_same_v<T, glm::ivec4>) return DataType::IVec4;
		else if constexpr (std::is_same_v<T, glm::uvec2>) return DataType::UVec2;
		else if constexpr (std::is_same_v<T, glm::uvec3>) return DataType::UVec3;
		else if constexpr (std::is_same_v<T, glm::uvec4>) return DataType::UVec4;
		else if constexpr (std::is_same_v<T, glm::mat3>) return DataType::Mat3;
		else if constexpr (std::is_same_v<T, glm::mat4>) return DataType::Mat4;
		else if constexpr (std::is_same_v<T, std::string>) return DataType::String;
		else if constexpr (std::is_same_v<T, std::vector<uint8_t>>) return DataType::Binary;
		else if constexpr (std::is_enum_v<T>) return DataType::Enum;
		else return DataType::Object;
	}

	// 属性描述结构体
	struct PropertyDesc
	{
		std::string Name;       // 属性名称
		DataType Type;          // 属性的数据类型
		size_t Offset;          // 工业级核心：该属性在结构体中的内存偏移量
		uint32_t FieldID;       // 属性ID，用于区分同名属性或版本控制
	};

	// 类型描述结构体
	struct TypeDesc
	{
		std::string Name = "";       // 结构体/类名称
		size_t Size = 0;            // 总内存大小
		std::vector<PropertyDesc> Properties; // 属性列表

		// 参数1：组件实例（std::any），参数2：属性描述，参数3：要设置的值（std::any）
		std::function<void(std::any, const PropertyDesc&, const std::any&)> SetValueErased;

		// 参数1：组件实例（std::any），参数2：属性描述，返回值：属性值（std::any）
		std::function<std::any(std::any, const PropertyDesc&)> GetValueErased;

		std::any UserData = {};
	};

	enum class TypeCategory
	{
		None,
		Component,
		Asset,
		Script,
	};

	// 全局类型注册表：负责存储所有注册的类型信息
	class TypeRegistry
	{
	public:
		static TypeRegistry& Get()
		{
			static TypeRegistry instance;
			return instance;
		}
	private:
		TypeRegistry() = default;
		~TypeRegistry() = default;
	public:
		// 禁止拷贝，保证注册表唯一性
		TypeRegistry(const TypeRegistry&) = delete;
		TypeRegistry& operator=(const TypeRegistry&) = delete;
	private:
		// 核心数据结构：哈希表存储类型名称到类型描述的映射，支持快速查询
		std::unordered_map<std::string, TypeDesc> m_Registry;
		std::unordered_map<TypeCategory, std::vector<std::string>> m_CategoryMap;

		// Binder 类，提供流式接口注册属性，并计算内存偏移量
		#pragma region Binder
	public:
		// TClass 是用户定义的组件类，Binder 负责为该类注册属性
		template<typename TClass>
		class Binder
		{
		public:
			Binder(TypeDesc& desc) : m_Desc(desc) {}

			constexpr uint32_t constexpr_hash(const char* str)
			{
				uint32_t hash = 2166136261u;
				while (*str)
				{
					hash = (hash ^ static_cast<uint8_t>(*str++)) * 16777619u;
				}
				return hash;
			}

			template<typename VariableType>
			Binder& Property(const std::string& name, VariableType TClass::* memberPtr, DataType type)
			{
				size_t offset = reinterpret_cast<size_t>(&(static_cast<TClass*>(nullptr)->*memberPtr));

				PropertyDesc prop { name, type, offset,constexpr_hash(name.c_str()) };
				m_Desc.Properties.push_back(prop);
				return *this;
			}

		private:
			TypeDesc& m_Desc;
		};

		#pragma endregion

	public:
		template<typename TClass>
		Binder<TClass> RegisterType(const std::string& className, TypeCategory category)
		{
			#define ASSIGN_ANY(EnumType, CppType) \
			case DataType::EnumType: \
			*reinterpret_cast<CppType*>(bytePtr) = std::any_cast<CppType>(value); \
			break;


			TypeDesc desc;
			desc.Name = className;
			desc.Size = sizeof(TClass);

			// 工业级：实现类型擦除的 Setter/Getter，保护内存安全
			desc.SetValueErased = [](std::any instancePtr, const PropertyDesc& prop, const std::any& value)
				{
					// 将 std::any 转回原始指针类型
					TClass* rawInstance = std::any_cast<TClass*>(instancePtr);
					// 计算属性的实际内存地址
					uint8_t* bytePtr = reinterpret_cast<uint8_t*>(rawInstance) + prop.Offset;


					#define ASSIGN_ANY(EnumType, CppType) \
					case DataType::EnumType: \
					*reinterpret_cast<CppType*>(bytePtr) = std::any_cast<CppType>(value); \
					break;

					switch (prop.Type)
					{
						ASSIGN_ANY(Bool, bool);
						ASSIGN_ANY(Char, char);
						ASSIGN_ANY(Int8, int8_t);
						ASSIGN_ANY(UInt8, uint8_t);
						ASSIGN_ANY(Int16, int16_t);
						ASSIGN_ANY(UInt16, uint16_t);
						ASSIGN_ANY(Int32, int32_t);
						ASSIGN_ANY(UInt32, uint32_t);
						ASSIGN_ANY(Int64, int64_t);
						ASSIGN_ANY(UInt64, uint64_t);
						ASSIGN_ANY(Float, float);
						ASSIGN_ANY(Double, double);
						ASSIGN_ANY(Vec2, glm::vec2);
						ASSIGN_ANY(Vec3, glm::vec3);
						ASSIGN_ANY(Vec4, glm::vec4);
						ASSIGN_ANY(IVec2, glm::ivec2);
						ASSIGN_ANY(IVec3, glm::ivec3);
						ASSIGN_ANY(IVec4, glm::ivec4);
						ASSIGN_ANY(UVec2, glm::uvec2);
						ASSIGN_ANY(UVec3, glm::uvec3);
						ASSIGN_ANY(UVec4, glm::uvec4);
						ASSIGN_ANY(Mat3, glm::mat3);
						ASSIGN_ANY(Mat4, glm::mat4);
						ASSIGN_ANY(String, std::string);
						ASSIGN_ANY(Binary, std::vector<uint8_t>);
						ASSIGN_ANY(Enum, uint32_t); // 枚举底层通常是 uint32_t，但反射时需要特殊处理)
						ASSIGN_ANY(Object, std::any); // 对象类型需要特殊处理，暂时用 std::any 占位

						default: break;
					}

					#undef ASSIGN_ANY // 用完立即取消宏，防止污染全局
				};

			desc.GetValueErased = [](std::any instancePtr, const PropertyDesc& prop) -> std::any
				{
					TClass* rawInstance = std::any_cast<TClass*>(instancePtr);
					uint8_t* bytePtr = reinterpret_cast<uint8_t*>(rawInstance) + prop.Offset;

					#define RETURN_ANY(EnumType, CppType) \
					case DataType::EnumType: \
					return *reinterpret_cast<CppType*>(bytePtr);

					switch (prop.Type)
					{
						RETURN_ANY(Bool, bool);
						RETURN_ANY(Char, char);
						RETURN_ANY(Int8, int8_t);
						RETURN_ANY(UInt8, uint8_t);
						RETURN_ANY(Int16, int16_t);
						RETURN_ANY(UInt16, uint16_t);
						RETURN_ANY(Int32, int32_t);
						RETURN_ANY(UInt32, uint32_t);
						RETURN_ANY(Int64, int64_t);
						RETURN_ANY(UInt64, uint64_t);
						RETURN_ANY(Float, float);
						RETURN_ANY(Double, double);
						RETURN_ANY(Vec2, glm::vec2);
						RETURN_ANY(Vec3, glm::vec3);
						RETURN_ANY(Vec4, glm::vec4);
						RETURN_ANY(IVec2, glm::ivec2);
						RETURN_ANY(IVec3, glm::ivec3);
						RETURN_ANY(IVec4, glm::ivec4);
						RETURN_ANY(UVec2, glm::uvec2);
						RETURN_ANY(UVec3, glm::uvec3);
						RETURN_ANY(UVec4, glm::uvec4);
						RETURN_ANY(Mat3, glm::mat3);
						RETURN_ANY(Mat4, glm::mat4);
						RETURN_ANY(String, std::string);
						RETURN_ANY(Binary, std::vector<uint8_t>);
						// 枚举和对象类型需要特殊处理，暂时用 std::any 占位
						default:
							return std::any();
					}
				};

			// 存入全局哈希表
			m_Registry[className] = desc;
			m_CategoryMap[category].push_back(className);
			return Binder<TClass>(m_Registry[className]);
		}

		// 根据类名获取类型描述信息，返回指针以避免不必要的复制
		const TypeDesc* GetTypeDesc(const std::string& className) const
		{
			auto it = m_Registry.find(className);
			if (it != m_Registry.end()) return &it->second;
			return nullptr;
		}

		TypeDesc* GetTypeDesc(const std::string& className)
		{
			auto it = m_Registry.find(className);
			if (it != m_Registry.end()) return &it->second;
			return nullptr;
		}


		const std::vector<std::string>& GetTypesByCategory(TypeCategory category) const
		{
			static const std::vector<std::string> emptyList;
			auto it = m_CategoryMap.find(category);
			if (it != m_CategoryMap.end()) return it->second;
			return emptyList;
		}

		const std::unordered_map<std::string, TypeDesc>& GetTemplateMap() const { return m_Registry; }

		template <typename T>
		static void RegisterTypeData(TypeCategory category)
		{
			std::any& userData = TypeRegistry::Get().GetTypeDesc(typeid(T).name())->UserData;

			switch (category)
			{
				case TypeCategory::Component:
					TypeDescDataComponent::Register<T>(userData);
					break;
			}
		}

	};

	#define REFLECT_BODY(TClass,Category) \
	using TREFLECTClass = TClass; \
	inline static struct AutoRegister_##TClass{ \
		AutoRegister_##TClass() { \
			const std::string className = typeid(TClass).name(); \
			TypeRegistry::Get().RegisterType<TClass>(className, Category); \
			TypeRegistry::RegisterTypeData<TClass>(Category);} \
	} s_AutoRegister;

	#define PROPERTY(varName) \
	inline static struct AutoProp_##varName{ \
		AutoProp_##varName(){ \
		TypeRegistry::Binder<TREFLECTClass>(*TypeRegistry::Get().GetTypeDesc(typeid(TREFLECTClass).name())).Property(#varName, &TREFLECTClass::varName, GetDataType<decltype(varName)>());} \
	}s_AutoProp_##varName;
}