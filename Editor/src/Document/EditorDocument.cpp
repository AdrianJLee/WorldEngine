#include "Document/EditorDocument.h"

#include "World/Core/Log.h"
#include "World/Core/StringPool.h"
#include "World/Level/Authoring/EntityDescriptor.h"
#include "World/Scene/Components.h"
#include "World/Scene/Entity.h"
#include "World/Scene/Scene.h"
#include "World/Scene/SceneSerializer.h"

#include <unordered_map>

namespace World
{
	namespace
	{
		// 关卡包里的实体 → 场景实体:走与 `.wd` 同一条实体级读路径(SceneSerializer),
		// 因此**全部组件**都在,不只是名字与 Transform。
		void EnsureLevelComponent(Scene& scene, entt::entity handle, uint32_t mask)
		{
			if (scene.GetRegistry().all_of<LevelEntityComponent>(handle))
				return;
			scene.GetRegistry().emplace<LevelEntityComponent>(handle, mask == 0 ? 1u : mask);
		}
	}

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
					const size_t applied = SceneSerializer::AppendEntityYaml(*newScene, desc.DocumentYaml, &importError);
					if (!importError.empty())
					{
						WLD_CORE_WARN("Level '{0}': entity {1} failed to import: {2}",
							packageDir.u8string(), desc.EntityUUID, importError);
					}
					imported += applied;
				}
				// 逐实体并入之后必须收尾:重算 Transform 缓存矩阵 + 重建层级反向索引,
				// 缺这一步会表现成"相机在物体内部 / 物体全堆在原点"。
				SceneSerializer::FinalizeEntityImport(*newScene);

				// 数据层:旧文件若没有 LevelEntityComponent,按包里的归属补上,保证渲染剔除
				// 与面板显示都有真实数据可用。
				{
					entt::registry& registry = newScene->GetRegistry();
					for (size_t i = 0; i < package.GetEntityCount(); ++i)
					{
						const auto& desc = package.GetEntity(i);
						if (desc.EntityUUID == 0)
							continue;
						for (const entt::entity handle : registry.view<UUIDComponent>())
						{
							if (static_cast<uint64_t>(registry.get<UUIDComponent>(handle).ID) != desc.EntityUUID)
								continue;
							EnsureLevelComponent(*newScene, handle, desc.DataLayerMask);
							break;
						}
					}
				}

				m_Scene = newScene;
				m_Path = packageDir;
				m_LevelPackagePath = packageDir;
				m_DataLayers = package.GetDataLayers();
				m_Dirty = false;
				m_LastError.clear();
				WLD_CORE_INFO("[level] 关卡包已载入: {0} ({1} 个实体, {2} 个数据层)",
					packageDir.u8string(), imported, m_DataLayers.size());
				return true;
			}
			WLD_CORE_WARN("Level package '{0}' rejected: {1}", packageDir.u8string(), err);
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
		m_LevelPackagePath.clear();
		m_DataLayers.clear();
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
		// 关卡包目标:目录、manifest.yaml,或当前文档本来就是关卡包(路径无 .wd 后缀)。
		const bool wantsPackage = isDirectory || isManifest || IsLevelPackage() || !path.has_extension();

		if (wantsPackage)
		{
			const std::filesystem::path packageDir = isDirectory ? path
				: (isManifest ? path.parent_path() : path);

			// 从**当前场景**重建实体集合:按 UUID 对齐,保留既有 ChunkID,丢掉的实体自然
			// 被 LevelPackage::Save 的剪枝删除。
			std::unordered_map<uint64_t, uint64_t> chunkIds;
			if (m_LevelPackagePath == packageDir)
			{
				Level::LevelPackage existing;
				std::string existingError;
				if (existing.Load(packageDir, &existingError))
					for (size_t i = 0; i < existing.GetEntityCount(); ++i)
						chunkIds[existing.GetEntity(i).EntityUUID] = existing.GetEntity(i).ChunkID;
			}

			Level::LevelPackage package;
			package.SetName(packageDir.filename().string());
			package.SetGridCellSize(64);
			package.GetDataLayers() = m_DataLayers.empty()
				? std::vector<Level::DataLayerDefinition> { { 1u, "Base_World", true } }
				: m_DataLayers;

			entt::registry& registry = m_Scene->GetRegistry();
			for (const entt::entity handle : registry.view<UUIDComponent>())
			{
				const Entity entity(m_Scene.get(), handle);

				Level::EntityDescriptor descriptor;
				std::string documentError;
				if (!SceneSerializer::SerializeEntityDocumentToYaml(entity, &descriptor.DocumentYaml, &documentError))
				{
					m_LastError = "Failed to serialize entity: " + documentError;
					return false;
				}
				descriptor.ChunkID = 0;
				const uint64_t entityUuid = static_cast<uint64_t>(registry.get<UUIDComponent>(handle).ID);
				if (const auto it = chunkIds.find(entityUuid); it != chunkIds.end())
					descriptor.ChunkID = it->second;
				descriptor.RefreshDerivedFromDocument();
				package.AddEntity(descriptor);
			}

			std::string err;
			if (!package.Save(packageDir, &err))
			{
				m_LastError = "Failed to save level package: " + err;
				return false;
			}

			m_Path = packageDir;
			m_LevelPackagePath = packageDir;
			m_DataLayers = package.GetDataLayers();
			m_Dirty = false;
			m_LastError.clear();
			WLD_CORE_INFO("[level] 关卡包已保存: {0} ({1} 个实体)", packageDir.u8string(), package.GetEntityCount());
			return true;
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
		m_LevelPackagePath.clear();
		m_DataLayers.clear();
		m_Dirty = false;
		m_LastError.clear();
	}
}
