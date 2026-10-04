// P6:固定步长 → 渲染插值的回归。
//
// 覆盖:
//   ① 插值数学:端点(alpha=0/1 逐位回到输入)、中点位置、旋转走 slerp 方向正确、带缩放时缩放不变;
//   ② 权威数据不动:开/关插值跑同一固定步序列,物理位姿与 Transform 逐位相同;
//   ③ 插值状态记录:开启后每个物理实体有 PhysicsInterpolationState,关闭时**不产生**该状态;
//   ④ alpha 来源:GameApp 的累加器余量换算正确(固定步整数倍 ⇒ 0;半步 ⇒ 0.5)。
#include "wldpch.h"
#include "World/Core/WorldContext.h"
#include "World/Gameplay/Runtime/GameApp.h"
#include "World/Physics/PhysicsEvents.h"
#include "World/Renderer/TransformInterpolation.h"
#include "World/Scene/Components.h"
#include "World/Scene/Entity.h"
#include "World/Scene/Scene.h"

#include <box2d/box2d.h>

#include <glm/gtc/quaternion.hpp>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <stdexcept>
#include <string>

namespace
{
	using namespace World;

	void Check(bool condition, const char* expression, int line)
	{
		if (!condition)
			throw std::runtime_error(std::string("line ") + std::to_string(line) + ": " + expression);
	}
#define CHECK(expression) Check(static_cast<bool>(expression), #expression, __LINE__)

	constexpr float kFixedStep = 1.0f / 60.0f;

	WorldContext& TestContext()
	{
		static WorldContext context;
		return context;
	}

	bool NearlyEqual(const glm::mat4& a, const glm::mat4& b, float epsilon = 1e-5f)
	{
		for (int column = 0; column < 4; ++column)
			for (int row = 0; row < 4; ++row)
				if (std::fabs(a[column][row] - b[column][row]) > epsilon) return false;
		return true;
	}

	Entity AddBox2D(Scene& scene, const char* name, const glm::vec3& location, RigidBody2DComponent::BodyType type,
		const glm::vec2& halfExtents)
	{
		Entity entity = Entity::CreateEntity(&scene, name);
		entity.AddComponent<TransformComponent>(location, glm::vec3(0.0f), glm::vec3(1.0f));
		RigidBody2DComponent& body = entity.AddComponent<RigidBody2DComponent>();
		body.Type = type;
		BoxCollider2DComponent& collider = entity.AddComponent<BoxCollider2DComponent>();
		collider.Size = halfExtents;
		collider.Restitution = 0.0f;
		return entity;
	}

