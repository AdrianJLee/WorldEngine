#include "wldpch.h"
#include "World/RHI/RhiFramebufferBridge.h"
#include "World/RHI/OpenGL/OpenGLRenderPass.h"

#include <glad/glad.h>

namespace World::Rhi
{
	uint32_t FramebufferAttachmentId(const Handle<Framebuffer>& framebuffer, size_t index)
	{
		const auto gl = std::dynamic_pointer_cast<OpenGL::OpenGLFramebuffer>(framebuffer);
		return gl ? gl->GetAttachmentID(index) : 0;
	}

	uint32_t FramebufferId(const Handle<Framebuffer>& framebuffer)
	{
		const auto gl = std::dynamic_pointer_cast<OpenGL::OpenGLFramebuffer>(framebuffer);
		return gl ? gl->GetID() : 0;
	}

	int FramebufferReadPixel(const Handle<Framebuffer>& framebuffer, uint32_t attachmentIndex, int x, int y)
	{
		const auto gl = std::dynamic_pointer_cast<OpenGL::OpenGLFramebuffer>(framebuffer);
		return gl ? gl->ReadPixel(attachmentIndex, x, y) : -1;
	}

	bool BlitFramebufferToBackbuffer(const Handle<Framebuffer>& framebuffer, Extent2D extent)
	{
		const auto gl = std::dynamic_pointer_cast<OpenGL::OpenGLFramebuffer>(framebuffer);
		if (!gl || extent.Width == 0 || extent.Height == 0)
			return false;
		const GLuint colorId = gl->GetAttachmentID(0);
		if (!colorId)
			return false;
		GLuint readFbo = 0;
		glCreateFramebuffers(1, &readFbo);
		glNamedFramebufferTexture(readFbo, GL_COLOR_ATTACHMENT0, colorId, 0);
		glBindFramebuffer(GL_READ_FRAMEBUFFER, readFbo);
		glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
		glBlitFramebuffer(0, 0, static_cast<GLint>(extent.Width), static_cast<GLint>(extent.Height),
			0, 0, static_cast<GLint>(extent.Width), static_cast<GLint>(extent.Height),
			GL_COLOR_BUFFER_BIT, GL_NEAREST);
		glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
		glDeleteFramebuffers(1, &readFbo);
		return true;
	}
}
