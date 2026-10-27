#include "wldpch.h"
#include "WUI/Panels/ModelPreviewPanel.h"
#include "World/Asset/AssetThumbnailCache.h"
#include "WUI/Panels/ViewportPanel.h"

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

		const int edges[12][2] = {
			{0, 1}, {1, 2}, {2, 3}, {3, 0},
			{4, 5}, {5, 6}, {6, 7}, {7, 4},
			{0, 4}, {1, 5}, {2, 6}, {3, 7}
		};

		const Wui::WuiColor boxCol = Alpha(theme.Accent, 0.85f);
		for (int i = 0; i < 12; ++i)
		{
			const int e0 = edges[i][0];
			const int e1 = edges[i][1];
			if (valid[e0] && valid[e1])
				Wui::LineSegment(ctx, sc[e0], sc[e1], boxCol, 1.5f);
		}
	}

	void ModelPreviewPanel::DrawViewportOverlay(Wui::WuiContext& ctx, const Wui::WuiRect& previewRect,
		const Wui::WuiTheme& theme)
	{
		DrawGroundGrid(ctx, previewRect, theme);
		DrawBoundsWireframe(ctx, previewRect, theme);

		// 视口悬浮工具栏 (深色半透明胶囊)
		const float btnW = 42.0f;
		const float btnH = 22.0f;
		const float gap = 4.0f;
		const float toolW = btnW * 5.0f + gap * 6.0f;
		const float toolH = btnH + 6.0f;
		const Wui::WuiRect pill { previewRect.X + 8.0f, previewRect.Y + 8.0f, toolW, toolH };
		Wui::PanelBackground(ctx, pill, Alpha(theme.PanelBg, 0.88f), 4.0f);
		Wui::HighlightOutline(ctx, pill, Alpha(theme.BorderStrong, 0.70f), 4.0f, 1.0f);

		float bx = pill.X + gap;
		const float by = pill.Y + 3.0f;

		auto drawToggle = [&](Wui::WuiId id, const std::string& label, bool& state, const std::string& tip)
		{
			const Wui::WuiRect bRect { bx, by, btnW, btnH };
			if (state)
				Wui::PanelBackground(ctx, bRect, Alpha(theme.Accent, 0.28f), 3.0f);
			if (Wui::Button(ctx, id, bRect, label, theme))
				state = !state;
			Wui::Tooltip(ctx, bRect, tip);
			bx += btnW + gap;
		};

		drawToggle(Wui::HashId("model.overlay.grid"), "Grid", m_ShowGrid,
			Wui::Tr("panel.model.overlay.grid", "Toggle 3D ground reference grid."));
		drawToggle(Wui::HashId("model.overlay.wire"), "Wire", m_ShowBounds,
			Wui::Tr("panel.model.overlay.wire", "Toggle AABB wireframe bounding box."));
		drawToggle(Wui::HashId("model.overlay.gizmo"), "Axis", m_ShowGizmo,
			Wui::Tr("panel.model.overlay.gizmo", "Toggle world coordinate axes indicator."));

		// Frame 按钮
		const Wui::WuiRect frameRect { bx, by, btnW, btnH };
		if (Wui::Button(ctx, Wui::HashId("model.overlay.frame"), frameRect, "Frame", theme))
			FramePreview();
		Wui::Tooltip(ctx, frameRect, Wui::Tr("panel.model.overlay.frame", "Frame camera to model bounds (F)."));
		bx += btnW + gap;

		// Reset 按钮
		const Wui::WuiRect resetRect { bx, by, btnW, btnH };
		if (Wui::Button(ctx, Wui::HashId("model.overlay.reset"), resetRect, "Reset", theme))
		{
			m_OrbitYaw = 0.6f;
			m_OrbitPitch = 0.25f;
			FramePreview();
		}
		Wui::Tooltip(ctx, resetRect, Wui::Tr("panel.model.overlay.reset", "Reset view orbit angles and distance."));

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
		Wui::SectionHeader(ctx, { x, y, width, 18.0f },
			Wui::Tr("panel.model.tab.materials", "Material Slots") + " (" + std::to_string(slots.size()) + ")",
			theme.Accent, theme);
		y += 24.0f;

		for (size_t index = 0; index < slots.size(); ++index)
		{
			const std::string& slot = slots[index];
			const float cardH = 92.0f;
			const Wui::WuiRect slotCard { x, y, width, cardH };
			Wui::PanelBackground(ctx, slotCard, theme.CardBg, 4.0f);
			Wui::HighlightOutline(ctx, slotCard, Alpha(theme.Border, 0.6f), 4.0f, 1.0f);

			// 槽位徽标 + 名称
			char slotBadge[32];
			std::snprintf(slotBadge, sizeof(slotBadge), "SLOT #%zu", index);
			Wui::Badge(ctx, { x + 10.0f, y + 8.0f, 62.0f, 18.0f }, slotBadge, Alpha(theme.Accent, 0.20f),
				theme.Accent, theme, theme.FontSizeCaption, true, 3.0f);

			const std::string slotName = slot.empty() ? "(empty slot)" : slot;
			Wui::Label(ctx, { x + 80.0f, y + 9.0f }, slotName, theme.TextBright, theme.FontSizeSmall);

			// 右侧直达材质编辑器按钮
			const float openW = 104.0f;
			const Wui::WuiRect openBtnRect { x + width - 10.0f - openW, y + 6.0f, openW, 22.0f };
			if (!slot.empty())
			{
				if (Wui::Button(ctx, Wui::HashId(("model.slot.open." + std::to_string(index)).c_str()),
					openBtnRect, Wui::Tr("panel.model.open_mat", "Edit Material"), theme))
				{
					host.OpenMaterialEditor(slot);
				}
				Wui::Tooltip(ctx, openBtnRect, "Open this material in the Material Editor.");
			}

			// 关联材质加载状态
			const Ref<Material> material = (index < m_SlotMaterials.size()) ? m_SlotMaterials[index] : nullptr;
			const bool missing = material == nullptr && !slot.empty();
			Wui::Label(ctx, { x + 10.0f, y + 32.0f },
				missing ? "[MISSING MATERIAL] File could not be loaded" : (slot.empty() ? "Unassigned material" : "Material Loaded OK"),
				missing ? theme.Danger : (slot.empty() ? theme.TextDisabled : theme.Success), theme.FontSizeCaption);

			// 贴图检测 (Albedo / Normal)
			if (material)
			{
				const MaterialDesc& desc = material->GetDesc();
				// Albedo
				const std::string albedoPath = desc.AlbedoTexture.empty() ? "(none)" : ShortenPath(desc.AlbedoTexture);
				const bool albedoExists = desc.AlbedoTexture.empty() || FingerprintAsset(desc.AlbedoTexture, nullptr).Exists;
				Wui::Label(ctx, { x + 10.0f, y + 52.0f }, "Albedo: " + albedoPath,
					albedoExists ? theme.TextMuted : theme.Danger, theme.FontSizeCaption);

				// Normal
				const std::string normalPath = desc.NormalTexture.empty() ? "(none)" : ShortenPath(desc.NormalTexture);
				const bool normalExists = desc.NormalTexture.empty() || FingerprintAsset(desc.NormalTexture, nullptr).Exists;
				Wui::Label(ctx, { x + 10.0f, y + 70.0f }, "Normal: " + normalPath,
					normalExists ? theme.TextMuted : theme.Danger, theme.FontSizeCaption);
			}
			else
			{
				Wui::Label(ctx, { x + 10.0f, y + 56.0f }, "No texture inspection available.",
					theme.TextDisabled, theme.FontSizeCaption);
			}

			y += cardH + 8.0f;
		}

		return y - rect.Y;
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

			const float rowH = 20.0f;
			const Wui::WuiRect rowRect { x, y, width, rowH };

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
			Wui::Label(ctx, { nodeX, y + 3.0f }, tag, tagCol, theme.FontSizeCaption);

			// 节点名称
			const std::string nameText = node.Name.empty() ? ("node_" + std::to_string(index)) : node.Name;
			Wui::Label(ctx, { nodeX + 22.0f, y + 2.0f }, nameText, theme.Text, theme.FontSizeSmall);

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

		if (!m_DataValid || m_Data.Animations.empty())
		{
			Wui::EmptyState(ctx, { x, y, width, 180.0f }, "CLIP",
				Wui::Tr("panel.model.clips.empty_title", "No Animation Clips"),
				Wui::Tr("panel.model.clips.empty_desc", "This model contains no skeletal animation sequences. Import a glTF/GLB with animations to play them here."),
				"", 0, theme);
			return 200.0f;
		}

		Wui::SectionHeader(ctx, { x, y, width, 18.0f },
			Wui::Tr("panel.model.tab.clips", "Animation Clips") + " (" + std::to_string(m_Data.Animations.size()) + ")",
			theme.Accent, theme);
		y += 24.0f;

		for (size_t index = 0; index < m_Data.Animations.size(); ++index)
		{
			const Asset::WModelAnimation& anim = m_Data.Animations[index];
			const bool isActive = static_cast<int>(index) == m_AnimClipIndex;

			const float cardH = 56.0f;
			const Wui::WuiRect clipCard { x, y, width, cardH };
			Wui::PanelBackground(ctx, clipCard, theme.CardBg, 4.0f);
			Wui::HighlightOutline(ctx, clipCard, isActive ? theme.Accent : Alpha(theme.Border, 0.6f), 4.0f,
				isActive ? 1.5f : 1.0f);

			// 状态徽标
			if (isActive)
			{
				Wui::Badge(ctx, { x + 8.0f, y + 8.0f, 54.0f, 18.0f }, "PLAYING", Alpha(theme.Accent, 0.25f),
					theme.Accent, theme, theme.FontSizeCaption, true, 3.0f);
			}
			else
			{
				Wui::Badge(ctx, { x + 8.0f, y + 8.0f, 42.0f, 18.0f }, "CLIP", Alpha(theme.TextDisabled, 0.20f),
					theme.TextMuted, theme, theme.FontSizeCaption, true, 3.0f);
			}

			// 片段名称
			const std::string clipTitle = anim.Name.empty() ? ("clip_" + std::to_string(index)) : anim.Name;
			Wui::Label(ctx, { x + 70.0f, y + 9.0f }, clipTitle, isActive ? theme.TextBright : theme.Text,
				theme.FontSizeSmall);

			// 时长与帧数
			const int frames = static_cast<int>(std::round(anim.Duration * 30.0f));
			char durStr[96];
			std::snprintf(durStr, sizeof(durStr), "%.2fs  |  %d frames @ 30 FPS", anim.Duration, frames);
			Wui::Label(ctx, { x + 8.0f, y + 32.0f }, durStr, theme.TextMuted, theme.FontSizeCaption);

			// 右侧操作按钮组 (Play / Open in AnimGraph)
			const float playBtnW = 60.0f;
			const float graphBtnW = 110.0f;
			const float bx = x + width - playBtnW - graphBtnW - 16.0f;

			if (Wui::Button(ctx, Wui::HashId(("model.clip.play." + std::to_string(index)).c_str()),
				{ bx, y + 16.0f, playBtnW, 24.0f }, isActive ? "Restart" : "Play", theme))
			{
				m_AnimClipIndex = static_cast<int>(index);
				m_AnimTime = 0.0f;
				m_AnimPlaying = true;
			}

			if (Wui::Button(ctx, Wui::HashId(("model.clip.open_graph." + std::to_string(index)).c_str()),
				{ bx + playBtnW + 6.0f, y + 16.0f, graphBtnW, 24.0f }, "Open in Graph", theme))
			{
				host.FocusIndependentWindow("animgraph");
				host.ActivatePanel("animgraph");
			}
			Wui::Tooltip(ctx, { bx + playBtnW + 6.0f, y + 16.0f, graphBtnW, 24.0f },
				"Switch to Animation Graph editor.");

			y += cardH + 8.0f;
		}

		return y - rect.Y;
	}
}
