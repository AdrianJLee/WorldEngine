#include "wldpch.h"
#include "World/Scene/Systems/WeatherSystem.h"
#include "World/Scene/Scene.h"
#include "World/Scene/Components.h"

namespace World
{
	void WeatherSystem::DeclareAccess(SystemAccess& access)
	{
		access.Write(entt::type_hash<WeatherSystemComponent>::value());
		access.Read(entt::type_hash<TransformComponent>::value());
	}

	void WeatherSystem::Update(Scene& scene, Timestep dt)
	{
		auto& reg = const_cast<entt::registry&>(static_cast<const Scene&>(scene).GetRegistry());
		for (auto entity : reg.view<WeatherSystemComponent>())
		{
			if (!scene.IsEntityInActiveDataLayers(entity))
				continue;
			auto& comp = reg.get<WeatherSystemComponent>(entity);
			WeatherSimulation::Update(comp, dt.GetSeconds());
		}
	}

	WeatherAtmosphereParams WeatherSystem::GetAtmosphere(const Scene& scene)
	{
		const auto& reg = scene.GetRegistry();
		for (auto entity : reg.view<WeatherSystemComponent>())
		{
			if (!scene.IsEntityInActiveDataLayers(entity))
				continue;
			const auto& comp = reg.get<WeatherSystemComponent>(entity);
			return WeatherSimulation::ComputeAtmosphereParams(comp);
		}
		return WeatherAtmosphereParams{};
	}

	void WeatherSystem::SetTimeOfDay(Scene& scene, float hours)
	{
		auto& reg = const_cast<entt::registry&>(static_cast<const Scene&>(scene).GetRegistry());
		for (auto entity : reg.view<WeatherSystemComponent>())
		{
			auto& comp = reg.get<WeatherSystemComponent>(entity);
			comp.TimeOfDay = std::fmod(hours + 24.0f, 24.0f);
		}
	}

	void WeatherSystem::TransitionTo(Scene& scene, const WeatherProfile& targetProfile, float durationSeconds)
	{
		auto& reg = const_cast<entt::registry&>(static_cast<const Scene&>(scene).GetRegistry());
		for (auto entity : reg.view<WeatherSystemComponent>())
		{
			auto& comp = reg.get<WeatherSystemComponent>(entity);
			WeatherSimulation::BlendTowards(comp, targetProfile, durationSeconds > 0.0f ? (1.0f / durationSeconds) : 1.0f);
		}
	}
}