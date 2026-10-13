#pragma once

#include "World/Core/Export.h"
#include <glm/glm.hpp>
#include <cstdint>
#include <vector>

namespace World
{
	// 瓦片地图网格拓扑形态
	enum class TilemapGridType : uint8_t
	{
		Orthogonal = 0,       // 笛卡尔正交直角网格 (默认)
		Isometric = 1,        // 2.5D 等轴测菱形网格 (斜视角 / 斜 45°)
		HexagonalPointy = 2,  // 六边形尖顶蜂窝网格 (Pointy-topped)
		HexagonalFlat = 3     // 六边形平顶蜂窝网格 (Flat-topped)
	};

	class WLD_API TilemapGridUtils
	{
	public:
		// 网格坐标 (x, y) 转换为局部空间几何中心坐标
		static glm::vec2 GridToLocal(int x, int y, TilemapGridType type,
			const glm::vec2& cellSize, uint32_t mapW, uint32_t mapH);

		// 局部空间坐标转换为网格离散坐标 (x, y)
		static glm::ivec2 LocalToGrid(const glm::vec2& localPos, TilemapGridType type,
			const glm::vec2& cellSize, uint32_t mapW, uint32_t mapH);

		// 获取单元格几何轮廓多边形顶点 (局部空间)
		// Orthogonal / Isometric: 4 顶点; Hexagonal: 6 顶点
		static std::vector<glm::vec2> GetCellPolygon(int x, int y, TilemapGridType type,
			const glm::vec2& cellSize, uint32_t mapW, uint32_t mapH);

		// 等轴测网格深度排序键计算 (x + y 越大，画面中越靠下，渲染越靠前)
		static float ComputeIsometricDepthKey(int x, int y);

		// 拓扑名称字符串转换
		static const char* GetGridTypeName(TilemapGridType type);
		static TilemapGridType ParseGridTypeName(const char* name);
	};
}
