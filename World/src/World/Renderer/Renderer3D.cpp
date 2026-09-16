#include "wldpch.h"
#include "World/Renderer/Renderer3D.h"

#include "World/Renderer/Renderer.h"
#include "World/Renderer/MaterialTextureCache.h"
#include "World/RHI/Vulkan/VulkanResources.h"
#include "World/Renderer/ShaderUtils.h"

#include <cstring>
#include <unordered_map>

namespace World
{
	namespace
	{
		// 每帧对象上限:对象 UBO/描述符集按"帧槽位 × 序号"预建,超出即拒绝(D8 用实例化替换)。
		//
		// 注意 64 而不是 32:对象序号是**跨调用方共享**的(主场景渲染器 + 各材质预览面板
		// 依次调用 BeginScene 复位序号)。如果序号空间不够,后来者会拿不到槽位;
		// 更隐蔽的是"序号复用"会让不同调用方争用同一份 UBO/描述符集——当两者写入的内容
		// 不同(例如两个材质面板各自的贴图),画面就会逐帧来回闪(用户实测"预览一直闪烁")。
		constexpr uint32_t kObjectsPerFrame = 64;

		struct ObjectUniforms
		{
			glm::mat4 Model { 1.0f };
			glm::vec4 BaseColor { 1.0f };
			// D3 材质标量(sRGB 空间颜色;标量用 vec4 承载,与 HLSL std140 布局逐字段对应)。
			glm::vec4 MetallicRoughness { 0.0f, 0.5f, 0.0f, 0.0f };
			glm::vec4 Emissive { 0.0f };
			glm::vec4 Flags { 0.0f };   // x = 有 albedo, y = 有法线, z = 双面
			// D7-1c:视口点选用的实体 id(SV_Target1),用 int4 承载(见 hlsl 里的说明:
			// 标量+短向量在 HLSL 与 std140 下偏移不一致,spirv-cross 会拒绝该块)。
			glm::ivec4 EntityId { -1, 0, 0, 0 };
		};
		static_assert(sizeof(ObjectUniforms) == 144, "ObjectUniforms must match Renderer3D_Solid.hlsl");

		struct MeshGpu
		{
			Rhi::Handle<Rhi::Buffer> VertexBuffer;
			Rhi::Handle<Rhi::Buffer> IndexBuffer;
			uint32_t IndexCount = 0;
		};

		struct State
		{
			Rhi::Handle<Rhi::RenderPass> RenderPass;
			Rhi::Handle<Rhi::Pipeline> Pipeline;             // 不透明
			Rhi::Handle<Rhi::Pipeline> TransparentPipeline;  // 混合 + 不写深度
			Rhi::Handle<Rhi::DescriptorSetLayout> ObjectLayout;
			Rhi::Handle<Rhi::DescriptorSetLayout> MaterialLayout;
			Rhi::Handle<Rhi::Sampler> MaterialSampler;
			Rhi::Handle<Rhi::CommandBuffer> CommandBuffer;
			Rhi::Handle<Rhi::Buffer> ObjectUniformBuffers[Renderer::FramesInFlight][kObjectsPerFrame];
			Rhi::Handle<Rhi::DescriptorSet> ObjectSets[Renderer::FramesInFlight][kObjectsPerFrame];
			std::unordered_map<const Mesh*, MeshGpu> MeshCache;
			// 每材质 × 帧槽位的贴图描述符集;材质 Revision 变化时重建。
			struct MaterialGpu
			{
				Rhi::Handle<Rhi::DescriptorSet> Sets[Renderer::FramesInFlight];
				uint32_t Revision = 0;
			};
			std::unordered_map<const Material*, MaterialGpu> MaterialCache;
			// set 0(全局相机)描述符集:预览这类"非 SceneRenderer 调用方"通过
			// SetGlobalDescriptorSet 传入,在管线绑定后(布局可用时)统一绑定。
			Rhi::Handle<Rhi::DescriptorSet> GlobalSet;
			// 待补写的材质描述符(见 MaterialSetFor 的说明:录制渲染通道期间不能
			// 调 vkUpdateDescriptorSets,否则驱动直接崩在 vkUpdateDescriptorSets)。
			std::vector<std::pair<const Material*, Ref<Material>>> PendingMaterialUpdates;
			std::vector<uint32_t> PendingMaterialSlots;
			uint32_t ObjectIndex = 0;
			Renderer3D::Statistics Stats;
		};

