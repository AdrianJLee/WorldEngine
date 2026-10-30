#pragma once

#include "World/RHI/Rhi.h"
#include "World/Renderer/EditorCamera.h"
#include "World/Renderer/Legacy/Gfx/Framebuffer.h"
#include "World/Renderer/Renderer.h"
#include "World/Renderer/Mesh.h"
#include "World/Renderer/AssetRegistry.h"
#include "World/Scene/Scene.h"

#include <glm/glm.hpp>

#include <unordered_set>
#include <unordered_map>

namespace World
{
	struct SceneRendererOptions
	{
		bool ShowGrid = true;
	};

	class WLD_API SceneRenderer : public IRenderExtractSink
	{
	public:
		void Init();
		void Shutdown();

		void BeginScene(Scene* scene, const SceneRendererOptions& options);
		void EndScene();

		// ---- PURE-ECS:Extract 阶段(相机无关的收�?接口契约�?Renderer/RenderExtract.h)----
		// �?本帧要画什�?填进 `scene.RenderExtract()`。Play/Simulate �?`render-extract`
		// 帧系统调�?PreRender 阶段,排在 animation-system 之后);编辑态不跑帧系统,
		// �?`SubmitScene` 兜底调同一份实�?幂等)�?
		void ExtractScene(Scene& scene, float deltaSeconds) override;
		// 场景析构时清�?sink 装在哪个场景�?的记�?�?Renderer/RenderExtract.h)�?
		void OnExtractSceneDestroyed(Scene& scene) override;

		// D5c-4a:骨骼动画的时间推进步�?�?。宿主每帧在 SubmitScene 之前设置;
		// SceneRenderer 在收�?3D 绘制前调�?AnimationSystem::Update 推进
		// SkinnedMeshRendererComponent �?Time 并刷新调色板。默�?0 = 不推�?
		// (相机预览�?同一场景的第二个 SceneRenderer"保持 0,否则同帧会推进两�?�?
		void SetDeltaSeconds(float deltaSeconds) { m_DeltaSeconds = deltaSeconds; }

		void SubmitScene(const Camera& camera, const glm::mat4& cameraTransform, Entity entity);
		void SubmitScene(const Camera& camera, const glm::mat4& cameraTransform);

		void OnResize(uint32_t width, uint32_t height);

		Ref<Framebuffer> GetTargetFramebuffer() const { return m_FramebufferView; }
		Rhi::Handle<Rhi::Texture> GetColorTexture() const { return m_ColorTexture; }
		Rhi::Handle<Rhi::Framebuffer> GetRhiTarget() const { return m_Framebuffer; }
		// P4-3:GetWidth/GetHeight 返回**渲染目标**尺寸(请求尺寸 × rendering.render_scale),
		// 拾取/读回/抓图都按它换�?需�?宿主请求的显示尺�?的调用方(例如按窗口尺寸做
		// 节流判断�?Runtime)用下面两个请求尺寸读�?否则倍率�?1 时会误判尺寸一直变化�?
		uint32_t GetRequestedWidth() const { return m_RequestedWidth; }
		uint32_t GetRequestedHeight() const { return m_RequestedHeight; }
		uint32_t GetWidth() const { return m_Width; }
		uint32_t GetHeight() const { return m_Height; }
		// D7-1c:后端无关地读�?entity-id 附件的一个像�?视口点选用)�?
		// 坐标�?*左上角为原点**(�?WUI 视口一�?,越界返回 -1;
		// 返回 -1 也表示该像素没有实体(附件清屏值就�?-1)�?
		int32_t ReadEntityIdAt(int32_t x, int32_t y);
		// 读回整张 entity-id 附件(显示朝向,行优�?:自动化自检�?扫描可见实体再逐点验证)�?
		bool ReadEntityIdBuffer(std::vector<int32_t>& outIds);
		// 开发验�?把颜色附件读回写 PPM�?
		void CaptureFrame(const std::filesystem::path& path) const;

		// P6:解析 2D 精灵/圆形的渲染世界矩阵。插值开启且该实�?或其祖先)被插值时返回
		// Scene::EnsureInterpolatedWorldTransforms 算好的插值矩�?否则返回权威
		// WorldTransformComponent(缺失时用 localMatrix 兜底)。提成公开静态入口是为了�?
		// "2D 是否吃插�?能被�?GPU 的单元测试直接验证�?
		static const glm::mat4* ResolveSpriteRenderMatrix(const Scene& scene, entt::entity entity,
			const glm::mat4& localMatrix);

