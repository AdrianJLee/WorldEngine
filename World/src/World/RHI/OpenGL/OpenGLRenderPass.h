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
		uint32_t GetAttachmentID(size_t index) const;
		int ReadPixel(uint32_t attachmentIndex, int x, int y);

	private:
		FramebufferDesc m_Desc;
		uint32_t m_ID = 0;
		std::vector<uint32_t> m_AttachmentIDs;
	};
}
