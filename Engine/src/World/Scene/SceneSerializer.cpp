#include "wldpch.h"
#include "SceneSerializer.h"

#include "World/Scene/Entity.h"
#include "World/Scene/Components.h"
#include "World/Scene/Hierarchy.h"
#include "World/Scene/ScriptEngine.h"
#include "World/Core/UUID.h"
#include "World/Gameplay/PrefabTypes.h"
#include "World/Script/ScriptProperties.h"
#include "World/Schema/SchemaWriter.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <limits>
#include <sstream>
#include <unordered_set>
#include <yaml-cpp/yaml.h>

namespace World
{
	namespace
	{
		constexpr const char* kNativeScriptType = "World::CppScriptComponent";
		constexpr const char* kLuaScriptType = "World::LuauScriptComponent";

		bool IsLeafKind(Schema::Kind kind)
		{
			return kind != Schema::Kind::Object && kind != Schema::Kind::Asset && kind != Schema::Kind::None;
		}

		// P4-U13b:存档里的实体身份一律是 UUIDComponent 的 UUID,不是 entt 句柄
		// (句柄带注册表状态,跨会话无意义)。句柄无效/没有 UUIDComponent → false。
		bool TryGetEntityUuid(const entt::registry& registry, entt::entity entity, uint64_t* outUuid)
		{
			if (entity == entt::null || !registry.valid(entity))
				return false;
			const auto* identity = registry.try_get<UUIDComponent>(entity);
			if (!identity)
				return false;
			if (outUuid)
				*outUuid = static_cast<uint64_t>(identity->ID);
			return true;
		}

		// 2026-09-26 重写:两个脚本组件的属性表**同一份 YAML 形态**(顺序 = 声明顺序):
		//   Properties:
		//     - { Name: Health, Type: Float, Value: 100 }
		// Type 写类型名(可读、可手改);值按 Kind 走 schema 的读写器,和普通字段同一条口径。
		// B 期:一条脚本属性(递归)。叶子写 `Type: Float|Vec3|…` + `Value: <标量/向量>`;
		// 结构化表写 `Type: Object` + `TypeName: <类名>` + `Value:` 下递归的同形条目
		// (裸 table / ReadOnly 的只写 Name+Type,值不落盘 —— 与"看得到、不进存档"同口径)。
		// C 期:数组/映射写 `Type: Array|Map` + `ElementType`(映射另有 `KeyType`/`ValueType`)——
		// 元素/值是叶子时 `Value` 是与组件字段同形的 flow seq/map(`Value: [12, 13]` / `Value: { hp: 10 }`),
		// 元素/值本身是集合(嵌套 `{{number}}`)时 `Value` 是子条目 seq(每项递归同形,自带 Name/Type)。
		// ReadOnly(裸 table / 元素类型不支持 / 推断失败)整条不写 Value。
		// D 期(2026-09-27):默认值/未设的行**不进场景** ——
		//   * 叶子:Value 未设或 == 声明默认值 → 整条不写(加载时按脚本默认值走,脚本改了老场景跟着变);
		//   * 结构化表:只写被记录过的子行(形状由声明提供);
		//   * 数组/映射:形状以场景为准(ShapeFromScene)→ 整条写,未设的行写 `~`
		//     (元素个数/键名保住,"未设"语义也读得回来)。
		void WriteScriptPropertyItem(YAML::Emitter& out, const ScriptProperty& property,
			Schema::YamlSchemaWriter& writer, bool shapeRow);

		// D1(2026-09-27):这条属性要不要写进场景?
		//   * 只读摘要(裸 table / 降级)—— 写 Name+Type(+TypeName) 便于手改与工具读,但不写 Value;
		//   * 场景记录过的行(值 != 声明默认值;容器 = 任一行被记录)→ 写;
		//   * 场景记录过形状的数组/映射(ShapeFromScene,读档置位)→ 即使所有行都回到"未设"
		//     也要写(元素个数/键名以场景为准);全未设的行写 `~`,脚本改默认值后照样跟着走。
		bool ShouldWriteScriptProperty(const ScriptProperty& property)
		{
			if (property.ReadOnly)
				return true;
			if (ScriptProperties::IsSceneRecorded(property))
				return true;
			return property.Collection != ScriptPropertyCollection::None && property.ShapeFromScene;
		}

