#include "wldpch.h"
#include "World/Renderer/CSM.h"

#include <glm/gtc/matrix_transform.hpp>
#include <algorithm>
#include <cmath>

namespace World
{
	void CSM::CalculateSplits(float nearClip, float shadowDistance, float lambda,
		uint32_t count, float* outSplits)
	{
		if (!outSplits || count == 0)
			return;

		const float clipRange = shadowDistance - nearClip;
		const float ratio = shadowDistance / std::max(nearClip, 0.001f);

		for (uint32_t i = 1; i <= count; ++i)
		{
			const float p = static_cast<float>(i) / static_cast<float>(count);
			const float logSplit = nearClip * std::pow(ratio, p);
			const float uniformSplit = nearClip + clipRange * p;
			outSplits[i - 1] = lambda * logSplit + (1.0f - lambda) * uniformSplit;
		}
	}

	void CSM::GetFrustumCornersWorldSpace(
		const glm::mat4&, const glm::mat4& view,
		float splitNear, float splitFar,
		glm::vec3 outCorners[8])
	{
		const glm::mat4 invView = glm::inverse(view);

		// 从反矩阵反向投影裁剪立方体 [-1, 1]
		const glm::mat4 sliceProj = glm::perspective(glm::radians(60.0f), 16.0f / 9.0f, splitNear, splitFar);
		const glm::mat4 invSlice = glm::inverse(sliceProj * view);

		int idx = 0;
		for (int x = 0; x < 2; ++x)
		{
			for (int y = 0; y < 2; ++y)
			{
				for (int z = 0; z < 2; ++z)
				{
					const glm::vec4 pt = invSlice * glm::vec4(
						2.0f * x - 1.0f,
						2.0f * y - 1.0f,
						2.0f * z - 1.0f,
						1.0f
					);
					outCorners[idx++] = glm::vec3(pt) / pt.w;
				}
			}
		}
	}

	void CSM::BuildCascades(
		float fovYRad, float aspectRatio, float nearClip,
		const glm::mat4& cameraTransform,
		const glm::vec3& lightDirection,
		const CSMConfig& config,
		std::array<CSMCascade, kMaxShadowCascades>& outCascades)
	{
		const uint32_t count = std::min(config.CascadeCount, kMaxShadowCascades);
		float splits[kMaxShadowCascades] = { 0.0f };
		CalculateSplits(nearClip, config.MaxShadowDistance, config.SplitLambda, count, splits);

		const glm::vec3 lightDir = glm::normalize(lightDirection);
		const glm::vec3 up(0.0f, 1.0f, 0.0f);
		const glm::vec3 camPos = glm::vec3(cameraTransform[3]);
		const glm::mat4 view = glm::inverse(cameraTransform);

		for (uint32_t i = 0; i < count; ++i)
		{
			const float prevSplit = (i == 0) ? nearClip : splits[i - 1];
			const float curSplit = splits[i];

			outCascades[i].SplitDistance = curSplit;

			// 计算切片角点 (基于相机构造空间)
			const float tanHalfFov = std::tan(fovYRad * 0.5f);
			const float nearH = tanHalfFov * prevSplit;
			const float nearW = nearH * aspectRatio;
			const float farH = tanHalfFov * curSplit;
			const float farW = farH * aspectRatio;

			glm::vec3 corners[8] = {
				// Near plane corners in camera space
				cameraTransform * glm::vec4(-nearW,  nearH, -prevSplit, 1.0f),
				cameraTransform * glm::vec4( nearW,  nearH, -prevSplit, 1.0f),
				cameraTransform * glm::vec4( nearW, -nearH, -prevSplit, 1.0f),
				cameraTransform * glm::vec4(-nearW, -nearH, -prevSplit, 1.0f),
				// Far plane corners in camera space
				cameraTransform * glm::vec4(-farW,  farH, -curSplit, 1.0f),
				cameraTransform * glm::vec4( farW,  farH, -curSplit, 1.0f),
				cameraTransform * glm::vec4( farW, -farH, -curSplit, 1.0f),
				cameraTransform * glm::vec4(-farW, -farH, -curSplit, 1.0f)
			};

			glm::vec3 frustumCenter(0.0f);
			for (int k = 0; k < 8; ++k)
				frustumCenter += corners[k];
			frustumCenter /= 8.0f;

			// 包围球半径，用于消除相机旋转时的阴影抖动 (Bounding Sphere Stabilization)
			float sphereRadius = 0.0f;
			for (int k = 0; k < 8; ++k)
				sphereRadius = std::max(sphereRadius, glm::distance(corners[k], frustumCenter));

			// 半径对齐到 16 单元格，消除微小浮点呼吸
			sphereRadius = std::ceil(sphereRadius * 16.0f) / 16.0f;

			outCascades[i].FrustumSphere = glm::vec4(frustumCenter, sphereRadius);

			// 构建光源观察矩阵
			const glm::vec3 lightEye = frustumCenter - lightDir * (sphereRadius + 100.0f);
			glm::mat4 lightView = glm::lookAt(lightEye, frustumCenter, std::abs(lightDir.y) > 0.99f ? glm::vec3(0, 0, 1) : up);

			// 纹素级对齐吸附 (Texel Snapping): 杜绝相机平移时的阴影边缘像素抖动
			const float worldUnitsPerTexel = (sphereRadius * 2.0f) / static_cast<float>(config.ShadowResolution);
			glm::vec4 shadowOrigin = lightView * glm::vec4(0.0f, 0.0f, 0.0f, 1.0f);
			shadowOrigin.x = std::round(shadowOrigin.x / worldUnitsPerTexel) * worldUnitsPerTexel;
			shadowOrigin.y = std::round(shadowOrigin.y / worldUnitsPerTexel) * worldUnitsPerTexel;
			// 补偿回光源视图矩阵
			glm::vec4 diff = shadowOrigin - (lightView * glm::vec4(0.0f, 0.0f, 0.0f, 1.0f));
			lightView[3][0] += diff.x;
			lightView[3][1] += diff.y;

			// 构造正交投影矩阵
			const glm::mat4 lightOrtho = glm::ortho(
				-sphereRadius, sphereRadius,
				-sphereRadius, sphereRadius,
				0.0f, sphereRadius * 2.0f + 200.0f
			);

			outCascades[i].ViewProjection = lightOrtho * lightView;
		}
	}
}
