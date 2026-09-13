#pragma once

#include "World/RHI/RhiCore.h"

namespace World::Rhi
{
	struct ShaderStageSource
	{
		ShaderStage Stage = ShaderStage::Vertex;
		std::string EntryPoint;
		std::vector<uint8_t> SpirV;   // 规范字节码(Vulkan 直接使用)
		std::string Glsl;             // GL 后端降级源(cook 阶段 spirv-cross 产出)
	};

	struct ShaderDesc
	{
		std::string DebugName;
		std::vector<ShaderStageSource> Stages;
	};

	class WLD_API Shader
	{
	public:
		virtual ~Shader() = default;
		virtual const ShaderDesc& GetDesc() const = 0;
	};
}
