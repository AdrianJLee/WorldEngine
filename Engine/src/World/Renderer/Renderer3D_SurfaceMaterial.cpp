#include "Renderer3D_Internal.h"

namespace World
{

using namespace Renderer3DDetail;

namespace Renderer3DDetail
{

const char* SurfaceVariantLabel(SurfacePipelineVariant variant){
			switch (variant)
			{
				case SurfacePipelineVariant::Solid: return "solid";
				case SurfacePipelineVariant::Transparent: return "transparent";
				case SurfacePipelineVariant::Instanced: return "instanced";
				case SurfacePipelineVariant::Skinned: return "skinned";
				default: return "unknown";
			}
		}


		// 变体回退的警告只打一次(键 × 变体),避免逐帧刷屏;上限 16 条防止无界增长。
void WarnSurfaceFallbackOnce(const std::string& key, SurfacePipelineVariant variant, const std::string& reason){
			static std::vector<std::string> warned;
			const std::string tag = key + "|" + SurfaceVariantLabel(variant);
			for (const std::string& existing : warned)
				if (existing == tag)
					return;
			if (warned.size() >= 16)
				return;
			warned.push_back(tag);
			WLD_CORE_WARN("Renderer3D: 表面管线变体 '{0}' 不可用(key '{1}'),本次绘制回退引擎管线:{2}",
				SurfaceVariantLabel(variant), key, reason);
		}


		// 注解类型在参数块里占的字节数(与 MaterialParams.cpp 的 ComponentCount * 4 同一口径;
		// bool 在 SPIR-V 侧是 uint32)。
uint32_t ParamTypeByteSize(ParamType type){
			switch (type)
			{
				case ParamType::Float:
				case ParamType::Int:
				case ParamType::Bool:  return 4;
				case ParamType::Vec2:  return 8;
				case ParamType::Vec3:  return 12;
				case ParamType::Vec4:
				case ParamType::Color: return 16;
				default:               return 0;   // Texture2D 是绑定,不进参数块
			}
		}


		// 参数块的字节:生效值 = 本文件覆盖 > 父级覆盖 > 注解默认。
		// MAT-UI4a:注解表(材质,来自磁盘源码)与参数块布局(编译产物反射)可能不同步 ——
		// 典型场景是编辑器里复制/改名了一条 `//! param` 但还没保存:布局里多一个成员,
		// 材质注解表里没有它。旧实现遇到这种字段就整块写零(PackParamValues 直接失败),
		// 表现为预览全黑。这里先把对不上的字段从布局里剔掉(它们保持 0 = 缺省),其余字段
		// 照常打包;只有"注解里声明了、但值文本坏掉"才退到注解默认值。单个字段的问题
		// 不再能把整块参数清零,并且每次都留下可读警告。
void PackSurfaceParamBytes(const Ref<Material>& material, const MaterialParamLayout& layout, std::vector<uint8_t>* out){
			const std::vector<MaterialParamDecl>& table = material->Params();
			MaterialParamLayout packable = layout;
			packable.Fields.clear();
			std::vector<std::string> skipped;
			for (const MaterialParamLayoutField& field : layout.Fields)
			{
				const MaterialParamDecl* decl = FindParamDecl(table, field.Name);
				if (!decl)
				{
					skipped.push_back(field.Name + "(缺注解声明)");
					continue;
				}
				if (!IsReflectedTypeCompatible(decl->Type, field.ReflectedType)
					|| ParamTypeByteSize(decl->Type) != field.Size)
				{
					skipped.push_back(field.Name + "(注解类型与布局不符)");
					continue;
				}
				if (field.Offset + field.Size > layout.CbufferSize)
				{
					skipped.push_back(field.Name + "(字段越界)");
					continue;
				}
				packable.Fields.push_back(field);
			}
			if (!skipped.empty())
			{
				std::string names;
				for (const std::string& name : skipped)
					names += (names.empty() ? "" : ", ") + name;
				WLD_CORE_WARN("Renderer3D: 材质 '{0}' 的参数块与注解表不一致,跳过 {1} 个字段(按 0 写入,其余参数照常):{2}",
					material->GetPath(), skipped.size(), names);
			}
			std::vector<MaterialParamOverride> resolved;
			resolved.reserve(table.size());
			for (const MaterialParamDecl& decl : table)
			{
				if (IsTextureParamType(decl.Type))
					continue;
				resolved.push_back(MaterialParamOverride { decl.Name, material->ResolvedParamValue(decl.Name) });
			}
			std::string error;
			if (PackParamValues(packable, table, resolved, out, &error))
				return;
			WLD_CORE_WARN("Renderer3D: 材质参数打包失败,退到注解默认值:{0}", error);
			resolved.clear();
			if (PackParamValues(packable, table, resolved, out, &error))
				return;
			WLD_CORE_WARN("Renderer3D: 材质参数默认值也不能打包,本次写零值:{0}", error);
			out->assign(layout.CbufferSize, 0);
		}


void QueueSurfaceUpdate(State& state, const Ref<Material>& material, uint32_t slot){
			state.PendingSurfaceUpdates.emplace_back(material.get(), material);
			state.PendingSurfaceSlots.push_back(slot);
		}


