#include "wldpch.h"
#include "Framebuffer.h"

#include "World/Renderer/Renderer.h"
#include "World/Platform/OpenGL/OpenGLFramebuffer.h"

namespace World
{
	Ref<Framebuffer> World::Framebuffer::Create(const FramebufferSpecification& spec)
	{
		switch (World::Renderer::GetAPI())
		{
			case RendererAPI::API::None:
				WLD_CORE_ASSERT(false, "RendererAPI::None is currently not supported!");
				return nullptr;
			case RendererAPI::API::OpenGL:
				return CreateRef<OpenGLFramebuffer>(spec);
		}
		WLD_CORE_ASSERT(false, "Unknown RendererAPI!");
		return nullptr;

	}
}