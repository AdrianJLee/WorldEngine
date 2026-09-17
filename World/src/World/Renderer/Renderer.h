#pragma once
#include "World/Core/Export.h"
#include "World/Renderer/RendererAPI.h"
#include "World/RHI/Rhi.h"

#include <cstdint>
#include <functional>

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
		// ---- 帧节拍与资源回收(B0:去阻塞同步) ----
		// 帧开始:等待本槽位的 fence(上一轮使用该槽位的提交已完成),并执行到期的延迟释放。
		static void BeginFrame();
		// 帧结束:推进帧槽位。
		static void EndFrame();
		static uint32_t FrameSlot();      // 当前帧槽位(0..kFramesInFlight-1)
		// 帧槽位数量(帧深)的唯一来源:所有"按帧槽位环形"的子系统
		// (场景命令缓冲/相机 UBO、WUI 命令缓冲/UBO/顶点索引缓冲等)必须用同一个值,
		// 否则槽位索引越界或跨帧复用仍在飞的资源。
		static constexpr uint32_t FramesInFlight = 3;
		static uint64_t FrameNumber();    // 单调递增的帧号
		// 延迟释放:GPU 可能仍在使用的资源改为"下一轮该槽位开始前"回收,不再用整队列 WaitIdle。
		static void QueueRelease(std::function<void()> release);
		// 等待 GPU 空闲(整设备排空)。只在重建交换链/后端切换/关闭等**低频**路径调用,
		// 用于保证随后释放的画面/信号量/描述符池不再被在飞命令引用。
		static void WaitForGpu();
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
		// ---- 设备销毁前的释放钩子 ----
		// 持有 RHI 句柄的子系统(WUI 后端、纹理注册表等)在这里登记释放回调:
		// 设备销毁前回调先跑,资源的析构才能带着有效设备执行。
		// 同一 owner 重复登记视为替换(幂等)。
		static void RegisterDeviceReleaseHook(void* owner, std::function<void()> hook);
		static void UnregisterDeviceReleaseHook(void* owner);
		// set 0 全局布局(相机 UBO binding 0),由 SceneRenderer 与 Renderer2D 管线共享。
		static Rhi::Handle<Rhi::DescriptorSetLayout> GetGlobalDescriptorSetLayout();
		// 开发验证:把当前默认帧缓冲读回并写 PPM(渲染基线截图)。
		static void CaptureFrame(const std::filesystem::path& path);
		// 读回指定离屏帧缓冲(场景渲染目标)写 PPM。
		static void CaptureFramebuffer(const std::filesystem::path& path, uint32_t fbo, uint32_t width, uint32_t height);
		// 读回当前上下文的默认帧缓冲(GL_BACK)写 PPM;独立窗口验证用。
		static void CaptureDefaultFramebuffer(const std::filesystem::path& path, uint32_t width, uint32_t height);
		// 后端无关的纹理读回(RHI CopyTextureToBuffer + Map):把任意采样纹理写成 PPM。
		// 双后端截图基线必须走这条(旧的 glReadPixels 路径在 Vulkan 下只能抓到全黑)。
		static bool CaptureTexture(const std::filesystem::path& path,
			const Rhi::Handle<Rhi::Texture>& texture, uint32_t width, uint32_t height);
		// 整窗抓图(交换链图像 / 默认帧缓冲)。命令层只登记请求,**抓取时机**由引擎决定:
		// 必须在 UI 通道提交之后、EndFramePresent(→Present 布局转换 + Present)之前调用
		// FlushPresentCaptures —— 早了会拍到上一帧/空白(实测全黑),而且提前改布局会让
		// 随后的 UI 通道 InitialLayout 与实际布局冲突(实测 VK_ERROR_DEVICE_LOST)。
		static void RequestPresentCapture(class PresentTarget* target,
			const std::filesystem::path& path, uint32_t width, uint32_t height);
		static void FlushPresentCaptures();
		// 开发诊断(GL):把当前上下文里挂起的 GL 错误全部取出并记日志。
		// "命令录了但什么都没画出来"时,GL 只会把失败原因留在错误队列里(静默丢弃 draw),
		// 所以判定"预览/离屏通道到底画没画进去"必须先看这里。返回清掉的错误个数。
		static int DrainGLErrors(const char* tag);
	private:
		struct SceneData
		{
			glm::mat4 ViewProjectionMatrix;
		};

		static WLD_API SceneData* m_SceneData;
		static WLD_API Rhi::Handle<Rhi::Device> m_Device;
	};
}


