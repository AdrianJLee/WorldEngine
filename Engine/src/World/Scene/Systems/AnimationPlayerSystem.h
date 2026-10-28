#pragma once

#include "World/Core/Export.h"
#include <entt.hpp>

namespace World
{
	class Scene;

	class WLD_API AnimationPlayerSystem
	{
	public:
		// 驱动带 AnimationPlayerComponent 且无 AnimGraphComponent 的实体：推进动画时间，直写 SkinnedMeshRendererComponent::RuntimePalette
		static void Update(Scene& scene, float deltaSeconds);
	};
}
