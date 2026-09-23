#pragma once

#include "World/RHI/RhiCore.h"

namespace World::Rhi
{
	// 能力表:前端按能力降级,不按后端猜特性。
	struct Capabilities
	{
		std::string BackendName;
		std::string RendererName;
		uint32_t ApiMajor = 0;
		uint32_t ApiMinor = 0;

		bool DescriptorIndexing = false;         // bindless 前提
		bool BindlessTextures = false;
		bool Compute = false;
		bool DrawIndirect = false;
		bool MultiDrawIndirect = false;
		bool PushConstants = false;
		bool TimelineSemaphores = false;
		bool AnisotropicFiltering = false;
		// 各向异性上限(设备限制);不支持或取不到时为 1。sampler 创建前按
		// min(设置值, 上限) clamp,避免超过 VkPhysicalDeviceLimits::maxSamplerAnisotropy。
		float MaxSamplerAnisotropy = 1.0f;
		bool DepthBiasClamp = false;
		bool TimestampQueries = false;
		bool MeshShaders = false;
		bool RayTracing = false;
		// 命令缓冲能否在工作线程录制(Vulkan: 每线程命令池 → true;
		// OpenGL: 需先落"延迟命令列表 + 渲染线程回放",在此之前为 false)。
		bool ParallelRecording = false;
		// 每帧可同时在飞的帧数(RHI 侧约定的上限;后端可按交换链图像数进一步限制)。
		uint32_t MaxFramesInFlight = 1;
		bool SamplerMirrorClampToEdge = false;
		bool TextureCompressionBC = false;
		// GL 4.6 的 SPIR-V 摄入路径(GL_ARB_gl_spirv):glShaderBinary + glSpecializeShader。
		// Slang 重构的目标形态是"Slang → SPIR-V → {Vulkan, GL}";缺这个能力时 GL 只能退回
		// GLSL 文本(过渡期),重构完成后即为硬性要求。Vulkan 后端不适用(恒 false)。
		bool SpirVShaderModules = false;
		uint32_t MaxColorAttachments = 4;
		uint32_t MaxSampleCount = 1;
		// P4-4a:整数颜色附件(R32_SINT 等,实体 id 通道用)的采样数上限 —— 与 MaxSampleCount
		// 分开:Vulkan 由 framebufferIntegerColorSampleCounts 折算,MSAA 的生效值取两者交集。
		uint32_t MaxIntegerSampleCount = 1;
		uint32_t MaxTextureSize = 4096;
		uint32_t MaxImageArrayLayers = 256;
		uint32_t MaxUniformBufferSize = 64 * 1024;
		uint32_t MaxStorageBufferSize = 128 * 1024 * 1024;
		uint32_t MaxPushConstantSize = 128;
	};
}
