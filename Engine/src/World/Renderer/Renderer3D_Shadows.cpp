#include "Renderer3D_Internal.h"

namespace World
{

using namespace Renderer3DDetail;

namespace Renderer3DDetail
{

		// D5c-3b:同一份对象 UBO 写入 + 把骨骼调色板写进 set 1 binding 3。
		// boneBuffer 为空 = 非蒙皮绘制:binding 3 用 State::DefaultPaletteBuffer 兜底,
		// 保证**每一个** set 1 都同时更新 binding 1 和 binding 3 —— Vulkan 的管线静态使用
		// u_Bones(声明在着色器里),一个只写了 binding 1 的描述符集在 vkCmdDrawIndexed 时
		// 会被验证层判为 VUID-vkCmdDrawIndexed-None-08600。
		// **GL 后端的 DescriptorSet::Update 是"整体替换"语义**(OpenGLDescriptorSet::Update 直接
		// 覆盖 m_Writes),分两次写会丢掉对象绑定 —— 所以 binding 1/3 必须在同一次 Update 里提交。
		// **GL 的 UBO 单元号 = binding(忽略 set)**:binding 3 是全局唯一的骨骼单元,
		// 曾经的 binding 2 与 set0 的灯光 UBO 同号,每个物体的 set1 绑定都会覆盖灯光 UBO
		// (GL 像素基线打红、实体 id 附件却一致)。占用表:0=相机、1=物体、2=灯光、3=骨骼。
void WriteObjectUniforms(State& state, uint32_t slot, uint32_t index, const ObjectUniforms& uniforms, Rhi::Handle<Rhi::Buffer>& buffer, Rhi::Handle<Rhi::DescriptorSet>& set, Rhi::Handle<Rhi::Buffer>& boneBuffer, const Rhi::Handle<Rhi::Buffer>& paramBuffer){
			// 每对象 UBO 独立分配:提交期写入不会与同帧其它对象互相覆盖。
			if (!buffer)
			{
				Rhi::BufferDesc uniformDesc;
				uniformDesc.Size = sizeof(ObjectUniforms);
				uniformDesc.Usage = Rhi::BufferUsageUniform;
				uniformDesc.Memory = Rhi::MemoryHint::HostVisible;
				uniformDesc.DebugName = "Renderer3D.ObjectUBO";
				buffer = Renderer::GetDevice()->CreateBuffer(uniformDesc);
			}
			if (!set)
				set = Renderer::GetDevice()->CreateDescriptorSet(state.ObjectLayout);
			buffer->SetData(&uniforms, sizeof(uniforms));
			Rhi::DescriptorWrite write;
			write.Binding = 1;
			write.Type = Rhi::DescriptorType::UniformBuffer;
			write.Buffer = buffer;
			Rhi::DescriptorWrite bones;
			bones.Binding = 3;
			bones.Type = Rhi::DescriptorType::UniformBuffer;
			bones.Buffer = boneBuffer ? boneBuffer : state.DefaultPaletteBuffer;
			// M4-S3:binding 4 = 表面材质参数块(register b4, space1)。**每次**都要写:
			// 描述符集按"帧槽位 × 对象序号"复用,同一序号在不同帧可能分别是表面绘制与
			// 引擎绘制 —— 只写 1/3 会让 binding 4 留着上一批已释放的参数缓冲(野描述符)。
			// 非表面绘制用共享的零值参数缓冲兜底(引擎着色器不读它,只为"永不留野句柄")。
			Rhi::DescriptorWrite params;
			params.Binding = 4;
			params.Type = Rhi::DescriptorType::UniformBuffer;
			params.Buffer = paramBuffer ? paramBuffer : state.SurfaceDefaultParamBuffer;
			set->Update({ write, bones, params });
			(void)slot; (void)index;
		}


		// 不关心参数块的调用方(阴影/常量色/引擎路径):binding 4 交给共享零值缓冲。
void WriteObjectUniforms(State& state, uint32_t slot, uint32_t index, const ObjectUniforms& uniforms, Rhi::Handle<Rhi::Buffer>& buffer, Rhi::Handle<Rhi::DescriptorSet>& set, Rhi::Handle<Rhi::Buffer>& boneBuffer){
			WriteObjectUniforms(state, slot, index, uniforms, buffer, set, boneBuffer,
				state.SurfaceDefaultParamBuffer);
		}


