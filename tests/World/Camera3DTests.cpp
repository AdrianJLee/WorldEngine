// P1b D1:3D 相机与射线数学基线(纯 glm,无窗口/无设备)。
#include "World/Core/Core.h"
#include "World/Core/WorldContext.h"
#include "World/Renderer/CameraRay.h"
#include "World/Renderer/EditorCamera3D.h"
#include "World/Renderer/ProjectionConventions.h"
#include "World/Scene/Components.h"
#include "World/Scene/Entity.h"
#include "World/Scene/Scene.h"
#include "World/Scene/SceneSerializer.h"
#include "World/Scene/Systems/CameraSystem.h"

#include <glm/gtc/matrix_transform.hpp>

#include <cmath>
#include <cstdio>
#include <filesystem>
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

	// 点到射线的最短距离。
	float DistanceToRay(const World::Ray& ray, const glm::vec3& point)
	{
		const glm::vec3 toPoint = point - ray.Origin;
		const float t = glm::dot(toPoint, ray.Direction);
		return glm::length(toPoint - ray.Direction * t);
	}
}

int main()
{
	try
	{
		using namespace World;

		EditorCamera3D camera(60.0f, 16.0f / 9.0f, 0.1f, 1000.0f, 10.0f);
		camera.SetViewportSize(1280, 720);
		camera.SetTarget({ 0.0f, 0.0f, 0.0f });

		// 1. 初始姿态:目标在正前方,距离与 GetDistance 一致。
		{
			const glm::vec3 toTarget = camera.GetTarget() - camera.GetPosition();
			const float length = glm::length(toTarget);
			CHECK(std::fabs(length - camera.GetDistance()) < 1e-4f);
			CHECK(glm::length(toTarget / length - camera.GetForward()) < 1e-5f);
			CHECK(glm::dot(camera.GetForward(), glm::vec3(0.0f, 0.0f, 1.0f)) > 0.999f);
		}

		// 2. Orbit 不改变到目标点的距离;pitch 限幅 ±89°。
		{
			const float before = glm::length(camera.GetTarget() - camera.GetPosition());
			camera.Orbit(35.0f, 25.0f);
			const float after = glm::length(camera.GetTarget() - camera.GetPosition());
			CHECK(std::fabs(before - after) < 1e-3f);
			CHECK(std::fabs(after - camera.GetDistance()) < 1e-3f);

			camera.Orbit(0.0f, 1000.0f);
			CHECK(camera.GetPitch() <= 89.0f + 1e-3f);
			CHECK(camera.GetPitch() >= -89.0f - 1e-3f);
			camera.Orbit(0.0f, -5000.0f);
			CHECK(camera.GetPitch() >= -89.0f - 1e-3f);
		}

		// 3. Dolly 有上下限,Pan 保持朝向(只平移目标)。
		{
			camera.Dolly(-1000.0f);
			CHECK(camera.GetDistance() > 0.0f);
			camera.SetDistance(12.0f);
			const glm::vec3 forwardBefore = camera.GetForward();
			camera.Pan(1.0f, -0.5f);
			CHECK(glm::length(camera.GetForward() - forwardBefore) < 1e-5f);
		}

		// 4. NDC 往返:世界点 → NDC → 世界射线,点应落在射线上。
		{
			const glm::mat4 viewProjection = camera.GetViewProjectionMatrix(false);
			const glm::mat4 inverseViewProjection = glm::inverse(viewProjection);
			const glm::vec3 worldPoint(2.0f, 1.5f, 3.0f);

			bool visible = false;
			const glm::vec2 ndc = WorldToNdc(worldPoint, viewProjection, &visible);
			CHECK(visible);

			const Ray ray = ScreenPointToRay(ndc, inverseViewProjection);
			CHECK(std::fabs(glm::length(ray.Direction) - 1.0f) < 1e-4f);
			CHECK(DistanceToRay(ray, worldPoint) < 1e-3f);

			// 屏幕中心射线方向应与相机前向一致(视线中心)。
			const Ray centerRay = ScreenPointToRay({ 0.0f, 0.0f }, inverseViewProjection);
			CHECK(glm::length(centerRay.Direction - camera.GetForward()) < 1e-3f);
		}

		// 5. 后端 NDC 适配:GL 保持原样,Vulkan = 翻转 Y 行 + 深度重映射(z'=0.5z+0.5w)。
		{
			const glm::mat4 gl = camera.GetProjectionMatrix(false);
			const glm::mat4 vk = camera.GetProjectionMatrix(true);
			for (int column = 0; column < 4; ++column)
			{
				CHECK(std::fabs(vk[column][1] + gl[column][1]) < 1e-6f);
				CHECK(std::fabs(vk[column][2] - (0.5f * gl[column][2] + 0.5f * gl[column][3])) < 1e-6f);
				CHECK(std::fabs(vk[column][3] - gl[column][3]) < 1e-6f);
			}
		}

		// 5b. 深度端点:相机前方 near/far 处的点,裁剪空间 z/w 必须落在
		//     GL:[-1,1](近=-1,远=+1)与 Vulkan:[0,1](近=0,远=1)内,并且单调递增。
		//     这是"3D 深度/面朝向异常"的回归锚点:只翻 Y 不补 Z 时,Vulkan 下的近平面
		//     端点会是 -1(落在合法范围外),近处几何被裁掉、深度比较失真。
		{
			const float nearClip = camera.GetNearClip();
			const float farClip = camera.GetFarClip();
			const glm::vec3 origin = camera.GetPosition();
			const glm::vec3 forward = camera.GetForward();
			const auto depthAt = [&](const glm::mat4& viewProjection, float distance)
			{
				const glm::vec4 clip = viewProjection * glm::vec4(origin + forward * distance, 1.0f);
				return clip.z / clip.w;
			};

			const glm::mat4 gl = camera.GetViewProjectionMatrix(false);
			CHECK(std::fabs(depthAt(gl, nearClip) + 1.0f) < 1e-3f);
			CHECK(std::fabs(depthAt(gl, farClip) - 1.0f) < 1e-3f);
			CHECK(depthAt(gl, nearClip) < depthAt(gl, 0.5f * (nearClip + farClip)));
			CHECK(depthAt(gl, 0.5f * (nearClip + farClip)) < depthAt(gl, farClip));

			const glm::mat4 vk = camera.GetViewProjectionMatrix(true);
			CHECK(std::fabs(depthAt(vk, nearClip) - 0.0f) < 1e-3f);
			CHECK(std::fabs(depthAt(vk, farClip) - 1.0f) < 1e-3f);
			CHECK(depthAt(vk, nearClip) < depthAt(vk, 0.5f * (nearClip + farClip)));
			CHECK(depthAt(vk, 0.5f * (nearClip + farClip)) < depthAt(vk, farClip));

			// 同一世界点在两种约定下的深度都在各自合法范围内(不会再出现"一半深度越界")。
			for (float t = 0.0f; t <= 1.0f; t += 0.1f)
			{
				const float distance = nearClip + (farClip - nearClip) * t;
				const float glDepth = depthAt(gl, distance);
				const float vkDepth = depthAt(vk, distance);
				CHECK(glDepth >= -1.0f - 1e-3f && glDepth <= 1.0f + 1e-3f);
				CHECK(vkDepth >= -1e-3f && vkDepth <= 1.0f + 1e-3f);
			}
		}

		// 5c. 需要同一份"变换后的可见性"约定:Y 翻转只改变画面方向,不改变"外壁 = CCW"的
		//     网格约定(两个后端共用 CCW 判正面,实测一致)。这条钉住"翻 Y 不等于翻绕序",
		//     以及 Vulkan 近端/远端映射的符号,避免再出现"按后端翻 FrontFace"的误改。
		{
			const glm::vec3 origin = camera.GetPosition();
			const glm::vec3 right = camera.GetRight();
			const glm::vec3 up = camera.GetUp();
			const glm::vec3 forward = camera.GetForward();
			const float distance = camera.GetDistance();
			const float half = 0.5f;
			// 相机前方、从相机看去逆时针的三个点(等价于一个外壁 CCW 的三角形)。
			const glm::vec3 a = origin + forward * distance - right * half - up * half;
			const glm::vec3 b = origin + forward * distance + right * half - up * half;
			const glm::vec3 c = origin + forward * distance - right * half + up * half;

			const auto signedArea = [&](const glm::mat4& viewProjection)
			{
				const auto toNdc = [&](const glm::vec3& point)
				{
					const glm::vec4 clip = viewProjection * glm::vec4(point, 1.0f);
					return glm::vec2(clip.x / clip.w, clip.y / clip.w);
				};
				const glm::vec2 p0 = toNdc(a);
				const glm::vec2 p1 = toNdc(b);
				const glm::vec2 p2 = toNdc(c);
				return 0.5f * ((p1.x - p0.x) * (p2.y - p0.y) - (p2.x - p0.x) * (p1.y - p0.y));
			};

			const float glArea = signedArea(camera.GetViewProjectionMatrix(false));
			const float vkArea = signedArea(camera.GetViewProjectionMatrix(true));
			CHECK(glArea > 0.0f);
			// Vulkan 适配翻转 Y 行 ⇒ 同一三角形在 NDC 上的有向面积符号相反;两个后端各自按
			// 自己的 framebuffer 约定判定,最终都把这个三角形当正面(以抓屏 A/B 实测为准)。
			CHECK(vkArea < 0.0f);
			CHECK(std::fabs(glArea + vkArea) < 1e-4f);
		}

		// 6. 射线与平面求交:命中 + 平行未命中 + 反方向未命中。
		{
			const Ray ray { { 0.0f, 1.0f, 0.0f }, { 0.0f, -1.0f, 0.0f } };
			float t = -1.0f;
			CHECK(IntersectPlane(ray, { 0.0f, 0.0f, 0.0f }, { 0.0f, 1.0f, 0.0f }, &t));
			CHECK(std::fabs(t - 1.0f) < 1e-5f);

			const Ray parallel { { 0.0f, 1.0f, 0.0f }, { 1.0f, 0.0f, 0.0f } };
			CHECK(!IntersectPlane(parallel, { 0.0f, 0.0f, 0.0f }, { 0.0f, 1.0f, 0.0f }, nullptr));

			const Ray away { { 0.0f, 1.0f, 0.0f }, { 0.0f, 1.0f, 0.0f } };
			CHECK(!IntersectPlane(away, { 0.0f, 0.0f, 0.0f }, { 0.0f, 1.0f, 0.0f }, nullptr));
		}

		// 7. 相机组件数据导向化(PECS)反假通过用例。
		//    A) FixedAspectRatio=true + 非默认 FOV/投影类型,经 .wd 往返后投影必须按读回参数重建;
		//       改动前 SceneCamera 的 m_ProjectionMatrix 保持默认构造值(且 FixedAspectRatio 时
		//       SetViewportSize 直接跳过)⇒ 此断言失败。
		//    B) 参数不变时连续刷新 0 次重算;参数变化时恰好 1 次(投影指纹脏标记)。
		{
			const std::filesystem::path root = std::filesystem::temp_directory_path() / "we-camera-pod-tests";
			std::filesystem::remove_all(root);
			std::filesystem::create_directories(root);
			const std::filesystem::path scenePath = root / "camera_pod.wd";

			{
				WorldContext context;
				Ref<Scene> scene = CreateRef<Scene>(context);
				Entity entity = Entity::CreateEntity(scene.get(), "Camera");
				CameraComponent cameraComponent;
				cameraComponent.Camera.m_ProjectionType = CameraSettings::ProjectionType::Perspective;
				cameraComponent.Camera.m_PerspectiveFOV = 63.0f;
				cameraComponent.Camera.m_PerspectiveNearClip = 0.25f;
				cameraComponent.Camera.m_PerspectiveFarClip = 250.0f;
				cameraComponent.FixedAspectRatio = true;
				cameraComponent.Primary = true;
				entity.AddComponent<CameraComponent>(cameraComponent);

				SceneSerializer serializer(scene);
				if (!serializer.Serialize(scenePath.string()))
					throw std::runtime_error(std::string("camera pod test: serialize failed: ") + serializer.GetLastError());
			}

			WorldContext loadedContext;
			Ref<Scene> loaded = CreateRef<Scene>(loadedContext);
			{
				SceneSerializer serializer(loaded);
				if (!serializer.Deserialize(scenePath.string()))
					throw std::runtime_error(std::string("camera pod test: deserialize failed: ") + serializer.GetLastError());
			}

			const entt::registry& loadedRegistry = static_cast<const Scene*>(loaded.get())->GetRegistry();
			entt::entity handle = entt::null;
			for (const entt::entity candidate : loadedRegistry.view<CameraComponent>())
			{
				handle = candidate;
				break;
			}
			CHECK(handle != entt::null);
			const CameraComponent& restored = loadedRegistry.get<CameraComponent>(handle);
			CHECK(restored.FixedAspectRatio);
			CHECK(restored.Camera.m_ProjectionType == CameraSettings::ProjectionType::Perspective);
			CHECK(std::fabs(restored.Camera.m_PerspectiveFOV - 63.0f) < 1e-4f);
			// m_AspectRatio 是 Transient(不入档)⇒ 读回是默认 1.0;FixedAspectRatio 相机以它为准。
			CHECK(std::fabs(restored.Camera.m_AspectRatio - 1.0f) < 1e-6f);

			const Camera& restoredView = loaded->GetCameraView(handle);
			const glm::mat4 expected = glm::perspective(glm::radians(63.0f), 1.0f, 0.25f, 250.0f);
			const glm::mat4& actual = restoredView.GetProjectionMatrix();
			for (int column = 0; column < 4; ++column)
				for (int row = 0; row < 4; ++row)
					CHECK(std::fabs(actual[column][row] - expected[column][row]) < 1e-6f);

			// 旧嵌套写法兼容:直接读入仓库既有的 .wd(相机仍是 Camera: { m_* } 形态)。
			WorldContext legacyContext;
			Ref<Scene> legacy = CreateRef<Scene>(legacyContext);
			{
				SceneSerializer serializer(legacy);
				if (!serializer.Deserialize("templates/project-example/assets/scenes/3DTest.wd"))
					throw std::runtime_error(std::string("camera pod test: legacy .wd load failed: ") + serializer.GetLastError());
			}
			const entt::registry& legacyRegistry = static_cast<const Scene*>(legacy.get())->GetRegistry();
			entt::entity legacyHandle = entt::null;
			for (const entt::entity candidate : legacyRegistry.view<CameraComponent>())
			{
				legacyHandle = candidate;
				break;
			}
			CHECK(legacyHandle != entt::null);
			const CameraComponent& legacyCamera = legacyRegistry.get<CameraComponent>(legacyHandle);
			CHECK(legacyCamera.Camera.m_ProjectionType == CameraSettings::ProjectionType::Perspective);
			CHECK(std::fabs(legacyCamera.Camera.m_PerspectiveFarClip - 200.0f) < 1e-4f);
			CHECK(legacyCamera.Primary);
			legacy->EnsureCameraView(legacyHandle);
			const glm::mat4 legacyProjection =
				CameraSystem::EnsureView(legacy->GetRegistry(), legacyHandle, 1280, 720).GetProjectionMatrix();
			const glm::mat4 legacyExpected = glm::perspective(glm::radians(legacyCamera.Camera.m_PerspectiveFOV),
				1280.0f / 720.0f, legacyCamera.Camera.m_PerspectiveNearClip, legacyCamera.Camera.m_PerspectiveFarClip);
			for (int column = 0; column < 4; ++column)
				for (int row = 0; row < 4; ++row)
					CHECK(std::fabs(legacyProjection[column][row] - legacyExpected[column][row]) < 1e-6f);

			// B) 指纹缓存:视图只在结构提交点建立,UpdateAllCameras 绝不凭空创建。
			WorldContext cacheContext;
			Ref<Scene> cacheScene = CreateRef<Scene>(cacheContext);
			Entity cacheEntity = Entity::CreateEntity(cacheScene.get(), "CacheCamera");
			CameraComponent cacheComponent;
			cacheComponent.Camera.m_ProjectionType = CameraSettings::ProjectionType::Perspective;
			cacheComponent.Camera.m_PerspectiveFOV = 50.0f;
			cacheEntity.AddComponent<CameraComponent>(cacheComponent);
			const entt::entity cacheHandle = static_cast<entt::entity>(cacheEntity);

			entt::registry& cacheRegistry = cacheScene->GetRegistry();
			CameraSystem::UpdateAllCameras(cacheRegistry, 1280, 720);
			CHECK(cacheRegistry.try_get<CameraViewComponent>(cacheHandle) == nullptr);

			const uint64_t beforeFirst = CameraSystem::ProjectionRebuildCount();
			cacheScene->EnsureCameraView(cacheHandle);   // 结构提交点建视图(与运行态路径一致)
			const glm::mat4 firstProjection =
				CameraSystem::EnsureView(cacheRegistry, cacheHandle, 1280, 720).GetProjectionMatrix();
			CHECK(CameraSystem::ProjectionRebuildCount() == beforeFirst + 1);   // Valid=false ⇒ 无条件重算 1 次
			const float expectedFocal = 1.0f / std::tan(glm::radians(50.0f) * 0.5f);
			CHECK(std::fabs(firstProjection[1][1] - expectedFocal) < 1e-5f);

			auto* cacheView = cacheRegistry.try_get<CameraViewComponent>(cacheHandle);
			CHECK(cacheView != nullptr && cacheView->Valid);
			cacheView->View = Camera(glm::mat4(0.0f));   // 破坏缓存:只有真的重算才会覆盖它
			const uint64_t beforeLoop = CameraSystem::ProjectionRebuildCount();
			for (int i = 0; i < 64; ++i)
				CameraSystem::UpdateAllCameras(cacheRegistry, 1280, 720);
			CHECK(CameraSystem::ProjectionRebuildCount() == beforeLoop);            // 参数不变 ⇒ 0 次
			CHECK(std::fabs(cacheView->View.GetProjectionMatrix()[0][0]) < 1e-9f);  // 哨兵未被覆盖

			cacheRegistry.get<CameraComponent>(cacheHandle).Camera.m_PerspectiveFOV = 70.0f;
			CameraSystem::UpdateAllCameras(cacheRegistry, 1280, 720);
			CHECK(CameraSystem::ProjectionRebuildCount() == beforeLoop + 1);        // 参数变 ⇒ 恰好 1 次
		}

		std::printf("World.Camera3D: all checks passed\n");
		return 0;
	}
	catch (const std::exception& error)
	{
		std::fprintf(stderr, "World.Camera3D FAILED: %s\n", error.what());
		return 1;
	}
}
