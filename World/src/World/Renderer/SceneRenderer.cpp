#include "wldpch.h"
#include "SceneRenderer.h"

#include "World/Renderer/Renderer.h"
#include "World/Renderer/Renderer2D.h"
#include "World/Renderer/Renderer3D.h"
#include "World/Renderer/AnimationSystem.h"
#include "World/Renderer/MaterialLibrary.h"
#include "World/Renderer/Mesh.h"
#include "World/Renderer/ProjectionConventions.h"
#include "World/Renderer/FrustumCull.h"
#include "World/Renderer/RenderSettings.h"
#include "World/Scene/Components.h"
#include "World/Scene/Hierarchy.h"
#include "World/RHI/RhiTextureBridge.h"
#include "World/Core/Thread/JobSystem.h"

#include <glm/gtc/matrix_transform.hpp>

#include <array>
#include <chrono>
#include <cfloat>
#include <cmath>
#include <map>
#include <tuple>

namespace World
{
	uint32_t SceneRenderer::FrameSlot() const
	{
		return static_cast<uint32_t>(Renderer::FrameSlot());
	}

	// ---- P1b D8b:GPU 时间戳 ----

	void SceneRenderer::InitGpuTiming()
	{
		m_GpuTiming = RenderSettings::Get().GpuTiming;
		if (!m_GpuTiming)
			return;
		const Rhi::Handle<Rhi::Device>& device = Renderer::GetDevice();
		if (!device || !device->GetCapabilities().TimestampQueries)
		{
			// 设备不支持时间戳 → 明确退回关闭,而不是返回 0 让上层误以为"GPU 不耗时"。
			if (device && !device->GetCapabilities().TimestampQueries)
				WLD_CORE_WARN("rendering.gpu_timing 已开启,但当前设备不支持时间戳查询;GPU 耗时保持 0");
			m_GpuTiming = false;
			return;
		}
		for (uint32_t slot = 0; slot < kFramesInFlight; ++slot)
		{
			m_TimestampPools[slot] = device->CreateQueryPool(Rhi::QueryType::Timestamp, 2);
			Rhi::BufferDesc bufferDesc;
			bufferDesc.Size = sizeof(uint64_t) * 2;
			bufferDesc.Usage = Rhi::BufferUsageTransferDst;
			bufferDesc.Memory = Rhi::MemoryHint::HostVisible;
			bufferDesc.DebugName = "SceneRenderer.GpuTimestamps";
			m_TimestampBuffers[slot] = device->CreateBuffer(bufferDesc);
			m_TimestampPending[slot] = false;
		}
	}

	void SceneRenderer::BeginGpuTiming(uint32_t slot)
	{
		if (!m_GpuTiming || !m_TimestampPools[slot])
			return;
		m_CommandBuffers[slot]->ResetQueryPool(m_TimestampPools[slot], 0, 2);
		m_CommandBuffers[slot]->WriteTimestamp(m_TimestampPools[slot], 0);
	}

	void SceneRenderer::EndGpuTiming(uint32_t slot)
	{
		if (!m_GpuTiming || !m_TimestampPools[slot])
			return;
		m_CommandBuffers[slot]->WriteTimestamp(m_TimestampPools[slot], 1);
		m_CommandBuffers[slot]->CopyQueryResults(m_TimestampPools[slot], m_TimestampBuffers[slot], 0, 2);
		m_TimestampPending[slot] = true;
	}

	double SceneRenderer::ReadGpuTiming(uint32_t slot)
	{
		if (!m_GpuTiming || !m_TimestampPending[slot] || !m_TimestampBuffers[slot])
			return m_LastGpuMilliseconds;
		uint64_t stamps[2] = {};
		if (void* mapped = m_TimestampBuffers[slot]->Map(0, sizeof(stamps)))
		{
			std::memcpy(stamps, mapped, sizeof(stamps));
			m_TimestampBuffers[slot]->Unmap();
		}
		m_TimestampPending[slot] = false;
		if (stamps[1] <= stamps[0])
			return m_LastGpuMilliseconds;
		const double periodNs = Renderer::GetDevice() ? Renderer::GetDevice()->GetTimestampPeriodNanoseconds() : 1.0;
		m_LastGpuMilliseconds = static_cast<double>(stamps[1] - stamps[0]) * periodNs / 1.0e6;
		return m_LastGpuMilliseconds;
	}

	namespace
	{
		// 旧 Framebuffer 接口适配器:编辑器显示/拾取仍走 GL id,迁到 RHI 后移除。
		class RhiFramebufferAdapter final : public Framebuffer
		{
		public:
			SceneRenderer* Owner = nullptr;
			Rhi::Handle<Rhi::Framebuffer> Target;
			uint32_t Width = 0;
			uint32_t Height = 0;

			void Resize(uint32_t width, uint32_t height) override
			{
				if (Owner)
					Owner->OnResize(width, height);
			}

			const FramebufferSpecification& GetSpecification() const override
			{
				static FramebufferSpecification spec;
				return spec;
			}

			uint32_t GetColorAttachmentRendererID(size_t index = 0) const override
			{
				return Rhi::FramebufferAttachmentId(Target, index);
			}

			void Bind() override {}
			void Unbind() override {}

			int ReadPixel(uint32_t attachmentIndex, int x, int y) override
			{
				return Rhi::FramebufferReadPixel(Target, attachmentIndex, x, y);
			}

			void ClearAttachment(uint32_t, int) override {}
		};
	}

