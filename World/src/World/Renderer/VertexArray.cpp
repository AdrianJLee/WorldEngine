#include "wldpch.h"
#include "VertexArray.h"

#include "World/Renderer/Renderer.h"
#include "Platform/OpenGL/OpenGLVertexArray.h"

namespace World
{
	Ref<VertexArray> VertexArray::Create()
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
				return std::make_shared<OpenGLVertexArray>();
			}

		}
		WLD_CORE_ASSERT(false, "Unknown RenderAPI!");
		return nullptr;
	}
}