		State& GetState()
		{
			static State state;
			return state;
		}

		Rhi::Handle<Rhi::Shader> CreateSolidShader(const char* path, const char* debugName)
		{
			Rhi::ShaderDesc desc;
			desc.DebugName = debugName;
			desc.Stages.push_back(ShaderCompiler::CompileStage(Rhi::ShaderStage::Vertex, path, "VSMain", "vs_6_0"));
			desc.Stages.push_back(ShaderCompiler::CompileStage(Rhi::ShaderStage::Fragment, path, "PSMain", "ps_6_0"));
			return Renderer::GetDevice()->CreateShader(desc);
		}

		// ---- D3 提交辅助(把三处重复代码收敛到一处) ----
		void WriteObjectUniforms(State& state, uint32_t slot, uint32_t index, const ObjectUniforms& uniforms)
		{
			// 每对象 UBO 独立分配:提交期写入不会与同帧其它对象互相覆盖。
			if (!state.ObjectUniformBuffers[slot][index])
			{
				Rhi::BufferDesc uniformDesc;
				uniformDesc.Size = sizeof(ObjectUniforms);
				uniformDesc.Usage = Rhi::BufferUsageUniform;
				uniformDesc.Memory = Rhi::MemoryHint::HostVisible;
				uniformDesc.DebugName = "Renderer3D.ObjectUBO";
				state.ObjectUniformBuffers[slot][index] = Renderer::GetDevice()->CreateBuffer(uniformDesc);
			}
			if (!state.ObjectSets[slot][index])
				state.ObjectSets[slot][index] = Renderer::GetDevice()->CreateDescriptorSet(state.ObjectLayout);
			state.ObjectUniformBuffers[slot][index]->SetData(&uniforms, sizeof(uniforms));
			Rhi::DescriptorWrite write;
			write.Binding = 1;
			write.Type = Rhi::DescriptorType::UniformBuffer;
			write.Buffer = state.ObjectUniformBuffers[slot][index];
			state.ObjectSets[slot][index]->Update({ write });
		}

		void BindObject(State& state, uint32_t slot, uint32_t index, const MeshGpu& mesh,
			const Rhi::Handle<Rhi::Pipeline>& pipeline, const Rhi::Handle<Rhi::DescriptorSet>& objectSet,
			const Rhi::Handle<Rhi::DescriptorSet>& materialSet)
		{
			state.CommandBuffer->BindPipeline(pipeline);
			state.CommandBuffer->BindDescriptorSet(objectSet, 1);
			if (materialSet)
				state.CommandBuffer->BindDescriptorSet(materialSet, 2);
		state.CommandBuffer->BindVertexBuffer(0, mesh.VertexBuffer);
		state.CommandBuffer->BindIndexBuffer(mesh.IndexBuffer);
		state.CommandBuffer->DrawIndexed(mesh.IndexCount);
			state.Stats.DrawCalls++;
			state.Stats.Triangles += mesh.IndexCount / 3;
			(void)slot; (void)index;
		}

