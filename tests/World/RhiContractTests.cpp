#include "World/RHI/Rhi.h"

#include <cstdio>
#include <cstdlib>

namespace
{
	void Check(bool condition, int line)
	{
		if (!condition)
		{
			std::fprintf(stderr, "World.RhiContract: FAILED at line %d\n", line);
			std::exit(1);
		}
	}
#define CHECK(expression) Check(static_cast<bool>(expression), __LINE__)
}

int main()
{
	static_assert(WORLD_RHI_ABI_VERSION == 2u, "RHI ABI version must be 2");

	// 每个 POD 描述符默认构造 + 关键字段可赋值:验证合同头可独立编译。
	World::Rhi::BufferDesc buffer;
	buffer.Size = 64;
	buffer.Usage = World::Rhi::BufferUsageVertex | World::Rhi::BufferUsageUniform;
	CHECK(buffer.Usage & World::Rhi::BufferUsageVertex);

	World::Rhi::TextureDesc texture;
	texture.Format = World::Rhi::Format::R8G8B8A8_SRGB;
	texture.Extent = { 256, 256, 1 };
	texture.Usage = World::Rhi::TextureUsageSampled;
	CHECK(texture.MipLevels == 1);

	World::Rhi::RenderPassDesc pass;
	World::Rhi::RenderPassAttachment color;
	color.Format = World::Rhi::Format::B8G8R8A8_UNORM;
	color.Load = World::Rhi::LoadOp::Clear;
	color.Store = World::Rhi::StoreOp::Store;
	pass.Attachments.push_back(color);
	World::Rhi::SubpassDesc subpass;
	subpass.ColorAttachments.push_back({ 0, World::Rhi::AttachmentLayout::ColorAttachment });
	subpass.DepthStencilAttachment.Index = UINT32_MAX;
	pass.Subpasses.push_back(subpass);
	pass.Dependencies.push_back({ UINT32_MAX, 0,
		World::Rhi::PipelineStageColorAttachmentOutput,
		World::Rhi::PipelineStageFragmentShader,
		World::Rhi::AccessColorAttachmentWrite,
		World::Rhi::AccessShaderRead,
		false });
	CHECK(!subpass.HasDepthStencil());

	World::Rhi::PipelineDesc pipeline;
	pipeline.Topology = World::Rhi::PrimitiveTopology::TriangleList;
	pipeline.Cull = World::Rhi::CullMode::Back;
	pipeline.DepthClamp = true;
	pipeline.SampleMask = 0x1;
	World::Rhi::StencilState stencil;
	stencil.Pass = World::Rhi::StencilOp::Replace;
	CHECK(stencil.Pass == World::Rhi::StencilOp::Replace);
	CHECK(World::Rhi::ShaderStageFlag(World::Rhi::ShaderStage::Vertex) !=
		World::Rhi::ShaderStageFlag(World::Rhi::ShaderStage::Fragment));

	World::Rhi::DescriptorSetLayoutDesc layout;
	layout.Bindings.push_back({ 0, World::Rhi::DescriptorType::CombinedImageSampler,
		World::Rhi::ShaderStageFlag(World::Rhi::ShaderStage::Fragment), 1 });
	CHECK(layout.Bindings[0].Count == 1);

	World::Rhi::ResourceBarrier barrier;
	barrier.Before = World::Rhi::ResourceState::Undefined;
	barrier.After = World::Rhi::ResourceState::ShaderReadOnly;
	CHECK(barrier.MipLevelCount == 1);

	World::Rhi::Capabilities capabilities;
	CHECK(capabilities.MaxSampleCount >= 1);
	CHECK(capabilities.TextureCompressionBC || !capabilities.TextureCompressionBC);   // 字段可读

	World::Rhi::SemaphoreCreateDesc semaphore;
	semaphore.Timeline = true;
	semaphore.InitialValue = 3;
	CHECK(semaphore.InitialValue == 3);

	World::Rhi::SwapchainDesc swapchain;
	swapchain.Present = World::Rhi::PresentMode::Mailbox;
	swapchain.Space = World::Rhi::ColorSpace::SrgbNonlinear;
	CHECK(swapchain.Present == World::Rhi::PresentMode::Mailbox);

	World::Rhi::DeviceLimits limits;
	CHECK(limits.MinUniformBufferOffsetAlignment >= 1);

	World::Rhi::ClearValue depthClear;
	depthClear.IsDepthStencil = true;
	depthClear.DepthStencil.Depth = 1.0f;
	CHECK(depthClear.IsDepthStencil);

	std::printf("World.RhiContract: all checks passed\n");
	return 0;
}
