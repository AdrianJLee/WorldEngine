#include "wldpch.h"
#include "World/Physics/ConvexDecomposition.h"

#include <algorithm>
#include <cmath>

namespace World
{
	namespace
	{
		float SignedVolume(const glm::vec3& a, const glm::vec3& b, const glm::vec3& c, const glm::vec3& d)
		{
			return glm::dot(a - d, glm::cross(b - d, c - d)) / 6.0f;
		}
	}

	bool ConvexHull::ContainsPoint(const glm::vec3& pt) const
	{
		if (Indices.size() < 3 || Vertices.empty())
			return false;

		for (size_t i = 0; i < Indices.size(); i += 3)
		{
			const glm::vec3& a = Vertices[Indices[i]];
			const glm::vec3& b = Vertices[Indices[i + 1]];
			const glm::vec3& c = Vertices[Indices[i + 2]];

			const glm::vec3 normal = glm::cross(b - a, c - a);
			if (glm::dot(pt - a, normal) > 0.001f)
				return false;
		}
		return true;
	}

	bool ConvexHull::IsConvex() const
	{
		if (Indices.size() < 12 || Vertices.size() < 4)
			return false;

		// 凸包所有顶点应在每个面的内侧或共面
		for (size_t i = 0; i < Indices.size(); i += 3)
		{
			const glm::vec3& a = Vertices[Indices[i]];
			const glm::vec3& b = Vertices[Indices[i + 1]];
			const glm::vec3& c = Vertices[Indices[i + 2]];
			const glm::vec3 n = glm::normalize(glm::cross(b - a, c - a));

			for (const auto& v : Vertices)
			{
				if (glm::dot(v - a, n) > 0.01f)
					return false;
			}
		}
		return true;
	}

	bool ConvexDecomposer::ComputeHull(const std::vector<glm::vec3>& inPoints, ConvexHull& outHull)
	{
		outHull.Vertices.clear();
		outHull.Indices.clear();

		if (inPoints.size() < 4)
			return false;

		// 计算点云包围盒六个轴向极值点构建初始凸四面体
		glm::vec3 minP = inPoints[0], maxP = inPoints[0];
		for (const auto& p : inPoints)
		{
			minP = glm::min(minP, p);
			maxP = glm::max(maxP, p);
		}

		outHull.Center = (minP + maxP) * 0.5f;

		// 构建规则凸多面体 (以 8 个角落极值点拟合)
		outHull.Vertices = {
			{ minP.x, minP.y, minP.z },
			{ maxP.x, minP.y, minP.z },
			{ maxP.x, maxP.y, minP.z },
			{ minP.x, maxP.y, minP.z },
			{ minP.x, minP.y, maxP.z },
			{ maxP.x, minP.y, maxP.z },
			{ maxP.x, maxP.y, maxP.z },
			{ minP.x, maxP.y, maxP.z }
		};

		// 12 个三角形面
		outHull.Indices = {
			0, 2, 1,  0, 3, 2, // -Z
			4, 5, 6,  4, 6, 7, // +Z
			0, 1, 5,  0, 5, 4, // -Y
			3, 6, 2,  3, 7, 6, // +Y
			0, 4, 7,  0, 7, 3, // -X
			1, 2, 6,  1, 6, 5  // +X
		};

		const glm::vec3 size = maxP - minP;
		outHull.Volume = size.x * size.y * size.z;
		return true;
	}

	void ConvexDecomposer::Decompose(
		const std::vector<glm::vec3>& vertices,
		const std::vector<uint32_t>& indices,
		uint32_t maxHulls,
		float,
		std::vector<ConvexHull>& outHulls)
	{
		outHulls.clear();
		if (vertices.empty())
			return;

		if (maxHulls <= 1 || vertices.size() <= 8)
		{
			ConvexHull singleHull;
			if (ComputeHull(vertices, singleHull))
				outHulls.push_back(singleHull);
			return;
		}

		// 沿主轴切分网格为前后两个凸包
		glm::vec3 minP = vertices[0], maxP = vertices[0];
		for (const auto& p : vertices)
		{
			minP = glm::min(minP, p);
			maxP = glm::max(maxP, p);
		}

		const glm::vec3 center = (minP + maxP) * 0.5f;
		const glm::vec3 extents = maxP - minP;

		int splitAxis = 0;
		if (extents.y > extents.x && extents.y > extents.z) splitAxis = 1;
		else if (extents.z > extents.x && extents.z > extents.y) splitAxis = 2;

		std::vector<glm::vec3> setA, setB;
		for (const auto& p : vertices)
		{
			if (p[splitAxis] <= center[splitAxis])
				setA.push_back(p);
			else
				setB.push_back(p);
		}

		if (setA.size() >= 4)
		{
			ConvexHull hullA;
			if (ComputeHull(setA, hullA))
				outHulls.push_back(hullA);
		}
		if (setB.size() >= 4)
		{
			ConvexHull hullB;
			if (ComputeHull(setB, hullB))
				outHulls.push_back(hullB);
		}

		if (outHulls.empty())
		{
			ConvexHull fallback;
			if (ComputeHull(vertices, fallback))
				outHulls.push_back(fallback);
		}
	}
}
