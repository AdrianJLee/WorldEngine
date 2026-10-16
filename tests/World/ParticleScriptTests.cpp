#include "wldpch.h"
#include "World/Core/WorldContext.h"
#include "World/Renderer/ParticleSystem.h"
#include "World/Scene/Components.h"
#include "World/Scene/Entity.h"
#include "World/Scene/Scene.h"
#include "World/Script/Bindings/BindParticle.h"
#include "World/Script/Runtime/ScriptEngine.h"
#include "World/Script/Vm/LuauVm.h"

#include <cassert>
#include <cmath>
#include <cstdio>
#include <iostream>
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

	void RunOk(const std::string& source, const char* chunk, int line)
	{
		std::string error;
		if (!ScriptEngine::GetState().RunString(source, chunk, &error))
			throw std::runtime_error(std::string("line ") + std::to_string(line) + ": " + chunk +
				" failed: " + error);
	}
#define RUN_OK(source, chunk) RunOk(source, chunk, __LINE__)

	void RunFail(const std::string& source, const char* chunk, int line)
	{
		std::string error;
		if (ScriptEngine::GetState().RunString(source, chunk, &error))
			throw std::runtime_error(std::string("line ") + std::to_string(line) + ": " + chunk +
				" unexpectedly succeeded");
	}
#define RUN_FAIL(source, chunk) RunFail(source, chunk, __LINE__)
}

