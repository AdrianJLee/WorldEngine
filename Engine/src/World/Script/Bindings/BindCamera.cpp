#include "wldpch.h"
#include "World/Script/Bindings/BindCamera.h"

#include "World/Renderer/PostProcess/PostProcessPasses.h"
#include "World/Renderer/VirtualCameraSystem.h"
#include "World/Scene/Components.h"
#include "World/Scene/Entity.h"
#include "World/Scene/Scene.h"
#include "World/Core/Application.h"
#include "World/Scene/Systems/CameraSystem.h"
#include <GLFW/glfw3.h>
#include "World/Script/Bindings/BindServices.h"
#include "World/Script/Runtime/ScriptEngine.h"
#include "World/Script/Vm/LuauVm.h"
#include "World/Script/Vm/ScriptBindingContext.h"
#include "World/Script/Vm/ScriptRef.h"
#include "World/Script/Vm/ScriptValue.h"

#include <glm/vec2.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace World
{
	namespace
	{
		const char* TypeNameOf(const ScriptValue& value)
		{
			return value.TypeName();
		}

		std::string MethodSignature(const std::string& table, const ScriptServiceMethod& method)
		{
			std::string signature = table + "." + method.Name + "(";
			for (std::size_t i = 0; i < method.ParamCount; ++i)
			{
				if (i) signature += ", ";
				signature += method.Params[i].Name;
				if (!method.Params[i].Required) signature += "?";
				signature += ": ";
				signature += method.Params[i].LuaType;
			}
			return signature + ")";
		}

		void NormalizeArgs(const ScriptValue*& args, std::size_t& count)
		{
			if (count > 0 && args[0].IsTable())
			{
				++args;
				--count;
			}
		}

		void CheckArgumentCount(const std::string& table, const ScriptServiceMethod& method,
			std::size_t count, std::size_t minimum)
		{
			if (count < minimum || count > method.ExpectedArgs)
				throw std::logic_error(MethodSignature(table, method) + " expects between " +
					std::to_string(minimum) + " and " + std::to_string(method.ExpectedArgs) +
					" argument(s); got " + std::to_string(count));
		}

		bool MatchesType(ScriptServiceArgType accepted, const ScriptValue& value)
		{
			if ((static_cast<uint32_t>(accepted) & static_cast<uint32_t>(ScriptServiceArgType::Boolean)) &&
				value.IsBoolean())
				return true;
			if ((static_cast<uint32_t>(accepted) & static_cast<uint32_t>(ScriptServiceArgType::Number)) &&
				(value.IsNumber() || value.IsUserdata()))
				return true;
			if ((static_cast<uint32_t>(accepted) & static_cast<uint32_t>(ScriptServiceArgType::String)) &&
				value.IsString())
				return true;
			if ((static_cast<uint32_t>(accepted) & static_cast<uint32_t>(ScriptServiceArgType::Table)) &&
				value.IsTable())
				return true;
			return false;
		}

		const ScriptValue& RequireArg(const std::string& table, const ScriptServiceMethod& method,
			const ScriptValue* args, std::size_t argCount, std::size_t index)
		{
			if (index >= argCount || args[index].IsNil())
				throw std::logic_error(MethodSignature(table, method) + " is missing required argument " +
					std::to_string(index + 1));
			if (!MatchesType(method.Params[index].Accepted, args[index]))
				throw std::logic_error(MethodSignature(table, method) + ": argument " + std::to_string(index + 1) +
					" ('" + method.Params[index].Name + "') must be " + method.Params[index].LuaType +
					", got " + TypeNameOf(args[index]));
			return args[index];
		}

		double RequireNumber(const std::string& table, const ScriptServiceMethod& method,
			const ScriptValue* args, std::size_t argCount, std::size_t index)
		{
			const ScriptValue& value = RequireArg(table, method, args, argCount, index);
			double result = 0.0;
			if (!value.AsNumber(&result))
				throw std::logic_error(MethodSignature(table, method) + ": argument " + std::to_string(index + 1) +
					" ('" + method.Params[index].Name + "') must be " + method.Params[index].LuaType);
			return result;
		}

		double OptionalNumber(const std::string& table, const ScriptServiceMethod& method,
			const ScriptValue* args, std::size_t argCount, std::size_t index, double fallback)
		{
			if (index >= argCount || args[index].IsNil())
				return fallback;
			if (!MatchesType(method.Params[index].Accepted, args[index]))
				throw std::logic_error(MethodSignature(table, method) + ": argument " + std::to_string(index + 1) +
					" ('" + method.Params[index].Name + "') must be " + method.Params[index].LuaType +
					", got " + TypeNameOf(args[index]));
			double result = fallback;
			args[index].AsNumber(&result);
			return result;
		}

		entt::entity RequireEntityId(const std::string& table, const ScriptServiceMethod& method,
			const ScriptValue* args, std::size_t count, std::size_t index)
		{
			if (index >= count || args[index].IsNil())
				throw std::logic_error(MethodSignature(table, method) + " is missing required argument " +
					std::to_string(index + 1) + " ('" + method.Params[index].Name + "')");

			const ScriptValue& val = args[index];
			if (val.IsNumber())
			{
				double num = 0.0;
				val.AsNumber(&num);
				return static_cast<entt::entity>(static_cast<uint32_t>(num));
			}

			ScriptBindingContext& context = ScriptEngine::GetBindingContext();
			Entity* entityPtr = nullptr;
			if (context.Unwrap<Entity>("Entity", val, &entityPtr) && entityPtr && entityPtr->IsValid())
			{
				return static_cast<entt::entity>(*entityPtr);
			}

			throw std::logic_error(MethodSignature(table, method) + ": argument " + std::to_string(index + 1) +
				" ('" + method.Params[index].Name + "') must be number or Entity, got " + TypeNameOf(val));
		}

		VirtualCameraComponent* FindActiveVirtualCamera(Scene& scene)
		{
			VirtualCameraComponent* bestCam = nullptr;
			int32_t maxPriority = std::numeric_limits<int32_t>::min();

			auto& reg = const_cast<entt::registry&>(static_cast<const Scene&>(scene).GetRegistry());
			auto view = reg.view<VirtualCameraComponent>();
			for (auto entity : view)
			{
				auto& vcam = view.get<VirtualCameraComponent>(entity);
				if ((vcam.Flags & VirtualCameraFlags::Active) != 0)
				{
					if (!bestCam || vcam.Priority > maxPriority)
					{
						bestCam = &vcam;
						maxPriority = vcam.Priority;
					}
				}
			}

			if (!bestCam)
			{
				for (auto entity : view)
				{
					auto& vcam = view.get<VirtualCameraComponent>(entity);
					if (!bestCam || vcam.Priority > maxPriority)
					{
						bestCam = &vcam;
						maxPriority = vcam.Priority;
					}
				}
			}

			return bestCam;
		}

		PostProcessComponent& GetOrCreatePostProcess(Scene& scene)
		{
			auto view = scene.GetRegistry().view<PostProcessComponent>();
			for (auto entity : view)
			{
				return view.get<PostProcessComponent>(entity);
			}
			Entity entity = Entity::CreateEntity(&scene, "PostProcessVolume");
			return entity.AddComponent<PostProcessComponent>();
		}

		void ForEachPostProcess(Scene& scene, const std::function<void(PostProcessComponent&)>& fn)
		{
			auto& reg = const_cast<entt::registry&>(static_cast<const Scene&>(scene).GetRegistry());
			auto view = reg.view<PostProcessComponent>();
			for (auto entity : view)
			{
				fn(view.get<PostProcessComponent>(entity));
			}
		}

		// ---- Camera 方法实现 ----

		ScriptValue CameraShakeImpl(const ScriptValue* args, std::size_t count)
		{
			NormalizeArgs(args, count);
			const ScriptServiceMethod& method = CameraServiceBindings(nullptr)[0].Methods[0];
			CheckArgumentCount("Camera", method, count, 1);
			Scene* scene = ScriptEngine::GetActiveScene();
			if (!scene)
				throw std::logic_error("Camera.Shake requires an active scene");

			const float trauma = static_cast<float>(RequireNumber("Camera", method, args, count, 0));
			if (auto* vcam = FindActiveVirtualCamera(*scene))
			{
				VirtualCameraSystem::AddTrauma(*vcam, trauma);
			}
			return ScriptValue::Nil();
		}

		ScriptValue CameraSetTargetImpl(const ScriptValue* args, std::size_t count)
		{
			NormalizeArgs(args, count);
			const ScriptServiceMethod& method = CameraServiceBindings(nullptr)[0].Methods[1];
			CheckArgumentCount("Camera", method, count, 1);
			Scene* scene = ScriptEngine::GetActiveScene();
			if (!scene)
				throw std::logic_error("Camera.SetTarget requires an active scene");

			entt::entity ent = RequireEntityId("Camera", method, args, count, 0);
			if (auto* vcam = FindActiveVirtualCamera(*scene))
			{
				vcam->FollowTarget = static_cast<uint32_t>(ent);
			}
			return ScriptValue::Nil();
		}

		ScriptValue CameraSetDampingImpl(const ScriptValue* args, std::size_t count)
		{
			NormalizeArgs(args, count);
			const ScriptServiceMethod& method = CameraServiceBindings(nullptr)[0].Methods[2];
			CheckArgumentCount("Camera", method, count, 3);
			Scene* scene = ScriptEngine::GetActiveScene();
			if (!scene)
				throw std::logic_error("Camera.SetDamping requires an active scene");

			const float dx = static_cast<float>(RequireNumber("Camera", method, args, count, 0));
			const float dy = static_cast<float>(RequireNumber("Camera", method, args, count, 1));
			const float dz = static_cast<float>(RequireNumber("Camera", method, args, count, 2));

			if (auto* vcam = FindActiveVirtualCamera(*scene))
			{
				vcam->Damping = glm::vec3(dx, dy, dz);
			}
			return ScriptValue::Nil();
		}

		ScriptValue CameraSetDeadZoneImpl(const ScriptValue* args, std::size_t count)
		{
			NormalizeArgs(args, count);
			const ScriptServiceMethod& method = CameraServiceBindings(nullptr)[0].Methods[3];
			CheckArgumentCount("Camera", method, count, 2);
			Scene* scene = ScriptEngine::GetActiveScene();
			if (!scene)
				throw std::logic_error("Camera.SetDeadZone requires an active scene");

			const float w = static_cast<float>(RequireNumber("Camera", method, args, count, 0));
			const float h = static_cast<float>(RequireNumber("Camera", method, args, count, 1));

			if (auto* vcam = FindActiveVirtualCamera(*scene))
			{
				vcam->DeadZone = glm::vec2(w, h);
			}
			return ScriptValue::Nil();
		}

		ScriptValue CameraGetPositionImpl(const ScriptValue* args, std::size_t count)
		{
			NormalizeArgs(args, count);
			const ScriptServiceMethod& method = CameraServiceBindings(nullptr)[0].Methods[4];
			CheckArgumentCount("Camera", method, count, 0);
			Scene* scene = ScriptEngine::GetActiveScene();
			if (!scene)
				throw std::logic_error("Camera.GetPosition requires an active scene");

			glm::vec3 pos(0.0f);
			bool found = false;

			const auto& reg = static_cast<const Scene*>(scene)->GetRegistry();
			auto primaryCamView = reg.view<CameraComponent, TransformComponent>();
			for (auto ent : primaryCamView)
			{
				const auto& cam = primaryCamView.get<CameraComponent>(ent);
				if (cam.Primary)
				{
					pos = primaryCamView.get<TransformComponent>(ent).Location;
					found = true;
					break;
				}
			}

			if (!found)
			{
				for (auto ent : primaryCamView)
				{
					pos = primaryCamView.get<TransformComponent>(ent).Location;
					found = true;
					break;
				}
			}

			if (!found)
			{
				auto vcamView = reg.view<VirtualCameraComponent, TransformComponent>();
				for (auto ent : vcamView)
				{
					const auto& vcam = vcamView.get<VirtualCameraComponent>(ent);
					if ((vcam.Flags & VirtualCameraFlags::Active) != 0)
					{
						pos = vcamView.get<TransformComponent>(ent).Location;
						found = true;
						break;
					}
				}
			}

			ScriptTableRef table = ScriptEngine::GetState().CreateTable();
			table.SetField("x", ScriptValue::Number(static_cast<double>(pos.x)));
			table.SetField("y", ScriptValue::Number(static_cast<double>(pos.y)));
			table.SetField("z", ScriptValue::Number(static_cast<double>(pos.z)));
			return table.ToValue();
		}

		static void GetCameraViewportDimensions(Scene& scene, float& outW, float& outH)
		{
			outW = 1280.0f;
			outH = 720.0f;
			if (Application::HasInstance())
			{
				if (void* nativeWin = Application::Get().GetWindow().GetNativeWindow())
				{
					int w = 0, h = 0;
					glfwGetWindowSize(static_cast<GLFWwindow*>(nativeWin), &w, &h);
					if (w > 0 && h > 0)
					{
						outW = static_cast<float>(w);
						outH = static_cast<float>(h);
						return;
					}
				}
				outW = static_cast<float>(Application::Get().GetWindow().GetWidth());
				outH = static_cast<float>(Application::Get().GetWindow().GetHeight());
			}
		}

		static bool FindPrimary2DCameraInfo(Scene& scene, float vpW, float vpH, glm::vec2& outCamPos, float& outZoom, float& outAspect)
		{
			const auto& reg = static_cast<const Scene&>(scene).GetRegistry();
			auto view = reg.view<CameraComponent, TransformComponent>();
			for (auto ent : view)
			{
				const auto& cc = view.get<CameraComponent>(ent);
				if (cc.Primary)
				{
					const auto& tc = view.get<TransformComponent>(ent);
					outCamPos = glm::vec2(tc.Location.x, tc.Location.y);
					outZoom = cc.Camera.m_OrthographicZoom > 0.001f ? cc.Camera.m_OrthographicZoom : 5.0f;
					outAspect = cc.Camera.m_AspectRatio > 0.001f ? cc.Camera.m_AspectRatio : (vpW / std::max(1.0f, vpH));
					return true;
				}
			}
			for (auto ent : view)
			{
				const auto& cc = view.get<CameraComponent>(ent);
				const auto& tc = view.get<TransformComponent>(ent);
				outCamPos = glm::vec2(tc.Location.x, tc.Location.y);
				outZoom = cc.Camera.m_OrthographicZoom > 0.001f ? cc.Camera.m_OrthographicZoom : 5.0f;
				outAspect = cc.Camera.m_AspectRatio > 0.001f ? cc.Camera.m_AspectRatio : (vpW / std::max(1.0f, vpH));
				return true;
			}
			outCamPos = glm::vec2(0.0f);
			outZoom = 5.0f;
			outAspect = vpW / std::max(1.0f, vpH);
			return false;
		}

		ScriptValue CameraScreenToWorld2DImpl(const ScriptValue* args, std::size_t count)
		{
			NormalizeArgs(args, count);
			const ScriptServiceMethod& method = CameraServiceBindings(nullptr)[0].Methods[5];
			CheckArgumentCount("Camera", method, count, 2);
			Scene* scene = ScriptEngine::GetActiveScene();
			if (!scene)
				throw std::logic_error("Camera.ScreenToWorld2D requires an active scene");

			const float screenX = static_cast<float>(RequireNumber("Camera", method, args, count, 0));
			const float screenY = static_cast<float>(RequireNumber("Camera", method, args, count, 1));

			float vpW = 1280.0f, vpH = 720.0f;
			GetCameraViewportDimensions(*scene, vpW, vpH);

			glm::vec2 camPos(0.0f);
			float zoom = 5.0f;
			float aspect = 16.0f / 9.0f;
			FindPrimary2DCameraInfo(*scene, vpW, vpH, camPos, zoom, aspect);

			const float ndcX = (screenX / vpW) * 2.0f - 1.0f;
			const float ndcY = 1.0f - (screenY / vpH) * 2.0f;

			const float worldX = camPos.x + ndcX * (aspect * zoom);
			const float worldY = camPos.y + ndcY * zoom;

			ScriptTableRef result = ScriptEngine::GetState().CreateTable();
			result.SetField("x", ScriptValue::Number(static_cast<double>(worldX)));
			result.SetField("y", ScriptValue::Number(static_cast<double>(worldY)));
			result.SetArrayElement(1, ScriptValue::Number(static_cast<double>(worldX)));
			result.SetArrayElement(2, ScriptValue::Number(static_cast<double>(worldY)));
			return result.ToValue();
		}

		ScriptValue CameraWorldToScreen2DImpl(const ScriptValue* args, std::size_t count)
		{
			NormalizeArgs(args, count);
			const ScriptServiceMethod& method = CameraServiceBindings(nullptr)[0].Methods[6];
			CheckArgumentCount("Camera", method, count, 2);
			Scene* scene = ScriptEngine::GetActiveScene();
			if (!scene)
				throw std::logic_error("Camera.WorldToScreen2D requires an active scene");

			const float worldX = static_cast<float>(RequireNumber("Camera", method, args, count, 0));
			const float worldY = static_cast<float>(RequireNumber("Camera", method, args, count, 1));

			float vpW = 1280.0f, vpH = 720.0f;
			GetCameraViewportDimensions(*scene, vpW, vpH);

			glm::vec2 camPos(0.0f);
			float zoom = 5.0f;
			float aspect = 16.0f / 9.0f;
			FindPrimary2DCameraInfo(*scene, vpW, vpH, camPos, zoom, aspect);

			const float ndcX = (worldX - camPos.x) / (aspect * zoom);
			const float ndcY = (worldY - camPos.y) / zoom;

			const float screenX = (ndcX + 1.0f) * 0.5f * vpW;
			const float screenY = (1.0f - ndcY) * 0.5f * vpH;

			ScriptTableRef result = ScriptEngine::GetState().CreateTable();
			result.SetField("x", ScriptValue::Number(static_cast<double>(screenX)));
			result.SetField("y", ScriptValue::Number(static_cast<double>(screenY)));
			result.SetArrayElement(1, ScriptValue::Number(static_cast<double>(screenX)));
			result.SetArrayElement(2, ScriptValue::Number(static_cast<double>(screenY)));
			return result.ToValue();
		}

		ScriptValue CameraGetMouseWorld2DImpl(const ScriptValue* args, std::size_t count)
		{
			NormalizeArgs(args, count);
			const ScriptServiceMethod& method = CameraServiceBindings(nullptr)[0].Methods[7];
			CheckArgumentCount("Camera", method, count, 0);
			Scene* scene = ScriptEngine::GetActiveScene();
			if (!scene)
				throw std::logic_error("Camera.GetMouseWorld2D requires an active scene");

			glm::vec2 mousePos = scene->GetInputSnapshot().MousePosition;

			float vpW = 1280.0f, vpH = 720.0f;
			GetCameraViewportDimensions(*scene, vpW, vpH);

			glm::vec2 camPos(0.0f);
			float zoom = 5.0f;
			float aspect = 16.0f / 9.0f;
			FindPrimary2DCameraInfo(*scene, vpW, vpH, camPos, zoom, aspect);

			const float ndcX = (mousePos.x / vpW) * 2.0f - 1.0f;
			const float ndcY = 1.0f - (mousePos.y / vpH) * 2.0f;

			const float worldX = camPos.x + ndcX * (aspect * zoom);
			const float worldY = camPos.y + ndcY * zoom;

			ScriptTableRef result = ScriptEngine::GetState().CreateTable();
			result.SetField("x", ScriptValue::Number(static_cast<double>(worldX)));
			result.SetField("y", ScriptValue::Number(static_cast<double>(worldY)));
			result.SetArrayElement(1, ScriptValue::Number(static_cast<double>(worldX)));
			result.SetArrayElement(2, ScriptValue::Number(static_cast<double>(worldY)));
			return result.ToValue();
		}

		// ---- PostProcess 方法实现 ----

		ScriptValue PostProcessFlashWhiteImpl(const ScriptValue* args, std::size_t count)
		{
			NormalizeArgs(args, count);
			const ScriptServiceMethod& method = CameraServiceBindings(nullptr)[1].Methods[0];
			CheckArgumentCount("PostProcess", method, count, 0);
			Scene* scene = ScriptEngine::GetActiveScene();
			if (!scene)
				throw std::logic_error("PostProcess.FlashWhite requires an active scene");

			const float intensity = static_cast<float>(OptionalNumber("PostProcess", method, args, count, 0, 1.0));
			const float decay = static_cast<float>(OptionalNumber("PostProcess", method, args, count, 1, 5.0));

			PostProcessPipeline::FlashWhite(*scene, intensity, decay);
			return ScriptValue::Nil();
		}

		ScriptValue PostProcessSetOutlineImpl(const ScriptValue* args, std::size_t count)
		{
			NormalizeArgs(args, count);
			const ScriptServiceMethod& method = CameraServiceBindings(nullptr)[1].Methods[1];
			CheckArgumentCount("PostProcess", method, count, 1);
			Scene* scene = ScriptEngine::GetActiveScene();
			if (!scene)
				throw std::logic_error("PostProcess.SetOutline requires an active scene");

			const float width = static_cast<float>(RequireNumber("PostProcess", method, args, count, 0));
			const float r = static_cast<float>(OptionalNumber("PostProcess", method, args, count, 1, 1.0));
			const float g = static_cast<float>(OptionalNumber("PostProcess", method, args, count, 2, 1.0));
			const float b = static_cast<float>(OptionalNumber("PostProcess", method, args, count, 3, 0.0));
			const float a = static_cast<float>(OptionalNumber("PostProcess", method, args, count, 4, 1.0));

			ForEachPostProcess(*scene, [=](PostProcessComponent& pp) {
				pp.OutlineWidth = width;
				pp.OutlineColor = glm::vec4(r, g, b, a);
			});
			return ScriptValue::Nil();
		}

		ScriptValue PostProcessSetDissolveImpl(const ScriptValue* args, std::size_t count)
		{
			NormalizeArgs(args, count);
			const ScriptServiceMethod& method = CameraServiceBindings(nullptr)[1].Methods[2];
			CheckArgumentCount("PostProcess", method, count, 1);
			Scene* scene = ScriptEngine::GetActiveScene();
			if (!scene)
				throw std::logic_error("PostProcess.SetDissolve requires an active scene");

			const float progress = static_cast<float>(RequireNumber("PostProcess", method, args, count, 0));
			ForEachPostProcess(*scene, [=](PostProcessComponent& pp) {
				pp.DissolveProgress = progress;
			});
			return ScriptValue::Nil();
		}

		ScriptValue PostProcessSetVignetteImpl(const ScriptValue* args, std::size_t count)
		{
			NormalizeArgs(args, count);
			const ScriptServiceMethod& method = CameraServiceBindings(nullptr)[1].Methods[3];
			CheckArgumentCount("PostProcess", method, count, 1);
			Scene* scene = ScriptEngine::GetActiveScene();
			if (!scene)
				throw std::logic_error("PostProcess.SetVignette requires an active scene");

			const float intensity = static_cast<float>(RequireNumber("PostProcess", method, args, count, 0));
			ForEachPostProcess(*scene, [=](PostProcessComponent& pp) {
				pp.VignetteIntensity = intensity;
			});
			return ScriptValue::Nil();
		}

		ScriptValue PostProcessSetBloomImpl(const ScriptValue* args, std::size_t count)
		{
			NormalizeArgs(args, count);
			const ScriptServiceMethod& method = CameraServiceBindings(nullptr)[1].Methods[4];
			CheckArgumentCount("PostProcess", method, count, 1);
			Scene* scene = ScriptEngine::GetActiveScene();
			if (!scene)
				throw std::logic_error("PostProcess.SetBloom requires an active scene");

			const float intensity = static_cast<float>(RequireNumber("PostProcess", method, args, count, 0));
			const float threshold = static_cast<float>(OptionalNumber("PostProcess", method, args, count, 1, 1.0));

			ForEachPostProcess(*scene, [=](PostProcessComponent& pp) {
				pp.BloomIntensity = intensity;
				pp.BloomThreshold = threshold;
			});
			return ScriptValue::Nil();
		}

		ScriptValue PostProcessSetChromaticAberrationImpl(const ScriptValue* args, std::size_t count)
		{
			NormalizeArgs(args, count);
			const ScriptServiceMethod& method = CameraServiceBindings(nullptr)[1].Methods[5];
			CheckArgumentCount("PostProcess", method, count, 1);
			Scene* scene = ScriptEngine::GetActiveScene();
			if (!scene)
				throw std::logic_error("PostProcess.SetChromaticAberration requires an active scene");

			const float amount = static_cast<float>(RequireNumber("PostProcess", method, args, count, 0));
			ForEachPostProcess(*scene, [=](PostProcessComponent& pp) {
				pp.ChromaticAberration = amount;
			});
			return ScriptValue::Nil();
		}

		// ---- 参数描述表 ----

		// Camera
		static const ScriptServiceParam cameraShakeParams[] = {
			{ "trauma", "number", ScriptServiceArgType::Number, true, "Camera trauma value in range [0, 1]." },
		};

		static const ScriptServiceParam cameraSetTargetParams[] = {
			{ "entityId", "number", ScriptServiceArgType::Number, true, "Target entity ID or Entity object to follow." },
		};

		static const ScriptServiceParam cameraSetDampingParams[] = {
			{ "dx", "number", ScriptServiceArgType::Number, true, "Smooth damping factor along X axis." },
			{ "dy", "number", ScriptServiceArgType::Number, true, "Smooth damping factor along Y axis." },
			{ "dz", "number", ScriptServiceArgType::Number, true, "Smooth damping factor along Z axis." },
		};

		static const ScriptServiceParam cameraSetDeadZoneParams[] = {
			{ "w", "number", ScriptServiceArgType::Number, true, "Normalized dead zone half-width [0, 1]." },
			{ "h", "number", ScriptServiceArgType::Number, true, "Normalized dead zone half-height [0, 1]." },
		};

		static const ScriptServiceParam cameraScreenToWorld2DParams[] = {
			{ "screenX", "number", ScriptServiceArgType::Number, true, "X pixel coordinate on screen or viewport." },
			{ "screenY", "number", ScriptServiceArgType::Number, true, "Y pixel coordinate on screen or viewport." },
		};

		static const ScriptServiceParam cameraWorldToScreen2DParams[] = {
			{ "worldX", "number", ScriptServiceArgType::Number, true, "X coordinate in 2D world units." },
			{ "worldY", "number", ScriptServiceArgType::Number, true, "Y coordinate in 2D world units." },
		};

		static const ScriptServiceMethod cameraMethods[] = {
			{ "Shake", [](const ScriptValue* args, std::size_t count) -> ScriptValue
			{
				return CameraShakeImpl(args, count);
			}, cameraShakeParams, 1, 1, "",
				"Apply screen shake trauma to the active virtual camera." },

			{ "SetTarget", [](const ScriptValue* args, std::size_t count) -> ScriptValue
			{
				return CameraSetTargetImpl(args, count);
			}, cameraSetTargetParams, 1, 1, "",
				"Set follow target entity on the active virtual camera." },

			{ "SetDamping", [](const ScriptValue* args, std::size_t count) -> ScriptValue
			{
				return CameraSetDampingImpl(args, count);
			}, cameraSetDampingParams, 3, 3, "",
				"Set smooth damping factors on the active virtual camera." },

			{ "SetDeadZone", [](const ScriptValue* args, std::size_t count) -> ScriptValue
			{
				return CameraSetDeadZoneImpl(args, count);
			}, cameraSetDeadZoneParams, 2, 2, "",
				"Set screen-space deadzone half extents on the active virtual camera." },

			{ "GetPosition", [](const ScriptValue* args, std::size_t count) -> ScriptValue
			{
				return CameraGetPositionImpl(args, count);
			}, nullptr, 0, 0, "table",
				"Get current primary camera world position as a { x, y, z } table." },

			{ "ScreenToWorld2D", [](const ScriptValue* args, std::size_t count) -> ScriptValue
			{
				return CameraScreenToWorld2DImpl(args, count);
			}, cameraScreenToWorld2DParams, 2, 2, "table",
				"Transform screen/viewport pixel coordinates into 2D world coordinates. Returns { x, y }." },

			{ "WorldToScreen2D", [](const ScriptValue* args, std::size_t count) -> ScriptValue
			{
				return CameraWorldToScreen2DImpl(args, count);
			}, cameraWorldToScreen2DParams, 2, 2, "table",
				"Transform 2D world coordinates into screen/viewport pixel coordinates. Returns { x, y }." },

			{ "GetMouseWorld2D", [](const ScriptValue* args, std::size_t count) -> ScriptValue
			{
				return CameraGetMouseWorld2DImpl(args, count);
			}, nullptr, 0, 0, "table",
				"Get the current mouse pointer location projected onto the 2D world plane. Returns { x, y }." },
		};

		// PostProcess
		static const ScriptServiceParam postProcessFlashWhiteParams[] = {
			{ "intensity", "number", ScriptServiceArgType::Number, false, "Flash intensity [0, 1] (default 1.0)." },
			{ "decay", "number", ScriptServiceArgType::Number, false, "Decay speed per second (default 5.0)." },
		};

		static const ScriptServiceParam postProcessSetOutlineParams[] = {
			{ "width", "number", ScriptServiceArgType::Number, true, "Outline width in pixels." },
			{ "r", "number", ScriptServiceArgType::Number, false, "Outline color Red component [0, 1] (default 1.0)." },
			{ "g", "number", ScriptServiceArgType::Number, false, "Outline color Green component [0, 1] (default 1.0)." },
			{ "b", "number", ScriptServiceArgType::Number, false, "Outline color Blue component [0, 1] (default 0.0)." },
			{ "a", "number", ScriptServiceArgType::Number, false, "Outline color Alpha component [0, 1] (default 1.0)." },
		};

		static const ScriptServiceParam postProcessSetDissolveParams[] = {
			{ "progress", "number", ScriptServiceArgType::Number, true, "Dissolve cutoff progress [0, 1]." },
		};

		static const ScriptServiceParam postProcessSetVignetteParams[] = {
			{ "intensity", "number", ScriptServiceArgType::Number, true, "Vignette border darkening intensity [0, 1]." },
		};

		static const ScriptServiceParam postProcessSetBloomParams[] = {
			{ "intensity", "number", ScriptServiceArgType::Number, true, "Bloom glow intensity." },
			{ "threshold", "number", ScriptServiceArgType::Number, false, "Luminance extraction threshold (default 1.0)." },
		};

		static const ScriptServiceParam postProcessSetChromaticAberrationParams[] = {
			{ "amount", "number", ScriptServiceArgType::Number, true, "Radial chromatic aberration fringing amount." },
		};

		static const ScriptServiceMethod postProcessMethods[] = {
			{ "FlashWhite", [](const ScriptValue* args, std::size_t count) -> ScriptValue
			{
				return PostProcessFlashWhiteImpl(args, count);
			}, postProcessFlashWhiteParams, 2, 2, "",
				"Trigger fullscreen white hit reaction flash." },

			{ "SetOutline", [](const ScriptValue* args, std::size_t count) -> ScriptValue
			{
				return PostProcessSetOutlineImpl(args, count);
			}, postProcessSetOutlineParams, 5, 5, "",
				"Configure fullscreen outline silhouette width and RGBA color." },

			{ "SetDissolve", [](const ScriptValue* args, std::size_t count) -> ScriptValue
			{
				return PostProcessSetDissolveImpl(args, count);
			}, postProcessSetDissolveParams, 1, 1, "",
				"Set dissolve effect transition cutoff progress." },

			{ "SetVignette", [](const ScriptValue* args, std::size_t count) -> ScriptValue
			{
				return PostProcessSetVignetteImpl(args, count);
			}, postProcessSetVignetteParams, 1, 1, "",
				"Set vignette darkening intensity." },

			{ "SetBloom", [](const ScriptValue* args, std::size_t count) -> ScriptValue
			{
				return PostProcessSetBloomImpl(args, count);
			}, postProcessSetBloomParams, 2, 2, "",
				"Configure bloom glow intensity and luminance threshold." },

			{ "SetChromaticAberration", [](const ScriptValue* args, std::size_t count) -> ScriptValue
			{
				return PostProcessSetChromaticAberrationImpl(args, count);
			}, postProcessSetChromaticAberrationParams, 1, 1, "",
				"Set radial chromatic aberration fringing intensity." },
		};

		static const ScriptServiceBinding cameraServiceBindingsTable[] = {
			{
				"Camera",
				"Read-only camera service table for virtual camera tracking, damping, deadzone, screen shake, and position queries.",
				cameraMethods,
				sizeof(cameraMethods) / sizeof(cameraMethods[0]),
			},
			{
				"PostProcess",
				"Read-only post-process service table for fullscreen effects: flash-white, outline, dissolve, vignette, bloom, and chromatic aberration.",
				postProcessMethods,
				sizeof(postProcessMethods) / sizeof(postProcessMethods[0]),
			},
		};

		bool RegisterServiceTable(ScriptBindingContext& bindings, const ScriptServiceBinding& service,
			std::string* error)
		{
			LuauVm* vm = bindings.Vm();
			if (!vm || !service.Name || !service.Name[0] || !service.Methods || service.MethodCount == 0)
			{
				if (error) *error = "invalid service binding descriptor";
				return false;
			}
			if (vm->GetGlobal(service.Name).Type() != ScriptValueType::Nil)
			{
				if (error) *error = std::string("global name is already in use: ") + service.Name;
				return false;
			}
			ScriptTableRef table = vm->CreateTable();
			if (!table.IsValid())
			{
				if (error) *error = std::string("failed to create the service table: ") + service.Name;
				return false;
			}
			for (std::size_t index = 0; index < service.MethodCount; ++index)
			{
				const ScriptServiceMethod& method = service.Methods[index];
				if (!method.Name || !method.Name[0] || !method.Function)
				{
					if (error) *error = std::string("service '") + service.Name + "' has an invalid method entry";
					return false;
				}
				std::size_t required = 0;
				for (std::size_t param = 0; param < method.ParamCount; ++param)
					if (method.Params[param].Required) ++required;
				if (required > method.ExpectedArgs || method.ExpectedArgs > method.ParamCount)
				{
					if (error) *error = std::string("service '") + service.Name + "." + method.Name +
						"' has an inconsistent parameter descriptor";
					return false;
				}
				if (!table.SetField(method.Name, bindings.CreateFunction(method.Name, method.Function)))
				{
					if (error) *error = std::string("failed to bind ") + service.Name + "." + method.Name;
					return false;
				}
			}
			if (!table.SetMetaField("__newindex", bindings.CreateFunction(service.Name,
				[](const ScriptValue*, std::size_t) -> ScriptValue
				{
					throw std::logic_error("service tables are read-only; use the documented methods");
				})))
			{
				if (error) *error = std::string("failed to lock the service table: ") + service.Name;
				return false;
			}
			if (!vm->SetGlobal(service.Name, table.ToValue()))
			{
				if (error) *error = std::string("failed to publish the service table: ") + service.Name;
				return false;
			}
			return true;
		}
	}

	bool RegisterCameraBindings(ScriptBindingContext& bindings, std::string* error)
	{
		std::size_t count = 0;
		const ScriptServiceBinding* services = CameraServiceBindings(&count);
		for (std::size_t index = 0; index < count; ++index)
		{
			if (!RegisterServiceTable(bindings, services[index], error))
				return false;
		}
		if (error) error->clear();
		return true;
	}

	const ScriptServiceBinding* CameraServiceBindings(std::size_t* count)
	{
		if (count)
			*count = sizeof(cameraServiceBindingsTable) / sizeof(cameraServiceBindingsTable[0]);
		return cameraServiceBindingsTable;
	}
}
