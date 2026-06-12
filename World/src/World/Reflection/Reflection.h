#pragma once
#include <any>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
namespace World
{
	template <typename T>
	struct is_ref_type : std::false_type {};
	template <typename T>
	struct is_ref_type<Ref<T>> : std::true_type {};

	// 数据类型枚举
	enum class DataType : uint8_t
	{
		None = 0,

		// ==========================================
		// 1. 基础标量类型 (按内存大小升序，1 -> 2 -> 4 -> 8 字节)
		// ==========================================
		#pragma region Base
		Bool,
		Char,
		Int8,
		UInt8,
		Int16,
		UInt16,
		Int32,
		UInt32,
		Int64,
		UInt64,
		Float,
		Double,
		#pragma endregion

		// ==========================================
		// 2. 数学向量类型 (分量数量升序，2 -> 3 -> 4)
		// ==========================================
		#pragma region Vector
	   // 8 字节 (2 * float)
		Vec2,
		// 12字节 (3 * float)
		Vec3,
		// 16字节 (4 * float)
		Vec4,

		Quat,

		// 8 字节 (2 * int32，新增：UI坐标/网格索引)
		IVec2,
		// 12字节 (3 * int32，新增：3D网格索引)
		IVec3,
		// 16字节 (4 * int32，新增：骨骼动画影响的 Bone IDs)
		IVec4,

		// 8 字节 (2 * uint32，新增)
		UVec2,
		// 12字节 (3 * uint32，新增)
		UVec3,
		// 16字节 (4 * uint32，新增)
		UVec4,

		#pragma endregion

		// ==========================================
		// 3. 矩阵类型 (游戏引擎中 Mat3/Mat4 最常用)
		// ==========================================
		#pragma region Matrix

		// 36字节 (3x3 float，新增：通常用于传递法线矩阵)
		Mat3,

		// 64字节 (4x4 float，变换矩阵)
		Mat4,

		#pragma endregion

		// ==========================================
		// 4. 高级/复合/引用类型 (属于不确定大小或堆分配类型)
		// ==========================================
		#pragma region Senior 

		// 字符串 (内部一般对应 std::string)
		String,

		// 二进制大对象 (新增，对应 std::vector<uint8_t> 或 裸数据指针，用于自定义序列化)
		Binary,

		// 枚举类型 (新增，底层通常是 uint32_t，但反射时需要特殊处理)
		Enum,

		// 嵌套子对象/组件 (新增，用于支持“类中类”的深层反射),需要特殊处理的复杂类型
		Object,

		AssetHandle,
		#pragma endregion

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
		else if constexpr (std::is_same_v<T, glm::quat>) return DataType::Quat;
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
		else if constexpr (is_ref_type<T>::value) return DataType::AssetHandle;
		else return DataType::Object;
	}

	struct EnumDesc
	{
		std::string Name; // 类型名称
		uint32_t UnderlyingSize = 4;
		bool IsSigned = false;
	};

	struct ObjectDesc
	{
		std::string Name; // 类型名称
	};

	struct AssetDesc
	{
		std::string Name; // 类型名称
		std::string Path; // 资源路径
	};

	// 属性描述结构体
	struct PropertyDesc
	{
		std::string Name;       // 属性名称
		DataType Type;          // 属性的数据类型
		size_t Offset;          // 工业级核心：该属性在结构体中的内存偏移量
		uint32_t FieldID;       // 属性ID，用于区分同名属性或版本控制
		std::any UserData;     // 用户数据字段，允许绑定任意类型的数据（如属性特定的反射信息、编辑器元数据等），实现高度灵活的扩展
	};

	enum class TypeCategory
	{
		None,
		Component, // 游戏对象组件
		Asset, // 游戏资源（纹理、模型、音频等）
		Script, // 游戏逻辑脚本
		EnumClass, // 枚举类
		NormalClass, // 普通类
	};

	static constexpr std::string_view GetTypeIdName(std::string_view name)
	{
		std::string_view cleanName = name;
		size_t lastColon = cleanName.find_last_of(':');
		if (lastColon != std::string_view::npos)
		{
			return cleanName.substr(lastColon + 1);
		}

		// 2. 兜底：如果没有命名空间，但 MSVC 带有 "struct " 或 "class " 前缀，按最后一个空格切
		size_t lastSpace = cleanName.find_last_of(' ');
		if (lastSpace != std::string_view::npos)
		{
			return cleanName.substr(lastSpace + 1);
		}

		return cleanName;
	}

