#pragma once

#include "World/RHI/RhiRenderPass.h"

namespace World::Rhi::OpenGL
{
	class OpenGLRenderPass : public RenderPass
	{
	public:
		explicit OpenGLRenderPass(const RenderPassDesc& desc) : m_Desc(desc) {}
		const RenderPassDesc& GetDesc() const override { return m_Desc; }

	private:
		RenderPassDesc m_Desc;
	};

	class OpenGLFramebuffer : public Framebuffer
	{
	public:
		OpenGLFramebuffer(const FramebufferDesc& desc);
		~OpenGLFramebuffer() override;

		const FramebufferDesc& GetDesc() const override { return m_Desc; }
		uint32_t GetID() const { return m_ID; }

	private:
		FramebufferDesc m_Desc;
		uint32_t m_ID = 0;
	};
}
