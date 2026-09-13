#pragma once
#include "World/Core/Export.h"
#include "World/Renderer/RendererAPI.h"
#include "World/RHI/Rhi.h"

namespace World
{
	class Renderer
	{
	public:
		static void Init();
		static void OnWindowResize(uint32_t width, uint32_t height);

		static void Submit(const Ref<class Shader>& shader, const Ref<class VertexArray>& vertexArray, const  glm::mat4& transform = glm::mat4(1.0));

		inline static RendererAPI::API GetAPI() { return RendererAPI::GetAPI(); }

		// 按 project.we.yaml 的 renderer 字段选择后端;W5 阶段 Vulkan 未接线时
		// 自动降级 OpenGL 并告警。宿主在加载 manifest 后调用。
		static void SetRequestedRenderer(const std::string& name);
		static Rhi::Handle<Rhi::Device> GetDevice() { return m_Device; }
		// 开发验证:把当前默认帧缓冲读回并写 PPM(渲染基线截图)。
		static void CaptureFrame(const std::filesystem::path& path);
	private:
		struct SceneData
		{
			glm::mat4 ViewProjectionMatrix;
		};

		static WLD_API SceneData* m_SceneData;
		static WLD_API Rhi::Handle<Rhi::Device> m_Device;
	};
}


