#include "wldpch.h"
#include "Shader.h"

#include "World/Renderer/Renderer.h"
#include "Platform/OpenGL/OpenGLShader.h"
namespace World
{
	Ref<Shader> Shader::Create()
	{
		switch (World::Renderer::GetAPI())
		{
			case RendererAPI::API::None:
				WLD_CORE_ASSERT(false, "RendererAPI::None is currently not supported!");
				return nullptr;
			case RendererAPI::API::OpenGL:
				return CreateRef<OpenGLShader>();
			case RendererAPI::API::Vulkan:
				// 旧 Vulkan 骨架已随 P1 W6 删除;Vulkan 走 RHI(World/RHI/Vulkan) +
				// RhiShader,不再经过这里。
				WLD_CORE_ASSERT(false, "Legacy Shader::Create is not available on Vulkan; use RHI shaders.");
				return nullptr;
		}
		WLD_CORE_ASSERT(false, "Unknown RendererAPI!");
		return nullptr;
	}
}
