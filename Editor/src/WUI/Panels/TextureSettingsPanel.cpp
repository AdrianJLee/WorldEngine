#include "wldpch.h"
#include "TextureSettingsPanel.h"

#include "EditorAssetTypes.h"

#include "World/Core/Sha256.h"
#include "World/RHI/RhiDevice.h"
#include "World/Renderer/MaterialTextureCache.h"
#include "World/Renderer/Renderer.h"
#include "World/Renderer/TextureCompiler.h"
#include "World/Renderer/TextureData.h"
#include "World/WUI/WuiAccessibility.h"
#include "World/WUI/WuiLocalization.h"
#include "World/WUI/WuiTextureRegistry.h"
#include "World/WUI/WuiWidgets.h"
#include "World/WUI/Widgets/WuiChrome.h"
#include "World/WUI/Widgets/WuiModal.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <fstream>
#include <iterator>
#include <vector>

namespace World
{
	namespace
	{
		// 源图扩展名(与 TextureCompiler::BakeDirectory 的候选顺序一致)。
		const char* const kSourceCandidates[] = { ".png", ".jpg", ".jpeg", ".tga", ".bmp" };

		bool ReadFileBytes(const std::filesystem::path& path, std::vector<uint8_t>& out,
			std::string& error)
		{
			std::ifstream input(path, std::ios::binary);
			if (!input.is_open())
			{
				error = "cannot read " + path.generic_string();
				return false;
			}
			out.assign(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
			return true;
		}

		// 与 TextureCompiler 的写盘口径一致:临时文件 + 原子替换(失败则先删再换一次)。
		bool WriteFileBytesAtomic(const std::filesystem::path& path, const std::vector<uint8_t>& bytes,
			std::string& error)
		{
			std::error_code directoryError;
			if (!path.parent_path().empty())
				std::filesystem::create_directories(path.parent_path(), directoryError);
			const std::filesystem::path temporary = path.string() + ".tmp-write";
			{
				std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
				if (!output.is_open())
				{
					error = "cannot write " + temporary.generic_string();
					return false;
				}
				output.write(reinterpret_cast<const char*>(bytes.data()),
					static_cast<std::streamsize>(bytes.size()));
			}
			std::error_code renameError;
			std::filesystem::rename(temporary, path, renameError);
			if (renameError)
			{
				std::error_code removeError;
				std::filesystem::remove(path, removeError);
				renameError.clear();
				std::filesystem::rename(temporary, path, renameError);
			}
			if (renameError)
			{
				error = "cannot replace " + path.generic_string() + ": " + renameError.message();
				std::error_code cleanupError;
				std::filesystem::remove(temporary, cleanupError);
				return false;
			}
			return true;
		}

		bool WriteTextFileAtomic(const std::filesystem::path& path, const std::string& text,
			std::string& error)
		{
			const std::vector<uint8_t> bytes(text.begin(), text.end());
			return WriteFileBytesAtomic(path, bytes, error);
		}

		// 面板自己的宽度裁剪(与其它面板同口径:超宽补 '…')。
		std::string EllipsizeToWidth(const Wui::WuiContext& ctx, std::string_view text, float maxWidth,
			float fontSize)
		{
			if (maxWidth <= 0.0f || text.empty())
				return std::string();
			if (ctx.MeasureTextWidth(text, fontSize) <= maxWidth)
				return std::string(text);
			std::string result(text);
			while (!result.empty())
			{
				result.pop_back();
				while (!result.empty() && (static_cast<unsigned char>(result.back()) & 0xC0u) == 0x80u)
					result.pop_back();
				if (ctx.MeasureTextWidth(result + "…", fontSize) <= maxWidth)
					return result + "…";
			}
			return std::string();
		}

		std::string UpperAscii(std::string text)
		{
			std::transform(text.begin(), text.end(), text.begin(),
				[](unsigned char c) { return static_cast<char>(std::toupper(c)); });
			return text;
		}

	}

	namespace Editor
	{
		TextureSettingsRequests& TextureSettingsRequests::Get()
		{
			static TextureSettingsRequests requests;
			return requests;
		}

		void TextureSettingsRequests::Request(const std::string& logicalPath, Kind kind)
		{
			if (logicalPath.empty())
				return;
			m_Logical = logicalPath;
			m_Kind = kind;
			m_Pending = true;
		}

		bool TextureSettingsRequests::Take(std::string& outLogicalPath, Kind& outKind)
		{
			if (!m_Pending)
				return false;
			outLogicalPath = m_Logical;
			outKind = m_Kind;
			m_Logical.clear();
			m_Pending = false;
			return true;
		}

		bool ResolveTextureSourceLogical(const std::filesystem::path& contentRoot,
			const std::string& logicalPath, const TextureImportSettings& settings,
			std::string& outSourceLogical, std::string& outError)
		{
			outSourceLogical.clear();
			outError.clear();
			// 传进来的就是源图(内容浏览器里的 png/jpg/…):它自己就是源。
			if (!IsTextureAssetPath(logicalPath))
			{
				outSourceLogical = logicalPath;
				return true;
			}
			if (!settings.Source.empty())
			{
				// 与烘焙器同一口径:内容根相对、不许绝对路径 / '..'。
				const std::filesystem::path declared(settings.Source);
				bool escapes = declared.is_absolute() || declared.has_root_name();
				for (const std::filesystem::path& part : declared)
					if (part == "..")
						escapes = true;
				if (escapes)
				{
					outError = "source must be a content-root relative path without '..'";
					return false;
				}
				if (!TextureCompiler::IsTextureSourceExtension(LowerExtension(declared)))
				{
					outError = "source '" + settings.Source + "' is not a supported image extension";
					return false;
				}
				outSourceLogical = declared.generic_string();
				return true;
			}
			const std::filesystem::path asset(logicalPath);
			for (const char* extension : kSourceCandidates)
			{
				const std::filesystem::path candidate =
					asset.parent_path() / (asset.stem().string() + extension);
				std::error_code existsError;
				if (std::filesystem::is_regular_file(contentRoot / candidate, existsError))
				{
					outSourceLogical = candidate.generic_string();
					return true;
				}
			}
			outError = "no source image (put the image next to the asset or set `source:`)";
			return false;
		}

		TextureArtifactStatus InspectTextureArtifact(const std::filesystem::path& contentRoot,
			const std::string& sourceLogical, const TextureImportSettings* settingsOverride)
		{
			TextureArtifactStatus status;
			status.SourceLogical = sourceLogical;
			if (sourceLogical.empty())
			{
				status.State = TextureArtifactState::NoSource;
				status.Detail = "no source image";
				return status;
			}
			const std::filesystem::path sourceFile = contentRoot / sourceLogical;
			std::error_code existsError;
			if (!std::filesystem::is_regular_file(sourceFile, existsError))
			{
				status.State = TextureArtifactState::NoSource;
				status.Detail = "source image not found: " + sourceLogical;
				return status;
			}

			TextureImportSettings settings;
			if (settingsOverride != nullptr)
				settings = *settingsOverride;
			else
			{
				const std::filesystem::path assetFile = contentRoot / TextureAssetPathForSource(sourceLogical);
				std::string loadError;
				if (!LoadTextureImportSettings(assetFile, settings, loadError))
				{
					status.State = TextureArtifactState::Invalid;
					status.Detail = loadError;
					return status;
				}
			}

			const std::filesystem::path artifactFile = sourceFile.string() + ".wtexc";
			std::vector<uint8_t> artifactBytes;
			std::string readError;
			if (!ReadFileBytes(artifactFile, artifactBytes, readError))
			{
				status.State = TextureArtifactState::NoArtifact;
				status.Detail = "no artifact yet: " + artifactFile.filename().generic_string();
				return status;
			}
			TextureArtifactHeader header;
			std::string parseError;
			if (!ParseTextureArtifactHeader(artifactBytes.data(), artifactBytes.size(), header, parseError))
			{
				status.State = TextureArtifactState::Invalid;
				status.Detail = parseError;
				return status;
			}
			status.Header = header;

			std::vector<uint8_t> sourceBytes;
			if (!ReadFileBytes(sourceFile, sourceBytes, readError))
			{
				status.State = TextureArtifactState::Invalid;
				status.Detail = readError;
				return status;
			}
			const Crypto::Sha256Digest digest = Crypto::Sha256(sourceBytes);
			if (std::memcmp(digest.Bytes, header.SourceSha256, sizeof(digest.Bytes)) != 0)
			{
				status.State = TextureArtifactState::Stale;
				status.Detail = "source image changed since the last bake";
				return status;
			}
			if (header.SettingsHash != settings.Hash())
			{
				status.State = TextureArtifactState::Stale;
				status.Detail = "settings changed since the last bake";
				return status;
			}
			status.State = TextureArtifactState::Fresh;
			return status;
		}