		// 非蒙皮路径的便捷重载:binding 3 交给默认调色板(7 参数版里 boneBuffer == nullptr 的分支)。
void WriteObjectUniforms(State& state, uint32_t slot, uint32_t index, const ObjectUniforms& uniforms, Rhi::Handle<Rhi::Buffer>& buffer, Rhi::Handle<Rhi::DescriptorSet>& set){
			Rhi::Handle<Rhi::Buffer> noPalette;
			WriteObjectUniforms(state, slot, index, uniforms, buffer, set, noPalette);
		}


		// D5c-3b:把 CPU 侧的关节调色板写进当前帧槽位的调色板 UBO。
		// 调色板长度超过 MaxBonePalette / 为空的话由调用方在更早处拒绝,这里是"已校验"路径。
		// P3-1③:着色器把关节下标 clamp 到 [0,127],它拿不到 paletteCount;所以这里把
		// [paletteCount, MaxBonePalette) 的尾段全部填成 palette[paletteCount-1] —— 越界关节
		// (含 >127)读到的就是最后一个有效矩阵,等价于 min(joint, paletteCount-1),也不会
		// 读到这份复用 8KB 缓冲里上一次绘制的残留(P3-1③ 之前的已知限制)。
		// 代价:每次蒙皮绘制上传完整 8KB 而不是 paletteCount×64B;换来的是与绘制顺序无关的
		// 确定性结果,且不需要改 HLSL 的 u_Bones[128] 布局(着色器不在本任务文件边界内)。
void WriteBoneUniforms(State& state, uint32_t slot, const glm::mat4* palette, uint32_t paletteCount, Rhi::Handle<Rhi::Buffer>& buffer){
			if (!buffer)
			{
				Rhi::BufferDesc desc;
				desc.Size = sizeof(BoneUniforms);
				desc.Usage = Rhi::BufferUsageUniform;
				desc.Memory = Rhi::MemoryHint::HostVisible;
				desc.DebugName = "Renderer3D.BoneUBO";
				buffer = Renderer::GetDevice()->CreateBuffer(desc);
			}
			if (!buffer)
				return;
			BoneUniforms padded;
			std::memcpy(padded.Bones, palette, static_cast<size_t>(paletteCount) * sizeof(glm::mat4));
			const glm::mat4& last = palette[paletteCount - 1];
			for (uint32_t bone = paletteCount; bone < Renderer3D::MaxBonePalette; ++bone)
				padded.Bones[bone] = last;
			buffer->SetData(&padded, sizeof(padded));
			(void)state; (void)slot;
		}


void BindObject(State& state, uint32_t slot, uint32_t index, const MeshGpu& mesh, const Rhi::Handle<Rhi::Pipeline>& pipeline, const Rhi::Handle<Rhi::DescriptorSet>& objectSet, const Rhi::Handle<Rhi::DescriptorSet>& materialSet, uint32_t indexCount, uint32_t firstIndex){
			state.CommandBuffer->BindPipeline(pipeline);
			state.CommandBuffer->BindDescriptorSet(objectSet, 1);
			if (materialSet)
				state.CommandBuffer->BindDescriptorSet(materialSet, 2);
			else if (state.DefaultMaterialSets[slot % Renderer::FramesInFlight])
				state.CommandBuffer->BindDescriptorSet(state.DefaultMaterialSets[slot % Renderer::FramesInFlight], 2);
			state.CommandBuffer->BindVertexBuffer(0, mesh.VertexBuffer);
			state.CommandBuffer->BindIndexBuffer(mesh.IndexBuffer);
			// D5:firstIndex 让"一个顶点/索引缓冲 + 多个 submesh"共用同一份 GPU 资源,
			// 每个 submesh 仍是独立绘制与独立对象槽位(后端已支持 BaseVertex 语义)。
			state.CommandBuffer->DrawIndexed(indexCount, 1, firstIndex);
			state.Stats.DrawCalls++;
			state.Stats.Triangles += indexCount / 3;
			(void)slot; (void)index;
		}


		// ---- M4-S3:表面函数材质(HLSL 代码态)----
		//
		// 一个材质要不要走表面管线,取决于"材质键 + 已发布版本":
		//  - 键来自 Material::SurfaceKey()(保存态 = 着色器内容根路径;编辑器里未保存的
		//    实时改动 = `<路径>#preview`,只有该面板的预览材质用它 —— D2 的键分离);
		//  - 版本由 MaterialSurfaceRuntime::Install 递增,0 = 从未安装 → 完全走引擎管线。
		// 表面模板的顶点输出与引擎着色器不兼容,所以选了表面管线就必须同时用它的 VS/PS。

