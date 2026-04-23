#pragma once
#ifdef WLD_PLATFORM_WINDOWS
extern World::Application* World::CreateApplication();

int main(int argc, char** argv)
{
	World::Log::Init();

	WLD_PROFILE_BEGIN_SESSION("Startup", "HazelProfile-Startup.json");
	auto app = World::CreateApplication();
	WLD_PROFILE_END_SESSION();

	WLD_PROFILE_BEGIN_SESSION("Runtime", "HazelProfile-Runtime.json");
	app->Run();
	WLD_PROFILE_END_SESSION();

	WLD_PROFILE_BEGIN_SESSION("Shutdown", "HazelProfile-Shutdown.json");
	delete app;
	WLD_PROFILE_END_SESSION();
}
#endif