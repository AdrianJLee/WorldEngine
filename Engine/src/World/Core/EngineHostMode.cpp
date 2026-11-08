#include "wldpch.h"
#include "World/Core/EngineHostMode.h"
#include <atomic>

namespace World
{
	namespace
	{
		std::atomic<EngineHostMode> s_HostMode{ EngineHostMode::Runtime };
	}

	void EngineEnvironment::SetHostMode(EngineHostMode mode)
	{
		s_HostMode.store(mode, std::memory_order_release);
	}

	EngineHostMode EngineEnvironment::GetHostMode()
	{
		return s_HostMode.load(std::memory_order_acquire);
	}

	bool EngineEnvironment::IsEditor()
	{
		return GetHostMode() == EngineHostMode::Editor;
	}

	bool EngineEnvironment::IsRuntime()
	{
		return GetHostMode() == EngineHostMode::Runtime;
	}

	bool EngineEnvironment::IsPackaged()
	{
		return GetHostMode() == EngineHostMode::Packaged;
	}

	bool EngineEnvironment::IsHeadless()
	{
		return GetHostMode() == EngineHostMode::Headless;
	}

	bool EngineEnvironment::IsTool()
	{
		return GetHostMode() == EngineHostMode::Tool;
	}

	bool EngineEnvironment::IsPureGameRuntime()
	{
		const EngineHostMode mode = GetHostMode();
		return mode == EngineHostMode::Runtime || mode == EngineHostMode::Packaged;
	}

	std::string_view EngineEnvironment::HostModeToString(EngineHostMode mode)
	{
		switch (mode)
		{
		case EngineHostMode::Editor:   return "Editor";
		case EngineHostMode::Runtime:  return "Runtime";
		case EngineHostMode::Packaged: return "Packaged";
		case EngineHostMode::Headless: return "Headless";
		case EngineHostMode::Tool:     return "Tool";
		default:                       return "Unknown";
		}
	}

	std::string_view EngineEnvironment::GetHostModeName()
	{
		return HostModeToString(GetHostMode());
	}
}