	void SceneRenderer::Init()
	{
		WLD_PROFILE_FUNCTION();
		if (m_Device)
			Shutdown();
		m_Device = Renderer::GetDevice();
		// D8b:GPU 时间戳池/读回缓冲(rendering.gpu_timing;设备不支持时自降级为关闭)。
		InitGpuTiming();
		for (uint32_t slot = 0; slot < kFramesInFlight; ++slot)
		{
			m_CommandBuffers[slot] = m_Device->CreateCommandBuffer("SceneRenderer");
		}

		// P4-4b:MSAA 生效采样数(启动期参数;设备上限已在 RenderSettings::Msaa 里折算)。
		// msaa==1 时下面的一切与旧代码逐字节一致:三附件全 Count1、ResolveAttachments 为空。
		m_Samples = RenderSettings::Msaa();
		const Rhi::SampleCount sceneSamples = static_cast<Rhi::SampleCount>(m_Samples);
		const bool multisampled = m_Samples > 1;

		Rhi::RenderPassDesc passDesc;
		Rhi::RenderPassAttachment color;
		color.Format = Rhi::Format::R8G8B8A8_UNORM;
		color.Samples = sceneSamples;
		color.Load = Rhi::LoadOp::Clear;
		color.Store = Rhi::StoreOp::Store;
		// 三个附件都是"每帧 Clear、内容不保留":初始布局声明为 Undefined,让渲染通道
		// 自己完成 隐式转换(如果声明成 ColorAttachment 而实际还在 Undefined,
		// 渲染通道会跳过转换 → 附件被以 Undefined 布局使用,验证层报 vkCmdDraw-None-09600)。
		color.InitialLayout = Rhi::AttachmentLayout::Undefined;
		color.FinalLayout = Rhi::AttachmentLayout::ColorAttachment;
		color.Clear.Color = { 0.1f, 0.1f, 0.1f, 1.0f };

		Rhi::RenderPassAttachment entityId;
		entityId.Format = Rhi::Format::R32_SINT;
		entityId.Samples = sceneSamples;
		entityId.Load = Rhi::LoadOp::Clear;
		entityId.Store = Rhi::StoreOp::Store;
		entityId.InitialLayout = Rhi::AttachmentLayout::Undefined;
		entityId.FinalLayout = Rhi::AttachmentLayout::ColorAttachment;
		const int minusOne = -1;
		std::memcpy(&entityId.Clear.Color, &minusOne, sizeof(int));

		Rhi::RenderPassAttachment depth;
		depth.Format = Rhi::Format::D24_UNORM_S8_UINT;
		depth.Samples = sceneSamples;
		depth.Load = Rhi::LoadOp::Clear;
		depth.Store = Rhi::StoreOp::Store;
		depth.InitialLayout = Rhi::AttachmentLayout::Undefined;
		depth.FinalLayout = Rhi::AttachmentLayout::DepthStencilAttachment;
		depth.Clear.IsDepthStencil = true;
		depth.Clear.DepthStencil.Depth = 1.0f;

		passDesc.Attachments = { color, entityId, depth };
		Rhi::SubpassDesc subpass;
		subpass.ColorAttachments = {
			{ 0, Rhi::AttachmentLayout::ColorAttachment },
			{ 1, Rhi::AttachmentLayout::ColorAttachment },
		};
		subpass.DepthStencilAttachment = { 2, Rhi::AttachmentLayout::DepthStencilAttachment };
		if (multisampled)
		{
			// P4-4b:五附件结构(与 Renderer2D/3D 的兼容通道、两个预览面板逐项一致,
			// 这是 Vulkan 复用同一批管线的前提):
			//   0 = 多采样颜色(R8G8B8A8_UNORM)   1 = 多采样实体 id(R32_SINT)
			//   2 = 多采样深度(D24_UNORM_S8_UINT,不 resolve)
			//   3 = 单采样颜色 resolve 目标(= 现有 m_ColorTexture)
			//   4 = 单采样实体 id resolve 目标(= 现有 m_EntityTexture)
			// resolve 目标必须出现在 FramebufferDesc::Attachments 的**同一附件下标**上。
			// 布局声明与旧的单采样颜色/实体附件一致(Initial Undefined = 每帧整体重写,
			// Final ColorAttachment = 帧末那条 ColorAttachment→ShaderReadOnly 屏障的前态)。
			Rhi::RenderPassAttachment colorResolve;
			colorResolve.Format = Rhi::Format::R8G8B8A8_UNORM;
			colorResolve.Samples = Rhi::SampleCount::Count1;
			colorResolve.Load = Rhi::LoadOp::DontCare;   // resolve 会整体覆盖
			colorResolve.Store = Rhi::StoreOp::Store;
			colorResolve.InitialLayout = Rhi::AttachmentLayout::Undefined;
			colorResolve.FinalLayout = Rhi::AttachmentLayout::ColorAttachment;
			Rhi::RenderPassAttachment entityResolve;
			entityResolve.Format = Rhi::Format::R32_SINT;
			entityResolve.Samples = Rhi::SampleCount::Count1;
			entityResolve.Load = Rhi::LoadOp::DontCare;
			entityResolve.Store = Rhi::StoreOp::Store;
			entityResolve.InitialLayout = Rhi::AttachmentLayout::Undefined;
			entityResolve.FinalLayout = Rhi::AttachmentLayout::ColorAttachment;
			passDesc.Attachments.push_back(colorResolve);    // 3
			passDesc.Attachments.push_back(entityResolve);   // 4
			subpass.ResolveAttachments = { 3, 4 };
			passDesc.DebugName = "SceneRenderer.ScenePass";
			WLD_CORE_INFO("[msaa] 场景渲染通道:颜色 / 实体 id / 深度 {0}x 多采样,resolve 到单采样颜色 / 实体 id 目标",
				m_Samples);
		}
		passDesc.Subpasses = { subpass };
		m_RenderPass = m_Device->CreateRenderPass(passDesc);

		Rhi::BufferDesc cameraDesc;
		cameraDesc.Size = sizeof(glm::mat4);
		cameraDesc.Usage = Rhi::BufferUsageUniform;
		cameraDesc.Memory = Rhi::MemoryHint::HostVisible;
		// D4:灯光 UBO(std140,496 字节:阴影矩阵 + 阴影参数 + 环境光 + 数量 + 8 盏灯)。
		Rhi::BufferDesc lightDesc;
		lightDesc.Size = sizeof(LightUniforms);
		lightDesc.Usage = Rhi::BufferUsageUniform;
		lightDesc.Memory = Rhi::MemoryHint::HostVisible;
		lightDesc.DebugName = "SceneRenderer.LightUBO";
		for (uint32_t slot = 0; slot < kFramesInFlight; ++slot)
		{
			m_CameraBuffers[slot] = m_Device->CreateBuffer(cameraDesc);
			m_LightBuffers[slot] = m_Device->CreateBuffer(lightDesc);
			m_GlobalDescriptorSets[slot] = m_Device->CreateDescriptorSet(
				Renderer::GetGlobalDescriptorSetLayout());
			// set0 的全部 binding 必须**一次**写完:GL 后端的描述符集 Update 是"整体替换"
			// 语义(binding 2 = 灯光 UBO、binding 3 = 阴影贴图,见 Renderer::GetGlobalDescriptorSetLayout)。
			std::vector<Rhi::DescriptorWrite> writes;
			Rhi::DescriptorWrite cameraWrite;
			cameraWrite.Binding = 0;
			cameraWrite.Type = Rhi::DescriptorType::UniformBuffer;
			cameraWrite.Buffer = m_CameraBuffers[slot];
			writes.push_back(cameraWrite);
			for (const Rhi::DescriptorWrite& extra : Renderer3D::MakeGlobalLightingWrites(m_LightBuffers[slot]))
				writes.push_back(extra);
			m_GlobalDescriptorSets[slot]->Update(writes);
		}

		m_FramebufferView = CreateRef<RhiFramebufferAdapter>();
		static_cast<RhiFramebufferAdapter*>(m_FramebufferView.get())->Owner = this;
		// P4-3:目标尺寸 = 请求尺寸 × rendering.render_scale。默认 1.0 时第一次换算
		// 得到的就是原来的 1280×720(同尺寸、同一条创建路径)。
		ApplyRenderScale();
	}

	void SceneRenderer::Shutdown()
	{
		WLD_PROFILE_FUNCTION();
		m_ActiveScene = nullptr;
		m_Framebuffer = nullptr;
		m_FramebufferView = nullptr;
		m_ColorTexture = nullptr;
		m_EntityTexture = nullptr;
		m_DepthTexture = nullptr;
		// P4-4b:多采样附件(msaa>1 时才有)同样在设备销毁前放掉。
		m_ColorMsaaTexture = nullptr;
		m_EntityMsaaTexture = nullptr;
		m_DepthMsaaTexture = nullptr;
		m_RenderPass = nullptr;
		for (uint32_t slot = 0; slot < kFramesInFlight; ++slot)
		{
			m_CommandBuffers[slot] = nullptr;
			m_CameraBuffers[slot] = nullptr;
			m_LightBuffers[slot] = nullptr;
			m_GlobalDescriptorSets[slot] = nullptr;
			// D8b:时间戳查询池/读回缓冲必须在设备销毁前释放(同 D4 阴影资源的口径)。
			m_TimestampPools[slot] = nullptr;
			m_TimestampBuffers[slot] = nullptr;
			m_TimestampPending[slot] = false;
		}
		m_GpuTiming = false;
		m_LastGpuMilliseconds = 0.0;
		m_FramebufferView = nullptr;
		m_Device = nullptr;
	}

	namespace
	{
		// P4-3:请求尺寸 → 渲染目标尺寸(宽高都乘倍率,四舍五入,最少 1 像素)。
		// 取整口径固定为 std::lround(0.5 远离零),再 clamp 到 ≥1 —— 倍率 0.25 时
		// 再小的请求尺寸也不会变成 0(0 尺寸纹理/帧缓冲非法)。
		uint32_t ScaleRenderExtent(uint32_t extent, float scale)
		{
			const long scaled = std::lround(static_cast<double>(extent) * static_cast<double>(scale));
			return static_cast<uint32_t>(std::max(1L, scaled));
		}
	}

	void SceneRenderer::ApplyRenderScale()
	{
		// 设备未就绪(Init 之前 / Shutdown 之后)不碰目标;Init 末尾会自己调一次。
		if (!m_Device)
			return;
		// 夹取:清单加载期已拒绝越界值(ProjectManifest::ValidateManifest);这里再夹一次,
		// 避免"直接 RenderSettings::Set 出 0/负数"把目标算成 0 尺寸。
		const float scale = std::clamp(RenderSettings::RenderScale(),
			Asset::RenderingSettings::MinRenderScale, Asset::RenderingSettings::MaxRenderScale);
		const uint32_t width = ScaleRenderExtent(m_RequestedWidth, scale);
		const uint32_t height = ScaleRenderExtent(m_RequestedHeight, scale);
		// 无变化(默认倍率 1.0 的常见路径):不创建也不释放任何资源,与旧代码同一条路径。
		if (m_Framebuffer && m_AppliedRenderScale == scale && width == m_Width && height == m_Height)
			return;
		m_AppliedRenderScale = scale;
		RecreateTargets(width, height);
	}