		// 每材质 × 帧槽位的贴图描述符集。材质 Revision 变化(编辑参数/换贴图/热重载)
		// 或后端重建(缓存被清空)时重建,保证"改了立刻生效"。
		Rhi::Handle<Rhi::DescriptorSet> MaterialSetFor(State& state, const Ref<Material>& material, uint32_t slot)
		{
			const MaterialDesc& desc = material->GetDesc();
			State::MaterialGpu& gpu = state.MaterialCache[material.get()];
			if (gpu.Revision != material->GetRevision() || !gpu.Sets[slot])
			{
				if (!gpu.Sets[slot])
					gpu.Sets[slot] = Renderer::GetDevice()->CreateDescriptorSet(state.MaterialLayout);
				// **不能在这里写描述符**:MaterialSetFor 是渲染通道录制期间调用的,
				// 而 vkUpdateDescriptorSets 不允许发生在渲染通道内部(实测驱动直接
				// 崩在 vkUpdateDescriptorSets:imageLayout=0xDDDDDDDD/imageView=0xDDDD…)。
				// 贴图选择与 sampler 在这里确定,真正的写入延迟到 EndScene(通道已结束)。
				state.PendingMaterialUpdates.emplace_back(material.get(), material);
				state.PendingMaterialSlots.push_back(slot);
			}
			return gpu.Sets[slot];
		}

		// 在渲染通道**之外**补写挂起的材质描述符(见 MaterialSetFor 的说明)。
		void FlushMaterialUpdates(State& state)
		{
			for (size_t i = 0; i < state.PendingMaterialUpdates.size(); ++i)
			{
				const Ref<Material>& material = state.PendingMaterialUpdates[i].second;
				const uint32_t slot = state.PendingMaterialSlots[i];
				State::MaterialGpu& gpu = state.MaterialCache[material.get()];
				if (!gpu.Sets[slot])
					continue;
				const MaterialDesc& desc = material->GetDesc();
				Rhi::DescriptorWrite albedo;
				albedo.Binding = 1;
				albedo.Type = Rhi::DescriptorType::CombinedImageSampler;
				albedo.Texture = MaterialTextureCache::Get().Get(desc.AlbedoTexture, /*srgb*/ true);
				albedo.Sampler = state.MaterialSampler;
				Rhi::DescriptorWrite normal;
				normal.Binding = 2;
				normal.Type = Rhi::DescriptorType::CombinedImageSampler;
				normal.Texture = MaterialTextureCache::Get().Get(desc.NormalTexture, /*srgb*/ false);
				normal.Sampler = state.MaterialSampler;
				gpu.Sets[slot]->Update({ albedo, normal });
				gpu.Revision = material->GetRevision();
			}
			state.PendingMaterialUpdates.clear();
			state.PendingMaterialSlots.clear();
		}

		void TraceSubmit(uint32_t index, uint32_t slot, uint32_t indexCount,
			const glm::mat4& transform, const glm::vec4& color)
		{
			// 诊断(WLD_TRACE_3D=1,只打印前 12 次提交):确认对象矩阵/颜色/缓冲是否真的送到 GPU。
			if (!std::getenv("WLD_TRACE_3D"))
				return;
			static int traced = 0;
			if (traced >= 12)
				return;
			traced++;
			WLD_CORE_INFO("[3d] submit#{0} slot={1} index={2} indices={3} model=[{4} {5} {6}] color=({7},{8},{9},{10})",
				traced, slot, index, indexCount,
				transform[3][0], transform[3][1], transform[3][2],
				color.r, color.g, color.b, color.a);
		}
	}

