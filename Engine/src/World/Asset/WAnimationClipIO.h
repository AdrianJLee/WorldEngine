#pragma once

#include "World/Core/AssetId.h"
#include "World/Core/AssetRef.h"
#include "World/Core/Export.h"
#include "World/Core/StringPool.h"
#include "World/Animation/AnimGraph.h"
#include "World/Asset/WSkeletonIO.h"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace World::Asset
{
	namespace WAnimationClipIO
	{
		constexpr uint32_t kMagic = 0x504C4357; // 'WCLP' (little-endian: 'W' | ('C' << 8) | ('L' << 16) | ('P' << 24))
		constexpr uint32_t kFormatVersion = 1;
	}

	struct WLD_API AnimationKeyVec3
	{
		float Time = 0.0f;
		glm::vec3 Value { 0.0f };
	};

	struct WLD_API AnimationKeyQuat
	{
		float Time = 0.0f;
		glm::quat Value { 1.0f, 0.0f, 0.0f, 0.0f };
	};

	struct WLD_API AnimationChannel
	{
		NameId BoneName;
		std::string BoneNameStr;
		std::vector<AnimationKeyVec3> PositionKeys;
		std::vector<AnimationKeyQuat> RotationKeys;
		std::vector<AnimationKeyVec3> ScaleKeys;

		glm::vec3 SamplePosition(float time, const glm::vec3& fallback = glm::vec3(0.0f)) const;
		glm::quat SampleRotation(float time, const glm::quat& fallback = glm::quat(1.0f, 0.0f, 0.0f, 0.0f)) const;
		glm::vec3 SampleScale(float time, const glm::vec3& fallback = glm::vec3(1.0f)) const;
	};

	struct WLD_API AnimationEvent
	{
		float Time = 0.0f;
		NameId Name;
		std::string NameStr;
		std::string Payload;
	};

	struct WLD_API WAnimationClipData
	{
		AssetId Identity;
		std::string Name;
		AssetRef Skeleton;
		std::string SkeletonPath;
		float Duration = 0.0f;
		float SampleRate = 30.0f;
		bool Loop = true;
		bool HasRootMotion = false;
		std::vector<AnimationKeyVec3> RootTranslation;
		std::vector<AnimationKeyQuat> RootRotation;
		std::vector<AnimationChannel> Channels;
		std::vector<AnimationEvent> Events;

		const AnimationChannel* FindChannel(NameId boneName) const;
		const AnimationChannel* FindChannel(const std::string& boneName) const;
	};

	namespace WAnimationClipIO
	{
		WLD_API std::vector<uint8_t> Serialize(const WAnimationClipData& data);
		WLD_API bool Parse(const uint8_t* bytes, size_t size, WAnimationClipData& out, std::string* error = nullptr);
		WLD_API bool WriteFile(const std::string& path, const WAnimationClipData& data, std::string* error = nullptr);
		WLD_API bool ReadFile(const std::string& path, WAnimationClipData& out, std::string* error = nullptr);

		// 采样动画片段：计算在指定时间、循环模式下的骨架姿态与根运动
		WLD_API bool SampleAnimationClip(const WAnimationClipData& clip,
			float time,
			bool loop,
			const WSkeletonData& skeleton,
			std::vector<JointTransform>& outPose,
			glm::vec3* outRootPos = nullptr,
			glm::quat* outRootRot = nullptr);
	}
}
