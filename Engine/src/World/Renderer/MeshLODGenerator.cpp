#include "World/Renderer/MeshLODGenerator.h"
#include "World/Renderer/MeshSimplifier.h"
#include "World/Asset/WModelIO.h"

#include <algorithm>
#include <cmath>
#include <filesystem>

namespace World
{
	bool MeshLODGenerator::GenerateLODs(
		const Asset::WModelData& sourceModel,
		const std::vector<LODLevelConfig>& configs,
		std::vector<GeneratedLODAsset>& outLODs,
		std::string* outError)
	{
		if (sourceModel.Vertices.empty() || sourceModel.Indices.empty())
		{
			if (outError) *outError = "Source model contains no vertices or indices";
			return false;
		}

		outLODs.clear();
		outLODs.reserve(configs.size());

		uint32_t totalSourceTriangles = static_cast<uint32_t>(sourceModel.Indices.size() / 3);

		for (const auto& config : configs)
		{
			GeneratedLODAsset lod;
			lod.Level = config.Level;
			lod.ModelData.Meta = sourceModel.Meta;
			lod.ModelData.Meta.Valid = true;
			lod.ModelData.Flags = sourceModel.Flags;
			lod.ModelData.VertexLayoutId = Asset::WModelIO::kVertexLayoutStandard;
			lod.ModelData.MaterialSlots = sourceModel.MaterialSlots;
			lod.ModelData.Nodes = sourceModel.Nodes;
			lod.ModelData.Skins = sourceModel.Skins;
			lod.ModelData.Animations = sourceModel.Animations;

			SimplificationOptions options;
			options.TargetRatio = config.TargetRatio;
			options.PreserveBorders = true;
			options.CheckNormalFlip = true;

			if (sourceModel.Submeshes.empty())
			{
				SimplificationResult res;
				if (!MeshSimplifier::Simplify(
					sourceModel.Vertices.data(), sourceModel.Vertices.size(),
					sourceModel.Indices.data(), sourceModel.Indices.size(),
					options, res, outError))
				{
					return false;
				}

				lod.ModelData.Vertices = std::move(res.Vertices);
				lod.ModelData.Indices = std::move(res.Indices);

				Asset::WModelSubmesh sub;
				sub.IndexOffset = 0;
				sub.IndexCount = static_cast<uint32_t>(lod.ModelData.Indices.size());
				sub.MaterialSlot = 0;

				glm::vec3 bMin(1e30f), bMax(-1e30f);
				for (const auto& v : lod.ModelData.Vertices)
				{
					bMin = glm::min(bMin, v.Position);
					bMax = glm::max(bMax, v.Position);
				}
				if (lod.ModelData.Vertices.empty())
				{
					bMin = sourceModel.Bounds.Min;
					bMax = sourceModel.Bounds.Max;
				}
				sub.Bounds.Min = bMin;
				sub.Bounds.Max = bMax;
				lod.ModelData.Submeshes.push_back(sub);
				lod.ModelData.Bounds = sub.Bounds;

				Asset::WModelMeshRange mr;
				mr.FirstSubmesh = 0;
				mr.SubmeshCount = 1;
				mr.SkinIndex = -1;
				lod.ModelData.Meshes.push_back(mr);
			}
			else
			{
				glm::vec3 globalMin(1e30f), globalMax(-1e30f);

				for (const auto& srcSub : sourceModel.Submeshes)
				{
					if (srcSub.IndexOffset + srcSub.IndexCount > sourceModel.Indices.size())
						continue;

					SimplificationResult res;
					if (!MeshSimplifier::Simplify(
						sourceModel.Vertices.data(), sourceModel.Vertices.size(),
						sourceModel.Indices.data() + srcSub.IndexOffset, srcSub.IndexCount,
						options, res, outError))
					{
						return false;
					}

					uint32_t vBase = static_cast<uint32_t>(lod.ModelData.Vertices.size());
					glm::vec3 subMin(1e30f), subMax(-1e30f);

					for (const auto& v : res.Vertices)
					{
						lod.ModelData.Vertices.push_back(v);
						subMin = glm::min(subMin, v.Position);
						subMax = glm::max(subMax, v.Position);
						globalMin = glm::min(globalMin, v.Position);
						globalMax = glm::max(globalMax, v.Position);
					}

					Asset::WModelSubmesh newSub;
					newSub.IndexOffset = static_cast<uint32_t>(lod.ModelData.Indices.size());
					newSub.IndexCount = static_cast<uint32_t>(res.Indices.size());
					newSub.MaterialSlot = srcSub.MaterialSlot;

					if (res.Vertices.empty())
					{
						newSub.Bounds = srcSub.Bounds;
					}
					else
					{
						newSub.Bounds.Min = subMin;
						newSub.Bounds.Max = subMax;
					}

					for (uint32_t idx : res.Indices)
					{
						lod.ModelData.Indices.push_back(idx + vBase);
					}

					lod.ModelData.Submeshes.push_back(newSub);
				}

				if (!lod.ModelData.Vertices.empty())
				{
					lod.ModelData.Bounds.Min = globalMin;
					lod.ModelData.Bounds.Max = globalMax;
				}
				else
				{
					lod.ModelData.Bounds = sourceModel.Bounds;
				}

				lod.ModelData.Meshes = sourceModel.Meshes;
			}

			lod.TriangleCount = static_cast<uint32_t>(lod.ModelData.Indices.size() / 3);
			lod.ActualRatio = totalSourceTriangles > 0 ? (static_cast<float>(lod.TriangleCount) / static_cast<float>(totalSourceTriangles)) : 0.0f;

			outLODs.push_back(std::move(lod));
		}

		return true;
	}

	bool MeshLODGenerator::GenerateAndSaveLODFiles(
		const std::string& sourceModelPath,
		const std::vector<LODLevelConfig>& configs,
		std::vector<GeneratedLODAsset>& outGenerated,
		std::string* outError)
	{
		Asset::WModelData sourceModel;
		std::string readErr;
		if (!Asset::WModelIO::ReadFile(sourceModelPath, sourceModel, &readErr))
		{
			if (outError) *outError = "Failed to read source model from '" + sourceModelPath + "': " + readErr;
			return false;
		}

		if (!GenerateLODs(sourceModel, configs, outGenerated, outError))
		{
			return false;
		}

		std::filesystem::path p(sourceModelPath);
		std::filesystem::path parentDir = p.parent_path();
		std::string stem = p.stem().string();

		for (auto& lod : outGenerated)
		{
			std::string filename = stem + "_LOD" + std::to_string(lod.Level) + ".wmodel";
			std::filesystem::path lodPath = parentDir.empty() ? std::filesystem::path(filename) : (parentDir / filename);
			lod.RelativePath = lodPath.generic_string();

			std::string writeErr;
			if (!Asset::WModelIO::WriteFile(lod.RelativePath, lod.ModelData, &writeErr))
			{
				if (outError) *outError = "Failed to write LOD file '" + lod.RelativePath + "': " + writeErr;
				return false;
			}
		}

		return true;
	}
}
