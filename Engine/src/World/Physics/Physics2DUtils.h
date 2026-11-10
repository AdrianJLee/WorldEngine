#pragma once

#include "World/Core/Export.h"
#include <glm/glm.hpp>
#include <string>
#include <string_view>
#include <vector>

namespace World::Physics2D
{
	// 字符串点集序列化与解析 (格式: "x,y;x,y;x,y")
	WLD_API std::vector<glm::vec2> ParsePointList(std::string_view text);
	WLD_API std::string FormatPointList(const std::vector<glm::vec2>& points);

	// 默认几何顶点预设
	WLD_API std::vector<glm::vec2> GetDefaultPolygonPoints();
	WLD_API std::vector<glm::vec2> GetDefaultChainPoints();

	// 2D 几何计算: 有向面积 (正值 = 逆时针 CCW, 负值 = 顺时针 CW)
	WLD_API float ComputePolygonSignedArea(const std::vector<glm::vec2>& points);

	// 判定多边形是否为凸多边形
	WLD_API bool IsPolygonConvex(const std::vector<glm::vec2>& points);

	// 多边形耳切法凸分解 / 三角剖分:
	// - 顶点数 <= 8 且为凸多边形: 返回单个凸多边形 (直接对接 Box2D v3 b2CreatePolygonShape)
	// - 顶点数 > 8 或为凹多边形: 耳切法三角剖分为若干三角形 (每个三角形 3 顶点 <= 8)
	WLD_API std::vector<std::vector<glm::vec2>> DecomposeOrTriangulatePolygon(const std::vector<glm::vec2>& points);

	// 单向平台效应器接触判定:
	// surfaceArcDeg: 固体表面角度跨度 (默认 180°, 即正上方半球)
	// rotOffsetDeg: 效应器局部角度偏移
	// entityRotZ: 实体世界 Z 轴旋转 (弧度)
	// normalFromPlatformToOther: Box2D 接触法线 (从平台指向碰撞体)
	// relVelocity: 碰撞体相对平台的移动速度 (米/秒)
	// 返回值: true = 发生阻挡碰撞; false = 允许穿透 (单向跳跃通过或从侧方/下方经过)
	WLD_API bool EvaluatePlatformEffector(float surfaceArcDeg, float rotOffsetDeg, float entityRotZ,
		const glm::vec2& normalFromPlatformToOther, const glm::vec2& relVelocity);
}
