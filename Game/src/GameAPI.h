#pragma once
#include "World.h" 
#include "Scripts/StressTest.h"
#ifdef _MSC_VER
#ifdef GAME_BUILD_DLL
#define GAME_API __declspec(dllexport)
#else
#define GAME_API __declspec(dllimport)
#endif
#else
#define GAME_API
#endif

extern "C"
{
	GAME_API inline void OnInitGameDLL(World::Application* hostInstance)
	{
		World::Application::SetInstance(hostInstance);
	}
	GAME_API inline void OnShutdownGameDLL()
	{

	}
	GAME_API inline void* GetGameTypeRegistry()
	{
		return &(World::TypeRegistry::Get());
	}
}