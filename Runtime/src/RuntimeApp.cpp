#include "World.h"
#include "RuntimeLayer.h"
// 这个文件是整个 Runtime 程序的入口,定义了 RuntimeApp 类并实现了 CreateApplication 函数
#include "World/Core/EntryPoint.h"

namespace World
{
	class RuntimeApp : public Application
	{
	public:
		RuntimeApp(World::WorldContext& context)
			:Application("Runtime", context)
		{
			// 内容挂载(VFS + 着色器产物解析)已由 Application 统一完成:
			// 必须在 Renderer::Init 之前,发行形态才能从内容包读着色器。
			PushLayer(WLD_ENGINE_NEW(RuntimeLayer));
		}

		~RuntimeApp()
		{
		}
	};

	Application* CreateApplication(World::WorldContext& context)
	{
		return new RuntimeApp(context);
	}
}