		// 材质 → 表面绘制状态。Active == false 时调用方**完全**走引擎管线(既有行为不变)。
SurfaceDrawState PrepareSurfaceDraw(State& state, const Ref<Material>& material, SurfacePipelineVariant variant, uint32_t slot){
			SurfaceDrawState draw;
			if (!material)
				return draw;
			const std::string key = material->SurfaceKey();
			if (key.empty())
				return draw;
			size_t version = SurfacePublishedVersion(key);
			if (version == 0)
			{
				// Slang-T6a:打包形态没有编辑器/编译器 —— 第一次真的要画这个键时,
				// 用包内烘好的成对产物装配一次(库侧幂等 + 负缓存,不会逐帧读盘)。
				// 开发形态包内没有这些产物 → 空操作,编辑器照旧现场编译 + Install。
				MaterialLibrary::Get().EnsureCookedSurfacePipeline(*material);
				version = SurfacePublishedVersion(key);
			}
			if (version == 0)
				return draw;
			Rhi::Handle<Rhi::Pipeline> pipeline;
			if (!FetchSurfacePipeline(key, variant, &pipeline))
			{
				WarnSurfaceFallbackOnce(key, variant,
					"该变体的顶点阶段没有建出管线(改材质着色器源时请保留 VSMain/VSMainInstanced/VSMainSkinned)");
				return draw;
			}

			State::SurfaceMaterialGpu& gpu = state.SurfaceCache[material.get()];
			const uint32_t revision = material->GetRevision();
			if (gpu.Owner != material)
			{
				// 地址复用保护:旧的 Material 已经释放、新对象拿到同一地址 → 整份复位。
				gpu = State::SurfaceMaterialGpu {};
				gpu.Owner = material;
			}
			if (gpu.Key != key || gpu.Version != version)
			{
				MaterialParamLayout layout;
				const bool hasLayout = FetchSurfaceParamLayout(key, &layout);
				const uint32_t previousMask = SurfaceNeededBindings(gpu.Layout, gpu.HasLayout);
				const uint32_t nextMask = SurfaceNeededBindings(layout, hasLayout);
				const uint32_t nextSize = hasLayout ? layout.CbufferSize : 0;
				// 布局 / 参数块大小真的变了才需要整份重写;只换版本(改代码、布局不动)
				// 沿用已写好的绑定,预览不会因为"换管线"白白闪一帧白贴图。
				const bool invalidate = previousMask != nextMask || gpu.ParamSize != nextSize;
				gpu.Key = key;
				gpu.KeyRevision = revision;
				gpu.Version = version;
				gpu.Layout = layout;
				gpu.HasLayout = hasLayout;
				gpu.ParamSize = nextSize;
				if (invalidate)
				{
					for (uint32_t index = 0; index < Renderer::FramesInFlight; ++index)
					{
						gpu.WrittenMask[index] = 0;
						gpu.ParamRevision[index] = 0;
						gpu.DescRevision[index] = 0;
					}
				}
			}

			// 参数 UBO:每材质 × 帧槽位;材质 Revision 变化时重打包(与引擎对象 UBO 同款
			// "提交期写 host-visible 缓冲")。
			if (gpu.HasLayout && gpu.ParamSize > 0)
			{
				Rhi::Handle<Rhi::Buffer>& buffer = gpu.ParamBuffers[slot];
				if (!buffer)
				{
					Rhi::BufferDesc desc;
					desc.Size = gpu.ParamSize;
					desc.Usage = Rhi::BufferUsageUniform;
					desc.Memory = Rhi::MemoryHint::HostVisible;
					desc.DebugName = "Renderer3D.SurfaceParamUBO";
					buffer = Renderer::GetDevice()->CreateBuffer(desc);
				}
				if (buffer && gpu.ParamRevision[slot] != revision)
				{
					std::vector<uint8_t> bytes;
					PackSurfaceParamBytes(material, gpu.Layout, &bytes);
					if (bytes.size() == gpu.ParamSize)
						buffer->SetData(bytes.data(), bytes.size());
					gpu.ParamRevision[slot] = revision;
				}
				draw.ParamBuffer = buffer;
			}

			Rhi::Handle<Rhi::DescriptorSet>& set = gpu.Sets[slot];
			if (!set)
				set = Renderer::GetDevice()->CreateDescriptorSet(state.SurfaceMaterialLayout);
			const uint32_t needed = SurfaceNeededBindings(gpu.Layout, gpu.HasLayout);
			const bool written = (gpu.WrittenMask[slot] & needed) == needed;
			if (!written || gpu.DescRevision[slot] != revision)
			{
				// 描述符写入必须留到通道外(与 MaterialSetFor 同款);同帧重复绘制不再排队。
				QueueSurfaceUpdate(state, material, slot);
				gpu.DescRevision[slot] = revision;
			}
			// 需要但还没写过的 binding → 这一帧先绑"默认表面材质集"(全白、全部槽位都写过);
			// 都写过(只是内容变了)就沿用旧集,下一帧补写 —— 与引擎材质同款一帧滞后。
			draw.SurfaceSet = written ? set : state.SurfaceDefaultSets[slot % Renderer::FramesInFlight];
			draw.Active = true;
			draw.Pipeline = pipeline;
			return draw;
		}


