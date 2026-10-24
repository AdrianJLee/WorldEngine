#pragma once

#include "World/Core/Export.h"
#include "World/Terrain/Terrain.h"
#include "World/Scene/Components.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>
#include <future>

namespace World
{
	class Scene;

	struct NavMeshTriangle
	{
		glm::vec3 A { 0.0f };
		glm::vec3 B { 0.0f };
		glm::vec3 C { 0.0f };
	};

	struct NavMeshOffMeshLink
	{
		glm::vec3 Start { 0.0f };
		glm::vec3 End { 0.0f };
		float Cost = 1.0f;
		uint8_t LinkType = 0;
		bool BiDirectional = true;
	};

	struct NavMeshAreaModifier
	{
		glm::vec3 Center { 0.0f };
		glm::vec3 Extents { 1.0f };
		float CostMultiplier = 2.0f;
		uint8_t AreaType = 1; // 0=Walkable, 1=Road, 2=Mud, 3=Hazard, 4=Impassable
	};

	struct NavMeshBakeSettings
	{
		float AgentRadius = 0.5f;
		float AgentHeight = 2.0f;
		float MaxClimbSlopeDeg = 45.0f;
		float StepHeight = 0.45f; // 台阶最大攀爬高度
		float VoxelSize = 1.0f;   // 体素网格步长
		bool IncludeMeshes = true;
		bool IncludeTerrain = true;
		bool IncludeWhitebox = true;
		bool IncludeColliders = true;
		glm::vec3 BoundsMin { -100.0f, -20.0f, -100.0f };
		glm::vec3 BoundsMax {  100.0f,  50.0f,  100.0f };
		bool UseCustomBounds = false;
	};

	struct NavMeshPolygon
	{
		std::vector<uint32_t> VertexIndices;
		std::vector<int32_t> Neighbors; // 邻接多边形下标，-1 表示边界外
		glm::vec3 Center { 0.0f };
		glm::vec3 Normal { 0.0f, 1.0f, 0.0f };
		float Area { 0.0f };
		bool Blocked { false }; // 动态障碍物打洞标记
		uint8_t AreaType = 0;
		float Cost = 1.0f;
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
		void BuildAdjacency(float stepHeight = 0.45f);

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

		// 全场景 3D 几何提取与通用烘焙 (同步与多线程异步后台烘焙)
		static void ExtractSceneGeometry(const Scene& scene, const NavMeshBakeSettings& settings,
		                                 std::vector<NavMeshTriangle>& outTriangles);

		void BakeFromTriangles(const std::vector<NavMeshTriangle>& triangles, const NavMeshBakeSettings& settings = {});
		void BakeFromScene(const Scene& scene, const NavMeshBakeSettings& settings = {});

		static std::future<std::shared_ptr<NavMesh>> BakeSceneAsync(const Scene& scene, const NavMeshBakeSettings& settings = {});

		// 跨网格跳跃链接 (Off-Mesh Links)
		void AddOffMeshLink(const NavMeshOffMeshLink& link);
		void ClearOffMeshLinks();
		const std::vector<NavMeshOffMeshLink>& GetOffMeshLinks() const { return m_OffMeshLinks; }

		// 区域代价修饰区 (Area Modifiers)
		void AddAreaModifier(const NavMeshAreaModifier& modifier);
		void ClearAreaModifiers();
		const std::vector<NavMeshAreaModifier>& GetAreaModifiers() const { return m_AreaModifiers; }

		// 通用空间感知查询
		bool Raycast(const glm::vec3& start, const glm::vec3& end, glm::vec3* outHitPoint = nullptr) const;
		bool GetRandomPointAround(const glm::vec3& center, float radius, glm::vec3& outPoint) const;
		bool IsPointWalkable(const glm::vec3& point, float tolerance = 0.5f) const;
		bool GetNearestPoint(const glm::vec3& point, float maxDist, glm::vec3& outPoint) const;

		// 动态障碍物实时雕刻阻挡
		void ApplyObstacle(const glm::vec3& boxCenter, const glm::vec3& boxExtents, bool carve);
		void ClearObstacles();

		// 序列化
		bool SaveToFile(const std::string& path) const;
		bool LoadFromFile(const std::string& path);

	private:
		std::vector<glm::vec3> m_Vertices;
		std::vector<NavMeshPolygon> m_Polygons;
		std::vector<NavMeshOffMeshLink> m_OffMeshLinks;
		std::vector<NavMeshAreaModifier> m_AreaModifiers;
	};

	class WLD_API NavMeshSystem
	{
	public:
		static void Update(Scene& scene, float dt, const NavMesh& navMesh);
		static void Update(Scene& scene, float dt, NavMesh& navMesh)
		{
			Update(scene, dt, static_cast<const NavMesh&>(navMesh));
		}
	};
}