	void SceneRenderer::RecreateTargets(uint32_t width, uint32_t height)
	{
		m_Width = std::max(1u, width);
		m_Height = std::max(1u, height);
		// 旧目标可能仍被在飞的帧引用:交给延迟释放队列,在栅栏通过后回收。
		if (m_Framebuffer || m_ColorTexture || m_EntityTexture || m_DepthTexture ||
			m_ColorMsaaTexture || m_EntityMsaaTexture || m_DepthMsaaTexture)
		{
			auto oldFramebuffer = m_Framebuffer;
			auto oldColor = m_ColorTexture;
			auto oldEntity = m_EntityTexture;
			auto oldDepth = m_DepthTexture;
			auto oldColorMsaa = m_ColorMsaaTexture;
			auto oldEntityMsaa = m_EntityMsaaTexture;
			auto oldDepthMsaa = m_DepthMsaaTexture;
			Renderer::QueueRelease([oldFramebuffer, oldColor, oldEntity, oldDepth,
				oldColorMsaa, oldEntityMsaa, oldDepthMsaa]() {});
		}

		Rhi::TextureDesc colorDesc;
		colorDesc.Type = Rhi::TextureType::Texture2D;
		colorDesc.Format = Rhi::Format::R8G8B8A8_UNORM;
		colorDesc.Extent = { m_Width, m_Height, 1 };
		colorDesc.Usage = Rhi::TextureUsageColorAttachment | Rhi::TextureUsageSampled;
		m_ColorTexture = m_Device->CreateTexture(colorDesc);

		Rhi::TextureDesc entityDesc;
		entityDesc.Type = Rhi::TextureType::Texture2D;
		entityDesc.Format = Rhi::Format::R32_SINT;
		entityDesc.Extent = { m_Width, m_Height, 1 };
		entityDesc.Usage = Rhi::TextureUsageColorAttachment;
		m_EntityTexture = m_Device->CreateTexture(entityDesc);

		Rhi::TextureDesc depthDesc;
		depthDesc.Type = Rhi::TextureType::Texture2D;
		depthDesc.Format = Rhi::Format::D24_UNORM_S8_UINT;
		depthDesc.Extent = { m_Width, m_Height, 1 };
		depthDesc.Usage = Rhi::TextureUsageDepthStencilAttachment;
		m_DepthTexture = m_Device->CreateTexture(depthDesc);

		Rhi::FramebufferDesc framebufferDesc;
		framebufferDesc.RenderPass = m_RenderPass;
		framebufferDesc.Extent = { m_Width, m_Height };
		if (m_Samples > 1)
		{
			// P4-4b:三类附件按生效采样数创建(resolve 目标仍是上面两张单采样纹理)。
			// 顺序必须与渲染通道附件表一致:0 颜色 / 1 实体 id / 2 深度 / 3 颜色 resolve / 4 实体 id resolve。
			const Rhi::SampleCount sceneSamples = static_cast<Rhi::SampleCount>(m_Samples);
			Rhi::TextureDesc msaaColorDesc = colorDesc;
			msaaColorDesc.Samples = sceneSamples;
			// 多采样颜色只做绘制附件(内容由 resolve 写入 m_ColorTexture,WUI/抓图不采样它)。
			msaaColorDesc.Usage = Rhi::TextureUsageColorAttachment;
			msaaColorDesc.DebugName = "SceneRenderer.ColorMSAA";
			m_ColorMsaaTexture = m_Device->CreateTexture(msaaColorDesc);
			Rhi::TextureDesc msaaEntityDesc = entityDesc;
			msaaEntityDesc.Samples = sceneSamples;
			msaaEntityDesc.DebugName = "SceneRenderer.EntityIdMSAA";
			m_EntityMsaaTexture = m_Device->CreateTexture(msaaEntityDesc);
			Rhi::TextureDesc msaaDepthDesc = depthDesc;
			msaaDepthDesc.Samples = sceneSamples;
			msaaDepthDesc.DebugName = "SceneRenderer.DepthMSAA";
			m_DepthMsaaTexture = m_Device->CreateTexture(msaaDepthDesc);
			framebufferDesc.Attachments = { m_ColorMsaaTexture, m_EntityMsaaTexture, m_DepthMsaaTexture,
				m_ColorTexture, m_EntityTexture };
		}
		else
		{
			framebufferDesc.Attachments = { m_ColorTexture, m_EntityTexture, m_DepthTexture };
		}
		m_Framebuffer = m_Device->CreateFramebuffer(framebufferDesc);

		if (m_FramebufferView)
		{
			auto* adapter = static_cast<RhiFramebufferAdapter*>(m_FramebufferView.get());
			adapter->Target = m_Framebuffer;
			adapter->Width = m_Width;
			adapter->Height = m_Height;
		}
	}

	void SceneRenderer::BeginScene(Scene* scene, const SceneRendererOptions& options)
	{
		// P4-3:帧起点重查 rendering.render_scale —— 编辑器面板改倍率后不重启、不缩放
		// 视口也能生效(BeginScene 在任何目标使用之前、上一帧提交之后运行)。倍率与尺寸
		// 都没变时 ApplyRenderScale 直接返回,不产生任何资源操作。
		ApplyRenderScale();
		m_ActiveScene = scene;
		m_Options = options;
	}

	void SceneRenderer::EndScene()
	{
		m_ActiveScene = nullptr;
	}

	void SceneRenderer::SubmitScene(const Camera& camera, const glm::mat4& cameraTransform)
	{
		RecordSubmit(camera, cameraTransform, Entity());
	}

	void SceneRenderer::SubmitScene(const Camera& camera, const glm::mat4& cameraTransform, Entity entity)
	{
		RecordSubmit(camera, cameraTransform, entity);
	}

