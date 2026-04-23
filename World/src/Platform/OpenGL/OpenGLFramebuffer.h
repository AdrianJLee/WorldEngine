#pragma once
#include "World/Renderer/Framebuffer.h"

namespace World
{

	class OpenGLFramebuffer :public Framebuffer
	{
	public:
		OpenGLFramebuffer(const FramebufferSpecification& spec);
		virtual ~OpenGLFramebuffer();
		virtual void Resize(uint32_t width, uint32_t height) override;
		void Invalidate();
		virtual const FramebufferSpecification& GetSpecification() const override { return m_Specification; }
		virtual uint32_t GetColorAttachmentRendererID(size_t index = 0) const override
		{
			WLD_CORE_ASSERT(index < m_ColorAttachments.size(), "Color attachment index out of bounds");
			return m_ColorAttachments[index];
		}

		virtual void Bind() override;
		virtual void Unbind() override;
		virtual int ReadPixel(uint32_t attachmentIndex, int x, int y) override;
		virtual void ClearAttachment(uint32_t attachmentIndex, int value) override;
	private:
		uint32_t m_RendererID = 0;
		FramebufferSpecification m_Specification;

		std::vector<FramebufferTextureSpecification> m_ColorAttachmentSpecifications;
		std::vector<uint32_t> m_ColorAttachments;

		FramebufferTextureSpecification m_DepthAttachmentSpecification = FramebufferTextureFormat::None;
		uint32_t m_DepthAttachment = 0;
	};
}

