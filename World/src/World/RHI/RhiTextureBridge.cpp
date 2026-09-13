#include "wldpch.h"
#include "World/RHI/RhiTextureBridge.h"
#include "World/RHI/OpenGL/OpenGLRenderPass.h"
#include "World/RHI/OpenGL/OpenGLTexture.h"

namespace World::Rhi
{
	Handle<Texture> WrapTexture2D(const Ref<Texture2D>& texture)
	{
		if (!texture)
			return nullptr;
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
