#include "wldpch.h"
#include "World/Scene/ScriptEngine.h"
#include "LuaTypeHelpers.h"

#include <glm/glm.hpp>

namespace World
{
	using namespace LuaTypeDetail;

	namespace
	{
		glm::mat4* Receiver(ScriptBindingContext& bindings, const ScriptValue* args, std::size_t count, const char* method)
		{
			return RequireReceiver<glm::mat4>(bindings, "mat4", args, count, method);
		}

		ScriptValue MakeMat4(ScriptBindingContext& bindings, const glm::mat4& value)
		{
			return NewUserdataOf(bindings, "mat4", value);
		}
	}

	void RegisterBuiltinMat4LuaType()
	{
		LuaTypeReflection type;
		type.ClassName = "mat4";
		type.Properties = {
			{ "[integer]", "vec4", "Column access uses zero-based indices 0 through 3; other indices raise a Lua error." }
		};
		type.Methods = {
			{ "inverse", {}, "mat4", "Return the inverse of this matrix." },
			{ "transpose", {}, "mat4", "Return the transpose of this matrix." },
			{ "determinant", {}, "number", "Return the determinant of this matrix." }
		};
		type.Constructors = {
			{ "new", {}, "mat4", "Construct a matrix using GLM's default constructor.", false },
			{ "new", { { "diagonal", "number", "Diagonal value; other entries are zero." } }, "mat4", "Construct a diagonal matrix.", false }
		};
		type.Operators = {
			{ "mul", "mat4", "mat4" },
			{ "mul", "vec4", "vec4" },
			{ "mul", "number", "mat4" }
		};
		type.BindFunc = [](ScriptBindingContext& bindings)
		{
			const ScriptMethodBinding methods[] = {
				{ "inverse", [](const ScriptValue* args, std::size_t count) -> ScriptValue
					{
						ScriptBindingContext& context = ScriptEngine::GetBindingContext();
						return MakeMat4(context, glm::inverse(*Receiver(context, args, count, "mat4:inverse")));
					} },
				{ "transpose", [](const ScriptValue* args, std::size_t count) -> ScriptValue
					{
						ScriptBindingContext& context = ScriptEngine::GetBindingContext();
						return MakeMat4(context, glm::transpose(*Receiver(context, args, count, "mat4:transpose")));
					} },
				{ "determinant", [](const ScriptValue* args, std::size_t count) -> ScriptValue
					{
						ScriptBindingContext& context = ScriptEngine::GetBindingContext();
						return ScriptValue::Number(glm::determinant(*Receiver(context, args, count, "mat4:determinant")));
					} },
			};

			const ScriptMetaMethodBinding metaMethods[] = {
				{ "__index", [](const ScriptValue* args, std::size_t count) -> ScriptValue
					{
						ScriptBindingContext& context = ScriptEngine::GetBindingContext();
						glm::mat4* self = Receiver(context, args, count, "mat4:__index");
						if (count < 2)
							return ScriptValue::Nil();
						double numeric = 0.0;
						if (args[1].AsNumber(&numeric))
						{
							const int index = RequireColumnIndex(args[1], 4, "mat4");
							return NewUserdataOf(context, "vec4", (*self)[index]);
						}
						std::string key;
						if (args[1].AsString(&key))
							return context.MethodsTable("mat4").GetField(key.c_str());
						return ScriptValue::Nil();
					} },
				{ "__newindex", [](const ScriptValue* args, std::size_t count) -> ScriptValue
					{
						ScriptBindingContext& context = ScriptEngine::GetBindingContext();
						glm::mat4* self = Receiver(context, args, count, "mat4:__newindex");
						if (count < 3)
							throw std::logic_error("mat4 column assignment needs a value");
						const int index = RequireColumnIndex(args[1], 4, "mat4");
						glm::vec4* value = TryUnwrap<glm::vec4>(context, "vec4", args[2]);
						if (!value)
							throw std::logic_error("mat4 column " + std::to_string(index) + " expects a vec4");
						(*self)[index] = *value;
						return ScriptValue::Nil();
					} },
				{ "__mul", [](const ScriptValue* args, std::size_t count) -> ScriptValue
					{
						ScriptBindingContext& context = ScriptEngine::GetBindingContext();
						glm::mat4* left = count > 0 ? TryUnwrap<glm::mat4>(context, "mat4", args[0]) : nullptr;
						if (!left || count < 2)
							throw std::logic_error("mat4 * ... expects a mat4 on the left");
						if (glm::mat4* other = TryUnwrap<glm::mat4>(context, "mat4", args[1]))
							return MakeMat4(context, (*left) * (*other));
						if (glm::vec4* vector = TryUnwrap<glm::vec4>(context, "vec4", args[1]))
							return NewUserdataOf(context, "vec4", (*left) * (*vector));
						double scalar = 0.0;
						if (args[1].AsNumber(&scalar))
							return MakeMat4(context, (*left) * static_cast<float>(scalar));
						throw std::logic_error("mat4 * ... expects mat4, vec4 or number");
					} },
			};

			ScriptUserTypeDesc desc;
			desc.Name = "mat4";
			desc.UserdataSize = sizeof(glm::mat4);
			desc.Constructor = [](void* data, const ScriptValue* args, std::size_t count)
				{
					if (count == 0)
					{
						new (data) glm::mat4();
						return;
					}
					if (count == 1)
					{
						new (data) glm::mat4(RequireFloat(args[0], "mat4.new"));
						return;
					}
					throw std::logic_error("mat4.new expects 0 or 1 arguments");
				};
			desc.Methods = methods;
			desc.MethodCount = sizeof(methods) / sizeof(methods[0]);
			desc.MetaMethods = metaMethods;
			desc.MetaMethodCount = sizeof(metaMethods) / sizeof(metaMethods[0]);

			std::string error;
			if (!bindings.RegisterUserType(desc, &error))
				throw std::logic_error("mat4 registration failed: " + error);
		};
		LuaReflectionRegistry::Register(type);
	}
}
