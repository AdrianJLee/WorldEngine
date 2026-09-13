#include "World/RHI/Rhi.h"

#include <cstdio>

int main()
{
	// 无 Vulkan 驱动/无显卡的环境优雅跳过:设备创建失败不是失败。
	std::string error;
	const World::Rhi::Handle<World::Rhi::Device> device =
		World::Rhi::CreateDevice(World::Rhi::Backend::Vulkan, {}, &error);
	if (!device)
	{
		std::printf("World.VulkanDevice: skipped (%s)\n", error.c_str());
		return 0;
	}

	const World::Rhi::Capabilities& capabilities = device->GetCapabilities();
	const World::Rhi::DeviceLimits limits = device->GetLimits();
	std::printf("World.VulkanDevice: %s (api %u.%u, maxTexture=%u, samples=%u, push=%u)\n",
		capabilities.RendererName.c_str(), capabilities.ApiMajor, capabilities.ApiMinor,
		capabilities.MaxTextureSize, capabilities.MaxSampleCount, capabilities.MaxPushConstantSize);
	if (limits.MinUniformBufferOffsetAlignment == 0)
	{
		std::fprintf(stderr, "World.VulkanDevice: invalid device limits\n");
		return 1;
	}
	device->WaitIdle();
	std::printf("World.VulkanDevice: ok\n");
	return 0;
}
