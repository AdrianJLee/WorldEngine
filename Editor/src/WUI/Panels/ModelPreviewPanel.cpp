#include "wldpch.h"
#include "ModelPreviewPanel.h"

#include "World/Renderer/MaterialLibrary.h"
#include "World/Renderer/ProjectionConventions.h"
#include "World/Renderer/Renderer.h"
#include "World/Renderer/Renderer3D.h"
#include "World/Renderer/RenderSettings.h"
#include "World/Renderer/AnimationSystem.h"
#include "World/Renderer/AssetHotReload.h"
#include "World/Core/Asset/GltfImporter.h"
#include "World/WUI/WuiLocalization.h"
#include "World/WUI/WuiTextureRegistry.h"
#include "World/WUI/WuiAccessibility.h"
#include "World/WUI/Widgets/WuiChrome.h"
#include "World/WUI/WuiWidgets.h"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <iomanip>
#include <sstream>

namespace World
{
	namespace
	{
		std::string ShortenPath(const std::string& path)
		{
			if (path.size() <= 52)
				return path;
			return "…" + path.substr(path.size() - 51);
		}

		std::string ModelStem(const std::string& logicalPath)
		{
			std::string stem;
			for (const char c : std::filesystem::path(logicalPath).stem().string())
				stem += (std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_') ? c : '_';
			return stem.empty() ? "model" : stem;
		}
	}

	ModelPreviewPanel::ModelPreviewPanel(std::string logicalPath)
	{
		// 预览资源属于当前设备:设备释放(后端切换/关闭)前必须把句柄放掉,
		// 否则会在设备之后析构(独立窗口崩溃那次的同类问题;实测漏掉会 0xC0000005)。
		Renderer::RegisterDeviceReleaseHook(this, [this] { ReleaseGpuResources(); });
		m_LogicalPath = std::move(logicalPath);
		std::replace(m_LogicalPath.begin(), m_LogicalPath.end(), '\\', '/');
		m_PanelId = "model:" + m_LogicalPath;
		const std::string name = std::filesystem::path(m_LogicalPath).filename().string();
		m_PanelTitle = "Model - " + (name.empty() ? m_LogicalPath : name);
		Reload();
	}

	ModelPreviewPanel::~ModelPreviewPanel()
	{
		Renderer::UnregisterDeviceReleaseHook(this);
		ReleaseGpuResources();
	}

	MeshBounds ModelPreviewPanel::ComputeWorldBounds() const
	{
		MeshBounds result;
		if (!m_Mesh)
			return result;
		const std::vector<MeshNode>& nodes = m_Mesh->GetNodes();
		const std::vector<MeshRange>& meshes = m_Mesh->GetMeshes();
		const std::vector<MeshSubmesh>& submeshes = m_Mesh->GetSubmeshes();
		if (nodes.empty() || meshes.empty())
			return m_Mesh->GetBounds();

		std::vector<glm::mat4> world(nodes.size(), glm::mat4(1.0f));
		bool any = false;
		glm::vec3 boundsMin { 0.0f };
		glm::vec3 boundsMax { 0.0f };
		for (size_t index = 0; index < nodes.size(); ++index)
		{
			const MeshNode& node = nodes[index];
			const glm::mat4 local = glm::translate(glm::mat4(1.0f), node.Translation)
				* glm::mat4_cast(node.Rotation)
				* glm::scale(glm::mat4(1.0f), node.Scale);
			world[index] = (node.Parent >= 0 && static_cast<size_t>(node.Parent) < index)
				? world[static_cast<size_t>(node.Parent)] * local : local;
			if (node.MeshIndex < 0 || static_cast<size_t>(node.MeshIndex) >= meshes.size())
				continue;
			const MeshRange& range = meshes[static_cast<size_t>(node.MeshIndex)];
			for (uint32_t sub = 0; sub < range.SubmeshCount; ++sub)
			{
				const uint32_t submeshIndex = range.FirstSubmesh + sub;
				if (submeshIndex >= submeshes.size())
					break;
				const MeshBounds& submeshBounds = submeshes[submeshIndex].Bounds;
				// 8 角点变换求 AABB(旋转下比"中心 + 半径"更紧,取景不会忽远忽近)。
				for (int corner = 0; corner < 8; ++corner)
				{
					const glm::vec3 point {
						(corner & 1) ? submeshBounds.Max.x : submeshBounds.Min.x,
						(corner & 2) ? submeshBounds.Max.y : submeshBounds.Min.y,
						(corner & 4) ? submeshBounds.Max.z : submeshBounds.Min.z };
					const glm::vec3 transformed = glm::vec3(world[index] * glm::vec4(point, 1.0f));
					if (!any)
					{
						boundsMin = boundsMax = transformed;
						any = true;
					}
					else
					{
						boundsMin = glm::min(boundsMin, transformed);
						boundsMax = glm::max(boundsMax, transformed);
					}
				}
			}
		}
		if (!any)
			return m_Mesh->GetBounds();
		result.Min = boundsMin;
		result.Max = boundsMax;
		return result;
	}