	private:
		void RecreateTargets(uint32_t width, uint32_t height);
		// P4-3:�?请求尺寸"按当�?render_scale 换算成渲染目标尺�?尺寸/倍率变化时才重建;
		// 默认倍率 1.0 且尺寸未变时直接返回,不产生任何资源操�?基线逐字节不�?�?
		void ApplyRenderScale();
		void RecordSubmit(const Camera& camera, const glm::mat4& cameraTransform, Entity selectedEntity);
		void RenderGeometry(const Camera& camera, const glm::mat4& cameraTransform);
		void RenderDecals(const Camera& camera, const glm::mat4& cameraTransform);
		void RenderParticles(const Camera& camera, const glm::mat4& cameraTransform);
		void Render2DLighting(const Camera& camera, const glm::mat4& cameraTransform);
		void RenderDebug(const Camera& camera, const glm::mat4& cameraTransform);
		void RenderGroundGrid(const Camera& camera, const glm::mat4& cameraTransform);
		void RenderTerrain(const Camera& camera, const glm::mat4& cameraTransform);
		void RenderFoliage(const Camera& camera, const glm::mat4& cameraTransform);
		// D5:网格/材质资产加载失败只警告一�?按路径去�?,避免逐帧刷屏�?
		void WarnOnce(const std::string& key, const std::string& message);
		// L2 驻留层入口。世界资源表存在时经 AssetRegistry 解析(记账 + 可回�?;
		// 没有(无宿主的单元测试、BeginScene 之前的调�?则退回直连加载函数�?
		Ref<Mesh> ResolveMeshAsset(const AssetRef& asset, std::string* error);
		Ref<Material> ResolveMaterialAsset(const AssetRef& asset, std::string* error);
		Rhi::Handle<Rhi::Texture> ResolveTextureAsset(const AssetRef& asset);

	private:
		Rhi::Handle<Rhi::Device> m_Device;
		// 帧深 2:命令缓冲/相机 UBO/描述符集按帧槽位环形,允许 CPU 录制�?GPU 执行重叠�?
		// �?Renderer::FramesInFlight 同源(�?Renderer.h 的说�?�?
		static constexpr uint32_t kFramesInFlight = Renderer::FramesInFlight;
		Rhi::Handle<Rhi::CommandBuffer> m_CommandBuffers[kFramesInFlight];
		Rhi::Handle<Rhi::RenderPass> m_RenderPass;
		Rhi::Handle<Rhi::Framebuffer> m_Framebuffer;
		Rhi::Handle<Rhi::Texture> m_ColorTexture;
		Rhi::Handle<Rhi::Texture> m_EntityTexture;
		Rhi::Handle<Rhi::Texture> m_DepthTexture;
		// ---- P4-4b:MSAA(rendering.msaa;启动期参�?----
		// Init 时从 RenderSettings::Msaa() 记录一�?设备上限已折�?�?=1 时上面三�?
		// 单采样纹理就是全部附�?与旧结构逐字节一�?;>1 时颜�?/ 实体 id / 深度三类
		// 附件按生效采样数创建在下面三个句柄里,并把结果 resolve 到单采样�?
		// m_ColorTexture / m_EntityTexture(渲染通道附件下标 3/4)�?
		uint32_t m_Samples = 1;
		Rhi::Handle<Rhi::Texture> m_ColorMsaaTexture;
		Rhi::Handle<Rhi::Texture> m_EntityMsaaTexture;
		Rhi::Handle<Rhi::Texture> m_DepthMsaaTexture;
		Rhi::Handle<Rhi::Buffer> m_CameraBuffers[kFramesInFlight];
		// D4:每帧槽位的灯�?UBO(方向�?点光/环境�?阴影矩阵,std140;�?Renderer3D.h)�?
		Rhi::Handle<Rhi::Buffer> m_LightBuffers[kFramesInFlight];
		Rhi::Handle<Rhi::DescriptorSet> m_GlobalDescriptorSets[kFramesInFlight];

		uint32_t FrameSlot() const;

		// ---- P1b D8b:GPU 时间�?rendering.gpu_timing,默认�?----
		// 每帧槽位一套查询池 + host-visible 读回缓冲。时间戳写在本槽位的命令缓冲�?
		// 结果**异步**读回 —�?拷贝命令每帧重发,后端只在结果可用时才拷贝(不可用则跳过,
		// 下一帧再�?,绝不阻塞主线�?读回缓冲在该槽位 kFramesInFlight 帧后被复用时
		// 才由 ReadGpuTiming 取走(代价只有一�?16B 拷贝与一�?Map)�?
		void InitGpuTiming();
		void BeginGpuTiming(uint32_t slot);
		void EndGpuTiming(uint32_t slot);
		double ReadGpuTiming(uint32_t slot);
		bool m_GpuTiming = false;
		Rhi::Handle<Rhi::QueryPool> m_TimestampPools[kFramesInFlight];
		Rhi::Handle<Rhi::Buffer> m_TimestampBuffers[kFramesInFlight];
		bool m_TimestampPending[kFramesInFlight] = {};   // true = 池里已有完整两点时间�?尚未成功读回
		double m_LastGpuMilliseconds = 0.0;

