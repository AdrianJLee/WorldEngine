#include "wldpch.h"
#include "Shader.h"

#include "World/Renderer/Renderer.h"
#include "Platform/OpenGL/OpenGLShader.h"
#include "Platform/Vulkan/DataHandles/VulkanShader.h"
#include "Platform/Vulkan/VulkanContext.h"
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
				auto device = VulkanContext::Get()->GetDevice();
				return CreateRef<VulkanShader>(device);
		}
		WLD_CORE_ASSERT(false, "Unknown RendererAPI!");
		return nullptr;
	}
}