#pragma once

#include "World/Core/Export.h"
#include "World/Terrain/Terrain.h"
#include "World/Scene/Components.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace World
{
	class Scene;

	struct NavMeshPolygon
	{
		std::vector<uint32_t> VertexIndices;
		std::vector<int32_t> Neighbors; // 邻接多边形下标，-1 表示边界外
		glm::vec3 Center { 0.0f };
		glm::vec3 Normal { 0.0f, 1.0f, 0.0f };
		float Area { 0.0f };
		bool Blocked { false }; // 动态障碍物打洞标记
	};

	struct NavMeshPortal
	{
		glm::vec3 Left { 0.0f };
		glm::vec3 Right { 0.0f };
	};

	class WLD_API NavMesh
	{
	public:
		NavMesh() = default;
		~NavMesh() = default;

		void Clear();

		uint32_t AddVertex(const glm::vec3& pos);
		uint32_t AddPolygon(const std::vector<uint32_t>& vertIndices);
		void BuildAdjacency();

		size_t GetVertexCount() const { return m_Vertices.size(); }
		size_t GetPolygonCount() const { return m_Polygons.size(); }
		const glm::vec3& GetVertex(uint32_t index) const { return m_Vertices[index]; }
		const NavMeshPolygon& GetPolygon(uint32_t index) const { return m_Polygons[index]; }
		NavMeshPolygon& GetPolygonMut(uint32_t index) { return m_Polygons[index]; }

		// 空间点正交投影至最近多边形表面
		bool SamplePosition(const glm::vec3& point, float maxDistance,
			glm::vec3& outNavPoint, uint32_t& outPolyIdx) const;

		// 3D A* 走廊搜索 + Funnel 漏斗平滑路径规划
		bool FindPath(const glm::vec3& start, const glm::vec3& end,
			std::vector<glm::vec3>& outPath) const;

		// 漏斗平滑算法 (String-Pulling): 将过境线门径收紧为最短折线
		static void FunnelSmooth(const std::vector<NavMeshPortal>& portals,
			const glm::vec3& start, const glm::vec3& end,
			std::vector<glm::vec3>& outPath);

		// 从地貌高度场自动烘焙生成可通行多边形网格
		void BakeFromTerrain(const TerrainData& terrain, float maxSlopeDeg = 45.0f, float stepSize = 4.0f);

		// 动态障碍物实时雕刻阻挡
		void ApplyObstacle(const glm::vec3& boxCenter, const glm::vec3& boxExtents, bool carve);
		void ClearObstacles();

		// 序列化
		bool SaveToFile(const std::string& path) const;
		bool LoadFromFile(const std::string& path);

	private:
		std::vector<glm::vec3> m_Vertices;
		std::vector<NavMeshPolygon> m_Polygons;
	};

	class WLD_API NavMeshSystem
	{
	public:
		static void Update(Scene& scene, float dt, const NavMesh& navMesh);
	};
}