	void SceneRenderer::RecordSubmit(const Camera& camera, const glm::mat4& cameraTransform, Entity selectedEntity)
	{
		if (!m_ActiveScene)
			return;

		// D8a:场景提交总耗时(统计阈值起点,与 Renderer 的帧时间口径不同:
		// 这里只量"收集 → 剔除 → 阴影 → 主通道 → 命令缓冲提交"这一段 CPU 时间)。
		const auto sceneStart = std::chrono::steady_clock::now();
		glm::mat4 viewProjection = camera.GetProjectionMatrix() * glm::inverse(cameraTransform);
		// 编辑/运行期都会改 Transform:每帧先重算层级世界矩阵,子实体才会跟随父实体
		// (此前只有序列化/Prefab 路径求解,见 Hierarchy.h)。
		Hierarchy::UpdateWorldTransforms(m_ActiveScene->m_Registry);
		// D5c-4a:先推进骨骼动画(写回 Time + 采样 → 节点世界矩阵 → 调色板),再收集绘制 ——
		// 蒙皮提交拿的是本帧的调色板。步长来自宿主 SetDeltaSeconds(默认 0 = 不推进)。
		AnimationSystem::Update(*m_ActiveScene, m_DeltaSeconds);
		// 后端适配:场景渲染到**离屏纹理**(WUI 用固定 UV 贴到视口),
		// 因此 Vulkan 只补深度范围、**不翻 Y**(翻了会在视口里上下颠倒,实测)。
		viewProjection = AdaptViewProjectionForOffscreen(viewProjection, Renderer::GetBackendName() == "vulkan");
		const uint32_t slot = FrameSlot();
		// D8b:该槽位 3 帧后被复用,上一轮提交的 GPU 工作已完成(帧栅栏)→ 读回上一轮时间戳。
		const double gpuMilliseconds = ReadGpuTiming(slot);
		m_CameraBuffers[slot]->SetData(&viewProjection, sizeof(glm::mat4));

		// ---- 3D 网格收集(D2c/D3) ----
		// 收集前移到阴影通道之前:方向光阴影的正交矩阵要覆盖本帧所有网格实体的世界包围盒。
		// 有 MaterialPath 时走材质(贴图/粗糙度/透明),否则沿用 Color 常量色(旧行为)。
		struct MeshDraw
		{
			entt::entity Entity;
			const glm::mat4* Model = nullptr;
			Ref<Mesh> MeshAsset;
			// D5:UINT32_MAX = 整网格提交(内置 primitive / 无 submesh 的资产);
			// 否则只提交该 submesh(独立对象槽位 + 该 submesh 槽位的材质)。
			uint32_t SubmeshIndex = UINT32_MAX;
			Ref<Material> MaterialAsset;
			glm::vec4 Color { 1.0f };
			bool Transparent = false;
			// D5c-4a:蒙皮绘制(走 Renderer3D::SubmitSkinned/SubmitShadowSkinned;不进实例化合批)。
			// Palette 是 AnimationSystem 当帧缓存的该实体调色板;nullptr = 该实体本帧没有蒙皮结果。
			bool Skinned = false;
			const std::vector<glm::mat4>* Palette = nullptr;
			// D8a:世界空间 AABB(逐子网格;视锥剔除用)。
			glm::vec3 WorldMin { 0.0f };
			glm::vec3 WorldMax { 0.0f };
		};
		std::vector<MeshDraw> draws;
		{
			auto meshView = m_ActiveScene->m_Registry.view<TransformComponent, MeshRendererComponent>();
			for (auto entity : meshView)
			{
				// D5c-4a:同时挂 SkinnedMeshRendererComponent 的实体交给下面的蒙皮收集块,
				// 这里跳过以免同一个实体画两遍(只挂 MeshRendererComponent 的实体逐字节不变)。
				if (m_ActiveScene->m_Registry.all_of<SkinnedMeshRendererComponent>(entity))
					continue;
				const auto& [transform, meshComponent] =
					meshView.get<TransformComponent, MeshRendererComponent>(entity);
				// D5:MeshPath 指向 .wmodel 时优先加载(进程内缓存);坏文件/读不到时回退到
				// 内置 primitive 并只警告一次,不阻断整帧渲染。
				Ref<Mesh> mesh;
				if (!meshComponent.MeshPath.empty())
				{
					std::string meshError;
					mesh = Mesh::LoadWModel(meshComponent.MeshPath, &meshError);
					if (!mesh)
						WarnOnce(meshComponent.MeshPath, "网格加载失败 '" + meshComponent.MeshPath + "': "
							+ meshError + "(回退到 Primitive)");
				}
				if (!mesh)
				{
					// D3:支持 sphere 原语(材质预览用;编辑器中也可直接摆球)。
					const bool plane = meshComponent.Primitive == "plane";
					const bool sphere = meshComponent.Primitive == "sphere";
					Ref<Mesh>& primitive = plane ? m_DebugPlane : (sphere ? m_DebugSphere : m_DebugCube);
					if (!primitive)
						primitive = plane ? Mesh::CreateUnitPlane(1.0f)
							: (sphere ? Mesh::CreateUnitSphere(1.0f, 32, 16) : Mesh::CreateUnitCube(1.0f));
					mesh = primitive;
				}
				if (!mesh)
					continue;

				// 实体级材质:非空 = **覆盖**该网格全部 submesh 的材质槽。
				// 加载失败(路径写错/文件坏)时回退到 Color/材质槽路径,不阻断整帧渲染。
				Ref<Material> overrideMaterial;
				if (!meshComponent.MaterialPath.empty())
				{
					std::string error;
					overrideMaterial = MaterialLibrary::Get().Load(meshComponent.MaterialPath, &error);
					if (!overrideMaterial)
						WarnOnce(meshComponent.MaterialPath, "材质加载失败 '" + meshComponent.MaterialPath
							+ "': " + error + "(回退到 Color/材质槽)");
				}
				// 层级实体用求解后的世界矩阵:直接提交本地矩阵会让子实体不跟随父实体
				// (实测"移动父项子项不动")。世界矩阵由本轮统一求解(见上方 UpdateWorldTransforms)。
				const glm::mat4* modelMatrix = &transform.Transform;
				if (m_ActiveScene->m_Registry.all_of<WorldTransformComponent>(entity))
					modelMatrix = &m_ActiveScene->m_Registry.get<WorldTransformComponent>(entity).Matrix;

				const auto makeDraw = [&](uint32_t submeshIndex, const Ref<Material>& material)
				{
					MeshDraw draw;
					draw.Entity = entity;
					draw.Model = modelMatrix;
					draw.MeshAsset = mesh;
					draw.SubmeshIndex = submeshIndex;
					draw.MaterialAsset = material;
					draw.Color = meshComponent.Color;
					draw.Transparent = material
						&& material->GetDesc().BlendMode == MaterialBlendMode::Transparent;
					// D8a:世界 AABB(逐子网格用子网格局部盒;整网格用整体盒)。
					const MeshBounds& localBounds = (submeshIndex != UINT32_MAX
						&& submeshIndex < mesh->GetSubmeshes().size())
						? mesh->GetSubmeshes()[submeshIndex].Bounds
						: mesh->GetBounds();
					TransformAabb(*modelMatrix, localBounds.Min, localBounds.Max,
						draw.WorldMin, draw.WorldMax);
					draws.push_back(std::move(draw));
				};

				if (mesh->HasSubmeshes() && !mesh->GetMeshes().empty())
				{
					// MeshIndex 越界(资产被替换/手填)回退到 mesh 0,不让实体整帧消失。
					uint32_t meshIndex = 0;
					if (meshComponent.MeshIndex > 0
						&& static_cast<size_t>(meshComponent.MeshIndex) < mesh->GetMeshes().size())
						meshIndex = static_cast<uint32_t>(meshComponent.MeshIndex);
					const MeshRange& range = mesh->GetMeshes()[meshIndex];
					const std::vector<std::string>& slots = mesh->GetMaterialSlots();
					for (uint32_t offset = 0; offset < range.SubmeshCount; ++offset)
					{
						const uint32_t submeshIndex = range.FirstSubmesh + offset;
						if (submeshIndex >= mesh->GetSubmeshes().size())
							break;
						Ref<Material> material = overrideMaterial;
						if (!material)
						{
							const int32_t slot = mesh->GetSubmeshes()[submeshIndex].MaterialSlot;
							if (slot >= 0 && static_cast<size_t>(slot) < slots.size() && !slots[slot].empty())
							{
								std::string slotError;
								material = MaterialLibrary::Get().Load(slots[slot], &slotError);
								if (!material)
									WarnOnce(slots[slot], "材质槽加载失败 '" + slots[slot] + "': " + slotError);
							}
						}
						makeDraw(submeshIndex, material);
					}
				}
				else
				{
					makeDraw(UINT32_MAX, overrideMaterial);
				}
			}
		}

		// ---- D5c-4a:蒙皮网格收集(SkinnedMeshRendererComponent) ----
		// 与静态路径同一套网格/材质/层级矩阵口径,只是多了"该实体本帧的调色板"并标记 Skinned。
		// 组件没有 Color 字段:常量色路径用白色(等价于 Renderer3D::Submit 的默认基色)。
		{
			auto skinnedView = m_ActiveScene->m_Registry.view<TransformComponent, SkinnedMeshRendererComponent>();
			for (auto entity : skinnedView)
			{
				const auto& [transform, skinned] =
					skinnedView.get<TransformComponent, SkinnedMeshRendererComponent>(entity);
				if (skinned.MeshPath.empty())
				{
					WarnOnce("skinned-path:" + std::to_string(static_cast<uint32_t>(entity)),
						"蒙皮网格缺少 MeshPath(实体 " + std::to_string(static_cast<uint32_t>(entity))
							+ "):跳过该实体的绘制");
					continue;
				}
				std::string meshError;
				Ref<Mesh> mesh = Mesh::LoadWModel(skinned.MeshPath, &meshError);
				if (!mesh)
				{
					WarnOnce(skinned.MeshPath, "网格加载失败 '" + skinned.MeshPath + "': " + meshError);
					continue;
				}
				// 实体级材质:非空 = 覆盖该网格全部 submesh 的材质槽(与静态路径同语义)。
				Ref<Material> overrideMaterial;
				if (!skinned.MaterialPath.empty())
				{
					std::string error;
					overrideMaterial = MaterialLibrary::Get().Load(skinned.MaterialPath, &error);
					if (!overrideMaterial)
						WarnOnce(skinned.MaterialPath, "材质加载失败 '" + skinned.MaterialPath
							+ "': " + error + "(回退到 Color/材质槽)");
				}
				// 层级实体用求解后的世界矩阵(与静态路径同一约定)。
				const glm::mat4* modelMatrix = &transform.Transform;
				if (m_ActiveScene->m_Registry.all_of<WorldTransformComponent>(entity))
					modelMatrix = &m_ActiveScene->m_Registry.get<WorldTransformComponent>(entity).Matrix;
				// AnimationSystem::Update 当帧算好的调色板;nullptr = 本帧取不到(读失败/非蒙皮)。
				const std::vector<glm::mat4>* palette = AnimationSystem::GetPalette(entity);

				const auto makeSkinnedDraw = [&](uint32_t submeshIndex, const Ref<Material>& material)
				{
					MeshDraw draw;
					draw.Entity = entity;
					draw.Model = modelMatrix;
					draw.MeshAsset = mesh;
					draw.SubmeshIndex = submeshIndex;
					draw.MaterialAsset = material;
					draw.Color = glm::vec4(1.0f);
					draw.Transparent = material
						&& material->GetDesc().BlendMode == MaterialBlendMode::Transparent;
					draw.Skinned = true;
					draw.Palette = palette;
					const MeshBounds& localBounds = (submeshIndex != UINT32_MAX
						&& submeshIndex < mesh->GetSubmeshes().size())
						? mesh->GetSubmeshes()[submeshIndex].Bounds
						: mesh->GetBounds();
					TransformAabb(*modelMatrix, localBounds.Min, localBounds.Max,
						draw.WorldMin, draw.WorldMax);
					draws.push_back(std::move(draw));
				};

				if (mesh->HasSubmeshes() && !mesh->GetMeshes().empty())
				{
					// MeshIndex 越界回退到 mesh 0(与静态路径同一规则)。
					uint32_t meshIndex = 0;
					if (skinned.MeshIndex > 0
						&& static_cast<size_t>(skinned.MeshIndex) < mesh->GetMeshes().size())
						meshIndex = static_cast<uint32_t>(skinned.MeshIndex);
					const MeshRange& range = mesh->GetMeshes()[meshIndex];
					const std::vector<std::string>& slots = mesh->GetMaterialSlots();
					for (uint32_t offset = 0; offset < range.SubmeshCount; ++offset)
					{
						const uint32_t submeshIndex = range.FirstSubmesh + offset;
						if (submeshIndex >= mesh->GetSubmeshes().size())
							break;
						Ref<Material> material = overrideMaterial;
						if (!material)
						{
							const int32_t slot = mesh->GetSubmeshes()[submeshIndex].MaterialSlot;
							if (slot >= 0 && static_cast<size_t>(slot) < slots.size() && !slots[slot].empty())
							{
								std::string slotError;
								material = MaterialLibrary::Get().Load(slots[slot], &slotError);
								if (!material)
									WarnOnce(slots[slot], "材质槽加载失败 '" + slots[slot] + "': " + slotError);
							}
						}
						makeSkinnedDraw(submeshIndex, material);
					}
				}
				else
				{
					makeSkinnedDraw(UINT32_MAX, overrideMaterial);
				}
			}
		}

		// ---- D8a:相机视锥剔除 ----
		// 主通道只提交视锥内的 draw;剔除掉的物体**不进阴影通道的判断**(见下:阴影用
		// 光源自己的正交视锥,否则"相机看不见但影子投进画面"的投影者会丢)。
		// D8a2:开关来自项目清单 `rendering.culling`(引擎用户可配置);
		// WLD_NO_CULL=1 仍可强制关闭(压力场景 A/B 基线用)。
		const bool cullingEnabled = RenderSettings::CullingEnabled();
		std::vector<uint32_t> visibleDraws;
		visibleDraws.reserve(draws.size());
		double cullMilliseconds = 0.0;
		{
			const auto cullStart = std::chrono::steady_clock::now();
			const FrustumPlanes cameraFrustum = ExtractFrustumPlanes(viewProjection);
			for (uint32_t index = 0; index < draws.size(); ++index)
			{
				const MeshDraw& draw = draws[index];
				if (!cullingEnabled
					|| AabbInFrustum(cameraFrustum, draw.WorldMin, draw.WorldMax))
					visibleDraws.push_back(index);
			}
			cullMilliseconds = std::chrono::duration<double, std::milli>(
				std::chrono::steady_clock::now() - cullStart).count();
		}
		uint32_t shadowCasters = 0;

		// ---- D4:灯光收集(registry 遍历顺序 = 截断顺序)+ 方向光阴影矩阵 + 灯光 UBO ----
		LightRig lightRig;
		{
			std::vector<DirectionalLightData> directionalLights;
			for (auto entity : m_ActiveScene->m_Registry.view<DirectionalLightComponent>())
			{
				const auto& light = m_ActiveScene->m_Registry.get<DirectionalLightComponent>(entity);
				directionalLights.push_back({ light.Color, light.Intensity, light.Direction, light.CastShadow });
			}
			std::vector<PointLightData> pointLights;
			for (auto entity : m_ActiveScene->m_Registry.view<PointLightComponent>())
			{
				const auto& light = m_ActiveScene->m_Registry.get<PointLightComponent>(entity);
				// 点光位置取实体世界位置(有层级时用求解后的世界矩阵,与网格同一约定)。
				glm::vec3 position { 0.0f };
				if (const auto* world = m_ActiveScene->m_Registry.try_get<WorldTransformComponent>(entity))
					position = glm::vec3(world->Matrix[3]);
				else if (const auto* transform = m_ActiveScene->m_Registry.try_get<TransformComponent>(entity))
					position = transform->Location;
				pointLights.push_back({ light.Color, light.Intensity, position, light.Range });
			}
			AmbientLightData ambientLight;
			bool hasAmbient = false;
			for (auto entity : m_ActiveScene->m_Registry.view<AmbientLightComponent>())
			{
				// 场景级:多盏时第一盏生效(与上限截断同一"registry 顺序"语义)。
				const auto& light = m_ActiveScene->m_Registry.get<AmbientLightComponent>(entity);
				ambientLight = { light.Color, light.Intensity };
				hasAmbient = true;
				break;
			}
			const bool glDepthConvention = Renderer::GetBackendName() != "vulkan";
			lightRig = Renderer3D::BuildLightRig(directionalLights, pointLights,
				hasAmbient ? &ambientLight : nullptr, glDepthConvention);
		}

		// D8a2:项目清单 `rendering.shadows=false` → 整条阴影通道关掉(投影者不提交、
		// 主通道不采样)。WLD_NO_SHADOWS=1 同样强制关闭(自动化)。
		if (!RenderSettings::ShadowsEnabled() && lightRig.ShadowCaster)
		{
			lightRig.ShadowCaster = false;
			lightRig.Uniforms.ShadowParams.x = 0.0f;
		}

		// 阴影矩阵:主方向光的正交视图(沿传播方向的反方向退到世界包围球外),盒子覆盖
		// 本帧全部网格实体。没有网格或没有 CastShadow 的主方向光时保持阴影禁用。
		std::vector<uint32_t> shadowDraws;
		if (lightRig.ShadowCaster && !draws.empty())
		{
			glm::vec3 boundsMin { FLT_MAX, FLT_MAX, FLT_MAX };
			glm::vec3 boundsMax { -FLT_MAX, -FLT_MAX, -FLT_MAX };
			for (const MeshDraw& draw : draws)
			{
				// D8a:直接用收集期算好的世界 AABB(逐子网格,更紧)。
				boundsMin = glm::min(boundsMin, draw.WorldMin);
				boundsMax = glm::max(boundsMax, draw.WorldMax);
			}
			const glm::vec3 center = (boundsMin + boundsMax) * 0.5f;
			const float radius = std::max(0.5f, glm::length((boundsMax - boundsMin) * 0.5f));
			const glm::vec3 direction = glm::vec3(lightRig.Uniforms.Lights[0].DirectionRange);
			const glm::vec3 up = std::fabs(direction.y) > 0.9f
				? glm::vec3(0.0f, 0.0f, 1.0f) : glm::vec3(0.0f, 1.0f, 0.0f);
			const glm::mat4 lightView = glm::lookAt(center - direction * (radius * 2.0f), center, up);
			const glm::mat4 lightProjection = glm::ortho(-radius, radius, -radius, radius,
				0.1f, radius * 4.0f);
			// 与离屏场景同一条投影适配:Vulkan 只补深度范围(z∈[0,1]),不翻 Y ——
			// 阴影 UV(depth 贴图行序)与主场景纹理同一约定,两个后端像素一致。
			glm::mat4 shadowViewProjection = AdaptViewProjectionForOffscreen(
				lightProjection * lightView, Renderer::GetBackendName() == "vulkan");
			Renderer3D::ApplyShadowCaster(lightRig, shadowViewProjection);

			// D8a:阴影通道按**光源正交视锥**剔除(相机视锥在这里不适用:视锥外的
			// 投影者仍可能把影子投进画面)。WLD_NO_CULL=1 时不做(与主通道同步 A/B)。
			if (cullingEnabled)
			{
				const FrustumPlanes lightFrustum = ExtractFrustumPlanes(shadowViewProjection);
				shadowDraws.reserve(draws.size());
				for (uint32_t index = 0; index < draws.size(); ++index)
				{
					const MeshDraw& draw = draws[index];
					if (AabbInFrustum(lightFrustum, draw.WorldMin, draw.WorldMax))
						shadowDraws.push_back(index);
				}
			}
		}
		if (!cullingEnabled && lightRig.ShadowCaster)
		{
			shadowDraws.clear();
			shadowDraws.reserve(draws.size());
			for (uint32_t index = 0; index < draws.size(); ++index)
				shadowDraws.push_back(index);
		}
		shadowCasters = static_cast<uint32_t>(shadowDraws.size());
		m_LightBuffers[slot]->SetData(&lightRig.Uniforms, sizeof(lightRig.Uniforms));

		// D8a:场景提交前后的 Renderer3D 计数器快照(单调累计)→ 差值即本帧增量。
		const Renderer3D::Statistics statsBeforeScene = Renderer3D::GetStats();
		m_CommandBuffers[slot]->Begin();

		// D4:方向光阴影通道(本帧 3D 主通道**之前**,同一命令缓冲):
		// depth-only 管线把投影者写进 2048² 深度图,主通道按 PCF 采样。
		// CPU 侧 steady_clock 计时进 Stats/日志("光照数量上限与耗时可见"验收条款)。
		double shadowPassMilliseconds = 0.0;
		if (lightRig.Uniforms.ShadowParams.x > 0.5f)
		{
			const auto shadowStart = std::chrono::steady_clock::now();
			std::vector<Rhi::ClearValue> shadowClears(2);
			shadowClears[1].IsDepthStencil = true;
			shadowClears[1].DepthStencil.Depth = 1.0f;
			m_CommandBuffers[slot]->BeginRenderPass(Renderer3D::GetShadowRenderPass(),
				Renderer3D::GetShadowFramebuffer(), shadowClears);
			// set0(相机 + 灯光 UBO):binding 2 提供 u_ShadowViewProjection;
			// 阴影管线不用 set2(材质),调用方只绑 0/1。
			m_CommandBuffers[slot]->BindDescriptorSet(m_GlobalDescriptorSets[slot], 0);
			Renderer3D::BeginShadowPass(m_CommandBuffers[slot]);
			// D8b-2:投影者按 (mesh, submesh) 分桶,桶内 ≥4 个实例才合批(阴影不看材质)。
			std::map<std::pair<const Mesh*, uint32_t>, std::vector<uint32_t>> shadowBuckets;
			if (RenderSettings::Get().Instancing)
			{
				for (const uint32_t drawIndex : shadowDraws)
				{
					const MeshDraw& draw = draws[drawIndex];
					// D5c-4a:蒙皮投影者不参与实例化合批(调色板逐物体,per-instance 通道里没有它)。
					if (draw.Skinned)
						continue;
					shadowBuckets[{ draw.MeshAsset.get(), draw.SubmeshIndex }].push_back(drawIndex);
				}
			}
			for (const uint32_t drawIndex : shadowDraws)
			{
				const MeshDraw& draw = draws[drawIndex];
				// D5c-4a:蒙皮投影者走蒙皮入口(否则影子留在绑定姿态)。调色板本帧取不到时:
				// 布局 1(模型没有 skin 数据)回退静态入口;布局 2 只跳过 —— 静态管线的顶点
				// 布局是 stride 32,读布局 2 的顶点缓冲会画出垃圾。
				if (draw.Skinned)
				{
					if (draw.Palette && !draw.Palette->empty())
					{
						Renderer3D::SubmitShadowSkinned(draw.MeshAsset, draw.SubmeshIndex, *draw.Model,
							draw.Palette->data(), static_cast<uint32_t>(draw.Palette->size()));
						continue;
					}
					if (draw.MeshAsset->GetVertexLayoutId() == Mesh::kVertexLayoutSkinned)
						continue;
				}
				const auto found = shadowBuckets.find({ draw.MeshAsset.get(), draw.SubmeshIndex });
				if (found != shadowBuckets.end() && found->second.size() >= 4)
					continue;   // 交给下面的实例化提交
				// D5:逐 submesh 提交(每条独立对象槽位),多材质模型的投影才完整。
				if (draw.SubmeshIndex == UINT32_MAX)
					Renderer3D::SubmitShadow(draw.MeshAsset, *draw.Model);
				else
					Renderer3D::SubmitShadowSubmesh(draw.MeshAsset, draw.SubmeshIndex, *draw.Model);
			}
			std::vector<glm::mat4> shadowTransforms;
			for (auto& [key, indices] : shadowBuckets)
			{
				if (indices.size() < 4)
					continue;
				shadowTransforms.clear();
				shadowTransforms.reserve(indices.size());
				for (const uint32_t drawIndex : indices)
					shadowTransforms.push_back(*draws[drawIndex].Model);
				const Ref<Mesh>& shadowMesh = draws[indices.front()].MeshAsset;
				if (Renderer3D::SubmitShadowInstanced(shadowMesh, key.second, shadowTransforms.data(),
					static_cast<uint32_t>(indices.size())) != 0)
					continue;
				// 回退:逐物体补交这一桶(与上面同一条路径)。
				for (const uint32_t drawIndex : indices)
				{
					const MeshDraw& draw = draws[drawIndex];
					if (draw.SubmeshIndex == UINT32_MAX)
						Renderer3D::SubmitShadow(draw.MeshAsset, *draw.Model);
					else
						Renderer3D::SubmitShadowSubmesh(draw.MeshAsset, draw.SubmeshIndex, *draw.Model);
				}
			}
			Renderer3D::EndShadowPass();
			m_CommandBuffers[slot]->EndRenderPass();
			shadowPassMilliseconds = std::chrono::duration<double, std::milli>(
				std::chrono::steady_clock::now() - shadowStart).count();
		}
		Renderer3D::ReportLighting(lightRig, shadowPassMilliseconds);

		std::vector<Rhi::ClearValue> clears(3);
		clears[0].Color = { 0.1f, 0.1f, 0.1f, 1.0f };
		const int minusOne = -1;
		std::memcpy(&clears[1].Color, &minusOne, sizeof(int));
		clears[2].IsDepthStencil = true;
		clears[2].DepthStencil.Depth = 1.0f;

		// D8b:起始时间戳在渲染通道**之外**写(vkCmdWriteTimestamp 不能在 render pass 内)。
		BeginGpuTiming(slot);
		m_CommandBuffers[slot]->BeginRenderPass(m_RenderPass, m_Framebuffer, clears);
		m_CommandBuffers[slot]->SetViewport({ 0, 0, static_cast<float>(m_Width), static_cast<float>(m_Height) });
		// 管线把视口/裁剪都设为动态状态,绑定后必须先设置再绘制
		// (VUID-vkCmdDrawIndexed-None-07832:动态裁剪未设置时状态未定义)。
		m_CommandBuffers[slot]->SetScissor({ 0, 0, m_Width, m_Height });
		m_CommandBuffers[slot]->BindDescriptorSet(m_GlobalDescriptorSets[slot]);

		// 开发钩子:WLD_DEBUG_CUBE=1 时在同一渲染通道里提交一个 3D 立方体,
		// 作为 3D 通道(Pipeline/深度/网格缓冲)在双后端下的冒烟基线(D2b)。
		if (std::getenv("WLD_DEBUG_CUBE"))
		{
			if (!m_DebugCube)
				m_DebugCube = Mesh::CreateUnitCube(1.0f);
			if (m_DebugCube)
			{
				Renderer3D::BeginScene(viewProjection, m_CommandBuffers[slot]);
				// 放在画面右上方,避免与 2D 精灵基线区域重叠(便于像素校验)。
				const glm::mat4 model = glm::translate(glm::mat4(1.0f), glm::vec3(0.78f, 0.62f, 0.0f))
					* glm::rotate(glm::mat4(1.0f), glm::radians(35.0f), glm::vec3(0.0f, 1.0f, 0.0f))
					* glm::rotate(glm::mat4(1.0f), glm::radians(22.0f), glm::vec3(1.0f, 0.0f, 0.0f))
					* glm::scale(glm::mat4(1.0f), glm::vec3(0.42f));
				Renderer3D::Submit(m_DebugCube, model, { 1.0f, 0.55f, 0.12f, 1.0f });
				Renderer3D::EndScene();
			}
		}

		// 3D 网格通道(D2c/D3):MeshRendererComponent 实体用 Transform 作模型矩阵;
		// 提交顺序:D3 材质排序要求"先不透明、后透明",否则透明面会遮挡其后的不透明物体。
		// 收集阶段已在阴影通道之前完成(见上方的 `draws`)。
		{
			if (!draws.empty())
			{

				Renderer3D::BeginScene(viewProjection, m_CommandBuffers[slot]);
				// D8b-2:不透明 draw 按"同网格 + 同 submesh + 同材质 + 同色"分桶,
				// 桶内 ≥4 个实例才用实例化合批(一次 DrawIndexed 画完整桶);不够 4 个的
				// 继续逐物体提交(状态切换更少)。透明物体一律走原路径(排序语义优先)。
				struct BatchKey
				{
					const Mesh* MeshPtr = nullptr;
					uint32_t Submesh = 0;
					const Material* MaterialPtr = nullptr;
					glm::vec4 Color { 1.0f };
					bool operator<(const BatchKey& other) const
					{
						return std::tie(MeshPtr, Submesh, MaterialPtr, Color.x, Color.y, Color.z, Color.w)
							< std::tie(other.MeshPtr, other.Submesh, other.MaterialPtr,
								other.Color.x, other.Color.y, other.Color.z, other.Color.w);
					}
				};
				constexpr size_t kMinBatchInstances = 4;
				std::map<BatchKey, std::vector<uint32_t>> buckets;
				if (RenderSettings::Get().Instancing)
				{
					for (const uint32_t drawIndex : visibleDraws)
					{
						const MeshDraw& draw = draws[drawIndex];
						// D5c-4a:蒙皮 draw 不进实例化桶(调色板逐物体,合批没有 per-instance 通道)。
						if (draw.Transparent || draw.Skinned)
							continue;
						buckets[{ draw.MeshAsset.get(), draw.SubmeshIndex, draw.MaterialAsset.get(),
							draw.Color }].push_back(drawIndex);
					}
				}
				const auto batchEligible = [&buckets, kMinBatchInstances](const MeshDraw& draw)
				{
					const auto found = buckets.find({ draw.MeshAsset.get(), draw.SubmeshIndex,
						draw.MaterialAsset.get(), draw.Color });
					return found != buckets.end() && found->second.size() >= kMinBatchInstances;
				};
				// 稳定分组:不透明按原顺序;**透明按"到相机距离从远到近"**(D8 收尾 ——
				// 不排序时后画的近物体会被先画的远物体混合盖掉,画面随视角抖)。
				std::vector<uint32_t> transparentOrder;
				{
					const glm::vec3 cameraPosition = glm::vec3(cameraTransform[3]);
					std::vector<std::pair<float, uint32_t>> sorted;
					for (const uint32_t drawIndex : visibleDraws)
					{
						const MeshDraw& draw = draws[drawIndex];
						if (!draw.Transparent)
							continue;
						const glm::vec3 center = (draw.WorldMin + draw.WorldMax) * 0.5f;
						const glm::vec3 delta = center - cameraPosition;
						sorted.emplace_back(glm::dot(delta, delta), drawIndex);
					}
					std::stable_sort(sorted.begin(), sorted.end(),
						[](const std::pair<float, uint32_t>& left, const std::pair<float, uint32_t>& right)
						{
							return left.first > right.first;   // 远的先画
						});
					transparentOrder.reserve(sorted.size());
					for (const auto& [distance, drawIndex] : sorted)
						transparentOrder.push_back(drawIndex);
				}
				for (const bool transparentPass : { false, true })
				{
					for (const uint32_t drawIndex : (transparentPass ? transparentOrder : visibleDraws))
					{
						const MeshDraw& draw = draws[drawIndex];
						// D8b-2:属于合批桶的不透明 draw 交给下面的实例化提交(避免重复画)。
						if (!draw.Transparent && batchEligible(draw))
							continue;
						if (draw.Transparent != transparentPass)
							continue;
						// D7-1c:把实体 id 一起提交,写进 entity-id 附件供视口点选读回。
						const int32_t entityId = static_cast<int32_t>(static_cast<uint32_t>(draw.Entity));
						// D5c-4a:蒙皮实体走蒙皮管线(透明材质本阶段被 SubmitSkinned 拒绝)。
						// 调色板/提交不可用时:布局 1(模型没有 skin 数据)回退静态路径,不丢物体;
						// 布局 2 的顶点 stride 是 64,不能进静态管线,只跳过并 warn 一次。
						if (draw.Skinned)
						{
							uint32_t submitted = UINT32_MAX;
							const uint32_t paletteCount = draw.Palette
								? static_cast<uint32_t>(draw.Palette->size()) : 0;
							if (paletteCount > 0)
							{
								if (draw.MaterialAsset)
									submitted = Renderer3D::SubmitSkinned(draw.MeshAsset, draw.SubmeshIndex,
										draw.MaterialAsset, *draw.Model, draw.Palette->data(), paletteCount, entityId);
								else
									submitted = Renderer3D::SubmitSkinned(draw.MeshAsset, draw.SubmeshIndex,
										draw.Color, *draw.Model, draw.Palette->data(), paletteCount, entityId);
							}
							if (submitted != UINT32_MAX)
								continue;
							if (draw.MeshAsset->GetVertexLayoutId() == Mesh::kVertexLayoutSkinned)
							{
								const std::string meshName = draw.MeshAsset->GetDesc().DebugName;
								WarnOnce("skinned-draw:" + meshName, draw.Transparent
									? ("透明材质的蒙皮网格本阶段不支持(跳过 '" + meshName + "')")
									: ("蒙皮提交被拒绝(调色板/对象槽位/每帧配额;跳过 '" + meshName + "')"));
								continue;
							}
						}
						if (draw.SubmeshIndex == UINT32_MAX)
						{
							if (draw.MaterialAsset)
								Renderer3D::Submit(draw.MeshAsset, draw.MaterialAsset, *draw.Model, entityId);
							else
								Renderer3D::Submit(draw.MeshAsset, *draw.Model, draw.Color, entityId);
						}
						else if (draw.MaterialAsset)
							Renderer3D::SubmitSubmesh(draw.MeshAsset, draw.SubmeshIndex, draw.MaterialAsset,
								*draw.Model, entityId);
						else
							Renderer3D::SubmitSubmesh(draw.MeshAsset, draw.SubmeshIndex, draw.Color,
								*draw.Model, entityId);
					}
				}
				// ② 合批桶整桶提交;失败(实例缓冲/对象槽位满)整桶回退逐物体,绝不丢物体。
				std::vector<glm::mat4> batchTransforms;
				std::vector<glm::vec4> batchColors;
				std::vector<int32_t> batchEntityIds;
				for (auto& [key, indices] : buckets)
				{
					if (indices.size() < kMinBatchInstances)
						continue;
					batchTransforms.clear();
					batchColors.clear();
					batchEntityIds.clear();
					batchTransforms.reserve(indices.size());
					batchColors.reserve(indices.size());
					batchEntityIds.reserve(indices.size());
					for (const uint32_t drawIndex : indices)
					{
						const MeshDraw& draw = draws[drawIndex];
						batchTransforms.push_back(*draw.Model);
						batchColors.push_back(draw.Color);
						batchEntityIds.push_back(static_cast<int32_t>(static_cast<uint32_t>(draw.Entity)));
					}
					const Ref<Mesh>& batchMesh = draws[indices.front()].MeshAsset;
					const Ref<Material>& batchMaterial = draws[indices.front()].MaterialAsset;
					const uint32_t submitted = Renderer3D::SubmitInstanced(batchMesh, key.Submesh, batchMaterial,
						batchTransforms.data(), batchColors.data(), batchEntityIds.data(),
						static_cast<uint32_t>(indices.size()));
					if (submitted != 0)
						continue;
					for (const uint32_t drawIndex : indices)
					{
						const MeshDraw& draw = draws[drawIndex];
						const int32_t entityId = static_cast<int32_t>(static_cast<uint32_t>(draw.Entity));
						if (draw.SubmeshIndex == UINT32_MAX)
						{
							if (draw.MaterialAsset)
								Renderer3D::Submit(draw.MeshAsset, draw.MaterialAsset, *draw.Model, entityId);
							else
								Renderer3D::Submit(draw.MeshAsset, *draw.Model, draw.Color, entityId);
						}
						else if (draw.MaterialAsset)
							Renderer3D::SubmitSubmesh(draw.MeshAsset, draw.SubmeshIndex, draw.MaterialAsset,
								*draw.Model, entityId);
						else
							Renderer3D::SubmitSubmesh(draw.MeshAsset, draw.SubmeshIndex, draw.Color,
								*draw.Model, entityId);
					}
				}
				Renderer3D::EndScene();
			}
		}

		Renderer2D::StartBatch();
		Renderer2D::BeginScene(camera, cameraTransform, m_CommandBuffers[slot]);

		if (selectedEntity)
		{
			// 2D 精灵:描边就画在精灵本身的四边形上(与物体重合)。
			// 3D 网格实体**不在这里画**:用实体 Transform 画出来的是一个 2D 方块 ——
			// 在 3D 视口里既不是物体轮廓、位置也不对(用户 2026-09-16 反馈)。
			// 3D 的选中框改由编辑器 UI 按"投影包围盒"画在场景图之上(D7-1c)。
			if (selectedEntity.HasComponent<SpriteComponent>() &&
				!selectedEntity.HasComponent<MeshRendererComponent>())
			{
				auto& transform = selectedEntity.GetComponent<TransformComponent>();
				Renderer2D::DrawRectCore(transform, { 1.0f, 0.5f, 0.0f, 1.0f }, selectedEntity);
			}
			RenderDebug(camera, cameraTransform);
		}
		RenderGeometry(camera, cameraTransform);

		Renderer2D::EndScene();
		m_CommandBuffers[slot]->EndRenderPass();
		// D8b:GPU 时间戳必须写在渲染通道之外(vkCmdWriteTimestamp 不能在 render pass 内),
		// 因此这一段量的是"3D+2D 主通道"从 BeginRenderPass 到 EndRenderPass 的 GPU 时间。
		EndGpuTiming(slot);
		// 场景颜色附件在命令缓冲内转为可采样布局:提交方无需再 WaitIdle 做外部转换,
		// 同一队列上后续提交(UI)按顺序即可安全采样。
		{
			Rhi::ResourceBarrier barrier;
			barrier.Texture = m_ColorTexture;
			barrier.Before = Rhi::ResourceState::ColorAttachment;
			barrier.After = Rhi::ResourceState::ShaderReadOnly;
			m_CommandBuffers[slot]->PipelineBarrier({ barrier });
		}
		m_CommandBuffers[slot]->End();
		Renderer::SubmitScene(m_CommandBuffers[slot], m_ColorTexture);

		// D8a:场景统计(编辑器 Stats 面板 / AI `stats.scene` / 压力场景脚本读取)。
		{
			Renderer3D::SceneStatistics stats;
			stats.Objects = static_cast<uint32_t>(draws.size());
			stats.Submitted = static_cast<uint32_t>(visibleDraws.size());
			stats.Culled = stats.Objects - stats.Submitted;
			stats.ShadowCasters = shadowCasters;
			const Renderer3D::Statistics statsNow = Renderer3D::GetStats();
			stats.DrawCalls = statsNow.DrawCalls - statsBeforeScene.DrawCalls;
			stats.Triangles = statsNow.Triangles - statsBeforeScene.Triangles;
			stats.DroppedObjects = statsNow.DroppedObjects - statsBeforeScene.DroppedObjects;
			stats.InstancedBatches = statsNow.InstancedBatches - statsBeforeScene.InstancedBatches;
			stats.InstancedObjects = statsNow.InstancedObjects - statsBeforeScene.InstancedObjects;
			stats.CullingEnabled = cullingEnabled;
			stats.InstancingEnabled = RenderSettings::Get().Instancing;
			stats.GpuMilliseconds = gpuMilliseconds;
			stats.CullMilliseconds = cullMilliseconds;
			stats.SceneMilliseconds = std::chrono::duration<double, std::milli>(
				std::chrono::steady_clock::now() - sceneStart).count();
			Renderer3D::ReportSceneStatistics(stats);
		}
	}

