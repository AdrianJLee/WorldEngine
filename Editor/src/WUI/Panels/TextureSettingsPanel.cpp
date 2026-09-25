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
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <shellapi.h>
#include <vector>

#pragma comment(lib, "shell32.lib")

namespace World
{
	namespace
	{
		// 源图扩展名(与 TextureCompiler::BakeDirectory 的候选顺序一致)。
		const char* const kSourceCandidates[] = { ".png", ".jpg", ".jpeg", ".tga", ".bmp" };

		// M4-TEX P5 预览口径:
		//   * 源图**解码一次**;长边超过 kPreviewSourceMaxEdge 时当场落一次(之后每次改设置都从这份
		//     像素重采样,不再回磁盘/解码器);
		//   * 草稿纹理再压到 kDraftPreviewMaxEdge(即时路径必须便宜;状态行标注 "capped",
		//     真产物烘完就换成全分辨率的产物纹理);
		//   * 需要真编码的改动(compression / mips / usage / 尺寸…)走 kPreviewBakeDebounceSeconds 防抖,
		//     在**工作线程**里跑 `TextureCompiler::BakeFile`。
		constexpr uint32_t kPreviewSourceMaxEdge = 2048;
		constexpr uint32_t kDraftPreviewMaxEdge = 1024;
		constexpr double kPreviewBakeDebounceSeconds = 0.35;
		constexpr float kMaxZoomFactor = 32.0f;

		double WallClockSeconds()
		{
			return std::chrono::duration<double>(
				std::chrono::steady_clock::now().time_since_epoch()).count();
		}

		std::string TrimAscii(const std::string& text)
		{
			const auto notSpace = [](unsigned char character) { return std::isspace(character) == 0; };
			std::string trimmed = text;
			trimmed.erase(trimmed.begin(), std::find_if(trimmed.begin(), trimmed.end(), notSpace));
			trimmed.erase(std::find_if(trimmed.rbegin(), trimmed.rend(), notSpace).base(), trimmed.end());
			return trimmed;
		}

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

		// 在资源管理器中定位一个文件(与内容浏览器 OpenInExplorer 同一口径:/select 选中该项,
		// 而不是打开文件)。项目里没有共享 helper(ContentBrowserPanel 的那份是私有成员),
		// 这里保留同一套 6 行调用,行为逐字一致。
		void RevealPathInExplorer(const std::filesystem::path& path)
		{
			if (path.empty())
				return;
			const std::wstring parameters = L"/select,\"" + std::filesystem::absolute(path).wstring() + L"\"";
			const HINSTANCE result = ShellExecuteW(nullptr, L"open", L"explorer.exe", parameters.c_str(),
				nullptr, SW_SHOWNORMAL);
			if (reinterpret_cast<intptr_t>(result) <= 32)
				WLD_CORE_WARN("[texture] cannot reveal '{0}' in Explorer (error {1})", path.string(),
					reinterpret_cast<intptr_t>(result));
		}

		// ---- 预览图像处理:与 TextureCompiler 的缩放口径一致(线性空间平均),压缩交给 GPU ----

		inline uint8_t ToLinearByte(uint8_t value)
		{
			const float linear = std::pow(static_cast<float>(value) / 255.0f, 2.2f);
			return static_cast<uint8_t>(std::lround(std::clamp(linear, 0.0f, 1.0f) * 255.0f));
		}

		inline uint8_t ToSrgbByte(uint8_t value)
		{
			const float srgb = std::pow(static_cast<float>(value) / 255.0f, 1.0f / 2.2f);
			return static_cast<uint8_t>(std::lround(std::clamp(srgb, 0.0f, 1.0f) * 255.0f));
		}

		// 盒式降采样(2x2 → 1);srgb=true 时先转线性再平均(避免缩小后整体变暗)。
		// 与 TextureCompiler.cpp 的 DownsampleBox 逐字段同口径。
		void DownsampleBox2x(const std::vector<uint8_t>& source, uint32_t sourceWidth, uint32_t sourceHeight,
			bool srgb, std::vector<uint8_t>& out, uint32_t& outWidth, uint32_t& outHeight)
		{
			outWidth = std::max(1u, sourceWidth / 2);
			outHeight = std::max(1u, sourceHeight / 2);
			out.assign(static_cast<size_t>(outWidth) * outHeight * 4u, 0);
			for (uint32_t y = 0; y < outHeight; ++y)
			{
				for (uint32_t x = 0; x < outWidth; ++x)
				{
					uint32_t accumulators[4] = { 0, 0, 0, 0 };
					uint32_t samples = 0;
					for (uint32_t dy = 0; dy < 2; ++dy)
					{
						const uint32_t sourceY = std::min(sourceHeight - 1, y * 2 + dy);
						for (uint32_t dx = 0; dx < 2; ++dx)
						{
							const uint32_t sourceX = std::min(sourceWidth - 1, x * 2 + dx);
							const uint8_t* texel = source.data()
								+ (static_cast<size_t>(sourceY) * sourceWidth + sourceX) * 4u;
							for (int channel = 0; channel < 4; ++channel)
							{
								const uint8_t value = srgb && channel < 3 ? ToLinearByte(texel[channel])
									: texel[channel];
								accumulators[channel] += value;
							}
							++samples;
						}
					}
					uint8_t* target = out.data() + (static_cast<size_t>(y) * outWidth + x) * 4u;
					for (int channel = 0; channel < 4; ++channel)
					{
						const uint8_t averaged = static_cast<uint8_t>(
							(accumulators[channel] + samples / 2) / samples);
						target[channel] = srgb && channel < 3 ? ToSrgbByte(averaged) : averaged;
					}
				}
			}
		}

		// 等比缩放到"最长边 = maxEdge"(maxEdge = 0 或未超限时原样返回)。逐级 2x 盒式 +
		// 最后一步最近邻补齐 —— 与 TextureCompiler::ResizeToMaxSize 同一口径(预览与产物尺寸一致)。
		std::vector<uint8_t> ResampleToMaxEdge(const std::vector<uint8_t>& source, uint32_t sourceWidth,
			uint32_t sourceHeight, uint32_t maxEdge, bool srgb, uint32_t& outWidth, uint32_t& outHeight)
		{
			outWidth = sourceWidth;
			outHeight = sourceHeight;
			if (maxEdge == 0 || std::max(sourceWidth, sourceHeight) <= maxEdge
				|| sourceWidth == 0 || sourceHeight == 0)
				return source;
			const double scale = static_cast<double>(maxEdge)
				/ static_cast<double>(std::max(sourceWidth, sourceHeight));
			const uint32_t targetWidth = std::max(1u, static_cast<uint32_t>(std::floor(sourceWidth * scale)));
			const uint32_t targetHeight = std::max(1u, static_cast<uint32_t>(std::floor(sourceHeight * scale)));

			std::vector<uint8_t> current = source;
			uint32_t width = sourceWidth;
			uint32_t height = sourceHeight;
			while (width / 2 >= targetWidth && height / 2 >= targetHeight && (width > 1 || height > 1))
			{
				std::vector<uint8_t> next;
				uint32_t nextWidth = 0;
				uint32_t nextHeight = 0;
				DownsampleBox2x(current, width, height, srgb, next, nextWidth, nextHeight);
				current.swap(next);
				width = nextWidth;
				height = nextHeight;
			}
			if (width != targetWidth || height != targetHeight)
			{
				std::vector<uint8_t> resized(static_cast<size_t>(targetWidth) * targetHeight * 4u, 0);
				for (uint32_t y = 0; y < targetHeight; ++y)
				{
					const uint32_t sourceY = std::min(height - 1,
						static_cast<uint32_t>(static_cast<uint64_t>(y) * height / targetHeight));
					for (uint32_t x = 0; x < targetWidth; ++x)
					{
						const uint32_t sourceX = std::min(width - 1,
							static_cast<uint32_t>(static_cast<uint64_t>(x) * width / targetWidth));
						std::memcpy(resized.data() + (static_cast<size_t>(y) * targetWidth + x) * 4u,
							current.data() + (static_cast<size_t>(sourceY) * width + sourceX) * 4u, 4u);
					}
				}
				current.swap(resized);
				width = targetWidth;
				height = targetHeight;
			}
			outWidth = width;
			outHeight = height;
			return current;
		}

		void FlipRowsInPlace(std::vector<uint8_t>& pixels, uint32_t width, uint32_t height)
		{
			const size_t rowBytes = static_cast<size_t>(width) * 4u;
			if (rowBytes == 0 || height < 2 || pixels.size() < rowBytes * height)
				return;
			std::vector<uint8_t> scratch(rowBytes);
			for (uint32_t row = 0; row < height / 2; ++row)
			{
				uint8_t* top = pixels.data() + static_cast<size_t>(row) * rowBytes;
				uint8_t* bottom = pixels.data() + static_cast<size_t>(height - 1u - row) * rowBytes;
				std::memcpy(scratch.data(), top, rowBytes);
				std::memcpy(top, bottom, rowBytes);
				std::memcpy(bottom, scratch.data(), rowBytes);
			}
		}

		void PremultiplyAlphaInPlace(std::vector<uint8_t>& pixels)
		{
			for (size_t offset = 0; offset + 3 < pixels.size(); offset += 4)
			{
				const uint32_t alpha = pixels[offset + 3];
				for (int channel = 0; channel < 3; ++channel)
					pixels[offset + static_cast<size_t>(channel)] = static_cast<uint8_t>(
						(pixels[offset + static_cast<size_t>(channel)] * alpha + 127) / 255);
			}
		}

		// 通道开关:channel 0 = RGB(原样);1..4 = R/G/B/A 单通道 → 灰度(不透明)。
		void ApplyChannelMask(std::vector<uint8_t>& pixels, int channel)
		{
			if (channel <= 0)
				return;
			const size_t index = static_cast<size_t>(channel - 1);
			for (size_t offset = 0; offset + 3 < pixels.size(); offset += 4)
			{
				const uint8_t value = pixels[offset + index];
				pixels[offset + 0] = value;
				pixels[offset + 1] = value;
				pixels[offset + 2] = value;
				pixels[offset + 3] = 255;
			}
		}

		// 产物块格式 → 预览用的 RHI 格式。**一律 UNORM**:WUI 与交换链都是非 sRGB 通道,
		// 预览要显示"文件里存的字节"(sRGB 是采样期解码,不改变像素内容);用 _SRGB 变体会让
		// 整个预览偏暗,也与图标/材质缩略图的显示口径不一致。压缩块本身与 sRGB 变体完全相同。
		Rhi::Format PreviewDisplayFormat(TextureBlockFormat format)
		{
			switch (format)
			{
				case TextureBlockFormat::Rgba8: return Rhi::Format::R8G8B8A8_UNORM;
				case TextureBlockFormat::Bc7: return Rhi::Format::BC7_UNORM;
				case TextureBlockFormat::Bc5: return Rhi::Format::BC5_UNORM;
				case TextureBlockFormat::Bc4: return Rhi::Format::BC4_UNORM;
				case TextureBlockFormat::Bc1: return Rhi::Format::BC1_UNORM;
				case TextureBlockFormat::Bc3: return Rhi::Format::BC3_UNORM;
				default: return Rhi::Format::Undefined;   // Rgba16f 等暂无上传路径
			}
		}

		const char* PreviewChannelCode(int channel)
		{
			switch (channel)
			{
				case 1: return "r";
				case 2: return "g";
				case 3: return "b";
				case 4: return "a";
				default: return "rgb";
			}
		}

		const char* PreviewStateCode(int state)
		{
			switch (state)
			{
				case 1: return "pending";
				case 2: return "encoding";
				case 3: return "baked";
				case 4: return "failed";
				default: return "idle";
			}
		}

		// 产物落点(与内核 `TextureCompiler::BakeDirectory` / `TextureData` 的查找口径一致):
		// 契约名 = `<同目录>/<源图主名>.wtexc`(2026-09-25 起产物名里不再带源图扩展名,
		// 见 tools/agents/dispatch/we_editor.md 的"命名口径")。
		std::filesystem::path ArtifactPathForSource(const std::filesystem::path& sourceFile)
		{
			return sourceFile.parent_path() / (sourceFile.stem().string() + ".wtexc");
		}