		bool CreateTextureAssetForSource(const std::filesystem::path& contentRoot,
			const std::string& sourceLogical, std::string* outAssetLogical, std::string& outError)
		{
			outError.clear();
			if (outAssetLogical)
				outAssetLogical->clear();
			const std::string assetLogical = TextureAssetPathForSource(sourceLogical);
			if (assetLogical.empty() || !IsTextureAssetPath(assetLogical))
			{
				outError = "cannot derive a .wtex path from '" + sourceLogical + "'";
				return false;
			}
			const std::filesystem::path sourceFile = contentRoot / sourceLogical;
			std::error_code existsError;
			if (!std::filesystem::is_regular_file(sourceFile, existsError))
			{
				outError = "source image not found: " + sourceLogical;
				return false;
			}
			const std::filesystem::path assetFile = contentRoot / assetLogical;
			if (std::filesystem::exists(assetFile, existsError))
			{
				outError = "texture asset already exists: " + assetLogical;
				return false;
			}
			const std::string text = TextureImportSettings {}.Serialize();
			if (!WriteTextFileAtomic(assetFile, text, outError))
				return false;
			if (outAssetLogical)
				*outAssetLogical = assetLogical;
			WLD_CORE_INFO("[texture] created texture asset {0} for {1}", assetLogical, sourceLogical);
			return true;
		}

		bool BakeTextureArtifactNow(const std::filesystem::path& contentRoot,
			const std::string& sourceLogical, const TextureImportSettings& settings, std::string& outError)
		{
			outError.clear();
			if (sourceLogical.empty())
			{
				outError = "no source image";
				return false;
			}
			const std::filesystem::path sourceFile = contentRoot / sourceLogical;
			std::vector<uint8_t> artifactBytes;
			TextureArtifactHeader header;
			std::string bakeError;
			if (!TextureCompiler::BakeFile(sourceFile, settings, artifactBytes, header, bakeError))
			{
				outError = bakeError;
				return false;
			}
			const std::filesystem::path artifactFile = sourceFile.string() + ".wtexc";
			std::string writeError;
			if (!WriteFileBytesAtomic(artifactFile, artifactBytes, writeError))
			{
				outError = writeError;
				return false;
			}
			// 材质贴图缓存按**逻辑路径**失效:下一次 Get 重新读盘(命中新产物)。
			MaterialTextureCache::Get().Invalidate(sourceLogical);
			WLD_CORE_INFO("[texture] baked {0} -> {1} ({2}, {3}x{4}, {5} mips)", sourceLogical,
				artifactFile.filename().generic_string(), TextureBlockFormatName(header.Format),
				header.Width, header.Height, header.MipCount);
			return true;
		}
	}

	TextureSettingsPanel::TextureSettingsPanel()
	{
		m_ContentRoot = std::filesystem::path(std::string(WLD_ASSETPATH));
	}

	TextureSettingsPanel::~TextureSettingsPanel() = default;

	void TextureSettingsPanel::EnsureContentRoot()
	{
		if (m_ContentRoot.empty())
			m_ContentRoot = std::filesystem::path(std::string(WLD_ASSETPATH));
	}

	std::string TextureSettingsPanel::AssetAbsolutePath() const
	{
		if (m_AssetLogical.empty())
			return std::string();
		return (m_ContentRoot / m_AssetLogical).string();
	}

	std::string TextureSettingsPanel::SourceAbsolutePath() const
	{
		if (m_SourceLogical.empty())
			return std::string();
		return (m_ContentRoot / m_SourceLogical).string();
	}

	std::string TextureSettingsPanel::ArtifactAbsolutePath() const
	{
		const std::string source = SourceAbsolutePath();
		return source.empty() ? std::string() : source + ".wtexc";
	}

	void TextureSettingsPanel::RequestOpenAsset(const std::string& logicalPath, bool resetConfirm)
	{
		EnsureContentRoot();
		std::string normalized = logicalPath;
		std::replace(normalized.begin(), normalized.end(), '\\', '/');
		// 源图(或任何非 `.wtex`)→ 资产路径(源图换扩展名),与内核同一条换算。
		if (!IsTextureAssetPath(normalized))
		{
			const std::string extension = LowerExtension(normalized);
			for (const char* candidate : kSourceCandidates)
			{
				if (extension == candidate)
				{
					normalized = TextureAssetPathForSource(normalized);
					break;
				}
			}
		}
		const bool sameAsset = normalized == m_AssetLogical;
		m_AssetLogical = normalized;
		if (!sameAsset)
		{
			// 换资产 = 丢弃未保存的编辑并重读(与材质面板换文档同口径:入口已经确认过)。
			m_Loaded = false;
			m_Dirty = false;
			m_AssetStampValid = false;
			m_Status.clear();
			m_StatusIsError = false;
			m_SourceStatsPath.clear();
			m_SourcePixels.clear();
			m_SourcePixelsSource.clear();
			m_DiskChanged = false;
			m_Scroll = 0.0f;
		}
		if (resetConfirm)
			m_PendingResetConfirm = true;
		WLD_CORE_INFO("[texture] settings panel: open '{0}'{1}", m_AssetLogical,
			resetConfirm ? " (reset confirm)" : "");
	}

	void TextureSettingsPanel::ReloadFromDisk(bool keepStatus)
	{
		EnsureContentRoot();
		m_LoadError.clear();
		m_DiskChanged = false;
		if (!keepStatus)
		{
			m_Status.clear();
			m_StatusIsError = false;
		}
		const std::filesystem::path assetFile = AssetAbsolutePath();
		std::error_code existsError;
		m_AssetFileExists = !assetFile.empty() && std::filesystem::is_regular_file(assetFile, existsError);
		std::string loadError;
		if (!LoadTextureImportSettings(assetFile, m_Settings, loadError))
		{
			m_LoadError = loadError;
			m_Loaded = false;
			m_SourceLogical.clear();
			m_SourceBuffer.clear();
			m_Artifact = Editor::TextureArtifactStatus {};
			m_Artifact.State = Editor::TextureArtifactState::Invalid;
			m_Artifact.Detail = loadError;
			return;
		}
		m_Loaded = true;
		m_Dirty = false;
		m_SourceBuffer = m_Settings.Source;
		m_AssetStampValid = false;
		if (m_AssetFileExists)
		{
			std::error_code stampError;
			const std::filesystem::file_time_type stamp = std::filesystem::last_write_time(assetFile, stampError);
			if (!stampError)
			{
				m_AssetStamp = stamp;
				m_AssetStampValid = true;
			}
		}

		std::string resolveError;
		std::string source;
		if (!Editor::ResolveTextureSourceLogical(m_ContentRoot, m_AssetLogical, m_Settings, source, resolveError))
		{
			m_SourceLogical.clear();
			m_Artifact = Editor::TextureArtifactStatus {};
			m_Artifact.State = Editor::TextureArtifactState::NoSource;
			m_Artifact.Detail = resolveError;
			return;
		}
		m_SourceLogical = source;
		RefreshArtifactState();
	}

