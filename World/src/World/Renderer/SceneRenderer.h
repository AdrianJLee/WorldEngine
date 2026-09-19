#pragma once

#include "World/RHI/Rhi.h"
#include "World/Renderer/EditorCamera.h"
#include "World/Renderer/Framebuffer.h"
#include "World/Renderer/Renderer.h"
#include "World/Renderer/Mesh.h"
#include "World/Scene/Scene.h"

#include <glm/glm.hpp>

#include <unordered_set>

namespace World
{
	struct SceneRendererOptions
	{
		bool ShowGrid = true;
	};

	class SceneRenderer
	{
	public:
		void Init();
		void Shutdown();

		void BeginScene(Scene* scene, const SceneRendererOptions& options);
		void EndScene();

		// D5c-4a:骨骼动画的时间推进步长(秒)。宿主每帧在 SubmitScene 之前设置;
		// SceneRenderer 在收集 3D 绘制前调用 AnimationSystem::Update 推进
		// SkinnedMeshRendererComponent 的 Time 并刷新调色板。默认 0 = 不推进
		// (相机预览等"同一场景的第二个 SceneRenderer"保持 0,否则同帧会推进两次)。
		void SetDeltaSeconds(float deltaSeconds) { m_DeltaSeconds = deltaSeconds; }

		void SubmitScene(const Camera& camera, const glm::mat4& cameraTransform, Entity entity);
		void SubmitScene(const Camera& camera, const glm::mat4& cameraTransform);

		void OnResize(uint32_t width, uint32_t height);

		Ref<Framebuffer> GetTargetFramebuffer() const { return m_FramebufferView; }
		Rhi::Handle<Rhi::Texture> GetColorTexture() const { return m_ColorTexture; }
		Rhi::Handle<Rhi::Framebuffer> GetRhiTarget() const { return m_Framebuffer; }
		// P4-3:GetWidth/GetHeight 返回**渲染目标**尺寸(请求尺寸 × rendering.render_scale),
		// 拾取/读回/抓图都按它换算;需要"宿主请求的显示尺寸"的调用方(例如按窗口尺寸做
		// 节流判断的 Runtime)用下面两个请求尺寸读法,否则倍率非 1 时会误判尺寸一直变化。
		uint32_t GetRequestedWidth() const { return m_RequestedWidth; }
		uint32_t GetRequestedHeight() const { return m_RequestedHeight; }
		uint32_t GetWidth() const { return m_Width; }
		uint32_t GetHeight() const { return m_Height; }
		// D7-1c:后端无关地读回 entity-id 附件的一个像素(视口点选用)。
		// 坐标以**左上角为原点**(与 WUI 视口一致),越界返回 -1;
		// 返回 -1 也表示该像素没有实体(附件清屏值就是 -1)。
		int32_t ReadEntityIdAt(int32_t x, int32_t y);
		// 读回整张 entity-id 附件(显示朝向,行优先):自动化自检用(扫描可见实体再逐点验证)。
		bool ReadEntityIdBuffer(std::vector<int32_t>& outIds);
		// 开发验证:把颜色附件读回写 PPM。
		void CaptureFrame(const std::filesystem::path& path) const;

	private:
		void RecreateTargets(uint32_t width, uint32_t height);
		// P4-3:把"请求尺寸"按当前 render_scale 换算成渲染目标尺寸,尺寸/倍率变化时才重建;
		// 默认倍率 1.0 且尺寸未变时直接返回,不产生任何资源操作(基线逐字节不变)。
		void ApplyRenderScale();
		void RecordSubmit(const Camera& camera, const glm::mat4& cameraTransform, Entity selectedEntity);
		void RenderGeometry(const Camera& camera, const glm::mat4& cameraTransform);
		void RenderDebug(const Camera& camera, const glm::mat4& cameraTransform);
		// D5:网格/材质资产加载失败只警告一次(按路径去重),避免逐帧刷屏。
		void WarnOnce(const std::string& key, const std::string& message);

