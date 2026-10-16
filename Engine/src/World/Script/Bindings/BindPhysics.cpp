#include "wldpch.h"
#include "World/Script/Bindings/BindPhysics.h"

#include "World/Physics/CharacterController.h"
#include "World/Physics/PhysicsQuery.h"
#include "World/Scene/Components.h"
#include "World/Scene/Entity.h"
#include "World/Scene/Scene.h"
#include "World/Script/Bindings/BindServices.h"
#include "World/Script/Runtime/ScriptEngine.h"
#include "World/Script/Vm/LuauVm.h"
#include "World/Script/Vm/ScriptBindingContext.h"
#include "World/Script/Vm/ScriptRef.h"
#include "World/Script/Vm/ScriptValue.h"

#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
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

		// ---- 方法实现 ----

		ScriptValue PhysicsRaycastImpl(const ScriptValue* args, std::size_t count)
		{
			NormalizeArgs(args, count);
			const ScriptServiceMethod& method = PhysicsServiceBindings(nullptr)[0].Methods[0];
			CheckArgumentCount("Physics", method, count, 6);
			Scene* scene = ScriptEngine::GetActiveScene();
			if (!scene)
				throw std::logic_error("Physics.Raycast requires an active scene");

			const float ox = static_cast<float>(RequireNumber("Physics", method, args, count, 0));
			const float oy = static_cast<float>(RequireNumber("Physics", method, args, count, 1));
			const float oz = static_cast<float>(RequireNumber("Physics", method, args, count, 2));
			const float dx = static_cast<float>(RequireNumber("Physics", method, args, count, 3));
			const float dy = static_cast<float>(RequireNumber("Physics", method, args, count, 4));
			const float dz = static_cast<float>(RequireNumber("Physics", method, args, count, 5));
			const float maxDist = static_cast<float>(OptionalNumber("Physics", method, args, count, 6, 1000.0));
			const uint32_t mask = static_cast<uint32_t>(static_cast<uint64_t>(OptionalNumber("Physics", method, args, count, 7, 4294967295.0)));

			Physics::RaycastHit hit = Physics::PhysicsQuerySystem::Raycast(*scene, glm::vec3(ox, oy, oz), glm::vec3(dx, dy, dz), maxDist, mask);

			ScriptTableRef result = ScriptEngine::GetState().CreateTable();
			result.SetField("Hit", ScriptValue::Boolean(hit.Hit));
			result.SetField("hit", ScriptValue::Boolean(hit.Hit));
			result.SetField("Distance", ScriptValue::Number(static_cast<double>(hit.Distance)));
			result.SetField("distance", ScriptValue::Number(static_cast<double>(hit.Distance)));

			ScriptTableRef pt = ScriptEngine::GetState().CreateTable();
			pt.SetField("x", ScriptValue::Number(static_cast<double>(hit.Point.x)));
			pt.SetField("y", ScriptValue::Number(static_cast<double>(hit.Point.y)));
			pt.SetField("z", ScriptValue::Number(static_cast<double>(hit.Point.z)));
			pt.SetArrayElement(1, ScriptValue::Number(static_cast<double>(hit.Point.x)));
			pt.SetArrayElement(2, ScriptValue::Number(static_cast<double>(hit.Point.y)));
			pt.SetArrayElement(3, ScriptValue::Number(static_cast<double>(hit.Point.z)));
			result.SetField("Point", pt.ToValue());
			result.SetField("point", pt.ToValue());

			ScriptTableRef norm = ScriptEngine::GetState().CreateTable();
			norm.SetField("x", ScriptValue::Number(static_cast<double>(hit.Normal.x)));
			norm.SetField("y", ScriptValue::Number(static_cast<double>(hit.Normal.y)));
			norm.SetField("z", ScriptValue::Number(static_cast<double>(hit.Normal.z)));
			norm.SetArrayElement(1, ScriptValue::Number(static_cast<double>(hit.Normal.x)));
			norm.SetArrayElement(2, ScriptValue::Number(static_cast<double>(hit.Normal.y)));
			norm.SetArrayElement(3, ScriptValue::Number(static_cast<double>(hit.Normal.z)));
			result.SetField("Normal", norm.ToValue());
			result.SetField("normal", norm.ToValue());

			const double entId = (hit.Hit && hit.Entity != entt::null) ? static_cast<double>(static_cast<uint32_t>(hit.Entity)) : 0.0;
			result.SetField("Entity", ScriptValue::Number(entId));
			result.SetField("entity", ScriptValue::Number(entId));

			return result.ToValue();
		}

		ScriptValue PhysicsRaycast2DImpl(const ScriptValue* args, std::size_t count)
		{
			NormalizeArgs(args, count);
			const ScriptServiceMethod& method = PhysicsServiceBindings(nullptr)[0].Methods[1];
			CheckArgumentCount("Physics", method, count, 4);
			Scene* scene = ScriptEngine::GetActiveScene();
			if (!scene)
				throw std::logic_error("Physics.Raycast2D requires an active scene");

			const float ox = static_cast<float>(RequireNumber("Physics", method, args, count, 0));
			const float oy = static_cast<float>(RequireNumber("Physics", method, args, count, 1));
			const float dx = static_cast<float>(RequireNumber("Physics", method, args, count, 2));
			const float dy = static_cast<float>(RequireNumber("Physics", method, args, count, 3));
			const float maxDist = static_cast<float>(OptionalNumber("Physics", method, args, count, 4, 1000.0));
			const uint32_t mask = static_cast<uint32_t>(static_cast<uint64_t>(OptionalNumber("Physics", method, args, count, 5, 4294967295.0)));

			Physics::RaycastHit hit = Physics::PhysicsQuerySystem::Raycast2D(*scene, glm::vec2(ox, oy), glm::vec2(dx, dy), maxDist, mask);

			ScriptTableRef result = ScriptEngine::GetState().CreateTable();
			result.SetField("Hit", ScriptValue::Boolean(hit.Hit));
			result.SetField("hit", ScriptValue::Boolean(hit.Hit));
			result.SetField("Distance", ScriptValue::Number(static_cast<double>(hit.Distance)));
			result.SetField("distance", ScriptValue::Number(static_cast<double>(hit.Distance)));

			ScriptTableRef pt = ScriptEngine::GetState().CreateTable();
			pt.SetField("x", ScriptValue::Number(static_cast<double>(hit.Point.x)));
			pt.SetField("y", ScriptValue::Number(static_cast<double>(hit.Point.y)));
			pt.SetArrayElement(1, ScriptValue::Number(static_cast<double>(hit.Point.x)));
			pt.SetArrayElement(2, ScriptValue::Number(static_cast<double>(hit.Point.y)));
			result.SetField("Point", pt.ToValue());
			result.SetField("point", pt.ToValue());

			ScriptTableRef norm = ScriptEngine::GetState().CreateTable();
			norm.SetField("x", ScriptValue::Number(static_cast<double>(hit.Normal.x)));
			norm.SetField("y", ScriptValue::Number(static_cast<double>(hit.Normal.y)));
			norm.SetArrayElement(1, ScriptValue::Number(static_cast<double>(hit.Normal.x)));
			norm.SetArrayElement(2, ScriptValue::Number(static_cast<double>(hit.Normal.y)));
			result.SetField("Normal", norm.ToValue());
			result.SetField("normal", norm.ToValue());

			const double entId = (hit.Hit && hit.Entity != entt::null) ? static_cast<double>(static_cast<uint32_t>(hit.Entity)) : 0.0;
			result.SetField("Entity", ScriptValue::Number(entId));
			result.SetField("entity", ScriptValue::Number(entId));

			return result.ToValue();
		}

		ScriptValue PhysicsSphereCastImpl(const ScriptValue* args, std::size_t count)
		{
			NormalizeArgs(args, count);
			const ScriptServiceMethod& method = PhysicsServiceBindings(nullptr)[0].Methods[2];
			CheckArgumentCount("Physics", method, count, 7);
			Scene* scene = ScriptEngine::GetActiveScene();
			if (!scene)
				throw std::logic_error("Physics.SphereCast requires an active scene");

			const float ox = static_cast<float>(RequireNumber("Physics", method, args, count, 0));
			const float oy = static_cast<float>(RequireNumber("Physics", method, args, count, 1));
			const float oz = static_cast<float>(RequireNumber("Physics", method, args, count, 2));
			const float radius = static_cast<float>(RequireNumber("Physics", method, args, count, 3));
			const float dx = static_cast<float>(RequireNumber("Physics", method, args, count, 4));
			const float dy = static_cast<float>(RequireNumber("Physics", method, args, count, 5));
			const float dz = static_cast<float>(RequireNumber("Physics", method, args, count, 6));
			const float maxDist = static_cast<float>(OptionalNumber("Physics", method, args, count, 7, 1000.0));
			const uint32_t mask = static_cast<uint32_t>(static_cast<uint64_t>(OptionalNumber("Physics", method, args, count, 8, 4294967295.0)));

			Physics::ShapeCastHit hit = Physics::PhysicsQuerySystem::SphereCast(*scene, glm::vec3(ox, oy, oz), radius, glm::vec3(dx, dy, dz), maxDist, mask);

			ScriptTableRef result = ScriptEngine::GetState().CreateTable();
			result.SetField("Hit", ScriptValue::Boolean(hit.Hit));
			result.SetField("hit", ScriptValue::Boolean(hit.Hit));
			result.SetField("Distance", ScriptValue::Number(static_cast<double>(hit.Distance)));
			result.SetField("distance", ScriptValue::Number(static_cast<double>(hit.Distance)));
			result.SetField("PenetrationDepth", ScriptValue::Number(static_cast<double>(hit.PenetrationDepth)));
			result.SetField("penetrationDepth", ScriptValue::Number(static_cast<double>(hit.PenetrationDepth)));

			ScriptTableRef pt = ScriptEngine::GetState().CreateTable();
			pt.SetField("x", ScriptValue::Number(static_cast<double>(hit.Point.x)));
			pt.SetField("y", ScriptValue::Number(static_cast<double>(hit.Point.y)));
			pt.SetField("z", ScriptValue::Number(static_cast<double>(hit.Point.z)));
			pt.SetArrayElement(1, ScriptValue::Number(static_cast<double>(hit.Point.x)));
			pt.SetArrayElement(2, ScriptValue::Number(static_cast<double>(hit.Point.y)));
			pt.SetArrayElement(3, ScriptValue::Number(static_cast<double>(hit.Point.z)));
			result.SetField("Point", pt.ToValue());
			result.SetField("point", pt.ToValue());

			ScriptTableRef norm = ScriptEngine::GetState().CreateTable();
			norm.SetField("x", ScriptValue::Number(static_cast<double>(hit.Normal.x)));
			norm.SetField("y", ScriptValue::Number(static_cast<double>(hit.Normal.y)));
			norm.SetField("z", ScriptValue::Number(static_cast<double>(hit.Normal.z)));
			norm.SetArrayElement(1, ScriptValue::Number(static_cast<double>(hit.Normal.x)));
			norm.SetArrayElement(2, ScriptValue::Number(static_cast<double>(hit.Normal.y)));
			norm.SetArrayElement(3, ScriptValue::Number(static_cast<double>(hit.Normal.z)));
			result.SetField("Normal", norm.ToValue());
			result.SetField("normal", norm.ToValue());

			const double entId = (hit.Hit && hit.Entity != entt::null) ? static_cast<double>(static_cast<uint32_t>(hit.Entity)) : 0.0;
			result.SetField("Entity", ScriptValue::Number(entId));
			result.SetField("entity", ScriptValue::Number(entId));

			return result.ToValue();
		}

		ScriptValue PhysicsOverlapSphereImpl(const ScriptValue* args, std::size_t count)
		{
			NormalizeArgs(args, count);
			const ScriptServiceMethod& method = PhysicsServiceBindings(nullptr)[0].Methods[3];
			CheckArgumentCount("Physics", method, count, 4);
			Scene* scene = ScriptEngine::GetActiveScene();
			if (!scene)
				throw std::logic_error("Physics.OverlapSphere requires an active scene");

			const float cx = static_cast<float>(RequireNumber("Physics", method, args, count, 0));
			const float cy = static_cast<float>(RequireNumber("Physics", method, args, count, 1));
			const float cz = static_cast<float>(RequireNumber("Physics", method, args, count, 2));
			const float radius = static_cast<float>(RequireNumber("Physics", method, args, count, 3));
			const uint32_t mask = static_cast<uint32_t>(static_cast<uint64_t>(OptionalNumber("Physics", method, args, count, 4, 4294967295.0)));

			std::vector<entt::entity> entities = Physics::PhysicsQuerySystem::OverlapSphere(*scene, glm::vec3(cx, cy, cz), radius, mask);

			ScriptTableRef result = ScriptEngine::GetState().CreateTable();
			for (std::size_t i = 0; i < entities.size(); ++i)
			{
				result.SetArrayElement(i + 1, ScriptValue::Number(static_cast<double>(static_cast<uint32_t>(entities[i]))));
			}
			result.SetField("Count", ScriptValue::Number(static_cast<double>(entities.size())));
			result.SetField("count", ScriptValue::Number(static_cast<double>(entities.size())));

			return result.ToValue();
		}

		ScriptValue PhysicsOverlapCircleImpl(const ScriptValue* args, std::size_t count)
		{
			NormalizeArgs(args, count);
			const ScriptServiceMethod& method = PhysicsServiceBindings(nullptr)[0].Methods[4];
			CheckArgumentCount("Physics", method, count, 3);
			Scene* scene = ScriptEngine::GetActiveScene();
			if (!scene)
				throw std::logic_error("Physics.OverlapCircle requires an active scene");

			const float cx = static_cast<float>(RequireNumber("Physics", method, args, count, 0));
			const float cy = static_cast<float>(RequireNumber("Physics", method, args, count, 1));
			const float radius = static_cast<float>(RequireNumber("Physics", method, args, count, 2));
			const uint32_t mask = static_cast<uint32_t>(static_cast<uint64_t>(OptionalNumber("Physics", method, args, count, 3, 4294967295.0)));

			std::vector<entt::entity> entities = Physics::PhysicsQuerySystem::OverlapSphere(*scene, glm::vec3(cx, cy, 0.0f), radius, mask);

			ScriptTableRef result = ScriptEngine::GetState().CreateTable();
			for (std::size_t i = 0; i < entities.size(); ++i)
			{
				result.SetArrayElement(i + 1, ScriptValue::Number(static_cast<double>(static_cast<uint32_t>(entities[i]))));
			}
			result.SetField("Count", ScriptValue::Number(static_cast<double>(entities.size())));
			result.SetField("count", ScriptValue::Number(static_cast<double>(entities.size())));

			return result.ToValue();
		}

		ScriptValue PhysicsMoveAndSlideImpl(const ScriptValue* args, std::size_t count)
		{
			NormalizeArgs(args, count);
			const ScriptServiceMethod& method = PhysicsServiceBindings(nullptr)[0].Methods[5];
			CheckArgumentCount("Physics", method, count, 5);
			Scene* scene = ScriptEngine::GetActiveScene();
			if (!scene)
				throw std::logic_error("Physics.MoveAndSlide requires an active scene");

			entt::entity ent = RequireEntityId("Physics", method, args, count, 0);
			const float dispX = static_cast<float>(RequireNumber("Physics", method, args, count, 1));
			const float dispY = static_cast<float>(RequireNumber("Physics", method, args, count, 2));
			const float dispZ = static_cast<float>(RequireNumber("Physics", method, args, count, 3));
			const float dt = static_cast<float>(RequireNumber("Physics", method, args, count, 4));

			glm::vec3 actualDisplacement = CharacterControllerSystem::MoveAndSlide(*scene, ent, glm::vec3(dispX, dispY, dispZ), dt);

			ScriptTableRef result = ScriptEngine::GetState().CreateTable();
			result.SetField("x", ScriptValue::Number(static_cast<double>(actualDisplacement.x)));
			result.SetField("y", ScriptValue::Number(static_cast<double>(actualDisplacement.y)));
			result.SetField("z", ScriptValue::Number(static_cast<double>(actualDisplacement.z)));
			result.SetArrayElement(1, ScriptValue::Number(static_cast<double>(actualDisplacement.x)));
			result.SetArrayElement(2, ScriptValue::Number(static_cast<double>(actualDisplacement.y)));
			result.SetArrayElement(3, ScriptValue::Number(static_cast<double>(actualDisplacement.z)));

			return result.ToValue();
		}

		ScriptValue PhysicsCanJumpImpl(const ScriptValue* args, std::size_t count)
		{
			NormalizeArgs(args, count);
			const ScriptServiceMethod& method = PhysicsServiceBindings(nullptr)[0].Methods[6];
			CheckArgumentCount("Physics", method, count, 1);
			Scene* scene = ScriptEngine::GetActiveScene();
			if (!scene)
				throw std::logic_error("Physics.CanJump requires an active scene");

			entt::entity ent = RequireEntityId("Physics", method, args, count, 0);

			const auto* kcc = scene->GetRegistry().try_get<CharacterControllerComponent>(ent);
			if (!kcc)
				return ScriptValue::Boolean(false);

			return ScriptValue::Boolean(CharacterControllerSystem::CanJump(*kcc));
		}

		ScriptValue PhysicsTryJumpImpl(const ScriptValue* args, std::size_t count)
		{
			NormalizeArgs(args, count);
			const ScriptServiceMethod& method = PhysicsServiceBindings(nullptr)[0].Methods[7];
			CheckArgumentCount("Physics", method, count, 2);
			Scene* scene = ScriptEngine::GetActiveScene();
			if (!scene)
				throw std::logic_error("Physics.TryJump requires an active scene");

			entt::entity ent = RequireEntityId("Physics", method, args, count, 0);
			const float jumpSpeed = static_cast<float>(RequireNumber("Physics", method, args, count, 1));

			auto* kcc = scene->GetRegistry().try_get<CharacterControllerComponent>(ent);
			if (!kcc)
				return ScriptValue::Boolean(false);

			return ScriptValue::Boolean(CharacterControllerSystem::TryJump(*kcc, jumpSpeed));
		}

		// ---- 参数与方法描述 ----

		static const ScriptServiceParam physicsRaycastParams[] = {
			{ "originX", "number", ScriptServiceArgType::Number, true, "Ray origin X in world units." },
			{ "originY", "number", ScriptServiceArgType::Number, true, "Ray origin Y in world units." },
			{ "originZ", "number", ScriptServiceArgType::Number, true, "Ray origin Z in world units." },
			{ "dirX", "number", ScriptServiceArgType::Number, true, "Ray direction X." },
			{ "dirY", "number", ScriptServiceArgType::Number, true, "Ray direction Y." },
			{ "dirZ", "number", ScriptServiceArgType::Number, true, "Ray direction Z." },
			{ "maxDist", "number", ScriptServiceArgType::Number, false, "Maximum ray distance (default 1000.0)." },
			{ "mask", "number", ScriptServiceArgType::Number, false, "Collision layer bitmask (default 0xFFFFFFFF)." },
		};

		static const ScriptServiceParam physicsRaycast2DParams[] = {
			{ "originX", "number", ScriptServiceArgType::Number, true, "Ray origin X in world units." },
			{ "originY", "number", ScriptServiceArgType::Number, true, "Ray origin Y in world units." },
			{ "dirX", "number", ScriptServiceArgType::Number, true, "Ray direction X." },
			{ "dirY", "number", ScriptServiceArgType::Number, true, "Ray direction Y." },
			{ "maxDist", "number", ScriptServiceArgType::Number, false, "Maximum ray distance (default 1000.0)." },
			{ "mask", "number", ScriptServiceArgType::Number, false, "Collision layer bitmask (default 0xFFFFFFFF)." },
		};

		static const ScriptServiceParam physicsSphereCastParams[] = {
			{ "originX", "number", ScriptServiceArgType::Number, true, "Sphere origin X in world units." },
			{ "originY", "number", ScriptServiceArgType::Number, true, "Sphere origin Y in world units." },
			{ "originZ", "number", ScriptServiceArgType::Number, true, "Sphere origin Z in world units." },
			{ "radius", "number", ScriptServiceArgType::Number, true, "Sphere radius in world units." },
			{ "dirX", "number", ScriptServiceArgType::Number, true, "Cast direction X." },
			{ "dirY", "number", ScriptServiceArgType::Number, true, "Cast direction Y." },
			{ "dirZ", "number", ScriptServiceArgType::Number, true, "Cast direction Z." },
			{ "maxDist", "number", ScriptServiceArgType::Number, false, "Maximum cast distance (default 1000.0)." },
			{ "mask", "number", ScriptServiceArgType::Number, false, "Collision layer bitmask (default 0xFFFFFFFF)." },
		};

		static const ScriptServiceParam physicsOverlapSphereParams[] = {
			{ "centerX", "number", ScriptServiceArgType::Number, true, "Sphere center X in world units." },
			{ "centerY", "number", ScriptServiceArgType::Number, true, "Sphere center Y in world units." },
			{ "centerZ", "number", ScriptServiceArgType::Number, true, "Sphere center Z in world units." },
			{ "radius", "number", ScriptServiceArgType::Number, true, "Sphere radius in world units." },
			{ "mask", "number", ScriptServiceArgType::Number, false, "Collision layer bitmask (default 0xFFFFFFFF)." },
		};

		static const ScriptServiceParam physicsOverlapCircleParams[] = {
			{ "centerX", "number", ScriptServiceArgType::Number, true, "Circle center X in world units." },
			{ "centerY", "number", ScriptServiceArgType::Number, true, "Circle center Y in world units." },
			{ "radius", "number", ScriptServiceArgType::Number, true, "Circle radius in world units." },
			{ "mask", "number", ScriptServiceArgType::Number, false, "Collision layer bitmask (default 0xFFFFFFFF)." },
		};

		static const ScriptServiceParam physicsMoveAndSlideParams[] = {
			{ "entityId", "number", ScriptServiceArgType::Number, true, "Character controller entity ID or Entity object." },
			{ "dispX", "number", ScriptServiceArgType::Number, true, "Desired displacement X." },
			{ "dispY", "number", ScriptServiceArgType::Number, true, "Desired displacement Y." },
			{ "dispZ", "number", ScriptServiceArgType::Number, true, "Desired displacement Z." },
			{ "dt", "number", ScriptServiceArgType::Number, true, "Delta time in seconds." },
		};

		static const ScriptServiceParam physicsCanJumpParams[] = {
			{ "entityId", "number", ScriptServiceArgType::Number, true, "Character controller entity ID or Entity object." },
		};

		static const ScriptServiceParam physicsTryJumpParams[] = {
			{ "entityId", "number", ScriptServiceArgType::Number, true, "Character controller entity ID or Entity object." },
			{ "jumpSpeed", "number", ScriptServiceArgType::Number, true, "Vertical jump speed." },
		};

		static const ScriptServiceMethod physicsMethods[] = {
			{ "Raycast", [](const ScriptValue* args, std::size_t count) -> ScriptValue
			{
				return PhysicsRaycastImpl(args, count);
			}, physicsRaycastParams, 8, 8, "table",
				"Perform a 3D raycast against scene colliders. Returns { Hit, Distance, Point, Normal, Entity }." },

			{ "Raycast2D", [](const ScriptValue* args, std::size_t count) -> ScriptValue
			{
				return PhysicsRaycast2DImpl(args, count);
			}, physicsRaycast2DParams, 6, 6, "table",
				"Perform a 2D raycast in XY plane against scene colliders. Returns { Hit, Distance, Point, Normal, Entity }." },

			{ "SphereCast", [](const ScriptValue* args, std::size_t count) -> ScriptValue
			{
				return PhysicsSphereCastImpl(args, count);
			}, physicsSphereCastParams, 9, 9, "table",
				"Perform a 3D sphere sweep cast against scene colliders. Returns { Hit, Distance, Point, Normal, Entity, PenetrationDepth }." },

			{ "OverlapSphere", [](const ScriptValue* args, std::size_t count) -> ScriptValue
			{
				return PhysicsOverlapSphereImpl(args, count);
			}, physicsOverlapSphereParams, 5, 5, "table",
				"Query overlapping entities within a 3D sphere. Returns array of entity IDs with .Count." },

			{ "OverlapCircle", [](const ScriptValue* args, std::size_t count) -> ScriptValue
			{
				return PhysicsOverlapCircleImpl(args, count);
			}, physicsOverlapCircleParams, 4, 4, "table",
				"Query overlapping entities within a 2D circle in XY plane. Returns array of entity IDs with .Count." },

			{ "MoveAndSlide", [](const ScriptValue* args, std::size_t count) -> ScriptValue
			{
				return PhysicsMoveAndSlideImpl(args, count);
			}, physicsMoveAndSlideParams, 5, 5, "table",
				"Execute kinematic move-and-slide projection for character controller. Returns { x, y, z }." },

			{ "CanJump", [](const ScriptValue* args, std::size_t count) -> ScriptValue
			{
				return PhysicsCanJumpImpl(args, count);
			}, physicsCanJumpParams, 1, 1, "boolean",
				"Check if character controller entity is grounded or within coyote time window." },

			{ "TryJump", [](const ScriptValue* args, std::size_t count) -> ScriptValue
			{
				return PhysicsTryJumpImpl(args, count);
			}, physicsTryJumpParams, 2, 2, "boolean",
				"Attempt jump with jump buffer and coyote time evaluation. Returns true if jump executed." },
		};

		static const ScriptServiceBinding physicsServiceBindingsTable[] = {
			{
				"Physics",
				"Read-only physics service table for 2D/3D raycasts, sweeps, overlaps, and kinematic character control.",
				physicsMethods,
				sizeof(physicsMethods) / sizeof(physicsMethods[0]),
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

	bool RegisterPhysicsBindings(ScriptBindingContext& bindings, std::string* error)
	{
		std::size_t count = 0;
		const ScriptServiceBinding* services = PhysicsServiceBindings(&count);
		for (std::size_t index = 0; index < count; ++index)
		{
			if (!RegisterServiceTable(bindings, services[index], error))
				return false;
		}
		if (error) error->clear();
		return true;
	}

	const ScriptServiceBinding* PhysicsServiceBindings(std::size_t* count)
	{
		if (count)
			*count = sizeof(physicsServiceBindingsTable) / sizeof(physicsServiceBindingsTable[0]);
		return physicsServiceBindingsTable;
	}
}
