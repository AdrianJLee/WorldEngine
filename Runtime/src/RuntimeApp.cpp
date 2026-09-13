#include "World.h"
#include "RuntimeLayer.h"
// 这个文件是整个 Runtime 程序的入口，定义了 RuntimeApp 类并实现了 CreateApplication 函数
#include "World/Core/EntryPoint.h"
#include "World/Core/Vfs/DirectoryProvider.h"
#include "World/Core/Vfs/PackageProvider.h"

#include <memory>
#include <system_error>

namespace World
{
	class RuntimeApp : public Application
	{
	public:
		RuntimeApp(World::WorldContext& context)
			:Application("Runtime", context)
		{
			// VFS 2.0:开发形态挂目录 provider,发行形态挂包 provider。
			MountContent(context.Vfs());

			PushLayer(WLD_ENGINE_NEW(RuntimeLayer));
		}

		~RuntimeApp()
		{
		}

	private:
		void MountContent(World::Vfs::Vfs& vfs)
		{
			// 开发形态:源码资产目录直读,优先级最高,保留热迭代体验。
			std::error_code dirEc;
			const std::filesystem::path assetsDir =
				std::filesystem::path(std::string(WLD_CURRENT_DIR) + "../Game/assets");
			if (!std::filesystem::is_directory(assetsDir, dirEc))
			{
				WLD_CORE_WARN("Assets directory '{0}' not found, skipping directory provider mount.",
					assetsDir.string());
			}
			else
			{
				vfs.Mount("dir:game-assets",
					std::make_shared<World::Vfs::DirectoryProvider>(assetsDir), 100);
			}

			// 发行形态:扫描 content 目录下的所有 .wpak 并逐个挂载;打开失败只记错误继续。
			const std::filesystem::path contentDir =
				std::filesystem::path(std::string(WLD_CURRENT_DIR) + "content");
			if (!std::filesystem::is_directory(contentDir, dirEc))
			{
				WLD_CORE_WARN("Content directory '{0}' not found, skipping package mounts.",
					contentDir.string());
				return;
			}

			std::error_code itEc;
			const std::filesystem::directory_iterator end;
			for (std::filesystem::directory_iterator it(contentDir, itEc); it != end; it.increment(itEc))
			{
				if (itEc)
				{
					WLD_CORE_WARN("Failed to enumerate content directory '{0}': {1}",
						contentDir.string(), itEc.message());
					break;
				}
				const std::filesystem::directory_entry& entry = *it;
				if (!entry.is_regular_file(itEc))
				{
					if (itEc)
					{
						WLD_CORE_WARN("Failed to inspect '{0}': {1}",
							entry.path().string(), itEc.message());
						break;
					}
					continue;
				}
				if (entry.path().extension() != ".wpak")
					continue;

				std::error_code openEc;
				std::shared_ptr<World::Vfs::PackageProvider> provider =
					World::Vfs::PackageProvider::Open(entry.path(), openEc);
				if (!provider)
				{
					WLD_CORE_ERROR("Failed to mount package '{0}': {1}",
						entry.path().string(), openEc.message());
					continue;
				}
				vfs.Mount("pak:" + entry.path().filename().string(), std::move(provider), 10);
				WLD_CORE_INFO("Mounted package '{0}'", entry.path().string());
			}
		}
	};

	Application* CreateApplication(World::WorldContext& context)
	{
		return new RuntimeApp(context);
	}
}
