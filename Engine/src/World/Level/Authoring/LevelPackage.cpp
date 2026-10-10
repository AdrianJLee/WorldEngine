#include "wldpch.h"
#include "World/Level/Authoring/LevelPackage.h"

#include "World/Core/Log.h"
#include "World/Core/WorldContext.h"
#include "World/Renderer/EditorCamera.h"
#include "World/Scene/Components.h"
#include "World/Scene/Entity.h"
#include "World/Scene/Scene.h"
#include "World/Scene/SceneSerializer.h"
#include "World/Gameplay/Prefab/Prefab.h"

#include <fstream>
#include <set>
#include <unordered_map>
#include <unordered_set>
#include <yaml-cpp/yaml.h>

namespace World::Level
{
	namespace
	{
		constexpr const char* kEntitiesDirName = "entities";

		// 先序形状签名:每个实体在**本列表内**的父下标(根 / 父在列表外 ⇒ -1)。
		//
		// 为什么单独要一层形状校验:逐实体比对剔除了 `HierarchyComponent`(Parent 是每个场景
		// 各自编号的 entt 句柄),于是"实例内部被拖动/重挂"这类**只改结构**的改动不会体现在
		// 指纹上。形状签名把它挡下来 —— 不一致 ⇒ 走稠密写,不让稀疏形态悄悄丢掉结构改动。
		std::string SubtreeShape(const entt::registry& registry, const std::vector<entt::entity>& order)
		{
			std::unordered_map<entt::entity, size_t> indexOf;
			indexOf.reserve(order.size());
			for (size_t index = 0; index < order.size(); ++index)
				indexOf.emplace(order[index], index);

			std::string shape;
			shape.reserve(order.size() * 4);
			for (const entt::entity handle : order)
			{
				long long parent = -1;
				if (const auto* hierarchy = registry.try_get<HierarchyComponent>(handle))
				{
					if (hierarchy->Parent != entt::null)
					{
						const auto found = indexOf.find(hierarchy->Parent);
						if (found != indexOf.end())
							parent = static_cast<long long>(found->second);
					}
				}
				shape += std::to_string(parent);
				shape += ',';
			}
			return shape;
		}
		// 稀疏放置(§16.10)的逐实体比对键:实体文档里**除这几项之外**的全部组件。
		//
		// 剔除的理由(每一项都是"同构的两棵树必然不同"的字段,不剔除会让稀疏永不成立):
		//   * `World::UUIDComponent` —— 重建时 UUID 由"模板先序 → UUID"映射贴回,而模板暂存
		//     场景里的 UUID 是新生成的;
		//   * `Prefab` —— 实例根自己的引用块,只活在实时实体上;
		//   * `World::HierarchyComponent` —— Parent 存的是 **entt 句柄**(schema 里是 UInt64),
		//     每个场景各自编号,暂存场景与关卡场景必然不同。结构由"形状签名"单独把关(见下),
		//     所以这里剔除它是安全的。
		std::string ComponentFingerprint(const std::string& documentYaml)
		{
			if (documentYaml.empty())
				return {};
			try
			{
				YAML::Node document = YAML::Load(documentYaml);
				YAML::Node entities = document["Entities"];
				if (!entities || !entities.IsSequence() || entities.size() == 0)
					return documentYaml;
				YAML::Node entity = entities[0];
				if (!entity || !entity.IsMap())
					return documentYaml;

				YAML::Emitter out;
				out << YAML::BeginMap;
				for (const auto& kv : entity)
				{
					const std::string key = kv.first.as<std::string>();
					if (key == "World::UUIDComponent" || key == "Prefab" || key == "World::HierarchyComponent")
						continue;
					out << YAML::Key << key << YAML::Value << kv.second;
				}
				out << YAML::EndMap;
				return out.c_str();
			}
			catch (const std::exception&)
			{
				return documentYaml;
			}
		}
		constexpr const char* kEntityExtension = ".entity";
	}

	struct LevelPackage::Impl
	{
		std::string Id;
		std::string Name;
		uint32_t GridCellSize{ 64 };
		LevelWorldSettings World;
		std::vector<DataLayerDefinition> DataLayers;
		std::vector<EntityDescriptor> Entities;
	};

