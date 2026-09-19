#include "wldpch.h"
#include "MaterialEditorPanel.h"

#include "World/Renderer/ProjectionConventions.h"
#include "World/Renderer/Renderer.h"
#include "World/Renderer/Renderer3D.h"
#include "World/Renderer/RenderSettings.h"
#include "World/WUI/WuiTextureRegistry.h"
#include "World/WUI/Widgets/WuiChrome.h"
#include "World/WUI/WuiWidgets.h"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <sstream>

namespace World
{
	namespace
	{
		constexpr float kPi = 3.14159265358979323846f;

		std::string ShortenPath(const std::string& path)
		{
			if (path.size() <= 46)
				return path;
			return "…" + path.substr(path.size() - 45);
		}

		// 扫描内容根下的贴图资产(下拉选择用);按扩展名白名单过滤。
		std::vector<std::string> ScanTextureCatalog()
		{
			std::vector<std::string> paths;
			std::error_code ec;
			const std::filesystem::path root = std::filesystem::path(std::string(WLD_GAME_DIR)) / "assets";
			if (!std::filesystem::exists(root, ec))
				return paths;
			for (const std::filesystem::directory_entry& entry :
				std::filesystem::recursive_directory_iterator(root,
					std::filesystem::directory_options::skip_permission_denied, ec))
			{
				if (!entry.is_regular_file(ec))
					continue;
				std::string extension = entry.path().extension().string();
				std::transform(extension.begin(), extension.end(), extension.begin(),
					[](unsigned char c) { return static_cast<char>(std::tolower(c)); });
				if (extension != ".png" && extension != ".jpg" && extension != ".jpeg" && extension != ".tga")
					continue;
				const std::filesystem::path relative = std::filesystem::relative(entry.path(), root, ec);
				if (!ec)
					paths.push_back(MaterialLibrary::NormalizePath(relative.generic_string()));
			}
			std::sort(paths.begin(), paths.end());
			return paths;
		}
	}

	MaterialEditorPanel::MaterialEditorPanel()
		: MaterialEditorPanel(std::string())
	{
	}

	MaterialEditorPanel::MaterialEditorPanel(std::string materialPath)
	{
		// 预览资源属于当前设备:设备释放(后端切换/关闭)前必须把句柄放掉,
		// 否则会在设备之后析构(独立窗口崩溃那次的同类问题)。
		Renderer::RegisterDeviceReleaseHook(this, [this] { ReleaseGpuResources(); });
		SetMaterialPathForPanel(materialPath);
	}

	void MaterialEditorPanel::SetMaterialPathForPanel(const std::string& path)
	{
		const std::string key = path.empty() ? std::string("(unsaved)") : MaterialLibrary::NormalizePath(path);
		m_PanelId = "material:" + key;
		std::filesystem::path file(key);
		std::string name = file.stem().string();
		if (name.empty())
			name = "Material";
		m_PanelTitle = "Material - " + name;
	}

	MaterialEditorPanel::~MaterialEditorPanel()
	{
		Renderer::UnregisterDeviceReleaseHook(this);
		ReleaseGpuResources();
	}

	void MaterialEditorPanel::OpenMaterial(const std::string& path)
	{
		std::string error;
		Ref<Material> material = MaterialLibrary::Get().Load(path, &error);
		if (!material)
		{
			m_Status = "加载失败: " + error;
			m_StatusIsError = true;
			return;
		}
		// 已有未保存改动时换目标:提示并保留原材质(不自动丢弃用户改动)。
		if (m_Material && m_Material->IsDirty() && m_Material->GetPath() != path)
		{
			m_Status = "当前材质有未保存改动(已切到新材质,原改动保留在内存)";
			m_StatusIsError = true;
		}
		else
		{
			m_Status = "已打开 " + path;
			m_StatusIsError = false;
		}
		m_Material = material;
		m_Path = material->GetPath();
		SetMaterialPathForPanel(m_Path);
		RefreshCatalog();
		RefreshPickerIndices();
		WLD_CORE_INFO("[material-ui] opened material '{0}'", m_Path);
	}

	void MaterialEditorPanel::RefreshCatalog()
	{
		const double now = std::chrono::duration<double>(
			std::chrono::steady_clock::now().time_since_epoch()).count();
		if (now - m_CatalogRefreshTime < 1.5)
			return;
		m_CatalogRefreshTime = now;
		m_MaterialPaths = MaterialLibrary::Get().ScanMaterials();
		m_TexturePaths = ScanTextureCatalog();
		RefreshPickerIndices();
	}

