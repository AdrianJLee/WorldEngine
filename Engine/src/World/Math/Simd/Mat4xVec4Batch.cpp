#include "wldpch.h"
#include "World/Math/Simd/Mat4xVec4Batch.h"

#include "World/Math/Simd/SimdConfig.h"   // S2:显式拿架构头,不依赖传递包含

namespace World::Math::Simd
{
	namespace
	{
		// 标量回退(S3):非 x86-64 目标走这里。与 SIMD 路径**同序**(col0*x + col1*y + col2*z + col3*w),
		// 因此两条路给出同一结果 —— 这也是"可以不带门控上线"的依据。
		void MultiplyMat4ByVec4Batch4_Scalar(const glm::mat4& transform, const glm::vec4* vertices,
			glm::vec3* outPositions)
		{
			for (uint32_t i = 0; i < 4; ++i)
				outPositions[i] = glm::vec3(transform * vertices[i]);
		}
	}

	void MultiplyMat4ByVec4Batch4(const glm::mat4& transform, const glm::vec4* vertices,
		glm::vec3* outPositions)
	{
#if WLD_SIMD_SSE2
		// 矩阵的 4 个列:只加载一次,供 4 个顶点复用(这是本函数相对 glm 循环的主要收益来源)。
		const float* m = reinterpret_cast<const float*>(&transform[0][0]);
		const __m128 col0 = _mm_loadu_ps(m + 0);
		const __m128 col1 = _mm_loadu_ps(m + 4);
		const __m128 col2 = _mm_loadu_ps(m + 8);
		const __m128 col3 = _mm_loadu_ps(m + 12);

		for (uint32_t i = 0; i < 4; ++i)
		{
			const __m128 v = _mm_loadu_ps(reinterpret_cast<const float*>(&vertices[i]));
			const __m128 r = _mm_add_ps(
				_mm_add_ps(_mm_mul_ps(col0, _mm_shuffle_ps(v, v, _MM_SHUFFLE(0, 0, 0, 0))),
					_mm_mul_ps(col1, _mm_shuffle_ps(v, v, _MM_SHUFFLE(1, 1, 1, 1)))),
				_mm_add_ps(_mm_mul_ps(col2, _mm_shuffle_ps(v, v, _MM_SHUFFLE(2, 2, 2, 2))),
					_mm_mul_ps(col3, _mm_shuffle_ps(v, v, _MM_SHUFFLE(3, 3, 3, 3)))));
			// 只写 xyz:xyz 用 12B 存储,避免 _mm_storeu_ps 到临时数组再逐元素拷贝
			// (那会带来 store-to-load forwarding 惩罚,见标准 §6.5.4 反模式)。
			alignas(16) float tmp[4];
			_mm_store_ps(tmp, r);
			outPositions[i] = glm::vec3(tmp[0], tmp[1], tmp[2]);
		}
#else
		MultiplyMat4ByVec4Batch4_Scalar(transform, vertices, outPositions);
#endif
	}
}