		// 容错候选:旧命名 `<源图全名>.wtexc`(内核 lookup 的第二候选)。编辑器只在契约名
		// 缺失时读它;`.wtexc` 不是资产,内容浏览器里一律不显示。
		std::filesystem::path LegacyArtifactPathForSource(const std::filesystem::path& sourceFile)
		{
			return std::filesystem::path(sourceFile.string() + ".wtexc");
		}

		// 预览缩放的统一口径:`Factor` 是"相对 fit"的倍率(跨草稿/产物换尺寸时取景不跳)。
		// 绝对范围 = [min(fit, 1), max(fit, 32)] —— 小图能退到 1:1(而不是被拉满到 fit),
		// 大图不会比 fit 更小(fit 本来就把整张装进预览)。
		struct PreviewZoomRange
		{
			float Fit = 1.0f;     // fit 的绝对倍率(屏幕像素 / 纹素)
			float MinFactor = 1.0f;
			float MaxFactor = 1.0f;
		};

		PreviewZoomRange PreviewZoomFor(const Wui::WuiRect& rect, uint32_t width, uint32_t height)
		{
			PreviewZoomRange range;
			if (width == 0 || height == 0 || rect.W <= 1.0f || rect.H <= 1.0f)
				return range;
			const float textureW = static_cast<float>(width);
			const float textureH = static_cast<float>(height);
			range.Fit = std::min(rect.W / textureW, rect.H / textureH);
			const float minZoom = std::min(range.Fit, 1.0f);
			const float maxZoom = std::max(range.Fit, kMaxZoomFactor);
			range.MinFactor = range.Fit > 0.0f ? minZoom / range.Fit : 1.0f;
			range.MaxFactor = range.Fit > 0.0f ? maxZoom / range.Fit : 1.0f;
			return range;
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

		// `.wtex` 资产的逻辑路径规范化:传进来的可能是源图(按同主名换算成资产)。空 = 不是纹理路径。
		std::string AssetLogicalForTexturePath(const std::string& logicalPath)
		{
			if (IsTextureAssetPath(logicalPath))
				return logicalPath;
			if (TextureCompiler::IsTextureSourceExtension(LowerExtension(logicalPath)))
				return TextureAssetPathForSource(logicalPath);
			return {};
		}

		// `source:` 文本 → 逻辑路径(与烘焙器同一口径:内容根相对、不许绝对路径 / '..'、扩展名受支持)。
		bool ResolveDeclaredSource(const std::string& declaredText, std::string& outLogical,
			std::string& outError)
		{
			outLogical.clear();
			outError.clear();
			const std::filesystem::path declared(declaredText);
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
				outError = "source '" + declaredText + "' is not a supported image extension";
				return false;
			}
			outLogical = declared.generic_string();
			return true;
		}

		// M4-TEX P9:`.wtex` 一律按**资产文件**读(`LoadTextureAssetFile` 自动区分容器 / 旧式)。
		// 旧口径(整份文件当 YAML 解析)会把容器里的二进制 payload 当 YAML 读 → 报错 →
		// 校验区误报 "no source image"(实测 `textures/quadrants.wtex`)。
		TextureAssetDocument LoadTextureAssetDocument(const std::filesystem::path& contentRoot,
			const std::string& logicalPath)
		{
			TextureAssetDocument document;
			const std::string assetLogical = AssetLogicalForTexturePath(logicalPath);
			if (assetLogical.empty())
			{
				document.Valid = false;
				document.Error = "'" + logicalPath + "' is not a .wtex asset (or its source image)";
				return document;
			}
			const std::filesystem::path assetFile = contentRoot / assetLogical;
			std::error_code existsError;
			document.Exists = std::filesystem::is_regular_file(assetFile, existsError);
			if (!document.Exists)
				return document;   // 缺 `.wtex` = 全默认(与内核 LoadTextureImportSettings 同一口径)
			TextureAssetFile file;
			std::string error;
			if (!LoadTextureAssetFile(assetFile, file, error))
			{
				document.Valid = false;
				document.Error = error;
				return document;
			}
			document.Settings = file.Settings;
			document.Payload = file.Payload;
			document.Container = !file.Payload.empty();
			document.LegacySource = file.LegacySource;
			return document;
		}

		bool ResolveTextureSource(const std::filesystem::path& contentRoot, const std::string& logicalPath,
			const TextureImportSettings& settings, TextureSourceResolution& out)
		{
			out = TextureSourceResolution {};
			if (logicalPath.empty())
			{
				out.Error = "no texture path";
				return false;
			}
			// ① 传进来的就是源图(内容浏览器里的 png/jpg/…):它自己就是字节来源。
			if (!IsTextureAssetPath(logicalPath))
			{
				if (!TextureCompiler::IsTextureSourceExtension(LowerExtension(logicalPath)))
				{
					out.Error = "'" + logicalPath + "' is not a texture asset or a supported image";
					return false;
				}
				out.BytesLogical = logicalPath;
				out.ImportSourceLogical = logicalPath;
				return true;
			}
			// ② `.wtex`:容器以**内嵌 payload** 为准 —— 外部源图只是导入源(可缺、可删)。
			const TextureAssetDocument assetLogicalDoc = LoadTextureAssetDocument(contentRoot, logicalPath);
			out.AssetExists = assetLogicalDoc.Exists;
			out.Container = assetLogicalDoc.Container;
			if (!assetLogicalDoc.Valid)
			{
				out.Error = assetLogicalDoc.Error;
				return false;
			}
			// 盘上的外部导入源(可选):显式 `source:` 优先,否则同目录同主名图片。
			if (!settings.Source.empty())
			{
				std::string declared;
				std::string declaredError;
				if (ResolveDeclaredSource(settings.Source, declared, declaredError))
					out.ImportSourceLogical = declared;
				else if (!assetLogicalDoc.Container)
				{
					out.Error = declaredError;
					return false;
				}
			}
			else
			{
				const std::filesystem::path asset(logicalPath);
				for (const char* extension : kSourceCandidates)
				{
					const std::filesystem::path candidate =
						asset.parent_path() / (asset.stem().string() + extension);
					std::error_code existsError;
					if (std::filesystem::is_regular_file(contentRoot / candidate, existsError))
					{
						out.ImportSourceLogical = candidate.generic_string();
						break;
					}
				}
			}
			if (!out.ImportSourceLogical.empty())
			{
				std::error_code existsError;
				if (!std::filesystem::is_regular_file(contentRoot / out.ImportSourceLogical, existsError))
					out.ImportSourceLogical.clear();
			}
			if (assetLogicalDoc.Container)
			{
				out.Embedded = true;
				out.BytesLogical = logicalPath;   // 字节在资产自身里(烘焙/预览读 payload)
				return true;
			}
			// ③ 旧式设置文件(没有 `---payload`):字节必须来自外部源图,否则才是真的 missing-source。
			if (out.ImportSourceLogical.empty())
			{
				out.Error = "no source image (old-style settings file: re-import it as a single file)";
				return false;
			}
			out.BytesLogical = out.ImportSourceLogical;
			return true;
		}

		// 兼容入口:调用方只关心"字节从哪来"时给逻辑路径(容器 = 资产自身)。
		bool ResolveTextureSourceLogical(const std::filesystem::path& contentRoot,
			const std::string& logicalPath, const TextureImportSettings& settings,
			std::string& outSourceLogical, std::string& outError)
		{
			TextureSourceResolution resolution;
			if (!ResolveTextureSource(contentRoot, logicalPath, settings, resolution))
			{
				outSourceLogical.clear();
				outError = resolution.Error;
				return false;
			}
			outSourceLogical = resolution.BytesLogical;
			outError.clear();
			return true;
		}

		bool ValidateTextureSourceText(const std::filesystem::path& contentRoot, const std::string& text,
			std::string& outError)
		{
			outError.clear();
			const std::string trimmed = TrimAscii(text);
			if (trimmed.empty())
				return true;   // 缺省 = 同目录同主名图片
			const std::filesystem::path declared(trimmed);
			bool escapes = declared.is_absolute() || declared.has_root_name();
			for (const std::filesystem::path& part : declared)
				if (part == "..")
					escapes = true;
			if (escapes)
			{
				outError = Wui::Tr("panel.texture.source.error.relative",
					"must be a content-root relative path without '..'");
				return false;
			}
			if (!TextureCompiler::IsTextureSourceExtension(LowerExtension(declared)))
			{
				outError = Wui::Tr("panel.texture.source.error.extension",
					"must be an image (.png / .jpg / .jpeg / .tga / .bmp)");
				return false;
			}
			std::error_code existsError;
			if (!std::filesystem::is_regular_file(contentRoot / declared, existsError))
			{
				outError = Wui::TrFormat("panel.texture.source.error.missing",
					"source image not found: {path}", { { "path", declared.generic_string() } });
				return false;
			}
			return true;
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
			// M4-TEX P9:`.wtex` 一律按资产文件读(容器 = 内嵌 payload;旧式 = 设置 + 外部源图)。
			// 旧口径把容器整份文件当 YAML 解析,会读失败 → 面板/浏览器误报 "no source image"。
			TextureAssetDocument document;
			bool container = false;
			if (IsTextureAssetPath(sourceLogical))
			{
				document = LoadTextureAssetDocument(contentRoot, sourceLogical);
				if (!document.Valid)
				{
					status.State = TextureArtifactState::Invalid;
					status.Detail = document.Error;
					return status;
				}
				container = document.Container;
			}
			else
			{
				// 源图引用:同主名 `.wtex` 在场 = 这张图归资产管 → 按**资产**判定(容器 = 内嵌字节);
				// 不在场 = 按图本身 + 默认设置判定(与内核"缺 sidecar = 默认设置"同一口径)。
				const std::string siblingAsset = TextureAssetPathForSource(sourceLogical);
				std::error_code siblingError;
				if (std::filesystem::is_regular_file(contentRoot / siblingAsset, siblingError))
					return InspectTextureArtifact(contentRoot, siblingAsset, settingsOverride);
			}
			if (settingsOverride != nullptr)
				settings = *settingsOverride;
			else if (container)
				settings = document.Settings;

			const std::filesystem::path artifactFile = ArtifactPathForSource(sourceFile);
			std::vector<uint8_t> artifactBytes;
			std::string readError;
			if (!ReadFileBytes(artifactFile, artifactBytes, readError)
				&& !ReadFileBytes(LegacyArtifactPathForSource(sourceFile), artifactBytes, readError))
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

			// 源字节口径与烘焙器一致:容器 = 内嵌 payload;源图/旧式 = 文件本身。
			std::vector<uint8_t> sourceBytes;
			if (container)
				sourceBytes = document.Payload;
			else if (!ReadFileBytes(sourceFile, sourceBytes, readError))
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
			// M4-TEX P9:导入 = 写**单文件容器**(YAML 头默认设置 + 该图片的原始字节)。
			// 旧口径(只有 `usage: color` 的设置文件 + `source:`)已废弃:一张纹理 = 一个文件。
			std::vector<uint8_t> payload;
			std::string readError;
			if (!ReadFileBytes(sourceFile, payload, readError) || payload.empty())
			{
				outError = readError.empty() ? ("empty source image: " + sourceLogical) : readError;
				return false;
			}
			TextureAssetFile container;
			container.Settings = TextureImportSettings {};
			container.Payload = std::move(payload);
			std::string saveError;
			if (!SaveTextureAssetFile(assetFile, container, saveError))
			{
				outError = saveError;
				return false;
			}
			if (outAssetLogical)
				*outAssetLogical = assetLogical;
			WLD_CORE_INFO("[texture] created single-file texture asset {0} for {1} ({2} bytes embedded)",
				assetLogical, sourceLogical, container.Payload.size());
			return true;
		}

