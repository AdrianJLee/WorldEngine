#include "wldpch.h"
#include "World/Physics/Physics2DUtils.h"

#include <algorithm>
#include <cmath>
#include <sstream>
#include <iomanip>

namespace World::Physics2D
{
	std::vector<glm::vec2> ParsePointList(std::string_view text)
	{
		std::vector<glm::vec2> points;
		if (text.empty())
			return points;

		std::string str(text);
		std::replace(str.begin(), str.end(), ';', ' ');
		std::replace(str.begin(), str.end(), ',', ' ');

		std::istringstream iss(str);
		float x = 0.0f, y = 0.0f;
		while (iss >> x >> y)
		{
			points.push_back({ x, y });
		}
		return points;
	}

	std::string FormatPointList(const std::vector<glm::vec2>& points)
	{
		if (points.empty())
			return "";

		std::ostringstream oss;
		oss << std::fixed << std::setprecision(4);
		for (size_t i = 0; i < points.size(); ++i)
		{
			if (i > 0)
				oss << ";";
			oss << points[i].x << "," << points[i].y;
		}
		return oss.str();
	}

	std::vector<glm::vec2> GetDefaultPolygonPoints()
	{
		return {
			{ -0.5f, -0.5f },
			{  0.5f, -0.5f },
			{  0.5f,  0.5f },
			{ -0.5f,  0.5f }
		};
	}

	std::vector<glm::vec2> GetDefaultChainPoints()
	{
		return {
			{ -2.0f, 0.0f },
			{ -1.0f, 0.5f },
			{  1.0f, 0.5f },
			{  2.0f, 0.0f }
		};
	}

	float ComputePolygonSignedArea(const std::vector<glm::vec2>& points)
	{
		const size_t n = points.size();
		if (n < 3)
			return 0.0f;

		float area = 0.0f;
		for (size_t i = 0; i < n; ++i)
		{
			const size_t next = (i + 1) % n;
			area += points[i].x * points[next].y - points[next].x * points[i].y;
		}
		return 0.5f * area;
	}

	namespace
	{
		float Cross2D(const glm::vec2& a, const glm::vec2& b, const glm::vec2& c)
		{
			return (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
		}

		bool PointInTriangle(const glm::vec2& p, const glm::vec2& a, const glm::vec2& b, const glm::vec2& c)
		{
			const float cp1 = Cross2D(a, b, p);
			const float cp2 = Cross2D(b, c, p);
			const float cp3 = Cross2D(c, a, p);
			const bool hasNeg = (cp1 < -1e-6f) || (cp2 < -1e-6f) || (cp3 < -1e-6f);
			const bool hasPos = (cp1 > 1e-6f) || (cp2 > 1e-6f) || (cp3 > 1e-6f);
			return !(hasNeg && hasPos);
		}
	}

	bool IsPolygonConvex(const std::vector<glm::vec2>& points)
	{
		const size_t n = points.size();
		if (n < 3)
			return false;
		if (n == 3)
			return true;

		bool hasPos = false;
		bool hasNeg = false;
		for (size_t i = 0; i < n; ++i)
		{
			const glm::vec2& a = points[i];
			const glm::vec2& b = points[(i + 1) % n];
			const glm::vec2& c = points[(i + 2) % n];
			const float cp = Cross2D(a, b, c);
			if (cp > 1e-5f) hasPos = true;
			if (cp < -1e-5f) hasNeg = true;
			if (hasPos && hasNeg)
				return false;
		}
		return true;
	}

	std::vector<std::vector<glm::vec2>> DecomposeOrTriangulatePolygon(const std::vector<glm::vec2>& inPoints)
	{
		std::vector<std::vector<glm::vec2>> result;
		if (inPoints.size() < 3)
			return result;

		std::vector<glm::vec2> pts = inPoints;
		// 确保逆时针 (CCW) 绕序
		if (ComputePolygonSignedArea(pts) < 0.0f)
		{
			std::reverse(pts.begin(), pts.end());
		}

		// 若为简单凸多边形且顶点数 <= 8 (Box2D v3 b2Polygon 单个形状最大上限)，直接返回
		if (pts.size() <= 8 && IsPolygonConvex(pts))
		{
			result.push_back(std::move(pts));
			return result;
		}

		// 否则使用耳切法 (Ear Clipping) 剖分为 N-2 个三角形 (每三角形 3 顶点 <= 8)
		std::vector<int> indices(pts.size());
		for (size_t i = 0; i < pts.size(); ++i)
			indices[i] = static_cast<int>(i);

		size_t attempts = 0;
		const size_t maxAttempts = indices.size() * 3;
		while (indices.size() > 3 && attempts < maxAttempts)
		{
			bool earFound = false;
			const size_t n = indices.size();
			for (size_t i = 0; i < n; ++i)
			{
				const int prevIdx = indices[(i + n - 1) % n];
				const int currIdx = indices[i];
				const int nextIdx = indices[(i + 1) % n];

				const glm::vec2& a = pts[prevIdx];
				const glm::vec2& b = pts[currIdx];
				const glm::vec2& c = pts[nextIdx];

				// 凸角检测 (逆时针下叉积 > 0)
				if (Cross2D(a, b, c) <= 1e-6f)
					continue;

				// 检测其余顶点是否落在此三角形内部
				bool isEar = true;
				for (size_t j = 0; j < n; ++j)
				{
					if (j == ((i + n - 1) % n) || j == i || j == ((i + 1) % n))
						continue;
					if (PointInTriangle(pts[indices[j]], a, b, c))
					{
						isEar = false;
						break;
					}
				}

				if (isEar)
				{
					result.push_back({ a, b, c });
					indices.erase(indices.begin() + i);
					earFound = true;
					break;
				}
			}

			if (!earFound)
			{
				// 非简单多边形或存在自交退化: 采用三角形扇 (Triangle Fan) 兜底分解
				break;
			}
			++attempts;
		}

		if (indices.size() == 3)
		{
			result.push_back({ pts[indices[0]], pts[indices[1]], pts[indices[2]] });
		}
		else if (indices.size() > 3)
		{
			// 兜底三角扇
			for (size_t i = 1; i + 1 < indices.size(); ++i)
			{
				result.push_back({ pts[indices[0]], pts[indices[i]], pts[indices[i + 1]] });
			}
		}

		return result;
	}

	bool EvaluatePlatformEffector(float surfaceArcDeg, float rotOffsetDeg, float entityRotZ,
		const glm::vec2& normalFromPlatformToOther, const glm::vec2& relVelocity)
	{
		const float platformAngle = entityRotZ + glm::radians(rotOffsetDeg);
		// 平台局部正上方对应的世界法线向量
		const glm::vec2 platformNormal { -std::sin(platformAngle), std::cos(platformAngle) };

		// 允许碰撞的表面角度范围 (弧度)
		const float halfArcRad = glm::radians(std::max(10.0f, std::min(360.0f, surfaceArcDeg)) * 0.5f);
		const float cosThreshold = std::cos(halfArcRad);

		// 碰撞法线与平台正上方夹角判定
		const float normDot = glm::dot(normalFromPlatformToOther, platformNormal);
		if (normDot < cosThreshold)
		{
			return false; // 处于单向表面有效弧度之外 (例如从下方或侧边经过)
		}

		// 相对速度检测: 若对方物体正在沿着平台法线正向移动 (自下向上穿透跳起)，允许其继续穿过
		const float velDot = glm::dot(relVelocity, platformNormal);
		if (velDot > 0.05f)
		{
			return false; // 向上跳起中，穿透不阻挡
		}

		// 从上方下落接触，进行坚固实体阻挡
		return true;
	}
}