		void WriteCollectionValue(YAML::Emitter& out, const ScriptProperty& property,
			Schema::YamlSchemaWriter& writer)
		{
			static const Schema::Value kUnset {};
			if (property.ElementKind == Schema::Kind::Object)
			{
				// 嵌套集合:子条目是完整属性(含各自的集合形态),递归同形才能读回。
				// 场景形状以场景为准 → 每一行都要写(未设的行写 `Value: ~`,见 ShouldWrite 的 shapeRow)。
				out << YAML::BeginSeq;
				for (const ScriptProperty& child : property.Children)
					WriteScriptPropertyItem(out, child, writer, /*shapeRow=*/true);
				out << YAML::EndSeq;
				return;
			}
			Schema::FieldSchema probe;
			probe.K = property.ElementKind;
			if (property.Collection == ScriptPropertyCollection::Array)
			{
				out << YAML::Flow << YAML::BeginSeq;
				for (const ScriptProperty& child : property.Children)
					// D1:未设的行写 `~`(不是材料化出来的脚本默认值 —— 默认值不进场景)。
					writer.WriteValue(probe,
						ScriptProperties::IsSceneRecorded(child) ? child.Value : kUnset);
				out << YAML::EndSeq;
				return;
			}
			out << YAML::Flow << YAML::BeginMap;
			for (const ScriptProperty& child : property.Children)
			{
				out << YAML::Key << child.Name << YAML::Value;
				writer.WriteValue(probe,
					ScriptProperties::IsSceneRecorded(child) ? child.Value : kUnset);
			}
			out << YAML::EndMap;
		}

		void WriteScriptPropertyItem(YAML::Emitter& out, const ScriptProperty& property,
			Schema::YamlSchemaWriter& writer, bool shapeRow)
		{
			const bool recorded = ScriptProperties::IsSceneRecorded(property);
			if (!property.ReadOnly && !recorded && !shapeRow && !property.ShapeFromScene)
				return;   // D1:未设 → 整条不写(加载时按脚本/场景默认值走)
			out << YAML::BeginMap;
			out << YAML::Key << "Name" << YAML::Value << property.Name;
			const bool array = property.Collection == ScriptPropertyCollection::Array;
			const bool map = property.Collection == ScriptPropertyCollection::Map;
			out << YAML::Key << "Type" << YAML::Value
				<< (array ? "Array" : (map ? "Map" : ScriptProperties::KindName(property.Type)));
			// CPPT-2:Enum / Asset 行的 TypeName 必写 —— Enum 读回要用它找枚举 schema(判断有无符号),
			// Asset 的 TypeName 是资产类型名(面板据此选下拉)。
			// CPPT-6:容器行同理 —— 元素是 Enum/Asset 时 TypeName 也必写(数组里每一行只是值,
			// 元素类型名只能挂在容器行上)。
			const bool containerTypeName = property.Collection != ScriptPropertyCollection::None
				&& (property.ElementKind == Schema::Kind::Enum || property.ElementKind == Schema::Kind::Asset);
			const bool needsTypeName = property.Collection == ScriptPropertyCollection::Struct
				|| property.Type == Schema::Kind::Enum || property.Type == Schema::Kind::Asset
				|| containerTypeName;
			if (needsTypeName && !property.TypeName.empty())
				out << YAML::Key << "TypeName" << YAML::Value << property.TypeName;
			if (array || map)
			{
				if (property.KeyKind != Schema::Kind::None && map)
					out << YAML::Key << "KeyType" << YAML::Value << ScriptProperties::KindName(property.KeyKind);
				if (property.ElementKind != Schema::Kind::None)
					out << YAML::Key << (map ? "ValueType" : "ElementType") << YAML::Value
						<< ScriptProperties::KindName(property.ElementKind);
				if (!property.ReadOnly)
				{
					out << YAML::Key << "Value" << YAML::Value;
					WriteCollectionValue(out, property, writer);
				}
			}
			else if (property.Type == Schema::Kind::Object)
			{
				// 结构化表:形状由声明提供 → 只写场景记录过的子行(未设的子字段不落盘)。
				if (!property.ReadOnly)
				{
					out << YAML::Key << "Value" << YAML::Value << YAML::BeginSeq;
					for (const ScriptProperty& child : property.Children)
						WriteScriptPropertyItem(out, child, writer, /*shapeRow=*/false);
					out << YAML::EndSeq;
				}
			}
			else
			{
				out << YAML::Key << "Value" << YAML::Value;
				Schema::FieldSchema probe;
				probe.K = property.Type;
				// 容器行里的叶子(shapeRow)未设时同样写 `~`,保持"未设"语义可读回。
				static const Schema::Value kUnset {};
				writer.WriteValue(probe, recorded ? property.Value : kUnset);
			}
			out << YAML::EndMap;
		}

