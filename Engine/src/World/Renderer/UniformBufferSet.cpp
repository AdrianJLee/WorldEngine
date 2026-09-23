#include "wldpch.h"
#include "UniformBufferSet.h"
#include "RendererAPI.h"

#include "World/Platform/OpenGL/OpenGLUniformBufferSet.h"
namespace World
{
	Ref<UniformBufferSet> UniformBufferSet::Create(uint32_t binding, int32_t size, uint32_t framesInFlight)
	{
		switch (RendererAPI::GetAPI())
		{
			case RendererAPI::API::None:
				WLD_CORE_ASSERT(false, "RendererAPI::None is currently not supported!");
				return nullptr;
			case RendererAPI::API::OpenGL:
				return CreateRef<OpenGLUniformBufferSet>(binding, size, framesInFlight);
			default:
				WLD_CORE_ASSERT(false, "Unknown RendererAPI!");
				return nullptr;
		}
	}
}