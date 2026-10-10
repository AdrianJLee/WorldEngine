#include "wldpch.h"
#include "World/Scene/SceneSerializer.h"

#include "World/Scene/Entity.h"
#include "World/Scene/Components.h"
#include "World/Scene/Hierarchy.h"
#include "World/Script/Runtime/ScriptEngine.h"
#include "World/Core/UUID.h"
#include "World/Gameplay/Prefab/PrefabTypes.h"
#include "World/Gameplay/Prefab/Prefab.h"
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

		// Level 2.0:实体块里的保留键 —— `Prefab:` 是本读写层自己的实例来源块,
		// 不是"未注册组件",不能被当成未知片段原样留存。
		constexpr const char* kPrefabBlockKey = "Prefab";

		// 一条 prefab 实例记录 → `Path/Root/Overrides` map(盘上一律写实体 UUID:
		// entt 句柄跨会话无效)。返回 false = 该记录不可写(根实体已消失)。
		bool EmitPrefabInstanceRecord(YAML::Emitter& out, const entt::registry& registry,
			const Gameplay::PrefabInstanceRecord& record, bool includeReconstructionMap = false)
		{
			uint64_t rootUuid = 0;
			if (!TryGetEntityUuid(registry, record.Root, &rootUuid))
			{
				WLD_CORE_WARN("SceneSerializer: dropping prefab instance '{0}' — its root entity is gone",
					record.PrefabPath);
				return false;
			}

			out << YAML::BeginMap;
			out << YAML::Key << "Path" << YAML::Value << record.PrefabPath;
			out << YAML::Key << "Root" << YAML::Value << rootUuid;
			// 稀疏放置(§16.10):带上"模板先序 → 实体 UUID"映射。加载端据此**实例化模板**并
			// 把 UUID 逐个贴回去,因此未落盘的子实体也能原样重建。没有这一项 = 稠密形态:
			// 加载端只按读到的实体文档重建实例记录(今天的 `.wd` / 全量路径)。
			if (includeReconstructionMap)
			{
				std::vector<entt::entity> subtree;
				Gameplay::FlattenInstanceSubtree(registry, record.Root, subtree);
				if (!subtree.empty())
				{
					out << YAML::Key << "Entities" << YAML::Value << YAML::Flow << YAML::BeginSeq;
					for (const entt::entity handle : subtree)
					{
						uint64_t uuid = 0;
						if (TryGetEntityUuid(registry, handle, &uuid))
							out << uuid;
					}
					out << YAML::EndSeq;
				}
			}
			if (!record.Overrides.empty())
			{
				out << YAML::Key << "Overrides" << YAML::Value << YAML::BeginSeq;
				for (const auto& [handle, fields] : record.Overrides)
				{
					if (fields.empty())
						continue;
					uint64_t entityUuid = 0;
					if (!TryGetEntityUuid(registry, static_cast<entt::entity>(handle), &entityUuid))
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
			return true;
		}

		// Level 2.0 §16.10:`Prefab:` 块(模板引用 + 覆盖字段 + 稀疏重建映射)的**唯一**解析处。
		// 导入侧(CapturePreservedEntityData)与关卡加载侧(SceneSerializer::ReadPrefabBlock ——
		// 它要在建场景**之前**就知道该不该实例化模板)共用它,避免两份口径漂移。
		bool ParsePrefabBlockNode(const YAML::Node& prefabNode, uint64_t entityUuid,
			Gameplay::PendingPrefabInstanceRecord* out)
		{
			if (!out || !prefabNode || !prefabNode.IsMap() || !prefabNode["Root"])
			{
				if (prefabNode && entityUuid != 0)
					WLD_CORE_WARN("Level import: entity {0} has a malformed 'Prefab' block; ignored", entityUuid);
				return false;
			}

			out->PrefabPath = prefabNode["Path"] ? prefabNode["Path"].as<std::string>("") : std::string();
			out->RootUuid = prefabNode["Root"].as<uint64_t>(0);

			// 稀疏重建映射(可选):`Entities: [uuid, ...]` = 模板先序的实体 UUID。
			out->EntityUuids.clear();
			if (const YAML::Node ids = prefabNode["Entities"])
			{
				if (ids.IsSequence())
					for (const YAML::Node& id : ids)
						out->EntityUuids.push_back(id.as<uint64_t>(0));
				else
					WLD_CORE_WARN("Level import: entity {0} has non-sequence 'Prefab.Entities'; ignored", entityUuid);
			}

			out->Overrides.clear();
			if (const YAML::Node overrides = prefabNode["Overrides"])
			{
				if (!overrides.IsSequence())
				{
					WLD_CORE_WARN("Level import: entity {0} has non-sequence 'Prefab.Overrides'; ignored", entityUuid);
				}
				else
				{
					for (const YAML::Node& entry : overrides)
					{
						if (!entry.IsMap() || !entry["Entity"] || !entry["Fields"])
							continue;
						std::vector<std::string> fields;
						for (const YAML::Node& field : entry["Fields"])
							fields.push_back(field.as<std::string>());
						if (!fields.empty())
							out->Overrides.emplace_back(entry["Entity"].as<uint64_t>(0), std::move(fields));
					}
				}
			}
			return true;
		}
		// Level 2.0:实体块里的"保留数据"统一采集 —— 未注册组件片段 + `Prefab:` 实例块。
		// `.wd` 全场景读 与 `.entity` 单实体读 共用这一处;两处各写一份正是"编辑器能读、
		// 流式路径丢数据"这类偏差的成因。
		void CapturePreservedEntityData(Scene& scene, YAML::Node entityNode, uint64_t entityUuid)
		{
			if (entityUuid == 0 || !entityNode.IsMap())
				return;

			const Schema::SchemaRegistry& schemas = scene.GetContext().Schemas();

			YAML::Node unknownMap(YAML::NodeType::Map);
			bool hasUnknown = false;
			for (const auto& kv : entityNode)
			{
				const std::string key = kv.first.as<std::string>();
				if (key == kPrefabBlockKey)
					continue;
				const Schema::TypeSchema* keyType = schemas.Find(key);
				const bool known = keyType && keyType->Storage != nullptr;
				if (!known)
				{
					unknownMap[key] = kv.second;
					hasUnknown = true;
				}
			}
			if (hasUnknown)
				scene.SetUnknownComponentFragment(UUID(entityUuid), YAML::Dump(unknownMap));

			Gameplay::PendingPrefabInstanceRecord pending;
			if (ParsePrefabBlockNode(entityNode[kPrefabBlockKey], entityUuid, &pending))
				scene.AddPendingPrefabInstance(std::move(pending));
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


		void SerializeEntity(YAML::Emitter& out, Entity entity, const Schema::SchemaRegistry& schemas,
			const std::unordered_map<UUID, std::string>& unknownNodes, Schema::YamlSchemaWriter& writer,
			const Gameplay::PrefabInstanceRecord* prefabInstance = nullptr)
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
			// Level 2.0:实例根自己那份文档带 `Prefab:`(模板引用 + 覆盖字段),必须写在
			// **实体 map 里面** —— 关卡里的 prefab 实例因此能重开不断链(Revert / Unpack / 改覆盖)。
			if (prefabInstance && prefabInstance->Root == static_cast<entt::entity>(entity))
			{
				Scene* owner = entity.GetScene();
				if (owner)
				{
					out << YAML::Key << kPrefabBlockKey << YAML::Value;
					// 稀疏实例的实体文档要带上"模板先序 → UUID"映射(加载端据此重建子树)。
					EmitPrefabInstanceRecord(out, owner->GetRegistry(), *prefabInstance,
						prefabInstance->SparsePlacement);
				}
			}
			out << YAML::EndMap;
		}

		// Level 2.0:把**一个 YAML 实体块**应用到场景(建实体 + 逐组件 Add/ReadFields)。
		// 与 Deserialize 的内联循环同一条口径;抽出来给 .wd 与 `.entity` 两条读路径共用 ——
		// 两处各写一份序列化正是"编辑器能读、流式路径丢组件"这类偏差的成因。
		entt::entity ApplyEntityNode(Scene& scene, YAML::Node entity, const Schema::SchemaRegistry& schemas)
		{
			entt::registry& registry = scene.GetRegistry();
			const entt::entity handle = registry.create();
			Entity newEntity(&scene, handle);

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
			}
			return handle;
		}

		// 单个实体内嵌块 → 场景,并维护"已存在 UUID"集合(UUID 是存档/差分的唯一键,
		// 同一批里重复出现或与场景已有实体撞 UUID 都必须跳过)。
		// 返回新实体句柄;被跳过(重复 UUID / 非 map)时返回 entt::null。
		entt::entity ApplyEntityNodeIfNew(Scene& scene, YAML::Node node, const Schema::SchemaRegistry& schemas,
			std::unordered_set<uint64_t>& existing)
		{
			if (!node.IsMap())
				return entt::null;

			uint64_t uuid = 0;
			if (YAML::Node identity = node["World::UUIDComponent"])
				if (YAML::Node id = identity["ID"])
					if (YAML::Node raw = id["m_UUID"])
						uuid = raw.as<uint64_t>(0);
			if (uuid != 0 && existing.count(uuid) != 0)
				return entt::null;

			const entt::entity handle = ApplyEntityNode(scene, node, schemas);
			if (uuid != 0)
				existing.insert(uuid);
			return handle;
		}

		// Level 2.0:把一个"单实体文档"(`Entities: [ {...} ]`)里的实体并入场景。
		// 文件路径与内存文本两条入口共用它 —— 实体级读口径只有这一处。
		// 已存在同 UUID 的实体一律跳过:同一关卡目录被重复并入时不能造出同 UUID 双胞胎。
		size_t AppendEntityNodes(Scene& scene, YAML::Node document, std::string* error)
		{
			YAML::Node entities = document["Entities"];
			if (!entities || !entities.IsSequence())
			{
				if (error) *error = "entity document has no Entities sequence";
				return 0;
			}

			const Schema::SchemaRegistry& schemas = scene.GetContext().Schemas();
			entt::registry& registry = scene.GetRegistry();

			std::unordered_set<uint64_t> existing;
			for (const entt::entity handle : registry.view<UUIDComponent>())
				existing.insert(static_cast<uint64_t>(registry.get<UUIDComponent>(handle).ID));

			size_t applied = 0;
			for (YAML::Node node : entities)
			{
				const entt::entity handle = ApplyEntityNodeIfNew(scene, node, schemas, existing);
				if (handle == entt::null)
					continue;
				++applied;
				uint64_t entityUuid = 0;
				if (TryGetEntityUuid(registry, handle, &entityUuid))
					CapturePreservedEntityData(scene, node, entityUuid);
			}
			return applied;
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
					EmitPrefabInstanceRecord(out, m_Scene->m_Registry, record);
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
		m_Scene->m_PendingPrefabRecords.clear();
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
		// 单一版本(2026-10-04 起不做旧格式兼容):缺失或不为 2 一律拒绝,不猜、不迁移。
		const int formatVersion = data["FormatVersion"] ? data["FormatVersion"].as<int>() : 0;
		if (formatVersion != 2)
		{
			WLD_CORE_ERROR("Unsupported scene format version '{0}' in '{1}' (expected 2)", formatVersion, filepath);
			m_LastError = "Unsupported scene format version in '" + filepath + "' (expected 2)";
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
				const entt::entity appliedHandle = ApplyEntityNode(*m_Scene, entity, schemas);
				Entity newEntity(m_Scene.get(), appliedHandle);
				if (newEntity.HasComponent<UUIDComponent>())
					CapturePreservedEntityData(*m_Scene, entity,
						static_cast<uint64_t>(newEntity.GetComponent<UUIDComponent>().ID));
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
			registry.remove<HierarchyChildrenComponent>(entity);
		std::vector<entt::entity> childrenToLink;
		for (const auto entity : registry.view<HierarchyComponent>())
		{
			const auto& hierarchy = registry.get<HierarchyComponent>(entity);
			if (hierarchy.Parent != entt::null && registry.valid(hierarchy.Parent))
				childrenToLink.push_back(entity);
		}
		for (const entt::entity child : childrenToLink)
		{
			const entt::entity parent = registry.get<HierarchyComponent>(child).Parent;
			(void)registry.get_or_emplace<HierarchyComponent>(parent);
			Hierarchy::MutableChildren(registry, parent).push_back(child);
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

	// ---- Level 2.0(OFPA)单实体文档读写 ----
	//
	// `.entity` 与 `.wd` 的实体块**逐字段同形**(同一个 SerializeEntity / ApplyEntityNode),
	// 因此单实体文件既能被人肉审阅、在 Git 里逐行 diff,也能被同一条读路径无损还原 ——
	// 这是"一个实体一个文件、多人零冲突"成立的前提。

	bool SceneSerializer::SerializeEntityDocument(const Entity& entity, const std::filesystem::path& filepath,
		std::string* error)
	{
		if (!entity.IsValid())
		{
			if (error) *error = "entity is not valid";
			return false;
		}
		Scene* scene = entity.GetScene();
		if (!scene)
		{
			if (error) *error = "entity has no scene";
			return false;
		}

		try
		{
			std::string yaml;
			if (!SerializeEntityDocumentToYaml(entity, &yaml, error))
				return false;

			if (filepath.has_parent_path())
				std::filesystem::create_directories(filepath.parent_path());

			std::ofstream fout(filepath);
			if (!fout.is_open())
			{
				if (error) *error = "cannot open for write: " + filepath.string();
				return false;
			}
			fout << yaml;
			return true;
		}
		catch (const std::exception& e)
		{
			if (error) *error = e.what();
			return false;
		}
	}

	bool SceneSerializer::SerializeEntityDocumentToYaml(const Entity& entity, std::string* outYaml,
		std::string* error)
	{
		if (!outYaml)
		{
			if (error) *error = "no output buffer";
			return false;
		}
		if (!entity.IsValid())
		{
			if (error) *error = "entity is not valid";
			return false;
		}
		Scene* scene = entity.GetScene();
		if (!scene)
		{
			if (error) *error = "entity has no scene";
			return false;
		}

		try
		{
			YAML::Emitter out;
			out << YAML::BeginMap;
			out << YAML::Key << "Entities" << YAML::Value << YAML::BeginSeq;
			{
				Schema::YamlSchemaWriter writer(out);
				const Gameplay::PrefabInstanceRecord* prefabInstance = nullptr;
				if (const Gameplay::PrefabInstanceRecord* candidate = scene->FindPrefabInstance(entity))
					if (candidate->Root == static_cast<entt::entity>(entity))
						prefabInstance = candidate;
				SerializeEntity(out, entity, scene->GetContext().Schemas(),
					scene->m_UnknownComponentNodes, writer, prefabInstance);
			}
			out << YAML::EndSeq;
			out << YAML::EndMap;
			*outYaml = out.c_str();
			return true;
		}
		catch (const std::exception& e)
		{
			if (error) *error = e.what();
			return false;
		}
	}

	size_t SceneSerializer::AppendEntityDocuments(Scene& scene, const std::vector<std::string>& documents,
		const std::vector<uint8_t>* skip, std::vector<Entity>* outApplied, std::string* error)
	{
		const Schema::SchemaRegistry& schemas = scene.GetContext().Schemas();
		entt::registry& registry = scene.GetRegistry();

		std::unordered_set<uint64_t> existing;
		for (const entt::entity handle : registry.view<UUIDComponent>())
			existing.insert(static_cast<uint64_t>(registry.get<UUIDComponent>(handle).ID));

		size_t applied = 0;
		YAML::Node document;
		for (size_t i = 0; i < documents.size(); ++i)
		{
			if (skip && i < skip->size() && (*skip)[i] != 0)
				continue;
			if (documents[i].empty())
				continue;

			try
			{
				document = YAML::Load(documents[i]);
			}
			catch (const std::exception& e)
			{
				if (error) *error = e.what();
				continue;
			}

			YAML::Node entities = document["Entities"];
			if (!entities || !entities.IsSequence())
				continue;

			for (YAML::Node node : entities)
			{
				const entt::entity handle = ApplyEntityNodeIfNew(scene, node, schemas, existing);
				if (handle == entt::null)
					continue;
				++applied;
				if (outApplied)
					outApplied->emplace_back(&scene, handle);
				uint64_t entityUuid = 0;
				if (TryGetEntityUuid(registry, handle, &entityUuid))
					CapturePreservedEntityData(scene, node, entityUuid);
			}
		}
		return applied;
	}

	void SceneSerializer::FinalizeEntityImport(Scene& scene)
	{
		entt::registry& registry = scene.GetRegistry();

		// 反序列化只写 Location/Rotation/Scale 原始字段,不触发缓存矩阵重算
		// (schema 生成的 setter 直接赋值)。这里统一重算一次,否则所有实体(含相机)
		// 会停在单位矩阵 —— 表现为"相机在物体内部 / 物体全部堆在原点"。
		for (const entt::entity handle : registry.view<TransformComponent>())
			registry.get<TransformComponent>(handle).RecalculateTransform();

		// 层级:Parent 已随字段读入,Children 不入库,这里按 Parent 重建反向索引。
		for (const entt::entity handle : registry.view<HierarchyComponent>())
			registry.remove<HierarchyChildrenComponent>(handle);
		std::vector<entt::entity> childrenToLink;
		for (const entt::entity handle : registry.view<HierarchyComponent>())
		{
			const auto& hierarchy = registry.get<HierarchyComponent>(handle);
			if (hierarchy.Parent != entt::null && registry.valid(hierarchy.Parent))
				childrenToLink.push_back(handle);
		}
		for (const entt::entity child : childrenToLink)
		{
			const entt::entity parent = registry.get<HierarchyComponent>(child).Parent;
			(void)registry.get_or_emplace<HierarchyComponent>(parent);
			Hierarchy::MutableChildren(registry, parent).push_back(child);
		}
		Hierarchy::UpdateWorldTransforms(registry);

		// Level 2.0:实体级 `.entity` 里记的 `Prefab:` 实例块(按 UUID 记账)—— 全部实体
		// 就位后反查句柄登记成实例记录。找不到的记录/条目丢弃并警告(与 `.wd` 的
		// `Prefabs:` 块同口径),不让一个悬空引用毁掉整个关卡加载。
		if (!scene.m_PendingPrefabRecords.empty())
		{
			std::unordered_map<UUID, entt::entity> handlesByUuid;
			for (const entt::entity handle : registry.view<UUIDComponent>())
				handlesByUuid.emplace(registry.get<UUIDComponent>(handle).ID, handle);

			for (const Gameplay::PendingPrefabInstanceRecord& pending : scene.m_PendingPrefabRecords)
			{
				const auto rootHandle = handlesByUuid.find(UUID(pending.RootUuid));
				if (rootHandle == handlesByUuid.end() || !registry.valid(rootHandle->second))
				{
					WLD_CORE_WARN("Level import: prefab instance references unknown root UUID {0}; dropped",
						pending.RootUuid);
					continue;
				}
				Gameplay::PrefabInstanceRecord& record =
					scene.AddPrefabInstance(pending.PrefabPath, rootHandle->second);
				for (const auto& [entityUuid, fields] : pending.Overrides)
				{
					const auto entityHandle = handlesByUuid.find(UUID(entityUuid));
					if (entityHandle == handlesByUuid.end() || !registry.valid(entityHandle->second))
					{
						WLD_CORE_WARN("Level import: prefab override references unknown entity UUID {0}; dropped",
							entityUuid);
						continue;
					}
					record.Overrides[static_cast<uint32_t>(entityHandle->second)] = fields;
				}
			}
			scene.m_PendingPrefabRecords.clear();
		}
	}

	size_t SceneSerializer::AppendEntityDocument(Scene& scene, const std::filesystem::path& filepath,
		std::string* error)
	{
		if (!std::filesystem::is_regular_file(filepath))
		{
			if (error) *error = "entity document not found: " + filepath.string();
			return 0;
		}

		YAML::Node data;
		try
		{
			data = YAML::LoadFile(filepath.string());
		}
		catch (const std::exception& e)
		{
			if (error) *error = e.what();
			return 0;
		}

		return AppendEntityNodes(scene, data, error);
	}

	size_t SceneSerializer::AppendEntityYaml(Scene& scene, const std::string& documentYaml, std::string* error)
	{
		YAML::Node data;
		try
		{
			data = YAML::Load(documentYaml);
		}
		catch (const std::exception& e)
		{
			if (error) *error = e.what();
			return 0;
		}
		return AppendEntityNodes(scene, data, error);
	}

	// ---- Level 2.0 §16.10:稀疏放置的合并读路径 ----
	//
	// 稀疏形态的关卡里,实例的一部分实体**没有**自己的 `.entity`(它们由"实例化模板"补齐)。
	// 因此加载顺序是:① 先在场景里重建实例子树(带盘上 UUID);② 再把落盘的实体文档**覆盖**上去。
	// ② 必须能覆盖已存在的 UUID —— `AppendEntityDocuments` 的"同 UUID 跳过"是流送/密集包的
	// 正确语义(重复并入不能造双胞胎),这里要的是相反语义,所以单独一条实现而不是改既有口径。
	size_t SceneSerializer::MergeEntityDocuments(Scene& scene, const std::vector<std::string>& documents,
		bool overwriteExisting, std::string* error)
	{
		const Schema::SchemaRegistry& schemas = scene.GetContext().Schemas();
		entt::registry& registry = scene.GetRegistry();

		std::unordered_map<uint64_t, entt::entity> handlesByUuid;
		for (const entt::entity handle : registry.view<UUIDComponent>())
			handlesByUuid.emplace(static_cast<uint64_t>(registry.get<UUIDComponent>(handle).ID), handle);

		std::unordered_set<uint64_t> existing;
		for (const auto& [uuid, handle] : handlesByUuid)
		{
			(void)handle;
			existing.insert(uuid);
		}

		size_t applied = 0;
		for (const std::string& documentYaml : documents)
		{
			if (documentYaml.empty())
				continue;
			YAML::Node document;
			try
			{
				document = YAML::Load(documentYaml);
			}
			catch (const std::exception& e)
			{
				if (error) *error = e.what();
				continue;
			}

			YAML::Node entities = document["Entities"];
			if (!entities || !entities.IsSequence())
				continue;

			for (YAML::Node node : entities)
			{
				if (!node.IsMap())
					continue;

				uint64_t uuid = 0;
				if (YAML::Node identity = node["World::UUIDComponent"])
					if (YAML::Node id = identity["ID"])
						if (YAML::Node raw = id["m_UUID"])
							uuid = raw.as<uint64_t>(0);

				const auto target = uuid != 0 ? handlesByUuid.find(uuid) : handlesByUuid.end();
				if (overwriteExisting && target != handlesByUuid.end() && registry.valid(target->second))
				{
					// 覆盖:先把文档读进一个临时实体(与新建同一条读路径,规避 schema Add 的
					// emplace 语义),再按组件 Copy 到既有实体(Copy = AddOrReplace)。
					const entt::entity staged = ApplyEntityNode(scene, node, schemas);
					Entity stagedEntity(&scene, staged);
					Entity targetEntity(&scene, target->second);
					for (const Schema::TypeSchema* schema : schemas.List(Schema::TypeCategory::Component))
					{
						if (!schema || !schema->Storage || !schema->Storage->Add)
							continue;
						const entt::id_type componentId = schema->Storage->ComponentId;
						if (!stagedEntity.HasComponent(componentId))
							continue;
						if (schema->Storage->Copy)
							schema->Storage->Copy(static_cast<void*>(&targetEntity),
								static_cast<void*>(&stagedEntity));
						else if (!targetEntity.HasComponent(componentId))
							schema->Storage->Add(static_cast<void*>(&targetEntity));
					}
					registry.destroy(staged);
					CapturePreservedEntityData(scene, node, uuid);
					++applied;
					continue;
				}

				const entt::entity handle = ApplyEntityNodeIfNew(scene, node, schemas, existing);
				if (handle == entt::null)
					continue;
				++applied;
				if (uuid != 0)
					handlesByUuid.emplace(uuid, handle);
				uint64_t appliedUuid = 0;
				if (TryGetEntityUuid(registry, handle, &appliedUuid))
					CapturePreservedEntityData(scene, node, appliedUuid);
			}
		}
		return applied;
	}

	bool SceneSerializer::ReadPrefabBlock(const std::string& documentYaml,
		Gameplay::PendingPrefabInstanceRecord* out)
	{
		if (!out || documentYaml.empty())
			return false;
		YAML::Node document;
		try
		{
			document = YAML::Load(documentYaml);
		}
		catch (const std::exception&)
		{
			return false;
		}
		YAML::Node entities = document["Entities"];
		if (!entities || !entities.IsSequence() || entities.size() == 0)
			return false;
		YAML::Node entity = entities[0];
		if (!entity || !entity.IsMap())
			return false;
		return ParsePrefabBlockNode(entity[kPrefabBlockKey], 0, out);
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
