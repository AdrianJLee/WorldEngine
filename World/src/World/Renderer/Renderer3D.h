#pragma once

#include "World/Core/Export.h"
#include "World/RHI/Rhi.h"
#include "World/Renderer/Material.h"
#include "World/Renderer/Mesh.h"

#include <glm/glm.hpp>

#include <vector>

namespace World
{
	// P1b D4:灯光 UBO 的 CPU 镜像(std140,与 Renderer3D_Solid.hlsl 的 cbuffer LightUniforms
	// 逐字段对应;数组元素 48 字节)。总大小 64+16+16+16+8*48 = 496。
	struct LightUniforms
	{
		struct Light
		{
			// xyz = 位置(点光)/ w = 0 点光 | 1 方向光。
			glm::vec4 PositionType { 0.0f, 0.0f, 0.0f, 0.0f };
			// rgb = 线性色, a = 强度。
			glm::vec4 ColorIntensity { 1.0f, 1.0f, 1.0f, 1.0f };
			// 方向光:xyz = 传播方向(已归一化);点光:x = 范围。
			glm::vec4 DirectionRange { 0.0f, -1.0f, 0.0f, 0.0f };
		};

		// 方向光正交矩阵(阴影通道写深度用同一矩阵;主通道用它算投影坐标)。
		glm::mat4 ShadowViewProjection { 1.0f };
		// x = 启用阴影, y = 深度 bias, z = 贴图边长, w = PCF 半径(纹素)。
		glm::vec4 ShadowParams { 0.0f, 0.0f, 2048.0f, 1.0f };
		// rgb = 环境光线性色, a = 强度(无 AmbientLightComponent 时为 0.25 灰默认值)。
		glm::vec4 Ambient { 0.25f, 0.25f, 0.25f, 1.0f };
		// x = 方向光数, y = 点光数, z = 深度约定(0 = Vulkan 的 [0,1] 裁剪深度,
		// 1 = OpenGL 的 [-1,1] 裁剪深度 → 深度缓冲存 (z+1)/2), w = 保留。
		glm::uvec4 LightCounts { 0u, 0u, 0u, 0u };
		Light Lights[8];
	};
	static_assert(sizeof(LightUniforms) == 496, "LightUniforms must match Renderer3D_Solid.hlsl (std140)");

	// 收集阶段得到的灯光数据(SceneRenderer 从组件填充;打包由 BuildLightRig 完成,
	// 因此上限/归一化/默认值都是纯函数,可在 headless 测试里直接断言)。
	struct DirectionalLightData
	{
		glm::vec3 Color { 1.0f, 1.0f, 1.0f };
		float Intensity = 1.0f;
		glm::vec3 Direction { 0.35f, -0.7f, 0.6f };
		bool CastShadow = false;
	};

	struct PointLightData
	{
		glm::vec3 Color { 1.0f, 1.0f, 1.0f };
		float Intensity = 1.0f;
		glm::vec3 Position { 0.0f, 0.0f, 0.0f };
		float Range = 10.0f;
	};

	struct AmbientLightData
	{
		glm::vec3 Color { 1.0f, 1.0f, 1.0f };
		float Intensity = 0.25f;
	};

	struct LightRig
	{
		LightUniforms Uniforms;
		uint32_t DirectionalLights = 0;
		uint32_t PointLights = 0;
		uint32_t TotalLights = 0;
		uint32_t DroppedLights = 0;
		bool ShadowCaster = false;   // 是否有"主方向光 + CastShadow"
	};

	// P1b D2b:3D 实心网格提交通道(与 Renderer2D 平行)。
	//
	// 资源约定:
	//  - set 0 = 全局相机/灯光(由 SceneRenderer 每帧绑定):
	//      binding 0 = 相机 UBO(u_ViewProjection),
	//      binding 2 = 灯光 UBO(D4:LightUniforms),
	//      binding 3 = 方向光阴影贴图(D4:2048² D32_SFLOAT,点采样 + ClampToEdge;
	//      必须用 D32 而不是 D24S8 —— 后者在 RHI 里的视图是 DEPTH|STENCIL 双 aspect,
	//      不能作为采样描述符,见 Renderer3D.cpp Init 的说明);
	//  - set 1 = 每对象 UBO(u_Model / u_BaseColor),按"帧槽位 × 对象序号"各一份,
	//    提交时直接映射写入(帧槽位由帧栅栏保护,不会在 GPU 使用中被覆盖)。
	// 首期每帧对象上限 D2bObjectsPerFrame;实例化/剔除/多材质在 D8 扩展。
	class WLD_API Renderer3D
	{
	public:
		static void Init();
		static void Shutdown();

		// ---- D4:光照上限与阴影贴图规格 ----
		static constexpr uint32_t MaxDirectionalLights = 1;   // 主方向光(阴影只做它)
		static constexpr uint32_t MaxPointLights = 7;
		static constexpr uint32_t MaxLights = MaxDirectionalLights + MaxPointLights;
		static constexpr uint32_t ShadowMapSize = 2048;