		void SerializeScriptProperties(YAML::Emitter& out, const std::vector<ScriptProperty>& properties,
			Schema::YamlSchemaWriter& writer)
		{
			// D1:全表都是"未设"时不写 Properties 段(复位后场景里就该看不到这些字段)。
			const bool anyWritable = std::any_of(properties.begin(), properties.end(),
				[](const ScriptProperty& property)
				{
					return ScriptProperties::IsPropertyKind(property.Type)
						&& ShouldWriteScriptProperty(property);
				});
			if (!anyWritable)
				return;
			out << YAML::Key << "Properties" << YAML::Value << YAML::BeginSeq;
			for (const ScriptProperty& property : properties)
			{
				if (!ScriptProperties::IsPropertyKind(property.Type))
					continue;
				WriteScriptPropertyItem(out, property, writer, /*shapeRow=*/false);
			}
			out << YAML::EndSeq;
		}

		// 读档侧的护栏:集合/结构化表的递归深度与模型同量级(超出的子条目丢弃并降级只读)。
		constexpr int kMaxSerializedPropertyDepth = 8;

		// CPPT-2(F-6):Schema::FieldSchema::GetEnum 是无捕获函数指针,而读回时要按行的 TypeName
		// 现查枚举 schema(判断有符号/无符号)。用线程局部指针把当前行的枚举喂给
		// Schema::YamlSchemaReader 的既有 Enum 分支,不复制 kind 读写逻辑(读档是单线程路径)。
		thread_local const Schema::EnumSchema* s_ReadbackEnum = nullptr;
		const Schema::EnumSchema* ReadbackEnumSchema() { return s_ReadbackEnum; }

