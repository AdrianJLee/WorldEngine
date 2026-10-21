#include "wldpch.h"
#include "World/NavMesh/NavMesh.h"
#include "World/Scene/Scene.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <map>
#include <queue>
#include <unordered_map>

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

	void NavMesh::BuildAdjacency()
	{
		// 边映射: (min(v0, v1), max(v0, v1)) -> (polyIdx, edgeIdx)
		std::map<std::pair<uint32_t, uint32_t>, std::pair<uint32_t, uint32_t>> edgeMap;

		for (uint32_t p = 0; p < m_Polygons.size(); ++p)
		{
			auto& poly = m_Polygons[p];
			const size_t vCount = poly.VertexIndices.size();
			for (size_t e = 0; e < vCount; ++e)
			{
				const uint32_t i0 = poly.VertexIndices[e];
				const uint32_t i1 = poly.VertexIndices[(e + 1) % vCount];
				const auto key = std::make_pair(std::min(i0, i1), std::max(i0, i1));

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
	}

	bool NavMesh::SamplePosition(const glm::vec3& point, float maxDistance,
		glm::vec3& outNavPoint, uint32_t& outPolyIdx) const
	{
		float bestDist2 = maxDistance * maxDistance;
		bool found = false;

		for (uint32_t p = 0; p < m_Polygons.size(); ++p)
		{
			const auto& poly = m_Polygons[p];
			if (poly.Blocked)
				continue;

			std::vector<glm::vec3> polyVerts;
			polyVerts.reserve(poly.VertexIndices.size());
			for (uint32_t vi : poly.VertexIndices)
				polyVerts.push_back(m_Vertices[vi]);

			// 投影到多边形表面
			const glm::vec3 toPt = point - poly.Center;
			const float distToPlane = glm::dot(toPt, poly.Normal);
			const glm::vec3 projected = point - poly.Normal * distToPlane;

			if (IsPointInConvexPolygon(projected, polyVerts))
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
				// 测试各边的最近点
				for (size_t i = 0; i < polyVerts.size(); ++i)
				{
					const glm::vec3& a = polyVerts[i];
					const glm::vec3& b = polyVerts[(i + 1) % polyVerts.size()];
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

	bool NavMesh::FindPath(const glm::vec3& start, const glm::vec3& end,
		std::vector<glm::vec3>& outPath) const
	{
		outPath.clear();
		if (m_Polygons.empty())
			return false;

		glm::vec3 navStart, navEnd;
		uint32_t polyStart = 0, polyEnd = 0;
		if (!SamplePosition(start, 50.0f, navStart, polyStart) ||
			!SamplePosition(end, 50.0f, navEnd, polyEnd))
		{
			return false;
		}

		if (polyStart == polyEnd)
		{
			outPath = { navStart, navEnd };
			return true;
		}

		// 3D A* 走廊搜索
		struct AStarNode
		{
			uint32_t PolyIdx;
			float F;
			bool operator>(const AStarNode& other) const { return F > other.F; }
		};

		std::priority_queue<AStarNode, std::vector<AStarNode>, std::greater<AStarNode>> openSet;
		std::unordered_map<uint32_t, float> gScore;
		std::unordered_map<uint32_t, uint32_t> cameFrom;

		gScore[polyStart] = 0.0f;
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
				if (m_Polygons[neighbor].Blocked)
					continue;

				const float tentativeG = gScore[curr] + glm::distance(currPoly.Center, m_Polygons[neighbor].Center);
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

		// 提取相邻多边形间的公共边作为过境线门径 (Portals)
		std::vector<NavMeshPortal> portals;
		for (size_t i = 0; i + 1 < corridor.size(); ++i)
		{
			const auto& pA = m_Polygons[corridor[i]];
			const auto& pB = m_Polygons[corridor[i + 1]];

			// 寻找公共边端点
			std::vector<uint32_t> sharedVerts;
			for (uint32_t va : pA.VertexIndices)
			{
				for (uint32_t vb : pB.VertexIndices)
				{
					if (va == vb)
						sharedVerts.push_back(va);
				}
			}

			if (sharedVerts.size() >= 2)
			{
				NavMeshPortal portal;
				portal.Left  = m_Vertices[sharedVerts[0]];
				portal.Right = m_Vertices[sharedVerts[1]];
				// 保证在前进方向上 Left 在左侧
				const glm::vec3 forward = glm::normalize(pB.Center - pA.Center);
				if (TriArea2D(pA.Center, portal.Left, portal.Right) < 0.0f)
					std::swap(portal.Left, portal.Right);
				portals.push_back(portal);
			}
		}

		// 漏斗平滑
		FunnelSmooth(portals, navStart, navEnd, outPath);
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
					// 右边跨过了左边，拐点落于 portalLeft
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
					// 左边跨过了右边，拐点落于 portalRight
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

		BuildAdjacency();
	}

	void NavMesh::ApplyObstacle(const glm::vec3& boxCenter, const glm::vec3& boxExtents, bool carve)
	{
		const glm::vec3 bMin = boxCenter - boxExtents;
		const glm::vec3 bMax = boxCenter + boxExtents;

		for (auto& poly : m_Polygons)
		{
			if (poly.Center.x >= bMin.x && poly.Center.x <= bMax.x &&
				poly.Center.z >= bMin.z && poly.Center.z <= bMax.z)
			{
				poly.Blocked = carve;
			}
		}
	}

	void NavMesh::ClearObstacles()
	{
		for (auto& poly : m_Polygons)
			poly.Blocked = false;
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

			// 重新推导中心与法线
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
		return true;
	}

	void NavMeshSystem::Update(Scene& scene, float dt, const NavMesh& navMesh)
	{
		if (dt <= 0.0f || navMesh.GetPolygonCount() == 0)
			return;

		// 收集动态障碍物
		for (auto entity : scene.GetRegistry().view<TransformComponent, NavMeshObstacleComponent>())
		{
			const auto& [xform, obs] = scene.GetRegistry().get<TransformComponent, NavMeshObstacleComponent>(entity);
			if (obs.CarveNavMesh)
			{
				const_cast<NavMesh&>(navMesh).ApplyObstacle(xform.Location, obs.Extents, true);
			}
		}

		// 推进各代理寻路与运动
		for (auto entity : scene.GetRegistry().view<TransformComponent, NavMeshAgentComponent>())
		{
			auto& [xform, agent] = scene.GetRegistry().get<TransformComponent, NavMeshAgentComponent>(entity);

			const float distToDest = glm::distance(xform.Location, agent.Destination);
			if (distToDest <= agent.StoppingDistance)
			{
				agent.ReachedDestination = 1;
				agent.HasPath = 0;
				continue;
			}

			// 如果未规划路径或需要重新规划
			std::vector<glm::vec3> path;
			if (navMesh.FindPath(xform.Location, agent.Destination, path) && path.size() >= 2)
			{
				agent.HasPath = 1;
				agent.ReachedDestination = 0;

				// 沿路径第一航点移动
				const glm::vec3 nextPoint = path[1];
				const glm::vec3 toNext = nextPoint - xform.Location;
				const float distNext = glm::length(toNext);

				if (distNext > 0.001f)
				{
					const glm::vec3 dir = toNext / distNext;
					const float moveDist = std::min(distNext, agent.Speed * dt);
					xform.Location += dir * moveDist;
				}
				if (glm::distance(xform.Location, agent.Destination) <= agent.StoppingDistance)
				{
					agent.ReachedDestination = 1;
					agent.HasPath = 0;
				}
			}
		}
	}
}


