#include "wldpch.h"
#include "SceneSerializer.h"

#include "World/Scene/Entity.h"
#include "World/Scene/Components.h"
#include "World/Core/UUID.h"
#include "World/Core/Cook/VFS.h"

#include <filesystem>
#include <fstream>
#include <unordered_set>
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

	static bool IsSupportedFieldType(DataType type)
	{
		switch (type)
		{
			case DataType::Bool:
			case DataType::Char:
			case DataType::Int8:
			case DataType::UInt8:
			case DataType::Int16:
			case DataType::UInt16:
			case DataType::Int32:
			case DataType::UInt32:
			case DataType::Int64:
			case DataType::UInt64:
			case DataType::Float:
			case DataType::Double:
			case DataType::Vec2:
			case DataType::Vec3:
			case DataType::Vec4:
			case DataType::Quat:
			case DataType::Mat3:
			case DataType::Mat4:
			case DataType::String:
			case DataType::Enum:
				return true;
			default:
				return false;
		}
	}

	static void SerializeFieldValue(YAML::Emitter& out, DataType type, const std::any& value, const std::any& userData)
	{
		switch (type)
		{
			case DataType::Bool: out << std::any_cast<bool>(value); break;
			case DataType::Char: out << std::any_cast<char>(value); break;
			case DataType::Int8: out << std::any_cast<int8_t>(value); break;
			case DataType::UInt8: out << std::any_cast<uint8_t>(value); break;
			case DataType::Int16: out << std::any_cast<int16_t>(value); break;
			case DataType::UInt16: out << std::any_cast<uint16_t>(value); break;
			case DataType::Int32: out << std::any_cast<int32_t>(value); break;
			case DataType::UInt32: out << std::any_cast<uint32_t>(value); break;
			case DataType::Int64: out << std::any_cast<int64_t>(value); break;
			case DataType::UInt64: out << std::any_cast<uint64_t>(value); break;
			case DataType::Float: out << std::any_cast<float>(value); break;
			case DataType::Double: out << std::any_cast<double>(value); break;
			case DataType::Vec2: out << std::any_cast<glm::vec2>(value); break;
			case DataType::Vec3: out << std::any_cast<glm::vec3>(value); break;
			case DataType::Vec4: out << std::any_cast<glm::vec4>(value); break;
			case DataType::Quat: out << std::any_cast<glm::quat>(value); break;
			case DataType::Mat3: out << std::any_cast<glm::mat3>(value); break;
			case DataType::Mat4: out << std::any_cast<glm::mat4>(value); break;
			case DataType::String: out << std::any_cast<std::string>(value); break;
			case DataType::Enum:
			{
				EnumDesc enumDesc = std::any_cast<EnumDesc>(userData);
				switch (enumDesc.UnderlyingSize)
				{
					case 1:
						if (enumDesc.IsSigned) out << (int32_t)std::any_cast<int8_t>(value);
						else                   out << (uint32_t)std::any_cast<uint8_t>(value);
						break;
					case 2:
						if (enumDesc.IsSigned) out << (int32_t)std::any_cast<int16_t>(value);
						else                   out << (uint32_t)std::any_cast<uint16_t>(value);
						break;
					case 4:
						if (enumDesc.IsSigned) out << std::any_cast<int32_t>(value);
						else                   out << std::any_cast<uint32_t>(value);
						break;
					case 8:
						if (enumDesc.IsSigned) out << std::any_cast<int64_t>(value);
						else                   out << std::any_cast<uint64_t>(value);
						break;
					default:
						out << 0;
						break;
				}
				break;
			}
			default:
				out << 0;
				break;
		}
	}

	static std::any DeserializeFieldValue(const YAML::Node& node, DataType type, const std::any& userData)
	{
		switch (type)
		{
			case DataType::Bool: return std::any(node.as<bool>());
			case DataType::Char: return std::any(node.as<char>());
			case DataType::Int8: return std::any(node.as<int8_t>());
			case DataType::UInt8: return std::any(node.as<uint8_t>());
			case DataType::Int16: return std::any(node.as<int16_t>());
			case DataType::UInt16: return std::any(node.as<uint16_t>());
			case DataType::Int32: return std::any(node.as<int32_t>());
			case DataType::UInt32: return std::any(node.as<uint32_t>());
			case DataType::Int64: return std::any(node.as<int64_t>());
			case DataType::UInt64: return std::any(node.as<uint64_t>());
			case DataType::Float: return std::any(node.as<float>());
			case DataType::Double: return std::any(node.as<double>());
			case DataType::Vec2: return std::any(node.as<glm::vec2>());
			case DataType::Vec3: return std::any(node.as<glm::vec3>());
			case DataType::Vec4: return std::any(node.as<glm::vec4>());
			case DataType::Quat: return std::any(node.as<glm::quat>());
			case DataType::Mat3: return std::any(node.as<glm::mat3>());
			case DataType::Mat4: return std::any(node.as<glm::mat4>());
			case DataType::String: return std::any(node.as<std::string>());
			case DataType::Enum:
			{
				EnumDesc enumDesc = std::any_cast<EnumDesc>(userData);
				switch (enumDesc.UnderlyingSize)
				{
					case 1:
						if (enumDesc.IsSigned) return std::any(node.as<int8_t>());
						return std::any(node.as<uint8_t>());
					case 2:
						if (enumDesc.IsSigned) return std::any(node.as<int16_t>());
						return std::any(node.as<uint16_t>());
					case 4:
						if (enumDesc.IsSigned) return std::any(node.as<int32_t>());
						return std::any(node.as<uint32_t>());
					case 8:
						if (enumDesc.IsSigned) return std::any(node.as<int64_t>());
						return std::any(node.as<uint64_t>());
					default:
						return std::any();
				}
			}
			default:
				return std::any();
		}
	}

	static void SerializeNativeScriptFieldValues(YAML::Emitter& out, NativeScriptComponent& script)
	{
		if (script.FieldValues.empty())
			return;
		const TypeDesc* scriptType = TypeRegistry::Get().GetTypeDesc(script.ScriptName);
		if (!scriptType)
			return;

		out << YAML::Key << "FieldValues" << YAML::Value << YAML::BeginMap;
		for (const auto& property : scriptType->Properties)
		{
			const auto it = script.FieldValues.find(property.Name);
			if (it == script.FieldValues.end() || !it->second.has_value())
				continue;
			if (!IsSupportedFieldType(property.Type))
				continue;
			out << YAML::Key << property.Name << YAML::Value;
			SerializeFieldValue(out, property.Type, it->second, property.UserData);
		}
		out << YAML::EndMap;
	}

	static void DeserializeNativeScriptFieldValues(const YAML::Node& node, NativeScriptComponent& script)
	{
		const YAML::Node fields = node["FieldValues"];
		if (!fields || !fields.IsMap())
			return;
		const TypeDesc* scriptType = TypeRegistry::Get().GetTypeDesc(script.ScriptName);
		if (!scriptType)
			return;
		for (const auto& property : scriptType->Properties)
		{
			const YAML::Node fieldNode = fields[property.Name];
			if (!fieldNode)
				continue;
			std::any value = DeserializeFieldValue(fieldNode, property.Type, property.UserData);
			if (value.has_value())
				script.FieldValues[property.Name] = std::move(value);
		}
	}

	static void SerializeLuaCachedFields(YAML::Emitter& out, LuaScriptComponent& script)
	{
		if (script.CachedFields.empty())
			return;
		out << YAML::Key << "CachedFields" << YAML::Value << YAML::BeginMap;
		for (const auto& [name, field] : script.CachedFields)
		{
			if (field.Type == LuaFieldType::None || !field.Value.has_value())
				continue;
			out << YAML::Key << name << YAML::Value << YAML::BeginMap;
			out << YAML::Key << "Type" << YAML::Value << static_cast<int>(field.Type);
			out << YAML::Key << "Value" << YAML::Value;
			switch (field.Type)
			{
				case LuaFieldType::Float: out << std::any_cast<float>(field.Value); break;
				case LuaFieldType::Int: out << std::any_cast<int>(field.Value); break;
				case LuaFieldType::Bool: out << std::any_cast<bool>(field.Value); break;
				case LuaFieldType::String: out << std::any_cast<std::string>(field.Value); break;
				default: break;
			}
			out << YAML::EndMap;
		}
		out << YAML::EndMap;
	}

	static void DeserializeLuaCachedFields(const YAML::Node& node, LuaScriptComponent& script)
	{
		const YAML::Node fields = node["CachedFields"];
		if (!fields || !fields.IsMap())
			return;
		for (const auto& kv : fields)
		{
			const std::string name = kv.first.as<std::string>();
			const YAML::Node typeNode = kv.second["Type"];
			const YAML::Node valueNode = kv.second["Value"];
			if (!typeNode || !valueNode)
				continue;

			LuaScriptField field;
			field.Type = static_cast<LuaFieldType>(typeNode.as<int>());
			switch (field.Type)
			{
				case LuaFieldType::Float: field.Value = valueNode.as<float>(); break;
				case LuaFieldType::Int: field.Value = valueNode.as<int>(); break;
				case LuaFieldType::Bool: field.Value = valueNode.as<bool>(); break;
				case LuaFieldType::String: field.Value = valueNode.as<std::string>(); break;
				default: continue;
			}
			script.CachedFields[name] = std::move(field);
		}
	}

	static void SerializeEntity(YAML::Emitter& out, Entity entity, const std::unordered_map<UUID, std::string>& unknownNodes)
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
					out << YAML::Key << typeDesc->TypeId << YAML::Value;
					out << YAML::BeginMap;

					void* componentInstance = entity.GetComponent(componentInfo->Id);
					SerializeProperties(out, typeDesc, componentInstance);

					if (typeDesc->TypeId == GetTypeFullName(typeid(NativeScriptComponent).name()))
						SerializeNativeScriptFieldValues(out, *static_cast<NativeScriptComponent*>(componentInstance));
					else if (typeDesc->TypeId == GetTypeFullName(typeid(LuaScriptComponent).name()))
						SerializeLuaCachedFields(out, *static_cast<LuaScriptComponent*>(componentInstance));

					out << YAML::EndMap;
				}
			}

		}

		const UUID entityUuid = entity.GetComponent<UUIDComponent>().ID;
		const auto unknownIt = unknownNodes.find(entityUuid);
		if (unknownIt != unknownNodes.end() && !unknownIt->second.empty())
		{
			try
			{
				const YAML::Node unknownMap = YAML::Load(unknownIt->second);
				for (const auto& kv : unknownMap)
					out << YAML::Key << kv.first.as<std::string>() << YAML::Value << kv.second;
			}
			catch (const std::exception& e)
			{
				WLD_CORE_WARN("Failed to re-emit preserved unknown component nodes: {0}", e.what());
			}
		}

		out << YAML::EndMap;
	}
	bool SceneSerializer::Serialize(const std::string& filepath)
	{
		YAML::Emitter out;
		out << YAML::BeginMap;
		{
			out << YAML::Key << "FormatVersion" << YAML::Value << 1;
			out << YAML::Key << "Scene" << YAML::Value << "Untitled";

			out << YAML::Key << "Entities" << YAML::Value << YAML::BeginSeq;

			// 使用反向迭代器遍历实体，以修正保存加载后的顺序颠倒问题
			auto& entities = m_Scene->m_Registry.storage<entt::entity>();
			std::unordered_set<UUID> emitted;

			for (auto it = entities.rbegin(); it != entities.rend(); ++it)
			{
				Entity ent { m_Scene.get(), *it };
				if (!ent)
				{
					return false;
				};

				SerializeEntity(out, ent, m_Scene->m_UnknownComponentNodes);
				if (ent.HasComponent<UUIDComponent>())
					emitted.insert(ent.GetComponent<UUIDComponent>().ID);
			}
			out << YAML::EndSeq;

			// 保留优先：若某实体携带了上次未识别的组件片段，但该实体已不存在，则阻止破坏性保存。
			for (const auto& [uuid, fragment] : m_Scene->m_UnknownComponentNodes)
			{
				if (emitted.find(uuid) == emitted.end())
				{
					m_LastError = "Cannot save scene: an entity with preserved unknown components no longer exists.";
					return false;
				}
			}
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
		return true;
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
		// 每次反序列化都重建“未知组件保留”集合，避免把上一个场景的未知片段带入本次保存。
		m_Scene->m_UnknownComponentNodes.clear();

		std::string yamlData;
		if (std::filesystem::exists(filepath))
		{
			std::ifstream stream(filepath);
			if (!stream.is_open())
			{
				WLD_CORE_ERROR("Could not open file '{0}'", filepath);
				m_LastError = "Could not open file '" + filepath + "'";
				return false;
			}
			std::stringstream strstream;
			strstream << stream.rdbuf();
			yamlData = strstream.str();
		}
		else
		{
			// Try to read from VFS
			auto data = VFS::ReadFile(filepath);
			if (data.empty())
			{
				WLD_CORE_ERROR("Could not load file '{0}' from disk or VFS", filepath);
				m_LastError = "Could not load file '" + filepath + "' from disk or VFS";
				return false;
			}
			yamlData = std::string(data.begin(), data.end());
		}

		YAML::Node data = YAML::Load(yamlData);
		if (data["FormatVersion"])
		{
			const int formatVersion = data["FormatVersion"].as<int>();
			(void)formatVersion; // 目前仅有 v1；缺失版本按 v1 兼容读取。
		}
		if (!data["Scene"])
		{
			WLD_CORE_ERROR("Could not find 'Scene' node in '{0}'", filepath);
			m_LastError = "Could not find 'Scene' node in '" + filepath + "'";
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

				// 收集未能识别的组件节点（保留优先，避免缺插件静默丢数据）。
				YAML::Node unknownMap(YAML::NodeType::Map);
				bool hasUnknown = false;
				for (const auto& kv : entity)
				{
					const std::string key = kv.first.as<std::string>();
					const TypeDesc* keyType = TypeRegistry::Get().GetTypeDesc(key);
					const bool known = keyType && std::any_cast<TypeDescDataComponent>(&keyType->UserData) != nullptr;
					if (!known)
					{
						unknownMap[key] = kv.second;
						hasUnknown = true;
					}
				}

				for (const auto& componentName : TypeRegistry::Get().GetTypesByCategory(TypeCategory::Component))
				{
					TypeDesc* typeDesc = TypeRegistry::Get().GetTypeDesc(componentName);
					if (!typeDesc)
						continue;

					// 兼容旧短名键：新文件用全名，旧文件用短名。
					YAML::Node compNode = entity[componentName];
					if (!compNode)
						compNode = entity[typeDesc->Name];
					if (!compNode)
						continue;

					if (TypeDescDataComponent* componentInfo = std::any_cast<TypeDescDataComponent>(&typeDesc->UserData))
					{
						if (componentInfo->AddFunc)
						{
							componentInfo->AddFunc(newEntity);
							void* rawPtr = newEntity.GetComponent(componentInfo->Id);
							if (rawPtr)
							{
								DeserializeProperties(compNode, typeDesc, rawPtr);

								if (typeDesc->TypeId == GetTypeFullName(typeid(NativeScriptComponent).name()))
								{
									NativeScriptComponent* nativeScript = static_cast<NativeScriptComponent*>(rawPtr);
									DeserializeNativeScriptFieldValues(compNode, *nativeScript);

									TypeDesc* scriptTypeDesc = TypeRegistry::Get().GetTypeDesc(nativeScript->ScriptName);
									if (scriptTypeDesc)
									{
										if (TypeDescDataScript* scriptInfo = std::any_cast<TypeDescDataScript>(&scriptTypeDesc->UserData))
										{
											if (scriptInfo->BindFunc)
											{
												scriptInfo->BindFunc(*nativeScript);
											}
										}
									}
									else
									{
										WLD_CORE_ERROR("Could not find script type '{0}' for NativeScriptComponent!", nativeScript->ScriptName);
									}
								}
								else if (typeDesc->TypeId == GetTypeFullName(typeid(LuaScriptComponent).name()))
								{
									LuaScriptComponent* luaScript = static_cast<LuaScriptComponent*>(rawPtr);
									DeserializeLuaCachedFields(compNode, *luaScript);
								}
							}
						}
					}
				}

				if (hasUnknown && newEntity.HasComponent<UUIDComponent>())
				{
					const UUID entityUuid = newEntity.GetComponent<UUIDComponent>().ID;
					m_Scene->m_UnknownComponentNodes[entityUuid] = YAML::Dump(unknownMap);
				}
			}
		}


		return true;
	}
	bool SceneSerializer::SerializeRuntime(const std::string& filepath)
	{
		return false;
	}
	bool SceneSerializer::DeserializeRuntime(const std::string& filepath)
	{
		return false;
	}
}