	void MaterialEditorPanel::RefreshPickerIndices()
	{
		if (!m_Material)
			return;
		const MaterialDesc& desc = m_Material->GetDesc();
		m_MaterialPickIndex = -1;
		for (size_t i = 0; i < m_MaterialPaths.size(); ++i)
			if (m_MaterialPaths[i] == m_Path)
				m_MaterialPickIndex = static_cast<int>(i);
		// 贴图下拉的选项是 "(无)" + m_TexturePaths,所以选中索引要 **+1** 对齐;
		// 之前少了这个偏移,选了一张贴图后下一帧索引回算成 0/错位 → 显示/生效成另一张。
		m_AlbedoPickIndex = 0;
		for (size_t i = 0; i < m_TexturePaths.size(); ++i)
			if (m_TexturePaths[i] == desc.AlbedoTexture)
				m_AlbedoPickIndex = static_cast<int>(i) + 1;
		m_NormalPickIndex = 0;
		for (size_t i = 0; i < m_TexturePaths.size(); ++i)
			if (m_TexturePaths[i] == desc.NormalTexture)
				m_NormalPickIndex = static_cast<int>(i) + 1;
	}

	void MaterialEditorPanel::ReleaseGpuResources()
	{
		m_PreviewPass = nullptr;
		m_PreviewFramebuffer = nullptr;
		m_PreviewColor = nullptr;
		m_PreviewEntityId = nullptr;
		m_PreviewDepth = nullptr;
		m_PreviewColorMsaa = nullptr;
		m_PreviewEntityMsaa = nullptr;
		m_PreviewDepthMsaa = nullptr;
		m_PreviewCommandBuffer = nullptr;
		m_PreviewCameraBuffer = nullptr;
		m_PreviewCameraSet = nullptr;
		m_PreviewTextureId = 0;
		m_GpuDevice = nullptr;
	}

	void MaterialEditorPanel::EnsureGpuResources()
	{
		Rhi::Handle<Rhi::Device> device = Renderer::GetDevice();
		if (!device)
			return;
		if (m_GpuDevice == device.get() && m_PreviewFramebuffer)
			return;
		ReleaseGpuResources();
		m_GpuDevice = device.get();

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
		colorDesc.Extent = { m_PreviewSize, m_PreviewSize, 1 };
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
		subpass.ColorAttachments = { { 0, Rhi::AttachmentLayout::ColorAttachment }, { 1, Rhi::AttachmentLayout::ColorAttachment } };
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
		framebufferDesc.Extent = { m_PreviewSize, m_PreviewSize };
		// 顺序必须与渲染通道附件表 1:1(Vulkan 要求 framebuffer 附件数/顺序与通道一致)。
		if (multisampled)
			framebufferDesc.Attachments = { m_PreviewColorMsaa, m_PreviewEntityMsaa, m_PreviewDepthMsaa,
				m_PreviewColor, m_PreviewEntityId };
		else
			framebufferDesc.Attachments = { m_PreviewColor, m_PreviewEntityId, m_PreviewDepth };
		framebufferDesc.DebugName = "Material.PreviewFramebuffer";
		m_PreviewFramebuffer = device->CreateFramebuffer(framebufferDesc);

		m_PreviewCommandBuffer = device->CreateCommandBuffer("Material.Preview");

		Rhi::BufferDesc cameraDesc;
		cameraDesc.Size = sizeof(glm::mat4);
		cameraDesc.Usage = Rhi::BufferUsageUniform;
		cameraDesc.Memory = Rhi::MemoryHint::HostVisible;
		cameraDesc.DebugName = "Material.Preview.Camera";
		m_PreviewCameraBuffer = device->CreateBuffer(cameraDesc);
		m_PreviewCameraSet = device->CreateDescriptorSet(Renderer::GetGlobalDescriptorSetLayout());
		if (m_PreviewCameraSet)
		{
			// P1b D4:set0 现在还有 binding 2(灯光 UBO)与 binding 3(阴影贴图),3D 管线静态使用它们;
			// 必须与相机(binding 0)**同一次 Update** 写完 —— GL 后端的 Update 是整体替换语义,
			// 分两次写会把相机绑定冲掉(实测 Vulkan 预览球体几乎全黑)。
			std::vector<Rhi::DescriptorWrite> writes;
			Rhi::DescriptorWrite camera;
			camera.Binding = 0;
			camera.Type = Rhi::DescriptorType::UniformBuffer;
			camera.Buffer = m_PreviewCameraBuffer;
			writes.push_back(camera);
			for (Rhi::DescriptorWrite& lighting : Renderer3D::MakeGlobalLightingWrites(nullptr))
				writes.push_back(std::move(lighting));
			m_PreviewCameraSet->Update(writes);
		}

		if (!m_PreviewSphere)
			m_PreviewSphere = Mesh::CreateUnitSphere(2.0f, 48, 24);
	}