	LevelPackage::LevelPackage()
		: m_Impl(std::make_unique<Impl>())
	{
	}

	LevelPackage::~LevelPackage() = default;
	LevelPackage::LevelPackage(LevelPackage&& other) noexcept = default;
	LevelPackage& LevelPackage::operator=(LevelPackage&& other) noexcept = default;

	const std::string& LevelPackage::GetId() const { return m_Impl->Id; }
	void LevelPackage::SetId(const std::string& id) { m_Impl->Id = id; }

	const std::string& LevelPackage::GetName() const { return m_Impl->Name; }
	void LevelPackage::SetName(const std::string& name) { m_Impl->Name = name; }

	uint32_t LevelPackage::GetGridCellSize() const { return m_Impl->GridCellSize; }
	void LevelPackage::SetGridCellSize(uint32_t size) { m_Impl->GridCellSize = size; }

	const LevelWorldSettings& LevelPackage::GetWorld() const { return m_Impl->World; }
	LevelWorldSettings& LevelPackage::GetWorld() { return m_Impl->World; }

	const std::vector<DataLayerDefinition>& LevelPackage::GetDataLayers() const { return m_Impl->DataLayers; }
	std::vector<DataLayerDefinition>& LevelPackage::GetDataLayers() { return m_Impl->DataLayers; }

	size_t LevelPackage::GetEntityCount() const { return m_Impl->Entities.size(); }
	const EntityDescriptor& LevelPackage::GetEntity(size_t index) const { return m_Impl->Entities[index]; }
	EntityDescriptor& LevelPackage::GetMutableEntity(size_t index) { return m_Impl->Entities[index]; }

	void LevelPackage::AddEntity(const EntityDescriptor& entity)
	{
		for (auto& existing : m_Impl->Entities)
			if (existing.EntityUUID == entity.EntityUUID)
			{
				existing = entity;
				return;
			}
		m_Impl->Entities.push_back(entity);
	}

	const EntityDescriptor* LevelPackage::FindEntity(uint64_t entityUuid) const
	{
		for (const auto& entity : m_Impl->Entities)
			if (entity.EntityUUID == entityUuid)
				return &entity;
		return nullptr;
	}

	std::filesystem::path LevelPackage::ContentDir() const
	{
		// 入口 = `<内容根>/scenes/<Id>.wlevel` ⇒ 内容 = `<内容根>/levels/<Id>`。
		// 单一推导来源:没有第二个地方记录内容位置,也就不会失配。
		if (m_EntryPath.empty())
			return {};
		const std::filesystem::path contentRoot = m_EntryPath.parent_path().parent_path();
		const std::string id = m_Impl->Id.empty() ? m_EntryPath.stem().string() : m_Impl->Id;
		return contentRoot / LevelsDirName / id;
	}

	bool LevelPackage::SaveEntry(std::string* outError) const
	{
		try
		{
			YAML::Emitter out;
			out << YAML::BeginMap;
			out << YAML::Key << "Id" << YAML::Value << m_Impl->Id;
			out << YAML::Key << "Name" << YAML::Value << m_Impl->Name;
			out << YAML::Key << "GridCellSize" << YAML::Value << m_Impl->GridCellSize;

			// `World:` 只在与默认不同时写(旧 manifest 形态不变)。
			if (!m_Impl->World.IsDefault())
			{
				out << YAML::Key << "World" << YAML::Value << YAML::BeginMap;
				if (m_Impl->World.HasGravityOverride)
					out << YAML::Key << "gravity" << YAML::Value << m_Impl->World.Gravity;
				if (m_Impl->World.PhysicsDebug)
					out << YAML::Key << "physics_debug" << YAML::Value << true;
				out << YAML::EndMap;
			}

			if (!m_Impl->DataLayers.empty())
			{
				out << YAML::Key << "DataLayers" << YAML::Value << YAML::BeginSeq;
				for (const auto& layer : m_Impl->DataLayers)
				{
					out << YAML::BeginMap;
					out << YAML::Key << "Mask" << YAML::Value << layer.LayerMask;
					out << YAML::Key << "Name" << YAML::Value << layer.Name;
					out << YAML::Key << "DefaultActive" << YAML::Value << layer.DefaultActive;
					out << YAML::EndMap;
				}
				out << YAML::EndSeq;
			}
			out << YAML::EndMap;

			const std::filesystem::path path = m_EntryPath;
			if (path.empty())
			{
				if (outError) *outError = "level entry path is empty";
				return false;
			}
			if (path.has_parent_path())
				std::filesystem::create_directories(path.parent_path());

			std::ofstream fout(path);
			if (!fout.is_open())
			{
				if (outError) *outError = "cannot write level entry: " + path.u8string();
				return false;
			}
			fout << out.c_str();
			return true;
		}
		catch (const std::exception& e)
		{
			if (outError) *outError = e.what();
			return false;
		}
	}

