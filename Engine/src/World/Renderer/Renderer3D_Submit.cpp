#include "Renderer3D_Internal.h"

namespace World
{

using namespace Renderer3DDetail;

namespace Renderer3DDetail
{

State& GetState(){
			static State state;
			return state;
		}


Rhi::Handle<Rhi::Shader> CreateSolidShader(const char* path, const char* debugName, const char* vertexEntry ){
			Rhi::ShaderDesc desc;
			desc.DebugName = debugName;
			desc.Stages.push_back(ShaderCompiler::CompileStage(Rhi::ShaderStage::Vertex, path, vertexEntry, "vs_6_0"));
			desc.Stages.push_back(ShaderCompiler::CompileStage(Rhi::ShaderStage::Fragment, path, "PSMain", "ps_6_0"));
			return Renderer::GetDevice()->CreateShader(desc);
		}


		// HOTR-P1-T3:热重载用的非断言版本(失败 = 空句柄 + 可读原因;空 stage 不进 CreateShader)。
Rhi::Handle<Rhi::Shader> TryCreateSolidShader(const char* path, const char* debugName, const char* vertexEntry, std::string* error){
			Rhi::ShaderDesc desc;
			desc.DebugName = debugName;
			Rhi::ShaderStageSource vertexStage;
			Rhi::ShaderStageSource pixelStage;
			if (!ShaderCompiler::TryCompileStage(Rhi::ShaderStage::Vertex, path, vertexEntry, "vs_6_0",
				vertexStage, error))
				return nullptr;
			if (!ShaderCompiler::TryCompileStage(Rhi::ShaderStage::Fragment, path, "PSMain", "ps_6_0",
				pixelStage, error))
				return nullptr;
			desc.Stages.push_back(std::move(vertexStage));
			desc.Stages.push_back(std::move(pixelStage));
			Rhi::Handle<Rhi::Shader> shader = Renderer::GetDevice()->CreateShader(desc);
			if (!shader && error)
				*error = std::string("CreateShader failed for ") + path;
			return shader;
		}


		// D8b-2:在网格布局后追加实例绑定(binding 1,PerInstance;6 × float4,stride 96B)。
MeshVertexLayout WithInstanceBinding(const MeshVertexLayout& layout){
			MeshVertexLayout result = layout;
			result.Bindings.push_back({ 1, sizeof(InstanceData), true });
			for (uint32_t index = 0; index < 6; ++index)
				result.Attributes.push_back({ 3 + index, 1, Rhi::Format::R32G32B32A32_SFLOAT, index * 16u });
			return result;
		}


