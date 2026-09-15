#include "wldpch.h"
#include "SceneSerializer.h"

#include "World/Scene/Entity.h"
#include "World/Scene/Components.h"
#include "World/Scene/Hierarchy.h"
#include "World/Core/UUID.h"
#include "World/Schema/SchemaWriter.h"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <unordered_set>
#include <yaml-cpp/yaml.h>

namespace World
{
	namespace
	{
		constexpr const char* kNativeScriptType = "World::NativeScriptComponent";
		constexpr const char* kLuaScriptType = "World::LuaScriptComponent";

		bool IsLeafKind(Schema::Kind kind)
		{
			return kind != Schema::Kind::Object && kind != Schema::Kind::Asset && kind != Schema::Kind::None;
		}

		void SerializeNativeScriptFieldValues(YAML::Emitter& out, NativeScriptComponent& script,
			const Schema::SchemaRegistry& schemas, Schema::YamlSchemaWriter& writer)
		{
			if (script.FieldValues.empty())
				return;
			const Schema::TypeSchema* scriptType = schemas.Find(script.ScriptName);
			if (!scriptType)
				return;

			out << YAML::Key << "FieldValues" << YAML::Value << YAML::BeginMap;
			for (const Schema::FieldSchema& field : scriptType->Fields)
			{
				if (field.Meta.Transient || !IsLeafKind(field.K))
					continue;
				const auto it = script.FieldValues.find(field.Name);
				if (it == script.FieldValues.end())
					continue;
				out << YAML::Key << field.Name << YAML::Value;
				writer.WriteValue(field, it->second);
			}
			out << YAML::EndMap;
		}

		void DeserializeNativeScriptFieldValues(const YAML::Node& node, NativeScriptComponent& script,
			const Schema::SchemaRegistry& schemas, Schema::YamlSchemaReader& reader)
		{
			const YAML::Node fields = node["FieldValues"];
			if (!fields || !fields.IsMap())
				return;
			const Schema::TypeSchema* scriptType = schemas.Find(script.ScriptName);
			if (!scriptType)
				return;
			for (const Schema::FieldSchema& field : scriptType->Fields)
			{
				if (field.Meta.Transient || !IsLeafKind(field.K))
					continue;
				const YAML::Node fieldNode = fields[field.Name];
				if (!fieldNode)
					continue;
				Schema::Value value;
				if (reader.ReadFieldValue(field, &value, fieldNode))
					script.FieldValues[field.Name] = std::move(value);
			}
		}

		void SerializeLuaCachedFields(YAML::Emitter& out, LuaScriptComponent& script)
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

		void DeserializeLuaCachedFields(const YAML::Node& node, LuaScriptComponent& script)
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