		bool ReadScriptPropertyItem(const YAML::Node& item, Schema::YamlSchemaReader& reader,
			ScriptProperty& property, int depth, const Schema::SchemaRegistry* schemas)
		{
			if (!item || !item.IsMap() || !item["Name"])
				return false;
			property = ScriptProperty {};
			property.Name = item["Name"].as<std::string>();
			const std::string typeName = item["Type"] ? item["Type"].as<std::string>() : std::string();
			// C 期:集合(数组/映射)。叶子元素/值是 flow seq/map;嵌套集合是子条目 seq。
			if (typeName == "Array" || typeName == "Map")
			{
				const bool map = typeName == "Map";
				property.Type = Schema::Kind::Object;
				property.Collection = map ? ScriptPropertyCollection::Map : ScriptPropertyCollection::Array;
				if (depth >= kMaxSerializedPropertyDepth)
				{
					property.ReadOnly = true;   // 超护栏:只留名字与类型,不假装能编辑
					return true;
				}
				if (item["TypeName"])
					property.TypeName = item["TypeName"].as<std::string>();
				const char* elementKey = map ? "ValueType" : "ElementType";
				if (item[elementKey])
					property.ElementKind = ScriptProperties::KindFromName(item[elementKey].as<std::string>());
				if (map)
					property.KeyKind = item["KeyType"]
						? ScriptProperties::KindFromName(item["KeyType"].as<std::string>())
						: Schema::Kind::String;
				const YAML::Node value = item["Value"];
				if (!value || property.ElementKind == Schema::Kind::None)
				{
					// 没有 Value(裸 table / 只读摘要)或没有元素类型(读不出来)→ 只读摘要。
					property.ReadOnly = true;
					return true;
				}
				property.ReadOnly = false;
				// D2:场景里真的写了这个容器(含空 seq/map)→ 形状以场景为准(合并时保留元素个数/键名)。
				property.ShapeFromScene = value.IsSequence() || value.IsMap();
				Schema::FieldSchema probe;
				probe.K = property.ElementKind;
				// CPPT-6:元素是 Enum 时按容器行的 TypeName 现查枚举 schema(F-6 同一条通道),
				// 否则叶读写器读不出整数(行会被丢掉)。thread_local 每次都重设:
				// 嵌套递归(Object 元素)结束时会把它清空。
				const Schema::EnumSchema* elementEnum = nullptr;
				if (property.ElementKind == Schema::Kind::Enum && schemas && !property.TypeName.empty())
				{
					elementEnum = schemas->FindEnum(property.TypeName);
					if (elementEnum)
						probe.GetEnum = &ReadbackEnumSchema;
				}
				if (property.ElementKind == Schema::Kind::Object)
				{
					if (value.IsSequence())
					{
						for (const YAML::Node& childNode : value)
						{
							ScriptProperty child;
							if (!ReadScriptPropertyItem(childNode, reader, child, depth + 1, schemas))
								continue;
							// CPPT-6-FIX2:元素行的形状由**父容器 + 行位置**决定,不是独立的结构化表字段。
							// ReadScriptPropertyItem 对 `Type: Object` 一律读成 Struct(旧存档/嵌套 struct
							// 字段都靠它),但容器的元素行必须回到面板写盘时的同一形状
							// (Collection=None + Type=Object + Children)——否则 ElementValueOf 会把它折成
							// ValueList,Play 时 WriteStructValue 整条失败(字段回落成员初值)。
							// 元素本身是容器(嵌套 `{{…}}`)时不在这里改写:那些行读回就是 Array/Map。
							if (child.Collection == ScriptPropertyCollection::Struct)
								child.Collection = ScriptPropertyCollection::None;
							property.Children.push_back(std::move(child));
						}
					}
				}
				else if (map)
				{
					if (value.IsMap())
					{
						for (const auto& entry : value)
						{
							ScriptProperty child;
							child.Name = entry.first.as<std::string>();
							child.Type = property.ElementKind;
								// D1:未设的行写 `~` —— 保留这一行(键名/位置是形状的一部分),值保持未设。
								if (!entry.second || entry.second.IsNull())
									property.Children.push_back(std::move(child));
								else
								{
									s_ReadbackEnum = elementEnum;
									if (reader.ReadFieldValue(probe, &child.Value, entry.second))
										property.Children.push_back(std::move(child));
								}
						}
					}
				}
				else if (value.IsSequence())
				{
					std::size_t index = 0;
					for (const YAML::Node& element : value)
					{
						ScriptProperty child;
						child.Name = std::to_string(++index);   // 数组行名 = 下标字符串(1 起)
						child.Type = property.ElementKind;
							// D1:未设的行写 `~` —— 保留这一行(下标是形状的一部分),值保持未设。
							if (!element || element.IsNull())
								property.Children.push_back(std::move(child));
							else
							{
								s_ReadbackEnum = elementEnum;
								if (reader.ReadFieldValue(probe, &child.Value, element))
									property.Children.push_back(std::move(child));
							}
					}
				}
				s_ReadbackEnum = nullptr;
				return true;
			}
			property.Type = ScriptProperties::KindFromName(typeName);
			if (!ScriptProperties::IsPropertyKind(property.Type))
				return false;
			if (item["TypeName"])
				property.TypeName = item["TypeName"].as<std::string>();
			if (property.Type == Schema::Kind::Object)
			{
				property.Collection = ScriptPropertyCollection::Struct;   // 旧存档的 Object 一律按结构化表读回
				if (depth >= kMaxSerializedPropertyDepth)
				{
					property.ReadOnly = true;
					return true;
				}
				const YAML::Node value = item["Value"];
				if (value && value.IsSequence())
				{
					for (const YAML::Node& childNode : value)
					{
						ScriptProperty child;
						if (ReadScriptPropertyItem(childNode, reader, child, depth + 1, schemas))
							property.Children.push_back(std::move(child));
					}
				}
				// 存档里没有子字段(裸 table / 只读摘要)→ 保持只读,不假装它有可编辑子行。
				property.ReadOnly = property.Children.empty();
				return true;
			}
			Schema::FieldSchema probe;
			probe.K = property.Type;
			Schema::Value value;
			if (property.Type == Schema::Kind::Enum && schemas && !property.TypeName.empty())
			{
				s_ReadbackEnum = schemas->FindEnum(property.TypeName);
				if (s_ReadbackEnum)
					probe.GetEnum = &ReadbackEnumSchema;
			}
			if (item["Value"] && reader.ReadFieldValue(probe, &value, item["Value"]))
				property.Value = std::move(value);
			s_ReadbackEnum = nullptr;
			return true;
		}

