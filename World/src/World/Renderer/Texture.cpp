#include "wldpch.h"
#include "Texture.h"

#include "World/Renderer/Renderer.h"
#include "Platform/OpenGL/OpenGLTexture.h"

namespace World
{
	Ref<Texture2D> Texture2D::Create(const std::string& path)
	{
		switch (Renderer::GetAPI())
		{
			case RendererAPI::API::None:
			{
				WLD_CORE_ASSERT(false, "RenderAPI::None is currently not supported!");
				return nullptr;
			}
			case RendererAPI::API::OpenGL:
			{
				return CreateRef<OpenGLTexture2D>(path);
			}
		}
		WLD_CORE_ASSERT(false, "Unknown RendererAPI!");
		return nullptr;
	}
	Ref<Texture2D> Texture2D::Create(uint32_t width, uint32_t height)
	{
		switch (Renderer::GetAPI())
		{
			case RendererAPI::API::None:
			{
				WLD_CORE_ASSERT(false, "RenderAPI::None is currently not supported!");
				return nullptr;
			}
			case RendererAPI::API::OpenGL:
			{
				return CreateRef<OpenGLTexture2D>(width, height);
			}
		}
		WLD_CORE_ASSERT(false, "Unknown RendererAPI!");
		return nullptr;
	}
}