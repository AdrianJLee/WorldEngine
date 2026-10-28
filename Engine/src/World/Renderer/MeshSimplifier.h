#pragma once

#include "World/Core/Export.h"
#include "World/Asset/WModelIO.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace World
{
	using Asset::WModelVertex;

	struct SimplificationOptions
	{
		float TargetRatio = 0.5f;     // 目标保留比例 (0.05f ~ 1.0f)
		float MaxError = 1e5f;
		bool PreserveBorders = true;
		bool CheckNormalFlip = true;
	};

	struct SimplificationResult
	{
		std::vector<WModelVertex> Vertices;
		std::vector<uint32_t> Indices;
		uint32_t OriginalTriangles = 0;
		uint32_t SimplifiedTriangles = 0;
		float ActualRatio = 0.0f;
		float MaxErrorObserved = 0.0f;
	};

	class WLD_API MeshSimplifier
	{
	public:
		static bool Simplify(
			const WModelVertex* vertices, size_t vertexCount,
			const uint32_t* indices, size_t indexCount,
			const SimplificationOptions& options,
			SimplificationResult& outResult,
			std::string* outError = nullptr);
	};
}
