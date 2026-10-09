#include "Document/EditorDocument.h"

#include "World/Core/Log.h"
#include "World/Scene/SceneSerializer.h"
#include "World/Scene/Scene.h"
#include "World/Scene/Entity.h"
#include "World/Scene/Components.h"
#include "World/Level/Authoring/LevelPackage.h"
#include "World/Level/Authoring/LegacySceneMigration.h"

namespace World
{
	EditorDocument::EditorDocument(WorldContext& context)
		: m_Context(&context), m_Scene(CreateRef<Scene>(context))
	{
	}

	bool EditorDocument::LoadFromFile(const std::filesystem::path& path)
	{
		if (path.empty())
		{
			m_LastError = "Cannot load scene: empty path.";
			return false;
		}

		// 优先检测是否为 Level 2.0 关卡目录或 manifest.yaml
		const bool isDirectory = std::filesystem::is_directory(path);
		const bool isManifest = (path.filename() == "manifest.yaml");
		if (isDirectory || isManifest)
		{
			const std::filesystem::path packageDir = isDirectory ? path : path.parent_path();
			Level::LevelPackage package;
			std::string err;
			if (package.Load(packageDir, &err))
			{
				Ref<Scene> newScene = CreateRef<Scene>(*m_Context);
				for (size_t i = 0; i < package.GetEntityCount(); ++i)
				{
					const auto& desc = package.GetEntity(i);
					Entity ent = Entity::CreateEntity(newScene.get(), desc.Name, UUID(desc.EntityUUID));
					if (ent.IsValid())
					{
						ent.AddComponent<TransformComponent>(
							desc.Position,
							glm::radians(desc.RotationEuler),
							desc.Scale
						);
					}
				}
				m_Scene = newScene;
				m_Path = packageDir;
				m_Dirty = false;
				m_LastError.clear();
				return true;
			}
		}

		Ref<Scene> newScene = CreateRef<Scene>(*m_Context);
		SceneSerializer serializer(newScene);
		if (!serializer.Deserialize(path.string()))
		{
			m_LastError = serializer.GetLastError().empty()
				? "Failed to load scene: " + path.string()
				: serializer.GetLastError();
			return false;
		}

		m_Scene = newScene;
		m_Path = path;
		m_Dirty = false;
		m_LastError.clear();
		return true;
	}

	bool EditorDocument::SaveTo(const std::filesystem::path& path)
	{
		if (path.empty())
		{
			m_LastError = "Cannot save scene: empty path.";
			return false;
		}

		const bool isDirectory = std::filesystem::is_directory(path);
		const bool isManifest = (path.filename() == "manifest.yaml");
		const bool hasNoExtension = !path.has_extension();

		// 如果目标是关卡目录或无后缀（OFPA 关卡模式）
		if (isDirectory || isManifest || hasNoExtension)
		{
			const std::filesystem::path packageDir = isDirectory ? path : (isManifest ? path.parent_path() : path);
			auto migResult = Level::LegacySceneMigrationTool::DeconstructSceneToPackage(*m_Scene, packageDir.stem().string(), 64);
			if (migResult.Success)
			{
				std::string err;
				if (migResult.Package.Save(packageDir, &err))
				{
					m_Path = packageDir;
					m_Dirty = false;
					m_LastError.clear();
					return true;
				}
				m_LastError = "Failed to save level package: " + err;
				return false;
			}
			m_LastError = "Failed to deconstruct scene: " + migResult.ErrorMessage;
			return false;
		}

		SceneSerializer serializer(m_Scene);
		if (!serializer.Serialize(path.string()))
		{
			m_LastError = serializer.GetLastError().empty()
				? "Failed to save scene: " + path.string()
				: serializer.GetLastError();
			return false;
		}

		m_Path = path;
		m_Dirty = false;
		m_LastError.clear();
		return true;
	}

	void EditorDocument::New()
	{
		m_Scene = CreateRef<Scene>(*m_Context);
		m_Path.clear();
		m_Dirty = false;
		m_LastError.clear();
	}
}