		// M4-TEX P2b:u_Flags.w = 法线贴图来自 BC5 产物(引擎标准着色器据此重建 Z)。
		//  - 数据源 = TextureLibrary 的产物查询(回退 stb 路径恒 0 ⇒ RGBA8 行为逐字节不变);
		//  - WLD_ENGINE_NORMAL_BC5=0 = 诊断覆盖,与包装层的 WLD_SURFACE_NORMAL_BC5 对称(A/B 抓图用)。
		// 首次加载那一帧查询可能未命中(描述符也要下一帧才写),与既有的一帧描述符滞后同口径。
float NormalBc5Flag(const MaterialDesc* desc){
			if (!desc || desc->NormalTexture.empty())
				return 0.0f;
			static const bool s_Disabled = []()
			{
				const char* value = std::getenv("WLD_ENGINE_NORMAL_BC5");
				return value && value[0] != '\0' && std::strcmp(value, "0") == 0;
			}();
			if (s_Disabled)
				return 0.0f;
			return TextureLibrary::Get().IsBc5Artifact(desc->NormalTexture, /*srgb*/ false)
				? 1.0f : 0.0f;
		}


		// M4-TEX P2b:材质贴图描述符的采样器来源 —— 产物命中时用产物头的 per-texture sampler
		// (wrap/filter/anisotropy,已按设备上限 clamp);其余(回退 stb / 共享白纹理 / 创建失败)
		// 继续用渲染器共享 sampler,老资产行为逐字节不变。
Rhi::Handle<Rhi::Sampler> TextureSamplerFor(const std::string& path, bool srgb, const Rhi::Handle<Rhi::Sampler>& shared){
			Rhi::Handle<Rhi::Sampler> sampler = TextureLibrary::Get().GetSampler(path, srgb);
			return sampler ? sampler : shared;
		}