	bool LevelPackage::Save(const std::filesystem::path& entryPath, std::string* outError)
	{
		const std::filesystem::path previousEntry = m_EntryPath;
		m_EntryPath = entryPath;

		// Id 从入口文件名推导(未显式设置时),保证"入口名 == Id"这条不变量天然成立。
		if (m_Impl->Id.empty())
			m_Impl->Id = entryPath.stem().string();

		if (!SaveEntry(outError))
		{
			m_EntryPath = previousEntry;
			return false;
		}

		try
		{
			const std::filesystem::path entitiesDir = ContentDir() / kEntitiesDirName;
			std::filesystem::create_directories(entitiesDir);

			std::set<std::string> liveFiles;
			for (const auto& entity : m_Impl->Entities)
			{
				const std::string filename = std::to_string(entity.EntityUUID) + kEntityExtension;
				liveFiles.insert(filename);
				const std::filesystem::path entityPath = entitiesDir / filename;
				if (!const_cast<EntityDescriptor&>(entity).Save(entityPath, outError))
					return false;
			}

			// 剪枝:编辑器里删掉的实体若留下旧 `.entity`,下次载入会"复活"成幽灵实体。
			for (const auto& entry : std::filesystem::directory_iterator(entitiesDir))
			{
				if (!entry.is_regular_file() || entry.path().extension() != kEntityExtension)
					continue;
				if (liveFiles.count(entry.path().filename().string()) != 0)
					continue;
				std::error_code removeError;
				std::filesystem::remove(entry.path(), removeError);
			}
			return true;
		}
		catch (const std::exception& e)
		{
			if (outError) *outError = e.what();
			return false;
		}
	}

	bool LevelPackage::Load(const std::filesystem::path& entryPath, std::string* outError)
	{
		if (!std::filesystem::is_regular_file(entryPath))
		{
			if (outError) *outError = "level entry not found: " + entryPath.u8string();
			return false;
		}

		try
		{
			YAML::Node root = YAML::LoadFile(entryPath.string());
			if (!root || !root.IsMap())
			{
				if (outError) *outError = "level entry is not a map: " + entryPath.u8string();
				return false;
			}

			m_EntryPath = entryPath;
			m_Impl->Id = root["Id"] ? root["Id"].as<std::string>("") : std::string();
			if (m_Impl->Id.empty())
				m_Impl->Id = entryPath.stem().string();
			m_Impl->Name = root["Name"] ? root["Name"].as<std::string>("") : std::string();
			if (m_Impl->Name.empty())
				m_Impl->Name = m_Impl->Id;
			m_Impl->GridCellSize = root["GridCellSize"] ? root["GridCellSize"].as<uint32_t>(64) : 64u;

			m_Impl->World = LevelWorldSettings {};
			if (YAML::Node world = root["World"])
			{
				if (world["gravity"])
				{
					m_Impl->World.Gravity = world["gravity"].as<float>(m_Impl->World.Gravity);
					m_Impl->World.HasGravityOverride = true;
				}
				if (world["physics_debug"])
					m_Impl->World.PhysicsDebug = world["physics_debug"].as<bool>(false);
			}

			m_Impl->DataLayers.clear();
			if (YAML::Node layers = root["DataLayers"])
			{
				for (const auto& layerNode : layers)
				{
					DataLayerDefinition def;
					if (layerNode["Mask"]) def.LayerMask = layerNode["Mask"].as<uint32_t>(1);
					if (layerNode["Name"]) def.Name = layerNode["Name"].as<std::string>();
					if (layerNode["DefaultActive"]) def.DefaultActive = layerNode["DefaultActive"].as<bool>(true);
					m_Impl->DataLayers.push_back(def);
				}
			}

			m_Impl->Entities.clear();
			const std::filesystem::path entitiesDir = ContentDir() / kEntitiesDirName;
			if (std::filesystem::exists(entitiesDir))
			{
				for (const auto& entry : std::filesystem::directory_iterator(entitiesDir))
				{
					if (!entry.is_regular_file() || entry.path().extension() != kEntityExtension)
						continue;
					EntityDescriptor entity;
					if (!entity.Load(entry.path(), outError))
						return false;
					m_Impl->Entities.push_back(std::move(entity));
				}
			}

			return true;
		}
		catch (const std::exception& e)
		{
			if (outError) *outError = e.what();
			return false;
		}
	}

