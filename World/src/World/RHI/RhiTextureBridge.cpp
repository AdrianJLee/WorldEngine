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
}
