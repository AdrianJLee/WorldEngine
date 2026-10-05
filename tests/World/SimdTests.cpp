// SIMD 使用规则的验证口径(标准 docs/dev/performance-and-data-layout.md §6.5)。
//
// 本套件证明三件事,缺一不可:
//   1. **逐位一致**:批量 SIMD 判定(lane = 对象)与逐对象标量参考**结果完全相同** ——
//      这是"可以默认开启、不需要门控"的前提(S4)。用大样本 + 边界样本 + 各种 count
//      (含非 4 倍数、1、0)覆盖,并按字节比较掩码。
//   2. **与权威实现同口径**:Math/Simd 里的标量参考与渲染器实际使用的
//      `World::AabbInFrustum`(FrustumCull.h)逐样本一致 —— 防止两份口径漂移。
//   3. **有判别力**:故意把一个平面推远/靠近,可见集合必须真的改变 ——
//      否则上面的"一致"可能只是"两边都全可见"的假通过。
#include "World/Math/Simd/FrustumCullSimd.h"
#include "World/Renderer/FrustumCull.h"

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <cstdio>
#include <cstring>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{
	using namespace World;

	void Check(bool condition, const char* expression, int line)
	{
		if (!condition)
			throw std::runtime_error(std::string("line ") + std::to_string(line) + ": " + expression);
	}
#define CHECK(expression) Check(static_cast<bool>(expression), #expression, __LINE__)

	FrustumPlanes MakeCameraFrustum()
	{
		const glm::mat4 view = glm::lookAt(glm::vec3(0.0f, 0.0f, 5.0f), glm::vec3(0.0f), glm::vec3(0.0f, 1.0f, 0.0f));
		const glm::mat4 proj = glm::perspective(glm::radians(60.0f), 16.0f / 9.0f, 0.1f, 100.0f);
		return ExtractFrustumPlanes(proj * view);
	}

	// 随机 AABB:一半落在视锥内/附近(有判别力),一半远在视锥外。
	void MakeRandomAabbs(std::mt19937& rng, std::size_t count,
		std::vector<glm::vec3>& mins, std::vector<glm::vec3>& maxs)
	{
		std::uniform_real_distribution<float> near(-6.0f, 6.0f);
		std::uniform_real_distribution<float> far(-400.0f, 400.0f);
		std::uniform_real_distribution<float> size(0.0f, 3.0f);
		mins.resize(count);
		maxs.resize(count);
		for (std::size_t i = 0; i < count; ++i)
		{
			const bool insideish = (i % 2) == 0;
			const float x = insideish ? near(rng) : far(rng);
			const float y = insideish ? near(rng) : far(rng);
			const float z = insideish ? near(rng) : far(rng);
			const glm::vec3 extent(size(rng));
			mins[i] = glm::vec3(x, y, z);
			maxs[i] = mins[i] + extent;
		}
	}

	// 边界样本:AABB 的顶点**恰好落在**视锥平面上(数学上 dist == 0,最容易被
	// "换个结合顺序" 的实现判反)。这一类是逐位一致性最关键的测试。
	void MakeBoundaryAabbs(const FrustumPlanes& frustum, std::vector<glm::vec3>& mins,
		std::vector<glm::vec3>& maxs)
	{
		mins.clear();
		maxs.clear();
		for (int p = 0; p < 6; ++p)
		{
			const glm::vec4& plane = frustum.Planes[p];
			// 平面上取一点(把法线方向的分量解出来),再围绕它做退化成"点/薄片/方块"的盒子。
			const float inv = (plane.z != 0.0f) ? (1.0f / plane.z) : 0.0f;
			const float z = -plane.w * inv;
			const glm::vec3 onPlane(0.0f, 0.0f, z);
			const float eps[3] = { 0.0f, 1e-6f, 0.5f };
			for (float e : eps)
			{
				mins.push_back(onPlane - glm::vec3(e));
				maxs.push_back(onPlane + glm::vec3(e));
				mins.push_back(onPlane);
				maxs.push_back(onPlane);                       // 退化点盒
				mins.push_back(onPlane - glm::vec3(e, 0.0f, 0.0f));
				maxs.push_back(onPlane + glm::vec3(0.0f, e, 0.0f)); // 薄片
			}
		}
	}

	// 用批量 SIMD 跑一遍,返回掩码。
	std::vector<std::uint8_t> RunBatch(const FrustumPlanes& frustum,
		const std::vector<glm::vec3>& mins, const std::vector<glm::vec3>& maxs)
	{
		std::vector<std::uint8_t> visible(mins.size(), 0);
		if (!mins.empty())
			Math::Simd::AabbInFrustumBatch(frustum.Planes, mins.data(), maxs.data(), mins.size(), visible.data());
		return visible;
	}

	// 与批量版逐样本比对:标量参考(Math/Simd)与渲染器权威实现各比一次。
	void CheckBatchMatchesScalar(const char* label, const FrustumPlanes& frustum,
		const std::vector<glm::vec3>& mins, const std::vector<glm::vec3>& maxs)
	{
		const std::vector<std::uint8_t> batch = RunBatch(frustum, mins, maxs);

		std::size_t visibleCount = 0;
		for (std::size_t i = 0; i < mins.size(); ++i)
		{
			const bool ref = Math::Simd::AabbInFrustumScalar(frustum.Planes, mins[i], maxs[i]);
			const bool authoritative = AabbInFrustum(frustum, mins[i], maxs[i]);
			CHECK(authoritative == ref);                       // 口径不漂移
			CHECK(batch[i] == (ref ? 1u : 0u));                // 逐位(逐字节)一致
			visibleCount += batch[i];
		}
		std::printf("[info] %s: %zu/%zu visible (%s backend)\n",
			label, visibleCount, mins.size(), Math::Simd::BackendName());
		CHECK((visibleCount > 0) && (visibleCount < mins.size()));  // 两边都非空/非满 ⇒ 有判别力
	}
}

