#include "wldpch.h"
#include "World/Renderer/Renderer3D.h"
#include "World/Renderer/RenderSettings.h"

#include "World/Renderer/Renderer.h"
#include "World/Renderer/MaterialTextureCache.h"
#include "World/RHI/Vulkan/VulkanResources.h"
#include "World/Renderer/ShaderUtils.h"

#include <cstring>
#include <filesystem>
#include <unordered_map>

#include <glm/gtc/matrix_access.hpp>

namespace World
{
	namespace
	{
		// 每帧对象上限:对象 UBO/描述符集按"帧槽位 × 序号"**按需**创建(首次用到才建),
		// 超出即拒绝并计入 Statistics.DroppedObjects(D8b 用实例化替换)。
		//
		// 注意 64 而不是 32:对象序号是**跨调用方共享**的(主场景渲染器 + 各材质预览面板
		// 依次调用 BeginScene 复位序号)。如果序号空间不够,后来者会拿不到槽位;
		// 更隐蔽的是"序号复用"会让不同调用方争用同一份 UBO/描述符集——当两者写入的内容
		// 不同(例如两个材质面板各自的贴图),画面就会逐帧来回闪(用户实测"预览一直闪烁")。
		//
		// D8a 起 64 → 1024:压力场景要求"≥1000 个网格实例"能全部提交(剔除前),
		// 64 会让第 65 个之后的物体静默消失。代价只是首帧按需创建的 UBO/描述符集
		// (144B × 3 帧槽位 × 1024 ≈ 440KB),没有预分配成本;真正的合并进 D8b 实例化。
		constexpr uint32_t kObjectsPerFrame = 1024;

		// D5c-4c:蒙皮调色板池分成互不重叠的两段(每份 8KB 的 BoneUniforms UBO)。
		//  - 顺序分配区 [0, MaxSkinnedDrawsPerFrame):SubmitSkinned / SubmitShadowSkinned 用
		//    PaletteCursor 自增取号,游标在 BeginScene / BeginShadowPass 复位(主场景路径);
		//  - 持久槽位保留区 [MaxSkinnedDrawsPerFrame, MaxSkinnedDrawsPerFrame + kObjectsPerFrame):
		//    SubmitSkinnedAtSlot 用**对象槽位**当键(paletteSlot = 保留区起点 + slotBase),
		//    跨帧稳定。顺序游标被拒绝时的上界恰是保留区起点(游标最大用到 255),
		//    所以"预览覆盖主场景调色板"在这两段下标上不可能发生。
		constexpr uint32_t kPaletteReservedBase = Renderer3D::MaxSkinnedDrawsPerFrame;
		constexpr uint32_t kPaletteSlotCount = kPaletteReservedBase + kObjectsPerFrame;

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

		// D5c-3b:骨骼调色板(set 1 binding 3,b0 = u_Model 之外的第二个 UBO)。
		// 与 Renderer3D_Solid.hlsl / Renderer3D_Shadow.hlsl 的 `cbuffer BoneUniforms : register(b3, space1)`
		// 逐字段对应:float4x4 u_Bones[128],行主序上传(mat4 的 16 个 float 按列主序存放,
		// 与 u_Model 同一条路径 —— 见 WriteObjectUniforms 的 SetData 与实例化 Row0..Row3 的对照)。
		// 尺寸 = 128 × 64B = 8KB < UBO 上限(16KB)。
		struct BoneUniforms
		{
			glm::mat4 Bones[Renderer3D::MaxBonePalette];
		};
		static_assert(sizeof(BoneUniforms) == Renderer3D::MaxBonePalette * 64,
			"BoneUniforms must be MaxBonePalette × mat4 (8KB)");
		static_assert(sizeof(BoneUniforms) == 8192, "u_Bones[128] must be 8192 bytes");

		// D8b-2:实例数据(96B;与 Renderer3D_Solid.hlsl 的 VS_INSTANCE_INPUT 逐字段对应)。
		// 模型矩阵按**行**上传(HLSL 的 float4x4(a,b,c,d) 按行构造),避免列/行主序歧义。
		// 实体 id 用 float 承载而不是整数属性:GL 后端建属性走 glVertexArrayAttribFormat
		// (不是 I 版),整数属性会被当浮点读,拾取 id 直接烂掉。
		struct InstanceData
		{
			glm::vec4 Row0 { 1.0f, 0.0f, 0.0f, 0.0f };
			glm::vec4 Row1 { 0.0f, 1.0f, 0.0f, 0.0f };
			glm::vec4 Row2 { 0.0f, 0.0f, 1.0f, 0.0f };
			glm::vec4 Row3 { 0.0f, 0.0f, 0.0f, 1.0f };
			glm::vec4 Color { 1.0f };
			glm::vec4 EntityId { -1.0f, 0.0f, 0.0f, 0.0f };
		};
		static_assert(sizeof(InstanceData) == 96, "InstanceData must match VS_INSTANCE_INPUT (96B)");

		// 每帧实例缓冲容量(4096 × 96B ≈ 384KB/帧槽位):超出即拒绝,由调用方回退逐物体路径。
		constexpr uint32_t kInstanceCapacity = 4096;

