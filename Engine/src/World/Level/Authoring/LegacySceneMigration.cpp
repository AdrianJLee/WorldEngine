#include "wldpch.h"
#include "World/Level/Authoring/LegacySceneMigration.h"

#include "World/Core/StringPool.h"
#include "World/Scene/Components.h"
#include "World/Scene/Entity.h"
#include "World/Scene/Scene.h"
#include "World/Scene/SceneSerializer.h"

namespace World::Level
{
	MigrationResult LegacySceneMigrationTool::DeconstructSceneToPackage(
		Scene& scene,
		const std::string& levelName,
		uint32_t gridCellSize)
	{
		MigrationResult result;

		try
		{
			result.Package.SetName(levelName);
			result.Package.SetGridCellSize(gridCellSize > 0 ? gridCellSize : 64);
			result.Package.GetDataLayers().push_back({ 1, "Base_World", true });

			entt::registry& registry = scene.GetRegistry();
			for (const entt::entity handle : registry.view<UUIDComponent>())
			{
				const Entity entity(&scene, handle);

				// 实体**全部组件**随文件走:直接复用场景文档那条实体级序列化,
				// 迁移出来的 `.entity` 与 `.wd` 里该实体的块逐字段同源。
				std::string document;
				std::string documentError;
				if (!SceneSerializer::SerializeEntityDocumentToYaml(entity, &document, &documentError))
				{
					result.Success = false;
					result.ErrorMessage = documentError;
					return result;
				}

				EntityDescriptor descriptor;
				descriptor.DocumentYaml = std::move(document);
				descriptor.DataLayerMask = 1;
				descriptor.RefreshDerivedFromDocument();

				result.Package.AddEntity(descriptor);
				result.EntitiesMigrated++;
			}

			result.Success = true;
			return result;
		}
		catch (const std::exception& e)
		{
			result.Success = false;
			result.ErrorMessage = e.what();
			return result;
		}
	}

	bool LegacySceneMigrationTool::ExportPackageToDisk(
		const LevelPackage& package,
		const std::filesystem::path& destinationDir,
		std::string* outError)
	{
		return package.Save(destinationDir, outError);
	}
}
