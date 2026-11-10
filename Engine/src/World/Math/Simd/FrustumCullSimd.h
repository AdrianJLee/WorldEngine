#pragma once

#include "World/Math/Simd/SimdConfig.h"

#include <glm/glm.hpp>

#include <cstddef>
#include <cstdint>

namespace World::Math::Simd
{
	// ---------------------------------------------------------------------------
	// AABB × 视锥 的**批量**判定(4 对象/批)。
	//
	// 为什么这是 SIMD 的正确用法(标准 §6.5.0 的 **B 轴:操作数/分支消除**):
	//   逐对象标量版 `AabbInFrustum` 对 6 个平面各做一次"取正顶点 + 点积 + 比较",
	//   并且**每个平面都可能早退**(`if (...) return false`)。在"大多可见"的场景里
	//   这些早退几乎从不发生 ⇒ 6 次/对象的纯分支预测压力。
	//   把 **4 个对象放进 4 个 lane、按平面循环**,一次算完 4 个对象 ⇒ 指令数约 1/4,
	//   且**完全无分支**(最后按位与出一个掩码)。
	//
	// 为什么它可以默认开启而不违反 S4(确定性):
	//   lane = 对象,平面循环是**逐 lane 完全同序**的 ——
	//   `((nx*px) + (ny*py)) + (nz*pz)` 再 `+ w`,与 glm 的 `compute_dot<vec3>`
	//   (`tmp.x + tmp.y + tmp.z`)逐位一致 ⇒ 可见集合与标量版**完全相同**。
	//   这条由 `World.Simd` 的对照测试用大样本 + 边界样本钉住。
	//
	// 输入口径:SoA(每个对象的 min/max 连续排列)。调用方按 16 个一组做**小规模 gather**
	//   (16×2×12B = 384B,留在 L1),而不是给 AoS 加跨距参数 —— 让本层保持纯粹、可单测。
	// ---------------------------------------------------------------------------

	// 单个对象的标量参考实现(与 Renderer::AabbInFrustum 同一数学与同一结合顺序)。
	// 放在这里是为了让 SIMD 版与参考版**同文件邻近**,避免两份口径漂移。
	inline bool AabbInFrustumScalar(const glm::vec4* planes, const glm::vec3& min, const glm::vec3& max)
	{
		for (int p = 0; p < 6; ++p)
		{
			const glm::vec4& plane = planes[p];
			const glm::vec3 positive {
				plane.x >= 0.0f ? max.x : min.x,
				plane.y >= 0.0f ? max.y : min.y,
				plane.z >= 0.0f ? max.z : min.z };
			if (glm::dot(glm::vec3(plane), positive) + plane.w < 0.0f)
				return false;
		}
		return true;
	}

