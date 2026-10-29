#include "wldpch.h"
#include "World/Physics/ConvexDecomposition.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <vector>

#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtx/norm.hpp>

namespace World
{
	namespace
	{
		float SignedVolume(const glm::vec3& a, const glm::vec3& b, const glm::vec3& c, const glm::vec3& d)
		{
			return glm::dot(a - d, glm::cross(b - d, c - d)) / 6.0f;
		}

		struct QuickFace
		{
			uint32_t v[3];
			glm::vec3 normal;
			float dist = 0.0f;
			std::vector<uint32_t> outsidePoints;
			float maxDist = 0.0f;
			uint32_t furthestPoint = 0;
			bool active = true;
		};

		struct DirectedEdge
		{
			uint32_t a = 0;
			uint32_t b = 0;

			bool operator==(const DirectedEdge& other) const
			{
				return a == other.a && b == other.b;
			}
		};
	}

	bool ConvexHull::ContainsPoint(const glm::vec3& pt) const
	{
		if (Indices.size() < 12 || Vertices.size() < 4)
			return false;

		for (size_t i = 0; i + 2 < Indices.size(); i += 3)
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
		for (size_t i = 0; i + 2 < Indices.size(); i += 3)
		{
			const glm::vec3& a = Vertices[Indices[i]];
			const glm::vec3& b = Vertices[Indices[i + 1]];
			const glm::vec3& c = Vertices[Indices[i + 2]];
			const glm::vec3 cross = glm::cross(b - a, c - a);
			const float len = glm::length(cross);
			if (len < 1e-6f)
				continue;
			const glm::vec3 n = cross / len;

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
		outHull.Center = glm::vec3(0.0f);
		outHull.Volume = 0.0f;

		if (inPoints.size() < 4)
			return false;

		// 1. 去重近距离点
		std::vector<glm::vec3> uniquePoints;
		uniquePoints.reserve(inPoints.size());
		for (const auto& p : inPoints)
		{
			bool duplicate = false;
			for (const auto& u : uniquePoints)
			{
				if (glm::distance2(p, u) < 1e-8f)
				{
					duplicate = true;
					break;
				}
			}
			if (!duplicate)
				uniquePoints.push_back(p);
		}

		if (uniquePoints.size() < 4)
			return false;

		// 2. 寻找 6 个轴向极值点构建初始非共面四面体
		size_t minX = 0, maxX = 0, minY = 0, maxY = 0, minZ = 0, maxZ = 0;
		for (size_t i = 1; i < uniquePoints.size(); ++i)
		{
			const auto& p = uniquePoints[i];
			if (p.x < uniquePoints[minX].x) minX = i;
			if (p.x > uniquePoints[maxX].x) maxX = i;
			if (p.y < uniquePoints[minY].y) minY = i;
			if (p.y > uniquePoints[maxY].y) maxY = i;
			if (p.z < uniquePoints[minZ].z) minZ = i;
			if (p.z > uniquePoints[maxZ].z) maxZ = i;
		}

		const size_t extremeIndices[6] = { minX, maxX, minY, maxY, minZ, maxZ };
		size_t v0 = minX, v1 = maxX;
		float maxDistSq = glm::distance2(uniquePoints[v0], uniquePoints[v1]);

		for (int i = 0; i < 6; ++i)
		{
			for (int j = i + 1; j < 6; ++j)
			{
				float d2 = glm::distance2(uniquePoints[extremeIndices[i]], uniquePoints[extremeIndices[j]]);
				if (d2 > maxDistSq)
				{
					maxDistSq = d2;
					v0 = extremeIndices[i];
					v1 = extremeIndices[j];
				}
			}
		}

		if (maxDistSq < 1e-6f)
			return false;

		// 寻找距离线段 v0-v1 最远的点 v2
		const glm::vec3 segDir = glm::normalize(uniquePoints[v1] - uniquePoints[v0]);
		float maxDistToLine = 0.0f;
		size_t v2 = 0;
		for (size_t i = 0; i < uniquePoints.size(); ++i)
		{
			const glm::vec3 diff = uniquePoints[i] - uniquePoints[v0];
			const glm::vec3 perp = diff - glm::dot(diff, segDir) * segDir;
			const float d2 = glm::length2(perp);
			if (d2 > maxDistToLine)
			{
				maxDistToLine = d2;
				v2 = i;
			}
		}

		if (maxDistToLine < 1e-6f)
			return false;

		// 寻找距离平面 v0-v1-v2 最远的点 v3
		glm::vec3 triNorm = glm::cross(uniquePoints[v1] - uniquePoints[v0], uniquePoints[v2] - uniquePoints[v0]);
		const float triNormLen = glm::length(triNorm);
		if (triNormLen < 1e-6f)
			return false;
		triNorm /= triNormLen;

		float maxDistToPlane = 0.0f;
		size_t v3 = 0;
		for (size_t i = 0; i < uniquePoints.size(); ++i)
		{
			const float dist = std::abs(glm::dot(uniquePoints[i] - uniquePoints[v0], triNorm));
			if (dist > maxDistToPlane)
			{
				maxDistToPlane = dist;
				v3 = i;
			}
		}

		if (maxDistToPlane < 1e-5f)
			return false;

		// 初始四面体质心
		const glm::vec3 centroid = (uniquePoints[v0] + uniquePoints[v1] + uniquePoints[v2] + uniquePoints[v3]) * 0.25f;

		// 构建初始 4 个面，统一法线指向四面体外部
		std::vector<QuickFace> faces;
		faces.reserve(32);

		auto addFace = [&](uint32_t a, uint32_t b, uint32_t c)
		{
			glm::vec3 n = glm::cross(uniquePoints[b] - uniquePoints[a], uniquePoints[c] - uniquePoints[a]);
			if (glm::dot(uniquePoints[a] - centroid, n) < 0.0f)
			{
				std::swap(b, c);
				n = -n;
			}
			const float nLen = glm::length(n);
			if (nLen < 1e-7f)
				return;
			n /= nLen;

			QuickFace f;
			f.v[0] = a;
			f.v[1] = b;
			f.v[2] = c;
			f.normal = n;
			f.dist = -glm::dot(n, uniquePoints[a]);
			f.active = true;
			faces.push_back(std::move(f));
		};

		addFace(static_cast<uint32_t>(v0), static_cast<uint32_t>(v1), static_cast<uint32_t>(v2));
		addFace(static_cast<uint32_t>(v0), static_cast<uint32_t>(v2), static_cast<uint32_t>(v3));
		addFace(static_cast<uint32_t>(v0), static_cast<uint32_t>(v3), static_cast<uint32_t>(v1));
		addFace(static_cast<uint32_t>(v1), static_cast<uint32_t>(v3), static_cast<uint32_t>(v2));

		if (faces.size() < 4)
			return false;

		// 3. 将其余点分配到各初始面的外点集 (Outside Sets)
		for (size_t i = 0; i < uniquePoints.size(); ++i)
		{
			if (i == v0 || i == v1 || i == v2 || i == v3)
				continue;

			const auto& p = uniquePoints[i];
			float bestDist = 1e-4f;
			int bestFace = -1;

			for (size_t f = 0; f < faces.size(); ++f)
			{
				const float d = glm::dot(faces[f].normal, p) + faces[f].dist;
				if (d > bestDist)
				{
					bestDist = d;
					bestFace = static_cast<int>(f);
				}
			}

			if (bestFace >= 0)
			{
				faces[bestFace].outsidePoints.push_back(static_cast<uint32_t>(i));
				if (bestDist > faces[bestFace].maxDist)
				{
					faces[bestFace].maxDist = bestDist;
					faces[bestFace].furthestPoint = static_cast<uint32_t>(i);
				}
			}
		}

		// 4. QuickHull 迭代：寻找拥有最远外点的面进行地平线边扩展
		while (true)
		{
			int maxFaceIdx = -1;
			float globalMaxDist = 1e-4f;
			for (size_t i = 0; i < faces.size(); ++i)
			{
				if (faces[i].active && !faces[i].outsidePoints.empty() && faces[i].maxDist > globalMaxDist)
				{
					globalMaxDist = faces[i].maxDist;
					maxFaceIdx = static_cast<int>(i);
				}
			}

			if (maxFaceIdx < 0)
				break; // 凸包闭合完成

			const uint32_t eyePtIdx = faces[maxFaceIdx].furthestPoint;
			const glm::vec3 eyePt = uniquePoints[eyePtIdx];

			// 找出对 eyePt 均可见的全部面
			std::vector<uint32_t> visibleFaces;
			for (size_t i = 0; i < faces.size(); ++i)
			{
				if (faces[i].active)
				{
					const float d = glm::dot(faces[i].normal, eyePt) + faces[i].dist;
					if (d > 1e-4f)
						visibleFaces.push_back(static_cast<uint32_t>(i));
				}
			}

			if (visibleFaces.empty())
				break;

			// 收集待删除面的孤立外点集
			std::vector<uint32_t> orphanedPoints;
			for (uint32_t fIdx : visibleFaces)
			{
				for (uint32_t pt : faces[fIdx].outsidePoints)
				{
					if (pt != eyePtIdx)
						orphanedPoints.push_back(pt);
				}
			}

			// 提取地平线边缘 (Horizon Edges)：可见面中有向边在可见面集合中不被反向共享的边缘
			std::vector<DirectedEdge> visibleEdges;
			visibleEdges.reserve(visibleFaces.size() * 3);
			for (uint32_t fIdx : visibleFaces)
			{
				const auto& f = faces[fIdx];
				visibleEdges.push_back({ f.v[0], f.v[1] });
				visibleEdges.push_back({ f.v[1], f.v[2] });
				visibleEdges.push_back({ f.v[2], f.v[0] });
			}

			std::vector<DirectedEdge> horizonEdges;
			for (size_t i = 0; i < visibleEdges.size(); ++i)
			{
				const DirectedEdge& edge = visibleEdges[i];
				bool sharedInsideVisible = false;
				for (size_t j = 0; j < visibleEdges.size(); ++j)
				{
					if (i != j && visibleEdges[j].a == edge.b && visibleEdges[j].b == edge.a)
					{
						sharedInsideVisible = true;
						break;
					}
				}
				if (!sharedInsideVisible)
					horizonEdges.push_back(edge);
			}

			// 停用所有可见面
			for (uint32_t fIdx : visibleFaces)
			{
				faces[fIdx].active = false;
				faces[fIdx].outsidePoints.clear();
			}

			// 沿地平线边缘向 eyePt 形成新三角面
			std::vector<uint32_t> newFaceIndices;
			for (const auto& edge : horizonEdges)
			{
				const glm::vec3& pA = uniquePoints[edge.a];
				const glm::vec3& pB = uniquePoints[edge.b];
				glm::vec3 n = glm::cross(pB - pA, eyePt - pA);
				const float len = glm::length(n);
				if (len < 1e-6f)
					continue;
				n /= len;

				QuickFace newFace;
				newFace.v[0] = edge.a;
				newFace.v[1] = edge.b;
				newFace.v[2] = eyePtIdx;
				newFace.normal = n;
				newFace.dist = -glm::dot(n, pA);
				newFace.active = true;

				uint32_t newIdx = static_cast<uint32_t>(faces.size());
				faces.push_back(std::move(newFace));
				newFaceIndices.push_back(newIdx);
			}

			// 将孤立点重新分配到新生成的面上
			for (uint32_t ptIdx : orphanedPoints)
			{
				const glm::vec3& p = uniquePoints[ptIdx];
				float bestDist = 1e-4f;
				int bestFace = -1;
				for (uint32_t nfIdx : newFaceIndices)
				{
					const float d = glm::dot(faces[nfIdx].normal, p) + faces[nfIdx].dist;
					if (d > bestDist)
					{
						bestDist = d;
						bestFace = static_cast<int>(nfIdx);
					}
				}
				if (bestFace >= 0)
				{
					faces[bestFace].outsidePoints.push_back(ptIdx);
					if (bestDist > faces[bestFace].maxDist)
					{
						faces[bestFace].maxDist = bestDist;
						faces[bestFace].furthestPoint = ptIdx;
					}
				}
			}
		}

		// 5. 组装最终凸包顶点与索引
		std::vector<int> remap(uniquePoints.size(), -1);
		for (const auto& face : faces)
		{
			if (!face.active)
				continue;

			for (int k = 0; k < 3; ++k)
			{
				uint32_t oldIdx = face.v[k];
				if (remap[oldIdx] < 0)
				{
					remap[oldIdx] = static_cast<int>(outHull.Vertices.size());
					outHull.Vertices.push_back(uniquePoints[oldIdx]);
				}
				outHull.Indices.push_back(static_cast<uint32_t>(remap[oldIdx]));
			}
		}

		if (outHull.Vertices.size() < 4 || outHull.Indices.size() < 12)
			return false;

		// 6. 计算凸包中心点与真实体积
		outHull.Center = glm::vec3(0.0f);
		for (const auto& v : outHull.Vertices)
			outHull.Center += v;
		outHull.Center /= static_cast<float>(outHull.Vertices.size());

		outHull.Volume = 0.0f;
		for (size_t i = 0; i + 2 < outHull.Indices.size(); i += 3)
		{
			const glm::vec3& a = outHull.Vertices[outHull.Indices[i]];
			const glm::vec3& b = outHull.Vertices[outHull.Indices[i + 1]];
			const glm::vec3& c = outHull.Vertices[outHull.Indices[i + 2]];
			outHull.Volume += SignedVolume(a, b, c, outHull.Center);
		}
		outHull.Volume = std::abs(outHull.Volume);

		return true;
	}

	void ConvexDecomposer::Decompose(
		const std::vector<glm::vec3>& vertices,
		const std::vector<uint32_t>&,
		uint32_t maxHulls,
		float concavityThreshold,
		std::vector<ConvexHull>& outHulls)
	{
		outHulls.clear();
		if (vertices.size() < 4)
			return;

		if (maxHulls <= 1)
		{
			ConvexHull singleHull;
			if (ComputeHull(vertices, singleHull))
				outHulls.push_back(std::move(singleHull));
			return;
		}

		// 递归剖分辅助函数
		std::function<void(const std::vector<glm::vec3>&, uint32_t)> decomposeSubmesh;
		decomposeSubmesh = [&](const std::vector<glm::vec3>& pts, uint32_t allowedHulls)
		{
			if (pts.size() < 4)
				return;

			ConvexHull hull;
			if (!ComputeHull(pts, hull))
				return;

			// 评估网格凹度：测量外凸包各面中心到输入点云的最近欧氏距离
			float maxConcavity = 0.0f;
			for (size_t i = 0; i + 2 < hull.Indices.size(); i += 3)
			{
				const glm::vec3& a = hull.Vertices[hull.Indices[i]];
				const glm::vec3& b = hull.Vertices[hull.Indices[i + 1]];
				const glm::vec3& c = hull.Vertices[hull.Indices[i + 2]];
				const glm::vec3 faceCenter = (a + b + c) / 3.0f;

				float minDistToPts = std::numeric_limits<float>::max();
				for (const auto& p : pts)
					minDistToPts = std::min(minDistToPts, glm::distance(faceCenter, p));

				maxConcavity = std::max(maxConcavity, minDistToPts);
			}

			// 若凹度在阈值内，或已达最大划分凸包上限，接受当前凸包
			if (maxConcavity <= concavityThreshold || allowedHulls <= 1 || pts.size() <= 8)
			{
				outHulls.push_back(std::move(hull));
				return;
			}

			// 沿包围盒最大延展主轴切分点云
			glm::vec3 minP = pts[0], maxP = pts[0];
			for (const auto& p : pts)
			{
				minP = glm::min(minP, p);
				maxP = glm::max(maxP, p);
			}

			const glm::vec3 extents = maxP - minP;
			int splitAxis = 0;
			if (extents.y > extents.x && extents.y > extents.z) splitAxis = 1;
			else if (extents.z > extents.x && extents.z > extents.y) splitAxis = 2;

			const float splitVal = (minP[splitAxis] + maxP[splitAxis]) * 0.5f;

			std::vector<glm::vec3> setA, setB;
			setA.reserve(pts.size());
			setB.reserve(pts.size());
			for (const auto& p : pts)
			{
				if (p[splitAxis] <= splitVal)
					setA.push_back(p);
				else
					setB.push_back(p);
			}

			// 保护子集点数 >= 4；若分流倾斜则按中位数切分
			if (setA.size() < 4 || setB.size() < 4)
			{
				std::vector<glm::vec3> sortedPts = pts;
				std::sort(sortedPts.begin(), sortedPts.end(), [splitAxis](const glm::vec3& l, const glm::vec3& r)
				{
					return l[splitAxis] < r[splitAxis];
				});
				const size_t mid = sortedPts.size() / 2;
				setA.assign(sortedPts.begin(), sortedPts.begin() + mid);
				setB.assign(sortedPts.begin() + mid, sortedPts.end());
			}

			if (setA.size() < 4 || setB.size() < 4)
			{
				outHulls.push_back(std::move(hull));
				return;
			}

			const uint32_t hullsA = allowedHulls / 2;
			const uint32_t hullsB = allowedHulls - hullsA;
			decomposeSubmesh(setA, hullsA);
			decomposeSubmesh(setB, hullsB);
		};

		decomposeSubmesh(vertices, maxHulls);

		if (outHulls.empty())
		{
			ConvexHull fallback;
			if (ComputeHull(vertices, fallback))
				outHulls.push_back(std::move(fallback));
		}
	}
}