		bool EnsureTextureAssetForSource(const std::filesystem::path& contentRoot,
			const std::string& sourceLogical, std::string* outAssetLogical, bool* outCreated,
			std::string& outError)
		{
			outError.clear();
			const std::string assetLogical = TextureAssetPathForSource(sourceLogical);
			if (assetLogical.empty() || !IsTextureAssetPath(assetLogical))
			{
				outError = "cannot derive a .wtex path from '" + sourceLogical + "'";
				return false;
			}
			if (outAssetLogical)
				*outAssetLogical = assetLogical;
			std::error_code existsError;
			if (std::filesystem::is_regular_file(contentRoot / assetLogical, existsError))
			{
				if (outCreated)
					*outCreated = false;
				return true;
			}
			std::string asset;
			if (!CreateTextureAssetForSource(contentRoot, sourceLogical, &asset, outError))
				return false;
			if (outAssetLogical)
				*outAssetLogical = asset;
			if (outCreated)
				*outCreated = true;
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
			bool baked = false;
			// M4-TEX P9:容器(`.wtex` + 内嵌 payload)按**内嵌字节**烘(与 cook 同一口径:
			// 产物头的 sourceSha256 = sha256(payload)),不读外部源图。
			if (IsTextureAssetPath(sourceLogical))
			{
				const TextureAssetDocument document = LoadTextureAssetDocument(contentRoot, sourceLogical);
				if (!document.Valid)
				{
					outError = document.Error;
					return false;
				}
				if (!document.Container)
				{
					outError = "old-style settings file has no embedded source bytes "
						"(re-import it as a single file: double-click its source image in the Content Browser)";
					return false;
				}
				baked = TextureCompiler::BakeBytes(document.Payload, settings, artifactBytes, header,
					bakeError);
			}
			else
				baked = TextureCompiler::BakeFile(sourceFile, settings, artifactBytes, header, bakeError);
			if (!baked)
			{
				outError = bakeError;
				return false;
			}
			const std::filesystem::path artifactFile = ArtifactPathForSource(sourceFile);
			std::string writeError;
			if (!WriteFileBytesAtomic(artifactFile, artifactBytes, writeError))
			{
				outError = writeError;
				return false;
			}
			// 旧命名(带源图扩展名)的残留副本:同一份源只保留一份产物 —— 契约名写成功之后
			// 把它删掉(它是可重建的产物,不是用户数据)。运行时的 lookup 会在契约名缺失时
			// 才回退到旧名,所以删除不会让运行时"突然找不到"。
			const std::filesystem::path legacyArtifact = LegacyArtifactPathForSource(sourceFile);
			std::error_code legacyError;
			if (std::filesystem::is_regular_file(legacyArtifact, legacyError)
				&& std::filesystem::remove(legacyArtifact, legacyError))
				WLD_CORE_INFO("[texture] removed legacy artifact copy: {0}",
					legacyArtifact.filename().generic_string());
			// 材质贴图缓存按**逻辑路径**失效:下一次 Get 重新读盘(命中新产物)。
			MaterialTextureCache::Get().Invalidate(sourceLogical);
			// 同一份纹理可能被"资产引用"与"源图引用"两种写法引用:两条缓冲键都失效。
			const std::string assetLogical = TextureAssetPathForSource(sourceLogical);
			if (assetLogical != sourceLogical)
				MaterialTextureCache::Get().Invalidate(assetLogical);
			else
			{
				const std::filesystem::path asset(sourceLogical);
				for (const char* extension : kSourceCandidates)
				{
					const std::string sibling =
						(asset.parent_path() / (asset.stem().string() + extension)).generic_string();
					std::error_code siblingError;
					if (std::filesystem::is_regular_file(contentRoot / sibling, siblingError))
						MaterialTextureCache::Get().Invalidate(sibling);
				}
			}
			WLD_CORE_INFO("[texture] baked {0} -> {1} ({2}, {3}x{4}, {5} mips)", sourceLogical,
				artifactFile.filename().generic_string(), TextureBlockFormatName(header.Format),
				header.Width, header.Height, header.MipCount);
			return true;
		}
	}

	TextureSettingsPanel::TextureSettingsPanel()
	{
		m_ContentRoot = std::filesystem::path(std::string(WLD_ASSETPATH));
		// 需要真编码的预览烘培在**工作线程**里跑(与 MaterialEditorPanel 的着色器编译线程同一套
		// 请求/结果/停止口径):UI 帧只做派发与上传,BC7 4K 也不会卡住编辑器。
		m_BakeThread = std::thread([this] { PreviewBakeWorkerLoop(); });
	}

	TextureSettingsPanel::~TextureSettingsPanel()
	{
		// 先收工作线程(它只写自己的结果槽,析构前必须确认它已退出)。
		{
			std::lock_guard<std::mutex> lock(m_BakeMutex);
			m_BakeThreadStop = true;
		}
		m_BakeCv.notify_all();
		if (m_BakeThread.joinable())
			m_BakeThread.join();
		InvalidatePreviewTextures();
	}

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
		return source.empty() ? std::string()
			: ArtifactPathForSource(std::filesystem::path(source)).string();
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
			m_DiskChanged = false;
			m_Scroll = 0.0f;
			m_SourceError.clear();
			m_Payload.clear();
			m_Container = false;
			m_LegacyAsset = false;
			m_ImportSourceLogical.clear();
			m_AssetFormNote.clear();
			m_LoadedSourceText.clear();
			m_Channel = PreviewChannel::Rgb;
			m_ZoomFactor = 1.0f;
			m_CenterU = 0.5f;
			m_CenterV = 0.5f;
			m_PanActive = false;
			m_PreviewSourceLogical.clear();
			m_PreviewSourcePixels.clear();
			m_PreviewSourceWidth = 0;
			m_PreviewSourceHeight = 0;
			m_PreviewSourceValid = false;
			m_PreviewSourceStampValid = false;
			m_PreviewSourceEmbedded = false;
			m_PreviewRevision = 0;
			m_BakedRevision = 0;
			m_BakeDueSeconds = 0.0;
			m_PreviewState = PreviewState::Idle;
			m_PreviewDetail.clear();
			m_LoadDiskArtifactRequested = true;
			m_DraftDirty = true;
			InvalidatePreviewTextures();
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
		// M4-TEX P9:`.wtex` 一律按**资产文件**读(容器 / 旧式自动区分;旧口径会把内嵌 payload
		// 当 YAML 解析而报错 —— 那正是校验区误报 "no source image" 的来源)。
		const Editor::TextureAssetDocument document =
			Editor::LoadTextureAssetDocument(m_ContentRoot, m_AssetLogical);
		if (!document.Valid)
		{
			m_LoadError = document.Error;
			m_Loaded = false;
			m_SourceLogical.clear();
			m_SourceBuffer.clear();
			m_Payload.clear();
			m_Container = false;
			m_LegacyAsset = false;
			m_ImportSourceLogical.clear();
			m_AssetFormNote.clear();
			m_Artifact = Editor::TextureArtifactStatus {};
			m_Artifact.State = Editor::TextureArtifactState::Invalid;
			m_Artifact.Detail = document.Error;
			return;
		}
		m_Settings = document.Settings;
		m_Payload = document.Payload;
		m_Container = document.Container;
		m_LegacyAsset = document.Exists && !document.Container;
		m_Loaded = true;
		m_Dirty = false;
		m_SourceBuffer = m_Settings.Source;
		m_LoadedSourceText = TrimAscii(m_SourceBuffer);
		m_SourceError.clear();
		if (!m_Settings.Source.empty())
			Editor::ValidateTextureSourceText(m_ContentRoot, m_Settings.Source, m_SourceError);
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

		Editor::TextureSourceResolution resolution;
		m_LoadDiskArtifactRequested = true;
		// 盘上读出来的设置 = 新的预览版本(外部改写 `.wtex` 之后,产物预览要重新判定)。
		TouchPreviewRevision();
		if (!Editor::ResolveTextureSource(m_ContentRoot, m_AssetLogical, m_Settings, resolution))
		{
			m_ImportSourceLogical.clear();
			m_SourceLogical.clear();
			m_Artifact = Editor::TextureArtifactStatus {};
			m_Artifact.State = Editor::TextureArtifactState::NoSource;
			m_Artifact.Detail = resolution.Error;
			m_AssetFormNote = m_LegacyAsset
				? Wui::Tr("panel.texture.form.legacy_broken",
					"Old-style settings file: no embedded source bytes and no source image — re-import "
					"the image to get a single-file asset.")
				: std::string();
			return;
		}
		m_SourceLogical = resolution.BytesLogical;
		m_ImportSourceLogical = resolution.ImportSourceLogical;
		m_AssetFormNote = resolution.Embedded
			? Wui::TrFormat("panel.texture.form.container",
				"Single-file asset: {bytes} bytes embedded; the import source is optional.",
				{ { "bytes", std::to_string(m_Payload.size()) } })
			: (m_LegacyAsset
				? Wui::Tr("panel.texture.form.legacy",
					"Old-style settings file (no embedded bytes). Press Apply to re-import it as a "
					"single-file asset.")
				: std::string());
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
		std::error_code stampError;
		const std::filesystem::file_time_type sourceStamp =
			std::filesystem::last_write_time(sourceFile, stampError);
		const bool assetExists = std::filesystem::is_regular_file(assetFile, stampError);
		const std::filesystem::file_time_type assetStamp = assetExists
			? std::filesystem::last_write_time(assetFile, stampError)
			: std::filesystem::file_time_type {};
		// 产物指纹:契约名优先,缺失时看旧命名 —— 两个都算"产物在场/变了"。
		std::filesystem::path artifactStampPath = ArtifactPathForSource(sourceFile);
		if (!std::filesystem::is_regular_file(artifactStampPath, stampError))
			artifactStampPath = LegacyArtifactPathForSource(sourceFile);
		const bool artifactExists = std::filesystem::is_regular_file(artifactStampPath, stampError);
		const std::filesystem::file_time_type artifactStamp = artifactExists
			? std::filesystem::last_write_time(artifactStampPath, stampError)
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

	void TextureSettingsPanel::TouchPreviewRevision()
	{
		++m_PreviewRevision;
		m_BakeDueSeconds = 0.0;   // 下一帧按新的时间戳重排防抖
		m_DraftDirty = true;
	}

	void TextureSettingsPanel::MarkSettingsDirty(const char* field)
	{
		m_Dirty = true;
		RefreshArtifactState();
		TouchPreviewRevision();
		if (m_Ctx)
			m_Ctx->RecordOp("texture", "edit", m_AssetLogical, field ? field : "");
	}

	bool TextureSettingsPanel::CurrentSourceBytes(std::vector<uint8_t>& outBytes,
		std::string& outAbsoluteSource, bool& outEmbedded) const
	{
		outBytes.clear();
		outAbsoluteSource.clear();
		outEmbedded = false;
		// M4-TEX P9:容器以**内嵌 payload**为准(源图只是导入源,可缺、可删)。
		if (m_Container && !m_Payload.empty())
		{
			outBytes = m_Payload;
			outEmbedded = true;
			return true;
		}
		if (m_SourceLogical.empty())
			return false;
		outAbsoluteSource = (m_ContentRoot / m_SourceLogical).string();
		return true;
	}

