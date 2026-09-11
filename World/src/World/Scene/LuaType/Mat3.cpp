#include "wldpch.h"
#include "World/Scene/ScriptEngine.h"

#include <stdexcept>

namespace World
{
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
		type.BindFunc = [](sol::state& lua)
		{
			lua.new_usertype<glm::mat3>(
				"mat3", sol::constructors<glm::mat3(), glm::mat3(float), glm::mat3(const glm::vec3&, const glm::vec3&, const glm::vec3&)>(),
				sol::meta_function::multiplication, sol::overload(
					[](const glm::mat3& a, const glm::mat3& b) -> glm::mat3 { return a * b; },
					[](const glm::mat3& a, const glm::vec3& b) -> glm::vec3 { return a * b; },
					[](const glm::mat3& a, const glm::vec2& b) -> glm::vec2 { return glm::vec2(a * glm::vec3(b, 1.0f)); },
					[](const glm::mat3& a, float b) -> glm::mat3 { return a * b; }
				),
				sol::meta_function::index, [](glm::mat3& matrix, int index) -> glm::vec3&
				{
					if (index < 0 || index >= 3)
						throw std::out_of_range("mat3 column index " + std::to_string(index) + " is outside [0, 2]");
					return matrix[index];
				},
				sol::meta_function::new_index, [](glm::mat3& matrix, int index, const glm::vec3& value)
				{
					if (index < 0 || index >= 3)
						throw std::out_of_range("mat3 column index " + std::to_string(index) + " is outside [0, 2]");
					matrix[index] = value;
				},
				"inverse", [](const glm::mat3& matrix) -> glm::mat3 { return glm::inverse(matrix); },
				"transpose", [](const glm::mat3& matrix) -> glm::mat3 { return glm::transpose(matrix); },
				"determinant", [](const glm::mat3& matrix) -> float { return glm::determinant(matrix); }
			);
		};
		LuaReflectionRegistry::Register(type);
	}
}