		// 在渲染通道**之外**(BeginScene)补写挂起的表面材质描述符。
void FlushSurfaceUpdates(State& state){
			for (size_t index = 0; index < state.PendingSurfaceUpdates.size(); ++index)
			{
				const Ref<Material>& material = state.PendingSurfaceUpdates[index].second;
				const uint32_t slot = state.PendingSurfaceSlots[index] % Renderer::FramesInFlight;
				const auto cached = state.SurfaceCache.find(material.get());
				if (cached == state.SurfaceCache.end())
					continue;
				State::SurfaceMaterialGpu& gpu = cached->second;
				Rhi::Handle<Rhi::DescriptorSet>& set = gpu.Sets[slot];
				if (!set)
					continue;

				const MaterialDesc& desc = material->GetDesc();
				std::vector<Rhi::DescriptorWrite> writes;
				writes.reserve(2 + gpu.Layout.Textures.size());
				Rhi::DescriptorWrite albedo;
				albedo.Binding = 1;
				albedo.Type = Rhi::DescriptorType::CombinedImageSampler;
				albedo.Texture = TextureLibrary::Get().Get(desc.AlbedoTexture, /*srgb*/ true);
				albedo.Sampler = TextureSamplerFor(desc.AlbedoTexture, /*srgb*/ true, state.MaterialSampler);
				writes.push_back(albedo);
				Rhi::DescriptorWrite normal;
				normal.Binding = 2;
				normal.Type = Rhi::DescriptorType::CombinedImageSampler;
				normal.Texture = TextureLibrary::Get().Get(desc.NormalTexture, /*srgb*/ false);
				normal.Sampler = TextureSamplerFor(desc.NormalTexture, /*srgb*/ false, state.MaterialSampler);
				writes.push_back(normal);
				// 注解声明的贴图参数:绑定 = 反射到的 slot(t4..t11);没赋值 → 默认白贴图。
				// 着色器**静态**使用这些槽,不写描述符在 Vulkan 下是 VUID-vkCmdDrawIndexed-None-08600。
				for (const MaterialParamTextureSlot& texture : gpu.Layout.Textures)
				{
					Rhi::DescriptorWrite write;
					write.Binding = texture.Binding;
					write.Type = Rhi::DescriptorType::CombinedImageSampler;
					// 参数贴图按 sRGB 采样(编辑器里参数贴图的主用途是颜色;线性数据贴图
					// 目前没有区分入口 —— 需要时由主 agent 决定加注解字段)。
					write.Texture = TextureLibrary::Get().Get(material->ResolvedParamValue(texture.Name),
						/*srgb*/ true);
					write.Sampler = TextureSamplerFor(material->ResolvedParamValue(texture.Name),
						/*srgb*/ true, state.MaterialSampler);
					writes.push_back(write);
				}
				set->Update(writes);
				gpu.WrittenMask[slot] |= SurfaceNeededBindings(gpu.Layout, gpu.HasLayout);
				gpu.DescRevision[slot] = material->GetRevision();
			}
			state.PendingSurfaceUpdates.clear();
			state.PendingSurfaceSlots.clear();
		}


		// D5c-3b:蒙皮绘制核心。主通道与阴影通道只有"管线/对象槽位区/是否绑材质"三处差异,
		// 统一走这里,保证调色板绑定与统计口径一致。
		// 阴影管线的布局只有 set0/set1:多绑一个 set2 在 Vulkan 下是非法绑定(实测直接崩)。
void DrawSkinnedObject(State& state, const MeshGpu& mesh, const glm::mat4& transform, int32_t entityId, const glm::vec4* baseColor, const glm::mat4* palette, uint32_t paletteCount, uint32_t objectIndex, uint32_t paletteSlot, uint32_t indexCount, uint32_t firstIndex, const Rhi::Handle<Rhi::Pipeline>& pipeline, const Ref<Material>& material, uint32_t slot, bool shadow){
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
					NormalBc5Flag(desc) };
			}
			else
			{
				uniforms.BaseColor = baseColor ? *baseColor : glm::vec4(1.0f);
				uniforms.MetallicRoughness = { 0.0f, 0.5f, 0.0f, 0.0f };
				uniforms.Emissive = { 0.0f, 0.0f, 0.0f, 0.0f };
				uniforms.Flags = { 0.0f, 0.0f, 0.0f, 0.0f };
			}
			// 对象 UBO(binding 1)与调色板(binding 3)必须在同一次 Update 里写(见上面的说明)。
			// M4-S3:表面材质走 Skinned 变体管线;没有已发布版本 / 该变体没建出来 → 引擎蒙皮管线。
			// 阴影通道(Depth-only)不属于降级,继续用引擎阴影管线(D6)。
			SurfaceDrawState surface;
			if (!shadow && material)
				surface = PrepareSurfaceDraw(state, material, SurfacePipelineVariant::Skinned, slot);
			WriteObjectUniforms(state, slot, objectIndex, uniforms, objectBuffer, objectSet, boneBuffer,
				surface.ParamBuffer);

