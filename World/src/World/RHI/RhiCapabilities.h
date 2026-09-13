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
		bool DepthBiasClamp = false;
		bool TimestampQueries = false;
		bool MeshShaders = false;
		bool RayTracing = false;
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
