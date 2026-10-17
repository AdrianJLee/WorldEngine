#include "wldpch.h"
#include "World/Core/WorldContext.h"
#include "World/Renderer/PostProcess/PostProcessPasses.h"
#include "World/Renderer/VirtualCameraSystem.h"
#include "World/Scene/Components.h"
#include "World/Scene/Entity.h"
#include "World/Scene/Scene.h"
#include "World/Script/Runtime/ScriptEngine.h"
#include "World/Script/Vm/LuauVm.h"
#include "World/Script/Vm/ScriptValue.h"

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

		// 1. 全局只读表 Camera 与 PostProcess 存在性校验
		RUN_OK(R"(
			assert(type(Camera) == "table", "Camera table must exist")
			assert(type(Camera.Shake) == "function", "Camera.Shake must be a function")
			assert(type(Camera.SetTarget) == "function", "Camera.SetTarget must be a function")
			assert(type(Camera.SetDamping) == "function", "Camera.SetDamping must be a function")
			assert(type(Camera.SetDeadZone) == "function", "Camera.SetDeadZone must be a function")
			assert(type(Camera.GetPosition) == "function", "Camera.GetPosition must be a function")

			assert(type(PostProcess) == "table", "PostProcess table must exist")
			assert(type(PostProcess.FlashWhite) == "function", "PostProcess.FlashWhite must be a function")
			assert(type(PostProcess.SetOutline) == "function", "PostProcess.SetOutline must be a function")
			assert(type(PostProcess.SetDissolve) == "function", "PostProcess.SetDissolve must be a function")
			assert(type(PostProcess.SetVignette) == "function", "PostProcess.SetVignette must be a function")
			assert(type(PostProcess.SetBloom) == "function", "PostProcess.SetBloom must be a function")
			assert(type(PostProcess.SetChromaticAberration) == "function", "PostProcess.SetChromaticAberration must be a function")
			assert(type(Input.IsKeyPressed) == "function", "Input.IsKeyPressed must be a function")
			assert(Input.IsKeyPressed("Space") == false, "Space should not be pressed in headless")
			assert(Input.IsKeyPressed(32) == false, "Keycode 32 should not be pressed in headless")
		)", "TableCheck");

		// 只读保护测试
		RUN_FAIL(R"(
			Camera.ReadOnlyTest = 123
		)", "CameraReadOnlyCheck");

		RUN_FAIL(R"(
			PostProcess.ReadOnlyTest = 456
		)", "PostProcessReadOnlyCheck");

		// 2. 场景与摄像机/后处理组件配置
		Scene scene(context);
		ScriptEngine::SetActiveScene(&scene);

		// 主摄像机实体 (Primary Camera)
		Entity mainCam = Entity::CreateEntity(&scene, "MainCamera");
		auto& camComp = mainCam.AddComponent<CameraComponent>();
		camComp.Primary = true;
		auto& camTr = mainCam.AddComponent<TransformComponent>();
		camTr.Location = glm::vec3(12.0f, 24.0f, 36.0f);

		// 跟随目标实体
		Entity targetEntity = Entity::CreateEntity(&scene, "PlayerTarget");
		auto& targetTr = targetEntity.AddComponent<TransformComponent>();
		targetTr.Location = glm::vec3(5.0f, 1.0f, 0.0f);
		uint32_t targetId = static_cast<uint32_t>(static_cast<entt::entity>(targetEntity));

		// 虚拟相机实体 (Virtual Camera)
		Entity vcamEntity = Entity::CreateEntity(&scene, "VirtualCam");
		auto& vcamComp = vcamEntity.AddComponent<VirtualCameraComponent>();
		vcamComp.Priority = 10;
		vcamComp.Flags = VirtualCameraFlags::Active;
		vcamComp.FollowTarget = 0;
		vcamComp.Damping = glm::vec3(0.1f);
		vcamComp.DeadZone = glm::vec2(0.1f);
		vcamComp.ScreenShakeTrauma = 0.0f;

		// 后处理实体 (PostProcess)
		Entity ppEntity = Entity::CreateEntity(&scene, "PostProcessVolume");
		auto& ppComp = ppEntity.AddComponent<PostProcessComponent>();
		ppComp.FlashWhiteIntensity = 0.0f;
		ppComp.OutlineWidth = 0.0f;
		ppComp.DissolveProgress = 0.0f;
		ppComp.VignetteIntensity = 0.0f;
		ppComp.BloomIntensity = 0.0f;
		ppComp.ChromaticAberration = 0.0f;

		std::string setupScript = "TARGET_ID = " + std::to_string(targetId) + "\n";
		RUN_OK(setupScript, "SetupTargetId");

		// 3. Camera.GetPosition 脚本测试
		RUN_OK(R"(
			local pos = Camera.GetPosition()
			assert(type(pos) == "table", "GetPosition must return a table")
			assert(type(pos.x) == "number", "pos.x must be number")
			assert(type(pos.y) == "number", "pos.y must be number")
			assert(type(pos.z) == "number", "pos.z must be number")
			assert(math.abs(pos.x - 12.0) < 0.01, "Camera X mismatch")
			assert(math.abs(pos.y - 24.0) < 0.01, "Camera Y mismatch")
			assert(math.abs(pos.z - 36.0) < 0.01, "Camera Z mismatch")
		)", "CameraGetPositionScriptTest");

		// 4. Camera.SetTarget 脚本测试
		RUN_OK(R"(
			Camera.SetTarget(TARGET_ID)
		)", "CameraSetTargetScriptTest");
		CHECK(vcamComp.FollowTarget == targetId);

		// 5. Camera.SetDamping 脚本测试
		RUN_OK(R"(
			Camera.SetDamping(0.25, 0.35, 0.45)
		)", "CameraSetDampingScriptTest");
		CHECK(std::abs(vcamComp.Damping.x - 0.25f) < 0.001f);
		CHECK(std::abs(vcamComp.Damping.y - 0.35f) < 0.001f);
		CHECK(std::abs(vcamComp.Damping.z - 0.45f) < 0.001f);

		// 6. Camera.SetDeadZone 脚本测试
		RUN_OK(R"(
			Camera.SetDeadZone(0.18, 0.28)
		)", "CameraSetDeadZoneScriptTest");
		CHECK(std::abs(vcamComp.DeadZone.x - 0.18f) < 0.001f);
		CHECK(std::abs(vcamComp.DeadZone.y - 0.28f) < 0.001f);

		// 7. Camera.Shake 脚本测试
		RUN_OK(R"(
			Camera.Shake(0.7)
		)", "CameraShakeScriptTest");
		CHECK(vcamComp.ScreenShakeTrauma > 0.0f);

		// 8. PostProcess.FlashWhite 脚本测试
		RUN_OK(R"(
			PostProcess.FlashWhite(0.85, 4.0)
		)", "PostProcessFlashWhiteScriptTest");
		CHECK(ppComp.FlashWhiteIntensity > 0.0f);

		// 9. PostProcess.SetOutline 脚本测试
		RUN_OK(R"(
			PostProcess.SetOutline(4.5, 1.0, 0.5, 0.2, 0.9)
		)", "PostProcessSetOutlineScriptTest");
		CHECK(std::abs(ppComp.OutlineWidth - 4.5f) < 0.001f);
		CHECK(std::abs(ppComp.OutlineColor.r - 1.0f) < 0.001f);
		CHECK(std::abs(ppComp.OutlineColor.g - 0.5f) < 0.001f);
		CHECK(std::abs(ppComp.OutlineColor.b - 0.2f) < 0.001f);
		CHECK(std::abs(ppComp.OutlineColor.a - 0.9f) < 0.001f);

		// 10. PostProcess.SetDissolve 脚本测试
		RUN_OK(R"(
			PostProcess.SetDissolve(0.65)
		)", "PostProcessSetDissolveScriptTest");
		CHECK(std::abs(ppComp.DissolveProgress - 0.65f) < 0.001f);

		// 11. PostProcess.SetVignette 脚本测试
		RUN_OK(R"(
			PostProcess.SetVignette(0.42)
		)", "PostProcessSetVignetteScriptTest");
		CHECK(std::abs(ppComp.VignetteIntensity - 0.42f) < 0.001f);

		// 12. PostProcess.SetBloom 脚本测试
		RUN_OK(R"(
			PostProcess.SetBloom(2.8, 0.75)
		)", "PostProcessSetBloomScriptTest");
		CHECK(std::abs(ppComp.BloomIntensity - 2.8f) < 0.001f);
		CHECK(std::abs(ppComp.BloomThreshold - 0.75f) < 0.001f);

		// 13. PostProcess.SetChromaticAberration 脚本测试
		RUN_OK(R"(
			PostProcess.SetChromaticAberration(0.16)
		)", "PostProcessSetChromaticAberrationScriptTest");
		CHECK(std::abs(ppComp.ChromaticAberration - 0.16f) < 0.001f);

		// 14. 错误参数与缺参校验 (RUN_FAIL)
		RUN_FAIL(R"(
			Camera.SetDamping(0.1, 0.2)
		)", "CameraSetDampingFewArgs");

		RUN_FAIL(R"(
			Camera.SetDeadZone(0.1)
		)", "CameraSetDeadZoneFewArgs");

		RUN_FAIL(R"(
			PostProcess.SetDissolve()
		)", "PostProcessSetDissolveNoArgs");

		RUN_FAIL(R"(
			PostProcess.SetVignette()
		)", "PostProcessSetVignetteNoArgs");

		RUN_FAIL(R"(
			PostProcess.SetChromaticAberration()
		)", "PostProcessSetChromaticAberrationNoArgs");

		// 15. 无活动场景时调用抛错校验
		ScriptEngine::SetActiveScene(nullptr);

		RUN_FAIL(R"(
			Camera.Shake(0.5)
		)", "CameraShakeNoScene");

		RUN_FAIL(R"(
			PostProcess.FlashWhite()
		)", "PostProcessFlashWhiteNoScene");

		ScriptEngine::Shutdown();
		std::printf("World.CameraPostProcessScript: all checks passed\n");
		return 0;
	}
	catch (const std::exception& ex)
	{
		std::fprintf(stderr, "World.CameraPostProcessScript: fatal: %s\n", ex.what());
		return 1;
	}
}
