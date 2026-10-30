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

		glm::vec3 baseSunColor;
		float baseIntensity = 0.0f;
		glm::vec4 baseSkyColor;

		if (sunElevDeg > 35.0f)
		{
			const float t = std::clamp((sunElevDeg - 35.0f) / 30.0f, 0.0f, 1.0f);
			baseSunColor = glm::mix(glm::vec3(1.0f, 0.88f, 0.70f), glm::vec3(1.0f, 0.98f, 0.92f), t);
			baseIntensity = glm::mix(1.0f, 1.35f, t);
			baseSkyColor = glm::mix(glm::vec4(0.35f, 0.55f, 0.82f, 1.0f), glm::vec4(0.25f, 0.50f, 0.85f, 1.0f), t);
		}
		else if (sunElevDeg > 8.0f)
		{
			const float t = std::clamp((sunElevDeg - 8.0f) / 27.0f, 0.0f, 1.0f);
			baseSunColor = glm::mix(glm::vec3(1.0f, 0.65f, 0.35f), glm::vec3(1.0f, 0.88f, 0.70f), t);
			baseIntensity = glm::mix(0.65f, 1.0f, t);
			baseSkyColor = glm::mix(glm::vec4(0.55f, 0.45f, 0.55f, 1.0f), glm::vec4(0.35f, 0.55f, 0.82f, 1.0f), t);
		}
		else if (sunElevDeg > -2.0f)
		{
			const float t = std::clamp((sunElevDeg - (-2.0f)) / 10.0f, 0.0f, 1.0f);
			baseSunColor = glm::mix(glm::vec3(0.95f, 0.35f, 0.15f), glm::vec3(1.0f, 0.65f, 0.35f), t);
			baseIntensity = glm::mix(0.20f, 0.65f, t);
			baseSkyColor = glm::mix(glm::vec4(0.68f, 0.32f, 0.22f, 1.0f), glm::vec4(0.55f, 0.45f, 0.55f, 1.0f), t);
		}
		else
		{
			const float t = std::clamp((-sunElevDeg - 2.0f) / 20.0f, 0.0f, 1.0f);
			baseSunColor = glm::vec3(0.38f, 0.48f, 0.68f);
			baseIntensity = glm::mix(0.15f, 0.06f, t);
			baseSkyColor = glm::mix(glm::vec4(0.12f, 0.15f, 0.25f, 1.0f), glm::vec4(0.04f, 0.06f, 0.12f, 1.0f), t);
		}

		if (sunElevDeg < 5.0f && moonElevDeg > 0.0f)
		{
			const float moonFactor = std::clamp(moonElevDeg / 40.0f, 0.0f, 1.0f);
			params.MoonLightColor = glm::vec3(0.65f, 0.78f, 1.0f);
			params.MoonLightIntensity = 0.08f * moonFactor;
		}
		else
		{
			params.MoonLightIntensity = 0.0f;
		}

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

		glm::vec4 cloudShadeColor;
		if (sunElevDeg > 15.0f)
		{
			const glm::vec4 dayCloud = glm::vec4(1.0f, 1.0f, 1.0f, 0.85f);
			const glm::vec4 stormCloud = glm::vec4(0.25f, 0.27f, 0.32f, 0.95f);
			cloudShadeColor = glm::mix(dayCloud, stormCloud, std::max(precip, cloudCoverage > 0.8f ? 0.6f : 0.0f));
		}
		else if (sunElevDeg > -5.0f)
		{
			cloudShadeColor = glm::vec4(1.0f, 0.60f, 0.38f, 0.90f);
		}
		else
		{
			cloudShadeColor = glm::vec4(0.20f, 0.24f, 0.35f, 0.75f);
		}
		params.CloudCoverage = cloudCoverage;
		params.CloudDensity = std::clamp(comp.CloudDensity, 0.1f, 10.0f);
		params.CloudColor = cloudShadeColor;

		const glm::vec3 windNorm = glm::length(comp.WindDirection) > 1e-3f ? glm::normalize(comp.WindDirection) : glm::vec3(1, 0, 0);
		params.CloudDriftOffset = glm::vec2(windNorm.x, windNorm.z) * (comp.WindSpeed * sceneTime * 0.005f);

		const glm::vec4 overcastSkyColor(0.35f, 0.38f, 0.44f, 1.0f);
		const float cloudSkyBlend = std::clamp(cloudCoverage * 0.75f + precip * 0.25f, 0.0f, 1.0f);
		params.SkyClearColor = glm::mix(baseSkyColor, overcastSkyColor, cloudSkyBlend);

		params.SkyZenithColor = params.SkyClearColor * 0.85f;
		params.SkyHorizonColor = glm::mix(params.SkyClearColor, glm::vec4(0.6f, 0.65f, 0.75f, 1.0f), 0.35f);

		const glm::vec3 baseAmbient = glm::vec3(params.SkyClearColor) * 0.45f;
		params.AmbientColor = baseAmbient;

		const float effectiveFog = std::clamp(baseFog * (1.0f + precip * 2.5f + cloudCoverage * 0.8f), 0.0001f, 0.05f);
		params.FogDensity = effectiveFog;
		params.FogColor = glm::vec3(params.SkyClearColor) * 0.9f;

		params.WindVelocity = windNorm * comp.WindSpeed;

		return params;
	}
}
