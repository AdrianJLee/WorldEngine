#include "wldpch.h"
#include "World/Core/Asset/ProjectMount.h"

#include "World/Core/Asset/ProjectManifest.h"
#include "World/Core/Vfs/DirectoryProvider.h"
#include "World/Core/Vfs/PackageProvider.h"
#include "World/Core/WorldContext.h"
#include "World/Renderer/ShaderUtils.h"

namespace World::Asset
{
	void MountProjectContent(World::WorldContext& context)
	{
		World::Vfs::Vfs& vfs = context.Vfs();

		std::filesystem::path manifestPath;
		if (World::Asset::ProjectManifest::Locate(std::filesystem::current_path(), &manifestPath))
		{
			std::string error;
			World::Asset::ProjectManifest manifest;
			if (World::Asset::ProjectManifest::Load(manifestPath, &manifest, &error))
			{
				// 开发形态:content_root 存在则挂目录 provider(优先级最高)。
				const std::filesystem::path contentRoot = manifest.ResolveContentRoot(manifestPath);
				std::error_code dirEc;
				if (std::filesystem::is_directory(contentRoot, dirEc))
					vfs.Mount("dir:game-assets",
						std::make_shared<World::Vfs::DirectoryProvider>(contentRoot), 100);

				// 发行形态:按 manifest 逐条挂载包,失败只记错误继续。
				for (const std::string& package : manifest.Packages)
				{
					// 包路径相对清单所在目录解析(发行目录 = 清单目录;开发目录 = 仓库 Game/)。
					std::filesystem::path pakPath = manifestPath.parent_path() / package;
					std::error_code existsEc;
					if (!std::filesystem::is_regular_file(pakPath, existsEc))
						pakPath = std::filesystem::current_path() / package;
					if (!std::filesystem::is_regular_file(pakPath, existsEc))
					{
						// 开发形态没有发行包属正常情况。
						WLD_CORE_INFO("Package '{0}' not present (development layout)", package);
						continue;
					}
					std::error_code openEc;
					std::shared_ptr<World::Vfs::PackageProvider> provider =
						World::Vfs::PackageProvider::Open(pakPath, openEc);
					if (!provider)
					{
						WLD_CORE_ERROR("Failed to mount package '{0}': {1}",
							pakPath.string(), openEc.message());
						continue;
					}
					vfs.Mount("pak:" + package, std::move(provider), 10);
					WLD_CORE_INFO("Mounted package '{0}'", pakPath.string());
				}
			}
			else
			{
				WLD_CORE_WARN("Failed to load manifest '{0}': {1}", manifestPath.string(), error);
			}
		}
		else
		{
			// 回退:无清单时的旧开发形态(仅保留到旧工程完全迁移)。
			WLD_CORE_WARN("No project manifest found; falling back to legacy content mounting.");
			std::error_code dirEc;
			const std::filesystem::path assetsDir =
				std::filesystem::path(std::string(WLD_CURRENT_DIR) + "../Game/assets");
			if (std::filesystem::is_directory(assetsDir, dirEc))
				vfs.Mount("dir:game-assets",
					std::make_shared<World::Vfs::DirectoryProvider>(assetsDir), 100);

			const std::filesystem::path contentDir = std::filesystem::current_path() / "content";
			if (std::filesystem::is_directory(contentDir, dirEc))
			{
				for (const auto& entry : std::filesystem::directory_iterator(contentDir, dirEc))
				{
					if (dirEc)
						break;
					if (!entry.is_regular_file(dirEc) || entry.path().extension() != ".wpak")
						continue;
					std::error_code openEc;
					std::shared_ptr<World::Vfs::PackageProvider> provider =
						World::Vfs::PackageProvider::Open(entry.path(), openEc);
					if (provider)
						vfs.Mount("pak:" + entry.path().filename().string(), std::move(provider), 10);
				}
			}
		}

		// 着色器烘焙产物解析:发行形态优先读包内 cooked 产物(不依赖源码树/dxc),
		// 开发形态未命中时回落到源码树按需编译。
		World::ShaderCompiler::SetArtifactResolver(
			[&vfs](const std::string& logical, std::vector<uint8_t>& out)
			{
				std::error_code readEc;
				return vfs.Read(logical, out, readEc) && !out.empty();
			});
	}
}