	bool LevelPackage::CreateEmpty(WorldContext& context, const std::filesystem::path& entryPath,
		const std::string& id, std::string* error)
	{
		if (entryPath.empty() || id.empty())
		{
			if (error) *error = "level entry path or id is empty";
			return false;
		}

		// 已存在 ⇒ 拒绝覆盖(别人的关卡)。
		std::error_code ec;
		if (std::filesystem::exists(entryPath, ec))
		{
			if (error) *error = "level entry already exists: " + entryPath.u8string();
			return false;
		}

		LevelPackage package;
		package.SetId(id);

		const std::filesystem::path contentDir = [&entryPath, &id]()
		{
			const std::filesystem::path contentRoot = entryPath.parent_path().parent_path();
			return contentRoot / LevelsDirName / id;
		}();
		if (std::filesystem::is_directory(contentDir, ec) && !std::filesystem::is_empty(contentDir, ec))
		{
			if (error) *error = "level content directory already exists and is not empty: "
				+ contentDir.u8string();
			return false;
		}

		// 起手内容:相机 + 方向光 + 一个立方体。新关卡打开就能看见东西,
		// 也能立刻验证"渲染走的是这个关卡",而不是一片黑让人怀疑没加载上。
		Scene scene(context);
		{
			Entity camera = Entity::CreateEntity(&scene, "Camera3D");
			camera.AddComponent<TransformComponent>(glm::vec3 { 0.0f, 1.0f, 5.0f });
			CameraComponent cameraComponent;
			cameraComponent.Camera.m_ProjectionType = CameraSettings::ProjectionType::Perspective;
			cameraComponent.Camera.m_PerspectiveFarClip = 200.0f;
			cameraComponent.Primary = true;
			camera.AddComponent<CameraComponent>(cameraComponent);

			Entity light = Entity::CreateEntity(&scene, "Directional Light");
			light.AddComponent<TransformComponent>();
			light.AddComponent<DirectionalLightComponent>();

			Entity cube = Entity::CreateEntity(&scene, "Cube");
			cube.AddComponent<TransformComponent>(glm::vec3 { 0.0f, 0.0f, 0.0f });
			MeshRendererComponent renderer;
			renderer.Primitive = MeshRendererComponent::PrimitiveShape::Cube;
			renderer.Color = glm::vec4 { 0.82f, 0.82f, 0.86f, 1.0f };
			cube.AddComponent<MeshRendererComponent>(renderer);
		}

		package.CaptureFromScene(scene, id, 64);
		package.GetDataLayers().push_back({ 1u, "Base_World", true });
		return package.Save(entryPath, error);
	}