	void SceneRenderer::RenderGeometry(const Camera&, const glm::mat4&)
	{
		// B3:每实体的顶点变换(纯数学,不触碰批次状态机)按阈值并行;
		// 批次写入仍按原顺序在主线程执行,绘制顺序与像素结果与串行版本一致。
		constexpr size_t kParallelPrepThreshold = 64;
		// 2D 精灵也要走层级世界矩阵:子实体跟随父实体。旧实现直接用子实体的**局部**矩阵,
		// 表现为"移动父项、子项不动"(用户 2026-09-16 反馈,3D 网格路径早已用世界矩阵)。
		const auto spriteMatrixOf = [this](entt::entity entity, const TransformComponent& transform) -> const glm::mat4&
		{
			if (const auto* world = m_ActiveScene->m_Registry.try_get<WorldTransformComponent>(entity))
				return world->Matrix;
			return transform.Transform;
		};
		{
			auto group = m_ActiveScene->m_Registry.group<TransformComponent>(entt::get<SpriteComponent>);
			std::vector<entt::entity> entities;
			for (auto entity : group)
				entities.push_back(entity);
			const size_t count = entities.size();
			if (count >= kParallelPrepThreshold && JobSystem::IsRunning())
			{
				std::vector<glm::mat4> transforms(count);
				std::vector<std::array<glm::vec3, 4>> positions(count);
				for (size_t i = 0; i < count; i++)
				{
					const auto& transform = std::get<0>(group.get<TransformComponent, SpriteComponent>(entities[i]));
					transforms[i] = spriteMatrixOf(entities[i], transform);
				}

				JobSystem::ParallelFor(static_cast<uint32_t>(count), 32, [&](uint32_t i)
				{
					Renderer2D::ComputeQuadPositions(transforms[i], positions[i].data());
				});

				for (size_t i = 0; i < count; i++)
				{
					auto [transform, sprite] = group.get<TransformComponent, SpriteComponent>(entities[i]);
					Renderer2D::DrawQuadPositions(positions[i].data(), sprite.Texture, sprite.Color,
						nullptr, sprite.TilingFactor, static_cast<uint32_t>(entities[i]));
				}
			}
			else
			{
				for (auto entity : entities)
				{
					auto [transform, sprite] = group.get<TransformComponent, SpriteComponent>(entity);
					Renderer2D::DrawQuadCore(spriteMatrixOf(entity, transform), sprite.Texture, sprite.Color, nullptr,
						sprite.TilingFactor, static_cast<uint32_t>(entity));
				}
			}
		}
		{
			auto view = m_ActiveScene->m_Registry.view<TransformComponent, CircleRendererComponent>();
			std::vector<entt::entity> entities;
			for (auto entity : view)
				entities.push_back(entity);
			const size_t count = entities.size();
			if (count >= kParallelPrepThreshold && JobSystem::IsRunning())
			{
				std::vector<glm::mat4> transforms(count);
				std::vector<std::array<glm::vec3, 4>> positions(count);
				for (size_t i = 0; i < count; i++)
					transforms[i] = view.get<TransformComponent>(entities[i]).Transform;
				JobSystem::ParallelFor(static_cast<uint32_t>(count), 32, [&](uint32_t i)
				{
					Renderer2D::ComputeCirclePositions(transforms[i], positions[i].data());
				});
				for (size_t i = 0; i < count; i++)
				{
					const auto& circle = view.get<CircleRendererComponent>(entities[i]);
					Renderer2D::DrawCirclePositions(positions[i].data(), circle.Color, circle.Thickness,
						circle.Fade, static_cast<uint32_t>(entities[i]));
				}
			}
			else
			{
				for (auto entity : entities)
				{
					auto& transform = view.get<TransformComponent>(entity);
					const auto& circle = view.get<CircleRendererComponent>(entity);
					Renderer2D::DrawCircleCore(transform.Transform, circle.Color, circle.Thickness,
						circle.Fade, static_cast<uint32_t>(entity));
				}
			}
		}
	}