	void ModelPreviewPanel::Reload()
	{
		// D5c-4b:重导/重载后清进程级动画模型缓存(含失败标记与 warn 去重),与 Mesh::ClearWModelCache 同款口径。
		AnimationSystem::ClearCache();
		m_AnimTime = 0.0f;
		m_AnimClockValid = false;
		m_TransparentSkipWarned = false;
		m_SkinnedSubmitWarned = false;
		m_SlotMaterials.clear();
		std::string error;
		m_Mesh = Mesh::LoadWModel(m_LogicalPath, &error);
		m_DataValid = Asset::WModelIO::ReadFile(m_LogicalPath, m_Data, &error);
		if (!m_Mesh)
		{
			m_Status = Wui::Tr("panel.model.status.load_failed", "Model load failed: ")
				+ (error.empty() ? m_LogicalPath : error);
			m_StatusIsError = true;
			WLD_CORE_WARN("[model] preview load failed '{0}': {1}", m_LogicalPath, error);
			return;
		}
		// 逐材质槽加载材质(失败保持 null,绘制退化为常量色;不阻塞预览)。
		m_SlotMaterials.resize(m_Mesh->GetMaterialSlots().size());
		for (size_t index = 0; index < m_Mesh->GetMaterialSlots().size(); ++index)
		{
			const std::string& slotPath = m_Mesh->GetMaterialSlots()[index];
			if (!slotPath.empty())
				m_SlotMaterials[index] = MaterialLibrary::Get().Load(slotPath);
		}
		// 相机按**世界空间**包围盒取景:节点树会把局部几何搬到别处(实测夹具 Grandchild 在
		// (1,2,3) 附近),只按局部包围盒取景会把模型放到画面外 → 预览只剩清屏色。
		const MeshBounds bounds = ComputeWorldBounds();
		m_Focus = bounds.GetCenter();
		const float radius = std::max(0.25f, glm::length(bounds.GetExtents()));
		m_MinDistance = radius * 0.6f;
		m_MaxDistance = radius * 40.0f;
		m_CameraDistance = radius * 3.2f;
		WLD_CORE_INFO("[model] preview frame '{0}': worldBounds=({1},{2},{3})..({4},{5},{6}) focus=({7},{8},{9}) radius={10:.3f} dist={11:.3f}",
			m_LogicalPath, m_Focus.x - bounds.GetExtents().x, m_Focus.y - bounds.GetExtents().y,
			m_Focus.z - bounds.GetExtents().z, m_Focus.x + bounds.GetExtents().x,
			m_Focus.y + bounds.GetExtents().y, m_Focus.z + bounds.GetExtents().z,
			m_Focus.x, m_Focus.y, m_Focus.z, radius, m_CameraDistance);
		std::ostringstream text;
		text << Wui::Tr("panel.model.status.loaded", "Loaded ") << m_LogicalPath
			<< Wui::Tr("panel.model.status.stats_nodes", " | nodes ") << m_Mesh->GetNodes().size()
			<< Wui::Tr("panel.model.status.stats_meshes", " / mesh ") << m_Mesh->GetMeshes().size()
			<< " / submesh " << m_Mesh->GetSubmeshes().size()
			<< Wui::Tr("panel.model.status.stats_vertices", " / vertices ") << m_Mesh->GetVertexCount()
			<< Wui::Tr("panel.model.status.stats_indices", " / indices ") << m_Mesh->GetIndexCount();
		m_Status = text.str();
		m_StatusIsError = false;
		ResolveSource();
		RefreshSyncState();
		WLD_CORE_INFO("[model] preview '{0}': nodes={1} meshes={2} submeshes={3} vertices={4}",
			m_LogicalPath, m_Mesh->GetNodes().size(), m_Mesh->GetMeshes().size(),
			m_Mesh->GetSubmeshes().size(), m_Mesh->GetVertexCount());
	}

	void ModelPreviewPanel::ResolveSource()
	{
		m_SourceLogical.clear();
		m_SourceExists = false;
		m_SettingsLoaded = false;
		// 首选:`.wmodel` meta 里记录的源逻辑路径(导入产物自报家门,不靠猜)。
		if (m_DataValid && !m_Data.Meta.SourcePath.empty())
		{
			const AssetFingerprint fingerprint = FingerprintAsset(m_Data.Meta.SourcePath, nullptr);
			if (fingerprint.Exists)
			{
				m_SourceLogical = m_Data.Meta.SourcePath;
				m_SourceExists = true;
			}
		}
		// 兜底:旧产物没有 SourcePath 时按"同目录同名 .gltf/.glb"探测。
		std::filesystem::path modelPath(m_LogicalPath);
		for (const char* extension : { ".gltf", ".glb" })
		{
			if (m_SourceExists)
				break;
			std::filesystem::path candidate = modelPath;
			candidate.replace_extension(extension);
			const std::string logical = candidate.generic_string();
			std::string error;
			const AssetFingerprint fingerprint = FingerprintAsset(logical, &error);
			if (fingerprint.Exists)
			{
				m_SourceLogical = logical;
				m_SourceExists = true;
				break;
			}
		}
		if (m_SourceExists)
		{
			std::string settingsWarning;
			m_Settings = Asset::ModelImportSettings::Load(
				(std::filesystem::path(WLD_ASSETPATH) / m_SourceLogical).string(), &settingsWarning);
			m_SettingsLoaded = true;
			std::filesystem::path sidecar(m_SourceLogical);
			sidecar.replace_extension(".wimport");
			const AssetFingerprint fingerprint = FingerprintAsset(sidecar.generic_string(), nullptr);
			m_SettingsFileFingerprint = fingerprint.Exists ? fingerprint.Value : 0;
		}
	}

	void ModelPreviewPanel::RefreshSyncState()
	{
		m_NeedsReimport = false;
		m_SyncDetail.clear();
		if (!m_DataValid)
		{
			m_SyncDetail = Wui::Tr("panel.model.sync.meta_unreadable", "Cannot read .wmodel metadata");
			m_NeedsReimport = true;
			return;
		}
		if (!m_SourceExists)
		{
			m_SyncDetail = Wui::Tr("panel.model.sync.source_missing",
				"Source file not found (same name .gltf/.glb in the same folder)");
			return;   // 没有源 = 无法重导,但也谈不上"过时"
		}
		const AssetFingerprint fingerprint = FingerprintAsset(m_SourceLogical, nullptr);
		if (fingerprint.FromContent && fingerprint.Value != m_Data.Meta.SourceFingerprint)
		{
			m_NeedsReimport = true;
			m_SyncDetail = Wui::Tr("panel.model.sync.source_changed", "Source file changed");
			return;
		}
		// `.wimport` 被外部改动 → 重新读设置(用户正在编辑但未保存的字段以文件为准)。
		std::filesystem::path sidecar(m_SourceLogical);
		sidecar.replace_extension(".wimport");
		const AssetFingerprint settingsFingerprint = FingerprintAsset(sidecar.generic_string(), nullptr);
		if (settingsFingerprint.Exists && settingsFingerprint.Value != m_SettingsFileFingerprint)
		{
			m_Settings = Asset::ModelImportSettings::Load(
				(std::filesystem::path(WLD_ASSETPATH) / m_SourceLogical).string(), nullptr);
			m_SettingsLoaded = true;
			m_SettingsFileFingerprint = settingsFingerprint.Value;
		}
		if (m_SettingsLoaded)
		{
			const uint64_t settingsHash = Asset::ModelImportSettings::Hash(m_Settings);
			if (settingsHash != m_Data.Meta.SettingsHash)
			{
				m_NeedsReimport = true;
				m_SyncDetail = Wui::Tr("panel.model.sync.settings_changed", "Import settings changed");
				return;
			}
		}
		if (m_Data.Meta.ImporterVersion != 1u)
		{
			m_NeedsReimport = true;
			m_SyncDetail = Wui::Tr("panel.model.sync.importer_upgraded", "Importer version upgraded");
		}
	}

