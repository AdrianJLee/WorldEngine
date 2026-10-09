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
				size_t imported = 0;
				for (size_t i = 0; i < package.GetEntityCount(); ++i)
				{
					const auto& desc = package.GetEntity(i);
					std::string importError;
					imported += SceneSerializer::AppendEntityYaml(*newScene, desc.DocumentYaml, &importError);
					if (!importError.empty())
						WLD_CORE_WARN("Level '{0}': entity {1} failed to import: {2}",
							packageDir.u8string(), desc.EntityUUID, importError);
				}
				// 逐实体并入之后必须收尾:重算 Transform 缓存矩阵 + 重建层级反向索引,
				// 缺这一步会表现成"相机在物体内部 / 物体全堆在原点"。
				SceneSerializer::FinalizeEntityImport(*newScene);

				WLD_CORE_INFO("[level] 关卡包已载入: {0} ({1} 个实体)", packageDir.u8string(), imported);

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