	uint64_t MaterialEditorPanel::RenderPreview()
	{
		if (!m_Material)
			return 0;
		EnsureGpuResources();
		if (!m_PreviewFramebuffer || !m_PreviewCameraSet || !m_PreviewSphere)
			return 0;

		const float distance = m_CameraDistance;
		const glm::vec3 eye {
			distance * std::cos(m_OrbitPitch) * std::sin(m_OrbitYaw),
			distance * std::sin(m_OrbitPitch),
			distance * std::cos(m_OrbitPitch) * std::cos(m_OrbitYaw) };
		const glm::mat4 view = glm::lookAt(eye, glm::vec3(0.0f), glm::vec3(0.0f, 1.0f, 0.0f));
		const glm::mat4 projection = glm::perspective(glm::radians(35.0f), 1.0f, 0.05f, 20.0f);
		// 与场景同一条投影适配(离屏不做 Y 翻转,只补 Vulkan 的深度范围)。
		const glm::mat4 viewProjection = AdaptViewProjectionForOffscreen(projection * view,
			Renderer::GetBackendName() == "vulkan");
		m_PreviewCameraBuffer->SetData(&viewProjection, sizeof(glm::mat4));

		std::vector<Rhi::ClearValue> clears(3);
		clears[0].Color = { 0.12f, 0.13f, 0.15f, 1.0f };
		const int minusOne = -1;
		std::memcpy(&clears[1].Color, &minusOne, sizeof(int));
		clears[2].IsDepthStencil = true;
		clears[2].DepthStencil.Depth = 1.0f;

		m_PreviewCommandBuffer->Begin();
		m_PreviewCommandBuffer->BeginRenderPass(m_PreviewPass, m_PreviewFramebuffer, clears);
		m_PreviewCommandBuffer->SetViewport({ 0, 0, static_cast<float>(m_PreviewSize), static_cast<float>(m_PreviewSize) });
		m_PreviewCommandBuffer->SetScissor({ 0, 0, m_PreviewSize, m_PreviewSize });
		m_PreviewCommandBuffer->BindDescriptorSet(m_PreviewCameraSet, 0);
		Renderer3D::BeginScene(viewProjection, m_PreviewCommandBuffer);
		// 预览用**固定对象槽位**(按面板身份映射):否则每个面板/主场景都从序号 0 开始分配,
		// 会争用同一份对象 UBO 与材质描述符集,两个内容不同的材质面板就会逐帧互相覆盖
		// (用户实测:预览一直闪烁)。
		const uint32_t slotBase = Renderer3D::ReserveSlotBase(
			static_cast<uint32_t>(Wui::HashId(m_PanelId.c_str()) ^ 0x9E37u));
		const uint32_t previewIndex = Renderer3D::SubmitAtSlot(slotBase, m_PreviewSphere, m_Material, glm::mat4(1.0f), -1);
		(void)previewIndex;
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
		if (std::getenv("WLD_TRACE_3D"))
		{
			static int tracedPreviewSlots = 0;
			if (tracedPreviewSlots < 24)
			{
				tracedPreviewSlots++;
				WLD_CORE_INFO("[material-ui] preview slot panel='{0}' base={1} objectIndex={2}",
					m_PanelId, slotBase, previewIndex);
			}
		}
		Renderer3D::EndScene();
		m_PreviewCommandBuffer->EndRenderPass();
		// 颜色附件已在 EndRenderPass 由渲染通道隐式转换为 FinalLayout(ShaderReadOnly),
		// 这里不再需要额外的 PipelineBarrier。
		m_PreviewCommandBuffer->End();
		const bool checkGlErrors = std::getenv("WLD_GL_ERRORS") != nullptr;
		if (checkGlErrors)
			Renderer::DrainGLErrors("preview-before-submit");
		Renderer::SubmitScene(m_PreviewCommandBuffer, m_PreviewColor);
		if (checkGlErrors)
			Renderer::DrainGLErrors("preview-after-submit");
		// AI 控制通道的一次性抓图请求(与 WLD_PREVIEW_TEX_CAPTURE 同一条读回路径)。
		if (!m_PendingPreviewCapture.empty())
		{
			if (Renderer::CaptureTexture(m_PendingPreviewCapture, m_PreviewColor, m_PreviewSize, m_PreviewSize))
				WLD_CORE_INFO("[ai] preview capture written: {0}", m_PendingPreviewCapture);
			m_PendingPreviewCapture.clear();
		}
		CapturePreviewTextureSequence();
		Wui::WuiTextureRegistry& registry = Wui::WuiTextureRegistry::Get();
		if (m_PreviewTextureId == 0 || registry.Generation() != m_UiTextureGeneration)
		{
			m_UiTextureGeneration = registry.Generation();
			if (m_PreviewTextureId == 0)
				m_PreviewTextureId = registry.Register(m_PreviewColor);
			else
				registry.Update(m_PreviewTextureId, m_PreviewColor);
		}
		return m_PreviewTextureId;
	}

