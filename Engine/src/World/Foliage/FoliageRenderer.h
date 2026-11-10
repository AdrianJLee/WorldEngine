#pragma once

#include "World/Foliage/Foliage.h"
#include "World/RHI/Rhi.h"
#include "World/Scene/Components.h"

#include <glm/glm.hpp>
#include <memory>
#include <unordered_map>

namespace World
{
	struct LocalWindGPU
	{
		glm::vec4 PositionRadius { 0.0f };     // xyz = world center, w = radius
		glm::vec4 DirectionStrength { 1.0f, 0.0f, 0.0f, 0.0f }; // xyz = direction/axis, w = strength
		glm::vec4 Params { 2.0f, 1.5f, 1.0f, 0.0f }; // x = speed, y = decay, z = type (0=Dir, 1=Radial, 2=Vortex), w = unused
	};
	static_assert(sizeof(LocalWindGPU) == 48, "LocalWindGPU must stay 48 bytes (std140)");

	constexpr size_t kMaxLocalWinds = 8;

	struct FoliageShaderUniforms
	{
		glm::vec4 WindParams { 1.5f, 0.4f, 0.0f, 1.0f }; // x=speed, y=strength, z=time, w=stiffness
		glm::vec4 WindDirection { 1.0f, 0.0f, 0.5f, 0.0f }; // xyz=dir, w=unused
		glm::vec4 CullParams { 150.0f, 20.0f, 0.0f, 0.0f }; // x=maxCull, y=fadeRange, z=localWindCount, w=unused
		LocalWindGPU LocalWinds[kMaxLocalWinds];
	};
	static_assert(sizeof(FoliageShaderUniforms) == 48 + 48 * kMaxLocalWinds, "FoliageShaderUniforms size mismatch");

	class WLD_API FoliageRenderer
	{
	public:
		static void Initialize();
		static void Shutdown();

		static void SetContext(const Rhi::Handle<Rhi::CommandBuffer>& cmdBuffer,
			const Rhi::Handle<Rhi::DescriptorSet>& globalSet);

		static void RenderFoliage(const FoliageData& data, const FoliageComponent& comp,
			const glm::vec3& cameraPosition, const glm::mat4& viewProj, float time);

		static void AddLocalWind(const glm::vec3& position, float radius, float strength, float speed = 3.0f, float decay = 1.5f, LocalWindType type = LocalWindType::Radial, const glm::vec3& direction = glm::vec3(1.0f, 0.0f, 0.0f));
		static void ClearLocalWinds();
		static size_t GetLocalWindCount();

		static std::shared_ptr<FoliageData> GetOrCreateFoliageData(const AssetRef& asset);
		static void ClearCache();
	};
}