	void TextureSettingsPanel::RefreshArtifactState(bool force)
	{
		if (m_SourceLogical.empty())
			return;
		// 指纹:(源图 / `.wtex` / `.wtexc`) 的 mtime + 内存设置 hash。三项都没动 = 复用上一次判定
		// (判定要读源字节算 sha256;源图可到 4K,不能每帧算)。
		const std::filesystem::path sourceFile = m_ContentRoot / m_SourceLogical;
		const std::filesystem::path assetFile =
			m_ContentRoot / TextureAssetPathForSource(m_SourceLogical);
		const std::filesystem::path artifactFile = sourceFile.string() + ".wtexc";
		std::error_code stampError;
		const std::filesystem::file_time_type sourceStamp =
			std::filesystem::last_write_time(sourceFile, stampError);
		const bool assetExists = std::filesystem::is_regular_file(assetFile, stampError);
		const std::filesystem::file_time_type assetStamp = assetExists
			? std::filesystem::last_write_time(assetFile, stampError)
			: std::filesystem::file_time_type {};
		const bool artifactExists = std::filesystem::is_regular_file(artifactFile, stampError);
		const std::filesystem::file_time_type artifactStamp = artifactExists
			? std::filesystem::last_write_time(artifactFile, stampError)
			: std::filesystem::file_time_type {};
		const uint64_t settingsHash = m_Settings.Hash();
		if (!force && m_ArtifactStampsValid && m_ArtifactSettingsHash == settingsHash
			&& m_ArtifactArtifactExists == artifactExists && m_ArtifactSourceStamp == sourceStamp
			&& m_ArtifactAssetStamp == assetStamp && m_ArtifactArtifactStamp == artifactStamp)
			return;
		m_ArtifactStampsValid = true;
		m_ArtifactSettingsHash = settingsHash;
		m_ArtifactArtifactExists = artifactExists;
		m_ArtifactSourceStamp = sourceStamp;
		m_ArtifactAssetStamp = assetStamp;
		m_ArtifactArtifactStamp = artifactStamp;
		// 面板有未落盘的编辑时,用**内存设置**判定"需重烘"(改一个字段立刻显示待烘)。
		m_Artifact = Editor::InspectTextureArtifact(m_ContentRoot, m_SourceLogical,
			m_Dirty ? &m_Settings : nullptr);
	}

	void TextureSettingsPanel::RefreshSourceStats(const std::string& absoluteSource)
	{
		if (absoluteSource.empty())
		{
			m_SourceWidth = 0;
			m_SourceHeight = 0;
			m_SourceValid = false;
			m_SourceStatsPath.clear();
			m_SourcePixels.clear();
			m_SourcePixelsSource.clear();
			return;
		}
		if (m_SourceStatsPath == absoluteSource)
			return;
		// 源图解码一次:尺寸进状态行,像素留给缩略图上传(上传成功后立刻释放)。
		const TextureData data = LoadTextureData(absoluteSource, /*flipVertically*/ false);
		m_SourceStatsPath = absoluteSource;
		m_SourceWidth = data.Width;
		m_SourceHeight = data.Height;
		m_SourceValid = data.Valid;
		m_SourcePixels = data.Pixels;
		m_SourcePixelsSource = absoluteSource;
	}

	void TextureSettingsPanel::ReleaseThumbnail()
	{
		m_Thumbnail = nullptr;
		m_ThumbnailId = 0;
		m_ThumbnailSource.clear();
		m_ThumbnailGeneration = ~0u;
		m_ThumbnailEpoch = ~0u;
	}

	void TextureSettingsPanel::EnsureThumbnail(PanelHost& host, const std::string& absoluteSource)
	{
		Wui::WuiTextureRegistry& registry = Wui::WuiTextureRegistry::Get();
		const uint32_t generation = registry.Generation();
		const uint32_t epoch = host.TextureEpoch();
		// 设备/GL 上下文重建(注册表整表清空或宿主纪元变化)⇒ 旧句柄失效,重建。
		const bool stale = m_ThumbnailId == 0 || m_ThumbnailSource != absoluteSource
			|| m_ThumbnailGeneration != generation || m_ThumbnailEpoch != epoch;
		if (!stale)
			return;
		ReleaseThumbnail();
		m_ThumbnailSource = absoluteSource;
		m_ThumbnailGeneration = generation;
		m_ThumbnailEpoch = epoch;
		if (absoluteSource.empty() || !m_SourceValid)
			return;
		std::vector<uint8_t> pixels;
		uint32_t width = m_SourceWidth;
		uint32_t height = m_SourceHeight;
		if (m_SourcePixelsSource == absoluteSource && !m_SourcePixels.empty())
			pixels = m_SourcePixels;
		else
		{
			// 尺寸读数来自另一张源图(或还没读):补一次解码(翻转 = 与 WUI 的 uv {0,1,1,-1} 配套)。
			const TextureData data = LoadTextureData(absoluteSource, /*flipVertically*/ true);
			if (!data.Valid)
				return;
			pixels = data.Pixels;
			width = data.Width;
			height = data.Height;
		}
		if (pixels.empty() || width == 0 || height == 0)
			return;
		if (m_SourcePixelsSource == absoluteSource)
		{
			// 上传用的像素按 {0,1,1,-1} 显示要求是**翻转**过的;未翻转的那份只服务尺寸读数。
			const size_t rowBytes = static_cast<size_t>(width) * 4u;
			std::vector<uint8_t> flipped(pixels.size());
			for (uint32_t row = 0; row < height; ++row)
				std::memcpy(flipped.data() + static_cast<size_t>(row) * rowBytes,
					pixels.data() + static_cast<size_t>(height - 1u - row) * rowBytes, rowBytes);
			pixels.swap(flipped);
		}
		Rhi::Handle<Rhi::Device> device = Renderer::GetDevice();
		if (!device)
			return;
		Rhi::TextureDesc desc;
		desc.Format = Rhi::Format::R8G8B8A8_UNORM;
		desc.Extent = { width, height, 1 };
		desc.MipLevels = 1;
		desc.Usage = Rhi::TextureUsageSampled;
		desc.DebugName = "texture-settings-thumbnail";
		Rhi::Handle<Rhi::Texture> texture = device->CreateTexture(desc);
		if (!texture)
			return;
		texture->SetData(pixels.data(), static_cast<uint64_t>(pixels.size()));
		m_Thumbnail = texture;
		m_ThumbnailId = registry.Register(texture);
		// 缩略图已经上传:释放尺寸读数那一份像素(避免 4K 源长期占内存)。
		if (m_SourcePixelsSource == absoluteSource)
		{
			m_SourcePixels.clear();
			m_SourcePixelsSource.clear();
		}
	}

	void TextureSettingsPanel::SyncWithDisk(const Wui::WuiContext& ctx)
	{
		if (m_AssetLogical.empty())
			return;
		EnsureContentRoot();
		const bool visibleGap = m_RenderedOnce && ctx.Frame() > m_LastRenderFrame + 1;
		const std::filesystem::path assetFile = AssetAbsolutePath();
		std::error_code stampError;
		const bool exists = std::filesystem::is_regular_file(assetFile, stampError);
		const std::filesystem::file_time_type stamp =
			exists ? std::filesystem::last_write_time(assetFile, stampError) : std::filesystem::file_time_type {};
		const bool changed = exists != m_AssetFileExists
			|| (exists && (!m_AssetStampValid || stampError || stamp != m_AssetStamp));
		if (!m_Loaded)
		{
			ReloadFromDisk(true);
			return;
		}
		if (changed || visibleGap)
		{
			if (m_Dirty && changed)
			{
				// 有未保存编辑:绝不静默覆盖用户输入,只提示(与材质/着色器面板同一口径)。
				m_DiskChanged = true;
			}
			else
			{
				ReloadFromDisk(true);
			}
		}
	}