	void MaterialEditorPanel::CapturePreviewTextureSequence()
	{
		// 无障碍诊断:把**预览纹理本身**连续写成 PPM(后端无关的 RHI 读回)。
		// 主窗口/独立窗口的整窗抓图在 Vulkan 下抓不到(glReadPixels 路径),而材质预览恰好
		// 只在 Vulkan 下有内容,所以"预览闪不闪"必须能直接读预览纹理:
		//   WLD_PREVIEW_TEX_CAPTURE=<目录>  + WLD_SCREEN_CAPTURE_START/_EVERY/_COUNT
		const char* dir = std::getenv("WLD_PREVIEW_TEX_CAPTURE");
		if (!dir || !*dir)
			return;
		static const int every = std::getenv("WLD_SCREEN_CAPTURE_EVERY") ? std::atoi(std::getenv("WLD_SCREEN_CAPTURE_EVERY")) : 1;
		static const int start = std::getenv("WLD_SCREEN_CAPTURE_START") ? std::atoi(std::getenv("WLD_SCREEN_CAPTURE_START")) : 0;
		static const int count = std::getenv("WLD_SCREEN_CAPTURE_COUNT") ? std::atoi(std::getenv("WLD_SCREEN_CAPTURE_COUNT")) : 60;
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
		Renderer::CaptureTexture(path, m_PreviewColor, m_PreviewSize, m_PreviewSize);
		++m_PreviewCaptureWritten;
	}

	void MaterialEditorPanel::SaveCurrent()
	{
		if (!m_Material)
			return;
		// 新建材质走内容浏览器(Create → New Material);这里只保存已落盘的材质。
		std::string path = m_Path.empty() ? m_NewPathBuffer : m_Path;
		if (path.empty())
		{
			m_Status = "保存失败: 未指定路径(新建材质请在内容浏览器里创建 .wmat)";
			m_StatusIsError = true;
			return;
		}
		std::string error;
		if (!MaterialLibrary::Get().Save(m_Material, path, &error))
		{
			m_Status = "保存失败: " + error;
			m_StatusIsError = true;
			return;
		}
		m_Path = m_Material->GetPath();
		SetMaterialPathForPanel(m_Path);
		RefreshPickerIndices();
		m_Status = "已保存 " + m_Path;
		m_StatusIsError = false;
	}

