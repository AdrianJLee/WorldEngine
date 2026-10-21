#pragma once

#include "World/Core/Export.h"
#include "World/Scene/Components.h"

namespace World
{
	class WLD_API WeatherSimulation
	{
	public:
		// 推进动态天气降水积分与地表自然蒸发微积分
		static void Update(WeatherSystemComponent& comp, float dt);
	};
}
