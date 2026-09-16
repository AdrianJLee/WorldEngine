#pragma once

#include "World/Core/Export.h"
#include "World/RHI/Rhi.h"
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
		// 提交一个网格实例;返回分配到的对象序号,超出上限返回 UINT32_MAX(调用方应报错/跳帧)。
		// entityId:D7-1c 视口点选用,写进 entity-id 附件(SV_Target1);-1 = 不可拾取。
		static uint32_t Submit(const Ref<Mesh>& mesh, const glm::mat4& transform,
			const glm::vec4& baseColor = glm::vec4(1.0f), int32_t entityId = -1);
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
