#include "wldpch.h"
#include "World/Scene/ScriptEngine.h"
#include "LuaTypeHelpers.h"

#include <glm/glm.hpp>

namespace World
{
	using namespace LuaTypeDetail;

	namespace
	{
		glm::vec2* Receiver(ScriptBindingContext& bindings, const ScriptValue* args, std::size_t count, const char* method)
		{
			return RequireReceiver<glm::vec2>(bindings, "vec2", args, count, method);
		}

		ScriptValue MakeVec2(ScriptBindingContext& bindings, const glm::vec2& value)
		{
			return NewUserdataOf(bindings, "vec2", value);
		}
	}

	void RegisterBuiltinVec2Binding(ScriptBindingContext& bindings)
	{
		const ScriptMethodBinding methods[] = {
			{ "length", [](const ScriptValue* args, std::size_t count) -> ScriptValue
				{
					ScriptBindingContext& context = ScriptEngine::GetBindingContext();
					return ScriptValue::Number(glm::length(*Receiver(context, args, count, "vec2:length")));
				} },
		};

		const ScriptMetaMethodBinding metaMethods[] = {
			{ "__index", [](const ScriptValue* args, std::size_t count) -> ScriptValue
				{
					ScriptBindingContext& context = ScriptEngine::GetBindingContext();
					glm::vec2* self = Receiver(context, args, count, "vec2:__index");
					std::string key;
					if (count < 2 || !args[1].AsString(&key))
						return ScriptValue::Nil();
					if (key == "x") return ScriptValue::Number(self->x);
					if (key == "y") return ScriptValue::Number(self->y);
					return context.MethodsTable("vec2").GetField(key.c_str());
				} },
			{ "__newindex", [](const ScriptValue* args, std::size_t count) -> ScriptValue
				{
					ScriptBindingContext& context = ScriptEngine::GetBindingContext();
					glm::vec2* self = Receiver(context, args, count, "vec2:__newindex");
					if (count < 3)
						throw std::logic_error("vec2 field assignment needs a value");
					const std::string key = RequireString(args[1], "vec2 field name");
					if (key == "x") { self->x = RequireFloat(args[2], "vec2.x"); return ScriptValue::Nil(); }
					if (key == "y") { self->y = RequireFloat(args[2], "vec2.y"); return ScriptValue::Nil(); }
					throw std::logic_error("vec2 has no writable field '" + key + "'");
				} },
			{ "__add", [](const ScriptValue* args, std::size_t count) -> ScriptValue
				{
					ScriptBindingContext& context = ScriptEngine::GetBindingContext();
					glm::vec2* left = Receiver(context, args, count, "vec2:__add");
					glm::vec2* right = count > 1 ? TryUnwrap<glm::vec2>(context, "vec2", args[1]) : nullptr;
					if (!right)
						throw std::logic_error("vec2 + vec2 expects another vec2");
					return MakeVec2(context, *left + *right);
				} },
			{ "__sub", [](const ScriptValue* args, std::size_t count) -> ScriptValue
				{
					ScriptBindingContext& context = ScriptEngine::GetBindingContext();
					glm::vec2* left = Receiver(context, args, count, "vec2:__sub");
					glm::vec2* right = count > 1 ? TryUnwrap<glm::vec2>(context, "vec2", args[1]) : nullptr;
					if (!right)
						throw std::logic_error("vec2 - vec2 expects another vec2");
					return MakeVec2(context, *left - *right);
				} },
			{ "__mul", [](const ScriptValue* args, std::size_t count) -> ScriptValue
				{
					ScriptBindingContext& context = ScriptEngine::GetBindingContext();
					glm::vec2* left = count > 0 ? TryUnwrap<glm::vec2>(context, "vec2", args[0]) : nullptr;
					glm::vec2* right = count > 1 ? TryUnwrap<glm::vec2>(context, "vec2", args[1]) : nullptr;
					double scalar = 0.0;
					if (left && count > 1 && args[1].AsNumber(&scalar))
						return MakeVec2(context, (*left) * static_cast<float>(scalar));
					if (!left && right && count > 0 && args[0].AsNumber(&scalar))
						return MakeVec2(context, static_cast<float>(scalar) * (*right));
					if (left && right)
						return MakeVec2(context, (*left) * (*right));
					throw std::logic_error("vec2 * ... expects (vec2, number), (number, vec2) or (vec2, vec2)");
				} },
			{ "__div", [](const ScriptValue* args, std::size_t count) -> ScriptValue
				{
					ScriptBindingContext& context = ScriptEngine::GetBindingContext();
					glm::vec2* left = count > 0 ? TryUnwrap<glm::vec2>(context, "vec2", args[0]) : nullptr;
					glm::vec2* right = count > 1 ? TryUnwrap<glm::vec2>(context, "vec2", args[1]) : nullptr;
					double scalar = 0.0;
					if (left && count > 1 && args[1].AsNumber(&scalar))
						return MakeVec2(context, (*left) / static_cast<float>(scalar));
					if (left && right)
						return MakeVec2(context, (*left) / (*right));
					throw std::logic_error("vec2 / ... expects (vec2, number) or (vec2, vec2)");
				} },
		};

		ScriptUserTypeDesc desc;
		desc.Name = "vec2";
		desc.UserdataSize = sizeof(glm::vec2);
		desc.Constructor = [](void* data, const ScriptValue* args, std::size_t count)
			{
				if (count == 0)
				{
					new (data) glm::vec2();
					return;
				}
				if (count == 1)
				{
					new (data) glm::vec2(RequireFloat(args[0], "vec2.new"));
					return;
				}
				if (count == 2)
				{
					new (data) glm::vec2(RequireFloat(args[0], "vec2.new"), RequireFloat(args[1], "vec2.new"));
					return;
				}
				throw std::logic_error("vec2.new expects 0, 1 or 2 arguments");
			};
		desc.Methods = methods;
		desc.MethodCount = sizeof(methods) / sizeof(methods[0]);
		desc.MetaMethods = metaMethods;
		desc.MetaMethodCount = sizeof(metaMethods) / sizeof(metaMethods[0]);

		std::string error;
		if (!bindings.RegisterUserType(desc, &error))
			throw std::logic_error("vec2 registration failed: " + error);
	}
}
