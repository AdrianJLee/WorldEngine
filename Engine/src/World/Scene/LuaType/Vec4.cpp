#include "wldpch.h"
#include "World/Scene/ScriptEngine.h"
#include "LuaTypeHelpers.h"

#include <glm/glm.hpp>

namespace World
{
	using namespace LuaTypeDetail;

	namespace
	{
		glm::vec4* Receiver(ScriptBindingContext& bindings, const ScriptValue* args, std::size_t count, const char* method)
		{
			return RequireReceiver<glm::vec4>(bindings, "vec4", args, count, method);
		}
	}

	void RegisterBuiltinVec4Binding(ScriptBindingContext& bindings)
	{
		const ScriptMethodBinding methods[] = {
			{ "length", [](const ScriptValue* args, std::size_t count) -> ScriptValue
				{
					ScriptBindingContext& context = ScriptEngine::GetBindingContext();
					return ScriptValue::Number(glm::length(*Receiver(context, args, count, "vec4:length")));
				} },
		};

		const ScriptMetaMethodBinding metaMethods[] = {
			{ "__index", [](const ScriptValue* args, std::size_t count) -> ScriptValue
				{
					ScriptBindingContext& context = ScriptEngine::GetBindingContext();
					glm::vec4* self = Receiver(context, args, count, "vec4:__index");
					std::string key;
					if (count < 2 || !args[1].AsString(&key))
						return ScriptValue::Nil();
					if (key == "x") return ScriptValue::Number(self->x);
					if (key == "y") return ScriptValue::Number(self->y);
					if (key == "z") return ScriptValue::Number(self->z);
					if (key == "w") return ScriptValue::Number(self->w);
					return context.MethodsTable("vec4").GetField(key.c_str());
				} },
			{ "__newindex", [](const ScriptValue* args, std::size_t count) -> ScriptValue
				{
					ScriptBindingContext& context = ScriptEngine::GetBindingContext();
					glm::vec4* self = Receiver(context, args, count, "vec4:__newindex");
					if (count < 3)
						throw std::logic_error("vec4 field assignment needs a value");
					const std::string key = RequireString(args[1], "vec4 field name");
					if (key == "x") { self->x = RequireFloat(args[2], "vec4.x"); return ScriptValue::Nil(); }
					if (key == "y") { self->y = RequireFloat(args[2], "vec4.y"); return ScriptValue::Nil(); }
					if (key == "z") { self->z = RequireFloat(args[2], "vec4.z"); return ScriptValue::Nil(); }
					if (key == "w") { self->w = RequireFloat(args[2], "vec4.w"); return ScriptValue::Nil(); }
					throw std::logic_error("vec4 has no writable field '" + key + "'");
				} },
		};

		ScriptUserTypeDesc desc;
		desc.Name = "vec4";
		desc.UserdataSize = sizeof(glm::vec4);
		desc.Constructor = [](void* data, const ScriptValue* args, std::size_t count)
			{
				if (count == 0)
				{
					new (data) glm::vec4();
					return;
				}
				if (count == 1)
				{
					new (data) glm::vec4(RequireFloat(args[0], "vec4.new"));
					return;
				}
				if (count == 4)
				{
					new (data) glm::vec4(RequireFloat(args[0], "vec4.new"), RequireFloat(args[1], "vec4.new"),
						RequireFloat(args[2], "vec4.new"), RequireFloat(args[3], "vec4.new"));
					return;
				}
				throw std::logic_error("vec4.new expects 0, 1 or 4 arguments");
			};
		desc.Methods = methods;
		desc.MethodCount = sizeof(methods) / sizeof(methods[0]);
		desc.MetaMethods = metaMethods;
		desc.MetaMethodCount = sizeof(metaMethods) / sizeof(metaMethods[0]);

		std::string error;
		if (!bindings.RegisterUserType(desc, &error))
			throw std::logic_error("vec4 registration failed: " + error);
	}
}
