#include "Renderer3D_Internal.h"

namespace World
{

using namespace Renderer3DDetail;

namespace Renderer3DDetail
{
		// 把一批实例打包写进当前帧槽位的实例缓冲;失败(容量不够/缓冲创建失败)返回 false。
bool UploadInstances(State& state, uint32_t slot, const glm::mat4* transforms, const glm::vec4* colors, const int32_t* entityIds, uint32_t count, uint64_t* outOffset){
			if (count == 0 || !transforms || state.InstanceCursor + count > kInstanceCapacity)
				return false;
			if (!state.InstanceBuffers[slot])
			{
				Rhi::BufferDesc desc;
				desc.Size = sizeof(InstanceData) * kInstanceCapacity;
				desc.Usage = Rhi::BufferUsageVertex;
				desc.Memory = Rhi::MemoryHint::HostVisible;
				desc.DebugName = "Renderer3D.InstanceBuffer";
				state.InstanceBuffers[slot] = Renderer::GetDevice()->CreateBuffer(desc);
				if (!state.InstanceBuffers[slot])
					return false;
			}
			std::vector<InstanceData> batch(count);
			for (uint32_t index = 0; index < count; ++index)
			{
				const glm::mat4& transform = transforms[index];
				// 按行上传:HLSL 侧 float4x4(a,b,c,d) 按行构造,与 mul(matrix, vector) 配套。
				batch[index].Row0 = glm::row(transform, 0);
				batch[index].Row1 = glm::row(transform, 1);
				batch[index].Row2 = glm::row(transform, 2);
				batch[index].Row3 = glm::row(transform, 3);
				batch[index].Color = colors ? colors[index] : glm::vec4(1.0f);
				batch[index].EntityId = glm::vec4(
					static_cast<float>(entityIds ? entityIds[index] : -1), 0.0f, 0.0f, 0.0f);
			}
			// 与对象 UBO 同一写入时机:录制期直写宿主可见内存,帧槽位由帧栅栏保护。
			*outOffset = static_cast<uint64_t>(state.InstanceCursor) * sizeof(InstanceData);
			state.InstanceBuffers[slot]->SetData(batch.data(), batch.size() * sizeof(InstanceData), *outOffset);
			state.InstanceCursor += count;
			return true;
		}


