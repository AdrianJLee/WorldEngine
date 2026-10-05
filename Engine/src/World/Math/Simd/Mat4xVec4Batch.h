#pragma once

#include <glm/glm.hpp>

#include <cstdint>

namespace World::Math::Simd
{
	// ---------------------------------------------------------------------------
	// mat4 × 4 个 vec4 的批量变换(每个 quad 一次调用:4 个角点)。
	//
	// 归属说明(标准 §6.5.2 S1):这是全仓**唯一**保留的手写 intrinsics,放在允许目录内。
	// 为什么保留 —— **实测裁定**,不是"看起来更快"(2026-10-05,Release,3 次复现,ns/quad):
	//     手写 SSE        3.5 – 3.8
	//     glm 朴素循环     5.8 – 5.9   (`out[c] = transforms[i] * corners[c]`)
	//     glm 提升列写法    7.5 – 7.6   (显式取 m[0..3] 再用 vec4 运算)
	//   ⇒ 手写版比**最好的可移植写法**快约 2.1×。按 §6.5.1 P0("先用库表达")这条已被排除,
	//   所以此处保留 intrinsics 并登记(见 docs/dev/performance-and-data-layout.md §6.6)。
	//   真正的原因是把**矩阵的 4 个列提升到顶点循环之外**再复用 4 次 —— 这条结构性收益
	//   用 glm 没能复现出同等代码质量。
	//
	// 契约:
	//   * 语义 = 逐顶点 `transform * vertices[i]`,取 xyz(与 glm 版观感一致);
	//   * 不改变任何调用方的确定性口径(同一输入同一输出);
	//   * 非 x86-64 目标走同文件的标量回退(S3),两条路都被 `World.Simd` 的对照测试覆盖。
	void MultiplyMat4ByVec4Batch4(const glm::mat4& transform, const glm::vec4* vertices,
		glm::vec3* outPositions);
}
