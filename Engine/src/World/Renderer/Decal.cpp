#include "wldpch.h"
#include "World/Renderer/Decal.h"
#include "World/Renderer/Renderer.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace World
{
	namespace
	{
		Rhi::Handle<Rhi::Texture> s_DefaultDecalTexture = nullptr;
	}

	bool DecalProjector::ProjectWorldPoint(
		const glm::mat4& invDecalTransform,
		const glm::vec3& worldPos,
		const glm::vec3& surfaceNormal,
		const glm::vec3& decalDirection,
		float normalCutoffDegrees,
		glm::vec2& outUV,
		float& outFade)
	{
		const glm::vec4 localPos4 = invDecalTransform * glm::vec4(worldPos, 1.0f);
		const glm::vec3 localPos = glm::vec3(localPos4);

		// 局部盒体 [-1, 1]^3 边界裁剪
		if (std::abs(localPos.x) > 1.0f || std::abs(localPos.y) > 1.0f || std::abs(localPos.z) > 1.0f)
			return false;

		// 法线掠射角防拉伸消隐 (Normal Cutoff)
		const float cosTheta = glm::dot(glm::normalize(surfaceNormal), glm::normalize(-decalDirection));
		const float cosCutoff = std::cos(glm::radians(std::clamp(normalCutoffDegrees, 0.0f, 89.0f)));
		if (cosTheta < cosCutoff)
			return false;

		// 投影 UV: local X/Z 映射到 [0, 1]
		outUV = glm::vec2(localPos.x * 0.5f + 0.5f, localPos.z * 0.5f + 0.5f);

		// 综合边缘深度与法线夹角平滑衰减
		const float normalFade = std::clamp((cosTheta - cosCutoff) / std::max(1.0f - cosCutoff, 1e-4f), 0.0f, 1.0f);
		const float depthFade = 1.0f - std::abs(localPos.y);
		outFade = normalFade * depthFade;

		return true;
	}

	bool DecalProjector::ProjectToPlane(
		const glm::mat4& decalTransform,
		const glm::mat4&,
		const glm::vec3& planePoint,
		const glm::vec3& planeNormal,
		const glm::vec3& decalDirection,
		float normalCutoffDegrees,
		glm::vec3 outPositions[4],
		glm::vec2 outUVs[4],
		float& outFade)
	{
		const glm::vec3 nPlane = glm::normalize(planeNormal);
		const glm::vec3 nDecal = glm::normalize(decalDirection);
		const float cosTheta = glm::dot(nPlane, -nDecal);
		const float cosCutoff = std::cos(glm::radians(std::clamp(normalCutoffDegrees, 0.0f, 89.0f)));
		if (cosTheta < cosCutoff)
			return false;

		// 局部投影空间 4 个角点在顶底面的射线
		// 投影方向沿局部 -Y 轴；UV X 对应局部 X，UV Y 对应局部 Z
		const glm::vec2 localCorners[4] = {
			{ -1.0f, -1.0f },
			{  1.0f, -1.0f },
			{  1.0f,  1.0f },
			{ -1.0f,  1.0f }
		};

		float totalFade = 0.0f;
		for (int i = 0; i < 4; ++i)
		{
			const glm::vec4 topLocal(localCorners[i].x,  1.0f, localCorners[i].y, 1.0f);
			const glm::vec4 botLocal(localCorners[i].x, -1.0f, localCorners[i].y, 1.0f);

			const glm::vec3 wTop = glm::vec3(decalTransform * topLocal);
			const glm::vec3 wBot = glm::vec3(decalTransform * botLocal);
			const glm::vec3 rayDir = wBot - wTop;

			const float denom = glm::dot(rayDir, nPlane);
			if (std::abs(denom) < 1e-6f)
				return false;

			const float t = glm::dot(planePoint - wTop, nPlane) / denom;
			if (t < -0.1f || t > 1.1f)
				return false;

			const float clampedT = std::clamp(t, 0.0f, 1.0f);
			glm::vec3 pIntersect = wTop + clampedT * rayDir;

			// 沿平面法线微量偏移 (Depth Bias 消除共面 Z-Fighting)
			pIntersect += nPlane * 0.002f;
			outPositions[i] = pIntersect;

			outUVs[i] = glm::vec2(localCorners[i].x * 0.5f + 0.5f, localCorners[i].y * 0.5f + 0.5f);

			const float localY = 1.0f - 2.0f * clampedT;
			const float depthFade = std::max(0.0f, 1.0f - std::abs(localY));
			const float normalFade = std::clamp((cosTheta - cosCutoff) / std::max(1.0f - cosCutoff, 1e-4f), 0.0f, 1.0f);
			totalFade += normalFade * depthFade;
		}

		outFade = totalFade * 0.25f;
		return outFade > 0.001f;
	}

	Rhi::Handle<Rhi::Texture> DecalRenderer::GetDefaultDecalTexture()
	{
		if (s_DefaultDecalTexture)
			return s_DefaultDecalTexture;

		auto device = Renderer::GetDevice();
		if (!device)
			return nullptr;

		constexpr uint32_t width = 64;
		constexpr uint32_t height = 64;
		std::vector<uint8_t> pixels(width * height * 4, 0);

		for (uint32_t y = 0; y < height; ++y)
		{
			for (uint32_t x = 0; x < width; ++x)
			{
				const float u = (static_cast<float>(x) + 0.5f) / static_cast<float>(width) * 2.0f - 1.0f;
				const float v = (static_cast<float>(y) + 0.5f) / static_cast<float>(height) * 2.0f - 1.0f;
				const float r = std::sqrt(u * u + v * v);
				const float theta = std::atan2(v, u);

				const size_t idx = (y * width + x) * 4;
				if (r > 1.0f)
					continue;

				// 弹孔核心区 (r < 0.22)
				if (r < 0.22f)
				{
					pixels[idx + 0] = 25;
					pixels[idx + 1] = 25;
					pixels[idx + 2] = 30;
					pixels[idx + 3] = 255;
				}
				// 灼烧内圈 (0.22 <= r < 0.38)
				else if (r < 0.38f)
				{
					const float t = (r - 0.22f) / 0.16f;
					pixels[idx + 0] = static_cast<uint8_t>(255.0f * (1.0f - t * 0.4f));
					pixels[idx + 1] = static_cast<uint8_t>(120.0f * (1.0f - t * 0.5f));
					pixels[idx + 2] = 30;
					pixels[idx + 3] = 245;
				}
				// 裂纹飞溅渐变外圈 (0.38 <= r <= 0.95)
				else
				{
					const float jaggedR = r + 0.08f * std::sin(8.0f * theta) + 0.05f * std::cos(14.0f * theta);
					if (jaggedR <= 0.95f)
					{
						const float falloff = std::clamp((0.95f - jaggedR) / 0.57f, 0.0f, 1.0f);
						const uint8_t alpha = static_cast<uint8_t>(falloff * 220.0f);
						pixels[idx + 0] = static_cast<uint8_t>(180.0f * falloff + 30.0f);
						pixels[idx + 1] = static_cast<uint8_t>(50.0f * falloff + 20.0f);
						pixels[idx + 2] = 25;
						pixels[idx + 3] = alpha;
					}
				}

				// 十字准星标线 (便于零配置下调试投影方位)
				if ((std::abs(u) < 0.04f || std::abs(v) < 0.04f) && r >= 0.45f && r <= 0.85f)
				{
					pixels[idx + 0] = 255;
					pixels[idx + 1] = 220;
					pixels[idx + 2] = 40;
					pixels[idx + 3] = 240;
				}
			}
		}

		Rhi::TextureDesc desc;
		desc.Type = Rhi::TextureType::Texture2D;
		desc.Format = Rhi::Format::R8G8B8A8_UNORM;
		desc.Extent = { width, height, 1 };
		desc.Usage = Rhi::TextureUsageSampled;

		s_DefaultDecalTexture = device->CreateTexture(desc);
		if (s_DefaultDecalTexture)
			s_DefaultDecalTexture->SetData(pixels.data(), static_cast<uint32_t>(pixels.size()));

		return s_DefaultDecalTexture;
	}

	void DecalRenderer::Shutdown()
	{
		s_DefaultDecalTexture = nullptr;
	}
}