#include "wldpch.h"
#include "OpenGLRenderPass.h"
#include "OpenGLTexture.h"

#include <glad/glad.h>

namespace World::Rhi::OpenGL
{
	namespace
	{
		bool IsDepthStencilFormat(Format format)
		{
			return format == Format::D16_UNORM || format == Format::D32_SFLOAT ||
				format == Format::D24_UNORM_S8_UINT || format == Format::D32_SFLOAT_S8_UINT;
		}
	}

	OpenGLFramebuffer::OpenGLFramebuffer(const FramebufferDesc& desc)
		: m_Desc(desc)
	{
		WLD_CORE_ASSERT(desc.RenderPass, "FramebufferDesc requires a render pass");
		const auto& passDesc = desc.RenderPass->GetDesc();

		glCreateFramebuffers(1, &m_ID);
		for (size_t i = 0; i < passDesc.Attachments.size() && i < desc.Attachments.size(); i++)
		{
			const auto& attachment = passDesc.Attachments[i];
			const auto texture = std::dynamic_pointer_cast<OpenGLTexture>(desc.Attachments[i]);
			if (!texture)
				continue;

			if (IsDepthStencilFormat(attachment.Format))
				glNamedFramebufferTexture(m_ID, GL_DEPTH_STENCIL_ATTACHMENT, texture->GetID(), 0);
			else
				glNamedFramebufferTexture(m_ID, GL_COLOR_ATTACHMENT0 + static_cast<GLenum>(i), texture->GetID(), 0);
		}

		if (glCheckNamedFramebufferStatus(m_ID, GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
			WLD_CORE_WARN("OpenGL framebuffer '{0}' is incomplete", desc.DebugName);
	}

	OpenGLFramebuffer::~OpenGLFramebuffer()
	{
		if (m_ID)
			glDeleteFramebuffers(1, &m_ID);
	}
}