	void MaterialEditorPanel::DrawToolbar(Wui::WuiContext& ctx, const Wui::WuiRect& rect, PanelHost& host)
	{
		const Wui::WuiTheme& theme = host.Theme();
		// 材质/贴图下拉列表:节流刷新(新资产 ~1.5s 内出现在列表里)。
		if (m_Material)
			RefreshCatalog();
		const float x = rect.X + 10.0f;
		float y = rect.Y + 6.0f;
		const float buttonW = 86.0f;
		const float gap = 8.0f;

		// 新建材质在内容浏览器(右键 → New Material)完成;面板只负责编辑与保存。
		if (Wui::Button(ctx, Wui::HashId("material.save"), { x, y, buttonW, 22.0f }, "Save", theme))
			SaveCurrent();
		if (Wui::Button(ctx, Wui::HashId("material.revert"), { x + (buttonW + gap), y, buttonW, 22.0f }, "Revert", theme))
		{
			if (m_Path.empty())
			{
				m_Status = "Revert 失败: 尚未落盘的材质没有可回退的磁盘版本";
				m_StatusIsError = true;
			}
			else
			{
				std::string error;
				if (MaterialLibrary::Get().Reload(m_Path, &error))
				{
					RefreshPickerIndices();
					m_Status = "已从磁盘重载 " + m_Path;
					m_StatusIsError = false;
				}
				else
				{
					m_Status = "重载失败: " + error;
					m_StatusIsError = true;
				}
			}
		}

		y += 30.0f;
		if (m_Path.empty())
		{
			Wui::Label(ctx, { x, y }, "尚未落盘:请在内容浏览器里新建材质", theme.TextMuted, 12.0f);
			y += 16.0f;
			const Wui::WuiRect field { x, y, rect.W - 20.0f, 22.0f };
			Wui::TextField(ctx, Wui::HashId("material.newpath"), field, m_NewPathBuffer, theme);
			y += 28.0f;
		}
	}

