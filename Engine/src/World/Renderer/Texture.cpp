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
			case RendererAPI::API::Vulkan:
			{
				// Texture2D 是 UI 侧纹理(WUI 图标/子纹理):主窗口始终保有
				// OpenGL 上下文,两种后端下 UI 纹理都以 GL 纹理承载;
				// 场景侧需要时由 Rhi::WrapTexture2D 桥接为 RHI 纹理。
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
			case RendererAPI::API::Vulkan:
			{
				return CreateRef<OpenGLTexture2D>(width, height);
			}
		}
		WLD_CORE_ASSERT(false, "Unknown RendererAPI!");
		return nullptr;
	}
}