	void TextureSettingsPanel::RefreshPreviewSource()
	{
		// 字节来源(唯一口径):容器 = 内嵌 payload(版本 = `.wtex` 自己的 mtime);
		// 旧式 = 外部源图(版本 = 源图 mtime)。
		const bool embedded = m_Container && !m_Payload.empty();
		const std::filesystem::path bytesFile = embedded
			? m_ContentRoot / m_AssetLogical
			: (m_SourceLogical.empty() ? std::filesystem::path() : m_ContentRoot / m_SourceLogical);
		std::error_code stampError;
		const std::filesystem::file_time_type stamp = bytesFile.empty()
			? std::filesystem::file_time_type {}
			: std::filesystem::last_write_time(bytesFile, stampError);
		if (m_PreviewSourceStampValid && !stampError && m_PreviewSourceLogical == m_SourceLogical
			&& m_PreviewSourceEmbedded == embedded && stamp == m_PreviewSourceStamp)
			return;

		m_PreviewSourceLogical = m_SourceLogical;
		m_PreviewSourceEmbedded = embedded;
		m_PreviewSourceStamp = stamp;
		m_PreviewSourceStampValid = !stampError;
		m_PreviewSourcePixels.clear();
		m_PreviewSourceWidth = 0;
		m_PreviewSourceHeight = 0;
		m_PreviewSourceValid = false;
		m_SourceWidth = 0;
		m_SourceHeight = 0;

		if (!bytesFile.empty())
		{
			const TextureData data = embedded
				? LoadTextureDataFromMemory(m_Payload, /*flipVertically*/ false)
				: LoadTextureData(bytesFile.string(), /*flipVertically*/ false);
			if (data.Valid && data.Width > 0 && data.Height > 0 && !data.Pixels.empty())
			{
				m_SourceWidth = data.Width;
				m_SourceHeight = data.Height;
				uint32_t keptWidth = data.Width;
				uint32_t keptHeight = data.Height;
				std::vector<uint8_t> kept = ResampleToMaxEdge(data.Pixels, data.Width, data.Height,
					kPreviewSourceMaxEdge, m_Settings.EffectiveSrgb(), keptWidth, keptHeight);
				m_PreviewSourcePixels.swap(kept);
				m_PreviewSourceWidth = keptWidth;
				m_PreviewSourceHeight = keptHeight;
				m_PreviewSourceValid = true;
			}
			WLD_CORE_INFO("[tex-preview] source '{0}'{1}: {2}x{3} (decoded once; {4}x{5} kept for the preview)",
				m_SourceLogical, embedded ? " (embedded payload)" : "", m_SourceWidth, m_SourceHeight,
				m_PreviewSourceWidth, m_PreviewSourceHeight);
		}
		// 源变了 = 旧产物/旧草稿都不再代表这份源:版本 +1,草稿重建,防抖重烘重新排队。
		TouchPreviewRevision();
	}

	void TextureSettingsPanel::PublishPreviewSlot(PreviewSlot& target, const PreviewSlot& incoming)
	{
		if (!incoming.Texture)
		{
			ReleasePreviewSlot(target);
			return;
		}
		Wui::WuiTextureRegistry& registry = Wui::WuiTextureRegistry::Get();
		const uint32_t generation = registry.Generation();
		PreviewSlot next = incoming;
		next.RegistryGeneration = generation;
		next.HostEpoch = m_HostTextureEpoch;
		if (target.Texture && target.Texture.get() == next.Texture.get())
		{
			next.Id = target.Id;
			target = next;
			return;
		}
		if (target.Id != 0 && target.RegistryGeneration == generation)
		{
			registry.Update(target.Id, next.Texture);
			next.Id = target.Id;
		}
		else
		{
			// 注册表整表清空(设备重建)或还没有槽位:重新登记一个新 id。
			next.Id = registry.Register(next.Texture);
		}
		if (target.Texture)
		{
			auto previous = target.Texture;
			Renderer::QueueRelease([previous]() {});
		}
		target = next;
	}

	void TextureSettingsPanel::ReleasePreviewSlot(PreviewSlot& slot)
	{
		if (slot.Id != 0)
		{
			Wui::WuiTextureRegistry& registry = Wui::WuiTextureRegistry::Get();
			if (registry.Generation() == slot.RegistryGeneration)
				registry.Update(slot.Id, nullptr);
		}
		if (slot.Texture)
		{
			auto texture = slot.Texture;
			Renderer::QueueRelease([texture]() {});
		}
		slot = PreviewSlot {};
	}

	void TextureSettingsPanel::InvalidatePreviewTextures()
	{
		ReleasePreviewSlot(m_Draft);
		ReleasePreviewSlot(m_ArtifactPreview);
		ReleasePreviewSlot(m_Shown);
		m_DraftDirty = true;
	}

	void TextureSettingsPanel::EnsureDraftPreview()
	{
		if (!m_DraftDirty)
			return;
		m_DraftDirty = false;
		if (!m_PreviewSourceValid || m_PreviewSourcePixels.empty() || m_PreviewSourceWidth == 0
			|| m_PreviewSourceHeight == 0)
		{
			ReleasePreviewSlot(m_Draft);
			return;
		}
		// 目标尺寸 = 产物的尺寸(max_size 生效),再按 kDraftPreviewMaxEdge 收敛(即时路径必须便宜)。
		uint32_t width = m_SourceWidth > 0 ? m_SourceWidth : m_PreviewSourceWidth;
		uint32_t height = m_SourceHeight > 0 ? m_SourceHeight : m_PreviewSourceHeight;
		bool capped = false;
		if (m_Settings.MaxSize > 0 && std::max(width, height) > m_Settings.MaxSize)
		{
			const double scale = static_cast<double>(m_Settings.MaxSize)
				/ static_cast<double>(std::max(width, height));
			width = std::max(1u, static_cast<uint32_t>(std::floor(width * scale)));
			height = std::max(1u, static_cast<uint32_t>(std::floor(height * scale)));
		}
		if (const uint32_t longest = std::max(width, height); longest > kDraftPreviewMaxEdge)
		{
			const double scale = static_cast<double>(kDraftPreviewMaxEdge) / static_cast<double>(longest);
			width = std::max(1u, static_cast<uint32_t>(std::floor(width * scale)));
			height = std::max(1u, static_cast<uint32_t>(std::floor(height * scale)));
			capped = true;
		}
		uint32_t actualWidth = m_PreviewSourceWidth;
		uint32_t actualHeight = m_PreviewSourceHeight;
		std::vector<uint8_t> pixels = ResampleToMaxEdge(m_PreviewSourcePixels, m_PreviewSourceWidth,
			m_PreviewSourceHeight, std::max(width, height), m_Settings.EffectiveSrgb(), actualWidth,
			actualHeight);
		if (m_Settings.FlipY)
			FlipRowsInPlace(pixels, actualWidth, actualHeight);
		if (m_Settings.PremultiplyAlpha)
			PremultiplyAlphaInPlace(pixels);
		ApplyChannelMask(pixels, static_cast<int>(m_Channel));
		UploadRgba8Draft(pixels, actualWidth, actualHeight, capped);
	}

	void TextureSettingsPanel::UploadRgba8Draft(const std::vector<uint8_t>& pixels, uint32_t width,
		uint32_t height, bool capped)
	{
		Rhi::Device* device = Renderer::GetDevice().get();
		const uint64_t expected = static_cast<uint64_t>(width) * height * 4u;
		if (!device || width == 0 || height == 0 || pixels.size() < expected)
		{
			ReleasePreviewSlot(m_Draft);
			return;
		}
		Rhi::TextureDesc desc;
		desc.Type = Rhi::TextureType::Texture2D;
		desc.Format = Rhi::Format::R8G8B8A8_UNORM;
		desc.Extent = { width, height, 1 };
		desc.MipLevels = 1;
		desc.Usage = Rhi::TextureUsageSampled | Rhi::TextureUsageTransferDst;
		desc.DebugName = "TextureSettings.draft." + m_SourceLogical;
		Rhi::Handle<Rhi::Texture> texture = device->CreateTexture(desc);
		if (!texture)
		{
			ReleasePreviewSlot(m_Draft);
			return;
		}
		texture->SetData(pixels.data(), static_cast<uint64_t>(pixels.size()));
		PreviewSlot incoming;
		incoming.Texture = texture;
		incoming.SourceLogical = m_SourceLogical;
		incoming.Width = width;
		incoming.Height = height;
		incoming.FormatName = "rgba8";
		incoming.MipCount = 1;
		incoming.VerticalFlip = false;
		incoming.Capped = capped;
		incoming.FromArtifact = false;
		incoming.Channel = static_cast<int>(m_Channel);
		incoming.Revision = m_PreviewRevision;
		incoming.Valid = true;
		PublishPreviewSlot(m_Draft, incoming);
	}

	bool TextureSettingsPanel::UploadArtifactPreview(const std::vector<uint8_t>& bytes, uint64_t revision)
	{
		TextureArtifactHeader header;
		std::string error;
		if (!ParseTextureArtifact(bytes, header, error))
		{
			m_PreviewDetail = error;
			WLD_CORE_WARN("[tex-preview] artifact parse failed for '{0}': {1}", m_SourceLogical, error);
			return false;
		}
		Rhi::Device* device = Renderer::GetDevice().get();
		if (!device)
			return false;
		const Rhi::Format format = PreviewDisplayFormat(header.Format);
		if (format == Rhi::Format::Undefined)
		{
			m_PreviewDetail = std::string("artifact format has no preview mapping: ")
				+ TextureBlockFormatName(header.Format);
			return false;
		}
		if (IsBlockCompressed(header.Format) && !device->GetCapabilities().TextureCompressionBC)
		{
			m_PreviewDetail = "device does not support BC texture compression";
			return false;
		}
		uint32_t mipLevels = std::max(1u, std::min(header.MipCount,
			TextureArtifactHeader::MipCountFor(header.Width, header.Height)));
		Rhi::TextureDesc desc;
		desc.Type = Rhi::TextureType::Texture2D;
		desc.Format = format;
		desc.Extent = { header.Width, header.Height, 1 };
		desc.MipLevels = mipLevels;
		desc.Usage = Rhi::TextureUsageSampled | Rhi::TextureUsageTransferDst;
		desc.DebugName = "TextureSettings.artifact." + m_SourceLogical;
		Rhi::Handle<Rhi::Texture> texture = device->CreateTexture(desc);
		if (!texture)
		{
			m_PreviewDetail = "creating the preview texture failed";
			return false;
		}
		for (uint32_t mip = 0; mip < mipLevels; ++mip)
		{
			const TextureArtifactMip& entry = header.Mips[mip];
			const uint64_t begin = static_cast<uint64_t>(kTextureArtifactHeaderSize) + entry.Offset;
			if (entry.Size == 0 || begin + entry.Size > bytes.size())
			{
				m_PreviewDetail = "artifact mip " + std::to_string(mip) + " is out of range";
				Renderer::QueueRelease([texture]() {});
				return false;
			}
			texture->SetData(bytes.data() + static_cast<size_t>(begin), entry.Size, /*layer*/ 0, mip);
		}
		PreviewSlot incoming;
		incoming.Texture = texture;
		incoming.SourceLogical = m_SourceLogical;
		incoming.Width = header.Width;
		incoming.Height = header.Height;
		incoming.FormatName = TextureBlockFormatName(header.Format);
		incoming.MipCount = mipLevels;
		// 产物在烘焙期按设置定死朝向:FlipY=false 时数据第 0 行 = 图像顶部,
		// FlipY=true 时第 0 行 = 图像底部(显示要按 v 反向取样)。
		incoming.VerticalFlip = m_Settings.FlipY;
		incoming.FromArtifact = true;
		incoming.Channel = static_cast<int>(PreviewChannel::Rgb);
		incoming.Revision = revision;
		incoming.Valid = true;
		PublishPreviewSlot(m_ArtifactPreview, incoming);
		WLD_CORE_INFO("[tex-preview] artifact preview '{0}': format={1} {2}x{3} mips={4} srgb={5} flipY={6}",
			m_SourceLogical, incoming.FormatName, header.Width, header.Height, mipLevels,
			header.Srgb ? 1 : 0, m_Settings.FlipY ? 1 : 0);
		return true;
	}

	bool TextureSettingsPanel::UploadArtifactFromDisk()
	{
		const std::string artifactPath = ArtifactAbsolutePath();
		if (artifactPath.empty())
			return false;
		std::vector<uint8_t> bytes;
		std::string error;
		if (!ReadFileBytes(artifactPath, bytes, error))
			return false;
		return UploadArtifactPreview(bytes, m_PreviewRevision);
	}

	void TextureSettingsPanel::DispatchPreviewBake()
	{
		std::vector<uint8_t> bytes;
		std::string absoluteSource;
		bool embedded = false;
		CurrentSourceBytes(bytes, absoluteSource, embedded);
		{
			std::lock_guard<std::mutex> lock(m_BakeMutex);
			if (m_BakeThreadStop)
				return;
			m_BakeSerial = m_PreviewRevision;
			m_BakeRequest.Serial = m_PreviewRevision;
			m_BakeRequest.AbsoluteSource = std::move(absoluteSource);
			m_BakeRequest.Bytes = std::move(bytes);
			m_BakeRequest.Settings = m_Settings;
			m_BakeRequestPending = true;
		}
		m_BakeInFlight = true;
		m_BakeCv.notify_one();
		WLD_CORE_INFO("[tex-preview] preview bake dispatch #{0} for '{1}'{2} ({3}x{4}?)", m_BakeSerial,
			m_SourceLogical, embedded ? " (embedded)" : "", m_SourceWidth, m_SourceHeight);
	}

