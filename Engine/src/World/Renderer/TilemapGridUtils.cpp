#include "wldpch.h"
#include "World/Renderer/TilemapGridUtils.h"

#include <cmath>
#include <algorithm>
#include <cstring>

namespace World
{
	glm::vec2 TilemapGridUtils::GridToLocal(int x, int y, TilemapGridType type,
		const glm::vec2& cellSize, uint32_t mapW, uint32_t mapH)
	{
		const float cellW = cellSize.x > 0.0f ? cellSize.x : 1.0f;
		const float cellH = cellSize.y > 0.0f ? cellSize.y : 1.0f;
		const float centerGX = static_cast<float>(mapW > 0 ? mapW - 1 : 0) * 0.5f;
		const float centerGY = static_cast<float>(mapH > 0 ? mapH - 1 : 0) * 0.5f;

		switch (type)
		{
		case TilemapGridType::Orthogonal:
		{
			const float mapWidth = static_cast<float>(mapW) * cellW;
			const float mapHeight = static_cast<float>(mapH) * cellH;
			const float originX = -mapWidth * 0.5f;
			const float originY = -mapHeight * 0.5f;
			return {
				originX + (static_cast<float>(x) + 0.5f) * cellW,
				originY + (static_cast<float>(mapH - 1 - y) + 0.5f) * cellH
			};
		}
		case TilemapGridType::Isometric:
		{
			const float dx = static_cast<float>(x) - centerGX;
			const float dy = static_cast<float>(y) - centerGY;
			return {
				(dx - dy) * cellW * 0.5f,
				-(dx + dy) * cellH * 0.5f
			};
		}
		case TilemapGridType::HexagonalPointy:
		{
			const float dx = static_cast<float>(x) - centerGX;
			const float dy = static_cast<float>(y) - centerGY;
			const float xOffset = (static_cast<int>(std::abs(y)) & 1) ? 0.5f : 0.0f;
			return {
				(dx + xOffset) * cellW,
				-dy * (cellH * 0.75f)
			};
		}
		case TilemapGridType::HexagonalFlat:
		{
			const float dx = static_cast<float>(x) - centerGX;
			const float dy = static_cast<float>(y) - centerGY;
			const float yOffset = (static_cast<int>(std::abs(x)) & 1) ? 0.5f : 0.0f;
			return {
				dx * (cellW * 0.75f),
				-(dy + yOffset) * cellH
			};
		}
		}

		return { 0.0f, 0.0f };
	}

	glm::ivec2 TilemapGridUtils::LocalToGrid(const glm::vec2& localPos, TilemapGridType type,
		const glm::vec2& cellSize, uint32_t mapW, uint32_t mapH)
	{
		const float cellW = cellSize.x > 0.0f ? cellSize.x : 1.0f;
		const float cellH = cellSize.y > 0.0f ? cellSize.y : 1.0f;
		const float centerGX = static_cast<float>(mapW > 0 ? mapW - 1 : 0) * 0.5f;
		const float centerGY = static_cast<float>(mapH > 0 ? mapH - 1 : 0) * 0.5f;

		switch (type)
		{
		case TilemapGridType::Orthogonal:
		{
			const float mapWidth = static_cast<float>(mapW) * cellW;
			const float mapHeight = static_cast<float>(mapH) * cellH;
			const float originX = -mapWidth * 0.5f;
			const float originY = -mapHeight * 0.5f;
			const int gx = static_cast<int>(std::floor((localPos.x - originX) / cellW));
			const int gy = static_cast<int>(mapH - 1 - std::floor((localPos.y - originY) / cellH));
			return {
				std::clamp(gx, 0, static_cast<int>(mapW - 1)),
				std::clamp(gy, 0, static_cast<int>(mapH - 1))
			};
		}
		case TilemapGridType::Isometric:
		{
			const float u = localPos.x / (cellW * 0.5f);
			const float v = -localPos.y / (cellH * 0.5f);
			const float dx = (u + v) * 0.5f;
			const float dy = (v - u) * 0.5f;
			const int gx = static_cast<int>(std::round(dx + centerGX));
			const int gy = static_cast<int>(std::round(dy + centerGY));
			return {
				std::clamp(gx, 0, static_cast<int>(mapW - 1)),
				std::clamp(gy, 0, static_cast<int>(mapH - 1))
			};
		}
		case TilemapGridType::HexagonalPointy:
		{
			const float approxY = -localPos.y / (cellH * 0.75f) + centerGY;
			const int gy = std::clamp(static_cast<int>(std::round(approxY)), 0, static_cast<int>(mapH - 1));
			const float xOffset = (static_cast<int>(std::abs(gy)) & 1) ? 0.5f : 0.0f;
			const float approxX = localPos.x / cellW - xOffset + centerGX;
			const int gx = std::clamp(static_cast<int>(std::round(approxX)), 0, static_cast<int>(mapW - 1));
			return { gx, gy };
		}
		case TilemapGridType::HexagonalFlat:
		{
			const float approxX = localPos.x / (cellW * 0.75f) + centerGX;
			const int gx = std::clamp(static_cast<int>(std::round(approxX)), 0, static_cast<int>(mapW - 1));
			const float yOffset = (static_cast<int>(std::abs(gx)) & 1) ? 0.5f : 0.0f;
			const float approxY = -localPos.y / cellH - yOffset + centerGY;
			const int gy = std::clamp(static_cast<int>(std::round(approxY)), 0, static_cast<int>(mapH - 1));
			return { gx, gy };
		}
		}

		return { 0, 0 };
	}

