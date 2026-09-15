#include "wldpch.h"
#include "SaveService.h"

#include "World/Schema/SchemaWriter.h"
#include "World/Scene/Components.h"
#include "World/Scene/Entity.h"
#include "World/Scene/Hierarchy.h"

#include <yaml-cpp/yaml.h>

#include <chrono>
#include <fstream>
#include <sstream>

namespace World::Gameplay
{
	namespace
	{
		constexpr const char* kTransformTypeName = "World::TransformComponent";

		std::string ReadFile(const std::filesystem::path& path, bool* ok)
		{
			std::ifstream stream(path, std::ios::binary);
			if (!stream)
			{
				*ok = false;
				return {};
			}
			std::ostringstream buffer;
			buffer << stream.rdbuf();
			*ok = true;
			return buffer.str();
		}
	}

	SaveService::SaveService(std::string projectId, SceneProvider scene)
		: SaveService(std::move(projectId), std::move(scene), {})
	{
	}

	SaveService::SaveService(std::string projectId, SceneProvider scene, std::filesystem::path userRoot)
		: m_ProjectId(std::move(projectId)), m_Scene(std::move(scene))
	{
		m_SaveTraits.insert(kTransformTypeName);

		if (userRoot.empty())
		{
			if (const char* overrideDir = std::getenv("WLD_SAVE_DIR"))
			{
				userRoot = overrideDir;   // 开发/自动化测试覆盖
			}
			else if (const char* localAppData = std::getenv("LOCALAPPDATA"))
			{
				userRoot = std::filesystem::path(localAppData) / m_ProjectId;
			}
			else
			{
				userRoot = std::filesystem::current_path() / "saves" / m_ProjectId;
			}
		}
		m_SaveRoot = userRoot / "saves";
	}

	std::filesystem::path SaveService::GetSlotPath(uint32_t slot) const
	{
		return m_SaveRoot / ("slot-" + std::to_string(slot) + ".wsave");
	}

	void SaveService::RegisterMigration(uint32_t fromVersion, MigrationFn fn)
	{
		m_Migrations[fromVersion] = std::move(fn);
	}

	void SaveService::RegisterSaveTrait(const std::string& schemaTypeName)
	{
		if (!schemaTypeName.empty())
			m_SaveTraits.insert(schemaTypeName);
	}

	bool SaveService::IsSaveTrait(const std::string& schemaTypeName) const
	{
		return m_SaveTraits.find(schemaTypeName) != m_SaveTraits.end();
	}

	void SaveService::SetGlobal(const std::string& key, GlobalValue value)
	{
		if (!key.empty())
			m_Globals[key] = std::move(value);
	}

	bool SaveService::TryGetGlobal(const std::string& key, GlobalValue* out) const
	{
		const auto it = m_Globals.find(key);
		if (it == m_Globals.end() || !out)
			return false;
		*out = it->second;
		return true;
	}

	// ---- 写档 ----

	bool SaveService::Save(uint32_t slot, const std::string& levelId)
	{
		m_LastError.clear();
		if (!WriteDocument(slot, levelId))
			return false;

		// 先写临时文件再原子替换:任何中途失败都不会破坏已有存档。
		std::error_code error;
		std::filesystem::create_directories(m_SaveRoot, error);
		if (error)
		{
			m_LastError = "cannot create save directory: " + error.message();
			return false;
		}
		return true;
	}

