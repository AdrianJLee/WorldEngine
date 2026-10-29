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
#include "World/Asset/WSkeletonIO.h"
#include "World/Asset/WAnimationClipIO.h"
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
		const std::string ext = LowerExtension(std::filesystem::path(m_LogicalPath));
		if (ext == ".wskeleton")
		{
			m_PanelTitle = "Skeleton - " + (name.empty() ? m_LogicalPath : name);
			m_ActiveTab = 2;
			m_ShowBones = true;
		}
		else if (ext == ".wclip")
		{
			m_PanelTitle = "Clip - " + (name.empty() ? m_LogicalPath : name);
			m_ActiveTab = 4;
			m_ShowBones = true;
			m_AnimPlaying = true;
		}
		else
		{
			m_PanelTitle = "Model - " + (name.empty() ? m_LogicalPath : name);
		}
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
		{
			if (m_DataValid && !m_Data.Nodes.empty())
			{
				bool any = false;
				glm::vec3 boundsMin { 0.0f };
				glm::vec3 boundsMax { 0.0f };
				const size_t nodeCount = m_Data.Nodes.size();
				std::vector<glm::mat4> world(nodeCount, glm::mat4(1.0f));
				for (size_t index = 0; index < nodeCount; ++index)
				{
					const auto& node = m_Data.Nodes[index];
					const glm::mat4 local = glm::translate(glm::mat4(1.0f), node.Translation)
						* glm::mat4_cast(node.Rotation)
						* glm::scale(glm::mat4(1.0f), node.Scale);
					world[index] = (node.Parent >= 0 && static_cast<size_t>(node.Parent) < index)
						? world[static_cast<size_t>(node.Parent)] * local : local;
					const glm::vec3 pos = glm::vec3(world[index][3]);
					if (!any)
					{
						boundsMin = boundsMax = pos;
						any = true;
					}
					else
					{
						boundsMin = glm::min(boundsMin, pos);
						boundsMax = glm::max(boundsMax, pos);
					}
				}
				if (any)
				{
					boundsMin -= glm::vec3(0.25f);
					boundsMax += glm::vec3(0.25f);
					result.Min = boundsMin;
					result.Max = boundsMax;
				}
			}
			return result;
		}
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
		m_Mesh.reset();
		m_Data = Asset::WModelData();
		m_DataValid = false;

		const std::string ext = LowerExtension(std::filesystem::path(m_LogicalPath));
		const std::string name = std::filesystem::path(m_LogicalPath).filename().string();

		if (ext == ".wskeleton")
		{
			m_PanelTitle = "Skeleton - " + (name.empty() ? m_LogicalPath : name);
			std::string error;

			std::string previewPath = m_PreviewMeshPath;
			if (previewPath.empty())
			{
				std::filesystem::path skelP(m_LogicalPath);
				std::string stem = skelP.stem().string();
				std::filesystem::path candidate = skelP.parent_path() / (stem + ".wmodel");
				if (std::filesystem::exists(candidate))
					previewPath = candidate.generic_string();
				else
					previewPath = "engine/models/characters/mannequin.wmodel";
			}
			m_PreviewMeshPath = previewPath;

			std::string meshError;
			m_Mesh = Mesh::LoadWModel(previewPath, &meshError);
			if (m_Mesh && Asset::WModelIO::ReadFile(previewPath, m_Data, &meshError))
			{
				m_DataValid = true;
				m_SlotMaterials.resize(m_Mesh->GetMaterialSlots().size());
				for (size_t idx = 0; idx < m_Mesh->GetMaterialSlots().size(); ++idx)
				{
					const std::string& slotPath = m_Mesh->GetMaterialSlots()[idx];
					if (!slotPath.empty())
						m_SlotMaterials[idx] = MaterialLibrary::Get().Load(slotPath);
				}
			}
			else
			{
				Asset::WSkeletonData skeleton;
				if (Asset::WSkeletonIO::ReadFile(m_LogicalPath, skeleton, &error))
				{
					m_Data.Nodes.clear();
					m_Data.Nodes.resize(skeleton.Bones.size());
					for (size_t i = 0; i < skeleton.Bones.size(); ++i)
					{
						const auto& bone = skeleton.Bones[i];
						m_Data.Nodes[i].Name = bone.NameStr.empty() ? StringPool::Get().NameOf(bone.Name) : bone.NameStr;
						m_Data.Nodes[i].Parent = bone.ParentIndex;
						m_Data.Nodes[i].MeshIndex = -1;
						m_Data.Nodes[i].Translation = bone.BindPoseLocal.Translation;
						m_Data.Nodes[i].Rotation = bone.BindPoseLocal.Rotation;
						m_Data.Nodes[i].Scale = bone.BindPoseLocal.Scale;
					}
					m_DataValid = true;
				}
			}

			m_ShowBones = true;
			m_ShowMesh = true;
			m_ActiveTab = 2;

			const MeshBounds bounds = ComputeWorldBounds();
			m_Focus = bounds.GetCenter();
			const float radius = std::max(0.25f, glm::length(bounds.GetExtents()));
			m_MinDistance = radius * 0.6f;
			m_MaxDistance = radius * 40.0f;
			m_CameraDistance = radius * 3.2f;

			m_Status = "Loaded Skeleton " + m_LogicalPath;
			m_StatusIsError = false;
			WLD_CORE_INFO("[skeleton] preview '{0}'", m_LogicalPath);
			return;
		}

		if (ext == ".wclip")
		{
			m_PanelTitle = "Clip - " + (name.empty() ? m_LogicalPath : name);
			std::string error;
			Asset::WAnimationClipData clipData;
			if (Asset::WAnimationClipIO::ReadFile(m_LogicalPath, clipData, &error))
			{
				std::string previewPath = m_PreviewMeshPath;
				if (previewPath.empty())
				{
					std::string skelPath = clipData.SkeletonPath;
					if (!skelPath.empty())
					{
						std::filesystem::path skP(skelPath);
						std::filesystem::path cand = skP.parent_path() / (skP.stem().string() + ".wmodel");
						if (std::filesystem::exists(cand))
							previewPath = cand.generic_string();
					}
					if (previewPath.empty())
						previewPath = "engine/models/characters/mannequin.wmodel";
				}
				m_PreviewMeshPath = previewPath;

				std::string meshError;
				m_Mesh = Mesh::LoadWModel(previewPath, &meshError);
				if (m_Mesh && Asset::WModelIO::ReadFile(previewPath, m_Data, &meshError))
				{
					m_DataValid = true;
					m_SlotMaterials.resize(m_Mesh->GetMaterialSlots().size());
					for (size_t idx = 0; idx < m_Mesh->GetMaterialSlots().size(); ++idx)
					{
						const std::string& slotPath = m_Mesh->GetMaterialSlots()[idx];
						if (!slotPath.empty())
							m_SlotMaterials[idx] = MaterialLibrary::Get().Load(slotPath);
					}
				}
				else
				{
					m_Data.Nodes.clear();
					for (const auto& ch : clipData.Channels)
					{
						Asset::WModelNode node;
						node.Name = ch.BoneNameStr.empty() ? StringPool::Get().NameOf(ch.BoneName) : ch.BoneNameStr;
						node.Parent = m_Data.Nodes.empty() ? -1 : 0;
						m_Data.Nodes.push_back(std::move(node));
					}
					m_DataValid = true;
				}

				m_Data.Animations.clear();
				Asset::WModelAnimation anim;
				anim.Name = clipData.Name.empty() ? std::filesystem::path(m_LogicalPath).stem().string() : clipData.Name;
				anim.Duration = std::max(0.05f, clipData.Duration);

				for (const auto& ch : clipData.Channels)
				{
					int32_t targetNode = -1;
					const std::string boneName = ch.BoneNameStr.empty() ? StringPool::Get().NameOf(ch.BoneName) : ch.BoneNameStr;
					for (size_t n = 0; n < m_Data.Nodes.size(); ++n)
					{
						if (m_Data.Nodes[n].Name == boneName)
						{
							targetNode = static_cast<int32_t>(n);
							break;
						}
					}
					if (targetNode < 0)
						continue;

					if (!ch.PositionKeys.empty())
					{
						Asset::WModelAnimationChannel ac;
						ac.TargetNode = static_cast<uint32_t>(targetNode);
						ac.Path = Asset::WModelIO::WModelAnimationPath::Translation;
						ac.Keys.reserve(ch.PositionKeys.size());
						for (const auto& k : ch.PositionKeys)
							ac.Keys.push_back({ k.Time, glm::vec4(k.Value, 0.0f) });
						anim.Channels.push_back(std::move(ac));
					}
					if (!ch.RotationKeys.empty())
					{
						Asset::WModelAnimationChannel ac;
						ac.TargetNode = static_cast<uint32_t>(targetNode);
						ac.Path = Asset::WModelIO::WModelAnimationPath::Rotation;
						ac.Keys.reserve(ch.RotationKeys.size());
						for (const auto& k : ch.RotationKeys)
							ac.Keys.push_back({ k.Time, glm::vec4(k.Value.x, k.Value.y, k.Value.z, k.Value.w) });
						anim.Channels.push_back(std::move(ac));
					}
					if (!ch.ScaleKeys.empty())
					{
						Asset::WModelAnimationChannel ac;
						ac.TargetNode = static_cast<uint32_t>(targetNode);
						ac.Path = Asset::WModelIO::WModelAnimationPath::Scale;
						ac.Keys.reserve(ch.ScaleKeys.size());
						for (const auto& k : ch.ScaleKeys)
							ac.Keys.push_back({ k.Time, glm::vec4(k.Value, 1.0f) });
						anim.Channels.push_back(std::move(ac));
					}
				}
				m_Data.Animations.push_back(std::move(anim));

				m_AnimPlaying = true;
				m_AnimClipIndex = 0;
				m_ShowMesh = true;
				m_ShowBones = false;
				m_ActiveTab = 4;

				const MeshBounds bounds = ComputeWorldBounds();
				m_Focus = bounds.GetCenter();
				const float radius = std::max(0.25f, glm::length(bounds.GetExtents()));
				m_MinDistance = radius * 0.6f;
				m_MaxDistance = radius * 40.0f;
				m_CameraDistance = radius * 3.2f;

				m_Status = "Loaded Clip " + m_LogicalPath + " | duration " + std::to_string(clipData.Duration) + "s";
				m_StatusIsError = false;
				WLD_CORE_INFO("[clip] preview '{0}': duration={1} channels={2}", m_LogicalPath, clipData.Duration, clipData.Channels.size());
				return;
			}
			else
			{
				m_Status = "Clip load failed: " + (error.empty() ? m_LogicalPath : error);
				m_StatusIsError = true;
				WLD_CORE_WARN("[clip] preview load failed '{0}': {1}", m_LogicalPath, error);
				return;
			}
		}

		m_PreviewLODIndex = 0;
		for (int i = 1; i <= 8; ++i)
			m_LODMeshes[i].reset();
		m_GeneratedLODs.clear();
		m_LODStatusMessage.clear();

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
		Ref<Mesh> activeMesh = m_Mesh;
		if (m_PreviewLODIndex >= 1 && m_PreviewLODIndex <= 8 && m_LODMeshes[m_PreviewLODIndex])
			activeMesh = m_LODMeshes[m_PreviewLODIndex];

		if (!activeMesh && !(m_DataValid && !m_Data.Nodes.empty()))
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
		const uint32_t slotBase = activeMesh ? Renderer3D::ReserveSlotBase(
			static_cast<uint32_t>(Wui::HashId(m_PanelId.c_str()) ^ 0x5F17u),
			static_cast<uint32_t>(std::max<size_t>(1, activeMesh->GetSubmeshes().size()))) : 0;
		if (activeMesh && m_ShowMesh)
		{
		uint32_t drawIndex = 0;
		const std::vector<MeshNode>& nodes = activeMesh->GetNodes();
		const std::vector<MeshRange>& meshes = activeMesh->GetMeshes();
		const std::vector<MeshSubmesh>& submeshes = activeMesh->GetSubmeshes();

		// D5c-4b:布局 2(蒙皮)的 mesh 一律走蒙皮提交 —— 静态管线按 32B stride 读 64B 顶点会画乱几何。
		// 调色板按当前 m_AnimTime 算(暂停/未播放时就是当前时间的姿态),按 mesh 下标缓存一份。
		const bool skinnedLayout = activeMesh->GetVertexLayoutId() == Mesh::kVertexLayoutSkinned;
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

		const std::vector<std::string>& materialSlots = activeMesh->GetMaterialSlots();
		const auto submitSubmesh = [&](int32_t meshIndex, uint32_t submeshIndex, const glm::mat4& transform)
		{
			const int32_t slot = (submeshIndex < submeshes.size()) ? submeshes[submeshIndex].MaterialSlot : -1;
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
				const uint32_t result = Renderer3D::SubmitSkinnedAtSlot(slotBase + drawIndex, activeMesh,
					submeshIndex, material, transform, palette.data(),
					static_cast<uint32_t>(palette.size()), -1);
				if (result == UINT32_MAX && !m_SkinnedSubmitWarned)
				{
					m_SkinnedSubmitWarned = true;
					WLD_CORE_WARN("[model] preview skinned submit failed for '{0}' submesh {1} "
						"(layout={2} joints={3});该 submesh 未绘制",
						m_LogicalPath, submeshIndex, activeMesh->GetVertexLayoutId(), palette.size());
				}
				++drawIndex;
				return;
			}
			Renderer3D::SubmitSubmeshAtSlot(slotBase + drawIndex, activeMesh, submeshIndex, material, transform, -1);
			++drawIndex;
		};

		if (nodes.empty())
		{
			// 没有节点树(理论上 v1 格式总有):退化为整体绘制。
			if (!activeMesh->HasSubmeshes())
			{
				const bool skinned = skinnedLayout && !skinPalettes.empty() && !skinPalettes[0].empty();
				if (skinned)
				{
					const std::vector<glm::mat4>& palette = skinPalettes[0];
					const uint32_t result = Renderer3D::SubmitSkinnedAtSlot(slotBase, activeMesh, UINT32_MAX,
						nullptr, glm::mat4(1.0f), palette.data(), static_cast<uint32_t>(palette.size()), -1);
					if (result == UINT32_MAX && !m_SkinnedSubmitWarned)
					{
						m_SkinnedSubmitWarned = true;
						WLD_CORE_WARN("[model] preview whole-mesh skinned submit failed for '{0}'", m_LogicalPath);
					}
				}
				else if (!skinnedLayout)
				{
					Renderer3D::SubmitAtSlot(slotBase, activeMesh, nullptr, glm::mat4(1.0f), -1);
				}
				else if (!m_SkinnedSubmitWarned)
				{
					// 布局 2 但没有可用调色板(容器数据不完整):静态管线按 32B stride 读 64B 顶点会画乱,宁可跳过。
					m_SkinnedSubmitWarned = true;
					WLD_CORE_WARN("[model] preview skips skinned whole-mesh '{0}':调色板不可用(layout={1})",
						m_LogicalPath, activeMesh->GetVertexLayoutId());
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
		PollAsyncLODTasks(host);

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
		const bool showAnimControls = m_DataValid && (HasSkinnedMesh() || !m_Data.Animations.empty());
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

			if (!ctx.IsTextInputActive() && ctx.WasKeyPressed(KeyCodes::K))
			{
				if (ctx.Input().Shift)
					RecordKeyframeForAllNodes(m_AnimTime);
				else if (m_SelectedNode >= 0)
					RecordKeyframeForNode(static_cast<uint32_t>(m_SelectedNode), m_AnimTime);
			}

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
		const std::string ext = LowerExtension(std::filesystem::path(m_LogicalPath));
		const char* badgeText = (ext == ".wskeleton") ? "SKEL" : ((ext == ".wclip") ? "CLIP" : "MODEL");
		const float badgeW = (ext == ".wclip") ? 38.0f : ((ext == ".wskeleton") ? 42.0f : 46.0f);
		Wui::Badge(ctx, { x, y + 2.0f, badgeW, 20.0f }, badgeText, Alpha(theme.Accent, 0.22f), theme.Accent,
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

		// Preview Mesh 下拉选取 (仅 Skeleton 与 Clip 显示)
		if (ext == ".wclip" || ext == ".wskeleton")
		{
			const std::vector<std::string>& modelAssets = Editor::AssetCatalog::PathsFor(EditorAssetKind::Model);
			int modelIdx = 0;
			const auto foundModel = std::find(modelAssets.begin(), modelAssets.end(), m_PreviewMeshPath);
			if (foundModel != modelAssets.end())
				modelIdx = static_cast<int>(std::distance(modelAssets.begin(), foundModel));

			Wui::Label(ctx, { x, y + 4.0f }, "Preview Mesh:", theme.TextMuted, theme.FontSizeCaption);
			x += 76.0f;
			const float comboMeshW = 140.0f;
			std::string shownMeshName = std::filesystem::path(m_PreviewMeshPath).stem().string();
			if (shownMeshName.empty()) shownMeshName = "(none)";
			if (Wui::Combo(ctx, Wui::HashId("model.preview.mesh.combo"), { x, y, comboMeshW, 22.0f }, "",
				modelAssets, modelIdx, theme, shownMeshName))
			{
				if (modelIdx >= 0 && static_cast<size_t>(modelIdx) < modelAssets.size())
				{
					m_PreviewMeshPath = modelAssets[static_cast<size_t>(modelIdx)];
					Reload();
				}
			}
			x += comboMeshW + 4.0f;
			if (Wui::Button(ctx, Wui::HashId("model.preview.mesh.browse"), { x, y, 26.0f, 22.0f }, "...", theme))
			{
				host.OpenAssetPicker("选择预览网格模型 (Model)", AssetCategory::Unknown, [this](const std::string& picked) {
					m_PreviewMeshPath = picked;
					Reload();
				});
			}
			x += 32.0f;
		}

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

		struct ScreenJoint
		{
			glm::vec2 Pos { 0.0f };
			bool Valid = false;
		};
		std::vector<ScreenJoint> sc;
		bool clickingOverlayUi = false;

		// 3D 骨骼层级线框与关节点可视化 (m_ShowBones)
		if (m_ShowBones && m_DataValid && !m_Data.Nodes.empty())
		{
			sc.resize(m_Data.Nodes.size());
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
				sc[i] = { screenPos, true };

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
			if (ctx.IsHovered(miniPill)) clickingOverlayUi = true;
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
			const float lodBtnW = 60.0f;
			const float frameBtnW = 50.0f;
			const float colBtnW = 24.0f;
			const float toolW = dispBtnW + camBtnW + lodBtnW + frameBtnW + colBtnW + gap * 6.0f;
			const float toolH = btnH + 6.0f;
			const Wui::WuiRect pill { previewRect.X + 8.0f, previewRect.Y + 8.0f, toolW, toolH };
			if (ctx.IsHovered(pill)) clickingOverlayUi = true;
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

			// LOD 实时渲染档位快捷切换
			std::string lodPillText = "Base (LOD 0)";
			if (m_PreviewLODIndex >= 1 && m_PreviewLODIndex <= 8)
				lodPillText = "LOD " + std::to_string(m_PreviewLODIndex);

			const Wui::WuiRect lodPillRect { bx, by, lodBtnW, btnH };
			auto lodPillTheme = theme;
			if (m_PreviewLODIndex > 0)
			{
				lodPillTheme.ButtonBg = Alpha(theme.Accent, 0.28f);
				lodPillTheme.Border = theme.Accent;
			}
			if (Wui::Button(ctx, Wui::HashId("model.overlay.lod_toggle"), lodPillRect, lodPillText, lodPillTheme))
			{
				const int maxLOD = std::clamp(static_cast<int>(m_LODConfigs.size()), 1, 8);
				m_PreviewLODIndex = (m_PreviewLODIndex + 1) % (maxLOD + 1);
			}
			Wui::Tooltip(ctx, lodPillRect, "切换视口当前渲染的 LOD 档位 (Base -> LOD 1 -> ...)");
			bx += lodBtnW + gap;

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
				drawCheck(Wui::HashId("model.disp.mesh"), Wui::Tr("panel.model.overlay.mesh", "Mesh"), m_ShowMesh);
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
		if (ctx.IsHovered(infoBadge)) clickingOverlayUi = true;
		Wui::PanelBackground(ctx, infoBadge, Alpha(theme.PanelBg, 0.75f), 3.0f);
		Wui::Label(ctx, { infoBadge.X + 6.0f, infoBadge.Y + 3.0f }, statsPill, theme.TextDisabled, theme.FontSizeCaption);

		// 视口右下角实时 LOD 统计信息徽标 (档位 + 三角面数 + 顶点数)
		Ref<Mesh> activeMeshForStats = m_Mesh;
		std::string activeLODName = "Base (LOD 0)";
		if (m_PreviewLODIndex >= 1 && m_PreviewLODIndex <= 8 && m_LODMeshes[m_PreviewLODIndex])
		{
			activeMeshForStats = m_LODMeshes[m_PreviewLODIndex];
			char buf[64];
			const float ratio = (m_PreviewLODIndex - 1 < static_cast<int>(m_LODConfigs.size()))
				? m_LODConfigs[m_PreviewLODIndex - 1].Ratio * 100.0f : 0.0f;
			std::snprintf(buf, sizeof(buf), "LOD %d (%.0f%%)", m_PreviewLODIndex, ratio);
			activeLODName = buf;
		}

		if (activeMeshForStats)
		{
			const uint32_t triCount = activeMeshForStats->GetIndexCount() / 3;
			const uint32_t vertCount = activeMeshForStats->GetVertexCount();
			std::string lodBadgeText = "[" + activeLODName + ": " + FormatNumber(triCount) + " tris | " + FormatNumber(vertCount) + " v]";
			const float lodBadgeW = ctx.MeasureTextWidth(lodBadgeText, theme.FontSizeCaption) + 14.0f;
			const float badgeX = previewRect.X + previewRect.W - lodBadgeW - 8.0f;
			const float badgeY = previewRect.Y + previewRect.H - 22.0f - (m_ShowGizmo ? 74.0f : 0.0f);
			const Wui::WuiRect lodBadgeRect { badgeX, badgeY, lodBadgeW, 18.0f };
			if (ctx.IsHovered(lodBadgeRect)) clickingOverlayUi = true;

			Wui::WuiColor badgeBorder = (m_PreviewLODIndex == 0)
				? theme.Accent
				: Wui::WuiColor{ 0.95f, std::max(0.2f, 0.85f - m_PreviewLODIndex * 0.08f), 0.10f + m_PreviewLODIndex * 0.08f, 1.0f };
			Wui::PanelBackground(ctx, lodBadgeRect, Alpha(theme.PanelBg, 0.85f), 3.0f);
			Wui::HighlightOutline(ctx, lodBadgeRect, Alpha(badgeBorder, 0.70f), 3.0f, 1.0f);
			Wui::Label(ctx, { lodBadgeRect.X + 7.0f, lodBadgeRect.Y + 3.0f }, lodBadgeText, badgeBorder, theme.FontSizeCaption);
		}

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
			if (ctx.IsHovered(axisRect)) clickingOverlayUi = true;
		}

		// 3D 视口关节点击选中 (Viewport Click to Select)
		if (m_ShowBones && m_DataValid && !sc.empty() && ctx.Input().MouseClicked[0]
			&& ctx.IsHovered(previewRect) && !clickingOverlayUi)
		{
			const glm::vec2 mousePos = ctx.Input().MousePos;
			float minDistance = 14.0f;
			int closestIndex = -1;
			for (size_t i = 0; i < sc.size(); ++i)
			{
				if (!sc[i].Valid)
					continue;
				const float dist = glm::distance(mousePos, sc[i].Pos);
				if (dist <= minDistance)
				{
					minDistance = dist;
					closestIndex = static_cast<int>(i);
				}
			}
			if (closestIndex >= 0)
			{
				m_SelectedNode = closestIndex;
				if (static_cast<size_t>(closestIndex) < m_Data.Nodes.size())
				{
					const auto& node = m_Data.Nodes[static_cast<size_t>(closestIndex)];
					m_SelectedNodeNameBuffer = node.Name;
					m_SelectedNodeParent = node.Parent;
					m_SelectedNodeTranslation = node.Translation;
					m_SelectedNodeRotationEuler = glm::degrees(glm::eulerAngles(node.Rotation));
					m_SelectedNodeScale = node.Scale;
				}
			}
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

		// 打关键帧按钮组: [Key Bone (K)] [Key All (Shift+K)]
		const float keySelW = 90.0f;
		const float keyAllW = 105.0f;
		const bool canKeySel = m_SelectedNode >= 0 && static_cast<size_t>(m_SelectedNode) < m_Data.Nodes.size();

		const Wui::WuiRect keySelRect { bx, curY, keySelW, btnH };
		if (Wui::Button(ctx, Wui::HashId("model.anim.key_selected"), keySelRect,
			Wui::Tr("panel.model.anim.key_selected", "Key Bone (K)"), theme))
		{
			if (canKeySel)
				RecordKeyframeForNode(static_cast<uint32_t>(m_SelectedNode), m_AnimTime);
		}
		Wui::Tooltip(ctx, keySelRect, Wui::Tr("panel.model.anim.key_selected_tip", "Record keyframe for selected bone at current time (K)"));
		bx += keySelW + 4.0f;

		const Wui::WuiRect keyAllRect { bx, curY, keyAllW, btnH };
		if (Wui::Button(ctx, Wui::HashId("model.anim.key_all"), keyAllRect,
			Wui::Tr("panel.model.anim.key_all", "Key All (Shift+K)"), theme))
		{
			RecordKeyframeForAllNodes(m_AnimTime);
		}
		Wui::Tooltip(ctx, keyAllRect, Wui::Tr("panel.model.anim.key_all_tip", "Record keyframe for all skeleton nodes at current time (Shift+K)"));
		bx += keyAllW + 12.0f;

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
			Wui::Tr("panel.model.tab.clips", "Clips") + (m_DataValid && !m_Data.Animations.empty() ? " (" + std::to_string(m_Data.Animations.size()) + ")" : ""),
			"LOD" + (m_GeneratedLODs.empty() ? "" : " (" + std::to_string(m_GeneratedLODs.size()) + ")")
		};

		Wui::TabBar(ctx, Wui::HashId("model.tabs"), tabArea, tabLabels, m_ActiveTab, theme);

		const float contentY = rect.Y + tabH + 6.0f;
		const float contentH = std::max(20.0f, rect.H - tabH - 6.0f);
		const Wui::WuiRect contentRect { rect.X, contentY, rect.W, contentH };

		const float scrollContentH = m_TabContentHeight[m_ActiveTab];

		Wui::BeginScrollArea(ctx, contentRect, scrollContentH, m_TabScroll[m_ActiveTab], theme);

		const Wui::WuiRect scrollSlice { rect.X, contentY - m_TabScroll[m_ActiveTab], rect.W - 14.0f, contentH };
		float actualH = 0.0f;
		if (m_ActiveTab == 0) actualH = DrawTabOverview(ctx, scrollSlice, host, theme);
		else if (m_ActiveTab == 1) actualH = DrawTabMaterials(ctx, scrollSlice, host, theme);
		else if (m_ActiveTab == 2) actualH = DrawTabSkeleton(ctx, scrollSlice, host, theme);
		else if (m_ActiveTab == 3) actualH = DrawTabImportSettings(ctx, scrollSlice, theme);
		else if (m_ActiveTab == 4) actualH = DrawTabClips(ctx, scrollSlice, host, theme);
		else if (m_ActiveTab == 5) actualH = DrawTabLOD(ctx, scrollSlice, host, theme);

		Wui::EndScrollArea(ctx);

		m_TabContentHeight[m_ActiveTab] = actualH;

		// 显式绘制滚动条
		if (actualH > contentH + 1.0f)
		{
			const float barW = 6.0f;
			const Wui::WuiRect barRect { rect.X + rect.W - barW - 2.0f, contentY, barW, contentH };
			Wui::ScrollBar(ctx, Wui::HashId(("model.tab.scrollbar." + std::to_string(m_ActiveTab)).c_str()),
				barRect, actualH, contentH, m_TabScroll[m_ActiveTab], theme, false);
		}
	}
	void ModelPreviewPanel::PollAsyncLODTasks(PanelHost& host)
	{
		if (m_AsyncGenRunning && m_AsyncGenFuture.valid())
		{
			if (m_AsyncGenFuture.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready)
			{
				auto res = m_AsyncGenFuture.get();
				m_AsyncGenRunning = false;
				if (res.Success)
				{
					m_GeneratedLODs = std::move(res.Assets);
					for (int i = 1; i <= 8; ++i)
						m_LODMeshes[i].reset();

					for (const auto& gen : m_GeneratedLODs)
					{
						if (gen.Level >= 1 && gen.Level <= 8)
						{
							std::string buildErr;
							m_LODMeshes[gen.Level] = Mesh::BuildFromWModel(gen.ModelData, gen.RelativePath, &buildErr);
						}
					}

					std::string summary;
					for (size_t i = 0; i < m_GeneratedLODs.size(); ++i)
					{
						if (i > 0) summary += ", ";
						char b[64];
						std::snprintf(b, sizeof(b), "LOD%u: %u tris (%.1f%%)",
							m_GeneratedLODs[i].Level, m_GeneratedLODs[i].TriangleCount, m_GeneratedLODs[i].ActualRatio * 100.0f);
						summary += b;
					}
					m_LODStatusMessage = "自动生成完成: " + summary;
					m_LODStatusIsError = false;
					m_Status = m_LODStatusMessage;
					m_StatusIsError = false;
					if (!m_GeneratedLODs.empty())
						m_PreviewLODIndex = 1;
					host.Notify("LOD 自动减面完成，已在视口切换至 LOD1 预览");
				}
				else
				{
					m_LODStatusMessage = "自动生成失败: " + res.Error;
					m_LODStatusIsError = true;
					m_Status = m_LODStatusMessage;
					m_StatusIsError = true;
					host.Notify(m_LODStatusMessage);
				}
			}
		}

		if (m_AsyncSaveRunning && m_AsyncSaveFuture.valid())
		{
			if (m_AsyncSaveFuture.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready)
			{
				auto res = m_AsyncSaveFuture.get();
				m_AsyncSaveRunning = false;
				if (res.Success)
				{
					std::string msg = "已成功保存 LOD 资产: ";
					for (size_t i = 0; i < res.SavedPaths.size(); ++i)
					{
						if (i > 0) msg += ", ";
						msg += res.SavedPaths[i];
						host.RefreshAssetCatalog(res.SavedPaths[i]);
					}
					m_LODStatusMessage = msg;
					m_LODStatusIsError = false;
					m_Status = msg;
					m_StatusIsError = false;
					host.Notify(msg);
				}
				else
				{
					m_LODStatusMessage = "保存 LOD 资产失败: " + res.Error;
					m_LODStatusIsError = true;
					m_Status = m_LODStatusMessage;
					m_StatusIsError = true;
					host.Notify(m_LODStatusMessage);
				}
			}
		}
	}

	float ModelPreviewPanel::DrawTabLOD(Wui::WuiContext& ctx, const Wui::WuiRect& rect, PanelHost& host,
		const Wui::WuiTheme& theme)
	{
		PollAsyncLODTasks(host);

		const float x = rect.X;
		const float width = rect.W;
		float y = rect.Y + 4.0f;

		if (!m_Mesh)
		{
			Wui::Label(ctx, { x, y }, "No mesh loaded.", theme.TextMuted, theme.FontSizeBody);
			return 24.0f;
		}

		// 异步进行中反馈横幅
		if (m_AsyncGenRunning)
		{
			const Wui::WuiRect bannerRect { x, y, width, 26.0f };
			Wui::PanelBackground(ctx, bannerRect, Wui::WuiColor{ 0.12f, 0.22f, 0.35f, 0.90f }, 3.0f);
			Wui::HighlightOutline(ctx, bannerRect, theme.Accent, 3.0f, 1.0f);
			const auto now = std::chrono::high_resolution_clock::now();
			const double elapsed = std::chrono::duration<double>(now - m_AsyncGenStartTime).count();
			char b[128];
			std::snprintf(b, sizeof(b), "[后台减面中...] 正在基于 QEM 算法异步生成各级网格 (%.1fs，UI 60+ FPS)", elapsed);
			Wui::Label(ctx, { bannerRect.X + 8.0f, bannerRect.Y + 5.0f }, b, theme.Accent, theme.FontSizeSmall);
			y += 32.0f;
		}
		else if (m_AsyncSaveRunning)
		{
			const Wui::WuiRect bannerRect { x, y, width, 26.0f };
			Wui::PanelBackground(ctx, bannerRect, Wui::WuiColor{ 0.12f, 0.22f, 0.35f, 0.90f }, 3.0f);
			Wui::HighlightOutline(ctx, bannerRect, theme.Accent, 3.0f, 1.0f);
			const auto now = std::chrono::high_resolution_clock::now();
			const double elapsed = std::chrono::duration<double>(now - m_AsyncSaveStartTime).count();
			char b[128];
			std::snprintf(b, sizeof(b), "[后台保存中...] 正在异步写出全部 LOD 资产文件 (%.1fs，UI 60+ FPS)", elapsed);
			Wui::Label(ctx, { bannerRect.X + 8.0f, bannerRect.Y + 5.0f }, b, theme.Accent, theme.FontSizeSmall);
			y += 32.0f;
		}
		else if (!m_LODStatusMessage.empty())
		{
			const Wui::WuiRect bannerRect { x, y, width, 26.0f };
			const Wui::WuiColor bgCol = m_LODStatusIsError
				? Wui::WuiColor{ 0.28f, 0.08f, 0.08f, 0.90f }
				: Wui::WuiColor{ 0.08f, 0.25f, 0.12f, 0.90f };
			const Wui::WuiColor borderCol = m_LODStatusIsError
				? theme.Danger
				: theme.Success;
			Wui::PanelBackground(ctx, bannerRect, bgCol, 3.0f);
			Wui::HighlightOutline(ctx, bannerRect, borderCol, 3.0f, 1.0f);
			Wui::Label(ctx, { bannerRect.X + 8.0f, bannerRect.Y + 5.0f }, m_LODStatusMessage, borderCol, theme.FontSizeSmall);
			y += 32.0f;
		}

		// 1. 基准模型几何状态 (Geometry Statistics)
		Wui::SectionHeader(ctx, { x, y, width, 18.0f },
			Wui::Tr("panel.model.lod.metrics", "Base Model Geometry Statistics"), theme.Accent, theme);
		y += 24.0f;

		const float colGap = 8.0f;
		const float colW = std::max(60.0f, (width - colGap * 3.0f) * 0.25f);
		const float cardH = 48.0f;

		auto drawStatCard = [&](float cx, const std::string& label, const std::string& val, const Wui::WuiColor& valCol)
		{
			const Wui::WuiRect cRect { cx, y, colW, cardH };
			Wui::PanelBackground(ctx, cRect, theme.CardBg, 4.0f);
			Wui::HighlightOutline(ctx, cRect, theme.Border, 4.0f, 1.0f);
			Wui::Label(ctx, { cRect.X + 8.0f, cRect.Y + 6.0f }, label, theme.TextMuted, theme.FontSizeCaption);
			Wui::Label(ctx, { cRect.X + 8.0f, cRect.Y + 22.0f }, val, valCol, theme.FontSizeHeading);
		};

		drawStatCard(x, "Submeshes", std::to_string(m_Mesh->GetSubmeshes().size()), theme.Text);
		drawStatCard(x + (colW + colGap), "Base Vertices", FormatNumber(m_Mesh->GetVertexCount()), theme.Text);
		drawStatCard(x + (colW + colGap) * 2.0f, "Base Triangles", FormatNumber(m_Mesh->GetIndexCount() / 3), theme.Text);

		std::string previewName = "LOD 0 (Base)";
		Wui::WuiColor previewCol = theme.Success;
		if (m_PreviewLODIndex >= 1 && m_PreviewLODIndex <= 8)
		{
			previewName = "LOD " + std::to_string(m_PreviewLODIndex);
			previewCol = Wui::WuiColor{ 0.95f, std::max(0.2f, 0.85f - m_PreviewLODIndex * 0.08f), 0.10f + m_PreviewLODIndex * 0.08f, 1.0f };
		}
		drawStatCard(x + (colW + colGap) * 3.0f, "Active Preview", previewName, previewCol);

		y += cardH + 14.0f;

		// 2. 视口 3D 预览档位切换 (Preview LOD Mode)
		Wui::SectionHeader(ctx, { x, y, width, 18.0f },
			Wui::Tr("panel.model.lod.preview_section", "Interactive 3D Viewport Preview Level"), theme.Accent, theme);
		y += 24.0f;

		Wui::Label(ctx, { x, y + 4.0f }, "视口实时渲染档位 (Preview LOD):", theme.Text, theme.FontSizeSmall);
		y += 22.0f;

		const int totalPreviewCount = 1 + static_cast<int>(m_LODConfigs.size());
		const int cols = (totalPreviewCount <= 4) ? totalPreviewCount : ((totalPreviewCount <= 6) ? 3 : 4);
		const float lodBtnGap = 6.0f;
		const float lodBtnW = std::max(70.0f, (width - lodBtnGap * (cols - 1)) / static_cast<float>(cols));
		const float lodBtnH = 26.0f;

		for (int i = 0; i < totalPreviewCount; ++i)
		{
			const int row = i / cols;
			const int col = i % cols;
			const Wui::WuiRect bRect { x + (lodBtnW + lodBtnGap) * col, y + (lodBtnH + 6.0f) * row, lodBtnW, lodBtnH };
			const bool selected = (m_PreviewLODIndex == i);

			auto btnTheme = theme;
			if (selected)
			{
				btnTheme.ButtonBg = theme.ActiveBg;
				btnTheme.Border = theme.Accent;
			}

			std::string label;
			if (i == 0)
			{
				label = "Base (LOD 0)";
			}
			else
			{
				label = "LOD " + std::to_string(i);
				if (i <= 8 && m_LODMeshes[i])
					label += " [" + FormatNumber(m_LODMeshes[i]->GetIndexCount() / 3) + "t]";
				else
					label += " (未生成)";
			}

			if (Wui::Button(ctx, Wui::HashId(("model.lod.preview_sel." + std::to_string(i)).c_str()),
				bRect, label, btnTheme))
			{
				m_PreviewLODIndex = i;
			}

			if (selected)
				Wui::HighlightOutline(ctx, bRect, theme.Accent, 3.0f, 1.5f);

			Wui::Tooltip(ctx, bRect, i == 0 ? "切换回高精度原始网格" : ("在视口 3D 渲染中实时预览 LOD " + std::to_string(i) + " 减面网格与拓扑结构"));
		}

		const int totalRows = (totalPreviewCount + cols - 1) / cols;
		y += (lodBtnH + 6.0f) * totalRows + 10.0f;

		// 3. 档位数量控制条与快捷预设模板 (Level Count & Presets)
		Wui::SectionHeader(ctx, { x, y, width, 18.0f },
			"LOD 档位层级与预设配置 (LOD Level Count & Presets)", theme.Accent, theme);
		y += 24.0f;

		std::string countLabel = "当前配置生成档位数: " + std::to_string(m_LODConfigs.size()) +
			" 档 (LOD 1 ~ LOD " + std::to_string(m_LODConfigs.size()) + "，含 Base 共 " +
			std::to_string(m_LODConfigs.size() + 1) + " 档，上限 8 档)";
		Wui::Label(ctx, { x, y + 2.0f }, countLabel, theme.Text, theme.FontSizeSmall);
		y += 22.0f;

		// 第一行: 增减档位按钮
		const float rowBtnH = 24.0f;
		const float gapSmall = 6.0f;
		const float countBtnW = std::max(100.0f, (width - gapSmall) * 0.5f);

		// [+ 添加档位]
		const Wui::WuiRect addRect { x, y, countBtnW, rowBtnH };
		const bool canAdd = (m_LODConfigs.size() < 8);
		auto addTheme = theme;
		if (!canAdd) addTheme.ButtonBg = Alpha(theme.ButtonBg, 0.4f);
		if (Wui::Button(ctx, Wui::HashId("model.lod.add_level"), addRect, "[+ 添加档位]", addTheme))
		{
			if (canAdd)
			{
				float prevRatio = m_LODConfigs.empty() ? 0.5f : m_LODConfigs.back().Ratio;
				float prevDist = m_LODConfigs.empty() ? 25.0f : m_LODConfigs.back().Distance;
				float newRatio = std::max(0.05f, std::round(prevRatio * 0.5f * 100.0f) / 100.0f);
				float newDist = std::min(2000.0f, std::round(prevDist * 1.6f));
				m_LODConfigs.push_back({ newRatio, newDist });
			}
		}
		Wui::Tooltip(ctx, addRect, canAdd ? "在末尾追加一级减面档位（上限 8 档）" : "已达到最大 8 档上限");

		// [-] 移除末尾档位
		const Wui::WuiRect removeRect { x + countBtnW + gapSmall, y, countBtnW, rowBtnH };
		const bool canRemove = (m_LODConfigs.size() > 1);
		auto remTheme = theme;
		if (!canRemove) remTheme.ButtonBg = Alpha(theme.ButtonBg, 0.4f);
		if (Wui::Button(ctx, Wui::HashId("model.lod.remove_level"), removeRect, "[-] 移除末尾档位", remTheme))
		{
			if (canRemove)
			{
				const int removedLvl = static_cast<int>(m_LODConfigs.size());
				if (removedLvl <= 8)
					m_LODMeshes[removedLvl].reset();
				m_LODConfigs.pop_back();
				if (m_PreviewLODIndex > static_cast<int>(m_LODConfigs.size()))
					m_PreviewLODIndex = static_cast<int>(m_LODConfigs.size());
			}
		}
		Wui::Tooltip(ctx, removeRect, canRemove ? "移除最后一级减面档位（下限 1 档）" : "至少保留 1 档");

		y += rowBtnH + 8.0f;

		// 第二行: 常用预设模板按钮组
		Wui::Label(ctx, { x, y + 2.0f }, "常用预设模板 (Presets):", theme.TextMuted, theme.FontSizeCaption);
		y += 18.0f;

		const float pBtnW = std::max(70.0f, (width - gapSmall * 3.0f) * 0.25f);
		const Wui::WuiRect p3Rect { x, y, pBtnW, rowBtnH };
		if (Wui::Button(ctx, Wui::HashId("model.lod.preset_3"), p3Rect, "[常用 3 档]", theme))
		{
			m_LODConfigs = { { 0.50f, 25.0f }, { 0.25f, 60.0f } };
			for (int k = 3; k <= 8; ++k) m_LODMeshes[k].reset();
			if (m_PreviewLODIndex > 2) m_PreviewLODIndex = 2;
			m_LODStatusMessage = "已应用常用 3 档预设 (LOD1: 50%, LOD2: 25%)";
			m_LODStatusIsError = false;
		}
		Wui::Tooltip(ctx, p3Rect, "应用常用 3 档配置：Base (100%), LOD1 (50% 25m), LOD2 (25% 60m)");

		const Wui::WuiRect p4Rect { x + (pBtnW + gapSmall), y, pBtnW, rowBtnH };
		if (Wui::Button(ctx, Wui::HashId("model.lod.preset_4"), p4Rect, "[精细 4 档]", theme))
		{
			m_LODConfigs = { { 0.60f, 20.0f }, { 0.35f, 45.0f }, { 0.15f, 80.0f } };
			for (int k = 4; k <= 8; ++k) m_LODMeshes[k].reset();
			if (m_PreviewLODIndex > 3) m_PreviewLODIndex = 3;
			m_LODStatusMessage = "已应用精细 4 档预设 (60% / 35% / 15%)";
			m_LODStatusIsError = false;
		}
		Wui::Tooltip(ctx, p4Rect, "应用精细 4 档配置：Base, LOD1 (60% 20m), LOD2 (35% 45m), LOD3 (15% 80m)");

		const Wui::WuiRect p6Rect { x + (pBtnW + gapSmall) * 2.0f, y, pBtnW, rowBtnH };
		if (Wui::Button(ctx, Wui::HashId("model.lod.preset_6"), p6Rect, "[次时代 6 档]", theme))
		{
			m_LODConfigs = {
				{ 0.75f, 15.0f },
				{ 0.50f, 30.0f },
				{ 0.30f, 50.0f },
				{ 0.15f, 80.0f },
				{ 0.05f, 120.0f }
			};
			for (int k = 6; k <= 8; ++k) m_LODMeshes[k].reset();
			if (m_PreviewLODIndex > 5) m_PreviewLODIndex = 5;
			m_LODStatusMessage = "已应用次时代 6 档预设 (75% / 50% / 30% / 15% / 5%)";
			m_LODStatusIsError = false;
		}
		Wui::Tooltip(ctx, p6Rect, "应用次时代 6 档配置：Base 到 LOD5 五级递减梯次");

		const Wui::WuiRect p8Rect { x + (pBtnW + gapSmall) * 3.0f, y, pBtnW, rowBtnH };
		if (Wui::Button(ctx, Wui::HashId("model.lod.preset_8"), p8Rect, "[全量 8 档 (Full)]", theme))
		{
			m_LODConfigs = {
				{ 0.75f, 15.0f },
				{ 0.55f, 30.0f },
				{ 0.38f, 50.0f },
				{ 0.24f, 75.0f },
				{ 0.14f, 105.0f },
				{ 0.08f, 140.0f },
				{ 0.04f, 180.0f },
				{ 0.02f, 220.0f }
			};
			m_LODStatusMessage = "已应用全量 8 级 LOD 极限梯度预设 (Base + LOD1~LOD8 共 9 档)";
			m_LODStatusIsError = false;
		}
		Wui::Tooltip(ctx, p8Rect, "应用全量 8 级 LOD 梯次配置：覆盖 Base 到 LOD8 完整 8 级动态自动减面与视距阶梯");

		y += rowBtnH + 16.0f;

		// 4. 逐档渲染参数微调卡片 (Per-Level Settings & Ladders)
		Wui::SectionHeader(ctx, { x, y, width, 18.0f },
			Wui::Tr("panel.model.lod.config_section", "LOD Generation Settings & Distance Ladders"), theme.Accent, theme);
		y += 24.0f;

		const float paramLabelW = 140.0f;
		const float paramFieldW = std::max(100.0f, width - paramLabelW - 60.0f);

		for (size_t idx = 0; idx < m_LODConfigs.size(); ++idx)
		{
			const int lvl = static_cast<int>(idx + 1);
			auto& item = m_LODConfigs[idx];

			const float cardHeight = 68.0f;
			const Wui::WuiRect cRect { x, y, width, cardHeight };
			Wui::PanelBackground(ctx, cRect, theme.CardBg, 4.0f);
			Wui::HighlightOutline(ctx, cRect, Alpha(theme.Border, 0.75f), 4.0f, 1.0f);

			// 卡片头部: 左侧标题，右侧面数统计
			std::string cardTitle = "LOD " + std::to_string(lvl);
			Wui::Label(ctx, { cRect.X + 8.0f, cRect.Y + 6.0f }, cardTitle, theme.Accent, theme.FontSizeBody);

			if (lvl <= 8 && m_LODMeshes[lvl])
			{
				const uint32_t tri = m_LODMeshes[lvl]->GetIndexCount() / 3;
				const uint32_t baseTri = m_Mesh->GetIndexCount() / 3;
				const float pct = (baseTri > 0) ? (static_cast<float>(tri) / static_cast<float>(baseTri) * 100.0f) : 0.0f;
				char statBuf[128];
				std::snprintf(statBuf, sizeof(statBuf), "实际生成: %s tris (%.1f%%)", FormatNumber(tri).c_str(), pct);
				const float statW = ctx.MeasureTextWidth(statBuf, theme.FontSizeSmall);
				Wui::Label(ctx, { cRect.X + cRect.W - statW - 10.0f, cRect.Y + 7.0f }, statBuf, theme.Success, theme.FontSizeSmall);
			}
			else
			{
				const char* notGen = "未生成";
				const float statW = ctx.MeasureTextWidth(notGen, theme.FontSizeSmall);
				Wui::Label(ctx, { cRect.X + cRect.W - statW - 10.0f, cRect.Y + 7.0f }, notGen, theme.TextDisabled, theme.FontSizeSmall);
			}

			// 滑块: 保留比例
			const float innerY1 = cRect.Y + 24.0f;
			Wui::Label(ctx, { cRect.X + 10.0f, innerY1 + 2.0f }, "保留比例 (Ratio):", theme.Text, theme.FontSizeSmall);
			Wui::SliderFloat(ctx, Wui::HashId(("model.lod.ratio." + std::to_string(lvl)).c_str()),
				{ cRect.X + paramLabelW, innerY1, paramFieldW, 18.0f }, item.Ratio, 0.05f, 0.95f, theme);
			char ratioStr[32];
			std::snprintf(ratioStr, sizeof(ratioStr), "%.0f%%", item.Ratio * 100.0f);
			Wui::Label(ctx, { cRect.X + paramLabelW + paramFieldW + 8.0f, innerY1 + 2.0f }, ratioStr, theme.Accent, theme.FontSizeSmall);

			// 视距微调
			const float innerY2 = cRect.Y + 44.0f;
			Wui::Label(ctx, { cRect.X + 10.0f, innerY2 + 2.0f }, "推荐视距 (m):", theme.Text, theme.FontSizeSmall);
			Wui::DragFloat(ctx, Wui::HashId(("model.lod.dist." + std::to_string(lvl)).c_str()),
				{ cRect.X + paramLabelW, innerY2, paramFieldW, 18.0f }, item.Distance, 0.5f, 1.0f, 2000.0f, theme);

			y += cardHeight + 8.0f;
		}

		// 边界保护开关 (Preserve Borders)
		if (Wui::Checkbox(ctx, Wui::HashId("model.lod.preserve_borders"),
			{ x, y, width, 22.0f }, "保护几何外轮廓边界与接缝 (Preserve Borders)", m_LODPreserveBorders, theme))
		{
		}
		Wui::Tooltip(ctx, { x, y, width, 22.0f }, "对仅被单一三角形引用的开边界施加高代价惩罚，防止网格孔洞变形或边界穿透");
		y += 30.0f;

		// 5. 一键自动生成与存盘操作 (Action Buttons)
		Wui::SectionHeader(ctx, { x, y, width, 18.0f },
			Wui::Tr("panel.model.lod.actions", "Actions & Asset Generation"), theme.Accent, theme);
		y += 24.0f;

		const float actionBtnW = std::max(140.0f, (width - 8.0f) * 0.5f);
		const float actionBtnH = 28.0f;

		// [Auto-Generate LODs / 自动生成多级 LOD]
		const Wui::WuiRect genBtnRect { x, y, actionBtnW, actionBtnH };
		auto genTheme = theme;
		genTheme.ButtonBg = theme.ActiveBg;
		genTheme.Border = theme.Accent;

		std::string genBtnText = "[Auto-Generate LODs] 自动生成 " + std::to_string(m_LODConfigs.size()) + " 档 LOD";
		if (m_AsyncGenRunning)
		{
			const auto now = std::chrono::high_resolution_clock::now();
			const double elapsed = std::chrono::duration<double>(now - m_AsyncGenStartTime).count();
			char buf[64];
			std::snprintf(buf, sizeof(buf), "[生成中...] (%.1fs)", elapsed);
			genBtnText = buf;
			genTheme.ButtonBg = Alpha(theme.ActiveBg, 0.5f);
		}

		if (Wui::Button(ctx, Wui::HashId("model.lod.autogen_btn"), genBtnRect, genBtnText, genTheme))
		{
			if (!m_AsyncGenRunning && !m_AsyncSaveRunning)
			{
				if (!m_DataValid || m_Data.Vertices.empty())
				{
					m_LODStatusMessage = "错误: 当前模型数据未就绪或未包含有效顶点";
					m_LODStatusIsError = true;
					host.Notify(m_LODStatusMessage);
				}
				else
				{
					std::vector<LODLevelConfig> configs;
					for (size_t i = 0; i < m_LODConfigs.size(); ++i)
					{
						LODLevelConfig cfg;
						cfg.Level = static_cast<uint8_t>(i + 1);
						cfg.TargetRatio = m_LODConfigs[i].Ratio;
						cfg.Distance = m_LODConfigs[i].Distance;
						configs.push_back(cfg);
					}

					Asset::WModelData sourceCopy = m_Data;
					std::string sourcePath = m_LogicalPath;
					m_AsyncGenStartTime = std::chrono::high_resolution_clock::now();
					m_AsyncGenRunning = true;
					m_AsyncGenFuture = std::async(std::launch::async, [sourceCopy = std::move(sourceCopy), configs, sourcePath]() -> AsyncLODGenResult {
						AsyncLODGenResult result;
						std::string genError;
						result.Success = MeshLODGenerator::GenerateLODs(sourceCopy, configs, result.Assets, &genError);
						result.Error = genError;
						if (result.Success)
						{
							std::filesystem::path p(sourcePath);
							std::filesystem::path parentDir = p.parent_path();
							std::string stem = p.stem().string();
							for (auto& gen : result.Assets)
							{
								std::string filename = stem + "_LOD" + std::to_string(gen.Level) + ".wmodel";
								std::filesystem::path lodPath = parentDir.empty() ? std::filesystem::path(filename) : (parentDir / filename);
								gen.RelativePath = lodPath.generic_string();
							}
						}
						return result;
					});
					m_LODStatusMessage = "后台异步工作线程正在计算 QEM 减面拓扑 (UI 保持 60+ FPS)...";
					m_LODStatusIsError = false;
					host.Notify("已启动全部 LOD 档位后台异步生成任务...");
				}
			}
		}
		Wui::Tooltip(ctx, genBtnRect, "在后台工作线程基于 QEM 算法执行 1~8 档减面，UI 保持 60+ FPS 流畅");

		// [Save LOD Files / 保存 LOD 资产]
		const Wui::WuiRect saveBtnRect { x + actionBtnW + 8.0f, y, actionBtnW, actionBtnH };
		std::string saveBtnText = "[Save LOD Files] 保存 " + std::to_string(m_LODConfigs.size()) + " 档 LOD 资产";
		auto saveTheme = theme;
		if (m_AsyncSaveRunning)
		{
			const auto now = std::chrono::high_resolution_clock::now();
			const double elapsed = std::chrono::duration<double>(now - m_AsyncSaveStartTime).count();
			char buf[64];
			std::snprintf(buf, sizeof(buf), "[保存中...] (%.1fs)", elapsed);
			saveBtnText = buf;
			saveTheme.ButtonBg = Alpha(theme.ButtonBg, 0.5f);
		}

		if (Wui::Button(ctx, Wui::HashId("model.lod.save_files_btn"), saveBtnRect, saveBtnText, saveTheme))
		{
			if (!m_AsyncSaveRunning && !m_AsyncGenRunning)
			{
				std::vector<LODLevelConfig> configs;
				for (size_t i = 0; i < m_LODConfigs.size(); ++i)
				{
					LODLevelConfig cfg;
					cfg.Level = static_cast<uint8_t>(i + 1);
					cfg.TargetRatio = m_LODConfigs[i].Ratio;
					cfg.Distance = m_LODConfigs[i].Distance;
					configs.push_back(cfg);
				}

				std::string logicalPath = m_LogicalPath;
				m_AsyncSaveStartTime = std::chrono::high_resolution_clock::now();
				m_AsyncSaveRunning = true;
				m_AsyncSaveFuture = std::async(std::launch::async, [logicalPath, configs]() -> AsyncLODSaveResult {
					AsyncLODSaveResult result;
					std::vector<GeneratedLODAsset> savedAssets;
					std::string saveErr;
					result.Success = MeshLODGenerator::GenerateAndSaveLODFiles(logicalPath, configs, savedAssets, &saveErr);
					result.Error = saveErr;
					if (result.Success)
					{
						for (const auto& a : savedAssets)
							result.SavedPaths.push_back(a.RelativePath);
					}
					return result;
				});
				m_LODStatusMessage = "正在后台异步写出全部 LOD 资产到磁盘...";
				m_LODStatusIsError = false;
				host.Notify("已启动全部 LOD 资产后台异步写盘任务...");
			}
		}
		Wui::Tooltip(ctx, saveBtnRect, "在模型同目录下异步写出全部 <ModelStem>_LOD1.wmodel 到 <ModelStem>_LODN.wmodel 资产文件");
		y += actionBtnH + 16.0f;

		// 6. 详细层级指标清单
		if (!m_GeneratedLODs.empty())
		{
			Wui::SectionHeader(ctx, { x, y, width, 18.0f }, "Generated LOD Details", theme.Accent, theme);
			y += 24.0f;

			const float rowH = 22.0f;
			for (const auto& gen : m_GeneratedLODs)
			{
				const Wui::WuiRect rowRect { x, y, width, rowH };
				Wui::PanelBackground(ctx, rowRect, theme.CardBg, 3.0f);
				char rowText[256];
				std::snprintf(rowText, sizeof(rowText), "  LOD %u:  %u 三角面 (%.1f%%)  ->  %s",
					gen.Level, gen.TriangleCount, gen.ActualRatio * 100.0f, gen.RelativePath.c_str());
				Wui::Label(ctx, { rowRect.X + 4.0f, rowRect.Y + 3.0f }, rowText, theme.Text, theme.FontSizeSmall);
				y += rowH + 4.0f;
			}
			y += 8.0f;
		}

		y += 24.0f;
		return y - rect.Y;
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
		// Action buttons on top-right: Key Bone / Reset Bind / Save Hierarchy
		const float saveBtnW = 95.0f;
		const float resetBtnW = 85.0f;
		const float keyBtnW = 90.0f;
		const Wui::WuiRect saveBtnRect { x + width - 10.0f - saveBtnW, cy - 2.0f, saveBtnW, 20.0f };
		if (Wui::Button(ctx, Wui::HashId("model.node.save"), saveBtnRect,
			Wui::Tr("panel.model.skeleton.save_hierarchy", "Save Asset"), theme))
		{
			const std::string curExt = LowerExtension(std::filesystem::path(m_LogicalPath));
			if (curExt == ".wclip")
			{
				SaveCurrentClipToFile();
			}
			else if (curExt == ".wskeleton")
			{
				Asset::WSkeletonData skeleton;
				skeleton.Bones.reserve(m_Data.Nodes.size());
				for (size_t i = 0; i < m_Data.Nodes.size(); ++i)
				{
					const auto& n = m_Data.Nodes[i];
					Asset::SkeletonBone b;
					b.Name = StringPool::Get().InternName(n.Name);
					b.NameStr = n.Name;
					b.ParentIndex = n.Parent;
					b.BindPoseLocal.Translation = n.Translation;
					b.BindPoseLocal.Rotation = n.Rotation;
					b.BindPoseLocal.Scale = n.Scale;
					skeleton.Bones.push_back(std::move(b));
				}
				std::string err;
				if (Asset::WSkeletonIO::WriteFile(m_LogicalPath, skeleton, &err))
				{
					m_Status = "Skeleton saved to " + m_LogicalPath;
					m_StatusIsError = false;
				}
				else
				{
					m_Status = "Failed saving skeleton: " + err;
					m_StatusIsError = true;
				}
			}
			else
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

		const Wui::WuiRect keyBtnRect { resetBtnRect.X - 6.0f - keyBtnW, cy - 2.0f, keyBtnW, 20.0f };
		if (Wui::Button(ctx, Wui::HashId("model.node.key_bone"), keyBtnRect,
			Wui::Tr("panel.model.anim.key_selected", "Key Bone (K)"), theme))
		{
			RecordKeyframeForNode(static_cast<uint32_t>(m_SelectedNode), m_AnimTime);
		}
		Wui::Tooltip(ctx, keyBtnRect, Wui::Tr("panel.model.anim.key_selected_tip", "Record keyframe for this bone at current timeline position (K)"));

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

	void ModelPreviewPanel::RecordKeyframeForNode(uint32_t nodeIndex, float time)
	{
		if (!m_DataValid || nodeIndex >= m_Data.Nodes.size())
			return;

		if (m_Data.Animations.empty())
		{
			Asset::WModelAnimation defaultClip;
			defaultClip.Name = "DefaultClip";
			defaultClip.Duration = std::max(1.0f, time);
			m_Data.Animations.push_back(std::move(defaultClip));
			m_AnimClipIndex = 0;
		}

		if (m_AnimClipIndex < 0 || m_AnimClipIndex >= static_cast<int>(m_Data.Animations.size()))
			m_AnimClipIndex = 0;

		Asset::WModelAnimation& clip = m_Data.Animations[static_cast<size_t>(m_AnimClipIndex)];
		if (time > clip.Duration)
			clip.Duration = time;

		const Asset::WModelNode& node = m_Data.Nodes[nodeIndex];

		auto recordChannel = [&](Asset::WModelIO::WModelAnimationPath path, const glm::vec4& value)
		{
			Asset::WModelAnimationChannel* targetChannel = nullptr;
			for (auto& ch : clip.Channels)
			{
				if (ch.TargetNode == nodeIndex && ch.Path == path)
				{
					targetChannel = &ch;
					break;
				}
			}
			if (!targetChannel)
			{
				clip.Channels.push_back({ nodeIndex, path, {} });
				targetChannel = &clip.Channels.back();
			}

			bool updated = false;
			for (auto& key : targetChannel->Keys)
			{
				if (std::abs(key.Time - time) < 0.001f)
				{
					key.Value = value;
					updated = true;
					break;
				}
			}
			if (!updated)
			{
				targetChannel->Keys.push_back({ time, value });
				std::sort(targetChannel->Keys.begin(), targetChannel->Keys.end(),
					[](const Asset::WModelAnimationKey& a, const Asset::WModelAnimationKey& b) {
						return a.Time < b.Time;
					});
			}
		};

		recordChannel(Asset::WModelIO::WModelAnimationPath::Translation, glm::vec4(node.Translation, 0.0f));
		recordChannel(Asset::WModelIO::WModelAnimationPath::Rotation,
			glm::vec4(node.Rotation.x, node.Rotation.y, node.Rotation.z, node.Rotation.w));
		recordChannel(Asset::WModelIO::WModelAnimationPath::Scale, glm::vec4(node.Scale, 0.0f));

		const std::string nodeLabel = node.Name.empty() ? ("node_" + std::to_string(nodeIndex)) : node.Name;
		char buf[128];
		std::snprintf(buf, sizeof(buf), "Keyframe recorded for %s at t=%.2fs", nodeLabel.c_str(), time);
		m_Status = buf;
		m_StatusIsError = false;
	}

	void ModelPreviewPanel::RecordKeyframeForAllNodes(float time)
	{
		if (!m_DataValid || m_Data.Nodes.empty())
			return;

		for (uint32_t i = 0; i < static_cast<uint32_t>(m_Data.Nodes.size()); ++i)
		{
			RecordKeyframeForNode(i, time);
		}

		char buf[128];
		std::snprintf(buf, sizeof(buf), "Keyframe recorded for all %zu nodes at t=%.2fs", m_Data.Nodes.size(), time);
		m_Status = buf;
		m_StatusIsError = false;
	}

	float ModelPreviewPanel::DrawTabSkeleton(Wui::WuiContext& ctx, const Wui::WuiRect& rect, PanelHost& host,
		const Wui::WuiTheme& theme)
	{
		(void)host;
		const float x = rect.X;
		const float width = rect.W;
		float y = rect.Y + 4.0f;

		const bool hasNodes = (m_Mesh && !m_Mesh->GetNodes().empty()) || (m_DataValid && !m_Data.Nodes.empty());
		if (!hasNodes)
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
		std::vector<MeshNode> tempNodes;
		const std::vector<MeshNode>& nodes = (m_Mesh && !m_Mesh->GetNodes().empty())
			? m_Mesh->GetNodes()
			: (m_DataValid ? (tempNodes = [&]() {
				std::vector<MeshNode> r;
				r.reserve(m_Data.Nodes.size());
				for (const auto& dn : m_Data.Nodes)
				{
					MeshNode mn;
					mn.Name = dn.Name;
					mn.Parent = dn.Parent;
					mn.MeshIndex = dn.MeshIndex;
					mn.Translation = dn.Translation;
					mn.Rotation = dn.Rotation;
					mn.Scale = dn.Scale;
					r.push_back(mn);
				}
				return r;
			}()) : tempNodes);
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

		// 构建父子拓扑映射
		std::vector<std::vector<size_t>> children(nodes.size());
		std::vector<size_t> roots;
		for (size_t i = 0; i < nodes.size(); ++i)
		{
			int32_t p = nodes[i].Parent;
			if (p >= 0 && static_cast<size_t>(p) < nodes.size() && static_cast<size_t>(p) != i)
				children[static_cast<size_t>(p)].push_back(i);
			else
				roots.push_back(i);
		}

		const std::string filterLower = ToLower(m_SkeletonFilter);
		std::vector<bool> subtreeMatches(nodes.size(), false);
		if (!filterLower.empty())
		{
			for (size_t i = 0; i < nodes.size(); ++i)
			{
				if (ToLower(nodes[i].Name).find(filterLower) != std::string::npos)
				{
					int cur = static_cast<int>(i);
					while (cur >= 0 && static_cast<size_t>(cur) < nodes.size())
					{
						subtreeMatches[static_cast<size_t>(cur)] = true;
						cur = nodes[static_cast<size_t>(cur)].Parent;
					}
				}
			}
		}

		std::vector<bool> visited(nodes.size(), false);
		std::function<void(size_t, int)> drawNodeBranch = [&](size_t index, int depth)
		{
			if (index >= nodes.size() || visited[index])
				return;
			visited[index] = true;

			if (!filterLower.empty() && !subtreeMatches[index])
				return;

			const MeshNode& node = nodes[index];
			const bool hasChildren = !children[index].empty();
			const bool isCollapsed = m_SkeletonCollapsedNodes.count(static_cast<int>(index)) > 0;

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
			float nodeX = x + indent;

			// 折叠/展开切换按钮 [v] / [>]
			if (hasChildren)
			{
				const Wui::WuiRect foldRect { nodeX, y + 2.0f, 16.0f, 18.0f };
				const char* foldIcon = isCollapsed ? ">" : "v";
				if (Wui::Button(ctx, Wui::HashId(("model.skel.fold." + std::to_string(index)).c_str()),
					foldRect, foldIcon, theme))
				{
					if (isCollapsed)
						m_SkeletonCollapsedNodes.erase(static_cast<int>(index));
					else
						m_SkeletonCollapsedNodes.insert(static_cast<int>(index));
				}
			}
			nodeX += 18.0f;

			// 节点类型指示 (Joint vs Mesh vs Group)
			bool isJoint = false;
			if (m_DataValid && !m_Data.Skins.empty())
			{
				for (uint32_t jn : m_Data.Skins[0].JointNodes)
				{
					if (jn == index) { isJoint = true; break; }
				}
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

			// 递归子节点展开
			if (hasChildren && (!isCollapsed || !filterLower.empty()))
			{
				for (size_t childIdx : children[index])
					drawNodeBranch(childIdx, depth + 1);
			}
		};

		for (size_t rootIdx : roots)
			drawNodeBranch(rootIdx, 0);

		for (size_t i = 0; i < nodes.size(); ++i)
		{
			if (!visited[i])
				drawNodeBranch(i, 0);
		}

		// 选中节点属性检视器 (稳定停靠在骨骼树下方，不引起上方树形抖动)
		if (m_SelectedNode >= 0 && static_cast<size_t>(m_SelectedNode) < m_Data.Nodes.size())
		{
			y += 12.0f;
			DrawNodeInspector(ctx, x, y, width, theme);
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
		(void)host;
		const float x = rect.X;
		const float width = rect.W;
		float y = rect.Y + 4.0f;

		const std::string currentExt = LowerExtension(std::filesystem::path(m_LogicalPath));
		const bool isWClip = (currentExt == ".wclip");

		// 1. 顶部标题栏与模式自适应动作按钮 (绝不重叠、绝不产生重复按钮)
		if (isWClip)
		{
			Wui::SectionHeader(ctx, { x, y, 160.0f, 22.0f },
				Wui::Tr("panel.model.tab.clip_editor", "Clip Editor (.wclip)"), theme.Accent, theme);

			const float saveW = 90.0f;
			const float newW = 80.0f;
			float bx = x + width - saveW;
			if (Wui::Button(ctx, Wui::HashId("model.clips.save_wclip"), { bx, y, saveW, 22.0f },
				Wui::Tr("panel.model.clips.save_clip", "Save .wclip"), theme))
			{
				SaveCurrentClipToFile();
			}
			Wui::Tooltip(ctx, { bx, y, saveW, 22.0f }, "Save all animation channels and keys back to .wclip file");

			bx -= newW + 6.0f;
			if (Wui::Button(ctx, Wui::HashId("model.clips.toggle_new"), { bx, y, newW, 22.0f },
				Wui::Tr("panel.model.clips.new_clip", "New Clip"), theme))
			{
				m_NewClipOpen = !m_NewClipOpen;
				m_SplitClipOpen = false;
			}
			Wui::Tooltip(ctx, { bx, y, newW, 22.0f }, "Create a new .wclip asset");

			y += 28.0f;
		}
		else
		{
			Wui::SectionHeader(ctx, { x, y, width, 22.0f },
				Wui::Tr("panel.model.tab.clips", "Animation Clips") + " (" + std::to_string(m_Data.Animations.size()) + ")",
				theme.Accent, theme);
			y += 26.0f;

			// 模型动画专用动作工具栏行 (避免与标题挤在同一行)
			float bx = x;
			const float saveAllW = 96.0f;
			const float exportW = 90.0f;
			const float newW = 76.0f;
			const float splitW = 70.0f;
			const float dupW = 54.0f;

			if (Wui::Button(ctx, Wui::HashId("model.clips.save_all"), { bx, y, saveAllW, 22.0f },
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
			Wui::Tooltip(ctx, { bx, y, saveAllW, 22.0f }, "Save all animation clips into .wmodel");
			bx += saveAllW + 6.0f;

			if (m_AnimClipIndex >= 0 && static_cast<size_t>(m_AnimClipIndex) < m_Data.Animations.size())
			{
				if (Wui::Button(ctx, Wui::HashId("model.clips.export_wclip"), { bx, y, exportW, 22.0f },
					Wui::Tr("panel.model.clips.export_clip", "Export .wclip"), theme))
				{
					ExportActiveClipToWClip(host);
				}
				Wui::Tooltip(ctx, { bx, y, exportW, 22.0f }, "Export active clip into a standalone .wclip file");
				bx += exportW + 6.0f;
			}

			if (Wui::Button(ctx, Wui::HashId("model.clips.toggle_new"), { bx, y, newW, 22.0f },
				Wui::Tr("panel.model.clips.new_clip", "New Clip"), theme))
			{
				m_NewClipOpen = !m_NewClipOpen;
				m_SplitClipOpen = false;
			}
			bx += newW + 6.0f;

			if (m_AnimClipIndex >= 0 && static_cast<size_t>(m_AnimClipIndex) < m_Data.Animations.size())
			{
				if (Wui::Button(ctx, Wui::HashId("model.clips.toggle_split"), { bx, y, splitW, 22.0f },
					Wui::Tr("panel.model.clips.split_clip", "Split Clip"), theme))
				{
					m_SplitClipOpen = !m_SplitClipOpen;
					m_NewClipOpen = false;
				}
				bx += splitW + 6.0f;

				if (Wui::Button(ctx, Wui::HashId("model.clips.duplicate"), { bx, y, dupW, 22.0f },
					Wui::Tr("panel.model.clips.duplicate_clip", "Duplicate"), theme))
				{
					Asset::WModelAnimation copy = m_Data.Animations[static_cast<size_t>(m_AnimClipIndex)];
					copy.Name = copy.Name + "_copy";
					m_Data.Animations.push_back(copy);
					m_AnimClipIndex = static_cast<int>(m_Data.Animations.size() - 1);
				}
			}
			y += 28.0f;
		}

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
				CreateNewClipAsset(host);
			}

			y += formH + 8.0f;
		}

		if (m_Data.Animations.empty())
		{
			Wui::EmptyState(ctx, { x, y, width, 180.0f }, "CLIP",
				Wui::Tr("panel.model.clips.empty_title", "No Animation Clips"),
				Wui::Tr("panel.model.clips.empty_desc", "This model contains no skeletal animation sequences. Click 'New Clip' to create one."),
				"", 0, theme);
			m_TabContentHeight[4] = y + 200.0f - rect.Y;
			return y + 200.0f - rect.Y;
		}

		// 2. 当前选中片段属性与动画编辑核心套件
		if (m_AnimClipIndex >= 0 && static_cast<size_t>(m_AnimClipIndex) < m_Data.Animations.size())
		{
			Asset::WModelAnimation& anim = m_Data.Animations[static_cast<size_t>(m_AnimClipIndex)];

			// ---- 片段属性卡片 ----
			const float propCardH = 74.0f;
			const Wui::WuiRect propCard { x, y, width, propCardH };
			Wui::PanelBackground(ctx, propCard, theme.CardBg, 4.0f);
			Wui::HighlightOutline(ctx, propCard, Alpha(theme.Accent, 0.45f), 4.0f, 1.0f);

			float py = y + 8.0f;
			Wui::Label(ctx, { x + 8.0f, py + 3.0f }, "Clip:", theme.TextMuted, theme.FontSizeCaption);
			const float nameFieldW = std::clamp(width * 0.32f, 100.0f, 180.0f);
			const Wui::WuiId cNameId = Wui::HashId("model.active_clip.name");
			Wui::TextFieldA11y ca11y;
			ca11y.Label = "Active Clip Name";
			Wui::TextField(ctx, cNameId, { x + 40.0f, py, nameFieldW, 22.0f }, anim.Name, theme, nullptr, &ca11y);
			if (ctx.Focus() == cNameId) ctx.SetTextInputActive(true);

			float curPx = x + 40.0f + nameFieldW + 8.0f;
			Wui::Label(ctx, { curPx, py + 3.0f }, "Duration:", theme.TextMuted, theme.FontSizeCaption);
			curPx += 56.0f;
			const float durW = 68.0f;
			Wui::DragFloat(ctx, Wui::HashId("model.active_clip.dur"), { curPx, py, durW, 22.0f }, anim.Duration, 0.05f, 0.1f, 300.0f, theme);
			curPx += durW + 8.0f;

			Wui::Checkbox(ctx, Wui::HashId("model.active_clip.loop"), { curPx, py + 2.0f, 65.0f, 18.0f }, "Loop", m_AnimLoop, theme);

			py += 28.0f;
			size_t totalKeys = 0;
			for (const auto& ch : anim.Channels) totalKeys += ch.Keys.size();
			const int totalFrames = static_cast<int>(std::round(anim.Duration * 30.0f));
			char statBuf[128];
			std::snprintf(statBuf, sizeof(statBuf), "%d frames @ 30 FPS  |  %zu channels  |  %zu total keys",
				totalFrames, anim.Channels.size(), totalKeys);
			Wui::Label(ctx, { x + 8.0f, py + 2.0f }, statBuf, theme.TextMuted, theme.FontSizeCaption);

			y += propCardH + 10.0f;

			// ---- 3. 骨骼姿态微调与关键帧录制 (Bone Pose Inspector) ----
			const float poseCardH = 176.0f;
			const Wui::WuiRect poseCard { x, y, width, poseCardH };
			Wui::PanelBackground(ctx, poseCard, theme.CardBg, 4.0f);
			Wui::HighlightOutline(ctx, poseCard, Alpha(theme.BorderStrong, 0.7f), 4.0f, 1.0f);

			float cy = y + 8.0f;
			Wui::Label(ctx, { x + 8.0f, cy }, "Bone Pose & Keyframes", theme.Accent, theme.FontSizeSmall);

			// 骨骼选择下拉列表
			std::vector<std::string> boneNames;
			boneNames.reserve(m_Data.Nodes.size());
			int boneComboIdx = 0;
			for (size_t n = 0; n < m_Data.Nodes.size(); ++n)
			{
				std::string bname = m_Data.Nodes[n].Name.empty() ? ("node_" + std::to_string(n)) : m_Data.Nodes[n].Name;
				if (static_cast<int>(n) == m_SelectedNode)
					boneComboIdx = static_cast<int>(n);
				boneNames.push_back(std::to_string(n) + ": " + bname);
			}
			if (m_SelectedNode < 0 && !m_Data.Nodes.empty())
			{
				m_SelectedNode = 0;
				boneComboIdx = 0;
			}

			const float bComboW = std::clamp(width * 0.40f, 120.0f, 200.0f);
			const float bComboX = x + width - bComboW - 8.0f;
			std::string selBoneLabel = (m_SelectedNode >= 0 && static_cast<size_t>(m_SelectedNode) < boneNames.size())
				? boneNames[static_cast<size_t>(m_SelectedNode)] : "(select bone)";
			if (Wui::Combo(ctx, Wui::HashId("model.clip.bone_sel"), { bComboX, cy - 2.0f, bComboW, 20.0f }, "",
				boneNames, boneComboIdx, theme, selBoneLabel))
			{
				if (boneComboIdx >= 0 && static_cast<size_t>(boneComboIdx) < m_Data.Nodes.size())
				{
					m_SelectedNode = boneComboIdx;
					const auto& snode = m_Data.Nodes[static_cast<size_t>(m_SelectedNode)];
					m_SelectedNodeTranslation = snode.Translation;
					m_SelectedNodeRotationEuler = glm::degrees(glm::eulerAngles(snode.Rotation));
					m_SelectedNodeScale = snode.Scale;
				}
			}

			cy += 24.0f;
			if (m_SelectedNode >= 0 && static_cast<size_t>(m_SelectedNode) < m_Data.Nodes.size())
			{
				Asset::WModelNode& snode = m_Data.Nodes[static_cast<size_t>(m_SelectedNode)];
				const float labelW = 86.0f;
				const float fieldW = width - labelW - 20.0f;
				const float compW = (fieldW - 8.0f) / 3.0f;

				// Translation
				Wui::Label(ctx, { x + 8.0f, cy + 3.0f }, "Translation", theme.TextMuted, theme.FontSizeCaption);
				if (Wui::DragFloat(ctx, Wui::HashId("model.cpose.tx"), { x + labelW, cy, compW, 20.0f }, snode.Translation.x, 0.01f, -100.0f, 100.0f, theme))
					m_SelectedNodeTranslation.x = snode.Translation.x;
				if (Wui::DragFloat(ctx, Wui::HashId("model.cpose.ty"), { x + labelW + compW + 4.0f, cy, compW, 20.0f }, snode.Translation.y, 0.01f, -100.0f, 100.0f, theme))
					m_SelectedNodeTranslation.y = snode.Translation.y;
				if (Wui::DragFloat(ctx, Wui::HashId("model.cpose.tz"), { x + labelW + (compW + 4.0f) * 2.0f, cy, compW, 20.0f }, snode.Translation.z, 0.01f, -100.0f, 100.0f, theme))
					m_SelectedNodeTranslation.z = snode.Translation.z;
				cy += 24.0f;

				// Rotation
				Wui::Label(ctx, { x + 8.0f, cy + 3.0f }, "Rotation (°)", theme.TextMuted, theme.FontSizeCaption);
				bool rch = false;
				rch |= Wui::DragFloat(ctx, Wui::HashId("model.cpose.ry"), { x + labelW, cy, compW, 20.0f }, m_SelectedNodeRotationEuler.x, 0.5f, -360.0f, 360.0f, theme);
				rch |= Wui::DragFloat(ctx, Wui::HashId("model.cpose.rp"), { x + labelW + compW + 4.0f, cy, compW, 20.0f }, m_SelectedNodeRotationEuler.y, 0.5f, -360.0f, 360.0f, theme);
				rch |= Wui::DragFloat(ctx, Wui::HashId("model.cpose.rr"), { x + labelW + (compW + 4.0f) * 2.0f, cy, compW, 20.0f }, m_SelectedNodeRotationEuler.z, 0.5f, -360.0f, 360.0f, theme);
				if (rch) snode.Rotation = glm::quat(glm::radians(m_SelectedNodeRotationEuler));
				cy += 24.0f;

				// Scale
				Wui::Label(ctx, { x + 8.0f, cy + 3.0f }, "Scale", theme.TextMuted, theme.FontSizeCaption);
				Wui::DragFloat(ctx, Wui::HashId("model.cpose.sx"), { x + labelW, cy, compW, 20.0f }, snode.Scale.x, 0.01f, 0.001f, 100.0f, theme);
				Wui::DragFloat(ctx, Wui::HashId("model.cpose.sy"), { x + labelW + compW + 4.0f, cy, compW, 20.0f }, snode.Scale.y, 0.01f, 0.001f, 100.0f, theme);
				Wui::DragFloat(ctx, Wui::HashId("model.cpose.sz"), { x + labelW + (compW + 4.0f) * 2.0f, cy, compW, 20.0f }, snode.Scale.z, 0.01f, 0.001f, 100.0f, theme);
				cy += 26.0f;

				// 关键帧操作按钮
				const float kbtnW = (width - 24.0f) / 3.0f;
				if (Wui::Button(ctx, Wui::HashId("model.cpose.key_bone"), { x + 8.0f, cy, kbtnW, 22.0f }, "Key Bone (K)", theme))
				{
					RecordKeyframeForNode(static_cast<uint32_t>(m_SelectedNode), m_AnimTime);
				}
				Wui::Tooltip(ctx, { x + 8.0f, cy, kbtnW, 22.0f }, "Record keyframe for this bone at current timeline frame (K)");

				if (Wui::Button(ctx, Wui::HashId("model.cpose.key_all"), { x + 8.0f + kbtnW + 4.0f, cy, kbtnW, 22.0f }, "Key All (Shift+K)", theme))
				{
					RecordKeyframeForAllNodes(m_AnimTime);
				}
				Wui::Tooltip(ctx, { x + 8.0f + kbtnW + 4.0f, cy, kbtnW, 22.0f }, "Record keyframe for all skeleton bones at current timeline frame (Shift+K)");

				if (Wui::Button(ctx, Wui::HashId("model.cpose.reset_bind"), { x + 8.0f + (kbtnW + 4.0f) * 2.0f, cy, kbtnW, 22.0f }, "Reset Pose", theme))
				{
					if (!m_Data.Skins.empty())
					{
						const auto& skin = m_Data.Skins[0];
						for (size_t j = 0; j < skin.JointNodes.size(); ++j)
						{
							if (skin.JointNodes[j] == static_cast<uint32_t>(m_SelectedNode))
							{
								if (j < skin.BindTranslations.size()) snode.Translation = skin.BindTranslations[j];
								if (j < skin.BindRotations.size()) snode.Rotation = skin.BindRotations[j];
								if (j < skin.BindScales.size()) snode.Scale = skin.BindScales[j];
								m_SelectedNodeTranslation = snode.Translation;
								m_SelectedNodeRotationEuler = glm::degrees(glm::eulerAngles(snode.Rotation));
								m_SelectedNodeScale = snode.Scale;
								break;
							}
						}
					}
				}
			}
			y += poseCardH + 10.0f;

			// ---- 4. 关键帧通道列表 (Animated Channels) ----
			Wui::SectionHeader(ctx, { x, y, width, 20.0f },
				"Animated Channels (" + std::to_string(anim.Channels.size()) + ")", theme.Accent, theme);
			y += 24.0f;

			for (size_t ci = 0; ci < anim.Channels.size(); ++ci)
			{
				const auto& ch = anim.Channels[ci];
				const std::string bname = (ch.TargetNode < m_Data.Nodes.size())
					? m_Data.Nodes[ch.TargetNode].Name : ("node_" + std::to_string(ch.TargetNode));
				const bool isSelectedBone = static_cast<int>(ch.TargetNode) == m_SelectedNode;

				const float chRowH = 24.0f;
				const Wui::WuiRect chRow { x, y, width, chRowH };
				Wui::PanelBackground(ctx, chRow, isSelectedBone ? Alpha(theme.Accent, 0.22f) : theme.CardBg, 3.0f);

				Wui::Label(ctx, { x + 8.0f, y + 4.0f }, bname, isSelectedBone ? theme.Accent : theme.TextBright, theme.FontSizeCaption);

				char kcountBuf[64];
				std::snprintf(kcountBuf, sizeof(kcountBuf), "%zu keys", ch.Keys.size());
				Wui::Label(ctx, { x + width - 110.0f, y + 4.0f }, kcountBuf, theme.TextMuted, theme.FontSizeCaption);

				if (Wui::Button(ctx, Wui::HashId(("model.ch.select." + std::to_string(ci)).c_str()),
					{ x + width - 44.0f, y + 2.0f, 38.0f, 20.0f }, "Edit", theme))
				{
					m_SelectedNode = static_cast<int>(ch.TargetNode);
					if (m_SelectedNode < static_cast<int>(m_Data.Nodes.size()))
					{
						const auto& snode = m_Data.Nodes[static_cast<size_t>(m_SelectedNode)];
						m_SelectedNodeTranslation = snode.Translation;
						m_SelectedNodeRotationEuler = glm::degrees(glm::eulerAngles(snode.Rotation));
						m_SelectedNodeScale = snode.Scale;
					}
				}

				y += chRowH + 4.0f;
			}
		}

		// 5. 其他片段切换列表 (多片段模型下展示)
		if (m_Data.Animations.size() > 1)
		{
			y += 10.0f;
			Wui::SectionHeader(ctx, { x, y, width, 20.0f },
				"Other Clips (" + std::to_string(m_Data.Animations.size()) + ")", theme.TextMuted, theme);
			y += 24.0f;

			for (size_t index = 0; index < m_Data.Animations.size(); ++index)
			{
				if (static_cast<int>(index) == m_AnimClipIndex) continue;
				Asset::WModelAnimation& oanim = m_Data.Animations[index];

				const float oCardH = 32.0f;
				const Wui::WuiRect oCard { x, y, width, oCardH };
				Wui::PanelBackground(ctx, oCard, theme.CardBg, 3.0f);

				Wui::Label(ctx, { x + 8.0f, y + 8.0f }, oanim.Name, theme.Text, theme.FontSizeCaption);

				char durBuf[64];
				std::snprintf(durBuf, sizeof(durBuf), "%.2fs", oanim.Duration);
				Wui::Label(ctx, { x + width - 130.0f, y + 8.0f }, durBuf, theme.TextMuted, theme.FontSizeCaption);

				if (Wui::Button(ctx, Wui::HashId(("model.oclip.switch." + std::to_string(index)).c_str()),
					{ x + width - 70.0f, y + 5.0f, 60.0f, 22.0f }, "Switch", theme))
				{
					m_AnimClipIndex = static_cast<int>(index);
					m_AnimTime = 0.0f;
				}

				y += oCardH + 4.0f;
			}
		}

		m_TabContentHeight[4] = std::max(500.0f, y - rect.Y + 60.0f);
		return y - rect.Y;
	}

	void ModelPreviewPanel::SaveCurrentClipToFile()
	{
		if (m_AnimClipIndex < 0 || static_cast<size_t>(m_AnimClipIndex) >= m_Data.Animations.size())
			return;

		const auto& anim = m_Data.Animations[static_cast<size_t>(m_AnimClipIndex)];
		Asset::WAnimationClipData clipData;
		clipData.Name = anim.Name;
		clipData.Duration = anim.Duration;
		clipData.SkeletonPath = "engine/models/characters/mannequin.wskeleton";

		Asset::WAnimationClipData existing;
		if (Asset::WAnimationClipIO::ReadFile(m_LogicalPath, existing))
		{
			clipData.SkeletonPath = existing.SkeletonPath;
			clipData.RootTranslation = existing.RootTranslation;
			clipData.RootRotation = existing.RootRotation;
			clipData.Events = existing.Events;
		}

		clipData.Channels.reserve(anim.Channels.size());
		for (const auto& ch : anim.Channels)
		{
			if (ch.TargetNode >= m_Data.Nodes.size())
				continue;
			const std::string boneName = m_Data.Nodes[ch.TargetNode].Name;
			if (boneName.empty())
				continue;

			Asset::AnimationChannel* targetCh = nullptr;
			for (auto& candidate : clipData.Channels)
			{
				if (candidate.BoneNameStr == boneName)
				{
					targetCh = &candidate;
					break;
				}
			}
			if (!targetCh)
			{
				Asset::AnimationChannel newCh;
				newCh.BoneName = StringPool::Get().InternName(boneName);
				newCh.BoneNameStr = boneName;
				clipData.Channels.push_back(std::move(newCh));
				targetCh = &clipData.Channels.back();
			}

			if (ch.Path == Asset::WModelIO::WModelAnimationPath::Translation)
			{
				targetCh->PositionKeys.clear();
				for (const auto& k : ch.Keys)
					targetCh->PositionKeys.push_back({ k.Time, glm::vec3(k.Value) });
			}
			else if (ch.Path == Asset::WModelIO::WModelAnimationPath::Rotation)
			{
				targetCh->RotationKeys.clear();
				for (const auto& k : ch.Keys)
					targetCh->RotationKeys.push_back({ k.Time, glm::quat(k.Value.w, k.Value.x, k.Value.y, k.Value.z) });
			}
			else if (ch.Path == Asset::WModelIO::WModelAnimationPath::Scale)
			{
				targetCh->ScaleKeys.clear();
				for (const auto& k : ch.Keys)
					targetCh->ScaleKeys.push_back({ k.Time, glm::vec3(k.Value) });
			}
		}

		std::string saveErr;
		if (Asset::WAnimationClipIO::WriteFile(m_LogicalPath, clipData, &saveErr))
		{
			m_Status = "Saved animation clip to " + m_LogicalPath;
			m_StatusIsError = false;
		}
		else
		{
			m_Status = "Save failed: " + saveErr;
			m_StatusIsError = true;
		}
	}

	void ModelPreviewPanel::ExportActiveClipToWClip(PanelHost& host)
	{
		if (m_AnimClipIndex < 0 || static_cast<size_t>(m_AnimClipIndex) >= m_Data.Animations.size())
			return;

		const auto& anim = m_Data.Animations[static_cast<size_t>(m_AnimClipIndex)];
		std::string cleanName = anim.Name.empty() ? ("clip_" + std::to_string(m_AnimClipIndex)) : anim.Name;
		for (char& c : cleanName)
			if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_') c = '_';

		std::filesystem::path dir = "projects/default/assets/animation";
		if (!std::filesystem::exists(dir))
			std::filesystem::create_directories(dir);
		std::filesystem::path target = dir / (cleanName + ".wclip");

		Asset::WSkeletonData skeleton;
		std::vector<Asset::WAnimationClipData> extractedClips;
		Asset::WSkeletonIO::ExtractSkeletonAndClips(m_Data, skeleton, extractedClips);

		std::string skelPath = (dir / (std::filesystem::path(m_LogicalPath).stem().string() + ".wskeleton")).generic_string();
		if (!std::filesystem::exists(skelPath))
		{
			std::string skelErr;
			Asset::WSkeletonIO::WriteFile(skelPath, skeleton, &skelErr);
			host.RefreshAssetCatalog(skelPath);
		}

		Asset::WAnimationClipData toWrite;
		bool found = false;
		for (const auto& c : extractedClips)
		{
			if (c.Name == anim.Name)
			{
				toWrite = c;
				toWrite.SkeletonPath = skelPath;
				found = true;
				break;
			}
		}
		if (!found && !extractedClips.empty())
		{
			toWrite = extractedClips[static_cast<size_t>(m_AnimClipIndex % extractedClips.size())];
			toWrite.SkeletonPath = skelPath;
		}

		std::string err;
		if (Asset::WAnimationClipIO::WriteFile(target.generic_string(), toWrite, &err))
		{
			m_Status = "Exported clip to " + target.generic_string();
			m_StatusIsError = false;
			host.RefreshAssetCatalog(target.generic_string());
		}
		else
		{
			m_Status = "Export failed: " + err;
			m_StatusIsError = true;
		}
	}

	void ModelPreviewPanel::CreateNewClipAsset(PanelHost& host)
	{
		std::string cleanName = m_NewClipNameBuffer.empty() ? "NewClip" : m_NewClipNameBuffer;
		for (char& c : cleanName)
			if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_') c = '_';

		std::filesystem::path dir = "projects/default/assets/animation";
		if (!std::filesystem::exists(dir))
			std::filesystem::create_directories(dir);
		std::filesystem::path target = dir / (cleanName + ".wclip");

		Asset::WAnimationClipData clip;
		clip.Name = cleanName;
		clip.Duration = std::max(0.1f, m_NewClipDurationBuffer);
		clip.SkeletonPath = "engine/models/characters/mannequin.wskeleton";

		if (m_DataValid && !m_Data.Nodes.empty())
		{
			clip.Channels.reserve(m_Data.Nodes.size());
			for (const auto& node : m_Data.Nodes)
			{
				if (node.Name.empty()) continue;
				Asset::AnimationChannel ch;
				ch.BoneName = StringPool::Get().InternName(node.Name);
				ch.BoneNameStr = node.Name;
				ch.PositionKeys.push_back({ 0.0f, node.Translation });
				ch.RotationKeys.push_back({ 0.0f, node.Rotation });
				ch.ScaleKeys.push_back({ 0.0f, node.Scale });
				clip.Channels.push_back(std::move(ch));
			}
		}

		std::string err;
		if (Asset::WAnimationClipIO::WriteFile(target.generic_string(), clip, &err))
		{
			m_Status = "Created animation clip: " + target.generic_string();
			m_StatusIsError = false;
			m_NewClipOpen = false;
			host.RefreshAssetCatalog(target.generic_string());
			host.OpenModelPreview(target.generic_string());
		}
		else
		{
			m_Status = "Creation failed: " + err;
			m_StatusIsError = true;
		}
	}
}