	bool ModelPreviewPanel::Reimport(std::string* message)
	{
		if (!m_SourceExists)
		{
			const std::string text = Wui::Tr("panel.model.reimport.source_missing",
				"Reimport failed: source file not found (same name .gltf/.glb next to ")
				+ m_LogicalPath + ")";
			if (message) *message = text;
			m_Status = text;
			m_StatusIsError = true;
			return false;
		}
		const std::filesystem::path source = std::filesystem::path(WLD_ASSETPATH) / m_SourceLogical;
		Asset::GltfImportResult imported;
		std::string error;
		if (!Asset::ImportFile(source, std::filesystem::path(WLD_ASSETPATH), &imported, &error))
		{
			const std::string text = Wui::Tr("panel.model.reimport.failed", "Reimport failed: ")
				+ (error.empty() ? Wui::Tr("panel.model.reimport.unknown_error", "unknown error") : error);
			if (message) *message = text;
			m_Status = text;
			m_StatusIsError = true;
			return false;
		}
		// .wmodel 缓存会让旧网格继续被场景引用 —— 重导后清缓存,重新读盘。
		Mesh::ClearWModelCache();
		Reload();
		const std::string text = Wui::Tr("panel.model.reimport.done", "Reimported ") + m_LogicalPath
			+ "(mesh " + std::to_string(imported.MeshCount)
			+ " / submesh " + std::to_string(imported.SubmeshCount)
			+ Wui::Tr("panel.model.stats.nodes", " / nodes ") + std::to_string(imported.NodeCount)
			+ Wui::Tr("panel.model.stats.materials", " / materials ")
			+ std::to_string(imported.MaterialPaths.size()) + ")";
		if (message) *message = text;
		m_Status = text;
		m_StatusIsError = false;
		WLD_CORE_INFO("[model] reimported '{0}' from '{1}' (nodes={2} submeshes={3})",
			m_LogicalPath, m_SourceLogical, imported.NodeCount, imported.SubmeshCount);
		return true;
	}

	void ModelPreviewPanel::EnsureGpuResources()
	{
		const Rhi::Handle<Rhi::Device>& device = Renderer::GetDevice();
		if (!device)
			return;
		if (m_GpuDevice == device.get() && m_PreviewFramebuffer)
			return;
		ReleaseGpuResources();
		m_GpuDevice = device.get();

		// P4-4b:rendering.msaa>1 时预览通道升级为与场景通道**完全相同**的五附件结构
		// (颜色 / 实体 id / 深度多采样 + 颜色 / 实体 id resolve 到各自的单采样纹理);
		// WUI 采样与 WLD_PREVIEW_TEX_CAPTURE 读的仍是单采样 m_PreviewColor,语义不变。
		const Rhi::SampleCount previewSamples = static_cast<Rhi::SampleCount>(RenderSettings::Msaa());
		const bool multisampled = previewSamples != Rhi::SampleCount::Count1;

		Rhi::RenderPassDesc passDesc;
		Rhi::RenderPassAttachment color;
		color.Format = Rhi::Format::R8G8B8A8_UNORM;
		color.Samples = previewSamples;
		color.Load = Rhi::LoadOp::Clear;
		color.Store = Rhi::StoreOp::Store;
		color.InitialLayout = Rhi::AttachmentLayout::ColorAttachment;
		color.FinalLayout = Rhi::AttachmentLayout::ShaderReadOnly;
		color.Clear.Color = { 0.12f, 0.13f, 0.15f, 1.0f };

		Rhi::RenderPassAttachment entityId;
		entityId.Format = Rhi::Format::R32_SINT;
		entityId.Samples = previewSamples;
		entityId.Load = Rhi::LoadOp::Clear;
		entityId.Store = Rhi::StoreOp::Store;
		entityId.InitialLayout = Rhi::AttachmentLayout::ColorAttachment;
		entityId.FinalLayout = Rhi::AttachmentLayout::ColorAttachment;
		const int minusOne = -1;
		std::memcpy(&entityId.Clear.Color, &minusOne, sizeof(int));

		Rhi::RenderPassAttachment depth;
		depth.Format = Rhi::Format::D24_UNORM_S8_UINT;
		depth.Samples = previewSamples;
		depth.Load = Rhi::LoadOp::Clear;
		depth.Store = Rhi::StoreOp::Store;
		depth.InitialLayout = Rhi::AttachmentLayout::DepthStencilAttachment;
		depth.FinalLayout = Rhi::AttachmentLayout::DepthStencilAttachment;
		depth.Clear.IsDepthStencil = true;
		depth.Clear.DepthStencil.Depth = 1.0f;

		passDesc.Attachments = { color, entityId, depth };
		Rhi::SubpassDesc subpass;
		subpass.ColorAttachments = {
			{ 0, Rhi::AttachmentLayout::ColorAttachment },
			{ 1, Rhi::AttachmentLayout::ColorAttachment },
		};
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
			colorResolve.InitialLayout = Rhi::AttachmentLayout::ColorAttachment;
			colorResolve.FinalLayout = Rhi::AttachmentLayout::ShaderReadOnly;
			Rhi::RenderPassAttachment entityResolve;
			entityResolve.Format = Rhi::Format::R32_SINT;
			entityResolve.Samples = Rhi::SampleCount::Count1;
			entityResolve.Load = Rhi::LoadOp::DontCare;
			entityResolve.Store = Rhi::StoreOp::Store;
			entityResolve.InitialLayout = Rhi::AttachmentLayout::ColorAttachment;
			entityResolve.FinalLayout = Rhi::AttachmentLayout::ColorAttachment;
			passDesc.Attachments.push_back(colorResolve);    // 3
			passDesc.Attachments.push_back(entityResolve);   // 4
			subpass.ResolveAttachments = { 3, 4 };
		}
		passDesc.Subpasses = { subpass };
		passDesc.DebugName = "Model.PreviewPass";
		m_PreviewPass = device->CreateRenderPass(passDesc);

		Rhi::TextureDesc colorDesc;
		colorDesc.Type = Rhi::TextureType::Texture2D;
		colorDesc.Format = Rhi::Format::R8G8B8A8_UNORM;
		colorDesc.Extent = { m_PreviewSize, m_PreviewSize, 1 };
		colorDesc.Usage = Rhi::TextureUsageColorAttachment | Rhi::TextureUsageSampled;
		colorDesc.DebugName = "Model.PreviewColor";
		m_PreviewColor = device->CreateTexture(colorDesc);

		Rhi::TextureDesc entityDesc;
		entityDesc.Type = Rhi::TextureType::Texture2D;
		entityDesc.Format = Rhi::Format::R32_SINT;
		entityDesc.Extent = { m_PreviewSize, m_PreviewSize, 1 };
		entityDesc.Usage = Rhi::TextureUsageColorAttachment;
		entityDesc.DebugName = "Model.PreviewEntityId";
		m_PreviewEntityId = device->CreateTexture(entityDesc);

