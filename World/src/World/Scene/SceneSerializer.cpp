#include "wldpch.h"
#include "SceneSerializer.h"

#include "World/Scene/Entity.h"
#include "World/Scene/Components.h"
#include "World/Core/UUID.h"

#include <filesystem>
#include <fstream>
#include <yaml-cpp/yaml.h>

namespace YAML
{
	template<>
	struct convert<glm::vec2>
	{
		static Node encode(const glm::vec2& vec)
		{
			Node node;
			node.push_back(vec.x);
			node.push_back(vec.y);
			return node;
		}
		static bool decode(const Node& node, glm::vec2& vec)
		{
			if (!node.IsSequence() || node.size() != 2)
				return false;
			vec.x = node[0].as<float>();
			vec.y = node[1].as<float>();
			return true;
		}
	};

	template<>
	struct convert<glm::vec3>
	{
		static Node encode(const glm::vec3& vec)
		{
			Node node;
			node.push_back(vec.x);
			node.push_back(vec.y);
			node.push_back(vec.z);
			return node;
		}
		static bool decode(const Node& node, glm::vec3& vec)
		{
			if (!node.IsSequence() || node.size() != 3)
				return false;
			vec.x = node[0].as<float>();
			vec.y = node[1].as<float>();
			vec.z = node[2].as<float>();
			return true;
		}

	};

	template<>
	struct convert<glm::vec4>
	{
		static Node encode(const glm::vec4& vec)
		{
			Node node;
			node.push_back(vec.x);
			node.push_back(vec.y);
			node.push_back(vec.z);
			node.push_back(vec.w);
			return node;
		}
		static bool decode(const Node& node, glm::vec4& vec)
		{
			if (!node.IsSequence() || node.size() != 4)
				return false;
			vec.x = node[0].as<float>();
			vec.y = node[1].as<float>();
			vec.z = node[2].as<float>();
			vec.w = node[3].as<float>();
			return true;
		}
	};

	template<>
	struct convert<glm::quat>
	{
		static Node encode(const glm::quat& quat)
		{
			Node node;
			node.push_back(quat.x);
			node.push_back(quat.y);
			node.push_back(quat.z);
			node.push_back(quat.w);
			return node;
		}
		static bool decode(const Node& node, glm::quat& quat)
		{
			if (!node.IsSequence() || node.size() != 4)
				return false;
			quat.x = node[0].as<float>();
			quat.y = node[1].as<float>();
			quat.z = node[2].as<float>();
			quat.w = node[3].as<float>();
			return true;
		}
	};

	template<>
	struct convert<glm::mat3>
	{
		static Node encode(const glm::mat3& mat)
		{
			Node node;
			for (int i = 0; i < 3; i++)
			{
				for (int j = 0; j < 3; j++)
				{
					node.push_back(mat[i][j]);
				}
			}
			return node;
		}
		static bool decode(const Node& node, glm::mat3& mat)
		{
			if (!node.IsSequence() || node.size() != 9)
				return false;
			for (int i = 0; i < 3; i++)
			{
				for (int j = 0; j < 3; j++)
				{
					mat[i][j] = node[i * 3 + j].as<float>();
				}
			}
			return true;
		}
	};

	template<>
	struct convert<glm::mat4>
	{
		static Node encode(const glm::mat4& mat)
		{
			Node node;
			for (int i = 0; i < 4; i++)
			{
				for (int j = 0; j < 4; j++)
				{
					node.push_back(mat[i][j]);
				}
			}
			return node;
		}
		static bool decode(const Node& node, glm::mat4& mat)
		{
			if (!node.IsSequence() || node.size() != 16)
				return false;
			for (int i = 0; i < 4; i++)
			{
				for (int j = 0; j < 4; j++)
				{
					mat[i][j] = node[i * 4 + j].as<float>();
				}
			}
			return true;
		}
	};
}
namespace World
{
	template<typename T>
	void AsValue(const YAML::Node& node, const char* key, T& value)
	{
		if (auto field = node[key])
		{
			try
			{
				value = field.as<T>();
			}
			catch (const std::exception& e)
			{
				WLD_CORE_WARN("Failed to parse field '{0}': {1}", key, e.what());
			}
		}
		else
		{
			WLD_CORE_WARN("Field '{0}' not found in YAML node!", key);
		}
	}
	template<typename T>
	T AsValue(const YAML::Node& node, const char* key)
	{
		if (auto field = node[key])
		{
			try
			{
				T value = field.as<T>();
				return value;
			}
			catch (const std::exception& e)
			{
				WLD_CORE_WARN("Failed to parse field '{0}': {1}", key, e.what());
			}
		}
		else
		{
			WLD_CORE_WARN("Field '{0}' not found in YAML node!", key);
		}
	}


