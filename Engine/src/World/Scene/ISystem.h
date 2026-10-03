#pragma once

#include "World/Core/Export.h"
#include "World/Core/Timestep.h"
#include "World/Gameplay/Runtime/SystemRegistry.h"
#include <string_view>
#include <string>
#include <vector>

namespace World
{
	class Scene;

	enum class SystemKind : uint8_t
	{
		Pipeline = 0,   // 常规每帧/每固定步推进
		Interval,       // 定时间隔节流推进
		Startup,        // 场景开局执行一次
		Teardown,       // 场景结束执行一次
	};

	class WLD_API ISystem
	{
	public:
		virtual ~ISystem() = default;
		virtual std::string_view Name() const = 0;
		virtual Gameplay::SystemPhase Phase() const { return Gameplay::SystemPhase::Update; }
		virtual SystemKind Kind() const { return Interval() > 0.0f ? SystemKind::Interval : SystemKind::Pipeline; }
		virtual float Interval() const { return 0.0f; }
		virtual bool ParallelSafe() const { return false; }
		virtual std::vector<std::string> After() const { return {}; }
		virtual bool ShouldRun(const Scene& scene) const { (void)scene; return true; }
		virtual void Update(Scene& scene, Timestep dt) = 0;
	};
}