			Rhi::Handle<Rhi::DescriptorSet> materialSet;
			if (!shadow && material)
				materialSet = surface.Active ? surface.SurfaceSet : MaterialSetFor(state, material, slot);
			const Rhi::Handle<Rhi::Pipeline>& effectivePipeline =
				surface.Active ? surface.Pipeline : pipeline;

			state.CommandBuffer->BindPipeline(effectivePipeline);
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
Rhi::Handle<Rhi::DescriptorSet> MaterialSetFor(State& state, const Ref<Material>& material, uint32_t slot){
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
void FlushMaterialUpdates(State& state){
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
				albedo.Texture = TextureLibrary::Get().Get(desc.AlbedoTexture, /*srgb*/ true);
				albedo.Sampler = TextureSamplerFor(desc.AlbedoTexture, /*srgb*/ true, state.MaterialSampler);
				Rhi::DescriptorWrite normal;
				normal.Binding = 2;
				normal.Type = Rhi::DescriptorType::CombinedImageSampler;
				normal.Texture = TextureLibrary::Get().Get(desc.NormalTexture, /*srgb*/ false);
				normal.Sampler = TextureSamplerFor(desc.NormalTexture, /*srgb*/ false, state.MaterialSampler);
				gpu.Sets[slot]->Update({ albedo, normal });
				gpu.Revision[slot] = material->GetRevision();
			}
			state.PendingMaterialUpdates.clear();
			state.PendingMaterialSlots.clear();
		}


void TraceSubmit(uint32_t index, uint32_t slot, uint32_t indexCount, const glm::mat4& transform, const glm::vec4& color){
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
void EnsureMeshBuffersFor(State& state, const Ref<Mesh>& mesh){
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


uint32_t SubmitObject(State& state, const Ref<Mesh>& mesh, const Ref<Material>& material, const glm::vec4& baseColor, const glm::mat4& transform, int32_t entityId, uint32_t indexCount, uint32_t firstIndex){
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

			// M4-S3:有已发布表面管线的材质走表面变体(Solid / Transparent),否则引擎管线。
			SurfaceDrawState surface;
			if (desc)
			{
				surface = PrepareSurfaceDraw(state, material,
					desc->BlendMode == MaterialBlendMode::Transparent
						? SurfacePipelineVariant::Transparent : SurfacePipelineVariant::Solid, slot);
			}

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
					NormalBc5Flag(desc) };
			}
			else
			{
				uniforms.BaseColor = baseColor;
				uniforms.MetallicRoughness = { 0.0f, 0.5f, 0.0f, 0.0f };
				uniforms.Emissive = { 0.0f, 0.0f, 0.0f, 0.0f };
				uniforms.Flags = { 0.0f, 0.0f, 0.0f, 0.0f };
			}
			WriteObjectUniforms(state, slot, index, uniforms,
				state.ObjectUniformBuffers[slot][index], state.ObjectSets[slot][index],
				state.DefaultPaletteBuffer, surface.ParamBuffer);

			Rhi::Handle<Rhi::DescriptorSet> materialSet;
			const Rhi::Handle<Rhi::Pipeline>* pipeline = &state.Pipeline;
			if (desc)
			{
				if (surface.Active)
				{
					// M4-S3:该材质有已发布的表面管线 → 走它(透明 = Transparent 变体)。
					pipeline = &surface.Pipeline;
					materialSet = surface.SurfaceSet;
				}
				else
				{
					materialSet = MaterialSetFor(state, material, slot);
					if (desc->BlendMode == MaterialBlendMode::Transparent)
						pipeline = &state.TransparentPipeline;
				}
			}
			BindObject(state, slot, index, cached->second, *pipeline, state.ObjectSets[slot][index],
				materialSet, indexCount, firstIndex);
			TraceSubmit(index, slot, indexCount, transform, uniforms.BaseColor);
			return index;
		}

}

