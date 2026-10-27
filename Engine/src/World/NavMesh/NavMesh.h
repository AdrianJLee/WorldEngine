#pragma once

#include "World/Core/Export.h"
#include "World/Terrain/Terrain.h"
#include "World/Scene/Components.h"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <cmath>

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

	namespace NavAreaType
	{
		constexpr uint8_t Walkable = 0;   // 常规可行走区 (1.0x 代价，标准净空)
		constexpr uint8_t Road = 1;       // 平坦道路 (0.5x 代价加速，明亮绿色)
		constexpr uint8_t Mud = 2;        // 泥泞沼泽 (3.0x 代价减速，紫色)
		constexpr uint8_t Hazard = 3;     // 危险熔岩 (10.0x 高危避让，橙红色)
		constexpr uint8_t Impassable = 4; // 永久阻断 (无法通行，纯红色)
		constexpr uint8_t LowCeiling = 5; // 半高限高通道 (净空 <= 1.2m，青蓝色；高角色 1.8m 阻断绕行，低角色/蹲伏 1.0m 畅行)
	}

	struct NavQueryFilter
	{
		uint32_t AreaMask = 0xFFFFFFFF; // 可通行区域位掩码 (bit N 对应 AreaType N)
		float AgentHeight = 1.8f;       // 代理站立/蹲伏实际高度 (米)
		float AgentRadius = 0.5f;       // 代理半径 (米)
		bool RequireClearance = true;    // 是否开启天花板净空判定

		bool CanTraverse(uint8_t areaType, float clearance) const
		{
			if ((AreaMask & (1u << areaType)) == 0)
				return false;
			if (RequireClearance && clearance > 0.0f && AgentHeight > clearance + 0.05f)
				return false;
			return true;
		}
	};

	struct NavMeshAreaModifier
	{
		glm::vec3 Center { 0.0f };
		glm::vec3 Extents { 1.0f };
		glm::quat Rotation { 1.0f, 0.0f, 0.0f, 0.0f };
		float CostMultiplier = 1.0f;
		float ClearanceHeight = 0.0f; // 连续净空高度上限（米，0 = 露天不受限，>0 = 任意自定义限高）
		uint8_t AreaType = 0;         // 地表类型（与限高彻底解耦，道路/泥地/草地均可叠加限高）
		uint8_t _Padding[7] { 0 };

		bool ContainsPoint(const glm::vec3& pt) const
		{
			const glm::vec3 localPt = glm::inverse(Rotation) * (pt - Center);
			return std::abs(localPt.x) <= Extents.x &&
			       std::abs(localPt.y) <= Extents.y &&
			       std::abs(localPt.z) <= Extents.z;
		}
	};

	static_assert(sizeof(NavMeshOffMeshLink) == 32, "NavMeshOffMeshLink must be 32 bytes");
	static_assert(sizeof(NavMeshAreaModifier) == 56, "NavMeshAreaModifier must be 56 bytes");

	struct NavMeshMacroTile
	{
		glm::vec3 Corners[4] {};
		glm::vec3 Center { 0.0f };
		float MinY = 0.0f;
		float MaxY = 0.0f;
		uint32_t PolyCount = 0;
		uint8_t DominantAreaType = 0;
		bool Blocked = false;
		std::vector<uint32_t> PolygonIndices;
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
		float Clearance = 0.0f; // 连续净空高度上限（米），0 = 露天不受限，>0 = 净空限高
		uint8_t BaseAreaType = 0;
		float BaseCost = 1.0f;
		float BaseClearance = 0.0f;
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

		// 空间点正交投影至最近多边形表面 (带查询过滤器：净空高度与区域掩码)
		bool SamplePosition(const glm::vec3& point, float maxDistance,
			glm::vec3& outNavPoint, uint32_t& outPolyIdx,
			const NavQueryFilter& filter = {}) const;

		// 3D A* 走廊搜索 + Funnel 漏斗平滑路径规划 (带查询过滤器：净空高度与区域掩码)
		bool FindPath(const glm::vec3& start, const glm::vec3& end,
			std::vector<glm::vec3>& outPath,
			const NavQueryFilter& filter = {}) const;

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
		void ResetAreaModifiers();
		void ReapplyAreaModifiers(const std::vector<NavMeshAreaModifier>& modifiers);
		const std::vector<NavMeshAreaModifier>& GetAreaModifiers() const { return m_AreaModifiers; }

		// 通用空间感知查询
		bool Raycast(const glm::vec3& start, const glm::vec3& end, glm::vec3* outHitPoint = nullptr) const;
		bool GetRandomPointAround(const glm::vec3& center, float radius, glm::vec3& outPoint) const;
		bool IsPointWalkable(const glm::vec3& point, float tolerance = 0.5f) const;
		bool GetNearestPoint(const glm::vec3& point, float maxDist, glm::vec3& outPoint) const;

		// 动态障碍物实时雕刻阻挡
		void ApplyObstacle(const glm::vec3& boxCenter, const glm::vec3& boxExtents, bool carve);
		void ClearObstacles();

		// 层次化宏观大区域瓦片 (Macro Tiles for LOD Navigation Display)
		const std::vector<NavMeshMacroTile>& GetMacroTiles() const { return m_MacroTiles; }
		void BuildMacroTiles(float tileSize = 24.0f);

		// 序列化
		bool SaveToFile(const std::string& path) const;
		bool LoadFromFile(const std::string& path);

	private:
		std::vector<glm::vec3> m_Vertices;
		std::vector<NavMeshPolygon> m_Polygons;
		std::vector<NavMeshOffMeshLink> m_OffMeshLinks;
		std::vector<NavMeshAreaModifier> m_AreaModifiers;
		std::vector<NavMeshMacroTile> m_MacroTiles;
		float m_MacroTileSize = 24.0f;
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