	// 批量判定:count 个对象(SoA),outVisible[i] = 1/0。
	inline void AabbInFrustumBatch(const glm::vec4* planes, const glm::vec3* mins, const glm::vec3* maxs,
		std::size_t count, std::uint8_t* outVisible)
	{
		std::size_t i = 0;
#if WLD_SIMD_SSE2
		for (; i + 4 <= count; i += 4)
		{
			// gather:每个对象一个 min(vec3)+ 一个 max(vec3)。x 进 lane 0..3。
			const __m128 mnx = _mm_set_ps(mins[i + 3].x, mins[i + 2].x, mins[i + 1].x, mins[i + 0].x);
			const __m128 mny = _mm_set_ps(mins[i + 3].y, mins[i + 2].y, mins[i + 1].y, mins[i + 0].y);
			const __m128 mnz = _mm_set_ps(mins[i + 3].z, mins[i + 2].z, mins[i + 1].z, mins[i + 0].z);
			const __m128 mxx = _mm_set_ps(maxs[i + 3].x, maxs[i + 2].x, maxs[i + 1].x, maxs[i + 0].x);
			const __m128 mxy = _mm_set_ps(maxs[i + 3].y, maxs[i + 2].y, maxs[i + 1].y, maxs[i + 0].y);
			const __m128 mxz = _mm_set_ps(maxs[i + 3].z, maxs[i + 2].z, maxs[i + 1].z, maxs[i + 0].z);

			__m128 visible = _mm_castsi128_ps(_mm_set1_epi32(-1));   // 全 1:先假设可见

			for (int p = 0; p < 6; ++p)
			{
				const glm::vec4& plane = planes[p];
				// 平面法线的符号在整个 batch 内是**统一**的 ⇒ 这里的选支可预测(每批每平面一次,
				// 不是每对象一次),且不参与浮点运算,故不影响逐位一致性。
				const __m128 px = (plane.x >= 0.0f) ? mxx : mnx;
				const __m128 py = (plane.y >= 0.0f) ? mxy : mny;
				const __m128 pz = (plane.z >= 0.0f) ? mxz : mnz;

				// 与 glm::compute_dot<vec3> 同序:((nx*px) + (ny*py)) + (nz*pz)。
				const __m128 d = _mm_add_ps(
					_mm_add_ps(_mm_mul_ps(_mm_set1_ps(plane.x), px), _mm_mul_ps(_mm_set1_ps(plane.y), py)),
					_mm_mul_ps(_mm_set1_ps(plane.z), pz));
				const __m128 dist = _mm_add_ps(d, _mm_set1_ps(plane.w));
				// 可见 ⇔ dist >= 0(标量版是 dist < 0 才拒绝)。
				visible = _mm_and_ps(visible, _mm_cmpge_ps(dist, _mm_setzero_ps()));
			}

			alignas(16) float lanes[4];
			_mm_store_ps(lanes, visible);
			for (int k = 0; k < 4; ++k)
				outVisible[i + static_cast<std::size_t>(k)] = (lanes[k] != 0.0f) ? std::uint8_t { 1 } : std::uint8_t { 0 };
		}
#endif
		// 标量回退(S3):非 x86-64 目标、以及每批不足 4 个的尾巴。
		for (; i < count; ++i)
			outVisible[i] = AabbInFrustumScalar(planes, mins[i], maxs[i]) ? std::uint8_t { 1 } : std::uint8_t { 0 };
	}

	// ---- 分块(tiling):渲染器要从 AoS 的绘制项里取样,而本层不依赖渲染器类型 ⇒
	//      用一个"取第 i 个对象 min/max"的回调把取样方式交给调用方。
	//      这样**分块 + 取样 + 判定**是同一份实现,测试可以直接覆盖它(而不是测一份副本)。

	// 每块的对象数:16 = 4 个 SSE 批;gather 缓冲 16×2×12B = 384B,留在 L1。
	inline constexpr std::size_t kAabbCullTileSize = 16;

	inline std::size_t AabbCullTileCount(std::size_t count) noexcept
	{
		return (count + kAabbCullTileSize - 1) / kAabbCullTileSize;
	}

	// 判定**一个**块(并行调度的粒度单位)。GetAabb(i, outMin, outMax) 负责取样。
	template <typename GetAabb>
	void AabbInFrustumTile(const glm::vec4* planes, std::size_t count, std::size_t tileIndex,
		GetAabb&& getAabb, std::uint8_t* outVisible)
	{
		const std::size_t begin = tileIndex * kAabbCullTileSize;
		if (begin >= count)
			return;
		const std::size_t end = (begin + kAabbCullTileSize < count) ? begin + kAabbCullTileSize : count;
		const std::size_t tileSize = end - begin;

		glm::vec3 tileMin[kAabbCullTileSize];
		glm::vec3 tileMax[kAabbCullTileSize];
		for (std::size_t k = 0; k < tileSize; ++k)
			getAabb(begin + k, tileMin[k], tileMax[k]);

		AabbInFrustumBatch(planes, tileMin, tileMax, tileSize, outVisible + begin);
	}

	// 判定全部对象(串行):结果与逐对象标量一致,但是批量 + 无分支。
	template <typename GetAabb>
	void AabbInFrustumTiled(const glm::vec4* planes, std::size_t count, GetAabb&& getAabb,
		std::uint8_t* outVisible)
	{
		const std::size_t tileCount = AabbCullTileCount(count);
		for (std::size_t tile = 0; tile < tileCount; ++tile)
			AabbInFrustumTile(planes, count, tile, getAabb, outVisible);
	}

}
