#include "wldpch.h"
#include "World/Environment/WeatherSimulation.h"
#include <algorithm>
#include <cmath>

namespace World
{
	namespace
	{
		constexpr float kPi = 3.14159265358979323846f;
	}

	void WeatherSimulation::Update(WeatherSystemComponent& comp, float dt)
	{
		if (dt <= 0.0f)
			return;

		if (comp.AutoCycleTime)
		{
			const float speed = std::clamp(comp.DayCycleSpeed, 0.001f, 10.0f);
			comp.TimeOfDay = std::fmod(comp.TimeOfDay + dt * speed, 24.0f);
			if (comp.TimeOfDay < 0.0f)
				comp.TimeOfDay += 24.0f;
		}

		if (comp.CurrentWeather != WeatherSystemComponent::WeatherType::Custom)
		{
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
			default:
				break;
			}

			const float t = std::clamp(dt * 0.5f, 0.0f, 1.0f);
			comp.PrecipitationIntensity = glm::mix(comp.PrecipitationIntensity, targetPrecip, t);
			comp.CloudCoverage = glm::mix(comp.CloudCoverage, targetCloud, t);
		}

		if (comp.PrecipitationIntensity > 0.05f)
		{
			comp.Wetness = std::min(1.0f, comp.Wetness + comp.PrecipitationIntensity * 0.20f * dt);
		}
		else
		{
			const float dryRate = 0.02f * (1.0f + comp.WindSpeed * 0.1f);
			comp.Wetness = std::max(0.0f, comp.Wetness - dryRate * dt);
		}
	}

	void WeatherSimulation::BlendTowards(WeatherSystemComponent& comp, const WeatherProfile& targetProfile, float blendAlpha)
	{
		const float t = std::clamp(blendAlpha, 0.0f, 1.0f);
		comp.CloudCoverage = glm::mix(comp.CloudCoverage, targetProfile.CloudCoverage, t);
		comp.CloudDensity = glm::mix(comp.CloudDensity, targetProfile.CloudDensity, t);
		comp.PrecipitationIntensity = glm::mix(comp.PrecipitationIntensity, targetProfile.PrecipitationIntensity, t);
		comp.FogDensity = glm::mix(comp.FogDensity, targetProfile.FogDensity, t);
		comp.WindSpeed = glm::mix(comp.WindSpeed, targetProfile.WindSpeed, t);
		const glm::vec3 targetDir = glm::length(targetProfile.WindDirection) > 1e-3f ? glm::normalize(targetProfile.WindDirection) : glm::vec3(1, 0, 0);
		comp.WindDirection = glm::normalize(glm::mix(comp.WindDirection, targetDir, t));
	}

	WeatherAtmosphereParams WeatherSimulation::ComputeAtmosphereParams(const WeatherSystemComponent& comp, float sceneTime)
	{
		WeatherAtmosphereParams params;

		const float tod = std::clamp(comp.TimeOfDay, 0.0f, 24.0f);
		const float solarPhase = ((tod - 6.0f) / 12.0f) * kPi;
		const float sunElevDeg = std::sin(solarPhase) * 65.0f;
		const float sunAzimDeg = (tod / 24.0f) * 360.0f;

		params.SunElevation = sunElevDeg;
		params.SunAzimuth = sunAzimDeg;

		const float elevRad = glm::radians(sunElevDeg);
		const float azimRad = glm::radians(sunAzimDeg);

		const glm::vec3 sunDir = glm::normalize(glm::vec3(
			std::cos(elevRad) * std::sin(azimRad),
			-std::sin(std::max(elevRad, glm::radians(-15.0f))),
			std::cos(elevRad) * std::cos(azimRad)
		));
		params.SunDirection = sunDir;

		const float moonElevDeg = -sunElevDeg;
		const float moonAzimDeg = std::fmod(sunAzimDeg + 180.0f, 360.0f);
		params.MoonElevation = moonElevDeg;

		const float moonElevRad = glm::radians(moonElevDeg);
		const float moonAzimRad = glm::radians(moonAzimDeg);
		const glm::vec3 moonDir = glm::normalize(glm::vec3(
			std::cos(moonElevRad) * std::sin(moonAzimRad),
			-std::sin(std::max(moonElevRad, glm::radians(-15.0f))),
			std::cos(moonElevRad) * std::cos(moonAzimRad)
		));
		params.MoonDirection = moonDir;

		// 1. 全域连续平滑昼夜与霞光微积分分析解 (消灭任何时间点分段突变)
		const float sinElev = std::sin(elevRad);
		const float daySmooth = glm::smoothstep(0.0f, 0.20f, sinElev); // -6°..+6° 晨昏平滑过渡
		const float twilightFactor = std::exp(-(sinElev * sinElev) / (2.0f * 0.08f * 0.08f)); // 高斯晚霞因子
		const float noonFactor = glm::smoothstep(0.20f, 0.85f, sinElev);

		// 2. 连续太阳光谱色温与光照强度
		const glm::vec3 sunNoonColor(1.0f, 0.98f, 0.92f);
		const glm::vec3 sunMidColor(1.0f, 0.85f, 0.65f);
		const glm::vec3 sunSunsetColor(1.0f, 0.48f, 0.18f);
		const glm::vec3 sunNightColor(0.25f, 0.35f, 0.55f);

		glm::vec3 baseSunColor = glm::mix(sunMidColor, sunNoonColor, noonFactor);
		baseSunColor = glm::mix(baseSunColor, sunSunsetColor, twilightFactor);
		baseSunColor = glm::mix(sunNightColor, baseSunColor, glm::smoothstep(-0.12f, -0.01f, sinElev));
		const float baseIntensity = glm::mix(0.0f, 1.35f, daySmooth);

		// 3. 连续天幕背景颜色
		const glm::vec4 skyNoonColor(0.22f, 0.48f, 0.85f, 1.0f);
		const glm::vec4 skyMidColor(0.35f, 0.55f, 0.82f, 1.0f);
		const glm::vec4 skySunsetColor(0.68f, 0.38f, 0.25f, 1.0f);
		const glm::vec4 skyNightColor(0.03f, 0.05f, 0.10f, 1.0f);

		glm::vec4 baseSkyColor = glm::mix(skyMidColor, skyNoonColor, noonFactor);
		baseSkyColor = glm::mix(baseSkyColor, skySunsetColor, twilightFactor * 0.85f);
		baseSkyColor = glm::mix(skyNightColor, baseSkyColor, daySmooth);

		// 4. 连续月光平滑过渡 (太阳落下后平滑升起，绝对无断崖跳闪)
		const float moonSinElev = std::sin(moonElevRad);
		const float moonSmooth = glm::smoothstep(0.0f, 0.20f, moonSinElev);
		params.MoonLightColor = glm::vec3(0.65f, 0.78f, 1.0f);
		params.MoonLightIntensity = 0.08f * moonSmooth;

		float cloudCoverage = std::clamp(comp.CloudCoverage, 0.0f, 1.0f);
		float precip = std::clamp(comp.PrecipitationIntensity, 0.0f, 1.0f);
		float baseFog = std::clamp(comp.FogDensity, 0.0001f, 0.05f);

		switch (comp.CurrentWeather)
		{
		case WeatherSystemComponent::WeatherType::Clear:
			cloudCoverage = std::min(cloudCoverage, 0.10f);
			baseFog = std::min(baseFog, 0.0005f);
			break;
		case WeatherSystemComponent::WeatherType::Overcast:
			cloudCoverage = std::max(cloudCoverage, 0.85f);
			baseFog = std::max(baseFog, 0.0015f);
			break;
		case WeatherSystemComponent::WeatherType::Snow:
			precip = std::max(precip, 0.30f);
			cloudCoverage = std::clamp(cloudCoverage, 0.40f, 0.65f);
			baseFog = std::max(baseFog, 0.0035f);
			break;
		case WeatherSystemComponent::WeatherType::Rain:
			precip = std::max(precip, 0.60f);
			cloudCoverage = std::max(cloudCoverage, 0.88f);
			baseFog = std::max(baseFog, 0.0055f);
			break;
		case WeatherSystemComponent::WeatherType::Storm:
			precip = std::max(precip, 0.95f);
			cloudCoverage = std::max(cloudCoverage, 0.95f);
			baseFog = std::max(baseFog, 0.0120f);
			break;
		default:
			break;
		}

		const float nightFactor = std::clamp((-sunElevDeg - 2.0f) / 15.0f, 0.0f, 1.0f);
		const float cloudClearFactor = std::clamp(1.0f - cloudCoverage * 1.25f, 0.0f, 1.0f);
		params.StarVisibility = nightFactor * cloudClearFactor;

		const float sunAttenuation = (1.0f - cloudCoverage * 0.70f) * (1.0f - precip * 0.25f);
		params.SunLightColor = baseSunColor;
		params.SunLightIntensity = baseIntensity * sunAttenuation;

		// 5. 连续云层光照色调
		const glm::vec4 dayCloudColor(1.0f, 1.0f, 1.0f, 0.88f);
		const glm::vec4 sunsetCloudColor(1.0f, 0.58f, 0.32f, 0.92f);
		const glm::vec4 nightCloudColor(0.18f, 0.22f, 0.32f, 0.78f);
		const glm::vec4 stormCloudColor(0.25f, 0.27f, 0.32f, 0.95f);

		glm::vec4 cloudShadeColor = glm::mix(dayCloudColor, sunsetCloudColor, twilightFactor);
		cloudShadeColor = glm::mix(nightCloudColor, cloudShadeColor, daySmooth);
		const float stormBlend = std::max(precip, cloudCoverage > 0.8f ? 0.65f : 0.0f);
		cloudShadeColor = glm::mix(cloudShadeColor, stormCloudColor, stormBlend);
		params.CloudCoverage = cloudCoverage;
		params.CloudDensity = std::clamp(comp.CloudDensity, 0.1f, 10.0f);
		params.CloudColor = cloudShadeColor;
		const glm::vec3 windNorm = glm::length(comp.WindDirection) > 1e-3f ? glm::normalize(comp.WindDirection) : glm::vec3(1, 0, 0);
		params.CloudDriftOffset = glm::vec2(windNorm.x, windNorm.z) * (comp.WindSpeed * sceneTime * 0.005f);

		const glm::vec4 overcastSkyColor(0.35f, 0.38f, 0.44f, 1.0f);
		const float cloudSkyBlend = std::clamp(cloudCoverage * 0.75f + precip * 0.25f, 0.0f, 1.0f);
		params.SkyClearColor = glm::mix(baseSkyColor, overcastSkyColor, cloudSkyBlend);

		params.SkyZenithColor = glm::vec4(glm::vec3(params.SkyClearColor) * 0.85f, 1.0f);
		params.SkyHorizonColor = glm::vec4(glm::mix(glm::vec3(params.SkyClearColor), glm::vec3(0.6f, 0.65f, 0.75f), 0.35f), 1.0f);
		params.SkyGroundColor = glm::vec4(glm::vec3(0.12f, 0.15f, 0.20f) * (daySmooth * 0.7f + 0.3f), 1.0f);
		params.SunDiscSize = 0.045f;
		params.MoonDiscSize = 0.038f;
		params.CloudScale = 1.25f;
		params.CloudSoftness = 0.5f;

		const glm::vec3 baseAmbient = glm::vec3(params.SkyClearColor) * 0.45f;
		params.AmbientColor = baseAmbient;

		const float effectiveFog = std::clamp(baseFog * (1.0f + precip * 2.5f + cloudCoverage * 0.8f), 0.0001f, 0.05f);
		params.FogDensity = effectiveFog;
		params.FogColor = glm::vec3(params.SkyClearColor) * 0.9f;

		params.WindVelocity = windNorm * comp.WindSpeed;

		return params;
	}
}
