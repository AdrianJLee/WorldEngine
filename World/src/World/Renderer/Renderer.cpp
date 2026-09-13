#include "wldpch.h"
#include "Renderer.h"

#include "World/Core/Application.h"
#include "World/Renderer/RenderCommand.h"
#include "World/Renderer/VertexArray.h"
#include "World/Renderer/Shader.h"
#include "World/Renderer/Renderer2D.h"

#include <glad/glad.h>

#include <fstream>

namespace World
{
	Renderer::SceneData* Renderer::m_SceneData = new Renderer::SceneData;
	Rhi::Handle<Rhi::Device> Renderer::m_Device = nullptr;
	void Renderer::Init()
	{
		// RHI 设备:W5 只接线 OpenGL 后端;设备在 GL 上下文创建后初始化。
		std::string error;
		m_Device = Rhi::CreateDevice(Rhi::Backend::Auto, {}, &error);
		WLD_CORE_ASSERT(m_Device, "Failed to create RHI device: {0}", error);

		RenderCommand::Init();

		Renderer2D::Init();
	}
	void Renderer::SetRequestedRenderer(const std::string& name)
	{
		const Rhi::Backend requested = name == "vulkan" ? Rhi::Backend::Vulkan
			: name == "opengl" ? Rhi::Backend::OpenGL
			: Rhi::Backend::Auto;

		std::string chosen;
		const Rhi::Backend actual = Rhi::ResolveBackend(requested, &chosen);
		RendererAPI::SetAPI(actual == Rhi::Backend::Vulkan ? RendererAPI::API::Vulkan : RendererAPI::API::OpenGL);
		if (requested == Rhi::Backend::Vulkan && actual != Rhi::Backend::Vulkan)
			WLD_CORE_WARN("Renderer '{0}' requested but unavailable; running {1}", name, chosen);
		else
			WLD_CORE_INFO("RHI backend selected: {0}", chosen);
	}
	void Renderer::OnWindowResize(uint32_t width, uint32_t height)
	{
		RenderCommand::SetViewport(0, 0, width, height);
	}

	void Renderer::Submit(const Ref<class Shader>& shader, const Ref<class VertexArray>& vertexArray, const  glm::mat4& transform)
	{
		shader->Bind();

		WLD_CORE_ERROR("Fix this.");
		//std::dynamic_pointer_cast<OpenGLShader>(shader)->UploadUniformMat4("u_ViewProjection", m_SceneData->ViewProjectionMatrix);
		//std::dynamic_pointer_cast<OpenGLShader>(shader)->UploadUniformMat4("u_Transform", transform);

		//vertexArray->Bind();
		// TODO: 这里应该允许用户指定索引数量，而不是每次都使用整个索引缓冲区的大小
		RenderCommand::DrawIndexed(vertexArray);
	}

	void Renderer::CaptureFrame(const std::filesystem::path& path)
	{
		if (!Application::HasInstance())
			return;
		const uint32_t width = Application::Get().GetWindow().GetWidth();
		const uint32_t height = Application::Get().GetWindow().GetHeight();
		std::vector<uint8_t> pixels(static_cast<size_t>(width) * height * 3);
		glReadBuffer(GL_BACK);
		glPixelStorei(GL_PACK_ALIGNMENT, 1);
		glReadPixels(0, 0, static_cast<GLsizei>(width), static_cast<GLsizei>(height),
			GL_RGB, GL_UNSIGNED_BYTE, pixels.data());

		std::ofstream file(path, std::ios::binary | std::ios::trunc);
		if (!file)
		{
			WLD_CORE_ERROR("[capture] cannot open {0}", path.string());
			return;
		}
		file << "P6\n" << width << " " << height << "\n255\n";
		for (uint32_t row = 0; row < height; ++row)
		{
			const uint32_t sourceRow = height - 1 - row;
			file.write(reinterpret_cast<const char*>(pixels.data() + static_cast<size_t>(sourceRow) * width * 3),
				static_cast<std::streamsize>(width) * 3);
		}
		WLD_CORE_INFO("[capture] wrote {0} ({1}x{2})", path.string(), width, height);
	}
}
