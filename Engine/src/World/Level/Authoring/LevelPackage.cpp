#include "wldpch.h"
#include "World/Level/Authoring/LevelPackage.h"

#include "World/Core/StringPool.h"
#include "World/Renderer/EditorCamera.h"
#include "World/Scene/Components.h"
#include "World/Scene/Entity.h"
#include "World/Scene/Scene.h"
#include "World/Core/WorldContext.h"
#include "World/Scene/SceneSerializer.h"
#include <fstream>
#include <set>
#include <yaml-cpp/yaml.h>

namespace World::Level
{
	struct LevelPackage::Impl
	{
		std::string Name;
		uint32_t GridCellSize{ 64 };
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

	const std::string& LevelPackage::GetName() const { return m_Impl->Name; }
	void LevelPackage::SetName(const std::string& name) { m_Impl->Name = name; }

	uint32_t LevelPackage::GetGridCellSize() const { return m_Impl->GridCellSize; }
	void LevelPackage::SetGridCellSize(uint32_t size) { m_Impl->GridCellSize = size; }

	const std::vector<DataLayerDefinition>& LevelPackage::GetDataLayers() const { return m_Impl->DataLayers; }
	std::vector<DataLayerDefinition>& LevelPackage::GetDataLayers() { return m_Impl->DataLayers; }

	size_t LevelPackage::GetEntityCount() const { return m_Impl->Entities.size(); }
	const EntityDescriptor& LevelPackage::GetEntity(size_t index) const { return m_Impl->Entities[index]; }
	EntityDescriptor& LevelPackage::GetMutableEntity(size_t index) { return m_Impl->Entities[index]; }

	void LevelPackage::AddEntity(const EntityDescriptor& entity)
	{
		for (auto& existing : m_Impl->Entities)
		{
			if (existing.EntityUUID == entity.EntityUUID)
			{
				existing = entity;
				return;
			}
		}
		m_Impl->Entities.push_back(entity);
	}

	const EntityDescriptor* LevelPackage::FindEntity(uint64_t entityUuid) const
	{
		for (const auto& entity : m_Impl->Entities)
		{
			if (entity.EntityUUID == entityUuid)
				return &entity;
		}
		return nullptr;
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
		return scene;
	}

	bool LevelPackage::CreateEmpty(WorldContext& context, const std::filesystem::path& packageDir,
		const std::string& levelName, std::string* error)
	{
		if (packageDir.empty())
		{
			if (error) *error = "level directory is empty";
			return false;
		}

		// 已存在且非空 = 别人的关卡,拒绝覆盖(与"新建项目"同一保护口径)。
		std::error_code ec;
		if (std::filesystem::is_directory(packageDir, ec))
		{
			const bool empty = std::filesystem::is_empty(packageDir, ec);
			if (ec || !empty)
			{
				if (error) *error = "level directory already exists and is not empty: "
					+ packageDir.u8string();
				return false;
			}
		}

		// 起手内容:相机 + 方向光 + 一个立方体。新关卡打开就能看见东西、也能立刻
		// 验证"渲染走的是这个关卡",而不是一片黑让人怀疑没加载上。
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

		LevelPackage package;
		package.CaptureFromScene(scene, levelName.empty() ? packageDir.filename().string() : levelName, 64);
		package.GetDataLayers().push_back({ 1u, "Base_World", true });
		return package.Save(packageDir, error);
	}