	void LevelPackage::CaptureFromScene(Scene& scene, const std::string& levelName, uint32_t gridCellSize)
	{
		m_Impl->Name = levelName;
		m_Impl->GridCellSize = gridCellSize > 0 ? gridCellSize : 64;
		m_Impl->Entities.clear();

		// 世界设置随关卡走(重力覆盖 / 物理调试);预制体不带这些。
		const Scene::WorldSettings& world = scene.GetWorldSettings();
		m_Impl->World = LevelWorldSettings {};
		if (world.HasGravityOverride())
		{
			m_Impl->World.Gravity = world.Gravity;
			m_Impl->World.HasGravityOverride = true;
		}
		m_Impl->World.PhysicsDebug = world.PhysicsDebug;

		entt::registry& registry = scene.GetRegistry();

		// ---- 稀疏放置(§16.10)----
		//
		// 目标:摆 200 个**未被改动**的实例 ⇒ 只落 200 份"实例根 + 模板引用",而不是 200 棵
		// 子树的完整副本(旧形态:仓库随摆放量线性膨胀,且失去传播)。
		//
		// 判定是**逐实体比对**,不是"相信同构":按模板实例化一份到暂存场景,再把实时实体与
		// 模板对应实体逐字段比 —— 有覆盖登记或有实际差异的后代照常落盘。因此实例内部被增删/
		// 拖动过时不会丢数据,最多退化成今天的稠密形态(告警说明原因)。
		// 内容根 = 关卡内容目录上溯两级(<内容根>/levels/<Id>)—— 实例记录里的 .wprefab 是
		// 相对**内容根**的逻辑路径(与编辑器 Paths::AssetRoot 同一口径)。
		const std::filesystem::path contentRoot = ContentDir().parent_path().parent_path();
		std::unordered_set<entt::entity> sparseSkipped;

		struct TemplateCache
		{
			bool Loaded = false;
			bool Failed = false;
			std::vector<std::string> Docs;   // 先序逐实体文档
			std::string Shape;               // 模板子树的先序形状签名(见 SubtreeShape)
		};
		std::unordered_map<std::string, TemplateCache> templates;

		for (Gameplay::PrefabInstanceRecord& record : scene.PrefabInstances())
		{
			record.SparsePlacement = false;
			if (!record.IsValid() || record.PrefabPath.empty() || contentRoot.empty())
				continue;
			if (!registry.valid(record.Root))
				continue;

			TemplateCache& cache = templates[record.PrefabPath];
			if (!cache.Loaded)
			{
				cache.Loaded = true;
				const std::filesystem::path prefabPath =
					contentRoot / std::filesystem::u8path(record.PrefabPath);
				std::error_code ec;
				if (!std::filesystem::is_regular_file(prefabPath, ec))
				{
					cache.Failed = true;
					WLD_CORE_WARN("LevelPackage: prefab '{0}' cannot be resolved under '{1}'; "
						"writing every entity of its instances (dense)",
						record.PrefabPath, contentRoot.u8string());
				}
				else
				{
					const Ref<Scene> staging = CreateRef<Scene>(scene.GetContext());
					const Gameplay::PrefabInstanceResult staged =
						Gameplay::InstantiateFromFile(prefabPath, *staging);
					if (!staged.IsValid())
					{
						cache.Failed = true;
					}
					else
					{
						std::vector<entt::entity> order;
						Gameplay::FlattenInstanceSubtree(staging->GetRegistry(),
							static_cast<entt::entity>(staged.Root), order);
						cache.Shape = SubtreeShape(staging->GetRegistry(), order);
						for (const entt::entity handle : order)
						{
							std::string doc;
							std::string ignored;
							if (SceneSerializer::SerializeEntityDocumentToYaml(
								Entity(staging.get(), handle), &doc, &ignored))
								cache.Docs.push_back(std::move(doc));
						}
					}
				}
			}
			if (cache.Failed)
				continue;

			std::vector<entt::entity> order;
			Gameplay::FlattenInstanceSubtree(registry, record.Root, order);
			if (order.size() != cache.Docs.size())
			{
				WLD_CORE_WARN("LevelPackage: prefab instance '{0}' has {1} entities but its template has {2}; "
					"writing every entity of the instance (dense)",
					record.PrefabPath, order.size(), cache.Docs.size());
				continue;
			}
			if (SubtreeShape(registry, order) != cache.Shape)
			{
				WLD_CORE_WARN("LevelPackage: prefab instance '{0}' no longer matches its template's structure; "
					"writing every entity of the instance (dense)", record.PrefabPath);
				continue;
			}

			record.SparsePlacement = true;
			for (size_t index = 1; index < order.size(); ++index)
			{
				// 有覆盖登记 ⇒ 值可能与模板不同 ⇒ 照常落盘(它是"差异实体")。
				if (record.Overrides.find(static_cast<uint32_t>(order[index])) != record.Overrides.end())
					continue;
				std::string live;
				std::string ignored;
				if (!SceneSerializer::SerializeEntityDocumentToYaml(Entity(&scene, order[index]), &live, &ignored))
					continue;
				if (ComponentFingerprint(live) == ComponentFingerprint(cache.Docs[index]))
					sparseSkipped.insert(order[index]);
			}
		}

		for (const entt::entity handle : registry.view<UUIDComponent>())
		{
			if (sparseSkipped.count(handle) != 0)
				continue;
			const Entity entity(&scene, handle);

			EntityDescriptor descriptor;
			std::string documentError;
			if (!SceneSerializer::SerializeEntityDocumentToYaml(entity, &descriptor.DocumentYaml, &documentError))
				continue;
			descriptor.RefreshDerivedFromDocument();
			m_Impl->Entities.push_back(std::move(descriptor));
		}
	}