	void TextureSettingsPanel::PreviewBakeWorkerLoop()
	{
		for (;;)
		{
			PreviewBakeRequest request;
			{
				std::unique_lock<std::mutex> lock(m_BakeMutex);
				m_BakeCv.wait(lock, [this]
				{
					return m_BakeThreadStop || m_BakeRequestPending;
				});
				if (m_BakeThreadStop)
					return;
				request = m_BakeRequest;
				m_BakeRequestPending = false;
			}
			// 工作线程只跑纯 CPU 的烘焙(内核自带编码器初始化与缓存),不碰 UI / 渲染 / 面板状态。
			PreviewBakeResult result;
			result.Serial = request.Serial;
			const double started = WallClockSeconds();
			TextureArtifactHeader header;
			// 容器:按内嵌字节烘(与 cook 同一口径);旧式:按外部源图文件烘。
			result.Success = !request.Bytes.empty()
				? TextureCompiler::BakeBytes(request.Bytes, request.Settings, result.Bytes, header,
					result.Error)
				: TextureCompiler::BakeFile(std::filesystem::path(request.AbsoluteSource),
					request.Settings, result.Bytes, header, result.Error);
			result.ElapsedMs = (WallClockSeconds() - started) * 1000.0;
			{
				std::lock_guard<std::mutex> lock(m_BakeMutex);
				if (m_BakeThreadStop)
					return;
				m_BakeResult = std::move(result);
				m_BakeResultReady = true;
			}
		}
	}

	void TextureSettingsPanel::PumpPreviewBake()
	{
		const double now = WallClockSeconds();
		// 本帧有没有可烘的字节(容器 = 内嵌 payload;旧式 = 外部源图)。
		std::vector<uint8_t> sourceBytes;
		std::string absoluteSource;
		bool embedded = false;
		const bool hasSourceBytes = CurrentSourceBytes(sourceBytes, absoluteSource, embedded);
		// 0) 宿主/设备重建(GL 上下文或 RHI 设备):旧句柄全部作废,草稿与产物都重建。
		Wui::WuiTextureRegistry& registry = Wui::WuiTextureRegistry::Get();
		if (m_PreviewGeneration != registry.Generation() || m_PreviewEpoch != m_HostTextureEpoch)
		{
			m_PreviewGeneration = registry.Generation();
			m_PreviewEpoch = m_HostTextureEpoch;
			InvalidatePreviewTextures();
			m_BakedRevision = 0;
			m_LoadDiskArtifactRequested = true;
		}
		// 1) 打开 / 磁盘重载之后:盘上**新鲜**的产物直接当预览(不必白等一次防抖)。
		if (m_LoadDiskArtifactRequested)
		{
			m_LoadDiskArtifactRequested = false;
			if (m_Artifact.State == Editor::TextureArtifactState::Fresh && UploadArtifactFromDisk())
			{
				m_BakedRevision = m_PreviewRevision;
				m_PreviewState = PreviewState::Baked;
				m_PreviewDetail.clear();
			}
		}
		// 2) 取回工作线程结果(只有仍对应当前设置版本的才采纳)。
		{
			std::lock_guard<std::mutex> lock(m_BakeMutex);
			if (m_BakeResultReady)
			{
				PreviewBakeResult result = std::move(m_BakeResult);
				m_BakeResult = PreviewBakeResult {};
				m_BakeResultReady = false;
				m_BakeInFlight = false;
				if (result.Serial == m_PreviewRevision)
				{
					if (result.Success)
					{
						if (UploadArtifactPreview(result.Bytes, result.Serial))
						{
							m_BakedRevision = result.Serial;
							m_PreviewState = PreviewState::Baked;
							m_PreviewDetail.clear();
							WLD_CORE_INFO("[tex-preview] preview baked '{0}' in {1:.0f} ms (rev {2})",
								m_SourceLogical, result.ElapsedMs, result.Serial);
						}
						else
						{
							m_PreviewState = PreviewState::Failed;
							if (m_PreviewDetail.empty())
								m_PreviewDetail = "preview upload failed";
							WLD_CORE_WARN("[tex-preview] preview upload failed for '{0}': {1}",
								m_SourceLogical, m_PreviewDetail);
						}
					}
					else
					{
						m_PreviewState = PreviewState::Failed;
						m_PreviewDetail = result.Error;
						WLD_CORE_WARN("[tex-preview] preview bake failed for '{0}': {1}",
							m_SourceLogical, result.Error);
					}
				}
			}
		}
		// 3) 版本没跟上 → 重新起防抖;防抖到点且线程空闲 → 派发。
		if (m_BakedRevision != m_PreviewRevision && m_BakeDueSeconds == 0.0 && !m_BakeInFlight)
			m_BakeDueSeconds = now + kPreviewBakeDebounceSeconds;
		if (m_BakeDueSeconds != 0.0 && now >= m_BakeDueSeconds && !m_BakeInFlight)
		{
			m_BakeDueSeconds = 0.0;
			if (!hasSourceBytes)
			{
				m_PreviewState = PreviewState::Idle;
				m_PreviewDetail = m_LegacyAsset
					? Wui::Tr("panel.texture.preview.no_bytes",
						"no embedded source bytes (old-style settings file: re-import the image)")
					: "no source image";
			}
			else
			{
				m_PreviewState = PreviewState::Encoding;
				m_PreviewDetail.clear();
				DispatchPreviewBake();
			}
		}
		else if (m_BakeInFlight)
			m_PreviewState = PreviewState::Encoding;
		else if (m_BakeDueSeconds != 0.0)
			m_PreviewState = PreviewState::Pending;
	}

	void TextureSettingsPanel::SelectShownPreview()
	{
		// 合成视图优先显示**当前设置**烘出来的产物(压缩效果可见);产物没跟上(刚改设置/烘失败/
		// 设备重建)或看单通道时,回落到 CPU 草稿 —— 任何时刻都有一张可看的图(不会变黑)。
		const bool artifactCurrent = m_ArtifactPreview.Valid
			&& m_ArtifactPreview.Revision == m_PreviewRevision
			&& m_ArtifactPreview.SourceLogical == m_SourceLogical
			&& m_Channel == PreviewChannel::Rgb;
		m_Shown = artifactCurrent ? m_ArtifactPreview : m_Draft;
	}

	std::string TextureSettingsPanel::PreviewSummary() const
	{
		if (!m_Shown.Valid)
			return Wui::Tr("panel.texture.preview.unavailable", "Preview unavailable");
		char buffer[64] = {};
		std::snprintf(buffer, sizeof(buffer), "%.3f", m_ZoomAbsolute);
		return Wui::TrFormat("panel.texture.preview.line",
			"Preview {kind} {format} {width}×{height} — {channel}, zoom {zoom}×",
			{ { "kind", m_Shown.FromArtifact
					? Wui::Tr("panel.texture.preview.kind.artifact", "artifact")
					: Wui::Tr("panel.texture.preview.kind.draft", "draft") },
				{ "format", UpperAscii(m_Shown.FormatName) },
				{ "width", std::to_string(m_Shown.Width) },
				{ "height", std::to_string(m_Shown.Height) },
				{ "channel", UpperAscii(PreviewChannelCode(m_Shown.Channel)) },
				{ "zoom", buffer } });
	}

