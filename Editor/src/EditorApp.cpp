#include "World.h"
#include "EditorLayer.h"
#include "MemoryTraceLayer.h"

// 这个文件是整个 Editor 程序的入口，定义了 EditorApp 类并实现了 CreateApplication 函数
#include "World/Core/EntryPoint.h"

namespace World
{
	class EditorApp : public Application
	{
	public:
		EditorApp()
			:Application("Editor")
		{
			PushLayer(WLD_ENGINE_NEW(EditorLayer));
			PushLayer(WLD_ENGINE_NEW(MemoryTraceLayer));
		}
		~EditorApp()
		{

		}
	};

	Application* CreateApplication()
	{

		return new EditorApp();
	}
}
