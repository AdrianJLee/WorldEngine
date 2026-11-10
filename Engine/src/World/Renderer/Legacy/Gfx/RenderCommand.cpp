#include "wldpch.h"
#include "World/Renderer/Legacy/Gfx/RenderCommand.h"

#include "World/Renderer/Legacy/Gfx/RendererAPI.h"
#include "World/Renderer/Legacy/OpenGL/OpenGLRendererAPI.h"

namespace World
{
	RendererAPI* RenderCommand::s_RendererAPI = new OpenGLRendererAPI();

}