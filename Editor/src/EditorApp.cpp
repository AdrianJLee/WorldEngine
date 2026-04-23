#include "World.h"
#include "EditorLayer.h"

#include "World/Core/EntryPoint.h"

namespace World
{
	class EditorApp : public Application
	{
	public:
		EditorApp()
			:Application("Editor")
		{
			PushLayer(new EditorLayer());
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
