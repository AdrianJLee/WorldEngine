#include "wldpch.h"
#include "World/Level/Authoring/LevelPackage.h"

#include "World/Core/Log.h"
#include "World/Core/WorldContext.h"
#include "World/Renderer/EditorCamera.h"
#include "World/Scene/Components.h"
#include "World/Scene/Entity.h"
#include "World/Scene/Scene.h"
#include "World/Scene/SceneSerializer.h"

#include <fstream>
#include <set>
#include <yaml-cpp/yaml.h>

namespace World::Level
{
	namespace
	{
		constexpr const char* kEntitiesDirName = "entities";
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
		for (const entt::entity handle : registry.view<UUIDComponent>())
		{
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
		for (size_t i = 0; i < m_Impl->Entities.size(); ++i)
		{
			const EntityDescriptor& descriptor = m_Impl->Entities[i];
			if (descriptor.DocumentYaml.empty())
				continue;

			std::string importError;
			SceneSerializer::AppendEntityYaml(*scene, descriptor.DocumentYaml, &importError);
			if (!importError.empty())
			{
				WLD_CORE_WARN("LevelPackage '{0}': 实体 {1} 并入失败: {2}",
					m_Impl->Name, descriptor.EntityUUID, importError);
			}
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
