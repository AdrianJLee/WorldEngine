#include "wldpch.h"
#include "CommandBuffer.h"
#include "RendererAPI.h"
#include "World/Platform/OpenGL/OpenGLCommandBuffer.h"
namespace World
{
	Ref<CommandBuffer> CommandBuffer::Create()
	{
		switch (RendererAPI::GetAPI())
		{
			case RendererAPI::API::None:
				WLD_CORE_ASSERT(false, "RendererAPI::None is currently not supported!");
				return nullptr;
			case RendererAPI::API::OpenGL:
				return CreateRef<OpenGLCommandBuffer>();
			default:
				WLD_CORE_ASSERT(false, "Unknown RendererAPI!");
				return nullptr;
		}
	}
}