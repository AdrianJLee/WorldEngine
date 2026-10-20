#pragma once

#include "World/Foliage/Foliage.h"
#include "World/RHI/Rhi.h"
#include "World/Scene/Components.h"

#include <glm/glm.hpp>
#include <memory>
#include <unordered_map>

namespace World
{
	struct FoliageShaderUniforms
	{
		glm::vec4 WindParams { 1.5f, 0.4f, 0.0f, 1.0f }; // x=speed, y=strength, z=time, w=stiffness
		glm::vec4 WindDirection { 1.0f, 0.0f, 0.5f, 0.0f }; // xyz=dir, w=unused
		glm::vec4 CullParams { 150.0f, 20.0f, 0.0f, 0.0f }; // x=maxCull, y=fadeRange, zw=unused
	};
	static_assert(sizeof(FoliageShaderUniforms) == 48, "FoliageShaderUniforms must stay 48 bytes (std140)");

	class WLD_API FoliageRenderer
	{
	public:
		static void Initialize();
		static void Shutdown();

		static void SetContext(const Rhi::Handle<Rhi::CommandBuffer>& cmdBuffer,
			const Rhi::Handle<Rhi::DescriptorSet>& globalSet);

		static void RenderFoliage(const FoliageData& data, const FoliageComponent& comp,
			const glm::vec3& cameraPosition, const glm::mat4& viewProj, float time);

		static std::shared_ptr<FoliageData> GetOrCreateFoliageData(const AssetRef& asset);
		static void ClearCache();
	};
}
