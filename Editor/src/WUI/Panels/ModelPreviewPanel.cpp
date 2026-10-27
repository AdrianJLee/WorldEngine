#include "wldpch.h"
#include "WUI/Panels/ModelPreviewPanel.h"
#include "World/Asset/AssetThumbnailCache.h"
#include "WUI/Panels/ViewportPanel.h"
#include "WUI/Common/EditorAssetCatalog.h"
#include <glm/gtc/quaternion.hpp>

#include "World/Renderer/MaterialLibrary.h"
#include "World/Renderer/ProjectionConventions.h"
#include "World/Renderer/Renderer.h"
#include "World/Renderer/Renderer3D.h"
#include "World/Renderer/RenderSettings.h"
#include "World/Renderer/AnimationSystem.h"
#include "World/Renderer/AssetHotReload.h"
#include "World/Asset/GltfImporter.h"
#include "World/Core/KeyCodes.h"
#include "World/Utils/Paths.h"
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
		Wui::WuiColor Alpha(Wui::WuiColor color, float alpha)
		{
			color.A = alpha;
			return color;
		}

		std::string FormatNumber(size_t n)
		{
			std::string s = std::to_string(n);
			int insertPosition = static_cast<int>(s.length()) - 3;
			while (insertPosition > 0)
			{
				s.insert(static_cast<size_t>(insertPosition), ",");
				insertPosition -= 3;
			}
			return s;
		}

		std::string ToLower(std::string_view s)
		{
			std::string result(s);
			std::transform(result.begin(), result.end(), result.begin(),
				[](unsigned char c) { return static_cast<char>(std::tolower(c)); });
			return result;
		}
		std::string ShortenPath(const std::string& path)
		{
			if (path.size() <= 52)
				return path;
			return "…" + path.substr(path.size() - 51);
		}

		// P4-U11:按像素宽度裁剪文本(尾部省略号)。WUI 内部的同名工具是文件局部实现,
		// 面板侧自己留一份 —— 材质槽/节点树的名字可能很长,不裁会画到邻居列上。
		std::string EllipsizeToWidthLocal(Wui::WuiContext& ctx, const std::string& text,
			float maxWidth, float fontSize)
		{
			if (text.empty() || maxWidth <= 0.0f)
				return std::string();
			if (ctx.MeasureTextWidth(text, fontSize) <= maxWidth)
				return text;
			const float ellipsisWidth = ctx.MeasureTextWidth("…", fontSize);
			std::string result;
			float width = 0.0f;
			for (size_t index = 0; index < text.size();)
			{
				// UTF-8 逐码点推进(不切开多字节字符)。
				size_t length = 1;
				const unsigned char lead = static_cast<unsigned char>(text[index]);
				if (lead >= 0xF0) length = 4;
				else if (lead >= 0xE0) length = 3;
				else if (lead >= 0xC0) length = 2;
				length = std::min(length, text.size() - index);
				const std::string glyph = text.substr(index, length);
				const float glyphWidth = ctx.MeasureTextWidth(glyph, fontSize);
				if (width + glyphWidth + ellipsisWidth > maxWidth)
					break;
				result += glyph;
				width += glyphWidth;
				index += length;
			}
			return result + "…";
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
		// P4-U11:源与设置的解析**必须先于**"加载失败就返回"—— 旧版本(v4 及更早)资产读不出来时
		// 面板仍然要能显示导入源并提供"重新导入"(否则用户的老资产没有任何恢复入口)。
		ResolveSource();
		RefreshSyncState();
		if (!m_Mesh)
		{
			m_Status = Wui::Tr("panel.model.status.load_failed", "Model load failed: ")
				+ (error.empty() ? m_LogicalPath : error)
				+ (m_SourceExists
					? Wui::Tr("panel.model.status.load_failed_hint",
						" — press Reimport to rebuild this asset from its source")
					: std::string());
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
			// P4-U11:设置来自**资产本身**(.wmodel 的 meta,用户在这里改过的就是它);
			// 资产还没写进设置时退回项目默认(project.we.yaml 的 imports:)/ 引擎默认。
			std::string settingsWarning;
			m_Settings = Asset::ModelImportSettings::ResolveForImport(
				(World::Paths::AssetRoot() / m_LogicalPath).string(),
				&settingsWarning, &m_SettingsFromAsset);
			if (!settingsWarning.empty())
				WLD_CORE_WARN("[model] import settings for '{0}': {1}", m_LogicalPath, settingsWarning);
			m_SettingsLoaded = true;
			m_SettingsDirty = false;
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
		if (m_SettingsDirty)
		{
			// 面板里改了设置但还没重新导入:顶部提示 + 重导按钮标星(P4-U11 起设置只随重导落地)。
			m_NeedsReimport = true;
			m_SyncDetail = Wui::Tr("panel.model.sync.settings_edited", "Import settings edited — reimport to apply");
			return;
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
		const std::filesystem::path source = World::Paths::AssetRoot() / m_SourceLogical;
		Asset::GltfImportResult imported;
		std::string error;
		// P4-U11:用**面板当前的设置**导入(用户在设置区改完直接应用);设置随 .wmodel 存盘,
		// 不再写 .wimport 旁路文件。
		if (!Asset::GltfImporter::ImportFileWithSettings(source.string(),
			World::Paths::AssetRoot().string(), m_Settings, &imported, &error))
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
		m_SettingsDirty = false;
		Reload();
		const std::string text = Wui::Tr("panel.model.reimport.done", "Reimported ") + m_LogicalPath
			+ "(mesh " + std::to_string(imported.MeshCount)
			+ " / submesh " + std::to_string(imported.SubmeshCount)
			// WLD-L10N-S3:节点数走占位符(`{count}`),语言包可以调整语序。
			+ Wui::TrFormat("panel.model.stats.nodes", " / nodes {count}",
				{ { "count", std::to_string(imported.NodeCount) } })
			+ Wui::Tr("panel.model.stats.materials", " / materials ")
			+ std::to_string(imported.MaterialPaths.size()) + ")";
		if (message) *message = text;
		m_Status = text;
		m_StatusIsError = false;
		WLD_CORE_INFO("[model] reimported '{0}' from '{1}' (nodes={2} submeshes={3})",
			m_LogicalPath, m_SourceLogical, imported.NodeCount, imported.SubmeshCount);
		return true;
	}

	void ModelPreviewPanel::UpdatePreviewTargetSize(const Wui::WuiRect& previewRect)
	{
		const float uiScale = Wui::UiScale() > 0.0f ? Wui::UiScale() : 1.0f;
		const uint32_t desiredW = std::clamp(static_cast<uint32_t>(std::round(previewRect.W * uiScale)), 256u, 4096u);
		const uint32_t desiredH = std::clamp(static_cast<uint32_t>(std::round(previewRect.H * uiScale)), 256u, 4096u);

		if (desiredW != m_PreviewWidth || desiredH != m_PreviewHeight)
		{
			m_PreviewWidth = desiredW;
			m_PreviewHeight = desiredH;
			ReleaseGpuResources();
		}
	}

	void ModelPreviewPanel::EnsureGpuResources()
	{
		if (m_PreviewWidth == 0 || m_PreviewHeight == 0)
		{
			m_PreviewWidth = 512;
			m_PreviewHeight = 512;
		}
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
		colorDesc.Extent = { m_PreviewWidth, m_PreviewHeight, 1 };
		colorDesc.Usage = Rhi::TextureUsageColorAttachment | Rhi::TextureUsageSampled;
		colorDesc.DebugName = "Model.PreviewColor";
		m_PreviewColor = device->CreateTexture(colorDesc);

		Rhi::TextureDesc entityDesc;
		entityDesc.Type = Rhi::TextureType::Texture2D;
		entityDesc.Format = Rhi::Format::R32_SINT;
		entityDesc.Extent = { m_PreviewWidth, m_PreviewHeight, 1 };
		entityDesc.Usage = Rhi::TextureUsageColorAttachment;
		entityDesc.DebugName = "Model.PreviewEntityId";
		m_PreviewEntityId = device->CreateTexture(entityDesc);

		Rhi::TextureDesc depthDesc;
		depthDesc.Type = Rhi::TextureType::Texture2D;
		depthDesc.Format = Rhi::Format::D24_UNORM_S8_UINT;
		depthDesc.Extent = { m_PreviewWidth, m_PreviewHeight, 1 };
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
		framebufferDesc.Extent = { m_PreviewWidth, m_PreviewHeight };
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
		m_PreviewTextureHandle = nullptr;
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
		const Asset::WModelAnimation* clip = ActiveClip();
		return clip ? clip->Duration : 0.0f;
	}

	// P4-U11:当前预览的动画条;下标越界(资产重导后条数变少)回退第 0 条。
	const Asset::WModelAnimation* ModelPreviewPanel::ActiveClip() const
	{
		if (!m_DataValid || m_Data.Animations.empty())
			return nullptr;
		const size_t index = static_cast<size_t>(std::clamp(m_AnimClipIndex, 0,
			static_cast<int>(m_Data.Animations.size()) - 1));
		return &m_Data.Animations[index];
	}

	// P4-U11:取景 = 回到包围盒中心 + 按半径给一个合适的距离(与 Reload 的初始取景同一条公式)。
	void ModelPreviewPanel::FramePreview()
	{
		if (!m_Mesh)
			return;
		const MeshBounds bounds = ComputeWorldBounds();
		m_Focus = bounds.GetCenter();
		const float radius = std::max(0.25f, glm::length(bounds.GetExtents()));
		m_MinDistance = radius * 0.6f;
		m_MaxDistance = radius * 40.0f;
		m_CameraDistance = radius * 3.2f;
		m_OrbitYaw = 0.6f;
		m_OrbitPitch = 0.25f;
	}

	uint64_t ModelPreviewPanel::RenderPreview()
	{
		if (!m_Mesh)
			return 0;
		// 必须在取命令缓冲**之前**创建资源:命令缓冲是 EnsureGpuResources 建的,
		// 先判断 !command 会永远早退(实测:模型预览一直显示"预览不可用")。
		EnsureGpuResources();
		// P4-UX16b:本帧槽位专属的命令缓冲(见头文件:单缓冲会在上一帧还没跑完时重录)。
		Rhi::Handle<Rhi::CommandBuffer>& command =
			m_PreviewCommands[Renderer::FrameSlot() % Renderer::FramesInFlight];
		if (!m_PreviewFramebuffer || !m_PreviewCameraSet || !command)
			return 0;

		const float distance = m_CameraDistance;
		const glm::vec3 eye {
			m_Focus.x + distance * std::cos(m_OrbitPitch) * std::sin(m_OrbitYaw),
			m_Focus.y + distance * std::sin(m_OrbitPitch),
			m_Focus.z + distance * std::cos(m_OrbitPitch) * std::cos(m_OrbitYaw) };
		const glm::mat4 view = glm::lookAt(eye, m_Focus, glm::vec3(0.0f, 1.0f, 0.0f));
		const float nearPlane = std::max(0.01f, m_CameraDistance * 0.01f);
		const float aspect = static_cast<float>(m_PreviewWidth) / static_cast<float>(std::max(1u, m_PreviewHeight));
		const glm::mat4 projection = glm::perspective(glm::radians(35.0f), aspect, nearPlane,
			m_CameraDistance * 4.0f + 100.0f);
		m_LastCameraView = view;
		m_LastCameraProj = projection;
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
		command->SetViewport({ 0, 0, static_cast<float>(m_PreviewWidth), static_cast<float>(m_PreviewHeight) });
		command->SetScissor({ 0, 0, m_PreviewWidth, m_PreviewHeight });
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
			const Asset::WModelAnimation* active = ActiveClip();
			const Asset::WModelAnimation& clip = active ? *active : kEmptyClip;
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
		if (!m_PendingPreviewCapture.empty())
		{
			if (Renderer::CaptureTexture(m_PendingPreviewCapture, m_PreviewColor,
				m_PreviewWidth, m_PreviewHeight))
				WLD_CORE_INFO("[ai] model preview capture written: {0}", m_PendingPreviewCapture);
			m_PendingPreviewCapture.clear();
		}
		CapturePreviewTextureSequence();

		Wui::WuiTextureRegistry& registry = Wui::WuiTextureRegistry::Get();
		if (m_PreviewTextureId == 0)
		{
			m_PreviewTextureId = registry.Register(m_PreviewColor);
			m_PreviewTextureHandle = m_PreviewColor.get();
			m_UiTextureGeneration = registry.Generation();
		}
		else if (m_PreviewTextureHandle != m_PreviewColor.get() || registry.Generation() != m_UiTextureGeneration)
		{
			registry.Update(m_PreviewTextureId, m_PreviewColor);
			m_PreviewTextureHandle = m_PreviewColor.get();
			m_UiTextureGeneration = registry.Generation();
		}
		if (m_PreviewTextureId != 0 && !m_LogicalPath.empty())
		{
			ThumbnailInfo info;
			info.Path = m_LogicalPath;
			info.Category = AssetCategory::Mesh;
			info.TextureId = m_PreviewTextureId;
			info.Width = m_PreviewWidth;
			info.Height = m_PreviewHeight;
			info.IsReady = true;
			info.IsOffscreenRender = true;
			AssetThumbnailCache::Get().RegisterThumbnail(m_LogicalPath, info);
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
		Renderer::CaptureTexture(path, m_PreviewColor, m_PreviewWidth, m_PreviewHeight);
		++m_PreviewCaptureWritten;
	}

	void ModelPreviewPanel::OnRender(Wui::WuiContext& ctx, const Wui::WuiRect& rect, PanelHost& host)
	{
		const Wui::WuiTheme& theme = host.Theme();
		Wui::PanelBackground(ctx, rect, theme.PanelBg);

		// 轻量轮询"需要重导"状态 (1s 节流)
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

		// 动画播放推进 (面板独立时钟, 首帧 0, clamp [0, 0.25])
		const bool showAnimControls = m_DataValid && HasSkinnedMesh() && !m_Data.Animations.empty();
		float animDelta = 0.0f;
		if (m_AnimClockValid)
			animDelta = std::clamp(static_cast<float>(now - m_LastAnimClock), 0.0f, 0.25f);
		m_LastAnimClock = now;
		m_AnimClockValid = true;
		const float animDuration = AnimationClipDuration();
		if (showAnimControls && animDuration > 0.0f)
		{
			m_AnimTime = std::clamp(AnimationSystem::AdvanceTime(m_AnimTime, animDelta, m_AnimSpeed,
				m_AnimPlaying, m_AnimLoop, animDuration), 0.0f, animDuration);
		}
		else if (showAnimControls)
		{
			m_AnimTime = 0.0f;
		}

		const float pad = 8.0f;
		const float x = rect.X + pad;
		const float width = rect.W - pad * 2.0f;
		if (width < 140.0f || rect.H < 80.0f)
			return;

		// ① 顶部资产主工具栏 (36px 高度)
		const float topBarH = 36.0f;
		const Wui::WuiRect topBarRect { rect.X, rect.Y, rect.W, topBarH };
		DrawTopToolbar(ctx, topBarRect, host, theme);

		// ② 主体区域: 宽窗口两列 (左视口/右选项卡), 窄窗口单列
		const float bodyTop = rect.Y + topBarH + 6.0f;
		const float bodyBottom = rect.Y + rect.H - 8.0f;
		const float bodyH = std::max(0.0f, bodyBottom - bodyTop);
		if (bodyH < 60.0f)
			return;

		const bool twoColumn = rect.W >= 560.0f;
		const float animBarH = showAnimControls ? 74.0f : 0.0f;

		float previewW = 0.0f;
		float previewH = 0.0f;
		float infoX = x;
		float infoTop = bodyTop;
		float infoW = width;
		float infoH = bodyH;

		if (twoColumn)
		{
			const float colGap = 12.0f;
			const float minInfoW = 380.0f;
			infoW = (width >= minInfoW + 240.0f + colGap)
				? std::clamp(width * 0.35f, minInfoW, 480.0f)
				: std::max(240.0f, width * 0.5f);
			previewW = std::max(180.0f, width - infoW - colGap);
			previewH = std::max(160.0f, bodyH - animBarH);
			infoX = x + previewW + colGap;
			infoW = std::max(160.0f, rect.X + rect.W - pad - infoX);
		}
		else
		{
			previewW = width;
			previewH = std::clamp(bodyH * 0.44f, 160.0f, 340.0f);
			infoTop = bodyTop + previewH + animBarH + 10.0f;
			infoH = std::max(60.0f, bodyBottom - infoTop);
		}

		// 视口区域
		const Wui::WuiRect previewRect { x, bodyTop, previewW, previewH };
		UpdatePreviewTargetSize(previewRect);
		const uint64_t textureId = RenderPreview();
		if (textureId != 0)
		{
			Wui::Image(ctx, previewRect, textureId, ViewChrome::kPreviewImageUv, theme);

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
			if (m_Orbiting && ctx.Input().MouseDown[0] && !ctx.IsDoubleClicked(previewRect))
			{
				const glm::vec2 delta = ctx.Input().MousePos - m_LastMouse;
				m_LastMouse = ctx.Input().MousePos;
				ViewChrome::ApplyOrbitDrag(m_OrbitYaw, m_OrbitPitch, delta);
			}
			if (m_Orbiting && !ctx.Input().MouseDown[0])
				m_Orbiting = false;

			if (ctx.IsDoubleClicked(previewRect) || (hovered && ctx.WasKeyPressed(KeyCodes::F)))
				FramePreview();

			if (hovered)
				ctx.SetCursor(m_Orbiting ? Wui::WuiCursor::Hand : Wui::WuiCursor::Arrow);

			// 悬浮工具栏、网格与指示器
			DrawViewportOverlay(ctx, previewRect, theme);
		}
		else
		{
			Wui::PanelBackground(ctx, previewRect, theme.ContentBg, theme.Radius);
			Wui::Label(ctx, { previewRect.X + 12.0f, previewRect.Y + 14.0f },
				Wui::Tr("panel.model.preview_unavailable", "Preview unavailable (model not loaded or RHI device not ready)"),
				theme.TextMuted, theme.FontSizeSmall);
		}

		// 视口下方: 动画时间轴控制器 (有蒙皮动画时展示)
		if (showAnimControls)
		{
			const float animY = bodyTop + previewH + 6.0f;
			DrawTransportBar(ctx, x, animY, previewW, theme);
		}

		// ③ 选项卡信息区 (Overview / Materials / Skeleton / Settings / Clips)
		if (infoH >= 40.0f && infoW >= 120.0f)
		{
			const Wui::WuiRect infoRect { infoX, infoTop, infoW, infoH };
			DrawModuleTabs(ctx, infoRect, host, theme);
		}
	}

	void ModelPreviewPanel::DrawTopToolbar(Wui::WuiContext& ctx, const Wui::WuiRect& bar, PanelHost& host,
		const Wui::WuiTheme& theme)
	{
		Wui::BarSurface(ctx, bar, theme.PanelHeader, theme.Border);
		const float pad = 10.0f;
		float x = bar.X + pad;
		const float y = bar.Y + (bar.H - 24.0f) * 0.5f;

		// ① 资产类型徽标
		const float badgeW = 46.0f;
		Wui::Badge(ctx, { x, y + 2.0f, badgeW, 20.0f }, "MODEL", Alpha(theme.Accent, 0.22f), theme.Accent,
			theme, theme.FontSizeCaption, true, 4.0f);
		x += badgeW + 8.0f;

		// ② 来源状态徽标 (In Sync / Reimport Needed / No Source)
		std::string syncBadgeText;
		Wui::WuiColor syncBadgeFill;
		Wui::WuiColor syncBadgeTextCol;
		if (m_NeedsReimport)
		{
			syncBadgeText = Wui::Tr("panel.model.badge.reimport", "REIMPORT NEEDED");
			syncBadgeFill = Alpha(theme.Warning, 0.22f);
			syncBadgeTextCol = theme.Warning;
		}
		else if (!m_SourceExists)
		{
			syncBadgeText = Wui::Tr("panel.model.badge.no_source", "NO SOURCE");
			syncBadgeFill = Alpha(theme.TextDisabled, 0.18f);
			syncBadgeTextCol = theme.TextDisabled;
		}
		else
		{
			syncBadgeText = Wui::Tr("panel.model.badge.in_sync", "IN SYNC");
			syncBadgeFill = Alpha(theme.Success, 0.20f);
			syncBadgeTextCol = theme.Success;
		}
		const float syncBadgeW = ctx.MeasureTextWidth(syncBadgeText, theme.FontSizeCaption) + 12.0f;
		Wui::Badge(ctx, { x, y + 2.0f, syncBadgeW, 20.0f }, syncBadgeText, syncBadgeFill, syncBadgeTextCol,
			theme, theme.FontSizeCaption, true, 4.0f);
		x += syncBadgeW + 10.0f;

		// 右侧动作按钮组 (自适应紧凑模式)
		const float btnH = 24.0f;
		const float gap = 6.0f;
		const bool compact = bar.W < 780.0f;
		const float instanceW = compact ? 88.0f : 108.0f;
		const float reimportW = compact ? 76.0f : 84.0f;
		const float frameW = compact ? 56.0f : 68.0f;
		const float resetW = compact ? 56.0f : 74.0f;
		const float actionsTotalW = instanceW + reimportW + frameW + resetW + gap * 3.0f;

		// 中间资产路径显示
		const float pathBudget = std::max(60.0f, bar.W - (x - bar.X) - actionsTotalW - 16.0f);
		const std::string pathText = ShortenPath(m_LogicalPath);
		Wui::LabelWithTerm(ctx, { x, y + 4.0f }, pathText, std::string(), theme.Text, theme.FontSizeBody,
			theme, pathBudget);

		std::string fullPathTip = m_LogicalPath;
		if (!m_SourceLogical.empty())
			fullPathTip += "\n" + Wui::Tr("panel.model.source_line", "Source: ") + m_SourceLogical;
		Wui::Tooltip(ctx, { x, y, pathBudget, 24.0f }, fullPathTip);

		// 右对齐按钮组
		float ax = bar.X + bar.W - pad - actionsTotalW;

		// Place in Scene
		if (Wui::ActionButton(ctx, Wui::HashId("model.instance"), { ax, y, instanceW, btnH },
			compact ? Wui::Tr("panel.model.instance.compact", "Place") : Wui::Tr("panel.model.instance", "Place in Scene"),
			theme, true,
			Wui::Tr("panel.model.instance_tooltip", "Instantiate this model into the active scene at origin (or selection)."), true))
		{
			std::string message;
			m_StatusIsError = !host.InstantiateModelFile(m_LogicalPath, &message);
			m_Status = message;
		}
		ax += instanceW + gap;

		// Reimport (带脏标提示)
		const bool dirty = m_NeedsReimport || m_SettingsDirty;
		const std::string reimportLabel = dirty
			? Wui::Tr("panel.model.reimport_dirty", "Reimport *")
			: Wui::Tr("panel.model.reimport", "Reimport");
		if (Wui::Button(ctx, Wui::HashId("model.reimport"), { ax, y, reimportW, btnH }, reimportLabel, theme))
		{
			std::string message;
			Reimport(&message);
		}
		Wui::Tooltip(ctx, { ax, y, reimportW, btnH }, Wui::Tr("panel.model.reimport_tooltip",
			"Apply the current import settings and rebuild this asset from its source (.wmodel is rewritten)."));
		ax += reimportW + gap;

		// Frame (F)
		if (Wui::Button(ctx, Wui::HashId("model.frame"), { ax, y, frameW, btnH },
			Wui::Tr("panel.model.frame", "Frame"), theme))
		{
			FramePreview();
		}
		Wui::Tooltip(ctx, { ax, y, frameW, btnH }, Wui::Tr("panel.model.frame_tooltip",
			"Frame the model in the preview (also: double-click the preview, or press F while hovering it)."));
		ax += frameW + gap;

		// Reset View
		if (Wui::Button(ctx, Wui::HashId("model.reset_view"), { ax, y, resetW, btnH },
			Wui::Tr("panel.model.reset_view", "Reset"), theme))
		{
			m_OrbitYaw = 0.6f;
			m_OrbitPitch = 0.25f;
			FramePreview();
		}
		Wui::Tooltip(ctx, { ax, y, resetW, btnH }, Wui::Tr("panel.model.reset_view_tooltip",
			"Reset view orbit angles and distance to default."));

		// 注册 status a11y 节点保持无障碍测试向后兼容
		std::string statusText;
		if (m_NeedsReimport)
			statusText = "Reimport needed: " + (m_SyncDetail.empty() ? "asset is stale" : m_SyncDetail);
		else if (!m_SourceExists)
			statusText = "In sync (no source file)";
		else
			statusText = "In sync";
		Wui::WuiAccessNode statusNode;
		statusNode.Id = Wui::HashId("model.status");
		statusNode.Window = Wui::WuiAccessibility::Get().CurrentWindow();
		statusNode.Panel = Wui::WuiAccessibility::Get().CurrentPanel();
		statusNode.Kind = "status";
		statusNode.Label = "model status";
		statusNode.Value = statusText;
		statusNode.Rect = { bar.X, bar.Y, bar.W, bar.H };
		statusNode.Interactive = false;
		Wui::WuiAccessibility::Get().Register(statusNode);
	}
	void ModelPreviewPanel::DrawGroundGrid(Wui::WuiContext& ctx, const Wui::WuiRect& previewRect,
		const Wui::WuiTheme& theme)
	{
		if (!m_ShowGrid)
			return;
		Wui::ClipScope clipScope(ctx, previewRect);
		const glm::mat4 vp = m_LastCameraProj * m_LastCameraView;

		auto project = [&](const glm::vec3& p, glm::vec2& out) -> bool
		{
			const glm::vec4 c = vp * glm::vec4(p, 1.0f);
			if (c.w <= 0.02f)
				return false;
			const float nx = c.x / c.w;
			const float ny = c.y / c.w;
			if (std::abs(nx) > 2.5f || std::abs(ny) > 2.5f)
				return false;
			out.x = previewRect.X + (nx * 0.5f + 0.5f) * previewRect.W;
			out.y = previewRect.Y + (0.5f - ny * 0.5f) * previewRect.H;
			return true;
		};

		const float radius = std::clamp(m_CameraDistance * 0.6f, 1.5f, 18.0f);
		const float step = radius > 6.0f ? 2.0f : (radius > 2.5f ? 1.0f : 0.5f);
		const int steps = static_cast<int>(radius / step);

		const Wui::WuiColor gridCol = Alpha(theme.Border, 0.45f);
		const Wui::WuiColor axisXCol = Alpha(theme.AxisX, 0.80f);
		const Wui::WuiColor axisZCol = Alpha(theme.AxisZ, 0.80f);

		for (int i = -steps; i <= steps; ++i)
		{
			const float offset = static_cast<float>(i) * step;
			glm::vec2 p0, p1;
			if (project({ -radius, 0.0f, offset }, p0) && project({ radius, 0.0f, offset }, p1))
			{
				const Wui::WuiColor col = (i == 0) ? axisXCol : gridCol;
				Wui::LineSegment(ctx, p0, p1, col, (i == 0) ? 1.5f : 1.0f);
			}
			if (project({ offset, 0.0f, -radius }, p0) && project({ offset, 0.0f, radius }, p1))
			{
				const Wui::WuiColor col = (i == 0) ? axisZCol : gridCol;
				Wui::LineSegment(ctx, p0, p1, col, (i == 0) ? 1.5f : 1.0f);
			}
		}
	}

void ModelPreviewPanel::DrawBoundsWireframe(Wui::WuiContext& ctx, const Wui::WuiRect& previewRect,
		const Wui::WuiTheme& theme)
	{
		if (!m_ShowBounds && !m_ShowWireframe)
			return;
		if (!m_Mesh)
			return;
		Wui::ClipScope clipScope(ctx, previewRect);
		const glm::mat4 vp = m_LastCameraProj * m_LastCameraView;

		auto project = [&](const glm::vec3& p, glm::vec2& out) -> bool
		{
			const glm::vec4 c = vp * glm::vec4(p, 1.0f);
			if (c.w <= 0.02f)
				return false;
			const float nx = c.x / c.w;
			const float ny = c.y / c.w;
			out.x = previewRect.X + (nx * 0.5f + 0.5f) * previewRect.W;
			out.y = previewRect.Y + (0.5f - ny * 0.5f) * previewRect.H;
			return true;
		};

		const MeshBounds bounds = ComputeWorldBounds();
		const glm::vec3 bmin = bounds.Min;
		const glm::vec3 bmax = bounds.Max;

		const glm::vec3 corners[8] = {
			{ bmin.x, bmin.y, bmin.z },
			{ bmax.x, bmin.y, bmin.z },
			{ bmax.x, bmin.y, bmax.z },
			{ bmin.x, bmin.y, bmax.z },
			{ bmin.x, bmax.y, bmin.z },
			{ bmax.x, bmax.y, bmin.z },
			{ bmax.x, bmax.y, bmax.z },
			{ bmin.x, bmax.y, bmax.z }
		};

		glm::vec2 sc[8];
		bool valid[8];
		for (int i = 0; i < 8; ++i)
			valid[i] = project(corners[i], sc[i]);

		const float distance = m_CameraDistance;
		const glm::vec3 eye {
			m_Focus.x + distance * std::cos(m_OrbitPitch) * std::sin(m_OrbitYaw),
			m_Focus.y + distance * std::sin(m_OrbitPitch),
			m_Focus.z + distance * std::cos(m_OrbitPitch) * std::cos(m_OrbitYaw)
		};

		struct BoxFace
		{
			glm::vec3 Normal;
			glm::vec3 Center;
			int Edges[4];
		};

		const int edges[12][2] = {
			{0, 1}, {1, 2}, {2, 3}, {3, 0},
			{4, 5}, {5, 6}, {6, 7}, {7, 4},
			{0, 4}, {1, 5}, {2, 6}, {3, 7}
		};

		const glm::vec3 bcenter = (bmin + bmax) * 0.5f;
		const BoxFace faces[6] = {
			{ { -1.0f,  0.0f,  0.0f }, { bmin.x, bcenter.y, bcenter.z }, { 3, 7, 8, 11 } },
			{ {  1.0f,  0.0f,  0.0f }, { bmax.x, bcenter.y, bcenter.z }, { 1, 5, 9, 10 } },
			{ {  0.0f, -1.0f,  0.0f }, { bcenter.x, bmin.y, bcenter.z }, { 0, 1, 2, 3 } },
			{ {  0.0f,  1.0f,  0.0f }, { bcenter.x, bmax.y, bcenter.z }, { 4, 5, 6, 7 } },
			{ {  0.0f,  0.0f, -1.0f }, { bcenter.x, bcenter.y, bmin.z }, { 0, 4, 8, 9 } },
			{ {  0.0f,  0.0f,  1.0f }, { bcenter.x, bcenter.y, bmax.z }, { 2, 6, 10, 11 } },
		};

		bool edgeIsFront[12] = { false };
		for (int f = 0; f < 6; ++f)
		{
			if (glm::dot(faces[f].Normal, eye - faces[f].Center) > 0.0f)
			{
				for (int e = 0; e < 4; ++e)
					edgeIsFront[faces[f].Edges[e]] = true;
			}
		}

		const Wui::WuiColor frontCol = Alpha(theme.Accent, 0.85f);
		const Wui::WuiColor backCol = Alpha(theme.Accent, 0.12f);

		for (int i = 0; i < 12; ++i)
		{
			const int e0 = edges[i][0];
			const int e1 = edges[i][1];
			if (!valid[e0] || !valid[e1])
				continue;

			if (edgeIsFront[i])
				Wui::LineSegment(ctx, sc[e0], sc[e1], frontCol, 1.5f);
			else if (!m_OccludeBackBounds)
				Wui::LineSegment(ctx, sc[e0], sc[e1], backCol, 1.0f);
		}
	}

	void ModelPreviewPanel::DrawViewportOverlay(Wui::WuiContext& ctx, const Wui::WuiRect& previewRect,
		const Wui::WuiTheme& theme)
	{
		DrawGroundGrid(ctx, previewRect, theme);
		DrawBoundsWireframe(ctx, previewRect, theme);

		const glm::mat4 vp = m_LastCameraProj * m_LastCameraView;
		auto project = [&](const glm::vec3& p, glm::vec2& out) -> bool
		{
			const glm::vec4 c = vp * glm::vec4(p, 1.0f);
			if (c.w <= 0.02f)
				return false;
			const float nx = c.x / c.w;
			const float ny = c.y / c.w;
			out.x = previewRect.X + (nx * 0.5f + 0.5f) * previewRect.W;
			out.y = previewRect.Y + (0.5f - ny * 0.5f) * previewRect.H;
			return true;
		};

		// 3D 骨骼层级线框与关节点可视化 (m_ShowBones)
		if (m_ShowBones && m_DataValid && !m_Data.Nodes.empty())
		{
			const size_t nodeCount = m_Data.Nodes.size();
			std::vector<glm::vec3> translations(nodeCount);
			std::vector<glm::quat> rotations(nodeCount);
			std::vector<glm::vec3> scales(nodeCount);
			for (size_t i = 0; i < nodeCount; ++i)
			{
				translations[i] = m_Data.Nodes[i].Translation;
				rotations[i] = m_Data.Nodes[i].Rotation;
				scales[i] = m_Data.Nodes[i].Scale;
			}

			const Asset::WModelAnimation* clip = ActiveClip();
			if (clip != nullptr && !clip->Channels.empty())
			{
				const float t = m_AnimTime;
				for (const auto& ch : clip->Channels)
				{
					if (ch.TargetNode >= nodeCount || ch.Keys.empty()) continue;
					if (t <= ch.Keys.front().Time)
					{
						if (ch.Path == Asset::WModelIO::WModelAnimationPath::Translation) translations[ch.TargetNode] = glm::vec3(ch.Keys.front().Value);
						else if (ch.Path == Asset::WModelIO::WModelAnimationPath::Rotation) rotations[ch.TargetNode] = glm::normalize(glm::quat(ch.Keys.front().Value.w, ch.Keys.front().Value.x, ch.Keys.front().Value.y, ch.Keys.front().Value.z));
						else if (ch.Path == Asset::WModelIO::WModelAnimationPath::Scale) scales[ch.TargetNode] = glm::vec3(ch.Keys.front().Value);
					}
					else if (t >= ch.Keys.back().Time)
					{
						if (ch.Path == Asset::WModelIO::WModelAnimationPath::Translation) translations[ch.TargetNode] = glm::vec3(ch.Keys.back().Value);
						else if (ch.Path == Asset::WModelIO::WModelAnimationPath::Rotation) rotations[ch.TargetNode] = glm::normalize(glm::quat(ch.Keys.back().Value.w, ch.Keys.back().Value.x, ch.Keys.back().Value.y, ch.Keys.back().Value.z));
						else if (ch.Path == Asset::WModelIO::WModelAnimationPath::Scale) scales[ch.TargetNode] = glm::vec3(ch.Keys.back().Value);
					}
					else
					{
						for (size_t k = 0; k + 1 < ch.Keys.size(); ++k)
						{
							if (t >= ch.Keys[k].Time && t <= ch.Keys[k+1].Time)
							{
								const float factor = (t - ch.Keys[k].Time) / std::max(0.0001f, ch.Keys[k+1].Time - ch.Keys[k].Time);
								if (ch.Path == Asset::WModelIO::WModelAnimationPath::Translation)
									translations[ch.TargetNode] = glm::mix(glm::vec3(ch.Keys[k].Value), glm::vec3(ch.Keys[k+1].Value), factor);
								else if (ch.Path == Asset::WModelIO::WModelAnimationPath::Rotation)
								{
									const glm::quat q0(ch.Keys[k].Value.w, ch.Keys[k].Value.x, ch.Keys[k].Value.y, ch.Keys[k].Value.z);
									const glm::quat q1(ch.Keys[k+1].Value.w, ch.Keys[k+1].Value.x, ch.Keys[k+1].Value.y, ch.Keys[k+1].Value.z);
									rotations[ch.TargetNode] = glm::slerp(glm::normalize(q0), glm::normalize(q1), factor);
								}
								else if (ch.Path == Asset::WModelIO::WModelAnimationPath::Scale)
									scales[ch.TargetNode] = glm::mix(glm::vec3(ch.Keys[k].Value), glm::vec3(ch.Keys[k+1].Value), factor);
								break;
							}
						}
					}
				}
			}

			std::vector<glm::mat4> nodeWorld(nodeCount, glm::mat4(1.0f));
			std::vector<uint8_t> state(nodeCount, 0);
			std::function<glm::mat4(size_t)> resolve = [&](size_t idx) -> glm::mat4
			{
				if (state[idx] == 2) return nodeWorld[idx];
				if (state[idx] == 1) return glm::mat4(1.0f);
				state[idx] = 1;
				glm::mat4 parentWorld(1.0f);
				int32_t p = m_Data.Nodes[idx].Parent;
				if (p >= 0 && static_cast<size_t>(p) < nodeCount && static_cast<size_t>(p) != idx)
					parentWorld = resolve(static_cast<size_t>(p));
				nodeWorld[idx] = parentWorld * glm::translate(glm::mat4(1.0f), translations[idx])
					* glm::mat4_cast(rotations[idx])
					* glm::scale(glm::mat4(1.0f), scales[idx]);
				state[idx] = 2;
				return nodeWorld[idx];
			};
			for (size_t i = 0; i < nodeCount; ++i)
				resolve(i);

			// 绘制骨骼连线与关节点
			for (size_t i = 0; i < nodeCount; ++i)
			{
				const glm::vec3 pos = glm::vec3(nodeWorld[i][3]);
				glm::vec2 screenPos;
				if (!project(pos, screenPos))
					continue;

				const int32_t p = m_Data.Nodes[i].Parent;
				if (p >= 0 && static_cast<size_t>(p) < nodeCount)
				{
					const glm::vec3 parentPos = glm::vec3(nodeWorld[static_cast<size_t>(p)][3]);
					glm::vec2 parentScreen;
					if (project(parentPos, parentScreen))
					{
						const bool isSel = static_cast<int>(i) == m_SelectedNode || p == m_SelectedNode;
						const Wui::WuiColor boneCol = isSel ? Alpha(theme.Accent, 0.95f) : Alpha(theme.TextMuted, 0.65f);
						Wui::LineSegment(ctx, parentScreen, screenPos, boneCol, isSel ? 2.0f : 1.2f);
					}
				}

				const bool isSel = static_cast<int>(i) == m_SelectedNode;
				const float markerRadius = isSel ? 5.0f : 3.5f;
				const Wui::WuiRect markerRect { screenPos.x - markerRadius, screenPos.y - markerRadius, markerRadius * 2.0f, markerRadius * 2.0f };
				Wui::PanelBackground(ctx, markerRect, isSel ? theme.Accent : Alpha(theme.TextBright, 0.8f), markerRadius);
				if (isSel)
					Wui::HighlightOutline(ctx, markerRect, theme.TextBright, markerRadius, 1.5f);

				if (m_ShowBoneNames)
				{
					const std::string& name = m_Data.Nodes[i].Name.empty() ? ("node_" + std::to_string(i)) : m_Data.Nodes[i].Name;
					Wui::Label(ctx, { screenPos.x + 6.0f, screenPos.y - 6.0f }, name, isSel ? theme.Accent : theme.TextDisabled, theme.FontSizeCaption);
				}

				if (isSel || m_ShowJointAxes)
				{
					const float axisLen = std::max(0.05f, m_CameraDistance * 0.04f);
					glm::vec2 xScreen, yScreen, zScreen;
					if (project(pos + glm::vec3(nodeWorld[i][0]) * axisLen, xScreen))
						Wui::LineSegment(ctx, screenPos, xScreen, Wui::WuiColor { 0.95f, 0.25f, 0.25f, 1.0f }, 2.0f);
					if (project(pos + glm::vec3(nodeWorld[i][1]) * axisLen, yScreen))
						Wui::LineSegment(ctx, screenPos, yScreen, Wui::WuiColor { 0.25f, 0.90f, 0.25f, 1.0f }, 2.0f);
					if (project(pos + glm::vec3(nodeWorld[i][2]) * axisLen, zScreen))
						Wui::LineSegment(ctx, screenPos, zScreen, Wui::WuiColor { 0.25f, 0.45f, 0.95f, 1.0f }, 2.0f);
				}
			}
		}

		// 可折叠视口悬浮工具栏 ("把功能折叠起来")
		const float btnH = 22.0f;
		const float gap = 4.0f;

		if (m_ToolbarCollapsed)
		{
			const float expandW = 60.0f;
			const Wui::WuiRect miniPill { previewRect.X + 8.0f, previewRect.Y + 8.0f, expandW, btnH + 6.0f };
			Wui::PanelBackground(ctx, miniPill, Alpha(theme.PanelBg, 0.88f), 4.0f);
			Wui::HighlightOutline(ctx, miniPill, Alpha(theme.BorderStrong, 0.70f), 4.0f, 1.0f);

			if (Wui::Button(ctx, Wui::HashId("model.overlay.expand"), { miniPill.X + 3.0f, miniPill.Y + 3.0f, expandW - 6.0f, btnH }, "> View", theme))
			{
				m_ToolbarCollapsed = false;
			}
			Wui::Tooltip(ctx, miniPill, "Expand viewport controls toolbar");
		}
		else
		{
			const float dispBtnW = 104.0f;
			const float camBtnW = 76.0f;
			const float frameBtnW = 50.0f;
			const float colBtnW = 24.0f;
			const float toolW = dispBtnW + camBtnW + frameBtnW + colBtnW + gap * 5.0f;
			const float toolH = btnH + 6.0f;
			const Wui::WuiRect pill { previewRect.X + 8.0f, previewRect.Y + 8.0f, toolW, toolH };
			Wui::PanelBackground(ctx, pill, Alpha(theme.PanelBg, 0.88f), 4.0f);
			Wui::HighlightOutline(ctx, pill, Alpha(theme.BorderStrong, 0.70f), 4.0f, 1.0f);

			float bx = pill.X + gap;
			const float by = pill.Y + 3.0f;

			// Display Modes 下拉菜单
			const Wui::WuiRect dispRect { bx, by, dispBtnW, btnH };
			if (m_DisplayModesOpen)
				Wui::PanelBackground(ctx, dispRect, Alpha(theme.Accent, 0.28f), 3.0f);
			if (Wui::Button(ctx, Wui::HashId("model.overlay.display_modes"), dispRect,
				Wui::Tr("panel.model.overlay.display_modes", "Display v"), theme))
			{
				m_DisplayModesOpen = !m_DisplayModesOpen;
				m_CameraMenuOpen = false;
			}
			Wui::Tooltip(ctx, dispRect, "Toggle viewport visual elements and display modes");
			bx += dispBtnW + gap;

			// Camera 视角下拉菜单
			const Wui::WuiRect camRect { bx, by, camBtnW, btnH };
			if (m_CameraMenuOpen)
				Wui::PanelBackground(ctx, camRect, Alpha(theme.Accent, 0.28f), 3.0f);
			if (Wui::Button(ctx, Wui::HashId("model.overlay.camera"), camRect,
				Wui::Tr("panel.model.overlay.camera", "Camera v"), theme))
			{
				m_CameraMenuOpen = !m_CameraMenuOpen;
				m_DisplayModesOpen = false;
			}
			Wui::Tooltip(ctx, camRect, "Select camera perspective or view angles");
			bx += camBtnW + gap;

			// Frame 按钮
			const Wui::WuiRect frameRect { bx, by, frameBtnW, btnH };
			if (Wui::Button(ctx, Wui::HashId("model.overlay.frame"), frameRect, "Frame", theme))
				FramePreview();
			Wui::Tooltip(ctx, frameRect, Wui::Tr("panel.model.overlay.frame", "Frame camera to model bounds (F)."));
			bx += frameBtnW + gap;

			// 折叠按钮 [<]
			const Wui::WuiRect colRect { bx, by, colBtnW, btnH };
			if (Wui::Button(ctx, Wui::HashId("model.overlay.collapse"), colRect, "<", theme))
			{
				m_ToolbarCollapsed = true;
				m_DisplayModesOpen = false;
				m_CameraMenuOpen = false;
			}
			Wui::Tooltip(ctx, colRect, "Collapse viewport controls toolbar");

			// Display Modes 弹窗
			if (m_DisplayModesOpen)
			{
				const float popW = 160.0f;
				const float itemH = 22.0f;
				const float popH = itemH * 7.0f + 12.0f;
				const Wui::WuiRect popRect { pill.X, pill.Y + pill.H + 4.0f, popW, popH };
				Wui::DrawPanelSurface(ctx, popRect, theme);

				float py = popRect.Y + 6.0f;
				auto drawCheck = [&](Wui::WuiId id, const std::string& label, bool& val)
				{
					Wui::Checkbox(ctx, id, { popRect.X + 8.0f, py, popW - 16.0f, itemH }, label, val, theme);
					py += itemH;
				};
				drawCheck(Wui::HashId("model.disp.grid"), Wui::Tr("panel.model.overlay.grid", "Grid"), m_ShowGrid);
				drawCheck(Wui::HashId("model.disp.axes"), Wui::Tr("panel.model.overlay.gizmo", "Axes"), m_ShowGizmo);
				drawCheck(Wui::HashId("model.disp.bones"), Wui::Tr("panel.model.overlay.bones", "Bones"), m_ShowBones);
				drawCheck(Wui::HashId("model.disp.bone_names"), Wui::Tr("panel.model.overlay.bone_names", "Bone Names"), m_ShowBoneNames);
				drawCheck(Wui::HashId("model.disp.joint_axes"), Wui::Tr("panel.model.overlay.joint_axes", "Joint Axes"), m_ShowJointAxes);
				drawCheck(Wui::HashId("model.disp.bounds"), Wui::Tr("panel.model.overlay.wire", "Bounds"), m_ShowBounds);
				drawCheck(Wui::HashId("model.disp.occlude"), Wui::Tr("panel.model.overlay.occlude_back", "Occlude Back"), m_OccludeBackBounds);

				if (ctx.Input().MouseClicked[0] && !ctx.IsHovered(popRect) && !ctx.IsHovered(dispRect))
					m_DisplayModesOpen = false;
			}

			// Camera 菜单弹窗
			if (m_CameraMenuOpen)
			{
				const float popW = 140.0f;
				const float itemH = 22.0f;
				const float popH = itemH * 6.0f + 12.0f;
				const Wui::WuiRect popRect { pill.X + dispBtnW + gap, pill.Y + pill.H + 4.0f, popW, popH };
				Wui::DrawPanelSurface(ctx, popRect, theme);

				float py = popRect.Y + 6.0f;
				auto drawItem = [&](Wui::WuiId id, const std::string& label, auto action)
				{
					if (Wui::MenuItem(ctx, id, { popRect.X + 4.0f, py, popW - 8.0f, itemH }, label, true, theme))
					{
						action();
						m_CameraMenuOpen = false;
					}
					py += itemH;
				};

				drawItem(Wui::HashId("model.cam.persp"), Wui::Tr("panel.model.overlay.cam_persp", "Perspective"), [&] {
					m_OrbitYaw = 0.6f; m_OrbitPitch = 0.25f; FramePreview();
				});
				drawItem(Wui::HashId("model.cam.top"), Wui::Tr("panel.model.overlay.cam_top", "Top (KP7)"), [&] {
					m_OrbitYaw = 0.0f; m_OrbitPitch = 1.57f; FramePreview();
				});
				drawItem(Wui::HashId("model.cam.front"), Wui::Tr("panel.model.overlay.cam_front", "Front (KP1)"), [&] {
					m_OrbitYaw = 0.0f; m_OrbitPitch = 0.0f; FramePreview();
				});
				drawItem(Wui::HashId("model.cam.side"), Wui::Tr("panel.model.overlay.cam_side", "Side (KP3)"), [&] {
					m_OrbitYaw = 1.57f; m_OrbitPitch = 0.0f; FramePreview();
				});
				drawItem(Wui::HashId("model.cam.frame"), Wui::Tr("panel.model.overlay.frame", "Frame (F)"), [&] {
					FramePreview();
				});
				drawItem(Wui::HashId("model.cam.reset"), Wui::Tr("panel.model.overlay.reset", "Reset View"), [&] {
					m_OrbitYaw = 0.6f; m_OrbitPitch = 0.25f; FramePreview();
				});

				if (ctx.Input().MouseClicked[0] && !ctx.IsHovered(popRect) && !ctx.IsHovered(camRect))
					m_CameraMenuOpen = false;
			}
		}

		// 视口左下角统计信息徽标 (分辨率 + 顶点数)
		std::string statsPill = std::to_string(m_PreviewWidth) + "x" + std::to_string(m_PreviewHeight);
		if (m_Mesh)
			statsPill += "  " + FormatNumber(m_Mesh->GetVertexCount()) + " v";
		const float pillWidth = ctx.MeasureTextWidth(statsPill, theme.FontSizeCaption) + 12.0f;
		const Wui::WuiRect infoBadge { previewRect.X + 8.0f, previewRect.Y + previewRect.H - 22.0f, pillWidth, 18.0f };
		Wui::PanelBackground(ctx, infoBadge, Alpha(theme.PanelBg, 0.75f), 3.0f);
		Wui::Label(ctx, { infoBadge.X + 6.0f, infoBadge.Y + 3.0f }, statsPill, theme.TextDisabled, theme.FontSizeCaption);

		// 视口右下角坐标系指示器
		if (m_ShowGizmo)
		{
			glm::vec3 axisRight { 1.0f, 0.0f, 0.0f };
			glm::vec3 axisUp { 0.0f, 1.0f, 0.0f };
			ViewChrome::OrbitBasis(m_OrbitYaw, m_OrbitPitch, &axisRight, &axisUp, nullptr);
			const Wui::WuiRect axisRect = ViewChrome::DrawAxisIndicator(ctx, previewRect, axisRight, axisUp);
			ViewChrome::RegisterAxisNode(axisRect, "model.axis",
				Wui::Tr("panel.model.axis", "Preview axes"),
				ViewChrome::AxisReadout(glm::degrees(m_OrbitYaw), glm::degrees(m_OrbitPitch), false),
				Wui::Tr("panel.model.axis.tooltip", "World axes: X red, Y green, Z blue."));
		}
	}

	float ModelPreviewPanel::DrawTransportBar(Wui::WuiContext& ctx, float x, float y, float width,
		const Wui::WuiTheme& theme)
	{
		const Asset::WModelAnimation* clip = ActiveClip();
		const float duration = clip ? clip->Duration : 0.0f;
		const float totalH = 68.0f;
		const Wui::WuiRect container { x, y, width, totalH };
		Wui::PanelBackground(ctx, container, theme.CardBg, 4.0f);
		Wui::HighlightOutline(ctx, container, Alpha(theme.Border, 0.50f), 4.0f, 1.0f);

		float curY = y + 6.0f;
		const float trackH = 18.0f;
		const float trackPad = 8.0f;
		const Wui::WuiRect trackRect { x + trackPad, curY, width - trackPad * 2.0f, trackH };

		// 擦洗交互
		const bool trackHovered = ctx.IsHovered(trackRect);
		if (trackHovered && ctx.Input().MouseClicked[0])
			m_Scrubbing = true;
		if (m_Scrubbing && ctx.Input().MouseDown[0])
		{
			const float mouseX = ctx.Input().MousePos.x;
			const float frac = std::clamp((mouseX - trackRect.X) / std::max(1.0f, trackRect.W), 0.0f, 1.0f);
			m_AnimTime = frac * (duration > 0.0f ? duration : 0.0f);
		}
		if (m_Scrubbing && !ctx.Input().MouseDown[0])
			m_Scrubbing = false;

		if (trackHovered || m_Scrubbing)
			ctx.SetCursor(Wui::WuiCursor::Hand);

		// 轨道背景与填充
		Wui::PanelBackground(ctx, trackRect, theme.TrackBg, 3.0f);
		const float progressFrac = duration > 0.0f ? std::clamp(m_AnimTime / duration, 0.0f, 1.0f) : 0.0f;
		const float fillW = trackRect.W * progressFrac;
		if (fillW > 0.0f)
			Wui::PanelBackground(ctx, { trackRect.X, trackRect.Y, fillW, trackH }, Alpha(theme.Accent, 0.35f), 3.0f);

		// 刻度线 (0%, 25%, 50%, 75%, 100%)
		for (int i = 0; i <= 4; ++i)
		{
			const float tx = trackRect.X + trackRect.W * (static_cast<float>(i) * 0.25f);
			Wui::LineSegment(ctx, { tx, trackRect.Y }, { tx, trackRect.Y + trackH },
				Alpha(theme.BorderStrong, 0.5f), 1.0f);
		}

		// 播放头指针
		const float headX = trackRect.X + fillW;
		Wui::LineSegment(ctx, { headX, trackRect.Y - 1.0f }, { headX, trackRect.Y + trackH + 1.0f }, theme.Accent, 2.0f);
		Wui::PanelBackground(ctx, { headX - 3.0f, trackRect.Y - 2.0f, 6.0f, 5.0f }, theme.TextBright, 1.0f);

		// 注册 a11y slider 契约
		{
			Wui::WuiAccessNode timeNode;
			timeNode.Id = Wui::HashId("model.anim.time");
			timeNode.Window = Wui::WuiAccessibility::Get().CurrentWindow();
			timeNode.Panel = Wui::WuiAccessibility::Get().CurrentPanel();
			timeNode.Kind = "slider";
			timeNode.Label = "animation time";
			char buf[64];
			std::snprintf(buf, sizeof(buf), "%.2f / %.2f", m_AnimTime, duration);
			timeNode.Value = buf;
			timeNode.Rect = trackRect;
			timeNode.Interactive = true;
			Wui::WuiAccessibility::Get().Register(timeNode);
		}

		curY += trackH + 8.0f;

		// 下方走带控制组: 首帧 / 上一帧 / 播放暂停 / 下一帧 / 尾帧 / 循环 / 时间双显 / 速度 / 片段下拉
		float bx = x + trackPad;
		const float btnH = 22.0f;
		const float miniW = 28.0f;

		// |< 首帧
		if (Wui::Button(ctx, Wui::HashId("model.anim.first"), { bx, curY, miniW, btnH }, "|<", theme))
			m_AnimTime = 0.0f;
		Wui::Tooltip(ctx, { bx, curY, miniW, btnH }, "First Frame (t = 0)");
		bx += miniW + 4.0f;

		// < 上一帧
		if (Wui::Button(ctx, Wui::HashId("model.anim.prev"), { bx, curY, miniW, btnH }, "<", theme))
			m_AnimTime = std::max(0.0f, m_AnimTime - 1.0f / 30.0f);
		Wui::Tooltip(ctx, { bx, curY, miniW, btnH }, "Previous Frame (step 1/30s)");
		bx += miniW + 4.0f;

		// Play / Pause (Accent)
		const float playW = 56.0f;
		const std::string playLabel = m_AnimPlaying
			? Wui::Tr("panel.model.anim.pause", "Pause")
			: Wui::Tr("panel.model.anim.play", "Play");
		if (m_AnimPlaying)
			Wui::PanelBackground(ctx, { bx, curY, playW, btnH }, Alpha(theme.Accent, 0.25f), 3.0f);
		if (Wui::Button(ctx, Wui::HashId("model.anim.play"), { bx, curY, playW, btnH }, playLabel, theme))
			m_AnimPlaying = !m_AnimPlaying;
		bx += playW + 4.0f;

		// > 下一帧
		if (Wui::Button(ctx, Wui::HashId("model.anim.next"), { bx, curY, miniW, btnH }, ">", theme))
			m_AnimTime = std::min(duration, m_AnimTime + 1.0f / 30.0f);
		Wui::Tooltip(ctx, { bx, curY, miniW, btnH }, "Next Frame (step 1/30s)");
		bx += miniW + 4.0f;

		// >| 尾帧
		if (Wui::Button(ctx, Wui::HashId("model.anim.last"), { bx, curY, miniW, btnH }, ">|", theme))
			m_AnimTime = duration;
		Wui::Tooltip(ctx, { bx, curY, miniW, btnH }, "Last Frame");
		bx += miniW + 6.0f;

		// Loop 循环
		bool loop = m_AnimLoop;
		if (Wui::Checkbox(ctx, Wui::HashId("model.anim.loop"), { bx, curY + 2.0f, 52.0f, 18.0f },
			Wui::Tr("panel.model.anim.loop", "Loop"), loop, theme))
			m_AnimLoop = loop;
		bx += 56.0f + 8.0f;

		// 时间 / 帧数双模态显示
		const int curFrame = static_cast<int>(std::round(m_AnimTime * 30.0f));
		const int totalFrames = static_cast<int>(std::round(duration * 30.0f));
		char timeStr[128];
		if (m_TimelineShowFrames)
			std::snprintf(timeStr, sizeof(timeStr), "F: %d / %d", curFrame, totalFrames);
		else
			std::snprintf(timeStr, sizeof(timeStr), "%.2fs / %.2fs", m_AnimTime, duration);

		const float timeTextW = ctx.MeasureTextWidth(timeStr, theme.FontSizeCaption);
		const Wui::WuiRect timeClickRect { bx, curY + 2.0f, timeTextW + 8.0f, 18.0f };
		Wui::Label(ctx, { bx, curY + 4.0f }, timeStr, theme.Text, theme.FontSizeCaption);
		if (ctx.IsHovered(timeClickRect))
		{
			ctx.SetCursor(Wui::WuiCursor::Hand);
			Wui::Tooltip(ctx, timeClickRect, "Click to toggle Seconds vs Frames display.");
		}
		if (ctx.IsClicked(timeClickRect))
			m_TimelineShowFrames = !m_TimelineShowFrames;
		bx += timeTextW + 16.0f;

		// 播放速度调节 (右侧)
		const float speedW = 72.0f;
		const float speedX = x + width - trackPad - speedW;
		if (speedX > bx)
		{
			Wui::Label(ctx, { speedX - 42.0f, curY + 4.0f }, "Speed", theme.TextMuted, theme.FontSizeCaption);
			Wui::DragFloat(ctx, Wui::HashId("model.anim.speed"), { speedX, curY, speedW, btnH },
				m_AnimSpeed, 0.05f, 0.1f, 4.0f, theme);
		}

		return totalH + 8.0f;
	}

	void ModelPreviewPanel::DrawModuleTabs(Wui::WuiContext& ctx, const Wui::WuiRect& rect, PanelHost& host,
		const Wui::WuiTheme& theme)
	{
		const float tabH = 26.0f;
		const Wui::WuiRect tabArea { rect.X, rect.Y, rect.W, tabH };

		std::vector<std::string> tabLabels = {
			Wui::Tr("panel.model.tab.overview", "Overview"),
			Wui::Tr("panel.model.tab.materials", "Materials") + (m_Mesh ? " (" + std::to_string(m_Mesh->GetMaterialSlots().size()) + ")" : ""),
			Wui::Tr("panel.model.tab.skeleton", "Skeleton") + (m_DataValid && !m_Data.Skins.empty() ? " (" + std::to_string(m_Data.Skins[0].JointNodes.size()) + ")" : ""),
			Wui::Tr("panel.model.tab.settings", "Import Settings") + (m_SettingsDirty ? " *" : ""),
			Wui::Tr("panel.model.tab.clips", "Clips") + (m_DataValid && !m_Data.Animations.empty() ? " (" + std::to_string(m_Data.Animations.size()) + ")" : "")
		};

		Wui::TabBar(ctx, Wui::HashId("model.tabs"), tabArea, tabLabels, m_ActiveTab, theme);

		const float contentY = rect.Y + tabH + 6.0f;
		const float contentH = std::max(20.0f, rect.H - tabH - 6.0f);
		const Wui::WuiRect contentRect { rect.X, contentY, rect.W, contentH };

		float estimatedH = 400.0f;
		if (m_ActiveTab == 0) estimatedH = 340.0f;
		else if (m_ActiveTab == 1) estimatedH = 80.0f + static_cast<float>(m_Mesh ? m_Mesh->GetMaterialSlots().size() : 0) * 110.0f;
		else if (m_ActiveTab == 2) estimatedH = 60.0f + static_cast<float>(m_Mesh ? m_Mesh->GetNodes().size() : 0) * 22.0f;
		else if (m_ActiveTab == 3) estimatedH = 360.0f;
		else if (m_ActiveTab == 4) estimatedH = 60.0f + static_cast<float>(m_DataValid ? m_Data.Animations.size() : 0) * 72.0f;

		Wui::BeginScrollArea(ctx, contentRect, estimatedH, m_TabScroll[m_ActiveTab], theme);

		const Wui::WuiRect scrollSlice { rect.X, contentY, rect.W - 12.0f, contentH };
		if (m_ActiveTab == 0)
			DrawTabOverview(ctx, scrollSlice, host, theme);
		else if (m_ActiveTab == 1)
			DrawTabMaterials(ctx, scrollSlice, host, theme);
		else if (m_ActiveTab == 2)
			DrawTabSkeleton(ctx, scrollSlice, host, theme);
		else if (m_ActiveTab == 3)
			DrawTabImportSettings(ctx, scrollSlice, theme);
		else if (m_ActiveTab == 4)
			DrawTabClips(ctx, scrollSlice, host, theme);

		Wui::EndScrollArea(ctx);
	}
	float ModelPreviewPanel::DrawTabOverview(Wui::WuiContext& ctx, const Wui::WuiRect& rect, PanelHost& host,
		const Wui::WuiTheme& theme)
	{
		(void)host;
		const float x = rect.X;
		const float width = rect.W;
		float y = rect.Y + 4.0f;

		if (!m_Mesh)
		{
			Wui::Label(ctx, { x, y }, "No mesh loaded.", theme.TextMuted, theme.FontSizeBody);
			return 24.0f;
		}

		// 4 宫格几何指标卡片 (Nodes, Submeshes, Vertices, Indices/Tris)
		Wui::SectionHeader(ctx, { x, y, width, 18.0f },
			Wui::Tr("panel.model.overview.metrics", "Geometry Statistics"), theme.Accent, theme);
		y += 24.0f;

		const float colGap = 8.0f;
		const float colW = std::max(60.0f, (width - colGap * 3.0f) * 0.25f);
		const float cardH = 50.0f;

		auto drawStatCard = [&](float cx, const std::string& label, const std::string& val)
		{
			const Wui::WuiRect cRect { cx, y, colW, cardH };
			Wui::PanelBackground(ctx, cRect, theme.CardBg, 4.0f);
			Wui::HighlightOutline(ctx, cRect, Alpha(theme.Border, 0.6f), 4.0f, 1.0f);
			Wui::Label(ctx, { cx + 8.0f, y + 6.0f }, label, theme.TextMuted, theme.FontSizeCaption);
			Wui::Label(ctx, { cx + 8.0f, y + 24.0f }, val, theme.TextBright, theme.FontSizeTitle);
		};

		drawStatCard(x, "Nodes", std::to_string(m_Mesh->GetNodes().size()));
		drawStatCard(x + (colW + colGap), "Submeshes", std::to_string(m_Mesh->GetSubmeshes().size()));
		drawStatCard(x + (colW + colGap) * 2.0f, "Vertices", FormatNumber(m_Mesh->GetVertexCount()));
		drawStatCard(x + (colW + colGap) * 3.0f, "Triangles", FormatNumber(m_Mesh->GetIndexCount() / 3));
		y += cardH + 14.0f;

		// AABB 包围盒卡片
		Wui::SectionHeader(ctx, { x, y, width, 18.0f },
			Wui::Tr("panel.model.overview.bounds", "Bounding Box & Dimensions"), theme.Accent, theme);
		y += 24.0f;

		const MeshBounds bounds = ComputeWorldBounds();
		const glm::vec3 extents = bounds.GetExtents() * 2.0f;
		const glm::vec3 center = bounds.GetCenter();

		const Wui::WuiRect bboxCard { x, y, width, 76.0f };
		Wui::PanelBackground(ctx, bboxCard, theme.CardBg, 4.0f);
		Wui::HighlightOutline(ctx, bboxCard, Alpha(theme.Border, 0.6f), 4.0f, 1.0f);

		char sizeBuf[128];
		std::snprintf(sizeBuf, sizeof(sizeBuf), "Size (W x H x D): %.2f m x %.2f m x %.2f m",
			extents.x, extents.y, extents.z);
		Wui::Label(ctx, { x + 10.0f, y + 8.0f }, sizeBuf, theme.Text, theme.FontSizeBody);

		char minMaxBuf[192];
		std::snprintf(minMaxBuf, sizeof(minMaxBuf), "Min: (%.2f, %.2f, %.2f)   Max: (%.2f, %.2f, %.2f)",
			bounds.Min.x, bounds.Min.y, bounds.Min.z, bounds.Max.x, bounds.Max.y, bounds.Max.z);
		Wui::Label(ctx, { x + 10.0f, y + 30.0f }, minMaxBuf, theme.TextMuted, theme.FontSizeCaption);

		char centerBuf[128];
		std::snprintf(centerBuf, sizeof(centerBuf), "Center: (%.2f, %.2f, %.2f)", center.x, center.y, center.z);
		Wui::Label(ctx, { x + 10.0f, y + 50.0f }, centerBuf, theme.TextDisabled, theme.FontSizeCaption);

		y += bboxCard.H + 14.0f;

		// 源文件详情与骨骼管线卡片
		Wui::SectionHeader(ctx, { x, y, width, 18.0f },
			Wui::Tr("panel.model.overview.source", "Source & Rig Details"), theme.Accent, theme);
		y += 24.0f;

		const Wui::WuiRect srcCard { x, y, width, 72.0f };
		Wui::PanelBackground(ctx, srcCard, theme.CardBg, 4.0f);
		Wui::HighlightOutline(ctx, srcCard, Alpha(theme.Border, 0.6f), 4.0f, 1.0f);

		const std::string srcText = m_SourceLogical.empty()
			? "No glTF/GLB import source found (standalone .wmodel)"
			: ("Import source: " + m_SourceLogical + (m_SourceExists ? " [Exists]" : " [Missing on disk]"));
		Wui::Label(ctx, { x + 10.0f, y + 8.0f }, srcText, m_SourceExists ? theme.Text : theme.Warning,
			theme.FontSizeSmall);

		const bool isSkinned = m_Mesh->GetVertexLayoutId() == Mesh::kVertexLayoutSkinned;
		std::string rigText = isSkinned ? "Vertex Layout: Skinned (64B/vtx)  |  " : "Vertex Layout: Static (32B/vtx)  |  ";
		if (m_DataValid && !m_Data.Skins.empty())
		{
			rigText += "Skins: " + std::to_string(m_Data.Skins.size())
				+ "  Joints: " + std::to_string(m_Data.Skins[0].JointNodes.size())
				+ "  Clips: " + std::to_string(m_Data.Animations.size());
		}
		else
		{
			rigText += "Static model (no skin or animation blocks)";
		}
		Wui::Label(ctx, { x + 10.0f, y + 30.0f }, rigText, theme.TextMuted, theme.FontSizeCaption);

		if (!m_Status.empty())
			Wui::Label(ctx, { x + 10.0f, y + 50.0f }, m_Status, m_StatusIsError ? theme.Danger : theme.TextDisabled,
				theme.FontSizeCaption);

		y += srcCard.H + 12.0f;
		return y - rect.Y;
	}

void ModelPreviewPanel::ApplySlotChange(size_t index, const std::string& newPath)
	{
		if (m_Mesh)
			m_Mesh->SetMaterialSlot(index, newPath);

		if (index >= m_Data.MaterialSlots.size())
			m_Data.MaterialSlots.resize(index + 1);
		m_Data.MaterialSlots[index] = newPath;

		if (index >= m_SlotMaterials.size())
			m_SlotMaterials.resize(index + 1);
		m_SlotMaterials[index] = newPath.empty() ? nullptr : MaterialLibrary::Get().Load(newPath);

		std::string err;
		if (Asset::WModelIO::WriteFile(m_LogicalPath, m_Data, &err))
		{
			m_Status = "Slot #" + std::to_string(index) + " updated & saved to .wmodel";
			m_StatusIsError = false;
		}
		else
		{
			m_Status = "Failed saving slot to .wmodel: " + err;
			m_StatusIsError = true;
		}
	}

	void ModelPreviewPanel::CreateAndAssignMaterial(size_t index)
	{
		const std::string stem = ModelStem(m_LogicalPath);
		const std::string targetPath = "materials/models/" + stem + "_slot" + std::to_string(index) + ".wmat";
		const std::filesystem::path physical = Paths::AssetRoot() / targetPath;
		std::error_code ec;
		std::filesystem::create_directories(physical.parent_path(), ec);

		if (!std::filesystem::exists(physical, ec))
		{
			Ref<Material> newMat = MaterialLibrary::Get().CreateDefault(stem + "_slot" + std::to_string(index));
			std::string saveErr;
			if (!MaterialLibrary::Get().Save(newMat, targetPath, &saveErr))
			{
				m_Status = "Failed creating material: " + saveErr;
				m_StatusIsError = true;
				return;
			}
		}

		if (index >= m_MaterialSlotBuffers.size())
			m_MaterialSlotBuffers.resize(index + 1);
		m_MaterialSlotBuffers[index] = targetPath;
		ApplySlotChange(index, targetPath);
	}

	float ModelPreviewPanel::DrawTabMaterials(Wui::WuiContext& ctx, const Wui::WuiRect& rect, PanelHost& host,
		const Wui::WuiTheme& theme)
	{
		const float x = rect.X;
		const float width = rect.W;
		float y = rect.Y + 4.0f;

		if (!m_Mesh || m_Mesh->GetMaterialSlots().empty())
		{
			Wui::EmptyState(ctx, { x, y, width, 180.0f }, "MAT",
				Wui::Tr("panel.model.materials.empty_title", "No Material Slots"),
				Wui::Tr("panel.model.materials.empty_desc", "This model has no material slots defined in its geometry."),
				"", 0, theme);
			return 200.0f;
		}

		const std::vector<std::string>& slots = m_Mesh->GetMaterialSlots();
		if (m_MaterialSlotBuffers.size() != slots.size())
		{
			m_MaterialSlotBuffers.resize(slots.size());
			for (size_t i = 0; i < slots.size(); ++i)
				m_MaterialSlotBuffers[i] = slots[i];
		}

		// Header & Save to .wmodel Action Button
		const float saveSlotsW = 160.0f;
		Wui::SectionHeader(ctx, { x, y, width - saveSlotsW - 8.0f, 22.0f },
			Wui::Tr("panel.model.tab.materials", "Material Slots") + " (" + std::to_string(slots.size()) + ")",
			theme.Accent, theme);

		const Wui::WuiRect saveSlotsBtn { x + width - saveSlotsW, y, saveSlotsW, 22.0f };
		if (Wui::Button(ctx, Wui::HashId("model.materials.save_all"), saveSlotsBtn,
			Wui::Tr("panel.model.materials.save_slots", "Save Slots to .wmodel"), theme))
		{
			for (size_t i = 0; i < slots.size() && i < m_MaterialSlotBuffers.size(); ++i)
			{
				if (i < m_Data.MaterialSlots.size())
					m_Data.MaterialSlots[i] = m_MaterialSlotBuffers[i];
				if (m_Mesh)
					m_Mesh->SetMaterialSlot(i, m_MaterialSlotBuffers[i]);
			}
			std::string err;
			if (Asset::WModelIO::WriteFile(m_LogicalPath, m_Data, &err))
			{
				m_Status = Wui::Tr("panel.model.materials.saved_ok", "Saved material slots to .wmodel");
				m_StatusIsError = false;
			}
			else
			{
				m_Status = Wui::Tr("panel.model.materials.save_failed", "Save failed: ") + err;
				m_StatusIsError = true;
			}
		}
		Wui::Tooltip(ctx, saveSlotsBtn, Wui::Tr("panel.model.materials.save_slots_tip", "Persist all material slot assignments directly into the .wmodel file."));
		y += 28.0f;

		const std::vector<std::string>& matAssets = Editor::AssetCatalog::PathsFor(EditorAssetKind::Material);
		std::vector<std::string> comboOpts;
		comboOpts.reserve(matAssets.size() + 2);
		comboOpts.push_back("(unassigned)");
		for (const auto& m : matAssets)
			comboOpts.push_back(m);

		for (size_t index = 0; index < slots.size(); ++index)
		{
			const std::string& slot = slots[index];
			const float cardH = 116.0f;
			const Wui::WuiRect slotCard { x, y, width, cardH };
			Wui::PanelBackground(ctx, slotCard, theme.CardBg, 4.0f);
			Wui::HighlightOutline(ctx, slotCard, Alpha(theme.Border, 0.6f), 4.0f, 1.0f);

			// Line 1: Badge + Editable Material Path text field + Clear + Edit Material
			char slotBadge[32];
			std::snprintf(slotBadge, sizeof(slotBadge), "SLOT #%zu", index);
			Wui::Badge(ctx, { x + 10.0f, y + 8.0f, 62.0f, 20.0f }, slotBadge, Alpha(theme.Accent, 0.20f),
				theme.Accent, theme, theme.FontSizeCaption, true, 3.0f);

			const float clearW = 54.0f;
			const float editW = 92.0f;
			const float btnRightX = x + width - 10.0f;

			const Wui::WuiRect clearBtnRect { btnRightX - clearW, y + 7.0f, clearW, 22.0f };
			if (Wui::Button(ctx, Wui::HashId(("model.slot.clear." + std::to_string(index)).c_str()),
				clearBtnRect, Wui::Tr("panel.model.slot_clear", "Clear"), theme))
			{
				m_MaterialSlotBuffers[index].clear();
				ApplySlotChange(index, "");
			}
			Wui::Tooltip(ctx, clearBtnRect, Wui::Tr("panel.model.slot_clear_tip", "Clear this material slot (unassign)."));

			const Wui::WuiRect editBtnRect { clearBtnRect.X - 6.0f - editW, y + 7.0f, editW, 22.0f };
			if (!slot.empty())
			{
				if (Wui::Button(ctx, Wui::HashId(("model.slot.open." + std::to_string(index)).c_str()),
					editBtnRect, Wui::Tr("panel.model.open_mat", "Edit Material"), theme))
				{
					host.OpenMaterialEditor(slot);
				}
				Wui::Tooltip(ctx, editBtnRect, Wui::Tr("panel.model.open_mat_tip", "Open this material in the Material Editor."));
			}

			const float pathFieldX = x + 78.0f;
			const float pathFieldW = std::max(80.0f, editBtnRect.X - 8.0f - pathFieldX);
			const Wui::WuiRect pathFieldRect { pathFieldX, y + 7.0f, pathFieldW, 22.0f };
			const Wui::WuiId pathFieldId = Wui::HashId(("model.slot.path." + std::to_string(index)).c_str());

			Wui::TextFieldA11y pathA11y;
			pathA11y.Label = "Material Slot " + std::to_string(index);
			pathA11y.Placeholder = "materials/name.wmat";
			const bool pathSubmitted = Wui::TextField(ctx, pathFieldId, pathFieldRect, m_MaterialSlotBuffers[index], theme, nullptr, &pathA11y);
			if (ctx.Focus() == pathFieldId)
				ctx.SetTextInputActive(true);

			if (pathSubmitted || (ctx.Focus() != pathFieldId && m_MaterialSlotBuffers[index] != slot))
			{
				ApplySlotChange(index, m_MaterialSlotBuffers[index]);
			}
			Wui::Tooltip(ctx, pathFieldRect, Wui::Tr("panel.model.slot_path_tip", ".wmat logical path. Press Enter or click away to save."));

			// Line 2: Browse Combo + Create .wmat button
			const float line2Y = y + 36.0f;
			const float createW = 104.0f;
			const Wui::WuiRect createBtnRect { x + width - 10.0f - createW, line2Y, createW, 22.0f };
			if (Wui::Button(ctx, Wui::HashId(("model.slot.create." + std::to_string(index)).c_str()),
				createBtnRect, Wui::Tr("panel.model.create_wmat", "Create .wmat"), theme))
			{
				CreateAndAssignMaterial(index);
			}
			Wui::Tooltip(ctx, createBtnRect, Wui::Tr("panel.model.create_wmat_tip", "Create a new PBR material for this slot and assign it."));

			int comboIndex = 0;
			if (!slot.empty())
			{
				const auto it = std::find(matAssets.begin(), matAssets.end(), slot);
				if (it != matAssets.end())
					comboIndex = static_cast<int>(std::distance(matAssets.begin(), it)) + 1;
			}
			const float comboW = std::max(120.0f, createBtnRect.X - 8.0f - (x + 10.0f));
			const Wui::WuiRect comboRect { x + 10.0f, line2Y, comboW, 22.0f };
			if (Wui::Combo(ctx, Wui::HashId(("model.slot.combo." + std::to_string(index)).c_str()), comboRect, "",
				comboOpts, comboIndex, theme, slot.empty() ? "(Pick Material)" : ShortenPath(slot)))
			{
				std::string picked = (comboIndex > 0 && static_cast<size_t>(comboIndex - 1) < matAssets.size())
					? matAssets[static_cast<size_t>(comboIndex - 1)] : "";
				m_MaterialSlotBuffers[index] = picked;
				ApplySlotChange(index, picked);
			}
			Wui::Tooltip(ctx, comboRect, Wui::Tr("panel.model.pick_mat_tip", "Select an existing .wmat material from the project catalog."));

			// Line 3: Material load status / albedo / normal info
			const float line3Y = y + 64.0f;
			const Ref<Material> material = (index < m_SlotMaterials.size()) ? m_SlotMaterials[index] : nullptr;
			const bool missing = material == nullptr && !slot.empty();
			Wui::Label(ctx, { x + 10.0f, line3Y },
				missing ? "[MISSING MATERIAL] File could not be loaded" : (slot.empty() ? "Unassigned material" : "Material Loaded OK"),
				missing ? theme.Danger : (slot.empty() ? theme.TextDisabled : theme.Success), theme.FontSizeCaption);

			if (material)
			{
				const MaterialDesc& desc = material->GetDesc();
				const std::string albedoPath = desc.AlbedoTexture.empty() ? "(none)" : ShortenPath(desc.AlbedoTexture);
				Wui::Label(ctx, { x + 10.0f, line3Y + 18.0f }, "Albedo: " + albedoPath,
					theme.TextMuted, theme.FontSizeCaption);

				const std::string normalPath = desc.NormalTexture.empty() ? "(none)" : ShortenPath(desc.NormalTexture);
				const float secondColX = x + width * 0.5f;
				Wui::Label(ctx, { secondColX, line3Y + 18.0f }, "Normal: " + normalPath,
					theme.TextMuted, theme.FontSizeCaption);
			}
			else
			{
				Wui::Label(ctx, { x + 10.0f, line3Y + 18.0f }, "No texture inspection available.",
					theme.TextDisabled, theme.FontSizeCaption);
			}

			y += cardH + 8.0f;
		}

		return y - rect.Y;
	}

void ModelPreviewPanel::DrawNodeInspector(Wui::WuiContext& ctx, float x, float& y, float width,
		const Wui::WuiTheme& theme)
	{
		if (m_SelectedNode < 0 || static_cast<size_t>(m_SelectedNode) >= m_Data.Nodes.size())
			return;

		Asset::WModelNode& node = m_Data.Nodes[static_cast<size_t>(m_SelectedNode)];
		const float cardH = 224.0f;
		const Wui::WuiRect cardRect { x, y, width, cardH };
		Wui::PanelBackground(ctx, cardRect, theme.CardBg, 4.0f);
		Wui::HighlightOutline(ctx, cardRect, Alpha(theme.Accent, 0.70f), 4.0f, 1.0f);

		float cy = y + 8.0f;
		std::string headerTitle = Wui::Tr("panel.model.skeleton.inspector", "Node Inspector") + ": "
			+ (node.Name.empty() ? ("node_" + std::to_string(m_SelectedNode)) : node.Name)
			+ " [#" + std::to_string(m_SelectedNode) + "]";
		Wui::Label(ctx, { x + 10.0f, cy }, headerTitle, theme.Accent, theme.FontSizeSmall);

		// Action buttons on top-right: Reset Bind / Save Hierarchy
		const float saveBtnW = 120.0f;
		const float resetBtnW = 100.0f;
		const Wui::WuiRect saveBtnRect { x + width - 10.0f - saveBtnW, cy - 2.0f, saveBtnW, 20.0f };
		if (Wui::Button(ctx, Wui::HashId("model.node.save"), saveBtnRect,
			Wui::Tr("panel.model.skeleton.save_hierarchy", "Save to Asset"), theme))
		{
			std::string err;
			if (Asset::WModelIO::WriteFile(m_LogicalPath, m_Data, &err))
			{
				Reload();
				m_Status = "Hierarchy saved to .wmodel";
				m_StatusIsError = false;
			}
			else
			{
				m_Status = "Failed saving hierarchy: " + err;
				m_StatusIsError = true;
			}
		}
		Wui::Tooltip(ctx, saveBtnRect, Wui::Tr("panel.model.skeleton.save_hierarchy_tip", "Save hierarchy changes into .wmodel"));

		const Wui::WuiRect resetBtnRect { saveBtnRect.X - 6.0f - resetBtnW, cy - 2.0f, resetBtnW, 20.0f };
		if (Wui::Button(ctx, Wui::HashId("model.node.reset_bind"), resetBtnRect,
			Wui::Tr("panel.model.skeleton.reset_bind", "Reset Bind"), theme))
		{
			if (!m_Data.Skins.empty())
			{
				const auto& skin = m_Data.Skins[0];
				for (size_t j = 0; j < skin.JointNodes.size(); ++j)
				{
					if (skin.JointNodes[j] == static_cast<uint32_t>(m_SelectedNode))
					{
						if (j < skin.BindTranslations.size()) node.Translation = skin.BindTranslations[j];
						if (j < skin.BindRotations.size()) node.Rotation = skin.BindRotations[j];
						if (j < skin.BindScales.size()) node.Scale = skin.BindScales[j];
						m_SelectedNodeTranslation = node.Translation;
						m_SelectedNodeRotationEuler = glm::degrees(glm::eulerAngles(node.Rotation));
						m_SelectedNodeScale = node.Scale;
						break;
					}
				}
			}
		}
		Wui::Tooltip(ctx, resetBtnRect, Wui::Tr("panel.model.skeleton.reset_bind_tip", "Reset translation/rotation/scale to bind pose"));

		cy += 24.0f;

		// Row 1: Node Name
		const float labelW = 90.0f;
		const float fieldW = width - labelW - 20.0f;
		Wui::Label(ctx, { x + 10.0f, cy + 3.0f }, Wui::Tr("panel.model.skeleton.node_name", "Name"), theme.TextMuted, theme.FontSizeCaption);
		const Wui::WuiId nameId = Wui::HashId("model.node.name_field");
		Wui::TextFieldA11y nameA11y;
		nameA11y.Label = "Node Name";
		nameA11y.Placeholder = "Node Name";
		if (Wui::TextField(ctx, nameId, { x + labelW + 10.0f, cy, fieldW, 20.0f }, m_SelectedNodeNameBuffer, theme, nullptr, &nameA11y))
		{
			node.Name = m_SelectedNodeNameBuffer;
			if (!m_Data.Skins.empty())
			{
				for (size_t j = 0; j < m_Data.Skins[0].JointNodes.size(); ++j)
				{
					if (m_Data.Skins[0].JointNodes[j] == static_cast<uint32_t>(m_SelectedNode)
						&& j < m_Data.Skins[0].JointNames.size())
					{
						m_Data.Skins[0].JointNames[j] = m_SelectedNodeNameBuffer;
					}
				}
			}
		}
		if (ctx.Focus() == nameId)
			ctx.SetTextInputActive(true);
		cy += 24.0f;

		// Row 2: Parent Selector Combo with cycle check
		Wui::Label(ctx, { x + 10.0f, cy + 3.0f }, Wui::Tr("panel.model.skeleton.parent", "Parent"), theme.TextMuted, theme.FontSizeCaption);

		auto wouldCauseCycle = [this](int candidateParent) -> bool
		{
			if (candidateParent < 0) return false;
			if (candidateParent == m_SelectedNode) return true;
			int cur = candidateParent;
			int depth = 0;
			while (cur >= 0 && depth < 64)
			{
				if (cur == m_SelectedNode) return true;
				if (static_cast<size_t>(cur) >= m_Data.Nodes.size()) break;
				cur = m_Data.Nodes[static_cast<size_t>(cur)].Parent;
				++depth;
			}
			return false;
		};

		std::vector<std::string> parentOptions;
		std::vector<int> parentIndices;
		parentOptions.push_back(Wui::Tr("panel.model.skeleton.parent_root", "-1 (Root)"));
		parentIndices.push_back(-1);

		int curComboIdx = 0;
		for (size_t idx = 0; idx < m_Data.Nodes.size(); ++idx)
		{
			if (static_cast<int>(idx) == m_SelectedNode) continue;
			if (wouldCauseCycle(static_cast<int>(idx))) continue;

			std::string optName = std::to_string(idx) + ": " + (m_Data.Nodes[idx].Name.empty() ? "node" : m_Data.Nodes[idx].Name);
			if (node.Parent == static_cast<int32_t>(idx))
				curComboIdx = static_cast<int>(parentIndices.size());
			parentIndices.push_back(static_cast<int>(idx));
			parentOptions.push_back(optName);
		}

		if (Wui::Combo(ctx, Wui::HashId("model.node.parent_combo"), { x + labelW + 10.0f, cy, fieldW, 20.0f }, "",
			parentOptions, curComboIdx, theme, parentOptions[static_cast<size_t>(curComboIdx)]))
		{
			if (curComboIdx >= 0 && static_cast<size_t>(curComboIdx) < parentIndices.size())
			{
				node.Parent = parentIndices[static_cast<size_t>(curComboIdx)];
			}
		}
		cy += 24.0f;

		// Row 3: Translation (X, Y, Z)
		Wui::Label(ctx, { x + 10.0f, cy + 3.0f }, Wui::Tr("panel.model.skeleton.translation", "Translation"), theme.TextMuted, theme.FontSizeCaption);
		const float compW = (fieldW - 8.0f) / 3.0f;
		if (Wui::DragFloat(ctx, Wui::HashId("model.node.tx"), { x + labelW + 10.0f, cy, compW, 20.0f }, node.Translation.x, 0.01f, -1000.0f, 1000.0f, theme))
			m_SelectedNodeTranslation.x = node.Translation.x;
		if (Wui::DragFloat(ctx, Wui::HashId("model.node.ty"), { x + labelW + 10.0f + compW + 4.0f, cy, compW, 20.0f }, node.Translation.y, 0.01f, -1000.0f, 1000.0f, theme))
			m_SelectedNodeTranslation.y = node.Translation.y;
		if (Wui::DragFloat(ctx, Wui::HashId("model.node.tz"), { x + labelW + 10.0f + (compW + 4.0f) * 2.0f, cy, compW, 20.0f }, node.Translation.z, 0.01f, -1000.0f, 1000.0f, theme))
			m_SelectedNodeTranslation.z = node.Translation.z;
		cy += 24.0f;

		// Row 4: Rotation (Yaw, Pitch, Roll in degrees)
		Wui::Label(ctx, { x + 10.0f, cy + 3.0f }, Wui::Tr("panel.model.skeleton.rotation", "Rotation (deg)"), theme.TextMuted, theme.FontSizeCaption);
		bool rotChanged = false;
		rotChanged |= Wui::DragFloat(ctx, Wui::HashId("model.node.ry"), { x + labelW + 10.0f, cy, compW, 20.0f }, m_SelectedNodeRotationEuler.x, 0.5f, -360.0f, 360.0f, theme);
		rotChanged |= Wui::DragFloat(ctx, Wui::HashId("model.node.rp"), { x + labelW + 10.0f + compW + 4.0f, cy, compW, 20.0f }, m_SelectedNodeRotationEuler.y, 0.5f, -360.0f, 360.0f, theme);
		rotChanged |= Wui::DragFloat(ctx, Wui::HashId("model.node.rr"), { x + labelW + 10.0f + (compW + 4.0f) * 2.0f, cy, compW, 20.0f }, m_SelectedNodeRotationEuler.z, 0.5f, -360.0f, 360.0f, theme);
		if (rotChanged)
		{
			node.Rotation = glm::quat(glm::radians(m_SelectedNodeRotationEuler));
		}
		cy += 24.0f;

		// Row 5: Scale (X, Y, Z)
		Wui::Label(ctx, { x + 10.0f, cy + 3.0f }, Wui::Tr("panel.model.skeleton.scale", "Scale"), theme.TextMuted, theme.FontSizeCaption);
		Wui::DragFloat(ctx, Wui::HashId("model.node.sx"), { x + labelW + 10.0f, cy, compW, 20.0f }, node.Scale.x, 0.01f, 0.001f, 100.0f, theme);
		Wui::DragFloat(ctx, Wui::HashId("model.node.sy"), { x + labelW + 10.0f + compW + 4.0f, cy, compW, 20.0f }, node.Scale.y, 0.01f, 0.001f, 100.0f, theme);
		Wui::DragFloat(ctx, Wui::HashId("model.node.sz"), { x + labelW + 10.0f + (compW + 4.0f) * 2.0f, cy, compW, 20.0f }, node.Scale.z, 0.01f, 0.001f, 100.0f, theme);
		cy += 26.0f;

		// Row 6: Add Child Socket button & Delete Socket button
		const float actBtnW = 140.0f;
		const Wui::WuiRect addSocketRect { x + labelW + 10.0f, cy, actBtnW, 22.0f };
		if (Wui::Button(ctx, Wui::HashId("model.node.add_socket"), addSocketRect,
			Wui::Tr("panel.model.skeleton.add_socket", "Add Child Socket"), theme))
		{
			Asset::WModelNode socketNode;
			socketNode.Parent = m_SelectedNode;
			socketNode.MeshIndex = -1;
			socketNode.Name = "socket_" + (node.Name.empty() ? std::to_string(m_SelectedNode) : node.Name);
			socketNode.Translation = glm::vec3(0.0f);
			socketNode.Rotation = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
			socketNode.Scale = glm::vec3(1.0f);
			m_Data.Nodes.push_back(socketNode);
			m_SelectedNode = static_cast<int>(m_Data.Nodes.size() - 1);
			m_SelectedNodeNameBuffer = socketNode.Name;
			m_SelectedNodeTranslation = socketNode.Translation;
			m_SelectedNodeRotationEuler = glm::vec3(0.0f);
			m_SelectedNodeScale = socketNode.Scale;
		}
		Wui::Tooltip(ctx, addSocketRect, Wui::Tr("panel.model.skeleton.add_socket_tip", "Append a new child socket node under this bone."));

		bool isSkinJoint = false;
		if (!m_Data.Skins.empty())
		{
			for (uint32_t jn : m_Data.Skins[0].JointNodes)
				if (jn == static_cast<uint32_t>(m_SelectedNode)) { isSkinJoint = true; break; }
		}
		const bool canDelete = node.MeshIndex < 0 && !isSkinJoint;
		const Wui::WuiRect delSocketRect { addSocketRect.X + actBtnW + 8.0f, cy, actBtnW, 22.0f };
		if (canDelete)
		{
			if (Wui::Button(ctx, Wui::HashId("model.node.delete"), delSocketRect,
				Wui::Tr("panel.model.skeleton.delete_socket", "Delete Socket"), theme))
			{
				int32_t oldParent = node.Parent;
				for (size_t k = 0; k < m_Data.Nodes.size(); ++k)
				{
					if (m_Data.Nodes[k].Parent == m_SelectedNode)
						m_Data.Nodes[k].Parent = oldParent;
					else if (m_Data.Nodes[k].Parent > m_SelectedNode)
						m_Data.Nodes[k].Parent--;
				}
				m_Data.Nodes.erase(m_Data.Nodes.begin() + m_SelectedNode);
				m_SelectedNode = -1;
			}
			Wui::Tooltip(ctx, delSocketRect, Wui::Tr("panel.model.skeleton.delete_socket_tip", "Delete this socket node and reparent children to its parent."));
		}

		y += cardH + 10.0f;
	}

	float ModelPreviewPanel::DrawTabSkeleton(Wui::WuiContext& ctx, const Wui::WuiRect& rect, PanelHost& host,
		const Wui::WuiTheme& theme)
	{
		(void)host;
		const float x = rect.X;
		const float width = rect.W;
		float y = rect.Y + 4.0f;

		if (!m_Mesh || m_Mesh->GetNodes().empty())
		{
			Wui::EmptyState(ctx, { x, y, width, 180.0f }, "SKEL",
				Wui::Tr("panel.model.skeleton.empty_title", "No Skeleton Nodes"),
				Wui::Tr("panel.model.skeleton.empty_desc", "This model has no transform nodes or skeletal hierarchy."),
				"", 0, theme);
			return 200.0f;
		}

		// 搜索过滤栏与输入焦点独占
		const float searchH = 22.0f;
		const float clearW = 22.0f;
		const Wui::WuiRect searchRect { x, y, width - clearW - 4.0f, searchH };
		const Wui::WuiId searchId = Wui::HashId("model.skeleton.search");

		Wui::TextFieldA11y a11y;
		a11y.Placeholder = "Search skeleton / joint nodes...";
		Wui::TextField(ctx, searchId, searchRect, m_SkeletonFilter, theme, nullptr, &a11y);
		if (ctx.Focus() == searchId)
			ctx.SetTextInputActive(true);

		if (Wui::Button(ctx, Wui::HashId("model.skeleton.clear"), { x + width - clearW, y, clearW, searchH }, "X", theme))
			m_SkeletonFilter.clear();

		y += searchH + 10.0f;

		// 选中节点属性检视器
		if (m_SelectedNode >= 0 && static_cast<size_t>(m_SelectedNode) < m_Data.Nodes.size())
		{
			DrawNodeInspector(ctx, x, y, width, theme);
		}

		// 统计标头
		const std::vector<MeshNode>& nodes = m_Mesh->GetNodes();
		size_t jointCount = 0;
		if (m_DataValid && !m_Data.Skins.empty())
			jointCount = m_Data.Skins[0].JointNodes.size();

		std::string headerText = "Hierarchy (" + std::to_string(nodes.size()) + " nodes";
		if (jointCount > 0)
			headerText += ", " + std::to_string(jointCount) + " joints)";
		else
			headerText += ")";

		Wui::SectionHeader(ctx, { x, y, width, 18.0f }, headerText, theme.Accent, theme);
		y += 24.0f;

		const std::string filterLower = ToLower(m_SkeletonFilter);

		for (size_t index = 0; index < nodes.size(); ++index)
		{
			const MeshNode& node = nodes[index];
			if (!filterLower.empty() && ToLower(node.Name).find(filterLower) == std::string::npos)
				continue;

			int depth = 0;
			for (int32_t parent = node.Parent; parent >= 0 && depth < 16;
				parent = nodes[static_cast<size_t>(parent)].Parent)
				++depth;

			const float rowH = 22.0f;
			const Wui::WuiRect rowRect { x, y, width, rowH };

			const bool isSelected = static_cast<int>(index) == m_SelectedNode;
			Wui::HoverRow(ctx, rowRect, ctx.IsHovered(rowRect), isSelected, theme, 3.0f);
			if (ctx.IsClicked(rowRect))
			{
				m_SelectedNode = static_cast<int>(index);
				m_SelectedNodeNameBuffer = node.Name;
				m_SelectedNodeParent = node.Parent;
				m_SelectedNodeTranslation = node.Translation;
				m_SelectedNodeRotationEuler = glm::degrees(glm::eulerAngles(node.Rotation));
				m_SelectedNodeScale = node.Scale;
			}

			// 缩进树形线条与指示
			const float indent = static_cast<float>(depth) * 14.0f;
			const float nodeX = x + indent;

			// 节点类型指示 (Joint vs Mesh vs Group)
			bool isJoint = false;
			if (m_DataValid && !m_Data.Skins.empty())
			{
				for (uint32_t jn : m_Data.Skins[0].JointNodes)
					if (jn == index) { isJoint = true; break; }
			}

			std::string tag = isJoint ? "[J]" : (node.MeshIndex >= 0 ? "[M]" : "[N]");
			Wui::WuiColor tagCol = isJoint ? theme.Accent : (node.MeshIndex >= 0 ? theme.Success : theme.TextDisabled);
			Wui::Label(ctx, { nodeX, y + 4.0f }, tag, tagCol, theme.FontSizeCaption);

			// 节点名称
			const std::string nameText = node.Name.empty() ? ("node_" + std::to_string(index)) : node.Name;
			Wui::Label(ctx, { nodeX + 22.0f, y + 3.0f }, nameText, isSelected ? theme.TextBright : theme.Text, theme.FontSizeSmall);

			// 悬停提示节点变换信息
			if (ctx.IsHovered(rowRect))
			{
				char transformTip[256];
				std::snprintf(transformTip, sizeof(transformTip),
					"Node: %s (Index %zu, Parent %d)\nTrans: (%.2f, %.2f, %.2f)\nScale: (%.2f, %.2f, %.2f)",
					nameText.c_str(), index, node.Parent,
					node.Translation.x, node.Translation.y, node.Translation.z,
					node.Scale.x, node.Scale.y, node.Scale.z);
				Wui::Tooltip(ctx, rowRect, transformTip);
			}

			y += rowH;
		}

		return y - rect.Y;
	}

	float ModelPreviewPanel::DrawTabImportSettings(Wui::WuiContext& ctx, const Wui::WuiRect& rect,
		const Wui::WuiTheme& theme)
	{
		const float x = rect.X;
		const float width = rect.W;
		float y = rect.Y + 4.0f;

		Wui::SectionHeader(ctx, { x, y, width, 18.0f },
			Wui::Tr("panel.model.import_settings", "Import Settings"), theme.Accent, theme);
		y += 24.0f;

		if (!m_SourceExists || !m_SettingsLoaded)
		{
			Wui::Label(ctx, { x, y }, Wui::Tr("panel.model.import_settings.unavailable",
				"No source file: import settings cannot be edited (reimport unavailable)."),
				theme.TextMuted, theme.FontSizeSmall);
			return (y - rect.Y) + 24.0f;
		}

		// 设置存储来源横幅
		Wui::Label(ctx, { x, y }, m_SettingsFromAsset
			? Wui::Tr("panel.model.import_settings.from_asset", "Source: Stored inside this .wmodel (Asset self-describing)")
			: Wui::Tr("panel.model.import_settings.from_project_defaults",
				"Source: Project defaults (saved to .wmodel upon reimport)"),
			theme.TextDisabled, theme.FontSizeCaption);
		y += 18.0f;

		if (m_SettingsDirty)
		{
			Wui::Label(ctx, { x, y },
				Wui::Tr("panel.model.import_settings.dirty", "* Settings modified — press Reimport to apply changes to .wmodel"),
				theme.Warning, theme.FontSizeCaption);
			y += 18.0f;
		}

		// 1. Geometry 分组
		Wui::Label(ctx, { x, y }, "Geometry", theme.Accent, theme.FontSizeSmall);
		y += 18.0f;

		const float halfW = (width - 12.0f) * 0.5f;

		// Scale
		Wui::Label(ctx, { x, y + 3.0f }, "Scale", theme.TextMuted, theme.FontSizeCaption);
		const float scaleBefore = m_Settings.Scale;
		Wui::DragFloat(ctx, Wui::HashId("model.import.scale"), { x + 44.0f, y, halfW - 48.0f, 20.0f },
			m_Settings.Scale, 0.05f, 0.01f, 100.0f, theme);
		if (m_Settings.Scale != scaleBefore)
			m_SettingsDirty = true;
		Wui::Tooltip(ctx, { x + 44.0f, y, halfW - 48.0f, 20.0f }, "Uniform scale baked into geometry during import.");

		// Up Axis
		static const std::vector<std::string> upAxes { "Y", "Z" };
		int upAxisIndex = m_Settings.UpAxis == 0 ? 0 : 1;
		Wui::Label(ctx, { x + halfW + 8.0f, y + 3.0f }, "Up Axis", theme.TextMuted, theme.FontSizeCaption);
		if (Wui::Combo(ctx, Wui::HashId("model.import.upaxis"),
			{ x + halfW + 56.0f, y, std::max(40.0f, width - halfW - 56.0f), 20.0f },
			"Up Axis", upAxes, upAxisIndex, theme))
		{
			m_Settings.UpAxis = upAxisIndex == 0 ? 0 : 1;
			m_SettingsDirty = true;
		}
		y += 24.0f;

		// Generate Normals
		bool generateNormals = m_Settings.GenerateNormals;
		if (Wui::Checkbox(ctx, Wui::HashId("model.import.normals"), { x, y, width, 18.0f },
			"Generate Missing Normals", generateNormals, theme))
		{
			m_Settings.GenerateNormals = generateNormals;
			m_SettingsDirty = true;
		}
		y += 24.0f;

		// 2. Materials & Textures 分组
		Wui::Label(ctx, { x, y }, "Materials & Textures", theme.Accent, theme.FontSizeSmall);
		y += 18.0f;

		bool exportMaterials = m_Settings.ExportMaterials;
		if (Wui::Checkbox(ctx, Wui::HashId("model.import.materials"), { x, y, halfW, 18.0f },
			"Export Materials", exportMaterials, theme))
		{
			m_Settings.ExportMaterials = exportMaterials;
			m_SettingsDirty = true;
		}

		bool exportTextures = m_Settings.ExportTextures;
		if (Wui::Checkbox(ctx, Wui::HashId("model.import.textures"), { x + halfW + 8.0f, y, halfW, 18.0f },
			"Export Textures", exportTextures, theme))
		{
			m_Settings.ExportTextures = exportTextures;
			m_SettingsDirty = true;
		}
		y += 24.0f;

		// 3. Rig & Animation 分组
		Wui::Label(ctx, { x, y }, "Rig & Animation", theme.Accent, theme.FontSizeSmall);
		y += 18.0f;

		bool importSkins = m_Settings.ImportSkins;
		if (Wui::Checkbox(ctx, Wui::HashId("model.import.skins"), { x, y, halfW, 18.0f },
			"Import Skins", importSkins, theme))
		{
			m_Settings.ImportSkins = importSkins;
			m_SettingsDirty = true;
		}

		bool importAnimations = m_Settings.ImportAnimations;
		if (Wui::Checkbox(ctx, Wui::HashId("model.import.animations"), { x + halfW + 8.0f, y, halfW, 18.0f },
			"Import Animations", importAnimations, theme))
		{
			m_Settings.ImportAnimations = importAnimations;
			m_SettingsDirty = true;
		}
		y += 30.0f;

		// 操作按钮组: 还原默认值 + 应用并重导
		const float btnW = 120.0f;
		if (Wui::Button(ctx, Wui::HashId("model.import.reset_defaults"), { x, y, btnW, 24.0f },
			"Reset Defaults", theme))
		{
			m_Settings = Asset::ModelImportSettings {};
			m_SettingsDirty = true;
		}
		Wui::Tooltip(ctx, { x, y, btnW, 24.0f }, "Reset settings to system defaults.");

		if (Wui::ActionButton(ctx, Wui::HashId("model.import.apply_reimport"), { x + btnW + 8.0f, y, 130.0f, 24.0f },
			"Apply & Reimport", theme, true, "Save settings to .wmodel and reimport from source.", true))
		{
			std::string message;
			Reimport(&message);
		}

		y += 32.0f;
		return y - rect.Y;
	}

float ModelPreviewPanel::DrawTabClips(Wui::WuiContext& ctx, const Wui::WuiRect& rect, PanelHost& host,
		const Wui::WuiTheme& theme)
	{
		const float x = rect.X;
		const float width = rect.W;
		float y = rect.Y + 4.0f;

		// 架构说明卡片
		const float archCardH = 68.0f;
		const Wui::WuiRect archCard { x, y, width, archCardH };
		Wui::PanelBackground(ctx, archCard, theme.CardBg, 4.0f);
		Wui::HighlightOutline(ctx, archCard, Alpha(theme.Accent, 0.40f), 4.0f, 1.0f);

		Wui::Badge(ctx, { x + 10.0f, y + 8.0f, 96.0f, 18.0f },
			Wui::Tr("panel.model.clips.arch_title", "ARCHITECTURE"), Alpha(theme.Accent, 0.20f),
			theme.Accent, theme, theme.FontSizeCaption, true, 3.0f);

		const std::string archDesc = Wui::Tr("panel.model.clips.arch_desc",
			"Architecture: In WorldEngine, 3D animations are stored inside .wmodel assets alongside skeleton hierarchy. "
			"AnimGraph (.wanimgraph) assets reference clips by name to drive runtime transitions and blending.");
		Wui::Label(ctx, { x + 10.0f, y + 32.0f },
			Wui::EllipsizeMiddleToWidth(ctx, archDesc, width - 20.0f, theme.FontSizeCaption),
			theme.TextMuted, theme.FontSizeCaption);
		Wui::Tooltip(ctx, archCard, archDesc);

		y += archCardH + 10.0f;

		// 片段工具栏
		const float newClipW = 100.0f;
		const float dupClipW = 90.0f;
		const float splitClipW = 90.0f;
		const float saveClipsW = 120.0f;

		Wui::SectionHeader(ctx, { x, y, 140.0f, 22.0f },
			Wui::Tr("panel.model.tab.clips", "Animation Clips") + " (" + std::to_string(m_Data.Animations.size()) + ")",
			theme.Accent, theme);

		float btnX = x + width - saveClipsW;
		if (Wui::Button(ctx, Wui::HashId("model.clips.save_all"), { btnX, y, saveClipsW, 22.0f },
			Wui::Tr("panel.model.clips.save_clips", "Save to .wmodel"), theme))
		{
			std::string err;
			if (Asset::WModelIO::WriteFile(m_LogicalPath, m_Data, &err))
			{
				AnimationSystem::ClearCache();
				m_Status = "Clips saved to .wmodel";
				m_StatusIsError = false;
			}
			else
			{
				m_Status = "Failed saving clips: " + err;
				m_StatusIsError = true;
			}
		}
		Wui::Tooltip(ctx, { btnX, y, saveClipsW, 22.0f }, Wui::Tr("panel.model.clips.save_clips_tip", "Save all animation clips into .wmodel"));

		btnX -= splitClipW + 6.0f;
		if (Wui::Button(ctx, Wui::HashId("model.clips.toggle_split"), { btnX, y, splitClipW, 22.0f },
			Wui::Tr("panel.model.clips.split_clip", "Split Clip"), theme))
		{
			m_SplitClipOpen = !m_SplitClipOpen;
			m_NewClipOpen = false;
		}

		btnX -= dupClipW + 6.0f;
		if (Wui::Button(ctx, Wui::HashId("model.clips.duplicate"), { btnX, y, dupClipW, 22.0f },
			Wui::Tr("panel.model.clips.duplicate_clip", "Duplicate"), theme))
		{
			if (m_AnimClipIndex >= 0 && static_cast<size_t>(m_AnimClipIndex) < m_Data.Animations.size())
			{
				Asset::WModelAnimation copy = m_Data.Animations[static_cast<size_t>(m_AnimClipIndex)];
				copy.Name = copy.Name + "_copy";
				m_Data.Animations.push_back(copy);
				m_AnimClipIndex = static_cast<int>(m_Data.Animations.size() - 1);
			}
		}

		btnX -= newClipW + 6.0f;
		if (Wui::Button(ctx, Wui::HashId("model.clips.toggle_new"), { btnX, y, newClipW, 22.0f },
			Wui::Tr("panel.model.clips.new_clip", "New Clip"), theme))
		{
			m_NewClipOpen = !m_NewClipOpen;
			m_SplitClipOpen = false;
		}

		y += 28.0f;

		// 新建片段内联表单
		if (m_NewClipOpen)
		{
			const float formH = 40.0f;
			const Wui::WuiRect formRect { x, y, width, formH };
			Wui::PanelBackground(ctx, formRect, theme.CardBg, 4.0f);
			Wui::HighlightOutline(ctx, formRect, Alpha(theme.BorderStrong, 0.7f), 4.0f, 1.0f);

			Wui::Label(ctx, { x + 10.0f, y + 10.0f }, Wui::Tr("panel.model.clips.clip_name", "Name:"), theme.TextMuted, theme.FontSizeCaption);
			const Wui::WuiId newNameId = Wui::HashId("model.clip.new_name");
			Wui::TextFieldA11y a11y;
			a11y.Label = "New Clip Name";
			a11y.Placeholder = "NewClip";
			Wui::TextField(ctx, newNameId, { x + 60.0f, y + 8.0f, 130.0f, 22.0f }, m_NewClipNameBuffer, theme, nullptr, &a11y);
			if (ctx.Focus() == newNameId)
				ctx.SetTextInputActive(true);

			Wui::Label(ctx, { x + 200.0f, y + 10.0f }, Wui::Tr("panel.model.clips.duration", "Duration:"), theme.TextMuted, theme.FontSizeCaption);
			Wui::DragFloat(ctx, Wui::HashId("model.clip.new_duration"), { x + 265.0f, y + 8.0f, 70.0f, 22.0f },
				m_NewClipDurationBuffer, 0.05f, 0.1f, 120.0f, theme);

			const float createBtnW = 70.0f;
			if (Wui::Button(ctx, Wui::HashId("model.clip.new_create_btn"), { x + 345.0f, y + 8.0f, createBtnW, 22.0f },
				Wui::Tr("panel.model.clips.create", "Create"), theme))
			{
				Asset::WModelAnimation anim;
				anim.Name = m_NewClipNameBuffer.empty() ? ("clip_" + std::to_string(m_Data.Animations.size())) : m_NewClipNameBuffer;
				anim.Duration = std::max(0.1f, m_NewClipDurationBuffer);
				m_Data.Animations.push_back(anim);
				m_AnimClipIndex = static_cast<int>(m_Data.Animations.size() - 1);
				m_AnimTime = 0.0f;
				m_NewClipOpen = false;
			}

			y += formH + 8.0f;
		}

		// 切片内联表单
		if (m_SplitClipOpen && m_AnimClipIndex >= 0 && static_cast<size_t>(m_AnimClipIndex) < m_Data.Animations.size())
		{
			const float formH = 40.0f;
			const Wui::WuiRect formRect { x, y, width, formH };
			Wui::PanelBackground(ctx, formRect, theme.CardBg, 4.0f);
			Wui::HighlightOutline(ctx, formRect, Alpha(theme.BorderStrong, 0.7f), 4.0f, 1.0f);

			const auto& src = m_Data.Animations[static_cast<size_t>(m_AnimClipIndex)];

			Wui::Label(ctx, { x + 10.0f, y + 10.0f }, Wui::Tr("panel.model.clips.split_start", "Start:"), theme.TextMuted, theme.FontSizeCaption);
			Wui::DragFloat(ctx, Wui::HashId("model.clip.split_start"), { x + 55.0f, y + 8.0f, 65.0f, 22.0f },
				m_SplitStart, 0.05f, 0.0f, src.Duration, theme);

			Wui::Label(ctx, { x + 130.0f, y + 10.0f }, Wui::Tr("panel.model.clips.split_end", "End:"), theme.TextMuted, theme.FontSizeCaption);
			Wui::DragFloat(ctx, Wui::HashId("model.clip.split_end"), { x + 170.0f, y + 8.0f, 65.0f, 22.0f },
				m_SplitEnd, 0.05f, 0.0f, src.Duration, theme);

			const float applySplitW = 90.0f;
			if (Wui::Button(ctx, Wui::HashId("model.clip.apply_split_btn"), { x + 245.0f, y + 8.0f, applySplitW, 22.0f },
				Wui::Tr("panel.model.clips.split_apply", "Split & Add"), theme))
			{
				float t0 = std::clamp(m_SplitStart, 0.0f, src.Duration);
				float t1 = std::clamp(m_SplitEnd, t0 + 0.01f, src.Duration);
				Asset::WModelAnimation sub;
				sub.Name = src.Name + "_sub";
				sub.Duration = t1 - t0;
				for (const auto& ch : src.Channels)
				{
					Asset::WModelAnimationChannel subCh;
					subCh.TargetNode = ch.TargetNode;
					subCh.Path = ch.Path;
					for (const auto& k : ch.Keys)
					{
						if (k.Time >= t0 && k.Time <= t1)
						{
							Asset::WModelAnimationKey subK = k;
							subK.Time -= t0;
							subCh.Keys.push_back(subK);
						}
					}
					if (subCh.Keys.empty() && !ch.Keys.empty())
					{
						Asset::WModelAnimationKey k0 = ch.Keys.front();
						k0.Time = 0.0f;
						subCh.Keys.push_back(k0);
					}
					sub.Channels.push_back(subCh);
				}
				m_Data.Animations.push_back(sub);
				m_AnimClipIndex = static_cast<int>(m_Data.Animations.size() - 1);
				m_SplitClipOpen = false;
			}

			y += formH + 8.0f;
		}

		if (m_Data.Animations.empty())
		{
			Wui::EmptyState(ctx, { x, y, width, 180.0f }, "CLIP",
				Wui::Tr("panel.model.clips.empty_title", "No Animation Clips"),
				Wui::Tr("panel.model.clips.empty_desc", "This model contains no skeletal animation sequences. Click 'New Clip' to create one."),
				"", 0, theme);
			return y + 180.0f - rect.Y;
		}

		// 渲染片段列表
		for (size_t index = 0; index < m_Data.Animations.size(); ++index)
		{
			Asset::WModelAnimation& anim = m_Data.Animations[index];
			const bool isActive = static_cast<int>(index) == m_AnimClipIndex;

			const float cardH = 62.0f;
			const Wui::WuiRect clipCard { x, y, width, cardH };
			Wui::PanelBackground(ctx, clipCard, theme.CardBg, 4.0f);
			Wui::HighlightOutline(ctx, clipCard, isActive ? theme.Accent : Alpha(theme.Border, 0.6f), 4.0f,
				isActive ? 1.5f : 1.0f);

			if (isActive)
				Wui::Badge(ctx, { x + 8.0f, y + 8.0f, 54.0f, 18.0f }, "PLAYING", Alpha(theme.Accent, 0.25f),
					theme.Accent, theme, theme.FontSizeCaption, true, 3.0f);
			else
				Wui::Badge(ctx, { x + 8.0f, y + 8.0f, 42.0f, 18.0f }, "CLIP", Alpha(theme.TextDisabled, 0.20f),
					theme.TextMuted, theme, theme.FontSizeCaption, true, 3.0f);

			// 可编辑片段名称
			const Wui::WuiId clipNameFieldId = Wui::HashId(("model.clip.name." + std::to_string(index)).c_str());
			const float nameW = 160.0f;
			Wui::TextFieldA11y nameA11y;
			nameA11y.Label = "Clip Name";
			Wui::TextField(ctx, clipNameFieldId, { x + 68.0f, y + 6.0f, nameW, 22.0f }, anim.Name, theme, nullptr, &nameA11y);
			if (ctx.Focus() == clipNameFieldId)
				ctx.SetTextInputActive(true);

			// 时长与帧数
			const int frames = static_cast<int>(std::round(anim.Duration * 30.0f));
			char durStr[96];
			std::snprintf(durStr, sizeof(durStr), "%.2fs  |  %d frames @ 30 FPS  |  %zu channels",
				anim.Duration, frames, anim.Channels.size());
			Wui::Label(ctx, { x + 8.0f, y + 36.0f }, durStr, theme.TextMuted, theme.FontSizeCaption);

			// 操作按钮: Play / Open in Graph / Delete
			const float playBtnW = 60.0f;
			const float graphBtnW = 110.0f;
			const float delBtnW = 26.0f;
			const float bx = x + width - playBtnW - graphBtnW - delBtnW - 16.0f;

			if (Wui::Button(ctx, Wui::HashId(("model.clip.play." + std::to_string(index)).c_str()),
				{ bx, y + 18.0f, playBtnW, 24.0f }, isActive ? "Restart" : "Play", theme))
			{
				m_AnimClipIndex = static_cast<int>(index);
				m_AnimTime = 0.0f;
				m_AnimPlaying = true;
			}

			if (Wui::Button(ctx, Wui::HashId(("model.clip.open_graph." + std::to_string(index)).c_str()),
				{ bx + playBtnW + 6.0f, y + 18.0f, graphBtnW, 24.0f }, "Open in Graph", theme))
			{
				host.FocusIndependentWindow("animgraph");
				host.ActivatePanel("animgraph");
			}
			Wui::Tooltip(ctx, { bx + playBtnW + 6.0f, y + 18.0f, graphBtnW, 24.0f },
				"Switch to Animation Graph editor.");

			if (Wui::Button(ctx, Wui::HashId(("model.clip.delete." + std::to_string(index)).c_str()),
				{ bx + playBtnW + graphBtnW + 12.0f, y + 18.0f, delBtnW, 24.0f }, "X", theme))
			{
				m_Data.Animations.erase(m_Data.Animations.begin() + index);
				if (m_AnimClipIndex >= static_cast<int>(m_Data.Animations.size()))
					m_AnimClipIndex = std::max(0, static_cast<int>(m_Data.Animations.size()) - 1);
				break;
			}
			Wui::Tooltip(ctx, { bx + playBtnW + graphBtnW + 12.0f, y + 18.0f, delBtnW, 24.0f },
				Wui::Tr("panel.model.clips.delete", "Delete Clip"));

			y += cardH + 8.0f;
		}

		return y - rect.Y;
	}
}