		Rhi::TextureDesc depthDesc;
		depthDesc.Type = Rhi::TextureType::Texture2D;
		depthDesc.Format = Rhi::Format::D24_UNORM_S8_UINT;
		depthDesc.Extent = { m_PreviewSize, m_PreviewSize, 1 };
		depthDesc.Usage = Rhi::TextureUsageDepthStencilAttachment;
		depthDesc.DebugName = "Model.PreviewDepth";
		if (multisampled)
		{
			// 多采样附件(只做绘制附件,内容由通道末的 resolve 写进上面的单采样纹理)。
			Rhi::TextureDesc msaaColorDesc = colorDesc;
			msaaColorDesc.Samples = previewSamples;
			msaaColorDesc.Usage = Rhi::TextureUsageColorAttachment;
			msaaColorDesc.DebugName = "Model.PreviewColorMSAA";
			m_PreviewColorMsaa = device->CreateTexture(msaaColorDesc);
			Rhi::TextureDesc msaaEntityDesc = msaaColorDesc;
			msaaEntityDesc.Format = Rhi::Format::R32_SINT;
			msaaEntityDesc.DebugName = "Model.PreviewEntityIdMSAA";
			m_PreviewEntityMsaa = device->CreateTexture(msaaEntityDesc);
			Rhi::TextureDesc msaaDepthDesc = msaaColorDesc;
			msaaDepthDesc.Format = Rhi::Format::D24_UNORM_S8_UINT;
			msaaDepthDesc.Usage = Rhi::TextureUsageDepthStencilAttachment;
			msaaDepthDesc.DebugName = "Model.PreviewDepthMSAA";
			m_PreviewDepthMsaa = device->CreateTexture(msaaDepthDesc);
		}
		else
		{
			// msaa==1:单采样深度就是附件(与旧代码逐字节一致)。
			m_PreviewDepth = device->CreateTexture(depthDesc);
		}

		Rhi::FramebufferDesc framebufferDesc;
		framebufferDesc.RenderPass = m_PreviewPass;
		framebufferDesc.Extent = { m_PreviewSize, m_PreviewSize };
		// 顺序必须与渲染通道附件表 1:1(Vulkan 要求 framebuffer 附件数/顺序与通道一致)。
		if (multisampled)
			framebufferDesc.Attachments = { m_PreviewColorMsaa, m_PreviewEntityMsaa, m_PreviewDepthMsaa,
				m_PreviewColor, m_PreviewEntityId };
		else
			framebufferDesc.Attachments = { m_PreviewColor, m_PreviewEntityId, m_PreviewDepth };
		framebufferDesc.DebugName = "Model.PreviewFramebuffer";
		m_PreviewFramebuffer = device->CreateFramebuffer(framebufferDesc);

		// P4-UX16b:每条帧槽位一条(与材质预览同因同修,见头文件说明)。
		for (uint32_t slot = 0; slot < Renderer::FramesInFlight; ++slot)
			m_PreviewCommands[slot] = device->CreateCommandBuffer("Model.Preview#" + std::to_string(slot));

		Rhi::BufferDesc cameraDesc;
		cameraDesc.Size = sizeof(glm::mat4);
		cameraDesc.Usage = Rhi::BufferUsageUniform;
		cameraDesc.Memory = Rhi::MemoryHint::HostVisible;
		cameraDesc.DebugName = "Model.Preview.Camera";
		m_PreviewCameraBuffer = device->CreateBuffer(cameraDesc);
		m_PreviewCameraSet = device->CreateDescriptorSet(Renderer::GetGlobalDescriptorSetLayout());
		if (m_PreviewCameraSet)
		{
			// 与材质预览同款:相机(binding 0)+ 灯光(binding 2)+ 阴影贴图(binding 3)
			// 必须在**同一次 Update**里写完(GL 的 Update 是整体替换语义)。
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
	}

	void ModelPreviewPanel::ReleaseGpuResources()
	{
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
		m_PreviewCameraSet = nullptr;
		m_GpuDevice = nullptr;
		++m_UiTextureGeneration;
	}

	// D5c-4b:资产里是否有绑定到有效 skin 的 mesh。
	bool ModelPreviewPanel::HasSkinnedMesh() const
	{
		if (!m_DataValid || m_Data.Skins.empty())
			return false;
		for (const Asset::WModelMeshRange& mesh : m_Data.Meshes)
		{
			if (mesh.SkinIndex >= 0 && static_cast<size_t>(mesh.SkinIndex) < m_Data.Skins.size())
				return true;
		}
		return false;
	}

	// D5c-4b:预览只播第 0 条 clip(控件/状态行/滑杆同一来源)。
	float ModelPreviewPanel::AnimationClipDuration() const
	{
		return (m_DataValid && !m_Data.Animations.empty()) ? m_Data.Animations[0].Duration : 0.0f;
	}