int main()
{
	try
	{
		const FrustumPlanes frustum = MakeCameraFrustum();

		// 1. 大样本随机。
		{
			std::mt19937 rng(20261005u);
			std::vector<glm::vec3> mins, maxs;
			MakeRandomAabbs(rng, 4096, mins, maxs);
			CheckBatchMatchesScalar("random 4096", frustum, mins, maxs);
		}

		// 2. 边界样本(恰好落在平面上)。
		{
			std::vector<glm::vec3> mins, maxs;
			MakeBoundaryAabbs(frustum, mins, maxs);
			CheckBatchMatchesScalar("boundary", frustum, mins, maxs);
		}

		// 3. 各种 count:0 / 1 / 2 / 3 / 4 / 5 / 7 / 8 / 63(覆盖 SIMD 批的尾巴与标量回退)。
		{
			std::mt19937 rng(7u);
			std::vector<glm::vec3> allMin, allMax;
			MakeRandomAabbs(rng, 64, allMin, allMax);
			for (std::size_t count : { std::size_t { 0 }, std::size_t { 1 }, std::size_t { 2 }, std::size_t { 3 },
				std::size_t { 4 }, std::size_t { 5 }, std::size_t { 7 }, std::size_t { 8 }, std::size_t { 63 } })
			{
				std::vector<glm::vec3> mins(allMin.begin(), allMin.begin() + static_cast<std::ptrdiff_t>(count));
				std::vector<glm::vec3> maxs(allMax.begin(), allMax.begin() + static_cast<std::ptrdiff_t>(count));
				const std::vector<std::uint8_t> batch = RunBatch(frustum, mins, maxs);
				for (std::size_t i = 0; i < count; ++i)
					CHECK(batch[i] == (Math::Simd::AabbInFrustumScalar(frustum.Planes, mins[i], maxs[i]) ? 1u : 0u));
			}
			std::printf("[info] count sweep (0,1,2,3,4,5,7,8,63): batch == scalar\n");
		}

		// 4. 判别力:把一个平面翻转方向,可见集合必须真的改变(否证"两边都全可见"式假通过)。
		{
			FrustumPlanes mutated = frustum;
			mutated.Planes[0].w += 1000.0f;   // 把左平面推到极远 ⇒ 几乎全部被剔除
			std::mt19937 rng(99u);
			std::vector<glm::vec3> mins, maxs;
			MakeRandomAabbs(rng, 512, mins, maxs);
			const std::vector<std::uint8_t> before = RunBatch(frustum, mins, maxs);
			const std::vector<std::uint8_t> after = RunBatch(mutated, mins, maxs);
			CHECK(std::memcmp(before.data(), after.data(), before.size()) != 0);
			// 且两边各自仍与自己一致的标量参考相符(变异后也要逐位一致)。
			for (std::size_t i = 0; i < mins.size(); ++i)
				CHECK(after[i] == (Math::Simd::AabbInFrustumScalar(mutated.Planes, mins[i], maxs[i]) ? 1u : 0u));
			std::printf("[info] mutated plane changes the visible set (test has discriminating power)\n");
		}

		std::puts("World.Simd: all checks passed");
		return 0;
	}
	catch (const std::exception& e)
	{
		std::fprintf(stderr, "World.Simd: FAILED - %s\n", e.what());
		return 1;
	}
}