		// 灯光打包(纯函数,无 GPU/Scene 依赖):方向光取前 1、点光取前 7、合计 ≤ 8,
		// 超出的进 DroppedLights;方向做归一化(零向量回退 -Y);ambient == nullptr 时
		// 用 0.25 灰默认值。glDepthConvention = GL 后端时阴影深度按 [-1,1] 裁剪深度换算。
		static LightRig BuildLightRig(const std::vector<DirectionalLightData>& directionalLights,
			const std::vector<PointLightData>& pointLights, const AmbientLightData* ambient,
			bool glDepthConvention);
		// 让主方向光(CastShadow)参与阴影:写入正交光源矩阵并启用 u_ShadowParams.x。
		static void ApplyShadowCaster(LightRig& rig, const glm::mat4& lightViewProjection);
		// 本帧灯光统计(编辑器 Stats 面板 + 自动化日志):数量上限、截断数与阴影通道 CPU 耗时;
		// 数量/阴影开关变化或首帧时打一行 [lighting]。
		static void ReportLighting(const LightRig& rig, double shadowPassMilliseconds);

		// 阴影通道:调用方(SceneRenderer)在自己的命令缓冲里 Begin/End 这个深度通道。
		static Rhi::Handle<Rhi::RenderPass> GetShadowRenderPass();
		static Rhi::Handle<Rhi::Framebuffer> GetShadowFramebuffer();
		static Rhi::Handle<Rhi::Texture> GetShadowMapTexture();
		static Rhi::Handle<Rhi::Sampler> GetShadowMapSampler();
		static void BeginShadowPass(const Rhi::Handle<Rhi::CommandBuffer>& commandBuffer);
		// 投影者提交(用独立的槽位区,不消耗主通道的对象序号);返回分配的槽位。
		static uint32_t SubmitShadow(const Ref<Mesh>& mesh, const glm::mat4& transform);
		static void EndShadowPass();

		// 自建 set0 的调用方(材质预览等)必须把这两个写(binding 2 = 灯光 UBO,
		// binding 3 = 阴影贴图)**和相机写(binding 0)放在同一次 set->Update 里**:
		// GL 后端的 OpenGLDescriptorSet::Update 是"整体替换"语义,分两次写会丢掉相机绑定。
		// lightUniformBuffer = nullptr 时用引擎内置的"预览默认灯光"(占位实现同款方向光 +
		// 0.25 环境光),保持既有预览观感;缺这两个 binding 的 set0 在 Vulkan 下会被验证层
		// 判为"静态使用的描述符未更新"。
		static std::vector<Rhi::DescriptorWrite> MakeGlobalLightingWrites(
			const Rhi::Handle<Rhi::Buffer>& lightUniformBuffer = nullptr);

		// 开始一个 3D 批次:viewProjection 已按后端做过 NDC Y 适配。
		static void BeginScene(const glm::mat4& viewProjection, const Rhi::Handle<Rhi::CommandBuffer>& commandBuffer);
		// 在 BeginScene 之后、绑定 set 0/1/2 之前调用:显式绑定不透明管线,
		// 让后端拿到管线布局(Vulkan 的 vkCmdBindDescriptorSets 需要它,否则 set 0
		// 会被"延迟到下一次 BindPipeline"——预览相机矩阵因此丢失,几何不出现)。
		static void BindPipelineForCurrentPass();
		// 预览等非 SceneRenderer 调用方:登记 set 0(全局相机)描述符集,
		// 会在管线绑定之后真正执行 vkCmdBindDescriptorSets。
		static void SetGlobalDescriptorSet(const Rhi::Handle<Rhi::DescriptorSet>& set);
		// 提交一个网格实例(旧接口:无材质,只用常量色,供预览/内部使用);
		// 返回分配到的对象序号,超出上限返回 UINT32_MAX(调用方应报错/跳帧)。
		// entityId:D7-1c 视口点选用,写进 entity-id 附件(SV_Target1);-1 = 不可拾取。
		static uint32_t Submit(const Ref<Mesh>& mesh, const glm::mat4& transform,
			const glm::vec4& baseColor = glm::vec4(1.0f), int32_t entityId = -1);

		// D3:带材质的提交。材质为 null 时退化为"常量色"路径(与上面一致)。
		// 透明材质走 Transparent 管线(混合 + 不写深度),由调用方负责排序(不透明先提交)。
		static uint32_t Submit(const Ref<Mesh>& mesh, const Ref<Material>& material, const glm::mat4& transform,
			int32_t entityId = -1);
		// 用**持久槽位**提交(材质预览这类"每帧都画、但只画一两个物体"的调用方):
		// 对象序号从 slotBase 开始分配,跨帧固定,避免与主场景/其它预览争用同一份
		// UBO 与描述符集(争用会让画面逐帧来回闪 —— 用户实测"预览一直闪烁")。
		static uint32_t SubmitAtSlot(uint32_t slotBase, const Ref<Mesh>& mesh, const Ref<Material>& material,
			const glm::mat4& transform, int32_t entityId = -1);
		// 预览/调试调用方按身份取一个稳定槽位(内部做环绕与保留区处理)。
		static uint32_t ReserveSlotBase(uint32_t identity, uint32_t span = 1);

		// 材质 GPU 资源(贴图描述符集)在材质 Revision 变化时自动重建;
		// 后端切换/设备重建后需要显式清空缓存。
		static void InvalidateMaterialCache();
		static void EndScene();

		struct Statistics
		{
			uint32_t DrawCalls = 0;
			uint32_t Triangles = 0;
			// D4:本帧灯光数量/上限/截断数与阴影通道 CPU 耗时(编辑器 Stats 面板读取)。
			uint32_t Lights = 0;
			uint32_t MaxLights = Renderer3D::MaxLights;
			uint32_t DroppedLights = 0;
			double ShadowPassMilliseconds = 0.0;
		};
		static Statistics GetStats();
		static void ResetStats();
		static uint32_t GetObjectsPerFrameLimit();

	private:
		static void EnsureMeshBuffers(const Ref<Mesh>& mesh);
	};
}