	// 类型描述结构体
	struct TypeDesc
	{
		std::string Name = "";       // 类型名称
		size_t Size = 0;            // 总内存大小
		std::vector<PropertyDesc> Properties; // 属性列表

		// 参数1：组件实例（std::any），参数2：属性描述，参数3：要设置的值（std::any）
		std::function<void(void*, const PropertyDesc&, const std::any&)> SetValueErased;

		// 参数1：组件实例（std::any），参数2：属性描述，返回值：属性值（std::any）
		std::function<std::any(void*, const PropertyDesc&)> GetValueErased;

		// 用户数据字段，允许绑定任意类型的数据（如组件特定的反射信息、编辑器元数据等），实现高度灵活的扩展
		std::any UserData = {};

		TypeCategory Category = TypeCategory::None; // 类型分类，便于编辑器组织和过滤

	};

	class Texture2D;
	// 全局类型注册表：负责存储所有注册的类型信息
	class TypeRegistry
	{
	public:
		static TypeRegistry& Get();
		void MergeFrom(const TypeRegistry& other);
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

			// FNV-1a 32位哈希函数，编译时计算字符串的哈希值，用于生成属性ID
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

				std::any userData = {};


				if constexpr (std::is_enum_v<VariableType>)
				{
					// 枚举变量
					EnumDesc enumDesc;
					enumDesc.Name = static_cast<std::string>(GetTypeIdName(typeid(VariableType).name()));
					enumDesc.UnderlyingSize = sizeof(std::underlying_type_t<VariableType>);
					enumDesc.IsSigned = std::is_signed_v<std::underlying_type_t<VariableType>>;

					userData = enumDesc;
				}
				else if constexpr (is_ref_type<VariableType>::value)
				{
					AssetDesc assetDesc;
					assetDesc.Name = static_cast<std::string>(GetTypeIdName(typeid(VariableType::element_type).name()));
					assetDesc.Path = "";
					userData = assetDesc;
				}
				else
				{
					if (type == DataType::Object)
					{
						userData = ObjectDesc { static_cast<std::string>(GetTypeIdName(typeid(VariableType).name())) };
					}
				}

				PropertyDesc prop { name, type, offset,constexpr_hash(name.c_str()),userData };
				m_Desc.Properties.push_back(prop);
				return *this;
			}