	std::string TextureSettingsPanel::ArtifactSummary() const
	{
		if (m_Artifact.State == Editor::TextureArtifactState::NoSource)
			return Wui::Tr("panel.texture.status.no_source", "Source image not found");
		if (m_Artifact.State == Editor::TextureArtifactState::Invalid)
			return Wui::TrFormat("panel.texture.status.invalid", "Artifact unreadable: {detail}",
				{ { "detail", m_Artifact.Detail } });
		if (m_Artifact.State == Editor::TextureArtifactState::NoArtifact)
		{
			return Wui::TrFormat("panel.texture.status.no_artifact",
				"No artifact yet ({name}) — press Apply or Reimport to bake it",
				{ { "name", std::filesystem::path(ArtifactAbsolutePath()).filename().generic_string() } });
		}
		const TextureArtifactHeader& header = m_Artifact.Header;
		const std::string text = Wui::TrFormat("panel.texture.status.artifact",
			"Artifact {format} {width}x{height}, {mips} mip(s)",
			{ { "format", UpperAscii(TextureBlockFormatName(header.Format)) },
				{ "width", std::to_string(header.Width) }, { "height", std::to_string(header.Height) },
				{ "mips", std::to_string(header.MipCount) } });
		if (m_Artifact.State == Editor::TextureArtifactState::Fresh)
			return text;
		return text + " — " + Wui::Tr("panel.texture.status.stale_hint",
			"needs re-bake (source or settings changed)");
	}

	float TextureSettingsPanel::FieldRowHeight(const Wui::WuiTheme& theme) const
	{
		return std::max(theme.RowHeight, 26.0f) + 4.0f;
	}

	void TextureSettingsPanel::OnRender(Wui::WuiContext& ctx, const Wui::WuiRect& rect, PanelHost& host)
	{
		m_Ctx = &ctx;
		EnsureContentRoot();
		const Wui::WuiTheme& theme = host.Theme();
		SyncWithDisk(ctx);

		if (m_AssetLogical.empty())
		{
			Wui::EmptyState(ctx, rect, std::string(),
				Wui::Tr("panel.texture.empty.title", "No texture asset open"),
				Wui::Tr("panel.texture.empty.hint",
					"Double-click a .wtex asset in the Content Browser, or right-click a source image and "
					"pick Create Texture Asset, to edit its import settings here."),
				std::string(), 0, theme);
			m_LastRenderFrame = ctx.Frame();
			m_RenderedOnce = true;
			return;
		}

		const std::string absoluteSource = SourceAbsolutePath();
		RefreshSourceStats(absoluteSource);
		RefreshArtifactState();
		EnsureThumbnail(host, absoluteSource);

		if (m_PendingResetConfirm)
		{
			m_PendingResetConfirm = false;
			if (m_Loaded && m_AssetFileExists)
			{
				m_ResetConfirmOpen = true;
				ctx.SetModal(Wui::HashId("texture.reset.modal"));
				host.SetPanelModalOwner(Id());
				ctx.RecordOp("texture", "reset-ask", m_AssetLogical, "confirm delete .wtex");
			}
		}

		float y = rect.Y + theme.Pad;
		DrawHeader(ctx, theme, host, rect, y);

		// 底部动作条固定(不随内容滚动):脚本/用户任何滚动位置都能点到 Apply。
		const float actionsHeight = theme.ControlHeight + theme.Pad * 2.0f;
		Wui::WuiRect actionsRect { rect.X, rect.Y + rect.H - actionsHeight, rect.W, actionsHeight };
		Wui::WuiRect bodyRect { rect.X, y, rect.W, std::max(0.0f, actionsRect.Y - y) };

		// 内容高度:预览块 + 12 行字段 + 状态两行(滚动区按它算)。
		const float rowH = FieldRowHeight(theme);
		const float previewSide = std::min(200.0f, std::max(120.0f, bodyRect.W * 0.42f));
		const bool stacked = bodyRect.W < 560.0f;
		const float previewBlock = stacked ? previewSide + theme.FontSizeSmall * 3.0f + theme.Pad * 2.0f
			: previewSide + theme.FontSizeSmall * 3.0f + theme.Pad * 2.0f;
		const int fieldRows = 12;
		const float contentHeight = theme.Pad * 2.0f + (stacked ? previewBlock : 0.0f)
			+ static_cast<float>(fieldRows) * rowH + theme.FontSizeSmall * 3.0f + theme.Pad * 3.0f;

		Wui::BeginScrollArea(ctx, bodyRect, contentHeight, m_Scroll, theme);
		float cursorY = bodyRect.Y + theme.Pad;
		Wui::WuiRect previewRect;
		Wui::WuiRect fieldsRect = bodyRect;
		if (stacked)
		{
			previewRect = { bodyRect.X + theme.Pad, cursorY, std::max(0.0f, bodyRect.W - theme.Pad * 2.0f),
				previewBlock };
			cursorY += previewBlock;
			fieldsRect = { bodyRect.X, cursorY, bodyRect.W, 0.0f };
		}
		else
		{
			const float leftW = previewBlock + theme.Pad * 2.0f;
			previewRect = { bodyRect.X + theme.Pad, cursorY, leftW, previewBlock };
			fieldsRect = { bodyRect.X + leftW, cursorY, std::max(0.0f, bodyRect.W - leftW), 0.0f };
		}
		DrawPreview(ctx, theme, previewRect);

		float fieldsY = fieldsRect.Y + theme.Pad;
		DrawFields(ctx, theme, fieldsRect, fieldsY);
		DrawStatusLine(ctx, theme, bodyRect, fieldsY);
		Wui::EndScrollArea(ctx);

		DrawActions(ctx, theme, host, actionsRect);

		if (m_ResetConfirmOpen)
			DrawResetConfirmModal(ctx, theme, host);

		m_LastRenderFrame = ctx.Frame();
		m_RenderedOnce = true;
	}

	void TextureSettingsPanel::DrawHeader(Wui::WuiContext& ctx, const Wui::WuiTheme& theme,
		PanelHost& host, const Wui::WuiRect& rect, float& y)
	{
		(void)host;
		const float x = rect.X + theme.Pad;
		const float width = std::max(0.0f, rect.W - theme.Pad * 2.0f);

		// 状态徽标(右侧):"已烘焙"/"需重烘"/"还没有产物" —— 同时登记 a11y 节点(ui.tree 证据)。
		std::string badgeText;
		std::string badgeValue;
		Wui::WuiColor badgeFill = theme.ActiveBg;
		switch (m_Artifact.State)
		{
			case Editor::TextureArtifactState::Fresh:
				badgeText = Wui::Tr("panel.texture.status.baked", "Baked");
				badgeValue = "fresh";
				badgeFill = theme.Success;
				break;
			case Editor::TextureArtifactState::Stale:
				badgeText = Wui::Tr("panel.texture.status.needs_bake", "Needs re-bake");
				badgeValue = "stale";
				badgeFill = theme.Warning;
				break;
			case Editor::TextureArtifactState::NoSource:
				badgeText = Wui::Tr("panel.texture.status.no_source_short", "No source");
				badgeValue = "no-source";
				badgeFill = theme.Danger;
				break;
			case Editor::TextureArtifactState::Invalid:
				badgeText = Wui::Tr("panel.texture.status.invalid_short", "Unreadable");
				badgeValue = "invalid";
				badgeFill = theme.Danger;
				break;
			case Editor::TextureArtifactState::NoArtifact:
			default:
				badgeText = Wui::Tr("panel.texture.status.not_baked", "Not baked");
				badgeValue = "none";
				break;
		}
		const float badgeSize = theme.FontSizeCaption;
		const float badgeW = ctx.MeasureTextWidth(badgeText, badgeSize) + theme.PadSmall * 2.0f;
		const float badgeH = badgeSize + 4.0f;
		const Wui::WuiRect badgeRect { rect.X + rect.W - theme.Pad - badgeW, y, badgeW, badgeH };
		Wui::Badge(ctx, badgeRect, badgeText, badgeFill,
			Wui::WuiColor { 1.0f, 1.0f, 1.0f, 1.0f }, theme, badgeSize);
		{
			Wui::WuiAccessNode node;
			node.Id = Wui::HashId("texture.status.badge");
			node.Window = Wui::WuiAccessibility::Get().CurrentWindow();
			node.Panel = Wui::WuiAccessibility::Get().CurrentPanel();
			node.Kind = "badge";
			node.Label = badgeText;
			node.Value = badgeValue;
			node.Tooltip = ArtifactSummary();
			node.Rect = badgeRect;
			node.Enabled = true;
			node.Interactive = false;
			Wui::WuiAccessibility::Get().Register(node);
		}

		const std::string title = EllipsizeToWidth(ctx, m_AssetLogical,
			std::max(0.0f, width - badgeW - theme.Pad), theme.FontSizeTitle);
		Wui::Label(ctx, { x, y }, title, theme.Text, theme.FontSizeTitle);
		y += theme.FontSizeTitle + theme.PadSmall;

		const std::string sourceText = m_SourceLogical.empty()
			? Wui::Tr("panel.texture.source.none", "Source image: (not found)")
			: (Wui::Tr("panel.texture.source", "Source image: ") + m_SourceLogical);
		Wui::Label(ctx, { x, y },
			EllipsizeToWidth(ctx, sourceText, width, theme.FontSizeSmall), theme.TextMuted,
			theme.FontSizeSmall);
		y += theme.FontSizeSmall + theme.Pad;

		if (!m_LoadError.empty())
		{
			Wui::Label(ctx, { x, y },
				EllipsizeToWidth(ctx, m_LoadError, width, theme.FontSizeSmall), theme.Danger,
				theme.FontSizeSmall);
			y += theme.FontSizeSmall + theme.PadSmall;
		}
	}