	// ① 插值数学(纯函数,独立于物理)。
	void InterpolationMathIsCorrect()
	{
		const glm::mat4 start = glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 10.0f, 0.0f));
		const glm::mat4 end = glm::translate(glm::mat4(1.0f), glm::vec3(4.0f, 2.0f, 0.0f));

		// 端点必须逐位回到输入(否则 alpha=0/1 会引入浮点噪声,基线就不"逐字节"了)。
		CHECK(InterpolateRigidTransform(start, end, 0.0f) == start);
		CHECK(InterpolateRigidTransform(start, end, 1.0f) == end);
		CHECK(InterpolateRigidTransform(start, end, -3.0f) == start);   // 越界夹取
		CHECK(InterpolateRigidTransform(start, end, 7.0f) == end);

		// 中点:位置线性 ⇒ 正好是平均值。
		const glm::mat4 middle = InterpolateRigidTransform(start, end, 0.5f);
		CHECK(NearlyEqual(middle, glm::translate(glm::mat4(1.0f), glm::vec3(2.0f, 6.0f, 0.0f))));

		// 旋转:绕 Y 轴 90° 的半程应是 45°,且插值方向正确(不是反向绕)。
		const glm::mat4 rotatedEnd = glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 0.0f, 0.0f))
			* glm::mat4_cast(glm::angleAxis(glm::radians(90.0f), glm::vec3(0.0f, 1.0f, 0.0f)));
		const glm::mat4 halfRotation = InterpolateRigidTransform(glm::mat4(1.0f), rotatedEnd, 0.5f);
		const glm::vec3 forward = glm::vec3(halfRotation * glm::vec4(0.0f, 0.0f, 1.0f, 0.0f));
		// 绕 Y 转 45° 后,前向向量 ≈ (sin45, 0, cos45)。
		CHECK(std::fabs(forward.x - 0.70710678f) < 1e-4f);
		CHECK(std::fabs(forward.z - 0.70710678f) < 1e-4f);

		// 带缩放:缩放取 current(物理不改缩放),位置仍线性插值。
		glm::mat4 scaledEnd = glm::translate(glm::mat4(1.0f), glm::vec3(4.0f, 0.0f, 0.0f));
		scaledEnd[0] *= 3.0f;
		scaledEnd[1] *= 3.0f;
		scaledEnd[2] *= 3.0f;
		const glm::mat4 scaledMiddle = InterpolateRigidTransform(glm::mat4(1.0f), scaledEnd, 0.5f);
		CHECK(std::fabs(glm::length(glm::vec3(scaledMiddle[0])) - 3.0f) < 1e-4f);
		CHECK(std::fabs(scaledMiddle[3][0] - 2.0f) < 1e-5f);
	}

	// ② 权威数据不动:开/关插值跑同一固定步序列,位姿逐位相同。
	void InterpolationDoesNotChangeAuthoritativeState()
	{
		const auto run = [](bool interpolation) -> std::pair<glm::vec2, glm::mat4>
		{
			Ref<Scene> scene = CreateRef<Scene>(TestContext());
			Entity ground = AddBox2D(*scene, "Ground", { 0.0f, 0.0f, 0.0f }, RigidBody2DComponent::BodyType::Static, { 2.0f, 0.5f });
			Entity box = AddBox2D(*scene, "Box", { 0.0f, 4.0f, 0.0f }, RigidBody2DComponent::BodyType::Dynamic, { 0.25f, 0.25f });
			(void)ground;
			scene->SetPhysicsInterpolationEnabled(interpolation);
			scene->OnRuntimeStart();
			for (int step = 0; step < 120; ++step)
			{
				// 模拟宿主:每帧推一个 alpha(真实宿主由 GameApp 提供)。
				scene->SetFixedStepAlpha(static_cast<float>(step % 2) * 0.5f);
				scene->OnFixedUpdate(Timestep(kFixedStep));
				scene->OnUpdateRuntime(Timestep(kFixedStep));
			}
			const b2Vec2 position = b2Body_GetPosition(scene->GetPhysicsBody2D(box));
			const glm::mat4 transform = box.GetComponent<TransformComponent>().GetLocalMatrix();
			scene->OnRuntimeStop();
			return { glm::vec2(position.x, position.y), transform };
		};

		const auto withInterpolation = run(true);
		const auto withoutInterpolation = run(false);
		std::printf("[info] authoritative: y %.9g vs %.9g\n",
			static_cast<double>(withInterpolation.first.y), static_cast<double>(withoutInterpolation.first.y));
		CHECK(withInterpolation.first == withoutInterpolation.first);          // 逐位
		CHECK(withInterpolation.second == withoutInterpolation.second);        // 逐位
	}

	// ③ 插值状态记录:开启才产生,关闭不产生。
	void InterpolationStateIsRecordedOnlyWhenEnabled()
	{
		const auto stateCount = [](bool interpolation) -> int
		{
			Ref<Scene> scene = CreateRef<Scene>(TestContext());
			AddBox2D(*scene, "Box", { 0.0f, 4.0f, 0.0f }, RigidBody2DComponent::BodyType::Dynamic, { 0.25f, 0.25f });
			scene->SetPhysicsInterpolationEnabled(interpolation);
			scene->OnRuntimeStart();
			for (int step = 0; step < 5; ++step)
				scene->OnFixedUpdate(Timestep(kFixedStep));
			// 只读遍历走 const registry(可变版 GetRegistry() 有运行态结构写保护 —— 这是对的)。
			const Scene& readOnlyScene = *scene;
			int count = 0;
			for (const entt::entity entity : readOnlyScene.GetRegistry().view<PhysicsInterpolationState>())
			{
				(void)entity;
				++count;
			}
			scene->OnRuntimeStop();
			return count;
		};

		const int enabled = stateCount(true);
		const int disabled = stateCount(false);
		std::printf("[info] interpolation state: enabled=%d disabled=%d\n", enabled, disabled);
		CHECK(enabled >= 1);
		CHECK(disabled == 0);
	}

	// ④ alpha 来源:累加器余量换算。
	void GameAppAlphaMatchesAccumulatorRemainder()
	{
		Gameplay::GameAppDesc desc;
		desc.ProjectId = "peks-p6";
		desc.FixedStepHz = 60;
		desc.MaxFixedStepsPerFrame = 4;
		Gameplay::GameApp::Create(desc);
		Gameplay::GameApp& app = Gameplay::GameApp::Get();
		app.SetPhaseCallbacks(Gameplay::GameApp::PhaseCallback(), Gameplay::GameApp::PhaseCallback(),
			Gameplay::GameApp::PhaseCallback());

		app.Tick(Timestep(1.0f / 60.0f));                       // 正好一步 ⇒ 余量 0
		const float exact = app.LastFixedStepAlpha();
		app.Tick(Timestep(1.0f / 120.0f));                     // 半步 ⇒ 余量 0.5
		const float half = app.LastFixedStepAlpha();
		std::printf("[info] alpha: exact=%.6f half=%.6f\n", static_cast<double>(exact), static_cast<double>(half));
		CHECK(std::fabs(exact) < 1e-5f);
		CHECK(std::fabs(half - 0.5f) < 1e-3f);

		Gameplay::GameApp::Shutdown();
	}
}

int main()
{
	try
	{
		const std::pair<const char*, void (*)()> tests[] = {
			{ "interpolation math (endpoints/midpoint/rotation/scale)", InterpolationMathIsCorrect },
			{ "interpolation does not change authoritative state", InterpolationDoesNotChangeAuthoritativeState },
			{ "interpolation state recorded only when enabled", InterpolationStateIsRecordedOnlyWhenEnabled },
			{ "GameApp alpha matches accumulator remainder", GameAppAlphaMatchesAccumulatorRemainder },
		};
		int failures = 0;
		for (const auto& [name, test] : tests)
		{
			try { test(); std::printf("[PASS] %s\n", name); }
			catch (const std::exception& error) { ++failures; std::fprintf(stderr, "[FAIL] %s: %s\n", name, error.what()); }
			catch (...) { ++failures; std::fprintf(stderr, "[FAIL] %s: unknown exception\n", name); }
		}
		if (failures == 0)
			std::printf("World.TransformInterpolation: all checks passed\n");
		else
			std::fprintf(stderr, "World.TransformInterpolation: %d group(s) failed\n", failures);
		return failures == 0 ? 0 : 1;
	}
	catch (const std::exception& error)
	{
		std::fprintf(stderr, "World.TransformInterpolation: fatal: %s\n", error.what());
		return 1;
	}
}
