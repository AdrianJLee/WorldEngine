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
		m_AttachmentIDs.resize(passDesc.Attachments.size(), 0);
		std::vector<GLenum> drawBuffers;
		for (size_t i = 0; i < passDesc.Attachments.size() && i < desc.Attachments.size(); i++)
		{
			const auto& attachment = passDesc.Attachments[i];
			const auto texture = std::dynamic_pointer_cast<OpenGLTexture>(desc.Attachments[i]);
			if (!texture)
				continue;
			m_AttachmentIDs[i] = texture->GetID();

			if (IsDepthStencilFormat(attachment.Format))
				glNamedFramebufferTexture(m_ID, GL_DEPTH_STENCIL_ATTACHMENT, texture->GetID(), 0);
			else
			{
				glNamedFramebufferTexture(m_ID, GL_COLOR_ATTACHMENT0 + static_cast<GLenum>(i), texture->GetID(), 0);
				drawBuffers.push_back(GL_COLOR_ATTACHMENT0 + static_cast<GLenum>(i));
			}
		}
		if (!drawBuffers.empty())
			glNamedFramebufferDrawBuffers(m_ID, static_cast<GLsizei>(drawBuffers.size()), drawBuffers.data());

		if (glCheckNamedFramebufferStatus(m_ID, GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
			WLD_CORE_WARN("OpenGL framebuffer '{0}' is incomplete", desc.DebugName);
		// 诊断(WLD_GL_TRACE_DRAW):把 FBO id 与它挂的纹理对起来 ——
		// "预览画到了哪个目标、读回读的是哪张纹理"必须能对上号。
		static const bool traceFramebuffers = std::getenv("WLD_GL_TRACE_DRAW") != nullptr;
		if (traceFramebuffers)
		{
			std::string attachments;
			for (size_t i = 0; i < m_AttachmentIDs.size(); ++i)
			{
				if (i)
					attachments += ",";
				attachments += std::to_string(m_AttachmentIDs[i]);
			}
			WLD_CORE_INFO("[gl-fbo] id={0} name='{1}' attachments=[{2}] ctx={3}", m_ID, desc.DebugName,
				attachments, reinterpret_cast<uintptr_t>(wglGetCurrentContext()));
		}
	}

	OpenGLFramebuffer::~OpenGLFramebuffer()
	{
		if (m_ID)
			glDeleteFramebuffers(1, &m_ID);
	}

	uint32_t OpenGLFramebuffer::GetAttachmentID(size_t index) const
	{
		return index < m_AttachmentIDs.size() ? m_AttachmentIDs[index] : 0;
	}

	int OpenGLFramebuffer::ReadPixel(uint32_t attachmentIndex, int x, int y)
	{
		if (attachmentIndex >= m_AttachmentIDs.size())
			return -1;
		glBindFramebuffer(GL_READ_FRAMEBUFFER, m_ID);
		glReadBuffer(GL_COLOR_ATTACHMENT0 + attachmentIndex);
		int pixel = -1;
		glReadPixels(x, y, 1, 1, GL_RED_INTEGER, GL_INT, &pixel);
		glReadBuffer(GL_NONE);
		glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
		return pixel;
	}
}