	void SceneRenderer::RenderDebug(const Camera&, const glm::mat4&)
	{
		auto view = m_ActiveScene->m_Registry.view<CircleCollider2DComponent>();
		for (auto entity : view)
		{
			auto& transform = m_ActiveScene->m_Registry.get<TransformComponent>(entity);
			auto& circleCollider = view.get<CircleCollider2DComponent>(entity);
			if (!circleCollider.ShowCollider)
				continue;
			glm::mat4 colliderTransform = glm::translate(glm::mat4(1.0f), transform.Location)
				* glm::rotate(glm::mat4(1.0f), transform.Rotation.z, glm::vec3(0.0f, 0.0f, 1.0f))
				* glm::translate(glm::mat4(1.0f), glm::vec3(circleCollider.Offset.x, circleCollider.Offset.y, 0.0f))
				* glm::scale(glm::mat4(1.0f), glm::vec3(transform.Scale.x * circleCollider.Radius * 2.0f,
					transform.Scale.y * circleCollider.Radius * 2.0f, 1.0f));
			Renderer2D::DrawCircleCore(colliderTransform, { 0.1f, 0.9f, 0.1f, 1.0f },
				0.025f, 0.005f, static_cast<uint32_t>(entity));
		}
	}

	void SceneRenderer::OnResize(uint32_t width, uint32_t height)
	{
		// P4-3:宿主传进来的是**请求/显示尺寸**(编辑器视口/运行时窗口)。渲染目标按
		// rendering.render_scale 换算;窗口尺寸、WUI/UI 与视口在屏幕上的占位都不受影响
		// —— 视口照旧把这张纹理拉伸显示(与显示任意尺寸目标同一条路径)。
		m_RequestedWidth = std::max(1u, width);
		m_RequestedHeight = std::max(1u, height);
		ApplyRenderScale();
	}