		// set 2 里"着色器静态使用、必须写描述符"的 binding 掩码:
		// 1 = albedo、2 = normal(表面模板总是静态采样),4.. = 反射到的贴图参数槽。
uint32_t SurfaceNeededBindings(const MaterialParamLayout& layout, bool hasLayout){
			uint32_t mask = (1u << 1) | (1u << 2);
			if (!hasLayout)
				return mask;
			for (const MaterialParamTextureSlot& slot : layout.Textures)
			{
				if (slot.Set == 2 && slot.Binding < 32)
					mask |= (1u << slot.Binding);
			}
			return mask;
		}

}

	// HOTR-P1-T3:引擎内建 shader 热重载 —— **只**重建 6 个 shader 对象 + 7 条管线:
	// 描述符布局 / 主通道与阴影通道 render pass / 阴影贴图与帧缓冲 / 默认调色板与灯光缓冲 /
	// 表面管线环境快照全部保留。任一 stage 编不出字节码或任一条管线建不出来 → 整个 owner
	// 放弃(旧句柄一个都不动),返回 0 并把可读原因写进 error。成功返回重建的管线数;
	// 旧管线走 QueueRelease 延迟释放(Vulkan 在飞命令缓冲仍引用旧 VkPipeline)。
uint32_t Renderer3D::ReloadShaders(std::string* error){
		WLD_PROFILE_FUNCTION();
		State& state = GetState();
		if (!Renderer::GetDevice() || !state.RenderPass || !state.ShadowPass
			|| !state.ObjectLayout || !state.MaterialLayout)
		{
			if (error) *error = "Renderer3D is not initialized";
			return 0;
		}

		const Rhi::FrontFace frontFace = Renderer::GetBackendName() == "vulkan"
			? Rhi::FrontFace::Clockwise : Rhi::FrontFace::CounterClockwise;
		const Rhi::SampleCount sceneSamples = static_cast<Rhi::SampleCount>(RenderSettings::Msaa());

		EnginePipelineSet pipelines;
		std::string reason;
		if (!CreateEnginePipelines(state, frontFace, sceneSamples, /*reload*/ true, pipelines, &reason))
		{
			if (error) *error = reason.empty() ? std::string("engine pipeline rebuild failed") : reason;
			return 0;
		}

		const Rhi::Handle<Rhi::Pipeline> oldSolid = state.Pipeline;
		const Rhi::Handle<Rhi::Pipeline> oldInstanced = state.InstancedPipeline;
		const Rhi::Handle<Rhi::Pipeline> oldTransparent = state.TransparentPipeline;
		const Rhi::Handle<Rhi::Pipeline> oldShadow = state.ShadowPipeline;
		const Rhi::Handle<Rhi::Pipeline> oldInstancedShadow = state.InstancedShadowPipeline;
		const Rhi::Handle<Rhi::Pipeline> oldSkinned = state.SkinnedPipeline;
		const Rhi::Handle<Rhi::Pipeline> oldSkinnedShadow = state.SkinnedShadowPipeline;
		state.Pipeline = pipelines.Solid;
		state.InstancedPipeline = pipelines.Instanced;
		state.TransparentPipeline = pipelines.Transparent;
		state.ShadowPipeline = pipelines.Shadow;
		state.InstancedShadowPipeline = pipelines.InstancedShadow;
		state.SkinnedPipeline = pipelines.Skinned;
		state.SkinnedShadowPipeline = pipelines.SkinnedShadow;
		Renderer::QueueRelease([oldSolid, oldInstanced, oldTransparent, oldShadow, oldInstancedShadow,
			oldSkinned, oldSkinnedShadow]() {});
		return 7;
	}


void Renderer3D::PurgeMeshGpu(const Mesh* mesh)
{
	if (!mesh)
		return;
	State& state = GetState();
	const auto found = state.MeshCache.find(mesh);
	if (found == state.MeshCache.end())
		return;

	// 先移出容器的所有权,再交给延迟释放:释放发生在"该帧槽位下一次开始前",
	// 那时在飞命令缓冲一定已经完成(与材质/管线热重载同一套纪律)。
	MeshGpu gpu = std::move(found->second);
	state.MeshCache.erase(found);
	Renderer::QueueRelease([gpu]() mutable
	{
		gpu.VertexBuffer = nullptr;
		gpu.IndexBuffer = nullptr;
		gpu.Owner.reset();
	});
}

void Renderer3D::Shutdown(){
		State& state = GetState();
		for (auto& slot : state.ObjectUniformBuffers)
			for (Rhi::Handle<Rhi::Buffer>& buffer : slot)
				buffer = nullptr;
		for (auto& slot : state.ObjectSets)
			for (Rhi::Handle<Rhi::DescriptorSet>& set : slot)
				set = nullptr;
		for (auto& slot : state.ShadowUniformBuffers)
			for (Rhi::Handle<Rhi::Buffer>& buffer : slot)
				buffer = nullptr;
		for (auto& slot : state.ShadowObjectSets)
			for (Rhi::Handle<Rhi::DescriptorSet>& set : slot)
				set = nullptr;
		state.ShadowObjectIndex = 0;
		state.DefaultLightBuffer = nullptr;
		for (Rhi::Handle<Rhi::DescriptorSet>& set : state.DefaultMaterialSets)
			set = nullptr;
		state.MeshCache.clear();
		state.MaterialCache.clear();
		// 材质贴图缓存持有 RHI 纹理句柄:必须在设备销毁前放掉,否则退出时
		// vkDestroyDevice 会报 "VkImage has not been destroyed"(实测 20+ 条 VUID)。
		TextureLibrary::Get().Clear();
		state.Pipeline = nullptr;
		state.TransparentPipeline = nullptr;
		// D8b-2:实例化管线与实例缓冲也要在设备销毁前放掉。
		state.InstancedPipeline = nullptr;
		state.InstancedShadowPipeline = nullptr;
		for (Rhi::Handle<Rhi::Buffer>& buffer : state.InstanceBuffers)
			buffer = nullptr;
		state.InstanceCursor = 0;
		// D5c-3b:骨骼调色板资源(默认 8KB 缓冲 + 每帧槽位的蒙皮调色板 UBO/描述符集)。
		state.SkinnedPipeline = nullptr;
		state.SkinnedShadowPipeline = nullptr;
		state.DefaultPaletteBuffer = nullptr;
		for (auto& slot : state.PaletteBuffers)
			for (Rhi::Handle<Rhi::Buffer>& buffer : slot)
				buffer = nullptr;
		state.PaletteCursor = 0;
		// D4:阴影资源必须在设备销毁前放掉(与材质贴图缓存同理)。
		state.ShadowPipeline = nullptr;
		state.ShadowFramebuffer = nullptr;
		state.ShadowMapTexture = nullptr;
		state.ShadowColorTexture = nullptr;
		state.ShadowSampler = nullptr;
		state.ShadowPass = nullptr;
		state.RenderPass = nullptr;
		state.ObjectLayout = nullptr;
		state.MaterialLayout = nullptr;
		// M4-S3:表面管线(Runtime 持有的)与表面材质的 GPU 状态也要在设备销毁前放掉。
		// Renderer 已经 WaitIdle,所以直接放句柄;这里**不能**走 QueueRelease ——
		// 延迟释放排的是"将来某一帧",句柄会活过设备销毁(静态析构里撞已销毁设备)。
		MaterialSurfaceRuntime::Shutdown();
		ClearSurfacePipelineEnvironment();
		state.SurfaceCache.clear();
		for (Rhi::Handle<Rhi::DescriptorSet>& set : state.SurfaceDefaultSets)
			set = nullptr;
		state.SurfaceDefaultParamBuffer = nullptr;
		state.SurfaceMaterialLayout = nullptr;
		state.PendingSurfaceUpdates.clear();
		state.PendingSurfaceSlots.clear();
		state.MaterialSampler = nullptr;
		state.CommandBuffer = nullptr;
		// 预览类调用方会把自己的 set0 登记进来(SetGlobalDescriptorSet):
		// 这里必须一并清掉,否则设备销毁后该句柄悬空,退出期会崩(实测 0xC0000005)。
		state.GlobalSet = nullptr;
		state.ObjectIndex = 0;
		state.Stats = {};
		state.LastLoggedDirectional = UINT32_MAX;
		state.LastLoggedPoint = UINT32_MAX;
		state.LastLoggedDropped = UINT32_MAX;
		state.LastLoggedShadow = -1;
	}


void Renderer3D::EnsureMeshBuffers(const Ref<Mesh>& mesh){
		// 实现在匿名命名空间的 EnsureMeshBuffersFor(逐 submesh 的提交辅助函数也要用它)。
		EnsureMeshBuffersFor(GetState(), mesh);
	}


void Renderer3D::BeginScene(const glm::mat4&, const Rhi::Handle<Rhi::CommandBuffer>& commandBuffer){
		// viewProjection 由全局相机 UBO 提供(SceneRenderer 已绑定 set 0),这里只需要命令缓冲与序号复位。
		State& state = GetState();
		// 上一帧挂起的材质描述符在这里补写:此时既不在渲染通道内,也没有在录制的命令缓冲,
		// 是唯一对驱动安全的写入时机(见 MaterialSetFor 的说明)。
		FlushMaterialUpdates(state);
		// M4-S3:表面材质的描述符(set 2:贴图槽)同样在这里补写。
		FlushSurfaceUpdates(state);
		state.CommandBuffer = commandBuffer;
		state.ObjectIndex = 0;
		// D8b-2:每批场景从这里开始重新分配实例缓冲区(帧槽位由帧栅栏保护)。
		state.InstanceCursor = 0;
		// D5c-3b:蒙皮调色板游标(帧槽位由帧栅栏保护,见 State::PaletteBuffers 的说明)。
		state.PaletteCursor = 0;
	}


void Renderer3D::BindPipelineForCurrentPass(){
		State& state = GetState();
		if (state.CommandBuffer && state.Pipeline)
		{
			state.CommandBuffer->BindPipeline(state.Pipeline);
			// 管线布局就绪后再绑 set 0:后端的描述符绑定需要布局,布局为空时绑定会被
			// 推迟到下一次 BindPipeline(实测:预览相机矩阵因此丢失、几何不出现)。
			if (state.GlobalSet)
				state.CommandBuffer->BindDescriptorSet(state.GlobalSet, 0);
		}
	}


void Renderer3D::SetGlobalDescriptorSet(const Rhi::Handle<Rhi::DescriptorSet>& set){
		// 只登记;真正的 vkCmdBindDescriptorSets 在管线绑定后进行(见 BindPipelineForCurrentPass)。
		GetState().GlobalSet = set;
	}


uint32_t Renderer3D::Submit(const Ref<Mesh>& mesh, const glm::mat4& transform, const glm::vec4& baseColor, int32_t entityId){
		return SubmitObject(GetState(), mesh, nullptr, baseColor, transform, entityId,
			mesh ? mesh->GetIndexCount() : 0u, 0u);
	}


uint32_t Renderer3D::Submit(const Ref<Mesh>& mesh, const Ref<Material>& material, const glm::mat4& transform, int32_t entityId){
		if (!material)
			return Submit(mesh, transform, glm::vec4(1.0f), entityId);
		return SubmitObject(GetState(), mesh, material, glm::vec4(1.0f), transform, entityId,
			mesh ? mesh->GetIndexCount() : 0u, 0u);
	}


uint32_t Renderer3D::SubmitSubmesh(const Ref<Mesh>& mesh, uint32_t submeshIndex, const Ref<Material>& material, const glm::mat4& transform, int32_t entityId){
		if (!mesh || submeshIndex >= mesh->GetSubmeshes().size())
			return UINT32_MAX;
		const MeshSubmesh& submesh = mesh->GetSubmeshes()[submeshIndex];
		return SubmitObject(GetState(), mesh, material, glm::vec4(1.0f), transform, entityId,
			submesh.IndexCount, submesh.IndexOffset);
	}


uint32_t Renderer3D::SubmitSubmesh(const Ref<Mesh>& mesh, uint32_t submeshIndex, const glm::vec4& baseColor, const glm::mat4& transform, int32_t entityId){
		if (!mesh || submeshIndex >= mesh->GetSubmeshes().size())
			return UINT32_MAX;
		const MeshSubmesh& submesh = mesh->GetSubmeshes()[submeshIndex];
		return SubmitObject(GetState(), mesh, nullptr, baseColor, transform, entityId,
			submesh.IndexCount, submesh.IndexOffset);
	}


	// ---- P1b D5c-3b:GPU 蒙皮 ----
uint32_t Renderer3D::SubmitSkinned(const Ref<Mesh>& mesh, uint32_t submeshIndex, const Ref<Material>& material, const glm::mat4& transform, const glm::mat4* palette, uint32_t paletteCount, int32_t entityId){
		// 材质为空:退化成常量色(与 Submit(mesh, material, …) 同款约定)。
		return SubmitSkinnedInternal(mesh, submeshIndex, material, nullptr, transform, palette,
			paletteCount, entityId, /*shadow*/ false);
	}

}
