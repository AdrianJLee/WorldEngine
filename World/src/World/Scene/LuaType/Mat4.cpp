#include "wldpch.h"
#include "World/Scene/ScriptEngine.h"

namespace World
{
	static MathTypeRegistrar s_Mat4Registrar(
		{
			"mat4",
			{
				{"inverse", "fun():mat4", "Get the inverse of the matrix"},
				{"transpose", "fun():mat4", "Get the transpose of the matrix"},
				{"determinant", "fun():number", "Get the determinant of the matrix"}
			},
			[](sol::state& lua)
			{
				lua.new_usertype<glm::mat4>(
					"mat4", sol::constructors<glm::mat4(), glm::mat4(float)>(),
					sol::meta_function::multiplication, sol::overload(
						// 1. mat4 * mat4 (矩阵乘法，用于级联变换组合)
						[](const glm::mat4& a, const glm::mat4& b) -> glm::mat4 { return a * b; },
						// 2. mat4 * vec4 (变换一个 4D 向量/方向)
						[](const glm::mat4& a, const glm::vec4& b) -> glm::vec4 { return a * b; },
						// 3. mat4 * float (矩阵整体缩放)
						[](const glm::mat4& a, float b) -> glm::mat4 { return a * b; }
					),
					sol::meta_function::index, [](glm::mat4& mat, int index) -> glm::vec4&
					{
						if (index < 0 || index >= 4)
						{
							WLD_ERROR("Lua mat4 index out of bounds: " + std::to_string(index));
						}
						return mat[index];
					},
					sol::meta_function::new_index, [](glm::mat4& mat, int index, const glm::vec4& val) -> void
					{
						if (index >= 0 && index < 4)
						{
							mat[index] = val;
						}
					},
					"inverse", [](const glm::mat4& m) -> glm::mat4 { return glm::inverse(m); },
					"transpose", [](const glm::mat4& m) -> glm::mat4 { return glm::transpose(m); },
					"determinant",[](const glm::mat4& m) -> float { return glm::determinant(m); }
					);
				}
		});
}