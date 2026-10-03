#include "MaterialEditorPanel_Internal.h"

namespace World
{

using namespace MaterialEditorPanelDetail;


	// ---- M3:继承 / 覆盖(状态唯一落点)----
	//  覆盖        = 本文件显式写了这个字段(.wmat 里有这一行);
	//  继承        = 本文件没写,但声明了父级文件 → 值来自父级链;
	//  引擎默认    = 本文件没写,父级链也到不了(没有父级 / 父级缺失退化)。
	// 三种状态都要在 a11y 里可读:`material.prop.<key>.state` 的 value 分别是
	// override / inherited / engine-default;`inherited` 还与"没写"一致 —— 探测脚本
	// 因此能直接断言"未覆盖字段 == 父级值"。
bool MaterialEditorPanel::FieldForKey(const std::string& key, MaterialField* field){
		struct Entry { const char* Key; MaterialField Field; };
		static const Entry kEntries[] = {
			{ "base", MaterialField::BaseColor },
			{ "metallic", MaterialField::Metallic },
			{ "roughness", MaterialField::Roughness },
			{ "emissive", MaterialField::Emissive },
			{ "albedo", MaterialField::AlbedoTexture },
			{ "normal", MaterialField::NormalTexture },
			{ "blend", MaterialField::BlendMode },
			{ "doublesided", MaterialField::DoubleSided },
		};
		for (const Entry& entry : kEntries)
		{
			if (key == entry.Key)
			{
				if (field)
					*field = entry.Field;
				return true;
			}
		}
		return false;
	}


MaterialEditorPanel::FieldState MaterialEditorPanel::StateOfField(MaterialField field) const{
		if (!m_Material)
			return FieldState::EngineDefault;
		if (m_Material->HasOverride(field))
			return FieldState::Override;
		// 父级文件真的解析到了才算"继承";父级缺失/坏(ResolvedParent() 为空)时值来自
		// 引擎默认 —— 头部会同时给出缺失告警(不把"退化的默认值"说成"父级的值")。
		return m_Material->ResolvedParent() ? FieldState::Inherited : FieldState::EngineDefault;
	}


const char* MaterialEditorPanel::FieldStateName(FieldState state){
		switch (state)
		{
			case FieldState::Override: return "override";
			case FieldState::Inherited: return "inherited";
			default: return "engine-default";
		}
	}


std::string MaterialEditorPanel::FieldStateDoc(const RowPlan& row, FieldState state) const{
		if (!m_Material)
			return {};
		const std::string value = MaterialIO::FormatFieldValue(m_Material->GetDesc(), row.Field);
		if (state == FieldState::Override)
		{
			const std::string parent = m_Material->ResolvedParent()
				? m_Material->ParentPath()
				: Wui::Tr("panel.material.parent.engine", "Engine Default");
			return Wui::Tr("panel.material.field.override.doc", "Overridden here:") + " " + value
				+ "\n" + Wui::Tr("panel.material.field.override.revert",
					"Press the revert button to inherit from:") + " " + parent;
		}
		if (state == FieldState::Inherited)
			return Wui::Tr("panel.material.field.inherited.doc", "Inherited from:") + " "
				+ m_Material->ParentPath() + " = " + value;
		return Wui::Tr("panel.material.field.engine.doc", "Engine default:") + " " + value;
	}


int MaterialEditorPanel::GroupOverrideCount(const std::string& groupKey) const{
		int count = 0;
		for (int index = 0; index < kRowSpecCount; ++index)
		{
			const RowSpec& spec = kRowSpecs[index];
			if (groupKey != spec.Group || !spec.HasReset || spec.ReadOnly)
				continue;
			if (groupKey == std::string("preview"))
			{
				if (PreviewOptionModified(spec.Key))
					++count;
				continue;
			}
			if (FieldModified(spec.Key))
				++count;
		}
		return count;
	}


bool MaterialEditorPanel::PreviewOptionModified(const std::string& key) const{
		if (key == "preview.mesh")
			return m_PreviewMesh != PreviewMesh::Sphere;
		if (key == "preview.bg")
			return m_PreviewBackground != PreviewBackground::Solid;
		if (key == "preview.light")
			return m_PreviewLighting != PreviewLighting::ThreePoint;
		if (key == "preview.light.intensity")
			return m_LightIntensity != 1.0f;
		if (key == "preview.light.azimuth")
			return m_LightAzimuth != 35.0f;
		if (key == "preview.light.elevation")
			return m_LightElevation != 45.0f;
		if (key == "preview.wireframe")
			return m_ShowWireframe;
		if (key == "preview.normals")
			return m_ShowNormals;
		if (key == "preview.uvchecker")
			return m_ShowUvChecker;
		return false;
	}


void MaterialEditorPanel::SetFieldToDefault(const std::string& key){
		if (!m_Material)
			return;
		// 预览选项不写进 .wmat:复位只动面板状态。
		if (key == "preview.mesh")
			m_PreviewMesh = PreviewMesh::Sphere;
		else if (key == "preview.bg")
			m_PreviewBackground = PreviewBackground::Solid;
		else if (key == "preview.light")
			m_PreviewLighting = PreviewLighting::ThreePoint;
		else if (key == "preview.light.intensity")
			m_LightIntensity = 1.0f;
		else if (key == "preview.light.azimuth")
			m_LightAzimuth = 35.0f;
		else if (key == "preview.light.elevation")
			m_LightElevation = 45.0f;
		else if (key == "preview.wireframe")
			m_ShowWireframe = false;
		else if (key == "preview.normals")
			m_ShowNormals = false;
		else if (key == "preview.uvchecker")
			m_ShowUvChecker = false;
		else
		{
			// M3:材质字段的复位 = **回退到父级**(没有父级 = 回退引擎内置默认):
			// 清掉覆盖位,值重新从父级链解析。Revision 与脏标记由 RevertField 负责
			// (本来就是继承态时是 no-op,不产生"假脏")。
			MaterialField field;
			if (!FieldForKey(key, &field))
				return;
			m_Material->RevertField(field);
		}
	}


void MaterialEditorPanel::ResetAllMaterialFields(){
		if (!m_Material)
			return;
		// M3:整份材质"回退到父级" —— 8 个可继承字段逐个清覆盖位(没有父级 = 引擎默认)。
		for (uint8_t index = 0; index < static_cast<uint8_t>(MaterialField::Count); ++index)
			m_Material->RevertField(static_cast<MaterialField>(index));
		m_Status = m_Material->ParentPath().empty()
			? Wui::Tr("panel.material.status.revert_engine",
				"Reverted every parameter to the engine default (not saved yet; press Save to write the .wmat)")
			: Wui::Tr("panel.material.status.revert_parent",
				"Reverted every parameter to its parent (not saved yet; press Save to write the .wmat)");
		m_StatusIsError = false;
	}


void MaterialEditorPanel::RefreshValidation(double now){
		m_Validation.clear();
		if (!m_Material)
			return;
		const MaterialDesc& desc = m_Material->GetDesc();
		const auto checkTexture = [&](const std::string& logical, const char* field,
			const std::string& missingSource, const std::string& missingAsset,
			const std::string& assetWithoutSource)
		{
			if (logical.empty())
				return;
			// M4-TEX-P11:现场判定(能不能用)来自编辑器侧适配层,面板不再自己解析资产/源图。
			const Editor::TextureRefFacts info = Editor::TextureRefFactsFor(logical);
			// ① 引用不在内容根内(绝对路径 / `..` 逃逸):打包/复制项目会丢 —— 第三类问题。
			if (!info.InContentRoot)
			{
				m_Validation.push_back({ field, logical, "unreferenced" });
				return;
			}
			// ② `.wtex` 资产:资产缺失 / 资产 → 源图缺失都要**说出来**(不是静默通过)。
			//    资产能读但源图缺 = 运行时只能回退成白纹理,所以同样是一条问题(独立 severity)。
			if (info.IsAsset)
			{
				if (!info.AssetExists)
				{
					m_Validation.push_back({ field, missingAsset + ": " + logical, "missing" });
					return;
				}
				// M4-TEX P9:单文件容器 = 有效(设置 + 内嵌源字节在一个文件里),不报 missing-source。
				// 只有**旧式**(无 `---payload`)且 `source:` 缺失/找不到才报,并给"可重新导入"的提示。
				if (!info.Embedded && (!info.Error.empty() || !info.SourceExists))
				{
					const std::string source = info.Source.empty()
						? Wui::Tr("panel.material.texture.ref.source_none", "(no source image)")
						: info.Source;
					std::string detail = assetWithoutSource + ": " + logical + " → " + source;
					if (info.Legacy)
						detail += " — " + Wui::Tr("panel.material.texture.warn.legacy_hint",
							"re-import the image in the editor to get a single-file asset");
					m_Validation.push_back({ field, detail, "missing-source" });
				}
				return;
			}
			if (!info.SourceExists)
				m_Validation.push_back({ field, missingSource + ": " + logical, "missing" });
		};
		checkTexture(desc.AlbedoTexture, "albedo",
			Wui::Tr("panel.material.validation.missing_albedo", "Albedo texture not found"),
			Wui::Tr("panel.material.validation.missing_albedo_asset",
				"Albedo texture asset not found"),
			Wui::Tr("panel.material.validation.missing_albedo_source",
				"Albedo texture asset has no source image"));
		checkTexture(desc.NormalTexture, "normal",
			Wui::Tr("panel.material.validation.missing_normal", "Normal texture not found"),
			Wui::Tr("panel.material.validation.missing_normal_asset",
				"Normal texture asset not found"),
			Wui::Tr("panel.material.validation.missing_normal_source",
				"Normal texture asset has no source image"));

		const auto checkRange = [&](const char* field, const char* label, float value, float minimum,
			float maximum)
		{
			if (value >= minimum && value <= maximum)
				return;
			char buffer[128] = {};
			// MAT-UI3b:校验条目的句子也要走目录(字段名保持技术原名:它就是 .wmat 里的字段)。
			std::snprintf(buffer, sizeof(buffer),
				Wui::Tr("panel.material.validation.range", "%s out of range [%g, %g]: %g").c_str(), label,
				static_cast<double>(minimum), static_cast<double>(maximum), static_cast<double>(value));
			m_Validation.push_back({ field, buffer, "range" });
		};
		checkRange("base", "BaseColor R", desc.BaseColor.r, 0.0f, 1.0f);
		checkRange("base", "BaseColor G", desc.BaseColor.g, 0.0f, 1.0f);
		checkRange("base", "BaseColor B", desc.BaseColor.b, 0.0f, 1.0f);
		checkRange("base", "BaseColor A", desc.BaseColor.a, 0.0f, 1.0f);
		checkRange("metallic", "Metallic", desc.Metallic, 0.0f, 1.0f);
		checkRange("roughness", "Roughness", desc.Roughness, 0.0f, 1.0f);
		checkRange("emissive", "Emissive R", desc.Emissive.r, 0.0f, 8.0f);
		checkRange("emissive", "Emissive G", desc.Emissive.g, 0.0f, 8.0f);
		checkRange("emissive", "Emissive B", desc.Emissive.b, 0.0f, 8.0f);
		m_ValidationRevision = m_Material->GetRevision();
		m_ValidationPath = m_Path;
		m_ValidationTime = now;
	}


void MaterialEditorPanel::ReleaseGpuResources(bool defer){
		if (defer)
		{
			// 帧在飞:把旧句柄搬到延迟释放队列(与 MaterialTextureCache::Invalidate /
			// SceneRenderer::OnResize 同一条路径)。GL 立即执行,Vulkan 等帧栅栏。
			std::array<Rhi::Handle<Rhi::CommandBuffer>, Renderer::FramesInFlight> commands {};
			for (uint32_t slot = 0; slot < Renderer::FramesInFlight; ++slot)
				commands[slot] = m_PreviewCommands[slot];
			Renderer::QueueRelease([pass = m_PreviewPass, framebuffer = m_PreviewFramebuffer,
				color = m_PreviewColor, entityId = m_PreviewEntityId, depth = m_PreviewDepth,
				colorMsaa = m_PreviewColorMsaa, entityMsaa = m_PreviewEntityMsaa,
				depthMsaa = m_PreviewDepthMsaa, commands, cameraBuffer = m_PreviewCameraBuffer,
				lightBuffer = m_PreviewLightBuffer, cameraSet = m_PreviewCameraSet]() {});
		}
		m_PreviewPass = nullptr;
		m_PreviewFramebuffer = nullptr;
		m_PreviewColor = nullptr;
		m_PreviewEntityId = nullptr;
		m_PreviewDepth = nullptr;
		m_PreviewColorMsaa = nullptr;
		m_PreviewEntityMsaa = nullptr;
		m_PreviewDepthMsaa = nullptr;
		for (Rhi::Handle<Rhi::CommandBuffer>& command : m_PreviewCommands)
			command = nullptr;
		m_PreviewCameraBuffer = nullptr;
		m_PreviewLightBuffer = nullptr;
		m_PreviewCameraSet = nullptr;
		m_GpuDevice = nullptr;
		m_GpuTargetW = 0;
		m_GpuTargetH = 0;
	}


void MaterialEditorPanel::EnsureGpuResources(){
		Rhi::Handle<Rhi::Device> device = Renderer::GetDevice();
		if (!device)
			return;
		if (m_GpuDevice == device.get() && m_PreviewFramebuffer
			&& m_GpuTargetW == m_PreviewTargetW && m_GpuTargetH == m_PreviewTargetH)
			return;
		// 尺寸变了:只重建 GPU 资源,**保留注册表槽位**(id 不变;Update 会推进内容代,
		// WUI 后端的描述符集缓存随之失效 —— 不会采样到已销毁贴图)。
		// 旧资源走延迟释放:本帧之前提交的命令可能还在引用它们(见 ReleaseGpuResources 注释)。
		const uint64_t registryId = m_PreviewTextureId;
		ReleaseGpuResources(/*defer*/ true);
		m_PreviewTextureId = registryId;
		m_GpuDevice = device.get();
		m_GpuTargetW = m_PreviewTargetW;
		m_GpuTargetH = m_PreviewTargetH;

		// 预览目标:与 SceneRenderer 同样的结构(颜色 + entity id + 深度),
		// 这样 Renderer3D 的管线(它就是按这个结构建的)可以直接用。
		// P4-4b:rendering.msaa>1 时升级为与场景通道**完全相同**的五附件结构 ——
		// 颜色 / 实体 id / 深度多采样,再把颜色 / 实体 id resolve 到各自的单采样纹理;
		// WUI 采样与抓图读的仍是单采样 m_PreviewColor,语义不变。
		const Rhi::SampleCount previewSamples = static_cast<Rhi::SampleCount>(RenderSettings::Msaa());
		const bool multisampled = previewSamples != Rhi::SampleCount::Count1;

		Rhi::TextureDesc colorDesc;
		colorDesc.Type = Rhi::TextureType::Texture2D;
		colorDesc.Format = Rhi::Format::R8G8B8A8_UNORM;
		colorDesc.Extent = { m_PreviewTargetW, m_PreviewTargetH, 1 };
		colorDesc.Usage = Rhi::TextureUsageColorAttachment | Rhi::TextureUsageSampled;
		colorDesc.DebugName = "Material.Preview.Color";
		m_PreviewColor = device->CreateTexture(colorDesc);

		Rhi::TextureDesc entityDesc = colorDesc;
		entityDesc.Format = Rhi::Format::R32_SINT;
		entityDesc.Usage = Rhi::TextureUsageColorAttachment;
		entityDesc.DebugName = "Material.Preview.EntityId";
		m_PreviewEntityId = device->CreateTexture(entityDesc);

		Rhi::TextureDesc depthDesc = colorDesc;
		depthDesc.Format = Rhi::Format::D24_UNORM_S8_UINT;
		depthDesc.Usage = Rhi::TextureUsageDepthStencilAttachment;
		depthDesc.DebugName = "Material.Preview.Depth";
		if (multisampled)
		{
			// 多采样附件(只做绘制附件,内容由通道末的 resolve 写进上面的单采样纹理)。
			Rhi::TextureDesc msaaColorDesc = colorDesc;
			msaaColorDesc.Samples = previewSamples;
			msaaColorDesc.Usage = Rhi::TextureUsageColorAttachment;
			msaaColorDesc.DebugName = "Material.Preview.ColorMSAA";
			m_PreviewColorMsaa = device->CreateTexture(msaaColorDesc);
			Rhi::TextureDesc msaaEntityDesc = msaaColorDesc;
			msaaEntityDesc.Format = Rhi::Format::R32_SINT;
			msaaEntityDesc.DebugName = "Material.Preview.EntityIdMSAA";
			m_PreviewEntityMsaa = device->CreateTexture(msaaEntityDesc);
			Rhi::TextureDesc msaaDepthDesc = msaaColorDesc;
			msaaDepthDesc.Format = Rhi::Format::D24_UNORM_S8_UINT;
			msaaDepthDesc.Usage = Rhi::TextureUsageDepthStencilAttachment;
			msaaDepthDesc.DebugName = "Material.Preview.DepthMSAA";
			m_PreviewDepthMsaa = device->CreateTexture(msaaDepthDesc);
		}
		else
		{
			// msaa==1:单采样深度就是附件(与旧代码逐字节一致)。
			m_PreviewDepth = device->CreateTexture(depthDesc);
		}

		Rhi::RenderPassDesc passDesc;
		Rhi::RenderPassAttachment color;
		color.Format = Rhi::Format::R8G8B8A8_UNORM;
		color.Samples = previewSamples;
		color.Load = Rhi::LoadOp::Clear;
		color.Store = Rhi::StoreOp::Store;
		color.InitialLayout = Rhi::AttachmentLayout::Undefined;
		// 预览纹理在本通道结束后立刻被 WUI 当采样贴图使用,所以"可采样"的那张附件
		// FinalLayout 直接声明为 ShaderReadOnly:渲染通道会做隐式转换。之前声明的是
		// ColorAttachment,文本又要手动转一次 ShaderReadOnly,而后端只发隐式转换
		// (手动屏障因布局一致被跳过),于是 UI 采样到"布局未就绪"的纹理 → 预览闪烁
		// (且抓图读到垃圾数据)。msaa>1 时被采样的是 resolve 目标(附件 3),多采样
		// 颜色附件只做绘制,因此停在 ColorAttachment。
		color.FinalLayout = multisampled ? Rhi::AttachmentLayout::ColorAttachment
			: Rhi::AttachmentLayout::ShaderReadOnly;
		color.Clear.Color = { 0.12f, 0.13f, 0.15f, 1.0f };
		Rhi::RenderPassAttachment entityId;
		entityId.Format = Rhi::Format::R32_SINT;
		entityId.Samples = previewSamples;
		entityId.Load = Rhi::LoadOp::Clear;
		entityId.Store = Rhi::StoreOp::Store;
		entityId.InitialLayout = Rhi::AttachmentLayout::Undefined;
		entityId.FinalLayout = Rhi::AttachmentLayout::ColorAttachment;
		const int minusOne = -1;
		std::memcpy(&entityId.Clear.Color, &minusOne, sizeof(int));
		Rhi::RenderPassAttachment depth;
		depth.Format = Rhi::Format::D24_UNORM_S8_UINT;
		depth.Samples = previewSamples;
		depth.Load = Rhi::LoadOp::Clear;
		depth.Store = Rhi::StoreOp::Store;
		depth.InitialLayout = Rhi::AttachmentLayout::Undefined;
		depth.FinalLayout = Rhi::AttachmentLayout::DepthStencilAttachment;
		depth.Clear.IsDepthStencil = true;
		depth.Clear.DepthStencil.Depth = 1.0f;
		passDesc.Attachments = { color, entityId, depth };
		Rhi::SubpassDesc subpass;
		subpass.ColorAttachments = { { 0, Rhi::AttachmentLayout::ColorAttachment },
			{ 1, Rhi::AttachmentLayout::ColorAttachment } };
		subpass.DepthStencilAttachment = { 2, Rhi::AttachmentLayout::DepthStencilAttachment };
		if (multisampled)
		{
			// 3 = 单采样颜色 resolve(即 m_PreviewColor,离场隐式转 ShaderReadOnly 供 WUI 采样),
			// 4 = 单采样实体 id resolve;与场景通道 / Renderer2D / Renderer3D 兼容通道同结构。
			Rhi::RenderPassAttachment colorResolve;
			colorResolve.Format = Rhi::Format::R8G8B8A8_UNORM;
			colorResolve.Samples = Rhi::SampleCount::Count1;
			colorResolve.Load = Rhi::LoadOp::DontCare;   // resolve 会整体覆盖
			colorResolve.Store = Rhi::StoreOp::Store;
			colorResolve.InitialLayout = Rhi::AttachmentLayout::Undefined;
			colorResolve.FinalLayout = Rhi::AttachmentLayout::ShaderReadOnly;
			Rhi::RenderPassAttachment entityResolve;
			entityResolve.Format = Rhi::Format::R32_SINT;
			entityResolve.Samples = Rhi::SampleCount::Count1;
			entityResolve.Load = Rhi::LoadOp::DontCare;
			entityResolve.Store = Rhi::StoreOp::Store;
			entityResolve.InitialLayout = Rhi::AttachmentLayout::Undefined;
			entityResolve.FinalLayout = Rhi::AttachmentLayout::ColorAttachment;
			passDesc.Attachments.push_back(colorResolve);    // 3
			passDesc.Attachments.push_back(entityResolve);   // 4
			subpass.ResolveAttachments = { 3, 4 };
		}
		passDesc.Subpasses = { subpass };
		passDesc.DebugName = "Material.PreviewPass";
		m_PreviewPass = device->CreateRenderPass(passDesc);

		Rhi::FramebufferDesc framebufferDesc;
		framebufferDesc.RenderPass = m_PreviewPass;
		framebufferDesc.Extent = { m_PreviewTargetW, m_PreviewTargetH };
		// 顺序必须与渲染通道附件表 1:1(Vulkan 要求 framebuffer 附件数/顺序与通道一致)。
		if (multisampled)
			framebufferDesc.Attachments = { m_PreviewColorMsaa, m_PreviewEntityMsaa, m_PreviewDepthMsaa,
				m_PreviewColor, m_PreviewEntityId };
		else
			framebufferDesc.Attachments = { m_PreviewColor, m_PreviewEntityId, m_PreviewDepth };
		framebufferDesc.DebugName = "Material.PreviewFramebuffer";
		m_PreviewFramebuffer = device->CreateFramebuffer(framebufferDesc);

		// P4-UX16b:每条帧槽位一条(见头文件说明)。调试名带槽位号,取证时能看出是哪一份。
		for (uint32_t slot = 0; slot < Renderer::FramesInFlight; ++slot)
			m_PreviewCommands[slot] = device->CreateCommandBuffer("Material.Preview#" + std::to_string(slot));

		Rhi::BufferDesc cameraDesc;
		cameraDesc.Size = sizeof(glm::mat4);
		cameraDesc.Usage = Rhi::BufferUsageUniform;
		cameraDesc.Memory = Rhi::MemoryHint::HostVisible;
		cameraDesc.DebugName = "Material.Preview.Camera";
		m_PreviewCameraBuffer = device->CreateBuffer(cameraDesc);

		// U21:预览灯光 UBO(set0 binding 2)。自建 set0 的调用方必须把相机(0)/灯光(2)/
		// 阴影贴图(3)写在**同一次 Update** 里(GL 后端是整体替换语义)。
		Rhi::BufferDesc lightDesc;
		lightDesc.Size = sizeof(LightUniforms);
		lightDesc.Usage = Rhi::BufferUsageUniform;
		lightDesc.Memory = Rhi::MemoryHint::HostVisible;
		lightDesc.DebugName = "Material.Preview.Light";
		m_PreviewLightBuffer = device->CreateBuffer(lightDesc);

		m_PreviewCameraSet = device->CreateDescriptorSet(Renderer::GetGlobalDescriptorSetLayout());
		if (m_PreviewCameraSet)
		{
			std::vector<Rhi::DescriptorWrite> writes;
			Rhi::DescriptorWrite camera;
			camera.Binding = 0;
			camera.Type = Rhi::DescriptorType::UniformBuffer;
			camera.Buffer = m_PreviewCameraBuffer;
			writes.push_back(camera);
			for (Rhi::DescriptorWrite& lighting : Renderer3D::MakeGlobalLightingWrites(m_PreviewLightBuffer))
				writes.push_back(std::move(lighting));
			m_PreviewCameraSet->Update(writes);
		}
	}


const Ref<Mesh>& MaterialEditorPanel::PreviewMeshFor(PreviewMesh kind){
		const int index = std::clamp(static_cast<int>(kind), 0, 2);
		if (!m_PreviewMeshes[index])
		{
			switch (static_cast<PreviewMesh>(index))
			{
				case PreviewMesh::Cube:
					m_PreviewMeshes[index] = Mesh::CreateUnitCube(1.7f);
					break;
				case PreviewMesh::Plane:
					m_PreviewMeshes[index] = Mesh::CreateUnitPlane(1.8f);
					break;
				case PreviewMesh::Sphere:
				default:
					m_PreviewMeshes[index] = Mesh::CreateUnitSphere(2.0f, 48, 24);
					break;
			}
		}
		return m_PreviewMeshes[index];
	}


void MaterialEditorPanel::BuildDerivedMeshes(){
		const int index = std::clamp(static_cast<int>(m_PreviewMesh), 0, 2);
		if (m_DerivedMeshFor == index && m_WireMesh && m_NormalMesh && m_CheckLightMesh && m_CheckDarkMesh)
			return;
		const Ref<Mesh>& source = PreviewMeshFor(m_PreviewMesh);
		if (!source)
			return;
		m_CheckLightMesh = nullptr;
		m_CheckDarkMesh = nullptr;
		m_WireMesh = nullptr;
		m_NormalMesh = nullptr;
		// 尺寸系数按包围半径算,球/立方/平面三种网格的观感一致。
		const float radius = std::max(0.2f, source->GetBounds().GetRadius());
		BuildCheckerMeshes(*source, &m_CheckLightMesh, &m_CheckDarkMesh);
		m_WireMesh = BuildWireMesh(*source, radius * 0.016f, radius * 0.0025f);
		// 法线柱:长度 25% 半径、截面半径 1.2% 半径(在 400px 预览上约 2-3px 宽,看得见)。
		m_NormalMesh = BuildNormalMesh(*source, radius * 0.25f, radius * 0.012f, radius * 0.004f);
		m_DerivedMeshFor = index;
	}


void MaterialEditorPanel::EnsureOverrideMaterials(){
		// 预览专用纯色材质:不进资产库缓存(不落盘、不出现在内容浏览器),
		// 只被本面板的覆盖绘制(线框/法线/UV 棋盘格)引用。
		if (!m_OverrideLight)
		{
			m_OverrideLight = MaterialLibrary::Get().CreateDefault("Material Preview Cell");
			m_OverrideLight->SetBaseColor({ 0.87f, 0.87f, 0.87f, 1.0f });
			m_OverrideLight->SetRoughness(0.95f);
			// 双面:覆盖层的四边形是按 UV 参数域拼的,UV 镜像的三角形上绕序会反转,
			// 单面材质会把它们当背面剔掉(实测 UV 棋盘格出现大片黑洞)。
			m_OverrideLight->SetDoubleSided(true);
		}
		if (!m_OverrideDark)
		{
			m_OverrideDark = MaterialLibrary::Get().CreateDefault("Material Preview Cell Dark");
			m_OverrideDark->SetBaseColor({ 0.18f, 0.20f, 0.24f, 1.0f });
			m_OverrideDark->SetRoughness(0.95f);
			m_OverrideDark->SetDoubleSided(true);
		}
		if (!m_OverrideWire)
		{
			m_OverrideWire = MaterialLibrary::Get().CreateDefault("Material Preview Wire");
			m_OverrideWire->SetBaseColor({ 1.0f, 0.62f, 0.18f, 1.0f });
			m_OverrideWire->SetRoughness(1.0f);
			m_OverrideWire->SetDoubleSided(true);
		}
		if (!m_OverrideNormal)
		{
			m_OverrideNormal = MaterialLibrary::Get().CreateDefault("Material Preview Normals");
			m_OverrideNormal->SetBaseColor({ 0.25f, 0.82f, 1.0f, 1.0f });
			m_OverrideNormal->SetRoughness(1.0f);
			m_OverrideNormal->SetDoubleSided(true);
		}
	}


