#pragma once
#include "World/Core/Export.h"
#include "World/Renderer/RendererAPI.h"
#include "World/RHI/Rhi.h"

namespace World
{
	// 每窗口一个呈现目标:主窗口由 Renderer 内部维护,独立浮动窗口按需创建。
	struct PresentTargetDesc
	{
		void* NativeWindow = nullptr;
		uint32_t Width = 0;
		uint32_t Height = 0;
		std::string DebugName;
	};
	struct PresentTarget;

	class Renderer
	{
	public:
		static void Init();
		static void Init(const std::string& backend);
		static void Shutdown();
		static void OnWindowResize(uint32_t width, uint32_t height);
		// ---- 帧呈现编排(UI/场景合成到窗口)----
		static PresentTarget* MainPresentTarget();
		static PresentTarget* CreatePresentTarget(const PresentTargetDesc& desc);
		static void DestroyPresentTarget(PresentTarget* target);
		static void ResizePresentTarget(PresentTarget* target, uint32_t width, uint32_t height);
		// target=nullptr 表示主窗口。
		static bool BeginFramePresent(PresentTarget* target = nullptr);
		static void EndFramePresent(PresentTarget* target = nullptr);
		static Rhi::Handle<Rhi::RenderPass> GetPresentRenderPass();
		static Rhi::Handle<Rhi::Framebuffer> GetPresentFramebuffer();
		static Rhi::Handle<Rhi::Texture> GetPresentTarget();
		static void SubmitUi(const Rhi::Handle<Rhi::CommandBuffer>& commandBuffer);
		static void SubmitScene(const Rhi::Handle<Rhi::CommandBuffer>& commandBuffer,
			const Rhi::Handle<Rhi::Texture>& colorTexture);

		static void Submit(const Ref<class Shader>& shader, const Ref<class VertexArray>& vertexArray, const  glm::mat4& transform = glm::mat4(1.0));

		inline static RendererAPI::API GetAPI() { return RendererAPI::GetAPI(); }

		// 按 project.we.yaml 的 renderer 字段选择后端;W5 阶段 Vulkan 未接线时
		// 自动降级 OpenGL 并告警。宿主在加载 manifest 后调用。
		// 记录目标后端;返回是否与当前不同。实际重建由宿主在安全时机调
		// Shutdown()/Init(backend) 完成。
		static bool SetRequestedRenderer(const std::string& name);
		static std::string GetBackendName();
		static Rhi::Handle<Rhi::Device> GetDevice() { return m_Device; }
		// set 0 全局布局(相机 UBO binding 0),由 SceneRenderer 与 Renderer2D 管线共享。
		static Rhi::Handle<Rhi::DescriptorSetLayout> GetGlobalDescriptorSetLayout();
		// 开发验证:把当前默认帧缓冲读回并写 PPM(渲染基线截图)。
		static void CaptureFrame(const std::filesystem::path& path);
		// 读回指定离屏帧缓冲(场景渲染目标)写 PPM。
		static void CaptureFramebuffer(const std::filesystem::path& path, uint32_t fbo, uint32_t width, uint32_t height);
		// 读回当前上下文的默认帧缓冲(GL_BACK)写 PPM;独立窗口验证用。
		static void CaptureDefaultFramebuffer(const std::filesystem::path& path, uint32_t width, uint32_t height);
	private:
		struct SceneData
		{
			glm::mat4 ViewProjectionMatrix;
		};

		static WLD_API SceneData* m_SceneData;
		static WLD_API Rhi::Handle<Rhi::Device> m_Device;
	};
}