	YAML::Emitter& operator<<(YAML::Emitter& out, const glm::vec2& vec)
	{
		out << YAML::Flow << YAML::BeginSeq << vec.x << vec.y << YAML::EndSeq;
		return out;
	}
	YAML::Emitter& operator<<(YAML::Emitter& out, const glm::vec3& vec)
	{
		out << YAML::Flow << YAML::BeginSeq << vec.x << vec.y << vec.z << YAML::EndSeq;
		return out;
	}
	YAML::Emitter& operator<<(YAML::Emitter& out, const glm::quat& quat)
	{
		out << YAML::Flow << YAML::BeginSeq << quat.x << quat.y << quat.z << quat.w << YAML::EndSeq;
		return out;
	}
	YAML::Emitter& operator<<(YAML::Emitter& out, const glm::vec4& vec)
	{
		out << YAML::Flow << YAML::BeginSeq << vec.x << vec.y << vec.z << vec.w << YAML::EndSeq;
		return out;
	}
	YAML::Emitter& operator<<(YAML::Emitter& out, const std::filesystem::path& path)
	{
		out << path.string();
		return out;
	}
	YAML::Emitter& operator<<(YAML::Emitter& out, const glm::mat3& mat)
	{
		out << YAML::Flow << YAML::BeginSeq;
		for (int i = 0; i < 3; i++)
		{
			for (int j = 0; j < 3; j++)
			{
				out << mat[i][j];
			}
		}
		out << YAML::EndSeq;
		return out;
	}

	YAML::Emitter& operator<<(YAML::Emitter& out, const glm::mat4& mat)
	{
		out << YAML::Flow << YAML::BeginSeq;
		for (int i = 0; i < 4; i++)
		{
			for (int j = 0; j < 4; j++)
			{
				out << mat[i][j];
			}
		}
		out << YAML::EndSeq;
		return out;
	}


	SceneSerializer::SceneSerializer(const Ref<Scene>& scene)
		: m_Scene(scene)
	{

	}

