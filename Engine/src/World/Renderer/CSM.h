#pragma once

#include "World/Core/Export.h"
#include <glm/glm.hpp>
#include <array>
#include <cstdint>

namespace World
{
	constexpr uint32_t kMaxShadowCascades = 4;

	struct CSMConfig
	{
		uint32_t CascadeCount { 4 };
		float SplitLambda { 0.85f };     // 0.0 = 纯均匀切分, 1.0 = 纯对数切分 (推荐 0.85 兼顾近景高精与远景覆盖)
		float MaxShadowDistance { 200.0f }; // 最大阴影可见距离 (米)
		uint32_t ShadowResolution { 2048 }; // 单级阴影分辨率
		float CascadeBlendRange { 3.0f }; // 级联切口软混合过渡带 (米)
	};

	struct CSMCascade
	{
		float SplitDistance { 0.0f };     // 视图空间切分深度 (米)
		glm::mat4 ViewProjection { 1.0f }; // 旋转正交光源视射矩阵 (已完成 Texel Snapping 纹素对齐)
		glm::vec4 FrustumSphere { 0.0f };  // xyz = 世界中心, w = 包围球半径
	};

	class WLD_API CSM
	{
	public:
		// 依据对数-均匀混合 Practical Split 算法计算各级切片深度
		static void CalculateSplits(float nearClip, float shadowDistance, float lambda,
			uint32_t count, float* outSplits);

		// 计算 4 级紧贴视锥切片的光源正交矩阵 (支持旋转对齐、深度拉伸与纹素级吸附避免相机抖动)
		static void BuildCascades(
			float fovYRad, float aspectRatio, float nearClip,
			const glm::mat4& cameraTransform,
			const glm::vec3& lightDirection,
			const CSMConfig& config,
			std::array<CSMCascade, kMaxShadowCascades>& outCascades);

		// 提取视锥切片的 8 个世界空间角点
		static void GetFrustumCornersWorldSpace(
			const glm::mat4& proj, const glm::mat4& view,
			float splitNear, float splitFar,
			glm::vec3 outCorners[8]);
	};
}
