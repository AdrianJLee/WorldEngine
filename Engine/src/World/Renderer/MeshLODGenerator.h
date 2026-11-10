#pragma once

#include "World/Core/Export.h"
#include "World/Asset/WModelIO.h"

#include <cstdint>
#include <string>
#include <vector>

namespace World
{
	struct LODLevelConfig
	{
		uint8_t Level = 1;            // 1, 2, 3...
		float TargetRatio = 0.5f;     // 0.5 = 50%, 0.25 = 25%
		float Distance = 30.0f;       // 建议切换视距
	};

	struct GeneratedLODAsset
	{
		uint8_t Level = 1;
		std::string RelativePath;     // 如 "models/rock_LOD1.wmodel"
		Asset::WModelData ModelData;
		uint32_t TriangleCount = 0;
		float ActualRatio = 0.0f;
	};

	class WLD_API MeshLODGenerator
	{
	public:
		// 对源 WModelData 批量生成多级 LOD 模型数据
		static bool GenerateLODs(
			const Asset::WModelData& sourceModel,
			const std::vector<LODLevelConfig>& configs,
			std::vector<GeneratedLODAsset>& outLODs,
			std::string* outError = nullptr);

		// 读取源文件，自动生成并在磁盘写出 <stem>_LOD{n}.wmodel
		static bool GenerateAndSaveLODFiles(
			const std::string& sourceModelPath,
			const std::vector<LODLevelConfig>& configs,
			std::vector<GeneratedLODAsset>& outGenerated,
			std::string* outError = nullptr);
	};
}
