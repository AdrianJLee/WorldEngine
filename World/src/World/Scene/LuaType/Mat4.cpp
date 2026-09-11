#include "wldpch.h"
#include "World/Scene/ScriptEngine.h"

#include <stdexcept>

namespace World
{
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
		type.BindFunc = [](sol::state& lua)
		{
			lua.new_usertype<glm::mat4>(
				"mat4", sol::constructors<glm::mat4(), glm::mat4(float)>(),
				sol::meta_function::multiplication, sol::overload(
					[](const glm::mat4& a, const glm::mat4& b) -> glm::mat4 { return a * b; },
					[](const glm::mat4& a, const glm::vec4& b) -> glm::vec4 { return a * b; },
					[](const glm::mat4& a, float b) -> glm::mat4 { return a * b; }
				),
				sol::meta_function::index, [](glm::mat4& matrix, int index) -> glm::vec4&
				{
					if (index < 0 || index >= 4)
						throw std::out_of_range("mat4 column index " + std::to_string(index) + " is outside [0, 3]");
					return matrix[index];
				},
				sol::meta_function::new_index, [](glm::mat4& matrix, int index, const glm::vec4& value)
				{
					if (index < 0 || index >= 4)
						throw std::out_of_range("mat4 column index " + std::to_string(index) + " is outside [0, 3]");
					matrix[index] = value;
				},
				"inverse", [](const glm::mat4& matrix) -> glm::mat4 { return glm::inverse(matrix); },
				"transpose", [](const glm::mat4& matrix) -> glm::mat4 { return glm::transpose(matrix); },
				"determinant", [](const glm::mat4& matrix) -> float { return glm::determinant(matrix); }
			);
		};
		LuaReflectionRegistry::Register(type);
	}
}