	private:
		Rhi::Handle<Rhi::Device> m_Device;
		// 帧深 2:命令缓冲/相机 UBO/描述符集按帧槽位环形,允许 CPU 录制与 GPU 执行重叠。
		// 与 Renderer::FramesInFlight 同源(见 Renderer.h 的说明)。
		static constexpr uint32_t kFramesInFlight = Renderer::FramesInFlight;
		Rhi::Handle<Rhi::CommandBuffer> m_CommandBuffers[kFramesInFlight];
		Rhi::Handle<Rhi::RenderPass> m_RenderPass;
		Rhi::Handle<Rhi::Framebuffer> m_Framebuffer;
		Rhi::Handle<Rhi::Texture> m_ColorTexture;
		Rhi::Handle<Rhi::Texture> m_EntityTexture;
		Rhi::Handle<Rhi::Texture> m_DepthTexture;
		// ---- P4-4b:MSAA(rendering.msaa;启动期参数)----
		// Init 时从 RenderSettings::Msaa() 记录一次(设备上限已折算)。==1 时上面三张
		// 单采样纹理就是全部附件(与旧结构逐字节一致);>1 时颜色 / 实体 id / 深度三类
		// 附件按生效采样数创建在下面三个句柄里,并把结果 resolve 到单采样的
		// m_ColorTexture / m_EntityTexture(渲染通道附件下标 3/4)。
		uint32_t m_Samples = 1;
		Rhi::Handle<Rhi::Texture> m_ColorMsaaTexture;
		Rhi::Handle<Rhi::Texture> m_EntityMsaaTexture;
		Rhi::Handle<Rhi::Texture> m_DepthMsaaTexture;
		Rhi::Handle<Rhi::Buffer> m_CameraBuffers[kFramesInFlight];
		// D4:每帧槽位的灯光 UBO(方向光/点光/环境光/阴影矩阵,std140;见 Renderer3D.h)。
		Rhi::Handle<Rhi::Buffer> m_LightBuffers[kFramesInFlight];
		Rhi::Handle<Rhi::DescriptorSet> m_GlobalDescriptorSets[kFramesInFlight];

		uint32_t FrameSlot() const;

		// ---- P1b D8b:GPU 时间戳(rendering.gpu_timing,默认关)----
		// 每帧槽位一套查询池 + 读回缓冲:本帧写时间戳、该槽位 3 帧后被复用时才读上一轮
		// 结果,所以测量本身不需要 WaitIdle(代价只有一次 16B 拷贝与一次 Map)。
		void InitGpuTiming();
		void BeginGpuTiming(uint32_t slot);
		void EndGpuTiming(uint32_t slot);
		double ReadGpuTiming(uint32_t slot);
		bool m_GpuTiming = false;
		Rhi::Handle<Rhi::QueryPool> m_TimestampPools[kFramesInFlight];
		Rhi::Handle<Rhi::Buffer> m_TimestampBuffers[kFramesInFlight];
		bool m_TimestampPending[kFramesInFlight] = {};
		double m_LastGpuMilliseconds = 0.0;

		// m_Width/m_Height = 渲染目标尺寸(实际创建的附件/视口尺寸)。
		uint32_t m_Width = 1280;
		uint32_t m_Height = 720;
		// P4-3:宿主请求的显示尺寸(OnResize 记录;编辑器视口/运行时窗口)与已套用的倍率
		// (哨兵初值 0 不在合法范围;Init 第一次是否创建由"目标还不存在"本身决定)。
		uint32_t m_RequestedWidth = 1280;
		uint32_t m_RequestedHeight = 720;
		float m_AppliedRenderScale = 0.0f;
		// D5c-4a:骨骼动画步长(SetDeltaSeconds;默认 0 = 不推进)。
		float m_DeltaSeconds = 0.0f;
		Scene* m_ActiveScene = nullptr;
		SceneRendererOptions m_Options;
		Ref<Framebuffer> m_FramebufferView;
		// WLD_DEBUG_CUBE 用的调试网格(D2b:3D 通道双后端冒烟基线)。
		Ref<Mesh> m_DebugCube;
		Ref<Mesh> m_DebugPlane;
		Ref<Mesh> m_DebugSphere;   // D3:sphere 原语的共享网格(材质预览/半球体实体)
		std::unordered_set<std::string> m_WarnedPaths;
	};
}