		// HOTR-P1-T3:**唯一**的引擎管线创建实现(Init 与 Renderer3D::ReloadShaders 共用,
		// 保证热重载重建出的管线与启动期逐项一致)。只建 shader + pipeline:
		// render pass(state.RenderPass / state.ShadowPass)、描述符布局、阴影贴图/帧缓冲/采样器
		// 都由调用方保证已存在 —— 热重载**不**重建它们。
		//   - reload=false:走既有断言编译入口(启动期失败即断言;行为不变);
		//   - reload=true :走非断言入口;任一 stage / 管线失败 → 返回 false + error,
		//     且**不修改** state 里的既有句柄(调用方保留旧管线)。
bool CreateEnginePipelines(State& state, Rhi::FrontFace frontFace, Rhi::SampleCount sceneSamples, bool reload, EnginePipelineSet& out, std::string* error){
			const auto createShader = [reload, error](const char* path, const char* debugName,
				const char* vertexEntry) -> Rhi::Handle<Rhi::Shader>
			{
				if (!reload)
					return CreateSolidShader(path, debugName, vertexEntry);
				return TryCreateSolidShader(path, debugName, vertexEntry, error);
			};

			const MeshVertexLayout meshLayout = Mesh::MakeStandardLayout();
			Rhi::PipelineDesc pipelineDesc;
			pipelineDesc.Shader = createShader("assets/shaders/Renderer3D_Solid.slang", "Renderer3D-Solid", "VSMain");
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
			pipelineDesc.Front = frontFace;
			pipelineDesc.Cull = Rhi::CullMode::Back;
			pipelineDesc.DepthStencil.DepthTest = true;
			pipelineDesc.DepthStencil.DepthWrite = true;
			// 深度约定:清值 1.0 + LessOrEqual(近处深度小者胜)。Vulkan 的 NDC z∈[0,1] 由
			// `ProjectionConventions.h` 的深度重映射保证(近平面 → 0、远平面 → 1),
			// 两个后端共用同一约定,因此**不再**保留比较方向的 A/B 开关。
			pipelineDesc.DepthStencil.DepthCompare = Rhi::CompareOp::LessOrEqual;
			// P4-4b:主通道管线采样数跟随 rendering.msaa(与 state.RenderPass 的子通道一致)。
			pipelineDesc.Samples = sceneSamples;

			// D8b-2:实例化合批管线(同一份 PSMain;VS 换成读 per-instance 模型矩阵的入口)。
			// 现有逐物体管线**不动**:预览/gizmo/透明物体继续走它。
			Rhi::PipelineDesc instancedDesc = pipelineDesc;
			instancedDesc.Shader = createShader("assets/shaders/Renderer3D_Solid.slang",
				"Renderer3D-Solid-Instanced", "VSMainInstanced");
			const MeshVertexLayout instancedLayout = WithInstanceBinding(meshLayout);
			instancedDesc.VertexBindings = instancedLayout.Bindings;
			instancedDesc.VertexAttributes = instancedLayout.Attributes;
			instancedDesc.DebugName = "Renderer3D.SolidPipeline.Instanced";

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

			// ---- D4:方向光阴影管线(用调用方已建好的阴影通道 state.ShadowPass)----
			Rhi::PipelineDesc shadowPipelineDesc = pipelineDesc;
			shadowPipelineDesc.Shader = createShader("assets/shaders/Renderer3D_Shadow.slang",
				"Renderer3D-Shadow", "VSMain");
			shadowPipelineDesc.RenderPass = state.ShadowPass;
			// P4-4b:阴影通道是单采样(`ShadowPass` 的两个附件都是 Count1),必须显式覆盖
			// pipelineDesc.Samples(它现在跟随主通道的 MSAA);否则 Vulkan 的
			// VUID-VkGraphicsPipelineCreateInfo-subpass-00757 会拒绝这条管线。
			shadowPipelineDesc.Samples = Rhi::SampleCount::Count1;
			// 阴影通道只绑 set0(灯光 UBO 提供 u_ShadowViewProjection)与 set1(对象 u_Model)。
			shadowPipelineDesc.DescriptorSetLayouts = { Renderer::GetGlobalDescriptorSetLayout(), state.ObjectLayout };
			shadowPipelineDesc.Blends = { Rhi::BlendAttachmentState {} };
			// 写**背面**(Cull=Front):闭合网格的自阴影 acne 天然消失,单面网格(地板/墙)
			// 不写深度、自然不投出自己的阴影。
			shadowPipelineDesc.Cull = Rhi::CullMode::Front;
			shadowPipelineDesc.DebugName = "Renderer3D.ShadowPipeline";

			// D8b-2:实例化阴影管线(投影者按 (mesh,submesh) 合批,一次画 N 个)。
			Rhi::PipelineDesc instancedShadowDesc = shadowPipelineDesc;
			instancedShadowDesc.Shader = createShader("assets/shaders/Renderer3D_Shadow.slang",
				"Renderer3D-Shadow-Instanced", "VSMainInstanced");
			instancedShadowDesc.VertexBindings = instancedLayout.Bindings;
			instancedShadowDesc.VertexAttributes = instancedLayout.Attributes;
			instancedShadowDesc.DebugName = "Renderer3D.ShadowPipeline.Instanced";

			// D5c-3b:蒙皮管线(主通道 + 阴影)。只有"顶点入口 = VSMainSkinned + 顶点属性 = 布局 2"
			// 与现有管线不同:剔除/深度/混合/描述符布局全部沿用,**现有管线与路径完全不动**。
			const MeshVertexLayout skinnedLayout = Mesh::MakeSkinnedLayout();
			Rhi::PipelineDesc skinnedDesc = pipelineDesc;
			skinnedDesc.Shader = createShader("assets/shaders/Renderer3D_Solid.slang",
				"Renderer3D-Solid-Skinned", "VSMainSkinned");
			skinnedDesc.VertexBindings = skinnedLayout.Bindings;
			skinnedDesc.VertexAttributes = skinnedLayout.Attributes;
			skinnedDesc.DebugName = "Renderer3D.SolidPipeline.Skinned";

			Rhi::PipelineDesc skinnedShadowDesc = shadowPipelineDesc;
			skinnedShadowDesc.Shader = createShader("assets/shaders/Renderer3D_Shadow.slang",
				"Renderer3D-Shadow-Skinned", "VSMainSkinned");
			skinnedShadowDesc.VertexBindings = skinnedLayout.Bindings;
			skinnedShadowDesc.VertexAttributes = skinnedLayout.Attributes;
			skinnedShadowDesc.DebugName = "Renderer3D.ShadowPipeline.Skinned";

			// 全部 shader 齐了才建管线(任一为空 = 整体失败,既有句柄一个都不动)。
			if (!pipelineDesc.Shader || !instancedDesc.Shader || !shadowPipelineDesc.Shader
				|| !instancedShadowDesc.Shader || !skinnedDesc.Shader || !skinnedShadowDesc.Shader)
			{
				if (error && error->empty())
					*error = "engine shader unavailable (Renderer3D)";
				return false;
			}

			out.Solid = Renderer::GetDevice()->CreatePipeline(pipelineDesc);
			out.Instanced = Renderer::GetDevice()->CreatePipeline(instancedDesc);
			out.Transparent = Renderer::GetDevice()->CreatePipeline(transparentDesc);
			out.Shadow = Renderer::GetDevice()->CreatePipeline(shadowPipelineDesc);
			out.InstancedShadow = Renderer::GetDevice()->CreatePipeline(instancedShadowDesc);
			out.Skinned = Renderer::GetDevice()->CreatePipeline(skinnedDesc);
			out.SkinnedShadow = Renderer::GetDevice()->CreatePipeline(skinnedShadowDesc);
			if (!out.Solid || !out.Instanced || !out.Transparent || !out.Shadow
				|| !out.InstancedShadow || !out.Skinned || !out.SkinnedShadow)
			{
				if (error && error->empty())
					*error = "pipeline creation failed (Renderer3D)";
				return false;
			}
			return true;
		}

}
}
