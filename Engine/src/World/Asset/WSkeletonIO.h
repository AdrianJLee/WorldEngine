#pragma once

#include "World/Core/AssetId.h"
#include "World/Core/Export.h"
#include "World/Core/StringPool.h"
#include "World/Animation/AnimGraph.h"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace World::Asset
{
	struct WModelData;
	struct WAnimationClipData;

	namespace WSkeletonIO
	{
		constexpr uint32_t kMagic = 0x4C4B5357; // 'WSKL' (little-endian: 'W' | ('S' << 8) | ('K' << 16) | ('L' << 24))
		constexpr uint32_t kFormatVersion = 1;
		constexpr uint32_t kMaxBones = 256;
	}

	struct WLD_API SkeletonBone
	{
		NameId Name;
		std::string NameStr;
		int32_t ParentIndex = -1;
		JointTransform BindPoseLocal;
		glm::mat4 InverseBindMatrix { 1.0f };
	};

	struct WLD_API SkeletonSocket
	{
		NameId Name;
		std::string NameStr;
		int32_t BoneIndex = -1;
		JointTransform RelativeTransform;
	};

	struct WLD_API WSkeletonData
	{
		AssetId Identity;
		std::string SourceModelPath;
		std::vector<SkeletonBone> Bones;
		std::vector<SkeletonSocket> Sockets;

		// 快速拓扑缓存：SubtreeIndices[i] 存储骨骼 i 的所有后代子骨骼索引列表，用于 O(1) 递归遮罩遍历
		std::vector<std::vector<uint32_t>> SubtreeIndices;

		void BuildSubtreeIndices();
		int32_t FindBoneIndex(NameId name) const;
		int32_t FindBoneIndex(const std::string& name) const;
		int32_t FindSocketIndex(NameId name) const;
		int32_t FindSocketIndex(const std::string& name) const;
	};

	namespace WSkeletonIO
	{
		WLD_API std::vector<uint8_t> Serialize(const WSkeletonData& data);
		WLD_API bool Parse(const uint8_t* bytes, size_t size, WSkeletonData& out, std::string* error = nullptr);
		WLD_API bool WriteFile(const std::string& path, const WSkeletonData& data, std::string* error = nullptr);
		WLD_API bool ReadFile(const std::string& path, WSkeletonData& out, std::string* error = nullptr);

		// 从已有 WModelData 中无缝抽取骨架与剪辑
		WLD_API bool ExtractSkeletonAndClips(const WModelData& model,
			WSkeletonData& outSkeleton,
			std::vector<WAnimationClipData>& outClips,
			std::string* error = nullptr);
	}
}