	void Renderer3D::Init()
	{
		WLD_PROFILE_FUNCTION();
		State& state = GetState();

		// set 1, binding 1:每对象 UBO(u_Model / u_BaseColor),顶点与像素阶段都要用。
		// binding 不能是 0:OpenGL 后端的 UBO 绑定单元 = binding(忽略 set 索引),
		// 用 0 会和 set0/binding0 的相机 UBO 抢同一个 unit,导致 GL 下 3D 全黑。
		Rhi::DescriptorSetLayoutDesc objectLayoutDesc;
		objectLayoutDesc.Bindings.push_back({ 1, Rhi::DescriptorType::UniformBuffer,
			Rhi::ShaderStageFlag(Rhi::ShaderStage::Vertex) | Rhi::ShaderStageFlag(Rhi::ShaderStage::Fragment), 1 });
		state.ObjectLayout = Renderer::GetDevice()->CreateDescriptorSetLayout(objectLayoutDesc);

		// D3 set 2:材质贴图(albedo = sRGB 贴图,normal = 线性贴图)。
		// binding 编号从 1 起:OpenGL 后端的绑定单元 = binding,避开 set0 的相机 UBO(0)。
		Rhi::DescriptorSetLayoutDesc materialLayoutDesc;
		materialLayoutDesc.Bindings.push_back({ 1, Rhi::DescriptorType::CombinedImageSampler,
			Rhi::ShaderStageFlag(Rhi::ShaderStage::Fragment), 1 });
		materialLayoutDesc.Bindings.push_back({ 2, Rhi::DescriptorType::CombinedImageSampler,
			Rhi::ShaderStageFlag(Rhi::ShaderStage::Fragment), 1 });
		materialLayoutDesc.DebugName = "Renderer3D.Material";
		state.MaterialLayout = Renderer::GetDevice()->CreateDescriptorSetLayout(materialLayoutDesc);

		// 材质采样器:重复寻址(平铺贴图常见需求)+ 线性过滤;mip 由贴图自带(当前单级)。
		Rhi::SamplerDesc materialSamplerDesc;
		materialSamplerDesc.MinFilter = Rhi::Filter::Linear;
		materialSamplerDesc.MagFilter = Rhi::Filter::Linear;
		materialSamplerDesc.AddressU = Rhi::SamplerAddressMode::Repeat;
		materialSamplerDesc.AddressV = Rhi::SamplerAddressMode::Repeat;
		materialSamplerDesc.DebugName = "Renderer3D.MaterialSampler";
		state.MaterialSampler = Renderer::GetDevice()->CreateSampler(materialSamplerDesc);

		// 与 SceneRenderer 目标结构一致的兼容渲染通道(颜色 + 实体 ID + 深度)。
		Rhi::RenderPassDesc passDesc;
		Rhi::RenderPassAttachment color;
		color.Format = Rhi::Format::R8G8B8A8_UNORM;
		color.Samples = Rhi::SampleCount::Count1;
		color.Load = Rhi::LoadOp::Clear;
		color.Store = Rhi::StoreOp::Store;
		color.InitialLayout = Rhi::AttachmentLayout::ColorAttachment;
		color.FinalLayout = Rhi::AttachmentLayout::ColorAttachment;
		Rhi::RenderPassAttachment entityId;
		entityId.Format = Rhi::Format::R32_SINT;
		entityId.Samples = Rhi::SampleCount::Count1;
		entityId.Load = Rhi::LoadOp::Clear;
		entityId.Store = Rhi::StoreOp::Store;
		entityId.InitialLayout = Rhi::AttachmentLayout::ColorAttachment;
		entityId.FinalLayout = Rhi::AttachmentLayout::ColorAttachment;
		Rhi::RenderPassAttachment depth;
		depth.Format = Rhi::Format::D24_UNORM_S8_UINT;
		depth.Samples = Rhi::SampleCount::Count1;
		depth.Load = Rhi::LoadOp::Clear;
		depth.Store = Rhi::StoreOp::Store;
		depth.InitialLayout = Rhi::AttachmentLayout::DepthStencilAttachment;
		depth.FinalLayout = Rhi::AttachmentLayout::DepthStencilAttachment;
		passDesc.Attachments = { color, entityId, depth };
		Rhi::SubpassDesc subpass;
		subpass.ColorAttachments = {
			{ 0, Rhi::AttachmentLayout::ColorAttachment },
			{ 1, Rhi::AttachmentLayout::ColorAttachment },
		};
		subpass.DepthStencilAttachment = { 2, Rhi::AttachmentLayout::DepthStencilAttachment };
		passDesc.Subpasses = { subpass };
		state.RenderPass = Renderer::GetDevice()->CreateRenderPass(passDesc);

		const MeshVertexLayout meshLayout = Mesh::MakeStandardLayout();
		Rhi::PipelineDesc pipelineDesc;
		pipelineDesc.Shader = CreateSolidShader("assets/shaders/Renderer3D_Solid.hlsl", "Renderer3D-Solid");
		pipelineDesc.RenderPass = state.RenderPass;
		// set 0 = 全局相机(SceneRenderer 绑定),set 1 = 每对象数据,set 2 = 材质贴图。
		pipelineDesc.DescriptorSetLayouts = { Renderer::GetGlobalDescriptorSetLayout(), state.ObjectLayout, state.MaterialLayout };
		pipelineDesc.VertexBindings = meshLayout.Bindings;
		pipelineDesc.VertexAttributes = meshLayout.Attributes;
		pipelineDesc.Topology = Rhi::PrimitiveTopology::TriangleList;
		// 剔除约定:网格按"外壁 = 从外侧看逆时针(CCW)"编写(Mesh::Create* 统一保证,
		// 回归见 World.Mesh)。正面判据必须与**后端的屏幕空间绕序约定**配套:
		// Vulkan 的帧缓冲 Y 向下,同样的世界绕序在它的光栅化约定里是反的,因此 Vulkan 用 CW。
		// 依据:离屏不再做 Y 翻转(见 ProjectionConventions.h)后实测——GL 用 CCW 正常,
		// Vulkan 用 CCW 会剔除外壁只剩内壁,改 CW 后恢复外壁。
		pipelineDesc.Front = Renderer::GetBackendName() == "vulkan"
			? Rhi::FrontFace::Clockwise : Rhi::FrontFace::CounterClockwise;
		pipelineDesc.Cull = Rhi::CullMode::Back;
		pipelineDesc.DepthStencil.DepthTest = true;
		pipelineDesc.DepthStencil.DepthWrite = true;
		// 深度约定:清值 1.0 + LessOrEqual(近处深度小者胜)。Vulkan 的 NDC z∈[0,1] 由
		// `ProjectionConventions.h` 的深度重映射保证(近平面 → 0、远平面 → 1),
		// 两个后端共用同一约定,因此**不再**保留比较方向的 A/B 开关。
		pipelineDesc.DepthStencil.DepthCompare = Rhi::CompareOp::LessOrEqual;
		state.Pipeline = Renderer::GetDevice()->CreatePipeline(pipelineDesc);

		// D3 透明管线:同着色器,+ alpha 混合、不写深度(深度测试仍然开,避免透明面互相穿透)。
		Rhi::PipelineDesc transparentDesc = pipelineDesc;
		transparentDesc.DepthStencil.DepthWrite = false;
		Rhi::BlendAttachmentState blend;
		blend.BlendEnable = true;
		blend.SrcColor = Rhi::BlendFactor::SrcAlpha;
		blend.DstColor = Rhi::BlendFactor::OneMinusSrcAlpha;
		blend.ColorOp = Rhi::BlendOp::Add;
		blend.SrcAlpha = Rhi::BlendFactor::One;
		blend.DstAlpha = Rhi::BlendFactor::OneMinusSrcAlpha;
		blend.AlphaOp = Rhi::BlendOp::Add;
		// 两个颜色附件:0 = 颜色(混合),1 = entity id(**必须关闭混合**,否则拾取 id 会被
		// 透明物体的 alpha 混坏)。
		Rhi::BlendAttachmentState idBlend;
		idBlend.BlendEnable = false;
		transparentDesc.Blends = { blend, idBlend };
		state.TransparentPipeline = Renderer::GetDevice()->CreatePipeline(transparentDesc);
	}

