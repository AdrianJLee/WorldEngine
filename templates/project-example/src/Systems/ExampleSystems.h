#pragma once

#include "World/Core/Log.h"
#include "World/Core/Timestep.h"
#include "World/Scene/Components.h"
#include "World/Scene/ISystem.h"
#include "World/Scene/Query.h"
#include "World/Scene/Scene.h"
#include "World/Scene/Systems/TransformSystem.h"

#include <cstddef>
#include <string_view>

namespace World::Game
{
	// ============================================================================
	// 系统 = 逻辑。
	//
	// 系统实例只在一次运行时内存在:Play/Simulate 开始时由 src/GameProject.cpp 的
	// AttachProjectSystems 挂上,停止时由 DetachProjectSystems 摘掉 —— 不要在这里
	// 存跨场景/跨运行的长期状态。
	//
	// 写一个系统的骨架:
	//   1) 继承 World::ISystem,实现 Name()(稳定字符串,Detach 时逐字匹配)与
	//      Update(Scene&, Timestep);
	//   2) Phase() 选帧管线阶段:PreFixed -> Fixed -> Update -> Late -> PreRender(默认 Update);
	//   3) ParallelSafe():true = 无共享可变状态、可与其它系统并行;false = 主线程独占
	//      (示例用 false,与 Lua 系统脚本、物理一致;要开并行先确认不碰共享数据);
	//   4) 在 src/GameProject.cpp 的 Attach 里 scene.RegisterSystem<T>(),
	//      在 Detach 里 scene.UnregisterSystem<T>()(类型安全对称,无需手写字符串)。
	// ============================================================================

	// 查询引擎已有的两个组件(Transform + Velocity),把"会动"的实体绕 Z 轴转起来 —— 一件在
	// Play 里一眼可见的事,演示 scene.Query<...>().Each(...) 的实际用法。
	class ExampleSpinSystem : public ISystem
	{
	public:
		std::string_view Name() const override { return "ExampleSpinSystem"; }
		Gameplay::SystemPhase Phase() const override { return Gameplay::SystemPhase::Update; }
		bool ParallelSafe() const override { return false; }

		void Update(Scene& scene, Timestep dt) override
		{
			const float delta = dt.GetSeconds();
			if (delta <= 0.0f)
				return;

			scene.Query<TransformComponent, const VelocityComponent>()
				.Each([delta](TransformComponent& transform, const VelocityComponent& velocity) {
					// 只转"有速度"的实体;静止实体保持不动。
					if (velocity.Linear == glm::vec3(0.0f) && velocity.Angular == glm::vec3(0.0f))
						return;
					transform.Rotation.z += kSpinRadiansPerSecond * delta;
					TransformSystem::Recalculate(transform);
				});
		}

	private:
		// 绕 Z 轴的固定角速度(弧度/秒);示例用常量,真实项目可以改从组件字段取。
		static constexpr float kSpinRadiansPerSecond = 0.75f;
	};

	// 每帧数一次场景实体数,只在第一次和数量变化时用 WLD_INFO 打一行(避免刷屏),
	// 演示"系统可以持有自己的运行期状态"。
	class ExampleEntityCountSystem : public ISystem
	{
	public:
		std::string_view Name() const override { return "ExampleEntityCountSystem"; }
		Gameplay::SystemPhase Phase() const override { return Gameplay::SystemPhase::Update; }
		bool ParallelSafe() const override { return false; }

		void Update(Scene& scene, Timestep dt) override
		{
			(void)dt;
			// 每个真实实体都带 UUIDComponent(编辑器的 HierarchyPanel 也用这个口径枚举实体)。
			const std::size_t count = scene.Query<UUIDComponent>().Count();
			if (m_HasLogged && count == m_LastCount)
				return;
			m_HasLogged = true;
			m_LastCount = count;
			WLD_INFO("[ExampleEntityCountSystem] entity count: {}", count);
		}

	private:
		bool m_HasLogged = false;
		std::size_t m_LastCount = 0;
	};
}
