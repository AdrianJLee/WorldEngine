#include "wldpch.h"
#include "World/Physics/ConvexDecomposition.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <limits>
#include <unordered_map>
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
			glm::vec3 normal { 0.0f };
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

		// 空间包围盒量化网格坐标与哈希 (用于 O(N) 线性去重)
		struct VoxelKey
		{
			int32_t x = 0;
			int32_t y = 0;
			int32_t z = 0;

			bool operator==(const VoxelKey& o) const noexcept
			{
				return x == o.x && y == o.y && z == o.z;
			}
		};

		struct VoxelKeyHash
		{
			size_t operator()(const VoxelKey& k) const noexcept
			{
				uint64_t h = (static_cast<uint64_t>(static_cast<uint32_t>(k.x)) * 73856093ULL) ^
				             (static_cast<uint64_t>(static_cast<uint32_t>(k.y)) * 19349663ULL) ^
				             (static_cast<uint64_t>(static_cast<uint32_t>(k.z)) * 83492791ULL);
				h ^= (h >> 33);
				h *= 0xff51afd7ed558ccdULL;
				h ^= (h >> 33);
				return static_cast<size_t>(h);
			}
		};

		// 自适应外壳特征降采样: 当输入点云 > maxSamples 时，提取 6 轴极值、128 球面极值与 8x8x8 最外层轮廓特征点
		std::vector<glm::vec3> SampleExtremeHullPoints(const std::vector<glm::vec3>& inPoints, size_t maxSamples = 512)
		{
			if (inPoints.size() <= maxSamples)
				return inPoints;

			glm::vec3 minP(std::numeric_limits<float>::max());
			glm::vec3 maxP(std::numeric_limits<float>::lowest());
			for (const auto& p : inPoints)
			{
				minP = glm::min(minP, p);
				maxP = glm::max(maxP, p);
			}
			const glm::vec3 center = (minP + maxP) * 0.5f;

			std::vector<bool> selected(inPoints.size(), false);
			std::vector<size_t> chosenIndices;
			chosenIndices.reserve(maxSamples);

			auto markIndex = [&](size_t idx)
			{
				if (idx < inPoints.size() && !selected[idx])
				{
					selected[idx] = true;
					chosenIndices.push_back(idx);
				}
			};

			// 1. 提取 6 个轴向极值点
			size_t minX = 0, maxX = 0, minY = 0, maxY = 0, minZ = 0, maxZ = 0;
			for (size_t i = 1; i < inPoints.size(); ++i)
			{
				const auto& p = inPoints[i];
				if (p.x < inPoints[minX].x) minX = i;
				if (p.x > inPoints[maxX].x) maxX = i;
				if (p.y < inPoints[minY].y) minY = i;
				if (p.y > inPoints[maxY].y) maxY = i;
				if (p.z < inPoints[minZ].z) minZ = i;
				if (p.z > inPoints[maxZ].z) maxZ = i;
			}
			markIndex(minX); markIndex(maxX);
			markIndex(minY); markIndex(maxY);
			markIndex(minZ); markIndex(maxZ);

			// 2. 球面均匀方向极值投射 (Fibonacci Sphere 128 方向)，锁定全角度凸包支撑极点
			constexpr size_t kDirCount = 128;
			for (size_t k = 0; k < kDirCount; ++k)
			{
				const float y = 1.0f - (static_cast<float>(k) / static_cast<float>(kDirCount - 1)) * 2.0f;
				const float radius = std::sqrt(std::max(0.0f, 1.0f - y * y));
				constexpr float goldenAngle = 2.3999632f; // pi * (3 - sqrt(5))
				const float theta = goldenAngle * static_cast<float>(k);
				const glm::vec3 dir(std::cos(theta) * radius, y, std::sin(theta) * radius);

				float maxDot = std::numeric_limits<float>::lowest();
				size_t bestIdx = 0;
				for (size_t i = 0; i < inPoints.size(); ++i)
				{
					const float d = glm::dot(inPoints[i], dir);
					if (d > maxDot)
					{
						maxDot = d;
						bestIdx = i;
					}
				}
				markIndex(bestIdx);
			}

			// 3. 空间 8x8x8 网格最外层采样: 筛选各空间扇区内距离质心最远的关键特征点
			constexpr int kGridDim = 8;
			const glm::vec3 extent = maxP - minP;
			const glm::vec3 invExtent = glm::vec3(
				extent.x > 1e-6f ? static_cast<float>(kGridDim) / extent.x : 0.0f,
				extent.y > 1e-6f ? static_cast<float>(kGridDim) / extent.y : 0.0f,
				extent.z > 1e-6f ? static_cast<float>(kGridDim) / extent.z : 0.0f
			);

			struct CellExtreme
			{
				size_t bestIdx = 0;
				float maxDistSq = -1.0f;
			};
			std::vector<CellExtreme> gridExtremes(kGridDim * kGridDim * kGridDim);

			for (size_t i = 0; i < inPoints.size(); ++i)
			{
				const auto& p = inPoints[i];
				const int gx = std::clamp(static_cast<int>((p.x - minP.x) * invExtent.x), 0, kGridDim - 1);
				const int gy = std::clamp(static_cast<int>((p.y - minP.y) * invExtent.y), 0, kGridDim - 1);
				const int gz = std::clamp(static_cast<int>((p.z - minP.z) * invExtent.z), 0, kGridDim - 1);
				const size_t cellIdx = static_cast<size_t>(gx + gy * kGridDim + gz * kGridDim * kGridDim);

				const float distSq = glm::distance2(p, center);
				if (distSq > gridExtremes[cellIdx].maxDistSq)
				{
					gridExtremes[cellIdx].maxDistSq = distSq;
					gridExtremes[cellIdx].bestIdx = i;
				}
			}

			for (const auto& cell : gridExtremes)
			{
				if (cell.maxDistSq >= 0.0f)
					markIndex(cell.bestIdx);
			}

			// 4. 若特征点数未达上限，采用均匀步进补足
			if (chosenIndices.size() < maxSamples)
			{
				const size_t stride = std::max<size_t>(1, inPoints.size() / (maxSamples - chosenIndices.size()));
				for (size_t i = 0; i < inPoints.size() && chosenIndices.size() < maxSamples; i += stride)
					markIndex(i);
			}

			std::vector<glm::vec3> result;
			result.reserve(chosenIndices.size());
			for (size_t idx : chosenIndices)
				result.push_back(inPoints[idx]);

			return result;
		}

		// 基于空间包围盒量化的哈希网格 O(N) 线性去重
		std::vector<glm::vec3> DeduplicatePointsVoxelHash(const std::vector<glm::vec3>& inPoints)
		{
			std::vector<glm::vec3> uniquePoints;
			uniquePoints.reserve(inPoints.size());

			if (inPoints.empty())
				return uniquePoints;

			glm::vec3 minP = inPoints[0], maxP = inPoints[0];
			for (const auto& p : inPoints)
			{
				minP = glm::min(minP, p);
				maxP = glm::max(maxP, p);
			}

			const glm::vec3 extents = maxP - minP;
			const float maxExtent = std::max({ extents.x, extents.y, extents.z });
			if (maxExtent < 1e-6f)
			{
				uniquePoints.push_back(inPoints[0]);
				return uniquePoints;
			}

			constexpr float kVoxelSize = 1e-4f;
			constexpr float kInvVoxelSize = 1.0f / kVoxelSize;
			constexpr float kDistSqThreshold = 1e-8f; // (1e-4f)^2

			std::unordered_map<VoxelKey, std::vector<uint32_t>, VoxelKeyHash> grid;
			grid.reserve(inPoints.size());

			for (const auto& p : inPoints)
			{
				const int32_t vx = static_cast<int32_t>(std::floor((p.x - minP.x) * kInvVoxelSize));
				const int32_t vy = static_cast<int32_t>(std::floor((p.y - minP.y) * kInvVoxelSize));
				const int32_t vz = static_cast<int32_t>(std::floor((p.z - minP.z) * kInvVoxelSize));

				bool duplicate = false;
				for (int32_t dz = -1; dz <= 1 && !duplicate; ++dz)
				{
					for (int32_t dy = -1; dy <= 1 && !duplicate; ++dy)
					{
						for (int32_t dx = -1; dx <= 1 && !duplicate; ++dx)
						{
							const VoxelKey neighborKey { vx + dx, vy + dy, vz + dz };
							auto it = grid.find(neighborKey);
							if (it != grid.end())
							{
								for (uint32_t uIdx : it->second)
								{
									if (glm::distance2(p, uniquePoints[uIdx]) < kDistSqThreshold)
									{
										duplicate = true;
										break;
									}
								}
							}
						}
					}
				}

				if (!duplicate)
				{
					const uint32_t newIdx = static_cast<uint32_t>(uniquePoints.size());
					uniquePoints.push_back(p);
					grid[{ vx, vy, vz }].push_back(newIdx);
				}
			}

			return uniquePoints;
		}
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

			const glm::vec3 cross = glm::cross(b - a, c - a);
			const float len = glm::length(cross);
			if (len < 1e-6f)
				continue;
			const glm::vec3 normal = cross / len;
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

		// 1. 自适应特征采样 (当顶点数 > 512 时提取前 512 个最外层关键顶点)
		const std::vector<glm::vec3> candidatePoints = SampleExtremeHullPoints(inPoints, 512);

		// 2. 基于空间哈希网格进行 O(N) 线性去重
		const std::vector<glm::vec3> uniquePoints = DeduplicatePointsVoxelHash(candidatePoints);
		if (uniquePoints.size() < 4)
			return false;

		// 3. 寻找 6 个轴向极值点构建初始非共面四面体
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
			const glm::vec3 faceCenter = (uniquePoints[a] + uniquePoints[b] + uniquePoints[c]) / 3.0f;
			if (glm::dot(faceCenter - centroid, n) < 0.0f)
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

		// 4. 将其余点分配到各初始面的外点集 (Outside Sets)
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

		// 5. QuickHull 迭代：寻找拥有最远外点的面进行地平线边扩展
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

			// 提取地平线边缘 (Horizon Edges)：统计可见面有向边频率，剔除退化退向边
			struct EdgeKey
			{
				uint32_t a;
				uint32_t b;

				bool operator==(const EdgeKey& o) const noexcept
				{
					return a == o.a && b == o.b;
				}
			};

			struct EdgeKeyHash
			{
				size_t operator()(const EdgeKey& k) const noexcept
				{
					return (static_cast<size_t>(k.a) << 16) ^ static_cast<size_t>(k.b);
				}
			};

			std::unordered_map<EdgeKey, int, EdgeKeyHash> edgeCount;
			for (uint32_t fIdx : visibleFaces)
			{
				const auto& f = faces[fIdx];
				edgeCount[{ f.v[0], f.v[1] }]++;
				edgeCount[{ f.v[1], f.v[2] }]++;
				edgeCount[{ f.v[2], f.v[0] }]++;
			}

			std::vector<DirectedEdge> horizonEdges;
			for (const auto& [edge, count] : edgeCount)
			{
				if (count == 1 && edgeCount.find({ edge.b, edge.a }) == edgeCount.end())
				{
					if (edge.a == edge.b)
						continue;
					if (glm::distance2(uniquePoints[edge.a], uniquePoints[edge.b]) < 1e-8f)
						continue;
					horizonEdges.push_back({ edge.a, edge.b });
				}
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
				if (edge.a == eyePtIdx || edge.b == eyePtIdx)
					continue;

				const glm::vec3& pA = uniquePoints[edge.a];
				const glm::vec3& pB = uniquePoints[edge.b];

				if (glm::distance2(pA, eyePt) < 1e-8f || glm::distance2(pB, eyePt) < 1e-8f)
					continue;

				glm::vec3 n = glm::cross(pB - pA, eyePt - pA);
				const float len = glm::length(n);
				if (len < 1e-6f)
					continue;
				n /= len;

				QuickFace newFace;
				newFace.v[0] = edge.a;
				newFace.v[1] = edge.b;
				newFace.v[2] = eyePtIdx;

				// 严密的新面质心法向朝向校验: 以凸包质心 centroid 为基准强制校对法向
				const glm::vec3 faceCenter = (pA + pB + eyePt) / 3.0f;
				if (glm::dot(n, faceCenter - centroid) < 0.0f)
				{
					n = -n;
					std::swap(newFace.v[0], newFace.v[1]);
				}

				newFace.normal = n;
				newFace.dist = -glm::dot(n, pA);
				newFace.active = true;

				const uint32_t newIdx = static_cast<uint32_t>(faces.size());
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

		// 6. 组装最终凸包顶点与索引
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

		// 7. 计算凸包中心点与真实体积
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

			// 评估网格凹度：测量外凸包各面中心到特征采样点云的最近距离
			// 为消除每面全量双重循环的高额耗时，对点云按特征进行等间隔稀疏采样 (上限 256 点)
			const size_t ptSampleStep = std::max<size_t>(1, pts.size() / 256);
			float maxConcavitySq = 0.0f;
			for (size_t i = 0; i + 2 < hull.Indices.size(); i += 3)
			{
				const glm::vec3& a = hull.Vertices[hull.Indices[i]];
				const glm::vec3& b = hull.Vertices[hull.Indices[i + 1]];
				const glm::vec3& c = hull.Vertices[hull.Indices[i + 2]];
				const glm::vec3 faceCenter = (a + b + c) / 3.0f;

				float minDistSq = std::numeric_limits<float>::max();
				for (size_t pIdx = 0; pIdx < pts.size(); pIdx += ptSampleStep)
				{
					const float d2 = glm::distance2(faceCenter, pts[pIdx]);
					if (d2 < minDistSq)
						minDistSq = d2;
				}

				if (minDistSq > maxConcavitySq)
					maxConcavitySq = minDistSq;
			}
			const float maxConcavity = std::sqrt(maxConcavitySq);

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
			// 切面过渡重叠保护带 (5% 沿轴包围盒延伸)，防止两子凸包在切割接缝处塌缩留缝
			const float overlapBand = extents[splitAxis] * 0.05f;

			std::vector<glm::vec3> setA, setB;
			setA.reserve(pts.size());
			setB.reserve(pts.size());
			for (const auto& p : pts)
			{
				if (p[splitAxis] <= splitVal + overlapBand)
					setA.push_back(p);
				if (p[splitAxis] >= splitVal - overlapBand)
					setB.push_back(p);
			}

			// 保护子集点数 >= 4；若分流倾斜则按中位数切分 (带中位数邻域平滑重叠)
			if (setA.size() < 4 || setB.size() < 4 || setA.size() == pts.size() || setB.size() == pts.size())
			{
				std::vector<glm::vec3> sortedPts = pts;
				std::sort(sortedPts.begin(), sortedPts.end(), [splitAxis](const glm::vec3& l, const glm::vec3& r)
				{
					return l[splitAxis] < r[splitAxis];
				});
				const size_t mid = sortedPts.size() / 2;
				const size_t overlap = std::max<size_t>(1, sortedPts.size() / 20); // 5% overlap
				const size_t endA = std::min(sortedPts.size(), mid + overlap);
				const size_t startB = (mid >= overlap) ? (mid - overlap) : 0;
				setA.assign(sortedPts.begin(), sortedPts.begin() + endA);
				setB.assign(sortedPts.begin() + startB, sortedPts.end());
			}

			if (setA.size() < 4 || setB.size() < 4 || (setA.size() == pts.size() && setB.size() == pts.size()))
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