	static void SerializeProperties(YAML::Emitter& out, const TypeDesc* typeDesc, void* instance)
	{
		for (const auto& property : typeDesc->Properties)
		{
			// 1. 基础类型序列化宏优化
			#define SERIALIZE_PROPERTY(Type, CPPType) \
			case DataType::Type: \
				out << YAML::Key << property.Name << YAML::Value \
					<< std::any_cast<CPPType>(typeDesc->GetValueErased(instance, property)); \
				break;

			switch (property.Type)
			{
				SERIALIZE_PROPERTY(Bool, bool);
				SERIALIZE_PROPERTY(Char, char);
				SERIALIZE_PROPERTY(Int8, int8_t);
				SERIALIZE_PROPERTY(UInt8, uint8_t);
				SERIALIZE_PROPERTY(Int16, int16_t);
				SERIALIZE_PROPERTY(UInt16, uint16_t);
				SERIALIZE_PROPERTY(Int32, int32_t);
				SERIALIZE_PROPERTY(UInt32, uint32_t);
				SERIALIZE_PROPERTY(Int64, int64_t);
				SERIALIZE_PROPERTY(UInt64, uint64_t);
				SERIALIZE_PROPERTY(Float, float);
				SERIALIZE_PROPERTY(Double, double);
				SERIALIZE_PROPERTY(Vec2, glm::vec2);
				SERIALIZE_PROPERTY(Vec3, glm::vec3);
				SERIALIZE_PROPERTY(Vec4, glm::vec4);
				SERIALIZE_PROPERTY(Quat, glm::quat);
				SERIALIZE_PROPERTY(Mat3, glm::mat3);
				SERIALIZE_PROPERTY(Mat4, glm::mat4);
				SERIALIZE_PROPERTY(String, std::string);

				case DataType::Enum:
				{
					// 优化：将公共的 YAML Key 写入提取到外部
					out << YAML::Key << property.Name << YAML::Value;

					EnumDesc enumDesc = std::any_cast<EnumDesc>(property.UserData);
					std::any enumValue = typeDesc->GetValueErased(instance, property);

					switch (enumDesc.UnderlyingSize)
					{
						case 1:
							if (enumDesc.IsSigned) out << (int32_t)std::any_cast<int8_t>(enumValue);
							else                   out << (uint32_t)std::any_cast<uint8_t>(enumValue);
							break;
						case 2:
							if (enumDesc.IsSigned) out << (int32_t)std::any_cast<int16_t>(enumValue);
							else                   out << (uint32_t)std::any_cast<uint16_t>(enumValue);
							break;
						case 4:
							if (enumDesc.IsSigned) out << std::any_cast<int32_t>(enumValue);
							else                   out << std::any_cast<uint32_t>(enumValue);
							break;
						case 8:
							if (enumDesc.IsSigned) out << std::any_cast<int64_t>(enumValue);
							else                   out << std::any_cast<uint64_t>(enumValue);
							break;
						default:
							WLD_CORE_WARN("Unsupported enum underlying size for property '{0}'!", property.Name);
							out << 0; // 兜底防止 YAML 格式错乱
							break;
					}
					break;
				}
				case DataType::AssetHandle:
				{
					AssetDesc assetDesc = std::any_cast<AssetDesc>(property.UserData);
					std::any val = typeDesc->GetValueErased(instance, property);
					void* rawRefPtr = std::any_cast<void*>(val);

					// 暂时只支持 Texture2D 类型的 Ref<T>，后续可以根据 assetDesc.Type 来支持更多类型
					Ref<Texture2D> myRef = *static_cast<Ref<Texture2D>*>(rawRefPtr);

					std::string assetPath = myRef.get() ? myRef->GetPath() : "";
					out << YAML::Key << property.Name << YAML::Value << assetPath;
					break;
				}

				case DataType::Object:
				{
					ObjectDesc objectDesc = std::any_cast<ObjectDesc>(property.UserData);
					const TypeDesc* objTypeDesc = TypeRegistry::Get().GetTypeDesc(objectDesc.Name);

					if (objTypeDesc)
					{
						out << YAML::Key << property.Name << YAML::Value;
						out << YAML::BeginMap;

						uint8_t* bytePtr = reinterpret_cast<uint8_t*>(instance) + property.Offset;
						void* childInstancePtr = reinterpret_cast<void*>(bytePtr);

						if (childInstancePtr)
						{
							SerializeProperties(out, objTypeDesc, childInstancePtr);
						}

						out << YAML::EndMap;
					}
					break;
				}
			}

			#undef SERIALIZE_PROPERTY
		}
	}

	static void SerializeEntity(YAML::Emitter& out, Entity entity)
	{
		WLD_ASSERT(entity.HasComponent<UUIDComponent>(), "Entity does not have a UUIDComponent!");

		out << YAML::BeginMap;

		for (const auto& componentName : TypeRegistry::Get().GetTypesByCategory(TypeCategory::Component))
		{
			TypeDesc* typeDesc = TypeRegistry::Get().GetTypeDesc(componentName);
			if (TypeDescDataComponent* componentInfo = std::any_cast<TypeDescDataComponent>(&typeDesc->UserData))
			{
				if (entity.HasComponent(componentInfo->Id))
				{
					out << YAML::Key << typeDesc->Name << YAML::Value;
					out << YAML::BeginMap;

					void* componentInstance = entity.GetComponent(componentInfo->Id);
					SerializeProperties(out, typeDesc, componentInstance);

					out << YAML::EndMap;
				}
			}

		}
		out << YAML::EndMap;
	}
	void SceneSerializer::Serialize(const std::string& filepath)
	{
		YAML::Emitter out;
		out << YAML::BeginMap;
		{
			out << YAML::Key << "Scene" << YAML::Value << "Untitled";

			out << YAML::Key << "Entities" << YAML::Value << YAML::BeginSeq;

			// 使用反向迭代器遍历实体，以修正保存加载后的顺序颠倒问题
			auto& entities = m_Scene->m_Registry.storage<entt::entity>();

			for (auto it = entities.rbegin(); it != entities.rend(); ++it)
			{
				Entity ent { m_Scene.get(), *it };
				if (!ent)
				{
					return;
				};

				SerializeEntity(out, ent);
			}
			out << YAML::EndSeq;
		}
		out << YAML::EndMap;


		{
			// 1. 将字符串路径转换为 path 对象
			std::filesystem::path path(filepath);

			// 2. 获取该文件所在的父目录路径
			// 例如：filepath 是 "assets/scenes/level.yaml"，parentPath 就是 "assets/scenes"
			std::filesystem::path parentPath = path.parent_path();

			// 3. 检查并创建路径
			// 如果 parentPath 为空（说明文件就在当前目录下），create_directories 会安全跳过
			if (!parentPath.empty() && !std::filesystem::exists(parentPath))
			{
				// create_directories 会创建多级目录（不存在的都会补齐）
				std::filesystem::create_directories(parentPath);
			}

			// 4. 现在可以放心地创建并写入文件了
			std::ofstream fout(filepath);
			if (fout.is_open())
			{
				fout << out.c_str();
				fout.close();
			}
		}
	}