	std::string TextureSettingsPanel::PreviewStateText() const
	{
		switch (m_PreviewState)
		{
			case PreviewState::Pending:
				return Wui::Tr("panel.texture.preview.state.pending", "encoding… (debounced)");
			case PreviewState::Encoding:
				return Wui::Tr("panel.texture.preview.state.encoding", "encoding…");
			case PreviewState::Baked:
				return Wui::Tr("panel.texture.preview.state.baked", "baked");
			case PreviewState::Failed:
				return m_PreviewDetail.empty()
					? Wui::Tr("panel.texture.preview.state.failed_short", "bake failed")
					: Wui::TrFormat("panel.texture.preview.state.failed", "bake failed: {detail}",
						{ { "detail", m_PreviewDetail } });
			case PreviewState::Idle:
			default:
				return Wui::Tr("panel.texture.preview.state.idle", "up to date");
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
		// 窄列口径:标签行 + 控件行(标签行里文本垂直居中)。
		return theme.ControlHeight + theme.ControlHeight + theme.PadSmall;
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

	void TextureSettingsPanel::OnRender(Wui::WuiContext& ctx, const Wui::WuiRect& rect, PanelHost& host)
	{
		m_Ctx = &ctx;
		EnsureContentRoot();
		const Wui::WuiTheme& theme = host.Theme();
		m_HostTextureEpoch = host.TextureEpoch();
		SyncWithDisk(ctx);

		if (m_AssetLogical.empty())
		{
			Wui::EmptyState(ctx, rect, std::string(),
				Wui::Tr("panel.texture.empty.title", "No texture asset open"),
				Wui::Tr("panel.texture.empty.hint",
					"Double-click a source image (imports it) or a .wtex asset in the Content Browser to "
					"edit its import settings here."),
				std::string(), 0, theme);
			m_LastRenderFrame = ctx.Frame();
			m_RenderedOnce = true;
			return;
		}

		// 每帧管线:源像素(解码一次;容器 = 内嵌 payload)→ 磁盘产物状态 → 预览重烘(防抖/工作线程)
		// → 草稿 → 选一张画。
		RefreshPreviewSource();
		RefreshArtifactState();
		PumpPreviewBake();
		EnsureDraftPreview();
		SelectShownPreview();

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
		const Wui::WuiRect actionsRect { rect.X, rect.Y + rect.H - actionsHeight, rect.W, actionsHeight };
		const Wui::WuiRect bodyRect { rect.X, y, rect.W,
			std::max(0.0f, actionsRect.Y - y - theme.PadSmall) };

		// 左主区 = 预览(≥ 60% 宽);右窄列 = 属性(240–280,整块 < 30%)。窄面板上下堆叠。
		if (bodyRect.W < 620.0f)
		{
			const float previewH = std::min(std::max(150.0f, std::floor(bodyRect.H * 0.5f)), bodyRect.H);
			const Wui::WuiRect previewRect { bodyRect.X, bodyRect.Y, bodyRect.W, previewH };
			const Wui::WuiRect columnRect { bodyRect.X, bodyRect.Y + previewH + theme.PadSmall, bodyRect.W,
				std::max(0.0f, bodyRect.H - previewH - theme.PadSmall) };
			DrawPreviewArea(ctx, theme, host, previewRect);
			DrawPropertiesColumn(ctx, theme, host, columnRect);
		}
		else
		{
			const float columnW = std::clamp(bodyRect.W * 0.26f, 240.0f, 280.0f);
			const Wui::WuiRect previewRect { bodyRect.X, bodyRect.Y,
				std::max(0.0f, bodyRect.W - columnW - theme.Pad), bodyRect.H };
			const Wui::WuiRect columnRect { previewRect.X + previewRect.W + theme.Pad, bodyRect.Y, columnW,
				bodyRect.H };
			DrawPreviewArea(ctx, theme, host, previewRect);
			DrawPropertiesColumn(ctx, theme, host, columnRect);
		}

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

		if (!m_LoadError.empty())
		{
			Wui::Label(ctx, { x, y },
				EllipsizeToWidth(ctx, m_LoadError, width, theme.FontSizeSmall), theme.Danger,
				theme.FontSizeSmall);
			y += theme.FontSizeSmall + theme.PadSmall;
		}
	}

	void TextureSettingsPanel::DrawPreviewArea(Wui::WuiContext& ctx, const Wui::WuiTheme& theme,
		PanelHost& host, const Wui::WuiRect& rect)
	{
		(void)host;
		if (rect.W <= 8.0f || rect.H <= 8.0f)
			return;
		const float toolbarH = std::max(theme.ControlHeight, 24.0f);
		const float statusH = (theme.FontSizeSmall + 3.0f) * 3.0f + theme.PadSmall;
		const float imageH = std::max(0.0f, rect.H - toolbarH - statusH - theme.PadSmall * 2.0f);
		const Wui::WuiRect toolbarRect { rect.X, rect.Y, rect.W, toolbarH };
		const Wui::WuiRect imageRect { rect.X, rect.Y + toolbarH + theme.PadSmall, rect.W, imageH };
		const Wui::WuiRect statusRect { rect.X, imageRect.Y + imageRect.H + theme.PadSmall, rect.W, statusH };

		// 通道开关(左):RGB / R / G / B / A —— Segmented 自带组节点 + 每格节点(脚本可点)。
		const float channelW = std::clamp(rect.W * 0.4f, 140.0f, 260.0f);
		const Wui::WuiRect channelRect { toolbarRect.X, toolbarRect.Y, channelW, toolbarH };
		const std::vector<std::string> options { "RGB", "R", "G", "B", "A" };
		int selected = static_cast<int>(m_Channel);
		if (Wui::Segmented(ctx, Wui::HashId("texture.preview.channels"), channelRect, options, selected, theme)
			&& selected >= 0 && selected <= static_cast<int>(PreviewChannel::Alpha))
		{
			m_Channel = static_cast<PreviewChannel>(selected);
			m_DraftDirty = true;
			if (m_Ctx)
				m_Ctx->RecordOp("texture", "preview-channel", m_AssetLogical,
					options[static_cast<size_t>(selected)]);
		}
		Wui::Tooltip(ctx, channelRect, Wui::Tr("panel.texture.preview.channels.tooltip",
			"Preview channels: RGB = composite (shows the baked artifact, compression visible); "
			"R/G/B/A = one channel as grayscale."));

		// 缩放:FIT / 1:1 + 读数(右侧)。
		const PreviewZoomRange zoomRange = PreviewZoomFor(imageRect, m_Shown.Valid ? m_Shown.Width : 0,
			m_Shown.Valid ? m_Shown.Height : 0);
		m_ZoomFactor = std::clamp(m_ZoomFactor, zoomRange.MinFactor, zoomRange.MaxFactor);
		const float buttonW = std::clamp(toolbarRect.W * 0.08f, 38.0f, 56.0f);
		const float oneToOneX = toolbarRect.X + toolbarRect.W - buttonW;
		const float fitX = oneToOneX - buttonW - theme.PadSmall;
		const Wui::WuiRect fitRect { fitX, toolbarRect.Y, buttonW, toolbarH };
		const Wui::WuiRect oneToOneRect { oneToOneX, toolbarRect.Y, buttonW, toolbarH };
		if (Wui::ButtonEx(ctx, Wui::HashId("texture.preview.fit"), fitRect,
				Wui::Tr("panel.texture.preview.fit", "Fit"), theme, true, false,
				Wui::Tr("panel.texture.preview.fit.tooltip",
					"Fit the whole texture into the preview (double-clicking the image does the same).")))
		{
			m_ZoomFactor = 1.0f;
			m_CenterU = 0.5f;
			m_CenterV = 0.5f;
			if (m_Ctx)
				m_Ctx->RecordOp("texture", "preview-zoom", m_AssetLogical, "fit");
		}
		if (Wui::ButtonEx(ctx, Wui::HashId("texture.preview.one_to_one"), oneToOneRect,
				Wui::Tr("panel.texture.preview.one_to_one", "1:1"), theme, true, false,
				Wui::Tr("panel.texture.preview.one_to_one.tooltip",
					"One texture pixel per screen pixel (clamped to this preview's zoom range).")))
		{
			// 1:1 = 一个纹素一个屏幕像素(小图会缩到 1:1,大图会放大到 1:1;范围见 PreviewZoomFor)。
			m_ZoomFactor = std::clamp(zoomRange.Fit > 0.0f ? 1.0f / zoomRange.Fit : 1.0f,
				zoomRange.MinFactor, zoomRange.MaxFactor);
			m_CenterU = 0.5f;
			m_CenterV = 0.5f;
			if (m_Ctx)
				m_Ctx->RecordOp("texture", "preview-zoom", m_AssetLogical, "1:1");
		}
		const float zoom = zoomRange.Fit * m_ZoomFactor;
		char zoomBuffer[32] = {};
		std::snprintf(zoomBuffer, sizeof(zoomBuffer), "%.2f×", zoom);
		const float zoomRight = fitRect.X - theme.Pad;
		const float zoomLeft = channelRect.X + channelRect.W + theme.Pad;
		const float zoomW = std::max(0.0f, std::min(std::max(56.0f, toolbarRect.W * 0.16f),
			zoomRight - zoomLeft));
		const Wui::WuiRect zoomRect { zoomRight - zoomW, toolbarRect.Y, zoomW, toolbarH };
		if (zoomRect.W > 4.0f)
			Wui::Label(ctx, { zoomRect.X + theme.PadSmall,
					zoomRect.Y + (zoomRect.H - theme.FontSizeSmall) * 0.5f },
				EllipsizeToWidth(ctx, zoomBuffer, std::max(0.0f, zoomRect.W - theme.PadSmall),
					theme.FontSizeSmall),
				theme.TextMuted, theme.FontSizeSmall);
		{
			Wui::WuiAccessNode node;
			node.Id = Wui::HashId("texture.preview.zoom");
			node.Window = Wui::WuiAccessibility::Get().CurrentWindow();
			node.Panel = Wui::WuiAccessibility::Get().CurrentPanel();
			node.Kind = "text";
			node.Label = Wui::Tr("panel.texture.preview.zoom.label", "Preview zoom");
			char value[96] = {};
			std::snprintf(value, sizeof(value), "zoom=%.3f;fit=%d", zoom,
				std::abs(m_ZoomFactor - 1.0f) <= 1e-3f ? 1 : 0);
			node.Value = value;
			node.Tooltip = Wui::Tr("panel.texture.preview.image.tooltip",
				"Wheel = zoom at the cursor, drag = pan, double-click = fit.");
			node.Rect = zoomRect.W > 4.0f ? zoomRect : toolbarRect;
			node.Enabled = true;
			node.Interactive = false;
			Wui::WuiAccessibility::Get().Register(node);
		}

		DrawPreviewImage(ctx, theme, imageRect);
		float statusY = statusRect.Y;
		DrawPreviewStatus(ctx, theme, statusRect, statusY);
	}

	void TextureSettingsPanel::DrawPreviewImage(Wui::WuiContext& ctx, const Wui::WuiTheme& theme,
		const Wui::WuiRect& rect)
	{
		Wui::DrawPanelSurface(ctx, rect, theme);
		if (rect.W <= 8.0f || rect.H <= 8.0f)
			return;
		if (!m_Shown.Valid || m_Shown.Width == 0 || m_Shown.Height == 0 || m_Shown.Id == 0)
		{
			Wui::Label(ctx, { rect.X + theme.Pad, rect.Y + theme.Pad },
				Wui::Tr("panel.texture.preview.unavailable", "Preview unavailable"), theme.TextMuted,
				theme.FontSizeSmall);
			m_ZoomAbsolute = 0.0f;
			return;
		}

		const float textureW = static_cast<float>(m_Shown.Width);
		const float textureH = static_cast<float>(m_Shown.Height);
		const PreviewZoomRange zoomRange = PreviewZoomFor(rect, m_Shown.Width, m_Shown.Height);
		m_ZoomFactor = std::clamp(m_ZoomFactor, zoomRange.MinFactor, zoomRange.MaxFactor);
		float zoom = zoomRange.Fit * m_ZoomFactor;
		float visibleW = std::clamp(rect.W / (zoom * textureW), 0.0f, 1.0f);
		float visibleH = std::clamp(rect.H / (zoom * textureH), 0.0f, 1.0f);
		const auto clampCenter = [&]()
		{
			m_CenterU = visibleW >= 1.0f ? 0.5f
				: std::clamp(m_CenterU, visibleW * 0.5f, 1.0f - visibleW * 0.5f);
			m_CenterV = visibleH >= 1.0f ? 0.5f
				: std::clamp(m_CenterV, visibleH * 0.5f, 1.0f - visibleH * 0.5f);
		};
		clampCenter();

		const glm::vec2 mouse = ctx.Input().MousePos;
		const bool hovered = ctx.IsHovered(rect);
		// 滚轮 = 以光标为锚点缩放(方向与真实鼠标一致:向上滚 = 放大)。
		if (hovered && ctx.Input().Wheel != 0.0f)
		{
			const float fx = std::clamp((mouse.x - rect.X) / std::max(1.0f, rect.W), 0.0f, 1.0f);
			const float fy = std::clamp((mouse.y - rect.Y) / std::max(1.0f, rect.H), 0.0f, 1.0f);
			const float anchorU = m_CenterU + (fx - 0.5f) * visibleW;
			const float anchorV = m_CenterV + (fy - 0.5f) * visibleH;
			m_ZoomFactor = std::clamp(m_ZoomFactor * std::pow(1.15f, ctx.Input().Wheel),
				zoomRange.MinFactor, zoomRange.MaxFactor);
			zoom = zoomRange.Fit * m_ZoomFactor;
			visibleW = std::clamp(rect.W / (zoom * textureW), 0.0f, 1.0f);
			visibleH = std::clamp(rect.H / (zoom * textureH), 0.0f, 1.0f);
			m_CenterU = anchorU - (fx - 0.5f) * visibleW;
			m_CenterV = anchorV - (fy - 0.5f) * visibleH;
			clampCenter();
			if (m_Ctx)
				m_Ctx->RecordOp("texture", "preview-wheel", m_AssetLogical, "wheel");
		}
		// 双击 = fit。
		if (hovered && ctx.IsDoubleClicked(rect))
		{
			m_ZoomFactor = 1.0f;
			m_CenterU = 0.5f;
			m_CenterV = 0.5f;
			if (m_Ctx)
				m_Ctx->RecordOp("texture", "preview-zoom", m_AssetLogical, "fit (double-click)");
		}
		// 拖拽 = 平移(内容跟手:向右拖 = 看图像左边)。
		if (hovered && ctx.Input().MouseClicked[0])
		{
			m_PanActive = true;
			m_PanLast = mouse;
		}
		if (!ctx.Input().MouseDown[0])
			m_PanActive = false;
		if (m_PanActive)
		{
			const glm::vec2 delta = mouse - m_PanLast;
			m_PanLast = mouse;
			if (delta.x != 0.0f || delta.y != 0.0f)
			{
				m_CenterU -= delta.x / (zoom * textureW);
				m_CenterV -= delta.y / (zoom * textureH);
				clampCenter();
			}
		}
		m_ZoomAbsolute = zoom;

		// 画:可见区(归一化)映射到居中矩形 —— uv 裁剪就是缩放/平移,不需要裁剪命令。
		const float u0 = m_CenterU - visibleW * 0.5f;
		const float v0 = m_CenterV - visibleH * 0.5f;
		const float textureV0 = m_Shown.VerticalFlip ? 1.0f - v0 : v0;
		const float textureV1 = m_Shown.VerticalFlip ? 1.0f - (v0 + visibleH) : (v0 + visibleH);
		const float drawW = visibleW * textureW * zoom;
		const float drawH = visibleH * textureH * zoom;
		const Wui::WuiRect target { rect.X + (rect.W - drawW) * 0.5f, rect.Y + (rect.H - drawH) * 0.5f,
			drawW, drawH };
		Wui::Image(ctx, target, m_Shown.Id, { u0, textureV0, visibleW, textureV1 - textureV0 }, theme);
		if (hovered)
			ctx.SetCursor(Wui::WuiCursor::Hand);
		Wui::Tooltip(ctx, rect, Wui::Tr("panel.texture.preview.image.tooltip",
			"Wheel = zoom at the cursor, drag = pan, double-click = fit."));

		// 无障碍:这张图 + 显示口径(通道 / 来源 / 格式 / 尺寸 / 缩放)—— AI 通道的主证据节点。
		{
			Wui::WuiAccessNode node;
			node.Id = Wui::HashId("texture.preview.image");
			node.Window = Wui::WuiAccessibility::Get().CurrentWindow();
			node.Panel = Wui::WuiAccessibility::Get().CurrentPanel();
			node.Kind = "image";
			node.Label = PreviewSummary();
			char value[256] = {};
			std::snprintf(value, sizeof(value),
				"channel=%s;kind=%s;format=%s;size=%ux%u;zoom=%.3f;source=%s",
				PreviewChannelCode(m_Shown.Channel),
				m_Shown.FromArtifact ? "artifact" : "draft", m_Shown.FormatName.c_str(),
				m_Shown.Width, m_Shown.Height, m_ZoomAbsolute, m_Shown.SourceLogical.c_str());
			node.Value = value;
			node.Tooltip = Wui::Tr("panel.texture.preview.image.tooltip",
				"Wheel = zoom at the cursor, drag = pan, double-click = fit.");
			node.Rect = rect;
			node.Enabled = true;
			node.Interactive = true;
			node.Visible = true;
			Wui::WuiAccessibility::Get().Register(node);
		}
	}

	void TextureSettingsPanel::DrawPreviewStatus(Wui::WuiContext& ctx, const Wui::WuiTheme& theme,
		const Wui::WuiRect& rect, float& y)
	{
		const float x = rect.X;
		const float width = std::max(0.0f, rect.W);
		Wui::Label(ctx, { x, y }, EllipsizeToWidth(ctx, PreviewSummary(), width, theme.FontSizeSmall),
			theme.Text, theme.FontSizeSmall);
		y += theme.FontSizeSmall + 3.0f;
		const Wui::WuiColor stateColor = m_PreviewState == PreviewState::Failed ? theme.Danger
			: (m_PreviewState == PreviewState::Baked ? theme.Success : theme.Warning);
		const std::string stateText = PreviewStateText();
		Wui::Label(ctx, { x, y }, EllipsizeToWidth(ctx, stateText, width, theme.FontSizeSmall),
			stateColor, theme.FontSizeSmall);
		{
			Wui::WuiAccessNode node;
			node.Id = Wui::HashId("texture.preview.state");
			node.Window = Wui::WuiAccessibility::Get().CurrentWindow();
			node.Panel = Wui::WuiAccessibility::Get().CurrentPanel();
			node.Kind = "text";
			node.Label = Wui::Tr("panel.texture.preview.state.label", "Preview bake state");
			node.Value = PreviewStateCode(static_cast<int>(m_PreviewState));
			node.Tooltip = stateText;
			node.Rect = { x, y, width, theme.FontSizeSmall };
			node.Enabled = true;
			node.Interactive = false;
			node.Visible = true;
			Wui::WuiAccessibility::Get().Register(node);
		}
		y += theme.FontSizeSmall + 3.0f;

		std::string hint;
		if (m_Shown.Capped)
			hint = Wui::TrFormat("panel.texture.preview.capped",
				"Draft preview capped to {width}×{height}; the artifact replaces it at full resolution",
				{ { "width", std::to_string(m_Shown.Width) },
					{ "height", std::to_string(m_Shown.Height) } });
		else if (m_Shown.Channel != static_cast<int>(PreviewChannel::Rgb))
			hint = Wui::Tr("panel.texture.preview.channel_hint",
				"Channel view is computed from the source image at the current size settings "
				"(compression is visible in the RGB view).");
		else if (m_PreviewState == PreviewState::Failed)
			hint = Wui::Tr("panel.texture.preview.keep_hint",
				"Keeping the last usable preview; edit a setting to retry.");
		if (!hint.empty())
			Wui::Label(ctx, { x, y }, EllipsizeToWidth(ctx, hint, width, theme.FontSizeCaption),
				theme.TextDisabled, theme.FontSizeCaption);
	}

	void TextureSettingsPanel::DrawPropertiesColumn(Wui::WuiContext& ctx, const Wui::WuiTheme& theme,
		PanelHost& host, const Wui::WuiRect& rect)
	{
		(void)host;
		if (rect.W <= 8.0f || rect.H <= 8.0f)
			return;
		const float rowH = FieldRowHeight(theme);
		const float statusLine = theme.FontSizeSmall + 3.0f;
		const float contentHeight = theme.Pad * 2.0f + rowH + theme.FontSizeCaption + theme.PadSmall
			+ (m_SourceError.empty() ? 0.0f : theme.FontSizeCaption + 3.0f)
			+ rowH * 11.0f + statusLine * 4.0f;
		Wui::BeginScrollArea(ctx, rect, contentHeight, m_Scroll, theme,
			Wui::HashId("texture.fields.scroll"));
		float y = rect.Y + theme.Pad;
		y = DrawSourceRow(ctx, theme, rect, y);
		y = DrawFields(ctx, theme, rect, y);
		y = DrawStatusLine(ctx, theme, rect, y);
		Wui::EndScrollArea(ctx);
	}

	float TextureSettingsPanel::DrawSourceRow(Wui::WuiContext& ctx, const Wui::WuiTheme& theme,
		const Wui::WuiRect& rect, float y)
	{
		const float x = rect.X;
		const float width = rect.W;
		// 标签行:左标签 + 右"在资源管理器中显示"(按钮宽度随列宽收窄,窄列也能点)。
		const float buttonW = std::clamp(width * 0.5f, 96.0f, 150.0f);
		const Wui::WuiRect labelRect { x, y, std::max(0.0f, width - buttonW - theme.PadSmall),
			theme.ControlHeight };
		Wui::Label(ctx, { labelRect.X, y + (theme.ControlHeight - theme.FontSizeBody) * 0.5f },
			EllipsizeToWidth(ctx, Wui::Tr("panel.texture.source", "Import source"), labelRect.W,
				theme.FontSizeBody),
			theme.TextMuted, theme.FontSizeBody);
		Wui::Tooltip(ctx, labelRect, Wui::Tr("panel.texture.source.tooltip",
			"Optional: content-root relative path of the image this single-file asset was imported "
			"from. The asset stores the source bytes itself, so an empty path (or a source image that "
			"moved away) still works; type a path and press Apply to re-embed that image."));
		const Wui::WuiRect revealRect { x + width - buttonW, y, buttonW, theme.ControlHeight };
		const bool canReveal = !m_SourceLogical.empty();
		if (Wui::ButtonEx(ctx, Wui::HashId("texture.field.source.reveal"), revealRect,
				Wui::Tr("panel.texture.source.reveal", "Show in Explorer"), theme, canReveal, false,
				canReveal
					? Wui::Tr("panel.texture.source.reveal.tooltip",
						"Reveal this texture in Windows Explorer (/select): a single-file asset points at "
						"its own .wtex, an old-style asset at its source image.")
					: Wui::Tr("panel.texture.source.reveal.tooltip.none",
						"Nothing to reveal yet: import a source image first.")))
		{
			RevealPathInExplorer(m_ContentRoot / m_SourceLogical);
			if (m_Ctx)
				m_Ctx->RecordOp("texture", "reveal-source", m_AssetLogical, m_SourceLogical);
		}
		y += theme.ControlHeight + theme.PadSmall;

		// 路径输入框:非法 / 不存在 → 行内错误(TextFieldEx 自己画在框下方),**不落盘**。
		Wui::TextFieldA11y a11y;
		a11y.Label = Wui::Tr("panel.texture.source", "Import source");
		a11y.Placeholder = Wui::Tr("panel.texture.source.placeholder",
			"(embedded bytes — optional import source)");
		const bool committed = Wui::TextFieldEx(ctx, Wui::HashId("texture.field.source"),
			{ x, y, width, theme.ControlHeight }, m_SourceBuffer, theme, m_SourceError, &a11y);
		y += theme.ControlHeight;
		if (!m_SourceError.empty())
			y += theme.FontSizeCaption + 3.0f;
		y += theme.PadSmall;
		if (committed)
		{
			const std::string trimmed = TrimAscii(m_SourceBuffer);
			std::string error;
			if (!Editor::ValidateTextureSourceText(m_ContentRoot, trimmed, error))
			{
				m_SourceError = error;
				if (m_Ctx)
					m_Ctx->RecordOp("texture", "source-rejected", m_AssetLogical, error);
			}
			else
			{
				m_SourceError.clear();
				if (m_Settings.Source != trimmed)
				{
					m_Settings.Source = trimmed;
					m_SourceBuffer = trimmed;
					MarkSettingsDirty("source");
				}
			}
		}

		// 字节来源 / 资产形态(M4-TEX P9):容器 = 内嵌源字节(源图只是可选的导入源);
		// 旧式 = 必须有一个外部源图,否则给"可重新导入"的提示。同一句话进 a11y 节点给脚本读。
		std::string form;
		std::string formCode;
		if (m_Container)
		{
			formCode = "container";
			form = m_AssetFormNote.empty()
				? Wui::TrFormat("panel.texture.form.container",
					"Single-file asset: {bytes} bytes embedded; the import source is optional.",
					{ { "bytes", std::to_string(m_Payload.size()) } })
				: m_AssetFormNote;
			if (!m_ImportSourceLogical.empty())
				form += "  " + Wui::TrFormat("panel.texture.source.import_on_disk",
					"Import source on disk: {path}",
					{ { "path", m_ImportSourceLogical } });
		}
		else if (m_LegacyAsset)
		{
			formCode = m_SourceLogical.empty() ? "legacy-broken" : "legacy";
			form = m_AssetFormNote.empty()
				? Wui::Tr("panel.texture.form.legacy",
					"Old-style settings file (no embedded bytes). Press Apply to re-import it as a "
					"single-file asset.")
				: m_AssetFormNote;
			if (!m_SourceLogical.empty())
				form += "  " + Wui::TrFormat("panel.texture.source.resolved", "Resolved: {path}",
					{ { "path", m_SourceLogical } });
		}
		else
		{
			formCode = m_SourceLogical.empty() ? "none" : "external";
			form = m_SourceLogical.empty()
				? Wui::Tr("panel.texture.source.resolved.none", "Resolved: (not found)")
				: Wui::TrFormat("panel.texture.source.resolved", "Resolved: {path}",
					{ { "path", m_SourceLogical } });
		}
		Wui::Label(ctx, { x, y }, EllipsizeToWidth(ctx, form, width, theme.FontSizeCaption),
			formCode == "none" ? theme.Danger
				: (formCode == "legacy" ? theme.Warning : theme.TextDisabled),
			theme.FontSizeCaption);
		{
			Wui::WuiAccessNode node;
			node.Id = Wui::HashId("texture.source.form");
			node.Window = Wui::WuiAccessibility::Get().CurrentWindow();
			node.Panel = Wui::WuiAccessibility::Get().CurrentPanel();
			node.Kind = "status";
			node.Label = Wui::Tr("panel.texture.source.form.label", "Asset form / byte source");
			node.Value = formCode;
			node.Tooltip = form;
			node.Rect = { x, y, width, theme.FontSizeCaption };
			node.Enabled = true;
			node.Interactive = false;
			node.Visible = true;
			Wui::WuiAccessibility::Get().Register(node);
		}
		{
			// 预览/烘焙真正读的那个字节来源(容器 = 资产自身;旧式 = 外部源图)——
			// "在资源管理器中显示"指向的也是它(P9:容器指向 `.wtex` 自己)。
			Wui::WuiAccessNode node;
			node.Id = Wui::HashId("texture.source.bytes");
			node.Window = Wui::WuiAccessibility::Get().CurrentWindow();
			node.Panel = Wui::WuiAccessibility::Get().CurrentPanel();
			node.Kind = "text";
			node.Label = Wui::Tr("panel.texture.source.bytes.label", "Byte source");
			node.Value = m_SourceLogical;
			node.Tooltip = Wui::Tr("panel.texture.source.bytes.tooltip",
				"Logical path the preview and the bake read: a single-file asset reads itself, an "
				"old-style asset reads its source image. Show in Explorer targets the same path.");
			node.Rect = { x, y + theme.FontSizeCaption, width, 2.0f };
			node.Enabled = true;
			node.Interactive = false;
			node.Visible = true;
			Wui::WuiAccessibility::Get().Register(node);
		}
		y += theme.FontSizeCaption + theme.PadSmall;
		return y;
	}

	float TextureSettingsPanel::DrawFields(Wui::WuiContext& ctx, const Wui::WuiTheme& theme,
		const Wui::WuiRect& rect, float y)
	{
		const float rowH = FieldRowHeight(theme);
		const float x = rect.X;
		const float width = rect.W;
		const float controlH = theme.ControlHeight;
		auto drawLabel = [&](const std::string& text, const std::string& tooltip, float rowTop)
		{
			const Wui::WuiRect labelRect { x, rowTop, width, theme.ControlHeight };
			Wui::Label(ctx, { x, rowTop + (theme.ControlHeight - theme.FontSizeBody) * 0.5f },
				EllipsizeToWidth(ctx, text, width, theme.FontSizeBody), theme.TextMuted,
				theme.FontSizeBody);
			if (!tooltip.empty())
				Wui::Tooltip(ctx, labelRect, tooltip);
		};
		auto controlRect = [&](float rowTop)
		{
			return Wui::WuiRect { x, rowTop + theme.ControlHeight, width, controlH };
		};

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
			if (Wui::Combo(ctx, Wui::HashId("texture.field.usage"), controlRect(y),
					Wui::Tr("panel.texture.usage", "Usage"), options, selected, theme)
				&& selected >= 0 && selected <= static_cast<int>(TextureUsage::Ui))
			{
				m_Settings.Usage = static_cast<TextureUsage>(selected);
				MarkSettingsDirty("usage");
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
					"Block format of the baked artifact. Auto follows usage. The preview re-bakes "
					"automatically a moment after a change; Apply / Reimport write it to disk."),
				y);
			if (Wui::Combo(ctx, Wui::HashId("texture.field.compression"), controlRect(y),
					Wui::Tr("panel.texture.compression", "Compression"), options, selected, theme)
				&& selected >= 0 && selected <= static_cast<int>(TextureCompression::BC3))
			{
				m_Settings.Compression = static_cast<TextureCompression>(selected);
				MarkSettingsDirty("compression");
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
			if (Wui::Combo(ctx, Wui::HashId("texture.field.srgb"), controlRect(y),
					Wui::Tr("panel.texture.srgb", "sRGB"), options, selected, theme))
			{
				if (selected == 0)
					m_Settings.SrgbExplicit = false;
				else
				{
					m_Settings.SrgbExplicit = true;
					m_Settings.Srgb = selected == 1;
				}
				MarkSettingsDirty("srgb");
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
			if (Wui::Combo(ctx, Wui::HashId("texture.field.mipmaps"), controlRect(y),
					Wui::Tr("panel.texture.mipmaps", "Mipmaps"), options, selected, theme))
			{
				if (selected == 0)
					m_Settings.MipmapsExplicit = false;
				else
				{
					m_Settings.MipmapsExplicit = true;
					m_Settings.Mipmaps = selected == 1;
				}
				MarkSettingsDirty("mipmaps");
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
			if (Wui::Combo(ctx, Wui::HashId("texture.field.mip_filter"), controlRect(y),
					Wui::Tr("panel.texture.mip_filter", "Mip filter"), options, selected, theme)
				&& selected >= 0 && selected <= static_cast<int>(TextureMipFilter::GammaCorrect))
			{
				m_Settings.MipFilter = static_cast<TextureMipFilter>(selected);
				MarkSettingsDirty("mip_filter");
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
			if (Wui::NumberFieldInt(ctx, Wui::HashId("texture.field.max_size"), controlRect(y), value, 0,
					16384, theme, style)
				&& value >= 0)
			{
				m_Settings.MaxSize = static_cast<uint32_t>(value);
				MarkSettingsDirty("max_size");
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
			if (Wui::Combo(ctx, Wui::HashId("texture.field.wrap"), controlRect(y),
					Wui::Tr("panel.texture.wrap", "Wrap"), options, selected, theme)
				&& selected >= 0 && selected <= static_cast<int>(TextureWrap::Mirror))
			{
				m_Settings.Wrap = static_cast<TextureWrap>(selected);
				MarkSettingsDirty("wrap");
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
			if (Wui::Combo(ctx, Wui::HashId("texture.field.filter"), controlRect(y),
					Wui::Tr("panel.texture.filter", "Filter"), options, selected, theme)
				&& selected >= 0 && selected <= static_cast<int>(TextureFilter::Trilinear))
			{
				m_Settings.Filter = static_cast<TextureFilter>(selected);
				MarkSettingsDirty("filter");
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
			if (Wui::StepperInt(ctx, Wui::HashId("texture.field.anisotropy"), controlRect(y), value, 1, 16,
					theme, Wui::WuiNumberStyle {}))
			{
				m_Settings.Anisotropy = static_cast<uint32_t>(value);
				MarkSettingsDirty("anisotropy");
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
			const Wui::WuiRect row = controlRect(y);
			if (Wui::Checkbox(ctx, Wui::HashId("texture.field.premultiply_alpha"),
					{ row.X, row.Y, std::min(row.W, 120.0f), row.H }, std::string(), value, theme))
			{
				m_Settings.PremultiplyAlpha = value;
				MarkSettingsDirty("premultiply_alpha");
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
			const Wui::WuiRect row = controlRect(y);
			if (Wui::Checkbox(ctx, Wui::HashId("texture.field.flip_y"),
					{ row.X, row.Y, std::min(row.W, 120.0f), row.H }, std::string(), value, theme))
			{
				m_Settings.FlipY = value;
				MarkSettingsDirty("flip_y");
			}
			y += rowH;
		}
		return y;
	}

	float TextureSettingsPanel::DrawStatusLine(Wui::WuiContext& ctx, const Wui::WuiTheme& theme,
		const Wui::WuiRect& rect, float y)
	{
		const float x = rect.X;
		const float width = std::max(0.0f, rect.W);
		Wui::Label(ctx, { x, y }, EllipsizeToWidth(ctx, ArtifactSummary(), width, theme.FontSizeSmall),
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
		return y;
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
					"Save the .wtex asset and re-bake <stem>.wtexc in the content root (the live artifact "
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

		// 导入源(可选)先在**这里**校验:非法 / 不存在 → 行内错误 + 不落盘(不改 .wtex、不重烘)。
		// M4-TEX P9:容器不依赖它(留空 = 用内嵌 payload);旧式文件(没有 payload)才必须有源图。
		const std::string trimmedSource = TrimAscii(m_SourceBuffer);
		std::string sourceTextError;
		if (!Editor::ValidateTextureSourceText(m_ContentRoot, trimmedSource, sourceTextError))
		{
			m_SourceError = sourceTextError;
			m_Status = Wui::TrFormat("panel.texture.status.source_invalid",
				"Cannot apply: fix the source path first ({detail})",
				{ { "detail", sourceTextError } });
			m_StatusIsError = true;
			if (m_Ctx)
				m_Ctx->RecordOp("texture", "apply-rejected", m_AssetLogical, sourceTextError);
			return;
		}
		m_SourceError.clear();
		const bool sourceChanged = m_Settings.Source != trimmedSource;
		m_Settings.Source = trimmedSource;
		m_SourceBuffer = trimmedSource;
		if (sourceChanged)
			TouchPreviewRevision();

		// 1) 定 payload(一张纹理 = 一个文件 = 设置头 + 内嵌源字节):
		//    * 已有容器 payload → **逐字节保持**(只改设置不碰 payload);
		//    * 导入源这一行被改过(或还没有 payload)→ 用该文件的原始字节(同一张图重导入 = 同字节);
		//    * 两者都没有 → 没有可内嵌的源,给可读错误(不再写旧式文件)。
		std::vector<uint8_t> payload = m_Payload;
		const bool sourceEdited = trimmedSource != m_LoadedSourceText;
		if (!trimmedSource.empty() && (payload.empty() || sourceEdited))
		{
			const std::filesystem::path imported = m_ContentRoot / std::filesystem::path(trimmedSource);
			std::vector<uint8_t> bytes;
			std::string readError;
			if (!ReadFileBytes(imported, bytes, readError) || bytes.empty())
			{
				m_Status = Wui::TrFormat("panel.texture.status.import_failed",
					"Cannot import '{path}': {detail}",
					{ { "path", trimmedSource }, { "detail", readError.empty()
							? std::string("empty file") : readError } });
				m_StatusIsError = true;
				m_SourceError = readError;
				if (m_Ctx)
					m_Ctx->RecordOp("texture", "apply-failed", m_AssetLogical, readError);
				return;
			}
			payload = std::move(bytes);
		}
		if (payload.empty())
		{
			m_Status = Wui::Tr("panel.texture.status.no_bytes",
				"Cannot apply: this asset has no embedded source bytes yet — set an import source "
				"(.png/.jpg/.jpeg/.tga/.bmp) or re-import the image, then Apply again.");
			m_StatusIsError = true;
			if (m_Ctx)
				m_Ctx->RecordOp("texture", "apply-rejected", m_AssetLogical, "no embedded payload");
			return;
		}

		// 2) 保存资产 = **单文件容器**(头 + `---payload` + 源字节;原子替换)。
		std::string writeError;
		TextureAssetFile container;
		container.Settings = m_Settings;
		container.Payload = payload;
		if (!SaveTextureAssetFile(std::filesystem::path(AssetAbsolutePath()), container, writeError))
		{
			m_Status = Wui::TrFormat("panel.texture.status.save_failed", "Cannot save the asset: {detail}",
				{ { "detail", writeError } });
			m_StatusIsError = true;
			if (m_Ctx)
				m_Ctx->RecordOp("texture", "apply-failed", m_AssetLogical, writeError);
			return;
		}
		m_Payload = std::move(payload);
		m_Container = true;
		m_LegacyAsset = false;
		m_LoadedSourceText = trimmedSource;
		// 字节来源 = 资产自身(容器);外部源图只是可选的导入源。
		m_SourceLogical = m_AssetLogical;
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
		}

		// 3) 就地重烘 `<主名>.wtexc`(内容根)+ 失效材质贴图缓存。
		std::string bakeError;
		if (!Editor::BakeTextureArtifactNow(m_ContentRoot, m_AssetLogical, m_Settings, bakeError))
		{
			m_Status = Wui::TrFormat("panel.texture.status.bake_failed", "Bake failed: {detail}",
				{ { "detail", bakeError } });
			m_StatusIsError = true;
			if (m_Ctx)
				m_Ctx->RecordOp("texture", "bake-failed", m_AssetLogical, bakeError);
			RefreshArtifactState(true);
			return;
		}
		m_Status = Wui::TrFormat("panel.texture.status.applied",
			"Saved {asset} and baked {artifact}", { { "asset", m_AssetLogical },
				{ "artifact", std::filesystem::path(ArtifactAbsolutePath()).filename().generic_string() } });
		m_StatusIsError = false;
		// 形态说明 + 盘上的可选导入源(容器本身不依赖它)。
		Editor::TextureSourceResolution resolution;
		m_ImportSourceLogical.clear();
		if (Editor::ResolveTextureSource(m_ContentRoot, m_AssetLogical, m_Settings, resolution))
			m_ImportSourceLogical = resolution.ImportSourceLogical;
		m_AssetFormNote = Wui::TrFormat("panel.texture.form.container",
			"Single-file asset: {bytes} bytes embedded; the import source is optional.",
			{ { "bytes", std::to_string(m_Payload.size()) } });
		RefreshArtifactState(true);
		// 3) 刚写出的产物直接当预览(不必等防抖的第二次烘)。
		if (UploadArtifactFromDisk())
		{
			m_BakedRevision = m_PreviewRevision;
			m_PreviewState = PreviewState::Baked;
			m_PreviewDetail.clear();
		}
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
		m_Payload.clear();
		m_Container = false;
		m_LegacyAsset = false;
		m_ImportSourceLogical.clear();
		m_AssetFormNote.clear();
		m_SourceBuffer.clear();
		m_LoadedSourceText.clear();
		m_SourceError.clear();
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
		TouchPreviewRevision();
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
		if (UploadArtifactFromDisk())
		{
			m_BakedRevision = m_PreviewRevision;
			m_PreviewState = PreviewState::Baked;
			m_PreviewDetail.clear();
		}
		if (m_Ctx)
			m_Ctx->RecordOp("texture", "reset", m_AssetLogical, "deleted .wtex + rebaked defaults");
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