		struct MeshGpu
		{
			Rhi::Handle<Rhi::Buffer> VertexBuffer;
			Rhi::Handle<Rhi::Buffer> IndexBuffer;
			uint32_t IndexCount = 0;
			// D5:持有 Mesh 的强引用。MeshCache 按裸指针做键,而 .wmodel 的进程内缓存可以被
			// ClearWModelCache 清空(或资产热重载释放旧 Mesh)——若不做这个防重,新 Mesh 复用
			// 同一地址时会命中旧 GPU 缓冲(索引数/顶点数据全部串味)。持有 Owner 后地址唯一。
			Ref<Mesh> Owner;
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
			// D8b-2:实例化合批(管线 + 每帧槽位的实例缓冲/游标)。
			Rhi::Handle<Rhi::Pipeline> InstancedPipeline;
			Rhi::Handle<Rhi::Pipeline> InstancedShadowPipeline;
			Rhi::Handle<Rhi::Buffer> InstanceBuffers[Renderer::FramesInFlight];
			uint32_t InstanceCursor = 0;
			Rhi::Handle<Rhi::Buffer> ObjectUniformBuffers[Renderer::FramesInFlight][kObjectsPerFrame];
			Rhi::Handle<Rhi::DescriptorSet> ObjectSets[Renderer::FramesInFlight][kObjectsPerFrame];
			// ---- D4:方向光阴影 ----
			// 阴影通道:2048² 深度附件 + 一个"凑数"颜色附件。颜色附件不是画东西用的:
			// OpenGL 的 FBO 完整性要求每个 draw buffer 都有附件(默认 draw buffer 是
			// COLOR_ATTACHMENT0),纯深度通道在 GL 下会 INCOMPLETE_DRAW_BUFFER、所有绘制被丢弃。
			// 该附件 LoadOp/StoreOp 都是 DontCare,不产生内存流量,也从不被采样。
			Rhi::Handle<Rhi::RenderPass> ShadowPass;
			Rhi::Handle<Rhi::Framebuffer> ShadowFramebuffer;
			Rhi::Handle<Rhi::Texture> ShadowMapTexture;
			Rhi::Handle<Rhi::Texture> ShadowColorTexture;
			Rhi::Handle<Rhi::Sampler> ShadowSampler;
			Rhi::Handle<Rhi::Pipeline> ShadowPipeline;
			// D5c-3b:蒙皮变体(顶点入口 VSMainSkinned,顶点属性用布局 2)。现有管线/路径不动。
			Rhi::Handle<Rhi::Pipeline> SkinnedPipeline;
			Rhi::Handle<Rhi::Pipeline> SkinnedShadowPipeline;
			// 投影者用独立的对象槽位区:不消耗主通道的对象序号(否则阴影 + 主通道的
			// 提交数会让 64 个对象槽位提前耗尽)。
			Rhi::Handle<Rhi::Buffer> ShadowUniformBuffers[Renderer::FramesInFlight][kObjectsPerFrame];
			Rhi::Handle<Rhi::DescriptorSet> ShadowObjectSets[Renderer::FramesInFlight][kObjectsPerFrame];
			uint32_t ShadowObjectIndex = 0;
			// ---- D5c-3b:骨骼调色板(set 1 binding 3)----
			// 默认调色板缓冲(128 个单位阵)。Vulkan 下蒙皮管线静态使用 set 1
			// binding 3(着色器里声明了 u_Bones),所以每个 set 1 都必须更新 binding 3:
			// 非蒙皮绘制在 WriteObjectUniforms 里把 binding 3 指向这份默认缓冲,否则
			// vkCmdDrawIndexed 会被验证层判为 VUID-vkCmdDrawIndexed-None-08600。
			// 内容在 Init 写一次,之后不再变(按声明长度固定分配 8KB)。
			Rhi::Handle<Rhi::Buffer> DefaultPaletteBuffer;
			// 蒙皮绘制:每份 8KB 调色板 UBO 按需惰性创建
			// (帧栅栏保护:BeginFrame 等到本槽位上轮提交完成,重写同一份才安全)。
			// 主通道与阴影通道**共用**这一池;每帧槽位的游标在 BeginScene/BeginShadowPass 复位。
			// D5c-4c:池 = 顺序分配区 + 持久槽位保留区(互不重叠,见 kPaletteReservedBase);
			// 顺序区仍是最多 MaxSkinnedDrawsPerFrame 份,保留区按对象槽位惰性使用。
			// P3-1③:这里**不需要**单独的骨骼描述符集 —— binding 3 统一写进该次绘制的
			// set 1 对象集(WriteObjectUniforms;GL 的 Update 是整体替换语义,必须同一次写)。
			Rhi::Handle<Rhi::Buffer> PaletteBuffers[Renderer::FramesInFlight][kPaletteSlotCount];
			uint32_t PaletteCursor = 0;
			// 自建 set0 的调用方(材质预览)用的默认灯光 UBO:占位实现同款方向光 + 0.25 环境光。
			Rhi::Handle<Rhi::Buffer> DefaultLightBuffer;
			// [lighting] 日志去重(首帧或数量/阴影开关变化时才打)。
			uint32_t LastLoggedDirectional = UINT32_MAX;
			uint32_t LastLoggedPoint = UINT32_MAX;
			uint32_t LastLoggedDropped = UINT32_MAX;
			int32_t LastLoggedShadow = -1;
			// 无材质绘制(Color 路径)也要绑定 set 2:管线/着色器**静态**使用材质贴图,
			// 不绑就是 VUID-vkCmdDrawIndexed-None-08600(set 2 越界),GL 侧虽然宽容但同样是隐患。
			// 内容无所谓(着色器在 Flags=0 时不采样),用白色兜底贴图保证描述符合法。
			Rhi::Handle<Rhi::DescriptorSet> DefaultMaterialSets[Renderer::FramesInFlight];
			std::unordered_map<const Mesh*, MeshGpu> MeshCache;
			// 每材质 × 帧槽位的贴图描述符集;材质 Revision 变化时重建。
			struct MaterialGpu
			{
				Rhi::Handle<Rhi::DescriptorSet> Sets[Renderer::FramesInFlight];
				// Revision 必须**按槽位**记录:三个帧槽位各自持有一份描述符集,只更新当前
				// 槽位、共用一个 Revision 时,另外两个槽位会永远停留在编辑前的贴图绑定,
				// 材质按 3 帧周期在新/旧贴图之间来回(用户实测"换贴图后预览一直闪烁")。
				uint32_t Revision[Renderer::FramesInFlight] = {};
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
			// D8a:场景级统计由 SceneRenderer 每帧覆盖报告(不随预览的 BeginScene 复位)。
			Renderer3D::SceneStatistics SceneStats;
			// D8a2:阴影贴图边长(Init 时从项目清单取;清单校验保证是 2 的幂)。
			uint32_t ShadowMapSize = Renderer3D::DefaultShadowMapSize;
		};

		State& GetState()
		{
			static State state;
			return state;
		}

		Rhi::Handle<Rhi::Shader> CreateSolidShader(const char* path, const char* debugName,
			const char* vertexEntry = "VSMain")
		{
			Rhi::ShaderDesc desc;
			desc.DebugName = debugName;
			desc.Stages.push_back(ShaderCompiler::CompileStage(Rhi::ShaderStage::Vertex, path, vertexEntry, "vs_6_0"));
			desc.Stages.push_back(ShaderCompiler::CompileStage(Rhi::ShaderStage::Fragment, path, "PSMain", "ps_6_0"));
			return Renderer::GetDevice()->CreateShader(desc);
		}

		// D8b-2:在网格布局后追加实例绑定(binding 1,PerInstance;6 × float4,stride 96B)。
		MeshVertexLayout WithInstanceBinding(const MeshVertexLayout& layout)
		{
			MeshVertexLayout result = layout;
			result.Bindings.push_back({ 1, sizeof(InstanceData), true });
			for (uint32_t index = 0; index < 6; ++index)
				result.Attributes.push_back({ 3 + index, 1, Rhi::Format::R32G32B32A32_SFLOAT, index * 16u });
			return result;
		}

		// ---- D3 提交辅助(把三处重复代码收敛到一处) ----
		// D5c-3b:7 参数版本(带骨骼调色板)先声明 —— 6 参数的便捷重载在它之后定义,
		// 否则"后定义先调用"过不了编译(实测 C2660)。
		void WriteObjectUniforms(State& state, uint32_t slot, uint32_t index, const ObjectUniforms& uniforms,
			Rhi::Handle<Rhi::Buffer>& buffer, Rhi::Handle<Rhi::DescriptorSet>& set,
			Rhi::Handle<Rhi::Buffer>& boneBuffer);

		// D5c-3b:蒙皮提交会用到材质描述符集,而它的定义在下方(实测 C3861)→ 先声明。
		Rhi::Handle<Rhi::DescriptorSet> MaterialSetFor(State& state, const Ref<Material>& material,
			uint32_t slot);

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
		void WriteObjectUniforms(State& state, uint32_t slot, uint32_t index, const ObjectUniforms& uniforms,
			Rhi::Handle<Rhi::Buffer>& buffer, Rhi::Handle<Rhi::DescriptorSet>& set,
			Rhi::Handle<Rhi::Buffer>& boneBuffer)
		{
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
			set->Update({ write, bones });
			(void)slot; (void)index;
		}

