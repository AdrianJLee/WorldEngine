#include "wldpch.h"
#include "RenderPass.h"
#include "Renderer.h"
#include "Platform/OpenGL/OpenGLRenderPass.h"

namespace World
{
	Ref<RenderPass> RenderPass::Create(const RenderPassSpecification& spec)
	{
		switch (Renderer::GetAPI())
		{
			case RendererAPI::API::None:
				WLD_CORE_ASSERT(false, "RendererAPI::None is currently not supported!");
				return nullptr;
			case RendererAPI::API::OpenGL:
				return CreateRef<OpenGLRenderPass>(spec);

			default:
				WLD_CORE_ASSERT(false, "Unknown RendererAPI!");
				return nullptr;
		}
	}
}