	Ref<Scene> LevelPackage::Instantiate(WorldContext& context) const
	{
		Ref<Scene> scene = CreateRef<Scene>(context);

		// ① 稀疏实例(§16.10):文档带"模板先序 → 实体 UUID"映射 ⇒ 先**实例化模板**并把这些
		//    UUID 贴回重建出来的实体。未落盘的子实体因此原样出现,且身份(覆盖条目键控、
		//    关卡里其它实体的引用)与存盘前一致。
		// 内容根 = 关卡内容目录上溯两级(<内容根>/levels/<Id>)—— 实例记录里的 .wprefab 是
		// 相对**内容根**的逻辑路径(与编辑器 Paths::AssetRoot 同一口径)。
		const std::filesystem::path contentRoot = ContentDir().parent_path().parent_path();
		if (!contentRoot.empty())
		{
			for (const EntityDescriptor& descriptor : m_Impl->Entities)
			{
				if (descriptor.DocumentYaml.empty())
					continue;
				Gameplay::PendingPrefabInstanceRecord block;
				if (!SceneSerializer::ReadPrefabBlock(descriptor.DocumentYaml, &block))
					continue;
				if (block.EntityUuids.empty() || block.PrefabPath.empty())
					continue;

				const std::filesystem::path prefabPath =
					contentRoot / std::filesystem::u8path(block.PrefabPath);
				std::error_code ec;
				if (!std::filesystem::is_regular_file(prefabPath, ec))
				{
					WLD_CORE_WARN("LevelPackage '{0}': sparse instance references missing prefab '{1}'; "
						"only the entities that have their own document will load",
						m_Impl->Id, block.PrefabPath);
					continue;
				}
				const Gameplay::PrefabInstanceResult staged = Gameplay::InstantiateFromFileWithIds(
					prefabPath, *scene, entt::null, block.EntityUuids);
				if (!staged.IsValid())
					WLD_CORE_WARN("LevelPackage '{0}': failed to rebuild sparse instance from '{1}'",
						m_Impl->Id, block.PrefabPath);
			}
		}

		// ② 逐实体文档:UUID 已存在(稀疏实例的根与"差异实体")⇒ 覆盖写;其余新建。覆盖语义
		//    正是稀疏形态需要的 —— 模板把值铺好,落盘的文档再把用户改过的部分盖上去。
		std::vector<std::string> documents;
		documents.reserve(m_Impl->Entities.size());
		for (const EntityDescriptor& descriptor : m_Impl->Entities)
			if (!descriptor.DocumentYaml.empty())
				documents.push_back(descriptor.DocumentYaml);

		std::string mergeError;
		SceneSerializer::MergeEntityDocuments(*scene, documents, /*overwriteExisting*/ true, &mergeError);
		if (!mergeError.empty())
		{
			WLD_CORE_WARN("LevelPackage '{0}': 部分实体文档并入失败: {1}", m_Impl->Name, mergeError);
		}
		SceneSerializer::FinalizeEntityImport(*scene);

		// 世界设置随关卡恢复 —— 与 CaptureFromScene 成对,否则关卡里设的重力覆盖
		// 存盘再开会静默回到项目默认。
		Scene::WorldSettings& world = scene->GetWorldSettings();
		world = Scene::WorldSettings {};
		// Gravity 默认 NaN(= 跟随项目清单),覆盖时才写实数。
		if (m_Impl->World.HasGravityOverride)
			world.Gravity = m_Impl->World.Gravity;
		if (m_Impl->World.PhysicsDebug)
			world.PhysicsDebug = true;
		return scene;
	}
}