	void LevelPackage::CaptureFromScene(Scene& scene, const std::string& levelName, uint32_t gridCellSize)
	{
		m_Impl->Name = levelName;
		m_Impl->GridCellSize = gridCellSize > 0 ? gridCellSize : 64;
		m_Impl->Entities.clear();

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

	bool LevelPackage::Save(const std::filesystem::path& packageDir, std::string* outError) const
	{
		try
		{
			std::filesystem::create_directories(packageDir);

			YAML::Emitter manifest;
			manifest << YAML::BeginMap;
			manifest << YAML::Key << "Name" << YAML::Value << m_Impl->Name;
			manifest << YAML::Key << "GridCellSize" << YAML::Value << m_Impl->GridCellSize;

			if (!m_Impl->DataLayers.empty())
			{
				manifest << YAML::Key << "DataLayers" << YAML::Value << YAML::BeginSeq;
				for (const auto& layer : m_Impl->DataLayers)
				{
					manifest << YAML::BeginMap;
					manifest << YAML::Key << "Mask" << YAML::Value << layer.LayerMask;
					manifest << YAML::Key << "Name" << YAML::Value << layer.Name;
					manifest << YAML::Key << "DefaultActive" << YAML::Value << layer.DefaultActive;
					manifest << YAML::EndMap;
				}
				manifest << YAML::EndSeq;
			}
			manifest << YAML::EndMap;

			const std::filesystem::path manifestPath = packageDir / "manifest.yaml";
			std::ofstream fout(manifestPath);
			if (!fout.is_open())
			{
				if (outError) *outError = "Failed to open manifest for write: " + manifestPath.string();
				return false;
			}
			fout << manifest.c_str();
			fout.close();

			const std::filesystem::path entitiesDir = packageDir / "entities";
			std::filesystem::create_directories(entitiesDir);

			std::set<std::string> liveFiles;
			for (const auto& entity : m_Impl->Entities)
			{
				const std::string filename = std::to_string(entity.EntityUUID) + ".entity";
				liveFiles.insert(filename);
				const std::filesystem::path entityPath = entitiesDir / filename;
				if (!entity.Save(entityPath, outError))
					return false;
			}

			// 剪枝:编辑器里删掉的实体若留下旧 `.entity`,下次载入会"复活"成幽灵实体。
			// 保存 = 让磁盘与内存一致,因此这里把不在集合里的实体文件删掉。
			for (const auto& entry : std::filesystem::directory_iterator(entitiesDir))
			{
				if (!entry.is_regular_file() || entry.path().extension() != ".entity")
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

	bool LevelPackage::Load(const std::filesystem::path& packageDir, std::string* outError)
	{
		if (!std::filesystem::exists(packageDir))
		{
			if (outError) *outError = "Package directory not found: " + packageDir.string();
			return false;
		}

		try
		{
			const std::filesystem::path manifestPath = packageDir / "manifest.yaml";
			if (!std::filesystem::exists(manifestPath))
			{
				if (outError) *outError = "manifest.yaml missing in package: " + packageDir.string();
				return false;
			}

			YAML::Node root = YAML::LoadFile(manifestPath.string());
			if (root["Name"]) m_Impl->Name = root["Name"].as<std::string>();
			if (root["GridCellSize"]) m_Impl->GridCellSize = root["GridCellSize"].as<uint32_t>();

			m_Impl->DataLayers.clear();
			if (YAML::Node layers = root["DataLayers"])
			{
				for (const auto& layerNode : layers)
				{
					DataLayerDefinition def;
					if (layerNode["Mask"]) def.LayerMask = layerNode["Mask"].as<uint32_t>();
					if (layerNode["Name"]) def.Name = layerNode["Name"].as<std::string>();
					if (layerNode["DefaultActive"]) def.DefaultActive = layerNode["DefaultActive"].as<bool>();
					m_Impl->DataLayers.push_back(def);
				}
			}

			m_Impl->Entities.clear();
			const std::filesystem::path entitiesDir = packageDir / "entities";
			if (std::filesystem::exists(entitiesDir))
			{
				for (const auto& entry : std::filesystem::directory_iterator(entitiesDir))
				{
					if (entry.is_regular_file() && entry.path().extension() == ".entity")
					{
						EntityDescriptor entity;
						if (!entity.Load(entry.path(), outError))
							return false;
						m_Impl->Entities.push_back(entity);
					}
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
}
