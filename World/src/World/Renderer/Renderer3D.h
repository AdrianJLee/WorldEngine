#pragma once

#include "World/Core/Export.h"
#include "World/RHI/Rhi.h"
#include "World/Renderer/Material.h"
#include "World/Renderer/Mesh.h"

#include <glm/glm.hpp>

namespace World
{
	// P1b D2b:3D 实心网格提交通道(与 Renderer2D 平行)。
	//
	// 资源约定:
	//  - set 0 = 全局相机 UBO(由 SceneRenderer 每帧绑定,提供 u_ViewProjection);
	//  - set 1 = 每对象 UBO(u_Model / u_BaseColor),按"帧槽位 × 对象序号"各一份,
	//    提交时直接映射写入(帧槽位由帧栅栏保护,不会在 GPU 使用中被覆盖)。
	// 首期每帧对象上限 D2bObjectsPerFrame;实例化/剔除/多材质在 D8 扩展。
	class WLD_API Renderer3D
	{
	public:
		static void Init();
		static void Shutdown();

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
		};
		static Statistics GetStats();
		static void ResetStats();
		static uint32_t GetObjectsPerFrameLimit();

	private:
		static void EnsureMeshBuffers(const Ref<Mesh>& mesh);
	};
}