	void MaterialEditorPanel::DrawParameters(Wui::WuiContext& ctx, const Wui::WuiRect& rect, PanelHost& host)
	{
		const Wui::WuiTheme& theme = host.Theme();
		if (!m_Material)
		{
			Wui::Label(ctx, { rect.X + 10.0f, rect.Y + 8.0f },
				"未打开材质:在内容浏览器双击 .wmat 文件", theme.TextMuted, 13.0f);
			return;
		}

		// 只读快照:编辑控件写各自局部变量,再经 Material 的 setter 落回实例
		// (setter 会自增 Revision,渲染侧缓存才会失效 → 预览即时更新)。
		const MaterialDesc& desc = m_Material->GetDesc();
		const float x = rect.X + 10.0f;
		const float width = rect.W - 20.0f;
		float y = rect.Y + 6.0f;

		// W5-L1:本帧任何参数写入 = "未保存修改"(以 Revision 是否前进为准,避免每帧误标)。
		// 资产热重载对 dirty 材质只报告不覆盖(SkippedDirty),不能静默丢掉面板里的改动。
		const auto applyEdit = [this](auto&& setter)
		{
			const uint32_t revision = m_Material->GetRevision();
			setter();
			if (m_Material->GetRevision() != revision)
				m_Material->MarkDirty(true);
		};

		Wui::Label(ctx, { x, y }, ShortenPath(m_Path.empty() ? "(未保存的新材质)" : m_Path)
			+ (m_Material->IsDirty() ? "  *" : ""), theme.Text, 13.0f);
		y += 20.0f;

		// ---- 颜色/标量 ----
		Wui::Label(ctx, { x, y }, "BaseColor (sRGB)", theme.TextMuted, 12.0f);
		y += 16.0f;
		const float colorW = (width - 3 * 4.0f) / 4.0f;
		glm::vec4 baseColor = desc.BaseColor;
		bool colorChanged = false;
		colorChanged |= Wui::DragFloat(ctx, Wui::HashId("material.base.r"), { x + 0 * (colorW + 4.0f), y, colorW, 20.0f }, baseColor.r, 0.01f, 0.0f, 1.0f, theme);
		colorChanged |= Wui::DragFloat(ctx, Wui::HashId("material.base.g"), { x + 1 * (colorW + 4.0f), y, colorW, 20.0f }, baseColor.g, 0.01f, 0.0f, 1.0f, theme);
		colorChanged |= Wui::DragFloat(ctx, Wui::HashId("material.base.b"), { x + 2 * (colorW + 4.0f), y, colorW, 20.0f }, baseColor.b, 0.01f, 0.0f, 1.0f, theme);
		colorChanged |= Wui::DragFloat(ctx, Wui::HashId("material.base.a"), { x + 3 * (colorW + 4.0f), y, colorW, 20.0f }, baseColor.a, 0.01f, 0.0f, 1.0f, theme);
		y += 26.0f;
		if (colorChanged)
			applyEdit([&] { m_Material->SetBaseColor(baseColor); });

		float metallic = desc.Metallic;
		Wui::Label(ctx, { x, y }, "Metallic", theme.TextMuted, 12.0f);
		Wui::SliderFloat(ctx, Wui::HashId("material.metallic"), { x + 70.0f, y - 2.0f, width - 70.0f, 18.0f }, metallic, 0.0f, 1.0f, theme);
		y += 24.0f;
		applyEdit([&] { m_Material->SetMetallic(metallic); });

		float roughness = desc.Roughness;
		Wui::Label(ctx, { x, y }, "Roughness", theme.TextMuted, 12.0f);
		Wui::SliderFloat(ctx, Wui::HashId("material.roughness"), { x + 70.0f, y - 2.0f, width - 70.0f, 18.0f }, roughness, 0.02f, 1.0f, theme);
		y += 24.0f;
		applyEdit([&] { m_Material->SetRoughness(roughness); });

		Wui::Label(ctx, { x, y }, "Emissive", theme.TextMuted, 12.0f);
		y += 16.0f;
		glm::vec3 emissive = desc.Emissive;
		bool emissiveChanged = false;
		const float emissiveW = (width - 2 * 4.0f) / 3.0f;
		emissiveChanged |= Wui::DragFloat(ctx, Wui::HashId("material.emissive.r"), { x + 0 * (emissiveW + 4.0f), y, emissiveW, 20.0f }, emissive.x, 0.01f, 0.0f, 8.0f, theme);
		emissiveChanged |= Wui::DragFloat(ctx, Wui::HashId("material.emissive.g"), { x + 1 * (emissiveW + 4.0f), y, emissiveW, 20.0f }, emissive.y, 0.01f, 0.0f, 8.0f, theme);
		emissiveChanged |= Wui::DragFloat(ctx, Wui::HashId("material.emissive.b"), { x + 2 * (emissiveW + 4.0f), y, emissiveW, 20.0f }, emissive.z, 0.01f, 0.0f, 8.0f, theme);
		y += 26.0f;
		if (emissiveChanged)
			applyEdit([&] { m_Material->SetEmissive(emissive); });

		// ---- 混合 / 双面 ----
		static const std::vector<std::string> blendModes { "Opaque", "Transparent" };
		int blendIndex = desc.BlendMode == MaterialBlendMode::Transparent ? 1 : 0;
		Wui::Label(ctx, { x, y }, "BlendMode", theme.TextMuted, 12.0f);
		if (Wui::Combo(ctx, Wui::HashId("material.blend"), { x + 70.0f, y - 4.0f, 140.0f, 20.0f }, blendModes[blendIndex], blendModes, blendIndex, theme))
			applyEdit([&] { m_Material->SetBlendMode(blendIndex == 1 ? MaterialBlendMode::Transparent : MaterialBlendMode::Opaque); });
		y += 26.0f;

		bool doubleSided = desc.DoubleSided;
		if (Wui::Checkbox(ctx, Wui::HashId("material.doublesided"), { x, y, 120.0f, 18.0f }, "Double Sided", doubleSided, theme))
			applyEdit([&] { m_Material->SetDoubleSided(doubleSided); });
		y += 26.0f;

		// 注:面板不提供"切换材质"入口 —— 从内容浏览器/菜单打开哪个材质,这个窗口就是
		// 哪个材质的编辑器(用户 2026-09-16 明确要求)。

		// ---- 贴图槽(可搜索下拉) ----
		std::vector<std::string> albedoOptions = m_TexturePaths;
		albedoOptions.insert(albedoOptions.begin(), "(无)");
		Wui::Label(ctx, { x, y }, "Albedo 贴图 (sRGB)", theme.TextMuted, 12.0f);
		y += 16.0f;
		if (Wui::SearchableCombo(ctx, Wui::HashId("material.albedo"), { x, y, width, 22.0f }, "",
			albedoOptions, m_AlbedoPickIndex, theme))
		{
			// 只在**真的换了一张**时才写回材质:否则每帧调用会让 Revision 每帧 +1,
			// 渲染侧每帧重建材质描述符集 → 预览逐帧闪(用户反馈"切换贴图后预览闪烁")。
			const std::string chosen = m_AlbedoPickIndex <= 0 ? std::string() : albedoOptions[m_AlbedoPickIndex];
			if (chosen != m_Material->GetDesc().AlbedoTexture)
				applyEdit([&] { m_Material->SetAlbedoTexture(chosen); });
		}
		y += 28.0f;
		std::vector<std::string> normalOptions = m_TexturePaths;
		normalOptions.insert(normalOptions.begin(), "(无)");
		Wui::Label(ctx, { x, y }, "Normal 贴图 (线性)", theme.TextMuted, 12.0f);
		y += 16.0f;
		if (Wui::SearchableCombo(ctx, Wui::HashId("material.normal"), { x, y, width, 22.0f }, "",
			normalOptions, m_NormalPickIndex, theme))
		{
			const std::string chosen = m_NormalPickIndex <= 0 ? std::string() : normalOptions[m_NormalPickIndex];
			if (chosen != m_Material->GetDesc().NormalTexture)
				applyEdit([&] { m_Material->SetNormalTexture(chosen); });
		}
		y += 28.0f;

		// ---- 状态行 ----
		if (!m_Status.empty())
			Wui::Label(ctx, { x, y }, m_Status, m_StatusIsError ? Wui::WuiColor { 1.0f, 0.45f, 0.4f, 1.0f } : theme.TextMuted, 12.0f);
	}