			// 枚举类型内部Property
			template <typename EnumType>
			Binder& PropertyEnum(const std::string& enumName, EnumType value)
			{
				DataType type;
				constexpr bool isSigned = std::is_signed_v<std::underlying_type_t<EnumType>>;
				constexpr uint32_t underlyingSize = sizeof(std::underlying_type_t<EnumType>);
				if constexpr (underlyingSize == 1)
				{
					type = isSigned ? DataType::Int8 : DataType::UInt8;
				}
				else if constexpr (underlyingSize == 2)
				{
					type = isSigned ? DataType::Int16 : DataType::UInt16;
				}
				else if constexpr (underlyingSize == 4)
				{
					type = isSigned ? DataType::Int32 : DataType::UInt32;
				}
				else if constexpr (underlyingSize == 8)
				{
					type = isSigned ? DataType::Int64 : DataType::UInt64;
				}
				PropertyDesc propName { enumName ,type, 0, 0, value };
				m_Desc.Properties.push_back(propName);
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
			desc.Category = category;
			// 实现类型擦除的 Setter/Getter，保护内存安全
			desc.SetValueErased = [](void* instancePtr, const PropertyDesc& prop, const std::any& value)
				{
					TClass* rawInstance = static_cast<TClass*>(instancePtr);

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
						ASSIGN_ANY(Quat, glm::quat);
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
						case DataType::Enum:
						{
							const EnumDesc& enumDesc = std::any_cast<EnumDesc>(prop.UserData);
							switch (enumDesc.UnderlyingSize)
							{
								case 1:
									if (enumDesc.IsSigned)
										*reinterpret_cast<int8_t*>(bytePtr) = std::any_cast<int8_t>(value);
									else
										*reinterpret_cast<uint8_t*>(bytePtr) = std::any_cast<uint8_t>(value);
									break;
								case 2:
									if (enumDesc.IsSigned)
										*reinterpret_cast<int16_t*>(bytePtr) = std::any_cast<int16_t>(value);
									else
										*reinterpret_cast<uint16_t*>(bytePtr) = std::any_cast<uint16_t>(value);
									break;
								case 4:
									if (enumDesc.IsSigned)
										*reinterpret_cast<int32_t*>(bytePtr) = std::any_cast<int32_t>(value);
									else
										*reinterpret_cast<uint32_t*>(bytePtr) = std::any_cast<uint32_t>(value);
									break;
								case 8:
									if (enumDesc.IsSigned)
										*reinterpret_cast<int64_t*>(bytePtr) = std::any_cast<int64_t>(value);
									else
										*reinterpret_cast<uint64_t*>(bytePtr) = std::any_cast<uint64_t>(value);
									break;
								default:
									WLD_ERROR("Unsupported enum underlying size: {}", enumDesc.UnderlyingSize);
							}
							break;
						}
						case DataType::AssetHandle:
						{
							/*
							Ref<Texture2D> newTexture = Texture2D::Create(...);
							classDesc->SetValueErased(&component, prop, std::any(static_cast<void*>(&newTexture)));
							*/
							// 资源句柄类型，假设是 Ref<T>，内部存储一个指针
							if (value.has_value())
							{
								if (value.type() == typeid(Ref<Texture2D>))
								{
									Ref<Texture2D> srcPtr = std::any_cast<Ref<Texture2D>>(value);

									// 采用正确的 C++ 赋值操作符进行拷贝和引用计数，而非直接按位硬拷贝内存
									*reinterpret_cast<Ref<Texture2D>*>(bytePtr) = srcPtr;
								}
								// 兼容旧有的反射保存 void* 指针情况（如果有别的途径走这里）
								else if (value.type() == typeid(void*))
								{
									void* srcPtr = std::any_cast<void*>(value);
									if (srcPtr)
									{
										// MSVC shared_ptr 大小为 16 字节
										// 这是非常危险的！仅为向前兼容保留
										std::memcpy(bytePtr, srcPtr, 16);
									}
								}
							}
							break;
						}
						case DataType::Object:
						{
							const ObjectDesc& objDesc = std::any_cast<ObjectDesc>(prop.UserData);
							const TypeDesc* typeDesc = TypeRegistry::Get().GetTypeDesc(objDesc.Name);
							if (value.has_value())
							{
								if (value.type() == typeid(void*))
								{
									void* srcPtr = std::any_cast<void*>(value);
									if (srcPtr)
									{
										// 核心：把外面传进来的结构体内存，按字节直接铺到当前成员的地址上
										std::memcpy(bytePtr, srcPtr, typeDesc->Size);
									}
								}
							}
							break;
						}

						default:
							WLD_ERROR("Unsupported data type for property '{}': {}", prop.Name, static_cast<int>(prop.Type));
							break;
					}

					#undef ASSIGN_ANY // 用完立即取消宏，防止污染全局
				};