	void Renderer3D::Shutdown()
	{
		State& state = GetState();
		for (auto& slot : state.ObjectUniformBuffers)
			for (Rhi::Handle<Rhi::Buffer>& buffer : slot)
				buffer = nullptr;
		for (auto& slot : state.ObjectSets)
			for (Rhi::Handle<Rhi::DescriptorSet>& set : slot)
				set = nullptr;
		state.MeshCache.clear();
		state.MaterialCache.clear();
		// 材质贴图缓存持有 RHI 纹理句柄:必须在设备销毁前放掉,否则退出时
		// vkDestroyDevice 会报 "VkImage has not been destroyed"(实测 20+ 条 VUID)。
		MaterialTextureCache::Get().Clear();
		state.Pipeline = nullptr;
		state.TransparentPipeline = nullptr;
		state.RenderPass = nullptr;
		state.ObjectLayout = nullptr;
		state.MaterialLayout = nullptr;
		state.MaterialSampler = nullptr;
		state.CommandBuffer = nullptr;
		state.ObjectIndex = 0;
		state.Stats = {};
	}

	void Renderer3D::EnsureMeshBuffers(const Ref<Mesh>& mesh)
	{
		State& state = GetState();
		if (state.MeshCache.find(mesh.get()) != state.MeshCache.end())
			return;

		const MeshDesc& desc = mesh->GetDesc();
		Rhi::BufferDesc vertexDesc;
		vertexDesc.Size = desc.VertexData.size();
		vertexDesc.Usage = Rhi::BufferUsageVertex;
		vertexDesc.InitialData = desc.VertexData.data();
		vertexDesc.DebugName = desc.DebugName + ".VB";
		Rhi::BufferDesc indexDesc;
		indexDesc.Size = desc.Indices.size() * sizeof(uint32_t);
		indexDesc.Usage = Rhi::BufferUsageIndex;
		indexDesc.InitialData = desc.Indices.data();
		indexDesc.DebugName = desc.DebugName + ".IB";

		MeshGpu gpu;
		gpu.VertexBuffer = Renderer::GetDevice()->CreateBuffer(vertexDesc);
		gpu.IndexBuffer = Renderer::GetDevice()->CreateBuffer(indexDesc);
		gpu.IndexCount = mesh->GetIndexCount();
		state.MeshCache.emplace(mesh.get(), gpu);
	}

