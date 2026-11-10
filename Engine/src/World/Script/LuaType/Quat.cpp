#include "wldpch.h"
#include "World/Script/Runtime/ScriptEngine.h"
#include "World/Script/LuaType/LuaTypeHelpers.h"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

namespace World
{
	using namespace LuaTypeDetail;

	namespace
	{
		glm::quat* Receiver(ScriptBindingContext& bindings, const ScriptValue* args, std::size_t count, const char* method)
		{
			return RequireReceiver<glm::quat>(bindings, "quat", args, count, method);
		}

		ScriptValue MakeQuat(ScriptBindingContext& bindings, const glm::quat& value)
		{
			return NewUserdataOf(bindings, "quat", value);
		}
	}

	void RegisterBuiltinQuatBinding(ScriptBindingContext& bindings)
	{
		const ScriptMethodBinding methods[] = {
			{ "identity", [](const ScriptValue* args, std::size_t count) -> ScriptValue
				{
					ScriptBindingContext& context = ScriptEngine::GetBindingContext();
					if (count != 0)
						throw std::logic_error("quat.identity expects no arguments");
					return MakeQuat(context, glm::quat(1.0f, 0.0f, 0.0f, 0.0f));
				} },
			{ "fromEuler", [](const ScriptValue* args, std::size_t count) -> ScriptValue
				{
					ScriptBindingContext& context = ScriptEngine::GetBindingContext();
					if (count != 3)
						throw std::logic_error("quat.fromEuler expects (xDeg, yDeg, zDeg)");
					const glm::vec3 degrees(
						RequireFloat(args[0], "quat.fromEuler xDeg"),
						RequireFloat(args[1], "quat.fromEuler yDeg"),
						RequireFloat(args[2], "quat.fromEuler zDeg"));
					return MakeQuat(context, glm::quat(glm::radians(degrees)));
				} },
			{ "fromAxisAngle", [](const ScriptValue* args, std::size_t count) -> ScriptValue
				{
					ScriptBindingContext& context = ScriptEngine::GetBindingContext();
					if (count != 2)
						throw std::logic_error("quat.fromAxisAngle expects (axis: vec3, degrees)");
					glm::vec3* axis = TryUnwrap<glm::vec3>(context, "vec3", args[0]);
					if (!axis)
						throw std::logic_error("quat.fromAxisAngle expects a vec3 axis");
					if (glm::dot(*axis, *axis) <= 1e-12f)
						throw std::logic_error("quat.fromAxisAngle requires a non-zero axis");
					const float degrees = RequireFloat(args[1], "quat.fromAxisAngle degrees");
					return MakeQuat(context, glm::angleAxis(glm::radians(degrees), glm::normalize(*axis)));
				} },
		};

		const ScriptMetaMethodBinding metaMethods[] = {
			{ "__index", [](const ScriptValue* args, std::size_t count) -> ScriptValue
				{
					ScriptBindingContext& context = ScriptEngine::GetBindingContext();
					glm::quat* self = Receiver(context, args, count, "quat:__index");
					std::string key;
					if (count < 2 || !args[1].AsString(&key))
						return ScriptValue::Nil();
					if (key == "x") return ScriptValue::Number(self->x);
					if (key == "y") return ScriptValue::Number(self->y);
					if (key == "z") return ScriptValue::Number(self->z);
					if (key == "w") return ScriptValue::Number(self->w);
					return context.MethodsTable("quat").GetField(key.c_str());
				} },
			{ "__newindex", [](const ScriptValue* args, std::size_t count) -> ScriptValue
				{
					ScriptBindingContext& context = ScriptEngine::GetBindingContext();
					glm::quat* self = Receiver(context, args, count, "quat:__newindex");
					if (count < 3)
						throw std::logic_error("quat field assignment needs a value");
					const std::string key = RequireString(args[1], "quat field name");
					if (key == "x") { self->x = RequireFloat(args[2], "quat.x"); return ScriptValue::Nil(); }
					if (key == "y") { self->y = RequireFloat(args[2], "quat.y"); return ScriptValue::Nil(); }
					if (key == "z") { self->z = RequireFloat(args[2], "quat.z"); return ScriptValue::Nil(); }
					if (key == "w") { self->w = RequireFloat(args[2], "quat.w"); return ScriptValue::Nil(); }
					throw std::logic_error("quat has no writable field '" + key + "'");
				} },
			{ "__mul", [](const ScriptValue* args, std::size_t count) -> ScriptValue
				{
					ScriptBindingContext& context = ScriptEngine::GetBindingContext();
					glm::quat* left = count > 0 ? TryUnwrap<glm::quat>(context, "quat", args[0]) : nullptr;
					if (!left)
						throw std::logic_error("quat multiplication expects a quat receiver");
					glm::quat* rightQuat = count > 1 ? TryUnwrap<glm::quat>(context, "quat", args[1]) : nullptr;
					if (rightQuat)
						return MakeQuat(context, (*left) * (*rightQuat));
					glm::vec3* rightVector = count > 1 ? TryUnwrap<glm::vec3>(context, "vec3", args[1]) : nullptr;
					if (rightVector)
						return NewUserdataOf(context, "vec3", (*left) * (*rightVector));
					throw std::logic_error("quat * ... expects (quat, quat) or (quat, vec3)");
				} },
		};

		ScriptUserTypeDesc desc;
		desc.Name = "quat";
		desc.UserdataSize = sizeof(glm::quat);
		desc.Constructor = [](void* data, const ScriptValue* args, std::size_t count)
			{
				if (count != 4)
					throw std::logic_error("quat.new expects (x, y, z, w)");
				// glm::quat 的构造参数顺序是 (w, x, y, z);脚本 API 按通常的 (x, y, z, w) 暴露。
				new (data) glm::quat(
					RequireFloat(args[3], "quat.new w"),
					RequireFloat(args[0], "quat.new x"),
					RequireFloat(args[1], "quat.new y"),
					RequireFloat(args[2], "quat.new z"));
			};
		desc.Methods = methods;
		desc.MethodCount = sizeof(methods) / sizeof(methods[0]);
		desc.MetaMethods = metaMethods;
		desc.MetaMethodCount = sizeof(metaMethods) / sizeof(metaMethods[0]);

		std::string error;
		if (!bindings.RegisterUserType(desc, &error))
			throw std::logic_error("quat registration failed: " + error);
	}
}