	std::vector<glm::vec2> TilemapGridUtils::GetCellPolygon(int x, int y, TilemapGridType type,
		const glm::vec2& cellSize, uint32_t mapW, uint32_t mapH)
	{
		const glm::vec2 center = GridToLocal(x, y, type, cellSize, mapW, mapH);
		const float hw = (cellSize.x > 0.0f ? cellSize.x : 1.0f) * 0.5f;
		const float hh = (cellSize.y > 0.0f ? cellSize.y : 1.0f) * 0.5f;

		switch (type)
		{
		case TilemapGridType::Orthogonal:
			return {
				{ center.x - hw, center.y - hh },
				{ center.x + hw, center.y - hh },
				{ center.x + hw, center.y + hh },
				{ center.x - hw, center.y + hh }
			};
		case TilemapGridType::Isometric:
			return {
				{ center.x,      center.y + hh },
				{ center.x + hw, center.y      },
				{ center.x,      center.y - hh },
				{ center.x - hw, center.y      }
			};
		case TilemapGridType::HexagonalPointy:
		{
			std::vector<glm::vec2> hex(6);
			for (int i = 0; i < 6; ++i)
			{
				float angle = glm::radians(30.0f + static_cast<float>(i) * 60.0f);
				hex[i] = { center.x + hw * std::cos(angle), center.y + hh * std::sin(angle) };
			}
			return hex;
		}
		case TilemapGridType::HexagonalFlat:
		{
			std::vector<glm::vec2> hex(6);
			for (int i = 0; i < 6; ++i)
			{
				float angle = glm::radians(static_cast<float>(i) * 60.0f);
				hex[i] = { center.x + hw * std::cos(angle), center.y + hh * std::sin(angle) };
			}
			return hex;
		}
		}

		return {};
	}

	float TilemapGridUtils::ComputeIsometricDepthKey(int x, int y)
	{
		return static_cast<float>(x + y);
	}

	const char* TilemapGridUtils::GetGridTypeName(TilemapGridType type)
	{
		switch (type)
		{
		case TilemapGridType::Orthogonal: return "Orthogonal";
		case TilemapGridType::Isometric: return "Isometric";
		case TilemapGridType::HexagonalPointy: return "HexagonalPointy";
		case TilemapGridType::HexagonalFlat: return "HexagonalFlat";
		}
		return "Orthogonal";
	}

	TilemapGridType TilemapGridUtils::ParseGridTypeName(const char* name)
	{
		if (!name) return TilemapGridType::Orthogonal;
		if (std::strcmp(name, "Isometric") == 0) return TilemapGridType::Isometric;
		if (std::strcmp(name, "HexagonalPointy") == 0) return TilemapGridType::HexagonalPointy;
		if (std::strcmp(name, "HexagonalFlat") == 0) return TilemapGridType::HexagonalFlat;
		return TilemapGridType::Orthogonal;
	}
}
