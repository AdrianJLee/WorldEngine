#include "wldpch.h"
#include "World/Level/Authoring/LegacySceneMigration.h"
#include "World/Core/StringPool.h"
#include "World/Scene/Scene.h"
#include "World/Scene/Components.h"
#include <glm/gtc/quaternion.hpp>

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

			auto& registry = scene.GetRegistry();
			auto view = registry.view<TagComponent, UUIDComponent>();

			for (auto entity : view)
			{
				const auto& tag = view.get<TagComponent>(entity);
				const auto& uuidComp = view.get<UUIDComponent>(entity);

				EntityDescriptor desc;
				desc.EntityUUID = static_cast<uint64_t>(uuidComp.ID);
				desc.Name = StringPool::Get().NameOf(tag.Tag);
				desc.DataLayerMask = 1;

				if (registry.all_of<TransformComponent>(entity))
				{
					const auto& tc = registry.get<TransformComponent>(entity);
					desc.Position = tc.Location;
					desc.RotationEuler = glm::degrees(glm::eulerAngles(tc.Rotation));
					desc.Scale = tc.Scale;
				}

				result.Package.AddEntity(desc);
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