	void Renderer3D::BeginScene(const glm::mat4&, const Rhi::Handle<Rhi::CommandBuffer>& commandBuffer)
	{
		// viewProjection 由全局相机 UBO 提供(SceneRenderer 已绑定 set 0),这里只需要命令缓冲与序号复位。
		State& state = GetState();
		// 上一帧挂起的材质描述符在这里补写:此时既不在渲染通道内,也没有在录制的命令缓冲,
		// 是唯一对驱动安全的写入时机(见 MaterialSetFor 的说明)。
		FlushMaterialUpdates(state);
		state.CommandBuffer = commandBuffer;
		state.ObjectIndex = 0;
	}

	void Renderer3D::BindPipelineForCurrentPass()
	{
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

	void Renderer3D::SetGlobalDescriptorSet(const Rhi::Handle<Rhi::DescriptorSet>& set)
	{
		// 只登记;真正的 vkCmdBindDescriptorSets 在管线绑定后进行(见 BindPipelineForCurrentPass)。
		GetState().GlobalSet = set;
	}

	uint32_t Renderer3D::Submit(const Ref<Mesh>& mesh, const glm::mat4& transform, const glm::vec4& baseColor,
		int32_t entityId)
	{
		State& state = GetState();
		if (!mesh || !state.CommandBuffer || !state.Pipeline)
			return UINT32_MAX;
		if (state.ObjectIndex >= kObjectsPerFrame)
			return UINT32_MAX;

		EnsureMeshBuffers(mesh);
		const auto cached = state.MeshCache.find(mesh.get());
		if (cached == state.MeshCache.end() || !cached->second.VertexBuffer)
			return UINT32_MAX;

		const uint32_t slot = Renderer::FrameSlot() % Renderer::FramesInFlight;
		const uint32_t index = state.ObjectIndex++;

		ObjectUniforms uniforms;
		uniforms.Model = transform;
		uniforms.BaseColor = baseColor;
		uniforms.MetallicRoughness = { 0.0f, 0.5f, 0.0f, 0.0f };
		uniforms.Emissive = { 0.0f, 0.0f, 0.0f, 0.0f };
		uniforms.Flags = { 0.0f, 0.0f, 0.0f, 0.0f };
		uniforms.EntityId = { entityId, 0, 0, 0 };
		WriteObjectUniforms(state, slot, index, uniforms);

		BindObject(state, slot, index, cached->second, state.Pipeline, state.ObjectSets[slot][index], nullptr);
		TraceSubmit(index, slot, cached->second.IndexCount, transform, baseColor);
		return index;
	}

	uint32_t Renderer3D::Submit(const Ref<Mesh>& mesh, const Ref<Material>& material, const glm::mat4& transform,
		int32_t entityId)
	{
		if (!material)
			return Submit(mesh, transform, glm::vec4(1.0f), entityId);
		State& state = GetState();
		if (!mesh || !state.CommandBuffer || !state.Pipeline)
			return UINT32_MAX;
		if (state.ObjectIndex >= kObjectsPerFrame)
			return UINT32_MAX;

		EnsureMeshBuffers(mesh);
		const auto cached = state.MeshCache.find(mesh.get());
		if (cached == state.MeshCache.end() || !cached->second.VertexBuffer)
			return UINT32_MAX;

		const MaterialDesc& desc = material->GetDesc();
		const uint32_t slot = Renderer::FrameSlot() % Renderer::FramesInFlight;
		const uint32_t index = state.ObjectIndex++;

		ObjectUniforms uniforms;
		uniforms.Model = transform;
		uniforms.BaseColor = desc.BaseColor;
		uniforms.MetallicRoughness = { desc.Metallic, desc.Roughness, 0.0f, 0.0f };
		uniforms.Emissive = { desc.Emissive.x, desc.Emissive.y, desc.Emissive.z, 0.0f };
		uniforms.Flags = {
			desc.AlbedoTexture.empty() ? 0.0f : 1.0f,
			desc.NormalTexture.empty() ? 0.0f : 1.0f,
			desc.DoubleSided ? 1.0f : 0.0f,
			0.0f };
		uniforms.EntityId = { entityId, 0, 0, 0 };
		WriteObjectUniforms(state, slot, index, uniforms);

		Rhi::Handle<Rhi::DescriptorSet> materialSet = MaterialSetFor(state, material, slot);
		const Rhi::Handle<Rhi::Pipeline>& pipeline = desc.BlendMode == MaterialBlendMode::Transparent
			? state.TransparentPipeline : state.Pipeline;
		BindObject(state, slot, index, cached->second, pipeline, state.ObjectSets[slot][index], materialSet);
		TraceSubmit(index, slot, cached->second.IndexCount, transform, desc.BaseColor);
		return index;
	}

	uint32_t Renderer3D::ReserveSlotBase(uint32_t identity, uint32_t span)
	{
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

	uint32_t Renderer3D::SubmitAtSlot(uint32_t slotBase, const Ref<Mesh>& mesh, const Ref<Material>& material,
		const glm::mat4& transform, int32_t entityId)
	{
		// 用固定序号提交:直接把 state.ObjectIndex 顶到 slotBase,让后续分配落在该槽位。
		State& state = GetState();
		if (!state.CommandBuffer || !state.Pipeline || slotBase >= kObjectsPerFrame)
			return UINT32_MAX;
		state.ObjectIndex = slotBase;
		const uint32_t result = Submit(mesh, material, transform, entityId);
		state.ObjectIndex = slotBase + 1;   // 同一调用方若还要再画一个,落在下一个槽位
		return result;
	}

	void Renderer3D::EndScene()
	{
		State& state = GetState();
		state.CommandBuffer = nullptr;
	}

	void Renderer3D::InvalidateMaterialCache()
	{
		GetState().MaterialCache.clear();
		MaterialTextureCache::Get().Clear();
	}

	Renderer3D::Statistics Renderer3D::GetStats()
	{
		return GetState().Stats;
	}

	void Renderer3D::ResetStats()
	{
		GetState().Stats = {};
	}

	uint32_t Renderer3D::GetObjectsPerFrameLimit()
	{
		return kObjectsPerFrame;
	}
}