		void SerializeEntity(YAML::Emitter& out, Entity entity, const Schema::SchemaRegistry& schemas,
			const std::unordered_map<UUID, std::string>& unknownNodes, Schema::YamlSchemaWriter& writer)
		{
			WLD_ASSERT(entity.HasComponent<UUIDComponent>(), "Entity does not have a UUIDComponent!");

			out << YAML::BeginMap;
			for (const Schema::TypeSchema* schema : schemas.List(Schema::TypeCategory::Component))
			{
				if (!schema || !schema->Storage)
					continue;
				if (!entity.HasComponent(schema->Storage->ComponentId))
					continue;

				void* instance = entity.GetComponent(schema->Storage->ComponentId);
				if (!instance)
					continue;

				out << YAML::Key << schema->Id.Name << YAML::Value;
				writer.BeginType(*schema, instance);
				for (const Schema::FieldSchema& field : schema->Fields)
					writer.WriteField(field, instance);

				if (schema->Id.Name == kNativeScriptType)
					SerializeNativeScriptFieldValues(out, *static_cast<NativeScriptComponent*>(instance), schemas, writer);
				else if (schema->Id.Name == kLuaScriptType)
					SerializeLuaCachedFields(out, *static_cast<LuaScriptComponent*>(instance));
				writer.EndType(*schema);
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
	}

	SceneSerializer::SceneSerializer(const Ref<Scene>& scene)
		: m_Scene(scene)
	{
	}

	bool SceneSerializer::Serialize(const std::string& filepath)
	{
		YAML::Emitter out;
		Schema::YamlSchemaWriter writer(out);
		out << YAML::BeginMap;
		{
			out << YAML::Key << "FormatVersion" << YAML::Value << 2;
			out << YAML::Key << "Scene" << YAML::Value << "Untitled";
			out << YAML::Key << "Entities" << YAML::Value << YAML::BeginSeq;

			auto& entities = m_Scene->m_Registry.storage<entt::entity>();
			std::unordered_set<UUID> emitted;
			for (auto it = entities.rbegin(); it != entities.rend(); ++it)
			{
				Entity ent { m_Scene.get(), *it };
				if (!ent)
					return false;
				SerializeEntity(out, ent, m_Scene->GetContext().Schemas(), m_Scene->m_UnknownComponentNodes, writer);
				if (ent.HasComponent<UUIDComponent>())
					emitted.insert(ent.GetComponent<UUIDComponent>().ID);
			}
			out << YAML::EndSeq;

			// 保留优先:未知组件片段对应的实体若已不存在,阻止破坏性保存。
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
			std::filesystem::path path(filepath);
			std::filesystem::path parentPath = path.parent_path();
			if (!parentPath.empty() && !std::filesystem::exists(parentPath))
				std::filesystem::create_directories(parentPath);

			const std::string tempPath = path.string() + ".tmp";
			{
				std::ofstream fout(tempPath, std::ios::binary | std::ios::trunc);
				if (!fout.is_open())
				{
					m_LastError = "Could not open temporary file for writing: " + tempPath;
					return false;
				}
				fout << out.c_str();
				fout.flush();
				if (!fout)
				{
					fout.close();
					std::error_code ignored;
					std::filesystem::remove(tempPath, ignored);
					m_LastError = "Failed to write scene data to: " + tempPath;
					return false;
				}
				fout.close();
			}

			const std::string backupPath = path.string() + ".bak";
			const bool hadExisting = std::filesystem::exists(path);
			if (hadExisting)
			{
				std::error_code ec;
				if (std::filesystem::exists(backupPath))
					std::filesystem::remove(backupPath, ec);
				ec.clear();
				std::filesystem::rename(path, backupPath, ec);
				if (ec)
				{
					std::error_code ignored;
					std::filesystem::remove(tempPath, ignored);
					m_LastError = "Failed to back up existing scene file: " + ec.message();
					return false;
				}
			}

			{
				std::error_code ec;
				std::filesystem::rename(tempPath, path, ec);
				if (ec)
				{
					if (hadExisting)
					{
						std::error_code ignored;
						std::filesystem::rename(backupPath, path, ignored);
					}
					m_LastError = "Failed to replace scene file: " + ec.message();
					return false;
				}
			}

			if (hadExisting)
			{
				std::error_code ignored;
				std::filesystem::remove(backupPath, ignored);
			}
		}
		return true;
	}

	bool SceneSerializer::Deserialize(const std::string& filepath)
	{
		// 每次反序列化都重建"未知组件保留"集合,避免把上一个场景的未知片段带入本次保存。
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
			// VFS 2.0:目录/包 provider 统一解析,带来源与错误码。
			std::vector<uint8_t> data;
			std::error_code vfsEc;
			m_Scene->GetContext().Vfs().Read(filepath, data, vfsEc);
			if (data.empty())
			{
				WLD_CORE_ERROR("Could not load file '{0}' from VFS", filepath);
				m_LastError = "Could not load file '" + filepath + "' from VFS";
				return false;
			}
			yamlData = std::string(data.begin(), data.end());
		}

		YAML::Node data = YAML::Load(yamlData);
		const int formatVersion = data["FormatVersion"] ? data["FormatVersion"].as<int>() : 1;
		if (formatVersion < 1 || formatVersion > 2)
		{
			WLD_CORE_ERROR("Unsupported scene format version '{0}' in '{1}'", formatVersion, filepath);
			m_LastError = "Unsupported scene format version in '" + filepath + "'";
			return false;
		}
		if (!data["Scene"])
		{
			WLD_CORE_ERROR("Could not find 'Scene' node in '{0}'", filepath);
			m_LastError = "Could not find 'Scene' node in '" + filepath + "'";
			return false;
		}

		const std::string sceneName = data["Scene"].as<std::string>();
		WLD_CORE_TRACE("Deserializing scene '{0}'", sceneName);
		Schema::SchemaRegistry& schemas = m_Scene->GetContext().Schemas();

		auto entities = data["Entities"];
		if (entities)
		{
			for (auto entity : entities)
			{
				auto newEntity = Entity(m_Scene.get(), m_Scene->m_Registry.create());

				YAML::Node unknownMap(YAML::NodeType::Map);
				bool hasUnknown = false;
				for (const auto& kv : entity)
				{
					const std::string key = kv.first.as<std::string>();
					const Schema::TypeSchema* keyType = schemas.Find(key);
					const bool known = keyType && keyType->Storage != nullptr;
					if (!known)
					{
						unknownMap[key] = kv.second;
						hasUnknown = true;
					}
				}

				Schema::YamlSchemaReader reader;
				for (const Schema::TypeSchema* schema : schemas.List(Schema::TypeCategory::Component))
				{
					if (!schema || !schema->Storage || !schema->Storage->Add)
						continue;
					YAML::Node compNode = entity[schema->Id.Name];
					if (!compNode)
					{
						const size_t separator = schema->Id.Name.rfind("::");
						if (separator != std::string::npos)
							compNode = entity[schema->Id.Name.substr(separator + 2)];
					}
					if (!compNode)
						continue;

					schema->Storage->Add(static_cast<void*>(&newEntity));
					void* rawInstance = newEntity.GetComponent(schema->Storage->ComponentId);
					if (!rawInstance)
						continue;
					reader.ReadFields(*schema, rawInstance, compNode);

					if (schema->Id.Name == kNativeScriptType)
					{
						NativeScriptComponent* nativeScript = static_cast<NativeScriptComponent*>(rawInstance);
						DeserializeNativeScriptFieldValues(compNode, *nativeScript, schemas, reader);
						const Schema::TypeSchema* scriptType = schemas.Find(nativeScript->ScriptName);
						if (scriptType && scriptType->Script)
							scriptType->Script->Bind(static_cast<void*>(nativeScript));
						else if (!nativeScript->ScriptName.empty())
							WLD_CORE_ERROR("Could not find script type '{0}' for NativeScriptComponent!", nativeScript->ScriptName);
					}
					else if (schema->Id.Name == kLuaScriptType)
					{
						DeserializeLuaCachedFields(compNode, *static_cast<LuaScriptComponent*>(rawInstance));
					}
				}

				if (hasUnknown && newEntity.HasComponent<UUIDComponent>())
				{
					const UUID entityUuid = newEntity.GetComponent<UUIDComponent>().ID;
					m_Scene->m_UnknownComponentNodes[entityUuid] = YAML::Dump(unknownMap);
				}
			}
		}

		// 反序列化只写入 Location/Rotation/Scale 原始字段,不会触发缓存矩阵重算
		// (schema 生成的 setter 直接赋值)。这里统一重算一次,否则所有实体(含相机)
		// 会停在单位矩阵——表现为"相机在物体内部/物体全部堆在原点"。
		auto& registry = m_Scene->m_Registry;
		for (const auto entity : registry.view<TransformComponent>())
			registry.get<TransformComponent>(entity).RecalculateTransform();

		// 层级:Parent 已随字段读入,Children 不入库,这里按 Parent 重建反向索引;
		// 之后求解一次世界矩阵(P2a W3b)。
		for (const auto entity : registry.view<HierarchyComponent>())
		{
			auto& hierarchy = registry.get<HierarchyComponent>(entity);
			hierarchy.Children.clear();
		}
		std::vector<entt::entity> childrenToLink;
		for (const auto entity : registry.view<HierarchyComponent>())
		{
			const auto& hierarchy = registry.get<HierarchyComponent>(entity);
			if (hierarchy.Parent != entt::null && registry.valid(hierarchy.Parent))
				childrenToLink.push_back(entity);
		}
		for (const entt::entity child : childrenToLink)
		{
			auto& parentHierarchy = registry.get_or_emplace<HierarchyComponent>(
				registry.get<HierarchyComponent>(child).Parent);
			parentHierarchy.Children.push_back(child);
		}
		Hierarchy::UpdateWorldTransforms(registry);

		return true;
	}

	bool SceneSerializer::SerializeRuntime(const std::string&)
	{
		return false;
	}

	bool SceneSerializer::DeserializeRuntime(const std::string&)
	{
		return false;
	}
}
