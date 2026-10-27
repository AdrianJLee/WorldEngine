#include "wldpch.h"
#include "World/NavMesh/NavMesh.h"
#include "World/Scene/Scene.h"
#include "World/Scene/Components.h"
#include "World/Level/WhiteboxBuilder.h"
#include "World/Terrain/Terrain.h"
#include "World/Terrain/TerrainRenderer.h"
#include "World/Asset/WModelIO.h"
#include "World/Core/StringPool.h"

#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <limits>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <map>
#include <queue>
#include <unordered_map>
#include <vector>
#include <future>
#include <chrono>

namespace World
{
	namespace
	{
		constexpr float kPi = 3.14159265358979323846f;

		// 2D 叉积 (在 XZ 平面上判断左右)
		float TriArea2D(const glm::vec3& a, const glm::vec3& b, const glm::vec3& c)
		{
			const float abx = b.x - a.x;
			const float abz = b.z - a.z;
			const float acx = c.x - a.x;
			const float acz = c.z - a.z;
			return acx * abz - abx * acz;
		}

		bool IsPointInConvexPolygon(const glm::vec3& pt, const std::vector<glm::vec3>& verts)
		{
			if (verts.size() < 3)
				return false;

			bool positive = false;
			bool negative = false;

			for (size_t i = 0; i < verts.size(); ++i)
			{
				const glm::vec3& a = verts[i];
				const glm::vec3& b = verts[(i + 1) % verts.size()];
				const float crossZ = (pt.x - a.x) * (b.z - a.z) - (pt.z - a.z) * (b.x - a.x);
				if (crossZ > 0.001f)
					positive = true;
				if (crossZ < -0.001f)
					negative = true;
				if (positive && negative)
					return false;
			}
			return true;
		}
	}

	void NavMesh::Clear()
	{
		m_Vertices.clear();
		m_Polygons.clear();
		m_MacroTiles.clear();
	}

	uint32_t NavMesh::AddVertex(const glm::vec3& pos)
	{
		m_Vertices.push_back(pos);
		return static_cast<uint32_t>(m_Vertices.size() - 1);
	}

	uint32_t NavMesh::AddPolygon(const std::vector<uint32_t>& vertIndices)
	{
		NavMeshPolygon poly;
		poly.VertexIndices = vertIndices;
		poly.Neighbors.assign(vertIndices.size(), -1);
		poly.AreaType = 0;
		poly.Cost = 1.0f;
		poly.Blocked = false;
		poly.Clearance = 0.0f;
		poly.BaseAreaType = 0;
		poly.BaseCost = 1.0f;
		poly.BaseClearance = 0.0f;

		glm::vec3 center(0.0f);
		for (uint32_t idx : vertIndices)
			center += m_Vertices[idx];
		if (!vertIndices.empty())
			center /= static_cast<float>(vertIndices.size());
		poly.Center = center;

		if (vertIndices.size() >= 3)
		{
			const glm::vec3& v0 = m_Vertices[vertIndices[0]];
			const glm::vec3& v1 = m_Vertices[vertIndices[1]];
			const glm::vec3& v2 = m_Vertices[vertIndices[2]];
			const glm::vec3 normal = glm::cross(v1 - v0, v2 - v0);
			const float len = glm::length(normal);
			poly.Normal = (len > 1e-4f) ? (normal / len) : glm::vec3(0.0f, 1.0f, 0.0f);
			poly.Area = len * 0.5f;
		}

		m_Polygons.push_back(std::move(poly));
		return static_cast<uint32_t>(m_Polygons.size() - 1);
	}

	void NavMesh::BuildAdjacency(float stepHeight)
	{
		// Phase 1: 精确顶点索引共享匹配 (高效 64 位整数哈希映射，彻底杜绝 std::map RB-Tree 节点分配瓶颈)
		struct EdgeKeyHash {
			size_t operator()(uint64_t k) const noexcept {
				k ^= k >> 33;
				k *= 0xff51afd7ed558ccdULL;
				k ^= k >> 33;
				k *= 0xc4ceb9fe1a85ec53ULL;
				k ^= k >> 33;
				return static_cast<size_t>(k);
			}
		};
		std::unordered_map<uint64_t, std::pair<uint32_t, uint32_t>, EdgeKeyHash> edgeMap;
		edgeMap.reserve(m_Polygons.size() * 3);

		for (uint32_t p = 0; p < m_Polygons.size(); ++p)
		{
			auto& poly = m_Polygons[p];
			const size_t vCount = poly.VertexIndices.size();
			if (poly.Neighbors.size() < vCount)
				poly.Neighbors.assign(vCount, -1);

			for (size_t e = 0; e < vCount; ++e)
			{
				const uint32_t i0 = poly.VertexIndices[e];
				const uint32_t i1 = poly.VertexIndices[(e + 1) % vCount];
				const uint64_t key = (static_cast<uint64_t>(std::min(i0, i1)) << 32) | static_cast<uint64_t>(std::max(i0, i1));

				auto it = edgeMap.find(key);
				if (it != edgeMap.end())
				{
					// 发现相邻多边形
					const uint32_t otherP = it->second.first;
					const uint32_t otherE = it->second.second;
					poly.Neighbors[e] = static_cast<int32_t>(otherP);
					m_Polygons[otherP].Neighbors[otherE] = static_cast<int32_t>(p);
				}
				else
				{
					edgeMap[key] = { p, static_cast<uint32_t>(e) };
				}
			}
		}

		// Phase 2: 台阶/楼梯跨越拓扑邻接连接 (高度落差 <= stepHeight 且水平投影重合)
		if (stepHeight > 0.0f)
		{
			struct OpenEdge
			{
				uint32_t PolyIdx;
				uint32_t EdgeIdx;
				glm::vec3 P0;
				glm::vec3 P1;
				glm::vec2 MidXZ;
			};

			std::vector<OpenEdge> openEdges;
			openEdges.reserve(m_Polygons.size() * 2);

			for (uint32_t p = 0; p < m_Polygons.size(); ++p)
			{
				const auto& poly = m_Polygons[p];
				const size_t vCount = poly.VertexIndices.size();
				for (size_t e = 0; e < vCount; ++e)
				{
					if (poly.Neighbors[e] == -1)
					{
						const glm::vec3& p0 = m_Vertices[poly.VertexIndices[e]];
						const glm::vec3& p1 = m_Vertices[poly.VertexIndices[(e + 1) % vCount]];
						openEdges.push_back({ p, static_cast<uint32_t>(e), p0, p1, glm::vec2((p0.x + p1.x) * 0.5f, (p0.z + p1.z) * 0.5f) });
					}
				}
			}

			// 空间网格分桶 (桶尺寸 1.0m)
			const float bucketSize = 1.0f;
			auto hashCell = [](int x, int z) -> uint64_t {
				return (static_cast<uint64_t>(static_cast<uint32_t>(x)) << 32) | static_cast<uint32_t>(z);
			};
			std::unordered_map<uint64_t, std::vector<size_t>> buckets;
			for (size_t i = 0; i < openEdges.size(); ++i)
			{
				int bx = static_cast<int>(std::floor(openEdges[i].MidXZ.x / bucketSize));
				int bz = static_cast<int>(std::floor(openEdges[i].MidXZ.y / bucketSize));
				buckets[hashCell(bx, bz)].push_back(i);
			}

			for (size_t i = 0; i < openEdges.size(); ++i)
			{
				const auto& edgeA = openEdges[i];
				if (m_Polygons[edgeA.PolyIdx].Neighbors[edgeA.EdgeIdx] != -1)
					continue;

				int bx = static_cast<int>(std::floor(edgeA.MidXZ.x / bucketSize));
				int bz = static_cast<int>(std::floor(edgeA.MidXZ.y / bucketSize));

				bool matched = false;
				for (int dz = -1; dz <= 1 && !matched; ++dz)
				{
					for (int dx = -1; dx <= 1 && !matched; ++dx)
					{
						auto it = buckets.find(hashCell(bx + dx, bz + dz));
						if (it == buckets.end())
							continue;

						for (size_t j : it->second)
						{
							if (i == j)
								continue;
							const auto& edgeB = openEdges[j];
							if (edgeA.PolyIdx == edgeB.PolyIdx)
								continue;
							if (m_Polygons[edgeB.PolyIdx].Neighbors[edgeB.EdgeIdx] != -1)
								continue;

							// 判定水平方向重合与垂直高度差
							float d01 = glm::distance(glm::vec2(edgeA.P0.x, edgeA.P0.z), glm::vec2(edgeB.P1.x, edgeB.P1.z));
							float d10 = glm::distance(glm::vec2(edgeA.P1.x, edgeA.P1.z), glm::vec2(edgeB.P0.x, edgeB.P0.z));
							float vDiff0 = std::abs(edgeA.P0.y - edgeB.P1.y);
							float vDiff1 = std::abs(edgeA.P1.y - edgeB.P0.y);

							if ((d01 < 0.25f && d10 < 0.25f && vDiff0 <= stepHeight + 0.05f && vDiff1 <= stepHeight + 0.05f) ||
								(glm::distance(glm::vec2(edgeA.P0.x, edgeA.P0.z), glm::vec2(edgeB.P0.x, edgeB.P0.z)) < 0.25f &&
								 glm::distance(glm::vec2(edgeA.P1.x, edgeA.P1.z), glm::vec2(edgeB.P1.x, edgeB.P1.z)) < 0.25f &&
								 std::abs(edgeA.P0.y - edgeB.P0.y) <= stepHeight + 0.05f && std::abs(edgeA.P1.y - edgeB.P1.y) <= stepHeight + 0.05f))
							{
								m_Polygons[edgeA.PolyIdx].Neighbors[edgeA.EdgeIdx] = static_cast<int32_t>(edgeB.PolyIdx);
								m_Polygons[edgeB.PolyIdx].Neighbors[edgeB.EdgeIdx] = static_cast<int32_t>(edgeA.PolyIdx);
								matched = true;
								break;
							}
						}
					}
				}
			}
		}
	}

	void NavMesh::AddOffMeshLink(const NavMeshOffMeshLink& link)
	{
		m_OffMeshLinks.push_back(link);

		if (!m_Polygons.empty())
		{
			glm::vec3 sPt, ePt;
			uint32_t pStart = 0, pEnd = 0;
			if (SamplePosition(link.Start, 5.0f, sPt, pStart) &&
				SamplePosition(link.End, 5.0f, ePt, pEnd) &&
				pStart != pEnd)
			{
				auto& nbrsS = m_Polygons[pStart].Neighbors;
				if (std::find(nbrsS.begin(), nbrsS.end(), static_cast<int32_t>(pEnd)) == nbrsS.end())
				{
					nbrsS.push_back(static_cast<int32_t>(pEnd));
				}
				if (link.BiDirectional)
				{
					auto& nbrsE = m_Polygons[pEnd].Neighbors;
					if (std::find(nbrsE.begin(), nbrsE.end(), static_cast<int32_t>(pStart)) == nbrsE.end())
					{
						nbrsE.push_back(static_cast<int32_t>(pStart));
					}
				}
			}
		}
	}

	void NavMesh::ClearOffMeshLinks()
	{
		m_OffMeshLinks.clear();
	}

	void NavMesh::AddAreaModifier(const NavMeshAreaModifier& modifier)
	{
		m_AreaModifiers.push_back(modifier);

		if (!m_Polygons.empty())
		{
			for (auto& poly : m_Polygons)
			{
				if (modifier.ContainsPoint(poly.Center))
				{
					poly.AreaType = modifier.AreaType;
					poly.Cost = modifier.CostMultiplier;

					float effClearance = modifier.ClearanceHeight;
					if (effClearance <= 0.0f)
					{
						effClearance = std::max(0.0f, (modifier.Center + modifier.Extents).y - poly.Center.y);
					}
					poly.Clearance = effClearance;
					if (modifier.AreaType == 4 || modifier.CostMultiplier <= 0.0f)
					{
						poly.Blocked = true;
					}
				}
			}
		}
	}

	void NavMesh::ClearAreaModifiers()
	{
		m_AreaModifiers.clear();
	}

	void NavMesh::ResetAreaModifiers()
	{
		m_AreaModifiers.clear();
		for (auto& poly : m_Polygons)
		{
			poly.AreaType = poly.BaseAreaType;
			poly.Cost = poly.BaseCost;
			poly.Clearance = poly.BaseClearance;
			poly.Blocked = (poly.AreaType == 4 || poly.Cost <= 0.0f);
		}
	}

	void NavMesh::ReapplyAreaModifiers(const std::vector<NavMeshAreaModifier>& modifiers)
	{
		ResetAreaModifiers();
		for (const auto& mod : modifiers)
		{
			AddAreaModifier(mod);
		}
		BuildMacroTiles(m_MacroTileSize);
	}