		// 非蒙皮路径的便捷重载:binding 3 交给默认调色板(7 参数版里 boneBuffer == nullptr 的分支)。
		void WriteObjectUniforms(State& state, uint32_t slot, uint32_t index, const ObjectUniforms& uniforms,
			Rhi::Handle<Rhi::Buffer>& buffer, Rhi::Handle<Rhi::DescriptorSet>& set)
		{
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
		void WriteBoneUniforms(State& state, uint32_t slot, const glm::mat4* palette, uint32_t paletteCount,
			Rhi::Handle<Rhi::Buffer>& buffer)
		{
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

		void BindObject(State& state, uint32_t slot, uint32_t index, const MeshGpu& mesh,
			const Rhi::Handle<Rhi::Pipeline>& pipeline, const Rhi::Handle<Rhi::DescriptorSet>& objectSet,
			const Rhi::Handle<Rhi::DescriptorSet>& materialSet, uint32_t indexCount, uint32_t firstIndex)
		{
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

		// D5c-3b:蒙皮绘制核心。主通道与阴影通道只有"管线/对象槽位区/是否绑材质"三处差异,
		// 统一走这里,保证调色板绑定与统计口径一致。
		// 阴影管线的布局只有 set0/set1:多绑一个 set2 在 Vulkan 下是非法绑定(实测直接崩)。
		void DrawSkinnedObject(State& state, const MeshGpu& mesh, const glm::mat4& transform, int32_t entityId,
			const glm::vec4* baseColor, const glm::mat4* palette, uint32_t paletteCount, uint32_t objectIndex,
			uint32_t paletteSlot, uint32_t indexCount, uint32_t firstIndex,
			const Rhi::Handle<Rhi::Pipeline>& pipeline, const Ref<Material>& material, uint32_t slot,
			bool shadow)
		{
			if (!mesh.VertexBuffer || !pipeline)
				return;
			// 调色板占一份独立的 8KB UBO:槽位由调用方定 —— 顺序模式已取号游标,保留模式
			// 与对象槽位一一对应(见 SubmitSkinnedInternal 的两段划分)。
			Rhi::Handle<Rhi::Buffer>& boneBuffer = state.PaletteBuffers[slot][paletteSlot];
			WriteBoneUniforms(state, slot, palette, paletteCount, boneBuffer);

			Rhi::Handle<Rhi::Buffer>& objectBuffer = shadow
				? state.ShadowUniformBuffers[slot][objectIndex]
				: state.ObjectUniformBuffers[slot][objectIndex];
			Rhi::Handle<Rhi::DescriptorSet>& objectSet = shadow
				? state.ShadowObjectSets[slot][objectIndex]
				: state.ObjectSets[slot][objectIndex];
			ObjectUniforms uniforms;
			uniforms.Model = transform;
			uniforms.EntityId = { entityId, 0, 0, 0 };
			// 与 SubmitObject 同口径:有材质时标量/贴图标志从材质描述填,
			// 否则用常量色 + 中性标量(SubmitSkinned 的 vec4 重载)。
			const MaterialDesc* desc = material ? &material->GetDesc() : nullptr;
			if (desc)
			{
				uniforms.BaseColor = desc->BaseColor;
				uniforms.MetallicRoughness = { desc->Metallic, desc->Roughness, 0.0f, 0.0f };
				uniforms.Emissive = { desc->Emissive.x, desc->Emissive.y, desc->Emissive.z, 0.0f };
				uniforms.Flags = {
					desc->AlbedoTexture.empty() ? 0.0f : 1.0f,
					desc->NormalTexture.empty() ? 0.0f : 1.0f,
					desc->DoubleSided ? 1.0f : 0.0f,
					0.0f };
			}
			else
			{
				uniforms.BaseColor = baseColor ? *baseColor : glm::vec4(1.0f);
				uniforms.MetallicRoughness = { 0.0f, 0.5f, 0.0f, 0.0f };
				uniforms.Emissive = { 0.0f, 0.0f, 0.0f, 0.0f };
				uniforms.Flags = { 0.0f, 0.0f, 0.0f, 0.0f };
			}
			// 对象 UBO(binding 1)与调色板(binding 3)必须在同一次 Update 里写(见上面的说明)。
			WriteObjectUniforms(state, slot, objectIndex, uniforms, objectBuffer, objectSet, boneBuffer);

			Rhi::Handle<Rhi::DescriptorSet> materialSet;
			if (!shadow && material)
				materialSet = MaterialSetFor(state, material, slot);

			state.CommandBuffer->BindPipeline(pipeline);
			state.CommandBuffer->BindDescriptorSet(objectSet, 1);
			if (!shadow)
			{
				if (materialSet)
					state.CommandBuffer->BindDescriptorSet(materialSet, 2);
				else if (state.DefaultMaterialSets[slot])
					state.CommandBuffer->BindDescriptorSet(state.DefaultMaterialSets[slot], 2);
			}
			state.CommandBuffer->BindVertexBuffer(0, mesh.VertexBuffer);
			state.CommandBuffer->BindIndexBuffer(mesh.IndexBuffer);
			state.CommandBuffer->DrawIndexed(indexCount, 1, firstIndex);
			state.Stats.DrawCalls++;
			state.Stats.Triangles += indexCount / 3;
		}

		// 每材质 × 帧槽位的贴图描述符集。材质 Revision 变化(编辑参数/换贴图/热重载)
		// 或后端重建(缓存被清空)时重建,保证"改了立刻生效"。
		Rhi::Handle<Rhi::DescriptorSet> MaterialSetFor(State& state, const Ref<Material>& material, uint32_t slot)
		{
			const MaterialDesc& desc = material->GetDesc();
			State::MaterialGpu& gpu = state.MaterialCache[material.get()];
			if (gpu.Revision[slot] != material->GetRevision() || !gpu.Sets[slot])
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
				gpu.Revision[slot] = material->GetRevision();
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

		// D2b/D3/D5 的三条提交路径(整网格常量色 / 整网格材质 / 逐 submesh)共用同一实现,
		// 保证对象槽位分配、材质描述符、统计口径完全一致。
		// 网格 GPU 缓冲的惰性创建放在匿名命名空间里(成员 EnsureMeshBuffers 只是转发),
		// 这样本辅助函数与成员提交路径共用同一份实现。
		void EnsureMeshBuffersFor(State& state, const Ref<Mesh>& mesh)
		{
			if (!mesh || state.MeshCache.find(mesh.get()) != state.MeshCache.end())
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
			// D5:缓存按裸指针做键 —— 必须持有 Mesh 强引用,防止 Mesh 被释放后新对象复用同一地址。
			gpu.Owner = mesh;
			state.MeshCache.emplace(mesh.get(), gpu);
		}

		uint32_t SubmitObject(State& state, const Ref<Mesh>& mesh, const Ref<Material>& material,
			const glm::vec4& baseColor, const glm::mat4& transform, int32_t entityId,
			uint32_t indexCount, uint32_t firstIndex)
		{
			if (!mesh || !state.CommandBuffer || !state.Pipeline)
				return UINT32_MAX;
			if (state.ObjectIndex >= kObjectsPerFrame)
			{
				state.Stats.DroppedObjects++;
				return UINT32_MAX;
			}
			if (indexCount == 0)
				return UINT32_MAX;

			EnsureMeshBuffersFor(state, mesh);
			const auto cached = state.MeshCache.find(mesh.get());
			if (cached == state.MeshCache.end() || !cached->second.VertexBuffer)
				return UINT32_MAX;

			const MaterialDesc* desc = material ? &material->GetDesc() : nullptr;
			const uint32_t slot = Renderer::FrameSlot() % Renderer::FramesInFlight;
			const uint32_t index = state.ObjectIndex++;

			ObjectUniforms uniforms;
			uniforms.Model = transform;
			uniforms.EntityId = { entityId, 0, 0, 0 };
			if (desc)
			{
				uniforms.BaseColor = desc->BaseColor;
				uniforms.MetallicRoughness = { desc->Metallic, desc->Roughness, 0.0f, 0.0f };
				uniforms.Emissive = { desc->Emissive.x, desc->Emissive.y, desc->Emissive.z, 0.0f };
				uniforms.Flags = {
					desc->AlbedoTexture.empty() ? 0.0f : 1.0f,
					desc->NormalTexture.empty() ? 0.0f : 1.0f,
					desc->DoubleSided ? 1.0f : 0.0f,
					0.0f };
			}
			else
			{
				uniforms.BaseColor = baseColor;
				uniforms.MetallicRoughness = { 0.0f, 0.5f, 0.0f, 0.0f };
				uniforms.Emissive = { 0.0f, 0.0f, 0.0f, 0.0f };
				uniforms.Flags = { 0.0f, 0.0f, 0.0f, 0.0f };
			}
			WriteObjectUniforms(state, slot, index, uniforms,
				state.ObjectUniformBuffers[slot][index], state.ObjectSets[slot][index]);

			Rhi::Handle<Rhi::DescriptorSet> materialSet;
			const Rhi::Handle<Rhi::Pipeline>* pipeline = &state.Pipeline;
			if (desc)
			{
				materialSet = MaterialSetFor(state, material, slot);
				if (desc->BlendMode == MaterialBlendMode::Transparent)
					pipeline = &state.TransparentPipeline;
			}
			BindObject(state, slot, index, cached->second, *pipeline, state.ObjectSets[slot][index],
				materialSet, indexCount, firstIndex);
			TraceSubmit(index, slot, indexCount, transform, uniforms.BaseColor);
			return index;
		}
	}

	void Renderer3D::Init()
	{
		WLD_PROFILE_FUNCTION();
		State& state = GetState();
		// D8a2:阴影贴图边长来自项目清单(rendering.shadow_map_size);资源在这里按它创建,
		// 因此该字段**启动时生效**。清单校验已保证 2 的幂且 ∈[256,4096]。
		// 先按工作目录装载一次(宿主没提前 Apply 时也能拿到用户设置)。
		RenderSettings::LoadFromProject(std::filesystem::current_path());
		state.ShadowMapSize = RenderSettings::Get().ShadowMapSize;

		// set 1, binding 1:每对象 UBO(u_Model / u_BaseColor),顶点与像素阶段都要用。
		// binding 不能是 0:OpenGL 后端的 UBO 绑定单元 = binding(忽略 set 索引),
		// 用 0 会和 set0/binding0 的相机 UBO 抢同一个 unit,导致 GL 下 3D 全黑。
		Rhi::DescriptorSetLayoutDesc objectLayoutDesc;
		objectLayoutDesc.Bindings.push_back({ 1, Rhi::DescriptorType::UniformBuffer,
			Rhi::ShaderStageFlag(Rhi::ShaderStage::Vertex) | Rhi::ShaderStageFlag(Rhi::ShaderStage::Fragment), 1 });
		// D5c-3b:binding 3 = 骨骼调色板(u_Bones[128],8KB),**只有顶点阶段**用。
		// 为什么不是 binding 2(2026-09-19 回归修复):GL 后端的绑定单元 = binding(忽略 set),
		// set0/binding2 已是灯光 UBO(u_ShadowViewProjection/u_Ambient/u_Lights);同号时
		// 每个物体的 set1 绑定会把灯光 UBO 从 GL 单元 2 上挤掉(颜色附件与 Vulkan 不一致,
		// 实体 id 附件仍一致 —— 像素基线失败的正是这个签名)。3 在整条管线里空闲:
		// UBO 单元 0=相机、1=物体、2=灯光、3=骨骼;贴图用的是 GL 纹理单元命名空间。
		// 冻结决定(主 agent 2026-09-19):调色板走每绘制的 UBO 而不是 SSBO —— GL 后端的
		// SSBO 描述符绑定支持未经验证,而"每绘制写 UBO"是本项目对象 UBO 已有的路径,
		// 不引入新的后端能力要求;128 × 64B = 8KB 在 UBO 上限(16KB)内。
		objectLayoutDesc.Bindings.push_back({ 3, Rhi::DescriptorType::UniformBuffer,
			Rhi::ShaderStageFlag(Rhi::ShaderStage::Vertex), 1 });
		objectLayoutDesc.DebugName = "Renderer3D.Object";
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
		// P4-1:最大各向异性来自项目清单(rendering.anisotropy,1..16,启动时生效);
		// 设备不支持各向异性过滤时退化到 1 并 warn 一次,避免渲染器反复刷日志。
		// P4-2:超过设备上限(Rhi::Capabilities::MaxSamplerAnisotropy)时按上限 clamp 并 warn 一次。
		Rhi::SamplerDesc materialSamplerDesc;
		materialSamplerDesc.MinFilter = Rhi::Filter::Linear;
		materialSamplerDesc.MagFilter = Rhi::Filter::Linear;
		materialSamplerDesc.AddressU = Rhi::SamplerAddressMode::Repeat;
		materialSamplerDesc.AddressV = Rhi::SamplerAddressMode::Repeat;
		{
			const float requested = static_cast<float>(RenderSettings::Get().Anisotropy);
			float anisotropy = std::max(1.0f, std::min(16.0f, requested));
			const Rhi::Capabilities& capabilities = Renderer::GetDevice()->GetCapabilities();
			if (anisotropy > 1.0f && !capabilities.AnisotropicFiltering)
			{
				static bool s_AnisotropyWarned = false;
				if (!s_AnisotropyWarned)
				{
					s_AnisotropyWarned = true;
					WLD_CORE_WARN("Renderer3D: 设备不支持各向异性过滤,rendering.anisotropy={0} 退化为 1",
						requested);
				}
				anisotropy = 1.0f;
			}
			else if (anisotropy > capabilities.MaxSamplerAnisotropy)
			{
				// P4-2:设置值超过设备上限时按上限 clamp(否则 Vulkan 会撞
				// VUID-VkSamplerCreateInfo-anisotropyEnable-01071),warn 一次。
				static bool s_AnisotropyClampWarned = false;
				if (!s_AnisotropyClampWarned)
				{
					s_AnisotropyClampWarned = true;
					WLD_CORE_WARN("Renderer3D: rendering.anisotropy={0} 超过设备上限 {1},已限制到设备上限",
						requested, capabilities.MaxSamplerAnisotropy);
				}
				anisotropy = std::max(1.0f, capabilities.MaxSamplerAnisotropy);
			}
			materialSamplerDesc.MaxAnisotropy = anisotropy;
		}
		materialSamplerDesc.DebugName = "Renderer3D.MaterialSampler";
		state.MaterialSampler = Renderer::GetDevice()->CreateSampler(materialSamplerDesc);

		// 无材质绘制用的默认材质描述符集(白色 albedo + 白色 normal;着色器在 Flags=0 时不采样,
		// 这里只要保证 set 2 有合法绑定,避免"管线静态使用 set 2 而绘制只绑了 0..1"的 VUID)。
		for (uint32_t slot = 0; slot < Renderer::FramesInFlight; ++slot)
		{
			state.DefaultMaterialSets[slot] = Renderer::GetDevice()->CreateDescriptorSet(state.MaterialLayout);
			if (!state.DefaultMaterialSets[slot])
				continue;
			Rhi::DescriptorWrite albedo;
			albedo.Binding = 1;
			albedo.Type = Rhi::DescriptorType::CombinedImageSampler;
			albedo.Texture = MaterialTextureCache::Get().Get(std::string(), /*srgb*/ true);
			albedo.Sampler = state.MaterialSampler;
			Rhi::DescriptorWrite normal;
			normal.Binding = 2;
			normal.Type = Rhi::DescriptorType::CombinedImageSampler;
			normal.Texture = MaterialTextureCache::Get().Get(std::string(), /*srgb*/ false);
			normal.Sampler = state.MaterialSampler;
			state.DefaultMaterialSets[slot]->Update({ albedo, normal });
		}

		// D5c-3b:默认骨骼调色板(set 1 binding 3)。内容无所谓(非蒙皮顶点入口根本不读 u_Bones),
		// 但 Vulkan 的管线**静态**使用该 binding,不绑就是 VUID-vkCmdDrawIndexed-None-08600;
		// 这里用"128 个单位阵"的 8KB 缓冲:每个对象描述符集在 WriteObjectUniforms 里把它
		// 写进 binding 3(蒙皮绘制写自己的调色板),不需要每帧更新。
		{
			BoneUniforms identityPalette;
			for (uint32_t joint = 0; joint < Renderer3D::MaxBonePalette; ++joint)
				identityPalette.Bones[joint] = glm::mat4(1.0f);
			Rhi::BufferDesc paletteDesc;
			paletteDesc.Size = sizeof(BoneUniforms);
			paletteDesc.Usage = Rhi::BufferUsageUniform;
			paletteDesc.Memory = Rhi::MemoryHint::HostVisible;
			paletteDesc.DebugName = "Renderer3D.DefaultBoneUBO";
			paletteDesc.InitialData = &identityPalette;
			state.DefaultPaletteBuffer = Renderer::GetDevice()->CreateBuffer(paletteDesc);
		}

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

		// D8b-2:实例化合批管线(同一份 PSMain;VS 换成读 per-instance 模型矩阵的入口)。
		// 现有逐物体管线**不动**:预览/gizmo/透明物体继续走它。
		{
			Rhi::PipelineDesc instancedDesc = pipelineDesc;
			instancedDesc.Shader = CreateSolidShader("assets/shaders/Renderer3D_Solid.hlsl",
				"Renderer3D-Solid-Instanced", "VSMainInstanced");
			const MeshVertexLayout instancedLayout = WithInstanceBinding(meshLayout);
			instancedDesc.VertexBindings = instancedLayout.Bindings;
			instancedDesc.VertexAttributes = instancedLayout.Attributes;
			instancedDesc.DebugName = "Renderer3D.SolidPipeline.Instanced";
			state.InstancedPipeline = Renderer::GetDevice()->CreatePipeline(instancedDesc);
		}

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

		// ---- D4:方向光阴影(2048² 深度通道 + 全局 set0 的 binding 3 采样) ----
		Rhi::TextureDesc shadowColorDesc;
		shadowColorDesc.Type = Rhi::TextureType::Texture2D;
		shadowColorDesc.Format = Rhi::Format::R8G8B8A8_UNORM;
		shadowColorDesc.Extent = { state.ShadowMapSize, state.ShadowMapSize, 1 };
		shadowColorDesc.Usage = Rhi::TextureUsageColorAttachment;
		shadowColorDesc.DebugName = "Renderer3D.ShadowColor";
		state.ShadowColorTexture = Renderer::GetDevice()->CreateTexture(shadowColorDesc);

		Rhi::TextureDesc shadowDepthDesc;
		shadowDepthDesc.Type = Rhi::TextureType::Texture2D;
		// 格式必须用 **D32_SFLOAT** 而不是 D24_UNORM_S8_UINT:后者在 RHI 的
		// VulkanTexture 里创建成 DEPTH|STENCIL 双 aspect 的 image view,附件用途没问题,
		// 但作为采样描述符会被验证层判为非法(VUID-VkDescriptorImageInfo-imageView-01976:
		// 深度/模板视图的 aspectMask 必须二选一),驱动采样出来的深度因此是错的
		// (实测表现:Vulkan 下近处几何全部误判为阴影,GL 正常)。
		// D32_SFLOAT 的视图只含 DEPTH aspect,且是 Vulkan 强制支持采样的深度格式。
		shadowDepthDesc.Format = Rhi::Format::D32_SFLOAT;
		shadowDepthDesc.Extent = { state.ShadowMapSize, state.ShadowMapSize, 1 };
		// 既是深度附件(阴影通道写)又要被主通道采样:两个 usage 都必须声明,
		// 否则 Vulkan 创建镜像时缺 SAMPLED_BIT,描述符写入即非法。
		shadowDepthDesc.Usage = Rhi::TextureUsageDepthStencilAttachment | Rhi::TextureUsageSampled;
		shadowDepthDesc.DebugName = "Renderer3D.ShadowMap";
		state.ShadowMapTexture = Renderer::GetDevice()->CreateTexture(shadowDepthDesc);

		Rhi::SamplerDesc shadowSamplerDesc;
		shadowSamplerDesc.MinFilter = Rhi::Filter::Nearest;
		shadowSamplerDesc.MagFilter = Rhi::Filter::Nearest;
		shadowSamplerDesc.MipmapMode = Rhi::SamplerMipmapMode::Nearest;
		shadowSamplerDesc.AddressU = Rhi::SamplerAddressMode::ClampToEdge;
		shadowSamplerDesc.AddressV = Rhi::SamplerAddressMode::ClampToEdge;
		shadowSamplerDesc.AddressW = Rhi::SamplerAddressMode::ClampToEdge;
		shadowSamplerDesc.DebugName = "Renderer3D.ShadowSampler";
		state.ShadowSampler = Renderer::GetDevice()->CreateSampler(shadowSamplerDesc);

		Rhi::RenderPassDesc shadowPassDesc;
		Rhi::RenderPassAttachment shadowColor;
		shadowColor.Format = Rhi::Format::R8G8B8A8_UNORM;
		shadowColor.Samples = Rhi::SampleCount::Count1;
		// 凑数颜色附件:不画颜色也不清(README 见 State 里的说明 —— GL 的 FBO 完整性要求
		// 默认 draw buffer(GL_COLOR_ATTACHMENT0)有附件,否则整个深度通道在 GL 下被静默丢弃)。
		shadowColor.Load = Rhi::LoadOp::DontCare;
		shadowColor.Store = Rhi::StoreOp::DontCare;
		shadowColor.InitialLayout = Rhi::AttachmentLayout::Undefined;
		shadowColor.FinalLayout = Rhi::AttachmentLayout::ColorAttachment;
		Rhi::RenderPassAttachment shadowDepth;
		shadowDepth.Format = Rhi::Format::D32_SFLOAT;   // 与 ShadowMapTexture 同格式(见上)
		shadowDepth.Samples = Rhi::SampleCount::Count1;
		shadowDepth.Load = Rhi::LoadOp::Clear;
		shadowDepth.Store = Rhi::StoreOp::Store;                       // 主通道要采样
		shadowDepth.InitialLayout = Rhi::AttachmentLayout::Undefined;  // 每帧整体重写,内容不保留
		// 离场即转 SHADER_READ_ONLY:主通道的采样描述符按这个布局写入,渲染通道自己
		// 隐式转换并同步纹理跟踪。**不要**再补 PipelineBarrier:RHI 的屏障目前固定按
		// COLOR aspect 发(VulkanCommand.cpp),对深度图会直接触发验证层错误(越界文件,
		// 已在报告里记给主 agent)。
		shadowDepth.FinalLayout = Rhi::AttachmentLayout::ShaderReadOnly;
		shadowDepth.Clear.IsDepthStencil = true;
		shadowDepth.Clear.DepthStencil.Depth = 1.0f;
		shadowPassDesc.Attachments = { shadowColor, shadowDepth };
		Rhi::SubpassDesc shadowSubpass;
		shadowSubpass.ColorAttachments = { { 0, Rhi::AttachmentLayout::ColorAttachment } };
		shadowSubpass.DepthStencilAttachment = { 1, Rhi::AttachmentLayout::DepthStencilAttachment };
		shadowPassDesc.Subpasses = { shadowSubpass };
		shadowPassDesc.DebugName = "Renderer3D.ShadowPass";
		state.ShadowPass = Renderer::GetDevice()->CreateRenderPass(shadowPassDesc);

		Rhi::FramebufferDesc shadowFramebufferDesc;
		shadowFramebufferDesc.RenderPass = state.ShadowPass;
		shadowFramebufferDesc.Extent = { state.ShadowMapSize, state.ShadowMapSize };
		shadowFramebufferDesc.Attachments = { state.ShadowColorTexture, state.ShadowMapTexture };
		shadowFramebufferDesc.DebugName = "Renderer3D.ShadowFramebuffer";
		state.ShadowFramebuffer = Renderer::GetDevice()->CreateFramebuffer(shadowFramebufferDesc);

		Rhi::PipelineDesc shadowPipelineDesc = pipelineDesc;
		shadowPipelineDesc.Shader = CreateSolidShader("assets/shaders/Renderer3D_Shadow.hlsl", "Renderer3D-Shadow");
		shadowPipelineDesc.RenderPass = state.ShadowPass;
		// 阴影通道只绑 set0(灯光 UBO 提供 u_ShadowViewProjection)与 set1(对象 u_Model)。
		shadowPipelineDesc.DescriptorSetLayouts = { Renderer::GetGlobalDescriptorSetLayout(), state.ObjectLayout };
		shadowPipelineDesc.Blends = { Rhi::BlendAttachmentState {} };
		// 写**背面**(Cull=Front):闭合网格的自阴影 acne 天然消失,单面网格(地板/墙)
		// 不写深度、自然不投出自己的阴影。
		shadowPipelineDesc.Cull = Rhi::CullMode::Front;
		shadowPipelineDesc.DebugName = "Renderer3D.ShadowPipeline";
		state.ShadowPipeline = Renderer::GetDevice()->CreatePipeline(shadowPipelineDesc);

		// D8b-2:实例化阴影管线(投影者按 (mesh,submesh) 合批,一次画 N 个)。
		{
			Rhi::PipelineDesc instancedShadowDesc = shadowPipelineDesc;
			instancedShadowDesc.Shader = CreateSolidShader("assets/shaders/Renderer3D_Shadow.hlsl",
				"Renderer3D-Shadow-Instanced", "VSMainInstanced");
			const MeshVertexLayout instancedLayout = WithInstanceBinding(meshLayout);
			instancedShadowDesc.VertexBindings = instancedLayout.Bindings;
			instancedShadowDesc.VertexAttributes = instancedLayout.Attributes;
			instancedShadowDesc.DebugName = "Renderer3D.ShadowPipeline.Instanced";
			state.InstancedShadowPipeline = Renderer::GetDevice()->CreatePipeline(instancedShadowDesc);
		}

		// D5c-3b:蒙皮管线(主通道 + 阴影)。只有"顶点入口 = VSMainSkinned + 顶点属性 = 布局 2"
		// 与现有管线不同:剔除/深度/混合/描述符布局全部沿用,**现有管线与路径完全不动**。
		{
			const MeshVertexLayout skinnedLayout = Mesh::MakeSkinnedLayout();
			Rhi::PipelineDesc skinnedDesc = pipelineDesc;
			skinnedDesc.Shader = CreateSolidShader("assets/shaders/Renderer3D_Solid.hlsl",
				"Renderer3D-Solid-Skinned", "VSMainSkinned");
			skinnedDesc.VertexBindings = skinnedLayout.Bindings;
			skinnedDesc.VertexAttributes = skinnedLayout.Attributes;
			skinnedDesc.DebugName = "Renderer3D.SolidPipeline.Skinned";
			state.SkinnedPipeline = Renderer::GetDevice()->CreatePipeline(skinnedDesc);

			Rhi::PipelineDesc skinnedShadowDesc = shadowPipelineDesc;
			skinnedShadowDesc.Shader = CreateSolidShader("assets/shaders/Renderer3D_Shadow.hlsl",
				"Renderer3D-Shadow-Skinned", "VSMainSkinned");
			skinnedShadowDesc.VertexBindings = skinnedLayout.Bindings;
			skinnedShadowDesc.VertexAttributes = skinnedLayout.Attributes;
			skinnedShadowDesc.DebugName = "Renderer3D.ShadowPipeline.Skinned";
			state.SkinnedShadowPipeline = Renderer::GetDevice()->CreatePipeline(skinnedShadowDesc);
		}

		// 预览用默认灯光(材质预览自建 set0 时绑定):占位实现同款方向光 + 0.25 环境光,
		// 保证 D4 之后预览观感不变(预览不进场景灯光收集)。
		{
			const bool glDepthConvention = Renderer::GetBackendName() != "vulkan";
			LightRig previewRig = BuildLightRig({ DirectionalLightData {} }, {}, nullptr, glDepthConvention);
			Rhi::BufferDesc lightDesc;
			lightDesc.Size = sizeof(LightUniforms);
			lightDesc.Usage = Rhi::BufferUsageUniform;
			lightDesc.Memory = Rhi::MemoryHint::HostVisible;
			lightDesc.DebugName = "Renderer3D.DefaultLightUBO";
			state.DefaultLightBuffer = Renderer::GetDevice()->CreateBuffer(lightDesc);
			if (state.DefaultLightBuffer)
				state.DefaultLightBuffer->SetData(&previewRig.Uniforms, sizeof(previewRig.Uniforms));
		}
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
		MaterialTextureCache::Get().Clear();
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

	void Renderer3D::EnsureMeshBuffers(const Ref<Mesh>& mesh)
	{
		// 实现在匿名命名空间的 EnsureMeshBuffersFor(逐 submesh 的提交辅助函数也要用它)。
		EnsureMeshBuffersFor(GetState(), mesh);
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
		// D8b-2:每批场景从这里开始重新分配实例缓冲区(帧槽位由帧栅栏保护)。
		state.InstanceCursor = 0;
		// D5c-3b:蒙皮调色板游标(帧槽位由帧栅栏保护,见 State::PaletteBuffers 的说明)。
		state.PaletteCursor = 0;
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
		return SubmitObject(GetState(), mesh, nullptr, baseColor, transform, entityId,
			mesh ? mesh->GetIndexCount() : 0u, 0u);
	}

	uint32_t Renderer3D::Submit(const Ref<Mesh>& mesh, const Ref<Material>& material, const glm::mat4& transform,
		int32_t entityId)
	{
		if (!material)
			return Submit(mesh, transform, glm::vec4(1.0f), entityId);
		return SubmitObject(GetState(), mesh, material, glm::vec4(1.0f), transform, entityId,
			mesh ? mesh->GetIndexCount() : 0u, 0u);
	}

	uint32_t Renderer3D::SubmitSubmesh(const Ref<Mesh>& mesh, uint32_t submeshIndex, const Ref<Material>& material,
		const glm::mat4& transform, int32_t entityId)
	{
		if (!mesh || submeshIndex >= mesh->GetSubmeshes().size())
			return UINT32_MAX;
		const MeshSubmesh& submesh = mesh->GetSubmeshes()[submeshIndex];
		return SubmitObject(GetState(), mesh, material, glm::vec4(1.0f), transform, entityId,
			submesh.IndexCount, submesh.IndexOffset);
	}

	uint32_t Renderer3D::SubmitSubmesh(const Ref<Mesh>& mesh, uint32_t submeshIndex, const glm::vec4& baseColor,
		const glm::mat4& transform, int32_t entityId)
	{
		if (!mesh || submeshIndex >= mesh->GetSubmeshes().size())
			return UINT32_MAX;
		const MeshSubmesh& submesh = mesh->GetSubmeshes()[submeshIndex];
		return SubmitObject(GetState(), mesh, nullptr, baseColor, transform, entityId,
			submesh.IndexCount, submesh.IndexOffset);
	}

	// ---- P1b D5c-3b:GPU 蒙皮 ----
	uint32_t Renderer3D::SubmitSkinned(const Ref<Mesh>& mesh, uint32_t submeshIndex,
		const Ref<Material>& material, const glm::mat4& transform, const glm::mat4* palette,
		uint32_t paletteCount, int32_t entityId)
	{
		// 材质为空:退化成常量色(与 Submit(mesh, material, …) 同款约定)。
		return SubmitSkinnedInternal(mesh, submeshIndex, material, nullptr, transform, palette,
			paletteCount, entityId, /*shadow*/ false);
	}

	uint32_t Renderer3D::SubmitSkinned(const Ref<Mesh>& mesh, uint32_t submeshIndex,
		const glm::vec4& baseColor, const glm::mat4& transform, const glm::mat4* palette,
		uint32_t paletteCount, int32_t entityId)
	{
		return SubmitSkinnedInternal(mesh, submeshIndex, nullptr, &baseColor, transform, palette,
			paletteCount, entityId, /*shadow*/ false);
	}

	uint32_t Renderer3D::SubmitShadowSkinned(const Ref<Mesh>& mesh, uint32_t submeshIndex,
		const glm::mat4& transform, const glm::mat4* palette, uint32_t paletteCount)
	{
		return SubmitSkinnedInternal(mesh, submeshIndex, nullptr, nullptr, transform, palette,
			paletteCount, -1, /*shadow*/ true);
	}

	uint32_t Renderer3D::SubmitSkinnedInternal(const Ref<Mesh>& mesh, uint32_t submeshIndex,
		const Ref<Material>& material, const glm::vec4* baseColor, const glm::mat4& transform,
		const glm::mat4* palette, uint32_t paletteCount, int32_t entityId, bool shadow,
		bool reservedPalette)
	{
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

	// ---- P1b D8b-2:实例化合批 ----
	namespace
	{
		// 把一批实例打包写进当前帧槽位的实例缓冲;失败(容量不够/缓冲创建失败)返回 false。
		bool UploadInstances(State& state, uint32_t slot, const glm::mat4* transforms,
			const glm::vec4* colors, const int32_t* entityIds, uint32_t count, uint64_t* outOffset)
		{
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
		void DrawInstancedBatch(State& state, const MeshGpu& mesh, uint32_t indexCount, uint32_t firstIndex,
			const Rhi::Handle<Rhi::Buffer>& instanceBuffer, uint64_t instanceOffset, uint32_t count,
			const Rhi::Handle<Rhi::Pipeline>& pipeline, const Rhi::Handle<Rhi::DescriptorSet>& objectSet,
			const Rhi::Handle<Rhi::DescriptorSet>& materialSet, uint32_t slot, bool bindMaterialStates)
		{
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

	uint32_t Renderer3D::SubmitInstanced(const Ref<Mesh>& mesh, uint32_t submeshIndex,
		const Ref<Material>& material, const glm::mat4* transforms, const glm::vec4* colors,
		const int32_t* entityIds, uint32_t count)
	{
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
		ObjectUniforms uniforms;
		uniforms.Model = glm::mat4(1.0f);
		uniforms.EntityId = { -1, 0, 0, 0 };
		const MaterialDesc* desc = material ? &material->GetDesc() : nullptr;
		if (desc)
		{
			uniforms.BaseColor = desc->BaseColor;
			uniforms.MetallicRoughness = { desc->Metallic, desc->Roughness, 0.0f, 0.0f };
			uniforms.Emissive = { desc->Emissive.x, desc->Emissive.y, desc->Emissive.z, 0.0f };
			uniforms.Flags = {
				desc->AlbedoTexture.empty() ? 0.0f : 1.0f,
				desc->NormalTexture.empty() ? 0.0f : 1.0f,
				desc->DoubleSided ? 1.0f : 0.0f,
				0.0f };
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
			state.ObjectUniformBuffers[slot][index], state.ObjectSets[slot][index]);

		Rhi::Handle<Rhi::DescriptorSet> materialSet;
		if (desc)
			materialSet = MaterialSetFor(state, material, slot);
		DrawInstancedBatch(state, cached->second, indexCount, firstIndex,
			state.InstanceBuffers[slot], instanceOffset, count, state.InstancedPipeline,
			state.ObjectSets[slot][index], materialSet, slot, true);
		return count;
	}

	uint32_t Renderer3D::SubmitShadowInstanced(const Ref<Mesh>& mesh, uint32_t submeshIndex,
		const glm::mat4* transforms, uint32_t count)
	{
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

	uint32_t Renderer3D::SubmitSubmeshAtSlot(uint32_t slotBase, const Ref<Mesh>& mesh, uint32_t submeshIndex,
		const Ref<Material>& material, const glm::mat4& transform, int32_t entityId)
	{
		// 与 SubmitAtSlot 同款:固定序号提交,避免预览与主场景争用对象槽位。
		State& state = GetState();
		if (!state.CommandBuffer || !state.Pipeline || slotBase >= kObjectsPerFrame)
			return UINT32_MAX;
		state.ObjectIndex = slotBase;
		const uint32_t result = SubmitSubmesh(mesh, submeshIndex, material, transform, entityId);
		state.ObjectIndex = slotBase + 1;
		return result;
	}

	uint32_t Renderer3D::SubmitSkinnedAtSlot(uint32_t slotBase, const Ref<Mesh>& mesh, uint32_t submeshIndex,
		const Ref<Material>& material, const glm::mat4& transform, const glm::mat4* palette,
		uint32_t paletteCount, int32_t entityId)
	{
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

	// ---- D4:灯光收集/打包(纯函数部分) ----
	LightRig Renderer3D::BuildLightRig(const std::vector<DirectionalLightData>& directionalLights,
		const std::vector<PointLightData>& pointLights, const AmbientLightData* ambient,
		bool glDepthConvention)
	{
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

	void Renderer3D::ApplyShadowCaster(LightRig& rig, const glm::mat4& lightViewProjection)
	{
		rig.Uniforms.ShadowViewProjection = lightViewProjection;
		rig.Uniforms.ShadowParams.x = 1.0f;
	}

	void Renderer3D::ReportLighting(const LightRig& rig, double shadowPassMilliseconds)
	{
		State& state = GetState();
		state.Stats.Lights = rig.TotalLights;
		state.Stats.MaxLights = GetMaxDirectionalLights() + GetMaxPointLights();
		state.Stats.DroppedLights = rig.DroppedLights;
		state.Stats.ShadowPassMilliseconds = shadowPassMilliseconds;

		// 首帧或"数量/截断/阴影开关"变化时打一行(自动化断言用;逐帧刷屏没有信息量)。
		const uint32_t directional = rig.Uniforms.LightCounts.x;
		const uint32_t point = rig.Uniforms.LightCounts.y;
		const int32_t shadow = rig.Uniforms.ShadowParams.x > 0.5f ? 1 : 0;
		if (directional == state.LastLoggedDirectional && point == state.LastLoggedPoint &&
			rig.DroppedLights == state.LastLoggedDropped && shadow == state.LastLoggedShadow)
			return;
		state.LastLoggedDirectional = directional;
		state.LastLoggedPoint = point;
		state.LastLoggedDropped = rig.DroppedLights;
		state.LastLoggedShadow = shadow;
		WLD_CORE_INFO("[lighting] directional={0} point={1} dropped={2} shadowMs={3:.3f} shadow={4}",
			directional, point, rig.DroppedLights, shadowPassMilliseconds, shadow);
	}

	std::vector<Rhi::DescriptorWrite> Renderer3D::MakeGlobalLightingWrites(
		const Rhi::Handle<Rhi::Buffer>& lightUniformBuffer)
	{
		State& state = GetState();
		std::vector<Rhi::DescriptorWrite> writes;
		const Rhi::Handle<Rhi::Buffer> buffer = lightUniformBuffer ? lightUniformBuffer : state.DefaultLightBuffer;
		if (buffer)
		{
			Rhi::DescriptorWrite light;
			light.Binding = 2;
			light.Type = Rhi::DescriptorType::UniformBuffer;
			light.Buffer = buffer;
			writes.push_back(light);
		}
		if (state.ShadowMapTexture)
		{
			Rhi::DescriptorWrite shadow;
			shadow.Binding = 3;
			shadow.Type = Rhi::DescriptorType::CombinedImageSampler;
			shadow.Texture = state.ShadowMapTexture;
			shadow.Sampler = state.ShadowSampler;
			writes.push_back(shadow);
		}
		return writes;
	}

	Rhi::Handle<Rhi::RenderPass> Renderer3D::GetShadowRenderPass()
	{
		return GetState().ShadowPass;
	}

	Rhi::Handle<Rhi::Framebuffer> Renderer3D::GetShadowFramebuffer()
	{
		return GetState().ShadowFramebuffer;
	}

	Rhi::Handle<Rhi::Texture> Renderer3D::GetShadowMapTexture()
	{
		return GetState().ShadowMapTexture;
	}

	Rhi::Handle<Rhi::Sampler> Renderer3D::GetShadowMapSampler()
	{
		return GetState().ShadowSampler;
	}

	void Renderer3D::BeginShadowPass(const Rhi::Handle<Rhi::CommandBuffer>& commandBuffer)
	{
		State& state = GetState();
		state.CommandBuffer = commandBuffer;
		state.ShadowObjectIndex = 0;
		// D5c-3b:阴影通道与主通道共用蒙皮调色板池,游标同样从 0 起(两者同属一次提交)。
		state.PaletteCursor = 0;
		if (!commandBuffer)
			return;
		// 阴影贴图是固定尺寸的离屏目标,视口/裁剪按贴图边长设置(管线是动态视口状态)。
		commandBuffer->SetViewport({ 0.0f, 0.0f, static_cast<float>(state.ShadowMapSize),
			static_cast<float>(state.ShadowMapSize) });
		commandBuffer->SetScissor({ 0, 0, state.ShadowMapSize, state.ShadowMapSize });
	}

	uint32_t Renderer3D::SubmitShadow(const Ref<Mesh>& mesh, const glm::mat4& transform)
	{
		State& state = GetState();
		if (!mesh || !state.CommandBuffer || !state.ShadowPipeline)
			return UINT32_MAX;
		if (state.ShadowObjectIndex >= kObjectsPerFrame)
			return UINT32_MAX;

		EnsureMeshBuffers(mesh);
		const auto cached = state.MeshCache.find(mesh.get());
		if (cached == state.MeshCache.end() || !cached->second.VertexBuffer)
			return UINT32_MAX;

		const uint32_t slot = Renderer::FrameSlot() % Renderer::FramesInFlight;
		const uint32_t index = state.ShadowObjectIndex++;

		// 阴影只需要 u_Model(顶点按 u_ShadowViewProjection × u_Model 变换),其余字段留默认。
		ObjectUniforms uniforms;
		uniforms.Model = transform;
		WriteObjectUniforms(state, slot, index, uniforms,
			state.ShadowUniformBuffers[slot][index], state.ShadowObjectSets[slot][index]);

		state.CommandBuffer->BindPipeline(state.ShadowPipeline);
		state.CommandBuffer->BindDescriptorSet(state.ShadowObjectSets[slot][index], 1);
		state.CommandBuffer->BindVertexBuffer(0, cached->second.VertexBuffer);
		state.CommandBuffer->BindIndexBuffer(cached->second.IndexBuffer);
		state.CommandBuffer->DrawIndexed(cached->second.IndexCount);
		return index;
	}

	uint32_t Renderer3D::SubmitShadowSubmesh(const Ref<Mesh>& mesh, uint32_t submeshIndex,
		const glm::mat4& transform)
	{
		State& state = GetState();
		if (!mesh || !state.CommandBuffer || !state.ShadowPipeline)
			return UINT32_MAX;
		if (submeshIndex >= mesh->GetSubmeshes().size())
			return UINT32_MAX;
		if (state.ShadowObjectIndex >= kObjectsPerFrame)
			return UINT32_MAX;
		const MeshSubmesh& submesh = mesh->GetSubmeshes()[submeshIndex];
		if (submesh.IndexCount == 0)
			return UINT32_MAX;

		EnsureMeshBuffers(mesh);
		const auto cached = state.MeshCache.find(mesh.get());
		if (cached == state.MeshCache.end() || !cached->second.VertexBuffer)
			return UINT32_MAX;

		const uint32_t slot = Renderer::FrameSlot() % Renderer::FramesInFlight;
		const uint32_t index = state.ShadowObjectIndex++;

		// 阴影只需要 u_Model(顶点按 u_ShadowViewProjection × u_Model 变换),其余字段留默认。
		ObjectUniforms uniforms;
		uniforms.Model = transform;
		WriteObjectUniforms(state, slot, index, uniforms,
			state.ShadowUniformBuffers[slot][index], state.ShadowObjectSets[slot][index]);

		state.CommandBuffer->BindPipeline(state.ShadowPipeline);
		state.CommandBuffer->BindDescriptorSet(state.ShadowObjectSets[slot][index], 1);
		state.CommandBuffer->BindVertexBuffer(0, cached->second.VertexBuffer);
		state.CommandBuffer->BindIndexBuffer(cached->second.IndexBuffer);
		state.CommandBuffer->DrawIndexed(submesh.IndexCount, 1, submesh.IndexOffset);
		return index;
	}

	void Renderer3D::EndShadowPass()
	{
		State& state = GetState();
		state.CommandBuffer = nullptr;
		state.ShadowObjectIndex = 0;
	}

	Renderer3D::Statistics Renderer3D::GetStats()
	{
		return GetState().Stats;
	}

	void Renderer3D::ResetStats()
	{
		GetState().Stats = {};
	}

	void Renderer3D::ReportSceneStatistics(const SceneStatistics& statistics)
	{
		GetState().SceneStats = statistics;
	}

	Renderer3D::SceneStatistics Renderer3D::GetSceneStatistics()
	{
		return GetState().SceneStats;
	}

	uint32_t Renderer3D::GetObjectsPerFrameLimit()
	{
		return kObjectsPerFrame;
	}

	uint32_t Renderer3D::GetShadowMapSize()
	{
		// Init 之前/未初始化时退回默认值(测试直接调 BuildLightRig 也走这条)。
		return GetState().ShadowMapSize;
	}

	uint32_t Renderer3D::GetMaxDirectionalLights()
	{
		return std::min(RenderSettings::Get().MaxDirectionalLights, MaxDirectionalLightCapacity);
	}

	uint32_t Renderer3D::GetMaxPointLights()
	{
		return std::min(RenderSettings::Get().MaxPointLights, MaxPointLightCapacity);
	}
}
