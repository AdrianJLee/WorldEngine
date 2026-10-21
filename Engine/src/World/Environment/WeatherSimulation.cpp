#include "wldpch.h"
#include "World/Environment/WeatherSimulation.h"
#include <algorithm>

namespace World
{
	void WeatherSimulation::Update(WeatherSystemComponent& comp, float dt)
	{
		if (dt <= 0.0f)
			return;

		// 降水强度与云层覆盖度目标映射
		float targetPrecip = 0.0f;
		float targetCloud = 0.2f;

		switch (comp.CurrentWeather)
		{
		case WeatherSystemComponent::WeatherType::Clear:
			targetPrecip = 0.0f;
			targetCloud = 0.1f;
			break;
		case WeatherSystemComponent::WeatherType::Overcast:
			targetPrecip = 0.0f;
			targetCloud = 0.7f;
			break;
		case WeatherSystemComponent::WeatherType::Rain:
			targetPrecip = 0.6f;
			targetCloud = 0.85f;
			break;
		case WeatherSystemComponent::WeatherType::Storm:
			targetPrecip = 1.0f;
			targetCloud = 1.0f;
			break;
		case WeatherSystemComponent::WeatherType::Snow:
			targetPrecip = 0.4f;
			targetCloud = 0.8f;
			break;
		}

		// 平滑插值过渡
		const float t = std::clamp(dt * 0.5f, 0.0f, 1.0f);
		comp.PrecipitationIntensity = glm::mix(comp.PrecipitationIntensity, targetPrecip, t);
		comp.CloudCoverage = glm::mix(comp.CloudCoverage, targetCloud, t);

		if (comp.PrecipitationIntensity > 0.05f)
		{
			// 降水累积地面积水与湿润度
			comp.Wetness = std::min(1.0f, comp.Wetness + comp.PrecipitationIntensity * 0.2f * dt);
		}
		else
		{
			// 无雨时，随风速与光照自然风干蒸发
			const float dryRate = 0.02f * (1.0f + comp.WindSpeed * 0.1f);
			comp.Wetness = std::max(0.0f, comp.Wetness - dryRate * dt);
		}
	}
}