int main()
{
	try
	{
		WorldContext context;
		ScriptEngine::Init();

		// 1. 全局只读表 Particle 存在性与函数签名校验
		RUN_OK(R"(
			assert(type(Particle) == "table", "Particle table must exist")
			assert(type(Particle.Emit) == "function", "Particle.Emit must be a function")
			assert(type(Particle.Play) == "function", "Particle.Play must be a function")
			assert(type(Particle.Stop) == "function", "Particle.Stop must be a function")
			assert(type(Particle.Restart) == "function", "Particle.Restart must be a function")
			assert(type(Particle.GetActiveCount) == "function", "Particle.GetActiveCount must be a function")
			assert(type(Particle.SetLockToXY) == "function", "Particle.SetLockToXY must be a function")
			assert(type(Particle.SetEmissionRate) == "function", "Particle.SetEmissionRate must be a function")
			assert(type(Particle.SetSpeed) == "function", "Particle.SetSpeed must be a function")
			assert(type(Particle.SetLifetime) == "function", "Particle.SetLifetime must be a function")
		)", "ParticleTableCheck");

		// 只读保护测试
		RUN_FAIL(R"(
			Particle.ReadOnlyTest = 123
		)", "ParticleReadOnlyCheck");

		// 2. 场景与粒子发射器实体配置
		Scene scene(context);
		ScriptEngine::SetActiveScene(&scene);

		Entity emitter = Entity::CreateEntity(&scene, "TestEmitter");
		auto& tr = emitter.AddComponent<TransformComponent>();
		tr.Location = glm::vec3(5.0f, 10.0f, 15.0f);

		auto& comp = emitter.AddComponent<ParticleSystemComponent>();
		comp.MaxParticles = 500;
		comp.EmissionRate = 30.0f;
		comp.Speed = 5.0f;
		comp.Lifetime = 2.0f;
		comp.LockToXYPlane = 0;
		comp.IsPlaying = 1;

		uint32_t emitterId = static_cast<uint32_t>(static_cast<entt::entity>(emitter));
		std::string setupScript = "EMITTER_ID = " + std::to_string(emitterId) + "\n";
		RUN_OK(setupScript, "SetupEmitterId");

		// 3. Particle.Emit 与 Particle.GetActiveCount 测试
		RUN_OK(R"(
			assert(Particle.GetActiveCount(EMITTER_ID) == 0, "Initial active count should be 0")
			local ok = Particle.Emit(EMITTER_ID, 25)
			assert(ok == true, "Particle.Emit should return true")
			local count = Particle.GetActiveCount(EMITTER_ID)
			assert(count == 25, "Active particle count should be 25 after emit")
		)", "EmitAndCountTest");

		CHECK(GetGlobalParticleSystem().GetPoolForEntity(emitterId).ActiveCount() == 25);

		// 4. Particle.SetLockToXY 测试
		RUN_OK(R"(
			local okLock = Particle.SetLockToXY(EMITTER_ID, true)
			assert(okLock == true, "SetLockToXY true should succeed")
		)", "SetLockToXYTrueTest");
		CHECK(comp.LockToXYPlane == 1);

		RUN_OK(R"(
			local okUnlock = Particle.SetLockToXY(EMITTER_ID, false)
			assert(okUnlock == true, "SetLockToXY false should succeed")
		)", "SetLockToXYFalseTest");
		CHECK(comp.LockToXYPlane == 0);

		// 5. Particle.SetEmissionRate 测试
		RUN_OK(R"(
			local okRate = Particle.SetEmissionRate(EMITTER_ID, 85.5)
			assert(okRate == true, "SetEmissionRate should succeed")
		)", "SetEmissionRateTest");
		CHECK(std::abs(comp.EmissionRate - 85.5f) < 0.001f);

		// 6. Particle.SetSpeed 测试
		RUN_OK(R"(
			local okSpeed = Particle.SetSpeed(EMITTER_ID, 14.2)
			assert(okSpeed == true, "SetSpeed should succeed")
		)", "SetSpeedTest");
		CHECK(std::abs(comp.Speed - 14.2f) < 0.001f);

		// 7. Particle.SetLifetime 测试
		RUN_OK(R"(
			local okLife = Particle.SetLifetime(EMITTER_ID, 4.8)
			assert(okLife == true, "SetLifetime should succeed")
		)", "SetLifetimeTest");
		CHECK(std::abs(comp.Lifetime - 4.8f) < 0.001f);

		// 8. Particle.Stop 与 Particle.Play 测试
		RUN_OK(R"(
			local okStop = Particle.Stop(EMITTER_ID)
			assert(okStop == true, "Stop should succeed")
		)", "StopTest");
		CHECK(comp.IsPlaying == 0);

		RUN_OK(R"(
			local okPlay = Particle.Play(EMITTER_ID)
			assert(okPlay == true, "Play should succeed")
		)", "PlayTest");
		CHECK(comp.IsPlaying == 1);

		// 9. Particle.Restart 测试 (重置粒子池)
		RUN_OK(R"(
			local okRestart = Particle.Restart(EMITTER_ID)
			assert(okRestart == true, "Restart should succeed")
			assert(Particle.GetActiveCount(EMITTER_ID) == 0, "Active count must be 0 after restart")
		)", "RestartTest");
		CHECK(comp.IsPlaying == 1);
		CHECK(GetGlobalParticleSystem().GetPoolForEntity(emitterId).ActiveCount() == 0);

		// 10. 冒号语法糖调用兼容测试 (Colon invocation)
		RUN_OK(R"(
			Particle:Emit(EMITTER_ID, 12)
			assert(Particle:GetActiveCount(EMITTER_ID) == 12, "Colon call active count mismatch")
			Particle:SetEmissionRate(EMITTER_ID, 150.0)
		)", "ColonCallTest");
		CHECK(GetGlobalParticleSystem().GetPoolForEntity(emitterId).ActiveCount() == 12);
		CHECK(std::abs(comp.EmissionRate - 150.0f) < 0.001f);

		// 11. 缺参与非法参数测试 (RUN_FAIL)
		RUN_FAIL(R"(
			Particle.Emit()
		)", "EmitNoArgsFail");

		RUN_FAIL(R"(
			Particle.SetEmissionRate(EMITTER_ID)
		)", "SetEmissionRateFewArgsFail");

		RUN_FAIL(R"(
			Particle.SetSpeed(EMITTER_ID)
		)", "SetSpeedFewArgsFail");

		RUN_FAIL(R"(
			Particle.SetLifetime(EMITTER_ID)
		)", "SetLifetimeFewArgsFail");

		RUN_FAIL(R"(
			Particle.SetLockToXY(EMITTER_ID)
		)", "SetLockToXYFewArgsFail");

		// 12. 无场景时调用异常测试
		ScriptEngine::SetActiveScene(nullptr);

		RUN_FAIL(R"(
			Particle.Emit(EMITTER_ID, 5)
		)", "EmitNoSceneFail");

		RUN_FAIL(R"(
			Particle.Play(EMITTER_ID)
		)", "PlayNoSceneFail");

		ScriptEngine::Shutdown();
		std::printf("World.ParticleScript: all checks passed\n");
		return 0;
	}
	catch (const std::exception& ex)
	{
		std::fprintf(stderr, "World.ParticleScript: fatal: %s\n", ex.what());
		return 1;
	}
}