	bool NavMesh::SamplePosition(const glm::vec3& point, float maxDistance,
		glm::vec3& outNavPoint, uint32_t& outPolyIdx,
		const NavQueryFilter& filter) const
	{
		float bestDist2 = maxDistance * maxDistance;
		bool found = false;

		for (uint32_t p = 0; p < static_cast<uint32_t>(m_Polygons.size()); ++p)
		{
			const auto& poly = m_Polygons[p];
			if (poly.Blocked || poly.Cost <= 0.0f)
				continue;
			if (!filter.CanTraverse(poly.AreaType, poly.Clearance))
				continue;

			// 快速中心距离粗筛：多边形半径通常小于 4.0m，超出当前最优距离 + 半径裕量则直接跳过
			const glm::vec3 toPt = point - poly.Center;
			const float centerDist2 = glm::dot(toPt, toPt);
			if (centerDist2 > bestDist2 + 16.0f)
				continue;

			const size_t vertCount = poly.VertexIndices.size();
			if (vertCount < 3)
				continue;

			// 点到多边形平面的正交投影
			const float distToPlane = glm::dot(toPt, poly.Normal);
			const glm::vec3 projected = point - poly.Normal * distToPlane;

			// 零堆内存分配的凸多边形内部检测 (直接按索引访问 m_Vertices)
			bool inside = true;
			bool positive = false;
			bool negative = false;
			for (size_t i = 0; i < vertCount; ++i)
			{
				const glm::vec3& a = m_Vertices[poly.VertexIndices[i]];
				const glm::vec3& b = m_Vertices[poly.VertexIndices[(i + 1) % vertCount]];
				const float crossZ = (projected.x - a.x) * (b.z - a.z) - (projected.z - a.z) * (b.x - a.x);
				if (crossZ > 0.001f) positive = true;
				if (crossZ < -0.001f) negative = true;
				if (positive && negative) { inside = false; break; }
			}

			if (inside)
			{
				const glm::vec3 diff1 = point - projected;
				const float d2 = glm::dot(diff1, diff1);
				if (d2 < bestDist2)
				{
					bestDist2 = d2;
					outNavPoint = projected;
					outPolyIdx = p;
					found = true;
				}
			}
			else
			{
				// 多边形各边最近点求取 (直接从 m_Vertices 读取，杜绝 vector 拷贝)
				for (size_t i = 0; i < vertCount; ++i)
				{
					const glm::vec3& a = m_Vertices[poly.VertexIndices[i]];
					const glm::vec3& b = m_Vertices[poly.VertexIndices[(i + 1) % vertCount]];
					const glm::vec3 ab = b - a;
					const float t = std::clamp(glm::dot(point - a, ab) / std::max(glm::dot(ab, ab), 1e-4f), 0.0f, 1.0f);
					const glm::vec3 edgePt = a + ab * t;
					const glm::vec3 diff2 = point - edgePt;
					const float d2 = glm::dot(diff2, diff2);
					if (d2 < bestDist2)
					{
						bestDist2 = d2;
						outNavPoint = edgePt;
						outPolyIdx = p;
						found = true;
					}
				}
			}
		}

		return found;
	}

	bool NavMesh::HasSharedBoundary(uint32_t polyA, uint32_t polyB) const
	{
		if (polyA >= m_Polygons.size() || polyB >= m_Polygons.size() || polyA == polyB)
			return false;

		const auto& pA = m_Polygons[polyA];
		const auto& pB = m_Polygons[polyB];

		// 1. 检查拓扑共享顶点
		size_t sharedCount = 0;
		for (uint32_t va : pA.VertexIndices)
		{
			for (uint32_t vb : pB.VertexIndices)
			{
				if (va == vb)
				{
					++sharedCount;
					if (sharedCount >= 2)
						return true;
				}
			}
		}

		// 2. 检查台阶连续边界 (水平贴合，垂直落差 <= 0.65m)
		sharedCount = 0;
		for (uint32_t va : pA.VertexIndices)
		{
			for (uint32_t vb : pB.VertexIndices)
			{
				const glm::vec3& pa = m_Vertices[va];
				const glm::vec3& pb = m_Vertices[vb];
				if (glm::distance(glm::vec2(pa.x, pa.z), glm::vec2(pb.x, pb.z)) < 0.25f &&
					std::abs(pa.y - pb.y) <= 0.65f)
				{
					++sharedCount;
					if (sharedCount >= 2)
						return true;
				}
			}
		}

		return false;
	}

	bool NavMesh::FindPath(const glm::vec3& start, const glm::vec3& end,
		std::vector<glm::vec3>& outPath,
		const NavQueryFilter& filter) const
	{
		outPath.clear();
		if (m_Polygons.empty())
			return false;

		glm::vec3 navStart, navEnd;
		uint32_t polyStart = 0, polyEnd = 0;
		if (!SamplePosition(start, 50.0f, navStart, polyStart, filter) ||
			!SamplePosition(end, 50.0f, navEnd, polyEnd, filter))
		{
			return false;
		}

		if (polyStart == polyEnd)
		{
			outPath = { navStart, navEnd };
			return true;
		}

		// 预解析跨网格跳跃 (Off-Mesh Links) 的拓扑连接与代价映射
		struct ResolvedLink
		{
			uint32_t FromPoly = 0;
			uint32_t ToPoly = 0;
			glm::vec3 StartPt { 0.0f };
			glm::vec3 EndPt { 0.0f };
			float Cost = 1.0f;
		};

		std::vector<ResolvedLink> resolvedLinks;
		if (!m_OffMeshLinks.empty())
		{
			resolvedLinks.reserve(m_OffMeshLinks.size() * 2);
			for (const auto& link : m_OffMeshLinks)
			{
				glm::vec3 sPt, ePt;
				uint32_t pStart = 0, pEnd = 0;
				if (SamplePosition(link.Start, 5.0f, sPt, pStart) &&
					SamplePosition(link.End, 5.0f, ePt, pEnd) &&
					pStart != pEnd)
				{
					resolvedLinks.push_back({ pStart, pEnd, link.Start, link.End, link.Cost });
					if (link.BiDirectional)
					{
						resolvedLinks.push_back({ pEnd, pStart, link.End, link.Start, link.Cost });
					}
				}
			}
		}

		// 3D A* 走廊搜索 (标准欧氏距离启发式 + 区域代价与跳跃代价加权)
		struct AStarNode
		{
			uint32_t PolyIdx;
			float F;
			bool operator>(const AStarNode& other) const { return F > other.F; }
		};

		std::priority_queue<AStarNode, std::vector<AStarNode>, std::greater<AStarNode>> openSet;
		std::unordered_map<uint32_t, float> gScore;
		std::unordered_map<uint32_t, uint32_t> cameFrom;
		gScore.reserve(256);
		cameFrom.reserve(256);

		gScore[polyStart] = 0.0f;
		// 启发式恢复标准欧氏距离：1.0f 权重保证目标牵引力
		openSet.push({ polyStart, glm::distance(m_Polygons[polyStart].Center, navEnd) });

		bool reached = false;

		while (!openSet.empty())
		{
			const uint32_t curr = openSet.top().PolyIdx;
			openSet.pop();

			if (curr == polyEnd)
			{
				reached = true;
				break;
			}

			const auto& currPoly = m_Polygons[curr];
			for (int32_t neighbor : currPoly.Neighbors)
			{
				if (neighbor < 0 || static_cast<size_t>(neighbor) >= m_Polygons.size())
					continue;
				const auto& nbrPoly = m_Polygons[neighbor];
				if (nbrPoly.Blocked || nbrPoly.Cost <= 0.0f)
					continue;
				if (!filter.CanTraverse(nbrPoly.AreaType, nbrPoly.Clearance))
					continue;

				// 检查是否为跨网格跳跃边
				const ResolvedLink* pLink = nullptr;
				for (const auto& rl : resolvedLinks)
				{
					if (rl.FromPoly == curr && rl.ToPoly == static_cast<uint32_t>(neighbor))
					{
						pLink = &rl;
						break;
					}
				}

				// 移动代价计算：若是跳跃边，将跳跃基础惩罚与 link.Cost 权重真实累加，避免平地直行误跳
				float edgeCost = 0.0f;
				if (pLink)
				{
					const float jumpDist = glm::distance(pLink->StartPt, pLink->EndPt);
					edgeCost = jumpDist * nbrPoly.Cost + 5.0f + std::max(pLink->Cost, 1.0f) * 4.0f;
				}
				else
				{
					const float edgeDist = glm::distance(currPoly.Center, nbrPoly.Center);
					edgeCost = edgeDist * nbrPoly.Cost;
				}

				const float tentativeG = gScore[curr] + edgeCost;

				auto itG = gScore.find(neighbor);
				if (itG == gScore.end() || tentativeG < itG->second)
				{
					cameFrom[neighbor] = curr;
					gScore[neighbor] = tentativeG;
					const float h = glm::distance(m_Polygons[neighbor].Center, navEnd);
					openSet.push({ static_cast<uint32_t>(neighbor), tentativeG + h });
				}
			}
		}

		if (!reached)
			return false;

		// 重构多边形走廊序列
		std::vector<uint32_t> corridor;
		uint32_t trace = polyEnd;
		while (trace != polyStart)
		{
			corridor.push_back(trace);
			trace = cameFrom[trace];
		}
		corridor.push_back(polyStart);
		std::reverse(corridor.begin(), corridor.end());
		for (size_t ci = 0; ci < corridor.size(); ++ci) {
			const auto& cp = m_Polygons[corridor[ci]];
			WLD_CORE_WARN("  [{0}] poly={1} center=({2:.2f},{3:.2f},{4:.2f})", ci, corridor[ci], cp.Center.x, cp.Center.y, cp.Center.z);
		}

		// 走廊跨跳跃点折返多边形剪枝：消除跳跃后紧接着折回跳跃前相邻区域的无效多边形
		if (corridor.size() >= 3)
		{
			for (size_t i = 0; i + 2 < corridor.size();)
			{
				if (!HasSharedBoundary(corridor[i], corridor[i + 1]))
				{
					if (HasSharedBoundary(corridor[i], corridor[i + 2]))
					{
						corridor.erase(corridor.begin() + i + 1);
						continue;
					}
					if (i + 3 < corridor.size() && HasSharedBoundary(corridor[i], corridor[i + 3]))
					{
						corridor.erase(corridor.begin() + i + 1, corridor.begin() + i + 3);
						continue;
					}
				}
				++i;
			}
		}

		// 提取过境线门径 (Portals) 并处理跨网格跳跃 (Off-Mesh Links)
		struct PathSegment
		{
			glm::vec3 SegStart;
			glm::vec3 SegEnd;
			std::vector<NavMeshPortal> Portals;
			bool HasJumpAfter = false;
			glm::vec3 JumpStart { 0.0f };
			glm::vec3 JumpEnd { 0.0f };
		};

		std::vector<PathSegment> segments;
		segments.push_back({ navStart, navEnd, {}, false, glm::vec3(0.0f), glm::vec3(0.0f) });

		for (size_t i = 0; i + 1 < corridor.size(); ++i)
		{
			const auto& pA = m_Polygons[corridor[i]];
			const auto& pB = m_Polygons[corridor[i + 1]];

			// 寻找公共边端点
			std::vector<glm::vec3> sharedVerts;
			for (uint32_t va : pA.VertexIndices)
			{
				for (uint32_t vb : pB.VertexIndices)
				{
					if (va == vb)
					{
						sharedVerts.push_back(m_Vertices[va]);
					}
				}
			}

			// 若顶点索引不同，检查是否为台阶边界 (水平贴合，垂直落差 <= StepHeight)
			if (sharedVerts.size() < 2)
			{
				sharedVerts.clear();
				for (uint32_t va : pA.VertexIndices)
				{
					for (uint32_t vb : pB.VertexIndices)
					{
						const glm::vec3& pa = m_Vertices[va];
						const glm::vec3& pb = m_Vertices[vb];
						if (glm::distance(glm::vec2(pa.x, pa.z), glm::vec2(pb.x, pb.z)) < 0.25f &&
							std::abs(pa.y - pb.y) <= 0.65f)
						{
							sharedVerts.push_back((pa + pb) * 0.5f);
						}
					}
				}
			}

			if (sharedVerts.size() >= 2)
			{
				NavMeshPortal portal;
				portal.Left = sharedVerts[0];
				portal.Right = sharedVerts[1];
				if (TriArea2D(pA.Center, portal.Left, portal.Right) < 0.0f)
					std::swap(portal.Left, portal.Right);
				segments.back().Portals.push_back(portal);
			}
			else
			{
				// 跨网格跳跃链接 (Off-Mesh Link) 检查
				bool matched = false;
				glm::vec3 jumpS = pA.Center;
				glm::vec3 jumpE = pB.Center;

				for (const auto& rl : resolvedLinks)
				{
					if (rl.FromPoly == corridor[i] && rl.ToPoly == corridor[i + 1])
					{
						jumpS = rl.StartPt;
						jumpE = rl.EndPt;
						matched = true;
						break;
					}
				}

				if (matched)
				{
					segments.back().HasJumpAfter = true;
					segments.back().SegEnd = jumpS;
					segments.back().JumpStart = jumpS;
					segments.back().JumpEnd = jumpE;

					PathSegment nextSeg;
					nextSeg.SegStart = jumpE;
					nextSeg.SegEnd = navEnd;
					segments.push_back(nextSeg);
				}
				else
				{
					// 未焊合/T-Junction 等拓扑相邻但未提取出共享顶点的普通连续网格，生成虚拟门径平滑穿过，绝不插入跳跃点
					glm::vec3 midPt = (pA.Center + pB.Center) * 0.5f;
					glm::vec3 dir = pB.Center - pA.Center;
					glm::vec3 perp(0.0f);
					if (dir.x * dir.x + dir.z * dir.z > 1e-4f)
					{
						perp = glm::normalize(glm::vec3(-dir.z, 0.0f, dir.x));
					}
					else
					{
						perp = glm::vec3(1.0f, 0.0f, 0.0f);
					}
					NavMeshPortal portal;
					portal.Left = midPt - perp * 0.5f;
					portal.Right = midPt + perp * 0.5f;
					if (TriArea2D(pA.Center, portal.Left, portal.Right) < 0.0f)
						std::swap(portal.Left, portal.Right);
					segments.back().Portals.push_back(portal);
				}
			}
		}

		outPath.clear();
		for (size_t s = 0; s < segments.size(); ++s)
		{
			std::vector<glm::vec3> segPath;
			FunnelSmooth(segments[s].Portals, segments[s].SegStart, segments[s].SegEnd, segPath);

			for (size_t p = 0; p < segPath.size(); ++p)
			{
				if (outPath.empty() || glm::distance(outPath.back(), segPath[p]) > 1e-3f)
				{
					outPath.push_back(segPath[p]);
				}
			}

			if (segments[s].HasJumpAfter)
			{
				if (outPath.empty() || glm::distance(outPath.back(), segments[s].JumpEnd) > 1e-3f)
				{
					outPath.push_back(segments[s].JumpEnd);
				}
			}
		}

		// Funnel 跨跳跃点折返保护：消除跳跃后锐角折回跳跃前区域的无效三角形
		if (outPath.size() >= 4)
		{
			bool pruned = false;
			do
			{
				pruned = false;
				for (size_t i = 1; i + 2 < outPath.size(); ++i)
				{
					bool isJump = false;
					for (const auto& rl : resolvedLinks)
					{
						if (glm::distance(outPath[i], rl.StartPt) < 0.2f &&
							glm::distance(outPath[i + 1], rl.EndPt) < 0.2f)
						{
							isJump = true;
							break;
						}
					}

					if (isJump)
					{
						const glm::vec3& pPrev = outPath[i - 1];
						const glm::vec3& jStart = outPath[i];
						const glm::vec3& jEnd = outPath[i + 1];

						for (size_t j = i + 2; j < std::min(outPath.size(), i + 4); ++j)
						{
							const glm::vec3& pNext = outPath[j];
							const glm::vec2 vJump(jEnd.x - jStart.x, jEnd.z - jStart.z);
							const glm::vec2 vBack(pNext.x - jEnd.x, pNext.z - jEnd.z);
							const float lenJump = glm::length(vJump);
							const float lenBack = glm::length(vBack);

							if (lenJump > 0.05f && lenBack > 0.05f)
							{
								const float cosTheta = glm::dot(vJump / lenJump, vBack / lenBack);
								if (cosTheta < -0.1f && std::abs(pPrev.y - pNext.y) <= 0.8f)
								{
									if (Raycast(pPrev, pNext))
									{
										outPath.erase(outPath.begin() + i, outPath.begin() + j);
										pruned = true;
										break;
									}
								}
							}
						}
					}
					if (pruned) break;
				}
			} while (pruned && outPath.size() >= 4);
		}

		return !outPath.empty();
	}

