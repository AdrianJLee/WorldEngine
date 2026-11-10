#pragma once

#include "World/RHI/RhiCore.h"

namespace World::Rhi
{
	struct ShaderStageSource
	{
		ShaderStage Stage = ShaderStage::Vertex;
		std::string EntryPoint;
		// 规范字节码:Vulkan 与 GL(GL 4.6 + GL_ARB_gl_spirv)都吃 SPIR-V。
		// Slang-T6b:GLSL 文本降级路径已删除 —— 后端拿不到 SPIR-V 阶段就是错误,不静默换源。
		std::vector<uint8_t> SpirV;
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
