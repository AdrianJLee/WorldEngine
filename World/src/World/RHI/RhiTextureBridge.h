#pragma once

#include "World/RHI/RhiDevice.h"
#include "World/Renderer/Texture.h"

namespace World::Rhi
{
	// 桥接旧渲染资源到 RHI(编辑器显示/拾取仍走 GL id;迁到 RHI 后移除)。
	WLD_API Handle<Texture> WrapTexture2D(const Handle<Device>& device, const Ref<Texture2D>& texture);
	WLD_API uint32_t FramebufferId(const Handle<Framebuffer>& framebuffer);
	WLD_API uint32_t FramebufferAttachmentId(const Handle<Framebuffer>& framebuffer, size_t index);
	WLD_API int FramebufferReadPixel(const Handle<Framebuffer>& framebuffer, uint32_t attachmentIndex, int x, int y);
	// 把 framebuffer 的颜色附件 blit 到窗口默认帧缓冲(仅 OpenGL 后端有效;
	// Vulkan 呈现走 Swapchain,此函数返回 false)。
	WLD_API bool BlitFramebufferToBackbuffer(const Handle<Framebuffer>& framebuffer, Extent2D extent);
}