	// U21+P4-U13f 口径:目标尺寸 = 预览区物理像素(设计单位 × UiScale),长边 [128, 2048] 等比 clamp。
	// render_scale **不参与**:材质预览自建 framebuffer(不走 SceneRenderer::OnResize),
	// 读数里把它回显出来,供探针断言"改 render_scale 不影响预览目标"。
void MaterialEditorPanel::UpdatePreviewTargetSize(const Wui::WuiRect& view){
		const float uiScale = Wui::UiScale() > 0.0f ? Wui::UiScale() : 1.0f;
		const float pixelW = std::max(1.0f, view.W * uiScale);
		const float pixelH = std::max(1.0f, view.H * uiScale);
		const float longSide = std::max(pixelW, pixelH);
		float clampScale = 1.0f;
		if (longSide > kPreviewTargetMaxSide)
			clampScale = kPreviewTargetMaxSide / longSide;
		else if (longSide < kPreviewTargetMinSide)
			clampScale = kPreviewTargetMinSide / longSide;
		const uint32_t targetW = static_cast<uint32_t>(std::max(1.0f, std::round(pixelW * clampScale)));
		const uint32_t targetH = static_cast<uint32_t>(std::max(1.0f, std::round(pixelH * clampScale)));
		m_PreviewUiScale = uiScale;
		m_PreviewViewW = view.W;
		m_PreviewViewH = view.H;
		if (targetW == m_PreviewTargetW && targetH == m_PreviewTargetH)
			return;
		m_PreviewTargetW = targetW;
		m_PreviewTargetH = targetH;
		WLD_CORE_INFO("[material-ui] preview target {0}x{1} (view {2:.0f}x{3:.0f} design, uiScale={4:.2f})",
			m_PreviewTargetW, m_PreviewTargetH, view.W, view.H, uiScale);
	}


uint64_t MaterialEditorPanel::RenderPreview(){
		if (!m_Material)
			return 0;
		EnsureGpuResources();
		// P4-UX16b:本帧槽位专属的命令缓冲(见头文件:单缓冲会在上一帧还没跑完时重录)。
		Rhi::Handle<Rhi::CommandBuffer>& command =
			m_PreviewCommands[Renderer::FrameSlot() % Renderer::FramesInFlight];
		const Ref<Mesh>& mesh = PreviewMeshFor(m_PreviewMesh);
		if (!m_PreviewFramebuffer || !m_PreviewCameraSet || !mesh || !command)
			return 0;

		EnsureOverrideMaterials();
		BuildDerivedMeshes();

		const float distance = m_CameraDistance;
		const glm::vec3 eye {
			distance * std::cos(m_OrbitPitch) * std::sin(m_OrbitYaw),
			distance * std::sin(m_OrbitPitch),
			distance * std::cos(m_OrbitPitch) * std::cos(m_OrbitYaw) };
		const glm::mat4 view = glm::lookAt(eye, glm::vec3(0.0f), glm::vec3(0.0f, 1.0f, 0.0f));
		// 宽高比 = 目标宽高比(非方形预览区不会被拉伸)。
		const float aspect = m_PreviewTargetH > 0
			? static_cast<float>(m_PreviewTargetW) / static_cast<float>(m_PreviewTargetH)
			: 1.0f;
		const glm::mat4 projection = glm::perspective(glm::radians(35.0f), aspect,
			std::max(0.01f, distance * 0.01f), distance * 8.0f + 10.0f);
		// 与场景同一条投影适配(离屏不做 Y 翻转,只补 Vulkan 的深度范围)。
		const glm::mat4 viewProjection = AdaptViewProjectionForOffscreen(projection * view,
			Renderer::GetBackendName() == "vulkan");
		m_PreviewCameraBuffer->SetData(&viewProjection, sizeof(glm::mat4));
		const LightUniforms lights = BuildPreviewLightUniforms(static_cast<int>(m_PreviewLighting),
			m_LightIntensity, m_LightAzimuth, m_LightElevation);
		if (m_PreviewLightBuffer)
			m_PreviewLightBuffer->SetData(&lights, sizeof(lights));

		std::vector<Rhi::ClearValue> clears(3);
		// 纯色背景 = 清屏色;渐变背景清成**透明**,由 WUI 在贴图下面画渐变(见 DrawPreview)。
		const bool gradientBackground = m_PreviewBackground == PreviewBackground::Gradient;
		if (gradientBackground)
			clears[0].Color = { 0.0f, 0.0f, 0.0f, 0.0f };
		else
			clears[0].Color = { 0.12f, 0.13f, 0.15f, 1.0f };
		const int minusOne = -1;
		std::memcpy(&clears[1].Color, &minusOne, sizeof(int));
		clears[2].IsDepthStencil = true;
		clears[2].DepthStencil.Depth = 1.0f;

		command->Begin();
		command->BeginRenderPass(m_PreviewPass, m_PreviewFramebuffer, clears);
		command->SetViewport({ 0, 0, static_cast<float>(m_PreviewTargetW),
			static_cast<float>(m_PreviewTargetH) });
		command->SetScissor({ 0, 0, m_PreviewTargetW, m_PreviewTargetH });
		command->BindDescriptorSet(m_PreviewCameraSet, 0);
		Renderer3D::BeginScene(viewProjection, command);
		// 预览用**固定槽位区**(按面板身份映射):否则每个面板/主场景都从序号 0 开始分配,
		// 会争用同一份对象 UBO 与材质描述符集,两个内容不同的材质面板就会逐帧互相覆盖
		// (用户实测:预览一直闪烁)。span=5:底材质 + 棋盘格两格 + 线框 + 法线。
		const uint32_t slotBase = Renderer3D::ReserveSlotBase(
			static_cast<uint32_t>(Wui::HashId(m_PanelId.c_str()) ^ 0x9E37u), 5);
		const glm::mat4 identity { 1.0f };
		// 覆盖层先画(它们贴在表面外侧,深度写让底材质不会盖掉细线)。
		if (m_ShowUvChecker && m_CheckLightMesh)
			Renderer3D::SubmitAtSlot(slotBase + 1, m_CheckLightMesh, m_OverrideLight, identity, -1);
		if (m_ShowUvChecker && m_CheckDarkMesh)
			Renderer3D::SubmitAtSlot(slotBase + 2, m_CheckDarkMesh, m_OverrideDark, identity, -1);
		if (m_ShowWireframe && m_WireMesh)
			Renderer3D::SubmitAtSlot(slotBase + 3, m_WireMesh, m_OverrideWire, identity, -1);
		if (m_ShowNormals && m_NormalMesh)
			Renderer3D::SubmitAtSlot(slotBase + 4, m_NormalMesh, m_OverrideNormal, identity, -1);
		// 底材质:UV 棋盘格打开时不画(棋盘格的意义是看 UV,不是看材质)。
		if (!m_ShowUvChecker)
			Renderer3D::SubmitAtSlot(slotBase, mesh, m_Material, identity, -1);
		// 诊断钩子(用户复现):WLD_MATERIAL_SWITCH_TEXTURE=<贴图路径> 在第 30 帧把指定面板的
		// Albedo 切到该贴图;WLD_MATERIAL_SWITCH_PANEL 指定面板(空 = 第一个面板)。
		if (const char* switchTo = std::getenv("WLD_MATERIAL_SWITCH_TEXTURE"))
		{
			const char* targetPanel = std::getenv("WLD_MATERIAL_SWITCH_PANEL");
			const bool matchesPanel = targetPanel == nullptr || *targetPanel == 0 || m_PanelId == targetPanel;
			if (matchesPanel)
			{
				static int switchCountdown = 30;
				if (switchCountdown > 0 && --switchCountdown == 0)
				{
					m_Material->SetAlbedoTexture(switchTo);
					WLD_CORE_INFO("[material-ui] diag switch albedo -> '{0}' panel='{1}' rev={2}",
						switchTo, m_PanelId, m_Material->GetRevision());
				}
			}
		}
		Renderer3D::EndScene();
		command->EndRenderPass();
		// 颜色附件已在 EndRenderPass 由渲染通道隐式转换为 FinalLayout(ShaderReadOnly),
		// 这里不再需要额外的 PipelineBarrier。
		command->End();
		const bool checkGlErrors = std::getenv("WLD_GL_ERRORS") != nullptr;
		if (checkGlErrors)
			Renderer::DrainGLErrors("preview-before-submit");
		Renderer::SubmitScene(command, m_PreviewColor);
		if (checkGlErrors)
			Renderer::DrainGLErrors("preview-after-submit");
		// AI 控制通道的一次性抓图请求(与 WLD_PREVIEW_TEX_CAPTURE 同一条读回路径)。
		if (!m_PendingPreviewCapture.empty())
		{
			if (Renderer::CaptureTexture(m_PendingPreviewCapture, m_PreviewColor,
				m_PreviewTargetW, m_PreviewTargetH))
				WLD_CORE_INFO("[ai] preview capture written: {0}", m_PendingPreviewCapture);
			m_PendingPreviewCapture.clear();
		}
		CapturePreviewTextureSequence();
		Wui::WuiTextureRegistry& registry = Wui::WuiTextureRegistry::Get();
		if (m_PreviewTextureId == 0)
		{
			m_UiTextureGeneration = registry.Generation();
			m_PreviewTextureId = registry.Register(m_PreviewColor);
		}
		else if (registry.Generation() != m_UiTextureGeneration)
		{
			// 注册表整表清空(设备重建)→ 同一槽位换成新句柄。
			m_UiTextureGeneration = registry.Generation();
			registry.Update(m_PreviewTextureId, m_PreviewColor);
		}
		else
		{
			// 尺寸变化只是换了纹理对象:槽位 id 不变,Update 推进内容代让 WUI 后端丢掉旧 set。
			registry.Update(m_PreviewTextureId, m_PreviewColor);
		}
		return m_PreviewTextureId;
	}


void MaterialEditorPanel::CapturePreviewTextureSequence(){
		// 无障碍诊断:把**预览纹理本身**连续写成 PPM(后端无关的 RHI 读回)。
		// 主窗口/独立窗口的整窗抓图在 Vulkan 下抓不到(glReadPixels 路径),而材质预览恰好
		// 只在 Vulkan 下有内容,所以"预览闪不闪"必须能直接读预览纹理:
		//   WLD_PREVIEW_TEX_CAPTURE=<目录>  + WLD_SCREEN_CAPTURE_START/_EVERY/_COUNT
		const char* dir = std::getenv("WLD_PREVIEW_TEX_CAPTURE");
		if (!dir || !*dir)
			return;
		static const int every = std::getenv("WLD_SCREEN_CAPTURE_EVERY")
			? std::atoi(std::getenv("WLD_SCREEN_CAPTURE_EVERY")) : 1;
		static const int start = std::getenv("WLD_SCREEN_CAPTURE_START")
			? std::atoi(std::getenv("WLD_SCREEN_CAPTURE_START")) : 0;
		static const int count = std::getenv("WLD_SCREEN_CAPTURE_COUNT")
			? std::atoi(std::getenv("WLD_SCREEN_CAPTURE_COUNT")) : 60;
		const int step = every > 0 ? every : 1;
		++m_PreviewCaptureFrame;
		if (m_PreviewCaptureFrame < start || m_PreviewCaptureWritten >= count
			|| (m_PreviewCaptureFrame - start) % step != 0)
			return;
		std::string stem;
		for (char c : m_PanelId)
			stem += (std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_') ? c : '_';
		const std::string path = std::string(dir) + "/preview-" + stem + "-"
			+ std::to_string(m_PreviewCaptureWritten) + ".ppm";
		Renderer::CaptureTexture(path, m_PreviewColor, m_PreviewTargetW, m_PreviewTargetH);
		++m_PreviewCaptureWritten;
	}


void MaterialEditorPanel::SaveCurrent(){
		if (!m_Material)
			return;
		// 新建材质走内容浏览器(Create → New Material);这里只保存已落盘的材质。
		std::string path = m_Path.empty() ? m_NewPathBuffer : m_Path;
		// U2d:未落盘材质的"另存为"路径先过行内校验(空 / 非法字符 / 目标已存在)→ 拒绝保存。
		if (m_Path.empty() && !path.empty())
		{
			const std::string pathError = NewMaterialPathError(path);
			if (!pathError.empty())
			{
				m_Status = Wui::Tr("panel.material.status.save_failed", "Save failed: ") + pathError;
				m_StatusIsError = true;
				return;
			}
		}
		if (path.empty())
		{
			m_NewPathAttempted = true;   // 让行内也标出"路径不能为空"
			m_Status = Wui::Tr("panel.material.status.no_path",
				"Save failed: no path (create new materials as .wmat in the Content Browser)");
			m_StatusIsError = true;
			return;
		}
		std::string error;
		if (!MaterialLibrary::Get().Save(m_Material, path, &error))
		{
			m_Status = Wui::Tr("panel.material.status.save_failed", "Save failed: ") + error;
			m_StatusIsError = true;
			return;
		}
		m_Path = m_Material->GetPath();
		SetMaterialPathForPanel(m_Path);
		RefreshMaterialPickIndex();
		// 保存 = 磁盘与内存一致:校验缓存要重算(缺贴图可能刚补上)。
		m_ValidationRevision = 0;
		m_Status = Wui::Tr("panel.material.status.saved", "Saved ") + m_Path;
		m_StatusIsError = false;
	}


void MaterialEditorPanel::FramePreview(){
		const Ref<Mesh>& mesh = PreviewMeshFor(m_PreviewMesh);
		const float radius = mesh ? std::max(0.2f, mesh->GetBounds().GetRadius()) : 1.0f;
		m_FocusDistance = radius * 3.0f;
		m_CameraMinDistance = radius * 0.6f;
		m_CameraMaxDistance = radius * 30.0f;
		m_CameraDistance = std::clamp(m_FocusDistance, m_CameraMinDistance, m_CameraMaxDistance);
	}


void MaterialEditorPanel::RevealMaterialOnDisk(){
		if (m_Path.empty())
		{
			m_Status = Wui::Tr("panel.material.status.reveal_failed",
				"Reveal failed: this material has never been saved to disk");
			m_StatusIsError = true;
			return;
		}
		const std::filesystem::path diskPath = ContentRootPath() / m_Path;
		std::error_code ec;
		const bool exists = std::filesystem::exists(diskPath, ec);
		if (!exists)
		{
			m_Status = Wui::Tr("panel.material.status.reveal_missing",
				"Reveal failed: the .wmat is not on disk yet (save it first)");
			m_StatusIsError = true;
			return;
		}
#ifdef _WIN32
		// 与内容浏览器"Show in Explorer"同一条系统调用(/select 打开所在文件夹并选中该项)。
		const std::wstring parameters = L"/select,\"" + std::filesystem::absolute(diskPath).wstring() + L"\"";
		const HINSTANCE result = ShellExecuteW(nullptr, L"open", L"explorer.exe", parameters.c_str(),
			nullptr, SW_SHOWNORMAL);
		if (reinterpret_cast<intptr_t>(result) <= 32)
		{
			m_Status = Wui::Tr("panel.material.status.reveal_failed", "Reveal failed");
			m_StatusIsError = true;
			return;
		}
		m_Status = Wui::Tr("panel.material.status.revealed", "Revealed in Explorer: ") + m_Path;
		m_StatusIsError = false;
#else
		m_Status = Wui::Tr("panel.material.status.reveal_unsupported",
			"Reveal is only implemented on Windows");
		m_StatusIsError = true;
#endif
	}

}
