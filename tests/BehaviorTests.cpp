// P2a W10-1(D3):最小 C++ IBehavior 示例 —— 固定步长下让实体沿 Y 轴上下浮动。
// 这份文件同时是"行为接口怎么用"的活文档:C++ 与 Luau 行为实现同一组生命周期(P2a §3.5)。
#include "World/Core/WorldContext.h"
#include "World/Gameplay/Behavior.h"
#include "World/Scene/Components.h"
#include "World/Scene/Entity.h"
#include "World/Scene/Scene.h"

#include <cmath>
#include <cstdio>
#include <stdexcept>
#include <string>

namespace
{
	void Check(bool condition, const char* expression, int line)
	{
		if (!condition)
			throw std::runtime_error(std::string("line ") + std::to_string(line) + ": " + expression);
	}
#define CHECK(expression) Check(static_cast<bool>(expression), #expression, __LINE__)

	// 示例行为:固定步长(权威模拟)里按正弦上下浮动,幅度 0.5、频率 2 rad/s。
	class FloatExampleBehavior final : public World::Gameplay::IBehavior
	{
	public:
		static constexpr float Amplitude = 0.5f;
		static constexpr float AngularSpeed = 2.0f;

		const World::Gameplay::BehaviorDesc& GetDesc() const override
		{
			// StableId 用于存档/热重载定位:不要用 C++ 类型名,便于重命名与迁移。
			static const World::Gameplay::BehaviorDesc desc {
				"game", "example.float", 1
			};
			return desc;
		}

		void OnCreate(World::Entity self) override
		{
			(void)self;
			m_Time = 0.0f;
			++CreateCount;
		}

		void OnFixedUpdate(World::Entity self, World::Timestep dt) override
		{
			m_Time += static_cast<float>(dt.GetSeconds());
			if (!self.HasComponent<World::TransformComponent>())
				return;
			World::TransformComponent& transform = self.GetComponent<World::TransformComponent>();
			transform.Location.y = std::sin(m_Time * AngularSpeed) * Amplitude;
			transform.RecalculateTransform();
		}

		void OnDestroy(World::Entity self) override
		{
			(void)self;
			++DestroyCount;
		}

		float Time() const { return m_Time; }
		int CreateCount = 0;
		int DestroyCount = 0;

	private:
		float m_Time = 0.0f;
	};
}

int main()
{
	try
	{
		using namespace World;

		WorldContext context;
		Scene scene(context);

		entt::registry& registry = scene.GetRegistry();
		const entt::entity handle = registry.create();
		registry.emplace<UUIDComponent>(handle, UUID());
		registry.emplace<TransformComponent>(handle);
		Entity entity { &scene, handle };

		FloatExampleBehavior behavior;
		CHECK(behavior.GetDesc().StableId == "example.float");
		CHECK(behavior.GetDesc().ModuleId == "game");
		CHECK(behavior.GetDesc().SchemaVersion == 1);

		behavior.OnCreate(entity);
		CHECK(behavior.CreateCount == 1);
		CHECK(std::fabs(registry.get<TransformComponent>(handle).Location.y) < 1e-6f);

		// 固定步长推进:60 Hz 下走满 1 秒(60 步)。
		const Timestep step(1.0 / 60.0);
		for (int i = 0; i < 60; ++i)
			behavior.OnFixedUpdate(entity, step);

		CHECK(std::fabs(behavior.Time() - 1.0f) < 1e-5f);
		// 确定性:同一组输入步数 → 同一结果(sin(2.0) * 0.5)。
		CHECK(std::fabs(registry.get<TransformComponent>(handle).Location.y
			- std::sin(FloatExampleBehavior::AngularSpeed * 1.0f) * FloatExampleBehavior::Amplitude) < 1e-5f);

		// 量级检查:中途确实离开过原点(不是"看起来在动但没动")。
		behavior.OnCreate(entity);
		float peak = 0.0f;
		for (int i = 0; i < 30; ++i)
		{
			behavior.OnFixedUpdate(entity, step);
			peak = std::max(peak, std::fabs(registry.get<TransformComponent>(handle).Location.y));
		}
		CHECK(peak > 0.4f && peak <= FloatExampleBehavior::Amplitude + 1e-4f);

		behavior.OnDestroy(entity);
		CHECK(behavior.DestroyCount == 1);

		std::printf("World.Behavior: all checks passed\n");
		return 0;
	}
	catch (const std::exception& error)
	{
		std::fprintf(stderr, "World.Behavior FAILED: %s\n", error.what());
		return 1;
	}
}
