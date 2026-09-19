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
		uint32_t MaxColorAttachments = 4;
		uint32_t MaxSampleCount = 1;
		uint32_t MaxTextureSize = 4096;
		uint32_t MaxImageArrayLayers = 256;
		uint32_t MaxUniformBufferSize = 64 * 1024;
		uint32_t MaxStorageBufferSize = 128 * 1024 * 1024;
		uint32_t MaxPushConstantSize = 128;
	};
}