void Renderer3D::Init(){
		WLD_PROFILE_FUNCTION();
		State& state = GetState();
		// D8a2:阴影贴图边长来自项目清单(rendering.shadow_map_size);资源在这里按它创建,
		// 因此该字段**启动时生效**。清单校验已保证 2 的幂且 ∈[256,4096]。
		// 先按工作目录装载一次(宿主没提前 Apply 时也能拿到用户设置)。
		RenderSettings::LoadFromProject(std::filesystem::current_path());
		state.ShadowMapSize = RenderSettings::Get().ShadowMapSize;
		// P4-4b:MSAA 生效采样数(启动期参数;设备上限已在 RenderSettings::Msaa 里折算)。
		// 主通道的兼容渲染通道与所有主通道管线用它;阴影通道保持单采样(见下面的
		// shadowPipelineDesc.Samples),否则 rasterizationSamples 与阴影子通道不匹配。
		const Rhi::SampleCount sceneSamples = static_cast<Rhi::SampleCount>(RenderSettings::Msaa());
		const bool multisampled = sceneSamples != Rhi::SampleCount::Count1;

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
		// M4-S3:binding 4 = 材质参数块 `cbuffer MaterialParams : register(b4, space1)`
		// (表面函数材质的注解参数,像素阶段)。既有引擎着色器不声明/不读它 → 行为不变;
		// 但布局里必须有它,否则表面管线(用同一份 set 1)在 Vulkan 下会因为
		// "着色器静态使用 binding 4 而布局里没有"直接建不出管线。
		objectLayoutDesc.Bindings.push_back({ 4, Rhi::DescriptorType::UniformBuffer,
			Rhi::ShaderStageFlag(Rhi::ShaderStage::Fragment), 1 });
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

		// M4-S3:表面材质的 set 2 = 1(albedo)/2(normal)+ 4..11(注解声明的贴图参数,
		// 对应 space2 的 t4..t11)。引擎材质继续用 state.MaterialLayout({1,2}),
		// 两套布局互不影响;槽位表是**固定**的一段,所以表面管线只依赖这一份布局。
		{
			Rhi::DescriptorSetLayoutDesc surfaceLayoutDesc;
			surfaceLayoutDesc.Bindings.push_back({ 1, Rhi::DescriptorType::CombinedImageSampler,
				Rhi::ShaderStageFlag(Rhi::ShaderStage::Fragment), 1 });
			surfaceLayoutDesc.Bindings.push_back({ 2, Rhi::DescriptorType::CombinedImageSampler,
				Rhi::ShaderStageFlag(Rhi::ShaderStage::Fragment), 1 });
			for (uint32_t index = 0; index < kMaxMaterialTextureSlots; ++index)
			{
				surfaceLayoutDesc.Bindings.push_back({ ParamTextureBaseBinding() + index,
					Rhi::DescriptorType::CombinedImageSampler,
					Rhi::ShaderStageFlag(Rhi::ShaderStage::Fragment), 1 });
			}
			surfaceLayoutDesc.DebugName = "Renderer3D.SurfaceMaterial";
			state.SurfaceMaterialLayout = Renderer::GetDevice()->CreateDescriptorSetLayout(surfaceLayoutDesc);
		}

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
			albedo.Texture = TextureLibrary::Get().Get(std::string(), /*srgb*/ true);
			albedo.Sampler = TextureSamplerFor(std::string(), /*srgb*/ true, state.MaterialSampler);
			Rhi::DescriptorWrite normal;
			normal.Binding = 2;
			normal.Type = Rhi::DescriptorType::CombinedImageSampler;
			normal.Texture = TextureLibrary::Get().Get(std::string(), /*srgb*/ false);
			normal.Sampler = TextureSamplerFor(std::string(), /*srgb*/ false, state.MaterialSampler);
			state.DefaultMaterialSets[slot]->Update({ albedo, normal });
		}

		// M4-S3:表面材质的"默认描述符集"(每帧槽位一份)+ 共享零值参数缓冲。
		//  - 默认表面集:1/2 + 4..11 全部写白色贴图 → 任何时候都合法。表面材质的描述符集
		//    刚建好、还没轮到通道外补写时,这一帧先绑它 —— 绝不把"没写过"的描述符集绑给管线;
		//  - 参数缓冲:binding 4 每次都要写(描述符集跨帧复用),非表面绘制用它兜底。
		{
			Rhi::BufferDesc paramDesc;
			paramDesc.Size = kDefaultParamBlockBytes;
			paramDesc.Usage = Rhi::BufferUsageUniform;
			paramDesc.Memory = Rhi::MemoryHint::HostVisible;
			paramDesc.DebugName = "Renderer3D.SurfaceDefaultParamUBO";
			state.SurfaceDefaultParamBuffer = Renderer::GetDevice()->CreateBuffer(paramDesc);
			if (state.SurfaceDefaultParamBuffer)
			{
				const std::vector<uint8_t> zeros(kDefaultParamBlockBytes, 0);
				state.SurfaceDefaultParamBuffer->SetData(zeros.data(), zeros.size());
			}
			for (uint32_t slot = 0; slot < Renderer::FramesInFlight; ++slot)
			{
				if (!state.SurfaceMaterialLayout)
					break;
				state.SurfaceDefaultSets[slot] =
					Renderer::GetDevice()->CreateDescriptorSet(state.SurfaceMaterialLayout);
				if (!state.SurfaceDefaultSets[slot])
					continue;
				Rhi::DescriptorWrite albedo;
				albedo.Binding = 1;
				albedo.Type = Rhi::DescriptorType::CombinedImageSampler;
				albedo.Texture = TextureLibrary::Get().Get(std::string(), /*srgb*/ true);
				albedo.Sampler = TextureSamplerFor(std::string(), /*srgb*/ true, state.MaterialSampler);
				Rhi::DescriptorWrite normal;
				normal.Binding = 2;
				normal.Type = Rhi::DescriptorType::CombinedImageSampler;
				normal.Texture = TextureLibrary::Get().Get(std::string(), /*srgb*/ false);
				normal.Sampler = TextureSamplerFor(std::string(), /*srgb*/ false, state.MaterialSampler);
				std::vector<Rhi::DescriptorWrite> writes { albedo, normal };
				for (uint32_t index = 0; index < kMaxMaterialTextureSlots; ++index)
				{
					Rhi::DescriptorWrite texture;
					texture.Binding = ParamTextureBaseBinding() + index;
					texture.Type = Rhi::DescriptorType::CombinedImageSampler;
					texture.Texture = TextureLibrary::Get().Get(std::string(), /*srgb*/ true);
					texture.Sampler = TextureSamplerFor(std::string(), /*srgb*/ true, state.MaterialSampler);
					writes.push_back(texture);
				}
				state.SurfaceDefaultSets[slot]->Update(writes);
			}
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
		color.Samples = sceneSamples;
		color.Load = Rhi::LoadOp::Clear;
		color.Store = Rhi::StoreOp::Store;
		color.InitialLayout = Rhi::AttachmentLayout::ColorAttachment;
		color.FinalLayout = Rhi::AttachmentLayout::ColorAttachment;
		Rhi::RenderPassAttachment entityId;
		entityId.Format = Rhi::Format::R32_SINT;
		entityId.Samples = sceneSamples;
		entityId.Load = Rhi::LoadOp::Clear;
		entityId.Store = Rhi::StoreOp::Store;
		entityId.InitialLayout = Rhi::AttachmentLayout::ColorAttachment;
		entityId.FinalLayout = Rhi::AttachmentLayout::ColorAttachment;
		Rhi::RenderPassAttachment depth;
		depth.Format = Rhi::Format::D24_UNORM_S8_UINT;
		depth.Samples = sceneSamples;
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
		if (multisampled)
		{
			// P4-4b:与场景通道/两个预览面板完全相同的五附件结构(见 SceneRenderer::Init)。
			Rhi::RenderPassAttachment colorResolve;
			colorResolve.Format = Rhi::Format::R8G8B8A8_UNORM;
			colorResolve.Samples = Rhi::SampleCount::Count1;
			colorResolve.Load = Rhi::LoadOp::DontCare;
			colorResolve.Store = Rhi::StoreOp::Store;
			colorResolve.InitialLayout = Rhi::AttachmentLayout::ColorAttachment;
			colorResolve.FinalLayout = Rhi::AttachmentLayout::ColorAttachment;
			Rhi::RenderPassAttachment entityResolve;
			entityResolve.Format = Rhi::Format::R32_SINT;
			entityResolve.Samples = Rhi::SampleCount::Count1;
			entityResolve.Load = Rhi::LoadOp::DontCare;
			entityResolve.Store = Rhi::StoreOp::Store;
			entityResolve.InitialLayout = Rhi::AttachmentLayout::ColorAttachment;
			entityResolve.FinalLayout = Rhi::AttachmentLayout::ColorAttachment;
			passDesc.Attachments.push_back(colorResolve);
			passDesc.Attachments.push_back(entityResolve);
			subpass.ResolveAttachments = { 3, 4 };
		}
		passDesc.Subpasses = { subpass };
		state.RenderPass = Renderer::GetDevice()->CreateRenderPass(passDesc);

		const MeshVertexLayout meshLayout = Mesh::MakeStandardLayout();
		// HOTR-P1-T3:7 条引擎管线(Solid/Instanced/Transparent/Shadow/InstancedShadow/Skinned/
		// SkinnedShadow)统一由 CreateEnginePipelines 创建 —— Init 与 ReloadShaders 共用同一实现,
		// 保证热重载重建出的管线与启动期逐项一致。这里先建阴影通道资源,管线在下面统一建。

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

		// HOTR-P1-T3:7 条引擎管线(唯一实现 = CreateEnginePipelines;热重载走同一函数)。
		const Rhi::FrontFace frontFace = Renderer::GetBackendName() == "vulkan"
			? Rhi::FrontFace::Clockwise : Rhi::FrontFace::CounterClockwise;
		EnginePipelineSet pipelines;
		{
			std::string pipelineError;
			if (!CreateEnginePipelines(state, frontFace, sceneSamples, /*reload*/ false, pipelines, &pipelineError))
			{
				// 启动路径的编译失败已在着色器入口里断言/记 ERROR;这里补一条可读的装配原因。
				WLD_CORE_ERROR("Renderer3D: engine pipeline creation failed: {0}", pipelineError);
			}
		}
		state.Pipeline = pipelines.Solid;
		state.InstancedPipeline = pipelines.Instanced;
		state.TransparentPipeline = pipelines.Transparent;
		state.ShadowPipeline = pipelines.Shadow;
		state.InstancedShadowPipeline = pipelines.InstancedShadow;
		state.SkinnedPipeline = pipelines.Skinned;
		state.SkinnedShadowPipeline = pipelines.SkinnedShadow;

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

		// M4-S3:把"建表面管线需要的引擎资源"登记给 MaterialSurfaceRuntime(内部头,
		// 公开头不变)。Runtime 拿到的是句柄快照,不访问 Renderer3D 的私有 state;
		// Install 只会用它建管线,设备销毁前由 Shutdown 清掉。
		{
			SurfacePipelineEnvironment surfaceEnvironment;
			surfaceEnvironment.Valid = true;
			surfaceEnvironment.RenderPass = state.RenderPass;
			surfaceEnvironment.GlobalLayout = Renderer::GetGlobalDescriptorSetLayout();
			surfaceEnvironment.ObjectLayout = state.ObjectLayout;
			surfaceEnvironment.SurfaceMaterialLayout = state.SurfaceMaterialLayout;
			surfaceEnvironment.SolidLayout = meshLayout;
			surfaceEnvironment.InstancedLayout = WithInstanceBinding(meshLayout);
			surfaceEnvironment.SkinnedLayout = Mesh::MakeSkinnedLayout();
			surfaceEnvironment.Samples = sceneSamples;
			surfaceEnvironment.Front = frontFace;
			RegisterSurfacePipelineEnvironment(surfaceEnvironment);
		}
	}

}
