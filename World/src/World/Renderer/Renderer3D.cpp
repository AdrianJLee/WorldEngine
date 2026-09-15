#include "wldpch.h"
#include "World/Renderer/Renderer3D.h"

#include "World/Renderer/Renderer.h"
#include "World/Renderer/ShaderUtils.h"

#include <cstring>
#include <unordered_map>

namespace World
{
	namespace
	{
		// 每帧对象上限:对象 UBO/描述符集按"帧槽位 × 序号"预建,超出即拒绝(D8 用实例化替换)。
		constexpr uint32_t kObjectsPerFrame = 32;

		struct ObjectUniforms
		{
			glm::mat4 Model { 1.0f };
			glm::vec4 BaseColor { 1.0f };
		};
		static_assert(sizeof(ObjectUniforms) == 80, "ObjectUniforms must match Renderer3D_Solid.hlsl");

		struct MeshGpu
		{
			Rhi::Handle<Rhi::Buffer> VertexBuffer;
			Rhi::Handle<Rhi::Buffer> IndexBuffer;
			uint32_t IndexCount = 0;
		};

		struct State
		{
			Rhi::Handle<Rhi::RenderPass> RenderPass;
			Rhi::Handle<Rhi::Pipeline> Pipeline;
			Rhi::Handle<Rhi::DescriptorSetLayout> ObjectLayout;
			Rhi::Handle<Rhi::CommandBuffer> CommandBuffer;
			Rhi::Handle<Rhi::Buffer> ObjectUniformBuffers[Renderer::FramesInFlight][kObjectsPerFrame];
			Rhi::Handle<Rhi::DescriptorSet> ObjectSets[Renderer::FramesInFlight][kObjectsPerFrame];
			std::unordered_map<const Mesh*, MeshGpu> MeshCache;
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
		// set 0 = 全局相机(SceneRenderer 绑定),set 1 = 每对象数据。
		pipelineDesc.DescriptorSetLayouts = { Renderer::GetGlobalDescriptorSetLayout(), state.ObjectLayout };
		pipelineDesc.VertexBindings = meshLayout.Bindings;
		pipelineDesc.VertexAttributes = meshLayout.Attributes;
		pipelineDesc.Topology = Rhi::PrimitiveTopology::TriangleList;
		// 剔除约定:所有网格按"外壁 = 从外侧看逆时针(CCW)"编写(Mesh::Create* 统一保证,
		// 回归见 World.Mesh),两个后端都用 CCW 判正面 —— 实测双后端一致,不需要按后端翻转。
		// 注意:这里曾按"Vulkan 的 Y 翻转会反转屏幕绕序"把 Vulkan 改成 CW,那是把 cube 自身的
		// 索引写反(与 plane 相反)误判成了后端差异,结果只修好了 cube、plane 反而被剔除。
		pipelineDesc.Cull = Rhi::CullMode::Back;
		pipelineDesc.Front = Rhi::FrontFace::CounterClockwise;
		pipelineDesc.DepthStencil.DepthTest = true;
		pipelineDesc.DepthStencil.DepthWrite = true;
		// 深度约定:清值 1.0 + LessOrEqual(近处深度小者胜)。Vulkan 的 NDC z∈[0,1] 由
		// `ProjectionConventions.h` 的深度重映射保证(近平面 → 0、远平面 → 1),
		// 两个后端共用同一约定,因此**不再**保留比较方向的 A/B 开关。
		pipelineDesc.DepthStencil.DepthCompare = Rhi::CompareOp::LessOrEqual;
		state.Pipeline = Renderer::GetDevice()->CreatePipeline(pipelineDesc);
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
		state.Pipeline = nullptr;
		state.RenderPass = nullptr;
		state.ObjectLayout = nullptr;
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
		state.CommandBuffer = commandBuffer;
		state.ObjectIndex = 0;
	}

	uint32_t Renderer3D::Submit(const Ref<Mesh>& mesh, const glm::mat4& transform, const glm::vec4& baseColor)
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

		const ObjectUniforms uniforms { transform, baseColor };
		state.ObjectUniformBuffers[slot][index]->SetData(&uniforms, sizeof(uniforms));
		Rhi::DescriptorWrite write;
		write.Binding = 1;
		write.Type = Rhi::DescriptorType::UniformBuffer;
		write.Buffer = state.ObjectUniformBuffers[slot][index];
		state.ObjectSets[slot][index]->Update({ write });

		state.CommandBuffer->BindPipeline(state.Pipeline);
		state.CommandBuffer->BindDescriptorSet(state.ObjectSets[slot][index], 1);
		state.CommandBuffer->BindVertexBuffer(0, cached->second.VertexBuffer);
		state.CommandBuffer->BindIndexBuffer(cached->second.IndexBuffer);
		state.CommandBuffer->DrawIndexed(cached->second.IndexCount);

		state.Stats.DrawCalls++;
		state.Stats.Triangles += cached->second.IndexCount / 3;
		// 诊断(WLD_TRACE_3D=1,只打印前 12 次提交):确认对象矩阵/颜色/缓冲是否真的送到 GPU。
		if (std::getenv("WLD_TRACE_3D"))
		{
			static int traced = 0;
			if (traced < 12)
			{
				traced++;
				WLD_CORE_INFO("[3d] submit#{0} slot={1} index={2} indices={3} model=[{4} {5} {6}] color=({7},{8},{9},{10})",
					traced, slot, index, cached->second.IndexCount,
					transform[3][0], transform[3][1], transform[3][2],
					baseColor.r, baseColor.g, baseColor.b, baseColor.a);
			}
		}
		return index;
	}

	void Renderer3D::EndScene()
	{
		GetState().CommandBuffer = nullptr;
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
