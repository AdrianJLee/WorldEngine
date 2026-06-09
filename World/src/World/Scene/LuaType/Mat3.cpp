#include "wldpch.h"
#include "World/Scene/ScriptEngine.h"

namespace World
{
	static MathTypeRegistrar s_Mat3Registrar(
		{
			"mat3",
			{
				{"inverse", "fun():mat3", "Get the inverse of the matrix"},
				{"transpose", "fun():mat3", "Get the transpose of the matrix"},
				{"determinant", "fun():number", "Get the determinant of the matrix"}
			},
			[](sol::state& lua)
			{
				lua.new_usertype<glm::mat3>(
					"mat3", sol::constructors <	glm::mat3(),glm::mat3(float),glm::mat3(const glm::vec3&, const glm::vec3&, const glm::vec3&)>(),

					sol::meta_function::multiplication, sol::overload(
						// 1. mat3 * mat3 (矩阵乘法，用于级联变换组合)
						[](const glm::mat3& a, const glm::mat3& b) -> glm::mat3 { return a * b; },

						// 2. mat3 * vec3 (变换一个 3D 向量/方向)
						[](const glm::mat3& a, const glm::vec3& b) -> glm::vec3 { return a * b; },

						// 3. mat3 * vec2 (🚨 2D 绝杀：把 vec2 补齐为齐次坐标 vec3(v, 1.0) 进行 2D 空间变换)
						[](const glm::mat3& a, const glm::vec2& b) -> glm::vec2
						{
							glm::vec3 res = a * glm::vec3(b, 1.0f);
							return glm::vec2(res.x, res.y);
						},
						// 4. mat3 * float (矩阵整体缩放)
						[](const glm::mat3& a, float b) -> glm::mat3 { return a * b; }
					),

					sol::meta_function::index, [](glm::mat3& mat, int index) -> glm::vec3&
					{
						if (index < 0 || index >= 3)
						{
							WLD_ERROR("Lua mat3 index out of bounds: " + std::to_string(index));
						}
						return mat[index];
					},


					sol::meta_function::new_index, [](glm::mat3& mat, int index, const glm::vec3& val) -> void
					{
						if (index >= 0 && index < 3)
						{
							mat[index] = val;
						}
					},

					"inverse" , [](const glm::mat3& m) -> glm::mat3 { return glm::inverse(m); },
					"transpose" , [](const glm::mat3& m) -> glm::mat3 { return glm::transpose(m); },
					"determinant" , [](const glm::mat3& m) -> float { return glm::determinant(m); }
				);
			}
		});
}