	bool SaveService::WriteDocument(uint32_t slot, const std::string& levelId)
	{
		Scene* scene = m_Scene ? m_Scene() : nullptr;
		if (!scene)
		{
			m_LastError = "no active scene";
			return false;
		}

		const uint64_t timestamp = static_cast<uint64_t>(
			std::chrono::duration_cast<std::chrono::seconds>(
				std::chrono::system_clock::now().time_since_epoch()).count());

		Schema::SchemaRegistry& schemas = scene->GetContext().Schemas();
		YAML::Emitter out;
		Schema::YamlSchemaWriter writer(out);

		out << YAML::BeginMap;
		out << YAML::Key << "SaveHeader" << YAML::Value << YAML::BeginMap
			<< YAML::Key << "ProjectId" << YAML::Value << m_ProjectId
			<< YAML::Key << "Version" << YAML::Value << CurrentVersion
			<< YAML::Key << "LevelId" << YAML::Value << levelId
			<< YAML::Key << "Timestamp" << YAML::Value << timestamp
			<< YAML::EndMap;

		// 全局块
		out << YAML::Key << "Globals" << YAML::Value << YAML::BeginMap;
		for (const auto& [key, value] : m_Globals)
		{
			// 带类型落盘:全局块的值类型必须显式保存,否则 YAML 标量往返后无法可靠还原 int/bool/string。
			out << YAML::Key << key << YAML::Value << YAML::BeginMap
				<< YAML::Key << "Type" << YAML::Value << static_cast<int>(value.Type)
				<< YAML::Key << "Value" << YAML::Value;
			switch (value.Type)
			{
				case GlobalValue::Kind::Int:    out << value.Int; break;
				case GlobalValue::Kind::Float:  out << value.Float; break;
				case GlobalValue::Kind::Bool:   out << value.Bool; break;
				case GlobalValue::Kind::String: out << value.String; break;
			}
			out << YAML::EndMap;
		}
		out << YAML::EndMap;

		// 场景块:实体(UUID 键)→ SaveTrait 白名单内的组件字段
		out << YAML::Key << "Entities" << YAML::Value << YAML::BeginSeq;
		// 只读遍历:SaveService 是 Scene 的 friend(见 Scene.h),直接用 registry 而不经
		// GetRegistry()(非 const 版本会触发"运行期结构写"断言,const 版本在 entt 3.x 不支持遍历)。
		entt::registry& registry = scene->m_Registry;
		auto& storage = registry.storage<entt::entity>();
		// 层级引用落成 UUID(entt 句柄跨会话无效):先建 handle → UUID 表。
		std::unordered_map<entt::entity, UUID> uuidByHandle;
		for (const entt::entity handle : storage)
			if (registry.all_of<UUIDComponent>(handle))
				uuidByHandle[handle] = registry.get<UUIDComponent>(handle).ID;
		for (const entt::entity handle : storage)
		{
			Entity entity { scene, handle };
			if (!entity || !entity.HasComponent<UUIDComponent>())
				continue;

			out << YAML::BeginMap;
			// UUID 以 uint64 落盘(与 .wd 的 schema 写法等价,避免依赖 yaml-cpp 的自定义转换)。
			out << YAML::Key << "UUID" << YAML::Value
				<< static_cast<uint64_t>(entity.GetComponent<UUIDComponent>().ID);
			for (const Schema::TypeSchema* schema : schemas.List(Schema::TypeCategory::Component))
			{
				if (!schema || !schema->Storage || !IsSaveTrait(schema->Id.Name))
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
			// 引用:层级(父子)按 UUID 落盘,读档时重建 —— 直接存 entt 句柄会指向错误实体。
			if (entity.HasComponent<HierarchyComponent>())
			{
				const HierarchyComponent& hierarchy = entity.GetComponent<HierarchyComponent>();
				out << YAML::Key << "Hierarchy" << YAML::Value << YAML::BeginMap;
				if (hierarchy.Parent != entt::null)
					if (const auto parent = uuidByHandle.find(hierarchy.Parent); parent != uuidByHandle.end())
						out << YAML::Key << "Parent" << YAML::Value << static_cast<uint64_t>(parent->second);
				out << YAML::Key << "Children" << YAML::Value << YAML::BeginSeq;
				for (const entt::entity child : hierarchy.Children)
					if (const auto found = uuidByHandle.find(child); found != uuidByHandle.end())
						out << static_cast<uint64_t>(found->second);
				out << YAML::EndSeq << YAML::EndMap;
			}
			out << YAML::EndMap;
		}
		out << YAML::EndSeq << YAML::EndMap;

		if (!out.good())
		{
			m_LastError = "failed to emit save document";
			return false;
		}

		std::error_code error;
		std::filesystem::create_directories(m_SaveRoot, error);
		if (error)
		{
			m_LastError = "cannot create save directory: " + error.message();
			return false;
		}

		const std::filesystem::path target = GetSlotPath(slot);
		const std::filesystem::path temporary = target.string() + ".tmp";
		{
			std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
			if (!stream)
			{
				m_LastError = "cannot open save file for writing: " + temporary.string();
				return false;
			}
			const std::string text = out.c_str();
			stream.write(text.data(), static_cast<std::streamsize>(text.size()));
			if (!stream.good())
			{
				m_LastError = "failed to write save file: " + temporary.string();
				return false;
			}
		}
		std::filesystem::rename(temporary, target, error);
		if (error)
		{
			std::filesystem::remove(temporary, error);
			m_LastError = "failed to replace save file: " + target.string();
			return false;
		}
		WLD_CORE_INFO("[save] wrote slot {} -> {} ({} entities)", slot, target.string(),
			static_cast<int>(storage.size()));
		return true;
	}

	// ---- 读档 ----

	bool SaveService::Load(uint32_t slot)
	{
		m_LastError.clear();
		return ApplyDocument(slot);
	}

	bool SaveService::ApplyDocument(uint32_t slot)
	{
		m_LastLoadReport = {};
		Scene* scene = m_Scene ? m_Scene() : nullptr;
		if (!scene)
		{
			m_LastError = "no active scene";
			return false;
		}

		bool readOk = false;
		const std::string text = ReadFile(GetSlotPath(slot), &readOk);
		if (!readOk)
		{
			m_LastError = "cannot read save slot " + std::to_string(slot);
			return false;
		}

		YAML::Node root;
		try
		{
			root = YAML::Load(text);
		}
		catch (const std::exception& exception)
		{
			m_LastError = std::string("corrupt save file: ") + exception.what();
			return false;
		}
		if (!root || !root.IsMap() || !root["SaveHeader"])
		{
			m_LastError = "corrupt save file: missing SaveHeader";
			return false;
		}

		uint32_t version = root["SaveHeader"]["Version"].as<uint32_t>(0);
		if (version > CurrentVersion)
		{
			m_LastError = "save was written by a newer version (" + std::to_string(version) + ")";
			return false;
		}
		for (uint32_t from = version; from < CurrentVersion; ++from)
		{
			const auto migration = m_Migrations.find(from);
			if (migration == m_Migrations.end())
			{
				m_LastError = "no migration registered from version " + std::to_string(from);
				return false;
			}
			if (!migration->second(from, &root))
			{
				m_LastError = "migration from version " + std::to_string(from) + " failed";
				return false;
			}
		}

		// 全局块
		if (const YAML::Node globals = root["Globals"]; globals && globals.IsMap())
		{
			for (const auto& entry : globals)
			{
				const std::string key = entry.first.as<std::string>();
				const YAML::Node node = entry.second;
				if (!node || !node.IsMap() || !node["Value"])
					continue;
				GlobalValue value;
				const int kind = node["Type"].as<int>(static_cast<int>(GlobalValue::Kind::String));
				value.Type = static_cast<GlobalValue::Kind>(kind);
				const YAML::Node scalar = node["Value"];
				switch (value.Type)
				{
					case GlobalValue::Kind::Int:    value.Int = scalar.as<int64_t>(); break;
					case GlobalValue::Kind::Float:  value.Float = scalar.as<double>(); break;
					case GlobalValue::Kind::Bool:   value.Bool = scalar.as<bool>(); break;
					case GlobalValue::Kind::String: value.String = scalar.as<std::string>(); break;
					default: continue;
				}
				m_Globals[key] = std::move(value);
			}
		}

		// 场景块:按 UUID 找实体 → 逐组件读字段(field id 由 schema 决定)
		std::unordered_map<UUID, Entity> byUuid;
		entt::registry& registry = scene->m_Registry;
		auto& storage = registry.storage<entt::entity>();
		for (const entt::entity handle : storage)
		{
			Entity entity { scene, handle };
			if (entity && entity.HasComponent<UUIDComponent>())
				byUuid[entity.GetComponent<UUIDComponent>().ID] = entity;
		}

		Schema::SchemaRegistry& schemas = scene->GetContext().Schemas();
		Schema::YamlSchemaReader reader;
		const YAML::Node entities = root["Entities"];
		if (entities && entities.IsSequence())
		{
			for (const YAML::Node entityNode : entities)
			{
				if (!entityNode["UUID"])
					continue;
				const UUID uuid(entityNode["UUID"].as<uint64_t>());
				const auto found = byUuid.find(uuid);
				Entity entity;
				if (found != byUuid.end())
				{
					entity = found->second;
					++m_LastLoadReport.EntitiesUpdated;
				}
				else
				{
					// 存档里有、当前场景没有:按存档重建实体(只含 SaveTrait 组件 —— 表现类组件由场景资产负责)。
					const entt::entity created = registry.create();
					registry.emplace<UUIDComponent>(created, uuid);
					entity = Entity(scene, created);
					byUuid[uuid] = entity;
					++m_LastLoadReport.EntitiesCreated;
				}
				for (const Schema::TypeSchema* schema : schemas.List(Schema::TypeCategory::Component))
				{
					if (!schema || !schema->Storage || !IsSaveTrait(schema->Id.Name))
						continue;
					const YAML::Node componentNode = entityNode[schema->Id.Name];
					if (!componentNode)
						continue;
					if (!entity.HasComponent(schema->Storage->ComponentId))
					{
						if (!schema->Storage->Add)
							continue;
						schema->Storage->Add(static_cast<void*>(&entity));
					}
					void* instance = entity.GetComponent(schema->Storage->ComponentId);
					if (!instance)
						continue;
					if (reader.ReadFields(*schema, instance, componentNode))
						++m_LastLoadReport.ComponentsApplied;
					else
					{
						++m_LastLoadReport.ComponentsFailed;
						WLD_CORE_WARN("[save] component '{}' on entity {} failed to load",
							schema->Id.Name, static_cast<uint64_t>(uuid));
					}
				}

				// 引用重建:层级父子按 UUID 解析回 entt 句柄;引用不到的目标计入报告(失效引用可检出)。
				if (const YAML::Node hierarchyNode = entityNode["Hierarchy"];
					hierarchyNode && hierarchyNode.IsMap())
				{
					const auto resolve = [&](const YAML::Node& uuidNode, entt::entity* out)
					{
						if (!uuidNode)
							return false;
						const UUID reference(uuidNode.as<uint64_t>());
						const auto it = byUuid.find(reference);
						if (it == byUuid.end())
							return false;
						*out = static_cast<entt::entity>(it->second);
						return true;
					};

					if (hierarchyNode["Parent"])
					{
						entt::entity parent = entt::null;
						if (resolve(hierarchyNode["Parent"], &parent) && parent != static_cast<entt::entity>(entity))
							Hierarchy::SetParent(registry, static_cast<entt::entity>(entity), parent);
						else if (parent == entt::null)
						{
							++m_LastLoadReport.ReferencesMissing;
							WLD_CORE_WARN("[save] hierarchy parent of entity {} is missing", static_cast<uint64_t>(uuid));
						}
					}
					const YAML::Node children = hierarchyNode["Children"];
					if (children && children.IsSequence())
					{
						for (const YAML::Node childNode : children)
						{
							entt::entity child = entt::null;
							if (resolve(childNode, &child))
								Hierarchy::SetParent(registry, child, static_cast<entt::entity>(entity));
							else
								++m_LastLoadReport.ReferencesMissing;
						}
					}
				}
			}
		}

		WLD_CORE_INFO("[save] loaded slot {} (version {})", slot, version);
		// 读档后必须重算缓存:TransformComponent 的缓存矩阵与层级世界矩阵不会自动刷新,
		// 否则数据虽然还原了,画面/拾取仍停在旧状态(实测"读取没效果")。
		for (const entt::entity handle : registry.view<TransformComponent>())
			registry.get<TransformComponent>(handle).RecalculateTransform();
		Hierarchy::UpdateWorldTransforms(registry);
		return true;
	}

	bool SaveService::Delete(uint32_t slot)
	{
		m_LastError.clear();
		std::error_code error;
		const bool removed = std::filesystem::remove(GetSlotPath(slot), error);
		if (error)
		{
			m_LastError = "failed to delete save: " + error.message();
			return false;
		}
		return removed;
	}

	std::vector<SaveSlotInfo> SaveService::ListSaves() const
	{
		std::vector<SaveSlotInfo> slots;
		std::error_code error;
		if (!std::filesystem::exists(m_SaveRoot, error))
			return slots;

		for (const std::filesystem::directory_entry& entry : std::filesystem::directory_iterator(m_SaveRoot, error))
		{
			if (error || !entry.is_regular_file())
				continue;
			const std::string filename = entry.path().filename().string();
			if (filename.rfind("slot-", 0) != 0 || entry.path().extension() != ".wsave")
				continue;

			SaveSlotInfo info;
			try
			{
				info.Slot = static_cast<uint32_t>(std::stoul(filename.substr(5)));
			}
			catch (const std::exception&)
			{
				continue;
			}

			bool readOk = false;
			const std::string text = ReadFile(entry.path(), &readOk);
			if (!readOk)
			{
				info.Error = "cannot read file";
				slots.push_back(std::move(info));
				continue;
			}
			try
			{
				const YAML::Node root = YAML::Load(text);   // 只读头,不整档应用
				const YAML::Node header = root["SaveHeader"];
				if (!header)
					throw std::runtime_error("missing SaveHeader");
				info.Header.ProjectId = header["ProjectId"].as<std::string>("");
				info.Header.Version = header["Version"].as<uint32_t>(0);
				info.Header.LevelId = header["LevelId"].as<std::string>("");
				info.Header.Timestamp = header["Timestamp"].as<uint64_t>(0);
				info.Valid = true;
			}
			catch (const std::exception& exception)
			{
				info.Error = exception.what();
			}
			slots.push_back(std::move(info));
		}

		std::sort(slots.begin(), slots.end(),
			[](const SaveSlotInfo& a, const SaveSlotInfo& b) { return a.Slot < b.Slot; });
		return slots;
	}
}