		// m_Width/m_Height = 渲染目标尺寸(实际创建的附�?视口尺寸)�?
		uint32_t m_Width = 1280;
		uint32_t m_Height = 720;
		// P4-3:宿主请求的显示尺�?OnResize 记录;编辑器视�?运行时窗�?与已套用的倍率
		// (哨兵初�?0 不在合法范围;Init 第一次是否创建由"目标还不存在"本身决定)�?
		uint32_t m_RequestedWidth = 1280;
		uint32_t m_RequestedHeight = 720;
		float m_AppliedRenderScale = 0.0f;
		// D5c-4a:骨骼动画步长(SetDeltaSeconds;默认 0 = 不推�?�?
		float m_DeltaSeconds = 0.0f;
		// 抽取 sink 装在哪个场景�?跨帧保持;EndScene 不摘,否则下一帧帧系统拿不�?�?
		Scene* m_ExtractSinkScene = nullptr;
		Scene* m_ActiveScene = nullptr;
		SceneRendererOptions m_Options;
		Ref<Framebuffer> m_FramebufferView;
		// WLD_DEBUG_CUBE 用的调试网格(D2b:3D 通道双后端冒烟基�?�?
		Ref<Mesh> m_DebugCube;
		Ref<Mesh> m_DebugPlane;
		Ref<Mesh> m_DebugSphere;   // D3:sphere 原语的共享网�?材质预览/半球体实�?
		// L2 资产驻留�?WorldContext::Resources() 里的世界级服�?BeginScene 惰性登记并缓存指针)�?
		AssetRegistry* m_AssetRegistry = nullptr;
		std::unordered_set<std::string> m_WarnedPaths;

		// 白盒原型与样条线网格缓存
		Ref<Mesh> GetOrCreateWhiteboxMesh(const struct WhiteboxComponent& comp);
		Ref<Mesh> GetOrCreateSplineMesh(Scene& scene, entt::entity entity, const struct SplineComponent& comp);

		struct WhiteboxCacheKey
		{
			uint8_t Shape = 0;
			uint8_t Subdivisions = 0;
			uint8_t Steps = 0;
			uint8_t _pad = 0;
			float Dimensions[3] = { 0.0f, 0.0f, 0.0f };
			float BevelSize = 0.0f;

			bool operator==(const WhiteboxCacheKey& o) const
			{
				return Shape == o.Shape && Subdivisions == o.Subdivisions && Steps == o.Steps &&
					Dimensions[0] == o.Dimensions[0] && Dimensions[1] == o.Dimensions[1] &&
					Dimensions[2] == o.Dimensions[2] && BevelSize == o.BevelSize;
			}
		};

		struct WhiteboxKeyHash
		{
			size_t operator()(const WhiteboxCacheKey& k) const noexcept
			{
				size_t h = 17;
				h = h * 31 + k.Shape;
				h = h * 31 + k.Subdivisions;
				h = h * 31 + k.Steps;
				h = h * 31 + std::hash<float>()(k.Dimensions[0]);
				h = h * 31 + std::hash<float>()(k.Dimensions[1]);
				h = h * 31 + std::hash<float>()(k.Dimensions[2]);
				h = h * 31 + std::hash<float>()(k.BevelSize);
				return h;
			}
		};

		struct SplineCacheEntry
		{
			uint64_t Hash = 0;
			Ref<Mesh> MeshAsset;
		};

		std::unordered_map<WhiteboxCacheKey, Ref<Mesh>, WhiteboxKeyHash> m_WhiteboxMeshCache;
		std::unordered_map<uint32_t, SplineCacheEntry> m_SplineMeshCache;

		float m_WaterAccumulatedTime = 0.0f;
		std::unordered_map<uint32_t, Ref<Mesh>> m_WaterMeshCache;
		std::unordered_map<uint32_t, glm::mat4> m_WaterModelMatrices;

		void EnsureCelestialMeshes();
		Ref<Mesh> m_SunMesh;
		Ref<Mesh> m_MoonMesh;
		Ref<Mesh> m_StarfieldMesh;
		Ref<Mesh> m_CloudMesh;
		glm::mat4 m_SunModel { 1.0f };
		glm::mat4 m_MoonModel { 1.0f };
		glm::mat4 m_StarfieldModel { 1.0f };
		glm::mat4 m_CloudModel { 1.0f };
	};
}