	void NavMesh::FunnelSmooth(const std::vector<NavMeshPortal>& portals,
		const glm::vec3& start, const glm::vec3& end,
		std::vector<glm::vec3>& outPath)
	{
		outPath.clear();
		outPath.push_back(start);

		if (portals.empty())
		{
			outPath.push_back(end);
			return;
		}

		glm::vec3 portalApex = start;
		glm::vec3 portalLeft = portals[0].Left;
		glm::vec3 portalRight = portals[0].Right;
		int apexIndex = 0, leftIndex = 0, rightIndex = 0;

		for (size_t i = 1; i < portals.size(); ++i)
		{
			const glm::vec3& left = portals[i].Left;
			const glm::vec3& right = portals[i].Right;

			// 收紧右边界
			if (TriArea2D(portalApex, portalRight, right) <= 0.0f)
			{
				if (portalApex == portalRight || TriArea2D(portalApex, portalLeft, right) > 0.0f)
				{
					portalRight = right;
					rightIndex = static_cast<int>(i);
				}
				else
				{
					outPath.push_back(portalLeft);
					portalApex = portalLeft;
					apexIndex = leftIndex;

					portalLeft = portalApex;
					portalRight = portalApex;
					leftIndex = apexIndex;
					rightIndex = apexIndex;
					i = apexIndex;
					continue;
				}
			}

			// 收紧左边界
			if (TriArea2D(portalApex, portalLeft, left) >= 0.0f)
			{
				if (portalApex == portalLeft || TriArea2D(portalApex, portalRight, left) < 0.0f)
				{
					portalLeft = left;
					leftIndex = static_cast<int>(i);
				}
				else
				{
					outPath.push_back(portalRight);
					portalApex = portalRight;
					apexIndex = rightIndex;

					portalLeft = portalApex;
					portalRight = portalApex;
					leftIndex = apexIndex;
					rightIndex = apexIndex;
					i = apexIndex;
					continue;
				}
			}
		}

		outPath.push_back(end);
	}

	void NavMesh::BakeFromTerrain(const TerrainData& terrain, float maxSlopeDeg, float stepSize)
	{
		Clear();
		if (terrain.GetWidth() < 2 || terrain.GetHeight() < 2 || stepSize <= 0.0f)
			return;

		const float worldW = terrain.GetWorldWidth();
		const float worldH = terrain.GetWorldHeight();

		const uint32_t numStepsX = std::max(1u, static_cast<uint32_t>(worldW / stepSize));
		const uint32_t numStepsZ = std::max(1u, static_cast<uint32_t>(worldH / stepSize));

		// 采样规则网格顶点
		std::vector<std::vector<uint32_t>> grid(numStepsZ + 1, std::vector<uint32_t>(numStepsX + 1, 0));
		for (uint32_t z = 0; z <= numStepsZ; ++z)
		{
			for (uint32_t x = 0; x <= numStepsX; ++x)
			{
				const float wx = std::min(static_cast<float>(x) * stepSize, worldW);
				const float wz = std::min(static_cast<float>(z) * stepSize, worldH);
				const float wy = terrain.SampleHeight(wx, wz);
				grid[z][x] = AddVertex(glm::vec3(wx, wy, wz));
			}
		}

		// 构建四边形/三角形多边形
		for (uint32_t z = 0; z < numStepsZ; ++z)
		{
			for (uint32_t x = 0; x < numStepsX; ++x)
			{
				const float wx = static_cast<float>(x) * stepSize + stepSize * 0.5f;
				const float wz = static_cast<float>(z) * stepSize + stepSize * 0.5f;

				const uint32_t gx = std::clamp(static_cast<uint32_t>(wx / terrain.GetCellSize()), 0u, terrain.GetWidth() - 1);
				const uint32_t gz = std::clamp(static_cast<uint32_t>(wz / terrain.GetCellSize()), 0u, terrain.GetHeight() - 1);
				if (terrain.IsHole(gx, gz))
					continue;

				const glm::vec3 normal = terrain.SampleNormal(wx, wz);
				const float slope = std::acos(std::clamp(normal.y, 0.0f, 1.0f)) * (180.0f / kPi);
				if (slope > maxSlopeDeg)
					continue; // 超过可攀爬最大坡度，剔除

				const uint32_t v0 = grid[z][x];
				const uint32_t v1 = grid[z][x + 1];
				const uint32_t v2 = grid[z + 1][x + 1];
				const uint32_t v3 = grid[z + 1][x];

				AddPolygon({ v0, v1, v2, v3 });
			}
		}

		BuildAdjacency(stepSize);

		// 应用 Area Modifiers 与 Off-Mesh Links
		for (auto& poly : m_Polygons)
		{
			for (const auto& mod : m_AreaModifiers)
			{
				if (mod.ContainsPoint(poly.Center))
				{
					poly.AreaType = mod.AreaType;
					poly.Cost = mod.CostMultiplier;
					if (mod.AreaType == 4 || mod.CostMultiplier <= 0.0f)
						poly.Blocked = true;
				}
			}
		}

		for (const auto& link : m_OffMeshLinks)
		{
			glm::vec3 sPt, ePt;
			uint32_t pStart = 0, pEnd = 0;
			if (SamplePosition(link.Start, 5.0f, sPt, pStart) &&
				SamplePosition(link.End, 5.0f, ePt, pEnd) &&
				pStart != pEnd)
			{
				auto& nbrsS = m_Polygons[pStart].Neighbors;
				if (std::find(nbrsS.begin(), nbrsS.end(), static_cast<int32_t>(pEnd)) == nbrsS.end())
				{
					nbrsS.push_back(static_cast<int32_t>(pEnd));
				}
				if (link.BiDirectional)
				{
					auto& nbrsE = m_Polygons[pEnd].Neighbors;
					if (std::find(nbrsE.begin(), nbrsE.end(), static_cast<int32_t>(pStart)) == nbrsE.end())
					{
						nbrsE.push_back(static_cast<int32_t>(pStart));
					}
				}
			}
		}
		BuildMacroTiles(m_MacroTileSize);
	}

