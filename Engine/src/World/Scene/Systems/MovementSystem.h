#pragma once

#include "World/Core/Export.h"
#include "World/Core/Timestep.h"
#include "World/Scene/ISystem.h"

namespace World
{
	class Scene;

	// Pure ECS: 空间位移系统
	// 批量遍历拥有 TransformComponent 与 VelocityComponent 的实体，并按帧推进位置与角度
	class WLD_API MovementSystem : public ISystem
	{
	public:
		std::string_view Name() const override { return "MovementSystem"; }
		Gameplay::SystemPhase Phase() const override { return Gameplay::SystemPhase::Update; }
		bool ParallelSafe() const override { return true; }
		void Update(Scene& scene, Timestep dt) override;
	};
}
