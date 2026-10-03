#include "Renderer3D_Internal.h"

namespace World
{

using namespace Renderer3DDetail;


uint32_t Renderer3D::SubmitSkinned(const Ref<Mesh>& mesh, uint32_t submeshIndex, const glm::vec4& baseColor, const glm::mat4& transform, const glm::mat4* palette, uint32_t paletteCount, int32_t entityId){
		return SubmitSkinnedInternal(mesh, submeshIndex, nullptr, &baseColor, transform, palette,
			paletteCount, entityId, /*shadow*/ false);
	}


uint32_t Renderer3D::SubmitShadowSkinned(const Ref<Mesh>& mesh, uint32_t submeshIndex, const glm::mat4& transform, const glm::mat4* palette, uint32_t paletteCount){
		return SubmitSkinnedInternal(mesh, submeshIndex, nullptr, nullptr, transform, palette,
			paletteCount, -1, /*shadow*/ true);
	}


uint32_t Renderer3D::SubmitSkinnedInternal(const Ref<Mesh>& mesh, uint32_t submeshIndex, const Ref<Material>& material, const glm::vec4* baseColor, const glm::mat4& transform, const glm::mat4* palette, uint32_t paletteCount, int32_t entityId, bool shadow, bool reservedPalette){
		State& state = GetState();
		const Rhi::Handle<Rhi::Pipeline>& pipeline = shadow ? state.SkinnedShadowPipeline : state.SkinnedPipeline;
		if (!mesh || !state.CommandBuffer || !pipeline)
			return UINT32_MAX;
		// 调色板约束:整块 ≤ MaxBonePalette 个矩阵;为空/超限一律拒绝(不绘制)。
		if (!palette || paletteCount == 0 || paletteCount > MaxBonePalette)
			return UINT32_MAX;
		// 布局约束:只有 .wmodel 布局 2(顶点带 joints/weights)能走蒙皮管线。
		// 布局 1 的顶点里没有关节数据,用蒙皮管线读会读越界 —— 明确拒绝,由调用方回退静态路径。
		if (mesh->GetVertexLayoutId() != Mesh::kVertexLayoutSkinned)
			return UINT32_MAX;
		// 透明材质约束:本阶段只建了蒙皮不透明管线(透明要另建混合/不写深度的变体,排序语义
		// 也不同)。这里拒绝而不是当不透明画 —— 由调用方决定回退,或等后续工作包补透明变体。
		if (material && material->GetDesc().BlendMode == MaterialBlendMode::Transparent)
			return UINT32_MAX;
		const bool wholeMesh = submeshIndex == UINT32_MAX;
		if (!wholeMesh && submeshIndex >= mesh->GetSubmeshes().size())
			return UINT32_MAX;

		uint32_t& objectIndex = shadow ? state.ShadowObjectIndex : state.ObjectIndex;
		if (objectIndex >= kObjectsPerFrame)
		{
			state.Stats.DroppedObjects++;
			return UINT32_MAX;
		}
		// 每帧的蒙皮调色板配额(每份 8KB UBO):主通道与阴影通道共用,由 BeginScene/BeginShadowPass 复位。
		// 只约束**顺序分配区**;保留区调用(SubmitSkinnedAtSlot)不消耗该游标(见 kPaletteReservedBase)。
		if (!reservedPalette && state.PaletteCursor >= MaxSkinnedDrawsPerFrame)
		{
			state.Stats.DroppedObjects++;
			return UINT32_MAX;
		}

		uint32_t indexCount = mesh->GetIndexCount();
		uint32_t firstIndex = 0;
		if (!wholeMesh)
		{
			const MeshSubmesh& submesh = mesh->GetSubmeshes()[submeshIndex];
			indexCount = submesh.IndexCount;
			firstIndex = submesh.IndexOffset;
		}
		if (indexCount == 0)
			return UINT32_MAX;

		EnsureMeshBuffersFor(state, mesh);
		const auto cached = state.MeshCache.find(mesh.get());
		if (cached == state.MeshCache.end() || !cached->second.VertexBuffer)
			return UINT32_MAX;

		const uint32_t slot = Renderer::FrameSlot() % Renderer::FramesInFlight;
		// 调色板槽位:
		//  - 顺序模式:取游标后自增(只有真走到绘制才占一份,与旧版一致);
		//  - 保留模式:键 = 对象槽位(此处 objectIndex 尚未自增,就是本次分配到的序号;
		//    SubmitSkinnedAtSlot 已把 ObjectIndex 顶到 slotBase,所以恒有 paletteSlot < kPaletteSlotCount)。
		uint32_t paletteSlot = 0;
		if (reservedPalette)
		{
			paletteSlot = kPaletteReservedBase + objectIndex;
			if (paletteSlot >= kPaletteSlotCount)
				return UINT32_MAX;
		}
		else
			paletteSlot = state.PaletteCursor++;
		const uint32_t index = objectIndex++;
		DrawSkinnedObject(state, cached->second, transform, entityId, baseColor, palette, paletteCount, index,
			paletteSlot, indexCount, firstIndex, pipeline, material, slot, shadow);
		return index;
	}

}
