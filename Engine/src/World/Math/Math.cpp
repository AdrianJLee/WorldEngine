#include "wldpch.h"
#include "World/Math/Math.h"
#include "World/Math/Simd/Mat4xVec4Batch.h"

#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtx/matrix_decompose.hpp>
#include <glm/gtc/type_ptr.hpp>
namespace World::Math
{
	bool DecomposeTransform(const glm::mat4& transform, glm::vec3& outLocation, glm::vec3& outRotation, glm::vec3& outScale)
	{
		// From glm::decompose in glm/gtx/matrix_decompose.hpp
		using namespace glm;
		using T = float;

		mat4 LocalMatrix(transform);

		// normalize the matrix.
		if (epsilonEqual(LocalMatrix[3][3], static_cast<T>(0), epsilon<T>()))
			return false;

		// First, isolate perspective.  This is the messiest.
		if (
			epsilonNotEqual(LocalMatrix[0][3], static_cast<T>(0), epsilon<T>()) ||
			epsilonNotEqual(LocalMatrix[1][3], static_cast<T>(0), epsilon<T>()) ||
			epsilonNotEqual(LocalMatrix[2][3], static_cast<T>(0), epsilon<T>()))
		{
			// Clear the perspective partition
			LocalMatrix[0][3] = LocalMatrix[1][3] = LocalMatrix[2][3] = static_cast<T>(0);
			LocalMatrix[3][3] = static_cast<T>(1);
		}

		// Next take care of translation (easy).
		outLocation = vec3(LocalMatrix[3]);
		LocalMatrix[3] = vec4(0, 0, 0, LocalMatrix[3].w);

		vec3 Row[3], Pdum3;

		// Now get scale and shear.
		for (length_t i = 0; i < 3; ++i)
			for (length_t j = 0; j < 3; ++j)
				Row[i][j] = LocalMatrix[i][j];

		// Compute X scale factor and normalize first row.
		outScale.x = length(Row[0]);
		Row[0] = detail::scale(Row[0], static_cast<T>(1));
		outScale.y = length(Row[1]);
		Row[1] = detail::scale(Row[1], static_cast<T>(1));
		outScale.z = length(Row[2]);
		Row[2] = detail::scale(Row[2], static_cast<T>(1));

		// At this point, the matrix (in rows[]) is orthonormal.
		// Check for a coordinate system flip.  If the determinant is -1, then negate the matrix and the scaling factors.
		vec3 pdum3 = cross(Row[1], Row[2]);
		if (dot(Row[0], pdum3) < 0)
		{
			// 如果翻转了，我们将 X 轴缩放设为负，并修正旋转矩阵的方向
			outScale.x *= -1.0f;
			Row[0] *= -1.0f;
			Row[1] *= -1.0f;
			Row[2] *= -1.0f;
		}


		outRotation.y = asin(-Row[0][2]);
		if (cos(outRotation.y) != 0)
		{
			outRotation.x = atan2(Row[1][2], Row[2][2]);
			outRotation.z = atan2(Row[0][1], Row[0][0]);
		}
		else
		{
			outRotation.x = atan2(-Row[2][0], Row[1][1]);
			outRotation.z = 0;
		}
		return true;
	}

	// 手写 SIMD 实现已按标准 §6.5.2 S1 移入允许目录 `World/Math/Simd/`
	// (`Mat4xVec4Batch4`,含显式架构头与标量回退)。这里保留同名薄转发,调用方不变。
	void Math::MultiplyMat4ByVec4_SIMD_x4(const glm::mat4& transform, const glm::vec4* vertices,
		glm::vec3* outPositions)
	{
		Simd::MultiplyMat4ByVec4Batch4(transform, vertices, outPositions);
	}
}