	void TextureSettingsPanel::DrawPreview(Wui::WuiContext& ctx, const Wui::WuiTheme& theme,
		const Wui::WuiRect& rect)
	{
		const float side = std::min(rect.W, rect.H - theme.FontSizeSmall * 3.0f - theme.Pad * 2.0f);
		const Wui::WuiRect box { rect.X, rect.Y, std::max(0.0f, side), std::max(0.0f, side) };
		Wui::DrawPanelSurface(ctx, box, theme);
		if (m_ThumbnailId != 0 && m_SourceValid && m_SourceWidth > 0 && m_SourceHeight > 0)
		{
			// 等比居中(源图宽高比放进方框内)。
			const float scale = std::min(box.W / static_cast<float>(m_SourceWidth),
				box.H / static_cast<float>(m_SourceHeight));
			const float drawW = static_cast<float>(m_SourceWidth) * scale;
			const float drawH = static_cast<float>(m_SourceHeight) * scale;
			const Wui::WuiRect target { box.X + (box.W - drawW) * 0.5f, box.Y + (box.H - drawH) * 0.5f,
				drawW, drawH };
			Wui::Image(ctx, target, m_ThumbnailId, { 0, 1, 1, -1 }, theme);
		}
		else
		{
			Wui::Label(ctx, { box.X + theme.Pad, box.Y + theme.Pad },
				Wui::Tr("panel.texture.preview.unavailable", "Preview unavailable"), theme.TextMuted,
				theme.FontSizeSmall);
		}
		float y = box.Y + box.H + theme.PadSmall;
		const std::string sizeText = (m_SourceValid && m_SourceWidth > 0)
			? Wui::TrFormat("panel.texture.source_size", "Source {width}x{height} px",
				{ { "width", std::to_string(m_SourceWidth) },
					{ "height", std::to_string(m_SourceHeight) } })
			: Wui::Tr("panel.texture.source_size.unknown", "Source size: unknown");
		Wui::Label(ctx, { rect.X, y }, EllipsizeToWidth(ctx, sizeText, rect.W, theme.FontSizeSmall),
			theme.TextMuted, theme.FontSizeSmall);
		y += theme.FontSizeSmall + 2.0f;
		const std::string hint = Wui::Tr("panel.texture.preview.hint",
			"Preview shows the source image (what gets baked).");
		Wui::Label(ctx, { rect.X, y }, EllipsizeToWidth(ctx, hint, rect.W, theme.FontSizeCaption),
			theme.TextDisabled, theme.FontSizeCaption);
	}