		// 一次 DrawIndexed(instanceCount = count):共享一个对象槽位(整批材质常量),
		// per-instance 数据来自 binding 1 的实例缓冲。
void DrawInstancedBatch(State& state, const MeshGpu& mesh, uint32_t indexCount, uint32_t firstIndex, const Rhi::Handle<Rhi::Buffer>& instanceBuffer, uint64_t instanceOffset, uint32_t count, const Rhi::Handle<Rhi::Pipeline>& pipeline, const Rhi::Handle<Rhi::DescriptorSet>& objectSet, const Rhi::Handle<Rhi::DescriptorSet>& materialSet, uint32_t slot, bool bindMaterialStates){
			state.CommandBuffer->BindPipeline(pipeline);
			state.CommandBuffer->BindDescriptorSet(objectSet, 1);
			// 阴影管线的布局只有 set0/set1:多绑一个 set2 在 Vulkan 下是非法绑定(实测直接崩)。
			if (bindMaterialStates)
			{
				if (materialSet)
					state.CommandBuffer->BindDescriptorSet(materialSet, 2);
				else if (state.DefaultMaterialSets[slot % Renderer::FramesInFlight])
					state.CommandBuffer->BindDescriptorSet(state.DefaultMaterialSets[slot % Renderer::FramesInFlight], 2);
			}
			state.CommandBuffer->BindVertexBuffer(0, mesh.VertexBuffer);
			state.CommandBuffer->BindVertexBuffer(1, instanceBuffer, instanceOffset);
			state.CommandBuffer->BindIndexBuffer(mesh.IndexBuffer);
			state.CommandBuffer->DrawIndexed(indexCount, count, firstIndex);
			state.Stats.DrawCalls++;
			state.Stats.Triangles += (indexCount / 3) * count;
			state.Stats.InstancedBatches++;
			state.Stats.InstancedObjects += count;
		}

}

uint32_t Renderer3D::SubmitInstanced(const Ref<Mesh>& mesh, uint32_t submeshIndex, const Ref<Material>& material, const glm::mat4* transforms, const glm::vec4* colors, const int32_t* entityIds, uint32_t count){
		State& state = GetState();
		if (!mesh || !state.CommandBuffer || !state.InstancedPipeline || count == 0 || !transforms)
			return 0;
		// UINT32_MAX = 整网格提交(内置 primitive / 无 submesh 的资产,与逐物体路径同语义)。
		const bool wholeMesh = submeshIndex == UINT32_MAX;
		if (!wholeMesh && submeshIndex >= mesh->GetSubmeshes().size())
			return 0;
		if (state.ObjectIndex >= kObjectsPerFrame)
		{
			state.Stats.DroppedObjects++;
			return 0;
		}
		EnsureMeshBuffersFor(state, mesh);
		const auto cached = state.MeshCache.find(mesh.get());
		if (cached == state.MeshCache.end() || !cached->second.VertexBuffer)
			return 0;
		uint32_t indexCount = cached->second.IndexCount;
		uint32_t firstIndex = 0;
		if (!wholeMesh)
		{
			const MeshSubmesh& submesh = mesh->GetSubmeshes()[submeshIndex];
			indexCount = submesh.IndexCount;
			firstIndex = submesh.IndexOffset;
		}
		if (indexCount == 0)
			return 0;

		const uint32_t slot = Renderer::FrameSlot() % Renderer::FramesInFlight;
		uint64_t instanceOffset = 0;
		if (!UploadInstances(state, slot, transforms, colors, entityIds, count, &instanceOffset))
			return 0;

		// 整批共享的对象槽位:材质标量来自 material,模型矩阵用单位阵(真实模型矩阵在实例属性里),
		// 实体 id 用 -1(per-instance 给出)。
		const uint32_t index = state.ObjectIndex++;
		// M4-S3:表面材质照常参与实例合批 —— 表面模板自带 VSMainInstanced(per-instance
		// 模型矩阵/颜色/实体 id),所以这里换的是"管线 + 材质集",不是"退出合批"。
		SurfaceDrawState surface;
		const MaterialDesc* desc = material ? &material->GetDesc() : nullptr;
		if (desc)
			surface = PrepareSurfaceDraw(state, material, SurfacePipelineVariant::Instanced, slot);
		ObjectUniforms uniforms;
		uniforms.Model = glm::mat4(1.0f);
		uniforms.EntityId = { -1, 0, 0, 0 };
		if (desc)
		{
			uniforms.BaseColor = desc->BaseColor;
			uniforms.MetallicRoughness = { desc->Metallic, desc->Roughness, 0.0f, 0.0f };
			uniforms.Emissive = { desc->Emissive.x, desc->Emissive.y, desc->Emissive.z, 0.0f };
			uniforms.Flags = {
				desc->AlbedoTexture.empty() ? 0.0f : 1.0f,
				desc->NormalTexture.empty() ? 0.0f : 1.0f,
				desc->DoubleSided ? 1.0f : 0.0f,
				NormalBc5Flag(desc) };
		}
		else
		{
			// 纯色物体:颜色走 per-instance 属性,这里只给中性的标量(与旧路径同款默认)。
			uniforms.BaseColor = glm::vec4(1.0f);
			uniforms.MetallicRoughness = { 0.0f, 0.5f, 0.0f, 0.0f };
			uniforms.Emissive = { 0.0f, 0.0f, 0.0f, 0.0f };
			uniforms.Flags = { 0.0f, 0.0f, 0.0f, 0.0f };
		}
		WriteObjectUniforms(state, slot, index, uniforms,
			state.ObjectUniformBuffers[slot][index], state.ObjectSets[slot][index],
			state.DefaultPaletteBuffer, surface.ParamBuffer);

		Rhi::Handle<Rhi::DescriptorSet> materialSet;
		const Rhi::Handle<Rhi::Pipeline>& batchPipeline =
			surface.Active ? surface.Pipeline : state.InstancedPipeline;
		if (desc)
			materialSet = surface.Active ? surface.SurfaceSet : MaterialSetFor(state, material, slot);
		DrawInstancedBatch(state, cached->second, indexCount, firstIndex,
			state.InstanceBuffers[slot], instanceOffset, count, batchPipeline,
			state.ObjectSets[slot][index], materialSet, slot, true);
		return count;
	}


uint32_t Renderer3D::SubmitShadowInstanced(const Ref<Mesh>& mesh, uint32_t submeshIndex, const glm::mat4* transforms, uint32_t count){
		State& state = GetState();
		if (!mesh || !state.CommandBuffer || !state.InstancedShadowPipeline || count == 0 || !transforms)
			return 0;
		const bool wholeMesh = submeshIndex == UINT32_MAX;
		if (!wholeMesh && submeshIndex >= mesh->GetSubmeshes().size())
			return 0;
		if (state.ShadowObjectIndex >= kObjectsPerFrame)
			return 0;
		EnsureMeshBuffersFor(state, mesh);
		const auto cached = state.MeshCache.find(mesh.get());
		if (cached == state.MeshCache.end() || !cached->second.VertexBuffer)
			return 0;
		uint32_t indexCount = cached->second.IndexCount;
		uint32_t firstIndex = 0;
		if (!wholeMesh)
		{
			const MeshSubmesh& submesh = mesh->GetSubmeshes()[submeshIndex];
			indexCount = submesh.IndexCount;
			firstIndex = submesh.IndexOffset;
		}
		if (indexCount == 0)
			return 0;
		const uint32_t slot = Renderer::FrameSlot() % Renderer::FramesInFlight;
		uint64_t instanceOffset = 0;
		if (!UploadInstances(state, slot, transforms, nullptr, nullptr, count, &instanceOffset))
			return 0;
		// 阴影通道只读 u_ShadowViewProjection 与实例矩阵;对象 UBO 仍要给一个合法的单位阵。
		const uint32_t shadowIndex = state.ShadowObjectIndex++;
		ObjectUniforms uniforms;
		uniforms.Model = glm::mat4(1.0f);
		WriteObjectUniforms(state, slot, shadowIndex, uniforms,
			state.ShadowUniformBuffers[slot][shadowIndex], state.ShadowObjectSets[slot][shadowIndex]);
		DrawInstancedBatch(state, cached->second, indexCount, firstIndex,
			state.InstanceBuffers[slot], instanceOffset, count, state.InstancedShadowPipeline,
			state.ShadowObjectSets[slot][shadowIndex], nullptr, slot, false);
		return count;
	}


uint32_t Renderer3D::ReserveSlotBase(uint32_t identity, uint32_t span){
		// 预留区:序号 0..kSceneSlotCount-1 归主场景的逐帧分配(SceneRenderer),
		// 之上按 identity 稳定映射,保证同一调用方每帧写同一批槽位。
		constexpr uint32_t kSceneSlotCount = 16;
		const uint32_t count = span == 0 ? 1 : span;
		const uint32_t slotCount = kObjectsPerFrame - kSceneSlotCount;   // 48 个槽位可用
		// 取模上界必须是"槽位总数 - 需要连续占用的数量 + 1",这样 base..base+count-1 不会越界;
		// 之前写成 (identity % (usable - count + 1)) 之外的变体时,多个面板会撞到同一槽位、
		// 互相覆盖 → 预览闪烁(用户实测"选贴图后任何材质都闪烁")。
		const uint32_t range = slotCount > count ? slotCount - count + 1 : 1;
		const uint32_t base = identity % range;
		return kSceneSlotCount + base;
	}


uint32_t Renderer3D::SubmitAtSlot(uint32_t slotBase, const Ref<Mesh>& mesh, const Ref<Material>& material, const glm::mat4& transform, int32_t entityId){
		// 用固定序号提交:直接把 state.ObjectIndex 顶到 slotBase,让后续分配落在该槽位。
		State& state = GetState();
		if (!state.CommandBuffer || !state.Pipeline || slotBase >= kObjectsPerFrame)
			return UINT32_MAX;
		state.ObjectIndex = slotBase;
		const uint32_t result = Submit(mesh, material, transform, entityId);
		state.ObjectIndex = slotBase + 1;   // 同一调用方若还要再画一个,落在下一个槽位
		return result;
	}


uint32_t Renderer3D::SubmitSubmeshAtSlot(uint32_t slotBase, const Ref<Mesh>& mesh, uint32_t submeshIndex, const Ref<Material>& material, const glm::mat4& transform, int32_t entityId){
		// 与 SubmitAtSlot 同款:固定序号提交,避免预览与主场景争用对象槽位。
		State& state = GetState();
		if (!state.CommandBuffer || !state.Pipeline || slotBase >= kObjectsPerFrame)
			return UINT32_MAX;
		state.ObjectIndex = slotBase;
		const uint32_t result = SubmitSubmesh(mesh, submeshIndex, material, transform, entityId);
		state.ObjectIndex = slotBase + 1;
		return result;
	}


uint32_t Renderer3D::SubmitSkinnedAtSlot(uint32_t slotBase, const Ref<Mesh>& mesh, uint32_t submeshIndex, const Ref<Material>& material, const glm::mat4& transform, const glm::mat4* palette, uint32_t paletteCount, int32_t entityId){
		// 与 SubmitSubmeshAtSlot 同款:把对象序号顶到 slotBase(持久槽位),并让调色板走保留区
		// (键 = 对象槽位)。顺序分配的调色板游标与保留区互不重叠 —— 预览的 BeginScene 清零
		// 游标也不会碰主场景已写好的对象 UBO/调色板(见 kPaletteReservedBase)。
		State& state = GetState();
		if (!state.CommandBuffer || !state.SkinnedPipeline || slotBase >= kObjectsPerFrame)
			return UINT32_MAX;
		state.ObjectIndex = slotBase;
		const uint32_t result = SubmitSkinnedInternal(mesh, submeshIndex, material, nullptr, transform,
			palette, paletteCount, entityId, /*shadow*/ false, /*reservedPalette*/ true);
		state.ObjectIndex = slotBase + 1;   // 同一调用方若还要再画一个 submesh,落在下一个槽位
		return result;
	}


void Renderer3D::EndScene(){
		State& state = GetState();
		state.CommandBuffer = nullptr;
	}


void Renderer3D::InvalidateMaterialCache(){
		GetState().MaterialCache.clear();
		// M4-S3:表面材质的描述符集引用材质贴图缓存里的贴图,一起失效(下一帧重建)。
		GetState().SurfaceCache.clear();
		TextureLibrary::Get().Clear();
	}


