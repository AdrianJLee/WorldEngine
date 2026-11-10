#pragma once

#include "World/Core/Export.h"
#include "World/Scene/Components.h"
#include "World/Scene/Scene.h"

#include <glm/glm.hpp>
#include <vector>

namespace World
{
	struct WLD_API PostProcessUniforms
	{
		float FlashWhiteIntensity = 0.0f;
		float OutlineWidth = 0.0f;
		glm::vec4 OutlineColor { 1.0f, 1.0f, 0.0f, 1.0f };
		float DissolveProgress = 0.0f;
		float BloomIntensity = 0.0f;
		float BloomThreshold = 1.0f;
		float VignetteIntensity = 0.0f;
		float ChromaticAberration = 0.0f;
		uint32_t Enabled = 1;
	};

	class WLD_API PostProcessPipeline
	{
	public:
		// 每帧更新: 遍历所有 PostProcessComponent, 衰减受击闪白, 收集全局特效参数
		static void Update(Scene& scene, float dt);

		// 触发全局受击闪白
		static void FlashWhite(Scene& scene, float intensity = 1.0f, float decay = 5.0f);

		// 获取当前场景的合成后处理参数
		static PostProcessUniforms GetActiveUniforms(const Scene& scene);

		// 模拟计算: 受击闪白颜色合成
		static glm::vec3 ApplyFlashWhite(const glm::vec3& baseColor, float intensity);

		// 模拟计算: 暗角衰减计算
		static float CalculateVignette(const glm::vec2& uv, float intensity);

		// 模拟计算: 径向色差采样 UV 偏移
		static void CalculateChromaticUVs(const glm::vec2& uv, float amount,
		                                  glm::vec2& outUvR, glm::vec2& outUvG, glm::vec2& outUvB);
	};
}
