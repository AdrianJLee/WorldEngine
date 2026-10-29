#pragma once

#include "World/Core/Export.h"
#include "World/RHI/Rhi.h"
#include <glm/glm.hpp>

namespace World
{
	class WLD_API DecalProjector
	{
	public:
		// 计算贴花局部立方体空间投影
		static bool ProjectWorldPoint(
			const glm::mat4& invDecalTransform,
			const glm::vec3& worldPos,
			const glm::vec3& surfaceNormal,
			const glm::vec3& decalDirection,
			float normalCutoffDegrees,
			glm::vec2& outUV,
			float& outFade);

		// 计算贴花在平面或地面上的投射四边形顶点 (4 个世界空间角点与对应 UV、Fade)
		static bool ProjectToPlane(
			const glm::mat4& decalTransform,
			const glm::mat4& invDecalTransform,
			const glm::vec3& planePoint,
			const glm::vec3& planeNormal,
			const glm::vec3& decalDirection,
			float normalCutoffDegrees,
			glm::vec3 outPositions[4],
			glm::vec2 outUVs[4],
			float& outFade);
	};

	class WLD_API DecalRenderer
	{
	public:
		// 获取或生成全局内置默认贴花纹理 (高对比度弹孔与渐变淡出图案，带透明度)
		static Rhi::Handle<Rhi::Texture> GetDefaultDecalTexture();

		// 释放全局内置贴花资源
		static void Shutdown();
	};
}