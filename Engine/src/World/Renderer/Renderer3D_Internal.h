#include "wldpch.h"
#include "World/Renderer/Renderer3D.h"
#include "World/Renderer/RenderSettings.h"

#include "World/Renderer/Renderer.h"
#include "World/Renderer/MaterialLibrary.h"
#include "World/Renderer/Texture/TextureLibrary.h"
#include "World/Renderer/MaterialSurfaceRuntime.h"
#include "World/Renderer/MaterialSurfaceRuntimeInternal.h"
#include "World/RHI/Vulkan/VulkanResources.h"
#include "World/Renderer/ShaderUtils.h"

#include <cstring>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <glm/gtc/matrix_access.hpp>

namespace World
{
namespace Renderer3DDetail {}   // 前置声明:下面的 using 必须先见到这个名字
using namespace Renderer3DDetail;   // 等价于拆分前的文件内匿名命名空间可见性
	namespace Renderer3DDetail
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

		// M4-S3:共享零值参数缓冲的字节数。非表面绘制也要把 set 1 binding 4 写掉
		// (描述符集跨帧复用,不能留上一批已释放的参数缓冲),引擎着色器不读它,
		// 所以只需要"够大、永远合法"。表面材质自己用反射出的真实大小创建缓冲。
		constexpr uint32_t kDefaultParamBlockBytes = 1024;

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
			glm::vec4 Flags { 0.0f };   // x = 有 albedo, y = 有法线, z = 双面, w = 法线是 BC5 产物
			// D7-1c:视口点选用的实体 id(SV_Target1),用 int4 承载(见 hlsl 里的说明:
			// 标量+短向量在 HLSL 与 std140 下偏移不一致)。
			glm::ivec4 EntityId { -1, 0, 0, 0 };
		};
		static_assert(sizeof(ObjectUniforms) == 144, "ObjectUniforms must match Renderer3D_Solid.slang");

		// D5c-3b:骨骼调色板(set 1 binding 3,b0 = u_Model 之外的第二个 UBO)。
		// 与 Renderer3D_Solid.slang / Renderer3D_Shadow.slang 的 `cbuffer BoneUniforms : register(b3, space1)`
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

		// D8b-2:实例数据(96B;与 Renderer3D_Solid.slang 的 VS_INSTANCE_INPUT 逐字段对应)。
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
			// ---- M4-S3:表面函数材质(HLSL 代码态)----
			// set 2 的布局与引擎材质不同(1/2 + 4..11 贴图参数),所以单独一份缓存。
			Rhi::Handle<Rhi::DescriptorSetLayout> SurfaceMaterialLayout;
			// 每个帧槽位的"默认表面材质集":绑定时永远合法(白色 albedo/normal + 全部贴图槽
			// 白色),用于"表面材质描述符集刚建好、还没轮到通道外补写"的那一帧 ——
			// 绝不把没写过的描述符集绑给管线(裸描述符 = 验证层报错 + 驱动未定义行为)。
			Rhi::Handle<Rhi::DescriptorSet> SurfaceDefaultSets[Renderer::FramesInFlight];
			// set 1 binding 4(参数块)的共享兜底缓冲:描述符集跨帧复用,非表面绘制也要写掉
			// 这个 binding,否则会留着上一批已释放的参数缓冲(野描述符)。
			Rhi::Handle<Rhi::Buffer> SurfaceDefaultParamBuffer;
			struct SurfaceMaterialGpu
			{
				// 缓存键是裸指针(与 MaterialGpu 同款);这里持有强引用防止 Mesh/Material 被
				// 释放后新对象复用同一地址 → 命中别人的表面状态。
				Ref<Material> Owner;
				std::string Key;                 // 表面键(材质 Revision 变化时重算)
				uint32_t KeyRevision = 0;
				size_t Version = 0;              // 已发布版本快照(0 = 从未安装)
				MaterialParamLayout Layout;      // 与该版本对应(反射自编译产物)
				bool HasLayout = false;
				uint32_t ParamSize = 0;          // 参数块字节数(0 = 没有参数)
				// 逐槽位状态:参数 UBO / 描述符集 / 已写入的 binding 掩码 / 打包与写入时的
				// 材质 Revision(见 MaterialGpu.Revision 的说明:必须按槽位记)。
				Rhi::Handle<Rhi::Buffer> ParamBuffers[Renderer::FramesInFlight];
				Rhi::Handle<Rhi::DescriptorSet> Sets[Renderer::FramesInFlight];
				uint32_t WrittenMask[Renderer::FramesInFlight] = {};
				uint32_t ParamRevision[Renderer::FramesInFlight] = {};
				uint32_t DescRevision[Renderer::FramesInFlight] = {};
			};
			std::unordered_map<const Material*, SurfaceMaterialGpu> SurfaceCache;
			// 表面材质描述符的挂起写入(与 PendingMaterialUpdates 同款:通道录制期间不写描述符)。
			std::vector<std::pair<const Material*, Ref<Material>>> PendingSurfaceUpdates;
			std::vector<uint32_t> PendingSurfaceSlots;
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
State& GetState();