	void MaterialEditorPanel::OnRender(Wui::WuiContext& ctx, const Wui::WuiRect& rect, PanelHost& host)
	{
		if (std::getenv("WLD_TRACE_3D"))
		{
			static int tracedPanel = 0;
			if (tracedPanel < 16)
			{
				tracedPanel++;
				WLD_CORE_INFO("[material-ui] panel OnRender panel='{0}' rect=({1},{2},{3},{4}) material={5}",
					m_PanelId, rect.X, rect.Y, rect.W, rect.H, static_cast<int>(m_Material ? 1 : 0));
			}
		}
		const Wui::WuiTheme& theme = host.Theme();

		// 左列 = 参数/文件;右列 = 预览球。窄面板时退化为上下布局。
		const bool wide = rect.W >= 560.0f;
		Wui::WuiRect parameterRect = rect;
		Wui::WuiRect previewRect = rect;
		if (wide)
		{
			const float previewW = std::min(300.0f, rect.W * 0.45f);
			previewRect = { rect.X + rect.W - previewW - 10.0f, rect.Y + 8.0f, previewW, previewW };
			parameterRect.W = rect.W - previewW - 30.0f;
		}
		else
		{
			previewRect = { rect.X + 10.0f, rect.Y + 8.0f, rect.W - 20.0f, rect.W - 20.0f };
			parameterRect = { rect.X, rect.Y + previewRect.H + 16.0f, rect.W, rect.H - previewRect.H - 24.0f };
		}

		// 预览:每帧重渲染(参数改动即时可见)。面板不可见时 EditorShell 不会调用本函数,
		// 因此没有隐藏面板的额外开销。
		const uint64_t textureId = RenderPreview();
		if (textureId != 0)
		{
			Wui::Image(ctx, previewRect, textureId, { 0, 0, 1, 1 }, theme);

			// 左键拖拽旋转预览相机(与 3D 视口一致的直觉:拖拽转物体)。
			const bool hovered = ctx.IsHovered(previewRect);
			// 滚轮缩放(镜头远近):距离越小越近;越近步长越小,便于微调。
			if (hovered && ctx.Input().Wheel != 0.0f)
			{
				const float step = std::max(0.1f, m_CameraDistance * 0.1f);
				m_CameraDistance = std::clamp(m_CameraDistance - ctx.Input().Wheel * step, 1.6f, 20.0f);
				WLD_CORE_INFO("[material-ui] preview zoom distance={0}", m_CameraDistance);
			}
			if (hovered && ctx.Input().MouseClicked[0])
			{
				m_Orbiting = true;
				m_LastMouse = ctx.Input().MousePos;
			}
			if (m_Orbiting && ctx.Input().MouseDown[0])
			{
				const glm::vec2 delta = ctx.Input().MousePos - m_LastMouse;
				m_LastMouse = ctx.Input().MousePos;
				m_OrbitYaw -= delta.x * 0.01f;
				m_OrbitPitch = std::clamp(m_OrbitPitch + delta.y * 0.01f, -1.45f, 1.45f);
			}
			if (m_Orbiting && !ctx.Input().MouseDown[0])
				m_Orbiting = false;
		}
		else
		{
			Wui::Label(ctx, { previewRect.X + 10.0f, previewRect.Y + 10.0f }, "预览不可用(RHI 设备未就绪)", theme.TextMuted, 12.0f);
		}

		DrawToolbar(ctx, parameterRect, host);
		DrawParameters(ctx, { parameterRect.X, parameterRect.Y + 56.0f, parameterRect.W, parameterRect.H - 56.0f }, host);
	}
}