		void DeserializeScriptProperties(const YAML::Node& node, std::vector<ScriptProperty>& properties,
			const Schema::SchemaRegistry* schemas)
		{
			properties.clear();
			const YAML::Node list = node["Properties"];
			if (!list || !list.IsSequence())
				return;
			Schema::YamlSchemaReader reader;
			for (const YAML::Node& item : list)
			{
				ScriptProperty property;
				if (ReadScriptPropertyItem(item, reader, property, 0, schemas))
					properties.push_back(std::move(property));
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
					SerializeScriptProperties(out, static_cast<CppScriptComponent*>(instance)->Properties, writer);
				else if (schema->Id.Name == kLuaScriptType)
					SerializeScriptProperties(out, static_cast<LuauScriptComponent*>(instance)->Properties, writer);
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
			// P4-U4:场景级设置。默认值**不写**(旧文件形态不变);旧引擎读新文件会忽略这个块。
			{
				const Scene::WorldSettings& world = m_Scene->GetWorldSettings();
				if (!world.IsDefault())
				{
					out << YAML::Key << "World" << YAML::Value << YAML::BeginMap;
					if (world.HasGravityOverride())
						out << YAML::Key << "physics_gravity" << YAML::Value << world.Gravity;
					if (world.PhysicsDebug)
						out << YAML::Key << "physics_debug" << YAML::Value << true;
					out << YAML::EndMap;
				}
			}
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

			// P4-U13b:Prefab 实例注册表(来源路径 + 覆盖字段)→ `Prefabs:` 块。
			// 只在该块非空时写,空场景/默认场景的 .wd 形态逐字节不变。
			// Root/Entity 一律写实体 UUID:entt 句柄跨会话无效,加载时按 UUID 反查句柄重建。
			const std::vector<Gameplay::PrefabInstanceRecord>& prefabs = m_Scene->PrefabInstances();
			if (!prefabs.empty())
			{
				out << YAML::Key << "Prefabs" << YAML::Value << YAML::BeginSeq;
				for (const Gameplay::PrefabInstanceRecord& record : prefabs)
				{
					uint64_t rootUuid = 0;
					if (!TryGetEntityUuid(m_Scene->m_Registry, record.Root, &rootUuid))
					{
						WLD_CORE_WARN("SceneSerializer: dropping prefab instance '{0}' — its root entity is gone",
							record.PrefabPath);
						continue;
					}

					out << YAML::BeginMap;
					out << YAML::Key << "Path" << YAML::Value << record.PrefabPath;
					out << YAML::Key << "Root" << YAML::Value << rootUuid;
					if (!record.Overrides.empty())
					{
						out << YAML::Key << "Overrides" << YAML::Value << YAML::BeginSeq;
						for (const auto& [handle, fields] : record.Overrides)
						{
							if (fields.empty())
								continue;
							uint64_t entityUuid = 0;
							if (!TryGetEntityUuid(m_Scene->m_Registry, static_cast<entt::entity>(handle), &entityUuid))
							{
								WLD_CORE_WARN("SceneSerializer: dropping an override entry of '{0}' — its entity is gone",
									record.PrefabPath);
								continue;
							}
							out << YAML::BeginMap;
							out << YAML::Key << "Entity" << YAML::Value << entityUuid;
							out << YAML::Key << "Fields" << YAML::Value << YAML::Flow << YAML::BeginSeq;
							for (const std::string& field : fields)
								out << field;
							out << YAML::EndSeq;
							out << YAML::EndMap;
						}
						out << YAML::EndSeq;
					}
					out << YAML::EndMap;
				}
				out << YAML::EndSeq;
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
		// P4-U13b:同理,prefab 实例注册表本次读档重建(旧集合不能残留)。
		m_Scene->m_PrefabInstances.clear();

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

		// P4-U4:场景级设置(缺块 = 全默认)。非法值不静默吞:记 warning 后按"未设置"处理。
		m_Scene->GetWorldSettings() = Scene::WorldSettings {};
		if (const YAML::Node world = data["World"])
		{
			if (!world.IsMap())
			{
				WLD_CORE_WARN("Scene 'World' must be a map in '{0}'; ignored", filepath);
			}
			else
			{
				Scene::WorldSettings& settings = m_Scene->GetWorldSettings();
				if (world["physics_gravity"])
				{
					const float gravity = world["physics_gravity"].as<float>(
						std::numeric_limits<float>::quiet_NaN());
					if (std::isfinite(gravity))
						settings.Gravity = gravity;
					else
						WLD_CORE_WARN("Scene 'World.physics_gravity' must be finite in '{0}'; using project gravity",
							filepath);
				}
				if (world["physics_debug"])
					settings.PhysicsDebug = world["physics_debug"].as<bool>(false);
			}
		}

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
						CppScriptComponent* nativeScript = static_cast<CppScriptComponent*>(rawInstance);
						DeserializeScriptProperties(compNode, nativeScript->Properties, &schemas);
						// 脚本类型已知时以 schema 为准补齐/裁剪属性表(声明顺序 = 显示顺序)。
						const Schema::TypeSchema* scriptType = schemas.Find(nativeScript->ScriptName);
						if (scriptType && scriptType->Category == Schema::TypeCategory::Script)
							ScriptProperties::SyncFromSchema(nativeScript->Properties, *scriptType);
						else if (!nativeScript->ScriptName.empty())
							WLD_CORE_WARN("C++ script is not registered (its properties are kept as data): {0}",
								nativeScript->ScriptName);
					}
					else if (schema->Id.Name == kLuaScriptType)
					{
						// Luau 的字段声明来自脚本文件,这里先读回场景里保存的值。
						LuauScriptComponent* luaScript = static_cast<LuauScriptComponent*>(rawInstance);
						DeserializeScriptProperties(compNode, luaScript->Properties, &schemas);
						// D1(2026-09-27 用户口径「复位 = 未设;场景里不写默认值,**加载时取脚本默认值**」):
						// 读档后立刻按声明同步一次 —— 未设字段在这里材料化出脚本默认值(只用于展示/Play
						// 兜底,`Value == 默认值` 的判定仍让它们不落盘),场景记录过的值与集合形状由合并
						// 保留。没有这一步,面板侧"声明签名未变就跳过同步"的门(VEC-C2)会让**同一进程里
						// 第二次打开同一个脚本**的场景丢掉属性行(未记录任何值时)或显示类型零值。
						// VM 未初始化(纯工具宿主)时不做:那时声明只有注解、没有默认值,同步会丢推断字段。
						if (ScriptEngine::IsInitialized())
							ScriptEngine::SyncScriptDeclarations(*luaScript, nullptr, nullptr);
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

		// P4-U13b:Prefab 实例注册表。必须等实体全部建完、层级重建之后再做:盘上只有 UUID,
		// 这里反查句柄重建 record.Root 与 Overrides 的键。UUID 找不到的记录/条目丢弃并警告
		// (旧引擎不认识该块、或手工编辑过的场景不因此加载失败)。
		if (const YAML::Node prefabs = data["Prefabs"])
		{
			if (!prefabs.IsSequence())
			{
				WLD_CORE_WARN("Scene 'Prefabs' must be a sequence in '{0}'; ignored", filepath);
			}
			else
			{
				std::unordered_map<UUID, entt::entity> handlesByUuid;
				for (const auto entity : registry.view<UUIDComponent>())
					handlesByUuid.emplace(registry.get<UUIDComponent>(entity).ID, entity);

				for (const YAML::Node& node : prefabs)
				{
					if (!node.IsMap() || !node["Root"])
					{
						WLD_CORE_WARN("Scene prefab instance without 'Root' in '{0}'; dropped", filepath);
						continue;
					}
					const uint64_t rootUuid = node["Root"].as<uint64_t>(0);
					const auto rootHandle = handlesByUuid.find(UUID(rootUuid));
					if (rootHandle == handlesByUuid.end() || !registry.valid(rootHandle->second))
					{
						WLD_CORE_WARN("Scene prefab instance references unknown root UUID {0} in '{1}'; dropped",
							rootUuid, filepath);
						continue;
					}

					Gameplay::PrefabInstanceRecord& record = m_Scene->AddPrefabInstance(
						node["Path"] ? node["Path"].as<std::string>("") : std::string(), rootHandle->second);

					const YAML::Node overrides = node["Overrides"];
					if (!overrides || !overrides.IsSequence())
						continue;
					for (const YAML::Node& entry : overrides)
					{
						if (!entry.IsMap() || !entry["Entity"] || !entry["Fields"])
							continue;
						const uint64_t entityUuid = entry["Entity"].as<uint64_t>(0);
						const auto entityHandle = handlesByUuid.find(UUID(entityUuid));
						if (entityHandle == handlesByUuid.end() || !registry.valid(entityHandle->second))
						{
							WLD_CORE_WARN("Scene prefab override references unknown entity UUID {0} in '{1}'; dropped",
								entityUuid, filepath);
							continue;
						}
						std::vector<std::string> fields;
						for (const YAML::Node& field : entry["Fields"])
							fields.push_back(field.as<std::string>());
						if (!fields.empty())
							record.Overrides[static_cast<uint32_t>(entityHandle->second)] = std::move(fields);
					}
				}
			}
		}

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