	void TextureSettingsPanel::DrawFields(Wui::WuiContext& ctx, const Wui::WuiTheme& theme,
		const Wui::WuiRect& rect, float& y)
	{
		const float rowH = FieldRowHeight(theme);
		const float labelW = std::min(160.0f, std::max(90.0f, rect.W * 0.42f));
		const float controlX = rect.X + labelW;
		const float controlW = std::max(80.0f, rect.W - labelW - theme.Pad * 2.0f);
		const float controlH = std::max(22.0f, rowH - 4.0f);
		auto rowRect = [&](float rowTop) { return Wui::WuiRect { controlX, rowTop, controlW, controlH }; };
		auto drawLabel = [&](const std::string& text, const std::string& tooltip, float rowTop)
		{
			const Wui::WuiRect labelRect { rect.X, rowTop, labelW - theme.PadSmall, controlH };
			Wui::Label(ctx, { labelRect.X, rowTop + (controlH - theme.FontSizeBody) * 0.5f },
				EllipsizeToWidth(ctx, text, labelRect.W, theme.FontSizeBody), theme.TextMuted,
				theme.FontSizeBody);
			if (!tooltip.empty())
				Wui::Tooltip(ctx, labelRect, tooltip);
		};

		// ---- 源图(`source:` 可改;空 = 同目录同主名)----
		drawLabel(Wui::Tr("panel.texture.source", "Source image"),
			Wui::Tr("panel.texture.source.tooltip",
				"Content-root relative path of the image this asset bakes. Leave it empty to use the "
				"sibling image with the same name (.png / .jpg / .jpeg / .tga / .bmp)."),
			y);
		{
			const Wui::WuiRect field = rowRect(y);
			Wui::TextFieldA11y a11y;
			a11y.Label = Wui::Tr("panel.texture.source", "Source image");
			a11y.Placeholder = Wui::Tr("panel.texture.source.placeholder",
				"(sibling image with the same name)");
			const bool committed = Wui::TextField(ctx, Wui::HashId("texture.field.source"), field,
				m_SourceBuffer, theme, nullptr, &a11y);
			if (committed && m_SourceBuffer != m_Settings.Source)
			{
				m_Settings.Source = m_SourceBuffer;
				m_Dirty = true;
				m_SourceStatsPath.clear();
				m_SourcePixels.clear();
				m_SourcePixelsSource.clear();
				RefreshArtifactState();
				if (m_Ctx) m_Ctx->RecordOp("texture", "edit", m_AssetLogical, "source");
			}
		}
		y += rowH;

		// ---- usage ----
		{
			const std::vector<std::string> options {
				Wui::Tr("panel.texture.usage.color", "Color (albedo)"),
				Wui::Tr("panel.texture.usage.normal", "Normal (R/G, BC5)"),
				Wui::Tr("panel.texture.usage.data", "Data (linear)"),
				Wui::Tr("panel.texture.usage.hdr", "HDR"),
				Wui::Tr("panel.texture.usage.ui", "UI (uncompressed)"),
			};
			int selected = static_cast<int>(m_Settings.Usage);
			drawLabel(Wui::Tr("panel.texture.usage", "Usage"),
				Wui::Tr("panel.texture.usage.tooltip",
					"How the texture is sampled: it picks the default compression / sRGB / mip policy "
					"(color → BC7, normal → BC5 + rebuilt Z, data → BC4, UI → uncompressed, no mips)."),
				y);
			if (Wui::Combo(ctx, Wui::HashId("texture.field.usage"), rowRect(y),
					Wui::Tr("panel.texture.usage", "Usage"), options, selected, theme)
				&& selected >= 0 && selected <= static_cast<int>(TextureUsage::Ui))
			{
				m_Settings.Usage = static_cast<TextureUsage>(selected);
				m_Dirty = true;
				RefreshArtifactState();
				if (m_Ctx) m_Ctx->RecordOp("texture", "edit", m_AssetLogical, "usage");
			}
			y += rowH;
		}

		// ---- compression ----
		{
			const std::vector<std::string> options {
				Wui::Tr("panel.texture.option.auto", "Auto (usage)"),
				Wui::Tr("panel.texture.compression.none", "None (RGBA8)"),
				Wui::Tr("panel.texture.compression.bc7", "BC7"),
				Wui::Tr("panel.texture.compression.bc5", "BC5"),
				Wui::Tr("panel.texture.compression.bc4", "BC4"),
				Wui::Tr("panel.texture.compression.bc1", "BC1"),
				Wui::Tr("panel.texture.compression.bc3", "BC3"),
			};
			int selected = static_cast<int>(m_Settings.Compression);
			drawLabel(Wui::Tr("panel.texture.compression", "Compression"),
				Wui::Tr("panel.texture.compression.tooltip",
					"Block format of the baked artifact. Auto follows usage. Re-bake (Apply / Reimport) "
					"to make a change take effect."),
				y);
			if (Wui::Combo(ctx, Wui::HashId("texture.field.compression"), rowRect(y),
					Wui::Tr("panel.texture.compression", "Compression"), options, selected, theme)
				&& selected >= 0 && selected <= static_cast<int>(TextureCompression::BC3))
			{
				m_Settings.Compression = static_cast<TextureCompression>(selected);
				m_Dirty = true;
				RefreshArtifactState();
				if (m_Ctx) m_Ctx->RecordOp("texture", "edit", m_AssetLogical, "compression");
			}
			y += rowH;
		}

		// ---- sRGB(三态:自动(usage)/ 是 / 否)----
		{
			const std::vector<std::string> options {
				Wui::Tr("panel.texture.option.auto", "Auto (usage)"),
				Wui::Tr("panel.texture.option.on", "On"),
				Wui::Tr("panel.texture.option.off", "Off"),
			};
			int selected = m_Settings.SrgbExplicit ? (m_Settings.Srgb ? 1 : 2) : 0;
			drawLabel(Wui::Tr("panel.texture.srgb", "sRGB"),
				Wui::Tr("panel.texture.srgb.tooltip",
					"Hardware sRGB decode on sampling. Auto = on for color / UI usage, off for data "
					"textures (normal / AO / roughness) — the most common source of wrong-looking colors."),
				y);
			if (Wui::Combo(ctx, Wui::HashId("texture.field.srgb"), rowRect(y),
					Wui::Tr("panel.texture.srgb", "sRGB"), options, selected, theme))
			{
				if (selected == 0)
					m_Settings.SrgbExplicit = false;
				else
				{
					m_Settings.SrgbExplicit = true;
					m_Settings.Srgb = selected == 1;
				}
				m_Dirty = true;
				RefreshArtifactState();
				if (m_Ctx) m_Ctx->RecordOp("texture", "edit", m_AssetLogical, "srgb");
			}
			y += rowH;
		}

		// ---- mipmaps(三态)----
		{
			const std::vector<std::string> options {
				Wui::Tr("panel.texture.option.auto", "Auto (usage)"),
				Wui::Tr("panel.texture.option.on", "On"),
				Wui::Tr("panel.texture.option.off", "Off"),
			};
			int selected = m_Settings.MipmapsExplicit ? (m_Settings.Mipmaps ? 1 : 2) : 0;
			drawLabel(Wui::Tr("panel.texture.mipmaps", "Mipmaps"),
				Wui::Tr("panel.texture.mipmaps.tooltip",
					"Bake a mip chain (auto = on for every usage except UI). Mips fix the shimmering of "
					"distant surfaces; UI atlases stay single-level."),
				y);
			if (Wui::Combo(ctx, Wui::HashId("texture.field.mipmaps"), rowRect(y),
					Wui::Tr("panel.texture.mipmaps", "Mipmaps"), options, selected, theme))
			{
				if (selected == 0)
					m_Settings.MipmapsExplicit = false;
				else
				{
					m_Settings.MipmapsExplicit = true;
					m_Settings.Mipmaps = selected == 1;
				}
				m_Dirty = true;
				RefreshArtifactState();
				if (m_Ctx) m_Ctx->RecordOp("texture", "edit", m_AssetLogical, "mipmaps");
			}
			y += rowH;
		}

		// ---- mip_filter ----
		{
			const std::vector<std::string> options {
				Wui::Tr("panel.texture.mip_filter.box", "Box"),
				Wui::Tr("panel.texture.mip_filter.gamma", "Gamma-correct"),
			};
			int selected = static_cast<int>(m_Settings.MipFilter);
			drawLabel(Wui::Tr("panel.texture.mip_filter", "Mip filter"),
				Wui::Tr("panel.texture.mip_filter.tooltip",
					"Downsampling filter for the mip chain: gamma-correct averages in linear space "
					"(sRGB textures keep their brightness), box averages the raw bytes."),
				y);
			if (Wui::Combo(ctx, Wui::HashId("texture.field.mip_filter"), rowRect(y),
					Wui::Tr("panel.texture.mip_filter", "Mip filter"), options, selected, theme)
				&& selected >= 0 && selected <= static_cast<int>(TextureMipFilter::GammaCorrect))
			{
				m_Settings.MipFilter = static_cast<TextureMipFilter>(selected);
				m_Dirty = true;
				RefreshArtifactState();
				if (m_Ctx) m_Ctx->RecordOp("texture", "edit", m_AssetLogical, "mip_filter");
			}
			y += rowH;
		}

		// ---- max_size ----
		{
			int64_t value = static_cast<int64_t>(m_Settings.MaxSize);
			drawLabel(Wui::Tr("panel.texture.max_size", "Max size"),
				Wui::Tr("panel.texture.max_size.tooltip",
					"Longest edge of the baked image in pixels; 0 = unlimited. Larger sources are scaled "
					"down proportionally (values under 4 are invalid)."),
				y);
			Wui::WuiNumberStyle style;
			style.Unit = "px";
			style.Steppers = true;
			if (Wui::NumberFieldInt(ctx, Wui::HashId("texture.field.max_size"), rowRect(y), value, 0,
					16384, theme, style)
				&& value >= 0)
			{
				m_Settings.MaxSize = static_cast<uint32_t>(value);
				m_Dirty = true;
				RefreshArtifactState();
				if (m_Ctx) m_Ctx->RecordOp("texture", "edit", m_AssetLogical, "max_size");
			}
			y += rowH;
		}

		// ---- wrap ----
		{
			const std::vector<std::string> options {
				Wui::Tr("panel.texture.wrap.repeat", "Repeat"),
				Wui::Tr("panel.texture.wrap.clamp", "Clamp"),
				Wui::Tr("panel.texture.wrap.mirror", "Mirror"),
			};
			int selected = static_cast<int>(m_Settings.Wrap);
			drawLabel(Wui::Tr("panel.texture.wrap", "Wrap"),
				Wui::Tr("panel.texture.wrap.tooltip",
					"Sampler address mode, baked into the artifact header and applied by the runtime."),
				y);
			if (Wui::Combo(ctx, Wui::HashId("texture.field.wrap"), rowRect(y),
					Wui::Tr("panel.texture.wrap", "Wrap"), options, selected, theme)
				&& selected >= 0 && selected <= static_cast<int>(TextureWrap::Mirror))
			{
				m_Settings.Wrap = static_cast<TextureWrap>(selected);
				m_Dirty = true;
				RefreshArtifactState();
				if (m_Ctx) m_Ctx->RecordOp("texture", "edit", m_AssetLogical, "wrap");
			}
			y += rowH;
		}

		// ---- filter ----
		{
			const std::vector<std::string> options {
				Wui::Tr("panel.texture.filter.point", "Point"),
				Wui::Tr("panel.texture.filter.bilinear", "Bilinear"),
				Wui::Tr("panel.texture.filter.trilinear", "Trilinear"),
			};
			int selected = static_cast<int>(m_Settings.Filter);
			drawLabel(Wui::Tr("panel.texture.filter", "Filter"),
				Wui::Tr("panel.texture.filter.tooltip",
					"Minification / magnification filter. Trilinear equals bilinear when mipmaps are off."),
				y);
			if (Wui::Combo(ctx, Wui::HashId("texture.field.filter"), rowRect(y),
					Wui::Tr("panel.texture.filter", "Filter"), options, selected, theme)
				&& selected >= 0 && selected <= static_cast<int>(TextureFilter::Trilinear))
			{
				m_Settings.Filter = static_cast<TextureFilter>(selected);
				m_Dirty = true;
				RefreshArtifactState();
				if (m_Ctx) m_Ctx->RecordOp("texture", "edit", m_AssetLogical, "filter");
			}
			y += rowH;
		}

		// ---- anisotropy ----
		{
			int value = static_cast<int>(m_Settings.Anisotropy);
			drawLabel(Wui::Tr("panel.texture.anisotropy", "Anisotropy"),
				Wui::Tr("panel.texture.anisotropy.tooltip",
					"1..16; the runtime clamps it to the device limit. Higher values keep slanted "
					"surfaces sharp."),
				y);
			if (Wui::StepperInt(ctx, Wui::HashId("texture.field.anisotropy"), rowRect(y), value, 1, 16,
					theme, Wui::WuiNumberStyle {}))
			{
				m_Settings.Anisotropy = static_cast<uint32_t>(value);
				m_Dirty = true;
				RefreshArtifactState();
				if (m_Ctx) m_Ctx->RecordOp("texture", "edit", m_AssetLogical, "anisotropy");
			}
			y += rowH;
		}

		// ---- premultiply_alpha ----
		{
			bool value = m_Settings.PremultiplyAlpha;
			drawLabel(Wui::Tr("panel.texture.premultiply_alpha", "Premultiply alpha"),
				Wui::Tr("panel.texture.premultiply_alpha.tooltip",
					"Store premultiplied alpha in the artifact (UI / decal style blending)."),
				y);
			const Wui::WuiRect row = rowRect(y);
			if (Wui::Checkbox(ctx, Wui::HashId("texture.field.premultiply_alpha"),
					{ row.X, row.Y, std::min(row.W, 120.0f), row.H },
					std::string(), value, theme))
			{
				m_Settings.PremultiplyAlpha = value;
				m_Dirty = true;
				RefreshArtifactState();
				if (m_Ctx) m_Ctx->RecordOp("texture", "edit", m_AssetLogical, "premultiply_alpha");
			}
			y += rowH;
		}

		// ---- flip_y ----
		{
			bool value = m_Settings.FlipY;
			drawLabel(Wui::Tr("panel.texture.flip_y", "Flip Y"),
				Wui::Tr("panel.texture.flip_y.tooltip",
					"Bake the image flipped vertically. Content textures default to no flip (UV origin "
					"top-left)."),
				y);
			const Wui::WuiRect row = rowRect(y);
			if (Wui::Checkbox(ctx, Wui::HashId("texture.field.flip_y"),
					{ row.X, row.Y, std::min(row.W, 120.0f), row.H }, std::string(), value, theme))
			{
				m_Settings.FlipY = value;
				m_Dirty = true;
				RefreshArtifactState();
				if (m_Ctx) m_Ctx->RecordOp("texture", "edit", m_AssetLogical, "flip_y");
			}
			y += rowH;
		}
	}