	static void DeserializeProperties(const YAML::Node& node, const TypeDesc* typeDesc, void* instance)
	{
		for (const auto& property : typeDesc->Properties)
		{
			// 该属性在 YAML 中不存在时跳过
			if (!node[property.Name])
				continue;

			auto propNode = node[property.Name];

			#define DESERIALIZE_PROPERTY(Type, CPPType) \
			case DataType::Type: \
			{ \
				CPPType val{}; \
				AsValue<CPPType>(node, property.Name.c_str(), val); \
				typeDesc->SetValueErased(instance, property, std::any(val)); \
				break; \
			}

			switch (property.Type)
			{
				DESERIALIZE_PROPERTY(Bool, bool);
				DESERIALIZE_PROPERTY(Char, char);
				DESERIALIZE_PROPERTY(Int8, int8_t);
				DESERIALIZE_PROPERTY(UInt8, uint8_t);
				DESERIALIZE_PROPERTY(Int16, int16_t);
				DESERIALIZE_PROPERTY(UInt16, uint16_t);
				DESERIALIZE_PROPERTY(Int32, int32_t);
				DESERIALIZE_PROPERTY(UInt32, uint32_t);
				DESERIALIZE_PROPERTY(Int64, int64_t);
				DESERIALIZE_PROPERTY(UInt64, uint64_t);
				DESERIALIZE_PROPERTY(Float, float);
				DESERIALIZE_PROPERTY(Double, double);
				DESERIALIZE_PROPERTY(Vec2, glm::vec2);
				DESERIALIZE_PROPERTY(Vec3, glm::vec3);
				DESERIALIZE_PROPERTY(Vec4, glm::vec4);
				DESERIALIZE_PROPERTY(Quat, glm::quat);
				DESERIALIZE_PROPERTY(Mat3, glm::mat3);
				DESERIALIZE_PROPERTY(Mat4, glm::mat4);
				DESERIALIZE_PROPERTY(String, std::string);

				case DataType::Enum:
				{
					EnumDesc enumDesc = std::any_cast<EnumDesc>(property.UserData);
					switch (enumDesc.UnderlyingSize)
					{
						case 1:
							if (enumDesc.IsSigned)
								typeDesc->SetValueErased(instance, property, std::any(AsValue<int8_t>(node, property.Name.c_str())));
							else
								typeDesc->SetValueErased(instance, property, std::any(AsValue<uint8_t>(node, property.Name.c_str())));
							break;
						case 2:
							if (enumDesc.IsSigned)
								typeDesc->SetValueErased(instance, property, std::any(AsValue<int16_t>(node, property.Name.c_str())));
							else
								typeDesc->SetValueErased(instance, property, std::any(AsValue<uint16_t>(node, property.Name.c_str())));
							break;
						case 4:
							if (enumDesc.IsSigned)
								typeDesc->SetValueErased(instance, property, std::any(AsValue<int32_t>(node, property.Name.c_str())));
							else
								typeDesc->SetValueErased(instance, property, std::any(AsValue<uint32_t>(node, property.Name.c_str())));
							break;
						case 8:
							if (enumDesc.IsSigned)
								typeDesc->SetValueErased(instance, property, std::any(AsValue<int64_t>(node, property.Name.c_str())));
							else
								typeDesc->SetValueErased(instance, property, std::any(AsValue<uint64_t>(node, property.Name.c_str())));
							break;
					}
					break;
				}
				case DataType::AssetHandle:
				{
					// 由于你在反射保存里是用指针交互且仅限于 Texture2D，这里的简单特化
					std::string path = AsValue<std::string>(node, property.Name.c_str());
					if (!path.empty())
					{
						// 创建出临时智能指针并保持在栈帧中存活
						Ref<Texture2D> newTexture = Texture2D::Create(path);
						// 此处不传 void* ，直接把结构完整的 Ref 封进 any
						typeDesc->SetValueErased(instance, property, std::any(newTexture));
					}
					break;
				}
				case DataType::Object:
				{
					ObjectDesc objectDesc = std::any_cast<ObjectDesc>(property.UserData);
					TypeDesc* objTypeDesc = TypeRegistry::Get().GetTypeDesc(objectDesc.Name);


					if (propNode.IsMap())
					{
						uint8_t* bytePtr = reinterpret_cast<uint8_t*>(instance) + property.Offset;
						void* childInstancePtr = reinterpret_cast<void*>(bytePtr);
						DeserializeProperties(propNode, objTypeDesc, childInstancePtr);
					}

					break;

				}


				default:
					break;
			}
			#undef DESERIALIZE_PROPERTY
		}
	}