	// ---- D4:灯光收集/打包(纯函数部分) ----
LightRig Renderer3D::BuildLightRig(const std::vector<DirectionalLightData>& directionalLights, const std::vector<PointLightData>& pointLights, const AmbientLightData* ambient, bool glDepthConvention){
		LightRig rig;
		// 深度约定:Vulkan 的裁剪空间 z∈[0,1] 直接就是深度缓冲值;GL 的 z∈[-1,1] 会被
		// 硬编码的 window-depth 映射成 (z+1)/2。着色器按这个标志换算(见 u_LightCounts.z)。
		rig.Uniforms.LightCounts.z = glDepthConvention ? 1u : 0u;
		if (ambient)
			rig.Uniforms.Ambient = { ambient->Color.r, ambient->Color.g, ambient->Color.b, ambient->Intensity };
		// ambient == nullptr:用结构体里的默认值(0.25 灰、强度 1)——
		// 与 D4 之前的占位实现同观感,既有场景(没有任何灯光组件)不会突然全黑。

		// 观感不回退:场景里**没有任何方向光**时,注入一盏与 D4 之前占位实现逐位一致的
		// 默认主光(DirectionalLightData 的默认值 = 方向 (0.35,-0.7,0.6) 归一化 / 白 / 1.0 / 不投影);
		// 一旦场景里存在方向光,默认主光立即让位(哪怕那盏灯强度为 0)。
		std::vector<DirectionalLightData> fallbackDirectional;
		const std::vector<DirectionalLightData>* directional = &directionalLights;
		if (directionalLights.empty())
		{
			fallbackDirectional.push_back(DirectionalLightData {});
			directional = &fallbackDirectional;
		}

		// D8a2:上限来自项目清单(引擎用户可配置),各字段在清单校验里已限幅到容量内;
		// 这里再 clamp 一次,防止运行期用 RenderSettings::Set 传入越界值(编辑器面板即时预览)。
		const Asset::RenderingSettings& settings = RenderSettings::Get();
		const uint32_t maxDirectional = std::min(settings.MaxDirectionalLights, MaxDirectionalLightCapacity);
		const uint32_t maxPoint = std::min(settings.MaxPointLights, MaxPointLightCapacity);
		const uint32_t directionalCount = std::min<uint32_t>(
			static_cast<uint32_t>(directional->size()), maxDirectional);
		const uint32_t pointCount = std::min<uint32_t>(
			static_cast<uint32_t>(pointLights.size()), maxPoint);

		uint32_t slot = 0;
		for (uint32_t index = 0; index < directionalCount; ++index)
		{
			const DirectionalLightData& source = (*directional)[index];
			// 方向归一化;零向量(组件刚加上、还没填方向)回退 -Y,避免 NaN 光照。
			const glm::vec3 direction = glm::length(source.Direction) > 1e-5f
				? glm::normalize(source.Direction) : glm::vec3(0.0f, -1.0f, 0.0f);
			LightUniforms::Light& light = rig.Uniforms.Lights[slot++];
			light.PositionType = { 0.0f, 0.0f, 0.0f, 1.0f };   // w = 1 → 方向光
			light.ColorIntensity = { source.Color.r, source.Color.g, source.Color.b, source.Intensity };
			light.DirectionRange = { direction.x, direction.y, direction.z, 0.0f };
			if (source.CastShadow)
				rig.ShadowCaster = true;
		}
		for (uint32_t index = 0; index < pointCount; ++index)
		{
			const PointLightData& source = pointLights[index];
			LightUniforms::Light& light = rig.Uniforms.Lights[slot++];
			light.PositionType = { source.Position.x, source.Position.y, source.Position.z, 0.0f };
			light.ColorIntensity = { source.Color.r, source.Color.g, source.Color.b, source.Intensity };
			light.DirectionRange = { source.Range, 0.0f, 0.0f, 0.0f };
		}

		rig.DirectionalLights = directionalCount;
		rig.PointLights = pointCount;
		rig.TotalLights = directionalCount + pointCount;
		// 截断数按**场景来源**计:默认主光是引擎注入的观感补偿,不算来源、也不算被丢弃。
		const uint32_t sourceDirectional = static_cast<uint32_t>(directionalLights.size());
		const uint32_t sourcePoint = static_cast<uint32_t>(pointLights.size());
		rig.DroppedLights = (sourceDirectional - std::min(sourceDirectional, maxDirectional))
			+ (sourcePoint - std::min(sourcePoint, maxPoint));
		rig.Uniforms.LightCounts.x = directionalCount;
		rig.Uniforms.LightCounts.y = pointCount;
		// D8a2:阴影贴图边长参与 PCF 纹素换算,必须与 Init 创建的资源一致(项目清单可改)。
		rig.Uniforms.ShadowParams.z = static_cast<float>(GetShadowMapSize());
		return rig;
	}

}
