#include "wldpch.h"
#include "World/Scene/ScriptEngine.h"
#include "LuaTypeHelpers.h"

#include <glm/glm.hpp>

namespace World
{
	using namespace LuaTypeDetail;

	namespace
	{
		glm::mat3* Receiver(ScriptBindingContext& bindings, const ScriptValue* args, std::size_t count, const char* method)
		{
			return RequireReceiver<glm::mat3>(bindings, "mat3", args, count, method);
		}

		ScriptValue MakeMat3(ScriptBindingContext& bindings, const glm::mat3& value)
		{
			return NewUserdataOf(bindings, "mat3", value);
		}
	}

	void RegisterBuiltinMat3LuaType()
	{
		LuaTypeReflection type;
		type.ClassName = "mat3";
		type.Properties = {
			{ "[integer]", "vec3", "Column access uses zero-based indices 0 through 2; other indices raise a Lua error." }
		};
		type.Methods = {
			{ "inverse", {}, "mat3", "Return the inverse of this matrix." },
			{ "transpose", {}, "mat3", "Return the transpose of this matrix." },
			{ "determinant", {}, "number", "Return the determinant of this matrix." }
		};
		type.Constructors = {
			{ "new", {}, "mat3", "Construct a matrix using GLM's default constructor.", false },
			{ "new", { { "diagonal", "number", "Diagonal value; other entries are zero." } }, "mat3", "Construct a diagonal matrix.", false },
			{ "new", { { "column0", "vec3", "First column." }, { "column1", "vec3", "Second column." }, { "column2", "vec3", "Third column." } }, "mat3", "Construct a matrix from three columns.", false }
		};
		type.Operators = {
			{ "mul", "mat3", "mat3" },
			{ "mul", "vec3", "vec3" },
			{ "mul", "vec2", "vec2" },
			{ "mul", "number", "mat3" }
		};
		type.BindFunc = [](ScriptBindingContext& bindings)
		{
			const ScriptMethodBinding methods[] = {
				{ "inverse", [](const ScriptValue* args, std::size_t count) -> ScriptValue
					{
						ScriptBindingContext& context = ScriptEngine::GetBindingContext();
						return MakeMat3(context, glm::inverse(*Receiver(context, args, count, "mat3:inverse")));
					} },
				{ "transpose", [](const ScriptValue* args, std::size_t count) -> ScriptValue
					{
						ScriptBindingContext& context = ScriptEngine::GetBindingContext();
						return MakeMat3(context, glm::transpose(*Receiver(context, args, count, "mat3:transpose")));
					} },
				{ "determinant", [](const ScriptValue* args, std::size_t count) -> ScriptValue
					{
						ScriptBindingContext& context = ScriptEngine::GetBindingContext();
						return ScriptValue::Number(glm::determinant(*Receiver(context, args, count, "mat3:determinant")));
					} },
			};

			const ScriptMetaMethodBinding metaMethods[] = {
				{ "__index", [](const ScriptValue* args, std::size_t count) -> ScriptValue
					{
						ScriptBindingContext& context = ScriptEngine::GetBindingContext();
						glm::mat3* self = Receiver(context, args, count, "mat3:__index");
						if (count < 2)
							return ScriptValue::Nil();
						double numeric = 0.0;
						if (args[1].AsNumber(&numeric))
						{
							const int index = RequireColumnIndex(args[1], 3, "mat3");
							return NewUserdataOf(context, "vec3", (*self)[index]);
						}
						std::string key;
						if (args[1].AsString(&key))
							return context.MethodsTable("mat3").GetField(key.c_str());
						return ScriptValue::Nil();
					} },
				{ "__newindex", [](const ScriptValue* args, std::size_t count) -> ScriptValue
					{
						ScriptBindingContext& context = ScriptEngine::GetBindingContext();
						glm::mat3* self = Receiver(context, args, count, "mat3:__newindex");
						if (count < 3)
							throw std::logic_error("mat3 column assignment needs a value");
						const int index = RequireColumnIndex(args[1], 3, "mat3");
						glm::vec3* value = TryUnwrap<glm::vec3>(context, "vec3", args[2]);
						if (!value)
							throw std::logic_error("mat3 column " + std::to_string(index) + " expects a vec3");
						(*self)[index] = *value;
						return ScriptValue::Nil();
					} },
				{ "__mul", [](const ScriptValue* args, std::size_t count) -> ScriptValue
					{
						ScriptBindingContext& context = ScriptEngine::GetBindingContext();
						glm::mat3* left = count > 0 ? TryUnwrap<glm::mat3>(context, "mat3", args[0]) : nullptr;
						if (!left || count < 2)
							throw std::logic_error("mat3 * ... expects a mat3 on the left");
						if (glm::mat3* other = TryUnwrap<glm::mat3>(context, "mat3", args[1]))
							return MakeMat3(context, (*left) * (*other));
						if (glm::vec3* vector = TryUnwrap<glm::vec3>(context, "vec3", args[1]))
							return NewUserdataOf(context, "vec3", (*left) * (*vector));
						if (glm::vec2* vector2 = TryUnwrap<glm::vec2>(context, "vec2", args[1]))
							return NewUserdataOf(context, "vec2", glm::vec2((*left) * glm::vec3(*vector2, 1.0f)));
						double scalar = 0.0;
						if (args[1].AsNumber(&scalar))
							return MakeMat3(context, (*left) * static_cast<float>(scalar));
						throw std::logic_error("mat3 * ... expects mat3, vec3, vec2 or number");
					} },
			};

			ScriptUserTypeDesc desc;
			desc.Name = "mat3";
			desc.UserdataSize = sizeof(glm::mat3);
			desc.Constructor = [&bindings](void* data, const ScriptValue* args, std::size_t count)
				{
					if (count == 0)
					{
						new (data) glm::mat3();
						return;
					}
					if (count == 1)
					{
						new (data) glm::mat3(RequireFloat(args[0], "mat3.new"));
						return;
					}
					if (count == 3)
					{
						glm::vec3 columns[3];
						for (std::size_t index = 0; index < 3; ++index)
						{
							glm::vec3* column = TryUnwrap<glm::vec3>(bindings, "vec3", args[index]);
							if (!column)
								throw std::logic_error("mat3.new expects vec3 columns");
							columns[index] = *column;
						}
						new (data) glm::mat3(columns[0], columns[1], columns[2]);
						return;
					}
					throw std::logic_error("mat3.new expects 0, 1 or 3 arguments");
				};
			desc.Methods = methods;
			desc.MethodCount = sizeof(methods) / sizeof(methods[0]);
			desc.MetaMethods = metaMethods;
			desc.MetaMethodCount = sizeof(metaMethods) / sizeof(metaMethods[0]);

			std::string error;
			if (!bindings.RegisterUserType(desc, &error))
				throw std::logic_error("mat3 registration failed: " + error);
		};
		LuaReflectionRegistry::Register(type);
	}
}