	bool SceneSerializer::Deserialize(const std::string& filepath)
	{
		std::ifstream stream(filepath);
		if (!stream.is_open())
		{
			WLD_CORE_ERROR("Could not open file '{0}'", filepath);
			return false;
		}
		std::stringstream strstream;
		// 1. 将文件内容读入字符串流
		strstream << stream.rdbuf();

		YAML::Node data = YAML::Load(strstream.str());
		if (!data["Scene"])
		{
			WLD_CORE_ERROR("Could not find 'Scene' node in '{0}'", filepath);
			return false;
		}

		std::string sceneName = data["Scene"].as<std::string>();
		WLD_CORE_TRACE("Deserializing scene '{0}'", sceneName);

		auto entities = data["Entities"];
		if (entities)
		{
			for (auto entity : entities)
			{
				auto newEntity = Entity(m_Scene.get(), m_Scene->m_Registry.create());

				for (const auto& componentName : TypeRegistry::Get().GetTypesByCategory(TypeCategory::Component))
				{
					// Check if evaluating YAML entity has a sub-node with the name of this component.
					// We should not be adding a component to an entity if it does not actually have it saved in YAML.
					if (!entity[componentName])
						continue;

					TypeDesc* typeDesc = TypeRegistry::Get().GetTypeDesc(componentName);
					if (TypeDescDataComponent* componentInfo = std::any_cast<TypeDescDataComponent>(&typeDesc->UserData))
					{
						if (componentInfo->AddFunc)
						{
							// 1. 调用已经绑定的 AddFunc (内部执行了 newEntity.AddComponent<T>())
							// 这会激发 EnTT 根据具体的编译期类型 T 懒加载并创建出对应的 storage 内存池
							componentInfo->AddFunc(newEntity);

							// 2. 从实体上获取在 Registry 里刚刚生成的真正组件对象的地址
							void* rawPtr = newEntity.GetComponent(componentInfo->Id);

							if (rawPtr)
							{
								// 3. 将解析出的属性值直接反序列化到该真实地址的结构体成员上
								DeserializeProperties(entity[componentName], typeDesc, rawPtr);

								if (typeid(NativeScriptComponent).name() == typeDesc->Name)
								{
									TypeDesc* scriptTypeDesc = TypeRegistry::Get().GetTypeDesc(((NativeScriptComponent*)rawPtr)->ScriptName);
									if (TypeDescDataScript* scriptInfo = std::any_cast<TypeDescDataScript>(&scriptTypeDesc->UserData))
									{
										if (scriptInfo->BindFunc)
										{
											scriptInfo->BindFunc(*(NativeScriptComponent*)rawPtr);
										}
									}

								}

							}


						}
					}

				}
			}
		}


		return true;
	}
	void SceneSerializer::SerializeRuntime(const std::string& filepath)
	{

	}
	bool SceneSerializer::DeserializeRuntime(const std::string& filepath)
	{
		return false;
	}
}