#include "wldpch.h"
#include "World/RHI/RhiTextureBridge.h"
#include "World/RHI/OpenGL/OpenGLRenderPass.h"
#include "World/RHI/OpenGL/OpenGLTexture.h"

#include <glad/glad.h>

namespace World::Rhi
{
	Handle<Texture> WrapTexture2D(const Handle<Device>& device, const Ref<Texture2D>& texture)
	{
		if (!texture || !device)
			return nullptr;

		// Vulkan 后端:旧 GL 纹理没有 CPU 像素,读回后经设备上传。
		if (device->GetCapabilities().BackendName == "Vulkan")
		{
			const uint32_t width = texture->GetWidth();
			const uint32_t height = texture->GetHeight();
			glBindTexture(GL_TEXTURE_2D, texture->GetRendererID());
			std::vector<uint8_t> pixels(static_cast<size_t>(width) * height * 4);
			glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());

			TextureDesc desc;
			desc.Type = TextureType::Texture2D;
			desc.Format = Format::R8G8B8A8_UNORM;
			desc.Extent = { width, height, 1 };
			desc.Usage = TextureUsageSampled;
			Handle<Texture> out = device->CreateTexture(desc);
			out->SetData(pixels.data(), pixels.size());
			return out;
		}

		TextureDesc desc;
		desc.Type = TextureType::Texture2D;
		desc.Format = Format::R8G8B8A8_UNORM;
		desc.Extent = { texture->GetWidth(), texture->GetHeight(), 1 };
		desc.Usage = TextureUsageSampled;
		auto wrapped = CreateRef<OpenGL::OpenGLTexture>(desc);
		wrapped->Adopt(texture->GetRendererID());
		return wrapped;
	}

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
