#include "wldpch.h"
#include "World/Scene/Systems/WeatherSystem.h"
#include "World/Scene/Scene.h"
#include "World/Scene/Components.h"

namespace World
{
	void WeatherSystem::DeclareAccess(SystemAccess& access)
	{
		access.Write(entt::type_hash<WeatherSystemComponent>::value());
		access.Read(entt::type_hash<WeatherVolumeComponent>::value());
		access.Read(entt::type_hash<TransformComponent>::value());
		access.Read(entt::type_hash<CameraComponent>::value());
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
		// 寻找场景中主相机位置以做生物群系天气体积采样
		glm::vec3 cameraPos(0.0f);
		const auto& reg = scene.GetRegistry();
		for (auto entity : reg.view<CameraComponent, TransformComponent>())
		{
			const auto& cam = reg.get<CameraComponent>(entity);
			if (cam.Primary)
			{
				cameraPos = reg.get<TransformComponent>(entity).Location;
				break;
			}
		}

		return GetAtmosphereAt(scene, cameraPos);
	}

	WeatherAtmosphereParams WeatherSystem::GetAtmosphereAt(const Scene& scene, const glm::vec3& worldPos)
	{
		const auto& reg = scene.GetRegistry();
		const WeatherSystemComponent* globalWeather = nullptr;

		for (auto entity : reg.view<WeatherSystemComponent>())
		{
			if (!scene.IsEntityInActiveDataLayers(entity))
				continue;
			globalWeather = &reg.get<WeatherSystemComponent>(entity);
			break;
		}

		if (!globalWeather)
			return WeatherAtmosphereParams{};

		// 收集场景中所有生效的局部天气体积
		std::vector<std::pair<glm::mat4, WeatherVolumeComponent>> volumes;
		for (auto entity : reg.view<WeatherVolumeComponent>())
		{
			if (!scene.IsEntityInActiveDataLayers(entity))
				continue;
			const auto& vol = reg.get<WeatherVolumeComponent>(entity);
			if (!vol.Enabled)
				continue;

			glm::mat4 transform(1.0f);
			if (reg.all_of<TransformComponent>(entity))
				transform = reg.get<TransformComponent>(entity).GetTransform();

			volumes.push_back({ transform, vol });
		}

		return WeatherSimulation::EvaluateLocalWeather(*globalWeather, worldPos, volumes, static_cast<float>(scene.GetTime().ElapsedSeconds));
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
