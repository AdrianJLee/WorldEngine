#pragma once

#include "World/Core/Export.h"
#include "World/Core/Timestep.h"
#include "World/Scene/ISystem.h"
#include "World/Environment/WeatherSimulation.h"

namespace World
{
	class Scene;

	// 动态天气与全天候环境框架系统 (WeatherSystem Framework)
	// 遵循 PECS 纯数据驱动架构，为游戏提供可插拔的天气运行机制与天球物理演进
	class WLD_API WeatherSystem : public ISystem
	{
	public:
		std::string_view Name() const override { return "WeatherSystem"; }
		Gameplay::SystemPhase Phase() const override { return Gameplay::SystemPhase::Update; }
		bool ParallelSafe() const override { return true; }
		void DeclareAccess(SystemAccess& access) override;
		void Update(Scene& scene, Timestep dt) override;

		// 静态框架查询与控制接口 (供 Gameplay/Script/Sequencer 统一驱动)
		static WeatherAtmosphereParams GetAtmosphere(const Scene& scene);
		static void SetTimeOfDay(Scene& scene, float hours);
		static void TransitionTo(Scene& scene, const WeatherProfile& targetProfile, float durationSeconds);
	};
}