	void TextureSettingsPanel::DrawStatusLine(Wui::WuiContext& ctx, const Wui::WuiTheme& theme,
		const Wui::WuiRect& rect, float& y)
	{
		const float x = rect.X + theme.Pad;
		const float width = std::max(0.0f, rect.W - theme.Pad * 2.0f);
		const std::string summary = ArtifactSummary();
		Wui::Label(ctx, { x, y }, EllipsizeToWidth(ctx, summary, width, theme.FontSizeSmall),
			m_Artifact.State == Editor::TextureArtifactState::Fresh ? theme.Text : theme.Warning,
			theme.FontSizeSmall);
		y += theme.FontSizeSmall + 3.0f;

		if (m_Dirty)
		{
			Wui::Label(ctx, { x, y },
				EllipsizeToWidth(ctx,
					Wui::Tr("panel.texture.status.dirty",
						"Edited — Apply saves the .wtex and re-bakes the artifact."),
					width, theme.FontSizeSmall),
				theme.Accent, theme.FontSizeSmall);
			y += theme.FontSizeSmall + 3.0f;
		}
		if (m_DiskChanged)
		{
			Wui::Label(ctx, { x, y },
				EllipsizeToWidth(ctx,
					Wui::Tr("panel.texture.status.disk_changed",
						"The .wtex changed on disk — close and reopen this panel to reload it (your edits "
						"were kept)."),
					width, theme.FontSizeSmall),
				theme.Warning, theme.FontSizeSmall);
			y += theme.FontSizeSmall + 3.0f;
		}
		if (!m_Status.empty())
		{
			Wui::Label(ctx, { x, y },
				EllipsizeToWidth(ctx, m_Status, width, theme.FontSizeSmall),
				m_StatusIsError ? theme.Danger : theme.Success, theme.FontSizeSmall);
			y += theme.FontSizeSmall + 3.0f;
		}
	}

	void TextureSettingsPanel::DrawActions(Wui::WuiContext& ctx, const Wui::WuiTheme& theme,
		PanelHost& host, const Wui::WuiRect& rect)
	{
		(void)host;
		Wui::DrawPanelSurface(ctx, rect, theme);
		const float gap = theme.PadSmall;
		const float buttonH = rect.H - theme.Pad * 2.0f;
		const float buttonW = std::max(70.0f, (rect.W - theme.Pad * 2.0f - gap * 2.0f) / 3.0f);
		const float buttonY = rect.Y + theme.Pad;
		const Wui::WuiRect applyRect { rect.X + theme.Pad, buttonY, buttonW, buttonH };
		const Wui::WuiRect reimportRect { applyRect.X + buttonW + gap, buttonY, buttonW, buttonH };
		const Wui::WuiRect resetRect { reimportRect.X + buttonW + gap, buttonY,
			std::max(60.0f, rect.W - theme.Pad * 2.0f - (buttonW + gap) * 2.0f), buttonH };
		if (Wui::ButtonEx(ctx, Wui::HashId("texture.apply"), applyRect,
				Wui::Tr("panel.texture.apply", "Apply"), theme, true, true,
				Wui::Tr("panel.texture.apply.tooltip",
					"Save the .wtex asset and re-bake <source>.wtexc in the content root (the live artifact "
					"the running game reads), then flush the material texture cache.")))
			SaveAndBake(false);
		if (Wui::ButtonEx(ctx, Wui::HashId("texture.reimport"), reimportRect,
				Wui::Tr("panel.texture.reimport", "Reimport"), theme, true, false,
				Wui::Tr("panel.texture.reimport.tooltip",
					"Force a re-bake from the current settings (same as Apply, ignoring the up-to-date "
					"short-circuit).")))
			SaveAndBake(true);
		if (Wui::ButtonEx(ctx, Wui::HashId("texture.reset"), resetRect,
				Wui::Tr("panel.texture.reset", "Reset to Defaults"), theme, m_AssetFileExists, false,
				m_AssetFileExists
					? Wui::Tr("panel.texture.reset.tooltip",
						"Delete this .wtex asset: the source image goes back to the engine defaults "
						"(usage = color). The artifact is re-baked with those defaults.")
					: Wui::Tr("panel.texture.reset.tooltip.none",
						"Nothing to reset: this asset has no .wtex file (defaults are already in effect).")))
		{
			m_ResetConfirmOpen = true;
			ctx.SetModal(Wui::HashId("texture.reset.modal"));
			host.SetPanelModalOwner(Id());
			ctx.RecordOp("texture", "reset-ask", m_AssetLogical, "confirm delete .wtex");
		}
	}