	void SceneRenderer::CaptureFrame(const std::filesystem::path& path) const
	{
		// 走后端无关的 RHI 读回:GL 的延迟命令列表与 Vulkan 的呈现路径下,
		// 旧的 glReadPixels 版本分别只能抓到清屏色与全黑。
		Renderer::CaptureTexture(path, m_ColorTexture, m_Width, m_Height);
		// 诊断/验证(D7-1c):WLD_CAPTURE_ENTITY=<路径> 时把 entity-id 附件一起读回。
		// 附件是 R32_SINT:0xFFFFFFFF(-1)读成白色 = 该像素没有实体,其它值就是实体句柄。
		if (const char* entityPath = std::getenv("WLD_CAPTURE_ENTITY"))
			Renderer::CaptureTexture(entityPath, m_EntityTexture, m_Width, m_Height);
	}

	// D7-1c:视口点选的后端无关读回。entity-id 附件是 R32_SINT 颜色附件,一帧结束停在
	// ColorAttachment(见 Init() 里 RenderPassAttachment::FinalLayout)。这里复用截图那条
	// RHI 通路(CopyTextureToBuffer + Map):整张附件拷到 host-visible buffer 后取目标像素。
	// 旧实现走 Framebuffer::ReadPixel(glReadPixels),Vulkan 下读不到东西。
	// 点击是低频操作,因此接受一次 WaitIdle + 全附件拷贝的代价。
	// 整张 entity-id 附件的读回(显示朝向:第 0 行 = 画面顶部,见 ReadEntityIdAt 的说明)。
	// 自动化自检用它扫描可见实体,再逐点走 ReadEntityIdAt 验证拾取路径。
	bool SceneRenderer::ReadEntityIdBuffer(std::vector<int32_t>& outIds)
	{
		if (!m_Device || !m_EntityTexture || m_Width == 0 || m_Height == 0)
			return false;

		Rhi::BufferDesc readbackDesc;
		readbackDesc.Size = static_cast<uint64_t>(m_Width) * m_Height * 4;
		readbackDesc.Usage = Rhi::BufferUsageTransferDst;
		readbackDesc.Memory = Rhi::MemoryHint::HostVisible;
		readbackDesc.DebugName = "PickReadback";
		Rhi::Handle<Rhi::Buffer> readback = m_Device->CreateBuffer(readbackDesc);
		if (!readback)
		{
			WLD_CORE_ERROR("[pick] failed to create readback buffer");
			return false;
		}
		// 与截图同样复用队列:每次点选都建队列会泄漏后端对象。
		static Rhi::Handle<Rhi::CommandQueue> s_PickQueue;
		if (!s_PickQueue)
			s_PickQueue = m_Device->CreateQueue("Pick");
		if (!s_PickQueue)
			return false;
		// 先等干净:保证读到的是点击前最后一次提交的附件内容。
		m_Device->WaitIdle();

		s_PickQueue->ExecuteImmediate([&](Rhi::CommandBuffer& cmd)
		{
			Rhi::ResourceBarrier toCopy;
			toCopy.Texture = m_EntityTexture;
			toCopy.Before = Rhi::ResourceState::ColorAttachment;
			toCopy.After = Rhi::ResourceState::CopySrc;
			cmd.PipelineBarrier({ toCopy });
			cmd.CopyTextureToBuffer(m_EntityTexture, readback, 0);
			Rhi::ResourceBarrier back;
			back.Texture = m_EntityTexture;
			back.Before = Rhi::ResourceState::CopySrc;
			back.After = Rhi::ResourceState::ColorAttachment;
			cmd.PipelineBarrier({ back });
		});

		const int32_t* pixels = static_cast<const int32_t*>(readback->Map());
		if (!pixels)
		{
			WLD_CORE_ERROR("[pick] readback buffer is not mappable on this backend");
			return false;
		}
		// 行序:必须和**显示**一致。场景纹理由 WUI 以 UV {0,1,1,-1} 贴到视口(上下翻转,
		// 见 ViewportPanel 的 m_SceneImage->Uv),因此视口局部坐标 y 对应的是附件里的
		// 第 (Height-1-y) 行 —— 两个后端都是这个约定(实测 GL/Vulkan 附件逐像素一致)。
		// 不翻行的话可点区域就是画面上物体的**上下镜像**(用户 2026-09-16 反馈:
		// "点方块任意位置应该选中,而且方形区域位置不对")。
		outIds.resize(static_cast<size_t>(m_Width) * m_Height);
		for (uint32_t row = 0; row < m_Height; ++row)
		{
			const size_t sourceRow = m_Height - 1 - row;
			std::memcpy(outIds.data() + static_cast<size_t>(row) * m_Width,
				pixels + sourceRow * m_Width, static_cast<size_t>(m_Width) * sizeof(int32_t));
		}
		readback->Unmap();
		return true;
	}

	int32_t SceneRenderer::ReadEntityIdAt(int32_t x, int32_t y)
	{
		if (x < 0 || y < 0 || x >= static_cast<int32_t>(m_Width) || y >= static_cast<int32_t>(m_Height))
			return -1;
		std::vector<int32_t> ids;
		if (!ReadEntityIdBuffer(ids))
			return -1;
		const int32_t id = ids[static_cast<size_t>(y) * m_Width + static_cast<uint32_t>(x)];
		if (std::getenv("WLD_TRACE_UI"))
			WLD_CORE_INFO("[pick] readback {0}x{1} at({2},{3}) id={4}", m_Width, m_Height, x, y, id);
		return id;
	}

	void SceneRenderer::WarnOnce(const std::string& key, const std::string& message)
	{
		if (m_WarnedPaths.insert(key).second)
			WLD_CORE_WARN("{0}", message);
	}
}
