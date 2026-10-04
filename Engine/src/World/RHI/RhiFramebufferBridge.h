#pragma once

#include "World/RHI/RhiDevice.h"

namespace World::Rhi
{
	// 后端无关的 framebuffer 助手(编辑器显示/拾取需要;纹理那条 GL→RHI 桥已在
	// 2026-10-04 的纹理收口里删除 —— 所有纹理现在都是 RHI 句柄,不再需要桥接)。
	WLD_API uint32_t FramebufferId(const Handle<Framebuffer>& framebuffer);
	WLD_API uint32_t FramebufferAttachmentId(const Handle<Framebuffer>& framebuffer, size_t index);
	WLD_API int FramebufferReadPixel(const Handle<Framebuffer>& framebuffer, uint32_t attachmentIndex, int x, int y);
	// 把 framebuffer 的颜色附件 blit 到窗口默认帧缓冲(仅 OpenGL 后端有效;
	// Vulkan 呈现走 Swapchain,此函数返回 false)。
	WLD_API bool BlitFramebufferToBackbuffer(const Handle<Framebuffer>& framebuffer, Extent2D extent);
}
