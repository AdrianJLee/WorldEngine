#include "World.h"
#include "RuntimeLayer.h"
// 这个文件是整个 Runtime 程序的入口，定义了 RuntimeApp 类并实现了 CreateApplication 函数
#include "World/Core/EntryPoint.h"

namespace World
{
	class RuntimeApp : public Application
	{
	public:
		RuntimeApp()
			:Application("Runtime")
		{
			// 在游戏程序启动时，自动扫描 content 目录下的所有 .wpak 或 .pak 文件，并挂载到 VFS 中
			std::filesystem::path contentDir = std::string(WLD_CURRENT_DIR) + "content";
			MountAllPakFiles(contentDir);

			PushLayer(WLD_ENGINE_NEW(RuntimeLayer));


		}
		~RuntimeApp()
		{

		}

	private:
		void MountAllPakFiles(const std::filesystem::path& contentDir)
		{
			// 检查目录是否存在
			if (!std::filesystem::exists(contentDir) || !std::filesystem::is_directory(contentDir))
			{
				WLD_CORE_WARN("Content directory '{0}' not found, skipping VFS mount.", contentDir.string());
				return;
			}

			// 遍历 content 文件夹下所有文件
			for (const auto& entry : std::filesystem::directory_iterator(contentDir))
			{
				if (entry.is_regular_file())
				{
					std::string ext = entry.path().extension().string();
					// 检查后缀是否为你的打包格式 .wpak 或者 .pak
					if (ext == ".wpak" || ext == ".pak")
					{
						WLD_CORE_INFO("Mounting pak file: {0}", entry.path().string());

						// 如果是后续的文件，传入 false 以追加模式合并进去。
						World::VFS::Mount(entry.path().string());

					}
				}
			}

		}
	};

	Application* CreateApplication()
	{

		return new RuntimeApp();
	}
}
