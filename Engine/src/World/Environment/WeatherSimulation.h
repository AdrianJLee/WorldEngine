#pragma once

#include "World/Core/Export.h"
#include "World/Scene/Components.h"

#include <glm/glm.hpp>
#include <string>

namespace World
{
	// 具象化天气配置描述符 (Weather Profile: 用于天气混合演进与游戏玩法机制扩展)
	struct WeatherProfile
	{
		std::string Name { "Clear Sky" };
		float CloudCoverage { 0.15f };
		float CloudDensity { 1.0f };
		float PrecipitationIntensity { 0.0f };
		float FogDensity { 0.0008f };
		float WindSpeed { 5.0f };
		glm::vec3 WindDirection { 1.0f, 0.0f, 0.2f };
		glm::vec3 AmbientColor { 0.25f, 0.30f, 0.40f };
		glm::vec4 SkyClearColor { 0.35f, 0.55f, 0.85f, 1.0f };
	};

	// 完整天球物理与光照大气输出 (Full Celestial & Atmospheric Parameters)
	struct WeatherAtmosphereParams
	{
		// 1. 太阳与直射日照 (Solar Trajectory & Direct Sunlight)
		glm::vec3 SunLightColor { 1.0f, 0.96f, 0.88f };
		float SunLightIntensity { 1.25f };
		glm::vec3 SunDirection { 0.45f, -0.75f, 0.45f }; // 入射向地面 (-Y)
		float SunElevation { 45.0f };                     // 俯仰角 (degrees)
		float SunAzimuth { 135.0f };                      // 方位角 (degrees)

		// 2. 月球与静谧月夜 (Lunar Trajectory & Moonlight)
		glm::vec3 MoonLightColor { 0.70f, 0.82f, 1.0f };
		float MoonLightIntensity { 0.08f };
		glm::vec3 MoonDirection { -0.45f, 0.75f, -0.45f };
		float MoonElevation { -45.0f };

		// 3. 繁星夜空 (Celestial Starfield)
		float StarVisibility { 0.0f };                    // 星空可见度 [0..1]，日落后天黑且无云时达到 1.0

		// 4. 大气色温与天空背景 (Atmospheric Rayleigh/Mie Scattering & Sky)
		glm::vec3 AmbientColor { 0.25f, 0.30f, 0.40f };
		glm::vec4 SkyClearColor { 0.35f, 0.55f, 0.85f, 1.0f };
		glm::vec4 SkyZenithColor { 0.18f, 0.35f, 0.75f, 1.0f };
		glm::vec4 SkyHorizonColor { 0.55f, 0.65f, 0.85f, 1.0f };
		float FogDensity { 0.0005f };
		glm::vec3 FogColor { 0.6f, 0.7f, 0.85f };

		// 5. 大气云层与动态风漂 (Volumetric Cloud Layer & Wind Drift)
		float CloudCoverage { 0.2f };
		float CloudDensity { 1.0f };
		glm::vec4 CloudColor { 1.0f, 1.0f, 1.0f, 0.6f };
		glm::vec2 CloudDriftOffset { 0.0f, 0.0f };

		// 6. 全局风场矢量 (Global Atmospheric Wind Field)
		glm::vec3 WindVelocity { 5.0f, 0.0f, 0.0f };
	};

	class WLD_API WeatherSimulation
	{
	public:
		// 通用物理天气主循环微积分 (连续昼夜时辰与地表湿润度微积分)
		static void Update(WeatherSystemComponent& comp, float dt);

		// 向指定天气配置平滑插值演进 (框架驱动接口)
		static void BlendTowards(WeatherSystemComponent& comp, const WeatherProfile& targetProfile, float blendAlpha);

		// 通用天球日光、色温与大气光照参数连续解析解
		static WeatherAtmosphereParams ComputeAtmosphereParams(const WeatherSystemComponent& comp, float sceneTime = 0.0f);
	};
}