	void NavMesh::BakeFromTriangles(const std::vector<NavMeshTriangle>& triangles, const NavMeshBakeSettings& settings)
	{
		Clear();
		if (triangles.empty())
			return;

		// 1. 坡度过滤 (法线 cos(theta) >= cos(MaxClimbSlopeDeg))
		const float minCosSlope = std::cos(glm::radians(std::clamp(settings.MaxClimbSlopeDeg, 0.0f, 89.0f)));
		std::vector<NavMeshTriangle> walkableTriangles;
		walkableTriangles.reserve(triangles.size());

		for (const auto& tri : triangles)
		{
			const glm::vec3 edge1 = tri.B - tri.A;
			const glm::vec3 edge2 = tri.C - tri.A;
			const glm::vec3 normal = glm::cross(edge1, edge2);
			const float len = glm::length(normal);
			if (len < 1e-5f)
				continue;
			const glm::vec3 unitNormal = normal / len;
			if (unitNormal.y >= minCosSlope)
			{
				walkableTriangles.push_back(tri);
			}
		}

		if (walkableTriangles.empty())
			return;

		// 2. 包围盒计算
		glm::vec3 bMin = walkableTriangles[0].A;
		glm::vec3 bMax = walkableTriangles[0].A;
		for (const auto& tri : walkableTriangles)
		{
			bMin = glm::min(bMin, glm::min(tri.A, glm::min(tri.B, tri.C)));
			bMax = glm::max(bMax, glm::max(tri.A, glm::max(tri.B, tri.C)));
		}

		if (settings.UseCustomBounds)
		{
			bMin = glm::max(bMin, settings.BoundsMin);
			bMax = glm::min(bMax, settings.BoundsMax);
		}

		if (bMin.x >= bMax.x || bMin.z >= bMax.z)
			return;

		// 3. 3D 体素网格光栅化与离散采样
		const float rawVoxelSize = (settings.VoxelSize >= 0.1f) ? settings.VoxelSize : 1.0f;
		// 工业级体素分辨率自适应保护 (对标 UE5 Recast Tile 限制：最大网格分辨率 192x192，超大开放世界自动对齐体素步长，保证毫秒级构建)
		constexpr uint32_t kMaxGridRes = 128u;
		const float maxDim = std::max(bMax.x - bMin.x, bMax.z - bMin.z);
		const float voxelSize = std::max(rawVoxelSize, maxDim / static_cast<float>(kMaxGridRes));
		const uint32_t numX = std::clamp(static_cast<uint32_t>(std::ceil((bMax.x - bMin.x) / voxelSize)), 1u, kMaxGridRes);
		const uint32_t numZ = std::clamp(static_cast<uint32_t>(std::ceil((bMax.z - bMin.z) / voxelSize)), 1u, kMaxGridRes);

		struct Span
		{
			float Height;
			glm::vec3 Normal;
		};
		std::vector<std::vector<Span>> cellSpans(numX * numZ);

		for (const auto& tri : walkableTriangles)
		{
			const glm::vec3 e1 = tri.B - tri.A;
			const glm::vec3 e2 = tri.C - tri.A;
			const glm::vec3 norm = glm::normalize(glm::cross(e1, e2));

			const float triMinX = std::min({tri.A.x, tri.B.x, tri.C.x});
			const float triMaxX = std::max({tri.A.x, tri.B.x, tri.C.x});
			const float triMinZ = std::min({tri.A.z, tri.B.z, tri.C.z});
			const float triMaxZ = std::max({tri.A.z, tri.B.z, tri.C.z});

			int minGX = std::clamp(static_cast<int>(std::floor((triMinX - bMin.x) / voxelSize)), 0, static_cast<int>(numX - 1));
			int maxGX = std::clamp(static_cast<int>(std::floor((triMaxX - bMin.x) / voxelSize)), 0, static_cast<int>(numX - 1));
			int minGZ = std::clamp(static_cast<int>(std::floor((triMinZ - bMin.z) / voxelSize)), 0, static_cast<int>(numZ - 1));
			int maxGZ = std::clamp(static_cast<int>(std::floor((triMaxZ - bMin.z) / voxelSize)), 0, static_cast<int>(numZ - 1));

			for (int gz = minGZ; gz <= maxGZ; ++gz)
			{
				for (int gx = minGX; gx <= maxGX; ++gx)
				{
					const float cx = bMin.x + (static_cast<float>(gx) + 0.5f) * voxelSize;
					const float cz = bMin.z + (static_cast<float>(gz) + 0.5f) * voxelSize;
					const glm::vec3 pt(cx, 0.0f, cz);

					// 2D XZ 三角形相交与包围测试
					const float s1 = TriArea2D(pt, tri.A, tri.B);
					const float s2 = TriArea2D(pt, tri.B, tri.C);
					const float s3 = TriArea2D(pt, tri.C, tri.A);
					const bool inside = !(((s1 < -1e-4f) || (s2 < -1e-4f) || (s3 < -1e-4f)) &&
					                      ((s1 > 1e-4f) || (s2 > 1e-4f) || (s3 > 1e-4f)));

					if (inside)
					{
						float cy = tri.A.y;
						if (std::abs(norm.y) > 1e-4f)
						{
							cy = tri.A.y - (norm.x * (cx - tri.A.x) + norm.z * (cz - tri.A.z)) / norm.y;
						}

						const float layerMergeThreshold = std::max(settings.AgentHeight, 1.85f);
						auto& spans = cellSpans[gz * numX + gx];
						bool merged = false;
						for (auto& s : spans)
						{
							if (std::abs(s.Height - cy) <= layerMergeThreshold)
							{
								s.Height = std::max(s.Height, cy);
								s.Normal = norm;
								merged = true;
								break;
							}
						}
						if (!merged)
						{
							spans.push_back({ cy, norm });
						}
					}
				}
			}
		}

		// 补齐微小三角面顶点所在的单元格
		for (const auto& tri : walkableTriangles)
		{
			const glm::vec3 pts[3] = { tri.A, tri.B, tri.C };
			const glm::vec3 e1 = tri.B - tri.A;
			const glm::vec3 e2 = tri.C - tri.A;
			const glm::vec3 norm = glm::normalize(glm::cross(e1, e2));

			for (int k = 0; k < 3; ++k)
			{
				int gx = std::clamp(static_cast<int>(std::floor((pts[k].x - bMin.x) / voxelSize)), 0, static_cast<int>(numX - 1));
				int gz = std::clamp(static_cast<int>(std::floor((pts[k].z - bMin.z) / voxelSize)), 0, static_cast<int>(numZ - 1));
				auto& spans = cellSpans[gz * numX + gx];
				if (!spans.empty())
					continue;
				spans.push_back({ pts[k].y, norm });
			}
		}

		// 4. 生成 3D 凸多边形 (Quad) 并焊合共面共享顶点 (64 位空间哈希，消除 std::tuple map 分配)
		auto quantCoord = [](float v) -> int64_t {
			return static_cast<int64_t>(std::round(v * 100.0f)); // 1cm 精度 (工业标准足以焊合多边形)
		};
		struct VertCoord {
			int64_t X, Y, Z;
			bool operator==(const VertCoord& o) const noexcept {
				return X == o.X && Y == o.Y && Z == o.Z;
			}
		};
		struct VertCoordHash {
			size_t operator()(const VertCoord& c) const noexcept {
				uint64_t h = (static_cast<uint64_t>(c.X) * 73856093ULL) ^
				             (static_cast<uint64_t>(c.Y) * 19349663ULL) ^
				             (static_cast<uint64_t>(c.Z) * 83492791ULL);
				h ^= h >> 33;
				h *= 0xff51afd7ed558ccdULL;
				h ^= h >> 33;
				return static_cast<size_t>(h);
			}
		};
		std::unordered_map<VertCoord, uint32_t, VertCoordHash> vertMap;
		vertMap.reserve(numX * numZ * 2);

		auto getOrAddVertex = [&](const glm::vec3& pos) -> uint32_t {
			const VertCoord coord { quantCoord(pos.x), quantCoord(pos.y), quantCoord(pos.z) };
			auto it = vertMap.find(coord);
			if (it != vertMap.end())
				return it->second;
			uint32_t idx = AddVertex(pos);
			vertMap[coord] = idx;
			return idx;
		};

		// 先将多层 Span 按高度升序整理，并预计算每个 Span 的四角共点焊合高度与 AreaModifier 属性
		struct GridSpanPoly
		{
			float Height = 0.0f;
			float Clearance = 0.0f;
			float CornerY[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
			uint8_t AreaType = 0;
			float Cost = 1.0f;
			bool Blocked = false;
			bool Consumed = false;
			uint32_t PolyIdx = UINT32_MAX;
		};
		std::vector<std::vector<GridSpanPoly>> gridCells(numX * numZ);

		const float maxSlopeClimb = voxelSize * std::tan(glm::radians(std::clamp(settings.MaxClimbSlopeDeg, 0.0f, 80.0f)));
		const float maxNeighborHeightDiff = settings.StepHeight + maxSlopeClimb + 0.08f;

		// 3.5 工业级代理半径边缘腐蚀与悬崖窄肋清理 (对标 Recast rcErodeWalkableArea):
		// 当地形格总数较大时，剔除陡崖立面上不足代理通行宽度的单格窄条/锯齿凸起以及悬崖中段孤立碎块
		if (numX >= 16 && numZ >= 16)
		{
			auto hasConnectedSpan = [&](int nx, int nz, float h) -> bool {
				if (nx < 0 || nx >= static_cast<int>(numX) || nz < 0 || nz >= static_cast<int>(numZ))
					return false;
				for (const auto& os : cellSpans[nz * numX + nx])
				{
					if (std::abs(os.Height - h) <= maxNeighborHeightDiff)
						return true;
				}
				return false;
			};

			for (int pass = 0; pass < 2; ++pass)
			{
				auto nextSpans = cellSpans;
				for (int gz = 0; gz < static_cast<int>(numZ); ++gz)
				{
					for (int gx = 0; gx < static_cast<int>(numX); ++gx)
					{
						auto& spList = nextSpans[gz * numX + gx];
						if (spList.empty()) continue;
						spList.erase(std::remove_if(spList.begin(), spList.end(), [&](const Span& sp) {
							// 仅对地面/山坡采样点做窄肋清理，保留高架桥等水平人造构件 (Normal.y > 0.995)
							if (sp.Normal.y > 0.985f && sp.Height > 1.8f)
								return false;
							const bool hasW = hasConnectedSpan(gx - 1, gz, sp.Height);
							const bool hasE = hasConnectedSpan(gx + 1, gz, sp.Height);
							const bool hasS = hasConnectedSpan(gx, gz - 1, sp.Height);
							const bool hasN = hasConnectedSpan(gx, gz + 1, sp.Height);
							const int count = (hasW ? 1 : 0) + (hasE ? 1 : 0) + (hasS ? 1 : 0) + (hasN ? 1 : 0);
							// 若单格在东西向或南北向完全悬空 (如 54° 陡崖上的 1 格宽横肋条)，或四邻不足 2 个，予以腐蚀剔除
							if (count <= 1)
								return true;
							if (sp.Normal.y < 0.95f && (!(hasW || hasE) || !(hasS || hasN)))
								return true;
							if (sp.Normal.y < 0.92f && count < 3)
								return true;
							return false;
						}), spList.end());
					}
				}
				cellSpans = std::move(nextSpans);
			}
		}
		const float layerTol = std::max(settings.StepHeight + voxelSize * 1.1f, 1.35f);

		auto getCornerY = [&](int cornerGX, int cornerGZ, float px, float pz, float refHeight) -> float {
			float sumY = 0.0f;
			int count = 0;
			const int nxOffsets[4] = { cornerGX - 1, cornerGX, cornerGX - 1, cornerGX };
			const int nzOffsets[4] = { cornerGZ - 1, cornerGZ - 1, cornerGZ, cornerGZ };
			for (int i = 0; i < 4; ++i)
			{
				const int nx = nxOffsets[i];
				const int nz = nzOffsets[i];
				if (nx >= 0 && nx < static_cast<int>(numX) && nz >= 0 && nz < static_cast<int>(numZ))
				{
					const auto& nspans = cellSpans[nz * numX + nx];
					const float ncx = bMin.x + (static_cast<float>(nx) + 0.5f) * voxelSize;
					const float ncz = bMin.z + (static_cast<float>(nz) + 0.5f) * voxelSize;
					for (const auto& os : nspans)
					{
						if (std::abs(os.Height - refHeight) <= layerTol)
						{
							float y = os.Height;
							if (std::abs(os.Normal.y) >= 1e-4f)
								y -= (os.Normal.x * (px - ncx) + os.Normal.z * (pz - ncz)) / os.Normal.y;
							sumY += y;
							count++;
							break;
						}
					}
				}
			}
			return (count > 0) ? (sumY / static_cast<float>(count)) : refHeight;
		};

		for (uint32_t gz = 0; gz < numZ; ++gz)
		{
			for (uint32_t gx = 0; gx < numX; ++gx)
			{
				auto& spans = cellSpans[gz * numX + gx];
				if (spans.empty())
					continue;
				std::sort(spans.begin(), spans.end(), [](const Span& a, const Span& b) {
					return a.Height < b.Height;
				});

				const float x0 = bMin.x + static_cast<float>(gx) * voxelSize;
				const float x1 = x0 + voxelSize;
				const float z0 = bMin.z + static_cast<float>(gz) * voxelSize;
				const float z1 = z0 + voxelSize;
				const float cx = (x0 + x1) * 0.5f;
				const float cz = (z0 + z1) * 0.5f;

				auto& dstList = gridCells[gz * numX + gx];
				dstList.reserve(spans.size());
				for (size_t k = 0; k < spans.size(); ++k)
				{
					const auto& span = spans[k];
					GridSpanPoly gsp;
					gsp.Height = span.Height;
					if (k + 1 < spans.size())
					{
						const float diff = spans[k + 1].Height - span.Height;
						if (diff >= std::max(settings.AgentHeight, 1.85f) && diff <= 6.0f)
							gsp.Clearance = diff;
					}
					gsp.CornerY[0] = getCornerY(static_cast<int>(gx), static_cast<int>(gz), x0, z0, span.Height);
					gsp.CornerY[1] = getCornerY(static_cast<int>(gx + 1), static_cast<int>(gz), x1, z0, span.Height);
					gsp.CornerY[2] = getCornerY(static_cast<int>(gx + 1), static_cast<int>(gz + 1), x1, z1, span.Height);
					gsp.CornerY[3] = getCornerY(static_cast<int>(gx), static_cast<int>(gz + 1), x0, z1, span.Height);

					const glm::vec3 cellCenter(cx, (gsp.CornerY[0] + gsp.CornerY[1] + gsp.CornerY[2] + gsp.CornerY[3]) * 0.25f, cz);
					for (const auto& mod : m_AreaModifiers)
					{
						if (mod.ContainsPoint(cellCenter))
						{
							gsp.AreaType = mod.AreaType;
							gsp.Cost = mod.CostMultiplier;
							if (mod.ClearanceHeight > 0.0f)
							{
								gsp.Clearance = mod.ClearanceHeight;
							}
							if (mod.AreaType == 4 || mod.CostMultiplier <= 0.0f)
								gsp.Blocked = true;
						}
					}
					dstList.push_back(gsp);
				}
			}
		}

		// 5. 自适应共面贪心网格合并 (Coplanar Greedy Quad Merging):
		// 复杂弯曲地形 (高差偏离双线性平面 > 3.5cm) 保持 1x1 高精度细网格紧贴地貌；
		// 平坦或均匀斜坡区域自动合并为最大 6x6 的大四边形，将多边形数量降低 75%~90%，大幅提升 Debug 渲染帧率！
		constexpr uint32_t kMaxMergeSpan = 6u;
		constexpr float kCoplanarEps = 0.035f;

		auto findMatchingSpan = [&](uint32_t x, uint32_t z, const GridSpanPoly& ref, float expectedH) -> GridSpanPoly* {
			auto& list = gridCells[z * numX + x];
			for (auto& cand : list)
			{
				if (cand.Consumed)
					continue;
				if (cand.AreaType != ref.AreaType || cand.Blocked != ref.Blocked ||
					std::abs(cand.Cost - ref.Cost) > 1e-3f || std::abs(cand.Clearance - ref.Clearance) > 0.05f)
					continue;
				if (std::abs(cand.Height - expectedH) <= maxNeighborHeightDiff)
					return &cand;
			}
			return nullptr;
		};

		auto isRectCoplanar = [&](uint32_t gx0, uint32_t gz0, uint32_t wCells, uint32_t hCells, const GridSpanPoly& baseSpan) -> bool {
			if (wCells == 1 && hCells == 1)
				return true;
			const auto* s00 = &baseSpan;
			const auto* s10 = findMatchingSpan(gx0 + wCells - 1, gz0, baseSpan, baseSpan.Height);
			const auto* s11 = findMatchingSpan(gx0 + wCells - 1, gz0 + hCells - 1, baseSpan, baseSpan.Height);
			const auto* s01 = findMatchingSpan(gx0, gz0 + hCells - 1, baseSpan, baseSpan.Height);
			if (!s10 || !s11 || !s01)
				return false;

			const float y00 = s00->CornerY[0];
			const float y10 = s10->CornerY[1];
			const float y11 = s11->CornerY[2];
			const float y01 = s01->CornerY[3];

			// 检验矩形内部所有单元格的四角是否全部共面 (误差 <= kCoplanarEps)
			for (uint32_t dz = 0; dz < hCells; ++dz)
			{
				float prevH = baseSpan.Height;
				for (uint32_t dx = 0; dx < wCells; ++dx)
				{
					const GridSpanPoly* cell = (dx == 0 && dz == 0)
						? &baseSpan
						: findMatchingSpan(gx0 + dx, gz0 + dz, baseSpan, prevH);
					if (!cell)
						return false;
					prevH = cell->Height;

					const float u0 = static_cast<float>(dx) / static_cast<float>(wCells);
					const float u1 = static_cast<float>(dx + 1) / static_cast<float>(wCells);
					const float v0 = static_cast<float>(dz) / static_cast<float>(hCells);
					const float v1 = static_cast<float>(dz + 1) / static_cast<float>(hCells);

					auto bilerp = [&](float u, float v) -> float {
						const float top = y00 * (1.0f - u) + y10 * u;
						const float bot = y01 * (1.0f - u) + y11 * u;
						return top * (1.0f - v) + bot * v;
					};

					if (std::abs(cell->CornerY[0] - bilerp(u0, v0)) > kCoplanarEps ||
						std::abs(cell->CornerY[1] - bilerp(u1, v0)) > kCoplanarEps ||
						std::abs(cell->CornerY[2] - bilerp(u1, v1)) > kCoplanarEps ||
						std::abs(cell->CornerY[3] - bilerp(u0, v1)) > kCoplanarEps)
					{
						return false;
					}
				}
			}
			return true;
		};

		for (uint32_t gz = 0; gz < numZ; ++gz)
		{
			for (uint32_t gx = 0; gx < numX; ++gx)
			{
				auto& spans = gridCells[gz * numX + gx];
				for (size_t k = 0; k < spans.size(); ++k)
				{
					auto& baseSpan = spans[k];
					if (baseSpan.Consumed)
						continue;

					uint32_t bestW = 1u;
					uint32_t bestH = 1u;
					while (bestW < kMaxMergeSpan && gx + bestW < numX && isRectCoplanar(gx, gz, bestW + 1, bestH, baseSpan))
						++bestW;
					while (bestH < kMaxMergeSpan && gz + bestH < numZ && isRectCoplanar(gx, gz, bestW, bestH + 1, baseSpan))
						++bestH;

					const auto* s00 = &baseSpan;
					const auto* s10 = (bestW == 1) ? &baseSpan : findMatchingSpan(gx + bestW - 1, gz, baseSpan, baseSpan.Height);
					const auto* s11 = (bestW == 1 && bestH == 1) ? &baseSpan : findMatchingSpan(gx + bestW - 1, gz + bestH - 1, baseSpan, baseSpan.Height);
					const auto* s01 = (bestH == 1) ? &baseSpan : findMatchingSpan(gx, gz + bestH - 1, baseSpan, baseSpan.Height);

					const float x0 = bMin.x + static_cast<float>(gx) * voxelSize;
					const float x1 = bMin.x + static_cast<float>(gx + bestW) * voxelSize;
					const float z0 = bMin.z + static_cast<float>(gz) * voxelSize;
					const float z1 = bMin.z + static_cast<float>(gz + bestH) * voxelSize;

					const glm::vec3 c0(x0, s00->CornerY[0], z0);
					const glm::vec3 c1(x1, s10->CornerY[1], z0);
					const glm::vec3 c2(x1, s11->CornerY[2], z1);
					const glm::vec3 c3(x0, s01->CornerY[3], z1);

					const uint32_t v0 = getOrAddVertex(c0);
					const uint32_t v1 = getOrAddVertex(c1);
					const uint32_t v2 = getOrAddVertex(c2);
					const uint32_t v3 = getOrAddVertex(c3);

					const uint32_t pIdx = AddPolygon({ v0, v1, v2, v3 });
					auto& poly = m_Polygons[pIdx];
					poly.AreaType = baseSpan.AreaType;
					poly.Cost = baseSpan.Cost;
					poly.Blocked = baseSpan.Blocked;
					poly.Clearance = baseSpan.Clearance;
					poly.BaseAreaType = baseSpan.AreaType;
					poly.BaseCost = baseSpan.Cost;
					poly.BaseClearance = baseSpan.Clearance;

					for (uint32_t dz = 0; dz < bestH; ++dz)
					{
						float prevH = baseSpan.Height;
						for (uint32_t dx = 0; dx < bestW; ++dx)
						{
							GridSpanPoly* cell = (dx == 0 && dz == 0)
								? &baseSpan
								: findMatchingSpan(gx + dx, gz + dz, baseSpan, prevH);
							if (cell)
							{
								prevH = cell->Height;
								cell->Consumed = true;
								cell->PolyIdx = pIdx;
							}
						}
					}
				}
			}
		}

		// 6. 建立合并后多边形的四边主邻接 (Neighbors[0..3]) 与扩展多邻接 (Neighbors[4..])
		auto linkPolysOnEdge = [&](uint32_t pA, int edgeA, uint32_t pB, int edgeB) {
			if (pA == pB || pA >= m_Polygons.size() || pB >= m_Polygons.size())
				return;
			auto& nbrsA = m_Polygons[pA].Neighbors;
			if (nbrsA[edgeA] == -1)
				nbrsA[edgeA] = static_cast<int32_t>(pB);
			else if (nbrsA[edgeA] != static_cast<int32_t>(pB) &&
			         std::find(nbrsA.begin(), nbrsA.end(), static_cast<int32_t>(pB)) == nbrsA.end())
				nbrsA.push_back(static_cast<int32_t>(pB));

			auto& nbrsB = m_Polygons[pB].Neighbors;
			if (nbrsB[edgeB] == -1)
				nbrsB[edgeB] = static_cast<int32_t>(pA);
			else if (nbrsB[edgeB] != static_cast<int32_t>(pA) &&
			         std::find(nbrsB.begin(), nbrsB.end(), static_cast<int32_t>(pA)) == nbrsB.end())
				nbrsB.push_back(static_cast<int32_t>(pA));
		};

		for (uint32_t gz = 0; gz < numZ; ++gz)
		{
			for (uint32_t gx = 0; gx < numX; ++gx)
			{
				const auto& spansA = gridCells[gz * numX + gx];
				if (spansA.empty()) continue;

				if (gx + 1 < numX)
				{
					const auto& spansEast = gridCells[gz * numX + (gx + 1)];
					for (const auto& pa : spansA)
					{
						if (pa.PolyIdx == UINT32_MAX) continue;
						for (const auto& pb : spansEast)
						{
							if (pb.PolyIdx == UINT32_MAX) continue;
							if (std::abs(pa.Height - pb.Height) <= maxNeighborHeightDiff)
							{
								linkPolysOnEdge(pa.PolyIdx, 1, pb.PolyIdx, 3);
								break;
							}
						}
					}
				}

				if (gz + 1 < numZ)
				{
					const auto& spansNorth = gridCells[(gz + 1) * numX + gx];
					for (const auto& pa : spansA)
					{
						if (pa.PolyIdx == UINT32_MAX) continue;
						for (const auto& pb : spansNorth)
						{
							if (pb.PolyIdx == UINT32_MAX) continue;
							if (std::abs(pa.Height - pb.Height) <= maxNeighborHeightDiff)
							{
								linkPolysOnEdge(pa.PolyIdx, 2, pb.PolyIdx, 0);
								break;
							}
						}
					}
				}
			}
		}

		// 6. 应用区域代价修饰区 (Area Modifiers)
		for (auto& poly : m_Polygons)
		{
			for (const auto& mod : m_AreaModifiers)
			{
				if (mod.ContainsPoint(poly.Center))
				{
					poly.AreaType = mod.AreaType;
					poly.Cost = mod.CostMultiplier;
					if (mod.ClearanceHeight > 0.0f)
					{
						poly.Clearance = mod.ClearanceHeight;
					}
					if (mod.AreaType == 4 || mod.CostMultiplier <= 0.0f)
					{
						poly.Blocked = true;
					}
				}
			}
		}

		// 7. 注入跨网格跳跃链接 (Off-Mesh Links)
		for (const auto& link : m_OffMeshLinks)
		{
			glm::vec3 navS, navE;
			uint32_t polyS = 0, polyE = 0;
			if (SamplePosition(link.Start, 5.0f, navS, polyS) &&
				SamplePosition(link.End, 5.0f, navE, polyE) &&
				polyS != polyE)
			{
				auto& nbrsS = m_Polygons[polyS].Neighbors;
				if (std::find(nbrsS.begin(), nbrsS.end(), static_cast<int32_t>(polyE)) == nbrsS.end())
				{
					nbrsS.push_back(static_cast<int32_t>(polyE));
				}

				if (link.BiDirectional)
				{
					auto& nbrsE = m_Polygons[polyE].Neighbors;
					if (std::find(nbrsE.begin(), nbrsE.end(), static_cast<int32_t>(polyS)) == nbrsE.end())
					{
						nbrsE.push_back(static_cast<int32_t>(polyS));
					}
				}
			}
		}
		BuildMacroTiles(m_MacroTileSize);
	}

	void NavMesh::ExtractSceneGeometry(const Scene& scene, const NavMeshBakeSettings& settings,
	                                   std::vector<NavMeshTriangle>& outTriangles)
	{
		outTriangles.clear();
		outTriangles.reserve(4096);
		auto& triangles = outTriangles;

		const auto& registry = scene.GetRegistry();

		// 1. WhiteboxComponent 原型几何体
		if (settings.IncludeWhitebox)
		{
			for (auto entity : registry.view<WhiteboxComponent>())
			{
				const auto& wb = registry.get<WhiteboxComponent>(entity);
				glm::mat4 worldMat(1.0f);
				if (const auto* wt = registry.try_get<WorldTransformComponent>(entity))
					worldMat = wt->Matrix;
				else if (const auto* tr = registry.try_get<TransformComponent>(entity))
					worldMat = tr->GetLocalMatrix();

				WhiteboxMesh mesh;
				switch (wb.Shape)
				{
				case WhiteboxComponent::ShapeType::Box:
					WhiteboxBuilder::BuildBox(wb.Dimensions, wb.BevelSize, mesh);
					break;
				case WhiteboxComponent::ShapeType::Cylinder:
					WhiteboxBuilder::BuildCylinder(wb.Dimensions.x * 0.5f, wb.Dimensions.y, std::max(6u, static_cast<uint32_t>(wb.Subdivisions * 8)), mesh);
					break;
				case WhiteboxComponent::ShapeType::Stairs:
					WhiteboxBuilder::BuildStairs(wb.Dimensions, std::max(1u, static_cast<uint32_t>(wb.Steps)), mesh);
					break;
				case WhiteboxComponent::ShapeType::Wedge:
					WhiteboxBuilder::BuildWedge(wb.Dimensions, mesh);
					break;
				case WhiteboxComponent::ShapeType::Arch:
					WhiteboxBuilder::BuildArch(wb.Dimensions, wb.Dimensions.x * 0.25f, mesh);
					break;
				default:
					WhiteboxBuilder::BuildBox(wb.Dimensions, wb.BevelSize, mesh);
					break;
				}

				for (size_t i = 0; i + 2 < mesh.Indices.size(); i += 3)
				{
					glm::vec3 p0 = glm::vec3(worldMat * glm::vec4(mesh.Vertices[mesh.Indices[i]], 1.0f));
					glm::vec3 p1 = glm::vec3(worldMat * glm::vec4(mesh.Vertices[mesh.Indices[i + 1]], 1.0f));
					glm::vec3 p2 = glm::vec3(worldMat * glm::vec4(mesh.Vertices[mesh.Indices[i + 2]], 1.0f));
					triangles.push_back({ p0, p1, p2 });
				}
			}
		}

		// 2. BoxCollider3DComponent 碰撞盒体 (排除动态障碍物与动态代理，避免动态 Carving 物体被当作静态固体烘进地面)
		if (settings.IncludeColliders)
		{
			for (auto entity : registry.view<BoxCollider3DComponent>())
			{
				if (registry.try_get<NavMeshObstacleComponent>(entity) || registry.try_get<NavMeshAgentComponent>(entity))
					continue;
				const auto& box = registry.get<BoxCollider3DComponent>(entity);
				glm::mat4 worldMat(1.0f);
				if (const auto* wt = registry.try_get<WorldTransformComponent>(entity))
					worldMat = wt->Matrix;
				else if (const auto* tr = registry.try_get<TransformComponent>(entity))
					worldMat = tr->GetLocalMatrix();

				const glm::vec3 c = box.Offset;
				const glm::vec3 h = glm::abs(box.HalfExtents);
				const glm::vec3 corners[8] = {
					c + glm::vec3(-h.x, -h.y, -h.z),
					c + glm::vec3( h.x, -h.y, -h.z),
					c + glm::vec3( h.x,  h.y, -h.z),
					c + glm::vec3(-h.x,  h.y, -h.z),
					c + glm::vec3(-h.x, -h.y,  h.z),
					c + glm::vec3( h.x, -h.y,  h.z),
					c + glm::vec3( h.x,  h.y,  h.z),
					c + glm::vec3(-h.x,  h.y,  h.z),
				};
				glm::vec3 wCorners[8];
				for (int k = 0; k < 8; ++k)
					wCorners[k] = glm::vec3(worldMat * glm::vec4(corners[k], 1.0f));

				static const uint32_t boxIndices[36] = {
					4, 5, 6,  4, 6, 7, // +Z
					1, 0, 3,  1, 3, 2, // -Z
					3, 7, 6,  3, 6, 2, // +Y (顶面 - CCW 正法线向上)
					0, 1, 5,  0, 5, 4, // -Y (底面)
					5, 1, 2,  5, 2, 6, // +X
					0, 4, 7,  0, 7, 3  // -X
				};
				for (int i = 0; i < 36; i += 3)
				{
					triangles.push_back({ wCorners[boxIndices[i]], wCorners[boxIndices[i + 1]], wCorners[boxIndices[i + 2]] });
				}
			}
		}

		// 3. MeshCollider3DComponent 与静态网格 MeshRendererComponent
		if (settings.IncludeColliders || settings.IncludeMeshes)
		{
			for (auto entity : registry.view<MeshCollider3DComponent>())
			{
				if (registry.try_get<NavMeshObstacleComponent>(entity) || registry.try_get<NavMeshAgentComponent>(entity))
					continue;
				const auto& mc = registry.get<MeshCollider3DComponent>(entity);
				glm::mat4 worldMat(1.0f);
				if (const auto* wt = registry.try_get<WorldTransformComponent>(entity))
					worldMat = wt->Matrix;
				else if (const auto* tr = registry.try_get<TransformComponent>(entity))
					worldMat = tr->GetLocalMatrix();

				std::string meshPath;
				if (mc.Mesh.HasPath())
					meshPath = std::string(StringPool::Get().PathOf(mc.Mesh.Path));
				else if (const auto* mr = registry.try_get<MeshRendererComponent>(entity))
				{
					if (mr->Mesh.HasPath())
						meshPath = std::string(StringPool::Get().PathOf(mr->Mesh.Path));
				}

				if (!meshPath.empty())
				{
					Asset::WModelData model;
					std::string err;
					if (Asset::WModelIO::ReadFile(meshPath, model, &err))
					{
						for (size_t i = 0; i + 2 < model.Indices.size(); i += 3)
						{
							uint32_t i0 = model.Indices[i];
							uint32_t i1 = model.Indices[i + 1];
							uint32_t i2 = model.Indices[i + 2];
							if (i0 < model.Vertices.size() && i1 < model.Vertices.size() && i2 < model.Vertices.size())
							{
								glm::vec3 p0 = glm::vec3(worldMat * glm::vec4(model.Vertices[i0].Position, 1.0f));
								glm::vec3 p1 = glm::vec3(worldMat * glm::vec4(model.Vertices[i1].Position, 1.0f));
								glm::vec3 p2 = glm::vec3(worldMat * glm::vec4(model.Vertices[i2].Position, 1.0f));
								triangles.push_back({ p0, p1, p2 });
							}
						}
					}
				}
			}

			if (settings.IncludeMeshes)
			{
				for (auto entity : registry.view<MeshRendererComponent>())
				{
					if (registry.try_get<MeshCollider3DComponent>(entity) ||
						registry.try_get<NavMeshObstacleComponent>(entity) ||
						registry.try_get<NavMeshAgentComponent>(entity))
						continue;

					const auto& mr = registry.get<MeshRendererComponent>(entity);
					if (!mr.Mesh.HasPath())
						continue;

					glm::mat4 worldMat(1.0f);
					if (const auto* wt = registry.try_get<WorldTransformComponent>(entity))
						worldMat = wt->Matrix;
					else if (const auto* tr = registry.try_get<TransformComponent>(entity))
						worldMat = tr->GetLocalMatrix();

					const std::string meshPath = std::string(StringPool::Get().PathOf(mr.Mesh.Path));
					Asset::WModelData model;
					std::string err;
					if (Asset::WModelIO::ReadFile(meshPath, model, &err))
					{
						for (size_t i = 0; i + 2 < model.Indices.size(); i += 3)
						{
							uint32_t i0 = model.Indices[i];
							uint32_t i1 = model.Indices[i + 1];
							uint32_t i2 = model.Indices[i + 2];
							if (i0 < model.Vertices.size() && i1 < model.Vertices.size() && i2 < model.Vertices.size())
							{
								glm::vec3 p0 = glm::vec3(worldMat * glm::vec4(model.Vertices[i0].Position, 1.0f));
								glm::vec3 p1 = glm::vec3(worldMat * glm::vec4(model.Vertices[i1].Position, 1.0f));
								glm::vec3 p2 = glm::vec3(worldMat * glm::vec4(model.Vertices[i2].Position, 1.0f));
								triangles.push_back({ p0, p1, p2 });
							}
						}
					}
				}
			}
		}

		// 4. TerrainComponent 高度场地貌 (工业级自适应采样步长 Nav LOD)
		if (settings.IncludeTerrain)
		{
			for (auto entity : registry.view<TerrainComponent>())
			{
				const auto& tc = registry.get<TerrainComponent>(entity);
				glm::mat4 worldMat(1.0f);
				if (const auto* wt = registry.try_get<WorldTransformComponent>(entity))
					worldMat = wt->Matrix;
				else if (const auto* tr = registry.try_get<TransformComponent>(entity))
					worldMat = tr->GetLocalMatrix();

				auto terrainData = TerrainRenderer::GetOrCreateTerrainData(tc.TerrainAsset, tc.CellSize, tc.HeightScale);
				if (terrainData && terrainData->GetWidth() >= 2 && terrainData->GetHeight() >= 2)
				{
					const uint32_t w = terrainData->GetWidth();
					const uint32_t h = terrainData->GetHeight();
					const float cs = terrainData->GetCellSize();

					// 工业级自适应地貌步长：当 VoxelSize > cs 时按 NavMesh 体素分辨率对齐步长提取，
					// 三角形数量减少 4x~64x，彻底根治大世界地貌烘焙卡死
					const uint32_t step = (w <= 128 && h <= 128)
						? 1u
						: std::clamp(static_cast<uint32_t>(std::round(std::max(settings.VoxelSize, cs) / std::max(0.1f, cs))), 1u, 8u);

					for (uint32_t z = 0; z + step <= h - 1; z += step)
					{
						const uint32_t zNext = std::min(z + step, h - 1);
						for (uint32_t x = 0; x + step <= w - 1; x += step)
						{
							const uint32_t xNext = std::min(x + step, w - 1);

							if (tc.HoleCutoutEnabled && (terrainData->IsHole(x, z) || terrainData->IsHole(xNext, z) ||
							                             terrainData->IsHole(x, zNext) || terrainData->IsHole(xNext, zNext)))
								continue;

							const float wx0 = static_cast<float>(x) * cs;
							const float wx1 = static_cast<float>(xNext) * cs;
							const float wz0 = static_cast<float>(z) * cs;
							const float wz1 = static_cast<float>(zNext) * cs;

							const glm::vec3 p00(wx0, terrainData->SampleHeight(wx0, wz0), wz0);
							const glm::vec3 p10(wx1, terrainData->SampleHeight(wx1, wz0), wz0);
							const glm::vec3 p11(wx1, terrainData->SampleHeight(wx1, wz1), wz1);
							const glm::vec3 p01(wx0, terrainData->SampleHeight(wx0, wz1), wz1);

							const glm::vec3 wp00 = glm::vec3(worldMat * glm::vec4(p00, 1.0f));
							const glm::vec3 wp10 = glm::vec3(worldMat * glm::vec4(p10, 1.0f));
							const glm::vec3 wp11 = glm::vec3(worldMat * glm::vec4(p11, 1.0f));
							const glm::vec3 wp01 = glm::vec3(worldMat * glm::vec4(p01, 1.0f));


							// 整格双三角面 3D 法线一致性过滤：仅当同一地形格的两个剖分三角形均满足最大爬坡角时才整格纳入，既消除对角线半格锯齿，又完整保留 30°~45° 连续斜坡
							const glm::vec3 n1 = glm::cross(wp11 - wp00, wp10 - wp00);
							const glm::vec3 n2 = glm::cross(wp01 - wp00, wp11 - wp00);
							const float l1 = glm::length(n1);
							const float l2 = glm::length(n2);
							const float minCellCos = std::cos(glm::radians(std::clamp(settings.MaxClimbSlopeDeg, 0.0f, 89.0f)));
							if (l1 < 1e-5f || l2 < 1e-5f || (n1.y / l1) < minCellCos || (n2.y / l2) < minCellCos)
								continue;

							triangles.push_back({ wp00, wp11, wp10 });
							triangles.push_back({ wp00, wp01, wp11 });
						}
					}
				}
			}
		}
	}

	void NavMesh::BakeFromScene(const Scene& scene, const NavMeshBakeSettings& settings)
	{
		std::vector<NavMeshTriangle> triangles;
		ExtractSceneGeometry(scene, settings, triangles);
		BakeFromTriangles(triangles, settings);

		const auto& registry = scene.GetRegistry();
		for (auto [entity, linkComp] : registry.view<NavMeshOffMeshLinkComponent>().each())
		{
			NavMeshOffMeshLink link;
			if (const auto* xform = registry.try_get<TransformComponent>(entity))
				link.Start = xform->Location;
			link.End = linkComp.TargetLocation;
			link.Cost = linkComp.Cost;
			link.LinkType = linkComp.LinkType;
			link.BiDirectional = (linkComp.BiDirectional != 0);
			AddOffMeshLink(link);
		}
		for (auto [entity, areaComp] : registry.view<NavMeshAreaModifierComponent>().each())
		{
			NavMeshAreaModifier mod;
			glm::vec3 scale(1.0f);
			if (const auto* xform = registry.try_get<TransformComponent>(entity))
			{
				mod.Center = xform->Location;
				mod.Rotation = xform->Rotation;
				scale = glm::abs(xform->Scale);
			}
			mod.Extents = areaComp.Extents * scale;
			mod.CostMultiplier = areaComp.CostMultiplier;
			mod.AreaType = areaComp.AreaType;
			mod.ClearanceHeight = areaComp.ClearanceHeight;
			AddAreaModifier(mod);
		}
	}

	std::future<std::shared_ptr<NavMesh>> NavMesh::BakeSceneAsync(const Scene& scene, const NavMeshBakeSettings& settings)
	{
		std::vector<NavMeshTriangle> triangles;
		ExtractSceneGeometry(scene, settings, triangles);

		std::vector<NavMeshAreaModifier> areaMods;
		std::vector<NavMeshOffMeshLink> links;

		const auto& registry = scene.GetRegistry();
		for (auto [entity, linkComp] : registry.view<NavMeshOffMeshLinkComponent>().each())
		{
			NavMeshOffMeshLink link;
			if (const auto* xform = registry.try_get<TransformComponent>(entity))
				link.Start = xform->Location;
			link.End = linkComp.TargetLocation;
			link.Cost = linkComp.Cost;
			link.LinkType = linkComp.LinkType;
			link.BiDirectional = (linkComp.BiDirectional != 0);
			links.push_back(link);
		}
		// 关键修复：场景无实体跳跃点时才从现有 NavMesh 继承资产级跳跃点，杜绝每次重烘重复翻倍累加
		if (links.empty())
		{
			if (auto curNav = scene.GetNavMesh())
				links = curNav->GetOffMeshLinks();
		}

		for (auto [entity, areaComp] : registry.view<NavMeshAreaModifierComponent>().each())
		{
			NavMeshAreaModifier mod;
			glm::vec3 scale(1.0f);
			if (const auto* xform = registry.try_get<TransformComponent>(entity))
			{
				mod.Center = xform->Location;
				mod.Rotation = xform->Rotation;
				scale = glm::abs(xform->Scale);
			}
			mod.Extents = areaComp.Extents * scale;
			mod.CostMultiplier = areaComp.CostMultiplier;
			mod.AreaType = areaComp.AreaType;
			mod.ClearanceHeight = areaComp.ClearanceHeight;
			areaMods.push_back(mod);
		}
		// 关键修复：场景无实体代价区时才从现有 NavMesh 继承资产级代价区
		if (areaMods.empty())
		{
			if (auto curNav = scene.GetNavMesh())
				areaMods = curNav->GetAreaModifiers();
		}

		return std::async(std::launch::async, [triangles = std::move(triangles),
		                                       areaMods = std::move(areaMods),
		                                       links = std::move(links),
		                                       settings]() mutable {
			auto nav = std::make_shared<NavMesh>();
			nav->BakeFromTriangles(triangles, settings);
			for (const auto& mod : areaMods) nav->AddAreaModifier(mod);
			for (const auto& link : links) nav->AddOffMeshLink(link);
			nav->BuildMacroTiles(24.0f);
			return nav;
		});
	}

	bool NavMesh::Raycast(const glm::vec3& start, const glm::vec3& end, glm::vec3* outHitPoint) const
	{
		if (m_Polygons.empty())
		{
			if (outHitPoint) *outHitPoint = start;
			return false;
		}

		glm::vec3 navStart;
		uint32_t polyStart = 0;
		if (!SamplePosition(start, 5.0f, navStart, polyStart))
		{
			if (outHitPoint) *outHitPoint = start;
			return false;
		}

		const float totalDist = glm::distance(navStart, end);
		if (totalDist < 1e-4f)
			return true;

		const float stepSize = 0.25f;
		const int steps = std::max(1, static_cast<int>(std::ceil(totalDist / stepSize)));
		glm::vec3 prevValid = navStart;

		for (int i = 1; i <= steps; ++i)
		{
			const float t = static_cast<float>(i) / static_cast<float>(steps);
			const glm::vec3 probe = glm::mix(navStart, end, t);

			glm::vec3 navPt;
			uint32_t polyIdx = 0;
			if (!SamplePosition(probe, 1.5f, navPt, polyIdx))
			{
				if (outHitPoint) *outHitPoint = prevValid;
				return false;
			}

			if (m_Polygons[polyIdx].Blocked || m_Polygons[polyIdx].Cost <= 0.0f)
			{
				if (outHitPoint) *outHitPoint = prevValid;
				return false;
			}

			if (std::abs(probe.y - navPt.y) > 1.8f)
			{
				if (outHitPoint) *outHitPoint = prevValid;
				return false;
			}

			prevValid = navPt;
		}

		return true;
	}

	bool NavMesh::GetRandomPointAround(const glm::vec3& center, float radius, glm::vec3& outPoint) const
	{
		if (m_Polygons.empty() || radius <= 0.0f)
			return false;

		std::vector<uint32_t> candidates;
		candidates.reserve(32);
		const float maxDist2 = (radius + 10.0f) * (radius + 10.0f);

		for (uint32_t p = 0; p < m_Polygons.size(); ++p)
		{
			const auto& poly = m_Polygons[p];
			if (poly.Blocked || poly.Cost <= 0.0f)
				continue;
			const glm::vec3 diff = poly.Center - center;
			if (diff.x * diff.x + diff.z * diff.z <= maxDist2)
			{
				candidates.push_back(p);
			}
		}

		if (candidates.empty())
		{
			for (uint32_t p = 0; p < m_Polygons.size(); ++p)
			{
				if (!m_Polygons[p].Blocked && m_Polygons[p].Cost > 0.0f)
				{
					candidates.push_back(p);
					break;
				}
			}
		}

		if (candidates.empty())
			return false;

		for (int attempt = 0; attempt < 16; ++attempt)
		{
			const float angle = (static_cast<float>(std::rand()) / static_cast<float>(RAND_MAX)) * 6.2831853f;
			const float r = radius * std::sqrt(static_cast<float>(std::rand()) / static_cast<float>(RAND_MAX));
			const glm::vec3 testPt = center + glm::vec3(r * std::cos(angle), 0.0f, r * std::sin(angle));

			glm::vec3 navPt;
			uint32_t polyIdx = 0;
			if (SamplePosition(testPt, 4.0f, navPt, polyIdx))
			{
				if (polyIdx < m_Polygons.size() && !m_Polygons[polyIdx].Blocked && m_Polygons[polyIdx].Cost > 0.0f)
				{
					if (glm::distance(center, navPt) <= radius + 1.0f)
					{
						outPoint = navPt;
						return true;
					}
				}
			}
		}

		const uint32_t chosen = candidates[std::rand() % candidates.size()];
		outPoint = m_Polygons[chosen].Center;
		return true;
	}

	bool NavMesh::IsPointWalkable(const glm::vec3& point, float tolerance) const
	{
		glm::vec3 navPt;
		uint32_t polyIdx = 0;
		if (!SamplePosition(point, tolerance, navPt, polyIdx))
			return false;
		if (polyIdx >= m_Polygons.size())
			return false;
		const auto& poly = m_Polygons[polyIdx];
		return !poly.Blocked && poly.Cost > 0.0f;
	}

	bool NavMesh::GetNearestPoint(const glm::vec3& point, float maxDist, glm::vec3& outPoint) const
	{
		uint32_t polyIdx = 0;
		return SamplePosition(point, maxDist, outPoint, polyIdx);
	}

	void NavMesh::ApplyObstacle(const glm::vec3& boxCenter, const glm::vec3& boxExtents, bool carve)
	{
		const glm::vec3 bMin = boxCenter - boxExtents;
		const glm::vec3 bMax = boxCenter + boxExtents;

		for (auto& poly : m_Polygons)
		{
			float pMinX = poly.Center.x, pMaxX = poly.Center.x;
			float pMinZ = poly.Center.z, pMaxZ = poly.Center.z;
			for (uint32_t vi : poly.VertexIndices)
			{
				if (vi < m_Vertices.size())
				{
					pMinX = std::min(pMinX, m_Vertices[vi].x);
					pMaxX = std::max(pMaxX, m_Vertices[vi].x);
					pMinZ = std::min(pMinZ, m_Vertices[vi].z);
					pMaxZ = std::max(pMaxZ, m_Vertices[vi].z);
				}
			}

			const bool overlapX = (pMinX <= bMax.x + 0.15f) && (pMaxX >= bMin.x - 0.15f);
			const bool overlapZ = (pMinZ <= bMax.z + 0.15f) && (pMaxZ >= bMin.z - 0.15f);
			const bool overlapY = (poly.Center.y >= bMin.y - 0.35f) && (poly.Center.y <= bMax.y + 0.35f);

			if (overlapX && overlapZ && overlapY)
			{
				poly.Blocked = carve;
			}
		}
		BuildMacroTiles(m_MacroTileSize);
	}

	void NavMesh::ClearObstacles()
	{
		for (auto& poly : m_Polygons)
		{
			poly.Blocked = (poly.AreaType == 4 || poly.Cost <= 0.0f);
		}
		BuildMacroTiles(m_MacroTileSize);
	}

	void NavMesh::BuildMacroTiles(float tileSize)
	{
		m_MacroTiles.clear();
		if (m_Polygons.empty())
			return;

		if (tileSize <= 0.0f)
			tileSize = (m_MacroTileSize > 0.0f) ? m_MacroTileSize : 24.0f;
		m_MacroTileSize = tileSize;

		struct TileCoord
		{
			int gx;
			int gz;
			bool operator<(const TileCoord& other) const noexcept
			{
				if (gx != other.gx) return gx < other.gx;
				return gz < other.gz;
			}
		};

		std::map<TileCoord, std::vector<uint32_t>> tileGroups;
		for (uint32_t i = 0; i < static_cast<uint32_t>(m_Polygons.size()); ++i)
		{
			const auto& poly = m_Polygons[i];
			const int gx = static_cast<int>(std::floor(poly.Center.x / tileSize));
			const int gz = static_cast<int>(std::floor(poly.Center.z / tileSize));
			tileGroups[{gx, gz}].push_back(i);
		}
		m_MacroTiles.reserve(tileGroups.size());
		for (const auto& [coord, polyIndices] : tileGroups)
		{
			if (polyIndices.empty())
				continue;

			NavMeshMacroTile macroTile;
			macroTile.PolyCount = static_cast<uint32_t>(polyIndices.size());

			const float minX = static_cast<float>(coord.gx) * tileSize;
			const float maxX = minX + tileSize;
			const float minZ = static_cast<float>(coord.gz) * tileSize;
			const float maxZ = minZ + tileSize;

			float minY = std::numeric_limits<float>::max();
			float maxY = -std::numeric_limits<float>::max();
			double sumCenterY = 0.0;
			bool hasVerts = false;

			std::map<uint8_t, uint32_t> areaTypeCounts;
			bool allBlocked = true;

			for (uint32_t pIdx : polyIndices)
			{
				const auto& poly = m_Polygons[pIdx];
				if (!poly.Blocked)
					allBlocked = false;

				areaTypeCounts[poly.AreaType]++;
				sumCenterY += poly.Center.y;

				if (poly.VertexIndices.empty())
				{
					minY = std::min(minY, poly.Center.y);
					maxY = std::max(maxY, poly.Center.y);
					hasVerts = true;
				}
				else
				{
					for (uint32_t vIdx : poly.VertexIndices)
					{
						if (vIdx < m_Vertices.size())
						{
							const float vy = m_Vertices[vIdx].y;
							minY = std::min(minY, vy);
							maxY = std::max(maxY, vy);
							hasVerts = true;
						}
					}
				}
			}

			if (hasVerts)
			{
				macroTile.MinY = minY;
				macroTile.MaxY = maxY;
			}
			else
			{
				macroTile.MinY = 0.0f;
				macroTile.MaxY = 0.0f;
			}

			const float avgY = !polyIndices.empty()
				? static_cast<float>(sumCenterY / static_cast<double>(polyIndices.size()))
				: 0.0f;

			// 4 个角点高程自适应贴合（四角寻找最近多边形顶点高程，贴合地貌起伏与斜坡，消除悬空与切入）
			float cornerY[4] = { avgY, avgY, avgY, avgY };
			float cornerDistSq[4] = { 1e9f, 1e9f, 1e9f, 1e9f };
			const glm::vec2 cornerXZ[4] = {
				{ minX, minZ }, { maxX, minZ }, { maxX, maxZ }, { minX, maxZ }
			};

			for (uint32_t pIdx : polyIndices)
			{
				const auto& poly = m_Polygons[pIdx];
				for (uint32_t vIdx : poly.VertexIndices)
				{
					if (vIdx < m_Vertices.size())
					{
						const auto& v = m_Vertices[vIdx];
						for (int c = 0; c < 4; ++c)
						{
							const float dx = v.x - cornerXZ[c].x;
							const float dz = v.z - cornerXZ[c].y;
							const float d2 = dx * dx + dz * dz;
							if (d2 < cornerDistSq[c])
							{
								cornerDistSq[c] = d2;
								cornerY[c] = v.y;
							}
						}
					}
				}
			}

			macroTile.Corners[0] = glm::vec3(minX, cornerY[0], minZ);
			macroTile.Corners[1] = glm::vec3(maxX, cornerY[1], minZ);
			macroTile.Corners[2] = glm::vec3(maxX, cornerY[2], maxZ);
			macroTile.Corners[3] = glm::vec3(minX, cornerY[3], maxZ);

			// 区域中心点
			macroTile.Center = glm::vec3(minX + tileSize * 0.5f, avgY, minZ + tileSize * 0.5f);

			// 优势区域类型 (DominantAreaType)
			uint8_t dominantArea = 0;
			uint32_t maxCount = 0;
			uint8_t maxSpecialArea = 0;
			uint32_t maxSpecialCount = 0;
			for (const auto& [area, count] : areaTypeCounts)
			{
				if (count > maxCount)
				{
					maxCount = count;
					dominantArea = area;
				}
				if (area != 0 && count > maxSpecialCount)
				{
					maxSpecialCount = count;
					maxSpecialArea = area;
				}
			}
			// 若有特征区域（道路/沼泽/危险区）占比显著（>= 6 个多边形或 >= 15%），宏观瓦片优先呈现该特征色彩
			if (maxSpecialCount >= 6 || (maxSpecialCount * 6 >= polyIndices.size() && maxSpecialCount > 0))
			{
				dominantArea = maxSpecialArea;
			}
			macroTile.DominantAreaType = dominantArea;
			macroTile.Blocked = allBlocked;
			macroTile.PolygonIndices = polyIndices;

			m_MacroTiles.push_back(std::move(macroTile));
		}
	}

	bool NavMesh::SaveToFile(const std::string& path) const
	{
		std::ofstream stream(path, std::ios::binary);
		if (!stream.is_open())
			return false;

		const uint32_t vCount = static_cast<uint32_t>(m_Vertices.size());
		stream.write(reinterpret_cast<const char*>(&vCount), sizeof(vCount));
		if (vCount > 0)
			stream.write(reinterpret_cast<const char*>(m_Vertices.data()), vCount * sizeof(glm::vec3));

		const uint32_t pCount = static_cast<uint32_t>(m_Polygons.size());
		stream.write(reinterpret_cast<const char*>(&pCount), sizeof(pCount));
		for (const auto& poly : m_Polygons)
		{
			const uint32_t viCount = static_cast<uint32_t>(poly.VertexIndices.size());
			stream.write(reinterpret_cast<const char*>(&viCount), sizeof(viCount));
			stream.write(reinterpret_cast<const char*>(poly.VertexIndices.data()), viCount * sizeof(uint32_t));

			const uint32_t nbrCount = static_cast<uint32_t>(poly.Neighbors.size());
			stream.write(reinterpret_cast<const char*>(&nbrCount), sizeof(nbrCount));
			stream.write(reinterpret_cast<const char*>(poly.Neighbors.data()), nbrCount * sizeof(int32_t));
		}

		// v3 扩展区块：保存 AreaType、Cost、OffMeshLinks 与 AreaModifiers (支持 OBB 定向包围盒)
		const uint32_t extMagic = 0x4E415633; // "NAV3"
		stream.write(reinterpret_cast<const char*>(&extMagic), sizeof(extMagic));

		for (const auto& poly : m_Polygons)
		{
			stream.write(reinterpret_cast<const char*>(&poly.AreaType), sizeof(poly.AreaType));
			stream.write(reinterpret_cast<const char*>(&poly.Cost), sizeof(poly.Cost));
		}

		const uint32_t linkCount = static_cast<uint32_t>(m_OffMeshLinks.size());
		stream.write(reinterpret_cast<const char*>(&linkCount), sizeof(linkCount));
		if (linkCount > 0)
		{
			stream.write(reinterpret_cast<const char*>(m_OffMeshLinks.data()), linkCount * sizeof(NavMeshOffMeshLink));
		}

		const uint32_t modCount = static_cast<uint32_t>(m_AreaModifiers.size());
		stream.write(reinterpret_cast<const char*>(&modCount), sizeof(modCount));
		if (modCount > 0)
		{
			stream.write(reinterpret_cast<const char*>(m_AreaModifiers.data()), modCount * sizeof(NavMeshAreaModifier));
		}

		return true;
	}

	bool NavMesh::LoadFromFile(const std::string& path)
	{
		std::ifstream stream(path, std::ios::binary);
		if (!stream.is_open())
			return false;

		Clear();
		uint32_t vCount = 0;
		stream.read(reinterpret_cast<char*>(&vCount), sizeof(vCount));
		m_Vertices.resize(vCount);
		if (vCount > 0)
			stream.read(reinterpret_cast<char*>(m_Vertices.data()), vCount * sizeof(glm::vec3));

		uint32_t pCount = 0;
		stream.read(reinterpret_cast<char*>(&pCount), sizeof(pCount));
		m_Polygons.resize(pCount);
		for (uint32_t i = 0; i < pCount; ++i)
		{
			auto& poly = m_Polygons[i];
			uint32_t viCount = 0;
			stream.read(reinterpret_cast<char*>(&viCount), sizeof(viCount));
			poly.VertexIndices.resize(viCount);
			stream.read(reinterpret_cast<char*>(poly.VertexIndices.data()), viCount * sizeof(uint32_t));

			uint32_t nbrCount = 0;
			stream.read(reinterpret_cast<char*>(&nbrCount), sizeof(nbrCount));
			poly.Neighbors.resize(nbrCount);
			stream.read(reinterpret_cast<char*>(poly.Neighbors.data()), nbrCount * sizeof(int32_t));

			poly.AreaType = 0;
			poly.Cost = 1.0f;
			poly.Blocked = false;
			poly.Clearance = 0.0f;
			poly.BaseAreaType = 0;
			poly.BaseCost = 1.0f;
			poly.BaseClearance = 0.0f;

			glm::vec3 center(0.0f);
			for (uint32_t idx : poly.VertexIndices)
				center += m_Vertices[idx];
			if (!poly.VertexIndices.empty())
				center /= static_cast<float>(poly.VertexIndices.size());
			poly.Center = center;

			if (poly.VertexIndices.size() >= 3)
			{
				const glm::vec3& v0 = m_Vertices[poly.VertexIndices[0]];
				const glm::vec3& v1 = m_Vertices[poly.VertexIndices[1]];
				const glm::vec3& v2 = m_Vertices[poly.VertexIndices[2]];
				poly.Normal = glm::normalize(glm::cross(v1 - v0, v2 - v0));
			}
		}

		// 尝试读取 v2 / v3 扩展元数据 (NAV2: 40B 无旋转兼容读取, NAV3: 56B 含 OBB Rotation)
		uint32_t extMagic = 0;
		if (stream.read(reinterpret_cast<char*>(&extMagic), sizeof(extMagic)) &&
		    (extMagic == 0x4E415632 || extMagic == 0x4E415633))
		{
			for (auto& poly : m_Polygons)
			{
				stream.read(reinterpret_cast<char*>(&poly.AreaType), sizeof(poly.AreaType));
				stream.read(reinterpret_cast<char*>(&poly.Cost), sizeof(poly.Cost));
			}

			uint32_t linkCount = 0;
			if (stream.read(reinterpret_cast<char*>(&linkCount), sizeof(linkCount)) && linkCount > 0)
			{
				m_OffMeshLinks.resize(linkCount);
				stream.read(reinterpret_cast<char*>(m_OffMeshLinks.data()), linkCount * sizeof(NavMeshOffMeshLink));

				// 建立跳跃点起始与目标多边形的相邻连接，使得 A* 能够跨越跳跃点进行导航寻路
				for (const auto& link : m_OffMeshLinks)
				{
					glm::vec3 sPt, ePt;
					uint32_t pStart = 0, pEnd = 0;
					if (SamplePosition(link.Start, 5.0f, sPt, pStart) &&
						SamplePosition(link.End, 5.0f, ePt, pEnd) &&
						pStart != pEnd)
					{
						auto& nbrsS = m_Polygons[pStart].Neighbors;
						if (std::find(nbrsS.begin(), nbrsS.end(), static_cast<int32_t>(pEnd)) == nbrsS.end())
						{
							nbrsS.push_back(static_cast<int32_t>(pEnd));
						}
						if (link.BiDirectional)
						{
							auto& nbrsE = m_Polygons[pEnd].Neighbors;
							if (std::find(nbrsE.begin(), nbrsE.end(), static_cast<int32_t>(pStart)) == nbrsE.end())
							{
								nbrsE.push_back(static_cast<int32_t>(pStart));
							}
						}
					}
				}
			}

			uint32_t modCount = 0;
			if (stream.read(reinterpret_cast<char*>(&modCount), sizeof(modCount)) && modCount > 0)
			{
				m_AreaModifiers.resize(modCount);
				if (extMagic == 0x4E415633)
				{
					stream.read(reinterpret_cast<char*>(m_AreaModifiers.data()), modCount * sizeof(NavMeshAreaModifier));
				}
				else // 0x4E415632 ("NAV2")
				{
					struct NavMeshAreaModifierV2
					{
						glm::vec3 Center { 0.0f };
						glm::vec3 Extents { 1.0f };
						float CostMultiplier = 1.0f;
						float ClearanceHeight = 0.0f;
						uint8_t AreaType = 0;
						uint8_t _Padding[7] { 0 };
					};
					static_assert(sizeof(NavMeshAreaModifierV2) == 40, "NavMeshAreaModifierV2 must be 40 bytes");

					for (uint32_t i = 0; i < modCount; ++i)
					{
						NavMeshAreaModifierV2 v2;
						stream.read(reinterpret_cast<char*>(&v2), sizeof(NavMeshAreaModifierV2));
						m_AreaModifiers[i].Center = v2.Center;
						m_AreaModifiers[i].Extents = v2.Extents;
						m_AreaModifiers[i].Rotation = glm::quat { 1.0f, 0.0f, 0.0f, 0.0f };
						m_AreaModifiers[i].CostMultiplier = v2.CostMultiplier;
						m_AreaModifiers[i].ClearanceHeight = v2.ClearanceHeight;
						m_AreaModifiers[i].AreaType = v2.AreaType;
						std::memset(m_AreaModifiers[i]._Padding, 0, sizeof(m_AreaModifiers[i]._Padding));
					}
				}

				for (const auto& mod : m_AreaModifiers)
				{
					if (mod.ClearanceHeight > 0.0f)
					{
						for (auto& poly : m_Polygons)
						{
							if (mod.ContainsPoint(poly.Center))
							{
								poly.Clearance = mod.ClearanceHeight;
							}
						}
					}
				}
			}
		}

		BuildMacroTiles(m_MacroTileSize);
		return true;
	}

	void NavMeshSystem::Update(Scene& scene, float dt, const NavMesh& navMesh)
	{
		if (dt <= 0.0f || navMesh.GetPolygonCount() == 0)
			return;

		// 非结构写操作（仅修改组件数据）：走 const Scene 访问注册表，避免命中运行期结构写保护断言
		auto& registry = const_cast<entt::registry&>(std::as_const(scene).GetRegistry());

		// 运行期动态代价修饰区与限高同步（支持运行时移动限高实体即刻改变通行净空）
		std::vector<NavMeshAreaModifier> liveAreaModifiers;
		for (auto entity : registry.view<TransformComponent, NavMeshAreaModifierComponent>())
		{
			const auto& [xform, areaComp] = registry.get<TransformComponent, NavMeshAreaModifierComponent>(entity);
			NavMeshAreaModifier mod;
			mod.Center = xform.Location;
			mod.Rotation = xform.Rotation;
			mod.Extents = glm::abs(xform.Scale) * areaComp.Extents;
			mod.CostMultiplier = areaComp.CostMultiplier;
			mod.ClearanceHeight = areaComp.ClearanceHeight;
			mod.AreaType = areaComp.AreaType;
			liveAreaModifiers.push_back(mod);
		}
		if (!liveAreaModifiers.empty())
		{
			const_cast<NavMesh&>(navMesh).ReapplyAreaModifiers(liveAreaModifiers);
		}

		// 消除上一帧残留的动态阻挡标记并按当前障碍物雕刻
		const_cast<NavMesh&>(navMesh).ClearObstacles();

		// 遍历所有带 NavMeshObstacleComponent 且 CarveNavMesh == 1 的实体，调用 ApplyObstacle
		for (auto entity : registry.view<TransformComponent, NavMeshObstacleComponent>())
		{
			const auto& [xform, obs] = registry.get<TransformComponent, NavMeshObstacleComponent>(entity);
			if (obs.CarveNavMesh == 1)
			{
				const glm::vec3 worldExtents = glm::abs(xform.Scale) * obs.Extents;
				const_cast<NavMesh&>(navMesh).ApplyObstacle(xform.Location, worldExtents, true);
			}
		}

		// 遍历所有带 NavMeshAgentComponent 的实体
		for (auto entity : registry.view<TransformComponent, NavMeshAgentComponent>())
		{
			auto& [xform, agent] = registry.get<TransformComponent, NavMeshAgentComponent>(entity);

			// 计算到 agent.Destination 的水平/三维距离
			const float distToDest = glm::distance(xform.Location, agent.Destination);
			if (distToDest <= agent.StoppingDistance)
			{
				agent.ReachedDestination = 1;
				agent.HasPath = 0;
				continue;
			}

			// 若未到达，调用 navMesh.FindPath (传入当前代理体型高度与掩码)
			NavQueryFilter filter;
			filter.AgentHeight = agent.GetEffectiveHeight();
			filter.AgentRadius = agent.AgentRadius;
			filter.AreaMask = agent.AreaMask;

			std::vector<glm::vec3> path;
			if (navMesh.FindPath(xform.Location, agent.Destination, path, filter) && path.size() >= 2)
			{
				agent.HasPath = 1;
				agent.ReachedDestination = 0;

				// 计算向下一个航点 path[1] 的方向
				const glm::vec3 dir = path[1] - xform.Location;
				const float distDir = glm::length(dir);

				if (distDir > 1e-4f)
				{
					// 计算平面移动距离
					const float moveDist = std::min(distDir, agent.Speed * dt);
					xform.Location += (dir / distDir) * moveDist;

					// 平滑偏航角插值旋转
					if (dir.x * dir.x + dir.z * dir.z > 1e-4f)
					{
						const float targetYaw = std::atan2(dir.x, dir.z);
						const glm::quat targetRot = glm::angleAxis(targetYaw, glm::vec3(0.0f, 1.0f, 0.0f));
						xform.Rotation = glm::slerp(xform.Rotation, targetRot, std::min(1.0f, 10.0f * dt));
					}

					// 地貌/网格自适应贴地 (Grounding Clamp)
					glm::vec3 navPt { 0.0f };
					uint32_t polyIdx = 0;
					if (navMesh.SamplePosition(xform.Location, 4.0f, navPt, polyIdx))
					{
						xform.Location.y = navPt.y;
					}

					xform.Flags |= (TransformFlags::DirtyLocal | TransformFlags::DirtyWorld);
				}

				if (glm::distance(xform.Location, agent.Destination) <= agent.StoppingDistance)
				{
					agent.ReachedDestination = 1;
					agent.HasPath = 0;
				}
			}
			else
			{
				if (agent.AutoRepath == 1)
				{
					agent.HasPath = 0;
				}
			}
		}
	}
}