			desc.GetValueErased = [](void* instancePtr, const PropertyDesc& prop) -> std::any
				{
					TClass* rawInstance = static_cast<TClass*>(instancePtr);

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
						RETURN_ANY(Quat, glm::quat);
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
						case DataType::Enum:
						{
							const EnumDesc& enumDesc = std::any_cast<EnumDesc>(prop.UserData);
							switch (enumDesc.UnderlyingSize)
							{
								case 1:
									if (enumDesc.IsSigned)
										return std::any(*reinterpret_cast<int8_t*>(bytePtr));
									else
										return std::any(*reinterpret_cast<uint8_t*>(bytePtr));
								case 2:
									if (enumDesc.IsSigned)
										return std::any(*reinterpret_cast<int16_t*>(bytePtr));
									else
										return std::any(*reinterpret_cast<uint16_t*>(bytePtr));
								case 4:
									if (enumDesc.IsSigned)
										return std::any(*reinterpret_cast<int32_t*>(bytePtr));
									else
										return std::any(*reinterpret_cast<uint32_t*>(bytePtr));
								case 8:
									if (enumDesc.IsSigned)
										return std::any(*reinterpret_cast<int64_t*>(bytePtr));
									else
										return std::any(*reinterpret_cast<uint64_t*>(bytePtr));
								default:
									WLD_ERROR("Unsupported enum underlying size: {}", enumDesc.UnderlyingSize);
							}
							break;
						}
						case DataType::AssetHandle:
						{
							/*
							std::any val = classDesc->GetValueErased(&component, prop);
							void* rawRefPtr = std::any_cast<void*>(val);
							Ref<Texture2D> myRef = *static_cast<Ref<Texture2D>*>(rawRefPtr);
							*/
							// 返回一个指向整个 Ref<T> 对象的 void*（此时它的 type 应该是 void*，代表 "地址"）
							// 外面拿到后可以强制转换成 Ref<T>* 并解引用，从而正确触发拷贝构造
							return std::any(reinterpret_cast<void*>(bytePtr));
						}
						case DataType::Object:
						{
							const ObjectDesc& objDesc = std::any_cast<ObjectDesc>(prop.UserData);

							const TypeDesc* objTypeDesc = TypeRegistry::Get().GetTypeDesc(objDesc.Name);

							if (objTypeDesc)
							{
								std::unordered_map<std::string, std::any> childValues;

								// 此时在这个大对象里的成员起始地址 bytePtr ，也就是这个内嵌的小结构体的地址指针
								// 它相当于内部子对象的 instancePtr ！
								void* childInstancePtr = reinterpret_cast<void*>(bytePtr);

								// 遍历这个嵌套类型的所有内部属性，并再次调用 GetValueErased
								for (const auto& childProp : objTypeDesc->Properties)
								{
									childValues[childProp.Name] = objTypeDesc->GetValueErased(childInstancePtr, childProp);
								}

								return std::make_any<std::unordered_map<std::string, std::any>>(childValues);
							}

							return std::make_any<std::unordered_map<std::string, std::any>>();
						}
						default:
							WLD_ERROR("Unsupported data type for property '{}': {}", prop.Name, static_cast<int>(prop.Type));
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

		template <TypeCategory Category, typename T>
		static void RegisterTypeData()
		{
			std::any& userData = TypeRegistry::Get().GetTypeDesc(static_cast<std::string>(GetTypeIdName(typeid(T).name())))->UserData;

			if constexpr (Category == TypeCategory::Component)
			{
				TypeDescDataComponent::Register<T>(userData);
			}
			else if constexpr (Category == TypeCategory::Script)
			{
				TypeDescDataScript::Register<T>(userData);
			}
		}

	};

	#define REFLECT_BODY(TClass,Category) \
	using TREFLECTClass = TClass; \
	inline static struct AutoRegister_##TClass{ \
		AutoRegister_##TClass() { \
			const std::string className = static_cast<std::string>(GetTypeIdName(typeid(TClass).name())); \
			TypeRegistry::Get().RegisterType<TClass>(className, Category); \
			TypeRegistry::RegisterTypeData<Category, TClass>();} \
	} s_AutoRegister;

	#define PROPERTY(varName) \
	inline static struct AutoProp_##varName{ \
		AutoProp_##varName(){ \
		TypeRegistry::Binder<TREFLECTClass>(*TypeRegistry::Get().GetTypeDesc(static_cast<std::string>(GetTypeIdName(typeid(TREFLECTClass).name())))).Property(#varName, &TREFLECTClass::varName, GetDataType<decltype(varName)>());} \
	}s_AutoProp_##varName;


	template<typename TEnum>
	struct AutoRegisterEnum
	{
		AutoRegisterEnum()
		{
			const std::string enumName = static_cast<std::string>(GetTypeIdName(typeid(TEnum).name()));
			TypeRegistry::Get().RegisterType<TEnum>(enumName, TypeCategory::EnumClass);
			TypeRegistry::RegisterTypeData<TypeCategory::EnumClass, TEnum>();
		}
	};

	#define REFLECT_ENUM(TClass) \
	inline static AutoRegisterEnum<TClass> s_AutoRegisterEnum;

	#define PROPERTY_ENUM(TEnum, varName) \
    inline static struct AutoPropEnum_##TEnum##_##varName { \
        AutoPropEnum_##TEnum##_##varName() { \
            TypeRegistry::Binder<TEnum>(*TypeRegistry::Get().GetTypeDesc(static_cast<std::string>(GetTypeIdName(typeid(TEnum).name())))).PropertyEnum<TEnum>(#varName, TEnum::varName); \
        } \
    } s_AutoPropEnum_##TEnum##_##varName;
}