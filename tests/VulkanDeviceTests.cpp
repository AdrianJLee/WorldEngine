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

	// 资源类冒烟:创建/销毁一轮,验证合同对象在 Vulkan 上可实例化。
	World::Rhi::Handle<World::Rhi::RenderPass> pass;
	World::Rhi::Handle<World::Rhi::Framebuffer> framebuffer;
	{
		using namespace World::Rhi;
		BufferDesc bufferDesc;
		bufferDesc.Size = 256;
		bufferDesc.Usage = BufferUsageVertex | BufferUsageUniform;
		const Handle<Buffer> buffer = device->CreateBuffer(bufferDesc);
		if (!buffer)
			return 1;
		buffer->SetData("hello", 5);

		TextureDesc textureDesc;
		textureDesc.Type = TextureType::Texture2D;
		textureDesc.Format = Format::R8G8B8A8_UNORM;
		textureDesc.Extent = { 4, 4, 1 };
		textureDesc.Usage = TextureUsageSampled;
		const Handle<Texture> texture = device->CreateTexture(textureDesc);
		if (!texture)
			return 1;
		texture->SetData("1234567890123456", 16);

		SamplerDesc samplerDesc;
		if (!device->CreateSampler(samplerDesc))
			return 1;

		RenderPassDesc passDesc;
		RenderPassAttachment color;
		color.Format = Format::R8G8B8A8_UNORM;
		color.Load = LoadOp::Clear;
		color.Store = StoreOp::Store;
		passDesc.Attachments = { color };
		SubpassDesc subpass;
		subpass.ColorAttachments = { { 0, AttachmentLayout::ColorAttachment } };
		passDesc.Subpasses = { subpass };
		pass = device->CreateRenderPass(passDesc);
		if (!pass)
			return 1;

		FramebufferDesc framebufferDesc;
		framebufferDesc.RenderPass = pass;
		framebufferDesc.Extent = { 4, 4 };
		framebufferDesc.Attachments = { texture };
		framebuffer = device->CreateFramebuffer(framebufferDesc);
		if (!framebuffer)
			return 1;

		DescriptorSetLayoutDesc layoutDesc;
		layoutDesc.Bindings.push_back({ 0, DescriptorType::UniformBuffer,
			ShaderStageFlag(ShaderStage::Vertex), 1 });
		const Handle<DescriptorSetLayout> layout = device->CreateDescriptorSetLayout(layoutDesc);
		const Handle<DescriptorSet> set = device->CreateDescriptorSet(layout);
		if (!set)
			return 1;
		DescriptorWrite write;
		write.Binding = 0;
		write.Type = DescriptorType::UniformBuffer;
		write.Buffer = buffer;
		set->Update({ write });

		if (!device->CreateFence(false) || !device->CreateFence(true))
			return 1;
		SemaphoreCreateDesc timelineDesc;
		timelineDesc.Timeline = true;
		if (!device->CreateSemaphore() || !device->CreateSemaphore(timelineDesc))
			return 1;
		if (!device->CreateQueryPool(QueryType::Occlusion, 2))
			return 1;
	}

	// 命令缓冲 + 队列 + 围栏:录制一个空渲染通道并提交执行。
	{
		using namespace World::Rhi;
		const Handle<CommandQueue> queue = device->CreateQueue();
		const Handle<CommandBuffer> commandBuffer = device->CreateCommandBuffer();
		const Handle<Fence> fence = device->CreateFence(false);
		if (!queue || !commandBuffer || !fence)
			return 1;
		commandBuffer->Begin();
		std::vector<ClearValue> clears(1);
		clears[0].Color = { 0.1f, 0.2f, 0.3f, 1.0f };
		commandBuffer->BeginRenderPass(pass, framebuffer, clears);
		commandBuffer->SetViewport({ 0, 0, 4, 4 });
		commandBuffer->SetScissor({ 0, 0, 4, 4 });
		commandBuffer->EndRenderPass();
		commandBuffer->End();
		queue->Submit({ { commandBuffer }, {}, {}, fence });
		fence->Wait();
	}
	device->WaitIdle();
	std::printf("World.VulkanDevice: ok\n");
	return 0;
}
