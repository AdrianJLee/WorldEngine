#include "wldpch.h"
#include "RenderCommand.h"

#include "World/Renderer/RendererAPI.h"
#include "Platform/OpenGL/OpenGLRendererAPI.h"

namespace World
{
	RendererAPI* RenderCommand::s_RendererAPI = new OpenGLRendererAPI();

}