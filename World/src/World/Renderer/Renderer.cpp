#include "wldpch.h"
#include "Renderer.h"

#include "World/Core/Asset/ProjectManifest.h"
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
	static Rhi::Handle<Rhi::DescriptorSetLayout> s_GlobalDescriptorSetLayout;
	static std::string s_BackendName = "opengl";

	void Renderer::Init()
	{
		// 按 project.we.yaml 的 renderer 选择后端;Vulkan 不可用时降级 OpenGL。
		std::string requested = "opengl";
		std::filesystem::path manifestPath;
		if (World::Asset::ProjectManifest::Locate(std::filesystem::current_path(), &manifestPath))
		{
			std::string manifestError;
			World::Asset::ProjectManifest manifest;
			if (World::Asset::ProjectManifest::Load(manifestPath, &manifest, &manifestError))
				requested = manifest.Renderer;
		}
		Init(requested);
	}

	void Renderer::Init(const std::string& backend)
	{
		if (m_Device)
			Shutdown();

		const Rhi::Backend requestedBackend =
			backend == "vulkan" ? Rhi::Backend::Vulkan : Rhi::Backend::OpenGL;
		std::string error;
		m_Device = Rhi::CreateDevice(requestedBackend, {}, &error);
		if (!m_Device)
		{
			WLD_CORE_WARN("Requested backend '{0}' unavailable ({1}); falling back to OpenGL",
				backend, error);
			error.clear();
			m_Device = Rhi::CreateDevice(Rhi::Backend::OpenGL, {}, &error);
		}
		WLD_CORE_ASSERT(m_Device, "Failed to create RHI device: {0}", error);

		// ShaderCompiler 依据 API 决定返回 SPIR-V 或 GLSL。
		RendererAPI::SetAPI(requestedBackend == Rhi::Backend::Vulkan && m_Device->GetCapabilities().BackendName == "Vulkan"
			? RendererAPI::API::Vulkan : RendererAPI::API::OpenGL);
		s_BackendName = RendererAPI::GetAPI() == RendererAPI::API::Vulkan ? "vulkan" : "opengl";

		RenderCommand::Init();
		Renderer2D::Init();
		WLD_CORE_INFO("RHI backend initialized: {0}", s_BackendName);
	}

	void Renderer::Shutdown()
	{
		WLD_PROFILE_FUNCTION();
		Renderer2D::Shutdown();
		s_GlobalDescriptorSetLayout = nullptr;
		m_Device = nullptr;
		s_BackendName = "opengl";
	}

	bool Renderer::SetRequestedRenderer(const std::string& name)
	{
		const std::string requested = name == "vulkan" ? "vulkan" : "opengl";
		return requested != s_BackendName;
	}

	std::string Renderer::GetBackendName()
	{
		return s_BackendName;
	}

	Rhi::Handle<Rhi::DescriptorSetLayout> Renderer::GetGlobalDescriptorSetLayout()
	{
		if (!s_GlobalDescriptorSetLayout && m_Device)
		{
			Rhi::DescriptorSetLayoutDesc desc;
			desc.Bindings.push_back({ 0, Rhi::DescriptorType::UniformBuffer,
				Rhi::ShaderStageFlag(Rhi::ShaderStage::Vertex), 1 });
			s_GlobalDescriptorSetLayout = m_Device->CreateDescriptorSetLayout(desc);
		}
		return s_GlobalDescriptorSetLayout;
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

	void Renderer::CaptureFramebuffer(const std::filesystem::path& path, uint32_t fbo, uint32_t width, uint32_t height)
	{
		if (width == 0 || height == 0)
			return;
		const GLint previous = 0;
		glGetIntegerv(GL_FRAMEBUFFER_BINDING, const_cast<GLint*>(&previous));
		glBindFramebuffer(GL_FRAMEBUFFER, fbo);

		std::vector<uint8_t> pixels(static_cast<size_t>(width) * height * 3);
		glReadBuffer(GL_COLOR_ATTACHMENT0);
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
		glBindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(previous));
		WLD_CORE_INFO("[capture] wrote framebuffer {0} ({1}x{2})", path.string(), width, height);
	}
}
