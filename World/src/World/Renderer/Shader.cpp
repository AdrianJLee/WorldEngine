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
				return std::make_shared<OpenGLShader>();
		}
		WLD_CORE_ASSERT(false, "Unknown RendererAPI!");
		return nullptr;
	}
}