Rhi::Handle<Rhi::Shader> CreateSolidShader(const char* path, const char* debugName, const char* vertexEntry = "VSMain");

Rhi::Handle<Rhi::Shader> TryCreateSolidShader(const char* path, const char* debugName, const char* vertexEntry, std::string* error);

MeshVertexLayout WithInstanceBinding(const MeshVertexLayout& layout);


		// HOTR-P1-T3:7 条引擎管线的集合(Init 与 ReloadShaders 共用;只在全部建成后才提交)。
		struct EnginePipelineSet
		{
			Rhi::Handle<Rhi::Pipeline> Solid;
			Rhi::Handle<Rhi::Pipeline> Instanced;
			Rhi::Handle<Rhi::Pipeline> Transparent;
			Rhi::Handle<Rhi::Pipeline> Shadow;
			Rhi::Handle<Rhi::Pipeline> InstancedShadow;
			Rhi::Handle<Rhi::Pipeline> Skinned;
			Rhi::Handle<Rhi::Pipeline> SkinnedShadow;
		};
bool CreateEnginePipelines(State& state, Rhi::FrontFace frontFace, Rhi::SampleCount sceneSamples, bool reload, EnginePipelineSet& out, std::string* error);


		// ---- D3 提交辅助(把三处重复代码收敛到一处) ----
		// D5c-3b:7 参数版本(带骨骼调色板)先声明 —— 6 参数的便捷重载在它之后定义,
		// 否则"后定义先调用"过不了编译(实测 C2660)。
		void WriteObjectUniforms(State& state, uint32_t slot, uint32_t index, const ObjectUniforms& uniforms,
			Rhi::Handle<Rhi::Buffer>& buffer, Rhi::Handle<Rhi::DescriptorSet>& set,
			Rhi::Handle<Rhi::Buffer>& boneBuffer, const Rhi::Handle<Rhi::Buffer>& paramBuffer);
		void WriteObjectUniforms(State& state, uint32_t slot, uint32_t index, const ObjectUniforms& uniforms,
			Rhi::Handle<Rhi::Buffer>& buffer, Rhi::Handle<Rhi::DescriptorSet>& set,
			Rhi::Handle<Rhi::Buffer>& boneBuffer);

		// D5c-3b:蒙皮提交会用到材质描述符集,而它的定义在下方(实测 C3861)→ 先声明。
		Rhi::Handle<Rhi::DescriptorSet> MaterialSetFor(State& state, const Ref<Material>& material,
			uint32_t slot);
void WriteObjectUniforms(State& state, uint32_t slot, uint32_t index, const ObjectUniforms& uniforms, Rhi::Handle<Rhi::Buffer>& buffer, Rhi::Handle<Rhi::DescriptorSet>& set, Rhi::Handle<Rhi::Buffer>& boneBuffer, const Rhi::Handle<Rhi::Buffer>& paramBuffer);

void WriteObjectUniforms(State& state, uint32_t slot, uint32_t index, const ObjectUniforms& uniforms, Rhi::Handle<Rhi::Buffer>& buffer, Rhi::Handle<Rhi::DescriptorSet>& set, Rhi::Handle<Rhi::Buffer>& boneBuffer);