	uint64_t ModelPreviewPanel::RenderPreview()
	{
		// P4-UX16b:本帧槽位专属的命令缓冲(见头文件:单缓冲会在上一帧还没跑完时重录)。
		Rhi::Handle<Rhi::CommandBuffer>& command =
			m_PreviewCommands[Renderer::FrameSlot() % Renderer::FramesInFlight];
		if (!command)
			return 0;
		if (!m_Mesh)
			return 0;
		EnsureGpuResources();
		if (!m_PreviewFramebuffer || !m_PreviewCameraSet)
			return 0;

		const float distance = m_CameraDistance;
		const glm::vec3 eye {
			m_Focus.x + distance * std::cos(m_OrbitPitch) * std::sin(m_OrbitYaw),
			m_Focus.y + distance * std::sin(m_OrbitPitch),
			m_Focus.z + distance * std::cos(m_OrbitPitch) * std::cos(m_OrbitYaw) };
		const glm::mat4 view = glm::lookAt(eye, m_Focus, glm::vec3(0.0f, 1.0f, 0.0f));
		const float nearPlane = std::max(0.01f, m_CameraDistance * 0.01f);
		const glm::mat4 projection = glm::perspective(glm::radians(35.0f), 1.0f, nearPlane,
			m_CameraDistance * 4.0f + 100.0f);
		const glm::mat4 viewProjection = AdaptViewProjectionForOffscreen(projection * view,
			Renderer::GetBackendName() == "vulkan");
		m_PreviewCameraBuffer->SetData(&viewProjection, sizeof(glm::mat4));

		std::vector<Rhi::ClearValue> clears(3);
		clears[0].Color = { 0.12f, 0.13f, 0.15f, 1.0f };
		const int minusOne = -1;
		std::memcpy(&clears[1].Color, &minusOne, sizeof(int));
		clears[2].IsDepthStencil = true;
		clears[2].DepthStencil.Depth = 1.0f;

		command->Begin();
		command->BeginRenderPass(m_PreviewPass, m_PreviewFramebuffer, clears);
		command->SetViewport({ 0, 0, static_cast<float>(m_PreviewSize), static_cast<float>(m_PreviewSize) });
		command->SetScissor({ 0, 0, m_PreviewSize, m_PreviewSize });
		command->BindDescriptorSet(m_PreviewCameraSet, 0);
		Renderer3D::BeginScene(viewProjection, command);
		// set0(相机 + 灯光 + 阴影贴图)必须登记给 Renderer3D:它在**管线绑定之后**才真正
		// 执行 vkCmdBindDescriptorSets(见 Renderer3D::BindPipelineForCurrentPass 的说明);
		// 只在命令缓冲上直绑(set 索引 0)在 Vulkan 下会被丢,表现为"预览什么都没有"。
		Renderer3D::SetGlobalDescriptorSet(m_PreviewCameraSet);
		Renderer3D::BindPipelineForCurrentPass();

		// 按节点树逐 submesh 绘制:节点变换 × (该 mesh 的 submesh 范围),材质按 submesh 槽位。
		// 槽位用面板身份固定的保留区,避免与主场景/其它预览争用对象 UBO(会逐帧闪)。
		const uint32_t slotBase = Renderer3D::ReserveSlotBase(
			static_cast<uint32_t>(Wui::HashId(m_PanelId.c_str()) ^ 0x5F17u),
			static_cast<uint32_t>(std::max<size_t>(1, m_Mesh->GetSubmeshes().size())));
		uint32_t drawIndex = 0;
		const std::vector<MeshNode>& nodes = m_Mesh->GetNodes();
		const std::vector<MeshRange>& meshes = m_Mesh->GetMeshes();
		const std::vector<MeshSubmesh>& submeshes = m_Mesh->GetSubmeshes();

		// D5c-4b:布局 2(蒙皮)的 mesh 一律走蒙皮提交 —— 静态管线按 32B stride 读 64B 顶点会画乱几何。
		// 调色板按当前 m_AnimTime 算(暂停/未播放时就是当前时间的姿态),按 mesh 下标缓存一份。
		const bool skinnedLayout = m_Mesh->GetVertexLayoutId() == Mesh::kVertexLayoutSkinned;
		std::vector<std::vector<glm::mat4>> skinPalettes;
		if (skinnedLayout && m_DataValid)
		{
			skinPalettes.resize(meshes.size());
			static const Asset::WModelAnimation kEmptyClip;
			const Asset::WModelAnimation& clip = m_Data.Animations.empty() ? kEmptyClip : m_Data.Animations[0];
			for (size_t meshIndex = 0; meshIndex < meshes.size() && meshIndex < m_Data.Meshes.size(); ++meshIndex)
			{
				const int32_t skinIndex = m_Data.Meshes[meshIndex].SkinIndex;
				if (skinIndex < 0 || static_cast<size_t>(skinIndex) >= m_Data.Skins.size())
					continue;
				skinPalettes[meshIndex] = AnimationSystem::ComputePalette(m_Data,
					static_cast<uint32_t>(skinIndex), clip, m_AnimTime);
			}
		}

		const std::vector<std::string>& materialSlots = m_Mesh->GetMaterialSlots();
		const auto submitSubmesh = [&](int32_t meshIndex, uint32_t submeshIndex, const glm::mat4& transform)
		{
			const int32_t slot = submeshes[submeshIndex].MaterialSlot;
			const Ref<Material> material =
				(slot >= 0 && static_cast<size_t>(slot) < m_SlotMaterials.size())
					? m_SlotMaterials[static_cast<size_t>(slot)] : nullptr;
			const bool skinned = skinnedLayout && meshIndex >= 0
				&& static_cast<size_t>(meshIndex) < skinPalettes.size()
				&& !skinPalettes[static_cast<size_t>(meshIndex)].empty();
			if (skinned)
			{
				const std::vector<glm::mat4>& palette = skinPalettes[static_cast<size_t>(meshIndex)];
				// 蒙皮管线本阶段只支持不透明材质:透明 submesh 跳过(引擎只拒绝不 warn,面板一次性 warn)。
				if (material && material->GetDesc().BlendMode == MaterialBlendMode::Transparent)
				{
					if (!m_TransparentSkipWarned)
					{
						m_TransparentSkipWarned = true;
						const std::string materialPath =
							(slot >= 0 && static_cast<size_t>(slot) < materialSlots.size())
								? materialSlots[static_cast<size_t>(slot)] : std::string("(unknown)");
						WLD_CORE_WARN("[model] preview skips transparent skinned submesh in '{0}' (材质 '{1}'):"
							"蒙皮管线暂不支持透明材质", m_LogicalPath, materialPath);
					}
					++drawIndex;
					return;
				}
				const uint32_t result = Renderer3D::SubmitSkinnedAtSlot(slotBase + drawIndex, m_Mesh,
					submeshIndex, material, transform, palette.data(),
					static_cast<uint32_t>(palette.size()), -1);
				if (result == UINT32_MAX && !m_SkinnedSubmitWarned)
				{
					m_SkinnedSubmitWarned = true;
					WLD_CORE_WARN("[model] preview skinned submit failed for '{0}' submesh {1} "
						"(layout={2} joints={3});该 submesh 未绘制",
						m_LogicalPath, submeshIndex, m_Mesh->GetVertexLayoutId(), palette.size());
				}
				++drawIndex;
				return;
			}
			Renderer3D::SubmitSubmeshAtSlot(slotBase + drawIndex, m_Mesh, submeshIndex, material, transform, -1);
			++drawIndex;
		};

		if (nodes.empty())
		{
			// 没有节点树(理论上 v1 格式总有):退化为整体绘制。
			if (!m_Mesh->HasSubmeshes())
			{
				const bool skinned = skinnedLayout && !skinPalettes.empty() && !skinPalettes[0].empty();
				if (skinned)
				{
					const std::vector<glm::mat4>& palette = skinPalettes[0];
					const uint32_t result = Renderer3D::SubmitSkinnedAtSlot(slotBase, m_Mesh, UINT32_MAX,
						nullptr, glm::mat4(1.0f), palette.data(), static_cast<uint32_t>(palette.size()), -1);
					if (result == UINT32_MAX && !m_SkinnedSubmitWarned)
					{
						m_SkinnedSubmitWarned = true;
						WLD_CORE_WARN("[model] preview whole-mesh skinned submit failed for '{0}'", m_LogicalPath);
					}
				}
				else if (!skinnedLayout)
				{
					Renderer3D::SubmitAtSlot(slotBase, m_Mesh, nullptr, glm::mat4(1.0f), -1);
				}
				else if (!m_SkinnedSubmitWarned)
				{
					// 布局 2 但没有可用调色板(容器数据不完整):静态管线按 32B stride 读 64B 顶点会画乱,宁可跳过。
					m_SkinnedSubmitWarned = true;
					WLD_CORE_WARN("[model] preview skips skinned whole-mesh '{0}':调色板不可用(layout={1})",
						m_LogicalPath, m_Mesh->GetVertexLayoutId());
				}
			}
			else
			{
				const int32_t meshIndex = meshes.empty() ? -1 : 0;
				for (uint32_t submeshIndex = 0; submeshIndex < submeshes.size(); ++submeshIndex)
					submitSubmesh(meshIndex, submeshIndex, glm::mat4(1.0f));
			}
		}
		else
		{
			// 世界矩阵:自顶向下(节点按父在前的顺序存储;父矩阵缓存一次)。
			std::vector<glm::mat4> world(nodes.size(), glm::mat4(1.0f));
			for (size_t index = 0; index < nodes.size(); ++index)
			{
				const MeshNode& node = nodes[index];
				const glm::mat4 local = glm::translate(glm::mat4(1.0f), node.Translation)
					* glm::mat4_cast(node.Rotation)
					* glm::scale(glm::mat4(1.0f), node.Scale);
				world[index] = (node.Parent >= 0 && static_cast<size_t>(node.Parent) < index)
					? world[static_cast<size_t>(node.Parent)] * local : local;
				const int32_t meshIndex = node.MeshIndex;
				if (meshIndex < 0 || static_cast<size_t>(meshIndex) >= meshes.size())
					continue;
				const MeshRange& range = meshes[static_cast<size_t>(meshIndex)];
				for (uint32_t sub = 0; sub < range.SubmeshCount; ++sub)
				{
					const uint32_t submeshIndex = range.FirstSubmesh + sub;
					if (submeshIndex >= submeshes.size())
						break;
					submitSubmesh(meshIndex, submeshIndex, world[index]);
				}
			}
		}
		Renderer3D::EndScene();
		command->EndRenderPass();
		command->End();
		Renderer::SubmitScene(command, m_PreviewColor);
		if (const char* trace = std::getenv("WLD_TRACE_MODEL_PREVIEW"))
		{
			static int traced = 0;
			if (traced++ < 5)
			{
				const Renderer3D::Statistics stats = Renderer3D::GetStats();
				WLD_CORE_INFO("[model] preview trace '{0}': slotBase={1} draws={2} submeshes={3}",
					m_LogicalPath, slotBase, stats.DrawCalls, m_Mesh->GetSubmeshes().size());
			}
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

	void ModelPreviewPanel::CapturePreviewTextureSequence()
	{
		// 与材质预览同款的无障碍抓图:WLD_PREVIEW_TEX_CAPTURE=<目录> + WLD_SCREEN_CAPTURE_START/_EVERY/_COUNT
		// 写出 preview-model_<名字>-<n>.ppm(Vulkan 下唯一能"看到"预览内容的路径)。
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
		const std::string path = std::string(dir) + "/preview-model_" + ModelStem(m_LogicalPath) + "-"
			+ std::to_string(m_PreviewCaptureWritten) + ".ppm";
		Renderer::CaptureTexture(path, m_PreviewColor, m_PreviewSize, m_PreviewSize);
		++m_PreviewCaptureWritten;
	}

	void ModelPreviewPanel::OnRender(Wui::WuiContext& ctx, const Wui::WuiRect& rect, PanelHost& host)
	{
		const Wui::WuiTheme& theme = host.Theme();
		Wui::PanelBackground(ctx, rect, { 0.09f, 0.095f, 0.105f, 1.0f });

		// D5b-2:轻量轮询"需要重导"状态(1s 节流):外部改源/改 `.wimport` 后不需要手动刷新。
		const double now = std::chrono::duration<double>(
			std::chrono::steady_clock::now().time_since_epoch()).count();
		if (m_SettingsLoaded || m_SourceExists)
		{
			if (now >= m_NextSyncCheck)
			{
				m_NextSyncCheck = now + 1.0;
				RefreshSyncState();
			}
		}

		// D5c-4b:播放时间用面板自己的帧间 dt(首帧 0、clamp [0,0.25]),不依赖 EditorLayer/PanelHost。
		const bool showAnimControls = m_DataValid && HasSkinnedMesh() && !m_Data.Animations.empty();
		float animDelta = 0.0f;
		if (m_AnimClockValid)
			animDelta = std::clamp(static_cast<float>(now - m_LastAnimClock), 0.0f, 0.25f);
		m_LastAnimClock = now;
		m_AnimClockValid = true;
		const float animDuration = AnimationClipDuration();
		if (showAnimControls && animDuration > 0.0f)
			m_AnimTime = std::clamp(AnimationSystem::AdvanceTime(m_AnimTime, animDelta, m_AnimSpeed,
				m_AnimPlaying, m_AnimLoop, animDuration), 0.0f, animDuration);
		else if (showAnimControls)
			m_AnimTime = 0.0f;

		const float x = rect.X + 10.0f;
		const float width = rect.W - 20.0f;
		float y = rect.Y + 8.0f;
		Wui::Label(ctx, { x, y }, ShortenPath(m_LogicalPath), theme.Text, 13.0f);
		y += 22.0f;

		// 预览区:上=图(约占四成高),下=资产视图(状态/设置/依赖/节点树)。
		const float previewSide = std::min(width, std::max(160.0f, rect.H * 0.40f));
		const Wui::WuiRect previewRect { x, y, std::max(64.0f, previewSide), std::max(64.0f, previewSide) };
		const uint64_t textureId = RenderPreview();
		if (textureId != 0)
		{
			Wui::Image(ctx, previewRect, textureId, { 0, 0, 1, 1 }, theme);
			const bool hovered = ctx.IsHovered(previewRect);
			if (hovered && ctx.Input().Wheel != 0.0f)
			{
				const float step = std::max(0.05f, m_CameraDistance * 0.1f);
				m_CameraDistance = std::clamp(m_CameraDistance - ctx.Input().Wheel * step,
					m_MinDistance, m_MaxDistance);
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
			Wui::Label(ctx, { previewRect.X + 10.0f, previewRect.Y + 10.0f },
				Wui::Tr("panel.model.preview_unavailable", "Preview unavailable (model not loaded or RHI device not ready)"),
				theme.TextMuted, 12.0f);
		}
		y += previewRect.H + 8.0f;
		if (showAnimControls)
			y = DrawAnimationControls(ctx, x, y, width, theme);
		DrawAssetView(ctx, { x, y, width, std::max(0.0f, rect.Y + rect.H - y - 6.0f) }, host);
	}

	// P1b D5b-2:模型资产视图 —— 同步状态(是否需要重导)/ 导入设置 / 依赖清单 / 节点树,
	// 以及"放进当前场景 / Reimport / 保存导入设置"三个动作。状态文本登记成只读无障碍节点,
	// E2E 可以直接断言(不需要鼠标)。
	void ModelPreviewPanel::DrawAssetView(Wui::WuiContext& ctx, const Wui::WuiRect& rect, PanelHost& host)
	{
		const Wui::WuiTheme& theme = host.Theme();
		const float x = rect.X;
		const float width = rect.W;
		float y = rect.Y;
		if (rect.H < 40.0f)
			return;

		std::string statusText;
		if (m_NeedsReimport)
			statusText = Wui::Tr("panel.model.sync.needs_reimport", "Reimport needed: ")
				+ (m_SyncDetail.empty()
					? Wui::Tr("panel.model.sync.asset_stale", "asset is out of date with its source")
					: m_SyncDetail);
		else if (!m_SourceExists)
			statusText = Wui::Tr("panel.model.sync.in_sync_no_source",
				"In sync (no source file found, reimport unavailable)");
		else
			statusText = Wui::Tr("panel.model.sync.in_sync", "In sync (source and settings unchanged)");
		Wui::WuiAccessNode statusNode;
		statusNode.Id = Wui::HashId("model.status");
		statusNode.Window = Wui::WuiAccessibility::Get().CurrentWindow();
		statusNode.Panel = Wui::WuiAccessibility::Get().CurrentPanel();
		statusNode.Kind = "status";
		statusNode.Label = "model status";
		statusNode.Value = statusText;
		statusNode.Rect = { x, y, width, 16.0f };
		statusNode.Interactive = false;
		Wui::WuiAccessibility::Get().Register(statusNode);
		Wui::Label(ctx, { x, y }, statusText,
			m_NeedsReimport ? Wui::WuiColor { 1.0f, 0.72f, 0.30f, 1.0f } : theme.TextMuted, 12.0f);
		y += 18.0f;

		const float buttonW = (width - 16.0f) / 3.0f;
		if (Wui::Button(ctx, Wui::HashId("model.instance"), { x, y, buttonW, 22.0f },
			Wui::Tr("panel.model.instance", "Place in Scene"), theme))
		{
			std::string message;
			if (!host.InstantiateModelFile(m_LogicalPath, &message))
			{
				m_Status = message;
				m_StatusIsError = true;
			}
			else
			{
				m_Status = message;
				m_StatusIsError = false;
			}
		}
		if (Wui::Button(ctx, Wui::HashId("model.reimport"), { x + buttonW + 8.0f, y, buttonW, 22.0f },
			m_NeedsReimport ? "Reimport *" : "Reimport", theme))
		{
			std::string message;
			Reimport(&message);
		}
		if (Wui::Button(ctx, Wui::HashId("model.settings.save"), { x + 2.0f * (buttonW + 8.0f), y, buttonW, 22.0f },
			Wui::Tr("panel.model.import_settings.save", "Save Import Settings"), theme))
		{
			if (!m_SourceExists)
			{
				m_Status = Wui::Tr("panel.model.import_settings.no_source",
					"Failed to save settings: no source file");
				m_StatusIsError = true;
			}
			else
			{
				std::string error;
				if (Asset::ModelImportSettings::Save(
					(std::filesystem::path(WLD_ASSETPATH) / m_SourceLogical).string(), m_Settings, &error))
				{
					m_Status = Wui::Tr("panel.model.import_settings.saved",
						"Import settings saved (.wimport); click Reimport to apply");
					m_StatusIsError = false;
					RefreshSyncState();
				}
				else
				{
					m_Status = Wui::Tr("panel.model.import_settings.save_failed", "Failed to save settings: ") + error;
					m_StatusIsError = true;
				}
			}
		}
		y += 28.0f;

		if (m_SourceExists && m_SettingsLoaded)
		{
			const float halfW = (width - 8.0f) * 0.5f;
			Wui::Label(ctx, { x, y + 4.0f }, "Scale", theme.TextMuted, 11.0f);
			Wui::DragFloat(ctx, Wui::HashId("model.import.scale"), { x + 46.0f, y, halfW - 50.0f, 18.0f },
				m_Settings.Scale, 0.05f, 0.01f, 100.0f, theme);
			static const std::vector<std::string> upAxes { "Y", "Z" };
			int upAxisIndex = m_Settings.UpAxis == 0 ? 0 : 1;
			// P4-UX1:Combo 只画当前值,标签在左侧单独绘制(与其它面板同一排版语法)。
			Wui::Label(ctx, { x + halfW + 8.0f, y + 2.0f }, "Up Axis", theme.TextMuted, 11.0f);
			if (Wui::Combo(ctx, Wui::HashId("model.import.upaxis"),
				{ x + halfW + 54.0f, y - 2.0f, width - halfW - 54.0f, 18.0f },
				"Up Axis", upAxes, upAxisIndex, theme))
				m_Settings.UpAxis = upAxisIndex == 0 ? 0 : 1;
			y += 22.0f;
			bool exportMaterials = m_Settings.ExportMaterials;
			if (Wui::Checkbox(ctx, Wui::HashId("model.import.materials"), { x, y, 140.0f, 16.0f },
				Wui::Tr("panel.model.import_settings.export_materials", "Export Materials"), exportMaterials, theme))
				m_Settings.ExportMaterials = exportMaterials;
			bool exportTextures = m_Settings.ExportTextures;
			if (Wui::Checkbox(ctx, Wui::HashId("model.import.textures"), { x + 150.0f, y, 160.0f, 16.0f },
				Wui::Tr("panel.model.import_settings.export_textures", "Export Textures"), exportTextures, theme))
				m_Settings.ExportTextures = exportTextures;
			bool generateNormals = m_Settings.GenerateNormals;
			if (Wui::Checkbox(ctx, Wui::HashId("model.import.normals"), { x + 320.0f, y, 170.0f, 16.0f },
				Wui::Tr("panel.model.import_settings.generate_normals", "Generate Missing Normals"),
				generateNormals, theme))
				m_Settings.GenerateNormals = generateNormals;
			y += 20.0f;
		}

		if (m_Mesh && y < rect.Y + rect.H - 16.0f)
		{
			const MeshBounds& bounds = m_Mesh->GetBounds();
			// 数值统计行同样走本地化:标签/分隔符都来自目录,数字与格式保持原样。
			std::ostringstream stats;
			stats << Wui::Tr("panel.model.stats.nodes_label", "nodes ") << m_Mesh->GetNodes().size()
				<< Wui::Tr("panel.model.stats.sep", " / ") << "mesh " << m_Mesh->GetMeshes().size()
				<< Wui::Tr("panel.model.stats.sep", " / ") << "submesh " << m_Mesh->GetSubmeshes().size()
				<< Wui::Tr("panel.model.stats.sep", " / ")
				<< Wui::Tr("panel.model.stats.vertices", "vertices ") << m_Mesh->GetVertexCount()
				<< Wui::Tr("panel.model.stats.sep", " / ")
				<< Wui::Tr("panel.model.stats.indices", "indices ") << m_Mesh->GetIndexCount()
				<< Wui::Tr("panel.model.stats.bounds", " | bounds (")
				<< std::fixed << std::setprecision(2)
				<< bounds.Min.x << "," << bounds.Min.y << "," << bounds.Min.z << ")~("
				<< bounds.Max.x << "," << bounds.Max.y << "," << bounds.Max.z << ")";
			Wui::Label(ctx, { x, y }, stats.str(), theme.TextMuted, 11.0f);
			y += 16.0f;
		}

		if (m_Mesh && !m_Mesh->GetNodes().empty() && y < rect.Y + rect.H - 20.0f)
		{
			Wui::Label(ctx, { x, y }, Wui::Tr("panel.model.node_tree", "Node tree:"), theme.TextMuted, 11.0f);
			y += 14.0f;
			const std::vector<MeshNode>& nodes = m_Mesh->GetNodes();
			for (size_t index = 0; index < nodes.size() && index < 8; ++index)
			{
				const MeshNode& treeNode = nodes[index];
				int depth = 0;
				for (int32_t parent = treeNode.Parent; parent >= 0 && depth < 8;
					parent = nodes[static_cast<size_t>(parent)].Parent)
					++depth;
				std::string text = std::string(static_cast<size_t>(depth) * 2, ' ') + "- " + treeNode.Name;
				if (treeNode.MeshIndex >= 0)
					text += "  [mesh " + std::to_string(treeNode.MeshIndex) + "]";
				Wui::Label(ctx, { x, y }, text, theme.Text, 11.0f);
				y += 13.0f;
				if (y > rect.Y + rect.H - 30.0f)
					break;
			}
		}

		if (m_Mesh && !m_Mesh->GetMaterialSlots().empty() && y < rect.Y + rect.H - 20.0f)
		{
			Wui::Label(ctx, { x, y }, Wui::Tr("panel.model.dependencies", "Dependencies:"), theme.TextMuted, 11.0f);
			y += 14.0f;
			const std::vector<std::string>& slots = m_Mesh->GetMaterialSlots();
			for (size_t index = 0; index < slots.size() && index < 6; ++index)
			{
				const std::string& slot = slots[index];
				if (slot.empty())
				{
					Wui::Label(ctx, { x, y }, Wui::Tr("panel.model.empty_slot", "- (empty material slot)"),
						theme.TextMuted, 11.0f);
					y += 13.0f;
					continue;
				}
				const Ref<Material> material = MaterialLibrary::Get().Load(slot);
				const bool missing = material == nullptr;
				Wui::Label(ctx, { x, y },
					(missing ? Wui::Tr("panel.model.missing", "[missing] ") : std::string("- ")) + slot,
					missing ? Wui::WuiColor { 1.0f, 0.45f, 0.4f, 1.0f } : theme.Text, 11.0f);
				y += 13.0f;
				if (material)
				{
					const MaterialDesc& desc = material->GetDesc();
					const std::string textures[2] = { desc.AlbedoTexture, desc.NormalTexture };
					for (const std::string& texture : textures)
					{
						if (texture.empty())
							continue;
						const AssetFingerprint fingerprint = FingerprintAsset(texture, nullptr);
						Wui::Label(ctx, { x + 12.0f, y },
							(fingerprint.Exists ? std::string("  tex ")
								: Wui::Tr("panel.model.missing_texture", "  [missing] tex ")) + texture,
							fingerprint.Exists ? theme.TextMuted
								: Wui::WuiColor { 1.0f, 0.45f, 0.4f, 1.0f }, 11.0f);
						y += 13.0f;
					}
				}
				if (y > rect.Y + rect.H - 14.0f)
					break;
			}
		}

		if (!m_Status.empty() && y < rect.Y + rect.H - 13.0f)
			Wui::Label(ctx, { x, y }, m_Status,
				m_StatusIsError ? Wui::WuiColor { 1.0f, 0.45f, 0.4f, 1.0f } : theme.TextMuted, 11.0f);
	}

	// D5c-4b:动画控制条 —— play/loop/speed/time + 只读状态行。
	// 只在"有 skin + ≥1 条动画"时由 OnRender 调用;没有动画的资产这条路径完全不进(旧行为不变)。
	float ModelPreviewPanel::DrawAnimationControls(Wui::WuiContext& ctx, float x, float y, float width,
		const Wui::WuiTheme& theme)
	{
		const Asset::WModelAnimation* clip = m_Data.Animations.empty() ? nullptr : &m_Data.Animations[0];
		const float duration = clip ? clip->Duration : 0.0f;

		// 第一行:播放/暂停 + 循环 + 速度(0.1..4)。
		const float playWidth = 56.0f;
		if (Wui::Button(ctx, Wui::HashId("model.anim.play"), { x, y, playWidth, 20.0f },
			m_AnimPlaying ? Wui::Tr("panel.model.anim.pause", "Pause")
				: Wui::Tr("panel.model.anim.play", "Play"), theme))
			m_AnimPlaying = !m_AnimPlaying;
		bool loop = m_AnimLoop;
		if (Wui::Checkbox(ctx, Wui::HashId("model.anim.loop"),
			{ x + playWidth + 8.0f, y + 2.0f, 56.0f, 16.0f },
			Wui::Tr("panel.model.anim.loop", "Loop"), loop, theme))
			m_AnimLoop = loop;
		Wui::Label(ctx, { x + playWidth + 72.0f, y + 3.0f },
			Wui::Tr("panel.model.anim.speed", "Speed"), theme.TextMuted, 11.0f);
		Wui::DragFloat(ctx, Wui::HashId("model.anim.speed"),
			{ x + playWidth + 100.0f, y, std::max(48.0f, width - playWidth - 100.0f), 18.0f },
			m_AnimSpeed, 0.05f, 0.1f, 4.0f, theme);
		y += 22.0f;

		// 第二行:时间滑杆(0..duration;零时长 clip 用退化范围,值固定 0)。
		Wui::Label(ctx, { x, y + 3.0f }, Wui::Tr("panel.model.anim.time", "Time"), theme.TextMuted, 11.0f);
		float time = std::clamp(m_AnimTime, 0.0f, duration > 0.0f ? duration : 0.0f);
		Wui::SliderFloat(ctx, Wui::HashId("model.anim.time"),
			{ x + 40.0f, y, std::max(40.0f, width - 40.0f), 18.0f }, time, 0.0f,
			duration > 0.0f ? duration : 0.001f, theme);
		m_AnimTime = duration > 0.0f ? time : 0.0f;
		y += 20.0f;

		// 状态行:clip/t/playing 来自面板状态;skin=骨架数、joints=该 mesh 所用 skin 的关节数。
		size_t jointCount = 0;
		for (const Asset::WModelMeshRange& mesh : m_Data.Meshes)
		{
			if (mesh.SkinIndex >= 0 && static_cast<size_t>(mesh.SkinIndex) < m_Data.Skins.size())
			{
				jointCount = m_Data.Skins[static_cast<size_t>(mesh.SkinIndex)].JointNodes.size();
				break;
			}
		}
		char status[192] = {};
		std::snprintf(status, sizeof(status), "clip=%s t=%.2f/%.2f playing=%d skin=%zu joints=%zu",
			(clip && !clip->Name.empty()) ? clip->Name.c_str() : "-", m_AnimTime, duration,
			m_AnimPlaying ? 1 : 0, m_Data.Skins.size(), jointCount);
		Wui::WuiAccessNode statusNode;
		statusNode.Id = Wui::HashId("model.anim.status");
		statusNode.Window = Wui::WuiAccessibility::Get().CurrentWindow();
		statusNode.Panel = Wui::WuiAccessibility::Get().CurrentPanel();
		statusNode.Kind = "status";
		statusNode.Label = "model animation";
		statusNode.Value = status;
		statusNode.Rect = { x, y, width, 16.0f };
		statusNode.Interactive = false;
		Wui::WuiAccessibility::Get().Register(statusNode);
		Wui::Label(ctx, { x, y }, status, theme.TextMuted, 11.0f);
		y += 16.0f;
		return y;
	}
}
