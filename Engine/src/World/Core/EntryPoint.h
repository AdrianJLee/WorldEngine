#pragma once
#ifdef WLD_PLATFORM_WINDOWS
extern World::Application* World::CreateApplication(World::WorldContext& context);

int main(int argc, char** argv)
{
	World::Log::Init();

	World::WorldContext context;

	WLD_PROFILE_BEGIN_SESSION("Startup", "WEProfile-Startup.json");
	auto app = World::CreateApplication(context);
	WLD_PROFILE_END_SESSION();

	WLD_PROFILE_BEGIN_SESSION("Runtime", "WEProfile-Runtime.json");
	app->Run();
	WLD_PROFILE_END_SESSION();

	WLD_PROFILE_BEGIN_SESSION("Shutdown", "WEProfile-Shutdown.json");
	delete app;
	WLD_PROFILE_END_SESSION();
}
#endif