void WriteObjectUniforms(State& state, uint32_t slot, uint32_t index, const ObjectUniforms& uniforms, Rhi::Handle<Rhi::Buffer>& buffer, Rhi::Handle<Rhi::DescriptorSet>& set);

void WriteBoneUniforms(State& state, uint32_t slot, const glm::mat4* palette, uint32_t paletteCount, Rhi::Handle<Rhi::Buffer>& buffer);

void BindObject(State& state, uint32_t slot, uint32_t index, const MeshGpu& mesh, const Rhi::Handle<Rhi::Pipeline>& pipeline, const Rhi::Handle<Rhi::DescriptorSet>& objectSet, const Rhi::Handle<Rhi::DescriptorSet>& materialSet, uint32_t indexCount, uint32_t firstIndex);

uint32_t SurfaceNeededBindings(const MaterialParamLayout& layout, bool hasLayout);

const char* SurfaceVariantLabel(SurfacePipelineVariant variant);

void WarnSurfaceFallbackOnce(const std::string& key, SurfacePipelineVariant variant, const std::string& reason);

uint32_t ParamTypeByteSize(ParamType type);

void PackSurfaceParamBytes(const Ref<Material>& material, const MaterialParamLayout& layout, std::vector<uint8_t>* out);


		struct SurfaceDrawState
		{
			bool Active = false;                             // true = 这一次绘制走表面管线
			Rhi::Handle<Rhi::Pipeline> Pipeline;
			Rhi::Handle<Rhi::DescriptorSet> SurfaceSet;      // set 2
			Rhi::Handle<Rhi::Buffer> ParamBuffer;            // set 1 binding 4(register b4, space1)
		};
void QueueSurfaceUpdate(State& state, const Ref<Material>& material, uint32_t slot);

SurfaceDrawState PrepareSurfaceDraw(State& state, const Ref<Material>& material, SurfacePipelineVariant variant, uint32_t slot);

float NormalBc5Flag(const MaterialDesc* desc);

Rhi::Handle<Rhi::Sampler> TextureSamplerFor(const std::string& path, bool srgb, const Rhi::Handle<Rhi::Sampler>& shared);

void FlushSurfaceUpdates(State& state);

void DrawSkinnedObject(State& state, const MeshGpu& mesh, const glm::mat4& transform, int32_t entityId, const glm::vec4* baseColor, const glm::mat4* palette, uint32_t paletteCount, uint32_t objectIndex, uint32_t paletteSlot, uint32_t indexCount, uint32_t firstIndex, const Rhi::Handle<Rhi::Pipeline>& pipeline, const Ref<Material>& material, uint32_t slot, bool shadow);

Rhi::Handle<Rhi::DescriptorSet> MaterialSetFor(State& state, const Ref<Material>& material, uint32_t slot);

void FlushMaterialUpdates(State& state);

void TraceSubmit(uint32_t index, uint32_t slot, uint32_t indexCount, const glm::mat4& transform, const glm::vec4& color);

void EnsureMeshBuffersFor(State& state, const Ref<Mesh>& mesh);

uint32_t SubmitObject(State& state, const Ref<Mesh>& mesh, const Ref<Material>& material, const glm::vec4& baseColor, const glm::mat4& transform, int32_t entityId, uint32_t indexCount, uint32_t firstIndex);

	}

	// ---- P1b D8b-2:实例化合批 ----
	namespace Renderer3DDetail
	{
bool UploadInstances(State& state, uint32_t slot, const glm::mat4* transforms, const glm::vec4* colors, const int32_t* entityIds, uint32_t count, uint64_t* outOffset);

void DrawInstancedBatch(State& state, const MeshGpu& mesh, uint32_t indexCount, uint32_t firstIndex, const Rhi::Handle<Rhi::Buffer>& instanceBuffer, uint64_t instanceOffset, uint32_t count, const Rhi::Handle<Rhi::Pipeline>& pipeline, const Rhi::Handle<Rhi::DescriptorSet>& objectSet, const Rhi::Handle<Rhi::DescriptorSet>& materialSet, uint32_t slot, bool bindMaterialStates);

	}
}