	void TextureSettingsPanel::SaveAndBake(bool force)
	{
		if (m_AssetLogical.empty())
			return;
		EnsureContentRoot();
		m_Settings.Source = m_SourceBuffer;

		// 源图先按设置解析(显式 source: 或同目录同主名),否则没有东西可烘。
		std::string resolveError;
		std::string source;
		if (!Editor::ResolveTextureSourceLogical(m_ContentRoot, m_AssetLogical, m_Settings, source,
				resolveError))
		{
			m_Status = Wui::TrFormat("panel.texture.status.no_source_error",
				"Cannot bake: {detail}", { { "detail", resolveError } });
			m_StatusIsError = true;
			if (m_Ctx) m_Ctx->RecordOp("texture", "apply-failed", m_AssetLogical, resolveError);
			return;
		}
		m_SourceLogical = source;

		// 1) 保存资产(只有显式字段落盘;默认值不写 —— 空资产 = 全默认)。
		const std::string serialized = m_Settings.Serialize();
		std::string writeError;
		if (!WriteTextFileAtomic(std::filesystem::path(AssetAbsolutePath()), serialized, writeError))
		{
			m_Status = Wui::TrFormat("panel.texture.status.save_failed", "Cannot save the asset: {detail}",
				{ { "detail", writeError } });
			m_StatusIsError = true;
			if (m_Ctx) m_Ctx->RecordOp("texture", "apply-failed", m_AssetLogical, writeError);
			return;
		}
		m_AssetFileExists = true;
		m_Dirty = false;
		m_DiskChanged = false;
		if (const std::filesystem::path assetFile = AssetAbsolutePath(); !assetFile.empty())
		{
			std::error_code stampError;
			const std::filesystem::file_time_type stamp =
				std::filesystem::last_write_time(assetFile, stampError);
			m_AssetStampValid = !stampError;
			if (!stampError)
				m_AssetStamp = stamp;
			else
				m_AssetStampValid = false;
		}

		// 2) 就地重烘 `<源图>.wtexc`(内容根)+ 失效材质贴图缓存。
		std::string bakeError;
		if (!Editor::BakeTextureArtifactNow(m_ContentRoot, m_SourceLogical, m_Settings, bakeError))
		{
			m_Status = Wui::TrFormat("panel.texture.status.bake_failed", "Bake failed: {detail}",
				{ { "detail", bakeError } });
			m_StatusIsError = true;
			if (m_Ctx) m_Ctx->RecordOp("texture", "bake-failed", m_AssetLogical, bakeError);
			RefreshArtifactState(true);
			return;
		}
		m_Status = Wui::TrFormat("panel.texture.status.applied",
			"Saved {asset} and baked {artifact}", { { "asset", m_AssetLogical },
				{ "artifact", std::filesystem::path(ArtifactAbsolutePath()).filename().generic_string() } });
		m_StatusIsError = false;
		m_SourceStatsPath.clear();   // 重烘可能改了产物尺寸口径:下一帧重读源读数(缩略图不变)
		RefreshArtifactState(true);
		if (m_Ctx)
			m_Ctx->RecordOp("texture", force ? "reimport" : "apply", m_AssetLogical,
				TextureBlockFormatName(m_Artifact.Header.Format));
	}

	void TextureSettingsPanel::ResetAssetToDefaults()
	{
		if (m_AssetLogical.empty())
			return;
		EnsureContentRoot();
		const std::filesystem::path assetFile = AssetAbsolutePath();
		std::error_code removeError;
		const bool removed = std::filesystem::remove(assetFile, removeError);
		m_Settings = TextureImportSettings {};
		m_SourceBuffer.clear();
		m_AssetFileExists = false;
		m_AssetStampValid = false;
		m_Dirty = false;
		m_DiskChanged = false;
		if (!removed && removeError)
		{
			m_Status = Wui::TrFormat("panel.texture.status.reset_failed", "Cannot delete the asset: {detail}",
				{ { "detail", removeError.message() } });
			m_StatusIsError = true;
			return;
		}
		std::string resolveError;
		std::string source;
		if (!Editor::ResolveTextureSourceLogical(m_ContentRoot, m_AssetLogical, m_Settings, source,
				resolveError))
		{
			m_SourceLogical.clear();
			m_Status = Wui::TrFormat("panel.texture.status.reset_no_source",
				"Deleted the asset, but no source image: {detail}", { { "detail", resolveError } });
			m_StatusIsError = true;
			RefreshArtifactState(true);
			return;
		}
		m_SourceLogical = source;
		std::string bakeError;
		if (!Editor::BakeTextureArtifactNow(m_ContentRoot, m_SourceLogical, m_Settings, bakeError))
		{
			m_Status = Wui::TrFormat("panel.texture.status.bake_failed", "Bake failed: {detail}",
				{ { "detail", bakeError } });
			m_StatusIsError = true;
			RefreshArtifactState(true);
			return;
		}
		m_Status = Wui::TrFormat("panel.texture.status.reset",
			"Deleted {asset}: the source is back to the default settings and was re-baked.",
			{ { "asset", m_AssetLogical } });
		m_StatusIsError = false;
		RefreshArtifactState(true);
		if (m_Ctx) m_Ctx->RecordOp("texture", "reset", m_AssetLogical, "deleted .wtex + rebaked defaults");
	}

	void TextureSettingsPanel::DrawResetConfirmModal(Wui::WuiContext& ctx, const Wui::WuiTheme& theme,
		PanelHost& host)
	{
		const Wui::WuiId modalId = Wui::HashId("texture.reset.modal");
		Wui::ModalFrameDesc frameDesc;
		frameDesc.Id = modalId;
		frameDesc.Title = Wui::Tr("panel.texture.reset.title", "Reset to default settings?");
		frameDesc.Size = { 520.0f, 210.0f };
		Wui::WuiRect frame;
		bool escapePressed = false;
		if (!Wui::BeginModalFrame(ctx, frameDesc, &frame, &escapePressed, theme))
		{
			m_ResetConfirmOpen = false;
			host.SetPanelModalOwner(std::string());
			return;
		}
		const float x = frame.X + 16.0f;
		const float width = frame.W - 32.0f;
		Wui::Label(ctx, { x, frame.Y + 52.0f },
			EllipsizeToWidth(ctx,
				Wui::TrFormat("panel.texture.reset.body", "Delete {asset}?",
					{ { "asset", m_AssetLogical } }),
				width, 14.0f),
			theme.Text, 14.0f);
		Wui::Label(ctx, { x, frame.Y + 78.0f },
			EllipsizeToWidth(ctx,
				Wui::Tr("panel.texture.reset.body2",
					"The source image keeps working with the engine defaults (usage = color, BC7); the "
					"artifact is re-baked and the material texture cache is flushed."),
				width, 12.0f),
			theme.TextMuted, 12.0f);
		const Wui::ModalResult result = Wui::ModalFooter(ctx, frame,
			Wui::Tr("panel.texture.reset.confirm", "Delete .wtex"),
			Wui::Tr("panel.texture.reset.cancel", "Cancel"),
			Wui::HashId("texture.reset.ok"), Wui::HashId("texture.reset.cancel"), true, theme);
		const bool confirmed = result == Wui::ModalResult::Confirm;
		const bool cancelled = result == Wui::ModalResult::Cancel || escapePressed;
		if (confirmed || cancelled)
			m_ResetConfirmOpen = false;
		Wui::EndModalFrame(ctx);
		if (confirmed)
		{
			if (ctx.Modal() == modalId)
				ctx.ClearModal();
			host.SetPanelModalOwner(std::string());
			ResetAssetToDefaults();
		}
		else if (cancelled)
		{
			if (ctx.Modal() == modalId)
				ctx.ClearModal();
			host.SetPanelModalOwner(std::string());
		}
	}
}
